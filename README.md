<p align="center">
  <img src="logo.svg" alt="accd" width="640"/>
</p>

<p align="center">
  <a href="https://github.com/renaudallard/assettocorsa_competizione_server/releases/latest">
    <img src="https://img.shields.io/github/v/release/renaudallard/assettocorsa_competizione_server?label=version&style=flat-square" alt="Latest Release"/>
  </a>
  <a href="https://github.com/renaudallard/assettocorsa_competizione_server/actions">
    <img src="https://img.shields.io/github/actions/workflow/status/renaudallard/assettocorsa_competizione_server/autorelease.yml?style=flat-square&label=build" alt="Build Status"/>
  </a>
  <img src="https://img.shields.io/badge/lang-C99-blue?style=flat-square" alt="C99"/>
  <img src="https://img.shields.io/badge/platforms-Linux%20%7C%20OpenBSD%20%7C%20FreeBSD-green?style=flat-square" alt="Platforms"/>
  <a href="LICENSE">
    <img src="https://img.shields.io/badge/license-BSD--2--Clause-orange?style=flat-square" alt="License"/>
  </a>
</p>

---

# accd — ACC dedicated server, clean-room reimplementation

An independent reimplementation of the Assetto Corsa Competizione
dedicated server, built so an unmodified ACC game client (Steam,
current build) can connect to it and play a private multiplayer
session — on **Linux**, **OpenBSD**, and **FreeBSD**, without Wine.

## Status

**Multiplayer works.** Two or more ACC clients connect, see each
other on track at full speed, complete laps, and race through
Practice, Qualifying, and Race sessions with a working countdown
timer, leaderboard, and session transitions.

Protocol correctness has been verified byte-for-byte against a
full 20-minute Kunos `accServer.exe` capture (101,897 packets,
2 players, P+Q+R on Misano).  All 20 server-to-client message
types match the stock server's transport (TCP vs UDP), cadence,
and wire format.

### What works

- **Handshake and connection lifecycle** — TCP framing (variable-
  width length prefix), handshake with password validation, client
  version check, DriverInfo/CarInfo parsing, connection state
  machine, disconnect notification.  Reject (`0x0c`) and accept
  (`0x0b`) formats validated against real Kunos `accServer.exe`.
- **Byte-exact welcome trailer** — the `0x0b` response body is
  built section-for-section from the real Kunos layout, with
  per-section helpers in `handshake.c`.  The body consists of:
  `u32` carIndex, `str_raw` server_name, `str_raw` track,
  `u8` spawnDef count, per-car spawnDef (CarInfo first, then
  DriverInfo array, then 27-byte spawnDef tail), `SeasonEntity`
  common 104 bytes (HudRules + AssistRules + GraphicsRules +
  RealismRules + GameplayRules + OnlineRules + RaceDirectorRules
  + 5 u16 vector counts), `EventEntity` (str_a trackName + 136
  bytes), `session_mgr_state` (variable-length: session_index +
  7 conditional per-session records + 23-byte tail), `assist_rules` +
  `leaderboard` section (header + one lb_entry per car + 2 byte
  tail), an 88-byte block from the unknown `*(0x1410e+0x20)`
  serializer, `trailer_additional_state` (68 bytes: 7 f32 grip +
  9 f32 WeatherStatus + f32 weekend_time, with the WeatherStatus
  block in the Kunos-verified order ambient/road/clouds/wind_dir/
  rain/wind_speed/dry_line/0/0), the `track_records`
  vector (u8 count + per-session 23-byte records), 2 tyre
  compound markers, `MultiplayerTrackRecord::writeToPacket`,
  `MultiplayerCommunityCompetitionRatingSeries`, and 3 trailer
  bytes.  Per-car layouts match `FUN_140032c90`,
  `FUN_14011c7c0`, `FUN_14011cea0`, `FUN_140034210` in the
  Kunos binary; the SeasonEntity fields match `FUN_14011e2a0`.
  The 136-byte `EventEntity` block and the 88-byte
  `*(0x1410e+0x20)` block are still emitted as static templates
  captured from a real Kunos welcome.
- **Post-accept welcome sequence** — `0x28` large state response,
  `0x36` initial leaderboard, `0x37` weather snapshot, and `0x4e`
  rating summary sent immediately after handshake accept, matching
  the real server's welcome sequence.  The `0x28` body matches
  FUN_140033890: session_index byte, 7 variable-length per-session
  records (u8 valid + conditional f32), and 23-byte tail.  Schedule
  f32 values are absolute timestamps in the client's game clock
  (`ts - server_now + client_ts + RTT/2`, matching FUN_1400418b0),
  built per-connection, sent every ~1s as a continuous heartbeat
  (verified against 902-message Kunos capture).  Progressive slot
  activation for race sessions (slots enable as phases are reached).
