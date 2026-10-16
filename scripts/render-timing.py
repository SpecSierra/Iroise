#!/usr/bin/env python3
"""Summarises the interface's frame timings from a phone log (win.log).

On the phone: QSG_RENDER_TIMING=1 QT_MESSAGE_PATTERN="%{time process} %{message}" sh /tmp/ladybird/start-browser.sh URL
then copy /tmp/ladybird/win.log here.  usage: render-timing.py win.log [from-seconds, default 5]
The interface draws only when an engine frame arrives, so its frame rate is the page's.
"""
import re, sys, statistics as st
t0 = float(sys.argv[2]) if len(sys.argv) > 2 else 5.0
R = re.compile(r'^\s*([\d.]+) Frame rendered .* in (\d+)ms, sync=(\d+), render=(\d+), swap=(\d+)')
G = re.compile(r'^\s*([\d.]+) Frame prepared .* polish=(\d+), lock=(\d+), blockedForSync=(\d+), animations=(\d+)')
rs, gs = [], []
for l in open(sys.argv[1], errors='replace'):
    m = R.match(l)
    if m and float(m[1]) >= t0: rs.append(tuple(map(float, m.groups())))
    m = G.match(l)
    if m and float(m[1]) >= t0: gs.append(tuple(map(float, m.groups())))
if len(rs) < 2:
    sys.exit("no frame timings after %.1f s: was QSG_RENDER_TIMING=1 set?" % t0)
ts = [r[0] for r in rs]; iv = [(b - a) * 1000 for a, b in zip(ts, ts[1:])]
print("frames", len(rs), "span %.1fs" % (ts[-1] - ts[0]), "fps %.1f" % (len(rs) / (ts[-1] - ts[0])))
for name, i in (("total", 1), ("sync", 2), ("render", 3), ("swap", 4)):
    v = [r[i] for r in rs]; print("%-6s mean %.1f  p95 %d  max %d" % (name, st.mean(v), sorted(v)[int(len(v) * .95)], max(v)))
print("intervals >20ms:", sum(1 for x in iv if x > 20), " >40ms:", sum(1 for x in iv if x > 40))
print("gui polish max", max((g[1] for g in gs), default=0), " anim max", max((g[4] for g in gs), default=0), " polish>=8:", sum(1 for g in gs if g[1] >= 8))
big = sorted(rs, key=lambda r: -r[1])[:5]; print("worst:", [(r[0], r[1:]) for r in big])
