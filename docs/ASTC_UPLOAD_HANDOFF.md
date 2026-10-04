# ASTC upload and stream recovery

ASTC decoding now hands the image to presentation after host decompression,
staging copy and upload submission. It does not wait for that upload on the CPU.
Both the decoder and the scene submit through the same application graphics queue.
The producer records a transfer-write → fragment-read image barrier before it
publishes the handle. That barrier covers later sampling submissions on the same
queue; a separate semaphore is unnecessary. See the
[Vulkan pipeline barrier rules](https://docs.vulkan.org/spec/latest/chapters/synchronization.html#synchronization-pipeline-barriers).

The worker still waits before reusing its command buffer. Its upload fence also
guards image/staging reuse, while presentation retains its image handle until the
render fence retires. Shutdown drains the upload before destroying its command
pool. This ordering depends on the shared queue; moving uploads to another queue
requires explicit queue synchronization and, if needed, ownership transfers.

An earlier timeline-semaphore prototype failed on Pico with `vkCreateSemaphore:
Incomplete`, despite advertised support. The same-queue path removes that driver
dependency. `debug.wivrn.nx.astc_sync_upload=1` forces the previous post-submit
fence wait for measurement. Clear the property and reconnect to restore async
uploads. This property is a benchmark override, read when each decoder is built.

The 180-frame worker summaries distinguish decompression/staging copy, prior
upload wait, synchronous post-submit wait and submission-to-handoff. With async
uploads, `received_from_decoder` means host decode and upload submission, not GPU
completion. An earlier timestamp alone does not prove lower photon latency.

Recovery from the stalled scene now requires a shared frame ID received within
250 ms across the required views. Individually fresh but mismatched eyes, or an
old retained matching pair, cannot revive the scene repeatedly. Initial startup
and seamless connection adoption retain their separate readiness rules.

Decoder replacement first drains presentation and releases retained handles.
It then moves the old decoders out and joins their workers without holding the
scene decoder lock, allowing any final callback to finish. This avoids destroying
an image pool while retained handles still reference it, and avoids joining a
worker blocked on that lock.

The focused readiness test is runnable with:

```sh
g++ -std=c++20 -Wall -Wextra -Werror -Iclient tests/stream_resume_freshness_test.cpp -o /tmp/stream-resume-test
/tmp/stream-resume-test
```

Hardware and live results, including failed attempts, are recorded in the
[NX Warp result archive](https://github.com/nerdrx/nx-warp/tree/main/bench/results/90fps-2026-10-04).
