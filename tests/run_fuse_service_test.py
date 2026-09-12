#!/usr/bin/env python3
"""Build/run the Qt6 bridge test against an existing local NX Fuse simulator.

Temporarily changes simulation controls; resets all three to false even on
test failure. Does not launch/stop workers or open cameras. Requires Qt6
development packages, C++20 compiler, pkg-config and Qt6 moc.
"""
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import urllib.request


ROOT = Path(__file__).resolve().parents[1]
URL = "http://127.0.0.1:8787"
HTTP = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def state():
    with HTTP.open(URL + "/api/state", timeout=3) as response:
        result = json.load(response)
    if result.get("mode") != "simulation" or not isinstance(result.get("joints"), dict):
        raise RuntimeError("Port 8787 is not an NX Fuse simulator")
    return result


def reset():
    # Verify endpoint again before sending simulator-only controls.
    state()
    payload = json.dumps(dict(enabled=False, camera_only=False, occluded=False)).encode()
    request = urllib.request.Request(URL + "/api/control", data=payload,
                                     headers={"Content-Type": "application/json"})
    with HTTP.open(request, timeout=3) as response:
        if json.load(response) != {"ok": True}:
            raise RuntimeError("Failed to reset simulation controls")


def main():
    state()
    moc = shutil.which("moc6") or "/usr/lib/qt6/moc"
    if " 6." not in subprocess.check_output([moc, "-v"], text=True, stderr=subprocess.STDOUT):
        raise RuntimeError("Qt6 moc required")
    flags = shlex.split(subprocess.check_output(
        ["pkg-config", "--cflags", "--libs", "Qt6Core", "Qt6Network", "Qt6Qml"], text=True))
    with tempfile.TemporaryDirectory(prefix="nx-fuse-qt-test-") as directory:
        temp = Path(directory)
        dashboard = ROOT / "dashboard"
        moc_flags = [flag for flag in flags if flag.startswith(("-I", "-D"))]
        subprocess.run([moc, *moc_flags, str(dashboard / "fuse_service.h"), "-o", str(temp / "moc.cpp")], check=True)
        subprocess.run([os.environ.get("CXX", "c++"), "-std=c++20", "-fPIC", "-I" + str(dashboard),
                        str(ROOT / "tests/fuse_service_test.cpp"), str(temp / "moc.cpp"),
                        str(dashboard / "fuse_service.cpp"), *flags, "-o", str(temp / "check")], check=True)
        try:
            subprocess.run([str(temp / "check")], check=True, timeout=35)
        finally:
            reset()
    restored = state()
    if any(restored[key] for key in ("enabled", "camera_only", "occluded")):
        raise RuntimeError("Simulation controls were not restored")
    print("PASS: external simulator still available; all controls false")


if __name__ == "__main__":
    main()
