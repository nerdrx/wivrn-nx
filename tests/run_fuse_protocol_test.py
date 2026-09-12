#!/usr/bin/env python3
"""Run FuseService protocol regressions against an isolated fake HTTP service."""
import http.server
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import threading
import urllib.parse
import urllib.request


ROOT = Path(__file__).resolve().parents[1]


class FakeFuse(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self):
        super().__init__(("127.0.0.1", 0), Handler)
        self.scenario = "valid"
        self.lock = threading.Lock()


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args):
        pass

    def send_json(self, value):
        payload = json.dumps(value, separators=(",", ":")).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def do_GET(self):  # noqa: N802
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path == "/__scenario":
            scenario = urllib.parse.parse_qs(parsed.query).get("name", [""])[0]
            with self.server.lock:
                self.server.scenario = scenario
            self.send_json({"ok": True})
            return
        if parsed.path == "/api/cameras":
            self.send_json({"devices": []})
            return
        if parsed.path != "/api/state":
            self.send_error(404)
            return
        with self.server.lock:
            scenario = self.server.scenario
        if scenario == "stalled":
            threading.Event().wait(3.0)
            return
        if scenario == "malformed":
            payload = b"{not-json"
            self.send_response(200)
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
            return
        if scenario == "wrong-mode":
            self.send_json({"mode": "not-simulation", "joints": {}})
            return
        if scenario == "oversize":
            payload = b"x" * (128 * 1024 + 2)
            self.send_response(200)
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
            return
        self.send_json({"mode": "simulation", "joints": {}})


def main():
    moc = shutil.which("moc6") or "/usr/lib/qt6/moc"
    if not Path(moc).exists():
        raise RuntimeError("Qt6 moc required")
    flags = shlex.split(subprocess.check_output(
        ["pkg-config", "--cflags", "--libs", "Qt6Core", "Qt6Network", "Qt6Qml"], text=True))
    with tempfile.TemporaryDirectory(prefix="nx-fuse-protocol-test-") as directory:
        temp = Path(directory)
        fake = FakeFuse()
        thread = threading.Thread(target=fake.serve_forever, daemon=True)
        thread.start()
        try:
            moc_flags = [flag for flag in flags if flag.startswith(("-I", "-D"))]
            subprocess.run([moc, *moc_flags, str(ROOT / "dashboard/fuse_service.h"), "-o", str(temp / "moc.cpp")], check=True)
            subprocess.run([os.environ.get("CXX", "c++"), "-std=c++20", "-fPIC", "-I" + str(ROOT / "dashboard"),
                            str(ROOT / "tests/fuse_protocol_test.cpp"), str(temp / "moc.cpp"),
                            str(ROOT / "dashboard/fuse_service.cpp"), *flags, "-o", str(temp / "check")], check=True)
            environment = os.environ.copy()
            environment["NX_FUSE_PORT"] = str(fake.server_port)
            subprocess.run([str(temp / "check")], check=True, timeout=45, env=environment)
        finally:
            fake.shutdown()
            fake.server_close()
            thread.join(timeout=2)
    print("PASS: malformed, wrong-mode, oversize, stalled, and reconnect recovery")


if __name__ == "__main__":
    main()
