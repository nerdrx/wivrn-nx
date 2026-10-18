#!/usr/bin/env python3
import json, shlex, subprocess, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build-server'
commands = json.loads((BUILD / 'compile_commands.json').read_text())
entry = next(x for x in commands if x['file'].endswith('server/driver/nx_fuse_tracker.cpp'))
flags = shlex.split(entry['command'])
for opt in ('-c',):
    while opt in flags: flags.remove(opt)
for opt in ('-o', '-MF', '-MT'):
    while opt in flags:
        i = flags.index(opt); del flags[i:i+2]
flags = [x for x in flags if not x.endswith('nx_fuse_tracker.cpp') and not x.startswith('-fmodule-mapper=')]
while '-x' in flags:
    i = flags.index('-x'); del flags[i:i+2]
with tempfile.TemporaryDirectory() as d:
    out = Path(d) / 'tracker-test'
    util = BUILD / '_deps/monado-build/src/xrt/auxiliary/util/libaux_util.a'
    subprocess.run(flags + ['-UNDEBUG', str(ROOT/'tests/nx_fuse_tracker_test.cpp'), str(ROOT/'server/driver/nx_fuse_tracker.cpp'), str(util), '-o', str(out)], cwd=BUILD, check=True)
    subprocess.run([str(out)], check=True)
print('nx_fuse_tracker device regression: OK')
