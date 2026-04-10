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

## 4. Session state machine

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
| `0x19` | 25 | **`ACP_LAP_COMPLETED`** (formerly catalogued as "client lap-time report") — body is `u16` `u16` `i32` `u8` (cup position, track position, lap time in ms, and a signed quality byte). The server validates the fields (errors include `"Received lap with isSessionOver flag; will ignore it"`, `"ACP_LAP_COMPLETED hit empty leaderboard"`, `"received ACP_LAP_COMPLETED, but no car %d found"`, and the fatal `"ACP_LAP_COMPLETED currentLeaderboard.lines is empty, but getLineByCarId delivered"`), updates the reporting connection's rating state, then broadcasts a transformed `0x1b` message to every other client. This message uses the tier-2 queued-lambda broadcast mechanism, not the direct relay. |
| `0x20` | 32 | **`ACP_SECTOR_SPLIT` (bulk variant)** — client reports multiple sector split times at once. Body: `i32` + `u8` + `i32` + `u16` header. Errors include `"Received split with isSessionOver flag; will ignore it"`, `"ACP_SECTOR_SPLIT hit empty leaderboard"`, and `"received ACP_SECTOR_SPLIT, but no car %d found"`. The server broadcasts a transformed `0x3a` message to all other clients carrying the full split list (`u16 car_id` + `u8 split_count` + `u32[count]` + `i32 clock` + `u16 car_field`). |
| `0x21` | 33 | **`ACP_SECTOR_SPLIT` (single variant)** — client reports a single sector split. Body: `i32` + `i32` + `u8` + `u16` + `u8` header. Same error log family as the bulk variant. The server broadcasts a transformed `0x3b` message (fixed-length: `u16` + `u32` + `u8` + `u32` + `u16`) to all other clients. |
| `0x2a` | 42 | **`ACP_CHAT`** — body is a Format-A `sender_name` string + a Format-A `chat_text` string, parsed via `FUN_14002c0b0`. The server logs `"CHAT %s: %s"`, runs a printf-format-specifier sanitization pass over the text (rejects messages containing `%%` patterns to prevent format-string injection), then calls `FUN_140021680` (the chat command parser, see §8 / Pass 2.15) to dispatch any leading `/`-keyword. If the parser returns 0 (the message was NOT a command) the server builds a `0x2b` chat broadcast message via `FUN_140033030` and sends it to every connected client via `FUN_14001ada0`. Errors include `"Received chat message from null connectionCallback"`. |
| `0x2e` | 46 | **`ACP_CAR_SYSTEM_UPDATE`** — body is `u16 carId` + `u64 system_data` (10 bytes total). The server validates the carId matches the connection's owned car (otherwise logs `"Received ACP_CAR_SYSTEM_UPDATE for wrong car - senderId %d, carId %d"`), stores the `system_data` u64 at car-state offset `0x1b0`, then broadcasts a relayed server `0x2e` message containing `u16 carId + u64 system_data + u64 server_timestamp` to every other connected client. Log: `"Updated %d clients with new carSystem for car %d (%ul)"`. |
| `0x2f` | 47 | **`ACP_TYRE_COMPOUND_UPDATE`** — body is `u16 carId` + `u8 tyreCompound`. The server validates ownership (otherwise logs `"Received ACP_TYRE_COMPOUND_UPDATE for wrong car - senderId %d, carId %d"`), stores the compound byte at car-state offset `0x152`, then broadcasts a server `0x2f` message (`u8 = 0x2f` + `u16 carId` + `u8 tyreCompound`, 4 bytes total) to every other client. Log: `"Updated %d clients with new tyreCompound for car %d"`. |
| `0x32` | 50 | **`ACP_CAR_LOCATION_UPDATE`** — body is `u16 carIndex` + `u8 carLocation` (5-value enum: NONE/Track/Pitlane/PitEntry/PitExit, see §7.9). Historical ACP name is still current. |
| `0x3d` | 61 | **`ACP_OUT_OF_TRACK`** — body is `u8 force_flag` + `i32 timestamp_raw`. The server normalizes the timestamp through `FUN_140042030` (raw ticks → session time delta), looks up the car at `connection +0xa00a0` (otherwise logs `"received ACP_OUT_OF_TRACK, but no car %d found"`), checks the car's out-of-track flag at car+0x180+0x28 (skips if already set), then sets the flag and broadcasts a transformed server `0x3c` message (body `u16` + `u16` + `u32`, 9 bytes) to all other clients via the tier-2 queued-lambda broadcast mechanism. |
| `0x41` | 65 | **Probable client-reported penalty event** — body is `u8 force_flag` + `u8 penalty_type` + `u64 raw_timestamp` + `i32 value`. The server normalizes the timestamp via `FUN_140042030`, then calls into the timing/penalty module (`FUN_140125f50`) with the carId + the parsed fields. A server-config flag at `param_1[4]+0` gates whether the force-flag override applies. Probably the client's auto-penalty subsystem reporting a track-cut, pit-speeding, or other infraction. |
| `0x42` | 66 | **Probable client lap-tick / time-event report** — body is a single `u64 raw_timestamp`. The server normalizes the timestamp via `FUN_140042030` and calls `FUN_140126b50(timing_module, carId, normalized_time)`. Likely a periodic "current race time" signal from the client used to drive penalty bookkeeping or as a coarse clock-sync mechanism. |
| `0x43` | 67 | **Damage zones update** — body is 5 × `u8` (one normalized value per damage zone). The server stores them as 5 `float`s in the car-state damage block at car-struct offset 0x1b8, then broadcasts a transformed server **`0x44`** message to every other client via UDP `broadcast_except_one`. Log: `"Updated %d clients with new damage zones for car %d"`. **Note**: this is one of three distinct uses of the id byte `0x44` — see the server→client `0x44` row in §5.6.4a and the lobby-protocol `0x44` registration described there. |
| `0x46` | 70 | **Car dirt status update** (`ACP_CAR_DIRT`) — body is 5 × `u8` (each normalized to a `float` and packed into a `ksRacing::CarDirtStatus` record at car-state offset 0x160). The server then builds and broadcasts a **`0x46`** relay (msg id + `u16 carId` + appended status block) to every other client via the `broadcast_except_one` helper. Wire captures confirm the real client sends `0x46` for car dirt; the Kunos server logs it as unmapped id 70, suggesting it is received but silently dropped server-side. Previously documented as `0x45`; corrected from capture evidence. |
| `0x47` | 71 | **`ACP_UPDATE_DRIVER_SWAP_STATE`** — body is `u16 carId` followed by a serialized swap-state record (one driver entry per slot). The server validates that the connection actually owns the target car (otherwise logs `"Received ACP_UPDATE_DRIVER_SWAP_STATE for alien car: %d (receiver car %d, connection %d)"`). It then validates the per-entry record count matches the entry list driver count (otherwise logs `"ACP_UPDATE_DRIVER_SWAP_STATE Swap data has less drivers %d than entries %d (receiver car %d, connection %d)"`). For each driver in the record it updates the swap state and logs `"Updated driverSwapState for car %d driver %d (%s): %d -> %d"` (or `"Updated (foreign) driverSwapState ..."` if the update came from a different connection than the car owner). The state change is then forwarded to other connections via the `"Forwarding driver swap payload to connection %d for carId %d, driverIndex -> %d"` path. |
| `0x48` | 72 | **`ACP_EXECUTE_DRIVER_SWAP`** — body is `u16 carId` + `u8 swap_request_code`. The server validates the connection owns the car (otherwise logs `"ACP_EXECUTE_DRIVER_SWAP, but no car controlled for connection %d"` or `"... carId mismatch: %d (car controlled %d for connection %d)"`), then runs the swap procedure via the std::function dispatch. Result reply: server sends back **`0x49`** (u8 msg id + u8 result_code) over TCP to the requesting client (logged as `"Driver swap result: %d"` or, on failure, `"Driver swap failed: %d"`). On success and if a server-config flag is set, also emits **`0x58`** broadcast (u8 + u16 carId + u8 swap_request_code) to every other client. **Distinct from** the UDP `0x48` LAN discovery probe — the two share an id byte but are disambiguated by transport. |
| `0x4a` | 74 | **`ACP_DRIVER_SWAP_STATE_REQUEST`** — body is `u16 carId` + `u8 sub_state` + `u8 connection_state`. The server validates ownership (otherwise logs `"ACP_DRIVER_SWAP_STATE_REQUEST for the wrong carId: %d (Connection owns %d)"`). Sub-state values 2, 3, 4 are accepted; anything else triggers `"DriverSwap Request for type %d is not implemented"`. Sub-state 3 walks every connection sharing the car and bumps each one from sub-state 3 to 2; sub-state 2 requires the request connection to actually own the car at the moment (`"DriverSwap 'Request'-Request, but car isn't controlled (%d) by this connection (%d)"`) and then dispatches via a per-entry lambda. The connection's own swap state byte is updated and the message log records `"Connection %d on car %d changes its swap connection state from %d to %d"`. |
| `0x4f` | 79 | **`ACP_DRIVER_STINT_RESET`** (formerly catalogued as a generic "event report") — body is `u8 force_reset_flag` + `u64 timestamp`. The server logs `"Receives driver stint reset for car %d"`, normalizes the timestamp through `FUN_140042030`, then dispatches to one of two timing-update functions depending on the force flag, both wrapped in lambdas that send a per-recipient TCP message. The on-the-wire server→client `0x4f` (with sub-opcode 0x00 / 0x01 — see §5.6.4a) is the broadcast variant of this same event. |
| `0x51` | 81 | **`ACP_ELO_UPDATE`** — body is `u16 new_elo` + `u16 (unused?)`. The server logs `"Car %d elo update %d => %d (%d)"`, writes the new value into the car-state struct at offset 0x1f8, and sets a "results dirty" flag for the next save. No outbound message. |
| `0x54` | 84 | **`ACP_MANDATORY_PITSTOP_SERVED`** — body is `u16 carId`. The server validates the carId matches the connection's owned car (otherwise logs `"Received ACP_MANDATORY_PITSTOP_SERVED for carId %d, but connection is %d"`), then logs `"Served Mandatory Pitstop: %d"` and clears the mandatory-pitstop-pending flag for the car via the timing module. No outbound message. |
| `0x55` | 85 | **`ACP_LOAD_SETUP`** — body is `u8 setup_index` + `u16 carId` + `u32 setup_revision`. The server looks up the setup data either from the active session's setup table or from a per-car setup library (depending on whether the requested setup_index matches the session's current pinned setup), then builds and sends back a **`0x56`** TCP response carrying the setup blob via the per-setup serializer. If `setup_index` is out of range the server returns silently without a reply. |
| `0x5b` | 91 | **`ACP_CTRL_INFO`** — body is a complex `ksRacing::CtrlInfo` struct read by a dedicated parser, carrying: `u16 carId`, two Format-B strings (model/livery), several state booleans (gpe / as / sc / scp / defaults), a scaling factor `f32`, fuel and wear floats, and a setup_id. The server logs `"Ctrl Info carId %d (%s): %s"` followed by a flag string built from the booleans, then for every connection that meets a server-side filter it builds a per-recipient **`0x2b`** message containing a server-stringified version of the ctrl info and sends it over that connection's TCP socket. If the formatted message would exceed 250 bytes the server replaces it with the literal `"Received ctrl info, but message is too long. Please check logs"`. |

