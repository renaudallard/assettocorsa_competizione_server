# accweb-bridge

Run [accweb](https://github.com/assetto-corsa-web/accweb) on top of
`accd` so the existing web dashboard can manage and monitor an
accd-hosted ACC server without code changes.

## Why this exists

`accweb` is hard-coded to launch a file named `accServer.exe` from
each server-instance directory and read its stdout with regex
matchers (`internal/pkg/server_manager/events/logparser/logparser.go`).
The wrapper here is a shell script that *is* named `accServer.exe`
so accweb's launcher invokes it.  The wrapper re-execs `accd` with
the right flags and lets accd's existing kunos-compatible stdout
banners (`log_kunos()` in `accd/log.c`) flow back into accweb's
event pipeline.

Result: every event accweb's parser knows about — server start,
session phase changes, connections, laps, chat, leaderboard
updates, disconnects — fires correctly against accd.

## One-time accweb setup

1. Install accd somewhere on `PATH` (`make install` puts it at
   `/usr/local/bin/accd`) or in a known location the wrapper
   probes (see `ACCD` env var below).
2. In accweb's own configuration, set `SkipWine: true` for the
   instance(s) you want to back with accd.  Without this accweb
   prepends `wine` to the launch command, which won't run a shell
   script.
3. Optional: if you build a release tarball of accweb instances
   for distribution, drop a copy of this wrapper into each one.

## Per-instance setup

For each accweb server instance directory (the path accweb stores
config JSON in):

```sh
# 1. Install the bridge as the "binary" accweb launches.
ln -s /usr/local/share/accd/accweb-bridge/accServer.exe accServer.exe

# 2. Accweb already writes configuration.json, settings.json,
#    event.json, entrylist.json, eventRules.json, assistRules.json,
#    and bop.json directly in the instance dir -- that's exactly
#    where accd's -c . flag expects them.  No conversion needed.

# 3. Start the instance from the accweb web UI.  accweb runs
#    ./accServer.exe (i.e. our wrapper), which execs accd -c . and
#    streams the kunos-format log lines back over stdout.
```

## ACCD env var (optional)

If `accd` is not on `PATH` and not in any of the locations the
wrapper probes (`/usr/local/bin/accd`, `/usr/bin/accd`, alongside
the wrapper, or two dirs up under `accd/accd`), set `ACCD` before
launching accweb.  In a systemd unit:

```ini
[Service]
Environment=ACCD=/opt/accd/bin/accd
ExecStart=/opt/accweb/accweb
```

## Verifying it works

Once accweb starts the instance:

```sh
# accweb writes its captured stdout to log/server.log in the
# instance dir (helper at internal/pkg/instance/instance.go:247).
tail -F path/to/accweb/instance/log/server.log
```

You should see the kunos-format banners — `Server starting with
version 256`, `Track <name> was set and updated`, etc.  In the
accweb web UI the live-state panels (cars, drivers, chat,
leaderboard, lap times) populate as drivers connect and run.

## What's NOT covered

- **Remote upgrade**.  accweb has an `UpdateAccServerExe` action
  that copies a new `accServer.exe` over the instance dir.  Don't
  use that — it would overwrite this wrapper.  Update accd via
  your distro's package manager or `make install`.
- **md5 checksums**.  accweb computes the wrapper's md5 and records
  it (`instance.go:204`).  This is harmless: accweb just stores
  the sum, it doesn't enforce a specific value.

## Caveats

- accweb expects the working directory to be the instance dir.
  Our wrapper passes `-c .` to accd so cfg files in cwd are
  picked up.
- accweb decodes stdout as UTF-16 LE when it detects a BOM
  (`instance.go:368-380`).  accd writes plain UTF-8 with no BOM, so
  accweb falls through to UTF-8 — works.
- The `Received Ping spike` log line (one of the 18 patterns in
  accweb's parser) is not implemented in accd, so the ping-spike
  panel will stay empty.  Every other panel works.
