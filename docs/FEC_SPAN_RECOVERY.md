# FEC recovery without temporary present-shard blobs

Recovery still starts with an owned parity copy and ends with an owned recovered
shard. Present shards are serialized through the same traits and field order,
but their existing spans are XORed directly into the recovery buffer instead of
being concatenated into another full blob first. Exact serialized lengths and
buffer bounds are validated before each XOR. Packet framing is unchanged.

Malformed parity testing exposed a pre-existing invalid-bool representation
load. Boolean deserialization now reads an unsigned byte, rejects values above
one, and only then creates a bool. Bool fields leave raw aggregate groups so
embedded invalid representations are checked too. Valid bytes and protocol
hashes remain unchanged: full protocol hash 416750f9c8bf9b90, bool 54ddd5c6099bcc3e,
view_info 6eb8b9b7a69b4fbc match source 27026bbd. Four representative video recovery
blobs (alpha false/true, timing absent/present) match the baseline byte-for-byte.

The focused production FEC test passes 4,292 checks normally and under combined
ASan/UBSan with halt-on-error enabled. It covers recovery ownership, metadata,
all lost-shard positions, K4/8/16, strided groups, malformed lengths and boolean
encodings. This does not prove complete headset recovery or fresh stereo rate.

Five matched ABBA blocks on a pinned desktop CPU compare baseline 27026bbd with
the combined direct-span and checked-bool implementation. Each invocation times
2,500 actual recoveries per condition. Across pooled condition/block averages,
median recovery cost falls 1.844 to 1.665 µs and requested allocation bytes 6,135
to 4,727. Reported p95 describes those per-condition loop averages, not the tail
latency of individual calls. Recovery is only exercised on loss; this result
cannot be converted into a per-frame or FPS saving.
