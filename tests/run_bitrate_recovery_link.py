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
parser.add_argument("--v2", action="store_true", help="Compare unconverted and mapped v2 budgets")
parser.add_argument("--v2-probes", action="store_true", help="Compare mapped v2 baseline and adaptive probe gains")
args = parser.parse_args()
args.v2 = args.v2 or args.v2_probes
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
    (tmp / "driver").mkdir()
    (tmp / "driver" / "bitrate_controller.h").write_bytes(subprocess.check_output(["git", "show", args.baseline_ref + ":server/driver/bitrate_controller.h"], cwd=repo))
    old.write_bytes(subprocess.check_output(["git", "show", args.baseline_ref + ":server/driver/bitrate_controller.cpp"], cwd=repo))
    labels = ("fixed-probe-v2", "adaptive-probe-v2") if args.v2_probes else (("legacy-v2", "mapped-v2") if args.v2 else ("fixed35", "adaptive"))
    for label, source in ((labels[0], str(old)), (labels[1], "server/driver/bitrate_controller.cpp")):
        exe = tmp / label
        build_flags = flags if label != labels[0] else [flags[0], "-I" + str(tmp), "-I" + str(tmp / "driver")] + flags[1:]
        subprocess.run(build_flags + ["-o", str(exe), "tests/bitrate_recovery_link_test.cpp", source, "common/smp.cpp", "-lcrypto"], cwd=repo, check=True)
        for delay in (0, 40, 100):
            for capacity in (400, 550, 700):
                run_args = [str(exe), str(delay), ".96", str(capacity)]
                if args.v2:
                    run_args += ["bbr", "tagged"]
                raw = subprocess.check_output(run_args, text=True)
                rows = list(csv.DictReader(io.StringIO(raw)))
                # Loss counters sampled at both interval boundaries: (40,70].
                weak = [r for r in rows if 40 <= float(r["seconds"]) < 70]
                at40 = next(r for r in rows if float(r["seconds"]) == 40)
                at70 = next(r for r in rows if float(r["seconds"]) == 70)
                recovered = next((round(float(r["seconds"]) - 70, 3) for r in rows if float(r["seconds"]) >= 70 and float(r["budget_mbps"]) == 1000), None)
                clean = [r for r in rows if 2 <= float(r["seconds"]) < 10]
                record = dict(mean_initial_budget_mbps=sum(float(r["budget_mbps"]) for r in clean) / len(clean), policy=label, feedback_delay_ms=delay, weak_capacity_mbps=capacity,
                              lost_frames_40_70=int(at70["lost_frames"]) - int(at40["lost_frames"]),
                              total_lost_frames=int(rows[-1]["lost_frames"]),
                              mean_weak_budget_mbps=sum(float(r["budget_mbps"]) for r in weak) / len(weak),
                              recovery_seconds=recovered)
                if recovered is None and not (args.v2 and not args.v2_probes and label == labels[0]):
                    raise RuntimeError(f"{label}, delay={delay}, capacity={capacity}: controller did not recover")
                summary.append(record)
                if delay == 40 and capacity == 550:
                    (out / (label + ".csv")).write_text(raw)
(out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
for old, new in zip(summary[:9], summary[9:]):
    if args.v2_probes:
        if new["lost_frames_40_70"] > old["lost_frames_40_70"] * .4:
            raise RuntimeError("Direct probe retries must cut model losses by at least 60%")
        if new["mean_weak_budget_mbps"] < old["mean_weak_budget_mbps"] * .95:
            raise RuntimeError("Direct probe loss savings must retain 95% of the weak-link budget")
        if new["recovery_seconds"] > old["recovery_seconds"] + 1.5:
            raise RuntimeError("Direct probe recovery must stay within 1.5 seconds of baseline")
    if args.v2:
        if new["mean_initial_budget_mbps"] < 999:
            raise RuntimeError("Mapped v2 must retain full clean-link budget")
        if new["mean_weak_budget_mbps"] >= 1000:
            raise RuntimeError("Mapped v2 must back off on the limited model link")
        if new["recovery_seconds"] > 12:
            raise RuntimeError("Mapped v2 must recover within twelve model seconds")
    else:
        if new["lost_frames_40_70"] > old["lost_frames_40_70"] * .8:
            raise RuntimeError("Adaptive policy must reduce weak-link model losses by at least 20%")
        if new["mean_weak_budget_mbps"] < old["mean_weak_budget_mbps"] * .9:
            raise RuntimeError("Loss reduction must not come from a large budget reduction")
        if new["recovery_seconds"] > 6:
            raise RuntimeError("Restored model link must recover within six seconds")
print("Passed nine model comparisons; evidence written to", out)
