#!/usr/bin/env python3
"""Turn a live capture into a record: every number the HUD and stat lines carry, once.

WHY THIS EXISTS. The operating point of this fork -- 38.5 fps paced, 16.69 ms of Adreno per
stereo frame, 92 ms of pose age, the Pass B split, the 92 % GPU duty -- has been quoted in
ADRs, in docs and in a paper draft, and every one of those quotes was read by eye out of a
log in nx-scratch/ that is not in the repository and will not survive the disk. A number
without a record is an anecdote. This makes the record.

WHAT IT DOES. Given a server log, optionally a client logcat capture, and optionally a time
window, it parses every periodic stat line both ends emit, aggregates each field over the
window (n / mean / min / max, and `last` for counters), and writes a JSON and a Markdown
record carrying the provenance: the log paths, their sizes and mtimes, the window, and the
exact line counts each figure came from.

THE WINDOW, AND AN HONEST LIMITATION. Client captures are logcat and every line carries a
wall clock, so --from/--to select on it exactly. **Server logs carry no timestamps at all**
-- the lines are `INFO [encode] nxwarp: ...` with nothing in front -- so a wall-clock window
cannot be applied to them. The server side is therefore selected by REPORT INDEX
(--server-from / --server-to, zero-based, over the 2-second encode reports), and when no
server window is given the whole session is used. Every record states which of the two it
got, because a reader comparing a server figure against a client figure over "the same
window" is entitled to know they were not selected the same way.

  tools/capture-record.py --name server76 \
      --server nx-scratch/live/server76.log \
      --client nx-scratch/live/cap76.log \
      --from 13:08:00 --to 13:09:30 \
      --note "90 s at the shipped operating point" \
      --out docs/measurements/2026-09-06
"""
import argparse, datetime, hashlib, json, os, re, statistics, sys

# --------------------------------------------------------------------------- helpers

NUM = r"([-+]?\d+(?:\.\d+)?)"


def f(x):
    return float(x)


class Series:
    """One field, sampled once per report line."""

    def __init__(self, unit=None, kind="gauge"):
        self.v = []
        self.unit = unit
        self.kind = kind          # gauge (mean/min/max) or counter (last)

    def add(self, x):
        self.v.append(x)

    def summary(self):
        if not self.v:
            return None
        d = {"n": len(self.v)}
        if self.kind == "counter":
            d["last"] = self.v[-1]
            d["first"] = self.v[0]
            d["delta"] = self.v[-1] - self.v[0]
        else:
            d["mean"] = round(statistics.fmean(self.v), 4)
            d["min"] = min(self.v)
            d["max"] = max(self.v)
            if len(self.v) > 1:
                d["stdev"] = round(statistics.stdev(self.v), 4)
        if self.unit:
            d["unit"] = self.unit
        return d


class Bag(dict):
    def s(self, key, unit=None, kind="gauge"):
        if key not in self:
            self[key] = Series(unit, kind)
        return self[key]

    def dump(self):
        return {k: v.summary() for k, v in sorted(self.items()) if v.summary()}


# --------------------------------------------------------------------------- server

SRV = [
    # the two-second encode report -- the densest line either end emits
    ("encode", re.compile(
        r"nxwarp: stream (?P<stream>\d+) encoded (?P<frames>\d+) frames in (?P<secs>[\d.]+)"
        r" s: (?P<enc>[\d.]+) ms/frame \(max (?P<encmax>[\d.]+)\), (?P<bpf>\d+) B/frame"
        # Two shapes, because "rc": "fixed" prints neither a target nor a controller:
        #   ... B/frame vs <target> target (<err>%), QP <q> [<lo>..<hi>], controller allows <M> Mbit/s
        #   ... B/frame at fixed QP <q>
        r"(?: vs (?P<target>\d+) target \((?P<err>[-+]?\d+)%\), QP (?P<qp>[\d.]+)"
        r" \[(?P<qplo>\d+)\.\.(?P<qphi>\d+)\], controller allows (?P<mbit>[\d.]+)"
        r" Mbit/s(?: \((?P<rcnote>[^)]*)\))?"
        r"| at fixed QP (?P<fqp>[\d.]+))"
        r", paced to (?P<fps>[\d.]+)"
        r" fps \((?:client decode (?P<cdec>[\d.]+) ms|(?P<nodec>client decode not reported yet))\),"
        r" (?P<unsent>\d+) composited")),
    ("tilemap", re.compile(
        r"tile mapping: (?P<spans>\d+) frame\(s\) with per-tile spans, (?P<chunks>\d+) with "
        r"the fixed-chunk fallback \(\"tile-map\": \"(?P<mode>[a-z]+)\"\)")),
    ("notheld", re.compile(
        r"the headset did not reconstruct (?P<n>\d+) frame\(s\) since the last report; "
        r"(?P<intra>\d+) of them cost an all-intra frame and (?P<free>\d+) named a frame "
        r"older[^.]*\. Last was frame (?P<last>\d+), (?P<why>[^\n]*)")),
]

