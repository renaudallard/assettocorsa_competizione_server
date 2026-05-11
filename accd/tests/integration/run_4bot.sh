#!/bin/sh
# 4-bot scenario: bot1 cycles every (cat, kind) penalty combo,
# bots 2-4 drive clean.  Run against both servers, capture pcaps.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

# Penalty bot1 args: send all 52 dispatcher-defined (cat, kind) pairs
# at value=3.  Stagger 1 s apart starting at tick 200 (10 s in).
RP=""
for combo in \
    0:1 0:2 0:3 0:4 0:5 0:6 0:7 \
    1:5 \
    2:5 \
    3:1 3:2 3:3 3:4 3:5 3:6 3:7 \
    4:5 4:6 \
    5:5 5:6 \
    6:5 6:6 \
    7:5 \
    8:1 8:2 8:3 8:4 8:5 8:6 \
    9:5 \
    10:5 10:6 \
    11:1 11:4 11:5 11:6 \
    12:5 12:6 \
    13:4 13:5 \
    14:5 14:6 \
    15:5 15:6 \
    16:1 16:4 16:5 16:6 \
    17:1 17:4 17:5 17:6
do
    RP="$RP --report-penalty $combo:3"
done

# 60 seconds covers tick 200 + 51*20 = 1220 = 61 s
export TEST_DURATION=70

BOT1="--race 911 --grid 1 --name BotPen $RP"
BOT2="--race 922 --grid 2 --name BotA"
BOT3="--race 933 --grid 3 --name BotB"
BOT4="--race 944 --grid 4 --name BotC"

echo "==> kunos run"
ssh accd@172.20.0.66 "sudo -n rm -f /tmp/kunos_run.pcap; cd ~/wine-test && \
    TEST_DURATION=$TEST_DURATION ./kunos_run_v2.sh \
    \"$BOT1\" \"$BOT2\" \"$BOT3\" \"$BOT4\" 2>&1 | tail -3"
scp accd@172.20.0.66:~/wine-test/kunos.pcap kunos_4bot.pcap

echo "==> accd run"
sudo -n rm -f /tmp/penalty_diff_accd.pcap accd.pcap
TEST_DURATION=$TEST_DURATION ./run_test_v2.sh \
    "$BOT1" "$BOT2" "$BOT3" "$BOT4" 2>&1 | tail -3
mv accd.pcap accd_4bot.pcap

echo "==> diff"
rm -f accd_4bot.legacy.pcap kunos_4bot.legacy.pcap
editcap -F pcap accd_4bot.pcap accd_4bot.legacy.pcap
editcap -F pcap kunos_4bot.pcap kunos_4bot.legacy.pcap
python3 diff_pcap.py --accd accd_4bot.legacy.pcap --kunos kunos_4bot.legacy.pcap
