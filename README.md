<p align="center">
  <img src="logo.svg" alt="accd" width="640"/>
</p>

# accd — ACC dedicated server, clean-room reimplementation

A work-in-progress independent reimplementation of the Assetto Corsa
Competizione dedicated server, built so an unmodified ACC game client
(Steam, current build) can connect to it and play a private multiplayer
session — on Linux and OpenBSD, without Wine.

## Status

**Phases 1–11 complete; phase 12 (real-client testing under Wine) is
the only outstanding task.**

The clean-room specification in
[`notebook-b/NOTEBOOK_B.md`](notebook-b/NOTEBOOK_B.md) documents:

- Transport framing for both TCP (variable-width length prefix,
  `u16` short or `0xFFFF + u32` extended) and UDP (datagram = message).
- The scalar wire types, byte order, and the two distinct string
  encodings (`u8` length + UTF-32 padded for short identifiers,
  `u16` length + raw UTF-16 LE for longer text).
- The client connection state machine (`0 → 1 → 3`) and the
  handshake request body (client version, password, embedded
  `CarInfo` and `DriverInfo`).
- The complete client → server message ID catalog — 22 TCP IDs,
  7 UDP IDs, plus LAN discovery — with known wire formats and
  semantic labels for every case.
- **31 server → client message IDs** with byte-exact wire formats,
  including the welcome trailer, per-tick broadcasters, leaderboard
  / weather / session results / grid positions / ratings, the
  driver swap state machine, BoP updates, setup data responses,
  chat broadcasts, and the welcome trailer redelivery.
- The ServerMonitor protobuf protocol (msg types 1–7), confirmed
  to be the same data carried by sim-protocol ids `0x01`–`0x07`.

The `accd/` C implementation now covers:

- **Phase 1** — TCP framing, primitive readers/writers, full
  handshake with real `0x0b` response.
- **Phase 2–3** — every TCP and UDP dispatcher case wired to a
  real handler (~21 TCP + 7 UDP), per-tick `0x1e`/`0x39` per-car
  broadcast fan-out, `0x14` keepalive heartbeat, `0x24` disconnect
  notify, `0x5f` admin query response, `0xc0` LAN discovery reply.
- **Phase 4** — hand-rolled JSON parser (recursive-descent,
  bump-allocated tree), hand-rolled protobuf encoder (varint,
  length-delimited, fixed32, submessage backpatch), and seven
  `ServerMonitor*` message builders (`HandshakeResult`, `CarEntry`,
  `ConnectionEntry`, `SessionState`, `ConfigurationState`,
  `RealtimeUpdate`, `Leaderboard`).  The post-handshake welcome
  push sequence `0x04` + `0x05` + `0x03` + `0x07` is emitted to
  the joining client automatically.
- **Phase 5** — session phase state machine
  (`PRE_SESSION → STARTING → PRACTICE / QUALIFYING / PRE_RACE →
  RACE → POST_SESSION → RESULTS`), driven from the tick loop with
  durations loaded from `event.json`.
- **Phase 6** — per-car `CarRaceState` (laps, position, sectors,
  best lap), lap-completion mutations, standings recomputation,
  `0x36` leaderboard broadcast on change, `0x3f` grid positions at
  race countdown, `0x3e` session results at session end.
- **Phase 7** — `results/YYMMDD_HHMMSS_<type>.json` writer at
  session end, schema matching `§9` of the clean-room spec.
- **Phase 8** — real admin chat commands: `/admin`, `/next`,
  `/restart`, `/kick`, `/ban` (with `0x2b` kick/ban notification),
  `/dq`, `/clear`, `/clear_all`, `/cleartp`, the time-penalty
  family (`/tp5`, `/tp5c`, `/tp15`, `/tp15c`, `/dt`, `/dtc`, and
  stop-and-go), `/ballast` and `/restrictor` with `0x53`
  `MultiplayerBOPUpdate` broadcast, `/connections`, `/legacy`,
  `/regular`.
- **Phase 9** — penalty queue per car, pit-speeding auto-detection
  from `ACP_CAR_UPDATE` velocity, mandatory-pitstop tracking.
- **Phase 10** — deterministic weather simulator (sin/cos rain and
  cloud cycle modeled on the algorithm in the binary's weather
  function), `0x37` weather broadcast on significant change.
- **Phase 11** — `entrylist.json` reader populating `Server.cars[]`
  as entry templates, `0x56` setup data response, `0x4e` per-
  connection rating summary, persistent kick/ban list in
  `cfg/banlist.txt`.

The implementation is **23 modules and ~7500 lines of portable
C99**, builds clean under `gcc -Wall -Wextra -Wpedantic -O2` on
Linux and under `clang -Wall -Wextra -Wpedantic -O2` on OpenBSD
7.8 arm64.  The OpenBSD build was verified end-to-end on a real
host — handshake + welcome push produces byte-identical wire
output to the Linux build.  On OpenBSD the process pledges to
`stdio inet` after binding its listening ports.

