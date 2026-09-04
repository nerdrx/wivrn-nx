# NX Warp in the WiVRn NX client

The NX Warp codec (`nxvc`, from the [nx-warp](https://github.com/nerdrx/nx-warp) project)
as a WiVRn video codec, decoder side.

The **authoritative** description of the server/client contract is the header comment of
`server/encoder/nxwarp_packetize.h`. This document is the client's half: what
`decoder_nxwarp` does, where the settings switch is, and what is knowingly missing. The
codec's own formats are not restated — `docs/TRANSPORT.md` (transport) and `docs/SYNTAX.md`
(bitstream) in nx-warp own those, and both ends implement them unchanged.

---

## 1. The wire

### 1.1 `video_codec::nxwarp`

Appended after `raw`, so every existing value keeps its number. `protocol_version` is a
hash of the whole packet variant and the enum's names and values feed into it, so the
version changes on its own: client and server ship together, and a mismatched pair refuses
to connect rather than misparsing.

### 1.2 `to_headset::nxwarp_datagram`

```cpp
struct nxwarp_datagram
{
    uint8_t stream_item_idx;   // 0 left, 1 right, 2 alpha, 3 quad — as the shard path
    uint8_t path_id;           // nxt path 0 or 1 — or 0xFF, see below
    std::vector<uint8_t> payload;
};
```

* `path_id` 0 or 1: `payload` is **verbatim** TRANSPORT.md section 2 — the 24-byte
  cleartext header, the ciphertext, the 16-byte tag — and goes straight to
  `nxt::Receiver::on_datagram(payload, path_id, now_us, &tiles)`. One datagram per packet,
  on the lossy (UDP) stream socket.
* `path_id == 0xFF`: **not an nxt datagram**. It is the codec's raw `.nxv` stream header,
  sent on the control socket and repeated every 90 frames, and it goes to
  `nxvc_vk_decoder_parse_stream_header`. It is what creates both the decoder and the
  receiver on this side, because it carries the geometry both are sized from; datagrams
  arriving before it are dropped.

The shard path, the parity shards, the nacks and WiVRn's adaptive FEC are all bypassed: NX
Warp has its own FEC, its own retransmit policy and its own feedback, and running WiVRn's
on top would fight them.

### 1.3 `from_headset::nxwarp_feedback`

```cpp
struct nxwarp_feedback
{
    uint8_t stream_item_idx;
    uint8_t path_id;
    std::vector<uint8_t> payload;
};
```

`payload` is verbatim TRANSPORT.md section 8 — the 8-byte header, one to three band
records with their bitmaps, the 4-byte trailer — produced by
`nxt::Receiver::band_deadline()` and consumed by `nxt::Sender::on_feedback()`. Neither end
reads its contents. Sent on the **control (TCP) socket**: it is small, it is cumulative
over three bands, and a lost one costs the encoder a frame of shadow knowledge about what
the headset actually holds.

### 1.4 `nxt::StreamConfig`, which both ends must derive identically

The receiver derives its nonces, its band boundaries and its run payload budget from these,
so a single disagreement shows up as an authentication failure on every datagram rather
than as anything diagnostic.

| field | value |
|---|---|
| `stream_id` | the WiVRn `stream_item_idx` |
| `cols` | `nxvc_vkd_stream_info::tiles_x` |
| `rows` | `tile_count / tiles_x` |
| `band_rows` | `min(rows, 6)` |
| `layers` | 1 |
| `mtu` | 1280 |
| `caps` | `kCapFec \| kCapPoseHdr \| kCapRleFeedback` |

AEAD: `nxt::make_null_aead()`, `key[i] = i`, `salt[i] = 0xA0 + i`. Deliberate, not a
shortcut left for later — the datagram already travelled inside WiVRn's authenticated,
encrypted stream socket, so a second real AEAD would encrypt ciphertext, would need a key
exchange this integration has no reason to invent, and would put OpenSSL or libsodium into
the Android client, which does not otherwise carry either. The stage stays in the pipeline
so the day NX Warp gets a socket of its own, the only change is which `Aead` is built.

---

## 2. Frame ↔ tile grid

The frame bitstream is cut into `chunk_bytes` pieces and chunk *i* is carried by tile index
*i* of the grid, in raster order. Chunk 0 carries a 4-byte little-endian length prefix in
front of the frame.

```
chunk_bytes = run_payload_budget - kDirEntryBytes - kPoseHeaderBytes
```

The pose header is subtracted because the first datagram of every band replicates it
(`kCapPoseHdr`) and the packetizer charges it to the same budget — a tile sized to
`max_tile_bytes()` therefore cannot start a band, and the run comes back empty.

Reassembly (`client/decoder/nxwarp/nxwarp_reassemble.cpp`) is a concatenation in tile-index
order, a length check, and nothing else. It returns nothing — and the client drops the
frame — when the run of indices from 0 has a hole, when a chunk other than the last is
short, or when fewer bytes arrived than the prefix declares. The band's feedback has
already gone out by then, which is how the encoder learns to refresh.

**Why it is not one tile per tile yet:** the CPU reference codec's C ABI reports a tile's
payload *length* but not its *offset*, so the server cannot hand the transport real
per-tile spans. Everything else is real — the runs, the class-A parity, the pose header,
the band deadlines, the feedback, the client shadow — and the bytes round-trip exactly;
what is lost is per-tile independence, so a lost chunk costs the frame rather than one
tile. When the Vulkan encoder lands behind the server's `nxwarp_codec` the mapping becomes
the identity and **the client does not change**: it still reassembles tiles in index order.

A consequence worth knowing: the grid holds `tiles_per_frame * chunk_bytes` bytes, so a
frame larger than that cannot be sent at all. The server logs "raise QP" and drops it.

---

## 3. The headset switch

**Settings → Advanced → "NX Warp codec"** (`client/scenes/gui_settings.cpp`, the `##nxwarp`
entry in the `sec_advanced` section), backed by `configuration::nxwarp`, serialised as
`"nxwarp"`.

It re-orders the codec list in `from_headset::headset_info` and does nothing else: on,
`nxwarp` goes to the front of `supported_codecs`; off, it is removed. There is **no
protocol field for it** — the codec negotiation that already exists is enough, and a server
without an NX Warp encoder simply picks the next codec in the list. The change needs a new
encode session, so it belongs in the handshake and not among the live `settings_changed`
toggles; the setter calls `on_streaming_changed()` and the entry is disabled while
connected.

The manual "Video codec" combo deliberately does not list NX Warp: it has its own switch,
and having it in both would make one of them a lie.

---

## 4. What is built

| file | role |
|---|---|
| `client/decoder/nxwarp/nxwarp_reassemble.{h,cpp}` | section 2, and nothing else — no Vulkan, no decoder |
| `client/decoder/nxwarp/nxwarp_decoder.{h,cpp}` | the `wivrn::decoder`: receiver, reassembly, `nxvc_vk_decoder`, image pool, feedback |
| `client/decoder/nxwarp/tools/nxwarp_loopback.cpp` | the whole depacketize-and-reassemble path without a headset |

Threading follows nx-warp `docs/INTEGRATION.md` 2.5. `push_datagram()` runs on WiVRn's
single network thread, beside the `recvmmsg` loop, and does only what is safe there:
depacketize, copy tile bytes (the receiver's spans point into scratch it reuses on the next
call), fire band deadlines, hand feedback to the socket. The decode and the image copy are
a job on a worker thread, as `android::decoder` already does with its `sync_queue`.

`nxvc_vk_decoder` **adopts** the client's `VkInstance`, `VkPhysicalDevice`, `VkDevice`,
queue and queue family, so no second device is created, and its submits are made under the
same `application::get_queue()` lock every other submitter holds. Its output is
`NXVC_VKD_OUT_YCBCR420` — two-plane 4:2:0, nx-warp `docs/INTEGRATION-DECISIONS.md` 3 — and
the two images are copied into one `G8_B8R8_2PLANE_420_UNORM` image sampled through the
same `VkSamplerYcbcrConversion` the hardware path already uses. `reprojection.glsl` and
`stream_defoveator` are unchanged: they see an image view and a sampler.

nx-warp is built as an `ExternalProject`, not a subdirectory: its own CMake reads
`CMAKE_SOURCE_DIR` for its include root, its install rules and its CPack resources, so as a
subdirectory of WiVRn it would look for its headers and its licence inside this tree. Only
`vk/common`, `vk/decoder` and `transport` are configured, and the install step is
`cmake --install` rather than the install target, which would first *build* nx-warp's
`nxvc-vkdec` CLI — and that links `vkWaitSemaphores`, which the NDK's android-29
`libvulkan.so` does not export.

### Testing without a headset

```
nxwarp-loopback in.nxv out.nxv [--loss F] [--seed N] [--mtu N]
```

Lays a real frame onto the grid exactly as the server does, packetizes with `nxt::Sender`,
optionally drops datagrams, receives with `nxt::Receiver`, reassembles with the client's own
code, and writes the result back out. With no loss the output must be **byte identical** to
the input, which makes every stage in between — the chunk mapping, the length prefix, the
run packing, the AEAD, the class-A FEC, the position-addressed placement — provably
round-trip, without a GPU. With loss it must refuse rather than produce a plausible short
frame.

---

## 5. Known gaps

Named here rather than left to be discovered.

* **No `view_info` on the NX Warp wire.** `to_headset::nxwarp_datagram` has no pose, fov or
  foveation field, and the codec's own 26-byte pose header is opaque to the transport and
  carries neither fov nor foveation. Frames are therefore published with a
  default-constructed `view_info_t`, and the reprojection is wrong. The decoder says so once
  in the log. This is the first thing to fix, and it needs a change on both ends: either an
  optional `view_info` on `nxwarp_datagram` (cheapest — one field, present on the frame's
  first datagram, exactly as `video_stream_data_shard` does it), or a pose-only shard
  alongside.
* **The band deadline is arrival-driven, not clock-driven.** TRANSPORT.md 7.4 anchors it on
  the runtime's predicted display time; the decoder does not have one, so a band is closed
  when a datagram for a later band or a later frame arrives. `nxt::DeadlineController` still
  runs and the feedback content is identical, but a band the sender went quiet on is only
  closed by the next frame. Plumbing `xrWaitFrame`'s prediction down from `scenes::stream`
  is the fix.
* **No per-tile pose reprojection** (nx-warp PAPER 4.3, INTEGRATION.md 2.3). Every tile of a
  frame is warped from the frame's pose, as with every other codec.
* **No client-side reference ring.** `nxvc_vk_decoder` owns whatever references the stream
  needs; the client publishes into WiVRn's existing three-slot `latest_frames`. Inter
  prediction across frames is untested from this side.
* **No hybrid mode.** `nxwarp_hybrid` and the `AMediaCodec` base layer are not built.
* `recvmmsg`'s `num_messages` is still 20 (INTEGRATION.md 2.5 argues for 64).
* The decoder has never run against a live server or on a headset. Both ends build, and the
  loopback proves the wire and the container round-trip, but the two have not been connected.
