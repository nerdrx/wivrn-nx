# The client's decode wall

On a Pico 4, streaming 1088x1088 per eye as one stereo frame at QP 22, the NX Warp
decoder reports a **22.8 ms wall** per frame against **16.7 ms of GPU**. The server paces
to that wall — `paced to 38.5 fps (client decode 23.5 ms)` — so the wall is the frame
rate, and about 6 ms of it is not the decode kernel.

This is where that 6 ms goes, measured rather than reasoned about, and what can be done
about it.

All figures below are the mean over the 46 two-second report windows of one 90 s capture
on `a779904b` (`nx-scratch/live/cap76.log`), Wi-Fi 5 GHz, RSSI −52, nothing else running
on the device.

## The accounting

The decoder's own two lines already close the books, once you put them together:

```
nxwarp[0] stage: gap 0.99 | wait-prev 0.00 | submit 0.60 (qlock 0.00 + codec 0.60)
                 | get_free 0.00 | fence-pre 0.00 | record 0.00 | qsubmit 0.00
                 | fence-post 22.08 | publish 0.00 ms
nxwarp[0] fence-post 22.09 ms = nxvc gpu 16.69 + copy gpu 0.330 + queue 5.05
```

So, per frame:

| stage | ms | what it is |
|---|---|---|
| nxvc gpu | 16.69 | the decode kernels (Pass A 3.0, Pass B 13.7) |
| copy gpu | 0.33 | the staging copy into the pool image |
| **queue** | **5.05** | fence wait minus both GPU durations |
| submit | 0.60 | handing the unit to nxvc (all of it `codec`, `qlock` 0.00) |
| everything else | ~0.10 | get_free, fence-pre, record, qsubmit, publish — all 0.00 |
| **wall** | **22.78** | |

There is no unaccounted remainder. `16.69 + 0.33 + 5.05 = 22.07`, which is `fence-post`
to two decimals, and `22.08 + 0.60 + ~0.1 = 22.78`, which is the wall. The whole of the
non-kernel time is **`queue` 5.05 ms and `submit` 0.60 ms**.

Three things are worth saying about what is NOT in there, because each was a candidate:

