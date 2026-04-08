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

- **Kunos lobby backend ("kson")**: HTTPS/TCP to Kunos infrastructure when `registerToLobby: 1`. Transmits server config and session state updates. `LOG` lines 4–5 show `"Sent Lobby Registration Request with trackName hungaroring"` → `"RegisterToLobby succeeded"` → `"Sent config to kson"`, and session phase changes emit `"Sent new session state to kson"`. **A reimplementation must set `registerToLobby: 0` and must not attempt to impersonate this endpoint.**
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
| `0x19` | 25 | **Client lap-time report** — body is `u16` `u16` `i32` `u8` (cup position, track position, lap time in ms, and a signed quality byte). The server validates the fields, updates the reporting connection's rating state, then broadcasts a transformed `0x1b` message to every other client (see §5.6.2 row for `0x1b`). This message uses the tier-2 queued-lambda broadcast mechanism, not the direct relay. |
| `0x20` | 32 | **Sector splits (bulk)** — client reports multiple sector split times at once. Body: `i32` + `u8` + `i32` + `u16` header, then the server broadcasts a transformed `0x3a` message to all other clients carrying the full split list (`u16 car_id` + `u8 split_count` + `u32[count]` + `i32 clock` + `u16 car_field`). |
| `0x21` | 33 | **Sector split (single)** — client reports a single sector split. Body: `i32` + `i32` + `u8` + `u16` + `u8` header. The server broadcasts a transformed `0x3b` message (fixed-length: `u16` + `u32` + `u8` + `u32` + `u16`) to all other clients. |
| `0x2a` | 42 | (body TBD) |
| `0x2e` | 46 | (body TBD) |
| `0x2f` | 47 | (body TBD) |
| `0x32` | 50 | **`ACP_CAR_LOCATION_UPDATE`** — body is `u16 carIndex` + `u8 carLocation` (5-value enum: NONE/Track/Pitlane/PitEntry/PitExit, see §7.9). Historical ACP name is still current. |
| `0x3d` | 61 | **Event report** — body is `u8` + `i32` (2 fields). The server broadcasts a transformed server `0x3c` message (body `u16` + `u16` + `u32`, 9 bytes) to all other clients. |
| `0x41` | 65 | `u8` + `u8` + `u64` + `i32` (4 fields; u64 likely timestamp) |
| `0x42` | 66 | Single `u64`. **Probable clock synchronization** — client sends its timestamp, server uses it to compute offsets. |
| `0x43` | 67 | (body delegated, not directly read) |
| `0x45` | 69 | Five `u8` fields (likely flag/enum bitmap) |
| `0x47` | 71 | (body delegated; server sends a response immediately) |
| `0x48` | 72 | `u16` + `u8` (distinct from the UDP `0x48` LAN discovery below; server sends a response) |
| `0x4a` | 74 | `u16` + `u8` + `u8` (server sends a response) |
| `0x4f` | 79 | **Event report with sub-variants** — body is `u8` + `u64`. The server broadcasts a transformed server `0x4f` message with a sub-opcode byte at body offset 3: sub `0x00` for the 4-byte variant (u16 + u8=0x00), sub `0x01` for the 12-byte variant (u16 + u8=0x01 + u64). |
| `0x51` | 81 | Two `u16` fields (probably a small command, e.g. "focus car X, camera Y") |
| `0x54` | 84 | Single `u16` (car index?) |
| `0x55` | 85 | `u8` + `u16` + `i32` followed by a full embedded **CarInfo** (via `CarInfo::readFromPacket`). Carries the client's updated car state. |
| `0x5b` | 91 | (body delegated, not directly read) |

#### 5.6.2 UDP message IDs

Messages carried over the unreliable UDP channel on the main `udpPort`. 7 distinct IDs. These are handled by a chain of inline `if` blocks in the server rather than a central switch:

