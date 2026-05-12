#!/bin/sh
# cat=14 (DamagedCar) tail wire.
# Per reference_kunos_wire_dispatcher.md FUN_1400f03b0:
#   cat=14 kind=5 (TP) -> wire 14 (universal)
#   cat=14 kind=6 (DQ) -> wire 29 (Disqualified_DamagedCar)
# The AC2 client main enum (FUN_1434f2fb0) covers 0..13 only, so no
# real client fires cat=14 via 0x41 — kunos's own FUN_140125f50
# callers don't either.  This test exercises the dispatcher path by
# having a bot send 0x41 with cat=14 directly; useful as a regression
# pin for the (renderable) DamagedCar wires.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

echo "combo | accd_tail | kunos_tail | status"
for combo in 14:5:3 14:6:3; do
    ssh accd@172.20.0.66 "sudo -n rm -f /tmp/kunos_run.pcap; cd ~/wine-test && ./kunos_run_v2.sh '--race 911 --grid 1 --report-penalty $combo' >/dev/null 2>&1"
    scp -q accd@172.20.0.66:~/wine-test/kunos.pcap /tmp/kunos_iter.pcap
    sudo -n rm -f /tmp/penalty_diff_accd.pcap accd.pcap
    ./run_test_v2.sh "--race 911 --grid 1 --report-penalty $combo" >/dev/null 2>&1
    rm -f /tmp/_a.pcap /tmp/_k.pcap
    editcap -F pcap accd.pcap /tmp/_a.pcap 2>/dev/null
    editcap -F pcap /tmp/kunos_iter.pcap /tmp/_k.pcap 2>/dev/null
    python3 -c "
import sys
sys.path.insert(0, '.')
from diff_pcap import reassemble_server_tx, walk_acc_frames
_, ab, _ = reassemble_server_tx('/tmp/_a.pcap', 9302)
_, kb, _ = reassemble_server_tx('/tmp/_k.pcap', 19298)
af = [b for o,l,b in walk_acc_frames(ab) if b[0]==0x36]
kf = [b for o,l,b in walk_acc_frames(kb) if b[0]==0x36]
if not af or not kf:
    print('$combo  NO_FRAMES')
else:
    a, k = af[-1], kf[-1]
    ta, tk = a[-4:].hex(), k[-4:].hex()
    print(f'$combo  accd={ta}  kunos={tk}  {\"IDENT\" if a==k else \"DIFFER\"}')
"
done
