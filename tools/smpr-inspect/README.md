# smpr-inspect

A drop-in CLI client for the ServerMonitor protobuf side-channel
that `accd` exposes on its gameplay TCP port.

```
$ smpr-inspect.py 127.0.0.1:9302 --interval 250
14:42:26  REGISTRATION_RESULT success=True conn=1 err=''
14:42:26  SERVER_CONFIGURATION name='race-night-3' track='misano' max=12 sessions=3
14:42:26  SESSION_STATE session=0 weekend_s=50400 cars=4 amb=22C road=30C clouds=0.3 rain=0.0
14:42:26  CAR_ENTRY car=1001 model=35 drv_conn=0 race#=911 cup=0
14:42:26  CAR_ENTRY car=1002 model=23 drv_conn=1 race#=7 cup=2
14:42:26  CONNECTION_ENTRY conn=0 name='Alice' 'Wonderland' steam=S76561199000000911 admin=False spectator=False
14:42:26  CONNECTION_ENTRY conn=1 name='Bob' 'Theron' steam=S76561199000000777 admin=False spectator=False
14:42:26  REALTIME_UPDATE now=50400000ms cars=0
14:42:27  LEADERBOARD bestLap=0ms entries=0
14:42:27  REALTIME_UPDATE now=50400250ms cars=0
...
```

## What it is

A standalone Python 3 script with **no third-party dependencies**
(no protobuf library, no requests, nothing — just `socket`,
`struct`, `json`, `time`).  Speaks accd's SMPR demux (first body
byte `0x0a`), connects, sends a hand-built
`ServerMonitorConnectionRequest`, and walks the framed protobuf
reply stream decoding the seven message types defined in
`accd/monitor.h`.

## Usage

```
smpr-inspect.py HOST:PORT [options]

  -i, --interval MS         REALTIME_UPDATE cadence in ms
                            (server clamps to [50, 10000]; default 250)
  -o, --output text|json|raw  output mode (default: text)
  -t, --seconds N           stop after N seconds (0 = forever)
  -n, --name NAME           display name in ConnectionRequest
  --self-contained          set sendSelfcontainingLeaderboards=true
  --extended                set sendExtendedLeaderboards=true
  -h, --help
```

### Output modes

- **`text`** (default) — one event per line, summary fields, easy
  to `grep` or `tail`.
- **`json`** — one NDJSON object per event, all fields included.
  Pipe to `jq`, ship to Loki / Promtail, drop into a Kafka topic,
  etc.
- **`raw`** — full decoded structure per event, pretty-printed.
  Best for debugging unknown / new fields.

### Typical use cases

```sh
# Quick "is the server alive and what's happening" peek.
smpr-inspect.py acc.example.com:9232

# Capture 60 s of telemetry to a file as NDJSON for offline
# analysis (one event per line).
smpr-inspect.py acc.example.com:9232 -o json -t 60 > race.ndjson

# Watch raw decoded fields while developing a custom dashboard.
smpr-inspect.py 127.0.0.1:9302 -o raw

# Pipe to jq to filter for only leaderboard updates.
smpr-inspect.py acc.example.com:9232 -o json | \
    jq -c 'select(.type=="Leaderboard")'

# Build a Prometheus exporter, a Slack bot, a chat overlay --
# the JSON line per event is the universal API.
smpr-inspect.py acc.example.com:9232 -o json | \
    python3 my_dashboard.py
```

## Requirements

Python 3.7+ (stdlib only).  No `pip install` needed.

## Compatibility

| Server | Works? |
|---|---|
| `accd` >= v0.3.55 (with SMPR support) | ✅ |
| Stock kunos `accServer.exe` | ❌ |

The kunos exe uses a different SMPR demux mechanism (a per-conn
flag set inside the sim handshake handler on an unconfirmed string
match — see `notebook-a/decomp/full/140041480.c`).  `smpr-inspect`
expects accd's `0x0a` first-byte demux specifically.  See
`memory:reference_smpr_ecosystem_audit.md` in the project memory
for the full audit.

## Message types decoded

All seven from kunos's `acc_server_protocol.proto v1` schema (and
`accd/monitor.h`):

| ID | Type | Emitted when |
|---|---|---|
| `0x01` | RegistrationResult | once, immediately after the ConnectionRequest |
| `0x02` | ServerConfiguration | once at registration, includes session list |
| `0x03` | SessionState | once at registration, plus inside every `0x06` |
| `0x04` | CarEntry | per car at registration, plus on join / leave |
| `0x05` | ConnectionEntry | per driver at registration, plus on join / leave |
| `0x06` | RealtimeUpdate | every `--interval` ms while the inspector is connected |
| `0x07` | Leaderboard | after every server-side leaderboard refresh |

## Caveats

- accd's `monitor_build_realtime_update` populates the `serverNow`
  timestamp, the `sessionState` sub, and the per-conn (`ConnectionEntry`)
  and per-car (`CarEntry`) submessages inside `0x06`; the `cars=N` field
  in the text output reflects the live car count.  (Earlier builds left
  the conn/car subs empty and always printed `cars=0`; that is fixed.)
- The inspector consumes one connection slot from accd's Conn
  pool (shared with sim clients).  On busy servers (many sim
  clients + many monitors), `maxConnections` may need bumping.
- accweb / accservermanager / emperorservers do NOT speak this
  protocol — they scrape stdout instead.  See
  `tools/accweb-bridge/` for the accweb integration recipe.

## Testing

The end-to-end smoke test that exercises the same code path lives
at `accd/tests/integration/run_smpr.sh`.  Running it locally
should show the same 7-message sequence the inspector prints.
