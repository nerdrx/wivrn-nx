# Multipath streaming (WiVRn NX)

Stream over two links at once — typically 5 GHz Wi-Fi plus USB-C (adb reverse tunnel) — so
that a hiccup or loss of either link does not interrupt the session.

## Model

- **Primary path**: today's connection — TCP control + UDP stream (or TCP-only), negotiated by
  the normal handshake.
- **Secondary path**: an additional TCP connection that *attaches* to the running session. It
  has its own crypto keys (fresh X448 exchange, like a reconnect), its own liveness state, and
  by default carries only keepalives and duplicated tracking. On primary failure, video and
  control flip to it.

Because the secondary is plain TCP, the USB tunnel (`adb reverse tcp:9757 tcp:9757`,
`wivrn+tcp://127.0.0.1:9757`) works unchanged as its transport, and none of the UDP
peer-pinning (kernel connect filter, single-address bind, handshake address check) applies.

## Stages

### Stage 0 — robustness prerequisites (valuable standalone) — **done**
- A malformed or short datagram is dropped and counted, never a session-kill
  (`stream_network.cpp` exit path, `wivrn_sockets.cpp` short-datagram throw).
- Per-path liveness replaces the single `active` bool; a send failure marks *that path* down.
- Video sender: per-path queues so a stalled TCP write cannot block UDP video.

### Stage 1 — path attachment — **done**
- Server issues a random `session_token` in `to_headset::handshake`, generated once per
  `wivrn_connection` (so it survives a reconnect and the main loop process keeps a valid copy
  after the fork).
- New `from_headset::attach_path{protocol_version, public_key, session_token, path_id}` as the
  first packet of a fresh TCP connection, answered by
  `to_headset::attach_path_response{public_key, state, path_id}`: fresh ephemeral X448 on the
  server side, authenticated by known (paired) key + token, no PIN/SMP. The main loop process
  keeps listening on the same port while a session runs; a connection whose first packet is not
  an attach is closed. The authenticated socket is handed to the monado process with SCM_RIGHTS,
  along with the derived key and IVs, over a *dedicated* AF_UNIX SOCK_DGRAM socketpair (the
  existing IPC socketpair is read with `recvmmsg` without ancillary data, which would drop the
  file descriptor).
- Client: a path manager (`client/secondary_path.{h,cpp}`) owned by the stream scene, on its own
  low-priority thread. While streaming, it probes `127.0.0.1:9757` every 5 s (reachable exactly
  when the USB tunnel is up), so plugging the cable mid-session brings the secondary path up
  automatically. The dashboard arms the adb reverse tunnel for every connected headset while a
  session is running (`Adb.sessionActive`, `Adb.usbTunnelEnabled`).
- Keepalive `from_headset::path_ping` → `to_headset::path_pong` every 250 ms on the secondary
  path, giving per-path liveness (dropped after 3 s of silence) + RTT logged client side.
- Tracking (and battery) duplicated onto the secondary; `history::add_sample` dedups by
  timestamp slot. Inputs, feedback and audio are NOT duplicated (input events must not repeat).
  Audio, when the low-latency audio path is on, rides `send_stream` in both directions, so it
  follows exactly the path selection video does — one path at a time, never both.
  Timesync stays pinned to the path carrying video; the estimator is reset on switch.

#### Stage 1 deviations from the original plan
- **Keepalive period is 250 ms, not 100 ms.** Stage 1 only logs RTT and liveness; 4/s is enough
  and cheaper. Stage 2 can raise it when failover depends on missed keepalives.
- **Keepalives are one-directional** (headset pings, server echoes). The server sends nothing
  else on a secondary path in this stage, so its liveness is simply "time since last packet".
- **Dedicated fd-passing socketpair** instead of the existing IPC one, see above.
- **Per-path key derivation uses `secrets::for_additional_path`.** `crypto::pbkdf2` passes the
  Diffie-Hellman result as `OSSL_KDF_PARAM_SECRET`, which PBKDF2 does not consume, so the
  regular `secrets` derivation depends on the PIN alone — for an already paired headset (PIN
  `000000`) that is a constant. A second TCP path would therefore have reused the primary's
  AES-CTR key stream, i.e. a two-time pad. `secrets::for_additional_path` folds the DH result
  into the PBKDF2 password so each path really gets its own key stream. The primary handshake
  is left untouched; that weakness is pre-existing and deserves its own fix.
- **The main loop process now keeps a listener open during a session.** It is closed as soon as
  monado reports `headset_disconnected` (it is about to listen on the same port for the
  reconnect), and `accept_connection` retries the bind until the port is free, which removes the
  race. A side effect is that a stray connection attempt during a session now gets accepted and
  closed instead of refused.
