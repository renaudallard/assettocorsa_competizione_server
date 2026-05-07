# ACC Dedicated Server Protocol — Clean-Room Specification (Notebook B)

> **Status**: draft 0.1, 2026-04-08
> **Target build**: ACC Dedicated Server 1.10.2, Steam build `14255706`
> **Source corpus**: publicly-shipped documentation and source code in Steam app `1430110`

## 0. Purpose, scope, and clean-room discipline

### 0.1 Purpose

This document is the clean-room specification for an independent reimplementation of the Assetto Corsa Competizione dedicated server, built so that an **unmodified** ACC game client (Steam build matching the target above) can connect to the reimplementation, play a private multiplayer session, and disconnect cleanly, without any traffic to Kunos's public backend.

### 0.2 Scope

- **In scope**: the sim-side wire protocol between ACC clients and `accServer.exe`, the JSON configuration schema, the session state machine, admin chat commands, result file schema, and the data model the protocol must express.
- **Out of scope**: the lobby/backend protocol ("kson"), the Kunos ratings/CP system, the Steam integration surface, Kunos's client-side broadcasting protocol (documented only because it clarifies the data model), anti-tamper.

### 0.3 Provenance rules for this document

Every fact in this document must be traceable to one of the following **public** sources. Citations appear inline as bracketed tags.

| Tag | Source |
|---|---|
| `HB §x.y` | `server/ServerAdminHandbook.pdf` (v1.10.2), section x.y |
| `CFG/<file>` | default configuration files in `server/cfg/` |
| `SDK/<file>` | `sdk/broadcasting/Sources/ksBroadcastingNetwork/*.cs` |
| `LOG` | `sdk/broadcasting/Sources/ksBroadcastingNetwork/server.log` (sample server log shipped in the SDK) |
| `TC` | `sdk/broadcasting/Testclient/readme.txt` |
| `CL` | `changelog.txt` |

**Forbidden sources for this document**: static or dynamic analysis of `accServer.exe`, packet captures, disassembly, decompilation, string extraction from the binary, Frida traces, debugger output, or anything derived from running the binary with instrumentation. Such work belongs in Notebook A and must not leak into this file.

Legal basis: EU Software Directive 2009/24/EC Article 6 permits reverse engineering for interoperability. This document stays within the even-stricter bound of "public documentation only", so the Art. 6 exception is not actually invoked here — it only becomes relevant for Notebook A.

### 0.4 Build pinning

Target: ACC Dedicated Server **1.10.2**, Steam build **`14255706`**, downloaded 2026-04-08.

`CL` lists "Protocol update to follow client update" notes for 1.6.0, 1.7.12, 1.8.0, 1.8.16, 1.9.8, 1.10.0, 1.10.1, and 1.10.2. Protocol compatibility is not maintained across builds. A reimplementation targeting build `14255706` is expected not to talk to clients on other builds.

---

## 1. Architecture overview

An ACC multiplayer session consists of:

- **One dedicated server process.** Runs headless, manages session state, accepts client connections over TCP and UDP, drives the session state machine, computes timing and penalties, optionally registers with Kunos's lobby backend.
- **N game clients.** Each connects to the dedicated server over TCP (control channel) and UDP (car state streaming).
- **Optionally, a spectator overlay chain.** A spectator-mode game client exposes a broadcasting UDP endpoint on localhost (see §12). Overlay software connects to the local game client, not to the dedicated server.
- **Optionally, server-monitoring tooling** (accweb, accservermanager, emperorservers). These connect to the dedicated server over a separate protobuf-based "ServerMonitor" protocol documented in §12B.

### 1.1 Two protocols, not one

The dedicated server implements **two distinct protocols** on its listening ports, serving different purposes:

1. **The sim-side protocol** (TCP + UDP on `tcpPort` / `udpPort`) — used by the ACC game client for multiplayer gameplay. **Hand-rolled binary wire format.** This is the protocol a reimplementation must speak if it wants unmodified ACC game clients to connect and play.
2. **The ServerMonitor protocol** (protobuf-based, `acc_server_protocol.proto` v1) — used by third-party admin/hosting tools to remotely monitor and control a running server. **Not needed for gameplay.** Implementing it is optional and gives you compatibility with existing monitoring tools.

The two protocols share **nothing** beyond the fact that they carry the same server state. They have different wire formats, different connection establishment, and different message type tables. Confusion between the two is a common trap.

**The dedicated server does not speak the broadcasting protocol** [`TC`, `SDK/BroadcastingNetworkProtocol.cs`]. The broadcasting protocol is a game-client feature, not a server feature. It is documented here because its enums and structs describe state the sim protocol must be able to encode, but the dedicated server never emits or receives broadcasting messages.

### 1.2 The Kunos implementation is not UE4

Contrary to a reasonable assumption, the Kunos dedicated server `accServer.exe` is **not** a UE4 build. It is a standalone C++ application with its own networking stack, sharing only game-logic code (physics, race rules, weather model) with the UE4-based game client. This conclusion is a useful constraint for reimplementation planning:

- The sim protocol is not UE4 netcode. It does not use `UNetDriver`, bunches, channels, property replication, or actor serialization. A reimplementation does not need to understand UE4's networking to be compatible.
- The wire format is a straightforward sequence of binary fields written by per-class serialization methods.
- The server is small (~1.7 MB binary), which bounds the total protocol surface area.

---

## 2. Transport

### 2.1 Listening sockets

From `HB §III.2.1` and `CFG/configuration.json`:

| Endpoint | Protocol | Purpose |
|---|---|---|
| `tcpPort` | TCP | Control channel: connect request, entry list, state transitions, chat, admin. |
| `udpPort` | UDP | Car-state streaming; used for ping probe. If a client shows no ping, this port is unreachable. |
| UDP `8999` | UDP | LAN discovery, all servers listen unless `lanDiscovery: 0`. Not configurable. |

`tcpPort` and `udpPort` may use the same numeric value (HB example uses `9201` for both; downloaded default uses `9231` UDP and `9232` TCP).

### 2.2 Outbound connections

- **Kunos lobby backend ("kson")**: TLS/TCP to Kunos infrastructure on port 443 when `registerToLobby: 1`. Uses a custom binary protocol with kson string encoding (u16 byte-length + UTF-16LE), not HTTP. The full protocol is documented in section 11. **A reimplementation must set `registerToLobby: 0` and must not attempt to impersonate this endpoint.**
- **Steam**: the Kunos server links the Steam client library. Driver identity uses Steam64 IDs with an `S` prefix (`HB §VI`). A reimplementation does not need Steam integration for LAN-only operation but cannot verify Steam IDs without it.

### 2.3 Byte order and framing conventions (from `SDK`)

The broadcasting protocol exhibits the following conventions, which we expect but have not verified to be reused in the sim protocol:

- **Little-endian** for all multi-byte integers and floats.
- **Strings**: `uint16` length prefix followed by UTF-8 bytes, no terminator [`SDK/BroadcastingNetworkProtocol.cs:349-354`].
- **Single-byte message type header** at the start of every message.
- **Message-per-datagram** on UDP (no length framing, each datagram is one complete message).

### 2.4 Client-side broadcasting port (out of scope)

Documented in `TC`: each game client can expose a local UDP broadcasting endpoint, configured in the client's `Documents/config/broadcasting.json`. Example from `TC`:

```json
{
    "updListenerPort": 9000,
    "connectionPassword": "asd",
    "commandPassword": ""
}
```

Note the original's `"updListenerPort"` spelling (not `"udp"`) — this is from the shipped source and must be preserved for compatibility.

---

## 3. JSON configuration schema

### 3.1 File encoding

All server-side configuration files are **UTF-16 LE with BOM**, not UTF-8 [`HB §III.2`]. Using UTF-8 "may seem to work but will lead to wrong readings." The reimplementation must read and write these files as UTF-16 LE.

Missing files are auto-regenerated with defaults on server start. Lowering `configVersion` in a file causes newly-added fields to be materialized from defaults on next start [`HB §III.2`].

### 3.2 `configuration.json` — networking identity

From `HB §III.2.1` and `CFG/configuration.json`:

| Field | Type | Default | Notes |
|---|---|---|---|
| `udpPort` | int | 9231 | UDP listener, car-state streaming |
| `tcpPort` | int | 9232 | TCP listener, control channel |
| `maxConnections` | int | 85 | Total connection cap (drivers + spectators + entrylist entries) |
| `lanDiscovery` | int 0/1 | 1 | Respond to LAN discovery probes on UDP 8999 |
| `registerToLobby` | int 0/1 | 1 | Register with Kunos backend. Set 0 for private MP. |
| `publicIP` | string | — | Explicit public IP when behind a gateway. Triggers additional backend handshake; server immediately shuts down on backend connect if this handshake fails. |
| `configVersion` | int | 1 | Schema version |

### 3.3 `settings.json` — server identity

From `HB §III.2.2` and `CFG/settings.json`:

| Field | Type | Notes |
|---|---|---|
| `serverName` | string | Displayed in ACC UI |
| `adminPassword` | string | Elevation password for admin chat commands |
| `carGroup` | string | `FreeForAll`, `GT3`, `GT4`, `GT2`, `GTC`, `TCX` |
| `trackMedalsRequirement` | int | -1 disables; otherwise 0..3 |
| `safetyRatingRequirement` | int | -1 disables; otherwise 0..99 |
| `racecraftRatingRequirement` | int | -1 disables; otherwise 0..99 |
| `password` | string | Empty = public; set = private MP |
| `spectatorPassword` | string | Must differ from `password` if both set |
| `maxCarSlots` | int | Car-slot cap; forced to 30 max for public MP |
| `dumpLeaderboards` | int 0/1 | Write `results/*.json` at session end |
| `isRaceLocked` | int 0/1 | Allow joining during race session |
| `randomizeTrackWhenEmpty` | int 0/1 | Cycle track when last driver leaves |
| `centralEntryListPath` | string | Override `cfg/entrylist.json` location; path separators must be `/` |
| `allowAutoDQ` | int 0/1 | 0 = downgrade auto-DQ to 30s stop&go for race-control review |
| `shortFormationLap` | int 0/1 | Long formation is private-only |
| `dumpEntryList` | int 0/1 | Save entry list at each Q session end |
| `formationLapType` | int | 0 = limiter lap, 1 = free (private only), 3 = default (position control + UI) |
| `ignorePrematureDisconnects` | int 0/1 | 1 = default, tolerates brief TCP drops; 0 = strict 5s inactivity timeout |
| `configVersion` | int | Schema version |

### 3.4 `event.json` — race weekend definition

From `HB §III.2.3` and `CFG/event.json`:

| Field | Type | Notes |
|---|---|---|
| `track` | string | From track catalog §7.1 |
| `preRaceWaitingTimeSeconds` | int | Minimum 30 |
| `sessionOverTimeSeconds` | int | Grace period after timer hits 0:00; default 120 is too short for long tracks |
| `ambientTemp` | int | Baseline °C |
| `cloudLevel` | float | 0.0..1.0 (discrete 0.1 steps) |
| `rain` | float | 0.0..1.0 (discrete 0.1 steps) |
| `weatherRandomness` | int | 0 = static; 1-4 realistic; 5-7 sensational |
| `postQualySeconds` | int | Gap after Q end / timeout before race start |
| `postRaceSeconds` | int | Gap after race end before next weekend |
| `metaData` | string | Passed through to result files |
| `simracerWeatherConditions` | int 0/1 | Experimental; caps rain/wetness at ~2/3 |
| `isFixedConditionQualification` | int 0/1 | Experimental; freezes conditions, requires `weatherRandomness: 0` |
| `sessions` | array | See §3.4.1 |
| `configVersion` | int | Schema version |

Obsolete: `trackTemp` (track temperature is simulated from ambient + sun + clouds).

#### 3.4.1 Session entries

Array element fields (`HB §III.2.3`):

| Field | Type | Notes |
|---|---|---|
| `hourOfDay` | int | 0..23 |
| `dayOfWeekend` | int | 1 = Friday, 2 = Saturday, 3 = Sunday |
| `timeMultiplier` | int | 0..24 (rate of in-game time vs real time) |
| `sessionType` | string | `"P"` Practice, `"Q"` Qualifying, `"R"` Race |
| `sessionDurationMinutes` | int | Session length in minutes |

Constraint: at least one non-race session must be present [`HB §III.2.3` remarks].

### 3.5 `eventRules.json` — pitstop rules

From `HB §III.2.4`. Fields: `qualifyStandingType`, `pitWindowLengthSec`, `driverStintTimeSec`, `mandatoryPitstopCount`, `maxTotalDrivingTime`, `maxDriversCount`, `isRefuellingAllowedInRace`, `isRefuellingTimeFixed`, `isMandatoryPitstopRefuellingRequired`, `isMandatoryPitstopTyreChangeRequired`, `isMandatoryPitstopSwapDriverRequired`, `tyreSetCount`.

Public MP ignores this file and uses defaults.

Key semantics:
- `driverStintTimeSec` and `maxTotalDrivingTime` are interdependent; if one is off, the other is auto-set to a safe value.
- Stint timer resets at pit entry, counts down again at pit exit; freezes while serving penalties.
- When `maxTotalDrivingTime` < current stint time, the total driving time overrides the stint timer (HUD background turns red).
- `maxDriversCount` auto-compensates `maxTotalDrivingTime` for entries with fewer drivers than the cap.

### 3.6 `assistRules.json` — driver aid rules

From `HB §III.2.5` and `CFG/assistRules.json`. Fields: `stabilityControlLevelMax` (0..100), and booleans `disableAutosteer`, `disableAutoLights`, `disableAutoWiper`, `disableAutoEngineStart`, `disableAutoPitLimiter`, `disableAutoGear`, `disableAutoClutch`, `disableIdealLine`. Public MP ignores this file.

Since ACC 1.8.11 [`CL`], assists with no manual override (automatic lights) are no longer enforced by the server, and reckless-driving DQ penalties are no longer reduced by `allowAutoDQ: 0`.

### 3.7 `entrylist.json` (optional, `cfg/entrylist.json`)

From `HB §VI`. Top-level:

```json
{
  "entries": [ ... ],
  "forceEntryList": 0
}
```

`forceEntryList: 1` rejects drivers not in the list (private-only). Each entry:

| Field | Type | Notes |
|---|---|---|
| `drivers` | array of driver objs | Must contain at least one driver with `playerID` |
| `raceNumber` | int | 1..998, -1 = user picks |
| `forcedCarModel` | int | -1 = user picks; otherwise from car catalog §7.2 |
| `overrideDriverInfo` | int 0/1 | Use entry-list name/category instead of client-supplied |
| `customCar` | string | Filename in `cars/` subfolder; forces livery/team/car choice |
| `overrideCarModelForCustomCar` | int 0/1 | 1 = force car model too; 0 = let user pick model but force livery/team |
| `isServerAdmin` | int 0/1 | Auto-elevate on join |
| `defaultGridPosition` | int | ≥1 = fixed grid slot if race starts without qualifying |
| `ballastKg` | int | 0..100, additive with `bop.json` |
| `restrictor` | int | 0..20 (%), additive with `bop.json` |

Driver object fields: `firstName`, `lastName`, `shortName`, `driverCategory` (see §7.3), `playerID` (Steam64 with `S` prefix).

### 3.8 `bop.json` (optional, `cfg/bop.json`)

From `HB §VI.3`. Top-level:

```json
{
  "entries": [
    { "track": "...", "carModel": N, "ballastKg": K, "restrictor": R }
  ]
}
```

Composite key `(track, carModel)`. Values are additive to entry-list values. Admin commands `/ballast` and `/restrictor` override until the car rejoins.

### 3.9 `serverList.json` (client-side, out of scope)

Documented for completeness (`HB §III.3.1`): lives in each client's `Users/Documents/Assetto Corsa Competizione/Config`. If present, the client will LAN-scan the IP in `leagueServerIP` instead of the local network, yielding direct-IP access to private servers. Use case: private leagues on `registerToLobby: 0` servers.

---

## 4. Session state machine (external broadcasting API)

This section describes the phase enum exposed by the broadcasting
SDK (used by overlay tooling).  The **internal** server state machine
that drives the sim-protocol wire format is a simpler 7-level model
documented in §5.7; a reimplementation must follow §5.7 on the wire
and can map to the enum below only when generating broadcasting-SDK
events.

From `SDK/BroadcastingEnums.cs` (`SessionPhase`):

```
0 NONE
1 Starting
2 PreFormation
3 FormationLap
4 PreSession
5 Session
6 SessionOver
7 PostSession
8 ResultUI
```

`LOG` lines 3–18 show real phase transitions with server clock (ms):

```
Practice session (no formation lap):
  Starting     → PreSession → Session    → SessionOver → PostSession → ResultUI
  phase 1 → 4 → 5 → 6 → 7 → 8
```

Phases 2 (PreFormation) and 3 (FormationLap) are skipped for non-race sessions. For a race, the full sequence is 1 → 2 → 3 → 4 → 5 → 6 → 7 → 8.

The server emits a phase-change message to every connected client and to the Kunos backend at every transition (`LOG` `"Sent new session state to kson"`).

