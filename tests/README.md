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

Expected output is similar to:

```text
default_span=40000000 loss_only_span=50000000 loss_only_loss=40000000
```

The log file is written in the checkout so it can be retained with the test
evidence; it contains no image or device data.
