# WiVRn NX server on Windows → SteamVR (OpenVR): feasibility study

Read-only assessment. Repo: `/run/media/nerdrx/Lex/claude/wivrn-nx`. Scope: PC-side **server** only
(the Android client is already cross-platform and out of scope).

**One-line answer up front:** this is *effectively a new project* — a from-scratch SteamVR/OpenVR
driver that reuses WiVRn's transport/encoder/client but throws away the entire Monado integration
layer. It largely reconstructs ALVR. Order of magnitude: **6–12+ person-months.** The single biggest
blocker is that the presentation model is **inverted** (WiVRn is an OpenXR *runtime that composites
frames itself*; SteamVR wants an OpenVR *driver that is handed already-composited frames*).

---

## 1. Monado coupling depth

**The WiVRn server binary *is* a Monado build.** It is not "a program that talks to Monado" — it
FetchContent-pulls Monado from freedesktop gitlab, applies local patches, and links Monado's internal
libraries directly:

- `CMakeLists.txt:272-277` — `patches/monado/*` applied to a pinned Monado
  (`monado-rev` = `5dbe934da3395df7f543f653a43fc325219705fa`), fetched from
  `gitlab.freedesktop.org/monado/monado.git`.
- `server/CMakeLists.txt:248-263` — `wivrn-server` links `aux_os aux_render aux_util aux_vk
  comp_multi comp_util ipc_server xrt-external-openxr xrt-interfaces`.
- Embedded Monado source: **~505,000 LOC** of C/C++ (`build-server/_deps/monado-src/src`).
- WiVRn's own code: **~31,846 LOC** under `server/`, **~10,866 LOC** under `common/` (protocol +
  sockets + crypto).

**How the streamed view is presented as an HMD today.** WiVRn provides *only an OpenXR runtime*
(`docs/steamvr.md:3`: "WiVRn only provides an OpenXR runtime"). The pieces:

- `server/target_instance_wivrn.cpp` — implements `xrt_instance` and the Monado entry point
  `xrt_instance_create` (line 99). `create_system()` (line 43) builds a `wivrn_session` which *is* an
  `xrt_system_devices`, plus the `xrt_system_compositor`.
- `server/main.cpp:268` forks; the child (`server_pid == 0`) calls Monado's
  `ipc_server_main_common(...)` (line 301) — i.e. it *is* the Monado IPC service (the OpenXR
  equivalent of `vrserver`), hosting OpenXR client apps over an `AF_UNIX` socket.
- `server/compositor/compositor.h:54` — `class compositor : public comp_base`. It overrides the
  `xrt_compositor` vtable: `predict_frame`, `mark_frame`, `begin_frame`, `layer_commit`,
  `get_display_refresh_rate` (lines 254-278). It consumes Monado swapchains
  (`comp_swapchain_image`, `comp_layer`, `comp_frame` — `compositor.cpp:94-96`).
- Devices are all `xrt_device`: `wivrn_hmd : xrt_device` (`server/driver/wivrn_hmd.h:44`),
  `wivrn_controller`, eye/face/body trackers, gamepad. Poses flow as `xrt_space_relation`
  (`server/driver/pose_list.h:26`).

**How OpenVR/SteamVR games reach it today (Linux).** SteamVR's `vrserver` is *not used at all*.
OpenVR games are translated *down* onto WiVRn's OpenXR runtime by an external shim — **xrizer** or
**OpenComposite** (`docs/steamvr.md:3`, `README.md:233`). `server/active_runtime.cpp` rewrites
`openvrpaths.vrpath` to point OpenVR at that shim (lines 192-199) and sets the OpenXR
`active_runtime` manifest to WiVRn. So today's chain is:
`OpenVR game → xrizer/OpenComposite → OpenXR → WiVRn(Monado) → compositor → encoders → network`.

### Subsystem split: Monado/xrt-coupled vs. genuinely portable

| Monado / `xrt_*` coupled (needs reimplementation) | Portable (survives a port) |
|---|---|
| Compositor (`compositor.cpp/.h` = `comp_base`, swapchain intake, `layer_commit`) | `wivrn_packets` protocol (`common/`, ~10.8k LOC) |
| All device drivers (`wivrn_hmd`/`wivrn_controller`/trackers = `xrt_device`) | Networking (`common/wivrn_sockets.cpp`, BSD sockets) |
| IPC service (`ipc_server`, `AF_UNIX`, `ipc_server_main_common`) | Encoders (Vulkan-based; take a `vk::Image`) |
| Instance/system/target plumbing (`target_instance_wivrn`, `wivrn_session : xrt_system_devices`) | Crypto (`common/crypto.h`, OpenSSL EVP) |
| Spaces (`xrt_space_overseer`), frame pacing (`comp_frame`/`predict_frame`) | Bitrate control (`driver/bitrate_controller.h` — only needs `wivrn_packets`) |
| Pose plumbing (`xrt_space_relation`), foveation/motion/quad compute passes fed by Monado layers | Audio PLC (`common/audio_plc.h`), FEC (`common/fec.h`) |

