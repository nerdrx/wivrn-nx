# Reconnect decoder reset — 2026-09-08

## Change under test

A seamless reconnect now marks the client stream as needing a decoder reset. The
next `video_stream_description` therefore rebuilds the video decoders even when
the server sends metadata identical to the previous session. Ordinary duplicate
descriptions still return through the existing fast path.

Relevant implementation:

- `client/scenes/stream.h`: `needs_decoder_reset` state.
- `client/scenes/stream_network.cpp`: arms the state after the fresh handshake.
- `client/scenes/stream.cpp`: consumes the state under `decoder_mutex` and
  recreates decoders.

## Evidence

The same client process survived a server restart. Its log shows:

```
18:41:42.552  seamless reconnect: primary path lost
18:42:02.371  seamless reconnect: stream resumed
18:42:02.429  Creating decoders, size 2176x2176
```

This verifies that the post-reconnect, equal-description path recreated the
client decoder.

## Limits

No desktop build or automated reconnect test was run. WayVR interfered with the
mouse during the manual check, and the test was stopped before a decoded frame
could be observed after the reset. Fresh decoder creation is verified; resumed
visual delivery and fresh-frame feedback remain unverified.

The final combined client APK is identified by `apk.sha256`; it also includes
the full-INTRA reference-copy extension. It was installed without launching
the headset app or WayVR. Desktop input testing remains stopped.
