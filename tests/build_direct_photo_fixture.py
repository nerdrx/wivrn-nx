#!/usr/bin/env python3
"""Build the standalone GPU photo fixture from an existing WiVRn build tree."""
from __future__ import annotations

import argparse
import json
import shlex
import subprocess
from pathlib import Path


def replace_output(args: list[str], output: Path) -> list[str]:
    result = list(args)
    try:
        i = result.index("-o")
    except ValueError as exc:
        raise RuntimeError("link/compile command has no -o") from exc
    result[i + 1] = str(output)
    return result


def compile_fixture(repo: Path, build: Path, source: Path, obj: Path) -> None:
    commands = build / "compile_commands.json"
    rows = json.loads(commands.read_text())
    row = next(
        (r for r in rows if str(r["file"]).endswith("/server/encoder/nxwarp_codec_direct.cpp")),
        None,
    )
    if row is None:
        raise RuntimeError(f"no nxwarp_codec_direct.cpp command in {commands}")
    args = list(row["arguments"]) if row.get("arguments") else shlex.split(row["command"])
    original = str(row["file"])
    directory = Path(row.get("directory", build)).resolve()
    original_path = (Path(original) if Path(original).is_absolute() else directory / original).resolve()
    source_index = next(
        (i for i, arg in enumerate(args)
         if arg == original or (not arg.startswith("-") and (directory / arg).resolve() == original_path)),
        None,
    )
    if source_index is None:
        raise RuntimeError("compile command does not contain its source argument")
    args[source_index] = str(source)
    cleaned: list[str] = []
    skip_next = False
    dependency_flags = {"-MF", "-MT", "-MQ", "-MJ"}
    for arg in args:
        if skip_next:
            skip_next = False
            continue
        if arg in {"-DNDEBUG", "-MD", "-MMD", "-MP"}:
            continue
        if arg in dependency_flags:
            skip_next = True
            continue
        if any(arg.startswith(flag) for flag in dependency_flags):
            continue
        cleaned.append(arg)
    args = cleaned
    args[1:1] = ["-DNX_DIRECT_TEST_WIDTH=2176", "-DNX_DIRECT_PHOTO_FIXTURE"]
    args.extend([f"-I{repo / 'server' / 'encoder'}"])
    subprocess.run(replace_output(args, obj), cwd=directory, check=True)


def link_fixture(build: Path, obj: Path, output: Path) -> None:
    link = build / "server/CMakeFiles/wivrn-server.dir/link.txt"
    args = shlex.split(link.read_text())
    args = [a for a in args if a != "-DNDEBUG" and "--dependency-file=" not in a]
    try:
        cut = args.index("-o")
        args[cut + 1] = str(output)
    except ValueError as exc:
        raise RuntimeError(f"no output flag in {link}") from exc
    # Link every production object except main; the fixture supplies its own main.
    args = [a for a in args if not a.endswith("main.cpp.o")]
    if str(obj) not in args:
        cut = args.index("-o")
        args.insert(cut, str(obj))
    subprocess.run(args, cwd=build / "server", check=True)


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Build a standalone Vulkan direct-blocks photo fixture. Runtime input must be "
            "raw 2160x2160 RGBA8; both eyes receive the same CPU NV12 approximation, while "
            "the native centre uses the source RGB bytes."
        )
    )
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    repo = args.repo_root.resolve()
    build = args.build_dir.resolve()
    source = (repo / "tests/direct_blocks_gpu_test.cpp").resolve()
    if not source.is_file():
        parser.error(f"missing fixture source: {source}")
    obj = args.output.resolve().with_suffix(".o")
    args.output.resolve().parent.mkdir(parents=True, exist_ok=True)
    compile_fixture(repo, build, source, obj)
    link_fixture(build, obj, args.output.resolve())
    print(f"built {args.output.resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
