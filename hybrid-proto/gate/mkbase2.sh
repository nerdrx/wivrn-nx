#!/usr/bin/env bash
# Base layers, take 2: CRF instead of ABR.
#
# x265's ABR rate model never converges on a 132-frame clip -- measured, it
# lands 2.2x under a 15 Mbit target AND 1.8 dB below what CRF reaches at less
# than half the rate. CRF gives a clean monotone rate-quality curve, so the
# operating points are chosen on that curve and reported at their ACHIEVED
# bitrate. Same low-latency IPPP / CTB-64 shape as before.
set -euo pipefail
cd "$(dirname "$0")"
. /run/media/nerdrx/Lex/claude/nx-warp/scripts/cpu-discipline.sh
nx_cpu_prefix 0-7

for fx in pan panels; do
  for crf in 6 10 14 18 22 26 30; do
    o=crfbase_${fx}_${crf}
    [ -f "$o.yuv" ] && continue
    "${NICE[@]}" ffmpeg -y -v error -f rawvideo -pix_fmt yuv420p -s 1088x1088 \
      -framerate 90 -i "loop_$fx.yuv" \
      -c:v libx265 -preset medium -pix_fmt yuv420p \
      -x265-params "crf=$crf:bframes=0:ref=1:scenecut=0:rc-lookahead=0:keyint=1000:min-keyint=1000:repeat-headers=1:annexb=1:slices=1:ctu=64:log-level=error:frame-threads=1:wpp=0" \
      -f hevc "$o.hevc"
    "${NICE[@]}" ffprobe -v error -select_streams v -show_packets \
      -show_entries packet=size -of csv=p=0 "$o.hevc" > "$o.sizes"
    "${NICE[@]}" ffmpeg -y -v error -f hevc -i "$o.hevc" -pix_fmt yuv420p -f rawvideo "$o.yuv"
  done
done

python3 - <<'PY'
import numpy as np, json
W=H=1088; FS=W*H*3//2
srcs={'pan':'/run/media/nerdrx/Lex/claude/nx-scratch/inter_pan8.yuv','panels':'panels.yuv'}
out={}
for fx,sp in srcs.items():
    src=np.fromfile(sp,dtype=np.uint8).reshape(12,FS)
    out[fx]=[]
    print(f"\n{fx}: HEVC base rate-quality curve (window 88..99)")
    print(f"  {'CRF':>4} {'B/frame':>9} {'Mbit/s@90':>10} {'PSNR-Y':>8}")
    for crf in (6,10,14,18,22,26,30):
        o=f"crfbase_{fx}_{crf}"
        s=[int(x) for x in open(o+'.sizes') if x.strip()]
        b=float(np.mean(s[88:100]))
        tot=0.0
        for k,f in enumerate(range(88,100)):
            d=np.fromfile(o+'.yuv',dtype=np.uint8,count=FS,offset=f*FS)[:W*H].astype(np.float64)
            tot+=((src[k][:W*H].astype(np.float64)-d)**2).mean()
        p=10*np.log10(255*255/(tot/12))
        out[fx].append({'crf':crf,'bytes':b,'mbit':b*8*90/1e6,'psnr':p})
        print(f"  {crf:>4} {b:>9.0f} {b*8*90/1e6:>10.2f} {p:>8.2f}")
json.dump(out,open('crfcurve.json','w'),indent=1)
PY
