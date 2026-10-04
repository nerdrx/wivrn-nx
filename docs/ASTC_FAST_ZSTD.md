# Faster independent ASTC compression trial

Set encoder option `_wivrn_astc_fast_zstd=1` to test Zstd level 1 instead of the
existing level 3 for independent ASTC packets. It is off by default and ignored
when motion-delta packing is enabled. The header, decompressor, ASTC blocks,
LZ4/raw fallback and 10% Zstd admission gate stay unchanged. There is still only
one Zstd attempt per packet. The option also applies to opt-in compact records.

A native q6 two-eye host CPU probe found lower packet-preparation time with
level 1 and approximately 3% more selected packet bytes. That can trade CPU cost
for transmission time; it does not guarantee the same quality rung after rate
control reacts to the extra bytes. Low bandwidth or different scenes may erase
the saving. Source is experimental: no active profile was changed or restarted.

The public report records matched/pinned follow-up timings, exact decode checks
and calculated wire costs. Calculated byte-transfer durations exclude actual
Wi-Fi, sharding, FEC, retransmissions and pipeline overlap. They are not latency
or FPS measurements. Physical photon latency and sustained moving-scene
performance remain unverified.
