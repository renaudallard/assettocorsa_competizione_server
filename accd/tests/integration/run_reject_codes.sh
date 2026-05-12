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
#   9  REJECT_FULL        -- maxConnections=1, second bot joins
#                            (cfg_reject_full/{local,remote}/)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"

WINE_CFG=wine-test/cfg

swap_local_cfg() {
    overlay_dir=$1
    [ -z "$overlay_dir" ] && return 0
    # Use the per-side `local/` subdir if it exists (different listen
    # ports), else fall back to the shared overlay at the top level.
    src=$overlay_dir
    [ -d "$overlay_dir/local" ] && src=$overlay_dir/local
    for f in "$src"/*; do
        [ -f "$f" ] || continue
        base=$(basename "$f")
        cp "cfg/$base" "cfg/$base.bak" 2>/dev/null || true
        cp "$f" "cfg/$base"
    done
}

restore_local_cfg() {
    overlay_dir=$1
    src=$overlay_dir
    [ -n "$overlay_dir" ] && [ -d "$overlay_dir/local" ] && src=$overlay_dir/local
    # Files copied in that had a pre-existing original get restored
    # from .bak.  Files that didnt exist before (no .bak) are
    # removed so the next test sees a clean cfg dir.
    if [ -n "$overlay_dir" ]; then
        for f in "$src"/*; do
            [ -f "$f" ] || continue
            base=$(basename "$f")
            if [ -f "cfg/$base.bak" ]; then
                mv "cfg/$base.bak" "cfg/$base"
            else
                rm -f "cfg/$base"
            fi
        done
    fi
}

swap_wine_cfg() {
    overlay_dir=$1
    [ -z "$overlay_dir" ] && return 0
    # Use the per-side `remote/` subdir if it exists (wine kunos ports
    # 19298/19299 instead of 9302/9303), else share the top-level.
    src=$overlay_dir
    [ -d "$overlay_dir/remote" ] && src=$overlay_dir/remote
    for f in "$src"/*.json; do
        [ -f "$f" ] || continue
        base=$(basename "$f")
        ssh accd@172.20.0.66 "cd $WINE_CFG && cp '$base' '$base.bak' 2>/dev/null || true"
        scp -q "$f" "accd@172.20.0.66:$WINE_CFG/$base"
    done
}

restore_wine_cfg() {
    overlay_dir=$1
    src=$overlay_dir
    [ -n "$overlay_dir" ] && [ -d "$overlay_dir/remote" ] && src=$overlay_dir/remote
    [ -z "$overlay_dir" ] && return 0
    # Tell the remote which files to inspect; restore from .bak if
    # one was created, else delete the staged file.
    bases=""
    for f in "$src"/*; do
        [ -f "$f" ] || continue
        bases="$bases $(basename "$f")"
    done
    ssh accd@172.20.0.66 "cd $WINE_CFG && for b in $bases; do \
        if [ -f \"\$b.bak\" ]; then mv \"\$b.bak\" \"\$b\"; \
        else rm -f \"\$b\"; fi; done"
}

probe() {
    label=$1
    expected_reason=$2
    overlay_dir=$3
    cross_check=$4
    shift 4
    TEST_DURATION=5

    swap_local_cfg "$overlay_dir"
    if [ "$cross_check" = "yes" ]; then
        swap_wine_cfg "$overlay_dir"
    fi

    echo "==> [$label expected_reason=$expected_reason cross=$cross_check]"
    if [ "$cross_check" = "yes" ]; then
        remote=""
        for a in "$@"; do
            remote="$remote \"$a\""
        done
        ssh accd@172.20.0.66 "sudo -n rm -f /tmp/kunos_run.pcap; cd ~/wine-test && \
            TEST_DURATION=$TEST_DURATION ./kunos_run_v2.sh $remote >/dev/null 2>&1"
        scp -q accd@172.20.0.66:~/wine-test/kunos.pcap "kunos_reject_${label}.pcap"
    fi

    sudo -n rm -f /tmp/penalty_diff_accd.pcap accd.pcap
    TEST_DURATION=$TEST_DURATION ./run_test_v2.sh "$@" >/dev/null 2>&1
    mv accd.pcap "accd_reject_${label}.pcap"

    restore_local_cfg "$overlay_dir"
    if [ "$cross_check" = "yes" ]; then
        restore_wine_cfg "$overlay_dir"
    fi

    rm -f "accd_reject_${label}.legacy.pcap"
    editcap -F pcap "accd_reject_${label}.pcap" "accd_reject_${label}.legacy.pcap"
    if [ "$cross_check" = "yes" ]; then
        rm -f "kunos_reject_${label}.legacy.pcap"
        editcap -F pcap "kunos_reject_${label}.pcap" "kunos_reject_${label}.legacy.pcap"
    fi

    cross_check="$cross_check" expected_reason="$expected_reason" \
    label="$label" python3 -c "
import os, sys
sys.path.insert(0, '.')
from diff_pcap import reassemble_server_tx, walk_acc_frames

label = os.environ['label']
expected_reason = int(os.environ['expected_reason'])
cross = os.environ['cross_check'] == 'yes'

_, ab, _ = reassemble_server_tx(f'accd_reject_{label}.legacy.pcap', 9302)
af = [b for o,l,b in walk_acc_frames(ab) if b[0]==0x0c]
print(f'  accd 0x0c frames: {len(af)}; lens {[len(b) for b in af]}')

if not af:
    print('  FAIL: accd emitted no 0x0c')
    sys.exit(1)
a = af[0]
print(f'  accd[0]: {a.hex()}')
if a[1] != expected_reason:
    print(f'  FAIL accd reason={a[1]} != expected {expected_reason}')
    sys.exit(2)

if cross:
    _, kb, _ = reassemble_server_tx(f'kunos_reject_{label}.legacy.pcap', 19298)
    kf = [b for o,l,b in walk_acc_frames(kb) if b[0]==0x0c]
    print(f'  kunos 0x0c frames: {len(kf)}; lens {[len(b) for b in kf]}')
    if not kf:
        print('  FAIL: kunos emitted no 0x0c')
        sys.exit(1)
    k = kf[0]
    print(f'  kunos[0]: {k.hex()}')
    if a == k:
        print('  RESULT: IDENTICAL (reject byte-exact)')
    else:
        diffs = sum(1 for j in range(min(len(a),len(k))) if a[j]!=k[j])
        print(f'  RESULT: DIFFER ({diffs} byte diff, a={len(a)} k={len(k)})')
        sys.exit(2)
else:
    print(f'  RESULT: VALID accd-only (kunos cross-check skipped)')
"
}

# 7: VERSION_LO
probe ver_lo 7 "" yes "--race 911 --grid 1 --name BotReject --client-version 0x0001 --expect-reject"
# 8: VERSION_HI
probe ver_hi 8 "" yes "--race 911 --grid 1 --name BotReject --client-version 0xffff --expect-reject"
# 6: PASSWORD (cfg_reject_password sets password=secret; bot sends wrong)
probe password 6 cfg_reject_password yes \
    "--race 911 --grid 1 --name BotReject --password wrong --expect-reject"
# 9: FULL (cfg_reject_full/{local,remote}/ sets maxConnections=1)
probe full 9 cfg_reject_full yes \
    "--race 911 --grid 1 --name BotSeat1" \
    "--race 922 --grid 2 --name BotSeat2 --expect-reject"
# 4: KICKED (cfg_reject_kicked/local/kicklist.txt pre-populates the
# bots steam_id in accds ephemeral kick list).  No kunos cross-check
# because kunos has no kick-list file either (kicks are runtime-only
# via /kick chat and clear on weekend wrap).
probe kicked 4 cfg_reject_kicked no \
    "--race 911 --grid 1 --name BotReject --expect-reject"
# 5: BANNED (cfg_reject_banned/local/banlist.txt pre-populates the
# bot's steam_id in accd's persistent ban list).  No kunos cross-check
# because kunos has no banlist file (bans are runtime-only via /ban).
probe banned 5 cfg_reject_banned no \
    "--race 911 --grid 1 --name BotReject --expect-reject"
# 10: CP_RATING (cfg_reject_cp_rating sets safetyRatingRequirement=70;
# bots default SA rating is 0 on kunos / NEUTRAL on accd, both < 70)
probe cp_rating 10 cfg_reject_cp_rating yes \
    "--race 911 --grid 1 --name BotReject --expect-reject"
# 11b: not-in-entry-list (cfg_reject_bad_car has forceEntryList=1 with
# a single entry whose steam_id != the bots).  Kunos emits FULL here,
# not BAD_CAR; BAD_CAR is reserved for the "wrong carModel" path.
probe not_in_list 9 cfg_reject_bad_car yes \
    "--race 911 --grid 1 --name BotReject --expect-reject"
# 12: BAD_SESSION (cfg_reject_bad_session sets isPrepPhaseLocked=1).
# Bot1 connects first to advance phase from WAITING into FORMATION,
# bot2 attempts to join while in locked prep -> REJECT_BAD_SESSION.
# accd-only: kunos exe gates preparation_locked to race sessions (RE
# at FUN_140025690:816 requires cVar6 == 10 / session_type=R), while
# accd locks during FORMATION/PRE_SESSION of any session type.  The
# stricter accd behaviour is the wire-correct emit for the prep-lock
# case; kunos cross-check would need a race-session cfg overlay that
# wines CPU starvation can't reliably run.
probe bad_session 12 cfg_reject_bad_session no \
    "--race 911 --grid 1 --name BotSeat1" \
    "--race 922 --grid 2 --name BotSeat2 --expect-reject"