| ID (hex) | ID (dec) | Name / meaning |
|---|---|---|
| `0x13` | 19 | **Silent keepalive** — the server receives these packets but does nothing with them; they skip both the per-id statistics tracking and the handler dispatch chain. A reimplementation should accept and silently drop them. |
| `0x16` | 22 | **`PONG_PHYSICS`** — physics-side clock-sync ping/pong used to measure per-client latency and adjust simulation timestamps. Body: `u16 conn_id` + `u16 ?` + `i32 ts1` + `i32 ts2`. |
| `0x17` | 23 | **Silent keepalive** — same treatment as `0x13`. Both ids are excluded from bandwidth/qos statistics and have no server-side handler. |
| `0x1e` | 30 | **`ACP_CAR_UPDATE`** — the per-tick car state update, sent by each client at simulation tick rate. Body layout: `u16 source_conn_id` + `u16 target_car_id` + `u8 flag` + `u32 value` (9-byte header) followed by 3 × `Vector3<float>` (36 bytes for position, velocity, and either angular velocity or forward direction), then a 4-byte input array (throttle / brake / clutch / handbrake or similar), several scalar state bytes, a `u16`, a 4 × `u32` array (sector / lap timing data), and more scalars. Total body ~60–80 bytes. Server validates that the source connection owns the target car and rejects the update otherwise. Wire order is **not** the same as the corresponding C++ struct field order — a reimplementation must preserve the observed wire sequence, not derive it from any public struct definition. |
| `0x22` | 34 | **`CAR_INFO_REQUEST`** — body is `u16 connectionId` + `u16 carIndex`. The server replies with a full `CarInfo` structure for the requested car. Historical ACP name is still current. |
| `0x5e` | 94 | Four-field record: `u16` + `u16` + `u64` + `u8` (possibly a time-synchronized event) |
| `0x5f` | 95 | **Admin / server-identity query** — the client sends a Format-B string (typically an identifier or admin password). If the string matches the server's configured identifier, the server replies on UDP with another Format-A string containing the server's name / identification. Used by admin tooling to verify server identity over UDP without establishing a full TCP session. A reimplementation targeting private MP can ignore these messages. |

#### 5.6.3 LAN discovery (UDP 8999)

One message ID on the fixed LAN discovery port:

| ID (hex) | ID (dec) | Direction | Meaning |
|---|---|---|---|
| `0x48` | 72 | client → server | LAN discovery probe; server responds with a brief info packet |

**Note the namespace overlap**: `0x48` is used on both the LAN discovery port and the main TCP channel, but with different semantics. A message is disambiguated by the transport / destination port, not just by the ID byte.

#### 5.6.4c Handshake response (message id `0x0b`)

After the server has processed a client's handshake request, it sends back a response message with **id `0x0b`**, regardless of whether the handshake was accepted or rejected. The same message id is used for both outcomes; the body distinguishes them.

Header (first 6 bytes of the body):

```
u8  msg_id = 0x0b
u16 protocol_version    (the server echoes its own version back — 0x100 for build 1.10.2)
u8  server_flags        (a configuration byte from server state; exact layout TBD)
u16 connection_id       (the server-assigned id for this client; 0xFFFF on rejection)
... followed by a larger trailer that carries the initial entry list + session state on accept
```

On rejection the connection_id is set to a sentinel (`0xFFFF`) and the trailer may be empty. On accept, a substantial trailer follows that enumerates the current connected-car list and session configuration.

**Trailer structure** (observed field-by-field sequence):

```
u32 carId                          (the assigned car, or 0xFFFFFFFF on reject)
string trackName                   (Format A, matches event.json "track")
string eventId                     (Format A)
u8  = 1                            (separator / version byte)
u8  session_count                  (number of configured sessions)
repeated {per-session record}      (session_count × variable-length records)
u8  = 1                            (separator)
<SeasonEntity record>              (appended via a virtual serializer)
u8  = 1                            (separator)
<SessionManager state record>      (appended via a sub-builder)
u8  = 1                            (separator)
<AssistRules record>               (appended via another sub-builder)
u8  = 1                            (separator)
<ServerConfiguration snapshot>     (appended via another virtual)
u8  = 1                            (separator)
<additional state>                 (appended via a final sub-builder)
u8  = 1                            (separator)
u8  connected_car_count
repeated {per-connected-car record}    (typically 20+ bytes per car)
    u8  field_a       (from car+? offset)
    u8  field_b
    u8  field_c       (adjusted by -1 on write)
    u32 field_d
    u16 field_e
    u32 field_f
    u32 field_g
    u8  field_h
    u8  field_i
    ... several more fields per car ...
... trailing state scalars ...
```