Session session duration from the `LOG` practice example:
- `Starting`: ~5000 ms
- `PreSession`: ~6 ms (phase 4 has zero "span" — it's a marker)
- `Session`: duration is `sessionDurationMinutes × 60 × 1000` ms + padding
- `SessionOver`: ~15 s after session end
- `PostSession`: ~10 s after SessionOver
- `ResultUI`: ~15 s after PostSession

(These are observed values from a sample log, not definitive. A reimplementation should treat the non-`Session` phases as configurable short delays.)

---

## 5. Sim protocol wire format

### 5.1 Transport framing

The sim protocol multiplexes messages over both TCP and UDP. Both transports carry the same conceptual "packet": a single contiguous byte buffer whose **first byte is the message type identifier**. The rest of the buffer is the message body, encoded as a sequence of fixed-width scalar fields with no per-field tags.

The same internal buffer class handles both transports; it carries a flag indicating whether the payload arrived over TCP or UDP, but the field formats and the message-type byte layout are identical between the two.

#### 5.1.1 UDP framing

On UDP, each datagram is one message. The datagram boundary is the message boundary. No explicit framing bytes — the length is the datagram length.

**Maximum UDP message size: 2048 bytes** (the receive buffer size). Messages larger than this are not supported over UDP and must be sent via TCP.

#### 5.1.2 TCP framing

TCP uses a variable-width length-prefix header so multiple messages can be streamed over a single connection and a reader can always determine the next message's boundary.

**Short format** — for messages whose body is 0 to 65534 bytes:

```
  offset 0   1   2   3   ...   1+n
         +-------+-----------------+
         | len   | body[0..n-1]    |
         +-------+-----------------+
         u16 LE
         (= n)
```

The first two bytes are a little-endian `u16` length giving the number of body bytes that follow. The body bytes immediately follow. Total frame size: `2 + n` bytes.

**Extended format** — for messages whose body is 65535 or more bytes:

```
  offset 0   1   2   3   4   5   6   ...   5+n
         +-------+---------------+-----------------+
         | 0xFFFF | len            | body[0..n-1]  |
         +-------+---------------+-----------------+
         u16 LE    u32 LE (= n)
         (sentinel)
```

The first two bytes are the sentinel value `0xFFFF` (u16 LE), followed by a little-endian `u32` length giving the number of body bytes that follow. Total frame size: `6 + n` bytes.

**Reader behavior**:
- Peek at the first `u16`. If less than `0xFFFF`, treat as short-format length.
- If exactly `0xFFFF`, read the next `u32` as extended-format length.
- Wait until `(header + body)` bytes are available before extracting.

**Writer behavior**:
- If body length < `0xFFFF`, emit the short header.
- If body length ≥ `0xFFFF`, emit the extended header. There is no way to express a body of exactly `0xFFFF` bytes in the short format — the sentinel value is reserved.

**A reimplementation should enforce a reasonable per-connection receive-accumulator cap** (e.g. 64 KB) and drop connections that exceed it. The Kunos server uses a 640 KB cap, which is much larger than needed for any legitimate message and is probably a legacy number.

### 5.2 Dispatch architecture

There is **no central dispatcher function** for incoming messages. Instead, each socket (both UDP and TCP) maintains a list of registered handler callbacks. On every received message, the socket iterates its handler list and invokes each callback with a wrapper around the message bytes.

Each handler self-selects by reading byte 0 of the message (the message type identifier) and comparing it against the set of IDs that handler cares about. Handlers that do not recognize the ID return early without touching the cursor.

A reimplementation may choose either architecture — the protocol contract is only that byte 0 is the message ID and handlers process bytes 1..n accordingly. A central switch-based dispatcher and a list-of-subscribers dispatcher are both compatible with the contract. The Kunos implementation uses the latter.

### 5.2 Scalar types

All scalar fields are **little-endian**, native-alignment not required (fields are packed with no padding):

| Type | Width | Notes |
|---|---|---|
| `u8` | 1 byte | unsigned; `bool` is encoded identically, non-zero = true |
| `u16` | 2 bytes | little-endian |
| `u32` | 4 bytes | little-endian |
| `i32` | 4 bytes | little-endian, two's complement |
| `f32` | 4 bytes | IEEE-754 single, little-endian |
| `u64` | 8 bytes | little-endian |
| `f64` | 8 bytes | IEEE-754 double, little-endian |

There are no varints, no tag bytes, no per-field headers. The type of each field is determined by its position in the message, governed by the message's schema.

### 5.3 String encoding

The protocol has **two string formats**. Which format a given field uses is determined by the schema of the message — there is no tag or marker on the wire indicating which format is in use. A reimplementation must know, for each string field, which format the protocol specifies.

#### 5.3.1 Format A — short string (u8 length, UTF-32 padded)

Used for short identifiers: driver first name, last name, short name (3 chars), race numbers-as-strings, small labels.

```
+--------+-----------------------------+
| u8 len | len × (u16 char + u16 zero) |
+--------+-----------------------------+
```

- **Length prefix**: 1 byte, unsigned. **Max 255 characters.**
- **Body**: exactly `len × 4` bytes. Each character occupies a 4-byte slot, of which the first 2 bytes are a little-endian UTF-16 code unit and the last 2 bytes are zero and ignored.

Effectively: **UTF-32LE with only BMP code points** (≤ U+FFFF). Non-BMP characters (emoji, supplementary plane) cannot be represented. In practice all fields using this format are display names and short labels that fit within BMP.

#### 5.3.2 Format B — long string (u16 length, raw UTF-16 LE)

Used for longer text: server name, chat messages, error text.

```
+----------+------------------------+
| u16 len  | len bytes of UTF-16 LE |
+----------+------------------------+
```

- **Length prefix**: 2 bytes, unsigned little-endian. **Represents the length in BYTES, not characters.** The value is expected to be even; each character is 2 bytes.
- **Body**: exactly `len` bytes of UTF-16 LE data (no padding, no zero terminator). `len / 2` characters.
- **Maximum**: 65535 bytes = 32767 characters in the worst case (only BMP), minus any per-message overhead. In practice limited further by the TCP accumulator cap.

Format B can represent the full Unicode BMP. Supplementary-plane code points (surrogate pairs) would be encoded as two 16-bit code units each; whether the server decodes them correctly is not confirmed and a reimplementation should probably treat them as literal UTF-16 and not try to interpret as grapheme clusters.

#### 5.3.3 How to tell which format is used

Only by reference to the message schema for each specific field. Both formats are used in the same protocol, in different messages. A reimplementation must document, for each serializer it writes, which format each string field uses.

Rule of thumb from observation: display names, short tags, and identity-like fields use Format A; free-form text, server metadata, and chat-like fields use Format B.

### 5.4 Optional trailing fields (Format A strings only)

### 5.4 Optional trailing fields

Some message schemas evolve by appending new fields at the tail. The protocol handles this with a simple mechanism: **the sender can stop writing at any field boundary, and the receiver checks whether the next field fits before reading it.**

In practice this only works cleanly for *strings* at the tail of the message, because the check the receiver uses is:

```
bool has_next_string(cursor):
    if cursor + 1 > buffer_end: return false
    byte len = buffer[cursor]              // peek length byte, do not advance
    if cursor + 1 + (len * 4) > buffer_end: return false
    return true
```

This exploits the fact that strings have a deterministic size (computable from the length prefix) so the receiver can know if a complete string record is available without consuming any bytes.

Scalar fields do not have a corresponding guard. Schemas that evolve by appending scalars break backward compatibility with older clients.

Deserializers that expect optional strings **must** apply this guard before every such field.

### 5.5 Serialization pattern — nested deserializers

Complex messages are deserialized by calling per-type deserialization methods recursively. Leaf types (e.g. a `CarInfo`) read primitive scalars in declaration order. Composite types (e.g. an `EventEntity` containing a track name, a circuit, graphics settings, a car set, a race configuration, a weather status, and weather data) read their own fields in declaration order and delegate each sub-object to its own deserializer.

There is no separate "message length" wrapping sub-objects; they are read into a contiguous stream and each deserializer consumes exactly the bytes it expects, leaving the cursor at the start of the next field.

A reimplementation must therefore implement deserializers in a matched pattern — per-type functions that read their own fields in the canonical order. The declaration order of each type becomes part of the protocol contract.

### 5.6 Cursor semantics

The receiving side uses a stateful cursor on the packet buffer. Each primitive read advances the cursor by the width of the value read. Strings advance the cursor by `1 + len * 4` bytes.

Bounds-check behavior on the Kunos server is **non-fatal**: out-of-range reads log an error but continue reading past the end of the buffer, potentially returning garbage. A clean reimplementation should be **stricter**: treat any out-of-range read as a protocol error and drop the connection. Non-fatal behavior is developer-mode debugging convenience and is not part of the contract.

### 5.5 Client connection state machine

A client connection is in exactly one of three states at any time:

| State | Meaning | Allowed inbound messages |
|---|---|---|
| `0` | Unauthenticated — client has just connected and has not been through a successful handshake yet | Only message id `9` (handshake / request connection). Any other message id causes immediate transition to state `3`. |
| `1` | Authenticated — client has passed the handshake and is playing | Message id `9` (re-authentication, rarely used), message id `0x10` (client disconnect), and all other IDs in the main dispatch set (§5.6) |
| `3` | Disconnecting / disconnected | All messages are ignored. The connection is closed at the next receive cycle. |

Transitions:

- `0 → 1`: on a successful response from the handshake handler.
- `0 → 3`: on handshake failure, or on any non-handshake message received in state `0`.
- `1 → 3`: on a message id `0x10`, or on a protocol error in a later message.
- `1 → 1`: normal message processing.

### 5.6 Message ID catalog (client → server)

All IDs listed here are for messages **from the client to the server**. The first byte of each message body is the ID. Server-to-client messages have a separate, not-yet-enumerated ID space.

Protocol version: **`0x100` (256)** for ACC Dedicated Server 1.10.2.

#### 5.6.1 TCP message IDs

Messages carried over the reliable TCP control channel. 22 distinct IDs:

| ID (hex) | ID (dec) | Name / meaning |
|---|---|---|
| `0x09` | 9 | Request connection (handshake); see §5.6.4 |
| `0x10` | 16 | Client-initiated graceful disconnect |
| `0x19` | 25 | **`ACP_LAP_COMPLETED`** (formerly catalogued as "client lap-time report") — body is `u16` `u16` `i32` `u8` (cup position, track position, lap time in ms, and an unsigned quality byte). The server validates the fields (errors include `"Received lap with isSessionOver flag; will ignore it"`, `"ACP_LAP_COMPLETED hit empty leaderboard"`, `"received ACP_LAP_COMPLETED, but no car %d found"`, and the fatal `"ACP_LAP_COMPLETED currentLeaderboard.lines is empty, but getLineByCarId delivered"`), updates the reporting connection's rating state, then broadcasts a transformed `0x1b` message to every other client. This message uses the tier-2 queued-lambda broadcast mechanism, not the direct relay. |
| `0x20` | 32 | **Sector split (single, per crossing)** — client reports a single sector split as it crosses each timing line. **Note**: the `accd/msg.h` constant is named `ACP_SECTOR_SPLIT_BULK` and earlier revisions of this spec called this the "bulk variant", but the exe's `FUN_1400142f0` case `0x20` handler treats one message = one split (no embedded count), so the correct semantic name is "single split per crossing" — clients emit one `0x20` per sector boundary and a separate `0x21` once at the start/finish line. Body (12 B with msg id): `u8 0x20` + `i32 sector_time_ms` + `u8 sector_index` (0/1/2) + `i32 clock_ms` (raw timestamp) + `u16 car_field` (lapstates / `isSessionOver` is bit `0x400`). Note that the exe layout has the two i32s in REVERSED order (`raw_ts` first, then `split_time`); the current accd reader stores them under names that match its swapped convention. The exe's case-0x20 handler calls `FUN_140126450 "addSplit"` to push the split into a per-line vector at car+0x30..+0x38 and broadcasts a transformed `0x3a` message to all other clients (`u16 car_id` + `u8 split_count` + `u32[count]` + `i32 clock` + `u16 car_field`). Errors: `"Received split with isSessionOver flag; will ignore it"`, `"ACP_SECTOR_SPLIT hit empty leaderboard"`, `"received ACP_SECTOR_SPLIT, but no car %d found"`. |
| `0x21` | 33 | **Lap completed** — emitted once per lap at the start/finish line crossing. Body (13 B with msg id): `u8 0x21` + `i32 laptime_ms` + `i32 raw_ts` + `u8 flag1` + `u16 lapstates` + `u8 flag2`. **Important**: the trailing `flag2` byte is **NOT** a sector index — real-client capture (`race-debug.pcap`, 2026-04-29 15:02:52) shows `flag2=59` on a normal lap completion. Bit `0x400` of `lapstates` is `isSessionOver`. The `accd/msg.h` constant is named `ACP_SECTOR_SPLIT_SINGLE` (misleading — actually lap-complete). Exe behaviour (case `0x21` of `FUN_1400142f0`): calls `FUN_14012b380` for lap-completion bookkeeping (mandatory-pit + driver-stint DQ checks via `FUN_14012ae10`), copies `car+0x1d0` → `car+0x1d8`, clears `car+0x1e8` u16 (out-of-track latch), broadcasts the `0x3b` lap-complete relay (`u16 car_id` + `u32 laptime` + `u8 flag1` + `u32 raw_ts` + `u16 lapstates`). **Does not** broadcast `0x3c` at lap end. Log: `"New laptime: %d for carId %d"`. |
| `0x2a` | 42 | **`ACP_CHAT`** — body is a Format-A `sender_name` string + a Format-A `chat_text` string, parsed via `FUN_14002c0b0`. The server logs `"CHAT %s: %s"`, runs a printf-format-specifier sanitization pass over the text (rejects messages containing `%%` patterns to prevent format-string injection), then calls `FUN_140021680` (the chat command parser, see §8 / Pass 2.15) to dispatch any leading `/`-keyword. If the parser returns 0 (the message was NOT a command) the server builds a `0x2b` chat broadcast message via `FUN_140033030` and sends it to every connected client via `FUN_14001ada0`. Errors include `"Received chat message from null connectionCallback"`. |
| `0x2e` | 46 | **`ACP_CAR_SYSTEM_UPDATE`** — body is `u16 carId` + `u64 system_data` (10 bytes total). The server validates the carId matches the connection's owned car (otherwise logs `"Received ACP_CAR_SYSTEM_UPDATE for wrong car - senderId %d, carId %d"`), stores the `system_data` u64 at car-state offset `0x1b0`, then broadcasts a relayed server `0x2e` message containing `u16 carId + u64 system_data + u64 server_timestamp` to every other connected client. Log: `"Updated %d clients with new carSystem for car %d (%ul)"`. |
| `0x2f` | 47 | **`ACP_TYRE_COMPOUND_UPDATE`** — body is `u16 carId` + `u8 tyreCompound`. The server validates ownership (otherwise logs `"Received ACP_TYRE_COMPOUND_UPDATE for wrong car - senderId %d, carId %d"`), stores the compound byte at car-state offset `0x152`, then broadcasts a server `0x2f` message (`u8 = 0x2f` + `u16 carId` + `u8 tyreCompound`, 4 bytes total) to every other client. Log: `"Updated %d clients with new tyreCompound for car %d"`. |
| `0x32` | 50 | **`ACP_CAR_LOCATION_UPDATE`** — body is `u16 carIndex` + `u8 carLocation` (5-value enum: NONE/Track/Pitlane/PitEntry/PitExit, see §7.9). Historical ACP name is still current. |
| `0x3d` | 61 | **`ACP_OUT_OF_TRACK`** — body is `u8 force_flag` + `i32 timestamp_raw`. The server normalizes the timestamp through `FUN_140042030` (raw ticks → session time delta), looks up the car at `connection +0xa00a0` (otherwise logs `"received ACP_OUT_OF_TRACK, but no car %d found"`), checks the car's out-of-track flag at car+0x180+0x28 (skips if already set), then sets the flag and broadcasts a transformed server `0x3c` message (body `u16` + `u16` + `u32`, 9 bytes) to all other clients via the tier-2 queued-lambda broadcast mechanism. |
| `0x41` | 65 | **Client-reported penalty event** — body is `u8 category` + `u8 penalty_type` + `u64 raw_timestamp` + `i32 value`. The first byte is a `DSQ_*` category enum (0..13), NOT a force flag — the exe calls it `param_5` and `client_category_to_reason` in `accd/handlers.c` maps it to a textual reason. The server normalizes the timestamp via `FUN_140042030`, then calls into the timing/penalty module (`FUN_140125f50`) with the carId + the parsed fields. The client's auto-penalty subsystem reports track-cut, pit-speeding, and other infractions through this message. |
| `0x42` | 66 | **Penalty-cleared notification** (the `accd/msg.h` constant `ACP_LAP_TICK` is misleading — this is NOT a per-tick message). Body is exactly `u8 0x42 + u64 timestamp` with **no leading force byte** (the accd reader at `handlers.c:1066` currently parses `u8 force + u64 ts` and is wrong by one byte). Sent **once per cleared-penalty edge** on the player's own car: when the client engine flips a pending DT (drive-through) or S&G (stop-and-go) penalty flag from active to cleared (`FUN_140e5b2e0` "Penalty cleared for %ls at %f"; warns "cleanPenalty in MP, but car isn't player car?!" if the affected car isn't the player's). Server commits the per-car pending Penalty record as served at the supplied wallclock via `FUN_140126b50`, which walks a list of 0x90-byte penalty entries, finds the entry whose +0x28 matches the carId, writes the timestamp to entry+0x68, then either finalises or extends the per-car Penalty record vector. The same routine is invoked by the admin `clear` chat command and by `FUN_140127440` ("Converted pending DT/S&G penalty to … time penalty for carId %d"). Not a clock-sync probe, not lag comp, not lap-segment-based. |
| `0x43` | 67 | **Damage zones update** — body is 5 × `u8` (one normalized value per damage zone). The server stores them as 5 `float`s in the car-state damage block at car-struct offset 0x1b8, then broadcasts a transformed server **`0x44`** message to every other client via UDP `broadcast_except_one`. Log: `"Updated %d clients with new damage zones for car %d"`. **Note**: this is one of three distinct uses of the id byte `0x44` — see the server→client `0x44` row in §5.6.4a and the lobby-protocol `0x44` registration described there. |
| `0x45` | 69 | **Car dirt status update** (`ACP_CAR_DIRT_UPDATE`) — body is `u16 carId` + 5 × `u8` (each normalized to a `float` and packed into a `ksRacing::CarDirtStatus` record at car-state offset 0x160). The server stores the dirt for the welcome spawnDef but **does NOT relay** — Kunos's exe also drops it (per pcap evidence; comment in `accd/handlers.c:1156`). An earlier revision of this spec listed this opcode as `0x46`; that was wrong. |
| `0x47` | 71 | **`ACP_UPDATE_DRIVER_SWAP_STATE`** — body is `u16 carId` followed by a serialized swap-state record (one driver entry per slot). The server validates that the connection actually owns the target car (otherwise logs `"Received ACP_UPDATE_DRIVER_SWAP_STATE for alien car: %d (receiver car %d, connection %d)"`). It then validates the per-entry record count matches the entry list driver count (otherwise logs `"ACP_UPDATE_DRIVER_SWAP_STATE Swap data has less drivers %d than entries %d (receiver car %d, connection %d)"`). For each driver in the record it updates the swap state and logs `"Updated driverSwapState for car %d driver %d (%s): %d -> %d"` (or `"Updated (foreign) driverSwapState ..."` if the update came from a different connection than the car owner). The state change is then forwarded to other connections via the `"Forwarding driver swap payload to connection %d for carId %d, driverIndex -> %d"` path. |
| `0x48` | 72 | **`ACP_EXECUTE_DRIVER_SWAP`** — body is `u16 carId` + `u8 swap_request_code`. The server validates the connection owns the car (otherwise logs `"ACP_EXECUTE_DRIVER_SWAP, but no car controlled for connection %d"` or `"... carId mismatch: %d (car controlled %d for connection %d)"`), then runs the swap procedure via the std::function dispatch. Result reply: server sends back **`0x49`** (u8 msg id + u8 result_code) over TCP to the requesting client (logged as `"Driver swap result: %d"` or, on failure, `"Driver swap failed: %d"`). On success and if a server-config flag is set, also emits **`0x58`** broadcast (u8 + u16 carId + u8 swap_request_code) to every other client. **Distinct from** the UDP `0x48` LAN discovery probe — the two share an id byte but are disambiguated by transport. |
| `0x4a` | 74 | **`ACP_DRIVER_SWAP_STATE_REQUEST`** — body is `u16 carId` + `u8 sub_state` + `u8 connection_state`. The server validates ownership (otherwise logs `"ACP_DRIVER_SWAP_STATE_REQUEST for the wrong carId: %d (Connection owns %d)"`). Sub-state values 2, 3, 4 are accepted; anything else triggers `"DriverSwap Request for type %d is not implemented"`. Sub-state 3 walks every connection sharing the car and bumps each one from sub-state 3 to 2; sub-state 2 requires the request connection to actually own the car at the moment (`"DriverSwap 'Request'-Request, but car isn't controlled (%d) by this connection (%d)"`) and then dispatches via a per-entry lambda. The connection's own swap state byte is updated and the message log records `"Connection %d on car %d changes its swap connection state from %d to %d"`. |
| `0x4f` | 79 | **`ACP_DRIVER_STINT_RESET`** (formerly catalogued as a generic "event report") — body is `u8 force_reset_flag` + `u64 timestamp`. The server logs `"Receives driver stint reset for car %d"`, normalizes the timestamp through `FUN_140042030`, then dispatches to one of two timing-update functions depending on the force flag, both wrapped in lambdas that send a per-recipient TCP message. The on-the-wire server→client `0x4f` (with sub-opcode 0x00 / 0x01 — see §5.6.4a) is the broadcast variant of this same event. |
| `0x51` | 81 | **`ACP_ELO_UPDATE`** — body is `u16 new_elo` + `u16 (unused?)`. The server logs `"Car %d elo update %d => %d (%d)"`, writes the new value into the car-state struct at offset 0x1f8, and sets a "results dirty" flag for the next save. No outbound message. |
| `0x54` | 84 | **`ACP_MANDATORY_PITSTOP_SERVED`** — body is `u16 carId`. The server validates the carId matches the connection's owned car (otherwise logs `"Received ACP_MANDATORY_PITSTOP_SERVED for carId %d, but connection is %d"`), then logs `"Served Mandatory Pitstop: %d"` and clears the mandatory-pitstop-pending flag for the car via the timing module. No outbound message. |
| `0x55` | 85 | **Lap-history request** (`accd/msg.h` constant `ACP_LOAD_SETUP` is a misnomer — this is the in-game garage's "Previous Laps" panel, NOT a car-setup file load; there is no kson-keyed setup blob anywhere in the wire format). Body is `u8 setup_index` + `u16 carId` + `u32 setup_revision`. The "setup_index" byte selects which session archive (P/Q/R) to retrieve. The server looks up the requested session's lap-history vector (live `car->race` for current session, or the persisted per-session archive for past sessions) and replies with `0x56` carrying the lap records. Out-of-range index → silent no-reply. |
| `0x5b` | 91 | **`ACP_CTRL_INFO`** — body is a `ksRacing::CtrlInfo` struct: `u32 carId` + Format-B `model` + `u8 gpe` + `u8 as` + `u8 sc_active` + `u32 scalar_a` + Format-B `cam_near` + Format-B `cam_far` + `u32 scalar_b` + `f32 sc_scale` + `u32 setup_id`. (The carId is u32, not u16; there are three Format-B strings, not two — `cam_near` and `cam_far` are camera trim labels, no livery field. The earlier description of "scp/defaults" booleans, "fuel and wear floats" was wrong.) The server logs `"Ctrl Info carId %d (%s): %s"` followed by a flag string built from `gpe`/`as`/`sc_active`, then for every connection that meets a server-side filter it builds a per-recipient **`0x2b`** chat message containing a server-stringified summary and sends it over that connection's TCP socket. If the formatted message would exceed 250 bytes the server replaces it with the literal `"Received ctrl info, but message is too long. Please check logs"`. |

#### 5.6.2 UDP message IDs

Messages carried over the unreliable UDP channel on the main `udpPort`. 7 distinct IDs. These are handled by a chain of inline `if` blocks in the server rather than a central switch:

| ID (hex) | ID (dec) | Name / meaning |
|---|---|---|
| `0x13` | 19 | **Keepalive (game-client)** — two forms disambiguated by length: <br> • **3-byte form** `u8 0x13 + u16 conn_id` — gameplay keepalive sent by ACC clients. The server uses this to associate the UDP source address with the TCP connection (peer-address learning) and replies with a `0x14` message containing a `u32` server timestamp plus per-car timing hints. <br> • **7-byte form** `u8 0x13 + u32 client_ts + u16 server_port` — used by the lobby browser (the same `0x13` byte but with extra payload). The server echoes the body back as a 7-byte `0x17` reply (see below). <br> Both forms are the primary mechanism for UDP peer-address association; a reimplementation must handle both, not silently drop the unknown one. |
| `0x16` | 22 | **`PONG_PHYSICS`** — client echo of the server's `0x14` keepalive. Body: `u16 conn_id` + `u32 server_timestamp_echo` + `u32 client_offset` (11 bytes total). Used by the server to measure per-client latency and adjust simulation timestamps. The exe-side handler at `FUN_1400420e0` returns 1 when the pong is the **first ever** for the connection OR establishes a new minimum RTT, in which case the server emits an extra `0x28` immediately (see §5.6.4a). |
| `0x17` | 23 | **Keepalive (variant)** — same two-form treatment as `0x13`. The 3-byte form triggers a `0x14` reply and UDP peer-address learning; the 7-byte lobby-probe form is echoed back as a 7-byte `0x17` reply. **Note that `0x17` is ALSO used in the outbound direction** — the UDP inline handler emits `0x17` messages as part of its processing of other request messages. Server→client `0x17` is a separate message with its own wire format. |
| `0x1e` | 30 | **`ACP_CAR_UPDATE`** — the per-tick car state update, sent by each client at simulation tick rate. Total wire size: **68 bytes including the msg id byte**. Field-by-field wire format with each value's role on the AC2 receiver (sender at AC2 client `FUN_14352f920`, server parser `FUN_140042900`, server relay re-writer `FUN_14001a170`, client applicator `FUN_1434a4590`): <br><br> `u8 = 0x1e` (msg id) <br> `u16 source_conn_id` <br> `u16 target_car_id` <br> `u8 packet_sequence` (rolling counter that wraps every 256 packets — server tracks `current - previous == 1` to compute drop rate; NOT a gear/pit flag) <br> `u32 client_timestamp_ms` <br> `Vector3 position` (12 B, car +0x8 — world position x/y/z) <br> `Vector3 orientation` (12 B, car +0x14 — Euler / forward-direction; receiver applies an `acos`-based reconciliation against velocity in `FUN_1434a4590:134-198`) <br> `Vector3 velocity` (12 B, car +0x20 — server computes `sqrt(x²+y²+z²)` and clamps against a km/h threshold for the "last seen moving" gate at car +0x158) <br> `u8 input_a[4]` (car +0x2e..+0x31 — **steering input, throttle, brake, clutch** as 4 driver axes, log-encoded `sign(v) × 10^(\|v\| × 127 / log10(maxV))` for high-precision near-zero, decoded by receiver to floats; drives the **driver hand/foot animation rig** on remote cars but NOT physics — physics is server-trusted) <br> `u8 yaw_or_torque` (car +0x32, signed-biased — `engine_torque × gearRatio` or yaw-rate-derived; driver-model only) <br> `u8 steer_wheel_deg_half` (car +0x33 — `steerAngleDeg × 2` clamped [-127..127] biased +127; receiver decodes `(b-127.0) × 0.5` and uses it to **rotate remote cars' steering wheel models** in chase / replay cameras) <br> `u16 engine_rpm` (car +0x36 — engine RPM 1..65000; drives **HUD RPM bar** + **engine sound pitch** for remote cars) <br> `u8 gear` (car +0x2c — R=0, N=1, gears 2..N+1; drives HUD overlay + remote engine sound state) <br> `u8 fuel_normalized` (car +0x34 — fuel level 0..255 mapping to 0..1, matches client physics offset +0xf64) <br> `u8 damage_normalized` (car +0x35 — pairs with the fuel byte; likely tyre / aero damage 0..1, client offset +0xf68) <br> `u32 server_timebase_delta` (car +0x44 — sender writes raw client physics clock; server **rewrites** this on relay to `(car+0x3c) - per_conn_time_offset_ms` so it carries the absolute server-timebase delta the receiver needs for **dead-reckoning extrapolation**.  This field plus position + orientation + velocity is the only client→server payload that physics actually consumes; everything else is animation / audio / HUD) <br> `u8 wheel_slip[4]` (car +0x48..+0x4b — per-wheel angular speed / slip, encoded `slip × 25.0` clamped 0..255; drives **tyre smoke / skid-mark / dust particle emitters** on remote cars, parsed +0x80..+0x8c) <br> `u8 lateral_g_or_head_lean` (car +0x4c, signed-biased — chassis lateral G derived from physics +0x1420; drives **driver-head animation lean**) <br> `i16 alive_sentinel` (car +0x1ec, sign-extended — base value `0x2711 = 10001`; server gates leader-pick / green-flag eligibility on `(value - 0x2711)` falling in a positive-result sentinel range AND `+0x153 == 1` AND `(+0x1e8 & 1) == 0`. Reset to 0 makes the car "stale") <br><br> The packet sequence counter is consumed by `FUN_1400419e0` which tracks valid-vs-out-of-order rates and computes clock skew between client and server timestamps. <br><br> Server validates that the source connection owns the target car and rejects updates with mismatched ownership (`"Received car update for a different car, connectionId %d. Expected: %d Received: %d"`). It also drops outdated packets where `client_timestamp_ms` is not newer than the last seen timestamp (`"Dropped outdated car_update paket for carId %d, clientTimestamp %d vs lastTimeStamp %d"`). <br><br> **Timestamp gate (subtle)**: the strict drop logic above must be paired with two reset conditions, otherwise a single bad timestamp pins `last_timestamp_ms` ahead of every subsequent packet and freezes the car in place: <br> 1. **Connection change** — the exe records `lastDrivingConnectionID` at car +6; if a different connection ID claims the same `carId` (legitimate reconnect into the same slot), `last_timestamp_ms` is reset so old timestamps from the new owner are accepted. <br> 2. **Backwards-jump escape hatch** — if a packet arrives with `client_timestamp_ms` more than 1000 ms behind the stored value, treat it as a legitimate forward-time anomaly (clients have been observed jumping ts backwards 230 s mid-session due to driver-side game pauses), reset `last_timestamp_ms`, and accept the packet. Without this, the race freezes for that car for the rest of the session. See `accd/handlers.c` h_udp_car_update + `state.c` `car_runtime_reset_gate`. <br><br> The same id byte `0x1e` is used by server→client periodic state broadcast — see §5.6.4a — and the server-side broadcast uses the same field offsets, confirming the format is symmetric across the relay. |
| `0x22` | 34 | **`CAR_INFO_REQUEST`** — body is `u16 connectionId` + `u16 carIndex`. The server replies with a full `CarInfo` structure for the requested car. Historical ACP name is still current. |
| `0x5e` | 94 | **Peer latency report** — body: `u16 source_conn` + `u16 target_conn` + `u64 latency_raw_ms` + `u8 forward_as_chat` (12 bytes after msg id). When `forward_as_chat` is non-zero, the server emits a `0x2b` chat to the target connection with body `"Latency error: %llu ms"` (the formatted `latency_raw_ms`). When zero, the server records the figure for diagnostics only. |
| `0x5f` | 95 | **Admin / server-identity query** — the server replies unconditionally on UDP with a Format-A string containing the server's name. (The reader does not currently validate any incoming identifier or password; an exe-faithful gating step against `param_1+0x14122` was scoped for "phase 2" but isn't implemented in the reimplementation. Used by admin tooling to verify server identity over UDP without establishing a full TCP session.) |

