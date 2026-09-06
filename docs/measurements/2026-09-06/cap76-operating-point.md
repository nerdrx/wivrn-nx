# Capture record: cap76-operating-point

**The shipped operating point: 16.69 ms of Adreno per stereo frame, 92 ms of pose age, and a GPU already at 92 %.**

90 seconds of a live Pico 4 session on the nx-warp-e2e head of the day. This is the capture every operating-point number in docs/CLIENT-DECODE-WALL.md and docs/CLIENT-REPROJECTION.md was read out of by eye.

| | |
|---|---|
| server log | `/run/media/nerdrx/Lex/claude/nx-scratch/live/cap76-server.log` (31615 B, 2026-09-06T13:09:35) |
| client capture | `/run/media/nerdrx/Lex/claude/nx-scratch/live/cap76.log` (167670 B, 2026-09-06T13:09:35) |
| window | client 13:08:03 .. 13:09:34; server reports 0..end |
| server build | _not carried by this log excerpt_ |
| server GPU | _not carried by this log excerpt_ |
| encoder backend | _not carried by this log excerpt_ |
| stream geometry | _not carried by this log excerpt_ |
| negotiated tools | _not carried by this log excerpt_ |
| entropy | _not carried by this log excerpt_ |
| encode reports in log | 45 |
| client stat lines that did not parse | 0 |
| Wi-Fi band / RSSI | not carried by this capture |

## The operating point

| | |
|---|---|
| paced frame rate | **38.46 fps** |
| bytes per frame (server) | **17148.27 B** |
| quantiser | **22** in [22..22] |
| server encode | **3.55 ms/frame** (worst 12.6) |
| client GPU per stereo frame | **16.69 ms** (passA 2.97 + passB 13.72) |
| client decode wall | **22.78 ms** |
| displayed pose age | **91.9 ms** mean, worst 154.7 |
| render loop | **43.64/s** against a 22.93 ms display period |
| display pass | **6.37 ms** per iteration |
| holes / refused | 0 / 0 |

### The Pass B split

`passB 13.72 ms = warp 0.1 + skip 8.25 + coded 5.41 + dir 0 + other 0` over tiles skip 289 / coded 289 / dir 0

The skip term is **60 %** of Pass B.

### GPU duty

display 6.37 ms x 43.64/s = **278 ms/s**; decode 16.69 ms x 38.5/s = **642 ms/s**; together **92 %** of one GPU.

## Server, every field

| field | n | mean | min | max | unit |
|---|---|---|---|---|---|
| `bytes_per_frame` | 45 | **17148.27** | 17147 | 17150 | B |
| `client_decode_ms` | 45 | **22.81** | 19.8 | 23.7 | ms |
| `controller_mbit` | 45 | **43.4** | 43.4 | 43.4 | Mbit/s |
| `encode_ms` | 45 | **3.55** | 3.3 | 4.6 | ms |
| `encode_ms_max` | 45 | **5.69** | 4.5 | 12.6 | ms |
| `frames_not_sent` | 45 | **94.07** | 90 | 102 | frames |
| `frames_per_report` | 45 | **86.89** | 80 | 90 | frames |
| `not_reconstructed` | 37 | **2.81** | 1 | 4 | frames |
| `not_reconstructed_cost_intra` | 37 | **2.81** | 1 | 4 | frames |
| `not_reconstructed_free` | 37 | **0** | 0 | 0 | frames |
| `paced_fps` | 45 | **38.46** | 37.2 | 42.1 | fps |
| `qp_hi` | 45 | **22** | 22 | 22 |  |
| `qp_lo` | 45 | **22** | 22 | 22 |  |
| `qp_mean` | 45 | **22** | 22 | 22 |  |
| `rate_error_pct` | 45 | **-87.89** | -88 | -87 | % |
| `report_seconds` | 45 | **2** | 2 | 2 | s |
| `target_bytes_per_frame` | 45 | **140951.2** | 129004 | 145884 | B |
| `tilemap_chunk_frames` | 45 | **0** | 0 | 0 | frames |
| `tilemap_span_frames` | 45 | **86.89** | 80 | 90 | frames |