The sub-records enumerated above are serialized by dedicated per-type builders. Partial structural information is known for each:

- **Session manager state**: one leading flag byte followed by **7 session records spaced 56 bytes apart in the server struct** (matches the max session count in a race weekend: FP1/FP2/FP3/Q/Q2/R/R2), followed by a tail record. Each session record is serialized by a dedicated per-session appender whose exact body is not yet decoded.
- **Assist rules**: built in a two-step pattern — a temporary snapshot is assembled from the server's runtime state (applying overrides from `assistRules.json`), then that snapshot is serialized into the outgoing packet with ~7 fields (matching the handbook's assist controls: stability control level, auto-steer, auto-lights, auto-wiper, auto-engine-start, auto-pit-limiter, auto-gear, auto-clutch, ideal-line).
- **Additional state**: conditional — several server-side flags control whether this record is emitted at all. When present, it contains runtime state that is only meaningful after a session has actually started (session phase, active rules, etc.). A minimal reimplementation can send an empty placeholder here.

A reimplementation aiming for wire-level compatibility with the accept path will need to decode the per-session appender and the assist-rules serializer (each is a few hundred bytes of decompilation). For phase-1 reject-only operation, neither is needed.

**Practical recommendations**:

1. **Minimum viable rejection**: send the 6-byte header alone with `connection_id = 0xFFFF` and no trailer. The client should disconnect cleanly.
2. **Minimum viable accept**: build a 6-byte header with `connection_id = <some value>`, followed by the carId + trackName + eventId + a session count of 0 + separators + an empty car list + trailing zeros. This may or may not be accepted by the client depending on how strictly it validates the sub-records — to be determined by testing.
3. **Full fidelity**: requires decoding each of the four sub-builders, each of which is probably 500–2000 bytes of decompilation. Pass 2.10 did not attempt this.

#### 5.6.4a Server → client message ID catalog

**The server → client direction uses a separate ID namespace from client → server.** An ID like `0x4f` sent from client to server is not the same message as `0x4f` sent from server to client; the two directions have independent handler tables with independent wire formats.

16 distinct server → client message IDs have been identified:

