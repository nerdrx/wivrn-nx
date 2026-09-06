# Tile streaming: the transport half of the atlas

ADR 0029 Cheat 1, from the client's side.

> **1. No whole-frame requirement on the client.** Coded tiles are applied to the atlas as they
> arrive; display never waits for a frame to complete. The frame id only orders atlas generations.

This is the design for that. It is written before the code because two of the three pieces it needs
are owned by other components, and guessing at either would produce a client that cannot be joined
to them.

## 1. What the client does today, and why none of it survives

Today the NX Warp client reassembles a **whole frame** and only then decodes it:

* `nxwarp_reassemble` concatenates a frame's tiles in index order, strips the 4-byte length prefix
  and checks the frame really is that long. A frame with a hole is **undecodable and dropped**.
* the decoded unit goes on a bounded queue of **one**, and a frame that arrives while one is queued
  displaces it — 75 to 99 frames per two seconds on the live loop.
* the display shows whatever the last completed frame produced.

Under `ATLAS` every one of those is wrong rather than merely slow:

| today | under `ATLAS` |
|---|---|
| a hole makes the frame undecodable | a hole costs exactly the tiles that were in it; SYNTAX 13.12.6 — the client "does nothing", its atlas entries keep the generation they had |
| the decoder's unit of work is a frame | the unit is a coded tile; a frame is only a generation label |
| the queue holds frames and drops them whole | there is nothing to queue: a tile is applied when it arrives |
| display waits for a decode | display is a warp from the atlas at panel rate, never waiting (13.12.5) |

## 2. The ordering rule, and the trap in it

SYNTAX 13.12.3 makes the within-frame case free:

> The order of atlas writes within a frame is unobservable in the finished atlas, because every tile
> writes only its own position and reads only positions written before this frame. A decoder may
> therefore apply coded tiles as they arrive and need not assemble a whole frame.

**Across** frames it is not free, and the rule is `src_frame` monotonicity. Step 3 sets
`src_frame := N` for a coded tile, so a tile position's source frame only ever moves forward:

> A tile of frame `N` arriving at position `t` is applied if and only if `t`'s current
> `src_frame < N`. Otherwise it is dropped.

If frame `N+1` coded `t`, `t.src_frame` is `N+1` and frame `N`'s late tile is dropped — it would
move the position backwards. If `N+1` did not code `t`, `t.src_frame` is still older than `N` and
the tile is applied. That is the whole rule and it needs no new state.

### The trap: the advance is per frame, the arrivals are not

Step 1 advances **every valid entry** by `H_N` once per frame. A tile of frame `N` must read its
entry's `C` *after frame N's advance and no later* (13.12.4). If the client has already advanced the
atlas to frame `M > N` for display, the stored `C` is composed too far, and it cannot be walked back:
`renorm` is an integer round-to-nearest division, so `C · H · H⁻¹ ≠ C`. Un-advancing is not
bit-exact and would fail conformance.

The resolution is that **the advance does not have to be eager, and display does not have to
advance anything.**

* 13.12.5 has the display compose `C` with the pose transform *in the display pass*. That
  composition is non-normative and writes nothing. So displaying at any pose never moves an entry.
* The stored `C` therefore only needs advancing when a **coded tile** at that position is about to
  read it.

So the client keeps, per entry, the frame its `C` is composed up to — call it `advanced_to`,
initially `src_frame` — and advances lazily:

```
apply_tile(t, N):
    if atlas[t].src_frame >= N:        # 13.12.3 step 3 monotonicity
        drop; return
    while atlas[t].advanced_to < N:    # 13.12.2, one right-multiplication per step
        atlas[t].C = renorm(atlas[t].C · H[atlas[t].advanced_to + 1])
        atlas[t].gen += 1
        atlas[t].advanced_to += 1
    decode t against atlas[t].C        # 13.12.4
    write back: C := I, src_frame := N, gen := 0, advanced_to := N, valid := 1
```

