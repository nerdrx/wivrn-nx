# Independent ASTC packing with fixed Zstd jobs

Experimental and **off by default**. On each native ASTC encoder, set the private
option `"_wivrn_astc_zstd_jobs": "1"` to request two Zstd workers, 512 KiB jobs
and no inter-job overlap (`ZSTD_c_overlapLog=1`). Each encoder owns its context;
parallel-eye packing is a separate opt-in option.

Only ordinary independent level-3 packing supports this option. Motion-delta,
compact and fast level-1 configurations ignore it with a startup log. Missing
multithreading support also logs a warning and retains legacy compression.
Server builds include worker support; no-server/Android builds keep it disabled.
Worker support alone does not activate worker compression.

Packets remain ordinary NAST v2 with standard Zstd-compressed ASTC data. There is
no new client format, spatial loss, transform or reconstruction pass. LZ4/raw
fallback, Zstd admission thresholds and bitrate control remain unchanged.

The same-fixture bundled-library check saved 0.864 and 1.025 ms paired mean in
two short CPU packing runs. One run regressed at p95. A separate asleep Pico
CPU replay reproduced the exact ASTC blocks with essentially unchanged decode
cost. **These do not prove live latency, fresh FPS or motion quality.** Extra
host workers may compete with a game, so leave the option off until a live A/B
test confirms useful headroom.

[Both runs, Pico gate, graphs and reproduction](https://github.com/nerdrx/nx-warp/tree/main/bench/results/90fps-2026-10-05/continuous/zstd-jobs)

`tests/nxastc_zstd_jobs_test.cpp` covers retained worker parameters, repeated
context use, disabled level-1/3 byte identity, ordinary packet decoding and
small high-entropy raw fallback. Its `--no-mt` mode requires a Zstd build without
worker support and checks safe configuration rejection plus legacy compression.

```sh
g++ -O2 -std=c++20 -Icommon -Ibuild-server/_deps/nx_zstd-src/lib \
    -Ibuild-server/_deps/nx_lz4-src/lib tests/nxastc_zstd_jobs_test.cpp \
    build-server/common/libwivrn-lz4.a \
    build-server/_deps/nx_zstd-build/lib/libzstd.a -pthread -o /tmp/nx-zstd-jobs
/tmp/nx-zstd-jobs
```