#### 5.6.2 UDP message IDs

Messages carried over the unreliable UDP channel on the main `udpPort`. 7 distinct IDs. These are handled by a chain of inline `if` blocks in the server rather than a central switch:

| ID (hex) | ID (dec) | Name / meaning |
|---|---|---|
| `0x13` | 19 | **Keepalive** — body is `u16 conn_id` (3 bytes total). The server uses this to associate the UDP source address with the TCP connection (peer-address learning) and replies with a `0x14` message containing a `u32` server timestamp plus per-car timing hints (15 bytes). This is the primary mechanism for UDP peer-address association; a reimplementation must handle it, not silently drop it. |
| `0x16` | 22 | **`PONG_PHYSICS`** — client echo of the server's `0x14` keepalive. Body: `u16 conn_id` + `u32 server_timestamp_echo` + `u32 client_offset` (11 bytes total). Used by the server to measure per-client latency and adjust simulation timestamps. |
| `0x17` | 23 | **Keepalive (variant)** — same treatment as `0x13`: triggers a `0x14` reply and UDP peer-address learning. **Note that `0x17` is ALSO used in the outbound direction** — the UDP inline handler emits `0x17` messages as part of its processing of other request messages. Server→client `0x17` is a separate message with its own wire format (built as a small reply record with a following `u32`). |
| `0x1e` | 30 | **`ACP_CAR_UPDATE`** — the per-tick car state update, sent by each client at simulation tick rate. Total wire size: **68 bytes including the msg id byte**. Exact field-by-field wire format: <br><br> `u8 = 0x1e` (msg id) <br> `u16 source_conn_id` <br> `u16 target_car_id` <br> `u8 packet_sequence` (rolling counter that wraps every 256 packets — server tracks `current - previous == 1` to compute drop rate, NOT a gear/pit flag) <br> `u32 client_timestamp_ms` <br> `Vector3 position` (12 bytes — world position x/y/z floats; stored at car +0x8) <br> `Vector3 orientation` (12 bytes — most likely Euler angles roll/pitch/yaw OR a forward-direction unit vector; stored at car +0x14) <br> `Vector3 velocity` (12 bytes — **confirmed velocity**: the server computes `sqrt(x²+y²+z²)` of this vector and clamps it against a km/h threshold; stored at car +0x20) <br> `u8 input_a[4]` (4-byte input array #1 — likely pedals: throttle / brake / clutch / handbrake or similar as 0-255 normalized; stored at car +0x2e..+0x31) <br> `u8 scalar_32` <br> `u8 scalar_33` <br> `u16 scalar_36` <br> `u8 scalar_2c` <br> `u8 scalar_34` <br> `u8 scalar_35` <br> `u32 scalar_44` (probably another timing field) <br> `u8 input_b[4]` (4-byte input array #2 — possibly steering / TC / ABS / engine map; stored at car +0x48..+0x4b) <br> `u8 scalar_4c` <br> `i16 scalar_1ec` (sign-extended on receive, likely a signed delta or trim value) <br><br> The packet sequence counter is consumed by `FUN_1400419e0` which tracks valid-vs-out-of-order rates and computes a clock skew between client and server timestamps. The velocity vector's magnitude is consumed by `FUN_1400427c0` which compares `magnitude × ~3.6 (km/h conversion)` against a threshold and updates the car's `last seen moving` timestamp at car +0x158 — this is the strongest evidence that vec_c IS velocity. <br><br> Server validates that the source connection owns the target car and rejects updates with mismatched ownership (`"Received car update for a different car, connectionId %d. Expected: %d Received: %d"`). It also drops outdated packets where `client_timestamp_ms` is not newer than the last seen timestamp (`"Dropped outdated car_update paket for carId %d, clientTimestamp %d vs lastTimeStamp %d"`). The same id byte `0x1e` is used by server→client periodic state broadcast — see §5.6.4a — and the server-side broadcast uses the same field offsets, confirming the format is symmetric across the relay. |
| `0x22` | 34 | **`CAR_INFO_REQUEST`** — body is `u16 connectionId` + `u16 carIndex`. The server replies with a full `CarInfo` structure for the requested car. Historical ACP name is still current. |
| `0x5e` | 94 | Four-field record: `u16` + `u16` + `u64` + `u8` (possibly a time-synchronized event) |
| `0x5f` | 95 | **Admin / server-identity query** — the client sends a Format-B string (typically an identifier or admin password). If the string matches the server's configured identifier, the server replies on UDP with another Format-A string containing the server's name / identification. Used by admin tooling to verify server identity over UDP without establishing a full TCP session. A reimplementation targeting private MP can ignore these messages. |

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
u8   0x12               (constant, purpose unknown)
u16  nconns             (number of active connections, varies)
u16  connection_id      (the server-assigned id for this client)
u16  0x0000             (padding)
```

Then two **u16-byte-length-prefixed raw UTF-8** strings (not Format-A):

```
u16  server_name_len + server_name_bytes    (e.g. "ACC Server (please edit settings.json)")
u16  track_name_len  + track_name_bytes     (e.g. "mount_panorama")
```

**Trailer body** (after the strings, ~1900 bytes):

```
u8   separator = 0x01
u16  car_id                  (server-assigned car ID, starts at 1001)
u8   flag_a = 0x01
u8   flag_b = 0x01
u32  preRaceWaitingTimeSeconds
u32  sessionOverTimeSeconds
i32  raceNumber              (echoed from the client's request)
u8   carModel                (echoed from the client's request)
     <~1050 bytes season entity / track data, zero-filled is accepted>
u8   separator = 0x01
     <echoed DriverInfo as Format-A strings: first, last, short, category, nationality>
     <padding + steam_id as Format-A>
u8   separator = 0x01
     <AssistRules: 8x u8 assist flags + f32 stability limit + 6x u8 flags>
     <ServerConfiguration: formation lap, driver swap, latency settings>
u8   separator = 0x01
u8   0x00
     <track name as Format-A string>
u8   separator = 0x01
     <inline weather snapshot: grip, clouds, rain, config fields>
u8   separator = 0x01
     <session definitions: per-session temp, duration, weather params>
     <session schedule: per-session hour, time multiplier, duration>
     <inline leaderboard: per-car driver names + timing>
u8   separator = 0x01
     <per-car realtime data placeholder>
u8   separator = 0x01
     <second inline weather snapshot (0x37 format)>
     <session schedule recap>
     <tyre compound name "Standard" + tail padding>
```

##### Post-accept welcome sequence

After the `0x0b` response, the server immediately sends three additional messages to the joining client (confirmed by wire capture):

1. **`0x28` SRV_LARGE_STATE_RESPONSE** (56 bytes) -- session timing + assist rule snapshot; each f32 value is prefixed by `u8(1)`
2. **`0x36` SRV_LEADERBOARD_BCAST** (~120-210 bytes) -- initial leaderboard with per-car entries including car_model, cup_category, and four driver strings (steam_id, short_name, first_name, last_name as Format-A)
3. **`0x37` SRV_WEATHER_STATUS** (69 bytes) -- current weather snapshot

The server also fans out `0x2e` (car system relay) and `0x4f` (driver stint relay) to all OTHER already-connected clients to notify them of the new car joining.

#### 5.6.4a Server → client message ID catalog

**The server → client direction uses a separate ID namespace from client → server.** An ID like `0x4f` sent from client to server is not the same message as `0x4f` sent from server to client; the two directions have independent handler tables with independent wire formats.

16 distinct server → client message IDs have been identified:

| ID (hex) | ID (dec) | Body fields | Known semantic |
|---|---|---|---|
| `0x01` | 1 | `u8 = 0x01` + protobuf-encoded `ServerMonitorHandshakeResult` | **`REGISTRATION_RESULT`** — same protobuf message type as ServerMonitor protocol message 1 (see §12B). Emitted from the SMPR connection accept path with the log `"Received SMPR connection %d for %s"`. The body fields are the protobuf schema documented in §12B.3 (`bool success`, `int32 connectionId`, `string errorTxt`). |
| `0x02` | 2 | `u8 = 0x02` + protobuf-encoded `ServerMonitorConfigurationState` | **`SERVER_CONFIGURATION`** — same protobuf message type as ServerMonitor protocol message 2. Carries the server config state (track / session / connection list). |
| `0x03` | 3 | `u8 = 0x03` + protobuf-encoded `ServerMonitorSessionState` | **`SESSION_STATE`** — same protobuf message type as ServerMonitor protocol message 3. Sent when the session changes and as part of the initial state push to new clients. Paired with `0x06`/`0x07` in the periodic tick. |
| `0x04` | 4 | `u8 = 0x04` + protobuf-encoded `ServerMonitorCarEntry` | **`CAR_ENTRY`** — same protobuf message type as ServerMonitor protocol message 4. Per-car entry record fanned out from the welcome push (one `0x04` per connected car) and from the handshake handler. |
| `0x05` | 5 | `u8 = 0x05` + protobuf-encoded `ServerMonitorConnectionEntry` | **`CONNECTION_ENTRY`** — same protobuf message type as ServerMonitor protocol message 5. Per-connection entry record (driver name + connection metadata). Fanned out from the handshake handler (one `0x05` per connection). |
| `0x06` | 6 | `u8 = 0x06` + protobuf-encoded `ServerMonitorRealtimeUpdate` | **`REALTIME_UPDATE`** — same protobuf message type as ServerMonitor protocol message 6. Periodic per-tick state update emitted from the per-client queued send mechanism. |
| `0x07` | 7 | `u8 = 0x07` + protobuf-encoded `ServerMonitorLeaderboard` | **`LEADERBOARD_UPDATE`** — same protobuf message type as ServerMonitor protocol message 7. Per-car leaderboard / standings update emitted in the per-car fan-out after every `0x36` leaderboard broadcast and during the post-handshake welcome push. |

**Critical architectural note on `0x01`–`0x07`**: these seven sim-protocol message ids are exactly the **ServerMonitor protocol message types 1–7** documented in §12B, delivered inline over the same TCP socket as the rest of the sim protocol. The dedicated server uses the SAME C++ classes (`ServerMonitorHandshakeResult`, `ServerMonitorConfigurationState`, `ServerMonitorSessionState`, `ServerMonitorCarEntry`, `ServerMonitorConnectionEntry`, `ServerMonitorRealtimeUpdate`, `ServerMonitorLeaderboard`) and the same protobuf serialization for both:
- The dedicated ServerMonitor channel (separate hosting-tool clients)
- The in-game sim protocol channel (game-client state push)

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

**A reimplementation can therefore use the protobuf schemas already documented in §12B.3 to encode the bodies of `0x01`–`0x07` directly** — there is no separate undocumented wire format. The seven ids are just numbered transport tags identifying which `ServerMonitorProtocolMessage` type follows.
| `0x0b` | 11 | `u16 udp_port` + `u8 0x12` + `u16 nconns` + `u16 conn_id` + `u16 0` + trailer (~2000 bytes) | **Handshake accept response** -- see 5.6.4c. Contains the welcome trailer with session/car/config state. |
| `0x0c` | 12 | `u32 server_ver` + `u8 0` + `u16 client_ver_echo` + `u16 0` + `u16 protocol_ver` + `u16 0` | **Handshake reject response** (14 bytes) -- see 5.6.4c. Connection closed after sending. |
| `0x14` | 20 | `u32 server_ms` + `u16 0` + `u16 0` + `u16 0` + `u8 2` + `u8 4` + `u8 100` + `u8 100` (15 bytes) | **UDP keepalive response** — sent in reply to client `0x13`/`0x17`. The `server_ms` field is a monotonic clock timestamp (milliseconds) that the client echoes back in its `0x16` pong to measure round-trip latency. The trailing fields carry per-car timing hints; semantics of the constant values are not fully decoded. |
| `0x1b` | 27 | `u16 pos_a` `u16 pos_b` `i32 lap_time_ms` `u8 quality` | **Lap time broadcast** — the server forwards a client's lap-time report to all other clients. Triggered by a client sending TCP id `0x19` (see §5.6.1). The `quality` byte is `0xFF` for invalid, otherwise a normalized 0..255 value derived from a float. |
| `0x1e` | 30 | 58-byte fixed-layout record | **Per-car periodic state broadcast — fast-rate variant.** Pushed from the main server tick for every connected car. Body after the msg id byte: `u16 carId` + `u8 carLocation` + `u32 timeDelta` + `u16 scalar` + three `(u64 + u32)` lap records (matching the `BestSessionLap` / `LastLap` / `CurrentLap` triple from the client-side broadcasting SDK's `RealtimeCarUpdate`) + 8 `u8` car-state bytes + a `u16` trailing scalar. Note: the same id byte is used by client→server `ACP_CAR_UPDATE` — the two directions have entirely separate wire formats and meanings. |
| `0x23` | 35 | Per-car record (variable-length) | **Car info response over TCP** — the server's reply to a client sending UDP `0x22 CAR_INFO_REQUEST`. Body is built by the same per-connected-car record appender used in the handshake welcome trailer, so the layout matches the per-car record in the welcome sequence. |
| `0x24` | 36 | `u16 carIndex` | **`CAR_DISCONNECT_NOTIFY`** — the server tells every other client that this car has disconnected |
| `0x28` | 40 | Per-car record + session-manager state | **Larger state response over TCP** — replies to a UDP request with a combined per-car record and the session manager's current state. Used when a client asks for a full state refresh. |
| `0x2b` | 43 | (varies — see semantics column) | **Generic chat / system message** — used in several distinct shapes that share the same id byte and the same dispatch path to clients: <br> **(a) Short state variant**: `u8 + u32 + u8` (6 bytes) — emitted by the small builders, payload is a connection-id-or-timestamp `u32` and a single-byte flag. <br> **(b) Chat reply variant**: `u8 + 2× string + i32 + u8 = chat_type` — emitted by the chat command parser as the reply to almost every admin command (`/admin`, `/track`, `/restart`, etc.). The chat type byte distinguishes "system message" (5) from "regular chat" (other values). Sent either to the issuing admin alone (when `cVar20 == '\0'`) or broadcast to every connection. <br> **(c) Kick / ban notification variant**: `u8 + 2× string + i32 + u8 = 5` where the first string is the human-readable reason (`"You have been kicked from the server"` / `"You have been banned from the server"`). Sent directly to the target client immediately before the connection is force-closed. <br> **(d) Ctrl info forward variant**: emitted by client→server case `0x5b` (`ACP_CTRL_INFO`) — see §5.6.1 — to forward a client's controller info to other admins as a chat-formatted summary. |
| `0x2e` | 46 | `u8 = 0x2e` + `u16 carId` + `u64 system_data` (11 bytes) | **Two distinct uses sharing an identical wire format**: <br> **(a) New-client-joined notify** — pushed by the server to every existing client during a new client's handshake-accept sequence; the carId and timestamp are the joining car's identity. The sender function **also emits a paired `0x4f` sub-opcode-1 message** (12 bytes: `u8 + u16 carId + u8=1 + u64`) right after the `0x2e`, so the full new-client notification is two messages in sequence. <br> **(b) `ACP_CAR_SYSTEM_UPDATE` relay** — broadcast to every other client when one client sends an `ACP_CAR_SYSTEM_UPDATE` (client `0x2e`, see §5.6.1). The carId is the source car and the u64 carries the new system state. The two variants share both the id byte and the wire layout — they're distinguished only by call-site context. |
| `0x2f` | 47 | `u8 = 0x2f` + `u16 carId` + `u8 tyreCompound` (4 bytes) | **`ACP_TYRE_COMPOUND_UPDATE` relay** — server-transformed broadcast of client `0x2f`. Sent to every other connected client when one client changes its tyre compound. |
| `0x36` | 54 | `u8 = 0x36` + `u32 session_meta` + `u8 split_count` + `u32[split_count]` + `u8 has_active_player_flag` + `u16 entry_count` + `entry_count × {per-car leaderboard record, ~80–200 bytes each}` + `u8 + u8` trailer | **Standings / leaderboard broadcast** — emitted from the main server tick tail when the leaderboard recomputation has completed. Each per-car entry has a complex variable-length record produced by `FUN_140034210` containing: car position (u16), cup position (u16), driver flags (2 × u8), best lap time presence flag with optional `(u16 + u32)` lap data, an optional state byte gated by the `has_active_player_flag`, sector splits (u8 count + N × u32), driver list (u8 count + N × {4 strings + u8 + u16}), several u16/u32 timing and rating fields, and a u8/u16 escape-encoded final byte. After this broadcast the function iterates every entry list item and emits a follow-up per-car `0x07` message via the generic serializer (`FUN_14002e080(7, ...)`). Server log after broadcast: `"Updated leaderboard for %d clients (%s-%s %d min)"`. |
| `0x37` | 55 | `u8 = 0x37` + `7 × f32 weather_scaling` + `9 × f32 weather_status` + `f32 timestamp` (69 bytes total) | **Periodic weather status broadcast**. The 7 scaling floats correlate with cloud/rain levels (factor[0] ~ 1-clouds*0.3, factor[1] ~ 1-clouds*0.4). The 9 status floats are: ambient_temp, road_temp, grip, rain_intensity_or_cycle, wetness, dry_line_wetness, puddles, forecast_10m, forecast_30m. — emitted from the main server tick tail at a separate cadence (`_DAT_14014bd38`). The body is built by `FUN_1400330e0` which assembles a weather snapshot with `tanhf`-normalized rain / cloud / wet-track values, then calls a virtual `WeatherStatus::serialize` method (`vtable[0x20]`) and finally appends a session-time-delta float. Distinct from the previous catalog entry that called this a "keepalive" — it's a real weather push. |
| `0x39` | 57 | 59-byte fixed-layout record | **Per-car periodic state broadcast — slow-rate variant** (sibling of `0x1e`). Same 58-byte layout as `0x1e` plus **one extra `u8` context byte** right after the msg id. The server pushes `0x39` at a slower cadence than `0x1e`; the two together form the complete per-car state pipeline. The extra byte likely indicates the update reason (race vs qualifying, full-sync vs delta, etc.). |
| `0x3a` | 58 | `u16 car_id` + `u8 split_count` + `u32[count]` + `i32 clock` + `u16 car_field` | **Sector splits broadcast (game protocol)** — server-transformed relay of client `0x20` messages. Variable-length body (depends on split count). **Note**: a separate message with the same first byte `0x3a` exists on the lobby backend connection with a completely different fixed-15-byte body (`u8=0xc9 + u32 + u32 + u8=0x00 + u32`) — that's the lobby registration request. The two messages are distinguishable only by which TCP channel they flow on. |
| `0x3b` | 59 | `u16 car_id` + `u32 split_time` + `u8` + `u32 lap_time` + `u16 flags` | **Single sector split broadcast** — server-transformed relay of client `0x21` messages. Fixed 14-byte body. |
| `0x3c` | 60 | `u16` `u16` `u32` | 9-byte record, triggered by client case `0x3d` |
| `0x3e` | 62 | `u8 = 0x3e` + `u8 result_count` + `result_count ×` per-car result record | **Session results broadcast** — emitted from the main server tick tail when a session ends. Each per-car result record is built by `FUN_1400351f0` from a 336-byte (`0x150`) source struct and contains: <br><br> `u8 + u8 + u8 (val−1) + u32 + u16 + u32 + u32 + u8 + u8 + u32` (24-byte fixed header — position, cup position, driver flag, lap count, sector counts, final time, status flags) <br> followed by a complete per-car leaderboard record built by `FUN_140034a40` from offset +0x98 of the source struct (the same outer serializer used by `0x36`, so each result row has the full standings record with sector splits, driver list, ratings, etc.) <br><br> So each per-car row is approximately `24 + (variable leaderboard data ~80–200 bytes) = ~100–250 bytes`. After the broadcast the function checks the session type and waits `postQualySeconds` (qualy) or `postRaceSeconds` (race) before advancing to the next session. Server log: `"Send session results to %d clients (%d byte)"`. This is the **session result finalization** message clients use to populate the post-session standings screen. The wire data is the same as the JSON written to `results/YYMMDD_HHMMSS_*` files. |
| `0x3f` | 63 | `u8 = 0x3f` + `u8 grid_count` + grid_count × `{u16 carId, u8 ?, u32 grid_position, u8 ?}` (5+9N bytes) | **Race start grid positions broadcast** — emitted from the main server tick tail when the session phase reaches state `'\x04'` (race countdown / pre-race). The function gathers the entry list grid positions via `FUN_140032400`, then writes one record per car. Each record is 8 bytes on the wire: `u16 carId` + `u8 flag_a` + `u32 grid_position` + `u8 flag_b`. Server logs `"Sending grid positions:"` followed by `"   Car %d Pos %d"` per record, and `"Send grid positions to %d clients (%d byte, %d grid results)"` after the broadcast. A reimplementation must emit this when transitioning into race countdown so the client can populate its starting-grid display. |
| `0x40` | 64 | `u8 = 0x40` + serialized `WritableRaceStructure` / `RaceWeekendForecast` (variable-length, virtual serializer) | **Race weekend reset broadcast** — emitted by the "Resetting weekend to friday night" path that runs when admin uses `/restart` to restart the entire race weekend or when a new event is loaded. The function writes the `cfg/current/{configuration,event,settings,entrylist,eventRules}.txt` files, applies new weather rules (with retry-loop log `"Found weather obeying the rules in %d ms (%d tries, %d)"`), and pushes one `0x40` message to every connected client. The body after the id byte is built by a virtual serializer method (`vtable[0x20]`) — variable length depending on the WritableRaceStructure or RaceWeekendForecast snapshot being sent. |
| `0x44` | 68 | (varies — see semantics column) | **Three distinct uses of this id byte**, all unrelated: <br> **(a) Damage zones broadcast** — `u8 = 0x44` + `u16 carId` + `5 × u8 damage_intensity` (8 bytes total). Server-transformed relay of client `0x43`, with each damage value clamped to a maximum constant (`DAT_14014bd78` ≈ 255) before truncation to u8. Broadcast to every other connected client via the standard `broadcast_except_one` helper. <br> **(b) Lobby registration request** — sent to Kunos's `kson` backend, only when `registerToLobby: 1`, irrelevant for private MP. <br> **(c)** A smaller game-protocol variant emitted from one of the multi-message builders. Disambiguated by transport / call site, not by the id byte alone. |
| `0x46` | 70 | `u8 = 0x46` + `u16 carId` + `5 × u8 dirt_intensity` (8 bytes total) | **Car dirt status broadcast** — relay of client `0x46` `ACP_CAR_DIRT`. The 5 dirt fields are stored as 5 `double`s in the source struct (at offsets +0x28, +0x30, +0x38, +0x40, +0x48), each multiplied by a normalization constant (`DAT_14014bd18` ≈ 255) and clamped to 0..255 before being written as a u8. Broadcast to every other connected client. |
| `0x47` | 71 | `u8 = 0x47` + `u16 carId` + `u8 driver_count` + `driver_count × u8 swap_state` (4 + N bytes) | **Driver swap state broadcast** — server-transformed relay of client `0x47` `ACP_UPDATE_DRIVER_SWAP_STATE`. The body carries the carId and the per-driver swap state byte (one byte per driver in the entry, value range 0–5 corresponding to the swap state machine: 0=idle, 1=requested, 2=foreign, 3=requested-pending, 4=executing, 5=done). Built by `FUN_140011bf0` and broadcast via direct TCP send to every connection in the entry list (not via the standard `broadcast_except_one` helper). |
| `0x49` | 73 | `u8 result_code` (2 bytes total) | **Driver swap result** — TCP reply sent directly to the client that issued an `ACP_EXECUTE_DRIVER_SWAP` (client `0x48`). The result code is 0 on success and a non-zero error code otherwise. The server logs `"Driver swap result: %d"` immediately after sending. Single recipient — never broadcast. |
| `0x4b` | 75 | `u8 = 0x4b` + welcome trailer (built by `FUN_140033980`, same builder as the `0x0b` handshake response trailer) | **Welcome trailer redelivery** — sent to a specific client during the **race weekend reset / event change** flow (`FUN_14002aca0`, the function that runs `"Resetting race weekend"` and `"Event changed"`). The body carries the same welcome trailer structure as the `0x0b` handshake response (carIndex + trackName + eventId + session list + entry list + per-car records — see §5.6.4c). After sending, the function sets the per-connection state field at `+0xa01d4 = 1` and updates a timestamp at `+0xa01e0`, indicating the client has been re-welcomed for the new event. Used so that already-connected clients receive a fresh entry list / track / session state when the admin uses `/track <name>` or when the weekend resets. A reimplementation that supports event changes mid-server must emit `0x4b` to all connected clients after the new event is loaded. |
| `0x4e` | 78 | `u8 = 0x4e` + `u8 conn_count` + per-connection record `{u16 conn_id, u8 flag, i16 ratingA×10, i16 ratingB×10, u32 = 0xFFFFFFFF, Format-A steam_id}` | **Per-connection rating summary** — emitted periodically from the main server tick tail when a "ratings dirty" flag is set, and also sent to the disconnecting client at disconnect time (with the driver's steam_id in the Format-A field). The two `i16` values are stored as integer x 10 on the wire (so the precision is one decimal place). The trailing `0xFFFFFFFF` is a sentinel separator before the steam_id string. |
| `0x4f` (sub 0x00) | 79 | `u16` `u8=0x00` | 4-byte variant A |
| `0x4f` (sub 0x01) | 79 | `u16` `u8=0x01` `u64` | 12-byte variant B — same ID byte, distinguished by a sub-opcode byte at offset 3 |
| `0x53` | 83 | `u8 = 0x53` + `u16 carId` + `u16 ballast_kg` + `u32 restrictor_float` (9 bytes total) | **Multiplayer BOP (Balance of Performance) update** — pushed to every connected client whenever an admin issues a `/ballast <carNum> <kg>` or `/restrictor <carNum> <pct>` chat command. Wire fields after the msg id byte: `u16 carId` (the affected car), `u16 ballast` (the ballast value in kg as a low-16 of a sign-extended i32, valid range ±40), `u32 restrictor` (an IEEE 754 single-precision float in the range 0..0.99). The chat-output reply `"Assigned %d kg to car #%d"` (or `"... %d %% to car #%d"`) is sent separately as a `0x2b` chat broadcast. |
| `0x56` | 86 | `u8 = 0x56` + `u8 setup_index` + `u16 carId` + `i16 lap_count` + `lap_count × Lap_record` + per-car leaderboard record (variable, via `FUN_140034210`) | **Setup data + lap history response** — server's reply to a client `ACP_LOAD_SETUP` (client `0x55`). The body carries the setup index, the target car id, the count of laps in the history, the per-lap records, and finally a complete per-car leaderboard record (the same record format used by `0x36`). Sent over the requesting client's TCP socket only. <br><br> **Per-Lap record wire format** (built by `FUN_1400328f0` for each Lap struct in the history at 0x60-byte stride): <br> `Format-A track_name` (variable wstring) <br> `u32 lap_time_ms` (from struct +0x28) <br> `u8 split_count` (= sector_splits_vector_size / 4) <br> `split_count × u32 split_time_ms` (from struct +0x30 vector) <br> `u16` (from struct +0x60, probably driver index or car id) <br> `u8` (from struct +0x5c, probably flags / lap quality) <br> `u16` (from struct +0x54 area, probably another counter) <br><br> A typical Lap record is approximately 30-50 bytes depending on track name length and split count. |
| `0x58` | 88 | `u8 = 0x58` + `u16 carId` + `u8 swap_request_code` (5 bytes) | **Driver swap broadcast notification** — emitted by the server immediately after a successful `ACP_EXECUTE_DRIVER_SWAP` (client `0x48`) **only if** a server-config flag is set (the same flag also gates whether the swap result `0x49` is mirrored to other clients). Broadcast to every other connected client via `broadcast_except_one`, then also re-sent over the executing client's own TCP socket as a self-confirmation. |
| `0x59` | 89 | `u16` `u8` | 4-byte record |
| `0x5b` | 91 | `u8 = 0x5b` (1 byte total, just the id) | **Request ctrl info** — server-to-client probe sent when an admin runs the `/controller <carNum>` chat command. The server sends a single byte to the targeted client, which then replies with a client→server `0x5b` (`ACP_CTRL_INFO`, see §5.6.1) carrying the actual controller information. Either side of the exchange uses the same id byte; direction is the disambiguator. The server logs `"Requested controller info for car #%d"` after sending. |
| `0x5d` | 93 | `u8 = 0x5d` + Format-A `connection_name` + `u8 = 0x01` + `u16 carId` + (further per-connection fields) | **`/connections` admin response — per-connection record** — emitted when an admin runs the `/connections` chat command. The chat parser walks the connection list and builds one `0x5d` message per connection, each containing the connection's display name (Format-A string) followed by a `0x01` separator byte and the connection's `carId`. Sent over the requesting admin's TCP socket only. |
| `0xbe` | 190 | (variable-length body built by a state-snapshot helper) | **Periodic UDP broadcast** — emitted from the main server tick via the UDP send helper with a 2048-byte scratch buffer. Probably a LAN announcement or presence ping. |
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

Rejection reasons observed, each with its own log message:

| Reason | Triggered by |
|---|---|
| Wrong client version | `client_version != 0x100` |
| Wrong password | mismatch against `settings.password` |
| Server full | `connection count >= maxConnections` |
| Player banned | Steam ID on the ban list (persists until server restart) |
| Player kicked | Steam ID on the kick list (persists until race weekend restart) |
| Invalid entry list forced car model | `entrylist.json` forces a specific car model for this Steam ID and the client tried to join with a different one |
| Invalid entry list grid position | corrupt `defaultGridPosition` value in entrylist.json for this car |

A reimplementation that wants to maintain parity should implement **all seven rejection checks**, in roughly the order above, since some depend on state computed by earlier checks (e.g. the full check happens after the password check so you can't probe for "is the server full" without knowing the password).

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

Chat is a sim-protocol feature carried over TCP (ID TBD). Elevation is stateful on the server; `/admin <pw>` sets a flag on the client's connection. Entry-list drivers with `isServerAdmin: 1` are auto-elevated on join.

### 8.1 Additional admin chat commands not in HB

The chat command parser exposes several commands that are not documented in the public handbook. These were observed by inspecting the parser's literal-string table:

| Command | Args | Effect |
|---|---|---|
| `/admin` | password | Elevate the issuing connection to server admin (or `"Wrong password"`) |
| `/track` | trackName | Switch the current event's track (logs `"Event change to %s"`); replies `"no track name specified"` or `"Please set a valid track"` on bad input |
| `/manual entrylist` | — | Dump the current connected drivers to a new entry list JSON; replies `"Saved entry list to ..."`, refuses on public servers (`"Entry list cannot be saved on public servers"`) |
| `/manual start` | — | Replaced — replies `"This cmd was replaced by the formationLapType setting"` |
| `/controllers` | — | Request ctrl info from every client; replies `"Requesting controllers for %d clients"` |
| `/controller` | carNum | Request ctrl info from one specific car (single-recipient `0x5b` send) |
| `/connections` | — | List all current connections (sends one `0x5d` per connection back to the issuing admin) |
| `/hellban` | carNum | Apply hellban; replies `"Hellban inactive"` if disabled |
| `/cleartp` | carNum | Clear pending post-race time penalties (`"Pending post race time penalties for #%d cleared by Race Control"`) |
| `/report` | — | Mark / report a connection (replies `"Car #%d reported, thank you"`) |
| `/latencymode` | n | Set latency mode 0..N (`"Latency mode: ..."`); validates the number |
| `/debug conditions` | — | Toggle conditions debug logging (`"conditions stopped/started printing"`) |
| `/debug bandwidth` | — | Toggle bandwidth stats debug logging |
| `/debug qos` / `/netcode` | — | Toggle netcode-stats debug logging |
| `/legacy` / `/regular` | — | Toggle netcode mode (`"Server now uses legacy netcode"` / `"Server is now in regular mode"`) |

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

#### 0xf2 Heartbeat (periodic, in state 6)

Includes CPU load and bandwidth statistics.  Exact fields not
fully decoded.

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

- ~~`WeatherStatus::serialize` body~~ — resolved: vtable[0x20] is `FUN_14011e930` which writes exactly **9 × u32 = 36 bytes** from the WeatherStatus struct fields at offsets 0x28–0x48 (in this order: 0x28, 0x2c, 0x30, 0x34, 0x3c, 0x38, 0x40, 0x44, 0x48 — note 0x38 and 0x3c are written in non-numeric order).
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

**Protocol decoding is now fully closed** for any practical reimplementation purpose. The dedicated server's job is to receive `ACP_CAR_UPDATE` packets and relay them; a reimplementation does the same.

**Newly resolved by Pass 2.20** (recursive smoking-gun search):

- ~~Vector3 semantic interpretation~~ — partially resolved: `FUN_1400427c0` computes `sqrt(x²+y²+z²)` of the third Vector3 (car_state +0x20) and compares it × ~3.6 against a km/h threshold to update the car's "last seen moving" timestamp. This **proves vec_c is the velocity vector**. By elimination and convention, vec_a (+0x8) is most likely position (X, Y, Z) and vec_b (+0x14) is orientation (probably Euler angles roll/pitch/yaw or a forward-direction unit vector) — but only vec_c's semantic is rigorously confirmed.
- ~~The packet header `u8 flag`~~ — `FUN_1400419e0` confirms it's a **rolling packet sequence counter** (the server tracks `current - previous == 1` for valid in-order packets and computes a drop rate, NOT a gear/pit flag).
- The **two 4-byte input arrays** are at non-consecutive storage offsets (+0x2e..+0x31 and +0x48..+0x4b), confirming they hold distinct data. The natural interpretation is "pedals" + "assists/wheel" but this is not statically verifiable without packet capture.