This is bit-exact against the eager form because 13.12.2 builds the composition "one step at a time
by right-multiplication" and this performs the same steps in the same order. It costs the client a
**ring of recent `H_N`** — nine i32 per eye per frame, 36 bytes; a 64-frame window is 4.6 kB for the
pair — which it must keep anyway to display at a pose newer than the last frame.

The two rules then coincide where it matters: `advanced_to` only moves past `N` when some tile at
that position was decoded at a frame `> N`, which is exactly when `src_frame > N` and the late tile
is dropped. **A tile is never dropped for a reason the atlas cannot state.**

## 3. What the wire has to carry first — and does not

**This is a prerequisite, and it is not the decoder's.**

`server/encoder/nxwarp_packetize.h` is explicit that a transport "tile" is not a codec tile:

> the frame bitstream is cut into chunks of `chunk_bytes` and chunk i is placed at tile index i of
> the grid, in raster order. […] What is lost is per-tile independence: a chunk that never arrives
> costs the frame rather than one tile.

A client cannot hand *coded tiles* to a decoder as they arrive when what arrives is an arbitrary
byte range of the frame's bitstream. **Tile streaming is blocked on the wire carrying real per-tile
byte spans**, and the header comment already names the fix and says it is available:

> When the Vulkan encoder lands behind nxwarp_codec it produces segments that are already
> datagram-sized and knows where every tile begins, and the mapping becomes the identity it was
> always meant to be.

It has landed, and the information is already computed and then dropped on the floor:
`nxvc_vke_tile` carries `offset` **and** `length`, and `wivrn::nxwarp_tile_desc` copies `index`,
`qp`, `mode`, `res_level` and `ref_delta` — but not `offset`. So the server-side prerequisite is
small and additive:

* **P1a.** ~~`nxwarp_tile_desc` gains `offset`~~ **done.** It carries `offset` and `length`, the
  vk backend fills both from `nxvc_vke_tile`, and `nxwarp_codec::reports_tile_spans()` is how a
  backend says whether it can. The reference backend cannot — `nxvc_tile_info` has no offset — so
  it answers false and keeps the chunk mapping.
* **P1b.** ~~`nxwarp_send_frame` places a tile's real byte span at its own tile index~~ **done,
  per frame.** Spans are used when the codec reports them *and* `nxwarp_spans_fit` says every
  coded tile fits a transport slot, since a tile bigger than a slot cannot be carried this way and
  the frame falls back whole rather than half. The client's `nxwarp_reassemble` changed with it:
  its per-tile hole and short-chunk tests were the chunk mapping's way of noticing a loss and are
  *wrong* under spans, where a skipped tile carries nothing and a coded tile is as long as its
  content. The length prefix is the test that is exact under both, so nothing on the wire says
  which mapping produced a frame.

  Measured, 20 frames at 960x544 QP 32 with one datagram and its group's parity taken off the
  link: 2700 coded tiles, 2700 transport slots, one each; the dropped datagram carried 8 of that
  frame's 135 tiles, and the headset's receipt map reports those 8 not held and **no others**.
  Under the chunk mapping the same run offers 262 slots for the same 20 frames and the one lost
  slot is a thirteenth of a frame's bytes at a tile index that names no codec tile.
  `docs/NXWARP-E2E.md` has the runbook and the table.

Both halves of P1 have landed. Tile streaming can now be *modelled* against a real encoder
(section 6, `--tile-stream`); the client itself still waits on section 4.

## 4. What the nxvc decoder API needs — OPEN, for the decoder agent

`nxvc_vk_decode_frame(dec, bytes, len, consumed)` takes a whole frame. There is no entry point that
takes a subset of tiles, and `atlas-decoder` has not added one. This is the question to settle
before the client is written, because the client's shape follows from the answer.

Two shapes, with what each costs the client:

**Option A — a tile-run decode call.**

```c
nxvc_vkd_status nxvc_vk_decode_tiles(nxvc_vk_decoder *dec,
                                     uint64_t frame_number,
                                     const uint8_t *frame_header, size_t header_len,
                                     const uint8_t *bytes, size_t len,
                                     uint32_t first_tile, uint32_t tile_count,
                                     uint32_t submit_flags);
```