- **A USB-primary session also gets a secondary path** over the same tunnel (the probe cannot
  tell them apart). It is harmless, just useless; Stage 2 should skip it.

### Stage 2 — failover — **done**
- A single state machine, `wivrn::path_selector` (`common/path_selector.h`), runs on both ends:
  on the server it decides where video and control go, on the headset where tracking, input and
  feedback go. The two are independent — an asymmetric state (server already on the secondary,
  headset still on the primary) is valid and costs nothing.
- Keepalives now run on the primary too (`path_ping{path_id = 0}` → `path_pong`, same 250 ms
  cadence, protocol unchanged). The echo always goes back on the path the ping came from, so
  each end sees the other path even while it is not using it. Any packet received on a path
  counts as liveness for it, so while the primary carries the stream the keepalive is redundant;
  it matters exactly when the primary is otherwise idle, i.e. after a failover.
- Triggers to the secondary, from the moment they are true:
  - **silence**: no packet at all on the primary (control and stream together) for 400 ms;
  - **fatal send error** on the primary control socket (TCP), which used to throw and tear the
    session down and is now caught at the send boundary;
  - **persistent send errors** on the primary stream socket (UDP) for 500 ms.
  Any of them switches only if a usable secondary path exists; otherwise the old behaviour is
  kept (session pauses, headset reconnects).
- On a switch the server forces an IDR on every encoder (`compositor::request_idr`, the
  `idr_handler::reset` the feedback path already uses), resets the clock-offset estimator and
  applies the path ceiling to the bitrate controller.
- Flip back after 5 s of uninterrupted primary traffic — the headset's primary keepalive is what
  produces that traffic — again with an IDR, and the ceiling restored.
- `bitrate_controller` gained `set_path_ceiling(optional<uint32_t>)`: a ceiling clamped on top of
  the client's, which re-seeds the controller (window flushed, state back to steady) because
  measurements taken on the other path say nothing about this one.

#### Stage 2 deviations from the original plan
- **Failover is driven by silence, not by missed keepalives.** The stream socket receives at a
  high rate anyway, so "no packet of any kind for 400 ms" is both cheaper and stricter than
  counting keepalives, and it degrades gracefully if the keepalive is the only traffic left.
- **The selector lives in `common/`, shared by both ends.** The two sides needed the same logic;
  having one copy also makes it testable without the monado build (see below).
- **A receive never clears a run of stream send errors.** Being able to receive says nothing
  about being able to send. Only a successful send does — which is why the headset's primary
  keepalive goes over the UDP stream socket: once everything else has moved to the secondary, it
  is the only send left on the primary and therefore the only thing that can clear the run. A
  switch also clears it, since the socket is idle from then on.
- **The UDP socket is never recreated.** A connected UDP socket latches at most one error (ICMP
  unreachable while the access point is gone, `ENETUNREACH` during a Wi-Fi reassociation) and
  reports it to the next send, receive *or poll* — Stage 0/1 turned all three into a session
  teardown, which is what made a transient drop fatal. All three now drain `SO_ERROR` and carry
  on, which is enough: the kernel clears the latch and the socket works again the moment the
  link is back. Recreating it client-side is not an option, the server pinned the headset's
  source port with `connect()` during the handshake, so a new socket would need a new handshake.
- **A broken control socket is removed from the poll set.** A TCP socket with a pending error
  reports `POLLERR` on every `poll`, which would spin the network thread at 100 % once the
  session keeps running on the secondary path.
- **The send timeout on the secondary path is raised from 50 ms to 1 s while it carries video.**
  50 ms is right for keepalives and duplicated tracking, but a whole frame legitimately takes
  longer than that to hand to the kernel, and a send that times out half way through a packet
  loses the TCP framing and costs the path.
- **The server's secondary socket is now behind a `shared_mutex`**, like the headset's. In
  Stage 1 only the network thread ever sent on it; now the two encoder sender threads and the
  worker thread do too, so closing or replacing the socket had to stop racing with them. Sends
  and receives take it in shared mode, attach/drop exclusively. The framing is safe because
  `TCP::send_raw` already serialises whole packets on its own mutex.
- **Tracking is no longer duplicated onto the secondary while it is the active path** — it would
  simply be sent twice on the same socket.
- **USB-primary sessions are detected on the server**, by comparing the peer addresses of the
  secondary and the control socket (both are the loopback when the primary already goes through
  `adb reverse`). Such a path is still attached — the headset asked for it — but marked
  non-failover. The headset skips probing altogether when the server address it connected to is
  a loopback address. This closes the Stage 1 deviation about useless secondary paths.