#### 5.6.3 LAN discovery (UDP 8999)

One message ID on the fixed LAN discovery port:

| ID (hex) | ID (dec) | Direction | Meaning |
|---|---|---|---|
| `0x48` | 72 | client → server | LAN discovery probe. The client sends from a random source port (typically 8998) to the server's port 8999 as either a broadcast (`255.255.255.255:8999` for LAN) or a directed unicast (for `serverList.json` remote servers). Probe format: `u8(0xbf) + u8(0x48) + u32(nonce)` (6 bytes). The `0xbf` byte is an outer envelope used only on the discovery port. The server replies with `0xc0` (see server→client table). |

**Note the namespace overlap**: `0x48` is used on both the LAN discovery port and the main TCP channel, but with different semantics. A message is disambiguated by the transport / destination port, not just by the ID byte.

#### 5.6.4c Handshake response (accept `0x0b` / reject `0x0c`)

The server uses **two different message IDs** for accept and reject outcomes, confirmed by probing a real Kunos accServer.exe 1.10.2 instance.

##### Reject response (message id `0x0c`)

When the handshake is rejected (wrong version, bad password, server full, banned), the server sends a **14-byte `0x0c` message** and closes the connection:

```
u8   msg_id = 0x0c
u32  server_internal_version = 7
u8   0x00
u16  client_version_echo     (the version the client sent)
u16  0x0000
u16  server_protocol_version = 0x0100 (256)
u16  0x0000
```

The `server_internal_version` field is 7 for client versions below `0x100` and 8 for versions above.

##### Accept response (message id `0x0b`)

On successful accept, the server sends an `0x0b` message with a **~2000-byte trailer** containing the full session/car/config state:

Accept header (10 bytes):

```
u8   msg_id = 0x0b
u16  udp_port           (the UDP port the client should send car updates to; 9231 default)
u8   send_rate_hz       (server tick rate in Hz; default 18 = 0x12.  The AC2 client
                         computes interval_ms = (1000/hz/2)*2 — for 18 Hz this rounds
                         to 54 ms — and stores it as the "SEND INTERVAL" double at
                         receiver +0x6a0.  Logged as
                         "Translated realtime interval hzToMiliseconds(%d)=%d".
                         Any positive value is accepted; 18 Hz matches Kunos exe)
u16  connection_id      (the server-assigned id for this client)
u32  car_id             (the server-assigned car slot for this connection;
                         starts at 1001 and increments per accept)
```

Note: an earlier revision of this spec described the trailing 4 bytes as `u16 nconns + u16 padding`; that was wrong. The bytes are the assigned car_id (verified at `accd/handshake.c:1511-1515`, function `handshake_send_accept`).

Then two **u16-byte-length-prefixed raw UTF-8** strings (not Format-A):

```
u16  server_name_len + server_name_bytes    (e.g. "ACC Server (please edit settings.json)")
u16  track_name_len  + track_name_bytes     (e.g. "mount_panorama")
```

**Trailer body** (after the server_name and track_name strings):

The trailer body starts with a `u8 num_spawn_defs` count followed
by one spawnDef record per connected car.  Each spawnDef carries
the full CarInfo and DriverInfo array from the client's handshake
plus server-side race state:

```
u8   num_spawn_defs        (count of active car slots)

FOR EACH active car:
u16  car_id                (server-assigned, starts at 1001)
u8   flag_a = 0x01
u8   flag_b = 0x01
     <CarInfo blob>        (variable size: team_name, nationality, car_model,
                            cup_category, strings; copied from the CarInfo
                            region of the client's 0x09 handshake)
u8   driver_count          (typically 1 except during driver swap)
FOR EACH driver:
     <DriverInfo blob>     (variable size: first_name, last_name, short_name,
                            nationality, steam_id Format-A strings + 41 bytes
                            rating block; copied from the DriverInfo region
                            of the client's 0x09 handshake)
u8   active_driver_index   (0-based index into the drivers array)
u64  spawn_timestamp       (0 on fresh spawn)
u8   flag_c, u8 flag_d     (0 on fresh spawn)
u8   tire_dirt[5]          (0-255 per compound, 0 on fresh spawn)
u8   damage[5]             (0-255 per component, 0 on fresh spawn)
u16  elo                   (0 on fresh spawn)
u32  stability             (0 on fresh spawn)
```

After the spawnDefs comes the **SeasonEntity block** (104 bytes
for a default configuration):

```
HudRules         u8[7]          (1 configured slot + 6 unset=2 sentinels)
AssistRules      u8 u8 f32 f32 u8[6]
                 (disable_ideal_line, disable_autosteer,
                  stability_min=0.0, stability_max=1.0,
                  disable_auto_pit_limiter, disable_auto_gear,
                  disable_auto_clutch, disable_auto_engine,
                  disable_auto_wiper, disable_auto_lights;
                  2 = "unset" sentinel)
GraphicsRules    u8[6]          (0 5 0 5 0 4 defaults)
RealismRules     u8 u8 f32 u8 f32 f32 u8[9]
                 (0 0 grip=0.8 1 fuel=1.0 tyre=0.5 1 1 1 1 1 1 1 1 2)
GameplayRules    u8 u8 u8 u8 u8 u32   (0 0 2 100 100 15)
OnlineRules      u8 u8 u8 u8 u16 u8[10]
                 (0 0 formation_type, short_formation,
                  post_race_seconds=300, weather_randomness=10,
                  medals=3, auto_dq, randomize, dump_lb,
                  dump_entries, race_locked, ignore_disconnects,
                  misc, misc)
RaceDirectorRules u8 u8 u8 u32 u32 u32 u8
                 (0 0 0 count=100 tickrate=3000 fifteen=15
                  default_formation=3)
u16 vec_count[5] (4 empty vectors + 1 EventEntity count=1)
```

After the SeasonEntity block comes the **EventEntity section**, then a series of trailing blocks.  The block-by-block layout below is verified end-to-end against Kunos misano pcap (frame 46872 of `kunos_misano_2players_full_session.pcapng`, 1864 B body, 2 cars + 3 sessions); the Python walker at `accd/tests/parse_welcome.py` consumes 1864/1864 bytes with zero leftover, and our `handshake.c` emit matches Kunos byte-for-byte.

##### EventEntity track-name + body (161 B for "misano")

```
fmt_a track_name           (u8 codepoint count + N × u32 codepoints + 1 trailing byte;
                            25 B for "misano" = u8(6) + 6 u32 codepoints + 1)

EventEntity body — 136 B total, dispatched via vtable[0x28] of seven sub-objects:
  CircuitInfo         19 B   3 u8 + 4 f32                 (track sub-object at +0x48)
  GraphicsInfo         9 B   6 u8 + u16 + u8              (graphics sub-object at +0x88)
  CarSet               0 B   << EMPTY — emitting any bytes here, even u16(0),
                                crashes AC2 (verified at v0.2.47/v0.2.64) >>
  RaceRules           16 B   12 fields packed (see below) (rules sub-object at +0xf8)
  WeatherRules header 32 B   4 u8 (`01 32 03 00`) + 7 f32 (status sub-object at +0x190)
  WeatherRules forecast 60 B 15 f32                       (data sub-object at +0x1e0)
```

RaceRules layout (16 B exactly, packed field-by-field per `FUN_14011d230`; the AC2 client zero-extends each value to its struct width, so u8 fields show 0..255):

```
u8   qualifyStandingType        (0=best lap, 1=superpole)
u8   superpoleMaxCar            (0xff = no limit; 0 trips the "INVALIDE EXIGENCES" widget)
u16  pitWindowLengthSec         (0xffff = unset)
u16  driverStintTimeSec         (0xffff = unset)
u8   isRefuellingAllowed
u8   isRefuellingTimeFixed
u8   maxDriversCount
u8   mandatoryPitstopCount      (0 = no mandatory pits)
u16  maxTotalDrivingTime        (0xffff = unset)
u8   isMandatoryPitRefuel
u8   isMandatoryPitTyre
u8   isMandatoryPitSwap
u8   tyreSetCount               (default 1)
```

