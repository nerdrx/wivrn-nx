# Capture record: dec-off-coupled-display

**The B leg of the flat A/B; see dec-on-decoupled-display for what that means.**

The B leg of a decoupled-display A/B. As with the A leg, the mode is asserted by the filename: no line in this capture names the hand-over the session took. The two legs agree to within the run-to-run spread on every field.

| | |
|---|---|
| server log | `/run/media/nerdrx/Lex/claude/nx-scratch/live/dec-off-server.log` (21270 B, 2026-09-06T15:15:25) |
| client capture | `/run/media/nerdrx/Lex/claude/nx-scratch/live/dec-off.log` (116616 B, 2026-09-06T15:15:25) |
| window | client 15:14:22 .. 15:15:25; server reports 0..end |
| server build | _not carried by this log excerpt_ |
| server GPU | _not carried by this log excerpt_ |
| encoder backend | _not carried by this log excerpt_ |
| stream geometry | _not carried by this log excerpt_ |
| negotiated tools | _not carried by this log excerpt_ |
| entropy | _not carried by this log excerpt_ |
| encode reports in log | 30 |
| client stat lines that did not parse | 0 |
| Wi-Fi band / RSSI | not carried by this capture |

## The operating point

| | |
|---|---|
| paced frame rate | **38.24 fps** |
| bytes per frame (server) | **17149.93 B** |
| quantiser | **22** in [22..22] |
| server encode | **3.66 ms/frame** (worst 8.3) |
| client GPU per stereo frame | **16.73 ms** (passA 2.98 + passB 13.78) |
| client decode wall | **22.89 ms** |
| displayed pose age | **92.69 ms** mean, worst 154.9 |
| render loop | **43.45/s** against a 23.03 ms display period |
| display pass | **6.36 ms** per iteration |
| holes / refused | 0 / 0 |

### The Pass B split

`passB 13.78 ms = warp 0.1 + skip 8.27 + coded 5.43 + dir 0 + other 0` over tiles skip 288.97 / coded 289.03 / dir 0

The skip term is **60 %** of Pass B.

### GPU duty

display 6.36 ms x 43.45/s = **276 ms/s**; decode 16.73 ms x 38.2/s = **640 ms/s**; together **92 %** of one GPU.

## Server, every field

| field | n | mean | min | max | unit |
|---|---|---|---|---|---|
| `bytes_per_frame` | 30 | **17149.93** | 17147 | 17174 | B |
| `client_decode_ms` | 30 | **22.83** | 19.9 | 23.7 | ms |
| `controller_mbit` | 30 | **43.4** | 43.4 | 43.4 | Mbit/s |
| `encode_ms` | 30 | **3.66** | 3.5 | 4 | ms |
| `encode_ms_max` | 30 | **5.39** | 4.6 | 8.3 | ms |
| `frames_not_sent` | 30 | **94.63** | 92 | 100 | frames |
| `frames_per_report` | 30 | **86.53** | 82 | 90 | frames |
| `not_reconstructed` | 25 | **2.28** | 1 | 4 | frames |
| `not_reconstructed_cost_intra` | 25 | **2.28** | 1 | 4 | frames |
| `not_reconstructed_free` | 25 | **0** | 0 | 0 | frames |
| `paced_fps` | 30 | **38.24** | 37.2 | 40.5 | fps |
| `qp_hi` | 30 | **22** | 22 | 22 |  |
| `qp_lo` | 30 | **22** | 22 | 22 |  |
| `qp_mean` | 30 | **22** | 22 | 22 |  |
| `rate_error_pct` | 30 | **-87.93** | -88 | -87 | % |
| `report_seconds` | 30 | **2** | 2 | 2 | s |
| `target_bytes_per_frame` | 30 | **141840.3** | 133507 | 145778 | B |
| `tilemap_chunk_frames` | 30 | **0** | 0 | 0 | frames |
| `tilemap_span_frames` | 30 | **86.53** | 82 | 90 | frames |

