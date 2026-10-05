# NXASTC recovery polling trial

This opt-in moves existing retransmit checks onto the stream poll's quiet period
when no video or control datagram arrives. It does not change the NACK policy,
frame retirement or FEC behavior. Disabled and ineligible states retain the
100 ms fallback.

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
A successful poll services NACKs on the same
network thread under the existing decoder-array shared lock. Enabled polling
adds two XR-clock queries per poll cycle, not per shard; device overhead and
scheduler delays remain measurement gates. Send errors retain
the existing behavior, including consuming the round.

`tests/accumulator_test.cpp` exercises the shared production deadline helper and
real shard-set missing-hole rules. This validates deterministic scheduling logic;
it is not a live network, Android property, power, or headset result. Before
enabling outside a host test, verify startup property handling, idle power, and
repair behavior on the intended Android device. The setting is opt-in and should
remain off unless those checks justify it.
