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

### Stage 0 — robustness prerequisites (valuable standalone)
- A malformed or short datagram is dropped and counted, never a session-kill
  (`stream_network.cpp` exit path, `wivrn_sockets.cpp` short-datagram throw).
- Per-path liveness replaces the single `active` bool; a send failure marks *that path* down.
- Video sender: per-path queues so a stalled TCP write cannot block UDP video.

### Stage 1 — path attachment
- Server issues a random `session_token` in `to_headset::handshake`.
- New `from_headset::attach_path{protocol_version, public_key, session_token}` on a fresh TCP
  connection: X448 + per-connection secrets as in `wivrn_connection::reset`, authenticated by
  known key + token. The main-process listener distinguishes attach from new-session and hands
  the fd + secrets to the monado process over the existing IPC socketpair (SCM_RIGHTS).
- Client: a path manager owned by the stream scene. While streaming, it probes
  `127.0.0.1:9757` periodically (reachable exactly when the USB tunnel is up), so plugging the
  cable mid-session brings the secondary path up automatically. Dashboard arms the adb reverse
  tunnel whenever a headset is present on USB during an active session.
- Keepalive/RTT probe pair on every path (~100 ms), giving per-path liveness + RTT.
- Tracking (and battery) duplicated onto the secondary; `history::add_sample` dedups by
  timestamp slot. Inputs, feedback and audio are NOT duplicated (input events must not repeat).
  Timesync stays pinned to the path carrying video; the estimator is reset on switch.

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

`multipath` key (server config) and a client toggle next to the connection settings:
enabled (default true when a second path is available), `usb-max-bitrate`.

## Testing without a headset

Linux client against a local server, two routes: loopback as "Wi-Fi", a veth/netns pair with
`tc netem` loss/latency as "USB". Kill or degrade one route mid-stream; success = session
survives, log shows path-down → flip → IDR → recovery, poses never stall.
