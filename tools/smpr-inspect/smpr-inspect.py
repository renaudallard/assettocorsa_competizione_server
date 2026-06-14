#!/usr/bin/env python3
"""smpr-inspect -- read the ServerMonitor protobuf side-channel an
accd instance exposes on its gameplay TCP port.

Stdlib only.  Connects, sends a ServerMonitorConnectionRequest,
walks the framed protobuf reply stream, decodes the seven message
types from accd/monitor.h, and prints them in one of three forms:

    --output text  (default)   one event per line, human-readable
    --output json              one NDJSON object per event
    --output raw               full decoded structure per event

The wire format follows section 12B.3 of NOTEBOOK_B.md and the
field numbers in accd/monitor.h.  See accd/smpr.c for the server-
side handler and accd/tests/integration/run_smpr.sh for the
canonical client smoke test that this tool is a polished
descendant of.

Caveats:

  - This client speaks accd's demux (first body byte 0x0a routes
    to SMPR).  The kunos exe uses a different gating mechanism
    (a per-conn flag set inside its sim-handshake handler on an
    unconfirmed wstring match -- see
    memory:reference_smpr_ecosystem_audit.md), so this tool will
    NOT work against a stock kunos accServer.exe.  No public
    third-party tool speaks the kunos demux either.
"""
import argparse
import json
import socket
import struct
import sys
import time

# --- protobuf primitives -------------------------------------------

WT_VARINT = 0
WT_FIXED64 = 1
WT_LEN = 2
WT_FIXED32 = 5


def read_varint(buf, pos):
    v = 0
    shift = 0
    while True:
        if pos >= len(buf):
            raise ValueError("varint truncated")
        b = buf[pos]
        pos += 1
        v |= (b & 0x7F) << shift
        if not (b & 0x80):
            return v, pos
        shift += 7
        if shift > 63:
            raise ValueError("varint too long")


def read_tag(buf, pos):
    v, pos = read_varint(buf, pos)
    return v >> 3, v & 0x7, pos


def skip_field(buf, pos, wire):
    if wire == WT_VARINT:
        _, pos = read_varint(buf, pos)
    elif wire == WT_FIXED64:
        pos += 8
    elif wire == WT_LEN:
        n, pos = read_varint(buf, pos)
        pos += n
    elif wire == WT_FIXED32:
        pos += 4
    else:
        raise ValueError(f"unknown wire {wire}")
    return pos


def read_string(buf, pos):
    n, pos = read_varint(buf, pos)
    s = buf[pos:pos + n].decode("utf-8", errors="replace")
    return s, pos + n


def read_bytes(buf, pos):
    n, pos = read_varint(buf, pos)
    return bytes(buf[pos:pos + n]), pos + n


def read_float(buf, pos):
    return struct.unpack_from("<f", buf, pos)[0], pos + 4


def read_double(buf, pos):
    return struct.unpack_from("<d", buf, pos)[0], pos + 8


def read_fixed32(buf, pos):
    # fixed32 carrying a 32-bit int (e.g. driverTimes ms), not a float.
    return struct.unpack_from("<i", buf, pos)[0], pos + 4


# --- schemas (match accd/monitor.h) --------------------------------

