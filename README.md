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

<h1 align="center">accd</h1>
<p align="center">
  <b>ACC dedicated server, clean-room reimplementation</b><br/>
  An unmodified Assetto Corsa Competizione client connects and races,<br/>
  on Linux, OpenBSD, or FreeBSD — no Wine, no Kunos binaries.
</p>

---

## Contents

- [Status](#status)
- [What works](#what-works)
- [Known limitations](#known-limitations)
- [Building](#building)
- [Running](#running)
  - [Configuration files](#configuration-files)
  - [Starting the server](#starting-the-server)
  - [Firewall / ports](#firewall--ports)
  - [Connecting from the ACC client](#connecting-from-the-acc-client)
  - [Admin console](#admin-console)
  - [Background service](#background-service)
  - [Quick smoke test](#quick-smoke-test)
- [Scope & legal posture](#scope--legal-posture)
- [Repository layout](#repository-layout)
- [Contributing](#contributing)
- [License](#license)

---

## Status

> **Multiplayer works.** Two or more ACC clients connect, see each
> other on track at full speed, complete laps, and race through
> Practice → Qualifying → Race with working countdown, leaderboard,
> and session transitions.

Protocol correctness has been verified byte-for-byte against a full
20-minute Kunos `accServer.exe` capture (101,897 packets, 2 players,
P+Q+R on Misano).  All 20 server-to-client message types match the
stock server's transport (TCP vs UDP), cadence, and wire format.

The clean-room protocol specification lives in
[`notebook-b/NOTEBOOK_B.md`](notebook-b/NOTEBOOK_B.md) and documents
every wire message, string encoding, and state transition.

---

## What works

### Connection & handshake

- **TCP framing** with variable-width length prefix; variable-length
  welcome trailer (`0x0b`) built section-for-section to match the
  Kunos layout byte-for-byte.  Reject (`0x0c`) codes 4–12 wired
  correctly.
- **Post-accept welcome sequence** — `0x28` large state, `0x36`
  leaderboard, `0x37` weather, `0x4e` rating summary sent in the
  right order and cadence.  Per-connection time-base projection
  (`ts − server_now + client_ts + RTT/2`) for `0x28` schedule
  timestamps.
- **Quick reconnect** by Steam ID drops the old conn and reuses the
  car slot, so race state, grid position, and penalty queue survive
  a mid-race disconnect.  Works both while the old socket is still
  alive and after the inactive-peer sweep removes it.
- **Mid-race join controls** — `unsafeRejoin: 0` refuses fresh
  handshakes during an active race; `/lockprep` freezes the
  preparation phase.  Returning drivers always bypass both.

### Session management

- **P / Q / R schedule** with automatic phase transitions, countdown
  timers, overtime hold, and weekend reset after the final race.
- **Position-based race start** — the green flag fires when the
  leader's normalised track position crosses a randomised trigger
  inside the configured green range, matching `FUN_14012f4a0` with
  no time fallback.  Broadcasts "Race start initialized" on fire.
- **Race grid from qualy** — race grid derived from the most recent
  prior qualifying session's finishing order.  `defaultGridPosition`
  in `entrylist.json` is used only when no prior Q/P exists.
- **Ranked leaderboard / results** — real-time standings on lap
  completion, `0x36` broadcast in ranked order, `0x3f` grid at race
  start, `0x3e` session results at session end.
- **Results file writer** — `results/YYMMDD_HHMMSS_<type>.json`
  matching the stock server schema.

### Telemetry & relay

- **Event-driven per-car relay** — incoming `0x1e` car updates are
  immediately relayed as `0x39` to all other peers at ~18 Hz, with
  per-peer timestamp adjustment for dead reckoning.
- **Weather & in-game clock** — deterministic sin/cos weather with
  seeded cycles; 5-second `0x37` broadcast carrying `weekend_time_s`
  driven by `hourOfDay` × `timeMultiplier`.
- **Driver swap** — full endurance-style swap state machine
  (`&swap`, `0x47`/`0x48`/`0x4a`/`0x58`) for multi-driver entries.
- **Live track change** — `/track <name>` swaps the track
  mid-session with `0x4b` welcome redelivery to every client.
- **ServerMonitor protocol** — protobuf builders for session state,
  cars, connections, leaderboard, and realtime updates.

### Admin & moderation

- **Chat / console commands**: `/admin`, `/next`, `/restart`,
  `/kick`, `/ban`, `/dq`, `/tp5`, `/tp15`, `/dt`, `/sg10..30` (all
  with collision variants), `/clear`, `/clear_all`, `/ballast`,
  `/restrictor`, `/track`, `/connections`, `/hellban`, `/lockprep`,
  `/unlockprep`, `/manual entrylist`, `/manual start`, `/wt`, `/go`,
  `/report`.
- **Penalty system** — per-car queue, mandatory pitstop tracking,
  3-lap deadline countdown for DT/SG with auto-DQ on miss
  (downgradable via `allowAutoDQ: 0`), pit-speeding auto-DQ from
  telemetry.
- **Persistent bans** — `/ban` writes to `cfg/banlist.txt` and
  survives restarts; banned Steam IDs are rejected on reconnect.

### Integration

- **LAN discovery** — UDP 8999 broadcast response so clients on the
  same network find the server automatically.
- **Public lobby registration** — `registerToLobby: 1` connects to
  the Kunos lobby backend so the server is listed in the in-game
  server browser.  Set `0` for direct-IP-only private servers.
- **Entry list** — `entrylist.json` populates slots; with
  `forceEntryList: 1`, only listed Steam IDs are accepted.
- **BoP** — `0x53` broadcast on ballast/restrictor changes.
- **Debug tracing** — `-d` flag or `debug` console command enables
  full wire hexdump of every message.
- **OpenBSD support** — builds on OpenBSD 7.8 arm64, runs under
  `pledge("stdio rpath wpath cpath inet")` after binding ports.

### Known limitations

- Car-to-car collisions are client-side physics; the server relays
  positions but does not arbitrate contact.
- Weather forecast curve in the welcome trailer is populated from
  current server state (ambient/wind/grip/puddles); the two
  variable-length forecast lists stay empty, so the in-game forecast
  HUD shows a flat prediction.  The deterministic sin/cos weather
  drift is still visible via the live `0x37` broadcast during the
  session.
- Single-threaded event loop.  The stock Kunos exe is multi-threaded
  (CONCRT worker queue + per-client threads).  accd uses one
  non-blocking `poll()` loop with a 256-packet UDP drain burst —
  comfortable at 30 cars × 18 Hz, but intentionally different from
  the exe's concurrency model.
- A handful of Kunos `settings.json` keys are parsed and stored but
  don't drive any accd behaviour because the backing feature isn't
  implemented: `writeLatencyFileDumps` (no latency-dumps sink),
  `configVersion` (no schema-migration), and the entire CP-server
  stack (`isCPServer`, `isCPInvServer`, `competitionRatingMin/Max`,
  `region`, etc.) — CP servers require the Kunos ranked backend we
  can't reach from a third-party server.

---

## Building

Portable C99, builds with either BSD or GNU `make`, no dependencies
beyond libc + iconv + libm.

### Linux

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
`-liconv` when iconv is not in libc.  Tested on OpenBSD 7.8 arm64
with `clang 19.1.7`.

### Install

```sh
cd accd
make install                                     # /usr/local/bin + man1
make install PREFIX=/usr DESTDIR=/tmp/staging    # for packaging
```

See `accd(1)` for the full reference.

---

## Running

### Configuration files

`accd` expects a `cfg/` directory containing JSON files.  Each file
may be UTF-16 LE with a BOM (the format `accServer.exe` writes) or
plain UTF-8 — detection is automatic.

```sh
./accd                           # uses ./cfg/
./accd /path/to/other/cfg        # explicit path
./accd -c /path/to/cfg           # alternative syntax
./accd -d                        # enable debug tracing
```

<details>
<summary><b>configuration.json — network</b></summary>

```json
{
    "udpPort": 9231,
    "tcpPort": 9232,
    "maxConnections": 30,
    "statsUdpPort": 0,
    "configVersion": 1
}
```

`statsUdpPort` is optional: when non-zero the server pushes a 1 Hz
`0xbe` state snapshot to `127.0.0.1:<port>` for local monitoring
tools (never routed off loopback).  `0` disables it.

</details>

<details>
<summary><b>settings.json — identity and policy</b></summary>

```json
{
    "serverName": "My accd server",
    "password": "",
    "adminPassword": "my-admin-pass",
    "spectatorPassword": "",
    "maxCarSlots": 30,
    "allowAutoDQ": 1,
    "registerToLobby": 1,
    "useAsyncLeaderboard": 1,
    "unsafeRejoin": 1,
    "ignorePrematureDisconnects": 0,
    "dumpLeaderboards": 0,
    "configVersion": 1
}
```

| Key | Default | Meaning |
|---|---|---|
| `password` | `""` | Required to join as a driver; empty means open. |
| `adminPassword` | `""` | Used in-game via `/admin <pw>` to elevate to admin. |
| `spectatorPassword` | `""` | Admits the client as a spectator. |
| `allowAutoDQ` | `1` | `0` downgrades failed DT/SG to a 30 s stop&go. |
| `registerToLobby` | `0` | `1` lists the server publicly in the ACC browser. |
| `useAsyncLeaderboard` | `1` | `0` broadcasts on every standings change. |
| `unsafeRejoin` | `1` | `0` refuses fresh mid-race handshakes. |
| `formationLapType` | `3` | Race-start variant. `1` manual (private only, verbose with "Race start initialized" chat), `3` default rolling (silent), `5` short formation. |
| `isPrepPhaseLocked` | `0` | `1` freezes the preparation phase; returning drivers still pass (same knob as the `/lockprep` admin command). |
| `shortFormationLap` | `0` | `1` shortens the formation lap (parsed and passed through; exe forces `1` on public servers). |
| `writeLatencyFileDumps` | `0` | `1` enables the latency-diagnostics file output (parsed; diagnostics sink not hooked yet). |
| `latencyStrategy` | `0` | Initial value for the `/latencymode` runtime toggle. |
| `doDriverSwapBroadcast` | `1` | `0` suppresses the 0x47 driver-swap-state fan-out; swap progress stays on the swapping car. |
| `ignorePrematureDisconnects` | `0` | `1` tolerates client-side premature drops. |
| `dumpLeaderboards` | `0` | `1` writes snapshots to `results/` on every update. |

</details>

<details>
<summary><b>event.json — track, weather, schedule</b></summary>

```json
{
    "track": "monza",
    "preRaceWaitingTimeSeconds": 80,
    "sessionOverTimeSeconds": 120,
    "ambientTemp": 22,
    "cloudLevel": 0.1,
    "rain": 0.0,
    "weatherRandomness": 1,
    "formationTriggerNormalizedRangeStart": 0.80,
    "greenFlagTriggerNormalizedRangeStart": 0.89,
    "greenFlagTriggerNormalizedRangeEnd":   0.96,
    "sessions": [
        { "hourOfDay": 12, "dayOfWeekend": 2, "timeMultiplier": 1,
          "sessionType": "P", "sessionDurationMinutes": 10 },
        { "hourOfDay": 14, "dayOfWeekend": 2, "timeMultiplier": 1,
          "sessionType": "Q", "sessionDurationMinutes": 10 },
        { "hourOfDay": 16, "dayOfWeekend": 3, "timeMultiplier": 2,
          "sessionType": "R", "sessionDurationMinutes": 20 }
    ],
    "configVersion": 1
}
```

Session types: `P` (Practice), `Q` (Qualifying), `R` (Race).

The three `formationTrigger*` / `greenFlag*` keys override the built-in
defaults for the position-based race-start gate (normalized track
positions 0..1).  Shown above at the exe's compiled-in fallback
values; leave absent to use them.

</details>

<details>
<summary><b>entrylist.json — optional pre-populated slots</b></summary>

Pre-assigns car entries with driver info, ballast, restrictor, and
grid positions.  If absent, the server accepts any client into the
first available slot.  With `forceEntryList: 1`, only Steam IDs in
the entry list are accepted.

</details>

<details>
<summary><b>Fetching stock Kunos config files via steamcmd</b></summary>

```sh
steamcmd +@sSteamCmdForcePlatformType windows \
         +force_install_dir /path/to/acc-server \
         +login <your steam username> \
         +app_update 1430110 validate +quit
cp -r /path/to/acc-server/server/cfg ./cfg
```

Stock files are UTF-16 LE; `accd` reads them as-is.  To convert to
UTF-8 for hand-editing:

```sh
iconv -f UTF-16LE -t UTF-8 cfg/settings.json | tr -d '\r' > tmp \
  && mv tmp cfg/settings.json
```

</details>

### Starting the server

```sh
cd accd
./accd
```

```
2026-04-18 08:19:24 INFO accd phase 1 starting (pid 78045)
2026-04-18 08:19:24 INFO config: tcp=9232 udp=9231 max=30 lan=1 track="monza"
2026-04-18 08:19:24 INFO lan discovery listening on udp/8999
2026-04-18 08:19:24 INFO admin console enabled (type 'help' for commands)
2026-04-18 08:19:24 INFO listening: tcp/9232 udp/9231 (Ctrl-C to stop)
```

Stop with `Ctrl-C`, `quit` at the console, or `kill -TERM <pid>`.

### Firewall / ports

| Port | Proto | Purpose |
|-----:|:-----:|---------|
| 9232 | TCP | Game connection (handshake, chat, session data) |
| 9231 | UDP | Car telemetry (position, inputs, timing) |
| 8999 | UDP | Client discovery (required for in-game find) |

Ports 9232 and 9231 are configurable in `configuration.json`; UDP
8999 is fixed by the ACC protocol.  All three must be open.

### Connecting from the ACC client

On the client machine:

```
%userprofile%\Documents\Assetto Corsa Competizione\Config\serverList.json
```

(macOS / CrossOver: `~/Documents/Games/Assetto Corsa Competizione/Config/serverList.json`)

```json
{ "leagueServerIP": "192.168.1.100" }
```

The server appears in the in-game multiplayer server list.

### Admin console

When stdin is a TTY, an interactive admin console runs alongside the
server:

```
$ ./accd
help
commands (leading / optional):
  help                show this list
  status              session phase, connections, tick
  show cars           list car slots in use
  show conns          list active connections
  next                advance to next session
  restart             restart current session
  kick <num>          kick car by race number
  ban <num>           kick + persistent ban
  dq <num>            disqualify
  tp5 <num>           5s time penalty (tp5c = collision)
  tp15 <num>          15s time penalty (tp15c)
  dt <num>            drive-through (dtc)
  sg10 <num>          10s stop-and-go (sg10c..sg30c)
  clear <num>         clear penalties for car
  clear_all           clear all penalties
  ballast <n> <kg>    assign ballast
  restrictor <n> %    assign restrictor
  track [name]        show or change track
  tracks              list available tracks
  connections         list connections (also broadcasts)
  debug               toggle debug tracing
  quit                shut down the server
```

Leading `/` is optional (both `next` and `/next` work).  Console
replies go to stdout, server logs go to stderr — split with
`./accd 2>accd.log`.

When stdin isn't a TTY (e.g. `./accd < /dev/null` or systemd) the
console disables itself and the server runs headless.  All admin
commands remain available via in-game chat after `/admin <password>`.

### Background service

Quick headless run:

```sh
./accd 2>accd.log &
```

Production: install the `.deb` or `.rpm` package and use the shipped
systemd unit (runs as an unprivileged dynamic user, sandboxed):

```sh
sudo systemctl enable --now accd
# config files go in /var/lib/accd/cfg/
```

### Quick smoke test

Reject path (wrong protocol version, expect 14-byte `0x0c`):

```sh
printf '\x03\x00\x09\x99\x00' | nc -q 1 127.0.0.1 9232 | xxd
# expects: 0e 00 0c 07 00 00 00 00 99 00 00 00 00 01 00 00
```

Accept path (correct version `0x100`, empty password):

```sh
printf '\x04\x00\x09\x00\x01\x00' | nc -q 1 127.0.0.1 9232 | xxd
# expects: large response starting with 0b 0f 24 ...
```

---

## Scope & legal posture

- **In scope**: private-multiplayer gameplay against the stock ACC
  client, running on Linux, OpenBSD, or FreeBSD, without Wine.
- **Out of scope**: the Community Competition rating / competition-
  point system, and anything that requires running Kunos code.

<details>
<summary><b>Legal posture (EU Directive 2009/24/EC, Article 6)</b></summary>

This is an independent-program reimplementation pursued for
**interoperability** purposes only, relying on the carve-out in
Article 6 of EU Directive 2009/24/EC on the legal protection of
computer programs, which permits reverse engineering of a computer
program when necessary to obtain the information needed to achieve
interoperability of an independently created program.

- You must own a legitimate copy of Assetto Corsa Competizione on
  Steam to make any use of this project.
- This repository ships **no Kunos code and no Kunos assets** — only
  an independent clean-room specification and an independent
  implementation.
- The specification in `notebook-b/` is derived exclusively from
  public documentation shipped by Kunos with the ACC Dedicated
  Server Steam tool (app 1430110) and from observations rewritten
  in the author's own words.
- A separate working set of dirty notes exists locally for
  reverse-engineering use during development.  These are gitignored
  and **never published**; only the clean-room specification is
  public.

</details>

---

## Repository layout

<details>
<summary>Tree</summary>

```
.
├── README.md                This file.
├── LICENSE                  BSD-2-Clause license.
├── VERSION                  Version number (triggers releases).
├── notebook-b/
│   └── NOTEBOOK_B.md        The public clean-room protocol spec.
├── accd/                    The C implementation (27 modules).
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
│   ├── lobby.{c,h}          Kunos public-lobby client.
│   ├── log.{c,h}            Timestamped logger + hexdump.
│   ├── monitor.{c,h}        ServerMonitor protobuf message builders.
│   ├── msg.h                All message id constants + enums.
│   ├── net.{c,h}            tcp_listen / udp_bind helpers.
│   ├── pb.{c,h}             Minimal write-only protobuf encoder.
│   ├── penalty.{c,h}        Per-car penalty queue.
│   ├── prim.{c,h}           Primitive readers / writers + strings.
│   ├── probe.c              Standalone protocol probe tool.
│   ├── ratings.{c,h}        Persistent SA/TR rating ledger.
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

</details>

27 modules, ~17,000 lines of portable C99.  No dependencies beyond
libc, iconv, and libm.

---

## Contributing

This project follows strict clean-room discipline.  Before
contributing, read **§ 0** of
[`notebook-b/NOTEBOOK_B.md`](notebook-b/NOTEBOOK_B.md).  In short:

- Every fact in the public spec must trace to a public source:
  handbook, SDK, changelog, default configs, shipped sample logs.
- Static or dynamic analysis of `accServer.exe` is kept in your own
  private dirty-notes directory (`notebook-a/` by convention,
  gitignored) and **never committed**.
- Facts promoted to the public spec must be rewritten, in your own
  words, as protocol-level statements about what bytes go on the
  wire — not as statements about any particular implementation.

---

## License

[BSD-2-Clause](LICENSE).  The clean-room specification in
`notebook-b/` is published under the same terms.

Nothing in this repository is authored, endorsed, or licensed by
Kunos Simulazioni or 505 Games.

---

## Support this project

If you find accd useful, you can support development:

[![PayPal](https://img.shields.io/badge/PayPal-Donate-blue.svg?logo=paypal)](https://www.paypal.me/RenaudAllard)
