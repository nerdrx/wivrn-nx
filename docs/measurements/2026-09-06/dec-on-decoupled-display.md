# Capture record: dec-on-decoupled-display

**An A/B that came out flat: against dec-off-coupled-display nothing moves by more than the run-to-run spread.**

The A leg of a decoupled-display A/B. TWO CAVEATS, both from the log rather than from the intent: neither capture carries a line naming which hand-over the session actually took, so the mode here is asserted by the FILENAME and by nothing else; and the measured difference against the B leg is 43.23 against 43.45 iterations/s, 93.06 against 92.69 ms of pose age and 16.74 against 16.73 ms of GPU -- flat. Either the setting did not take, or at this operating point it does not move these numbers. The record is kept as evidence of the second question, not as evidence for the first.

| | |
|---|---|
| server log | `/run/media/nerdrx/Lex/claude/nx-scratch/live/dec-on-server.log` (21417 B, 2026-09-06T15:13:01) |
| client capture | `/run/media/nerdrx/Lex/claude/nx-scratch/live/dec-on.log` (116635 B, 2026-09-06T15:13:01) |
| window | client 15:11:57 .. 15:13:01; server reports 0..end |
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
| paced frame rate | **38.03 fps** |
| bytes per frame (server) | **17150.83 B** |
| quantiser | **22** in [22..22] |
| server encode | **3.64 ms/frame** (worst 6) |
| client GPU per stereo frame | **16.74 ms** (passA 3 + passB 13.74) |
| client decode wall | **22.94 ms** |
| displayed pose age | **93.06 ms** mean, worst 154.2 |
| render loop | **43.23/s** against a 23.14 ms display period |
| display pass | **6.34 ms** per iteration |
| holes / refused | 0 / 0 |

### The Pass B split

`passB 13.74 ms = warp 0.1 + skip 8.23 + coded 5.45 + dir 0 + other 0` over tiles skip 288.97 / coded 289.03 / dir 0

The skip term is **60 %** of Pass B.

### GPU duty

display 6.34 ms x 43.23/s = **274 ms/s**; decode 16.74 ms x 38.0/s = **637 ms/s**; together **91 %** of one GPU.

## Server, every field

| field | n | mean | min | max | unit |
|---|---|---|---|---|---|
| `bytes_per_frame` | 30 | **17150.83** | 17147 | 17175 | B |
| `client_decode_ms` | 30 | **23.1** | 20.8 | 23.7 | ms |
| `controller_mbit` | 30 | **43.4** | 43.4 | 43.4 | Mbit/s |
| `encode_ms` | 30 | **3.64** | 3.5 | 3.8 | ms |
| `encode_ms_max` | 30 | **5.28** | 4.7 | 6 | ms |
| `frames_not_sent` | 30 | **93.93** | 91 | 100 | frames |
| `frames_per_report` | 30 | **87.23** | 81 | 91 | frames |
| `not_reconstructed` | 26 | **2.54** | 1 | 5 | frames |
| `not_reconstructed_cost_intra` | 26 | **2.54** | 1 | 5 | frames |
| `not_reconstructed_free` | 26 | **0** | 0 | 0 | frames |
| `paced_fps` | 30 | **38.03** | 37.1 | 39.8 | fps |
| `qp_hi` | 30 | **22** | 22 | 22 |  |
| `qp_lo` | 30 | **22** | 22 | 22 |  |
| `qp_mean` | 30 | **22** | 22 | 22 |  |
| `rate_error_pct` | 30 | **-87.97** | -88 | -87 | % |
| `report_seconds` | 30 | **2** | 2 | 2 | s |
| `target_bytes_per_frame` | 30 | **142524.87** | 135699 | 146059 | B |
| `tilemap_chunk_frames` | 30 | **0** | 0 | 0 | frames |
| `tilemap_span_frames` | 30 | **87.23** | 81 | 91 | frames |

## Client, every field

