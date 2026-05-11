#!/bin/sh
# Run every (cat, kind) combo from FUN_1400f03b0 against both servers,
# diff the byte content of the per-car tail in the post-penalty 0x36.
# Output: tmp/penalty_diff/matrix_results.tsv
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

OUT=matrix_results.tsv
echo -e "cat\tkind\tvalue\tlen_a\tlen_k\ttail_a\ttail_k\tstatus" >$OUT

# (cat, kind) pairs that have a defined wire code per the dispatcher.
# Format: cat:kind (value is fixed at 3).
# Source: reference_kunos_wire_dispatcher.md truth table.
COMBOS="
0:1 0:2 0:3 0:4 0:5 0:6 0:7
1:5
2:5
3:1 3:2 3:3 3:4 3:5 3:6 3:7
4:5 4:6
5:5 5:6
6:5 6:6
7:5
8:1 8:2 8:3 8:4 8:5 8:6
9:5
10:5 10:6
11:1 11:4 11:5 11:6
12:5 12:6
13:4 13:5
14:5 14:6
15:5 15:6
16:1 16:4 16:5 16:6
17:1 17:4 17:5 17:6
"

run_one() {
    local cat=$1 kind=$2 value=$3
    local combo="$cat:$kind:$value"

    ssh accd@172.20.0.66 "sudo -n rm -f /tmp/kunos_run.pcap; cd ~/wine-test && ./kunos_run_v2.sh '--race 911 --grid 1 --report-penalty $combo' 2>&1 | tail -1" >/dev/null 2>&1
    scp accd@172.20.0.66:~/wine-test/kunos.pcap /tmp/kunos_iter.pcap >/dev/null 2>&1
    sudo -n rm -f /tmp/penalty_diff_accd.pcap accd.pcap
    ./run_test_v2.sh "--race 911 --grid 1 --report-penalty $combo" 2>&1 | tail -1 >/dev/null 2>&1
    rm -f /tmp/_a.pcap /tmp/_k.pcap
    editcap -F pcap accd.pcap /tmp/_a.pcap 2>/dev/null
    editcap -F pcap /tmp/kunos_iter.pcap /tmp/_k.pcap 2>/dev/null

    python3 -c "
import sys
sys.path.insert(0, '$HERE')
from diff_pcap import reassemble_server_tx, walk_acc_frames
try:
    _, ab, _ = reassemble_server_tx('/tmp/_a.pcap', 9302)
    _, kb, _ = reassemble_server_tx('/tmp/_k.pcap', 19298)
    af = [b for o,l,b in walk_acc_frames(ab) if b[0]==0x36]
    kf = [b for o,l,b in walk_acc_frames(kb) if b[0]==0x36]
    if not af or not kf:
        print('${cat}\t${kind}\t${value}\t0\t0\t-\t-\tNO_FRAMES')
        sys.exit()
    a = af[-1]
    k = kf[-1]
    ta = a[-4:].hex()
    tk = k[-4:].hex()
    if a == k:
        status = 'IDENTICAL'
    else:
        diffs = sum(1 for j in range(min(len(a),len(k))) if a[j]!=k[j])
        status = f'DIFFER_{diffs}b'
    print(f'${cat}\t${kind}\t${value}\t{len(a)}\t{len(k)}\t{ta}\t{tk}\t{status}')
except Exception as e:
    print(f'${cat}\t${kind}\t${value}\t0\t0\t-\t-\tERR_{e}')
" >>$OUT
}

for pair in $COMBOS; do
    cat=$(echo $pair | cut -d: -f1)
    kind=$(echo $pair | cut -d: -f2)
    echo "Running cat=$cat kind=$kind..."
    run_one $cat $kind 3
done

echo
echo "Results in $OUT:"
column -t -s $'\t' $OUT
