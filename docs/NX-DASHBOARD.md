# Native NX dashboard and Fuse research console

The dashboard uses native Qt Quick/Kirigami controls and Canvas rendering.
It does not embed a browser or WebView. The shell has Overview, Streaming,
Body Tracking and Diagnostics destinations, with existing headset, mirror,
installer and setup pages retained. Alt+1 through Alt+4 select the main pages.

The new app icon uses an original W/visor inside an NX violet crystal.
The master, small and tray variants share the lowered visor alignment.
Dashboard colors follow the NX Hub design tokens; the system palette option
remains available in Streaming. Fuse's research workspace currently uses its
own fixed dark palette.

## Build and launch

Build the dashboard using the project's configured desktop build:

```sh
cmake --build build-server --target wivrn-dashboard -j 6
NX_FUSE_WORKER=/absolute/path/to/nx-fuse/app.py tools/launch-nx-preview.sh
```

The preview launcher sets `NX_DASHBOARD_PREVIEW=1`. This disables VR server
startup and attachment and isolates dashboard preferences under a preview
application name. It leaves your existing VR server alone. Fuse simulation
remains usable. `NX_DASHBOARD_BINARY` can point to a different dashboard build.

For normal server operation, run the built dashboard without the preview
environment variable. Existing pairing, connection and settings behavior
remains on the original WiVRn path. This change does not install or replace
the production dashboard automatically.

## Native Fuse page

Opening Body Tracking checks the local simulator at `127.0.0.1:8787`.
If it is not running, Start local simulator launches `python3` with the
absolute `NX_FUSE_WORKER` file, preferring its adjacent `.venv/bin/python3`
when available. This is a development setup; packaged worker
discovery is future work. No shell command is assembled from UI inputs.

The bridge checks the response is a simulation, polls asynchronously, caps
response size, and times out stalled requests. Controls are serialized. A
worker started by this dashboard is owned by it and stopped on exit. An
existing external worker can be disconnected without being terminated.

The page includes assisted and camera-only modes, simulated occlusion, body
comparison, discovered video nodes, and input debugging. Ctrl+D opens a
separate native debug window while Body Tracking is active. The fusion comparison remains synthetic.
The camera debug view separately provides explicit per-device capture, an
optional MediaPipe overlay, and front/side views of hip-relative inferred 3D.
Use NX Fuse’s `setup-camera.sh --download-model` once and set `NX_FUSE_MODEL`
to the downloaded task file before launching the worker. Nothing downloads
or starts capturing when the dashboard opens. Multiple capture devices can
run together; selecting a different preview does not stop the previous one.
Stop each camera explicitly, or stop the worker that owns capture.

Model coordinates are inferred, not measured depth or calibrated VR poses.
Wearer association, camera-to-VR calibration, historical time alignment and
live VR pose injection remain outstanding. A separate offline calibration
helper checks matched 3D samples; it is not a camera calibration wizard.
`NX_FUSE_PORT` can select another local development port; it defaults to 8787.

## Validation

```sh
# Requires an existing compatible external simulator on port 8787:
python3 tests/run_fuse_service_test.py
# Uses its own ephemeral local fake server; leaves running workers alone:
python3 tests/run_fuse_protocol_test.py
```

The Qt6 test compiles the bridge and exercises asynchronous attachment,
all three controls, coalesced updates, disconnect, and external-worker
survival. It restores simulation controls to false. No cameras or VR devices
are opened. The native dashboard is also checked inside headless gamescope
for main-page navigation, connected body rendering and detached debug UI.

No live streaming or hardware performance result is implied by these checks.