- **Both paths down is still a teardown.** If the control socket is broken for good *and* the
  secondary is gone, the network thread throws and the session pauses and waits for a reconnect,
  as before.

### Non-goals (for now)
- ~~Striping video across both paths simultaneously.~~ Done in Stage 3, once the accumulator
  was deepened to tolerate inter-path skew (2 → 6 frames).

## Config

Client (headset settings, streaming page): `multipath_usb` (default on) plus `multipath_combine`
(default off), surfaced as one three-way *USB connection* selector — Off / Backup only /
Combine (experimental). `multipath_usb` off is the one state the server is never told about
(there is nothing to tell it over); the two on-states send `settings_changed.multipath` =
`backup` / `combine`.

Dashboard: `usb_backup_tunnel`, "USB backup tunnel", default on — arms `adb reverse tcp:9757
tcp:9757` for every connected headset while a session is running.

Server (`config.json`):

```json
"multipath": { "usb-max-bitrate": 100000000 }
```

The bitrate ceiling, in bits per second, applied while the secondary (USB) path carries video.
It is clamped against the ceiling the client asked for, so the lower of the two always wins.
Default 100 Mbit/s. There is no headset toggle of its own: the existing "USB backup connection"
switch governs the whole feature.

## Observing Stage 1

Everything Stage 1 does shows up in the logs. Server (`wivrn-server`):
`Listening for secondary paths on port 9757`, `Path attach: secondary path 1 handed to the
session`, `Secondary path 1 attached`, `Secondary path 1 alive, N packets received, last X ms
ago` (every 5 s), `Secondary path 1 detached: <reason>`. Headset: `Secondary path 1 attached
over 127.0.0.1:9757`, `Secondary path 1 RTT x.xx ms` (every 5 s), `Secondary path detached:
<reason>`. Dashboard: `USB backup tunnel armed for <serial>`.

## Observing Stage 2

Every switch is logged at info level on both ends, with the reason and the time since the
previous one. Server: `Path switch: video and control now on the secondary path (nothing
received on the primary path for 420 ms), 0 ms since the last switch`, followed by `Failover to
the secondary path: <reason>, IDR forced, clock estimator reset` and `Bitrate ceiling for the
active path: 100.0 Mbit/s (client asked for 150.0 Mbit/s)`. Headset: `Path switch: tracking and
input now on the secondary path (...)`. Transient trouble that did not (yet) cause a switch
shows up as `Send errors on the primary stream socket: N new, M total` and `Primary control
socket failed: <error>, failing over`.

## Testing the selector

`path_selector` has no dependency beyond the standard library, so it can be driven directly:

```
g++ -std=c++23 -I common -o failover_test failover_test.cpp && ./failover_test
```

The harness runs the state machine on a virtual clock (silence, control send error, datagram
error grace, hysteresis, secondary lost, reset) and then wires it to two socketpairs standing in
for the two paths, kills the primary, and checks that sends land on the secondary and come back
once the primary has been healthy for the whole window.

## Testing without a headset

Linux client against a local server, two routes: loopback as "Wi-Fi", a veth/netns pair with
`tc netem` loss/latency as "USB". Kill or degrade one route mid-stream; success = session
survives, log shows path-down → flip → IDR → recovery, poses never stall.

### Stage 3 — striping (bandwidth aggregation) — **done**

The selector gains a third posture, `combine`, entered from `primary` once both paths have
been healthy for the hysteresis window *and* the headset asked to combine *and* the secondary
is a genuinely different link (never a USB-primary session striping over two halves of one
tunnel). Wi-Fi stays the primary and is paced exactly as before; the shards past a per-frame
byte budget spill to the secondary (USB TCP), where they travel in parallel. Either path
degrading collapses `combine` back to the Stage 2 postures with a forced IDR.

- **Split point.** Each frame gets a wall-clock delivery window — the slice pacing gave it, or,
  unpaced/late, its share of a frame period. At the primary path's measured capacity `C`, that
  window carries `budget = C * window_ns / 8e9` bytes; everything past that byte offset spills.
  `C` is latched when `combine` is entered, from the delivered-bandwidth estimate (or
  `bitrate / pacing_window`), so it is a Wi-Fi-only measurement and does not chase the bitrate
  upward while combining. At the operating point (100 Mbit/s Wi-Fi, 0.4 window, 90 fps, three
  streams) the budget equals one stream's frame, so nothing spills until the controller raises
  the bitrate past what Wi-Fi was actually delivering — every added bit is what spills.
