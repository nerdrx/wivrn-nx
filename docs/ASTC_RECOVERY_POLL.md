# NXASTC recovery polling trial

This opt-in moves existing retransmit checks onto the stream poll's quiet period
when no video or control datagram arrives. NACK and FEC policy stay unchanged.
If the separate native ASTC frame deadline is enabled, quiet polling also
services its existing retirement policy. Disabled and ineligible states retain
the 100 ms fallback.

On Android, `debug.wivrn.nx.recovery_poll` must equal exactly `1` when the
network thread starts. Host builds accept `WIVRN_NX_RECOVERY_POLL=1` for local
testing. The default is off. Only NXASTC accumulators participate.

The timeout supplier runs after pending stream, control, and secondary packets
have been drained. It uses each incomplete frame's existing last-shard/NACK
timestamps and 2.5 ms quiet gate. An exact missing-shard check is deferred until
that gate is due, preserving unknown-tail and parity suppression; at most two
existing NACK rounds are possible. Completion checks use the received-count invariant;
exact missing-hole scans are deferred until due. Already-confirmed due repairs
use a zero poll timeout, avoiding an extra 1 ms wait; future deadlines still round
up to whole milliseconds. Servicing a repair advances its last-NACK timestamp
or exhausts its two-round budget before the next deadline calculation.
A successful poll services retirement and NACKs on the same
network thread under the existing decoder-array shared lock. Enabled polling
adds two XR-clock queries per poll cycle, not per shard; device overhead and
scheduler delays remain measurement gates. Send errors retain
the existing behavior, including consuming the round.

With Android `debug.wivrn.nx.astc_deadline=1`, an incomplete, nonempty front
frame and a newer complete frame also supply a wake-up at the front's first
receipt plus two display periods. This remains scheduled after NACK rounds are
exhausted or retransmission is disabled. The front is pumped before another
NACK check, so a retired frame is not requested again. Empty/complete fronts,
missing scenes and invalid clock/period values supply no retirement deadline;
overflow is rejected. The deadline option remains default-off and is sampled
when the accumulator is constructed.

This is a scheduling target, not a two-frame wall-clock guarantee. Future
timeouts round up to milliseconds; thread scheduling can delay service. Display
period changes are observed on subsequent polls. Completely absent frames
without a first-receipt timestamp keep the existing window/skew policy.

`tests/accumulator_test.cpp` exercises the shared production deadline helper and
real shard-set missing-hole rules. This validates deterministic scheduling logic;
it is not a live network, Android property, power, or headset result. Before
enabling outside a host test, verify startup property handling, idle power, and
repair behavior on the intended Android device. The setting is opt-in and should
remain off unless those checks justify it.