Roughly: the compositor + drivers + IPC + instance/system layer — the bulk of what makes the server
work as a headset — is Monado abstraction. The transport, codecs, and crypto are not.

---

## 2. What "SteamVR instead" concretely requires

### WiVRn's frame flow today (it composites its own frames)

```
OpenXR game renders into swapchains
  → Monado comp_multi replays the client's layer stack
  → wivrn::compositor (comp_base) in layer_commit(): layer_squasher + foveation
      + optional motion-warp + quad promotion → produces one Y_CbCr vk::Image per eye
  → video_encoder::present_image(vk::Image y_cbcr, sem, frame_index)   [encoder/video_encoder.h:222]
  → encode() → shards → wivrn_session network send
```

Critically: **WiVRn does its own compositing.** `layer_commit` (`compositor.h:273`) receives the
game's individual OpenXR layers and flattens them itself (the squasher, foveation, quad-layer
promotion, motion smoothing all operate on that layer stack).

### What SteamVR's model provides instead

SteamVR loads a **driver DLL** exporting `HmdDriverFactory()`. The driver implements:
`vr::IServerTrackedDeviceProvider` → registers a `vr::ITrackedDeviceServerDriver` (the HMD) which
exposes `vr::IVRDisplayComponent` and, for streaming, `vr::IVRDriverDirectModeComponent`. **SteamVR's
own `vrcompositor` composites the game's submitted layers** (including its dashboard/overlays and its
own reprojection), then calls the driver's `Present()` / `PostPresent()`, handing it the **already
-composited** eye frame as a shared **DXGI/D3D11** texture (or Vulkan image). The driver encodes that
texture. Poses go the other way: the driver calls
`vr::IVRServerDriverHost::TrackedDevicePoseUpdated(...)` with a `DriverPose_t`.

### How ALVR actually does it (the reference architecture)

- `alvr_server_openvr` is compiled as a `cdylib` exporting `HmdDriverFactory()`; it's a **hybrid
  C++ (OpenVR via bindgen) + Rust (streaming core)** driver.
