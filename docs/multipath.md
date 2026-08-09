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
- Striping video across both paths simultaneously. The reassembly window is two frames deep,
  so paths with >1 frame-period skew would lose frames; failover already captures most of the
  value. Revisit only with a deepened accumulator.

## Config

Client (headset settings, streaming page): `multipath_usb`, "USB backup connection", default on.

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
