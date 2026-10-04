# Bounded ASTC packet buffer reuse

The assembler previously lost its vector allocation every time a complete
packet moved to the worker. The next packet grew through repeated insertions.
The decoder now keeps one spare allocation, transferred by clear/swap under
the existing decoder mutex. Completed worker packets and evicted queued packets
can return storage; an assembler with capacity already allocated keeps it.

Only a spare whose actual vector capacity is at most `raw_bytes + 32` is kept.
For native 2176² ASTC 8×8 this adds at most 1,183,776 bytes per decoder instance.
A vector grown beyond that bound is not cached. Active assembler, queued packets,
CPU decode scratch, reference caches and GPU images are separate allocations.
Shutdown does not accept new spare buffers.

Packet bytes still enter the assembler through the same checked inserts. The
optimization removes capacity growth/relocation; it does not remove incoming
payload copies, decompression or staging traffic. Worker recycling happens after
decode and handoff, on the image-pool drop path, and after caught decode errors.
The staging path retains ordinary CPU decode scratch plus a forward upload copy.

The exact production helper is tested for ownership, larger-spare selection,
capacity rejection and a 10,000-iteration producer/worker handoff under a real
mutex. Normal, ASan/UBSan and ThreadSanitizer tests pass. Existing packet decode,
motion and compact-v4 checks also pass; those format tests do not exercise the
Vulkan worker lifecycle.

A separate host component probe builds valid native q6 NX packets and divides
each into 270 contiguous fragments. Across 1,000 packets per fixture, growths fall
from 10,000 to 20: the recycled case needs only its first two startup allocations
cycles. Logical relocation bytes fall from 788,214,000 to 1,576,428 for the dark
fixture, and 489,064,000 to 978,128 for forest. Appended bytes are unchanged.
The sequential host timing comparison was not pinned or interleaved and is not
an end-to-end latency or Pico result; the deterministic allocation/relocation
counts are the retained evidence.

Run the production-helper test (no private inputs needed):

```sh
c++ -std=c++23 -O2 tests/astc_packet_buffer_recycle_test.cpp -lzstd -llz4 -pthread -o /tmp/astc-recycle-test
/tmp/astc-recycle-test
```

Optional private ASTC fixture paths run the component probe after ownership
checks. Do not publish supplied photographs or their raw payloads.