| field | n | mean | min | max | unit |
|---|---|---|---|---|---|
| `bytes_per_frame` | 32 | **17152.47** | 17142 | 17219 | B |
| `cache_represented` | 32 | **0** | 0 | 0 | frames |
| `defoveate_mpx` | 32 | **2.37** | 2.37 | 2.37 | Mpx |
| `defoveate_scale` | 32 | **0.5** | 0.5 | 0.5 |  |
| `display_pass_ms` | 32 | **6.34** | 6.3 | 6.4 | ms |
| `display_period_ms` | 32 | **23.14** | 22.2 | 23.7 | ms |
| `frames_per_report` | 32 | **84.97** | 81 | 86 | frames |
| `gpu_ms_per_stereo_frame` | 32 | **16.74** | 16.6 | 17 | ms |
| `holes` | 32 | **0** | 0 | 0 | frames |
| `iter_fence_ms` | 32 | **4.75** | 2.7 | 7.7 | ms |
| `iter_fence_worst_ms` | 32 | **20.3** | 10.2 | 23.7 | ms |
| `iter_queries_ms` | 32 | **0** | 0 | 0 | ms |
| `iter_render_ms` | 32 | **5.23** | 3.1 | 8.2 | ms |
| `iter_submit_ms` | 32 | **0** | 0 | 0 | ms |
| `loop_gated_out` | 32 | **0** | 0 | 0 | iterations |
| `loop_iterations` | 32 | **86.91** | 85 | 90 | iterations |
| `loop_nothing_to_show` | 32 | **0** | 0 | 0 | iterations |
| `loop_rate` | 32 | **43.23** | 42.2 | 45 | /s |
| `loop_submitted_layer` | 32 | **86.91** | 85 | 90 | iterations |
| `misses_late` | 32 | **0** | 0 | 0 | frames |
| `misses_overrun` | 32 | **0** | 0 | 0 | frames |
| `misses_skipped_refresh` | 32 | **0.34** | 0 | 1 | frames |
| `net_datagrams_per_report` | 32 | **10020.56** | 8554 | 10440 | datagrams |
| `net_decoded_total` | 32 | last **4250** | first 1618 | +2632 | frames |
| `net_frames_closed` | 32 | **87.09** | 81 | 90 | frames |
| `net_frames_late` | 32 | **0** | 0 | 0 | frames |
| `net_frames_with_hole` | 32 | **0** | 0 | 0 | frames |
| `net_ms_per_datagram` | 32 | **0.01** | 0.01 | 0.01 | ms |
| `net_ms_per_second` | 32 | **61.14** | 56.8 | 64.47 | ms/s |
| `net_out_of_order` | 32 | **0** | 0 | 0 | datagrams |
| `net_queued_for_worker` | 32 | **0.03** | 0 | 1 | frames |
| `net_stragglers_dropped` | 32 | last **4574** | first 1874 | +2700 | datagrams |
| `passA_ms` | 32 | **3** | 2.9 | 3.1 | ms |
| `passB_coded_ms` | 32 | **5.45** | 5.4 | 5.5 | ms |
| `passB_dir_ms` | 32 | **0** | 0 | 0 | ms |
| `passB_ms` | 32 | **13.74** | 13.5 | 13.9 | ms |
| `passB_other_ms` | 32 | **0** | 0 | 0 | ms |
| `passB_skip_ms` | 32 | **8.23** | 8.1 | 8.4 | ms |
| `passB_tiles_coded` | 32 | **289.03** | 289 | 290 | tiles |
| `passB_tiles_dir` | 32 | **0** | 0 | 0 | tiles |
| `passB_tiles_skip` | 32 | **288.97** | 288 | 289 | tiles |
| `passB_total_ms` | 32 | **13.74** | 13.5 | 13.9 | ms |
| `passB_warp_ms` | 32 | **0.1** | 0.1 | 0.1 | ms |
| `pose_age_mean_ms` | 32 | **93.06** | 88.3 | 98 | ms |
| `pose_age_worst_ms` | 32 | **130.18** | 108.9 | 154.2 | ms |
| `pose_frames` | 32 | **86.91** | 85 | 90 | frames |
| `reassembly_buffer_kB` | 32 | **17** | 17 | 17 | kB |
| `reassembly_held_kB` | 32 | **17** | 17 | 17 | kB |
| `reassembly_ms_per_frame` | 32 | **0.03** | 0.02 | 0.03 | ms |
| `reassembly_ratio` | 32 | **1** | 1 | 1 | x |
| `refused` | 32 | **0** | 0 | 0 | frames |
| `report_seconds` | 32 | **2** | 2 | 2 | s |
| `rx_datagrams` | 32 | last **504305** | first 193739 | +310566 | datagrams |
| `rx_late` | 32 | last **444** | first 311 | +133 | datagrams |
| `rx_placed` | 32 | last **1317160** | first 536727 | +780433 | datagrams |
| `sel_arrival_ms` | 32 | **23.44** | 22 | 26.4 | ms |
| `sel_decoded` | 32 | last **4250** | first 1618 | +2632 | frames |
| `sel_dropped_late` | 32 | last **221** | first 155 | +66 | frames |
| `sel_withheld` | 32 | last **138** | first 72 | +66 | frames |
| `stage_arrival_ms` | 32 | **23.2** | 22 | 26.4 | ms |
| `stage_fence_post_ms` | 32 | **22.08** | 21.7 | 22.7 | ms |
| `stage_fence_pre_ms` | 32 | **0** | 0 | 0 | ms |
| `stage_gap_ms` | 32 | **0.76** | 0.1 | 2.1 | ms |
| `stage_get_free_ms` | 32 | **0** | 0 | 0 | ms |
| `stage_publish_ms` | 32 | **0** | 0 | 0 | ms |
| `stage_qsubmit_ms` | 32 | **0** | 0 | 0 | ms |
| `stage_record_ms` | 32 | **0** | 0 | 0 | ms |
| `stage_stride` | 32 | **1** | 1 | 1 |  |
| `stage_submit_codec_ms` | 32 | **0.8** | 0.8 | 0.8 | ms |
| `stage_submit_ms` | 32 | **0.8** | 0.8 | 0.8 | ms |
| `stage_submit_qlock_ms` | 32 | **0** | 0 | 0 | ms |
| `stage_wait_prev_ms` | 32 | **0** | 0 | 0 | ms |
| `stage_withheld` | 32 | **2.16** | 0 | 4 | frames |
| `submit_lead_mean_ms` | 32 | **46.91** | 45.6 | 48.5 | ms |
| `submit_lead_worst_ms` | 32 | **28.84** | 19.1 | 38.9 | ms |
| `wait_prev_ms` | 32 | **0** | 0 | 0 | ms |
| `wall_ms` | 32 | **22.94** | 22.5 | 23.5 | ms |

## Provenance

```json
{
 "server": {
  "path": "/run/media/nerdrx/Lex/claude/nx-scratch/live/dec-on-server.log",
  "bytes": 21417,
  "mtime": "2026-09-06T15:13:01",
  "sha256": "bd2d20f50633b3c114bb99ac03f8a8fac4d42db588b9c026ab19a2038542b0ba"
 },
 "client": {
  "path": "/run/media/nerdrx/Lex/claude/nx-scratch/live/dec-on.log",
  "bytes": 116635,
  "mtime": "2026-09-06T15:13:01",
  "sha256": "ee475f71351f037bd90db45bd29263424c70fd1b6eef039cd376415b259cac3f"
 }
}
```

_Generated by `tools/capture-record.py` on 2026-09-06T20:52:38. Every figure above is parsed from the logs named; nothing is entered by hand._