- **Full message dispatch** — all 22 TCP and 7 UDP client-to-server
  message types are handled; 17 TCP and 3 UDP server-to-client
  message types implemented (plus 7 ServerMonitor protobuf types).
  Transport (TCP vs UDP) verified byte-for-byte against Kunos
  capture for every message type.
- **Event-driven per-car relay** — incoming `0x1e` car updates are
  immediately relayed as `0x39` to all other peers via UDP (~18 Hz),
  matching the exe's event-driven architecture.  Per-peer timestamp
  adjustment using client-to-client pong deltas for correct
  dead-reckoning.  UDP car updates matched to connections by
  `source_conn_id` for NAT support.
- **Session management** — configurable session sequence
  (Practice / Qualifying / Race) with automatic phase transitions,
  session timers, and session advance/restart via admin commands.
  `preRaceWaitingTimeSeconds` and `sessionOverTimeSeconds` parsed
  from `event.json`.  Dynamic overtime hold for race sessions
  (freezes at overtime until all cars finish their lap).  Weekend
  reset (`0x40`) loops back to session 0 after race ends.
- **Leaderboard and standings** — real-time standings recomputation
  on lap completion, leaderboard broadcast (`0x36`), grid positions
  at race start (`0x3f`), session results at session end (`0x3e`
  with full `session_mgr_state` + `leaderboard_section` body).
- **Results file writer** — `results/YYMMDD_HHMMSS_<type>.json`
  written at session end, matching the stock server schema.
- **Admin commands** — full set via in-game chat or the stdin
  console: `/admin`, `/next`, `/restart`, `/kick`, `/ban`, `/dq`,
  time penalties (`/tp5`, `/tp15`, drive-through, stop-and-go),
  `/ballast`, `/restrictor`, `/clear`, `/connections`.
- **Penalty system** — per-car penalty queue, pit-speed enforcement
  from telemetry, mandatory pitstop tracking.
- **Weather and in-game clock** — deterministic sin/cos weather
  simulator with cloud and rain cycles, seeded from the session
  start time so the first tick produces consistent values.  `0x37`
  weather broadcast fires unconditionally every 5 seconds carrying
  `weekend_time_s`, which the client uses to drive the sun
  position and in-game clock display.  `weekend_time_s` is
  initialized to `hourOfDay * 3600` at session start and advances
  with the configured `timeMultiplier`.
- **ServerMonitor protocol** — protobuf message builders for
  session state, car entries, connection entries, leaderboard,
  and realtime updates (available for future broadcasting
  protocol support).
- **Entry list** — `entrylist.json` reader populating car templates
  with driver info, ballast, restrictor, grid positions.  When
  `forceEntryList: 1`, only Steam IDs in the entry list are
  accepted; others are rejected.
- **LAN discovery** — UDP 8999 broadcast response so clients on
  the same network find the server automatically.
- **Rating summary** — `0x4e` per-connection broadcast on
  connection events (matching Kunos cadence).
- **BoP updates** — `0x53` broadcast on ballast/restrictor changes.
- **Driver swap** — full endurance-style driver swap state machine
  for multi-driver entries, with `&swap` chat command and `0x47` /
  `0x48` / `0x4a` / `0x58` wire protocol.
- **Live track change** — `/track <name>` changes the track
  mid-session with `0x4b` welcome redelivery to all clients.
- **Persistent bans** — `/ban` writes to `cfg/banlist.txt` and
  rejects banned Steam IDs on reconnect.  Survives restarts.
- **Debug tracing** — `-d` flag or `debug` console command enables
  full wire hexdump of every message sent and received.
- **Admin console** — interactive stdin console when running from a
  terminal (see below).
- **OpenBSD support** — builds and runs on OpenBSD 7.8 arm64 with
  `pledge("stdio rpath wpath cpath inet")` after binding ports.

### Known limitations

- No public lobby registration (`registerToLobby` is always 0).
- No Championship Points / CP / SA rating tracking.
- Car-to-car collisions are client-side physics; the server relays
  positions but does not arbitrate contact.
- Two welcome trailer sub-structures (EventEntity, `*(0x1410e+0x20)`)
  are still static templates.

### Protocol specification

The clean-room protocol specification in
[`notebook-b/NOTEBOOK_B.md`](notebook-b/NOTEBOOK_B.md) documents
the full wire protocol: transport framing, string encodings,
connection state machine, all client and server message IDs with
byte-exact wire formats, and the ServerMonitor protobuf schema.

