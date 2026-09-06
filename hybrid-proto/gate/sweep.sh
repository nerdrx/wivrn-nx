#!/usr/bin/env bash
# Three nxvc runs per fixture:
#   A. nxvc-only anchor      -- inter prediction on, real poses, QP sweep.
#                               The thing hybrid has to beat.
#   B. nxvc all-skip floor   -- every tile forced WARP_SKIP. What a frame costs
#                               when the codec codes nothing at all; the
#                               signalling floor ADR-0029 Cheat 9 is about.
#   C. residual over base    -- the base-vs-source difference coded as an nxvc
#                               picture. nxv-enc takes no external prediction,
#                               so this is the closest measurable proxy for an
#                               enhancement layer (see NXWARP-HYBRID-GATE.md).
#
# Bytes/frame are reported over frames 1..11, excluding the intra frame 0, to
# match the HEVC window which is steady-state P.
set -euo pipefail
cd "$(dirname "$0")"
. /run/media/nerdrx/Lex/claude/nx-warp/scripts/cpu-discipline.sh
nx_cpu_prefix 0-7
B=/run/media/nerdrx/Lex/claude/nx-warp/build-vk/bin
SC=/run/media/nerdrx/Lex/claude/nx-scratch
QPS="22 24 26 28 30"

# all-skip map: 289 tiles x 12 frames of 0x01
python3 -c "open('skipall.map','wb').write(b'\x01'*(289*12))"

for fx in pan panels; do
  case $fx in
    pan)    SRC=$SC/inter_pan8.yuv;     POSES=$SC/inter_pan8.poses.json ;;
    panels) SRC=panels.yuv;             POSES=panels.poses.json ;;
  esac

  for qp in $QPS; do
    # --- A: nxvc-only anchor
    o=nxvc_${fx}_qp${qp}
    [ -f "$o.nxv" ] || "${NICE[@]}" $B/nxv-enc --in "$SRC" --w 1088 --h 1088 \
        --pix yuv420p --qp $qp --inter on --poses "$POSES" --out "$o.nxv" --quiet
    [ -f "$o.yuv" ] || "${NICE[@]}" $B/nxv-dec --in "$o.nxv" --out "$o.yuv" \
        --pix yuv420p --quiet

    # --- B: all-skip floor (same QP; only the signalling remains)
    s=skip_${fx}_qp${qp}
    [ -f "$s.nxv" ] || "${NICE[@]}" $B/nxv-enc --in "$SRC" --w 1088 --h 1088 \
        --pix yuv420p --qp $qp --inter on --poses "$POSES" \
        --skip-map skipall.map --out "$s.nxv" --quiet

    # --- C: residual over each base rate
    for mb in 5 10 15; do
      r=resid_${fx}_${mb}m
      [ -f "$r.yuv" ] || continue
      c=rc_${fx}_${mb}m_qp${qp}
      [ -f "$c.nxv" ] || "${NICE[@]}" $B/nxv-enc --in "$r.yuv" --w 1088 --h 1088 \
          --pix yuv420p --qp $qp --out "$c.nxv" --quiet
      [ -f "$c.yuv" ] || "${NICE[@]}" $B/nxv-dec --in "$c.nxv" --out "$c.yuv" \
          --pix yuv420p --quiet
    done
  done
done
echo "sweep done"
ls -la *.nxv | wc -l