- **Windows frame capture**: `OvrDirectModeComponent` implements `IVRDriverDirectModeComponent`;
  SteamVR calls `Present()` with the composited texture, and ALVR feeds it straight into
  **NVENC/AMF/QuickSync** via D3D11. (On *Linux* ALVR can't use that interface, so it uses a Vulkan
  layer wrapping `vkQueuePresentKHR` instead — the inverse of WiVRn's situation.)
- ALVR does **not** composite; SteamVR does. ALVR just receives the final frame + injects poses.

### The consequence for WiVRn

Porting to SteamVR means **deleting WiVRn's compositor and feeding the encoders from SteamVR's
`Present()` texture instead.** That has two knock-on effects:

1. WiVRn's compositor value (squasher, foveation, quad-layer promotion, server-side motion warp) is
   built around having the *OpenXR layer stack*. SteamVR hands you a flattened frame — there is no
   layer stack to foveate-from-source or promote a quad out of. Those passes lose their input.
2. The encoders currently take a WiVRn-produced `vk::Image` in a Y_CbCr layout. They'd have to ingest
   a D3D11 texture (via D3D→Vulkan/CUDA interop, or a native D3D encoder path). The Vulkan-video and
   x264 encoders could be adapted; NVENC already speaks CUDA and could take the D3D texture more
   directly than it takes the current Vulkan-opaque-fd image.

So WiVRn's protocol + network + adaptive stack *could* sit behind an ALVR-shaped driver — but the
whole Monado front half is replaced, not adapted.

---

## 3. Platform portability of the non-Monado parts (the Linux-isms)

| Concern | File(s) | Verdict |
|---|---|---|
| **mDNS discovery (avahi)** | `server/avahi_publisher.*`, `CMakeLists.txt:146` | **Needs Windows equivalent** — Bonjour/`dnssd` or a native mDNS lib. Moderate. |
| **Audio (PipeWire)** | `server/audio/audio_pipewire.*`, `CMakeLists.txt:143` | **Needs rewrite of backend** (WASAPI). But the interesting part, PLC, is in `common/audio_plc.h` — portable, survives. |
| **D-Bus / systemd** | `main.cpp` (gdbus server), `server/start_systemd_unit.cpp`, `systemd_units_manager` | **Needs replacement** — no D-Bus/systemd on Windows; the dashboard control channel and unit-based app launch must move to a named pipe / local RPC. |
| **Internal IPC: `AF_UNIX` socketpair + `poll` + `SCM_RIGHTS` fd-passing** | `main.cpp:1248-1268`, `wivrn_ipc.cpp:37-125` (`send_path_to_monado`/`receive_path_from_main`) | **Hard, but the pattern is proven.** Windows has no `SCM_RIGHTS`. Monado *already* solved this for its **own** IPC (its design doc notes `SCM_RIGHTS` is unavailable on Windows and it uses `DuplicateHandle` + a named pipe instead — and that port is implemented, `ipc_server_mainloop` carries a Win32 `pipe_handle`). But WiVRn's NX **multipath handoff is a *separate* socketpair** that passes a live TCP socket fd from the main process to the monado child — it must get the same `WSADuplicateSocket`/`DuplicateHandle` treatment independently, or the two processes must collapse into one. NX addition. |
| **VAAPI encoder** | `server/encoder/ffmpeg/video_encoder_va.cpp` (`eDmaBufEXT`, `libdrm`, `CMakeLists.txt:133`) | **Hard-Linux, drop it.** dmabuf + libdrm are Linux-only. |
| **NVENC encoder** | `video_encoder_nvenc.cpp:368,394` (`eOpaqueFd`, `getMemoryFdKHR`) | **Portable with a change.** CUDA/NVENC are cross-platform, but the Vulkan interop uses the POSIX `OpaqueFd` handle + `getMemoryFdKHR`; on Windows this becomes `eOpaqueWin32` + `getMemoryWin32HandleKHR`. Small, localized change. |
| **Vulkan-video encoder** | `video_encoder_vulkan*.cpp` | **Most portable HW path** — no external-memory handles, encodes the `vk::Image` in-device. |
| **x264 software encoder** | `video_encoder_x264.cpp:202-284` | **Fully portable** — Vulkan buffer copy (`TransferDst`) → CPU read. |
| **`fork()` process model** | `main.cpp:268` (monado child), `start_application.cpp:112` (games), `setpgrp()` | **Blocker for the internal split; trivial for game launch.** Launching games → `CreateProcess`. But the main↔monado `fork` relies on the child *inheriting* the connection socket + the two socketpairs — no Windows analogue; must collapse to one process or duplicate handles explicitly. NX addition. |
| **Network sockets** | `common/wivrn_sockets.cpp` (`AF_INET6`, `netinet`, `poll()`) | **Winsock shim** — `WSAStartup`, `closesocket`, `WSAPoll`, no `MSG_NOSIGNAL`. Moderate, mechanical. |
| **Crypto** | `common/crypto.h` (OpenSSL EVP, x448) | **Portable.** |
| **Paths/config** | XDG dirs, `u_file_get_path_in_runtime_dir` (`main.cpp:99`) | **Needs Windows paths** (`%LOCALAPPDATA%`). Small. |

**Being blunt about the two NX-specific hard spots:** the `SCM_RIGHTS` fd-passing (the multipath
secondary-path handoff, `wivrn_ipc.cpp`) and the `fork()` two-process model (`main.cpp`) are the
architecturally load-bearing Linux-isms. They aren't a `#ifdef` away — they encode "a privileged
supervisor process owns the listener and hands live sockets to a compositor process." On Windows that
whole shape has to be redesigned (most naturally into a single process, which then interacts badly
with SteamVR's requirement that *your code runs inside `vrserver`*, not in a process you fork).

---

## 4. Realistic options, ranked

### (a) Build Monado on Windows, keep WiVRn ~as-is — *most code reuse, but wrong target*
Monado's Windows port is **more mature than "research project"**: it builds under VS2022 with deps via
**vcpkg**, has a **dedicated Windows CI job that builds and runs unit tests**, supports both in-process
and out-of-process service (`XRT_FEATURE_SERVICE=ON`, `monado-service.exe`), and its **IPC is already
ported** (named pipes + `DuplicateHandle` replacing `SCM_RIGHTS`). `hello_xr` and Unity OpenXR apps run
end-to-end. The `xrt_*` core, the `aux/os` threading/time layer, and handle-based buffer/sync passing
are all cross-platform.

A genuinely important nuance in WiVRn's favour: the standard Monado-on-Windows blocker — *"no headset
drivers yet, direct mode on Windows is more complicated"* — is about **driving a physical HMD** (USB
probing off udev/hidraw, and DXGI/direct scanout to a panel). **WiVRn needs neither.** Its compositor
renders to an **offscreen `vk::Image` that it hands to encoders** — it never scans out to a display
swapchain — and its "HMD" is a virtual `xrt_device` fed by the network, not a USB device. So the two
things that stop ordinary Monado-on-Windows do **not** stop WiVRn. That makes (a) *technically* the
least-rewrite path.

**But it still does not meet the goal.** Even fully working, this gives you
**Monado-as-an-OpenXR-runtime on Windows, not a SteamVR driver.** OpenVR/SteamVR-native games would
still not route through you; you'd still need xrizer/OpenComposite for OpenVR titles, and you'd be
*competing with SteamVR* for the OpenXR runtime slot rather than plugging into it. Separately, WiVRn's
server build has **zero** Windows support: `server/CMakeLists.txt` hard-requires (no platform gate)
`gdbus-codegen`, `avahi-*`, `glib/gio-unix`, `libnotify`, `libpipewire`, `libsystemd`, `libdrm` — so
you must still do all of §3 (mDNS, WASAPI audio, D-Bus/systemd replacement, Winsock, the NX multipath
handoff, paths) *plus* rebase onto Monado's Windows branch and carry WiVRn's Monado patches forward.
**Effort:** several months, **and the deliverable is not "present to SteamVR."** Reuse: almost
everything. Verdict: lowest-rewrite, **wrong target** — only choose it if the real goal is "WiVRn as a
Windows OpenXR runtime," not "WiVRn inside SteamVR."

### (b) Reimplement the server as a native OpenVR/SteamVR driver (the ALVR-shaped path)
Write a `HmdDriverFactory` DLL: `IServerTrackedDeviceProvider` + HMD/controller
`ITrackedDeviceServerDriver` + `IVRDisplayComponent` + `IVRDriverDirectModeComponent`. In `Present()`
take SteamVR's composited texture, hand it to WiVRn's encoders; feed poses back via
`TrackedDevicePoseUpdated`. **Reuse:** `common/` protocol, crypto, networking, the encoders (with the
D3D/Vulkan-interop rework), and the whole adaptive stack (bitrate/FEC/pacing) + the client, unchanged.
**Rewrite:** the entire Monado layer — compositor, every `xrt_device`, the IPC service, spaces,
target/instance — plus §3's Windows platform work. This is the **only option that actually presents to
SteamVR.**
**Effort:** ~6–12+ person-months for one experienced dev. Reuse ≈ the portable column of §1; rewrite ≈
the Monado column of §1 (its raison d'être is replaced, not translated).

### (c) Don't — use ALVR
Option (b) is, structurally, **rebuilding ALVR.** ALVR already is a Windows SteamVR streamer with
NVENC/AMF, adaptive bitrate, and its own headset client. WiVRn NX's genuinely unique value is the
**transport/adaptive stack** (multipath USB failover, Adaptive-v2/BBR bitrate, radio-aware stepping,
smooth pacing, FEC, audio PLC, encoder failover, motion smoothing) — and most of that lives in the
*portable* half and could in principle be grafted onto an ALVR-style driver. The honest framing: a
Windows-SteamVR WiVRn would duplicate ALVR's skeleton to carry WiVRn's transport muscles, and you'd
be maintaining a second SteamVR driver forever. If Windows+SteamVR is the requirement, contributing
WiVRn's transport ideas to ALVR is a far better ROI than porting.

---

## 5. Would the NX features survive the port?

| NX feature | Where it lives | Survives? |
|---|---|---|
| **Multipath USB failover + fd-passing** | Protocol `attach_path` (portable) **+** `SCM_RIGHTS`/`fork` handoff in `wivrn_ipc.cpp`, `main.cpp` (Linux) | **Split.** Connection logic + protocol survive; the fd-handoff mechanism must be reimplemented (`WSADuplicateSocket` / single-process). |
| **Adaptive bitrate / Adaptive-v2 (BBR-ish) / radio-aware / pacing** | `driver/bitrate_controller.h` (deps only `wivrn_packets`), encoder sender | **Survives wholesale** — portable; radio-aware just reads `from_headset::wifi_state`. |
| **FEC (parity shards)** | `common/fec.h` + encoder sender | **Survives.** |
| **Audio PLC** | `common/audio_plc.h` | **Survives** (the PipeWire backend does not). |
| **Encoder failover** | Logic in `compositor.cpp` (`fail_over_encoder`), watchdog in `encoder/` | **Mostly survives** — encoders + `encoder_watchdog` are portable; the swap logic must move into the new driver's frame loop. |
| **Motion smoothing (estimator/warper)** | `server/compositor/*` compute shaders over the composited image | **Needs reimplementation, and value overlaps.** It reprojects *from the OpenXR layer stack*; SteamVR gives a flat frame and does its own reprojection. Shaders reusable only if you keep a Vulkan stage. |
| **Quad layers** | `compositor/quad_converter.*`, driven by `comp_layer` | **Lost.** Requires the OpenXR layer stack; SteamVR hands you a pre-flattened frame with no separable quad. |
| **Standby-pose fix** | `driver/pose_list.cpp` (freezes last *tracked* pose) | **Logic survives**, re-expressed against OpenVR `DriverPose_t` at the driver boundary. |
| **Mirror** | `compositor/pipewire_mirror.cpp` | **Lost** (PipeWire); Windows would need a different capture. |

Pattern: everything in `common/` and the transport/encoder path survives; everything that lives in
the Monado compositor or touches the OpenXR layer stack (motion warp, quad layers, mirror) does not.

---

## Bottom line

**Effectively a new project — not a weekend hack, and more than "a few months of adapting."** Budget
**6–12+ person-months** for a working native SteamVR driver, and accept that it substantially
recreates ALVR while grafting on WiVRn's transport stack.

**Single biggest blocker:** the entire presentation model is inverted. WiVRn is an OpenXR *runtime*
that **composites its own frames** via an embedded Monado (`compositor : comp_base`, ~half of the
31k-LOC server plus the 505k-LOC Monado it links). SteamVR requires an OpenVR *driver* that is
**handed already-composited frames** by `vrcompositor` through `IVRDriverDirectModeComponent::Present`.
There is no thin adapter between those two shapes: the compositor, every `xrt_device`, the IPC
service, and the instance/system plumbing have no SteamVR analogue and must be rewritten. (Runner-up
blocker: the `SCM_RIGHTS`/`fork` multipath process model has no Windows equivalent.)

The "build Monado on Windows" path (option a) is the least code — and Monado's Windows build is now
CI-tested with a `DuplicateHandle`-based IPC, and WiVRn's offscreen compositor sidesteps the missing
Windows direct-mode entirely — but it **does not deliver SteamVR presentation.** It leaves you a
Monado-on-Windows OpenXR runtime competing with SteamVR and still needing xrizer/OpenComposite for
OpenVR games. It answers a different question than the one asked.

---

## Addendum: option (a′) — Monado on Windows + OpenComposite (replace, don't bridge)

A follow-up question: build Monado on Windows and bridge to SteamVR via a helper/translation
layer? Analysis:

**You cannot cheaply bridge *out of* SteamVR.** SteamVR exposes its composited frame only to an
OpenVR *driver* (direct mode); any helper that pulls frames from a running SteamVR must BE that
driver — i.e. option (b), the full rewrite. No lighter hook exists.

**But you don't need SteamVR.** The "translation layer" is OpenComposite/xrizer — reimplementations
of the OpenVR API that forward to an OpenXR runtime — which WiVRn already integrates
(`active_runtime.cpp`, `docs/steamvr.md`). So:

- Build Monado on Windows (viable — WiVRn's offscreen compositor sidesteps the missing Windows
  direct-mode). Port the WiVRn server's Linux deps (§3: avahi→Bonjour, PipeWire→WASAPI,
  D-Bus/systemd→local RPC, sockets→Winsock, fork/SCM_RIGHTS model).
- OpenVR games → OpenComposite/xrizer → Monado→WiVRn. **SteamVR is replaced, not bridged.**
- OpenXR games (incl. VRChat's native OpenXR path) → Monado directly, no shim.

**Advantage over option (b): ALL NX features survive** — the Monado compositor stays, so
quad-layer overlays, motion smoothing, and the mirror keep working; no inverted-model problem.

**Cost:** the §3 Windows platform-port only (months, but tractable) — no compositor/driver rewrite.

**Caveats:** OpenComposite/xrizer game-compat is not 100%; you lose SteamVR's compositor, overlays,
chaperone, and binding UI; games that hard-require vrserver won't launch.

**Verdict:** (a′) is the strongest path *if Windows is wanted* — far less work than (b), preserves
the NX stack, and for a VRChat-centric setup runs nearly clean via OpenXR. The tradeoff is that it
supplants SteamVR rather than integrating with it. Option (b) remains the only path that lives
*inside* SteamVR, at ~10x the effort.