---

## Scope

- **In scope**: private-multiplayer gameplay against the stock ACC
  game client, running on Linux or OpenBSD, without Wine.
- **Out of scope**: Kunos's public lobby backend, the Community
  Competition rating / competition-point system, and anything that
  requires talking to Kunos infrastructure. This server is always
  `registerToLobby: 0`. Join is by direct IP via `serverList.json`
  on the client side.

---

## Legal posture

This is an independent-program reimplementation pursued for
**interoperability** purposes only. It relies on the carve-out in
Article 6 of EU Directive 2009/24/EC on the legal protection of
computer programs, which permits reverse engineering of a computer
program when necessary to obtain the information needed to achieve
interoperability of an independently created program.

- You must own a legitimate copy of Assetto Corsa Competizione on
  Steam to make any use of this project.
- This repository ships **no Kunos code and no Kunos assets**. It
  ships only an independent clean-room specification and an
  independent implementation.
- The specification in `notebook-b/` is derived exclusively from
  public documentation shipped by Kunos with the ACC Dedicated
  Server Steam tool (app 1430110) and, where necessary, from
  observations rewritten in the author's own words.
- A separate working set of dirty notes exists locally for
  reverse-engineering use during development. These dirty notes
  are gitignored and are **never published**; only the clean-room
  specification is public.

## Repository layout

```
.
├── README.md                This file.
├── LICENSE                  BSD-2-Clause license.
├── VERSION                  Version number (triggers releases).
├── notebook-b/
│   └── NOTEBOOK_B.md        The public clean-room protocol spec.
├── accd/                    The C implementation (24 modules).
│   ├── main.c               Poll loop + signal handling + lifecycle.
│   ├── bans.{c,h}           Persistent kick / ban list.
│   ├── bcast.{c,h}          Broadcast helpers (TCP + UDP relay).
│   ├── chat.{c,h}           Admin chat commands + penalty dispatch.
│   ├── config.{c,h}         JSON config reader (UTF-16 LE or UTF-8).
│   ├── console.{c,h}        stdin admin console (poll-driven).
│   ├── dispatch.{c,h}       TCP / UDP message dispatchers.
│   ├── entrylist.{c,h}      entrylist.json reader.
│   ├── handlers.{c,h}       Per-msg-id handlers (22 TCP + 4 UDP).
│   ├── handshake.{c,h}      ACP_REQUEST_CONNECTION + 0x0b + welcome.
│   ├── io.{c,h}             Byte buffer + TCP framing layer.
│   ├── json.{c,h}           Recursive-descent JSON parser.
│   ├── lan.{c,h}            UDP 8999 LAN discovery handler.
│   ├── log.{c,h}            Timestamped logger + hexdump.
│   ├── monitor.{c,h}        ServerMonitor protobuf message builders.
│   ├── msg.h                All message id constants + enums.
│   ├── net.{c,h}            tcp_listen / udp_bind helpers.
│   ├── pb.{c,h}             Minimal write-only protobuf encoder.
│   ├── penalty.{c,h}        Per-car penalty queue.
│   ├── prim.{c,h}           Primitive readers / writers + strings.
│   ├── probe.c              Standalone protocol probe tool.
│   ├── results.{c,h}        Session results JSON writer.
│   ├── session.{c,h}        Session phase machine + standings sort.
│   ├── state.{c,h}          Per-conn / global server state structs.
│   ├── tick.{c,h}           Event-driven relay + periodic broadcasts.
│   ├── weather.{c,h}        Deterministic sin/cos weather simulator.
│   └── Makefile
├── debian/                  Debian/Ubuntu packaging.
├── redhat/                  Fedora/Rocky RPM spec.
└── .github/workflows/       CI: autorelease + multi-distro packaging.
```

The implementation is **24 modules and ~12,000 lines of portable
C99**, with no dependencies beyond libc, iconv, and libm.

## Building

The implementation (`accd/`) is portable C99 that builds with either
BSD or GNU `make`, using only libc + iconv + libm.

### Linux (glibc --- iconv is in libc)

```sh
cd accd
make
```

Tested on Debian sid aarch64 with `gcc 15.2.0`.

### OpenBSD

```sh
cd accd
make
```

The Makefile auto-detects `/usr/local/include/iconv.h` and links
`-liconv` when iconv is not in libc.  No extra flags needed.

Tested on OpenBSD 7.8 arm64 with `clang 19.1.7`.  On OpenBSD
the process pledges to `stdio rpath wpath cpath inet` after
binding its listening ports.