- **FEC.** Groups are built BEFORE the split, so parity is path-agnostic and a UDP-dropped
  shard can be rebuilt from copies that arrived over the tunnel. A group *every* shard of which
  spilled needs no parity (TCP drops nothing) and its parity shard is suppressed so it does not
  eat Wi-Fi budget; a group straddling the split keeps its parity. Parity always rides the
  primary path — the only one that can lose a packet.
- **Reassembly window** deepened 2 → 6 frames (`client/decoder/frame_window.h`,
  `shard_set.h`). A frame is no longer given up on because a newer index arrived — over two
  paths of different latency that is normal — but only when a *complete* frame more than ~3
  frame periods (skew tolerance) newer exists, or when it falls off the 6-deep end. Memory is
  bounded to 6 shard vectors per stream. Duplicate shards (the same one over both paths) are
  dropped, and a `submitted` cursor makes re-examining a frame after a newer one moved
  idempotent — a decoder is only ever fed the oldest frame, in index order, once.
- **Bitrate controller** (`set_combined(bool, usb_bps)`): while combining, the delivered-rate
  estimator already measures the aggregate — `8 * frame_bytes / (received_last - received_first)`
  spans both paths because the frame's bytes are the whole frame's and the span runs first-to-
  last arrival across both — so nothing is credited on top (that would double-count). The
  ceiling stays the client's; the failover USB clamp is taken *off* while combining (it caps a
  session running on the tunnel, not one spilling its tail onto it). Leaving `combine` re-seeds
  the controller and the caller reapplies the correct single-path ceiling in the same breath.
- **Headset control**: the *USB backup connection* toggle became a three-way *USB connection*
  selector — Off / Backup only / Combine (experimental). Off still means "never attach a path";
  the two on-states map to `multipath_mode::backup` / `::combine` (`settings_changed.multipath`,
  an `optional<multipath_mode>` like `bitrate_control`/`motion_smoothing_mode` — empty from an
  older client means backup, since striping needs a window it lacks). The server honours it live.
- **HUD**: `path_state` gained `combining`; the Link card shows *Wi-Fi + USB* with a live
  `xx% / yy%` byte split (cheap per-path counters differenced each status period,
  `transport_status.wifi_share_pct`).

#### Stage 3 deviations from the original plan
- **The aggregate ceiling is NOT `wifi-estimate + usb-max`.** The ceiling stays exactly the
  bitrate the headset asked for. Combining is a way to *reach* a number a single Wi-Fi link
  could not, not a licence to exceed the user's choice; the BBR estimator discovers the extra
  capacity on its own because it already measures both paths as one span, so raising the
  ceiling would only let it overshoot. `usb-max-bitrate` is recorded for the log line, not added.
- **`combine` is a server-side posture only.** The headset's uplink is tiny — there is nothing
  to aggregate in the headset→server direction — so its selector never calls `set_combine_allowed`
  and never leaves its Stage 2 postures. The two ends stay independent, as in Stage 2.
- **`combine` is only ever entered from a `primary` posture already in force**, never from one
  the same `update()` just arrived at, and never straight from `secondary`. The Wi-Fi share the
  split is sized against must be a measurement of the primary carrying the whole stream, which a
  path that took over a moment ago has not produced; the primary must re-prove itself (its own
  hysteresis) before it may combine.
- **The pacer is given the primary's *share*, not the whole frame.** Pacing the whole frame
  while only a prefix rides Wi-Fi would hand that prefix over at `1/(spilled fraction)` times
  the sized rate — the very burst pacing exists to prevent — so the shard_pacer is built over
  the split point.
- **A dedicated send timeout for the spill (250 ms).** The spill is written from the same
  encoder thread that drives the UDP socket, so a stalled tunnel would stall Wi-Fi video with
  it; 250 ms bounds the damage without the 1 s a full frame on a USB-*primary* session needs.
- **Interaction with the committed TCP self-poison invariant** (commit ddf164cc). A `wivrn::TCP`
  socket poisons itself on the first send failure — its AES-CTR keystream has advanced past
  bytes that never reached the wire — and throws on every later send/receive. The spill send
  (`wivrn_connection::send_spill`) therefore never retries the secondary: on a throw it drops
  the path there and then (closing the socket, which flips `secondary_usable` false and
  collapses the posture on the next `update()`), and the encoder reroutes that shard and the
  rest of the frame onto the primary. The per-frame `spill_scheduler::fail()` latch guarantees
  no further shard of the frame touches the poisoned socket. This is exercised by
  `striping_test.cpp` Part C.