SRV_ONCE = [
    ("build", re.compile(r"^WiVRn (?P<v>\S+) starting")),
    ("geometry", re.compile(
        r"nxwarp: both eyes on stream 0 as one (?P<w>\d+)x(?P<h>\d+) stereo frame "
        r"\((?P<tiles>\d+) tiles\)")),
    ("backend", re.compile(r"nxwarp: stream 0 backend: (?P<backend>.+?)(?:, reading|$)")),
    ("tools", re.compile(
        r"nxwarp: stream 0 tools (?P<enc>0x[0-9a-f]+) \((?P<names>[^)]*)\), headset "
        r"(?P<client>0x[0-9a-f]+)")),
    ("entropy", re.compile(r'nxwarp: "entropy": "(?P<req>\w+)" -> (?P<got>\w+)')),
    ("rc", re.compile(
        r"rate control on: (?P<bps>\d+) bit/s is (?P<bpf>\d+) B/frame at (?P<hz>[\d.]+)"
        r" Hz, QP band (?P<lo>\d+)\.\.(?P<hi>\d+)")),
    ("bitrate_cfg", re.compile(
        r"Automatic bitrate enabled, (?P<algo>[^,]+), ceiling (?P<ceil>[\d.]+)"
        r" Mbit/s, floor (?P<floor>[\d.]+) Mbit/s")),
    ("gpu", re.compile(r"^\tGPU: (?P<gpu>.+)$")),
    ("fec", re.compile(r"Forward error correction enabled, (?P<fec>.+)$")),
]


# A line that looks like a periodic report and does not parse is silent data loss: the record
# would simply be missing a sample and would look complete. Both parsers therefore count the
# near-misses and put the count in the record, so a regex that falls behind a log format change
# announces itself instead of quietly shrinking the sample.
SRV_LOOKS = re.compile(r"nxwarp: stream \d+ encoded \d+ frames in")
CLI_LOOKS = re.compile(r"nxwarp\[\d+\]: \d+ frames in |render: \d+ iterations in |"
                       r"passB [\d.]+ ms = warp |render: displayed pose age ")


