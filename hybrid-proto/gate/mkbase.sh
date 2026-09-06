#!/usr/bin/env bash
# HEVC base layers for the patch-source gate, at the shape a WiVRn base layer
# would have: low-latency IPPP, ref=1, no B, no lookahead, no scenecut, CTB 64
# so CTB boundaries coincide with the nxvc 64x64 tile grid.
#
# The fixtures are 12 frames, which is far too short for rate control to
# settle, so each is PING-PONG looped (period 22 = 12 forward, 10 back; no cut,
# so no artificial scene change) to 132 frames. One IDR at frame 0 and none
# after, so the measurement window is pure steady-state P.
#
# The evaluated window is frames 88..99, which is a forward pass of the
# original 12 (88 % 22 == 0) at a point where rate control has settled.
set -euo pipefail
cd "$(dirname "$0")"

. /run/media/nerdrx/Lex/claude/nx-warp/scripts/cpu-discipline.sh
nx_cpu_prefix 0-7

W=1088; H=1088; N=12; PERIOD=22; TOTAL=132
SCRATCH=/run/media/nerdrx/Lex/claude/nx-scratch

pingpong() { # src dst
  python3 - "$1" "$2" <<'PY'
import sys, numpy as np
src, dst = sys.argv[1], sys.argv[2]
W=H=1088; FS=W*H*3//2; N=12; PERIOD=22; TOTAL=132
a=np.fromfile(src,dtype=np.uint8).reshape(-1,FS)
assert a.shape[0]==N, a.shape
with open(dst,'wb') as f:
    for i in range(TOTAL):
        p=i%PERIOD
        f.write(a[p if p<N else PERIOD-p].tobytes())
print(f"{dst}: {TOTAL} frames")
PY
}

for fx in pan panels; do
  case $fx in
    pan)    SRC=$SCRATCH/inter_pan8.yuv ;;
    panels) SRC=panels.yuv ;;
  esac
  [ -f "loop_$fx.yuv" ] || pingpong "$SRC" "loop_$fx.yuv"

  for mb in 5 10 15; do
    out="base_${fx}_${mb}m"
    [ -f "$out.hevc" ] && continue
    echo "== $out"
    "${NICE[@]}" ffmpeg -y -v warning -f rawvideo -pix_fmt yuv420p -s ${W}x${H} \
      -framerate 90 -i "loop_$fx.yuv" \
      -c:v libx265 -preset medium -pix_fmt yuv420p \
      -x265-params "bitrate=${mb}000:vbv-maxrate=$((mb*1200)):vbv-bufsize=$((mb*1000*2/90)):bframes=0:ref=1:scenecut=0:rc-lookahead=0:keyint=1000:min-keyint=1000:repeat-headers=1:annexb=1:slices=1:ctu=64:log-level=error:frame-threads=1:wpp=0" \
      -f hevc "$out.hevc"
    # the conforming decode -- measured bit-identical to the Pico's ASIC
    "${NICE[@]}" ffmpeg -y -v warning -f hevc -i "$out.hevc" -pix_fmt yuv420p -f rawvideo "$out.yuv"
    # per-frame sizes, so bytes/frame is measured over the evaluation window
    "${NICE[@]}" ffprobe -v error -select_streams v -show_packets \
      -show_entries packet=size -of csv=p=0 "$out.hevc" > "$out.sizes"
    python3 - "$out" <<'PY'
import sys
o=sys.argv[1]; s=[int(x) for x in open(o+'.sizes') if x.strip()]
win=s[88:100]
print(f"   {len(s)} AUs; window 88..99 mean {sum(win)/len(win):8.0f} B "
      f"-> {sum(win)/len(win)*8*90/1e6:6.2f} Mbit/s at 90 fps")
PY
  done
done
