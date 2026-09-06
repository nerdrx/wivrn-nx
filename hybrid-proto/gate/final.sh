#!/usr/bin/env bash
# Final pipeline on the CRF operating points.
#
# Operating points, chosen on the measured CRF rate-quality curve and named by
# their ACHIEVED bitrate:
#   panels  CRF 26 / 18 / 14  ->  4.26 / 10.46 / 14.68 Mbit/s   (~5 / 10 / 15)
#   pan     CRF 22 / 14 /  6  ->  2.36 /  3.34 /  4.68 Mbit/s   (saturates)
set -euo pipefail
cd "$(dirname "$0")"
. /run/media/nerdrx/Lex/claude/nx-warp/scripts/cpu-discipline.sh
nx_cpu_prefix 0-7
B=/run/media/nerdrx/Lex/claude/nx-warp/build-vk/bin
SC=/run/media/nerdrx/Lex/claude/nx-scratch
QPS="22 24 26 28 30"

pts_pan="22 14 6"
pts_panels="26 18 14"

for fx in pan panels; do
  case $fx in
    pan)    SRC=$SC/inter_pan8.yuv; PTS=$pts_pan ;;
    panels) SRC=panels.yuv;         PTS=$pts_panels ;;
  esac
  for crf in $PTS; do
    tag=${fx}_c${crf}
    # per-tile stats + the residual planes
    [ -f "st_$tag.json" ] || "${NICE[@]}" python3 analyse.py "$fx" "c$crf" "$SRC" \
        "crfbase_${fx}_${crf}.yuv" "st_$tag.json" "rs_$tag.yuv"
    # code the residual with nxvc, and decode it back
    for qp in $QPS; do
      [ -f "rq_${tag}_$qp.nxv" ] || "${NICE[@]}" $B/nxv-enc --in "rs_$tag.yuv" \
          --w 1088 --h 1088 --pix yuv420p --qp $qp --out "rq_${tag}_$qp.nxv" --quiet
      [ -f "rq_${tag}_$qp.yuv" ] || "${NICE[@]}" $B/nxv-dec --in "rq_${tag}_$qp.nxv" \
          --out "rq_${tag}_$qp.yuv" --pix yuv420p --quiet
    done
  done
done
echo "final sweep done"
