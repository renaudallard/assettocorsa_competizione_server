#!/bin/sh
# spectatorPassword handshake regression.  When the client sends the
# spectator password in the 0x09 handshake, accd's handshake.c:1882
# sets c->is_spectator=1, gives the conn no car slot, and emits the
# "handshake: spectator join" log line.  /connections then shows the
# peer with the "[spectator]" tag.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

ACCD=/home/r/code/assettocorsa/accd/accd
BOT=/home/r/code/assettocorsa/tools/bot/bot

ACCD_PID=""
BOT_PIDS=""
cleanup_on_exit() {
    rc=$?
    [ -n "$BOT_PIDS" ] && for pid in $BOT_PIDS; do kill -TERM "$pid" 2>/dev/null || :; done
    [ -n "$ACCD_PID" ] && kill -TERM "$ACCD_PID" 2>/dev/null || :
    for f in cfg/settings.json.bak; do
        [ -f "$f" ] || continue
        mv "$f" "${f%.bak}" || true
    done
    exit $rc
}
trap cleanup_on_exit INT TERM HUP EXIT
for f in cfg/*.bak; do
    [ -f "$f" ] || continue
    mv "$f" "${f%.bak}"
done

pkill -KILL -f 'accd -c '       >/dev/null 2>&1 || true
pkill -KILL -f 'tools/bot/bot ' >/dev/null 2>&1 || true
sleep 1

cp cfg/settings.json cfg/settings.json.bak
python3 -c "
import json
o = json.load(open('cfg/settings.json'))
o['spectatorPassword'] = 'spectate'
json.dump(o, open('cfg/settings.json', 'w'), indent=4)
print('  cfg/settings.json: spectatorPassword = spectate')
"

rm -f accd.log bot1.log bot2.log
$ACCD -c cfg >accd.log 2>&1 &
ACCD_PID=$!
for i in 1 2 3 4 5; do
    if ss -tln 2>/dev/null | grep -q ':9302'; then break; fi
    sleep 0.3
done

echo "==> bot1 joins normally"
"$BOT" --host 127.0.0.1 --tcp 9302 \
    --race 911 --grid 1 --name "Driver" >bot1.log 2>&1 &
BOT_PIDS="$BOT_PIDS $!"
sleep 2

echo "==> bot2 joins with spectator password"
"$BOT" --host 127.0.0.1 --tcp 9302 \
    --race 999 --grid 9 --name "Spec" \
    --password spectate >bot2.log 2>&1 &
BOT_PIDS="$BOT_PIDS $!"

sleep 5
for pid in $BOT_PIDS; do kill -TERM "$pid" 2>/dev/null || true; done
kill -TERM "$ACCD_PID" 2>/dev/null || true
wait "$ACCD_PID" 2>/dev/null || true

if ! grep -q 'spectator join' accd.log; then
    echo "FAIL: no 'spectator join' log line"
    grep -E 'spectator|handshake' accd.log | head -5 >&2
    exit 1
fi
echo "  handshake: spectator join logged"

# bot2 should also receive a 0x0b welcome (spectators still get one).
if ! grep -q 'welcome ok' bot2.log; then
    echo "FAIL: spectator bot did not receive 0x0b welcome"
    tail -5 bot2.log >&2
    exit 2
fi
echo "  spectator received welcome trailer"

echo "RESULT: PASS (spectatorPassword handshake path accepts the join)"
