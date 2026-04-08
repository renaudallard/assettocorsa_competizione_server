<p align="center">
  <img src="logo.svg" alt="accd" width="640"/>
</p>

# accd — ACC dedicated server, clean-room reimplementation

A work-in-progress independent reimplementation of the Assetto Corsa
Competizione dedicated server, built so an unmodified ACC game client
(Steam, current build) can connect to it and play a private multiplayer
session — on Linux and OpenBSD, without Wine.

## Status

**Phase 1 — handshake works end-to-end.**  The clean-room
specification in [`notebook-b/NOTEBOOK_B.md`](notebook-b/NOTEBOOK_B.md)
now documents:

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

**The `accd/` C implementation is at phase 1**: it reads the stock
Kunos config files, binds the TCP and UDP listeners, accepts
framed TCP messages, parses `ACP_REQUEST_CONNECTION`, and replies
with a real `0x0b` handshake response containing either a rejection
code or a minimum-viable welcome trailer.  Every other TCP and UDP
message id has a dispatch stub that logs the id and length but
does not yet mutate state.

The implementation is split into 13 modules totalling about
2700 lines of portable C99, builds clean under `gcc -Wall -Wextra
-Wpedantic`, and pledges `stdio inet` on OpenBSD after binding.

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
├── accd/                  The C implementation (work in progress).
│   ├── main.c             Poll loop + signal handling + lifecycle.
│   ├── config.{c,h}       UTF-16 LE JSON config reader.
│   ├── state.{c,h}        Per-conn / global server state structs.
│   ├── msg.h              All 53 message id constants + enums.
│   ├── io.{c,h}           Byte buffer + TCP framing layer.
│   ├── prim.{c,h}         Primitive readers / writers + strings.
│   ├── net.{c,h}          tcp_listen / udp_bind helpers.
│   ├── log.{c,h}          Timestamped logger + hexdump.
│   ├── handshake.{c,h}    ACP_REQUEST_CONNECTION + 0x0b response.
│   ├── dispatch.{c,h}     TCP / UDP message dispatchers.
│   ├── chat.{c,h}         Admin chat command parser skeleton.
│   ├── tick.{c,h}         Periodic server tick stub.
│   ├── lan.{c,h}          UDP 8999 LAN discovery handler.
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
BSD or GNU `make`, using only libc (iconv is in libc on both Linux
glibc and OpenBSD).

```sh
cd accd
make
```

Tested on Debian sid aarch64.  Intended to build unchanged on
OpenBSD; please file an issue if it does not.

On OpenBSD the process pledges to `stdio inet` after binding its
listening ports.

## Running

`accd` expects a `cfg/` directory containing `configuration.json`
and (optionally) `settings.json` + `event.json`, all UTF-16 LE per
the Kunos server layout.  Pass the cfg directory as the first
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