def parse_server(path, ifrom, ito):
    b, once, reports, encoder_cfg = Bag(), {}, 0, []
    unparsed = 0
    in_cfg = False
    with open(path, errors="replace") as fh:
        for line in fh:
            for name, rx in SRV_ONCE:
                if name in once:
                    continue
                m = rx.search(line)
                if m:
                    once[name] = {k: v for k, v in m.groupdict().items() if v is not None}
            if "Encoder configuration:" in line:
                in_cfg = True
                continue
            if in_cfg:
                if line.startswith("\t"):
                    encoder_cfg.append(line.strip())
                    continue
                in_cfg = False

            m = SRV[0][1].search(line)
            if not m and SRV_LOOKS.search(line):
                unparsed += 1
            if m:
                idx = reports
                reports += 1
                if idx < ifrom or (ito is not None and idx > ito):
                    continue
                g = m.groupdict()
                b.s("frames_per_report", "frames").add(int(g["frames"]))
                b.s("report_seconds", "s").add(f(g["secs"]))
                b.s("encode_ms", "ms").add(f(g["enc"]))
                b.s("encode_ms_max", "ms").add(f(g["encmax"]))
                b.s("bytes_per_frame", "B").add(int(g["bpf"]))
                if g["target"] is not None:
                    b.s("target_bytes_per_frame", "B").add(int(g["target"]))
                    b.s("rate_error_pct", "%").add(int(g["err"]))
                    b.s("qp_mean").add(f(g["qp"]))
                    b.s("qp_lo").add(int(g["qplo"]))
                    b.s("qp_hi").add(int(g["qphi"]))
                    b.s("controller_mbit", "Mbit/s").add(f(g["mbit"]))
                else:
                    # "rc": "fixed" -- one quantiser for the session, no controller
                    b.s("qp_mean").add(f(g["fqp"]))
                    once.setdefault("rate_control", "fixed")
                b.s("paced_fps", "fps").add(f(g["fps"]))
                if g["cdec"] is not None:
                    b.s("client_decode_ms", "ms").add(f(g["cdec"]))
                b.s("frames_not_sent", "frames").add(int(g["unsent"]))
                continue
            m = SRV[1][1].search(line)
            if m:
                b.s("tilemap_span_frames", "frames").add(int(m.group("spans")))
                b.s("tilemap_chunk_frames", "frames").add(int(m.group("chunks")))
                once.setdefault("tile_map_mode", m.group("mode"))
                continue
            m = SRV[2][1].search(line)
            if m:
                b.s("not_reconstructed", "frames").add(int(m.group("n")))
                b.s("not_reconstructed_cost_intra", "frames").add(int(m.group("intra")))
                b.s("not_reconstructed_free", "frames").add(int(m.group("free")))
                once.setdefault("not_reconstructed_last_reason", m.group("why").strip())
    if encoder_cfg:
        once["encoder_configuration"] = encoder_cfg
    once["encode_reports_in_log"] = reports
    once["report_lines_not_parsed"] = unparsed
    return b, once


# --------------------------------------------------------------------------- client

TS = re.compile(r"^(\d\d)-(\d\d) (\d\d):(\d\d):(\d\d)\.(\d\d\d)")

