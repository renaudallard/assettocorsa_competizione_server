#!/bin/sh
# Driver stint reset (0x4f) relay regression.
# Bot1 emits ACP_DRIVER_STINT_RESET (0x4f) twice — once with force=0
# (voluntary reset / tow path, 4-byte server relay) and once with
# force=1 (forced reset, 12-byte relay carrying the u64 timestamp).
#
# Kunos's dispatcher case 0x4f reads u8 force + u64 ts and emits a
# SwapState relay; accd's h_driver_stint_reset mirrors the same wire.
# Compare the relayed 0x4f frame sequence between the two servers.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

run_one() {
    label=$1
    force=$2
    BOT1="--race 911 --grid 1 --name BotStint --stint-reset 200:$force"
    BOT2="--race 922 --grid 2 --name BotPeer"
    TEST_DURATION=30

    echo "==> [$label force=$force] kunos run"
    ssh accd@172.20.0.66 "sudo -n rm -f /tmp/kunos_run.pcap; cd ~/wine-test && \
        TEST_DURATION=$TEST_DURATION ./kunos_run_v2.sh \"$BOT1\" \"$BOT2\" >/dev/null 2>&1"
    scp -q accd@172.20.0.66:~/wine-test/kunos.pcap "kunos_stint_${label}.pcap"

    echo "==> [$label force=$force] accd run"
    sudo -n rm -f /tmp/penalty_diff_accd.pcap accd.pcap
    TEST_DURATION=$TEST_DURATION ./run_test_v2.sh "$BOT1" "$BOT2" >/dev/null 2>&1
    mv accd.pcap "accd_stint_${label}.pcap"

    rm -f "accd_stint_${label}.legacy.pcap" "kunos_stint_${label}.legacy.pcap"
    editcap -F pcap "accd_stint_${label}.pcap" "accd_stint_${label}.legacy.pcap"
    editcap -F pcap "kunos_stint_${label}.pcap" "kunos_stint_${label}.legacy.pcap"

    python3 -c "
import sys
sys.path.insert(0, '.')
from diff_pcap import reassemble_server_tx, walk_acc_frames

_, ab, _ = reassemble_server_tx('accd_stint_${label}.legacy.pcap', 9302)
_, kb, _ = reassemble_server_tx('kunos_stint_${label}.legacy.pcap', 19298)
af = [b for o,l,b in walk_acc_frames(ab) if b[0]==0x4f]
kf = [b for o,l,b in walk_acc_frames(kb) if b[0]==0x4f]

print(f'  accd 0x4f frames: {len(af)}; lengths {[len(b) for b in af]}')
print(f'  kunos 0x4f frames: {len(kf)}; lengths {[len(b) for b in kf]}')
if not af or not kf:
    print('  FAIL NO_FRAMES')
    sys.exit(1)
a, k = af[0], kf[0]
print(f'  accd[0]:  {a.hex()}')
print(f'  kunos[0]: {k.hex()}')

# force=0 emits a 4-B body and is fully byte-exact.  force=1 emits a
# 12-B body whose last 8 bytes are the IEEE-754 transformed ts; both
# servers now mirror kunos's FUN_140042030 path (client_ts +
# session-relative clock offset).  Bit-exact match isn't reachable
# because the per-conn offset depends on pong-arrival timing which
# differs between accd's loopback and kunos's wine path; assert the
# trailing 8 bytes parse as a finite double within +/- 60 s of zero
# (session-relative magnitudes).
import struct
HEAD = 4
if a[:HEAD] != k[:HEAD]:
    print(f'  RESULT: DIFFER ({sum(1 for j in range(HEAD) if a[j]!=k[j])} byte diff in prefix)')
    sys.exit(2)
if len(a) == 4 and len(k) == 4:
    print('  RESULT: IDENTICAL (4-B voluntary-reset relay byte-exact)')
elif len(a) == 12 and len(k) == 12:
    a_d = struct.unpack('<d', a[4:12])[0]
    k_d = struct.unpack('<d', k[4:12])[0]
    print(f'  ts doubles: accd={a_d} kunos={k_d}')
    if abs(a_d) < 60000 and abs(k_d) < 60000:
        print('  RESULT: PARTIAL — 4-B prefix IDENT, both ts doubles '
              'in session-relative range (bit-exact match needs '
              'identical pong-arrival timing)')
    else:
        print('  RESULT: DIFFER — ts double out of session range')
        sys.exit(2)
else:
    print(f'  RESULT: unexpected lengths a={len(a)} k={len(k)}')
    sys.exit(2)
"
}

run_one f0 0
run_one f1 1
