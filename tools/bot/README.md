# bot — protocol-test driving bot for ACC dedicated servers

A small UDP/TCP client that walks the Assetto Corsa Competizione
join sequence and drives a deterministic line on a real circuit
geometry.  Written for protocol/wire validation against
[accd](../../accd/) and against stock `accServer.exe` running
under Wine.

## What it is

- A single C99 source file (`bot.c`, ~1900 lines) plus two Python
  helpers for getting a racing line out of game data or out of a
  pcap recording.
- Performs the real handshake: TCP `0x09` request, parses the
  `0x0b` welcome trailer, then 30 Hz UDP `0x1e` car-update +
  on-demand `0x32` location packets with synthesised position,
  velocity and heading.
- Models enough of normal play to keep the server happy: formation
  lap (under 70 km/h), pit-lane (sub-22 m/s + `location=Pitlane`),
  per-sector `0x20` splits + lap-complete `0x21` at S/F (matching
  the kunos wire convention so the server's lap counter advances
  in P / Q / R), mandatory pit served (`0x54`), keepalive pong
  (`0x16`), reconnect with exponential backoff, mid-race join,
  damage zones (`0x43`), dirt (`0x45`), tyre compound (`0x2f`).
- Drives on a kinematic model: corner radius from the racing-line
  geometry, aero growth at speed, simple wear, bump recovery.  Each
  0x1e car-update carries content-varying input bytes derived from
  the per-tick physics state -- steering from yaw delta, throttle
  / brake from the v_target vs v_current gap, gear / rpm / fuel /
  damage from the lap + speed model, wheel-slip approximated from
  v_current/V_RACE.  Use `--zero-inputs` to opt back into the
  legacy all-zero behaviour for byte-diff parity with old pcaps.

## What it isn't

- **Not a cheat tool.**  The kinematic model targets *staying on
  the line and not invalidating laps*, not minimising sector
  times.  Lap times are typically 110–120 % of human pace.  Top
  drivers' times are out of reach by design.
- **Not a Steam impersonator.**  The bot can claim any name and
  Steam ID, but public Kunos-listed servers verify a Steam auth
  ticket the bot doesn't carry, so they reject the join.  Useful
  for your own private/LAN server, not for getting on someone
  else's lobby pretending to be them.
- **Not subtle.**  Telemetry analysis would flag it instantly:
  nobody's brake/throttle traces look like a kinematic model's.

## Build

```
make
```

C99 and `libm` are the only requirements.  Tested on Linux (glibc)
and OpenBSD.

## Usage

```
./bot --host 127.0.0.1 --tcp 9232 \
      --race 911 --name BotOne \
      --track waypoints/brands_hatch.csv \
      --laps 5
```

Flags:

| Flag | Default | Meaning |
|------|---------|---------|
| `--host` H | (required) | server hostname or IP |
| `--tcp` P | `9232` | TCP port |
| `--race` N | `911` | race number on the entry list |
| `--name` S | `Bot` | driver first name |
| `--track` FILE | none | CSV waypoints `norm_pos x y z [speed]`; without one the bot drives a synthetic stadium loop |
| `--length` M | derived | override track length in metres (otherwise sums waypoint distances) |
| `--pit-on-lap` N | never | enter pit on lap N (1-based) |
| `--laps` N | infinite | quit after N completed laps |
| `--grid` N | `1` | grid position, 1 = pole |
| `--mid-race` | off | join an in-progress race, skip formation |
| `--no-mandatory-pit` | off | skip the `0x54` after pit traversal |
| `--bump` M, `--bump-at-lap` N | off | one-shot lateral kick, in metres, at the start of lap N |

Test-only knobs the wire-level integration tests under
`accd/tests/integration/` rely on:

| Flag | Meaning |
|------|---------|
| `--client-version` N | override the protocol version sent in `0x09` (default `0x0100`) |
| `--password` S | server password to send in handshake |
| `--expect-reject` | exit cleanly on `0x0c` reject (used by reject-code matrix tests) |
| `--chat-start-tick` N, `--chat` S | inject one-shot chat at tick N (underscores → spaces) |
| `--report-penalty` C:K:V | self-report `0x41` ACP_REPORT_PENALTY (category, kind, value) |
| `--penalty-start-tick` N | delay the first `0x41` until tick N (needed for PHASE_SESSION gate) |
| `--send-penalty-served` | emit `0x42` after pit traversal |
| `--stint-reset` T:F | emit `0x4f` driver-stint reset at tick T with force flag F |
| `--load-setup` T:S | request `0x55` garage setup load at tick T, slot S |
| `--swap-state` T:S[,S2...] | emit `0x47` driver-swap state at tick T |
| `--swap-request` T:sub:state | emit `0x4a` driver-swap state request |
| `--damage` T:z1,z2,z3,z4,z5 | emit `0x43` damage zones at tick T |
| `--pit-speed` M | override pit-lane speed cap (m/s; default 18, server pit-speeding DQ fires above 22.22) |
| `--sa-contact` T:target:quality | emit `0x19` SA-contact at tick T (target = race number, 0xffff = wall hit) |
| `--oot-at` T | emit `0x3d` out-of-track at tick T (force=0) |
| `--time-event` T | emit `0x5e` UDP latency event at tick T (loopback to self, chat=1) |
| `--flap-at` N | force a TCP close at tick N (exercises reconnect cascade) |
| `--zero-inputs` | emit legacy all-zero input / rpm / gear / fuel / damage bytes in every 0x1e car-update; default is the realistic-content encoding |

## Getting a racing line

The bot needs a CSV waypoint file shaped `norm_pos x y z [speed]`
to drive a real track.  No racing-line data ships with this
repository — Assetto Corsa Competizione's track data is Kunos's
copyrighted material, and our internal test pcaps are
protocol-probing sessions where the bot-driven trajectory is not a
representative racing line.  Two ways to produce a CSV yourself:

1. **From a local ACC install** (Windows or via a Wine prefix
   that has the game installed), use `parse_ai.py` on the source
   `fastlane.ai`:

   ```
   python3 parse_ai.py \
       "$ACC_INSTALL_DIR/Content/Tracks/brands_hatch/data/fastlane.ai" \
       brands_hatch.csv
   ```

2. **From a pcap of a race**, use `extract_racing_line.py` on a
   capture that includes UDP `0x1e ACP_CAR_UPDATE` packets:

   ```
   tcpdump -w race.pcap -i any 'port 9232'
   # ... run a real race or a recorded session ...
   python3 extract_racing_line.py race.pcap waypoints/
   ```

   This produces one CSV per `car_id` observed.

The CSV format is whitespace-separated, one waypoint per line,
sorted by `norm_pos` ascending.  The first column wraps from 0 to
~1; the bot rebuilds a closed polyline on load.

## Sample server config

`cfg/event.json` is a minimal one-session config you can drop into
`accd/cfg/` (or onto a stock `accServer.exe` install) so the bot
has something to connect to.  Edit the `track` field to match
whatever waypoint CSV you generated.

## Source layout

```
bot.c                 main driver (single TU, ~1900 lines)
parse_ai.py           Kunos .ai → bot CSV
extract_racing_line.py  pcap → bot CSV
Makefile              `make` builds bot
cfg/event.json        sample server config
```

## License

2-clause BSD — same as accd.

## Ethics

This is a protocol-testing tool.  Run it against your own server.
Pointing it at someone else's server to spam connections, fill
slots so real players can't join, or pretend to be a specific
named player ranges from rude to illegal depending on
jurisdiction.  Don't.
