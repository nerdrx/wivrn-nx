# Native-colour centre (experimental)

Set `NX_DIRECT_NATIVE_CENTER=1` before launching the server. Requires a paired direct-backend stream with LZ4 and safety enabled, at least 256×256 pixels per eye, and an updated client supporting stream versions 7/8. It is disabled by default. The enabled user session preserves its existing bitrate settings.

A tile-aligned 128×128 patch per eye retains one RGB888 colour per encoded pixel before chroma averaging. The centre bypasses palette coding; the rest of the frame and the safety prefix retain their previous representations. This is spatially native colour when the source map is 1:1, not 10-bit or floating-point losslessness. A one-time source-footprint log records the mapping for verification.

The patch reserves 131,200 bytes per frame before compression (94.46 Mbit/s at 90 Hz, 118.08 Mbit/s including the conservative 25% transport allowance). It consumes part of the selected budget; it is not extra traffic outside the slider limit. Low budgets can reduce peripheral detail or admission rate. No extra client rendering pass is introduced.

The wire uses descriptor bit 29 in mode 0, a 29-bit word offset, 1024 RGB888 words plus one alignment word per native tile. Frame version 2 requires native negotiation; legacy payloads remain valid, and the low-resolution safety layout remains legacy. Bounds and offsets are validated before upload.

Tests and live evidence: https://github.com/nerdrx/nx-warp/tree/main/bench/results/90fps-2026-09-22/native-centre
