#!/bin/sh
# 0x0c reject wire format regression matrix.
# Drives the bot through each reject path and compares the 0x0c reply
# bytes between accd and kunos.
#
# Wire layout (per reference_wire_reject_codes.md):
#   u8 0x0c + u8 reason + u32 sub + u32 detail_a + u32 detail_b
#
# Variants exercised:
#   7  REJECT_VERSION_LO  -- client_version <= 0xff           (--client-version)
#   8  REJECT_VERSION_HI  -- client_version >  0xff           (--client-version)
#   6  REJECT_PASSWORD    -- server requires pwd, bot sends wrong
#                                                             (cfg_reject_password)
#   9  REJECT_FULL        -- maxConnections=1, second bot joins.
#                            Deferred: kunos and accd cfgs use
#                            different TCP/UDP ports (19298 vs 9302)
#                            so a shared configuration.json overlay
#                            can't be scp'd to both sides as-is.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

WINE_CFG=wine-test/cfg

swap_local_cfg() {
    overlay_dir=$1
    [ -z "$overlay_dir" ] && return 0
    for f in "$overlay_dir"/*.json; do
        [ -f "$f" ] || continue
        base=$(basename "$f")
        cp "cfg/$base" "cfg/$base.bak" 2>/dev/null || true
        cp "$f" "cfg/$base"
    done
}

restore_local_cfg() {
    for f in cfg/*.json.bak; do
        [ -f "$f" ] || continue
        mv "$f" "${f%.bak}"
    done
}

swap_wine_cfg() {
    overlay_dir=$1
    [ -z "$overlay_dir" ] && return 0
    for f in "$overlay_dir"/*.json; do
        [ -f "$f" ] || continue
        base=$(basename "$f")
        ssh accd@172.20.0.66 "cd $WINE_CFG && cp '$base' '$base.bak' 2>/dev/null || true"
        scp -q "$f" "accd@172.20.0.66:$WINE_CFG/$base"
    done
}

restore_wine_cfg() {
    ssh accd@172.20.0.66 "cd $WINE_CFG && for f in *.json.bak; do [ -f \"\$f\" ] && mv \"\$f\" \"\${f%.bak}\" || true; done"
}

probe() {
    label=$1
    expected_reason=$2
    overlay_dir=$3
    shift 3
    TEST_DURATION=5

    swap_local_cfg "$overlay_dir"
    swap_wine_cfg "$overlay_dir"

    echo "==> [$label expected_reason=$expected_reason]"
    # Build a single shell-quoted string for the remote ssh invocation
    # so multi-bot variants preserve their per-bot arg boundaries.
    remote=""
    for a in "$@"; do
        remote="$remote \"$a\""
    done
    ssh accd@172.20.0.66 "sudo -n rm -f /tmp/kunos_run.pcap; cd ~/wine-test && \
        TEST_DURATION=$TEST_DURATION ./kunos_run_v2.sh $remote >/dev/null 2>&1"
    scp -q accd@172.20.0.66:~/wine-test/kunos.pcap "kunos_reject_${label}.pcap"

    sudo -n rm -f /tmp/penalty_diff_accd.pcap accd.pcap
    TEST_DURATION=$TEST_DURATION ./run_test_v2.sh "$@" >/dev/null 2>&1
    mv accd.pcap "accd_reject_${label}.pcap"

    restore_local_cfg
    restore_wine_cfg

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

# 7: VERSION_LO
probe ver_lo 7 "" "--race 911 --grid 1 --name BotReject --client-version 0x0001 --expect-reject"
# 8: VERSION_HI
probe ver_hi 8 "" "--race 911 --grid 1 --name BotReject --client-version 0xffff --expect-reject"
# 6: PASSWORD (cfg_reject_password sets password=secret; bot sends wrong)
probe password 6 cfg_reject_password "--race 911 --grid 1 --name BotReject --password wrong --expect-reject"