SCHEMAS = {
    0x01: ("RegistrationResult", {
        1: ("success", "bool"),
        2: ("connectionId", "i32"),
        3: ("errorTxt", "string"),
    }),
    0x02: ("ServerConfiguration", {
        1: ("serverName", "string"),
        2: ("trackName", "string"),
        3: ("maxSlots", "i32"),
        4: ("trackMedals", "i32"),
        5: ("saRequired", "i32"),
        6: ("isPwProtected", "bool"),
        7: ("isLockedEntryList", "bool"),
        8: ("sessions", "sub:SessionDef"),
    }),
    0x03: ("SessionState", {
        1: ("currentSessionIndex", "i32"),
        2: ("weekendTimeSeconds", "i32"),
        3: ("idealLineGrip", "f32"),
        4: ("ambientTemp", "f32"),
        5: ("roadTemp", "f32"),
        6: ("cloudLevel", "f32"),
        7: ("rainLevel", "f32"),
        8: ("trackWetness", "f32"),
        9: ("dryLineWetness", "f32"),
        10: ("trackPuddles", "f32"),
        11: ("rainForecast10Min", "f32"),
        12: ("rainForecast30Min", "f32"),
        13: ("carsConnected", "i32"),
    }),
    0x04: ("CarEntry", {
        1: ("carId", "i32"),
        2: ("carModel", "enum"),
        3: ("drivingConnectionId", "i32"),
        4: ("raceNumber", "i32"),
        5: ("cupCategory", "enum"),
    }),
    0x05: ("ConnectionEntry", {
        1: ("connectionId", "i32"),
        2: ("firstName", "string"),
        3: ("lastName", "string"),
        4: ("shortName", "string"),
        5: ("playerId", "string"),
        6: ("isAdmin", "bool"),
        7: ("isSpecator", "bool"),
    }),
    0x06: ("RealtimeUpdate", {
        1: ("serverNow", "f64"),
        2: ("sessionState", "sub:SessionState"),
        3: ("connections", "sub:ConnectionEntry"),
        4: ("cars", "sub:CarEntry"),
    }),
    0x07: ("Leaderboard", {
        1: ("bestLap", "i32"),
        2: ("bestSplits", "i32"),
        3: ("isDeclaredWetSession", "bool"),
        4: ("entries", "sub:LeaderboardEntry"),
    }),
}

SUBSCHEMAS = {
    "SessionDef": {
        1: ("sessionType", "enum"),
        2: ("round", "i32"),
        3: ("durationSeconds", "i32"),
        4: ("raceDay", "i32"),
        5: ("minuteOfDay", "i32"),
        6: ("timeMultiplier", "i32"),
        7: ("overtimeDurationS", "i32"),
        8: ("preRaceWaitTimeS", "i32"),
    },
    "SessionState": SCHEMAS[0x03][1],
    "ConnectionEntry": SCHEMAS[0x05][1],
    "CarEntry": SCHEMAS[0x04][1],
    "LeaderboardEntry": {
        1: ("carEntry", "sub:CarEntry"),
        2: ("currentSteamId", "string"),
        3: ("missingMandatoryPits", "i32"),
        4: ("driverTimes", "fx32"),
        5: ("lastLapTime", "i32"),
        6: ("lastLapSplits", "i32"),
        7: ("bestLapTime", "i32"),
        8: ("bestLapSplits", "i32"),
        9: ("lapCount", "i32"),
        10: ("totalTime", "i32"),
        11: ("currentPenalty", "enum"),
        12: ("currentPenaltyValue", "i32"),
        13: ("driverName", "string"),
        14: ("driverShortName", "string"),
        15: ("carModel", "enum"),
    },
}


def decode(buf, schema):
    pos, out = 0, {}
    while pos < len(buf):
        try:
            field, wire, pos = read_tag(buf, pos)
        except ValueError:
            break
        info = schema.get(field)
        if info is None:
            try:
                pos = skip_field(buf, pos, wire)
            except ValueError:
                break
            continue
        name, kind = info
        try:
            if kind == "bool":
                v, pos = read_varint(buf, pos)
                val = bool(v)
            elif kind == "i32":
                v, pos = read_varint(buf, pos)
                # Negative int32 is wire-encoded as a 10-byte varint
                # (full 64-bit two's complement).  Truncate to 32 bits
                # first, then interpret as signed.
                v &= 0xFFFFFFFF
                val = v - (1 << 32) if v >= (1 << 31) else v
            elif kind == "enum":
                val, pos = read_varint(buf, pos)
            elif kind == "f32":
                val, pos = read_float(buf, pos)
                val = round(val, 4)
            elif kind == "f64":
                val, pos = read_double(buf, pos)
                val = round(val, 4)
            elif kind == "fx32":
                val, pos = read_fixed32(buf, pos)
            elif kind == "string":
                val, pos = read_string(buf, pos)
            elif kind.startswith("sub:"):
                sub_name = kind[4:]
                sub_buf, pos = read_bytes(buf, pos)
                sub_schema = SUBSCHEMAS.get(sub_name)
                val = decode(sub_buf, sub_schema) if sub_schema else \
                      f"<unknown:{sub_name}>"
            else:
                pos = skip_field(buf, pos, wire)
                continue
        except ValueError:
            break
        # Repeated fields land multiple times under the same tag --
        # collect them into a list so the second hit doesn't overwrite
        # the first.
        if name in out:
            if not isinstance(out[name], list) or \
               (out[name] and not isinstance(out[name][0], type(val))
                and kind.startswith("sub:") is False):
                out[name] = [out[name]]
            out[name].append(val)
        else:
            # Submessages that the schema marks repeated start as a
            # single-entry list so the formatter knows it's a vector.
            if kind.startswith("sub:") and name in {
                "sessions", "connections", "cars", "entries"
            }:
                out[name] = [val]
            else:
                out[name] = val
    return out