| ID (hex) | ID (dec) | Body fields | Known semantic |
|---|---|---|---|
| `0x01` | 1 | (generic serializer + object body) | **Rejection / invalid-payload notification** — sent from the client disconnect-cleanup path. Log strings in the caller include `"Invalid payload (%d)"` and `"File too big, can't parse safely"`. |
| `0x02` | 2 | (generic serializer + object body) | **Generic state snapshot push** — used by a state-push helper function called from various other flows. |
| `0x03` | 3 | (generic serializer + object body) | **Track / session change notification** — sent when the track or session changes and as part of the initial state push to new clients. Paired with `0x07` in the state push sequence. |
| `0x04` | 4 | (inline-built record, ~14 bytes) | **Handshake state A** — part of the welcome sequence following a successful handshake. The enclosing builder also sends `0x03` and `0x07`. |
| `0x05` | 5 | (generic serializer + object body) | **Handshake state B** — sent alongside `0x04` during the welcome sequence. Also emitted by the driver-swap forwarder. |
| `0x06` | 6 | (generic serializer + object body) | **Per-client queued state update** — generic state push used by the per-client send mechanism. |
| `0x07` | 7 | (generic serializer + object body) | **Session running state** — paired with `0x03` in both the initial state push and the main server tick's periodic broadcasts. |
| `0x0b` | 11 | `u16 version` + `u8 flags` + `u16 conn_id` + trailer | **Handshake response** — see §5.6.4c. Used for both accept and reject outcomes. |
| `0x0c` | 12 | `u8` + `u32` + `u32` + `u32` | 14-byte state record |
| `0x14` | 20 | (1-byte body, just the id) | Silent keepalive / ack |
| `0x1b` | 27 | `u16 pos_a` `u16 pos_b` `i32 lap_time_ms` `u8 quality` | **Lap time broadcast** — the server forwards a client's lap-time report to all other clients. Triggered by a client sending TCP id `0x19` (see §5.6.1). The `quality` byte is `0xFF` for invalid, otherwise a normalized 0..255 value derived from a float. |
| `0x1e` | 30 | `u16 car_id` + `u8 car_location` + `u32 timing` + `u16` + `u64 timestamp` + `u32` + ... (~21 fields total) | **Per-car periodic state broadcast** — pushed from the main server tick at the "fast" update rate. Carries compact per-car state (position class, lap timing, split info). Note: the same id byte is used by client→server `ACP_CAR_UPDATE` — the two directions have entirely separate wire formats and meanings. |
| `0x23` | 35 | Per-car record (variable-length) | **Car info response over TCP** — the server's reply to a client sending UDP `0x22 CAR_INFO_REQUEST`. Body is built by the same per-connected-car record appender used in the handshake welcome trailer, so the layout matches the per-car record in the welcome sequence. |
| `0x24` | 36 | `u16 carIndex` | **`CAR_DISCONNECT_NOTIFY`** — the server tells every other client that this car has disconnected |
| `0x28` | 40 | Per-car record + session-manager state | **Larger state response over TCP** — replies to a UDP request with a combined per-car record and the session manager's current state. Used when a client asks for a full state refresh. |
| `0x2b` | 43 | `u32` `u8` | 6-byte record; emitted from two distinct call sites plus from the session-transition multi-message builders |
| `0x2e` | 46 | `u16` `u64` | **New-client-joined notify** — 11-byte record with a u64 timestamp. Pushed by the server to every existing client during a new client's handshake-accept sequence (called from the handshake handler with each other client's TCP socket as the target). A reimplementation joining a second client will need to emit this to the first client at the right moment. |
| `0x36` | 54 | (1-byte body) | Keepalive / tick broadcast — emitted from the main server tick tail alongside `0x37` and `0x3e` as a periodic heartbeat burst. |
| `0x37` | 55 | (body is empty — just the id byte) | Keepalive — emitted with `0x36` and `0x3e` from the main server tick. |
| `0x39` | 57 | `u8` + `u16 car_id` + `u8 car_location` + `u32` + `u16` + `u64` + `u32` + ... (~22 fields total) | **Per-car periodic state broadcast** (sibling of `0x1e`) — pushed from the main server tick at a different cadence than `0x1e`. Wire format is very similar; the two together comprise the server's tier-2 state push pipeline. Likely one is the "fast" update (position/lap) and the other is the "slow" update (rating/history). |
| `0x3a` | 58 | `u16 car_id` + `u8 split_count` + `u32[count]` + `i32 clock` + `u16 car_field` | **Sector splits broadcast (game protocol)** — server-transformed relay of client `0x20` messages. Variable-length body (depends on split count). **Note**: a separate message with the same first byte `0x3a` exists on the lobby backend connection with a completely different fixed-15-byte body (`u8=0xc9 + u32 + u32 + u8=0x00 + u32`) — that's the lobby registration request. The two messages are distinguishable only by which TCP channel they flow on. |
| `0x3b` | 59 | `u16 car_id` + `u32 split_time` + `u8` + `u32 lap_time` + `u16 flags` | **Single sector split broadcast** — server-transformed relay of client `0x21` messages. Fixed 14-byte body. |
| `0x3c` | 60 | `u16` `u16` `u32` | 9-byte record, triggered by client case `0x3d` |
| `0x3e` | 62 | (body is empty — just the id byte) | Probable keepalive |
| `0x40` | 64 | (body TBD) | Multi-message builder — the enclosing function contains two `vec_init` calls and writes at least `0x40` as a first byte. Body details need further decoding. |
| `0x44` | 68 | *not part of game-client protocol* | Lobby registration request to Kunos's `kson` backend — only sent when `registerToLobby: 1`, irrelevant for private MP |
| `0x47` | 71 | `u16` `u8` `u8` | 5-byte record |
| `0x4f` (sub 0x00) | 79 | `u16` `u8=0x00` | 4-byte variant A |
| `0x4f` (sub 0x01) | 79 | `u16` `u8=0x01` `u64` | 12-byte variant B — same ID byte, distinguished by a sub-opcode byte at offset 3 |
| `0x53` | 83 | (body TBD) | Emitted from at least **two** distinct builders: the driver-swap forwarder and the larger session-transition multi-message builder. Likely two unrelated uses of the same id byte, distinguished by context. |
| `0x59` | 89 | `u16` `u8` | 4-byte record |
| `0x5b` | 91 | (body TBD) | **Note**: client → server TCP also has a `0x5b` message with a different meaning. This server → client `0x5b` is one of the three ids emitted by the session-transition multi-message builder and is likely a session-state notification. |
| `0x5d` | 93 | `u8 u8` (2 bytes) | Emitted by the session-transition multi-message builder alongside `0x5b` and `0x2b`. |
| `0xbe` | 190 | (variable-length body built by a state-snapshot helper) | **Periodic UDP broadcast** — emitted from the main server tick via the UDP send helper with a 2048-byte scratch buffer. Probably a LAN announcement or presence ping. |
| `0xc0` | 192 | `u8 0xc0` + server info record + `u8 car_count` + per-car summary | **LAN discovery response** — the server's reply to a client `0x48` probe on UDP 8999. Contains the server name, capacity, connected-car count and per-car summary info. A reimplementation must emit this to be visible on the client's "LAN servers" list. |

