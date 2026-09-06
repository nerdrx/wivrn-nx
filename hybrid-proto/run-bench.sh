#!/usr/bin/env bash
# Sweep the MediaCodec HEVC decode measurement on the attached Pico 4.
# Read-only with respect to the device: pushes a binary and three streams to
# /data/local/tmp, runs them, pulls JSON. Never touches wivrn-server or the
# installed org.meumeu.wivrn.nx.warp package.
set -euo pipefail
cd "$(dirname "$0")"
D=/data/local/tmp/nxhybrid
B=/data/local/tmp/nxhevcbench
mkdir -p results

run() { # name mode rate extra...
  local name="$1" mode="$2" rate="$3"; shift 3
  echo "-- $name"
  adb shell "cd $D && $B --stream base_1088_${rate}.hevc --index base_1088_${rate}.idx \
      --csd base_1088_${rate}.csd --mode $mode --frames ${FRAMES:-200} --warmup ${WARMUP:-40} \
      --name $name --json $D/$name.json $*" 2>&1 | grep -vE '^\[format\]' | sed 's/^/   /'
  adb pull "$D/$name.json" "results/$name.json" >/dev/null 2>&1 || true
}

echo "device: $(adb shell getprop ro.product.model | tr -d '\r') / Android $(adb shell getprop ro.build.version.release | tr -d '\r')"
echo "gpu: $(adb shell getprop ro.hardware.vulkan | tr -d '\r')  soc: $(adb shell getprop ro.board.platform | tr -d '\r')"
echo

for rate in 5m 10m 50m; do
  run "lat-${rate}-base"  latency    "$rate"
  run "lat-${rate}-ll"    latency    "$rate" --low-latency
  run "lat-${rate}-qti"   latency    "$rate" --qti-low-latency
  run "thr-${rate}-base"  throughput "$rate"
  run "thr-${rate}-qti"   throughput "$rate" --qti-low-latency
done
