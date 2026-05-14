#!/bin/sh
# run_paired.sh — run a kunos+accd diff in parallel.
#
# Usage:
#   ./run_paired.sh TESTNAME TEST_DURATION "bot1_args" ["bot2_args" ...]
#
# Produces two pcap pairs (kunos_<name>.pcap, accd_<name>.pcap) and
# converts to legacy pcap.  Caller is expected to invoke diff_pcap.py
# / cmp_36.py against the results.
#
# Kunos runs on wine VM (172.20.0.66, ports 19298/19299).
# accd runs locally (ports 9302/9303).
# The two processes don't share state and can run concurrently.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

# Helper script, not a self-contained test: must be called with args.
# Reject the 0/1-arg invocation up front so a bulk `for t in run_*.sh`
# sweep aborts cleanly with rc=2 instead of dying on shift.
if [ $# -lt 3 ]; then
    echo "usage: $0 TESTNAME TEST_DURATION bot1_args [bot2_args ...]" >&2
    exit 2
fi
NAME=$1
DURATION=$2
shift 2

# Assemble bot args for ssh quoting (single string, double-quotes
# protected) — both invocations use the same args.
BOTS=""
for ba in "$@"; do
    BOTS="$BOTS \"$ba\""
done

echo "==> kunos + accd run in parallel (duration=${DURATION}s)"

# Kunos half — background
(
    ssh accd@172.20.0.66 "sudo -n rm -f /tmp/kunos_run.pcap; cd ~/wine-test && \
        TEST_DURATION=$DURATION ./kunos_run_v2.sh $BOTS >/dev/null 2>&1"
    scp -q accd@172.20.0.66:~/wine-test/kunos.pcap kunos_${NAME}.pcap
    echo "  kunos pcap saved"
) &
KUNOS_PID=$!

# accd half — also background so they run concurrently
(
    sudo -n rm -f /tmp/penalty_diff_accd.pcap accd.pcap
    TEST_DURATION=$DURATION ./run_test_v2.sh "$@" >/dev/null 2>&1
    mv accd.pcap accd_${NAME}.pcap
    echo "  accd pcap saved"
) &
ACCD_PID=$!

wait $KUNOS_PID
wait $ACCD_PID

echo "==> converting to legacy pcap"
rm -f accd_${NAME}.legacy.pcap kunos_${NAME}.legacy.pcap
editcap -F pcap accd_${NAME}.pcap accd_${NAME}.legacy.pcap
editcap -F pcap kunos_${NAME}.pcap kunos_${NAME}.legacy.pcap
echo "==> pair ready: accd_${NAME}.legacy.pcap + kunos_${NAME}.legacy.pcap"