### Installing

```sh
cd accd
make install          # installs to /usr/local/bin and man1
make install PREFIX=/usr DESTDIR=/tmp/staging   # for packaging
```

The install target places the `accd` binary and man page.
See `accd(1)` for full documentation of options, configuration
files, and admin commands.

## Running

### Configuration

`accd` expects a `cfg/` directory containing JSON configuration
files.  Each file may be either UTF-16 LE with a BOM (the format
`accServer.exe` writes) or plain UTF-8 (so the files can be
hand-edited in any normal text editor); detection is automatic.

By default `accd` looks for `cfg/` in the current directory; an
alternative path can be passed as the first argument:

```sh
./accd                           # uses ./cfg/
./accd /path/to/other/cfg        # explicit path
./accd -d                        # enable debug tracing
./accd -c /path/to/cfg           # alternative to positional arg
```

#### configuration.json

Network settings:

```json
{
    "udpPort": 9231,
    "tcpPort": 9232,
    "maxConnections": 30,
    "configVersion": 1
}
```

The server always listens on UDP 8999 for client discovery probes
(the ACC client sends a discovery probe before connecting, even for
remote servers).  Ensure this port is open in your firewall.

#### settings.json

Server identity and passwords:

```json
{
    "serverName": "My accd server",
    "password": "",
    "adminPassword": "my-admin-pass",
    "spectatorPassword": "",
    "maxCarSlots": 30,
    "configVersion": 1
}
```

- `password` — required to join as a driver; empty means open.
- `adminPassword` — used in-game via `/admin <password>` to
  elevate to admin rights (kick, ban, penalties, etc.).
- `spectatorPassword` — alternative that admits the client as a
  spectator.

#### event.json

Track, weather, and session schedule:

```json
{
    "track": "monza",
    "preRaceWaitingTimeSeconds": 80,
    "sessionOverTimeSeconds": 120,
    "ambientTemp": 22,
    "cloudLevel": 0.1,
    "rain": 0.0,
    "weatherRandomness": 1,
    "sessions": [
        {
            "hourOfDay": 12,
            "dayOfWeekend": 2,
            "timeMultiplier": 1,
            "sessionType": "P",
            "sessionDurationMinutes": 10
        },
        {
            "hourOfDay": 14,
            "dayOfWeekend": 2,
            "timeMultiplier": 1,
            "sessionType": "Q",
            "sessionDurationMinutes": 10
        },
        {
            "hourOfDay": 16,
            "dayOfWeekend": 3,
            "timeMultiplier": 2,
            "sessionType": "R",
            "sessionDurationMinutes": 20
        }
    ],
    "configVersion": 1
}
```

Session types: `P` (Practice), `Q` (Qualifying), `R` (Race).

#### entrylist.json (optional)

Pre-populated car entries with driver info, ballast, restrictor,
and grid positions.  If absent, the server accepts any client into
the first available slot.

### Fetching stock Kunos config files

To get the default config files from the ACC Dedicated Server Steam
tool (requires a Steam account that owns ACC):

```sh
steamcmd +@sSteamCmdForcePlatformType windows \
         +force_install_dir /path/to/acc-server \
         +login <your steam username> \
         +app_update 1430110 validate +quit
cp -r /path/to/acc-server/server/cfg ./cfg
```

The stock files are UTF-16 LE; `accd` reads them as-is or you can
convert them to UTF-8 for easy editing with `iconv`:

```sh
iconv -f UTF-16LE -t UTF-8 cfg/settings.json | tr -d '\r' > tmp && mv tmp cfg/settings.json
```

### Starting the server

```sh
cd accd
./accd
```

The server binds the configured TCP and UDP ports (default 9232
and 9231), starts the session schedule from `event.json`, and
begins accepting client connections.  Log output goes to stderr:

```
2026-04-09 08:19:24.384 INFO accd phase 1 starting (pid 78045)
2026-04-09 08:19:24.385 INFO config: tcp=9232 udp=9231 max=30 lan=1 track="monza"
2026-04-09 08:19:24.385 INFO lan discovery listening on udp/8999
2026-04-09 08:19:24.385 INFO admin console enabled (type 'help' for commands)
2026-04-09 08:19:24.385 INFO listening: tcp/9232 udp/9231 (Ctrl-C to stop)
```

Stop the server with `Ctrl-C`, `quit` at the console, or
`kill -TERM <pid>`.

### Firewall / port forwarding

Open these ports for clients to connect:

