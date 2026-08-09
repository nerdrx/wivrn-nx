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

### Stage 2 — failover
- Primary declared down after N missed keepalives (~250 ms) or a send error; video + control
  producer output flips to the secondary; an IDR is forced (reuse the existing IDR machinery)
  so the decoder recovers instantly.
- Switch back only after the primary is healthy for a hysteresis window (~5 s), again with an
  IDR.
- `bitrate_controller` learns about switches: window flushed, per-path ceiling applied
  (`bitrate-auto.usb-max-bitrate`, default 100 Mbit/s) — the AIMD then adapts within the new
  path's budget. Feedback's whole-link utilisation metric is valid again because only one path
  carries video at a time.
- Clock-offset estimator reset on switch (its regression assumes one path's latency).

### Non-goals (for now)
- Striping video across both paths simultaneously. The reassembly window is two frames deep,
  so paths with >1 frame-period skew would lose frames; failover already captures most of the
  value. Revisit only with a deepened accumulator.

## Config

Client (headset settings, streaming page): `multipath_usb`, "USB backup connection", default on.

Dashboard: `usb_backup_tunnel`, "USB backup tunnel", default on — arms `adb reverse tcp:9757
tcp:9757` for every connected headset while a session is running.

Stage 2 adds a `multipath` key to the server config for `usb-max-bitrate`.

## Observing Stage 1

Everything Stage 1 does shows up in the logs. Server (`wivrn-server`):
`Listening for secondary paths on port 9757`, `Path attach: secondary path 1 handed to the
session`, `Secondary path 1 attached`, `Secondary path 1 alive, N packets received, last X ms
ago` (every 5 s), `Secondary path 1 detached: <reason>`. Headset: `Secondary path 1 attached
over 127.0.0.1:9757`, `Secondary path 1 RTT x.xx ms` (every 5 s), `Secondary path detached:
<reason>`. Dashboard: `USB backup tunnel armed for <serial>`.

## Testing without a headset

Linux client against a local server, two routes: loopback as "Wi-Fi", a veth/netns pair with
`tc netem` loss/latency as "USB". Kill or degrade one route mid-stream; success = session
survives, log shows path-down → flip → IDR → recovery, poses never stall.