The one remaining task (phase 12) is to run the server against a
real ACC game client under Wine to verify the ServerMonitor
protobuf field numbers and the welcome trailer layout.  Until
that happens, field numbers in `monitor.h` are a best-guess from
the catalog and may need small adjustments once a real client
tries to decode them.

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
├── README.md              This file.
├── NOTEBOOK_B.md          — see notebook-b/
├── notebook-b/
│   └── NOTEBOOK_B.md      The public clean-room protocol spec.
├── accd/                  The C implementation.
│   ├── main.c             Poll loop + signal handling + lifecycle.
│   ├── bans.{c,h}         Persistent kick / ban list.
│   ├── bcast.{c,h}        Broadcast helpers (tier-1 direct relay).
│   ├── chat.{c,h}         Admin chat commands + penalty dispatch.
│   ├── config.{c,h}       UTF-16 LE JSON config reader.
│   ├── dispatch.{c,h}     TCP / UDP message dispatchers.
│   ├── entrylist.{c,h}    entrylist.json reader.
│   ├── handlers.{c,h}     Per-msg-id handlers (21 TCP + 7 UDP).
│   ├── handshake.{c,h}    ACP_REQUEST_CONNECTION + 0x0b + welcome.
│   ├── io.{c,h}           Byte buffer + TCP framing layer.
│   ├── json.{c,h}         Recursive-descent JSON parser.
│   ├── lan.{c,h}          UDP 8999 LAN discovery handler.
│   ├── log.{c,h}          Timestamped logger + hexdump.
│   ├── monitor.{c,h}      ServerMonitor protobuf message builders.
│   ├── msg.h              All message id constants + enums.
│   ├── net.{c,h}          tcp_listen / udp_bind helpers.
│   ├── pb.{c,h}           Minimal write-only protobuf encoder.
│   ├── penalty.{c,h}      Per-car penalty queue.
│   ├── prim.{c,h}         Primitive readers / writers + strings.
│   ├── results.{c,h}      Session results JSON writer.
│   ├── session.{c,h}      Session phase machine + standings sort.
│   ├── state.{c,h}        Per-conn / global server state structs.
│   ├── tick.{c,h}         Periodic tick + leaderboard broadcasts.
│   ├── weather.{c,h}      Deterministic sin/cos weather simulator.
│   ├── Makefile
│   └── README
├── .gitignore
├── acc-server/            The Kunos server download. Gitignored.
│                          Fetch it yourself via SteamCMD (below);
│                          you will need this locally to test the
│                          reimplementation against a real client.
└── tmp/                   Scratch directory. Gitignored.
```

## Building

The implementation (`accd/`) is portable C99 that builds with either
BSD or GNU `make`, using only libc + iconv + libm.

### Linux (glibc — iconv is in libc)

```sh
cd accd
make
```

Tested on Debian sid aarch64 with `gcc 15.2.0`.

### OpenBSD (iconv lives in ports at `/usr/local`)

```sh
cd accd
make CFLAGS="-I/usr/local/include" \
     LDFLAGS="-L/usr/local/lib" LIBS="-liconv"
```

Tested on OpenBSD 7.8 arm64 with `clang 19.1.7`.  The build is
byte-identical in output to the Linux build: the same 189-byte
handshake response + welcome push sequence is produced for the
same synthetic client input.

On OpenBSD the process pledges to `stdio inet` after binding its
listening ports.

## Running

`accd` expects a `cfg/` directory containing `configuration.json`
and (optionally) `settings.json`, `event.json`, `entrylist.json`.
Each file may be either UTF-16 LE with a BOM (the format
`accServer.exe` writes) or plain UTF-8 (so the files can be
hand-edited in any normal text editor on Linux or OpenBSD);
detection is automatic.  Pass the cfg directory as the first
argument, or run from a directory that contains it:

```sh
./accd ../acc-server/server/cfg
```

To fetch the stock Kunos cfg files (you need a Steam account that
owns Assetto Corsa Competizione):

```sh
steamcmd +@sSteamCmdForcePlatformType windows \
         +force_install_dir /path/to/acc-server \
         +login <your steam username> \
         +app_update 1430110 validate +quit
```

The server prints the parsed config, binds the configured ports,
runs a 10 Hz poll/tick loop, accepts TCP connections, parses any
framed handshake message, and replies with the 0x0b response.
Every other message is logged but not yet processed.

### Quick smoke test (no real client)

Reject path — wrong protocol version:

```sh
printf '\x03\x00\x09\x99\x00' | nc -q 1 127.0.0.1 9232 | xxd
# expects: 07 00 0b 00 01 00 ff ff 01
```

Accept path — empty server password:

```sh
printf '\x04\x00\x09\x00\x01\x00' | nc -q 1 127.0.0.1 9232 | xxd
# expects: 29 00 0b 00 01 00 00 00 00 00 00 00 ...
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

TBD. The intent is a permissive open-source license (e.g. ISC /
BSD-2-Clause) on all code the project publishes, to match the
OpenBSD ecosystem expectations. The clean-room specification text
is authored for public reference.

Nothing in this repository is authored, endorsed, or licensed by
Kunos Simulazioni or 505 Games.
