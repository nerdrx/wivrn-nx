# GPU motion truth fixture

Runs the production downsample, estimate and warp compute shaders headlessly on Vulkan. Requires the NX Warp bench core static library (build `bench/build-host` first).

```sh
cmake -S tests/motion_gpu_truth -B build-motion-truth -DBENCH_ROOT=/absolute/path/nx-warp/bench
cmake --build build-motion-truth
VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation ./build-motion-truth/motion_gpu_truth 1 8 4
```

Arguments: extrapolation step, horizontal displacement, vertical displacement. Defaults: 1, 8, 4. Run separately with `0 8 4`, `1 0 0`, and `1 16 16`.

Previous image is at time 0, current at time 1 (shift D), target truth at time 2 (shift 2D). The warp consumes current plus its previous-to-current motion field. Step zero must reproduce the held current image, not the future target. RGB RMSE compares both eyes over the central 128×128 region, excluding border disocclusion. PPM files show the first eye. Both eyes intentionally share the same synthetic input.

This is a 256×256 synthetic quality fixture on the host GPU, not Pico performance, live decoded HEVC, physical head movement or motion-to-photon latency. Partial coarse-level shifts can match incorrectly; preserve failures alongside successful cases. Quantized zero SAD does not establish a globally unique match. The current fixture prints evidence rather than imposing an overall pass gate.

Large positive shifts must be below 96 pixels on either axis. Scoring excludes newly uncovered pixels using lower bounds max(64,2*DX), max(64,2*DY); the upper bound remains 192. Thus the scored area shrinks for shifts above 32 pixels. Pyramid diagnostics also clip their starting coordinates to avoid unsigned index underflow.