## Client, every field

| field | n | mean | min | max | unit |
|---|---|---|---|---|---|
| `bytes_per_frame` | 46 | **17148.09** | 17144 | 17152 | B |
| `cache_represented` | 46 | **0** | 0 | 0 | frames |
| `defoveate_mpx` | 46 | **2.37** | 2.37 | 2.37 | Mpx |
| `defoveate_scale` | 46 | **0.5** | 0.5 | 0.5 |  |
| `display_pass_ms` | 46 | **6.37** | 6.3 | 6.5 | ms |
| `display_period_ms` | 46 | **22.93** | 21.4 | 23.5 | ms |
| `frames_per_report` | 46 | **84.67** | 80 | 86 | frames |
| `gpu_ms_per_stereo_frame` | 46 | **16.69** | 16.5 | 17 | ms |
| `holes` | 46 | **0** | 0 | 0 | frames |
| `iter_fence_ms` | 46 | **5.06** | 2 | 7.9 | ms |
| `iter_fence_worst_ms` | 46 | **20.97** | 9.7 | 23.8 | ms |
| `iter_queries_ms` | 46 | **0** | 0 | 0 | ms |
| `iter_render_ms` | 46 | **5.47** | 2.4 | 8.3 | ms |
| `iter_submit_ms` | 46 | **0** | 0 | 0 | ms |
| `loop_gated_out` | 46 | **0** | 0 | 0 | iterations |
| `loop_iterations` | 46 | **87.83** | 85 | 94 | iterations |
| `loop_nothing_to_show` | 46 | **0** | 0 | 0 | iterations |
| `loop_rate` | 46 | **43.64** | 42.3 | 47 | /s |
| `loop_submitted_layer` | 46 | **87.83** | 85 | 94 | iterations |
| `misses_late` | 46 | **0** | 0 | 0 | frames |
| `misses_overrun` | 46 | **0** | 0 | 0 | frames |
| `misses_skipped_refresh` | 46 | **0.35** | 0 | 1 | frames |
| `net_datagrams_per_report` | 46 | **10094.52** | 9280 | 10556 | datagrams |
| `net_decoded_total` | 46 | last **5554** | first 1744 | +3810 | frames |
| `net_frames_closed` | 46 | **87.02** | 80 | 91 | frames |
| `net_frames_late` | 46 | **0** | 0 | 0 | frames |
| `net_frames_with_hole` | 46 | **0** | 0 | 0 | frames |
| `net_ms_per_datagram` | 46 | **0.01** | 0.01 | 0.01 | ms |
| `net_ms_per_second` | 46 | **44.59** | 40.69 | 46.49 | ms/s |
| `net_out_of_order` | 46 | **0** | 0 | 0 | datagrams |
| `net_queued_for_worker` | 46 | **0.02** | 0 | 1 | frames |
| `net_stragglers_dropped` | 46 | last **5890** | first 1977 | +3913 | datagrams |
| `passA_ms` | 46 | **2.97** | 2.9 | 3.1 | ms |
| `passB_coded_ms` | 46 | **5.41** | 5.3 | 5.5 | ms |
| `passB_dir_ms` | 46 | **0** | 0 | 0 | ms |
| `passB_ms` | 46 | **13.72** | 13.6 | 14 | ms |
| `passB_other_ms` | 46 | **0** | 0 | 0 | ms |
| `passB_skip_ms` | 46 | **8.25** | 8 | 8.5 | ms |
| `passB_tiles_coded` | 46 | **289** | 289 | 289 | tiles |
| `passB_tiles_dir` | 46 | **0** | 0 | 0 | tiles |
| `passB_tiles_skip` | 46 | **289** | 289 | 289 | tiles |
| `passB_total_ms` | 46 | **13.72** | 13.6 | 14 | ms |
| `passB_warp_ms` | 46 | **0.1** | 0.1 | 0.1 | ms |
| `pose_age_mean_ms` | 46 | **91.9** | 82.4 | 96.8 | ms |
| `pose_age_worst_ms` | 46 | **131.8** | 108.7 | 154.7 | ms |
| `pose_frames` | 46 | **87.83** | 85 | 94 | frames |
| `reassembly_buffer_kB` | 46 | **17** | 17 | 17 | kB |
| `reassembly_held_kB` | 46 | **17** | 17 | 17 | kB |
| `reassembly_ms_per_frame` | 46 | **0.02** | 0.02 | 0.03 | ms |
| `reassembly_ratio` | 46 | **1** | 1 | 1 | x |
| `refused` | 46 | **0** | 0 | 0 | frames |
| `report_seconds` | 46 | **2** | 2 | 2 | s |
| `rx_datagrams` | 46 | last **660272** | first 206364 | +453908 | datagrams |
| `rx_late` | 46 | last **271** | first 271 | +0 | datagrams |
| `rx_placed` | 46 | last **1699917** | first 569062 | +1130855 | datagrams |
| `sel_arrival_ms` | 46 | **23.17** | 21.9 | 27 | ms |
| `sel_decoded` | 46 | last **5554** | first 1744 | +3810 | frames |
| `sel_dropped_late` | 46 | last **239** | first 135 | +104 | frames |
| `sel_withheld` | 46 | last **151** | first 47 | +104 | frames |
| `stage_arrival_ms` | 46 | **23.19** | 21.9 | 28.2 | ms |
| `stage_fence_post_ms` | 46 | **22.08** | 21 | 22.7 | ms |
| `stage_fence_pre_ms` | 46 | **0** | 0 | 0 | ms |
| `stage_gap_ms` | 46 | **0.99** | 0.3 | 3.2 | ms |
| `stage_get_free_ms` | 46 | **0** | 0 | 0 | ms |
| `stage_publish_ms` | 46 | **0** | 0 | 0 | ms |
| `stage_qsubmit_ms` | 46 | **0** | 0 | 0 | ms |
| `stage_record_ms` | 46 | **0** | 0 | 0 | ms |
| `stage_stride` | 46 | **1** | 1 | 1 |  |
| `stage_submit_codec_ms` | 46 | **0.6** | 0.6 | 0.7 | ms |
| `stage_submit_ms` | 46 | **0.6** | 0.6 | 0.7 | ms |
| `stage_submit_qlock_ms` | 46 | **0** | 0 | 0 | ms |
| `stage_wait_prev_ms` | 46 | **0** | 0 | 0 | ms |
| `stage_withheld` | 46 | **2.33** | 0 | 4 | frames |
| `submit_lead_mean_ms` | 46 | **46.67** | 44 | 48.3 | ms |
| `submit_lead_worst_ms` | 46 | **28.02** | 19.1 | 36.8 | ms |
| `wait_prev_ms` | 46 | **0** | 0 | 0 | ms |
| `wall_ms` | 46 | **22.78** | 21.7 | 23.4 | ms |

## Provenance

```json
{
 "server": {
  "path": "/run/media/nerdrx/Lex/claude/nx-scratch/live/cap76-server.log",
  "bytes": 31615,
  "mtime": "2026-09-06T13:09:35",
  "sha256": "a9946427d1307b3c585263f4234dc6adfc58dbc887b527f830714fcd09f5c374"
 },
 "client": {
  "path": "/run/media/nerdrx/Lex/claude/nx-scratch/live/cap76.log",
  "bytes": 167670,
  "mtime": "2026-09-06T13:09:35",
  "sha256": "b64d24c778c248250f151bf106c36eb41a72ca59a86bd1384f0f869782123832"
 }
}
```

_Generated by `tools/capture-record.py` on 2026-09-06T20:52:38. Every figure above is parsed from the logs named; nothing is entered by hand._