| Port | Protocol | Purpose |
|------|----------|---------|
| 9232 | TCP | Game connection (handshake, chat, session data) |
| 9231 | UDP | Car telemetry (position, inputs, timing) |
| 8999 | UDP | Client discovery (required for clients to find the server) |

Ports 9232 and 9231 are configurable in `configuration.json`.
Port 8999 is fixed by the ACC protocol.  All three ports must be
open in the firewall for clients to connect.

### Connecting from the ACC game client

On the client machine, create or edit the file:

```
%userprofile%\Documents\Assetto Corsa Competizione\Config\serverList.json
```

On macOS with CrossOver, the path is typically:

```
~/Documents/Games/Assetto Corsa Competizione/Config/serverList.json
```

The file contains a single JSON object with the server IP:

```json
{
    "leagueServerIP": "192.168.1.100"
}
```

The game connects on the default ports (TCP 9232 / UDP 9231).
If the server has a password, it is entered in-game when joining.

The server will appear in the ACC multiplayer server list.

### Admin console

When stdin is a TTY (interactive terminal or SSH session), an
admin console is automatically enabled.  Type commands directly
to control the server without needing to connect as an in-game
client:

```
$ ./accd
[...startup logs on stderr...]
help
commands (leading / optional):
  help             show this list
  status           session phase, connections, tick
  show cars        list car slots in use
  show conns       list active connections
  next             advance to next session
  restart          restart current session
  kick <num>       kick car by race number
  ban <num>        kick + persistent ban
  dq <num>         disqualify
  tp5 <num>        5s time penalty (tp5c = collision)
  tp15 <num>       15s time penalty (tp15c)
  dt <num>         drive-through (dtc)
  sg10 <num>       10s stop-and-go (sg10c..sg30c)
  clear <num>      clear penalties for car
  clear_all        clear all penalties
  ballast <n> <kg> assign ballast
  restrictor <n> % assign restrictor
  track [name]     show or change track
  tracks           list available tracks
  connections      list connections (also broadcasts)
  debug            toggle debug tracing
  quit             shut down the server
status
session 0  phase=PRACTICE  remaining=540000 ms  tick=42  conns=1
```

The leading `/` is optional (both `next` and `/next` work).
Console replies go to stdout, server logs go to stderr.
Separate them with `./accd 2>accd.log`.

When stdin is not a TTY (e.g. `./accd < /dev/null` or systemd),
the console disables itself and the server runs headless.  All
admin commands remain available via in-game chat after
authenticating with `/admin <password>`.

### Running as a background service

To run headless without the console:

```sh
./accd 2>accd.log &
```

Or install the `.deb` / `.rpm` package and use the shipped systemd
unit, which runs as an unprivileged dynamic user with sandboxing:

```sh
sudo systemctl enable --now accd
# config files go in /var/lib/accd/cfg/
```

### Quick smoke test (no real client)

Reject path (wrong protocol version, should get 14-byte `0x0c`
reject and connection closed):

```sh
printf '\x03\x00\x09\x99\x00' | nc -q 1 127.0.0.1 9232 | xxd
# expects: 0e 00 0c 07 00 00 00 00 99 00 00 00 00 01 00 00
```

Accept path (correct version `0x100`, empty password, should get
`0x0b` accept with welcome trailer):

```sh
printf '\x04\x00\x09\x00\x01\x00' | nc -q 1 127.0.0.1 9232 | xxd
# expects: large response starting with 0b 0f 24 ...
```

---

## Contributing

This project follows strict clean-room discipline. Before
contributing, please read section 0 of
[`notebook-b/NOTEBOOK_B.md`](notebook-b/NOTEBOOK_B.md). In short:

- All facts in the public specification (`notebook-b/`) must be
  traceable to a public source (handbook, SDK, changelog, default
  configs, shipped sample log).
- If you derive information from static or dynamic analysis of
  `accServer.exe`, that work must be kept in your own private
  dirty-notes directory (`notebook-a/` is conventional; gitignored)
  and must **not** be committed to this repository.
- Facts from dirty notes may only be promoted to the public spec
  after being rewritten, in your own words, as protocol-level
  statements about what bytes go on the wire in what order — not
  as statements about any particular implementation of the
  protocol.

## License

BSD-2-Clause. See [`LICENSE`](LICENSE) for the full text.  The clean-room
specification text in `notebook-b/` is authored for public
reference under the same terms.

Nothing in this repository is authored, endorsed, or licensed by
Kunos Simulazioni or 505 Games.

## Support This Project

If you find this project useful, you can support its development:

[![PayPal](https://img.shields.io/badge/PayPal-Donate-blue.svg?logo=paypal)](https://www.paypal.me/RenaudAllard)
