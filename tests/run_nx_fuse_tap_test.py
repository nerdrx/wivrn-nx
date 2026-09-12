#!/usr/bin/env python3
import json
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build-server"


def compile_fixture(output):
    commands = json.loads((BUILD / "compile_commands.json").read_text())
    command = next(item["command"] for item in commands if item["file"].endswith("server/driver/nx_fuse_tap.cpp"))
    flags = shlex.split(command)
    for flag in ("-c",):
        flags.remove(flag)
    for prefix in ("-o", "-MF", "-MT"):
        while prefix in flags:
            index = flags.index(prefix)
            del flags[index:index + 2]
    flags = [flag for flag in flags if not flag.startswith("-fmodule-mapper=") and flag != "-fdeps-format=p1689r5" and not flag.endswith("server/driver/nx_fuse_tap.cpp")]
    subprocess.run(flags + ["-ffunction-sections", "-fdata-sections", str(ROOT / "tests/nx_fuse_tap_test.cpp"), str(ROOT / "server/driver/nx_fuse_tap.cpp"), "-Wl,--gc-sections", "-x", "none", str(BUILD / "common/libwivrn-common.a"), "-lcrypto", "-o", str(output)], cwd=BUILD, check=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--nx-fuse-root", type=Path, default=os.environ.get("NX_FUSE_ROOT"), help="path containing tracking.py")
    args = parser.parse_args()
    if args.nx_fuse_root is None:
        parser.error("provide --nx-fuse-root or set NX_FUSE_ROOT to the nx-fuse checkout")
    decoder_root = Path(args.nx_fuse_root).expanduser().resolve()
    if not (decoder_root / "tracking.py").is_file():
        parser.error(f"NX_FUSE_ROOT must contain tracking.py: {decoder_root}")
    sys.path.insert(0, str(decoder_root))
    import tracking

    with tempfile.TemporaryDirectory() as directory:
        binary = Path(directory) / "nx-fuse-tap-test"
        socket_path = Path(directory) / "tap.sock"
        compile_fixture(binary)
        result = subprocess.run([str(binary), str(socket_path)], cwd=ROOT, check=True, capture_output=True, text=True)
        packets = [bytes.fromhex(line) for line in result.stdout.splitlines() if line]
        assert len(packets) == 4, len(packets)
        decoded = [tracking.decode_packet(packet) for packet in packets]
        assert decoded[0]["route"] == "anchors"
        assert decoded[0]["sequence"] == 1
        assert decoded[0]["records"][0]["joint"] == "head"
        assert decoded[1]["route"] == "BD"
        assert decoded[1]["records"][0]["joint"] == "hip"
        assert decoded[1]["records"][0]["position"][0] == 3
        assert decoded[2]["route"] == "HTC"
        assert decoded[2]["records"][0]["joint"] == "generic_0"
        assert decoded[3]["generation"] != decoded[0]["generation"]
        assert decoded[3]["sequence"] == 4
    print("nx_fuse_tap cross-language wire test: OK")


if __name__ == "__main__":
    main()
