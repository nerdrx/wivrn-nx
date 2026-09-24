#!/usr/bin/env python3
"""Compare controller policies on a toy FIFO link; no device/server is started."""
import argparse
import csv
import io
import json
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--build-dir", type=Path, required=True, help="Configured build with Monado/Boost dependency sources")
parser.add_argument("--output-dir", type=Path, required=True)
parser.add_argument("--baseline-ref", default="3b142e1b")
args = parser.parse_args()
repo = Path(__file__).resolve().parents[1]
b = args.build_dir.resolve()
out = args.output_dir.resolve()
out.mkdir(parents=True, exist_ok=True)
flags = ["g++", "-std=c++23", "-Iserver", "-Iserver/driver", "-Icommon", "-I" + str(b / "common")]
flags += ["-I" + str(b / p) for p in ("_deps/monado-src/src/xrt/include", "_deps/monado-src/src/xrt/auxiliary", "_deps/monado-src/src/external/openxr_includes")]
flags += ["-isystem", "external", "-isystem", str(b / "_deps/boost-src/libs/pfr/include")]
summary = []
with tempfile.TemporaryDirectory(prefix="nx-recovery-") as tmp:
    tmp = Path(tmp)
    old = tmp / "baseline.cpp"
    old.write_bytes(subprocess.check_output(["git", "show", args.baseline_ref + ":server/driver/bitrate_controller.cpp"], cwd=repo))
    for label, source in (("fixed35", str(old)), ("adaptive", "server/driver/bitrate_controller.cpp")):
        exe = tmp / label
        subprocess.run(flags + ["-o", str(exe), "tests/bitrate_recovery_link_test.cpp", source, "common/smp.cpp", "-lcrypto"], cwd=repo, check=True)
        for delay in (0, 40, 100):
            for capacity in (400, 550, 700):
                raw = subprocess.check_output([str(exe), str(delay), ".96", str(capacity)], text=True)
                rows = list(csv.DictReader(io.StringIO(raw)))
                # Loss counters sampled at both interval boundaries: (40,70].
                weak = [r for r in rows if 40 <= float(r["seconds"]) < 70]
                at40 = next(r for r in rows if float(r["seconds"]) == 40)
                at70 = next(r for r in rows if float(r["seconds"]) == 70)
                recovered = next((round(float(r["seconds"]) - 70, 3) for r in rows if float(r["seconds"]) >= 70 and float(r["budget_mbps"]) == 1000), None)
                record = dict(policy=label, feedback_delay_ms=delay, weak_capacity_mbps=capacity,
                              lost_frames_40_70=int(at70["lost_frames"]) - int(at40["lost_frames"]),
                              total_lost_frames=int(rows[-1]["lost_frames"]),
                              mean_weak_budget_mbps=sum(float(r["budget_mbps"]) for r in weak) / len(weak),
                              recovery_seconds=recovered)
                if recovered is None:
                    raise RuntimeError("Controller did not recover on restored clean link")
                summary.append(record)
                if delay == 40 and capacity == 550:
                    (out / (label + ".csv")).write_text(raw)
(out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
for old, new in zip(summary[:9], summary[9:]):
    if new["lost_frames_40_70"] > old["lost_frames_40_70"] * .8:
        raise RuntimeError("Adaptive policy must reduce weak-link model losses by at least 20%")
    if new["mean_weak_budget_mbps"] < old["mean_weak_budget_mbps"] * .9:
        raise RuntimeError("Loss reduction must not come from a large budget reduction")
    if new["recovery_seconds"] > 6:
        raise RuntimeError("Restored model link must recover within six seconds")
print("Passed nine model comparisons; evidence written to", out)
