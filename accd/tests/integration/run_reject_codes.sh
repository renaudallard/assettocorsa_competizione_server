#!/bin/sh
# 0x0c reject wire format regression.
# Drive the bot through a few bad-handshake variants and compare the
# resulting 0x0c reply between accd and kunos.
#
# Wire layout (per reference_wire_reject_codes.md):
#   u8 0x0c + u8 reason + u32 sub + u32 detail_a + u32 detail_b
# Reasons exercised here:
#   7  REJECT_VERSION_LO  -- client_version <= 0xff
#   8  REJECT_VERSION_HI  -- client_version >  0xff
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

probe() {
    label=$1
    ver=$2
    expected_reason=$3
    BOT="--race 911 --grid 1 --name BotReject \
        --client-version $ver --expect-reject"
    TEST_DURATION=5

    echo "==> [$label ver=$ver] kunos run"
    ssh accd@172.20.0.66 "sudo -n rm -f /tmp/kunos_run.pcap; cd ~/wine-test && \
        TEST_DURATION=$TEST_DURATION ./kunos_run_v2.sh \"$BOT\" >/dev/null 2>&1"
    scp -q accd@172.20.0.66:~/wine-test/kunos.pcap "kunos_reject_${label}.pcap"

    echo "==> [$label ver=$ver] accd run"
    sudo -n rm -f /tmp/penalty_diff_accd.pcap accd.pcap
    TEST_DURATION=$TEST_DURATION ./run_test_v2.sh "$BOT" >/dev/null 2>&1
    mv accd.pcap "accd_reject_${label}.pcap"

    rm -f "accd_reject_${label}.legacy.pcap" "kunos_reject_${label}.legacy.pcap"
    editcap -F pcap "accd_reject_${label}.pcap" "accd_reject_${label}.legacy.pcap"
    editcap -F pcap "kunos_reject_${label}.pcap" "kunos_reject_${label}.legacy.pcap"

    python3 -c "
import sys
sys.path.insert(0, '.')
from diff_pcap import reassemble_server_tx, walk_acc_frames

_, ab, _ = reassemble_server_tx('accd_reject_${label}.legacy.pcap', 9302)
_, kb, _ = reassemble_server_tx('kunos_reject_${label}.legacy.pcap', 19298)
af = [b for o,l,b in walk_acc_frames(ab) if b[0]==0x0c]
kf = [b for o,l,b in walk_acc_frames(kb) if b[0]==0x0c]

print(f'  accd 0x0c frames: {len(af)}; lens {[len(b) for b in af]}')
print(f'  kunos 0x0c frames: {len(kf)}; lens {[len(b) for b in kf]}')
if not af or not kf:
    print('  FAIL NO_FRAMES')
    sys.exit(1)
a, k = af[0], kf[0]
print(f'  accd[0]:  {a.hex()}')
print(f'  kunos[0]: {k.hex()}')
if a == k and a[1] == $expected_reason:
    print('  RESULT: IDENTICAL (reject byte-exact)')
elif a == k:
    print(f'  RESULT: IDENTICAL bytes but reason {a[1]} != expected $expected_reason')
else:
    diffs = sum(1 for j in range(min(len(a),len(k))) if a[j]!=k[j])
    print(f'  RESULT: DIFFER ({diffs} byte diff, a={len(a)} k={len(k)})')
    sys.exit(2)
"
}

probe lo 0x0001 7
probe hi 0xffff 8
