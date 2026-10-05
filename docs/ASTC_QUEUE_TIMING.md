# ASTC pending-queue diagnostics

This diagnostic is **off by default**. On Android, setting
`debug.wivrn.nx.astc_queue_timing` to exactly `1` before constructing an ASTC
decoder adds queue metrics to its existing 180-successful-handoff summary.
The property is read once per decoder, not polled each frame.

The new fields report:

- Mean and maximum time from a completed packet entering the pending queue
  until the decoder worker removes it.
- Number of dequeues used for that mean, including later-rejected packets.
- Oldest pending packets dropped because the existing two-entry queue was full.

Counters/timestamps are protected by the existing queue mutex. Summary counters
reset together under that mutex; the queue window is described as “since prior
summary”, not necessarily exactly 180 dequeues. When disabled, no new clock
reads or counter collection occurs and the old summary format remains.

This changes no queue/drop policy, references, transport or rendering. Dwell
does not include networking, GPU work after dequeue, display scanout, or photon
latency. Maximum dwell is not a percentile. A prolonged failure with fewer than
180 successful handoffs may produce no summary.

The complete Android arm64 RelWithDebInfo native target builds with this change.
Runtime values and enabled-path overhead remain unmeasured; no APK was installed
and no property was activated during implementation.