CLI = [
    ("decode", re.compile(
        r"nxwarp\[(?P<stream>\d+)\]: (?P<frames>\d+) frames in (?P<secs>[\d.]+) s: "
        r"(?P<bpf>\d+) B/frame, wait-prev (?P<waitprev>[\d.]+) ms, wall (?P<wall>[\d.]+) ms, "
        r"nxvc passA (?P<pa>[\d.]+) passB (?P<pb>[\d.]+) gpu (?P<gpu>[\d.]+) ms; "
        r"holes (?P<holes>\d+), refused (?P<refused>\d+)")),
    ("passb", re.compile(
        r"passB (?P<tot>[\d.]+) ms = warp (?P<warp>[\d.]+) \+ skip (?P<skip>[\d.]+) \+ "
        r"coded (?P<coded>[\d.]+) \+ dir (?P<dir>[\d.]+) \+ other (?P<other>[\d.]+)"
        r"(?:; tiles skip (?P<tskip>\d+) / coded (?P<tcoded>\d+) / dir (?P<tdir>\d+))?")),
    ("stage", re.compile(
        r"stage: gap (?P<gap>[\d.]+) \| wait-prev (?P<waitprev>[\d.]+) \| "
        r"submit (?P<submit>[\d.]+) \(qlock (?P<qlock>[\d.]+) \+ codec (?P<codec>[\d.]+)\) \| "
        r"get_free (?P<getfree>[\d.]+) \| fence-pre (?P<fpre>[\d.]+) \| "
        r"record (?P<record>[\d.]+) \| qsubmit (?P<qsub>[\d.]+) \| "
        r"fence-post (?P<fpost>[\d.]+) \| publish (?P<publish>[\d.]+) ms; "
        r"withheld (?P<withheld>\d+), stride (?P<stride>\d+), arrival (?P<arrival>[\d.]+) ms")),
    ("net", re.compile(
        r"net: (?P<closed>\d+) frames closed in (?P<secs>[\d.]+) s, (?P<holes>\d+) with a hole, "
        r"(?P<ooo>\d+) out-of-order datagrams, (?P<late>\d+) frames completed late, "
        r"(?P<queued>\d+) queued for the worker, (?P<decoded>\d+) decoded so far, "
        r"(?P<strag>\d+) stragglers dropped")),
    ("netcost", re.compile(
        r"net: (?P<per>[\d.]+) ms per datagram over (?P<dg>\d+) datagrams "
        r"\((?P<persec>[\d.]+) ms of every second\)")),
    ("sel", re.compile(
        r"sel: stride (?P<stride>\d+), arrival (?P<arrival>[\d.]+) ms, dropped-late (?P<dl>\d+), "
        r"withheld (?P<wh>\d+), decoded (?P<dec>\d+)")),
    ("rx", re.compile(
        r"rx: (?P<dg>\d+) datagrams, placed (?P<placed>\d+) \([^)]*\), late (?P<late>\d+) ")),
    ("reasm", re.compile(
        r"reassembly \(network thread\): (?P<ms>[\d.]+) ms/frame over (?P<n>\d+) frames, "
        r"buffer (?P<buf>\d+) kB held for (?P<held>\d+) kB of unit \((?P<ratio>[\d.]+)x\)")),
    ("loop", re.compile(
        r"render: (?P<iters>\d+) iterations in (?P<secs>[\d.]+) s \((?P<rate>[\d.]+)/s\), "
        r"(?P<layer>\d+) submitted a layer, (?P<gated>\d+) skipped by the repeat gate, "
        r"(?P<nothing>\d+) with nothing to show; display period (?P<period>[\d.]+) ms")),
    ("apppass", re.compile(
        r"render: this app's own GPU pass (?P<ms>[\d.]+) ms per iteration")),
    ("defov", re.compile(
        r"render: defoveate (?P<w>\d+)x(?P<h>\d+) per eye x2 = (?P<mpx>[\d.]+) "
        r"Mpx/frame at scale (?P<scale>[\d.]+) atlas-mode (?P<atlas>\d+); "
        r"(?P<cache>\d+) re-presented")),
    ("pose", re.compile(
        r"render: displayed pose age (?P<age>[\d.]+) ms mean \(worst (?P<worst>[\d.]+)\) over "
        r"(?P<n>\d+) frames \| submit lead (?P<lead>[\d.]+) ms mean \(worst (?P<leadw>[\d.]+)"
        r"\) \| misses: (?P<overrun>\d+) overrun (?P<late>\d+) late (?P<skipped>\d+) skipped")),
    ("periter", re.compile(
        r"render: per iteration fence (?P<fence>[\d.]+) \(worst (?P<fw>[\d.]+)\) \| "
        r"queries (?P<q>[\d.]+) \| submit (?P<sub>[\d.]+) \| whole render\(\) (?P<r>[\d.]+) ms")),
]

WIFI = re.compile(
    r"(?:RSSI|rssi)[ =:]*(?P<rssi>-?\d+)|(?:band|Band)[ =:]*(?P<band>2\.4|5|6)\s*GHz|"
    r"link speed[ =:]*(?P<link>\d+)", re.I)


