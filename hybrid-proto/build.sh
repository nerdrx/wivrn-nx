#!/usr/bin/env bash
# Build the standalone on-device harnesses for arm64 and push them to
# /data/local/tmp. Nothing here touches wivrn-server or the installed
# org.meumeu.wivrn.nx.warp package.
set -euo pipefail
cd "$(dirname "$0")"

. /run/media/nerdrx/Lex/claude/nx-warp/scripts/cpu-discipline.sh
nx_cpu_prefix 0-7

NDK=/run/media/nerdrx/Lex/claude/tools/android-sdk/ndk/29.0.14206865
TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64
CC=$TC/bin/aarch64-linux-android29-clang
API=29

mkdir -p build
for t in "${@:-nxhevcbench nxahbvk}"; do
  [ -f "src/$t.c" ] || continue
  echo "== $t"
  LIBS="-lmediandk -landroid -lnativewindow -llog"
  case "$t" in nxahbvk) LIBS="$LIBS -lvulkan";; esac
  "${NICE[@]}" "$CC" -O2 -Wall -Wextra -Wno-unused-parameter \
    -o "build/$t" "src/$t.c" $LIBS
  "$TC/bin/llvm-strip" "build/$t"
  ls -la "build/$t"
  adb push "build/$t" /data/local/tmp/ >/dev/null
  adb shell chmod 755 "/data/local/tmp/$t"
done
echo "pushed."

# --- host-side components and their tests (no device needed)
if [ "${1:-}" = "host" ] || [ $# -eq 0 ]; then
  VKINC=/run/media/nerdrx/Lex/claude/tools/vulkan-headers-src/include
  echo "== shaders"
  for s in 0 1 2; do
    glslc -fshader-stage=comp -O -DSTORE=$s src/base_patch.comp -o "build/base_patch_s$s.spv"
  done
  echo "== test_base_patch (RADV/lavapipe)"
  "${NICE[@]}" gcc -O2 -Wall -Wextra -Wno-unused-parameter -I"$VKINC" \
    -o build/test_base_patch src/test_base_patch.c -lvulkan
  echo "== test_base_shadow (libavcodec)"
  "${NICE[@]}" g++ -O2 -std=c++20 -Wall \
    -o build/test_base_shadow src/test_base_shadow.cpp \
    $(pkg-config --cflags --libs libavcodec libavutil)
fi
