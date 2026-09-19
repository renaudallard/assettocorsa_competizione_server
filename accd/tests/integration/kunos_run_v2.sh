#!/bin/sh
# Run the stock accServer.exe under wine plus N bots with arbitrary args.
#
# This is the kunos half of every paired test and it runs ON THE WINE VM,
# not here.  The copy in this directory is the source of truth; deploy it
# with
#
#   scp accd/tests/integration/kunos_run_v2.sh accd@172.20.0.66:~/wine-test/
#
# Usage: ./kunos_run_v2.sh "bot1_args" ["bot2_args" ...]
# Bot args are space-separated; --host and --tcp come from the defaults
# below.  Do not add a --track here: the accd half (run_test_v2.sh) drives
# the bot's default synthetic loop, and a real racing line on this side
# only makes the two halves cover different ground.
#
# Environment:
#   TEST_DURATION  seconds to hold the session open once every bot is
#                  seated (default 30)
#   BOT_DELAYS     per-bot start delay in seconds, one value per bot;
#                  see the launch loop
set -e
HERE=/home/accd/wine-test
cd "$HERE"

rm -f cfg/current/*.json log/*.log results/*.json kunos.pcap kunos.log bot*.log 2>/dev/null || true

PCAP_TMP=/tmp/kunos_run.pcap
rm -f "$PCAP_TMP"
sudo -n tcpdump -i lo -w "$PCAP_TMP" 'tcp port 19298 or udp port 19299' &
TCPDUMP_PID=$!
sleep 1

export WINEDEBUG=-all
wine ./accServer.exe >kunos.log 2>&1 &
WINE_PID=$!

for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
    if ss -tlnp 2>/dev/null | grep -q ':19298'; then break; fi
    sleep 0.5
done

BOT_PIDS=""
i=0
for botargs in "$@"; do
    i=$((i + 1))
    # BOT_DELAYS carries one value per bot: seconds to wait before
    # launching that bot, so BOT_DELAYS="0 85" seats the first right
    # away and the second 85 s later.  Missing entries keep the 0.3 s
    # stagger below.  Mirrors run_test_v2.sh on the accd side.
    bot_delay=""
    n=0
    for v in ${BOT_DELAYS:-}; do
        n=$((n + 1))
        if [ "$n" -eq "$i" ]; then
            bot_delay=$v
        fi
    done
    if [ -n "$bot_delay" ]; then
        sleep "$bot_delay"
    fi
    ./bot --host 127.0.0.1 --tcp 19298 $botargs \
        >bot$i.log 2>&1 &
    BOT_PIDS="$BOT_PIDS $!"
    sleep 0.3   # stagger so handshakes don't race
done

sleep "${TEST_DURATION:-30}"
# Stagger bot disconnects.
for pid in $BOT_PIDS; do
    kill -TERM $pid 2>/dev/null || true
    sleep 0.5
done
for pid in $BOT_PIDS; do
    wait $pid 2>/dev/null || true
done

sudo -n pkill -TERM -f accServer.exe 2>/dev/null || true
sleep 1
sudo -n pkill -KILL -f accServer.exe 2>/dev/null || true
wait "$WINE_PID" 2>/dev/null || true

sudo -n pkill -INT -f "tcpdump -i lo -w $PCAP_TMP" 2>/dev/null || true
wait "$TCPDUMP_PID" 2>/dev/null || true
sleep 1
sudo -n cp "$PCAP_TMP" kunos.pcap
sudo -n chown "$(id -u):$(id -g)" kunos.pcap

ls -la kunos.pcap kunos.log bot*.log
