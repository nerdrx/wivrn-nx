# ASTC direct RGB input

Set `WIVRN_ASTC_DIRECT_RGB=1` before starting the server to opt in. The compositor selects this path only when both eye streams are enabled NX ASTC with equal extents, no additional stream is enabled, and the device supports an RGBA8 storage/sampled image at the stream extent. Otherwise it keeps the existing planar YCbCr path. Other encoder profiles are unchanged; the packet format and client are unchanged.

The compositor still applies its source crop and gaze remap, vertical flip, motion warp, linear-to-sRGB conversion, lens mask, and optional native-center copy. It writes the resulting RGB bytes directly to RGBA8, skipping the RGB-to-YCbCr conversion and 4:2:0 chroma reduction. ASTC reads those RGB bytes. This can preserve fine colour edges, but the intermediate is 4 bytes per pixel instead of NV12's 1.5, increasing image storage and traffic; the offline photo proxy showed a compressed-payload trade-off. Expect a modest image-quality gain, not a speedup.

## Evidence and limits

- **Native stream smoke:** the server selected direct RGB, produced 18 nonblack encoder windows, and the off-head viewer ran at 89.2–90.5 render iterations/s in a short wake capture. There was no inside-headset image inspection or controlled bitrate comparison. This verifies operation of the tested profile, not perceived quality, sustained motion, or headset-wide stability.
- **CPU photo proxy:** an offline RGB-versus-reconstructed-4:2:0 comparison using the same ASTC fit showed 0.13–0.43 dB full-frame PSNR loss and 0.11–0.20 dB ROI loss for the 4:2:0 proxy across two scenes at q2/q4/q6. The proxy is an approximation of compositor YCbCr output; it omits live foveation geometry and may differ in GPU rounding. It supports subsampling as one source of colour-edge loss, not as a direct capture of the native stream.
- **Synthetic shader equivalence:** a local Vulkan check of the production foveation shader compared direct RGBA8 output with the CPU value immediately before the old YCbCr matrix. On synthetic remapped, flipped, and masked inputs, maximum channel error was 1 byte (MAE 0.023735). This validates the branch's colour stage, not photo quality, compression, performance, or live streaming.

Render timing labels compare the server-stamped predicted display target with packet and refresh timestamps; they do not use a source-capture timestamp. The signed offsets may be negative when packets arrive before their scheduled target or a future-targeted ASTC frame is selected early. They are scheduling offsets, not capture-to-display latency, which is unavailable from current telemetry.

The opt-in path increases compositor image bandwidth and has only been checked for the tested profile and hardware. Leave it unset to retain the established planar path.