def parse_client(path, tfrom, tto):
    b, once = Bag(), {}
    seen, kept, wifi, unparsed = 0, 0, {}, 0
    with open(path, errors="replace") as fh:
        for line in fh:
            m = TS.match(line)
            if m:
                seen += 1
                t = f"{m.group(3)}:{m.group(4)}:{m.group(5)}"
                if tfrom and t < tfrom:
                    continue
                if tto and t > tto:
                    continue
                kept += 1
            w = WIFI.search(line)
            if w and any(w.groupdict().values()):
                for k, v in w.groupdict().items():
                    if v is not None:
                        wifi[k] = v

            if CLI_LOOKS.search(line) and not any(rx.search(line) for _, rx in CLI):
                unparsed += 1
            m = CLI[0][1].search(line)
            if m:
                g = m.groupdict()
                b.s("frames_per_report", "frames").add(int(g["frames"]))
                b.s("report_seconds", "s").add(f(g["secs"]))
                b.s("bytes_per_frame", "B").add(int(g["bpf"]))
                b.s("wait_prev_ms", "ms").add(f(g["waitprev"]))
                b.s("wall_ms", "ms").add(f(g["wall"]))
                b.s("passA_ms", "ms").add(f(g["pa"]))
                b.s("passB_ms", "ms").add(f(g["pb"]))
                b.s("gpu_ms_per_stereo_frame", "ms").add(f(g["gpu"]))
                b.s("holes", "frames").add(int(g["holes"]))
                b.s("refused", "frames").add(int(g["refused"]))
                continue
            m = CLI[1][1].search(line)
            if m:
                g = m.groupdict()
                b.s("passB_total_ms", "ms").add(f(g["tot"]))
                b.s("passB_warp_ms", "ms").add(f(g["warp"]))
                b.s("passB_skip_ms", "ms").add(f(g["skip"]))
                b.s("passB_coded_ms", "ms").add(f(g["coded"]))
                b.s("passB_dir_ms", "ms").add(f(g["dir"]))
                b.s("passB_other_ms", "ms").add(f(g["other"]))
                if g["tskip"] is not None:
                    b.s("passB_tiles_skip", "tiles").add(int(g["tskip"]))
                    b.s("passB_tiles_coded", "tiles").add(int(g["tcoded"]))
                    b.s("passB_tiles_dir", "tiles").add(int(g["tdir"]))
                continue
            m = CLI[2][1].search(line)
            if m:
                g = m.groupdict()
                for key, name in [("gap", "stage_gap_ms"), ("waitprev", "stage_wait_prev_ms"),
                                  ("submit", "stage_submit_ms"), ("qlock", "stage_submit_qlock_ms"),
                                  ("codec", "stage_submit_codec_ms"), ("getfree", "stage_get_free_ms"),
                                  ("fpre", "stage_fence_pre_ms"), ("record", "stage_record_ms"),
                                  ("qsub", "stage_qsubmit_ms"), ("fpost", "stage_fence_post_ms"),
                                  ("publish", "stage_publish_ms"), ("arrival", "stage_arrival_ms")]:
                    b.s(name, "ms").add(f(g[key]))
                b.s("stage_withheld", "frames").add(int(g["withheld"]))
                b.s("stage_stride").add(int(g["stride"]))
                continue
            m = CLI[3][1].search(line)
            if m:
                g = m.groupdict()
                b.s("net_frames_closed", "frames").add(int(g["closed"]))
                b.s("net_frames_with_hole", "frames").add(int(g["holes"]))
                b.s("net_out_of_order", "datagrams").add(int(g["ooo"]))
                b.s("net_frames_late", "frames").add(int(g["late"]))
                b.s("net_queued_for_worker", "frames").add(int(g["queued"]))
                b.s("net_decoded_total", "frames", "counter").add(int(g["decoded"]))
                b.s("net_stragglers_dropped", "datagrams", "counter").add(int(g["strag"]))
                continue
            m = CLI[4][1].search(line)
            if m:
                g = m.groupdict()
                b.s("net_ms_per_datagram", "ms").add(f(g["per"]))
                b.s("net_datagrams_per_report", "datagrams").add(int(g["dg"]))
                b.s("net_ms_per_second", "ms/s").add(f(g["persec"]))
                continue
            m = CLI[5][1].search(line)
            if m:
                g = m.groupdict()
                b.s("sel_arrival_ms", "ms").add(f(g["arrival"]))
                b.s("sel_dropped_late", "frames", "counter").add(int(g["dl"]))
                b.s("sel_withheld", "frames", "counter").add(int(g["wh"]))
                b.s("sel_decoded", "frames", "counter").add(int(g["dec"]))
                continue
            m = CLI[6][1].search(line)
            if m:
                g = m.groupdict()
                b.s("rx_datagrams", "datagrams", "counter").add(int(g["dg"]))
                b.s("rx_placed", "datagrams", "counter").add(int(g["placed"]))
                b.s("rx_late", "datagrams", "counter").add(int(g["late"]))
                continue
            m = CLI[7][1].search(line)
            if m:
                g = m.groupdict()
                b.s("reassembly_ms_per_frame", "ms").add(f(g["ms"]))
                b.s("reassembly_buffer_kB", "kB").add(int(g["buf"]))
                b.s("reassembly_held_kB", "kB").add(int(g["held"]))
                b.s("reassembly_ratio", "x").add(f(g["ratio"]))
                continue
            m = CLI[8][1].search(line)
            if m:
                g = m.groupdict()
                b.s("loop_iterations", "iterations").add(int(g["iters"]))
                b.s("loop_rate", "/s").add(f(g["rate"]))
                b.s("loop_submitted_layer", "iterations").add(int(g["layer"]))
                b.s("loop_gated_out", "iterations").add(int(g["gated"]))
                b.s("loop_nothing_to_show", "iterations").add(int(g["nothing"]))
                b.s("display_period_ms", "ms").add(f(g["period"]))
                continue
            m = CLI[9][1].search(line)
            if m:
                b.s("display_pass_ms", "ms").add(f(m.group("ms")))
                continue
            m = CLI[10][1].search(line)
            if m:
                g = m.groupdict()
                once.setdefault("defoveate_per_eye", f'{g["w"]}x{g["h"]}')
                once.setdefault("atlas_mode", int(g["atlas"]))
                b.s("defoveate_mpx", "Mpx").add(f(g["mpx"]))
                b.s("defoveate_scale").add(f(g["scale"]))
                b.s("cache_represented", "frames").add(int(g["cache"]))
                continue
            m = CLI[11][1].search(line)
            if m:
                g = m.groupdict()
                b.s("pose_age_mean_ms", "ms").add(f(g["age"]))
                b.s("pose_age_worst_ms", "ms").add(f(g["worst"]))
                b.s("pose_frames", "frames").add(int(g["n"]))
                b.s("submit_lead_mean_ms", "ms").add(f(g["lead"]))
                b.s("submit_lead_worst_ms", "ms").add(f(g["leadw"]))
                b.s("misses_overrun", "frames").add(int(g["overrun"]))
                b.s("misses_late", "frames").add(int(g["late"]))
                b.s("misses_skipped_refresh", "frames").add(int(g["skipped"]))
                continue
            m = CLI[12][1].search(line)
            if m:
                g = m.groupdict()
                b.s("iter_fence_ms", "ms").add(f(g["fence"]))
                b.s("iter_fence_worst_ms", "ms").add(f(g["fw"]))
                b.s("iter_queries_ms", "ms").add(f(g["q"]))
                b.s("iter_submit_ms", "ms").add(f(g["sub"]))
                b.s("iter_render_ms", "ms").add(f(g["r"]))
    once["timestamped_lines"] = seen
    once["lines_in_window"] = kept
    once["stat_lines_not_parsed"] = unparsed
    # Wi-Fi is not in any capture this fork produces today; the field exists so that a
    # capture that DOES carry it records it rather than dropping it on the floor.
    once["wifi"] = wifi or None
    return b, once


