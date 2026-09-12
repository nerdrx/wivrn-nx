#!/usr/bin/env python3
import json, os, shlex, subprocess, tempfile, sys, time
from types import SimpleNamespace
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]; BUILD=ROOT/'build-server'
def main():
    commands=json.loads((BUILD/'compile_commands.json').read_text())
    command=next(x['command'] for x in commands if x['file'].endswith('server/driver/nx_fuse_corrections.cpp'))
    flags=shlex.split(command)
    for x in ('-c',): flags.remove(x)
    for x in ('-o','-MF','-MT'):
        while x in flags: i=flags.index(x); del flags[i:i+2]
    flags=[x for x in flags if not x.startswith('-fmodule-mapper=') and x!='-fdeps-format=p1689r5' and not x.endswith('nx_fuse_corrections.cpp')]
    with tempfile.TemporaryDirectory() as d:
        out=Path(d)/'check'; subprocess.run(flags+['-ffunction-sections','-fdata-sections',str(ROOT/'tests/nx_fuse_corrections_test.cpp'),str(ROOT/'server/driver/nx_fuse_corrections.cpp'),'-Wl,--gc-sections','-x','none',str(BUILD/'common/libwivrn-common.a'),'-lcrypto','-o',str(out)],cwd=BUILD,check=True)
        subprocess.run([str(out),str(Path(d)/'corrections.sock')],check=True)
        fuse_root = os.environ.get('NX_FUSE_ROOT')
        if fuse_root:
            sys.path.insert(0, fuse_root)
            from assistance import Assistance
            for mode in ('assist', 'camera_only'):
                path = Path(d)/'corrections.sock'
                process = subprocess.Popen([str(out), str(path), mode], stdout=subprocess.PIPE, stdin=subprocess.PIPE, text=True)
                assist = None
                try:
                    assert process.stdout.readline().strip() == 'READY'
                    stamp = time.monotonic_ns()
                    body = {'enabled':True, 'available':True, 'generation':'77', 'time_ns':stamp,
                            'joints':[{'joint':'hip', 'confidence':.95, 'position':[.2,1,0],
                                       'orientation':[0,0,0,1], 'baseline':[0,1,0]}]}
                    feed = {'connected':True, 'generation':'77', 'records':
                            [{'joint':'hip','route':'BD','flags':0x11}] if mode == 'assist' else []}
                    process.stdin.write(str(stamp)+'\n'); process.stdin.flush()
                    assist = Assistance(SimpleNamespace(snapshot=lambda:body), SimpleNamespace(snapshot=lambda:feed), path, start=False, camera_tracker_capable=True)
                    if mode == 'camera_only': assist.set_output_mode(mode)
                    assist.set_enabled(True)
                    time.sleep(.03)
                    assist.step()
                    assert process.wait(timeout=2) == 0, mode
                finally:
                    if assist: assist.close()
                    if process.poll() is None: process.kill(); process.wait()
                    process.stdout.close(); process.stdin.close()
                print('Python Assistance '+mode+' to C++ adapter: OK')
    print('nx_fuse_corrections validation test: OK')
if __name__=='__main__': main()
