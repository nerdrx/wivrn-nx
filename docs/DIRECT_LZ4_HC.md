# Optional direct-block LZ4 HC

Normal LZ4 remains the default. Enable `Stronger lossless compression (LZ4 HC)` in the headset's NX Warp settings to use HC level 2 with direct LZ4. Disable it to return to the normal compressor. The option applies live to detail and safety payloads; it does not change wire format, independent chunks, raw fallback, or the client decoder.

Host fixture tests found 10.3% additional scene savings at +0.637 ms compression time, 3.5% photo savings at +1.563 ms, and no noise savings at +2.028 ms. These are compression-only medians, not live or Pico latency measurements. HC is therefore not enabled automatically.

`tests/direct_blocks_lz4_hc_bench.cpp` accepts 2176×2176 stereo NXDF files and compares levels 2, 3, 6, and 9, excluding eight warmups and validating decoder round trips. Link with the pinned LZ4 library including lz4hc.c. Full evidence: https://github.com/nerdrx/nx-warp/tree/main/bench/results/90fps-2026-09-22/outer-color-hc
