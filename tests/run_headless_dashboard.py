#!/usr/bin/env python3
"""Smoke-test the native dashboard on an isolated Gamescope display only."""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time


def child(args):
    # Gamescope must supply the only display. Qt cannot choose the desktop Wayland socket.
    if not os.environ.get('DISPLAY') or os.environ.get('WAYLAND_DISPLAY'):
        raise RuntimeError('Expected only the private Gamescope X display')
    output = Path(args.output)
    (output/'display.json').write_text(json.dumps({'display':os.environ['DISPLAY'], 'backend':'headless'}))
    with (output/'dashboard.log').open('w') as log:
        app = subprocess.Popen([args.dashboard], stdout=log, stderr=subprocess.STDOUT)
        try:
            time.sleep(3)
            if app.poll() is not None:
                raise RuntimeError('Dashboard exited during startup')
            windows = subprocess.check_output(['xdotool','search','--onlyvisible','--pid',str(app.pid)],text=True,timeout=3).split()
            if not windows:
                raise RuntimeError('Dashboard has no mapped window on the private display')
            geometry = subprocess.check_output(['xdotool','getwindowgeometry','--shell',windows[0]],text=True,timeout=3)
            (output/'window.txt').write_text(geometry)
            (output/'passed').write_text('Dashboard mapped a window on the private display\n')
        finally:
            app.terminate()
            try: app.wait(timeout=3)
            except subprocess.TimeoutExpired: app.kill(); app.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dashboard',default=str(Path(__file__).resolve().parents[1]/'build-server/server/wivrn-dashboard'))
    parser.add_argument('--port',type=int,default=8802,help='Local test fixture port, never the live worker')
    parser.add_argument('--output',required=True)
    parser.add_argument('--child',action='store_true',help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.child:
        child(args)
        return
    if args.port in (8787,8788) or not 1024 <= args.port <= 65535:
        parser.error('Use a separate fixture port')
    output = Path(args.output).absolute()
    output.mkdir(parents=True,exist_ok=True)
    (output/'passed').unlink(missing_ok=True)
    with tempfile.TemporaryDirectory(prefix='nx-dashboard-test-') as config:
        environment = dict(os.environ)
        for name in ('DISPLAY','WAYLAND_DISPLAY','XAUTHORITY','DBUS_SESSION_BUS_ADDRESS'):
            environment.pop(name,None)
        environment.update(QT_QPA_PLATFORM='xcb', QT_NO_XDG_DESKTOP_PORTAL='1', NX_DASHBOARD_PREVIEW='1', NX_FUSE_PORT=str(args.port),
                           XDG_CONFIG_HOME=config, XDG_CACHE_HOME=config+'/cache')
        command = ['dbus-run-session','--','gamescope','--backend=headless','-W','1280','-H','900',
                   '-w','1280','-h','900','--','env','-u','WAYLAND_DISPLAY',sys.executable,
                   str(Path(__file__).resolve()),'--child','--dashboard',str(Path(args.dashboard).absolute()),
                   '--port',str(args.port),'--output',str(output)]
        with (output/'gamescope.log').open('w') as log:
            process = subprocess.Popen(command,env=environment,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
            try:
                if process.wait(timeout=20) or not (output/'passed').is_file():
                    raise RuntimeError('Headless smoke failed; inspect gamescope.log')
            finally:
                # Clean descendants even if the compositor or dashboard fails.
                try: os.killpg(process.pid,signal.SIGTERM)
                except ProcessLookupError: pass
    print('Isolated headless dashboard smoke: OK; '+str(output))


if __name__ == '__main__': main()
