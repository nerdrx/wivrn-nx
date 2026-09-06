#!/usr/bin/env bash
# Generate the HEVC base-layer test streams for the hybrid scoping measurement.
#
# Geometry: 1088x1088 per eye (the WiVRn NX / Pico 4 per-eye stream size the
# scoping task names). Source material is the nx-warp corpus entry
# vr-mixed-1024-v2 (2048x1024 SBS, 36 frames, head-rotation class) -- the left
# eye is cropped and scaled to 1088x1088, then looped to FRAMES frames.
#
# Encoder shape mirrors what a WiVRn base layer would actually be:
#   bframes=0, ref=1, no scenecut, no lookahead, repeat-headers, annexb,
#   one slice per picture (so one access unit per picture, which the AU index
#   below assumes), CTB size 64 so CTB boundaries land on the nxvc tile grid.
#
# Outputs, per bitrate, into streams/:
#   base_1088_<rate>.hevc       Annex-B elementary stream
#   base_1088_<rate>.idx        "offset size keyflag" per access unit
#   base_1088_<rate>.ref.yuv    FFmpeg's decode of that stream (NV12-ordered
#                               yuv420p), the conforming-decoder reference the
#                               Pico's output is compared against
set -euo pipefail
cd "$(dirname "$0")"

. /run/media/nerdrx/Lex/claude/nx-warp/scripts/cpu-discipline.sh
nx_cpu_prefix 0-7

CORPUS=/run/media/nerdrx/Lex/claude/nx-scratch/nx-warp/corpus
SRC="$CORPUS/vr-mixed-1024-v2.yuv420p.yuv"
FRAMES="${FRAMES:-288}"
W=1088; H=1088

[ -f "$SRC" ] || { echo "missing corpus source $SRC" >&2; exit 1; }

mkdir -p streams

# 36 frames of 2048x1024 yuv420p; crop the left eye, scale to 1088^2, loop.
SRC_Y4M=streams/src_1088.y4m
if [ ! -f "$SRC_Y4M" ]; then
  echo "== building $FRAMES-frame 1088x1088 source"
  "${NICE[@]}" ffmpeg -y -v warning \
    -f rawvideo -pix_fmt yuv420p -s 2048x1024 -framerate 90 -i "$SRC" \
    -filter_complex "crop=1024:1024:0:0,scale=${W}:${H}:flags=lanczos,loop=loop=-1:size=36:start=0,trim=end_frame=${FRAMES},setpts=N/90/TB" \
    -pix_fmt yuv420p "$SRC_Y4M"
fi

encode() {
  local rate="$1"
  local out="streams/base_1088_${rate}.hevc"
  local n="${rate%m}"
  echo "== encoding $out"
  "${NICE[@]}" ffmpeg -y -v warning -i "$SRC_Y4M" \
    -c:v libx265 -preset medium -pix_fmt yuv420p \
    -x265-params "bitrate=${n}000:vbv-maxrate=$(( n * 1200 )):vbv-bufsize=$(( n * 2000 / 90 * 2 )):bframes=0:ref=1:scenecut=0:rc-lookahead=0:keyint=90:min-keyint=90:repeat-headers=1:annexb=1:slices=1:ctu=64:log-level=error:frame-threads=1:wpp=0" \
    -f hevc "$out"
  # authoritative AU boundaries from ffprobe, so the harness needs no parser
  "${NICE[@]}" ffprobe -v error -select_streams v -show_packets \
    -show_entries packet=pos,size,flags -of csv=p=0 "$out" \
    | awk -F, '{k=($3 ~ /K/)?1:0; print $2, $1, k}' > "streams/base_1088_${rate}.idx"
  # the conforming-decoder reference
  "${NICE[@]}" ffmpeg -y -v warning -f hevc -i "$out" \
    -pix_fmt yuv420p -f rawvideo "streams/base_1088_${rate}.ref.yuv"
  ls -la "$out" "streams/base_1088_${rate}.idx" "streams/base_1088_${rate}.ref.yuv"
  echo "   AUs: $(wc -l < "streams/base_1088_${rate}.idx")  mean AU bytes: $(awk '{s+=$2;n++} END{printf "%.0f", s/n}' "streams/base_1088_${rate}.idx")"
}

for r in "${@:-5m 10m 50m}"; do encode "$r"; done