The client calls it once per arriving run. It needs the frame's header (the pose and `warp_ext`)
before any tile of that frame, which the transport already delivers on the frame's first datagram.
*Client cost:* it must hold the frame header until the frame's last tile, and must handle a run
arriving before the header (reordering) by parking the run.
*Decoder cost:* a decode entry that does not assume it owns the whole frame — per-call Pass A over
the run's tiles, Pass B over the same set, atlas write-back for those positions only.

**Option B — whole-frame call with a tile mask.**

```c
nxvc_vkd_status nxvc_vk_decode_frame_masked(nxvc_vk_decoder *dec,
                                            const uint8_t *bytes, size_t len,
                                            const uint32_t *tile_mask, size_t mask_words,
                                            size_t *consumed);
```

*Client cost:* it must still assemble the whole frame buffer, which is most of what Cheat 1 exists
to avoid — the latency saved is only the decode's, not the reassembly's. It does allow a frame with
a hole to decode the tiles that arrived, which is worth having on its own.
*Decoder cost:* smaller; the existing call with a mask applied to the tile walk.

**The client wants A.** B is a strictly weaker version of the same thing and leaves the reassembly
wait in place. But A is the larger change on the decoder side and the decoder agent owns that
judgement, so this is a question and not a decision. **Two things are needed either way**, and they
are worth stating separately because they are independent of A-vs-B:

1. **The atlas must be addressable per tile position** — the decode call must be able to write back
   `C`, `src_frame`, `gen`, `flags`, `res_level` for the positions it touched and nothing else.
2. **The lazy advance of section 2 must be expressible.** Either the decoder performs it (it is
   given `frame_number` and the `H` ring and advances the entries it is about to read), or it
   exposes the atlas table and the client advances before calling. The first keeps 13.12.2's integer
   composition in one implementation, which is where it belongs.

## 5. Receipts become per tile run

Section 7 of the ADR says the unit of the positive acknowledgement becomes the tile, "which is what
the receipt map already is". Concretely, on this client:

| today | under tile streaming |
|---|---|
| `nxwarp_frame_not_held{frame_id, reason}` — one report per frame | one report per **tile run**: `{frame_id, first_tile, count, reason}` |
| `held_base`/`held_mask` confirm frames the codec took | confirm **runs**: the same pair, over tile indices within a frame |
| reasons: `hole`, `stride`, `backlog`, `codec` | `hole` and `codec` survive; **`stride` and `backlog` cease to exist** |

`stride` and `backlog` disappear because they are properties of a queue of frames, and there is no
such queue: a tile is applied on arrival. That is a real simplification of the not-held path rather
than a renaming, and it removes the two reasons that dominate the live loop today.

One reason is **added**: `superseded` — a tile dropped by the `src_frame` monotonicity rule of
section 2. It is not a loss and the encoder must not answer it with a refresh: the position already
holds a *newer* generation than the tile that was dropped. It is reported because the encoder's
shadow needs to know the tile it sent was not applied, or it will believe the client holds
generation `N` at that position when the client holds `N+1`. Both are "not the one I sent", and only
the report distinguishes them.

## 6. How the harness models it

`wivrn-nxwarp-e2e` already delivers datagrams through a lossy, reordering link and already knows
each frame's tile directory. What it needs:

* **Arrival-order delivery.** Today the harness's client accumulates a frame and decodes it whole.
  Under the flag it hands each arriving run to the atlas model in **arrival order**, not index
  order, which is what exercises the section 2 rule at all.
* **Out-of-order rows.** `--reorder` already crosses frame boundaries at the datagram level; the
  new assertion is that a run of frame `N` arriving after runs of frame `N+1` is applied or dropped
  by `src_frame` and never by which arrived first.
* **The generation rule, asserted.** For every dropped run the harness must be able to say which
  rule dropped it, and the count of `superseded` must equal the count of positions where a later
  frame coded the same tile before the earlier one arrived. That number is computable from the
  link's own delivery order, so it is an assertion and not a report.
