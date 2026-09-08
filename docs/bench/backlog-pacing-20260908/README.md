# Treat worker backlog as decoder overload

The live Pico session after the materialized-reference optimization still
reported 19 worker-backlog drops and 19 all-intra recovery events in one
reporting window. The sender had measured 17.4 ms decode time in that window.
The preserved excerpt is a live observation, not a controlled speed comparison.

The pacer reacted immediately only to `reason::stride`; it ignored
`reason::backlog`, even though that reason means the decode worker could not
keep up with arriving frames. Both now increment the same overload counter.
The existing response applies: increase the interval by 5%, bounded below by
the measured decode target. Normal pacing limits and recovery slew are unchanged.

This also counts overload reports whose reference recovery was already answered
by a later intra frame. Their recovery need is stale, but the overload observation
is still relevant. Session reset snapshots the counter to prevent replay on reconnect.
Network holes and codec refusals do not count as decoder-overload signals.

Every distinct missing frame still reaches reference invalidation. Coalescing
those IDs could make the sender predict from a picture the headset never decoded.
The fix concerns admission before encoding, rather than weakening recovery.

Validation: the complete streamer target builds successfully; both feedback paths
and reconnect counter reset were reviewed. **Live cadence improvement remains
unmeasured**; this change alone does not establish usable moving-head latency.
The installed Pico decoder retains the [materialized-copy optimization](https://github.com/nerdrx/nx-warp/commit/43d6f0e).