# --- I/O ------------------------------------------------------------

def build_connection_request(name, interval_ms, self_contained, extended,
                             register_all):
    """Hand-built ServerMonitorConnectionRequest body."""
    body = bytearray()
    # field 1: displayName (string)
    if name:
        nb = name.encode("utf-8")
        body += bytes([0x0A, len(nb)]) + nb
    else:
        body += bytes([0x0A, 0])
    # field 2: realtimeCarUpdateInterval (varint)
    body += b"\x10"
    v = interval_ms
    while v >= 0x80:
        body.append((v & 0x7F) | 0x80)
        v >>= 7
    body.append(v)
    # bool fields
    body += bytes([0x18, 1 if self_contained else 0])
    body += bytes([0x20, 1 if extended else 0])
    body += bytes([0x28, 1 if register_all else 0])
    return bytes(body)


def frame_walk(sock, seconds):
    """Read TCP frames (u16-length-prefixed) for up to `seconds`."""
    sock.settimeout(0.25)
    deadline = time.time() + seconds if seconds > 0 else None
    buf = bytearray()
    while True:
        if deadline is not None and time.time() >= deadline:
            return
        try:
            chunk = sock.recv(8192)
        except socket.timeout:
            continue
        if not chunk:
            return
        buf.extend(chunk)
        while len(buf) >= 2:
            flen = buf[0] | (buf[1] << 8)
            if len(buf) < 2 + flen:
                break
            yield bytes(buf[2:2 + flen])
            del buf[:2 + flen]


# --- formatting -----------------------------------------------------

PHASE_BY_TYPE = {0: "Practice", 1: "Qualifying", 2: "Race"}


def fmt_text(mtype, name, fields):
    """One-line summary per message."""
    if mtype == 0x01:
        return (f"REGISTRATION_RESULT success={fields.get('success')!s} "
                f"conn={fields.get('connectionId')} "
                f"err={fields.get('errorTxt', '')!r}")
    if mtype == 0x02:
        nsess = len(fields.get("sessions", [])) if isinstance(
            fields.get("sessions"), list) else (1 if fields.get(
                "sessions") else 0)
        return (f"SERVER_CONFIGURATION name={fields.get('serverName')!r} "
                f"track={fields.get('trackName')!r} "
                f"max={fields.get('maxSlots')} sessions={nsess}")
    if mtype == 0x03:
        return (f"SESSION_STATE session={fields.get('currentSessionIndex')} "
                f"weekend_s={fields.get('weekendTimeSeconds')} "
                f"cars={fields.get('carsConnected')} "
                f"amb={fields.get('ambientTemp')}C "
                f"road={fields.get('roadTemp')}C "
                f"clouds={fields.get('cloudLevel')} "
                f"rain={fields.get('rainLevel')}")
    if mtype == 0x04:
        return (f"CAR_ENTRY car={fields.get('carId')} "
                f"model={fields.get('carModel')} "
                f"drv_conn={fields.get('drivingConnectionId')} "
                f"race#={fields.get('raceNumber')} "
                f"cup={fields.get('cupCategory')}")
    if mtype == 0x05:
        return (f"CONNECTION_ENTRY conn={fields.get('connectionId')} "
                f"name={fields.get('firstName')!r} "
                f"{fields.get('lastName')!r} "
                f"steam={fields.get('playerId')} "
                f"admin={fields.get('isAdmin')!s} "
                f"spectator={fields.get('isSpecator')!s}")
    if mtype == 0x06:
        ncars = len(fields.get("cars", [])) if isinstance(
            fields.get("cars"), list) else 0
        return (f"REALTIME_UPDATE now={fields.get('serverNow')}ms "
                f"cars={ncars}")
    if mtype == 0x07:
        nentries = len(fields.get("entries", [])) if isinstance(
            fields.get("entries"), list) else 0
        return (f"LEADERBOARD bestLap={fields.get('bestLap')}ms "
                f"entries={nentries}")
    return f"{name} (0x{mtype:02x})  {fields}"