## Client, every field

| field | n | mean | min | max | unit |
|---|---|---|---|---|---|
| `bytes_per_frame` | 32 | **17149.62** | 17144 | 17176 | B |
| `cache_represented` | 32 | **0** | 0 | 0 | frames |
| `defoveate_mpx` | 32 | **2.37** | 2.37 | 2.37 | Mpx |
| `defoveate_scale` | 32 | **0.5** | 0.5 | 0.5 |  |
| `display_pass_ms` | 32 | **6.36** | 6.3 | 6.4 | ms |
| `display_period_ms` | 32 | **23.03** | 22.2 | 23.7 | ms |
| `frames_per_report` | 32 | **84.66** | 81 | 86 | frames |
| `gpu_ms_per_stereo_frame` | 32 | **16.73** | 16.6 | 16.9 | ms |
| `holes` | 32 | **0** | 0 | 0 | frames |
| `iter_fence_ms` | 32 | **5.12** | 2.6 | 8.1 | ms |
| `iter_fence_worst_ms` | 32 | **21.15** | 10.6 | 23.7 | ms |
| `iter_queries_ms` | 32 | **0** | 0 | 0 | ms |
| `iter_render_ms` | 32 | **5.6** | 3 | 8.5 | ms |
| `iter_submit_ms` | 32 | **0** | 0 | 0 | ms |
| `loop_gated_out` | 32 | **0** | 0 | 0 | iterations |
| `loop_iterations` | 32 | **87.41** | 85 | 90 | iterations |
| `loop_nothing_to_show` | 32 | **0** | 0 | 0 | iterations |
| `loop_rate` | 32 | **43.45** | 42.2 | 45 | /s |
| `loop_submitted_layer` | 32 | **87.41** | 85 | 90 | iterations |
| `misses_late` | 32 | **0** | 0 | 0 | frames |
| `misses_overrun` | 32 | **0** | 0 | 0 | frames |
| `misses_skipped_refresh` | 32 | **0.34** | 0 | 1 | frames |
| `net_datagrams_per_report` | 32 | **10000.12** | 8617 | 10440 | datagrams |
| `net_decoded_total` | 32 | last **4238** | first 1611 | +2627 | frames |
| `net_frames_closed` | 32 | **86.69** | 81 | 90 | frames |
| `net_frames_late` | 32 | **0** | 0 | 0 | frames |
| `net_frames_with_hole` | 32 | **0** | 0 | 0 | frames |
| `net_ms_per_datagram` | 32 | **0.01** | 0.01 | 0.01 | ms |
| `net_ms_per_second` | 32 | **61.9** | 58.11 | 64.18 | ms/s |
| `net_out_of_order` | 32 | **0** | 0 | 0 | datagrams |
| `net_queued_for_worker` | 32 | **0** | 0 | 0 | frames |
| `net_stragglers_dropped` | 32 | last **4549** | first 1862 | +2687 | datagrams |
| `passA_ms` | 32 | **2.98** | 2.9 | 3 | ms |
| `passB_coded_ms` | 32 | **5.43** | 5.4 | 5.5 | ms |
| `passB_dir_ms` | 32 | **0** | 0 | 0 | ms |
| `passB_ms` | 32 | **13.78** | 13.6 | 13.9 | ms |
| `passB_other_ms` | 32 | **0** | 0 | 0 | ms |
| `passB_skip_ms` | 32 | **8.27** | 8.1 | 8.4 | ms |
| `passB_tiles_coded` | 32 | **289.03** | 289 | 290 | tiles |
| `passB_tiles_dir` | 32 | **0** | 0 | 0 | tiles |
| `passB_tiles_skip` | 32 | **288.97** | 288 | 289 | tiles |
| `passB_total_ms` | 32 | **13.78** | 13.6 | 13.9 | ms |
| `passB_warp_ms` | 32 | **0.1** | 0.1 | 0.1 | ms |
| `pose_age_mean_ms` | 32 | **92.69** | 88.7 | 95.3 | ms |
| `pose_age_worst_ms` | 32 | **131.57** | 112.2 | 154.9 | ms |
| `pose_frames` | 32 | **87.41** | 85 | 90 | frames |
| `reassembly_buffer_kB` | 32 | **17** | 17 | 17 | kB |
| `reassembly_held_kB` | 32 | **17** | 17 | 17 | kB |
| `reassembly_ms_per_frame` | 32 | **0.03** | 0.02 | 0.03 | ms |
| `reassembly_ratio` | 32 | **1** | 1 | 1 | x |
| `refused` | 32 | **0** | 0 | 0 | frames |
| `report_seconds` | 32 | **2** | 2 | 2 | s |
| `rx_datagrams` | 32 | last **501883** | first 191971 | +309912 | datagrams |
| `rx_late` | 32 | last **142** | first 56 | +86 | datagrams |
| `rx_placed` | 32 | last **1309323** | first 532696 | +776627 | datagrams |
| `sel_arrival_ms` | 32 | **23.26** | 22 | 26.7 | ms |
| `sel_decoded` | 32 | last **4238** | first 1611 | +2627 | frames |
| `sel_dropped_late` | 32 | last **207** | first 147 | +60 | frames |
| `sel_withheld` | 32 | last **124** | first 64 | +60 | frames |
| `stage_arrival_ms` | 32 | **23.46** | 22 | 30.5 | ms |
| `stage_fence_post_ms` | 32 | **22.03** | 21.6 | 22.5 | ms |
| `stage_fence_pre_ms` | 32 | **0** | 0 | 0 | ms |
| `stage_gap_ms` | 32 | **0.88** | 0.1 | 2.2 | ms |
| `stage_get_free_ms` | 32 | **0** | 0 | 0 | ms |
| `stage_publish_ms` | 32 | **0** | 0 | 0 | ms |
| `stage_qsubmit_ms` | 32 | **0** | 0 | 0 | ms |
| `stage_record_ms` | 32 | **0** | 0 | 0 | ms |
| `stage_stride` | 32 | **1** | 1 | 1 |  |
| `stage_submit_codec_ms` | 32 | **0.8** | 0.8 | 0.8 | ms |
| `stage_submit_ms` | 32 | **0.8** | 0.8 | 0.8 | ms |
| `stage_submit_qlock_ms` | 32 | **0** | 0 | 0 | ms |
| `stage_wait_prev_ms` | 32 | **0** | 0 | 0 | ms |
| `stage_withheld` | 32 | **1.97** | 0 | 4 | frames |
| `submit_lead_mean_ms` | 32 | **46.78** | 45.5 | 47.9 | ms |
| `submit_lead_worst_ms` | 32 | **27.49** | 20.5 | 39.1 | ms |
| `wait_prev_ms` | 32 | **0** | 0 | 0 | ms |
| `wall_ms` | 32 | **22.89** | 22.5 | 23.4 | ms |

## Provenance

```json
{
 "server": {
  "path": "/run/media/nerdrx/Lex/claude/nx-scratch/live/dec-off-server.log",
  "bytes": 21270,
  "mtime": "2026-09-06T15:15:25",
  "sha256": "2cf213bb55be61fce7dd0804ad17cb64eb8b9e46eee884d486ff3e8d5393717b"
 },
 "client": {
  "path": "/run/media/nerdrx/Lex/claude/nx-scratch/live/dec-off.log",
  "bytes": 116616,
  "mtime": "2026-09-06T15:15:25",
  "sha256": "1a86d72c47da494a7d10c17c2479f2943c6c5ed2fa70833833844fac101278ad"
 }
}
```

_Generated by `tools/capture-record.py` on 2026-09-06T20:52:38. Every figure above is parsed from the logs named; nothing is entered by hand._