* **A shadow atlas.** The harness keeps the encoder-side atlas (which the encoder must keep anyway)
  and compares it to the client's after every frame. **This is the byte-identity proof**: the atlas
  is the normative output (13.12), so "the same input produces the same atlas" is the whole
  correctness claim, and it is stronger than comparing pictures because the picture is explicitly
  not normative.

The proof obligation for the flag is therefore precise: **for the same input and the same link
behaviour, the atlas after the last frame is byte-identical between the frame-complete path and the
tile-streaming path.** Where the link is clean the two must agree trivially; where it loses, they
must agree because the *set of tiles applied* is the same, only the order differs.

**Landed**, as `--tile-stream`, over the tile runs the link actually delivered. `C` is modelled as
the ordered sequence of `H` steps folded into it, which is exactly the property 13.12.2's
one-step-at-a-time right-multiplication has and the only one the comparison needs: two paths agree
on `C` if and only if they took the same steps in the same order. Both paths are flushed to the
last frame before the comparison — advancing forward is always legal, it is un-advancing that
section 2 shows is not bit-exact, and neither path ever does it.

One thing the design did not anticipate: the harness's `--reorder` held a datagram back by one to
three slots, which reorders *within* a frame and therefore never reaches the rule at all. A tile of
frame `N` has to arrive after a tile of frame `N+1` **at the same position**, and at six or seven
datagrams a frame that means falling a whole frame behind. `--reorder-depth` is that knob; at 8 the
rule fires on 36 tiles of 800 and the two atlases still agree, which is the assertion doing work
rather than passing vacuously. `docs/NXWARP-E2E.md` has the table.

## 7. What pacing means when there are no frame-sized decodes

Send pacing today answers "how often may I send a frame, given the headset decodes one in `d` ms".
Under `ATLAS` there is no frame-sized decode and the question changes shape:

* **The pace's unit becomes coded tiles per second, not frames per second.** The client's cost is
  per coded tile; a frame with 8 coded tiles and a frame with 300 are not the same load and the
  current controller cannot tell them apart. The headset already reports what is needed — its
  decode cost — but it must report it per tile rather than per frame.
* **The bounded queue's job disappears** and with it the reason the pace exists in its current form.
  Its remaining job is honest: do not put more coded tiles per second on the wire than the headset's
  GPU can absorb, which is a *throughput* limit, not a latency one.
* **And latency is no longer the pace's business at all.** The measurement in `docs/nxwarp.md`
  already showed that pacing holds none of the motion-to-photon budget (~0 ms of 46-53). Under the
  atlas that becomes structural rather than incidental: display never waits for the network, so
  nothing the pace does can add display latency. It becomes purely a throughput governor.

That argues the pace should be **rewritten in terms of coded tiles once the atlas lands, and not
before** — its current form is correct for the current model, and changing both at once would leave
no working configuration to compare against.

## 8. Order of work

1. ~~**P1** (server): per-tile spans on the wire.~~ **Done.** See section 3.
2. **The decoder API** (section 4): A or B — decided as **A**, `nxvc_vk_decode_tiles(first_tile,
   count)` with the lazy advance inside the decoder; being built on nx-warp's `atlas-decoder`.
3. ~~**The harness model** (section 6), behind `--tile-stream`~~ **Done**, with the atlas
   comparison. What is deferred with the API is only the comparison against the *real* decoder's
   atlas; the order-independence claim itself is asserted today, and `--reorder-depth` is what
   makes the monotonicity rule fire at all.
4. **The client** (sections 2 and 5), behind a flag, frame-complete remaining the default. Still
   blocked on 2, and it is the only thing that is.

The receipts of section 5 remain to be moved to tile runs on the wire: the harness checks the
positive acknowledgement per tile today (it reads `nxt::ClientShadow`, which has always been per
tile), but `from_headset::nxwarp_frame_not_held` is still one report per frame, and `superseded`
is not yet a reason it can carry. That is part of 4, since nothing can report a superseded tile
until something applies tiles out of order.
