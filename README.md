<p align="center">
  <img src="logo.svg" alt="accd" width="640"/>
</p>

# accd — ACC dedicated server, clean-room reimplementation

An independent reimplementation of the Assetto Corsa Competizione
dedicated server, built so an unmodified ACC game client (Steam,
current build) can connect to it and play a private multiplayer
session — on Linux and OpenBSD, without Wine.

## Status

The server implements the full ACC multiplayer protocol and can
host private sessions.  The handshake, reject, and welcome
response formats have been validated against a real Kunos
`accServer.exe` 1.10.2 instance.  The server correctly accepts
connections, assigns car IDs, and begins the session lifecycle.

### What works

- **Handshake and connection lifecycle** — TCP framing (variable-
  width length prefix), handshake with password validation, client
  version check, DriverInfo/CarInfo parsing, connection state
  machine, disconnect notification.  Reject (`0x0c`) and accept
  (`0x0b`) formats validated against real Kunos `accServer.exe`.
- **Post-accept welcome sequence** — `0x28` large state response,
  `0x36` initial leaderboard, `0x37` weather snapshot, and `0x4e`
  rating summary sent immediately after handshake accept, matching
  the real server's welcome sequence.
- **Full message dispatch** — all 22 TCP and 7 UDP client-to-server
  message types are handled; 31 server-to-client message types are
  implemented.
- **Per-car state broadcast** — 10 Hz fast-rate (`0x1e`) and 1 Hz
  slow-rate (`0x39`) per-car broadcasts relayed to all other
  connections.
- **Session management** — configurable session sequence
  (Practice / Qualifying / Race) with automatic phase transitions,
  session timers, and session advance/restart via admin commands.
- **Leaderboard and standings** — real-time standings recomputation
  on lap completion, leaderboard broadcast (`0x36`), grid positions
  at race start (`0x3f`), session results at session end (`0x3e`).
- **Results file writer** — `results/YYMMDD_HHMMSS_<type>.json`
  written at session end, matching the stock server schema.
- **Admin commands** — full set via in-game chat or the stdin
  console: `/admin`, `/next`, `/restart`, `/kick`, `/ban`, `/dq`,
  time penalties (`/tp5`, `/tp15`, drive-through, stop-and-go),
  `/ballast`, `/restrictor`, `/clear`, `/connections`.
- **Penalty system** — per-car penalty queue, pit-speed enforcement
  from telemetry, mandatory pitstop tracking.
- **Weather** — deterministic sin/cos weather simulator with cloud
  and rain cycles, broadcast on significant change (`0x37`).
- **ServerMonitor protocol** — protobuf message builders for
  session state, car entries, connection entries, leaderboard,
  and realtime updates (available for future broadcasting
  protocol support).
- **Entry list** — `entrylist.json` reader populating car templates
  with driver info, ballast, restrictor, grid positions.
- **LAN discovery** — UDP 8999 broadcast response so clients on
  the same network find the server automatically.
- **Rating summary** — periodic `0x4e` per-connection broadcast.
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
- No Championship Points / CP rating system.
- Entry list enforcement is not yet implemented (any client is
  accepted regardless of `entrylist.json` content).

The clean-room protocol specification in
[`notebook-b/NOTEBOOK_B.md`](notebook-b/NOTEBOOK_B.md) documents
the full wire protocol: transport framing, string encodings,
connection state machine, all client and server message IDs with
byte-exact wire formats, and the ServerMonitor protobuf schema.

## Scope

- **In scope**: private-multiplayer gameplay against the stock ACC
  game client, running on Linux or OpenBSD, without Wine.
- **Out of scope**: Kunos's public lobby backend, the Community
  Competition rating / competition-point system, and anything that
  requires talking to Kunos infrastructure. This server is always
  `registerToLobby: 0`. Join is by direct IP via `serverList.json`
  on the client side.

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
│   ├── bcast.{c,h}          Broadcast helpers (tier-1 direct relay).
│   ├── chat.{c,h}           Admin chat commands + penalty dispatch.
│   ├── config.{c,h}         JSON config reader (UTF-16 LE or UTF-8).
│   ├── console.{c,h}        stdin admin console (poll-driven).
│   ├── dispatch.{c,h}       TCP / UDP message dispatchers.
│   ├── entrylist.{c,h}      entrylist.json reader.
│   ├── handlers.{c,h}       Per-msg-id handlers (21 TCP + 7 UDP).
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
│   ├── results.{c,h}        Session results JSON writer.
│   ├── session.{c,h}        Session phase machine + standings sort.
│   ├── state.{c,h}          Per-conn / global server state structs.
│   ├── tick.{c,h}           Periodic tick + leaderboard broadcasts.
│   ├── weather.{c,h}        Deterministic sin/cos weather simulator.
│   └── Makefile
├── debian/                  Debian/Ubuntu packaging.
├── redhat/                  Fedora/Rocky RPM spec.
└── .github/workflows/       CI: autorelease + multi-distro packaging.
```

The implementation is **24 modules and ~10,000 lines of portable
C99**, with no dependencies beyond libc, iconv, and libm.

## Building

The implementation (`accd/`) is portable C99 that builds with either
BSD or GNU `make`, using only libc + iconv + libm.

### Linux (glibc — iconv is in libc)

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
    "lanDiscovery": 1,
    "configVersion": 1
}
```

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
| 8999 | UDP | LAN discovery (optional, local network only) |

Ports 9232 and 9231 are configurable in `configuration.json`.

### Connecting from the ACC game client

On the client machine, create or edit the file:

```
%userprofile%\Documents\Assetto Corsa Competizione\Config\serverList.json
```

Example `serverList.json`:

```json
[
    {
        "Name": "My accd server",
        "Address": "192.168.1.100",
        "Port": 9232,
        "Password": "",
        "IsLan": false
    },
    {
        "Name": "LAN server",
        "Address": "10.0.0.5",
        "Port": 9232,
        "Password": "secret",
        "IsLan": true
    }
]
```

- `Address` -- the IP or hostname of the machine running accd.
- `Port` -- must match `tcpPort` in `configuration.json` (default 9232).
- `Password` -- must match the `password` field in the server's
  `settings.json`; empty string for open servers.
- `IsLan` -- set to `true` for servers on the same local network.

The servers will appear in the game's multiplayer server browser
under the "Favourites" tab.

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
