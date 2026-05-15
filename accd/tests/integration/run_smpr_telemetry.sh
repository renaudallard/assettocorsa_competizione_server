#!/bin/sh
# SMPR per-car content regression.
#
# Note: the SMPR protobuf schema (accd/monitor.h, NOTEBOOK_B §12B.3)
# does NOT carry per-car fuel / damage / rpm telemetry -- those
# bytes live only on the wire-side 0x1e car_update path.  This
# test instead pins the SMPR CAR_ENTRY + CONNECTION_ENTRY content
# that IS in the schema:
#
#   - CAR_ENTRY (msg 0x04) carries car_id, car_model, race_number.
#   - CONNECTION_ENTRY (msg 0x05) carries first_name.
#
# We start 2 bots with distinct race_numbers + names, connect a
# probe to SMPR, and assert both bots' race_numbers + driver names
# appear in the protobuf stream.  A future regression in
# monitor_build_car_entry / monitor_build_connection_entry would
# surface as a missing race_number or empty name field.
set -e
cd "$(dirname "$0")"

pkill -KILL -f 'accd -c '       >/dev/null 2>&1 || true
pkill -KILL -f 'tools/bot/bot ' >/dev/null 2>&1 || true
sleep 1

RUNDIR=$(mktemp -d -p . smpr_telem.XXXXXX)
cp -r cfg/. "$RUNDIR/"

APID=
B1=
B2=
cleanup() {
    for p in "$B1" "$B2" "$APID"; do
        [ -n "$p" ] && kill -TERM "$p" 2>/dev/null || :
    done
    wait 2>/dev/null || :
    rm -rf "$RUNDIR" smpr_telem_*.log
}
trap cleanup EXIT INT TERM

rm -f smpr_telem_accd.log smpr_telem_bot1.log smpr_telem_bot2.log
/home/r/code/assettocorsa/accd/accd -c "$RUNDIR" >smpr_telem_accd.log 2>&1 &
APID=$!
sleep 1

/home/r/code/assettocorsa/tools/bot/bot --host 127.0.0.1 --tcp 9302 \
    --race 911 --grid 1 --name BotAlpha >smpr_telem_bot1.log 2>&1 &
B1=$!
/home/r/code/assettocorsa/tools/bot/bot --host 127.0.0.1 --tcp 9302 \
    --race 912 --grid 2 --name BotBeta >smpr_telem_bot2.log 2>&1 &
B2=$!
sleep 3

python3 - <<'PY'
import socket, struct, sys, time

def read_varint(buf, pos):
    v = 0
    shift = 0
    while True:
        b = buf[pos]
        v |= (b & 0x7f) << shift
        pos += 1
        if not (b & 0x80):
            return v, pos
        shift += 7

def walk_fields(body):
    pos = 0
    while pos < len(body):
        tag, pos = read_varint(body, pos)
        fn = tag >> 3
        wt = tag & 7
        if wt == 0:
            v, pos = read_varint(body, pos)
            yield fn, ('varint', v)
        elif wt == 2:
            n, pos = read_varint(body, pos)
            yield fn, ('len', body[pos:pos+n])
            pos += n
        elif wt == 5:
            yield fn, ('fixed32', body[pos:pos+4])
            pos += 4
        elif wt == 1:
            yield fn, ('fixed64', body[pos:pos+8])
            pos += 8
        else:
            raise ValueError(f'unsupported wire type {wt}')

req = b'\x0a\x05probe' + b'\x10\xfa\x01' + b'\x18\x01' + b'\x20\x01' + b'\x28\x01'
hdr = struct.pack('<H', len(req))

sock = socket.create_connection(('127.0.0.1', 9302), timeout=3)
sock.sendall(hdr + req)
sock.settimeout(0.3)

buf = b''
car_entries = []          # list of {car_id, race_number, ...}
conn_first_names = []     # driver first names seen in CONNECTION_ENTRY
lb_entry_names = []       # driver names seen in LEADERBOARD entries
rtu_car_ids = set()       # cars listed inside REALTIME_UPDATE
deadline = time.time() + 3.5
while time.time() < deadline:
    try:
        chunk = sock.recv(8192)
    except socket.timeout:
        continue
    if not chunk: break
    buf += chunk
    while len(buf) >= 2:
        flen = buf[0] | (buf[1] << 8)
        if len(buf) < 2 + flen: break
        msg = buf[2:2+flen]
        buf = buf[2+flen:]
        if not msg: continue
        mt, body = msg[0], msg[1:]
        if mt == 0x04:    # CAR_ENTRY
            ce = {}
            try:
                for fn, (wt, v) in walk_fields(body):
                    if fn == 1 and wt == 'varint': ce['car_id'] = v
                    elif fn == 4 and wt == 'varint': ce['race_number'] = v
                    elif fn == 2 and wt == 'len':    ce['car_model'] = v
            except Exception:
                pass
            if ce: car_entries.append(ce)
        elif mt == 0x05:  # CONNECTION_ENTRY
            try:
                for fn, (wt, v) in walk_fields(body):
                    if fn == 2 and wt == 'len':  # PB_CONN_FIRST_NAME
                        conn_first_names.append(v.decode('utf-8','replace'))
            except Exception:
                pass
        elif mt == 0x06:  # REALTIME_UPDATE
            try:
                for fn, (wt, v) in walk_fields(body):
                    if fn == 4 and wt == 'len':  # PB_RTU_CARS submessage
                        for sub_fn, (sub_wt, sv) in walk_fields(v):
                            if sub_fn == 1 and sub_wt == 'varint':
                                rtu_car_ids.add(sv)
            except Exception:
                pass
        elif mt == 0x07:  # LEADERBOARD
            try:
                for fn, (wt, v) in walk_fields(body):
                    if fn == 4 and wt == 'len':  # PB_LB_ENTRIES sub
                        for sub_fn, (sub_wt, sv) in walk_fields(v):
                            if sub_fn == 13 and sub_wt == 'len':
                                lb_entry_names.append(sv.decode('utf-8','replace'))
            except Exception:
                pass

print(f'CAR_ENTRY count: {len(car_entries)}')
race_nums = sorted({e.get("race_number") for e in car_entries if "race_number" in e})
print(f'  race_numbers seen: {race_nums}')

print(f'CONNECTION_ENTRY first_names: {sorted(set(conn_first_names))}')

want = {911, 912}
got = set(race_nums)
if not want.issubset(got):
    print(f'FAIL: expected race numbers {sorted(want)}, got {sorted(got)}')
    sys.exit(1)

names_seen = set(conn_first_names)
need_names = {'BotAlpha', 'BotBeta'}
missing = need_names - names_seen
if missing:
    print(f'FAIL: missing driver names in CONNECTION_ENTRY: {missing}')
    sys.exit(2)

print(f'REALTIME_UPDATE cars repeated: {sorted(rtu_car_ids)}')
if not rtu_car_ids:
    print('FAIL: no car_ids inside REALTIME_UPDATE cars subs')
    sys.exit(3)

print(f'LEADERBOARD driver names: {sorted(set(lb_entry_names))}')
# Drivers default to first_name='Bot' last_name='Driver' so the
# combined PB_LBE_DRIVER_NAME is 'Bot Driver'.  Just assert non-empty.
if not lb_entry_names:
    print('FAIL: no LEADERBOARD entries (wiring stub regression?)')
    sys.exit(4)

print('RESULT: PASS (SMPR exposes race_numbers, names, RTU cars, LB entries)')
PY