A comprehensive sweep has found **23 distinct server → client message IDs**. This list should be ≥90% complete for messages that use the standard `build-and-send` pattern. Messages built by generic serializers (where the msg id is a parameter, not a literal) or by virtual methods that don't initialize their own output vector may still be missing — most likely one or two additional ids at most.

#### 5.6.4d Post-handshake welcome sequence

When a client successfully handshakes, the server emits **multiple messages in sequence** before the client is considered fully joined. A reimplementation that wants to be wire-compatible on the accept path must emit all of these, not just the handshake response:

```
To the joining client:
  1. 0x0b  — handshake response (car id, protocol version,
             connection id, session/track trailer)
  2. 0x04  — handshake state A (inline-built state record)
  3. 0x05  — handshake state B (generic serializer)
  4. 0x03  — track / session change notification
             (generic serializer)
  5. 0x07  — session running state
             (generic serializer)

To every OTHER currently connected client:
  6. 0x2e  — new-client-joined notify (u16 + u64 timestamp)
```

Messages 3 and 4 (ids `0x03` and `0x07`) are the same as the ones emitted periodically by the main server tick, so if the server has a unified "push current state" function, it can be reused here. Messages 1 and 2 are handshake-specific. Message 6 is fan-out to existing clients.

A reimplementation that only sends `0x0b` and stops will likely see the client disconnect after a timeout, because the client is waiting for the rest of the welcome sequence before proceeding to the session view.

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

### 5.7 Known message IDs (historical, provisional)

The `server.log` file shipped in the broadcasting SDK distribution (`LOG`) is a debug capture from an earlier build of the ACC server and contains named message IDs with their integer values. These names **may or may not still be in use** in the current target build (1.10.2); they are transcribed here as a starting point for correlating observed traffic with protocol semantics. The current build's debug logging was refactored and these names are no longer emitted.

### 5.1 TCP messages (client → server)