> Note on `FUN_1434f4810`: the AC2 reader for RaceRules consumes 18 bytes by reading 12 fields, then 2 discarded u8s, then 1 u8 stored at struct +0x48.  Our wire emits 16 bytes; the extra 2 reads consume the first 2 bytes of WeatherRules header (`01 32`), which are inert in the AC2 client because the WeatherRules header reader subsequently realigns its own cursor.  Pre-v0.3.5 attempts to emit a "true" 18-byte block (with two literal `0x01`s before tyreSetCount per the exe's wire writer) all crashed AC2 — see `reference_eventrules_wire_vs_ac2_struct.md` in the maintainer's notes.

##### Trailing blocks (after EventEntity body)

```
session_mgr_state              variable B   1 u8 idx + 7 valid/invalid records (each 1+opt 4 B) +
                                            23 B tail; misano has 6 valid + 1 invalid + tail = 55 B
Leaderboard                   variable B   outer header (12 B) + N per-car records + 2 B assist tail;
                                            misano (2 cars) is 402 B
Top-level WeatherData         variable B   11 fixed u32/f32 + i16 nSine + nSine × u32 +
                                            i16 nCosine + nCosine × u32; misano with live weather
                                            (5 sine + 1 cosine) is 72 B; empty forecast is 48 B
TrackConditions               68 B          17 f32 (FUN_14352cb30 reader)
write_track_records           variable B   u8 session_count + N × 23 B per session;
                                            misano (3 sessions) is 70 B
dirt fields                   2 B           u8 update_freq=5 + u8 delta_thresh=5
MTR (MultiplayerTrackRecord)  19 B          empty TrackRecord (3 empty kson_strings + 13 numeric bytes).
                                            Byte 4 is 0xf0 in misano pcap, 0xd0 in our emit — but the AC2
                                            client reads MTR via FUN_1434f5070 then **destroys the parsed
                                            struct** at FUN_1434d3260 in 14352a150.c:507 without copying
                                            it to the receiver state.  The byte difference is functionally
                                            harmless (the entire MTR block is read-and-discarded by the
                                            game client; emit it for wire-shape parity with Kunos and
                                            move on)
RatingSeries                  37 B          str_raw "Standard" + str_raw "" + u32(1) + 21 B empty
                                            RatingLine (3 ksstr + u8 + u32 + 3 u16 + u32)
tail                          3 B           u8 formation_lap_type + u8(0) + u8(0)
```

Misano frame 46872 totals **1864 B** body.  All sub-objects above are dispatched via `vtable[0x28]` (`readFromPacket`) of the corresponding receiver class on the AC2 client; the receiver-side classes are `ksRacing::CircuitInfo`, `ksRacing::GraphicsInfo`, `ksRacing::CarSet` (empty), `ksRacing::EventRules`, `ksRacing::WeatherStatus` (header), `ksRacing::WeatherData` (forecast and top-level), `ksRacing::TrackConditions`, `ksRacing::TrackRecord` (MTR), `ksRacing::RatingSeries`, `ksRacing::RatingLine`.  The full top-level reader is `FUN_14352a150`.

##### Post-accept welcome sequence

After the `0x0b` response, the server immediately sends three additional messages to the joining client (confirmed by wire capture):

1. **`0x28` SRV_LARGE_STATE_RESPONSE** (56 bytes) -- session timing + assist rule snapshot; each f32 value is prefixed by `u8(1)`
2. **`0x36` SRV_LEADERBOARD_BCAST** (~120-210 bytes) -- initial leaderboard with per-car entries including car_model, cup_category, and four driver strings (steam_id, short_name, first_name, last_name as Format-A)
3. **`0x37` SRV_WEATHER_STATUS** (69 bytes) -- current weather snapshot

The server also fans out `0x2e` (car system relay) and `0x4f` (driver stint relay) to all OTHER already-connected clients to notify them of the new car joining.

#### 5.6.4a Server → client message ID catalog

**The server → client direction uses a separate ID namespace from client → server.** An ID like `0x4f` sent from client to server is not the same message as `0x4f` sent from server to client; the two directions have independent handler tables with independent wire formats.

31 distinct server → client message IDs have been identified across two disjoint connection roles:

- **IDs `0x01`–`0x07`** are emitted **only to SMPR (ServerMonitor) connections** — external monitoring tools such as accweb, accservermanager, emperorservers.  The game client never receives them and has no parser for them in `AC2-Win64-Shipping.exe`.  See §12B for the full protocol.
- **All other IDs** are emitted to game-client connections.  That's the gameplay sim protocol.

The dedicated server tells the two roles apart at dispatch time via a flag stored at connection-struct offset `+0x1403e`: `FUN_140041480` routes to the SMPR handler (`FUN_140041ac0`, which emits `0x01`–`0x07`) when the flag is set, and to the game-client handler (`FUN_1400142f0`) otherwise.  An accd reimplementation that only targets the game-client path can safely skip `0x01`–`0x07` entirely.

| ID (hex) | ID (dec) | Body fields | Known semantic |
|---|---|---|---|
| `0x01` | 1 | `u8 = 0x01` + protobuf-encoded `ServerMonitorHandshakeResult` | **`REGISTRATION_RESULT`** — same protobuf message type as ServerMonitor protocol message 1 (see §12B). Emitted from the SMPR connection accept path with the log `"Received SMPR connection %d for %s"`. The body fields are the protobuf schema documented in §12B.3 (`bool success`, `int32 connectionId`, `string errorTxt`). |
| `0x02` | 2 | `u8 = 0x02` + protobuf-encoded `ServerMonitorConfigurationState` | **`SERVER_CONFIGURATION`** — same protobuf message type as ServerMonitor protocol message 2. Carries the server config state (track / session / connection list). |
| `0x03` | 3 | `u8 = 0x03` + protobuf-encoded `ServerMonitorSessionState` | **`SESSION_STATE`** — same protobuf message type as ServerMonitor protocol message 3. Sent when the session changes and as part of the initial state push to new clients. Paired with `0x06`/`0x07` in the periodic tick. |
| `0x04` | 4 | `u8 = 0x04` + protobuf-encoded `ServerMonitorCarEntry` | **`CAR_ENTRY`** — same protobuf message type as ServerMonitor protocol message 4. Per-car entry record fanned out from the welcome push (one `0x04` per connected car) and from the handshake handler. |
| `0x05` | 5 | `u8 = 0x05` + protobuf-encoded `ServerMonitorConnectionEntry` | **`CONNECTION_ENTRY`** — same protobuf message type as ServerMonitor protocol message 5. Per-connection entry record (driver name + connection metadata). Fanned out from the handshake handler (one `0x05` per connection). |
| `0x06` | 6 | `u8 = 0x06` + protobuf-encoded `ServerMonitorRealtimeUpdate` | **`REALTIME_UPDATE`** — same protobuf message type as ServerMonitor protocol message 6. Periodic per-tick state update emitted from the per-client queued send mechanism. |
| `0x07` | 7 | `u8 = 0x07` + protobuf-encoded `ServerMonitorLeaderboard` | **`LEADERBOARD_UPDATE`** — same protobuf message type as ServerMonitor protocol message 7. Per-car leaderboard / standings update emitted in the per-car fan-out after every `0x36` leaderboard broadcast and during the post-handshake welcome push. |

**Critical architectural note on `0x01`–`0x07`**: these seven message ids are exactly the **ServerMonitor protocol message types 1–7** documented in §12B, delivered over TCP to SMPR connections only — **never** to the game-client connection.  The dedicated server uses the same C++ classes (`ServerMonitorHandshakeResult`, `ServerMonitorConfigurationState`, `ServerMonitorSessionState`, `ServerMonitorCarEntry`, `ServerMonitorConnectionEntry`, `ServerMonitorRealtimeUpdate`, `ServerMonitorLeaderboard`) and the same protobuf wire format in both places, but the routing is disjoint:
- SMPR connections (hosting-tool clients) receive `0x01`–`0x07` exclusively.
- Game-client connections (the ACC sim) receive every other id in this table.

The dispatching wrapper is a single helper `FUN_14002e080(msg_id, polymorphic_object, send_target)` that:
1. Initializes a ByteVector
2. Writes the `u8` msg_id byte
3. Calls `vtable[0x58]` (`getSerializedSize()`) on the object to get the buffer size
4. Calls `FUN_140053e50` which calls `vtable[0x68]` (`serializeInto()`) to write the protobuf bytes
5. Concatenates the result into the ByteVector
6. TCP-sends via `FUN_14004cc50`

There are exactly **7 caller sites** in the binary that build the per-class objects:

| Caller | IDs | Object class | Purpose |
|---|---|---|---|
| `FUN_140041ac0` | `0x01` | `ServerMonitorHandshakeResult` | SMPR connection accepted, after `"Received SMPR connection %d for %s"` |
| `FUN_14002e210` | `0x02` | `ServerMonitorConfigurationState` | Server configuration state push |
| `FUN_14002aca0` | `0x03` | `ServerMonitorSessionState` | Race weekend reset / event change |
| `FUN_14002f710` | `0x03`, `0x07` | `ServerMonitorSessionState` + `ServerMonitorLeaderboard` | Server tick tail (periodic state + per-car fan-out after each `0x36`) |
| `FUN_140025690` | `0x04`, `0x05` | `ServerMonitorCarEntry` + `ServerMonitorConnectionEntry` | Handshake handler, fans out per-car and per-connection records |
| `FUN_14001ce70` | `0x06` | `ServerMonitorRealtimeUpdate` | Per-client queued realtime state event |
| `FUN_14001ca20` | `0x02`, `0x03`, `0x04`, `0x07` | mix | Post-handshake welcome state push (full sync sequence) |

**A reimplementation that wants SMPR compatibility can use the protobuf schemas documented in §12B.3 to encode the bodies of `0x01`–`0x07` directly** — there is no separate undocumented wire format.  The seven ids are just numbered transport tags identifying which `ServerMonitorProtocolMessage` type follows.  A game-only reimplementation can skip them entirely; no game client depends on them.
| `0x0b` | 11 | `u16 udp_port` + `u8 0x12` + `u16 nconns` + `u16 conn_id` + `u16 0` + trailer (~2000 bytes) | **Handshake accept response** -- see 5.6.4c. Contains the welcome trailer with session/car/config state. |
| `0x0c` | 12 | `u32 server_ver` + `u8 0` + `u16 client_ver_echo` + `u16 0` + `u16 protocol_ver` + `u16 0` | **Handshake reject response** (14 bytes) -- see 5.6.4c. Connection closed after sending. |
| `0x14` | 20 | `u32 server_ms` + `u16 conn_rtt` + `u16 avg_ping` + `u16 max_ping` + `u8 2` + `u8 4` + `u8 100` + `u8 100` (15 bytes) | **UDP keepalive response** — sent in reply to client `0x13`/`0x17`. The `server_ms` field is a monotonic clock timestamp (milliseconds) that the client echoes back in its `0x16` pong to measure round-trip latency. **The three middle u16 slots are NOT zero** — they carry the per-recipient `conn_rtt`, `avg_ping`, and `max_ping` values measured from the connection's pong history (verified against `kunos_wine_full_race.pcap`; emitted at `accd/tick.c:309-328`, `build_keepalive_pkt`). <br><br> The trailing four bytes `2, 4, 100, 100` are **interpolation / lag-compensation tunables**: AC2 (`FUN_1435276f0:126-294`) reads each as a u8 then divides by 100.0f and stores as a float at `param_1+0x380, +0x384, +0x388, +0x38c` (the stored values are `0.02, 0.04, 1.0, 1.0`). The two small floats look like prediction fudge factors; the two `1.0`s look like blend weights for the dead-reckoning code path. <br><br> **This is the 1 Hz heartbeat**, not `0x28` — see the `0x28` row below for the corrected emit cadence. |
| `0x1b` | 27 | `u8 0x1b` + `u16 cup_position` + `u16 track_position` + `i32 lap_time_ms` + `u8 quality` (10 bytes total) | **Lap time broadcast** — the server forwards a client's lap-time report to all other clients. Triggered by a client sending TCP id `0x19` (see §5.6.1). The two u16s are the reporting car's standings positions (cup-class `cup_position` and overall `track_position`); names inferred from ACC HUD semantics — the AC2 reader at `143526030.c` case `0x1b` doesn't label them but stores the pair into a 24-byte `LapBroadcastEntry` at receiver `+0x488`. The quality byte is signed `char` on the receiver: negative → "invalid lap" → quality float `-1.0f`; non-negative → `quality / 10.0f` (0..N float on the AC2 side). |
| `0x1e` | 30 | `u8 0x1e` + 63-byte per-car record (64 bytes total) | **Per-car periodic state broadcast — fast-rate, single-record variant.** Pushed from the main server tick (`FUN_14001a170` in accServer) for cars whose dirty flag is set since the last emit. The record body is byte-identical to the `0x39` bundled variant's per-car records. Field map of the 63-byte record (offsets relative to record start, after the msg id): <br><br> `+0x00 u16 car_id` (server source `Car+0x150`) <br> `+0x02 u8 input_state_a` (source `Car+0x2d` — sequence/dirty flag, AC2 compares against last-seen for ordered drop test) <br> `+0x03 s32 server_time_delta_ms` (server-rewritten on relay: `Car+0x3c (last_received_client_ts) − server_basetime_ms`; gates the AC2 1 s look-back drop logic) <br> `+0x07 u16 tick_seq` (source `Car+0x50`) <br> `+0x09 12 B position` (xyz f32 vec3) <br> `+0x15 12 B velocity` (xyz f32 vec3) <br> `+0x21 12 B orient_normal` (heading-basis vec3, used for the reverse-direction check) <br> `+0x2d 4 × u8 input_a` (steer / clutch / handbrake / aux as log-encoded signed values, AC2 decodes via `sign(b−128) × (10^((b−128)/k))`) <br> `+0x31 u8 gear` (source `Car+0x32`, signed-biased `b−127`) <br> `+0x32 u8 steer_wheel_deg_half` or fuel% (source `Car+0x33`, decoded `(b−127) × 0.5`) <br> `+0x33 u16 engine_rpm` (source `Car+0x36`, raw u16 1..65000) <br> `+0x35 u8 input_state_b` (source `Car+0x2c` — damage / lights / aux flags bitfield) <br> `+0x36 u8 throttle` (source `Car+0x34`, normalized to 0..1 via `b/255`) <br> `+0x37 u8 brake` (source `Car+0x35`, normalized to 0..1 via `b/255`) <br> `+0x38 4 × u8 wheel_slip` (source `Car+0x48..+0x4b`, per-wheel slip 0..255 → `b/25.0` as float; FL/FR/RL/RR) <br> `+0x3c u8 lateral_g_or_torque` (source `Car+0x4c`, signed `b−127.0` on receiver) <br> `+0x3d s16 trailing_scalar` (server-rewritten on relay: `Car+0x1ec` clamped to `[-0x8000, 0x7fff]`; carries lap-progress / interpolation-smoothing data) <br><br> Total = `2+1+4+2+12+12+12+4+1+1+2+1+1+1+4+1+2 = 63 B`. The same id byte is used by client→server `ACP_CAR_UPDATE` (68 B body in that direction, see §5.6.2) — the C→S has the conn_id at offset 4 which the server strips on relay, replacing those 4 bytes with the server-timebase delta and dropping 4 B from the C→S 68 to land at 64 here. |
| `0x39` | 57 | `u8 0x39` + `u8 record_count` (≤ 8) + `record_count × 63 B per-car record` | **Per-car periodic state broadcast — slow-rate, batched variant** (sibling of `0x1e`). Built by `FUN_14001a6a0` (accServer); contains the self-check log `"CarUpdate size is unexpected ... %d byte expected"` with constant `0x3f = 63` for the per-record body size. The record body is byte-identical to the `0x1e` record map above. The leading `u8 record_count` is **NOT** a per-record context byte (an earlier revision of this spec said so — that was wrong); it is a bundle-level count of `min(remaining_dirty_cars, 8)`. The server caps each `0x39` packet to 8 cars and re-emits another `0x39` for the next batch. With max N=8 the packet is `2 + 8×63 = 506 B`. The slow-rate loop walks every car whose dirty bit is set since last emit and bundles them; bundles are sent only to peers that pass `FUN_140041640` (peer-state OK gate). |
| `0x23` | 35 | Per-car record (variable-length) | **Car info response over TCP** — the server's reply to a client sending UDP `0x22 CAR_INFO_REQUEST`. Body is built by the same per-connected-car record appender used in the handshake welcome trailer, so the layout matches the per-car record in the welcome sequence. |
| `0x24` | 36 | `u16 carIndex` | **`CAR_DISCONNECT_NOTIFY`** — the server tells every other client that this car has disconnected |
| `0x28` | 40 | `u8 = 0x28` + `u8 session_index` + 7 × per-slot record + 23-byte tail | **SessionManager state push** — built by `FUN_140033890`. **Event-driven, NOT 1 Hz**: the exe emits `0x28` only when the session index changed, the per-slot descriptor changed, or the phase byte changed (see `FUN_14002f710:716-749`); the typical end-to-end emit-to-recipient latency is ~3 ms. The 1 Hz heartbeat is `0x14` keepalive, NOT `0x28` (an earlier revision of this spec described a "~1 s cadence" — that was wrong). An additional **pong-driven extra emit** path also fires: when the per-conn pong handler (`FUN_1400420e0`) detects either a first-ever pong OR a new minimum RTT, it returns 1 and the server immediately sends one extra `0x28` to that connection. Each per-slot record is `u8 valid` + (if valid) `f32 ts_in_client_clock`. The 23-byte tail (`FUN_140034f60`) is: `u8 hour_of_day, u8 0, u8 race_flag (1 if session_type==R, else 0), f32 1.0 grip, u16 sched_field (80 for race / 3 otherwise), u32 session_duration_s, u32 session_overtime_s, u8 0, u8 session_type (P=0, Q=4, R=10), f32 1.0`. Tail byte +2 toggles 0/1 strictly on race vs non-race. Total size varies (32/40/44/56 bytes) depending on how many of the 7 per-slot timestamps are populated. |
| `0x2b` | 43 | (varies — see semantics column) | **Generic chat / system message** — used in several distinct shapes that share the same id byte and the same dispatch path to clients: <br> **(a) Short state variant**: `u8 + u32 + u8` (6 bytes) — emitted by the small builders, payload is a connection-id-or-timestamp `u32` and a single-byte flag. <br> **(b) Chat reply variant**: `u8 + 2× string + i32 + u8 = chat_type` — emitted by the chat command parser as the reply to almost every admin command (`/admin`, `/track`, `/restart`, etc.). The chat type byte distinguishes "system message" (5) from "regular chat" (other values). Sent either to the issuing admin alone (when `cVar20 == '\0'`) or broadcast to every connection. <br> **(c) Kick / ban notification variant**: `u8 + 2× string + i32 + u8 = 5` where the first string is the human-readable reason (`"You have been kicked from the server"` / `"You have been banned from the server"`). Sent directly to the target client immediately before the connection is force-closed. <br> **(d) Ctrl info forward variant**: emitted by client→server case `0x5b` (`ACP_CTRL_INFO`) — see §5.6.1 — to forward a client's controller info to other admins as a chat-formatted summary. |
| `0x2e` | 46 | `u8 = 0x2e` + `u16 carId` + `u64 system_data` (11 bytes) | **Two distinct uses sharing an identical wire format**: <br> **(a) New-client-joined notify** — pushed by the server to every existing client during a new client's handshake-accept sequence; the carId and timestamp are the joining car's identity. The sender function **also emits a paired `0x4f` sub-opcode-1 message** (12 bytes: `u8 + u16 carId + u8=1 + u64`) right after the `0x2e`, so the full new-client notification is two messages in sequence. <br> **(b) `ACP_CAR_SYSTEM_UPDATE` relay** — broadcast to every other client when one client sends an `ACP_CAR_SYSTEM_UPDATE` (client `0x2e`, see §5.6.1). The carId is the source car and the u64 carries the new system state. The two variants share both the id byte and the wire layout — they're distinguished only by call-site context. |
| `0x2f` | 47 | `u8 = 0x2f` + `u16 carId` + `u8 tyreCompound` (4 bytes) | **`ACP_TYRE_COMPOUND_UPDATE` relay** — server-transformed broadcast of client `0x2f`. Sent to every other connected client when one client changes its tyre compound. |
| `0x36` | 54 | `u8 = 0x36` + `u32 session_meta` + `u8 split_count` + `u32[split_count]` + `u8 cvar8` (always 1) + `u16 entry_count` + `entry_count × {per-car leaderboard record, ~80–200 bytes each}` + `u8 + u8` trailer | **Standings / leaderboard broadcast** — emitted from the main server tick tail when the leaderboard recomputation has completed. Each per-car entry has a complex variable-length record produced by `FUN_140034210`. The 4-byte header of each per-car record carries `u16 car_id` + `u16 race_number` + `u8 car_model` + `u8 cup_category` + `u16 reserved`; the `car_model` byte comes from the **runtime Car struct at +0x58** and `cup_category` from **runtime Car +0x5c**, NOT from the original handshake CarInfo (which has `carModelType` at +0xf0). Conflating the two has shipped at least one regression where every PRO driver rendered as Porsche 991 GT3 R; see the in-code comment at `accd/handshake.c:664-673`. After this broadcast the function iterates every entry list item and emits a follow-up per-car `0x07` message via the generic serializer (`FUN_14002e080(7, ...)`). Server log after broadcast: `"Updated leaderboard for %d clients (%s-%s %d min)"`. <br><br> **Outer `cvar8` byte** must be 1 unconditionally. AC2's per-line reader `FUN_14352ae00:100-103` only refreshes the per-car `LeaderboardLine +0x204 missingMandatoryPitstop` field from the wire when this byte is non-zero; otherwise AC2 leaves +0x204 at its constructor default `0xffffffff` (read as u8 = 255). The default value rendered as 255 was the source of the long-running "OBLIGATOIRE 255/0 INVALIDE EXIGENCES" widget in race sessions — fixed in v0.3.7 (commit `91276f8`) by always emitting `cvar8 = 1` + per-car inner byte = 0 (matching Kunos misano). <br><br> **Per-car cvar8-gated byte** (the byte emitted right after the active-penalty block when cvar8 != 0) must be the **count of mandatory pit stops still owed by this car**, NOT `formation_mid_passed` (the formation-lap progress flag). Earlier accd revisions emitted `formation_mid_passed` here on the theory that the byte controlled a formation-lap HUD gate; the actual semantic on the AC2 receiver is `missingMandatoryPitstop`. For sessions without mandatory pits emit `0`; for sessions with mandatory pits the value must track the live pending-stop count per car (currently always-zero in accd, will need a follow-up to source from the timing module when `mandatoryPitstopCount > 0`). <br><br> The "INVALIDE" red badge alongside the OBLIGATOIRE counter is rendered by a `IsValidForMandatory(isDriverSwapRequired, isTyreChangeRequired, isRefuellingRequired) -> bool` UFunction (FName cluster at file offset `0x3c87e60`, va `0x143c87e78`) that returns false when `missingMandatoryPitstop > mandatoryPitstopCount`. <br><br> Per-car HUD-widget notes: `lap_history` empty case must emit `u8(0)` (NOT three `LAP_TIME_INVALID` sentinels — the sentinels force `wide_flag=1` for every car pre-race, switching the sector list to u32 encoding while the exe stayed in u16). The orange-1 badge in the HUD tile is "mandatory pit pending" (driven by the same `+0x204` field), not a penalty. |
| `0x37` | 55 | `u8 = 0x37` + 17 × f32 (69 bytes total) | **Periodic weather status broadcast**. Outer wire is the **`TrackConditions` block (17 floats)** — built by `FUN_1400330e0` and emitted at the cadence gated by `_DAT_14014bd38`. The 9 inner `WeatherStatus` floats correspond to the exe's `WeatherStatus::serialize` output (`FUN_14011e930` writes from struct offsets `0x28, 0x2c, 0x30, 0x34, 0x3c, 0x38, 0x40, 0x44, 0x48`); the outer block adds 7 leading f32 (TrackConditions outer fields) and a 1 trailing f32 (weekend time). Slot layout: <br><br> `[0]` grip-now (≈ `1 - clouds*0.3`) <br> `[1]` grip-green (constant from `randomizeGreenFlagTriggers`, e.g. 0.97) <br> `[2..4]` reserved zeros <br> `[5]` track wetness (current) <br> `[6]` track wetness (target / mirror of [5]) <br> `[7]` ambient temp °C <br> `[8]` road temp °C <br> `[9]` wind speed <br> `[10]` wind direction (degrees, signed; can be negative) <br> `[11]` cloud level (0..1) <br> `[12]` rain level (0..1) <br> `[13]` WS+0x40 — dry-line wetness / puddles factor (~0.8 idle) <br> `[14]` WS+0x44 — `tanhf`-normalized 0..1; **stored at AC2 receiver `WeatherStatus +0x44` but no game-side consumer**: not exported by JSON, not read by graphics shaders, physics, AI driver routines, or environment effects.  The default constructor (`FUN_14106db70`) only initialises through +0x40, so this slot is zero-by-default on the client.  Audit of all 28 vftable users found only bulk struct-copies (debug overlays read it into locals that are never used).  Emitting `0.0f` is safe. <br> `[15]` WS+0x48 — same as [14]: stored, never read.  Inert on the AC2 client. <br> `[16]` weekend time as f32 seconds <br><br> The same 17-float layout is used for the **welcome trailer's TrackConditions** block (see §5.6.4c above). The reader is `FUN_14352cb30` on the AC2 side, which reads 7 leading u32 + a vtable[0x28] dispatch into the WeatherStatus sub-object (24 B = 6 f32) + 1 trailing f32 — the welcome and the 0x37 broadcast share the same wire shape. The slot mis-attribution that put cloud/rain/wind in slots `[9]/[11]/[12]` was a pcap mis-read fixed 2026-04-17. |
| `0x39` | 57 | 59-byte fixed-layout record | **Per-car periodic state broadcast — slow-rate variant** (sibling of `0x1e`). Same 58-byte layout as `0x1e` plus **one extra `u8` context byte** right after the msg id. The server pushes `0x39` at a slower cadence than `0x1e`; the two together form the complete per-car state pipeline. The extra byte likely indicates the update reason (race vs qualifying, full-sync vs delta, etc.). |
| `0x3a` | 58 | `u16 car_id` + `u8 split_count` + `u32[count]` + `i32 clock` + `u16 car_field` | **Sector splits broadcast (game protocol)** — server-transformed relay of client `0x20` messages. Variable-length body (depends on split count). **Note**: a separate message with the same first byte `0x3a` exists on the lobby backend connection with a completely different fixed-15-byte body (`u8=0xc9 + u32 + u32 + u8=0x00 + u32`) — that's the lobby registration request. The two messages are distinguishable only by which TCP channel they flow on. |
| `0x3b` | 59 | `u16 car_id` + `u32 split_time` + `u8` + `u32 lap_time` + `u16 flags` | **Single sector split broadcast** — server-transformed relay of client `0x21` messages. Fixed 14-byte body. |
| `0x3c` | 60 | `u8 = 0x3c` + `u16 carId` + `u16 cuts` + `u32 ts` (9 bytes total) | **Out-of-track relay** — broadcast to every other client whenever the source client's `0x3d` (`ACP_OUT_OF_TRACK`) handler increments the cuts counter. The `cuts` u16 is the running track-cut count for the car; `ts` is the normalized session timestamp. |
| `0x3e` | 62 | `u8 = 0x3e` + `u8 result_count` + `result_count ×` per-car result record | **Session results broadcast** — emitted from the main server tick tail when a session ends. Each per-car result record is built by `FUN_1400351f0` from a 336-byte (`0x150`) source struct and contains: <br><br> `u8 + u8 + u8 (val−1) + u32 + u16 + u32 + u32 + u8 + u8 + u32` (24-byte fixed header — position, cup position, driver flag, lap count, sector counts, final time, status flags) <br> followed by a complete per-car leaderboard record built by `FUN_140034a40` from offset +0x98 of the source struct (the same outer serializer used by `0x36`, so each result row has the full standings record with sector splits, driver list, ratings, etc.) <br><br> So each per-car row is approximately `24 + (variable leaderboard data ~80–200 bytes) = ~100–250 bytes`. After the broadcast the function checks the session type and waits `postQualySeconds` (qualy) or `postRaceSeconds` (race) before advancing to the next session. Server log: `"Send session results to %d clients (%d byte)"`. This is the **session result finalization** message clients use to populate the post-session standings screen. The wire data is the same as the JSON written to `results/YYMMDD_HHMMSS_*` files. |
| `0x3f` | 63 | `u8 = 0x3f` + `u8 grid_count` + grid_count × `{u16 carId, u8 ?, u32 grid_position, u8 ?}` (5+9N bytes) | **Race start grid positions broadcast** — emitted from the main server tick tail when the session phase reaches state `'\x04'` (race countdown / pre-race). The function gathers the entry list grid positions via `FUN_140032400`, then writes one record per car. Each record is 8 bytes on the wire: `u16 carId` + `u8 flag_a` + `u32 grid_position` + `u8 flag_b`. Server logs `"Sending grid positions:"` followed by `"   Car %d Pos %d"` per record, and `"Send grid positions to %d clients (%d byte, %d grid results)"` after the broadcast. A reimplementation must emit this when transitioning into race countdown so the client can populate its starting-grid display. |
| `0x40` | 64 | `u8 = 0x40` + serialized `WritableRaceStructure` / `RaceWeekendForecast` (variable-length, virtual serializer) | **Race weekend reset broadcast** — emitted by the "Resetting weekend to friday night" path that runs when admin uses `/restart` to restart the entire race weekend or when a new event is loaded. The function writes the `cfg/current/{configuration,event,settings,entrylist,eventRules}.txt` files, applies new weather rules (with retry-loop log `"Found weather obeying the rules in %d ms (%d tries, %d)"`), and pushes one `0x40` message to every connected client. The body after the id byte is built by a virtual serializer method (`vtable[0x20]`) — variable length depending on the WritableRaceStructure or RaceWeekendForecast snapshot being sent. |
| `0x44` | 68 | (varies — see semantics column) | **Two distinct uses of this id byte**, disambiguated by transport: <br> **(a) Damage zones broadcast** — sim-protocol UDP relay. Wire: `u8 = 0x44` + `u16 carId` + `5 × u8 damage_intensity` (8 bytes total). Server-transformed relay of client `0x43`, with each damage value clamped to a maximum constant (`DAT_14014bd78` ≈ 255) before truncation to u8. Broadcast to every other connected client via the standard `broadcast_except_one` helper. Emit site: `accd/handlers.c:1119`. <br> **(b) Lobby registration request** — sent to Kunos's `kson` backend over the lobby TCP channel only when `registerToLobby: 1`. Wire shape and field map are documented in §11.5 (`0x44 Registration`). Irrelevant for private MP. Emit site: `accd/lobby.c:351` (constant `LOBBY_MSG_REGISTER`). <br> An earlier revision of this spec mentioned a third "smaller game-protocol variant" (c); no emit site for it exists in this codebase or in the AC2 client decomp from prior reverse-engineering passes — that entry was apocryphal and is dropped. |
| `0x46` | 70 | (not currently emitted) | **Car dirt status — never relayed**. The accd reimplementation receives client `0x45` `ACP_CAR_DIRT_UPDATE`, stores the values for use in the welcome trailer's per-car spawnDef, but does NOT broadcast a `0x46` relay; Kunos's exe also drops it (per pcap evidence). Earlier revisions of this spec described an 8-byte relay (msg id + carId + 5 × u8 dirt) — that emit path is not present in either Kunos or this reimplementation. |
| `0x47` | 71 | `u8 = 0x47` + `u16 carId` + `u8 driver_count` + `driver_count × u8 swap_state` (4 + N bytes) | **Driver swap state broadcast** — server-transformed relay of client `0x47` `ACP_UPDATE_DRIVER_SWAP_STATE`. The body carries the carId and the per-driver swap state byte (one byte per driver in the entry, value range 0–5 corresponding to the swap state machine: 0=idle, 1=requested, 2=foreign, 3=requested-pending, 4=executing, 5=done). Built by `FUN_140011bf0` and broadcast via direct TCP send to every connection in the entry list (not via the standard `broadcast_except_one` helper). |
| `0x49` | 73 | `u8 result_code` (2 bytes total) | **Driver swap result** — TCP reply sent directly to the client that issued an `ACP_EXECUTE_DRIVER_SWAP` (client `0x48`). The result code is 0 on success and a non-zero error code otherwise. The server logs `"Driver swap result: %d"` immediately after sending. Single recipient — never broadcast. |
| `0x4b` | 75 | `u8 = 0x4b` + welcome trailer (built by `FUN_140033980`, same builder as the `0x0b` handshake response trailer) | **Welcome trailer redelivery** — sent to a specific client during the **race weekend reset / event change** flow (`FUN_14002aca0`, the function that runs `"Resetting race weekend"` and `"Event changed"`). The body carries the same welcome trailer structure as the `0x0b` handshake response (carIndex + trackName + eventId + session list + entry list + per-car records — see §5.6.4c). After sending, the function sets the per-connection state field at `+0xa01d4 = 1` and updates a timestamp at `+0xa01e0`, indicating the client has been re-welcomed for the new event. Used so that already-connected clients receive a fresh entry list / track / session state when the admin uses `/track <name>` or when the weekend resets. A reimplementation that supports event changes mid-server must emit `0x4b` to all connected clients after the new event is loaded. |
| `0x4e` | 78 | `u8 = 0x4e` + `u8 conn_count` + per-connection record `{u16 conn_id, u8 zero, i16 ratingA×100, i16 ratingB×100, u32 (read-and-discarded by AC2), Format-A steam_id}` | **Per-connection rating summary** — Kunos misano wire (capture `s2c_0x4e_0011_1776014739.473.bin`) is **13 B pre-string** per entry: `u16 + u8 + u16 + u16 + u32 + str_a`.  AC2 reader at `143526030.c:786-832` reads `u16 car_id, u8 zero, u16 sa, u16 tr, u32 (return value DISCARDED at line 807), kson_str steam_id`.  The two i16 ratings are encoded ×100 (so 20850 = 208.50); the trailing u32 is read-and-discarded — its bit pattern is observed as `0xFFFFFFFF` in disconnect captures and a per-driver value in periodic emits, but **AC2 never stores it**, so its purpose is purely on the server side (probably a sentinel mark for "disconnecting") or for the broader accServer telemetry pipeline. <br><br> **`accd` divergence**: `handshake.c:875-907` (`build_rating_summary`, periodic + welcome path) currently emits without the trailing `u32` and without two extra `i16(-1)`s — net 11 B pre-string, AC2 mis-reads sa/tr from the wrong slots. `state.c:272-289` (disconnect path) emits TWO extra `i16(-1)` sentinels ahead of the `u32 0xFFFFFFFF` — net 17 B pre-string, AC2 then reads our two i16(-1)s as the discard-u32 (bit pattern `FF FF FF FF` happens to match, no immediate misalignment) but then reads our actual `u32 0xFFFFFFFF` as the str_a u8 length-prefix (= 0xFF = 255 codepoints), triggering a 1020-byte over-read that garbles the steam_id parse.  See `tmp/kunos_misano_extracted/s2c_0x4e_0011_1776014739.473.bin` for the reference wire; the fix is to drop both `wr_i16(-1)` calls in `state.c:278-279` and emit the canonical 13-B layout. |
| `0x4f` (sub 0x00) | 79 | `u8 0x4f` + `u16 carId` + `u8 0x00` (4 bytes total) | **Driver-stint reset — plain notice variant.** Emitter: accServer `FUN_140017f70` (vtable slot 2 of an internal lambda).  AC2 client receiver: `143526030.c:833-857` case `0x4f` — appends a `DriverStintEntry` to the vector at `param_1+0x5c8/+0x5d0/+0x5d8` with `{u16 carId, has_ts=false, ts=0, recv_clock = client+0x650}` (0x18 B stride).  HUD computes elapsed stint as `now - recv_clock` (i.e. starts the stint timer at packet receive time).  Triggered when a client sends C→S `0x4f` with a `mode` byte of 0 (the "force-reset" path). |
| `0x4f` (sub 0x01) | 79 | `u8 0x4f` + `u16 carId` + `u8 0x01` + `u64 server_stint_epoch_ms` (12 bytes total) | **Driver-stint reset — anchored variant.** Emitter: accServer `FUN_1400180b0`.  Same AC2 receiver path as sub-0 but the entry is stored as `{u16 carId, has_ts=true, ts=server_stint_epoch_ms, recv_clock}` so the HUD anchors elapsed stint at the server-supplied epoch instead of receive time. <br><br> Triggered in two distinct flows: <br> 1. **C→S 0x4f reset request with mode != 0** — server forwards the validated client timestamp as `server_stint_epoch_ms`. <br> 2. **New-client welcome cascade** at `FUN_14002dcb0` (accServer): for every existing car the server emits the standard `0x2e` driver descriptor; **if** the session is past warmup (gated by `0 < *(int*)(server_state[0x140f3] + 0x15c)`), the server **also** emits a paired `0x4f` sub-1 with `FUN_140041fc0` (active stint epoch) right after the `0x2e`.  Skipping the gate or emitting sub-0 on welcome cascade makes HUD timers start ticking from connect time instead of the actual stint start; emitting sub-1 with the correct anchor preserves continuity for late joiners. |
| `0x53` | 83 | `u8 = 0x53` + `u16 carId` + `u16 restrictor_pct100` + `u32 ballast_kg` (9 bytes total) | **Multiplayer BOP (Balance of Performance) update** — pushed to every connected client whenever an admin issues a `/ballast <carNum> <kg>` or `/restrictor <carNum> <pct>` chat command. Wire field order is **restrictor before ballast**: `u16 carId` (the affected car) + `u16 restrictor_pct100` (the restrictor percentage × 100, e.g. 5000 = 50.00 %, range 0..9999) + `u32 ballast_kg` (a raw u32, NOT a float, valid range ±40 kg sign-extended). An earlier revision of this spec had the field order reversed (ballast before restrictor) and labelled the second field `restrictor_float` — both are wrong; see in-file comment at `accd/chat.c:171-179` and the emit site at `chat.c:181-186`. The chat-output reply `"Assigned %d kg to car #%d"` (or `"... %d %% to car #%d"`) is sent separately as a `0x2b` chat broadcast. |
| `0x56` | 86 | `u8 = 0x56` + `u16 (session_type<<8 \| car_id)` packed + `i16 lap_count` + `lap_count × Lap_record` + per-car leaderboard record (variable, via `FUN_140034210`) | **Lap history response** (NOT a setup data response — `0x55`/`0x56` is the in-game garage's "Previous Laps" panel; there is no car-setup blob in this wire). Server's reply to a client `0x55` (`ACP_LOAD_SETUP` is a misnomer in the constant name). The body carries the requested session-type + target car id (packed into one u16 by `FUN_1400328f0`), the count of laps in that session's history, the per-lap records, and finally a complete per-car leaderboard record (same record format used by `0x36`). Sent over the requesting client's TCP socket only. <br><br> **Per-Lap record wire format** (built by `FUN_1400328f0` for each Lap struct in the history vector at 0x60-byte stride): <br> `str_a track_name` (variable Format-A wstring — u8 codepoint count + N × u32 codepoints) <br> `u32 lap_time_ms` (from Lap struct +0x28) <br> `u8 split_count` (= sector_splits_vector_size_bytes / 4) <br> `split_count × u32 split_time_ms` (from Lap struct +0x30 vector) <br> `u16 car_id` (from Lap struct +0x60) <br> `u8 lap_quality` (from Lap struct +0x5c — flags / quality bits) <br> `u16 lap_number` (from Lap struct +0x54) <br><br> A typical Lap record is approximately 30-50 bytes depending on track name length and split count. The trailing leaderboard record is the same shape as a single per-car entry inside a `0x36` broadcast (see the `0x36` row above). Confirmed against `accd/notebook-a/decomp/full/1400328f0.c`. |
| `0x58` | 88 | `u8 = 0x58` + `u16 carId` + `u8 swap_request_code` (5 bytes) | **Driver swap broadcast notification** — emitted by the server immediately after a successful `ACP_EXECUTE_DRIVER_SWAP` (client `0x48`) **only if** a server-config flag is set (the same flag also gates whether the swap result `0x49` is mirrored to other clients). Broadcast to every other connected client via `broadcast_except_one`, then also re-sent over the executing client's own TCP socket as a self-confirmation. |
| `0x59` | 89 | `u8 0x59` + `u16 car_id` + `u8 target_driver_index` (4 bytes total) | **Driver handover request acknowledgement.** Server sends this to the driver who issued an in-game `&swap <driver_name>` chat command, as a confirmation that the request was received and is now pending. AC2 client uses it to display the "handover pending" UI on the source driver's screen until the matching `0x48` `ACP_EXECUTE_DRIVER_SWAP` arrives (when the target driver actually takes the wheel). The `target_driver_index` is the (driver_index − 1) slot of the entrylist driver who will take over. Single-recipient send (over the source connection's TCP socket only); never broadcast. Emitter: `accd/chat.c:559-577` (mirrors exe `FUN_140027990`). |
| `0x5b` | 91 | `u8 = 0x5b` (1 byte total, just the id) | **Request ctrl info** — server-to-client probe sent when an admin runs the `/controller <carNum>` chat command. The server sends a single byte to the targeted client, which then replies with a client→server `0x5b` (`ACP_CTRL_INFO`, see §5.6.1) carrying the actual controller information. Either side of the exchange uses the same id byte; direction is the disambiguator. The server logs `"Requested controller info for car #%d"` after sending. |
| `0x5d` | 93 | (not currently emitted) | **`/connections` admin response** — Kunos uses an `0x5d` per-row layout, but the accd reimplementation answers `/connections` with a series of plain `0x2b` chat broadcasts (`accd/chat.c:885-899`) instead of `0x5d`. Spec retained for future Kunos-parity work. |
| `0xbe` | 190 | (variable-length body built by a state-snapshot helper) | **Localhost telemetry UDP** — emitted from the main server tick via the UDP send helper to `127.0.0.1:<stats_udp_port>` only (NOT a LAN announce / presence ping). Used by the bundled stats / HUD-overlay tools to read live server state without going through the lobby; if no listener is running it's a silent no-op. See `accd/tick.c:616-655`. |
| `0xc0` | 192 | `u8 0xc0` + `str_a(server_name)` + `u8 client_count` + `u8 has_password` + `u16 tcp_port` + `u32 echo_nonce` + `u8 session_type` | **LAN discovery response** — the server's reply to a client `0x48` probe on UDP 8999. The probe format is `u8(0xbf) + u8(0x48) + u32(nonce)` (6 bytes). The `echo_nonce` field must match the probe's nonce. The `session_type` is `0xfa` when the server is in the waiting-for-drivers state, otherwise the active session type (P=0, Q=4, R=10). The `has_password` flag is 1 if `settings.json` has a non-empty `password` field (note: the password is NOT enforced for LAN/direct connections, only for lobby-registered servers). |

A comprehensive sweep plus deep TCP-dispatcher case decoding plus a server-tick-tail walk plus a final inline-write audit has found **31 distinct server → client message IDs**. This list is now considered complete: the audit cross-checked every literal `*(u8*)(...) = 0xNN;` write across all decompiled functions and confirmed that no other byte values appear as msg ids (the few additional bytes found, like `0xc9`, `0xff`, and various offsets, are either lobby internal magic bytes, sentinels, or struct field values rather than transport msg ids).

#### 5.6.4d Post-handshake welcome sequence

When a client successfully handshakes, the server emits **multiple messages in sequence** before the client is considered fully joined. Confirmed by probing a real Kunos accServer.exe 1.10.2:

```
To the joining client (in order):
  1. 0x0b  -- handshake accept (UDP port, conn_id, ~1000-2000
              byte welcome trailer with session/car/config state)
  2. 0x28  -- SRV_LARGE_STATE_RESPONSE (56 bytes, session
              timing + assist rule snapshot; f32 values prefixed
              by u8(1))
  3. 0x36  -- SRV_LEADERBOARD_BCAST (~120-210 bytes, initial
              leaderboard with per-car entries)
  4. 0x37  -- SRV_WEATHER_STATUS (69 bytes, current weather)

To every OTHER currently connected client:
  5. 0x2e  -- new-client-joined notify (u16 carId + u64
              timestamp)
  6. 0x4f  -- paired driver stint relay (u16 carId + u8=1 +
              u64 timestamp)
```

Note: the protobuf messages `0x03`-`0x07` are for the **ServerMonitor broadcasting protocol** (separate UDP port), not the game client sim protocol. The game client welcome uses sim-protocol messages `0x28`/`0x36`/`0x37` instead. The `0x4e` rating summary is sent at disconnect and periodically on standings changes, not during the welcome sequence.

#### 5.6.4b Relay / broadcast architecture (two-tier)

The server has **two distinct broadcast mechanisms** for forwarding client-originated events to other connected clients:

**Tier 1 — direct relay** (used by message ids `0x2a`, `0x2e`, `0x2f`, `0x32` and similar).

The server receives a message from client A, reads the fields it needs to validate or log, and then **broadcasts the same payload byte-for-byte** to every other connected client. The inbound and outbound bodies are identical — the server doesn't re-serialize.

A reimplementation can handle tier-1 messages with a simple "receive, validate, forward" loop without needing to understand the body contents beyond minimal validation.

**Tier 2 — queued-lambda broadcast with transformation** (used by message ids `0x19` lap report, `0x20` and `0x21` sector splits, and a handful of other rate-heavy update messages).

The server receives the message, reads the client's reported fields, updates its own state (ratings, lap counts, etc.), and then **builds a per-recipient message with a different message id** and broadcasts that. The outbound message is **not** the same as the inbound one — the server transforms the fields, computes derived values, and chooses a different id for the server → client direction.

Concrete example: a client sends `0x19` (cup position, track position, lap time, quality). The server validates and records the lap, then broadcasts a new `0x1b` message to every other client containing the same four fields but with a normalized quality byte and the server's authoritative timestamp.

Tier-2 broadcasts allow per-client customization (e.g. a client can be told about another client's lap time with its own relative-to-my-best delta baked in), rate-limiting, and different confidentiality levels per recipient.

**Neither tier is used for**:
- The handshake response (always direct, single-recipient).
- The per-tick `ACP_CAR_UPDATE` stream — the server absorbs these updates into its CarEntity state and pushes state via the tier-2 broadcast mechanism on its own schedule.
- Server-originated events (disconnect notifications, session phase changes, weather updates).

A reimplementation that supports only tier-1 direct relay will pass basic client-to-client chat / location updates but will fail on lap-time reporting and sector splits; tier-2 requires slightly more plumbing (a small queue and a per-message transformation function) but is still straightforward in C.

#### 5.6.4 Handshake (message id `0x09`)

Called when a new TCP client first connects. The body field order is:

```
u16 client_version
string_A password
... (additional fields, partially decoded: at least 4 bytes, 2 uint16s, plus
     a full embedded CarInfo sub-structure with 32 fields and an embedded
     DriverInfo with name / category / Steam id)
```

- **`client_version`** must exactly equal `0x100` (256) for ACC 1.10.2. Any other value causes rejection with the log string `"rejecting new connection with wrong client version %d (server runs %d)"`. **This version byte changes between ACC releases and is the primary build-gating mechanism.**
- **`password`** uses **Format A** string encoding (§5.3.1). It is compared as an exact std::wstring against the server's `settings.password` field. An empty server password matches only an empty client password.
- **The rest of the handshake body carries the client's own `DriverInfo` and `CarInfo`**, including first name, last name, short name, Steam ID, chosen car model, livery, team name, and a handful of flag bytes. The server parses this into an internal `CarEntry` / `DriverEntry` pair and uses it to populate the entry list.

Rejection reasons. Each one writes a 1-byte enum value into the `0x0c` reply (and triggers a log message); values match the exe's `FUN_14002db30` call sites exactly so the ACC client renders the right error dialog:

| Code | Symbolic name | Triggered by |
|---|---|---|
| 4 | `REJECT_KICKED` | Steam ID on the kick list (persists until race weekend restart) |
| 5 | `REJECT_BANNED` | Steam ID on the ban list (persists until server restart) |
| 6 | `REJECT_PASSWORD` | password mismatch against `settings.password` |
| 7 | `REJECT_VERSION_LO` | `client_version <= 0xff` |
| 8 | `REJECT_VERSION_HI` | `client_version > 0xff` (and != `0x100`) |
| 9 | `REJECT_FULL` | `connection count >= maxConnections` (sub-code 0 = driver slots full, 1 = spectator slots full) |
| 10 | `REJECT_CP_RATING` | safety / race-craft / track-medals rating below the server's configured threshold |
| 11 | `REJECT_BAD_CAR` | `entrylist.json` forces a specific car model for this Steam ID and the client tried to join with a different one (or model not allowed by car-group filter) |
| 12 | `REJECT_BAD_SESSION` | mid-race join with `unsafeRejoin=false`, late-qualy after results broadcast, locked preparation phase, or other "session not joinable now" condition |

A reimplementation that wants to maintain parity should implement all of these checks. Some depend on state computed by earlier checks (e.g. the full check happens after the password check so you can't probe for "is the server full" without knowing the password). Code `12` (`REJECT_BAD_SESSION`) was an earlier known-unknown — the reconnect-cascade gate documented in §5.6.4d is its source.

#### 5.6.4d Reconnect / mid-race join cascade

A returning Steam ID hits a three-stage gate before the handshake reaches the standard car-slot allocation path:

1. **Live-conn quick-reconnect** — if a still-connected `Conn` already has the same Steam ID, the server tears down the old connection (drops it via `FUN_140041900`) and proceeds to allocate a fresh slot for the new arrival. This handles "the player force-quit and reopened the client" without needing manual admin intervention.
2. **Saved-driver reclaim** — if no live connection has the Steam ID but a persistent driver record exists (the server keeps a vector of past drivers at `param_1[0x140f7..0x140f8]`, stride 0x30, indexed by Steam ID + driver name), the server restores the original `carId` and `carModel` so the player rejoins the same slot they had before disconnecting. This preserves race state across short disconnects.
3. **Reject gate** — if neither (1) nor (2) applies, the server may still REJECT the connection with code `12` (`REJECT_BAD_SESSION`) when:
   - The session is past `PHASE_COMPLETED` (results have been broadcast and the leaderboard is frozen — "late qualy")
   - The session is in a locked preparation phase
   - `unsafeRejoin=false` and the session type is RACE
   - The connection is from a CP-server entrant whose ratings dropped below threshold during their absence

If the gate passes, the handshake proceeds to standard slot allocation as for a first-time joiner.

---

### 5.7 Session phase state machine (internal 7-level model)

The `SessionPhase` enum exposed by `SDK/BroadcastingEnums.cs` (§4) is an
external broadcasting API mapping.  **Internally the dedicated server
uses a simpler 7-level phase numbering driven entirely by a pre-computed
time schedule.**  The internal model is what ends up on the wire in the
sim protocol, so a reimplementation needs to follow it to keep the
game client's session display in sync.

#### 5.7.1 Phase numbers

Phase is a `u8` returned by a pure function of the server clock
(`serverNow`, double milliseconds) and 6 scheduled timestamps stored
in the session-manager struct:

| Phase | Log string              | Meaning                                     |
| ----- | ----------------------- | ------------------------------------------- |
| 1     | `<waiting for drivers>` | No driver connected; slots unset            |
| 2     | (not logged)            | Intermediate (e.g. pre-formation for races) |
| 3     | `<pre session>`         | Countdown intro after driver-detected       |
| 4     | `<session>`             | Active session                              |
| 5     | `<session overtime>`    | Past scheduled end, grace period            |
| 6     | `<session completed>`   | Aftercare / results pending                 |
| 7     | (not logged)            | Sentinel; triggers session-advance          |

Non-race sessions never visit phase 2.  A race uses 2 for pre-formation
and 3 for the formation lap (gated by `formationLapType`).

#### 5.7.2 Phase computation

```c
computeCurrentPhase(SessionManager *sm, double serverNow):
    if (!sm->ts0_valid)                   return 1;  /* waiting        */
    if (serverNow <  sm->ts0_preStart)    return 1;
    if (!sm->ts1_valid)                   return 2;
    if (serverNow <  sm->ts1_phase2Bnd)   return 2;
    if (sm->flag_override_stop_at_2)      return 2;
    if (!sm->ts2_valid)                   return 3;  /* pre session    */
    if (serverNow <  sm->ts2_activeStart) return 3;
    if (!sm->ts3_valid)                   return 4;  /* session        */
    if (serverNow <  sm->ts3_activeEnd)   return 4;
    if (!sm->ts4_valid)                   return 5;  /* overtime       */
    if (serverNow <  sm->ts4_overtimeEnd) return 5;
    if (sm->flag_override_stop_at_5)      return 5;
    if (!sm->ts5_valid)                   return 6;  /* completed      */
    if (serverNow <  sm->ts5_aftercare)   return 6;
    return 7;                                         /* advance        */
```

Phase transitions are therefore **purely time-driven**.  Nothing except
the clock advances the phase — no driver-ready event, no grid-loaded
acknowledgement, no client handshake is needed.

#### 5.7.3 Schedule population

When at least one driver is detected during phase 1, the session manager
populates all 6 schedule slots in one shot:

```
ts0_preStart     = serverNow - 1.0                               /* backdate */
ts1_phase2Bnd    = ts0 + preSessionDurationS * 1000
ts2_activeStart  = ts1                                           /* same for non-race */
ts3_activeEnd    = ts2 + sessionDurationS * 1000
ts4_overtimeEnd  = ts3 + overtimeDurationS * 1000
ts5_aftercare    = ts4 + postSessionDurationS * 1000
```

**Observed defaults** from a real Kunos Practice (15 min) + Qualifying
(10 min) capture:

- `preSessionDuration`: ~3000 ms — **hardcoded** for non-race sessions,
  independent of `preRaceWaitingTimeSeconds`
- `sessionDuration`: `sessionDurationMinutes * 60000` ms (from config)
- `overtimeDuration`: 120000 ms (2 min, hardcoded default corresponding
  to `sessionOverTimeSeconds` in config)
- `aftercare`: effectively 0 ms when the sole driver has no active laps;
  configurable via `postQualySeconds` / `postRaceSeconds`

For race sessions, `preRaceWaitingTimeSeconds` from config is used for
the pre-formation wait, with a minimum clamp of 80 s for public
multiplayer (logged as
`preRaceWaitingTimeSeconds (%d) has been set to 80s`).

#### 5.7.4 Phase entry broadcasts

The session tick recomputes the phase every iteration.  When it differs
from the previously observed phase, the tick emits up to four
broadcasts inside a single "phase transition" code block, each gated
differently:

1. **`0x28` SESSION_HEADER** — emitted **unconditionally** to every
   connection, carrying the full 6-slot schedule plus session metadata
   (session index, hour of day, time multiplier, duration, session
   type, grip).  Standard form is 56 bytes; a shorter 32-byte form
   is used as a one-shot between the old session's last 0x28 and
   the new session's `startSession()` call, with all 6 time slots
   absent/zero and the tail carrying next-session metadata.

2. **`0x36` LEADERBOARD_BCAST** — emitted **only when the entry list
   changed** compared to a snapshot from the previous broadcast.
   Fires at welcome and at session-advance; does **not** fire on
   pre_session → session or other within-session phase changes.
   Entry-list changes (car joining / leaving) also trigger a `0x36`.

3. **`0x37` WEATHER_37** — emitted when `serverNow - last_weather_ms >
   5000`.  It is a **5-second heartbeat**, not a phase-change broadcast,
   but it often coincides with phase transitions because both run in
   the same tick handler.

4. **`0x4e` RATING_SUMMARY** — emitted when
   `serverNow - last_rating_ms > 81000` **AND** a rating-dirty flag
   is set.  In a single-client P→Q capture it fires only once (at
   welcome).  It is not a per-phase-change broadcast.

At the phase 6 → 7 transition (entry into the terminal sentinel), the
server additionally emits:

5. **`0x3e` SESSION_RESULTS** — unconditional, to every connection.
   Logged as `Send session results to %d clients (%d byte)`.  Body
   is a 1-byte result count plus per-car records.

6. **`0x3f` GRID_POSITIONS** (race-only) — emitted **only** when the
   next session is a race, i.e. at the end of Qualifying preceding a
   Race.  There is no separate PRE_RACE phase in the internal model;
   grid positions are computed at the end of Quali and sent via
   `0x3f` in the 6 → 7 transition block.

Every phase 6 → 7 transition also calls the session-advance path.

#### 5.7.5 Session advance

Session advance:

1. Bumps the session counter modulo `session_count`.
2. Reads the new session config.
3. If the new counter wrapped to 0 (end-of-weekend): builds a local
   `0x3e` results frame, then calls the weekend-reset path which
   emits `0x4b` welcome-redelivery to every connected client and
   logs `Event changed` + `Resetting race weekend`.
4. If the counter did **not** wrap: fast path — no 0x4b, no weekend
   reset.  The next tick picks up the new counter, triggers the
   "Session changed: %s -> %s %d" log, and the phase-1 handler calls
   `startSession()` again once a driver is present.

Between the old session's phase-7 exit and the new session's phase-1→3
entry, there is a brief window (tens of milliseconds to seconds) in
which the new session's schedule has `ts0_valid=0`, so the phase
function returns 1 and the standard phase-1 handler runs.  This is
what the log line `Detected sessionPhase <session completed> -> <waiting
for drivers>` documents.  A fresh zeroed `0x28` is emitted during this
window.

#### 5.7.6 Observed transition timings

From a real Kunos server session (1 client, Practice → Qualifying):

```
server t   event                                    phase transition
-------    ---------------------------------------  -----------------
   0       Starting server                          -
   0       Event changed, Resetting race weekend    -
   0       Reset time to first session 0 -> 50400   weekend_time_s = 50400
 116 730   1 Driver(s) detected, starting session   1 → 3  (Practice)
 119 836   phase 3 → 4 (session) elapsed 3106 ms    3 → 4  (Practice)
 ... 15 min of Practice ...
1 019 840  phase 4 → 5 (overtime)                    4 → 5
1 022 973  phase 5 → 6 (session completed)           5 → 6
1 022 989  Session completed: Practice/<completed>   emits 0x3e
1 023 004  Session aftercare over, advancing         session_advance()
1 023 004  Session changed: Practice -> Qualifying 0 counter bumped
1 023 004  phase 6 → 1 (new session, unset)          1 (Qualifying)
1 023 020  1 Driver(s) detected, starting session    1 → 3 (Qualifying)
1 026 030  phase 3 → 4 (session) elapsed 3010 ms     3 → 4 (Qualifying)
```

Key durations observed:

- **pre_session**: ~3000 ms (hardcoded for non-race)
- **session**: as configured by `sessionDurationMinutes`
- **overtime**: ~3133 ms in this capture — cleared early because the
  sole driver had no valid lap; nominal 120000 ms
- **aftercare**: ~16 ms in this capture; configurable via
  `postQualySeconds` / `postRaceSeconds`
- **session counter bump and new-session init**: ~16 ms

---

### 5.8 Penalty system

The penalty subsystem spans both directions of the sim protocol and an internal server-side state machine. This section documents every piece end to end.

#### 5.8.1 Architecture overview

```
                  +--------------------------+
                  |  AC2 client violation    |
                  |  detector (cutting,      |
                  |  pit-speeding, …)        |
                  +-----------+--------------+
                              |
                  C->S 0x41 (DSQ category)
                              |
                              v
              +---------------+---------------------+
              |   accServer FUN_140125f50           |
              |   (per-car PenaltySheet, kind 1..6) |
              |   counter += value; @0x100 escalate |
              |   ladder: DT -> SG10 -> SG20 ->     |
              |   SG30 -> DQ                        |
              +---------------+---------------------+
                              |
              +-------+-------+-------+----------------+
              |       |       |       |                |
        0x36 lb  0x2b chat  results.json  per-car HUD
        wire     "5s pen.   schema       widgets
        emit     for #N"    (V.1.8.11)   (orange-1
                                          mandatory-pit
                                          badge)

         |
         |  C->S 0x42 (penalty cleared on the client side)
         |
         v
   accServer FUN_140126b50 -> mark sheet entry served,
   append to inner Penalty record vector

         |
         |  Race end (FUN_14012b380 session-over branch)
         v
   accServer FUN_140127440 -> convert unserved DT/SG to
   PostRaceTime (DT->30 s, SG10->40 s, SG20->50 s,
   SG30->60 s) per handbook V.1.8.11
```

#### 5.8.2 Penalty kinds (server-side, 1..7)

The accServer's `FUN_140125f50` indexes a per-car PenaltySheet by `exe_kind` 1..6, with kind 7 added by AC2's wire emit:

| exe_kind | name | severity | trigger |
|---|---|---|---|
| 1 | DriveThrough | 1 | lap-bound DT (clear within 3 laps) |
| 2 | StopAndGo_10 | 2 | 10-second stationary penalty in pit-box |
| 3 | StopAndGo_20 | 3 | 20-second stationary penalty |
| 4 | StopAndGo_30 | 4 | 30-second stationary penalty |
| 5 | PostRaceTime | 5 | non-serveable; added to total at race end |
| 6 | Disqualified | 6 | terminal; race result voided |
| 7 | RemoveBestLaptime | — | clears the car's session best time (qualy / hot-lap mode) |

**Severity ladder**: DT escalates to SG10 if not served by lap N+3, SG10→SG20→SG30→DQ on each subsequent escalation. The escalation step is `(force + 2) × 2` (so without `force`: DT→SG30, with `force=1`: DT→DQ via the SG30 step).

The PostRaceTime kind has its own dedicated PenaltySheet at `Server+0x140f3` `+0x48..+0x50` (separate from the main DT/SG/DQ sheet at `+0x30..+0x38`). When TP `counter` reaches `0x100` it materialises a PostRaceTime Penalty record, then re-enters `FUN_140125f50` with `force=1, exe_kind=6` to also push a DQ — so accumulated TP can escalate to DQ.

#### 5.8.3 Per-car PenaltySheet entry layout

Stored in the timing module's master vector at `Server+0xa0848`. Stride 0x90 per entry:

```
+0x28  u32   carId               (key)
+0x58  u8    category            (8 = race-control admin, 6 = stint, 0xc/0xd = stint-limit, …)
+0x59  u8    severity            (1=DT, 2=SG10, 3=SG20, 4=SG30, 5=TP, 6=DQ)
+0x5c  u32   reason              (one of REASON_*)
+0x60  u64   issued_ts_ms
+0x68  u64   served_ts_ms        (0 = not served yet)
+0x70  i32   counter             (accumulates; escalates at 0x100)
+0x78  ptr   inner_penalty_begin (vector of ksRacing::Penalty, 0x48 B per record)
+0x80  ptr   inner_penalty_end
+0x88  ptr   inner_penalty_cap
```

Each inner `ksRacing::Penalty` record is 0x48 B and carries a copy of `(category, severity, reason, issued_ts, value)` for emission via the `0x36` per-car penalty queue.

#### 5.8.4 DSQ category enum (C->S 0x41 first byte)

Authoritative integer values, recovered from AC2 client's `FUN_1434f2fb0` (the cat-byte-to-display-string translator). Source path on the client is `FUN_140e59bf0` -> `FUN_14352db40` (the 0x41 wire emit, the only writer in the binary):

| cat | AC2 internal name | Trigger function |
|---|---|---|
| 0 | `Cutting` | `FUN_140e19000` (track-cut callback from `updateCarLocation`) |
| 1 | `Collision` | (not yet pinned to a single trigger; observed on heavy hits) |
| 2 | `IllegalOvertake` | (not pinned) |
| 3 | `PitSpeeding` | `FUN_1410dba70:339` (`PitSpeedingDetector` log at line 320) |
| 4 | `PitEntry` | (issued by the wrong-direction-on-pit-entry detector) |
| 5 | `PitExit` | (issued by the pit-exit-line crossing detector) |
| 6 | `IgnoredMandatoryPit` | end-of-race mandatory-pit miss (`FUN_1410c74b0:1509`) |
| 7 | `UnsafeRejoin` | (not pinned) |
| 8 | `Trolling` | `FUN_14101a5e0:72` (reads existing penalty record; emits when reason byte = 14) |
| 9 | `ReverseInPitlane` | (not pinned) |
| 10 | `WrongWay` | `FUN_1410202b0:145,150` (pit-entry check, both branches) and `FUN_1410c74b0:347,353` (over-time-in-pit check) |
| 11 | (alias of 6 — `IgnoredMandatoryPit`) | (deduplication of cat 6) |
| 12 | `ExceededDriverStintLimit` | `FUN_1410c74b0:1407` (negative-delta wrong-way) |
| 13 | `DriverRanNoStint` | (not pinned) |
| 0x10 / 0x11 | (out-of-enum) | `FUN_140e5ab60:131` (green-flag false-start detector) — server's default branch routes to `RACE_CONTROL` |

> **Important: the accd reimplementation's category-to-reason mapping at `accd/handlers.c:944-963` (`client_category_to_reason`) is wrong for 13 of the 14 cases.** Only cat=0 is correctly mapped. The current handler routes `Collision` to `PIT_SPEEDING`, `PitSpeeding` to `IGNORED_DRIVER_STINT`, etc., so the server's PenaltyQueue records the wrong reason and the wire emit shows wrong penalty descriptions on the leaderboard / HUD. This is a known parity gap; will be fixed when the reason-enum is extended (cat=1 `Collision` and cat=2 `IllegalOvertake` have no current `enum penalty_reason` value).

#### 5.8.5 Penalty reason enum (server internal)

The server-side `enum penalty_reason` (in `accd/state.h`) maps each violation to a category that combines with `penalty_kind` to produce a 0..35 ServerMonitorPenaltyShortcut wire value (see §5.8.7).

```
REASON_NONE                         0
REASON_CUTTING                      1
REASON_PIT_SPEEDING                 2
REASON_IGNORED_MANDATORY_PIT        3
REASON_RACE_CONTROL                 4
REASON_PIT_ENTRY                    5
REASON_PIT_EXIT                     6
REASON_WRONG_WAY                    7
REASON_LIGHTS_OFF                   8
REASON_IGNORED_DRIVER_STINT         9
REASON_EXCEEDED_DRIVER_STINT_LIMIT 10
REASON_DRIVER_RAN_NO_STINT         11
REASON_DAMAGED_CAR                 12
REASON_SPEEDING_ON_START           13
REASON_WRONG_POSITION_ON_START     14
```

Two AC2 categories (cat=1 `Collision`, cat=2 `IllegalOvertake`) have no current `enum penalty_reason` mapping; the spec needs `REASON_COLLISION` and `REASON_ILLEGAL_OVERTAKE` to round-trip them.

#### 5.8.6 Admin chat penalty commands

All admin penalty commands are dispatched by exe `FUN_14001dae0` (called from `FUN_140021680`). All pass `category=0x08` (race-control), `reason=REASON_RACE_CONTROL`, into `FUN_140125f50`. accd implementation: `accd/chat.c` -> `chat_do_penalty()` -> `penalty_enqueue()`.

| Command | exe_kind | value | force | collision | accd PEN_* | broadcast string |
|---|---|---|---|---|---|---|
| `/dq <n>` | 6 (DQ) | 3 | 1 | 0 | PEN_DQ | `"Car #%d was disqualified by Race Control"` |
| `/dt <n>` | 1 (DT) | 3 | 0 | 0 | PEN_DT | `"Drivethrough penalty for car #%d"` |
| `/dtc <n>` | 1 | 3 | 0 | 1 | PEN_DTC | `"Drivethrough penalty for car #%d - causing a collision"` |
| `/sg10 <n>` | 2 (SG10) | 3 | 0 | 0 | PEN_SG10 | `"Stop and Go 10s penalty for car #%d"` |
| `/sg10c <n>` | 2 | 3 | 0 | 1 | PEN_SG10C | `... - causing a collision` |
| `/sg20 <n>` | 3 (SG20) | 3 | 0 | 0 | PEN_SG20 | `"Stop and Go 20s penalty for car #%d"` |
| `/sg20c <n>` | 3 | 3 | 0 | 1 | PEN_SG20C | `... - causing a collision` |
| `/sg30 <n>` | 4 (SG30) | 3 | 0 | 0 | PEN_SG30 | `"Stop and Go 30s penalty for car #%d"` |
| `/sg30c <n>` | 4 | 3 | 0 | 1 | PEN_SG30C | `... - causing a collision` |
| `/tp5 <n>` | 5 (TP) | 5 | 0 | 0 | PEN_TP5 | `"5s penalty for car #%d"` |
| `/tp5c <n>` | 5 | 5 | 0 | 1 | PEN_TP5 | `... - causing a collision` |
| `/tp15 <n>` | 5 | 0xf | 0 | 0 | PEN_TP15 | `"15s penalty for car #%d"` |
| `/tp15c <n>` | 5 | 0xf | 0 | 1 | PEN_TP15 | `... - causing a collision` |
| `/clear <n>` | (calls `FUN_140126b50`) | n/a | n/a | n/a | clears queue | `"Pending penalties for #%d cleared by Race Control"` |
| `/cleartp <n>` | 5 | 0 | 1 | 0 | clears TP only | `"Pending post race time penalties for #%d cleared by Race Control"` |
| `/clear_all` | (n/a) | n/a | n/a | n/a | every car | `"All pending penalties cleared by Race Control"` |

All chats are emitted as `0x2b` broadcasts with sender = `"Race Control"` and `chat_type = 4` (system info — see §5.8.8). The collision suffix " - causing a collision" overrides any reason-derived suffix in `penalty_format_chat`.

#### 5.8.7 ServerMonitorPenaltyShortcut wire mapping (0..35)

Used in:
- 0x36 leaderboard per-car penalty queue (`pq.count + count × i32 wire_value`)
- ServerMonitor `0x07 LEADERBOARD_UPDATE` protobuf encoding
- 0x36 active-penalty preamble (single u8 + u16 wire_value)

The 36-value enum is sparse — values 6 and 12 (`RemoveBestLaptime_*`) are AC2-side autotelemetry territory and are not emitted by the server.

| value | name | source `(PEN_*, REASON_*)` |
|---|---|---|
| 0 | `No_Penalty` | unknown / fallback |
| 1 | `DriveThrough_Cutting` | `(PEN_DT/PEN_DTC, REASON_CUTTING)` |
| 2 | `StopAndGo_10_Cutting` | `(PEN_SG10/PEN_SG10C, REASON_CUTTING)` |
| 3 | `StopAndGo_20_Cutting` | `(PEN_SG20/PEN_SG20C, REASON_CUTTING)` |
| 4 | `StopAndGo_30_Cutting` | `(PEN_SG30/PEN_SG30C, REASON_CUTTING)` |
| 5 | `Disqualified_Cutting` | `(PEN_DQ, REASON_CUTTING)` |
| 6 | `RemoveBestLaptime_Cutting` | not emitted by server |
| 7 | `DriveThrough_PitSpeeding` | `(PEN_DT/DTC, REASON_PIT_SPEEDING)` |
| 8 | `StopAndGo_10_PitSpeeding` | `(PEN_SG10/C, REASON_PIT_SPEEDING)` |
| 9 | `StopAndGo_20_PitSpeeding` | `(PEN_SG20/C, REASON_PIT_SPEEDING)` |
| 10 | `StopAndGo_30_PitSpeeding` | `(PEN_SG30/C, REASON_PIT_SPEEDING)` |
| 11 | `Disqualified_PitSpeeding` | `(PEN_DQ, REASON_PIT_SPEEDING)` |
| 12 | `RemoveBestLaptime_PitSpeeding` | not emitted by server |
| 13 | `Disqualified_IgnoredMandatoryPit` | `(PEN_DQ, REASON_IGNORED_MANDATORY_PIT)` |
| 14 | `PostRaceTime` | `(PEN_TP5/PEN_TP15, REASON_RACE_CONTROL)` — admin `/tp5` `/tp15` |
| 15 | `DriveThrough_RaceControl` | admin `/dt` `/dtc` |
| 16 | `StopAndGo_10_RaceControl` | admin `/sg10` `/sg10c` |
| 17 | `StopAndGo_20_RaceControl` | admin `/sg20` `/sg20c` |
| 18 | `StopAndGo_30_RaceControl` | admin `/sg30` `/sg30c` |
| 19 | `Disqualified_RaceControl` | admin `/dq` |
| 20 | `Disqualified_PitEntry` | `(PEN_DQ, REASON_PIT_ENTRY)` |
| 21 | `Disqualified_PitExit` | `(PEN_DQ, REASON_PIT_EXIT)` |
| 22 | `Disqualified_WrongWay` | `(PEN_DQ, REASON_WRONG_WAY)` |
| 23 | `Disqualified_LightsOff` | `(PEN_DQ, REASON_LIGHTS_OFF)` |
| 24 | `DriveThrough_IgnoredDriverStint` | `(PEN_DT/DTC, REASON_IGNORED_DRIVER_STINT)` |
| 25 | `StopAndGo_30_IgnoredDriverStint` | `(PEN_SG30/C, REASON_IGNORED_DRIVER_STINT)` |
| 26 | `Disqualified_IgnoredDriverStint` | `(PEN_DQ, REASON_IGNORED_DRIVER_STINT)` |
| 27 | `Disqualified_ExceededDriverStintLimit` | `(PEN_DQ, REASON_EXCEEDED_DRIVER_STINT_LIMIT)` |
| 28 | `Disqualified_DriverRanNoStint` | `(PEN_DQ, REASON_DRIVER_RAN_NO_STINT)` |
| 29 | `Disqualified_DamagedCar` | `(PEN_DQ, REASON_DAMAGED_CAR)` |
| 30 | `DriveThrough_SpeedingOnStart` | `(PEN_DT/DTC, REASON_SPEEDING_ON_START)` |
| 31 | `StopAndGo_30_SpeedingOnStart` | `(PEN_SG30/C, REASON_SPEEDING_ON_START)` |
| 32 | `Disqualified_SpeedingOnStart` | `(PEN_DQ, REASON_SPEEDING_ON_START)` |
| 33 | `DriveThrough_WrongPositionOnStart` | `(PEN_DT/DTC, REASON_WRONG_POSITION_ON_START)` |
| 34 | `StopAndGo_30_WrongPositionOnStart` | `(PEN_SG30/C, REASON_WRONG_POSITION_ON_START)` |
| 35 | `Disqualified_WrongPositionOnStart` | `(PEN_DQ, REASON_WRONG_POSITION_ON_START)` |

#### 5.8.8 0x2b chat broadcast wire format

Used by every penalty announcement and by general chat / admin command replies:

```
u8     0x2b                 (SRV_CHAT_OR_STATE)
str_a  sender               (Format-A wstring; "Race Control" for penalties)
str_a  body                 (the human-readable message, e.g. "5s penalty for car #3")
i32    0                    (reserved / always zero on the wire)
u8     chat_type            (4 = system info, 5 = warning)
```

The accd codebase emits exactly two `chat_type` values:
- **4 (system info)** — penalty notifications, BoP changes, `/wt` weather dump, `/start` advance, all admin-driven announcements
- **5 (warning)** — kick / ban announcements ("You have been kicked from the server"), also used for server-emergency messages

Other values (0..3, 6..N) are defined by the protocol but unused by the reimplementation.

#### 5.8.9 Race-end conversion (FUN_140127440)

At session end (race only), the exe walks every car's main PenaltySheet and converts unserved DT/SG entries to PostRaceTime per handbook V.1.8.11:

| from severity | log string | seconds added |
|---|---|---|
| 1 (DT) | `"Converted pending DT penalty to 30s time penalty"` | 30 |
| 2 (SG10) | `"Converted pending 10s S&G penalty to 40s time penalty"` | 40 |
| 3 (SG20) | `"Converted pending 20s S&G penalty to 50s time penalty"` | 50 |
| 4 (SG30) | `"Converted pending 30s S&G penalty to 60s time penalty"` | 60 |

Triggered only from `FUN_14012b380` (the lap-close session-over branch and abnormal-end branch). Practice / qualifying do NOT invoke this conversion — those sessions don't have lap-bound penalty serving.

> **accd parity gap**: `accd/penalty.c` `penalty_total_ms` returns the right total at session end (sum of TP + converted DT/SG) so `results.json` race times are correct. But the accd PenaltyQueue is **not rewritten** to materialise the converted TP entries — the queue still shows the original DT/SG kinds at session end. ServerMonitor wire emit (and any post-race per-car penalty list rendered in the HUD) will show a different list than Kunos's exe. Not yet fixed.

#### 5.8.10 0x36 leaderboard penalty fields

Each per-car record in a `0x36` broadcast carries:

```
... earlier fields (car_id, race_number, car_model, cup_category, …) ...
u8   active_present            (1 if there's a non-served DT/SG/DQ; 0 otherwise)
if active_present:
    u16 active_wire_value      (one of the 0..35 ServerMonitorPenaltyShortcut codes)
    f32 active_laps_remaining  (countdown for lap-bound DT/SG; 0 if not applicable)
[ … cvar8-gated u8 missingMandatoryPitstop, see 0x36 row in 5.6.4a … ]
u8   pq_count                  (total queue length)
i32 [pq_count]                 (each entry's wire_value as a signed 32-bit integer)
... later fields (sectors, lap history, …) ...
```

The "active" prefix carries the front-of-queue serve-able penalty (the one the orange-1 HUD widget renders); the `pq_count + i32[]` array carries the full queue for the post-race standings panel.

#### 5.8.11 Cross-references

- §5.6.1 row `0x41` — wire format for the C→S report
- §5.6.1 row `0x42` — wire format for the C→S clear notification
- §5.6.1 row `0x54` — `ACP_MANDATORY_PITSTOP_SERVED` (clears mandatory-pit-pending flag)
- §5.6.4a row `0x2b` — outer chat broadcast envelope shared with general chat
- §5.6.4a row `0x36` — leaderboard broadcast that carries the active + queue penalty fields
- §8.1 — admin chat command surface (table here is canonical)
- §12B.4 — ServerMonitor enums (the 0..35 enum referenced from there)

---

## 6. Data model

The broadcasting SDK's data model describes everything Kunos considers worth exposing to overlay tooling. The sim-side protocol must carry at least this information from server to client (because the game client populates its local broadcasting state from whatever it receives from the server).

### 6.1 Car entry (`SDK/Structs/CarInfo.cs`)

```
ushort CarIndex
byte   CarModelType         // see §7.2
string TeamName
int32  RaceNumber
byte   CupCategory          // see §7.4
byte   CurrentDriverIndex
NationalityEnum Nationality // uint16, see §7.5
DriverInfo[]    Drivers     // variable length
```

### 6.2 Driver (`SDK/Structs/DriverInfo.cs`)

```
string FirstName
string LastName
string ShortName            // 3 chars in UI
byte   Category             // DriverCategory, see §7.3
NationalityEnum Nationality // uint16
```

### 6.3 Car state, per tick (`SDK/Structs/RealtimeCarUpdate.cs` + deserializer in `BroadcastingNetworkProtocol.cs:206-245`)

```
uint16 CarIndex
uint16 DriverIndex          // driver swap changes this
byte   DriverCount
byte   Gear                 // wire value biased by +2: R=1, N=2, 1st=3, …
float  WorldPosX
float  WorldPosY            // 2D: no Z at the broadcasting layer
float  Yaw
byte   CarLocation          // NONE=0, Track=1, Pitlane=2, PitEntry=3, PitExit=4
uint16 Kmh                  // speed
uint16 Position             // 1-based, official P/Q/R position
uint16 CupPosition          // 1-based, cup-category position
uint16 TrackPosition        // 1-based, on-track position
float  SplinePosition       // 0.0 .. 1.0 along track centerline spline
uint16 Laps
int32  Delta                // ms, realtime delta to best session lap
LapInfo BestSessionLap
LapInfo LastLap
LapInfo CurrentLap
```

`WorldPosX`/`Y` being 2D confirms the broadcasting layer projects onto the map; the sim protocol will carry full 3D position + rotation.

### 6.4 Session state, per tick (`SDK/Structs/RealtimeUpdate.cs` + deserializer `BroadcastingNetworkProtocol.cs:170-204`)

```
uint16 EventIndex
uint16 SessionIndex
byte   SessionType          // see §7.6
byte   Phase                // SessionPhase enum, see §4
float  SessionTime          // ms
float  SessionEndTime       // ms
int32  FocusedCarIndex
string ActiveCameraSet      // client-side concept
string ActiveCamera         //       "
string CurrentHudPage       //       "
byte   IsReplayPlaying
  if IsReplayPlaying:
    float ReplaySessionTime
    float ReplayRemainingTime
float  TimeOfDay            // ms
byte   AmbientTemp          // °C
byte   TrackTemp            // °C (simulated from ambient + clouds + sun)
byte   Clouds               // 0..10, divide by 10.0
byte   RainLevel            // 0..10, divide by 10.0
byte   Wetness              // 0..10, divide by 10.0
LapInfo BestSessionLap
```

The camera/HUD-page fields are client-side concepts. The sim protocol probably carries different fields; these appear because the broadcasting protocol reflects the *client's own state*, not the server's.

### 6.5 Track data (`SDK/Structs/TrackData.cs` + deserializer `BroadcastingNetworkProtocol.cs:247-282`)

```
string TrackName
int32  TrackId
int32  TrackMeters          // track length
Dictionary<string, List<string>> CameraSets  // client-side
List<string> HUDPages                        // client-side
```

Wire format for the maps: `byte cameraSetCount`, then for each set `string name, byte cameraCount, cameraCount × string`. HUD pages: `byte pageCount, pageCount × string`. This framing style (byte count + iterated items) is probably reused in the sim protocol for the entry list and camera-related messages.

### 6.6 Lap (`SDK/Structs/LapInfo.cs` + deserializer `BroadcastingNetworkProtocol.cs:306-347`)

Wire format:

```
int32  LaptimeMS            // Int32.MaxValue = no time
uint16 CarIndex
uint16 DriverIndex
byte   SplitCount
SplitCount × int32 Splits   // each Int32.MaxValue = null
byte   IsInvalid
byte   IsValidForBest
byte   IsOutlap
byte   IsInlap
```

`LapType` is a derived field: `Outlap` if `isOutlap`, `Inlap` if `isInlap`, otherwise `Regular`. Splits count is typically 3 but the format is extensible.

### 6.7 Broadcasting event (`SDK/Structs/BroadcastingEvent.cs`)

```
byte   Type                 // BroadcastingCarEventType, see §7.7
string Msg
int32  TimeMs
int32  CarId
```

This is a client-originated event and the sim protocol may or may not carry it. It's useful because the `Type` enum enumerates every "interesting" race event the game knows about.

---

## 7. Catalogs

### 7.1 Track list (`HB §IX.1`)

| `track` value | Unique pit boxes | Private slots |
|---|---|---|
| `monza` | 29 | 60 |
| `zolder` | 34 | 50 |
| `brands_hatch` | 32 | 50 |
| `silverstone` | 36 | 60 |
| `paul_ricard` | 33 | 80 |
| `misano` | 30 | 50 |
| `spa` | 82 | 82 |
| `nurburgring` | 30 | 50 |
| `barcelona` | 29 | 50 |
| `hungaroring` | 27 | 50 |
| `zandvoort` | 25 | 50 |
| `kyalami` | 40 | 50 |
| `mount_panorama` | 36 | 50 |
| `suzuka` | 51 | 105 |
| `laguna_seca` | 30 | 50 |
| `imola` | 30 | 50 |
| `oulton_park` | 28 | 50 |
| `donington` | 37 | 50 |
| `snetterton` | 26 | 50 |
| `cota` | 30 | 70 |
| `indianapolis` | 30 | 60 |
| `watkins_glen` | 30 | 60 |
| `valencia` | 29 | 50 |
| `nurburgring_24h` | 50 | 110 |

Year suffixes (`_2019`, `_2020`, `_2021`) are deprecated since 1.8.0 [`CL`]. Public MP is capped at `min(uniquePitBoxes, 30)`.

### 7.2 Car model list (`HB §IX.3`)

GT3 (0-36):

| ID | Model |
|---|---|
| 0 | Porsche 991 GT3 R |
| 1 | Mercedes-AMG GT3 |
| 2 | Ferrari 488 GT3 |
| 3 | Audi R8 LMS |
| 4 | Lamborghini Huracan GT3 |
| 5 | McLaren 650S GT3 |
| 6 | Nissan GT-R Nismo GT3 2018 |
| 7 | BMW M6 GT3 |
| 8 | Bentley Continental GT3 2018 |
| 9 | Porsche 991II GT3 Cup |
| 10 | Nissan GT-R Nismo GT3 2017 |
| 11 | Bentley Continental GT3 2016 |
| 12 | Aston Martin V12 Vantage GT3 |
| 13 | Lamborghini Gallardo R-EX |
| 14 | Jaguar G3 |
| 15 | Lexus RC F GT3 |
| 16 | Lamborghini Huracan Evo (2019) |
| 17 | Honda NSX GT3 |
| 18 | Lamborghini Huracan SuperTrofeo |
| 19 | Audi R8 LMS Evo (2019) |
| 20 | AMR V8 Vantage (2019) |
| 21 | Honda NSX Evo (2019) |
| 22 | McLaren 720S GT3 (2019) |
| 23 | Porsche 911II GT3 R (2019) |
| 24 | Ferrari 488 GT3 Evo 2020 |
| 25 | Mercedes-AMG GT3 2020 |
| 26 | Ferrari 488 Challenge Evo |
| 27 | BMW M2 CS Racing |
| 28 | Porsche 911 GT3 Cup (Type 992) |
| 29 | Lamborghini Huracán Super Trofeo EVO2 |
| 30 | BMW M4 GT3 |
| 31 | Audi R8 LMS GT3 evo II |
| 32 | Ferrari 296 GT3 |
| 33 | Lamborghini Huracan Evo2 |
| 34 | Porsche 992 GT3 R |
| 35 | McLaren 720S GT3 Evo 2023 |
| 36 | Ford Mustang GT3 |

GT4 (50-61):

| ID | Model |
|---|---|
| 50 | Alpine A110 GT4 |
| 51 | AMR V8 Vantage GT4 |
| 52 | Audi R8 LMS GT4 |
| 53 | BMW M4 GT4 |
| 55 | Chevrolet Camaro GT4 |
| 56 | Ginetta G55 GT4 |
| 57 | KTM X-Bow GT4 |
| 58 | Maserati MC GT4 |
| 59 | McLaren 570S GT4 |
| 60 | Mercedes-AMG GT4 |
| 61 | Porsche 718 Cayman GT4 |

GT2 (80-86):

| ID | Model |
|---|---|
| 80 | Audi R8 LMS GT2 |
| 82 | KTM XBOW GT2 |
| 83 | Maserati MC20 GT2 |
| 84 | Mercedes AMG GT2 |
| 85 | Porsche 911 GT2 RS CS Evo |
| 86 | Porsche 935 |

Gaps (37-49, 54, 62-79, 81, 87+) are not assigned as of 1.10.2.

### 7.3 Driver category (`HB §IX.4`, `SDK/BroadcastingEnums.cs` `DriverCategory`)

| Value | Category |
|---|---|
| 0 | Bronze |
| 1 | Silver |
| 2 | Gold |
| 3 | Platinum |
| 255 | Error (SDK-side sentinel) |

### 7.4 Cup category (`HB §IX.5`, `BroadcastingNetworkProtocol.cs:146`)

| Value | Category |
|---|---|
| 0 | Overall / Pro |
| 1 | ProAm |
| 2 | Am |
| 3 | Silver |
| 4 | National |

### 7.5 Nationality (`SDK/BroadcastingEnums.cs` `NationalityEnum`)

84 values, `uint16`. `0` = Any. Full list: Italy(1), Germany(2), France(3), Spain(4), GreatBritain(5), Hungary(6), Belgium(7), Switzerland(8), Austria(9), Russia(10), Thailand(11), Netherlands(12), Poland(13), Argentina(14), Monaco(15), Ireland(16), Brazil(17), SouthAfrica(18), PuertoRico(19), Slovakia(20), Oman(21), Greece(22), SaudiArabia(23), Norway(24), Turkey(25), SouthKorea(26), Lebanon(27), Armenia(28), Mexico(29), Sweden(30), Finland(31), Denmark(32), Croatia(33), Canada(34), China(35), Portugal(36), Singapore(37), Indonesia(38), USA(39), NewZealand(40), Australia(41), SanMarino(42), UAE(43), Luxembourg(44), Kuwait(45), HongKong(46), Colombia(47), Japan(48), Andorra(49), Azerbaijan(50), Bulgaria(51), Cuba(52), CzechRepublic(53), Estonia(54), Georgia(55), India(56), Israel(57), Jamaica(58), Latvia(59), Lithuania(60), Macau(61), Malaysia(62), Nepal(63), NewCaledonia(64), Nigeria(65), NorthernIreland(66), PapuaNewGuinea(67), Philippines(68), Qatar(69), Romania(70), Scotland(71), Serbia(72), Slovenia(73), Taiwan(74), Ukraine(75), Venezuela(76), Wales(77), Iran(78), Bahrain(79), Zimbabwe(80), ChineseTaipei(81), Chile(82), Uruguay(83), Madagascar(84).

### 7.6 Session type (`HB §IX.6`, `SDK/BroadcastingEnums.cs` `RaceSessionType`)

`HB` documents three types as JSON strings (`"P"`, `"Q"`, `"R"`). The wire enum from SDK has more:

| Value | Type | Source |
|---|---|---|
| 0 | Practice | both |
| 4 | Qualifying | both |
| 9 | Superpole | SDK only |
| 10 | Race | both |
| 11 | Hotlap | SDK only |
| 12 | Hotstint | SDK only |
| 13 | HotlapSuperpole | SDK only |
| 14 | Replay | SDK only |

The four SDK-only values are private-MP / hotlap-server / replay features. Phase-1 reimplementation only needs `Practice`, `Qualifying`, `Race`.

### 7.7 Broadcasting event type (`SDK/BroadcastingEnums.cs`)

| Value | Type |
|---|---|
| 0 | None |
| 1 | GreenFlag |
| 2 | SessionOver |
| 3 | PenaltyCommMsg |
| 4 | Accident |
| 5 | LapCompleted |
| 6 | BestSessionLap |
| 7 | BestPersonalLap |

### 7.8 Lap type (`SDK/BroadcastingEnums.cs`)

| Value | Type |
|---|---|
| 0 | ERROR |
| 1 | Outlap |
| 2 | Regular |
| 3 | Inlap |

Note: the wire representation in `BroadcastingNetworkProtocol.cs:306-347` uses two separate booleans (`isOutlap`, `isInlap`) not this enum; the enum is a client-side derived value.

### 7.9 Car location (`SDK/BroadcastingEnums.cs`)

| Value | Location |
|---|---|
| 0 | NONE |
| 1 | Track |
| 2 | Pitlane |
| 3 | PitEntry |
| 4 | PitExit |

Delivered by `ACP_CAR_LOCATION_UPDATE` (TCP ID 50) in the sim protocol.

---

## 8. Admin chat commands

From `HB §V`. Admin elevation: chat `/admin <adminPassword>`. Elevated commands:

| Command | Args | Effect |
|---|---|---|
| `/next` | — | Skip current session |
| `/restart` | — | Restart current session (not during preparation) |
| `/kick` | carNum | Kick until race weekend restart |
| `/ban` | carNum | Ban until server restart |
| `/dq` | carNum | Disqualify, teleport to pits, lock controls |
| `/clear` | carNum | Remove pending penalties + DSQ for one car |
| `/clear_all` | — | Remove all penalties + DSQ |
| `/tp5` / `/tp5c` | carNum | 5s time penalty ("c" variant = "for causing a collision") |
| `/tp15` / `/tp15c` | carNum | 15s time penalty |
| `/dt` / `/dtc` | carNum | Drive-through; 3 laps to serve, else DSQ; mid-race finish → 80s time penalty |
| `/sg10` | carNum | Stop&go 10s |
| `/sg20` | carNum | Stop&go 20s |
| `/sg30` | carNum | Stop&go 30s |
| `/ballast` | carNum kg | Set ballast 0..100 |
| `/restrictor` | carNum pct | Set restrictor 0..20 (%) |
| `/manual entrylist` | — | Dump current connected drivers to an entry list JSON |
| `/debug formation` | — | Print formation lap car states |
| `/debug bandwidth` | — | Toggle bandwidth trace (TCP + UDP) |
| `/debug qos` | — | Toggle QoS trace |

Non-admin chat command (for driver swap in driver-swap teams): `&swap <driverNum>`, usable during Practice/Qualifying while in the pitlane.

Chat is a sim-protocol feature carried over TCP. Client → server messages use ID `0x2a` (`ACP_CHAT`); server → client messages (broadcasts and admin replies) use ID `0x2b` (`SRV_CHAT_OR_STATE`) — see §5.6.1 / §5.6.4a / §5.8.8 for the wire formats. Elevation is stateful on the server; `/admin <pw>` sets a flag on the client's connection. Entry-list drivers with `isServerAdmin: 1` are auto-elevated on join.

### 8.1 Additional admin chat commands not in HB

The chat command parser exposes several commands that are not documented in the public handbook. These were observed by inspecting the parser's literal-string table:

| Command | Args | Effect |
|---|---|---|
| `/admin` | password | Elevate the issuing connection to server admin (or `"Wrong password"`) |
| `/track` | trackName | Switch the current event's track. Validates `trackName` against the per-track table at exe `FUN_14012c510` (returns -1 on unknown name → "Please set a valid track"). On valid input the server snapshots the current `cfg/*.json` files into `cfg/current/*.txt`, logs `"Event change to %s"`, and broadcasts a `0x40` race-weekend reset to every client |
| `/manual entrylist` | — | Dump the current connected drivers to a new entry list JSON; replies `"Saved entry list to ..."`. **Refuses on public-listed servers** (lobby-registered = listed as joinable from the in-game browser): replies `"Entry list cannot be saved on public servers"`. The public-server gate is a defensive check to avoid accidentally writing entrylists for servers that aren't yours |
| `/manual start` | — | Replaced — replies `"This cmd was replaced by the formationLapType setting"` |
| `/go` | — | Zero out `ServerState.ts[0]` and `ServerState.ts[1]` to collapse the pre-session wait — useful when an admin wants the schedule to advance to the first session immediately without waiting for the configured pre-grid window |
| `/controllers` | — | Request ctrl info from every client; replies `"Requesting controllers for %d clients"` |
| `/controller` | carNum | Request ctrl info from one specific car (single-recipient `0x5b` send) |
| `/connections` | — | List all current connections (Kunos sends one `0x5d` per connection; the accd reimplementation answers with one `0x2b` chat per connection) |
| `/hellban` | carNum | Apply hellban; replies `"Hellban inactive"` if disabled |
| `/cleartp` | carNum | Clear pending post-race time penalties (`"Pending post race time penalties for #%d cleared by Race Control"`) |
| `/report` | — | Mark / report a connection (replies `"Car #%d reported, thank you"`). **The only non-admin `/`-command** — every other command in this table requires admin elevation via `/admin`. |
| `/latencymode` | n | Set latency mode 0..N (`"Latency mode: ..."`); validates the number |
| `/wt` | — | Dump current weather state to chat (`"Standard weather:"` or `"Snowflake weather:"` line + ambient °C, road °C, grip, wind speed, wet track) |
| `/mp` | — | Toggle the legacy / regular netcode mode (synonym for `/legacy` and `/regular`); flips the `legacy_netcode` flag at server-state `param_1+0x22` |
| `/debug conditions` | — | Toggle conditions debug logging (`"conditions stopped/started printing"`) |
| `/debug bandwidth` | — | Toggle bandwidth stats debug logging |
| `/debug qos` / `/netcode` | — | Toggle netcode-stats debug logging |
| `/legacy` / `/regular` | — | Toggle netcode mode (`"Server now uses legacy netcode"` / `"Server is now in regular mode"`) |

**Driver-initiated `&swap`** (note the `&` prefix, not `/`): when a driver in a multi-driver car types `&swap <driver_name>`, the server:
1. Validates the name matches one of the car's registered drivers
2. Emits `0x59` (driver-handover request) to every other client sharing the car
3. Emits `0x47` (driver swap state broadcast) to update everyone's swap UI
4. On execution, the standard `0x48`/`0x49`/`0x58` swap-result messages flow

This is the only `&` command and the only command available to non-admin drivers besides `/report`.

Penalty commands (`/dq`, `/dt`, `/dtc`, `/sg10`, `/sg10c`, `/sg20`, `/sg20c`, `/sg30`, `/sg30c`, `/tp5`, `/tp5c`, `/tp15`, `/tp15c`) generate human-readable broadcast strings such as `"5s penalty for car #%d"`, `"Drivethrough penalty for car #%d - causing a collision"`, etc., delivered as `0x2b` chat broadcasts to all connections.

`/ballast` and `/restrictor` additionally emit a `0x53` `MultiplayerBOPUpdate` message (see §5.6.4a) with the new ballast / restrictor values, sent to every connection.

The kick / ban path (`/kick` and `/ban`) emits a `0x2b` chat-style notification with the message `"You have been kicked from the server"` or `"You have been banned from the server"` directly to the target client immediately before force-closing the TCP socket.

A reimplementation of the chat command surface for private MP needs to handle at minimum: `/admin`, `/next`, `/restart`, `/kick`, `/ban`, `/dq`, `/clear`, the time / drive-through penalty family, `/ballast`, `/restrictor`, and the `&swap` self-service command. The `/debug *` commands are local debug toggles only and do not need to behave faithfully.

---

## 9. Result file schema

From `HB §VIII.1`. File path: `server/results/YYMMDD_HHMMSS_X.json` where `X` ∈ `{P, Q, R}`. Enabled by `settings.json` `dumpLeaderboards: 1`; the `results/` folder must exist.

Top-level structure:

```json
{
  "sessionType": "R",
  "trackName": "silverstone",
  "sessionIndex": 1,
  "sessionResult": {
    "bestlap": 117915,
    "bestSplits": [34770, 49359, 33258],
    "isWetSession": 0,
    "type": 1,
    "leaderBoardLines": [
      {
        "car": {
          "carId": 1073,
          "raceNumber": 912,
          "carModel": 0,
          "cupCategory": 0,
          "teamName": "",
          "drivers": [ { "firstName": "...", "lastName": "...", "shortName": "...", "playerId": "S76561..." } ]
        },
        "currentDriver": { ... },
        "currentDriverIndex": 0,
        "timing": {
          "lastLap": 119223,
          "lastSplits": [35286, 50178, 33759],
          "bestLap": 118404,
          "bestSplits": [35265, 49659, 33438],
          "totalTime": 719894,
          "lapCount": 6,
          "lastSplitId": 0
        },
        "missingMandatoryPitstop": 0,
        "driverTotalTimes": [0.0]
      }
    ]
  },
  "laps": [
    {
      "carId": 1073,
      "driverIndex": 0,
      "laptime": 125511,
      "isValidForBest": true,
      "splits": [40197, 51537, 33777]
    }
  ],
  "penalties": [
    {
      "carId": 1079,
      "driverIndex": 0,
      "reason": "Cutting",
      "penalty": "DriveThrough",
      "penaltyValue": 3,
      "violationInLap": 0,
      "clearedInLap": 1
    }
  ]
}
```

Times are integers in milliseconds. `carId` in the result file is **not** the same as `CarIndex` in the broadcasting protocol — `carId` appears to be an internal numeric identifier (values like 1073 in the example).

---

## 10. Weather and track simulation

`HB §IV` describes both in detail. Summary:

### 10.1 Weather model

Three parameters: `cloudLevel`, `rain`, `weatherRandomness`. Simulation starts Friday 00:00 and runs to whenever the configured session starts. Time multiplier accelerates the simulation.

- `weatherRandomness` 0 = static; 1-4 "fairly realistic"; 5-7 "sensational"
- `cloudLevel` gates rain chance; below ~60% clouds, rain is unlikely
- `rain` sets baseline rain gravity when rain falls
- Gravity toward thunderstorm was removed in recent versions
- `isFixedConditionQualification` is an experimental override for league Q sessions

Handbook provides seven example scenario presets (anything-can-happen, gradual-variation, overcast-no-rain, sunny, overcast-with-potential-rain, light-medium-rain, medium-heavy-rain). These are combinations of the three parameters; see `HB §IV.3`.

### 10.2 Track model

- Track rubber/cleanliness evolves independently of weather, runs in real time (not accelerated by `timeMultiplier`)
- Simulated support-program traffic adds rubber over the simulated Friday/Saturday
- Water dissipation rate depends on sun angle, cloud level, temperature, wind
- Starting condition depends on `dayOfWeekend` and `hourOfDay` of the first session

For phase 1 of the reimplementation: implement a stub that reports constant conditions. Real weather/track simulation is not required for basic multiplayer.

---

## 11. Lobby / backend integration

### 11.1 Transport

TLS/TCP to port 443.  The binary uses Windows Schannel (not a
bundled TLS library).  Two lobby servers are hardcoded:

| Role | IP | Hosting |
|------|----|---------|
| Primary | `131.153.158.178` | PhoenixNAP, Netherlands |
| Secondary | `144.76.81.131` | Hetzner, Germany |

Three hidden `configuration.json` keys override them:
`lobbyPrimaryIP_DO_NOT_PUBLISH`, `lobbySecondaryIP_DO_NOT_PUBLISH`,
`lobbyPort_DO_NOT_PUBLISH`.

### 11.2 Connection flow

1. **TCP connect** to lobby IP port 443, set `TCP_NODELAY`.
2. **TLS handshake** (Schannel).
3. **Send 256-byte probe**: `u16(tcp_port)` + `u8(tcp_port % 77)` + `u8(tcp_port / 21)` + 252 zero bytes.
4. **Sleep 1 second**, then check connection.
5. **Send 0x44 registration** (variable length, ~200+ bytes).
6. **Wait for 0xef** accept/reject from lobby.
7. If accepted (byte 0 = 0): enter operational state.
8. If rejected (byte 0 = 1-6): log reason, `exit(1)`.

### 11.3 State machine

| State | Name | Description |
|-------|------|-------------|
| 1 | Rejected | Backend rejected, fatal (`exit(1)`) |
| 2 | Disconnected | Waiting to retry.  Interval: 10s for first 3 attempts, 30s + random jitter thereafter. |
| 3 | Connecting | TCP+TLS handshake in progress |
| 4 | Connected | TLS up, sending 0x44 registration |
| 5 | Handshake | Registration sent, waiting for 0xef response.  Timeout triggers reconnect. |
| 6 | Operational | Sending heartbeats, handling incoming commands |

### 11.4 Kson string encoding

The kson protocol uses its own string format, distinct from
the game protocol's Format-A:

```
u16 byte_length
u8[byte_length] UTF-16LE data
```

Error guard: `"UDPPacket::writeKsonString called with byte length over 65k"`.

Two writer functions exist in the binary:
- `FUN_14004d240(buffer, wchar_t*)` -- from a wide string pointer
- `FUN_14004d490(buffer, string_object*)` -- from a C++ std::wstring

### 11.5 Client-to-lobby messages

#### 0x44 Registration (~200+ bytes, variable)

```
u8   0x44
kson_str  server_password
u32  tcp_port
u32  udp_port
kson_str  track_name
kson_str  server_name
u8   server_state
u8   has_password (bool)
u8   config_flag (+0x1c8)
u8   config_flag (+0x1cc)
u8   config_flag (+0x1d0)
u8   session_type (P=0, Q=4, R=10)
u8   config_bool (+0x228)
u8   config_bool (+0x229)
u8   config_bool (+0x231)
u8   wine_detected (0/1, via GetProcAddress("wine_get_version"))
u8   car_state_byte (from first car entry)
u8   competition_mode (0=off, 1=type1, 2=type2)
[conditional competition config bytes]
u8   config_byte (+0x310)
u8   car_count
per-car {
    u8   car_model
    u8   cup_category
    u8   driver_category
    i16  rating (scaled)
    u16  driver_nationality
    u16  car_nationality
    u8   float_cast
}
u8   timing_field
kson_str  config_string_1
kson_str  config_string_2
kson_str  config_string_3
```

The Wine detection flag is notable: the server explicitly reports
whether it runs under Wine/CrossOver.  Rejection reason 5 targets
Wine servers that cause "problems for other users".

#### 0x3a Unregistration (fixed, 15 bytes + kson_str)

```
u8   0x3a
u8   0xc9 (magic)
u32  7
u32  6
u8   0x00
kson_str  server_name
u32  connection_count
```

#### 0xd7 Config response (reply to lobby's 0xf6)

```
u8   0xd7
kson_str  server_name
kson_str  track_name
kson_str  password
```

#### 0xcb Session update (state transitions)

```
u8   0xcb
[10-byte preamble — see §11.4]
u8   session_type      (P=0, Q=4, R=10)
u8   internal_phase    (1..7 — internal 7-level model from §5.7, NOT the SDK SessionPhase enum)
u32  time_remaining_s
```

Sent from the server to the lobby whenever the active session changes (session index, phase byte, or descriptor) and on connection state transitions. The lobby uses this to update its session badge for every listed server.

> **Phase enum mismatch**: the lobby `0xcb` carries the **internal 7-level phase** value (1=`WAITING`, 2=`PRE_SESSION`, 3=`SESSION`, 4=`POST_SESSION`, 5=`PENDING`, 6=`SESSION_OVER`, 7=`COMPLETED`) — NOT the SDK `SessionPhase` enum exposed on the broadcasting protocol from §4. Reimplementations that translate `0xcb` to/from the SDK enum need an explicit mapping table.

#### 0xd0 Lap-time event (per finished lap)

```
u8   0xd0
[preamble]
u16  car_id
u32  laptime_ms
u16  lap_states           (= same `lapstates` u16 as in client `0x21`)
kson_str  driver_steam_id
```

Sent on every successfully completed lap so the lobby can keep "best lap so far" diagnostics for hotlap-style listings.

#### 0xd1 Drivers list update

```
u8   0xd1
[preamble]
u8   driver_count
per-driver {
    u16     car_id
    kson_str  steam_id
    kson_str  short_name
    kson_str  first_name
    kson_str  last_name
    u8      driver_category
    u8      cup_category
    u16     nationality
    i16     rating ×100
}
```

Sent when the connected-drivers roster changes (join, leave, swap completed).

#### 0xd2 Wrecker report

```
u8   0xd2
[preamble]
u16  carId
kson_str  reporter_steam_id
kson_str  reported_steam_id
u8   incident_kind
u32  timestamp
```

Driver-initiated report of a deliberate-collision / griefing incident, forwarded to the lobby for moderation review. Generated when a driver issues `/report` (the only non-admin `/`-command).

#### 0xd3 CP race results

```
u8   0xd3
[preamble]
kson_str  cp_event_id
kson_str  result_blob_json
```

Sent from CP-server (Competition Points) hosts at race end to push the post-race rating deltas and standings to the lobby's CP backend. Out of scope for non-CP servers.

#### 0xf2 Heartbeat (periodic, in state 6)

```
u8   0xf2
[preamble]            (10-byte fixed header tying this to the registered server)
u8   load             (= (char)(rainLevel * DAT_140150698) — packed CPU/IO load proxy)
u8   0
u8   seq              (time-derived rolling counter)
```

Total wire size: 14 B. Constant-byte heartbeats get the server delisted from the public lobby once `connected_drivers > 0` (the lobby's freshness check rejects servers whose heartbeat byte 11 = `load` never changes).

### 11.6 Lobby-to-server messages

| Cmd | Name | Description |
|-----|------|-------------|
| `0xef` | Accept/Reject | Byte 1: 0=accepted, 1-6=rejection reason |
| `0xf1` | Unknown | Delegates to `FUN_1400473f0` |
| `0xf3` | CP data push | Competition Points data.  Reads two strings then a `ServerEventConfig`.  Logs `"Receiving CP data for %s @ %s"` |
| `0xf4` | State push A | Reads two strings, builds a `0x2b` message to clients via `FUN_1400251b0` |
| `0xf5` | State push B | Reads two strings, builds a `0x2b` variant via `FUN_140025470` |
| `0xf6` | Config request | Server replies with `0xd7` containing name, track, password |
| `0xfd` | Acknowledgment | Clears the "waiting for ack" flag |

### 11.7 Rejection reasons

| Code | Message |
|------|---------|
| 0 | `"Unexpected: Rejected with reason NotRejected"` |
| 1 | `"Server is outdated and can't connect to the lobby anymore"` |
| 2 | `"This server is of a higher version compared to the lobby"` |
| 3 | `"This server has been blocked by the backend"` |
| 4 | `"This server has been rejected by the backend for unknown reasons"` |
| 5 | `"This server isn't running on a supported platform AND is configured in a way that causes problems"` |
| 6 | `"Server did not respond on the public IP"` |

### 11.8 What the reimplementation does

**Nothing.** `registerToLobby` is hard-wired to `0`. The
reimplementation is invisible to the Kunos lobby and must be
joined via direct IP (see `serverList.json` mechanism in
`HB III.3.1`).

The reimplementation **must not** attempt to impersonate the
Kunos backend in traffic to real ACC clients, as this would
exceed the Art. 6 interoperability carve-out and would attack
Kunos's infrastructure by proxy.

---

## 12B. ServerMonitor protocol (protobuf)

A second protocol, entirely separate from the sim-side protocol, is used by server-monitoring and hosting tools. Unlike the sim protocol, this one is **protobuf-based** and its schema is fully known.

## 12A. Rating / CP system

### 12A.1 Architecture

The ACC rating system (SA, RC, Track Medals, Competition Points)
is entirely computed by the Kunos lobby backend.  The dedicated
server is a pure **relay/store**: it receives rating data from the
backend via the kson `0xf3` CP push (see section 11.6), stores it
per-connection, and periodically broadcasts it to clients via `0x4e`.

The server does NOT compute ratings locally.  Evidence:

- `FUN_140042030` (tagged `update_connection_rating` in
  notebook-a) is a 57-byte timestamp normalizer, not a rating
  calculator.  It reads two doubles from the connection struct
  and adds them to a parameter.
- The connection's rating field at struct offset +0x3f is never
  written by any lap-completion, session-end, or incident handler.
- The `ACP_ELO_UPDATE` (`0x51`) handler only stores a
  client-reported elo value; the server does not validate or
  compute it.
- The `-10` (= -1.0 after /10) values in wire captures are the
  uninitialized sentinel, sent when no backend has populated the
  rating.

### 12A.2 Rating fields

Known rating categories from the SDK and configuration:

| Name | Config key | Range | Source |
|------|-----------|-------|--------|
| Safety Rating (SA) | `safetyRatingRequirement` | 0-99 | Backend |
| Racecraft Rating (RC) | `racecraftRatingRequirement` | 0-99 | Backend |
| Track Medals (TM) | `trackMedalsRequirement` | 0-3 | Backend |
| Driver Category | (handshake) | 0=Bronze..3=Platinum | Client |
| Cup Category | (handshake/entrylist) | 0=Overall..4=Am | Config |
| Elo | `ACP_ELO_UPDATE` (0x51) | integer | Client-reported |

String fields "CN" (Consistency), "CC" (Car Control), "PC" (Pace),
and "TR" (Trust) do not appear in the server binary.  If they
exist, they are client-side-only or backend-only concepts.

### 12A.3 The 0x4e rating broadcast

Format (per-connection record):

```
u16  conn_id
u8   presence_flag (0 = no car found, non-zero = valid)
i16  ratingA * 10  (signed, 1 decimal place precision)
i16  ratingB * 10  (signed, same source as ratingA)
u32  0xFFFFFFFF    (sentinel)
str_a steam_id     (Format-A string)
```

Both ratingA and ratingB are read from the same connection struct
offset (+0x3f) in the Kunos binary, so they are always identical.
The distinction between SA and RC may only be meaningful when the
lobby backend populates them separately.

Broadcast trigger: the server tick checks a `ratings_dirty` flag
(connection list offset +0x140a1) on a configurable cooldown timer.
The flag is set when standings change.

### 12A.4 Implications for the reimplementation

Without a lobby backend connection, the server correctly sends
default/zero ratings.  This matches the Kunos server's behavior
when disconnected from the backend.  The `settings.json`
requirement fields are effectively ignored (treated as -1 =
disabled) since there is no backend to provide real rating data.

No local rating computation is possible or necessary.

---

### 12B.1 Transport and framing

Uses the same TCP listener as the sim protocol (`tcpPort`). The server distinguishes ServerMonitor clients from sim clients at connection time via the first message, which is a `ServerMonitorConnectionRequest` protobuf message instead of a sim-protocol handshake. (The exact demultiplexing rule needs to be confirmed; it may be based on a magic byte, an SNI-like prefix, or lazy fallback.)

Standard protobuf binary wire format (little-endian varints, length-delimited strings, tag-wire-type headers). No encryption. No custom framing beyond what protobuf itself provides.

### 12B.2 Message types

Protocol request types (`ServerMonitorProtocolRequest` enum):

| Value | Name |
|---|---|
| 0 | `PROTOCOL_REQUEST_ERROR` |
| 1 | `REGISTER_FOR_UPDATES` |
| 2 | `UNREGISTER_INTERFACE` |

Protocol message types (`ServerMonitorProtocolMessage` enum) — these are what the server pushes to the monitor:

| Value | Name |
|---|---|
| 0 | `PROTOCOL_MESSAGE_ERROR` |
| 1 | `REGISTRATION_RESULT` |
| 2 | `SERVER_CONFIGURATION` |
| 3 | `SESSION_STATE` |
| 4 | `CAR_ENTRY` |
| 5 | `CONNECTION_ENTRY` |
| 6 | `REALTIME_UPDATE` |
| 7 | `LEADERBOARD_UPDATE` |

### 12B.3 Key protobuf messages

**`ServerMonitorConnectionRequest`** — the monitor's hello:
```
string displayName
int32  realtimeCarUpdateInterval
bool   sendSelfcontainingLeaderboards
bool   sendExtendedLeaderboards
bool   registerToAllEvents
```

**`ServerMonitorHandshakeResult`** — the server's reply:
```
bool   success
int32  connectionId
string errorTxt
```

**`ServerMonitorConnectionEntry`** — per-connection state:
```
int32  connectionId
string firstName
string lastName
string shortName
string playerId        // Steam64 with 'S' prefix
bool   isAdmin
bool   isSpecator       // note: typo preserved from wire format
```

**`ServerMonitorCarEntry`**:
```
int32                        carId
ServerMonitorCarModelType    carModel
int32                        drivingConnectionId
int32                        raceNumber
ServerMonitorCupCategory     cupCategory
```

**`ServerMonitorSessionDef`** (one per configured session in `event.json`):
```
ServerMonitorSessionType  sessionType
int32                     round
int32                     durationSeconds
int32                     raceDay
int32                     minuteOfDay
int32                     timeMultiplier
int32                     overtimeDurationS
int32                     preRaceWaitTimeS
```

**`ServerMonitorConfigurationState`** — the server's current config snapshot:
```
string                      serverName
string                      trackName
int32                       maxSlots
int32                       trackMedals
int32                       saRequired
bool                        isPwProtected
bool                        isLockedEntryList
repeated ServerMonitorSessionDef  sessions
```

**`ServerMonitorSessionState`** — the current session snapshot:
```
int32  currentSessionIndex
int32  weekendTimeSeconds
float  idealLineGrip
int32  ambientTemp
int32  roadTemp
float  cloudLevel
float  rainLevel
float  trackWetness
float  dryLineWetness
float  trackPuddles
float  rainForecast10min
float  rainForecast30min
int32  carsConnected
```

**`ServerMonitorRealtimeCarState`**:
```
int32                 carId
int32                 drivingConnectionId
repeated int32        teamConnections
TimedValue            (?)     // exact field name not recovered
```

**`ServerMonitorRealtimeConnectionState`**:
```
int32    connectionId
int32    lastPing
int32    currentAveragePing
int32    legacyLatencyOffset
int32    lockstepReferencePing
int32    lockstepLatencyOffset
int32    lockstepAccumulatedLatencyError
int32    lastUdpPaketReceived       // typo preserved
repeated TimedValue accumulatedLatencyErrorHistory
```

**`ServerMonitorRealtimeUpdate`** — the periodic push (every `realtimeCarUpdateInterval` ms):
```
int32                                          serverNow
ServerMonitorSessionState                      sessionState
repeated ServerMonitorRealtimeConnectionState  connections
repeated ServerMonitorRealtimeCarState         cars
```

**`ServerMonitorLeaderboardEntry`**:
```
ServerMonitorCarEntry          carEntry
string                         currentConnectionSteamId
int32                          missingMandatoryPitstops
repeated int32                 driverTimes
int32                          lastLapTime
repeated int32                 lastLapSplits
int32                          bestLapTime
repeated int32                 bestLapSplits
int32                          lapCount
int32                          totalTime
ServerMonitorPenaltyShortcut   currentPenalty
int32                          currentPenaltyValue
string                         driverName
string                         driverShortName
ServerMonitorCarModelType      carModel
```

**`ServerMonitorLeaderboard`**:
```
int32                                    bestLap
repeated int32                           bestSplits
bool                                     isDeclaredWetSession
repeated ServerMonitorLeaderboardEntry   entries
```

**`ServerMonitorChatMessages`**:
```
int32   authorConnectionId
int32   serverTimestamp
string  message
```

### 12B.4 ServerMonitor enums

**`ServerMonitorSessionType`**: `Practice=0, Qualifying=1, Race=2` (note: these are wire values in the ServerMonitor protocol, which differ from both the handbook IX.6 values `{0, 4, 10}` and the SDK broadcasting enum values. The ServerMonitor protocol uses its own, simpler enumeration. Implementations must translate.)

**`ServerMonitorCupCategory`**: `Overall=0, ProAm=1, Silver=2, National=3` — only 4 values, missing `Am` from handbook §IX.5. The ServerMonitor protocol is apparently missing this value; monitoring tools either don't distinguish Am from something else or the enum is outdated.

**`ServerMonitorCarModelType`**: 38 entries corresponding to handbook §IX.3 car IDs 0-26 (GT3) and 50-61 (GT4), but missing all post-1.2 additions (BMW M2 CS Racing, Porsche 911 GT3 Cup Type 992, Lamborghini Huracán Super Trofeo EVO2, BMW M4 GT3, Audi R8 LMS GT3 evo II, Ferrari 296 GT3, Lamborghini Huracan Evo2, Porsche 992 GT3 R, McLaren 720S GT3 Evo 2023, Ford Mustang GT3, and all GT2 cars). The enum was apparently frozen at an early release; new cars may be reported as numeric IDs without an enum name, or the ServerMonitor API may simply lack fidelity for newer rosters.

**`ServerMonitorPenaltyShortcut`**: 34 values combining penalty action and reason into single enum constants. Full list:

```
0  No_Penalty
1  DriveThrough_Cutting
2  StopAndGo_10_Cutting
3  StopAndGo_20_Cutting
4  StopAndGo_30_Cutting
5  Disqualified_Cutting
6  RemoveBestLaptime_Cutting
7  DriveThrough_PitSpeeding
8  StopAndGo_10_PitSpeeding
9  StopAndGo_20_PitSpeeding
10 StopAndGo_30_PitSpeeding
11 Disqualified_PitSpeeding
12 RemoveBestLaptime_PitSpeeding
13 Disqualified_IgnoredMandatoryPit
14 PostRaceTime
15 DriveThrough_RaceControl
16 StopAndGo_10_RaceControl
17 StopAndGo_20_RaceControl
18 StopAndGo_30_RaceControl
19 Disqualified_RaceControl
20 Disqualified_PitEntry
21 Disqualified_PitExit
22 Disqualified_WrongWay
23 Disqualified_LightsOff
24 DriveThrough_IgnoredDriverStint
25 StopAndGo_30_IgnoredDriverStint
26 Disqualified_IgnoredDriverStint
27 Disqualified_ExceededDriverStintLimit
28 Disqualified_DriverRanNoStint
29 Disqualified_DamagedCar
30 DriveThrough_SpeedingOnStart
31 StopAndGo_30_SpeedingOnStart
32 Disqualified_SpeedingOnStart
33 DriveThrough_WrongPositionOnStart
34 StopAndGo_30_WrongPositionOnStart
35 Disqualified_WrongPositionOnStart
```

These enumerate every reason the server can auto-penalize a car: cutting (going off-track for an advantage), pit speeding, race-control-assigned penalties (admin `/dt`, `/sg`, etc.), pit entry/exit violations, wrong-way, lights-off rule, driver stint time limits, driver swap rules, speeding on race start, wrong grid position on race start. Any reimplementation that wants to claim a "correct" penalty system must handle all of these reasons (phase 5+ work).

### 12B.5 Notes for a reimplementation

- **ServerMonitor is optional.** The ACC game client does not use it. You can skip implementation entirely if you don't care about admin tooling compatibility.
- **If you do implement it**, the protobuf schema is self-describing and can be regenerated from the descriptor data embedded in the Kunos binary (see Notebook A). The schema is stable-ish — fields are additive and enum values are locked.
- **The `serverNow` field in `ServerMonitorRealtimeUpdate`** is likely a monotonic millisecond clock, not a wall clock. Validate before assuming.
- **Typos on the wire**: `isSpecator` (should be `isSpectator`) and `lastUdpPaketReceived` (should be `Packet`). These are frozen into the protobuf schema and must be preserved for wire compatibility.
- **ServerMonitor protocol version**: the binary embeds proto3 syntax, but there is no version field in any of the messages. Schema evolution appears to be based purely on protobuf's forward/backward compatibility guarantees (new fields are safely ignored by older readers).

---

## 12. Client-side broadcasting protocol (reference)

Documented here only because it reveals parts of the data model and wire conventions that inform the sim-side protocol. **The dedicated server does not implement this protocol.**

### 12.1 Transport

Single UDP socket on the game client (`Documents/config/broadcasting.json` in the client). One datagram per message. Little-endian.

### 12.2 String format

`uint16 length` + UTF-8 bytes, no terminator (`SDK/BroadcastingNetworkProtocol.cs:349-354`).

### 12.3 Outbound message types (client → ACC game client)

| ID | Name | Source |
|---|---|---|
| 1 | REGISTER_COMMAND_APPLICATION | `BroadcastingNetworkProtocol.cs:14` |
| 9 | UNREGISTER_COMMAND_APPLICATION | " |
| 10 | REQUEST_ENTRY_LIST | " |
| 11 | REQUEST_TRACK_DATA | " |
| 49 | CHANGE_HUD_PAGE | " |
| 50 | CHANGE_FOCUS | " |
| 51 | INSTANT_REPLAY_REQUEST | " |
| 52 | PLAY_MANUAL_REPLAY_HIGHLIGHT | planned, unimplemented |
| 60 | SAVE_MANUAL_REPLAY_HIGHLIGHT | planned, unimplemented |

### 12.4 Inbound message types (ACC game client → overlay)

| ID | Name | Source |
|---|---|---|
| 1 | REGISTRATION_RESULT | `BroadcastingNetworkProtocol.cs:28` |
| 2 | REALTIME_UPDATE | " |
| 3 | REALTIME_CAR_UPDATE | " |
| 4 | ENTRY_LIST | " |
| 5 | TRACK_DATA | " |
| 6 | ENTRY_LIST_CAR | " |
| 7 | BROADCASTING_EVENT | " |

### 12.5 Versioning

`const int BROADCASTING_PROTOCOL_VERSION = 4;` (`BroadcastingNetworkProtocol.cs:41`). Client sends this in `REGISTER_COMMAND_APPLICATION`; the ACC game client accepts or rejects.

### 12.6 REGISTER handshake wire format

```
byte   1 (REGISTER_COMMAND_APPLICATION)
byte   4 (BROADCASTING_PROTOCOL_VERSION)
string displayName
string connectionPassword
int32  msRealtimeUpdateInterval
string commandPassword
```

(`BroadcastingNetworkProtocol.cs:370-385`)

### 12.7 REGISTRATION_RESULT wire format

```
byte   1 (REGISTRATION_RESULT)
int32  ConnectionId
byte   connectionSuccess (>0 = true)
byte   isReadonly (0 = write access, >0 = read-only)
string errorMessage
```

(`BroadcastingNetworkProtocol.cs:105-118`)

### 12.8 Other messages

See `BroadcastingNetworkProtocol.cs` directly. Full wire format is documented inline in that source file's `ProcessMessage` method.

---

## 13. Changelog highlights (from `CL`)

Key protocol-affecting entries:

- **1.5.8** — ACC server was separated from the main ACC Steam product and became a standalone Steam Tool.
- **1.7.4** — Added `ignorePrematureDisconnects`. Defaults to 1; set 0 for strict 5s inactivity timeout on "not supported operating systems where TCP sockets act differently." Strong hint that TCP socket handling is fragile.
- **1.8.0** — Weather model aligned with client; track year suffixes deprecated; adjusted formation trigger points.
- **1.8.5** — Added `publicIP` setting.
- **1.8.11** — `allowAutoDQ: 0` no longer reduces reckless-driving DQ or failure-to-serve-penalty DQ. Assists without manual override no longer enforced.
- **1.8.17** — Added option to access servers with `registerToLobby: 0`. Fixed a server vulnerability reported by Leonard Schüngel. Updated handbook.
- **1.9.x / 1.10.x** — Protocol updates to follow client releases, each with new car/track DLC.

Every "Protocol update to follow client update" note means the sim wire format changed. Notebook A work must be re-done whenever the target build is bumped.

---

## 14. Known-unknowns summary

This list is what Notebook A still needs to resolve after Passes 1 through 2.17. Most of the original blind spots are now answered in §5, §6, §8 and §11 of this document. The shortlist of what remains, with everything that has been definitively answered or **negatively** answered called out:

**Resolved by the static analysis pipeline:**

- ~~Pit stop service request/grant flow~~ — **negative finding**: there is no separate pit stop service sub-protocol on the dedicated server. The pit menu (fuel / tyre / repair selection) is entirely **client-side**; the server only validates the served-mandatory event via client→server `0x54` `ACP_MANDATORY_PITSTOP_SERVED`. Pit-related rules (refuelling required, tyre change required, driver swap required, auto pit limiter) are server-config-only. A reimplementation does not need to handle a pit stop service protocol — only `0x54`.
- ~~Result finalization sub-protocol~~ — resolved: server→client `0x3e` is the **session results broadcast** (built from the timing module's session results getter), emitted from the main server tick tail when a session ends. Result files are written to `results/YYMMDD_HHMMSS_*` (regular + `_CP_` championship variant + `_entrylist` snapshot). After the broadcast the server waits `postQualySeconds` (qualy) or `postRaceSeconds` (race) before advancing.
- ~~Race start grid positions push~~ — resolved: server→client `0x3f` is the race start grid positions broadcast, emitted at session phase 4 (race countdown). A reimplementation must emit this for the client to render the starting grid.
- ~~Per-tick rating push~~ — resolved: server→client `0x4e` is the periodic per-connection rating summary (timer-paced).
- ~~Leaderboard push~~ — resolved: server→client `0x36` is the leaderboard update broadcast (was previously mislabelled as a keepalive). Followed by per-car `0x07` fan-out via the generic serializer.

**Resolved by Pass 2.18 / Pass 2.19 (serializer & vtable decoding):**

- ~~`WeatherStatus::serialize` body~~ — resolved: vtable[0x20] is `FUN_14011e930` which writes a **9-float WeatherStatus block**.  Final wire order is **ambient, road, windSpeed, windDirection, cloudLevel, rainLevel, WS+0x40, WS+0x44, WS+0x48**, derived from the serialize write order `0x28, 0x2c, 0x30, 0x34, 0x3c, 0x38, 0x40, 0x44, 0x48` combined with the field names in the JSON serializer `FUN_140114100` (string literals `ambientTemperature`, `roadTemperature`, `windSpeed`, `windDirection`, `rainLevel`, `cloudLevel` map to struct offsets 0x28, 0x2c, 0x30, 0x34, 0x38, 0x3c respectively).  An earlier reading of a 2026-04-15 capture as `clouds, wind_dir, rain, wind_speed` at slots `[9..12]` was wrong — see §5.6.4a catalog entry for `0x37` for the corrected layout.
- ~~`0x36` per-car leaderboard record body~~ — resolved: the second half of `FUN_140034210` adds two variable-length lists (sector splits / lap times), each with a u8 count, a `u8 wide_flag` (1 if any value exceeds 0xffff, in which case all entries are u32; otherwise u16 capped at 0xffff), and the trailing two u8 fields from struct offsets 0x200 and 0x201.
- ~~`0x40` race weekend reset body~~ — resolved: the object passed at `param_1[0x1410e]` is a `WeatherData`-derived object whose vtable[0x20] (`FUN_14011e660`) writes **12 × u32 weather/forecast scaling factors** from struct offsets 0x28–0x58 (with 0x2c skipped) followed by **two variable-length u32 vector lists** (forecast samples). Total: 1 + 48 + 2 + N1×4 + 2 + N2×4 bytes.
- ~~`ACP_CAR_UPDATE` exact wire format~~ — fully resolved (see §5.6.2). 68 bytes total. Three Vector3 blocks at consecutive storage offsets, two distinct 4-byte input arrays at non-consecutive storage offsets.

**All practical unknowns are now closed by Pass 2.22.** The seven generic-serializer ids `0x01`–`0x07` were resolved as **wrappers around the ServerMonitor protobuf message types 1–7** (already documented in §12B): the dedicated server uses the same C++ classes (`ServerMonitorHandshakeResult`, `ServerMonitorConfigurationState`, `ServerMonitorSessionState`, `ServerMonitorCarEntry`, `ServerMonitorConnectionEntry`, `ServerMonitorRealtimeUpdate`, `ServerMonitorLeaderboard`) and the same protobuf serialization for both the dedicated ServerMonitor channel and the in-game state push. So the body of each `0x0N` message is exactly the protobuf encoding of the corresponding `ServerMonitorProtocolMessage` type, schema in §12B.3.

Other Pass 2.22 resolutions:

- ~~`0x47` server→client wire format~~ — resolved: `u8 + u16 carId + u8 driver_count + driver_count × u8 swap_state` (4+N bytes). Built by `FUN_140011bf0`.
- ~~Per-car session result record (336 bytes source)~~ — resolved: 24-byte fixed header + a complete leaderboard record via `FUN_140034a40` from struct offset +0x98. Each row is ~100–250 bytes on the wire.
- ~~`0x56` per-Lap record wire format~~ — resolved: Format-A track_name + u32 lap_time + u8 split_count + N × u32 splits + u16 + u8 + u16. Built by `FUN_1400328f0` per 0x60-byte source struct.

**Practical recommendation for reimplementers**: for `0x01`–`0x07`, link against the ServerMonitor `.proto` schema and call its serializer directly. The wire bytes are byte-identical to the dedicated ServerMonitor channel — you do NOT need a separate implementation. For all other ids, the catalog in §5.6.4a now has byte-exact wire formats.

**The two remaining "runtime-only" questions are now answered by a definitive STRUCTURAL finding from Pass 2.23**: the dedicated server does NOT process car position, orientation, or input semantically anywhere. It is a **pure relay** for the per-tick `ACP_CAR_UPDATE` body. Specifically:

1. The server has **zero trigonometric calls** (`sinf` / `cosf` / `atan2f` / `asinf` / `acosf`) on any per-car state field. The only `sinf`/`cosf` callers in the entire binary are inside `FUN_140116830`, the **weather rain/cloud simulator** (it modulates rain over time using sinusoids; nothing to do with cars).
2. The server has **zero string references** to "WorldPos", "PosX", "Yaw", "rotation", "heading", "distance", or "coordinate" — only `defaultGridPosition` (an integer slot index, not a 3D coordinate).
3. The server has **zero per-car float math** beyond the velocity magnitude check at car +0x20 (`sqrt(x²+y²+z²)` to update the "last seen moving" timestamp at +0x158).
4. The server has **zero broadcasting-protocol code** that would translate per-car state into the SDK's `RealtimeCarUpdate` format. The only "broadcasting" strings in the binary are `"Couldn't setup UDP broadcasting socket"` (LAN discovery on port 8999), unrelated to the broadcasting SDK.
5. Both `0x1e` and `0x39` server→client broadcasters relay vec_a, vec_b, vec_c byte-for-byte from the parsed inbound `ACP_CAR_UPDATE` without any transformation. The 0x39 broadcaster has a **strict size check** that expects each per-car block to be exactly `0x3f = 63` bytes, logged as `"CarUpdate size is unexpected; did you forget to update the megapak? (%d byte, %d byte expected)"`.

**Therefore the question "which Vector3 is position vs orientation?" has no answer in the dedicated server's binary**, because the dedicated server does not need to know. A reimplementation can:

1. **Trust the bytes blindly** — store the 36 bytes of vec_a + vec_b + vec_c in the per-car state, relay them in `0x1e` / `0x39` broadcasts.
2. **Apply the convention from the broadcasting SDK** for any non-relay purpose: `WorldPosX, WorldPosY` come first in the SDK's `RealtimeCarUpdate`, so the natural interpretation is **vec_a (+0x8) = world position (x, y, z)**, **vec_b (+0x14) = orientation** (most likely forward direction unit vector or Euler angles roll/pitch/yaw), **vec_c (+0x20) = velocity** (rigorously confirmed via the magnitude check).
3. **Verify with a single packet capture** if byte-perfect compatibility matters — values for position will be in the meter scale (typically -2000..+2000 for racing tracks), orientation in radians (-π..+π) or unit-vector range (-1..+1).

The same logic applies to the various scalar bytes (`scalar_32`, `scalar_33`, `scalar_36`, `scalar_2c`, `scalar_34`, `scalar_35`, `scalar_44`, `scalar_4c`, `scalar_1ec`) and the two 4-byte input arrays: the server stores and relays them but never interprets them. Their semantic exists only in the game client.

## 15. Non-sim UDP noise observed in the wild

Real ACC clients send occasional **non-sim-protocol UDP packets** to the game-server port.  Documenting what we've seen so reimplementers don't chase it as a protocol gap:

### 15.1 QUIC v1 Initial packets (msg_id = `0xc3`, body ≈ 1200 bytes)

Byte layout starts:

```
c3 00 00 00 01 08 78 e7 98 46 bb f3 79 84 ...
```

- Byte 0 `0xc3` = `0b11000011`: QUIC long-header form, fixed bit, Initial packet type, pn_len=4 (RFC 9000 §17.2).
- Bytes 1-4 `00 00 00 01`: QUIC v1 version.
- Byte 5 `0x08`: DCID length = 8.
- Bytes 6-13: destination connection ID (random).
- Remaining ~1180 bytes: AES-GCM-encrypted Initial payload (high entropy, as expected).

Observed in bursts of 10+ packets across 2-5 ms from multiple distinct client IPs, consistent with a QUIC client's **Initial-retry storm** when the peer doesn't complete the handshake.  Likely origin: misdirected telemetry / background QUIC probe from the client's process, *not* an ACC sim-protocol feature.

**accServer.exe doesn't handle it either** — neither `FUN_140027f80` (UDP inline handler) nor `FUN_140029250` (LAN discovery handler) compares against `0xc3`.  Stock Kunos servers drop these packets with no reply.  The retry storm dies on its own after the client's Initial retry count is exhausted.

Reimplementations should drop them silently or log at WARN for operator visibility.  Implementing a QUIC responder is not useful — we don't know what application-layer service the client expects on top of the QUIC transport, and the client's handshake will fail on SNI/ALPN mismatch anyway.

**Protocol decoding is now fully closed** for any practical reimplementation purpose. The dedicated server's job is to receive `ACP_CAR_UPDATE` packets and relay them; a reimplementation does the same.

**Newly resolved by Pass 2.20** (recursive smoking-gun search):

- ~~Vector3 semantic interpretation~~ — partially resolved: `FUN_1400427c0` computes `sqrt(x²+y²+z²)` of the third Vector3 (car_state +0x20) and compares it × ~3.6 against a km/h threshold to update the car's "last seen moving" timestamp. This **proves vec_c is the velocity vector**. By elimination and convention, vec_a (+0x8) is most likely position (X, Y, Z) and vec_b (+0x14) is orientation (probably Euler angles roll/pitch/yaw or a forward-direction unit vector) — but only vec_c's semantic is rigorously confirmed.
- ~~The packet header `u8 flag`~~ — `FUN_1400419e0` confirms it's a **rolling packet sequence counter** (the server tracks `current - previous == 1` for valid in-order packets and computes a drop rate, NOT a gear/pit flag).
- The **two 4-byte input arrays** are at non-consecutive storage offsets (+0x2e..+0x31 and +0x48..+0x4b), confirming they hold distinct data. The natural interpretation is "pedals" + "assists/wheel" but this is not statically verifiable without packet capture.