# --------------------------------------------------------------------------- output

def provenance(path):
    st = os.stat(path)
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return {
        "path": path,
        "bytes": st.st_size,
        "mtime": datetime.datetime.fromtimestamp(st.st_mtime).isoformat(timespec="seconds"),
        "sha256": h.hexdigest(),
    }


# A trimmed server log -- one cut to the window of interest -- carries the periodic reports
# but not the once-per-session header lines. That is a property of the excerpt, not a parse
# failure, and the record has to say which so a reader does not read "?" as "unknown".
ABSENT = "_not carried by this log excerpt_"


def miss(v):
    return v if v else ABSENT


def code(v):
    return f"`{v}`" if v else ABSENT


def fmt(v):
    if v is None:
        return "-"
    if isinstance(v, float):
        return f"{v:.2f}".rstrip("0").rstrip(".") if abs(v) < 1e6 else f"{v:.0f}"
    return str(v)


def table(bag, keys=None):
    d = bag.dump()
    keys = keys or sorted(d)
    rows = ["| field | n | mean | min | max | unit |", "|---|---|---|---|---|---|"]
    for k in keys:
        if k not in d:
            continue
        s = d[k]
        if "mean" in s:
            rows.append(f"| `{k}` | {s['n']} | **{fmt(s['mean'])}** | {fmt(s['min'])} | "
                        f"{fmt(s['max'])} | {s.get('unit', '')} |")
        else:
            rows.append(f"| `{k}` | {s['n']} | last **{fmt(s['last'])}** | "
                        f"first {fmt(s['first'])} | +{fmt(s['delta'])} | {s.get('unit', '')} |")
    return "\n".join(rows) if len(rows) > 2 else "_nothing parsed_"


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--name", required=True)
    p.add_argument("--server", required=True)
    p.add_argument("--client")
    p.add_argument("--from", dest="tfrom", help="HH:MM:SS, client capture only")
    p.add_argument("--to", dest="tto", help="HH:MM:SS, client capture only")
    p.add_argument("--server-from", type=int, default=0,
                   help="first encode report to include, zero-based")
    p.add_argument("--server-to", type=int, default=None)
    p.add_argument("--note", default="")
    p.add_argument("--verdict", default="", help="one line: what this capture shows")
    p.add_argument("--out", required=True)
    a = p.parse_args()

    sbag, sonce = parse_server(a.server, a.server_from, a.server_to)
    cbag, conce = (parse_client(a.client, a.tfrom, a.tto) if a.client else (Bag(), {}))

    rec = {
        "name": a.name,
        "note": a.note,
        "verdict": a.verdict,
        "generated": datetime.datetime.now().isoformat(timespec="seconds"),
        "tool": "tools/capture-record.py",
        "window": {
            "client_from": a.tfrom, "client_to": a.tto,
            "server_report_from": a.server_from, "server_report_to": a.server_to,
            "note": ("Server logs carry no timestamps, so the wall-clock window applies to "
                     "the client capture only; the server side is selected by report index."),
        },
        "provenance": {
            "server": provenance(a.server),
            "client": provenance(a.client) if a.client else None,
        },
        "server": {"session": sonce, "metrics": sbag.dump()},
        "client": {"session": conce, "metrics": cbag.dump()},
    }
    os.makedirs(a.out, exist_ok=True)
    with open(os.path.join(a.out, a.name + ".json"), "w") as fh:
        json.dump(rec, fh, indent=1, sort_keys=False)
        fh.write("\n")

    sm, cm = sbag.dump(), cbag.dump()

    def g(d, k, stat="mean"):
        return d.get(k, {}).get(stat)

    head = [f"# Capture record: {a.name}", ""]
    if a.verdict:
        head.append(f"**{a.verdict}**")
        head.append("")
    if a.note:
        head += [a.note, ""]
    head += [
        "| | |", "|---|---|",
        f"| server log | `{a.server}` ({rec['provenance']['server']['bytes']} B, "
        f"{rec['provenance']['server']['mtime']}) |",
    ]
    if a.client:
        head.append(f"| client capture | `{a.client}` "
                    f"({rec['provenance']['client']['bytes']} B, "
                    f"{rec['provenance']['client']['mtime']}) |")
    head += [
        f"| window | client {a.tfrom or 'whole capture'} .. {a.tto or 'end'}; "
        f"server reports {a.server_from}..{a.server_to if a.server_to is not None else 'end'} |",
        f"| server build | {code(sonce.get('build', {}).get('v'))} |",
        f"| server GPU | {miss(sonce.get('gpu', {}).get('gpu'))} |",
        f"| encoder backend | {miss(sonce.get('backend', {}).get('backend'))} |",
        f"| stream geometry | " + (
            f"{sonce['geometry']['w']}x{sonce['geometry']['h']}, "
            f"{sonce['geometry']['tiles']} tiles" if 'geometry' in sonce else ABSENT) + " |",
        f"| negotiated tools | " + (
            f"`{sonce['tools']['enc']}` ({sonce['tools']['names']})"
            if 'tools' in sonce else ABSENT) + " |",
        f"| entropy | " + (
            f"{sonce['entropy']['req']} -> {sonce['entropy']['got']}"
            if 'entropy' in sonce else ABSENT) + " |",
        f"| encode reports in log | {sonce.get('encode_reports_in_log', 0)}"
        + (f" (**{sonce['report_lines_not_parsed']} report line(s) did not parse**)"
           if sonce.get("report_lines_not_parsed") else "") + " |",
        f"| client stat lines that did not parse | "
        f"{conce.get('stat_lines_not_parsed', 0) if a.client else 'no client capture'} |",
        f"| Wi-Fi band / RSSI | {conce.get('wifi') or 'not carried by this capture'} |",
        "",
        "## The operating point",
        "",
        "| | |", "|---|---|",
        f"| paced frame rate | **{fmt(g(sm,'paced_fps'))} fps** |",
        f"| bytes per frame (server) | **{fmt(g(sm,'bytes_per_frame'))} B** |",
        f"| quantiser | **{fmt(g(sm,'qp_mean'))}** in [{fmt(g(sm,'qp_lo','min'))}"
        f"..{fmt(g(sm,'qp_hi','max'))}] |",
        f"| server encode | **{fmt(g(sm,'encode_ms'))} ms/frame** "
        f"(worst {fmt(g(sm,'encode_ms_max','max'))}) |",
        f"| client GPU per stereo frame | **{fmt(g(cm,'gpu_ms_per_stereo_frame'))} ms** "
        f"(passA {fmt(g(cm,'passA_ms'))} + passB {fmt(g(cm,'passB_ms'))}) |",
        f"| client decode wall | **{fmt(g(cm,'wall_ms'))} ms** |",
        f"| displayed pose age | **{fmt(g(cm,'pose_age_mean_ms'))} ms** mean, "
        f"worst {fmt(g(cm,'pose_age_worst_ms','max'))} |",
        f"| render loop | **{fmt(g(cm,'loop_rate'))}/s** against a "
        f"{fmt(g(cm,'display_period_ms'))} ms display period |",
        f"| display pass | **{fmt(g(cm,'display_pass_ms'))} ms** per iteration |",
        f"| holes / refused | {fmt(g(cm,'holes','max'))} / {fmt(g(cm,'refused','max'))} |",
        "",
    ]
    if "passB_skip_ms" in cm:
        head += [
            "### The Pass B split",
            "",
            f"`passB {fmt(g(cm,'passB_total_ms'))} ms = warp {fmt(g(cm,'passB_warp_ms'))} "
            f"+ skip {fmt(g(cm,'passB_skip_ms'))} + coded {fmt(g(cm,'passB_coded_ms'))} "
            f"+ dir {fmt(g(cm,'passB_dir_ms'))} + other {fmt(g(cm,'passB_other_ms'))}`"
            + (f" over tiles skip {fmt(g(cm,'passB_tiles_skip'))} / coded "
               f"{fmt(g(cm,'passB_tiles_coded'))} / dir {fmt(g(cm,'passB_tiles_dir'))}"
               if "passB_tiles_skip" in cm else ""),
            "",
        ]
        sk, tot = g(cm, "passB_skip_ms"), g(cm, "passB_total_ms")
        if sk and tot:
            head += [f"The skip term is **{sk / tot * 100:.0f} %** of Pass B.", ""]
    if "display_pass_ms" in cm and "gpu_ms_per_stereo_frame" in cm:
        dp, lr = g(cm, "display_pass_ms"), g(cm, "loop_rate")
        dec, fps = g(cm, "gpu_ms_per_stereo_frame"), g(sm, "paced_fps")
        if dp and lr and dec and fps:
            head += [
                "### GPU duty",
                "",
                f"display {dp:.2f} ms x {lr:.2f}/s = **{dp*lr:.0f} ms/s**; "
                f"decode {dec:.2f} ms x {fps:.1f}/s = **{dec*fps:.0f} ms/s**; "
                f"together **{(dp*lr + dec*fps)/10:.0f} %** of one GPU.",
                "",
            ]
    body = ["## Server, every field", "", table(sbag), "",
            "## Client, every field", "", table(cbag), ""]
    if sonce.get("encoder_configuration"):
        body += ["## Encoder configuration, as the server printed it", "", "```"] + \
                sonce["encoder_configuration"] + ["```", ""]
    body += ["## Provenance", "", "```json",
             json.dumps(rec["provenance"], indent=1), "```", "",
             f"_Generated by `{rec['tool']}` on {rec['generated']}. "
             "Every figure above is parsed from the logs named; nothing is entered by hand._"]
    with open(os.path.join(a.out, a.name + ".md"), "w") as fh:
        fh.write("\n".join(head + body) + "\n")
    print(f"{a.name}: server {len(sm)} fields / {sonce.get('encode_reports_in_log',0)} reports, "
          f"client {len(cm)} fields")


if __name__ == "__main__":
    main()