| ID | Name | Seen in LOG context |
|---|---|---|
| 9 | `ACP_REQUEST_CONNECTION` | First packet on a new TCP connection |
| 33 | *(unnamed in log)* | Periodic during car state streaming |
| 34 | `CAR_INFO_REQUEST` | Client asks about another car's info; server replies with "Car Info Response sent idRequested: N, carIndex: K, driverIndex: J" |
| 41 | *(unnamed)* | Appears once during connection setup between ID 44 and ID 45 |
| 44 | *(unnamed)* | Appears twice during connection setup after first UDP packet |
| 45 | *(unnamed)* | Appears once during connection setup after ID 41 |
| 48 | *(unnamed)* | Appears once during connection setup before ID 49 |
| 49 | `ACP_RACE_MANAGER_INFO_COMPLETED` | Client signals it is ready to receive packets |
| 50 | `ACP_CAR_LOCATION_UPDATE` | Frequent; carries car pit/track location enum (see §6.3) |

IDs not yet mapped to names: 33, 41, 44, 45, 48.

### 5.2 UDP packets

| ID | Name | Notes |
|---|---|---|
| 19 | *(unnamed)* | First UDP packet emitted by client after ACP_REQUEST_CONNECTION |

### 5.3 Observed handshake sequence

From `LOG` lines 26-33 and 58-73, a complete client join looks like:

```
Server:  New connection received <socketFd>
Client→: TCP ID 9 (ACP_REQUEST_CONNECTION)   — includes CLIENT VERSION: 212
Server:  addNewConnectedCar
Server:  Found and added a new player
Server:  New Connection created, new connectedCarId: N new connectionId: N
Server→: Sent connected drivers list to kson   (backend only)
Client→: UDP ID 19
Client→: TCP ID 44
Client→: TCP ID 44
Client→: TCP ID 41
Client→: TCP ID 45
Client→: TCP ID 48
Client→: TCP ID 49 (ACP_RACE_MANAGER_INFO_COMPLETED)
Client←: (server sends entry list?)
Client→: TCP ID 34 (CAR_INFO_REQUEST) for each other car
Server→: Car Info Response (unknown ID) with idRequested, carIndex, driverIndex
Client→: TCP ID 50 (ACP_CAR_LOCATION_UPDATE) every time car crosses pit boundary
Client→: TCP ID 33 (unknown) periodic
```

The `CLIENT VERSION: 212` in the log does not match the `Server Version 212` at the top, so version 212 is a **protocol version** from an earlier ACC build, not the Steam build number. The shipped log is from a historical state and the current target build (`14255706`) may have a different protocol version number. A reimplementation should read the version from the client's first `ACP_REQUEST_CONNECTION` and either accept or reject.

### 5.4 Open questions for the sim protocol

Not answered by any public source; to be resolved in Notebook A:

1. **Framing on TCP**: length prefix? `uint16` / `uint32`? Or fixed-width headers?
2. **Complete ID table**: how many ACP IDs exist; what are 33, 41, 44, 45, 48 named; are there ID ranges for subsystems (e.g. all session-related IDs in 0..20)?
3. **Authentication**: does `ACP_REQUEST_CONNECTION` carry the server password, spectator password, admin password, Steam ID, rating values? In what order?
4. **UDP binding**: does the server bind a per-client UDP port, or does every client share `udpPort`? Does the client "claim" its slot over TCP first?
5. **Car state frame rate**: how often does the client push its own position? How often does the server echo other cars' positions?
6. **Chat channel**: is chat on TCP only? Does admin `/elevation` use a separate ID or the same chat ID with a magic prefix?
7. **Entry list push**: how does the server deliver the 24-car entry list after `ACP_RACE_MANAGER_INFO_COMPLETED`? One message with the whole list, or streamed?
8. **Phase change push**: which ID delivers session phase transitions to clients?
9. **Disconnection semantics**: clean disconnect ID? What does `ignorePrematureDisconnects` actually gate?
10. **Weather/track update push**: periodic or on-change?
11. **Result finalization**: how are lap times acknowledged by the server as official?
12. **Obfuscation**: does the handshake include any hash/XOR/rolling key? (Historical gossip says yes; not verified from any public source.)

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

### 11.1 What the Kunos server does

