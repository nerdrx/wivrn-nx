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
