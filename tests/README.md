# Focused controller checks

Run the opt-in AIMD diagnostic check from the repository root. `BUILD_DIR`
must be a configured build directory containing the Monado and Boost
dependencies used by the project:

```sh
BUILD_DIR=${BUILD_DIR:-build-server}
g++ -std=c++23 \
  -I server -I common -I "$BUILD_DIR/common" \
  -I "$BUILD_DIR/_deps/monado-src/src/xrt/include" \
  -I "$BUILD_DIR/_deps/monado-src/src/xrt/auxiliary" \
  -I "$BUILD_DIR/_deps/monado-src/src/external/openxr_includes" \
  -isystem external \
  -isystem "$BUILD_DIR/_deps/boost-src/libs/pfr/include" \
  -o bitrate_aimd_loss_only_test \
  tests/bitrate_aimd_loss_only_test.cpp \
  server/driver/bitrate_controller.cpp common/smp.cpp -lcrypto
./bitrate_aimd_loss_only_test | tee bitrate_aimd_loss_only_test.log
```

The check covers clean recovery to the full 1 Gbit/s ceiling after sustained
loss, ordinary-mode behavior, and loss/late/span guards. It prints recovery
times in simulated nanoseconds; these are not live network measurements.
Repeat compilation with `-DNDEBUG` to verify checks remain active.

The log file is written in the checkout so it can be retained with the test
evidence; it contains no image or device data.

## Closed-loop recovery model

`python3 tests/run_bitrate_recovery_link.py --build-dir "$BUILD_DIR" --output-dir /tmp/nx-recovery-results`

Compiles the real controller at fixed-35% baseline `3b142e1b` and the working
version. Tests nine combinations of a 400/550/700 Mbit/s *budget-equivalent*
link and 0/40/100 ms feedback delay. Each run simulates 100 seconds at 90 Hz.
This is a toy FIFO, not Wi-Fi, codec quality, or headset validation. See the
harness comments and report for assumptions. It checks reduced losses without
large budget sacrifice and recovery to the full requested ceiling. No server,
network interface, or headset is touched.

## Direct v2 budget model

For focused regression checks, use the compile command above with
`bitrate_bbr_budget_test` instead of `bitrate_aimd_loss_only_test` for both
the source and output executable. Repeat with `-DNDEBUG`. It covers clean
compression changes, unchanged delivery-rate accounting, mixed streams,
loss without byte callbacks, estimator expiry, and genuine congestion cuts.

The direct v2 model is run with:

```sh
python3 tests/run_bitrate_recovery_link.py --v2 --baseline-ref 66a5e6e6 --build-dir "$BUILD_DIR" --output-dir /tmp/nx-v2-budget-results
```

It freezes the exact controller input budget, including safety allocation, and
the frame period at encode time. The model maps that per-frame budget through
the actual datagram bytes while leaving the physical bandwidth estimator in
datagram bits/s. Mapping is restricted to the sole primary direct stream.
Clean estimates cannot lower quality by themselves; loss and sustained spans
above 1.10 refresh periods retain the cut path. The direct policy uses 500 ms
probe/decrease/steady timing, 1.10 probe gain, and an exact final ceiling step
below a 5% remaining gap.

The nine model cases, compared with baseline `66a5e6e6`, recovered in 2.4–7.2 seconds. No server or Pico was used,
and no live validation is implied. Results:
[v2 budget matrix](https://github.com/nerdrx/nx-warp/tree/main/bench/results/90fps-2026-09-24/v2-budget).


## Direct checkerboard half-refresh

```sh
g++ -std=c++20 -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Icommon tests/direct_checkerboard_test.cpp -o /tmp/direct_checkerboard_test
/tmp/direct_checkerboard_test

g++ -std=c++20 -O2 -Icommon tests/direct_checkerboard_bench.cpp \
  -llz4 -lzstd -o /tmp/direct_checkerboard_bench
/tmp/direct_checkerboard_bench frame1.nxdf frame2.nxdf
```

The first test requires assertions (do not compile with `NDEBUG`). It checks
both phases for all palette modes and native RGB888/RGB565, stream versions,
legacy rejection, truncation, offset overflow and reserved flags. The benchmark
round-trips LZ4/Zstd bytes exactly and prints CSV; its timings exclude packing,
transport and headset presentation. Fixture geometry must match the defaults
in the harness. `direct_checkerboard_motion.cpp` is a CPU reconstruction of the
wire format for synthetic previews, not a GPU/headset timing test.

### Packed checkerboard upload

```sh
g++ -std=c++20 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Icommon tests/direct_checkerboard_upload_test.cpp -o /tmp/direct_checkerboard_upload_test
/tmp/direct_checkerboard_upload_test

g++ -std=c++20 -O3 -Icommon tests/direct_checkerboard_upload_bench.cpp \
  -o /tmp/direct_checkerboard_upload_bench
/tmp/direct_checkerboard_upload_bench current-full.nxdf history-full.nxdf
```

Keep assertions enabled. The upload tests check phase palettes, selectors,
native formats, mode changes, missing/full history, malformed input and the
expanded-buffer size limit. NXDU is an internal upload format, not NXDF wire data.
The benchmark expects two 2176×2176-per-eye full RGB888 NXDF fixtures. It prepares
checker inputs before timing, warms both phases, then alternates 180 measured
merges. Output includes a checksum and per-phase timing. These are standalone
CPU merge timings, excluding decompression, GPU upload and presentation.

The pixel-parity harness renders the production fragment shader and compares
its output against the former history-sampling algorithm. It requires Vulkan
headers/loader, glslangValidator, and local captured frames (2176×2176 per eye):

```sh
g++ -std=c++20 -O2 -Icommon tests/direct_checkerboard_gpu_fragment_test.cpp \
  -lvulkan -o /tmp/checker-fragment-test
glslangValidator -V --target-env vulkan1.0 -S vert \
  tests/direct_checkerboard_fullscreen.vert.glsl -o /tmp/checker.vert.spv
glslangValidator -V --target-env vulkan1.0 -S frag \
  client/shaders/reprojection_direct.frag.glsl -o /tmp/checker.frag.spv
/tmp/checker-fragment-test /tmp/checker.frag.spv /tmp/checker.vert.spv \
  current.nxdf previous.nxdf merged.nxdu
```

`NX_CHECKER_SRGB=1` also checks the sRGB attachment/conversion path.
`NX_CHECKER_DUMP_RGBA=/tmp/eye.rgba` saves private raw output for inspection.
Optional `NX_CHECKER_WIDTH`/`NX_CHECKER_HEIGHT` change output sampling; scaled
CPU comparisons can differ at coordinate boundaries, so compare old/new GPU
readbacks directly for those experiments. `--legacy-deband-phase` accepts the
old fragment shader with phase moved to `deband.z` (no NXDU argument).
`direct_checkerboard_gpu_sample_test.cpp` isolates the production sampling
function in a compute shader; `direct_checkerboard_upload_sampling_test.cpp`
provides the CPU reference. These tools do not measure live frame cadence.