From `LOG`:
- `"Sent Lobby Registration Request with trackName hungaroring"` at startup
- `"RegisterToLobby succeeded"`
- `"WARNING: Lobby accepted connection"` (the WARNING prefix is odd; possibly a level mismatch in the original log code)
- `"Sent config to kson"` once after registration
- `"Sent connected drivers list to kson"` on each driver change
- `"Sent new session state to kson"` on every session phase transition

### 11.2 What the reimplementation does

**Nothing.** `registerToLobby` is hard-wired to `0`. The reimplementation is invisible to the Kunos lobby and must be joined via direct IP (see `serverList.json` mechanism in `HB §III.3.1`).

The reimplementation **must not** attempt to impersonate the Kunos backend in traffic to real ACC clients, as this would exceed the Art. 6 interoperability carve-out and would attack Kunos's infrastructure by proxy.

---

## 12B. ServerMonitor protocol (protobuf)

A second protocol, entirely separate from the sim-side protocol, is used by server-monitoring and hosting tools. Unlike the sim protocol, this one is **protobuf-based** and its schema is fully known.

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

This list is what Notebook A needs to resolve. Keeping it here (with only the question, not any answer derived from the binary) is clean-room-safe.

1. Full ACP_ message ID table and names (we have 9, 34, 49, 50; need 33, 41, 44, 45, 48 and everything else).
2. TCP framing (length prefix size, header layout).
3. UDP packet framing (message ID placement).
4. Connection handshake payload format (ACP_REQUEST_CONNECTION contents, ordering, password/auth location).
5. Entry list push format and timing relative to handshake.
6. Per-tick car update format on UDP (full 3D, rotation, physics channels).
7. Per-tick session state push format.
8. Chat channel ID and frame layout.
9. Weather/track update push ID and schedule.
10. Pit stop request/grant/execute sub-protocol.
11. Penalty assignment sub-protocol (admin + auto).
12. Result finalization sub-protocol.
13. Disconnection semantics and `ignorePrematureDisconnects` behavior.
14. Whether any portion of the handshake is obfuscated or keyed, and if so how.
15. The meaning of the `CLIENT VERSION:` number and how it's negotiated.

---

## 15. Implementation phasing (mirrors project plan)

Each phase is a checkpoint where we can stop, assess, and decide whether to continue.

- **Phase 0** — This notebook. Set up lab (Wine + capture + ACC client VM). Write initial skeleton in C (portable to Linux and OpenBSD): UDP and TCP listeners on the documented ports, config file readers (UTF-16 LE), no protocol logic. *Success criterion*: the skeleton starts, reads the default configs, binds the ports, logs every byte received, and fails gracefully.
- **Phase 1** — Reach handshake. Observe the first `ACP_REQUEST_CONNECTION` in a real pcap, document the framing in this notebook, respond well enough that the client does not immediately tear down. *Success criterion*: real ACC client sees the reimplementation as "a server it can talk to", even if the connection ultimately fails.
- **Phase 2** — Reach `ACP_RACE_MANAGER_INFO_COMPLETED`. Implement the 5-message setup sequence (IDs 44, 44, 41, 45, 48 per LOG). Provide enough session/track/entry-list data that the client reports "ready to receive packets". *Success criterion*: LOG-equivalent sequence appears in our server's own log.
- **Phase 3** — Load into a car. Implement entry list push, car info responses, enough state to put the client on-track at Monza with one car. *Success criterion*: the user is sitting in a car in the pits.
- **Phase 4** — Drive laps. Accept UDP car-state updates, echo them back to self (or to second client), track sector times. *Success criterion*: user can drive a valid lap and see a lap time.
- **Phase 5** — Multi-client, chat, admin. Two real clients in the same session; chat works; `/admin` elevation works.
- **Phase 6+** — Everything else (results, weather, penalties, pitstops, race-start procedure, BoP, entry list gating). Each a separate decision.

## 16. Change log for this document

| Date | Change |
|---|---|
| 2026-04-08 | Initial draft 0.1 from handbook 1.10.2, SDK sources, shipped server.log, default configs, and changelog.txt. |
