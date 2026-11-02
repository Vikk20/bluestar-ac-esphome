#!/usr/bin/env python3
"""Per-event table: every k43 tick (compressor counter) with k24 bytes, k25, room temp, setpoint, mode, fan, preset."""
import re, glob, sys, os
ac = sys.argv[1] if len(sys.argv) > 1 else "."; A = os.path.expanduser(os.environ.get("AC_LOG_DIR", "."))
st = {"room": None, "sp": None, "mode": None, "fan": None, "pwr": None, "turbo": None, "sleep": None}
MODE = {"0": "cool", "1": "fan", "2": "dry", "3": "heat", "4": "auto"}
rows = []; pend = {}
for f in sorted(glob.glob(f"{A}/{ac}/*.log")):
    day = os.path.basename(f)[5:10]
    for l in open(f, errors="replace"):
        m = re.search(r"^(\S+) .*Record ([0-9A-F]{2})=(\S+)", l)
        if not m: continue
        t, k, v = m.groups(); t = day + " " + t[:5]
        if k == "99": st["room"] = int(v, 16) / 10
        elif k == "02": st["sp"] = int(v, 16) / 10
        elif k == "01": st["pwr"] = v
        elif k == "03": st["mode"] = v
        elif k == "04": st["fan"] = v
        elif k == "07": st["turbo"] = v
        elif k == "11": st["sleep"] = v
        elif k == "24": pend["k24"] = v; pend["t"] = t
        elif k == "25": pend["k25"] = v
        elif k == "43":
            pend["k43"] = int(v, 16); pend.setdefault("t", t)
            k24 = pend.get("k24", "")
            rows.append((pend["t"], pend["k43"], int(k24[:2], 16) if k24 else None, int(k24[4:6], 16) if len(k24) >= 6 else None,
                         int(pend.get("k25", "0") or "0", 16), st["room"], st["sp"], st["fan"], st["turbo"], st["sleep"]))
            pend = {}
print("time         k43   Hz? step  k25  room   sp   fan turbo sleep")
for r in rows: print("%s %5s %4s %4s %4s %5s %5s %4s %4s %4s" % tuple("" if x is None else x for x in r))
