#!/bin/sh
# Slot-reuse race_archive / race-state leak regression.
#
# Regression test for v0.3.44 fix `7918fb5` (handshake: reset race
# state and archive when a new player takes a freed slot).
# Pre-fix flow:
#   1. Bot1 (race 911 -> steam_id S...911) connects, completes a
#      lap so its CarRaceState.lap_history is populated, disconnects.
#      conn_drop clears `used=0` but preserves the race state on
#      purpose so a same-steam zombie-slot reclaim works.
#   2. Bot2 (race 912 -> different steam_id) joins.  server_alloc_car
#      hands it the first `!used` slot — Bot1's slot.  No reset.
#   3. Bot2 sends 0x55 ACP_LOAD_SETUP.  The 0x56 reply walks
#      car->race.lap_history and emits Bot1's laps to Bot2.  Data
#      leak across players.
# Post-fix the handshake fresh-occupant branch wipes race + archive,
# so Bot2's 0x56 reports lap_count=0.
#
# Pure accd-side validation — kunos's slot-reuse behaviour isn't
# characterised in our notebooks, so we don't diff against it here.
# The check that matters is the byte at offset 4..5 of the 0x56
# body (i16 lap_count) being zero.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

ACCD=/home/r/code/assettocorsa/accd/accd
BOT=/home/r/code/assettocorsa/tools/bot/bot

mkdir -p log
rm -f cfg/current/*.json log/*.log accd.pcap accd.log bot*.log 2>/dev/null || true

PCAP_TMP=/tmp/penalty_diff_accd.pcap
rm -f "$PCAP_TMP"
sudo -n dumpcap -i lo -w "$PCAP_TMP" -f 'tcp port 9302 or udp port 9303' \
    -q >/dev/null 2>&1 &
TCPDUMP_PID=$!
sleep 1

"$ACCD" -c cfg >accd.log 2>&1 &
ACCD_PID=$!

for i in 1 2 3 4 5 6 7 8 9 10; do
    if ss -tlnp 2>/dev/null | grep -q ':9302'; then break; fi
    sleep 0.5
done

# BotKeep stays connected for the whole test so accd doesn't trigger
# the "no drivers, resetting to first session" auto-reset between
# Bot1's disconnect and Bot2's join.  Without it, session_reset()
# clears every car's race state via memset and we'd never observe
# the slot-reuse leak the handshake fix is supposed to plug.
"$BOT" --host 127.0.0.1 --tcp 9302 --race 999 --grid 4 \
    --name BotKeep \
    >botkeep.log 2>&1 &
BOTKEEP_PID=$!
sleep 1

echo "==> Bot1 (race 911) — drive two laps, then exit"
# --laps 2 widens the leak signal: Bot1 records two entries in
# lap_history (the immediate S/F-crossing "fake lap 1" at t=1s plus
# a real lap completed after circling the synthetic stadium loop).
# Bot2 then adds its own fake lap; without the slot-reuse fix Bot2's
# 0x56 reports lap_count=3 (2 inherited + 1 own), with the fix it
# reports lap_count=1 (own only).
"$BOT" --host 127.0.0.1 --tcp 9302 --race 911 --grid 1 \
    --name BotA --laps 2 \
    >bot1.log 2>&1 &
BOT1_PID=$!
# Stadium loop ≈ 1189 m at V_RACE — two laps should complete in
# ~40 s.  Give 90 s as a generous ceiling.
for _ in $(seq 1 180); do
    kill -0 $BOT1_PID 2>/dev/null || break
    sleep 0.5
done
kill -TERM $BOT1_PID 2>/dev/null || true
wait $BOT1_PID 2>/dev/null || true

# Give accd a moment to process Bot1's disconnect (conn_drop +
# slot release) before Bot2 starts its handshake.
sleep 2

echo "==> Bot2 (race 912, different steam_id) — load_setup at tick 60"
"$BOT" --host 127.0.0.1 --tcp 9302 --race 912 --grid 1 \
    --name BotB --load-setup 60:0 \
    >bot2.log 2>&1 &
BOT2_PID=$!
sleep 12
kill -TERM $BOT2_PID 2>/dev/null || true
wait $BOT2_PID 2>/dev/null || true

kill -TERM $BOTKEEP_PID 2>/dev/null || true
wait $BOTKEEP_PID 2>/dev/null || true

kill -TERM "$ACCD_PID" 2>/dev/null || true
wait "$ACCD_PID" 2>/dev/null || true

sudo -n kill -INT "$TCPDUMP_PID" 2>/dev/null || true
sleep 1
sudo -n chown "$(id -u):$(id -g)" "$PCAP_TMP" 2>/dev/null || true
mv "$PCAP_TMP" accd_slot_reuse.pcap

echo "==> walking pcap"
rm -f accd_slot_reuse.legacy.pcap
editcap -F pcap accd_slot_reuse.pcap accd_slot_reuse.legacy.pcap

python3 -c "
import struct, sys
sys.path.insert(0, '.')
from diff_pcap import reassemble_server_tx, walk_acc_frames

_, ab, _ = reassemble_server_tx('accd_slot_reuse.legacy.pcap', 9302)
af = [b for o,l,b in walk_acc_frames(ab) if b[0] == 0x56]
print(f'0x56 frames seen: {len(af)}')
if not af:
    print('FAIL: no 0x56 reply observed — Bot2 never reached its '
          'load_setup tick or its handshake was rejected')
    sys.exit(1)

# Wire: u8(0x56) u8(sess_type) u16(car_id) i16(lap_count) ...
body = af[-1]
sess_type = body[1]
car_id = struct.unpack('<H', body[2:4])[0]
lap_count = struct.unpack('<h', body[4:6])[0]
print(f'last 0x56: sess_type={sess_type} car_id={car_id} '
      f'lap_count={lap_count}')

# Bot2 itself crosses S/F right after its handshake and records a
# fake \"lap 1\" before the load_setup tick.  So with the fix in
# place the expected value is 1 (Bot2's own fake lap); without the
# fix Bot2 inherits Bot1's two entries and lands at 3.
if lap_count > 1:
    print(f'FAIL: lap_count={lap_count} — Bot1 history leaked into '
          f'Bot2 (race_archive / race state not reset on slot reuse)')
    sys.exit(2)

print('RESULT: PASS (fresh slot occupant sees only its own fake lap)')
"
