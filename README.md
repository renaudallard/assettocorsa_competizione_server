# Assetto Corsa Competizione dedicated server — clean-room reimplementation

A work-in-progress independent reimplementation of the Assetto Corsa
Competizione dedicated server, built so an unmodified ACC game client
(Steam, current build) can connect to it and play a private multiplayer
session — on Linux and OpenBSD, without Wine.

**Status**: very early. Phase 0 skeleton binds the configured TCP and
UDP ports and logs every byte received. No protocol logic yet. See
[`notebook-b/NOTEBOOK_B.md`](notebook-b/NOTEBOOK_B.md) for the protocol
specification this code is working towards.

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
│   ├── main.c
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

`accd` expects to find `cfg/configuration.json` (UTF-16 LE, the
format Kunos uses) in its current working directory, matching the
Kunos server's layout. The simplest way to run it against a real
configuration is from inside the Kunos server's `server/` directory,
which you will need to fetch yourself:

```sh
# One-time fetch of the Kunos ACC dedicated server tool. You need a
# Steam account that owns Assetto Corsa Competizione.
steamcmd +@sSteamCmdForcePlatformType windows \
         +force_install_dir /path/to/acc-server \
         +login <your steam username> \
         +app_update 1430110 validate +quit
```

Then:

```sh
cd /path/to/acc-server/server
/path/to/accd/accd
```

It will print the parsed config, bind the configured ports, and
log every byte received until interrupted (Ctrl-C).

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