def main():
    ap = argparse.ArgumentParser(
        description="Read accd's ServerMonitor protobuf side-channel.")
    ap.add_argument("endpoint", help="host:port (the accd tcpPort)")
    ap.add_argument("-i", "--interval", type=int, default=250,
                    help="REALTIME_UPDATE cadence in ms "
                    "(server clamps to [50, 10000]; default 250)")
    ap.add_argument("-o", "--output", choices=("text", "json", "raw"),
                    default="text", help="output mode (default text)")
    ap.add_argument("-t", "--seconds", type=float, default=0,
                    help="stop after N seconds (0 = forever)")
    ap.add_argument("-n", "--name", default="smpr-inspect",
                    help="display name in the ConnectionRequest")
    ap.add_argument("--self-contained", action="store_true",
                    help="ConnectionRequest sendSelfcontainingLeaderboards")
    ap.add_argument("--extended", action="store_true",
                    help="ConnectionRequest sendExtendedLeaderboards")
    args = ap.parse_args()

    if ":" not in args.endpoint:
        ap.error("endpoint must be host:port")
    host, _, port_s = args.endpoint.rpartition(":")
    try:
        port = int(port_s)
    except ValueError:
        ap.error(f"bad port: {port_s!r}")

    body = build_connection_request(
        args.name, args.interval,
        args.self_contained, args.extended, True)
    frame = struct.pack("<H", len(body)) + body

    try:
        sock = socket.create_connection((host, port), timeout=5)
    except OSError as e:
        print(f"connect {host}:{port} failed: {e}", file=sys.stderr)
        return 1
    sock.sendall(frame)

    try:
        for msg in frame_walk(sock, args.seconds):
            if not msg:
                continue
            mtype = msg[0]
            sch = SCHEMAS.get(mtype)
            if sch is None:
                if args.output == "json":
                    print(json.dumps({
                        "type": f"unknown_0x{mtype:02x}",
                        "len": len(msg), "hex": msg.hex()
                    }))
                else:
                    print(f"unknown 0x{mtype:02x} len={len(msg)} "
                          f"hex={msg.hex()[:64]}")
                continue
            name, schema = sch
            fields = decode(msg[1:], schema)
            ts = time.strftime("%H:%M:%S")
            if args.output == "json":
                print(json.dumps({"type": name, "msgId": mtype,
                                  "ts": ts, **fields}))
            elif args.output == "raw":
                print(f"--- {ts} {name} (0x{mtype:02x}) ---")
                print(json.dumps(fields, indent=2, default=str))
            else:
                print(f"{ts}  {fmt_text(mtype, name, fields)}")
            sys.stdout.flush()
    except (KeyboardInterrupt, BrokenPipeError):
        # BrokenPipeError fires when piped to `head` / `less` and the
        # downstream tool closes early -- that's a clean exit, not a
        # failure.  Close stdout so the atexit flush doesn't re-raise.
        try:
            sys.stdout.close()
        except OSError:
            pass
    finally:
        sock.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