* **Reassembly is free.** `0.020 ms/frame`, on the network thread, off this path
  entirely. The network thread's total cost is `0.009 ms per datagram over 10440
  datagrams (45.51 ms of every second)` — 4.5% of one core, and none of it in the wall.
* **Command recording is free.** `record 0.00 | qsubmit 0.00 | get_free 0.00`. The
  command buffer *is* fully re-recorded every frame — `cmd.reset()`, four image
  barriers, two `vkCmdCopyImage`, two timestamp writes (`nxwarp_decoder.cpp:1377-1458`)
  — it simply costs less than the 0.01 ms the line resolves. Pre-recording it would buy
  nothing measurable.
* **The queue lock is not contended.** `qlock 0.00` of a `submit` that is 0.60. The
  0.60 ms is inside nxvc, not waiting for the shared queue mutex.

## What `queue` actually is

`sched_ms` is computed as

```cpp
prof.sched_ms += ms(t_fence_post - t_qsubmit) - copy_gpu_ms - st.gpu_ms;
```

— the host's fence wait, minus the two GPU durations it contains. It is therefore the sum
of three things the decoder cannot currently tell apart:

1. time between `vkQueueSubmit` and the GPU actually starting our work,
2. the drain between nxvc's submission finishing and the copy starting,
3. host wake latency after the fence signals.

**It is not contention between the eyes.** The decoder has instrumentation for exactly
that (`saw_other` / `after_other_ms` / `overlapped_other`, comparing each stream's copy
timestamps in the one device clock), and its report line never fires in this capture:
this is a stereo session, both eyes are one stream, and stream 2 (the alpha plane) never
decodes a frame.

## The finding: the GPU is at 92%

The client's own reprojection pass and the decode share one Adreno hardware ring. From
the same capture:

```
render: this app's own GPU pass 6.37 ms per iteration      loop 43.64/s
nxwarp[0]:                     16.69 ms per frame          decode 38.5/s
```

```
render GPU load : 6.37 x 43.64 = 278 ms/s
decode GPU load : 16.69 x 38.5 = 643 ms/s
total           :               921 ms/s  = 92% of the GPU
```

At 92% duty, a submission that arrives while the other consumer is running waits for it.
That is what `queue` is measuring, and it is why the number is 5 ms rather than the tens
of microseconds a submission costs on an idle queue. **The decode wall is not a CPU
problem.** Every CPU stage on the path is 0.00 except a 0.60 ms call into nxvc.

This also explains the shape of the whole session: a 22.8 ms wall on a 90 Hz panel, a
loop turning at 43.6/s with a 22.9 ms display period, and the server pacing to 38.5 fps.
One GPU is doing two jobs and is nearly full.

## The one structural unknown: which queue the decode actually got

`get_decode_queue(slot)` (`application.h:363-379`) hands out queue 1 or 2 of a family the
client asked for `min(3, queueCount)` of. **If the device gave only one queue, it falls
back to `vk_queue` — the same `VkQueue` and the same mutex the render thread submits
through** (`application.h:366`, `application.cpp:1074-1076`).

The capture argues against that having happened here: `qlock 0.00` is the time to acquire
the decode queue's mutex, measured on a lock held across the whole of
`nxvc_vk_decode_frame_ex`, and a mutex shared with a render thread submitting 43 times a
second would not round to zero. But "the CPU lock was not contended" is not the same
statement as "these are different hardware queues", and on Adreno they are the same ring
either way.

Nothing in the log says how many queues were created or which one a stream is using, so
this cannot be settled from a capture. It is one line of logging, it is decoder-local, and
it is implemented on this branch.

## Ranked plan

Ordered by ms/s of GPU recovered, because that is the currency the wall is denominated
in — not by how interesting the change is.

### 1. Pass B `skip` — 8.25 of the 13.7 ms Pass B, ~318 ms/s

`passB 13.7 ms = warp 0.10 + skip 8.25 + coded 5.41 + dir 0.00 + other 0.00`

Sixty per cent of Pass B, and a third of the entire GPU budget of the device, is
`WARP_SKIP` tiles running the normative integer pose warp. This is the single largest
term in the decode wall by a wide margin and it is already the Pass B agent's lane. Every
other item on this list is smaller than the error bar on this one.

### 2. Reprojection pass — 278 ms/s, roughly a third of it recoverable

`reduce_gpu_load` is **off** in this capture (`0 re-presented from the cache`). The loop
turns 43.6 times a second while 38.5 decoded frames arrive, so ~12% of iterations redraw
an image identical to the last one. Turning the existing re-present cache on recovers
~33 ms/s for no new code.

Beyond that the pass is already cheap: `defoveate 1088x1088 per eye x2 = 2.37 Mpx/frame
at scale 0.50`. It is 6.37 ms for 2.37 Mpx, which is not obviously wrong for this device,
but it is the second-largest GPU consumer and has had no optimisation pass of its own.

### 3. Split `queue` so the next change is aimed rather than guessed — no device needed

`queue` is three effects in one number, and they have different fixes: waiting for the
GPU wants less GPU work, host wake latency wants thread priority. **We currently cannot
tell which dominates**, and both proposals below are worthless until we can. This is the
piece implemented on this branch — see "What is implemented here".

### 4. Decode thread priority and affinity — unknown, plausibly ~1 ms

**Nothing in the client sets thread priority or affinity anywhere**, except
`secondary_path.cpp:83` (`setpriority(PRIO_PROCESS, 0, 10)`) on a network path thread.
The decode worker is a bare `std::thread` (`nxwarp_decoder.cpp:289`) that does not even
go through `utils::named_thread` — and `named_thread` only calls `pthread_setname_np`, so
it would not have set a priority either. There is no `pthread_setschedparam`, no
`sched_setaffinity`, no `SCHED_FIFO` and no `nice()` anywhere in `client/`. Vulkan queue
priorities are all `0.0f`, deliberately equal (`application.cpp:973-975`). The decode
worker and the render thread run at Android's default priority on whatever core the
scheduler picks, which on an XR2 includes the silver cores.

This matters more on this device than it looks, because of how the fence is waited on.
There are two blocking `vkWaitForFences(..., UINT64_MAX)` per frame — one on the previous
frame's copy before recording (`:1373`, `fence-pre 0.00`) and one on **this** frame's copy
after submit (`:1548`), taken only when `signal_on_queue` is false. On the Pico that is
always: timeline semaphore creation throws, so `host_sync` is on for every session
(decoupled display or not — see the A/B in the session report). So the decode worker
blocks on the host for the whole GPU duration of every frame and is then woken. The wake
is inside `queue`.

The CPU work on the path is near zero, so this cannot help the compute — but it can help
the **wake**: a thread resumed from `vkWaitForFences` on a little core takes longer to
run again, and that latency is inside `queue`. Worth trying `sched_setaffinity` to the
big cores plus a raised priority for the decode worker, but only once (3) says host wake
is actually a meaningful share. Needs the device to validate.

### 5. Things that look attractive and are not worth doing

* **Pre-recorded command buffers.** `record 0.00 ms`. There is nothing to recover.
* **Removing the staging copy.** `copy gpu 0.330 ms`, 13 ms/s — 1.4% of the budget.
  Worth stating precisely, because the obvious framing does not fit this path: **there is
  no R8 view and no one-tap sampling in the NX Warp decoder.** nxvc outputs
  `NXVC_VKD_OUT_YCBCR420` and the pool image is `G8_B8R8_2PLANE_420_UNORM` sampled
  through a `VkSamplerYcbcrConversion` (`nxwarp_decoder.cpp:238-243, 339, 447-449`). The
  R8 one-tap language elsewhere in the tree belongs to `raw_decoder.cpp` and to
  `reprojection.glsl`, where it is a *lower-bound baseline for comparison*, not an
  implemented path; "no blit, no copy" in `NXWARP-HYBRID.md` is the MediaCodec /
  AHardwareBuffer import, which this path deliberately does not use.

  The copy is two `vkCmdCopyImage` calls (luma plane, chroma plane) and it exists for a
  stated reason: *"nxvc leaves its output in GENERAL and overwrites it in place on the
  next frame"* (`:1388-1389`). Removing it means giving nxvc a double-buffered output to
  render into, not changing how the client samples. That is an nxvc change for 13 ms/s,
  which is why it is on this list rather than above it.
* **Submitting the next frame before the previous fence.** The queue is at 92% duty;
  deeper pipelining into a full queue moves the wait, it does not remove it. It would
  also undo the decoupled-display property that a published frame is a finished frame.

## What is implemented here

One thing, from item (3), and less than that item asks for — so it is worth being exact
about what landed and what did not.

**Landed: the decoder reports its own GPU duty cycle.**

```
nxwarp[0] gpu duty 643 ms/s (64% of one GPU) for decode alone; the display pass is extra
```

`gpu_ms` summed over the report window against the window's own length. This is the
number that turns "`queue` is 5 ms" into "the GPU is nearly full", and it is the missing
half of the arithmetic at the top of this document: the render loop already prints its
own pass cost and rate, so a reader can now add 278 to 643 and see 92% without leaving
the log. Verified on the harness (23 frames × 5.1 ms over 2.1 s reported as 57 ms/s).

**Not landed: splitting `queue` into "queued before GPU start" and "host wake after GPU
end".** I intended to and could not do it honestly. The decoder holds device-clock
timestamps for the *copy* only (`ts[0]`, `ts[1]` from a two-query pool, `:1385`/`:1457`),
and nxvc reports its decode as a *duration*, not as absolute device timestamps. Splitting
the residual needs either an absolute start from nxvc or a host/device correlation
(`VK_EXT_calibrated_timestamps`). Both are real options; neither is a line of code, and
neither can be validated here. Reporting a split derived from timestamps that cannot
support it would be worse than reporting none.

**Not landed, and deliberately: naming the queue.** The decoder passes `stream_index` to
`host.with_queue()` as a slot *request*; whether it gets queue 2, queue 1, or the render
thread's own queue is decided in `application::get_decode_queue()` from host state the
decoder cannot see. Logging the slot here would print the question dressed as the answer.
The line belongs on the host side, where the fallback is chosen.

Both the change and the restraint are decoder-local, which matters: `nxwarp_decoder.cpp`
is compiled into the e2e harness as well as the client, so `merge-wivrn.sh` exercises this
code and a mistake in it fails a harness leg rather than a headset.

## What still needs the device

The A/B for (2) and (4), and the confirmation that (3)'s split points where this document
argues it will. The measurement recipe is the one that produced the numbers above: one
server, one link, `nx-scratch/live-tools/connect.sh` to prove the stream scene is current,
90 s of capture, and the aggregation in the report for that session.
