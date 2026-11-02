#!/usr/bin/env python3
"""Minimal syslog receiver for the ESP's `syslog:` output: appends every line to <dir>/<YYYY-MM-DD>.log.
Usage: ac_syslog.py [listen_port=5514] [log_dir=./ac-logs]  (run as a systemd service for long-term logging)"""
import datetime, os, re, socket, sys
port = int(sys.argv[1]) if len(sys.argv) > 1 else 5514
d = sys.argv[2] if len(sys.argv) > 2 else "ac-logs"
os.makedirs(d, exist_ok=True)
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind(("0.0.0.0", port))
day, f = None, None
while True:
    data, (ip, _) = s.recvfrom(4096)
    now = datetime.datetime.now()
    if now.date() != day:
        day = now.date(); f and f.close()
        f = open(os.path.join(d, "%s.log" % day), "a", buffering=1)
    f.write("%s %s %s\n" % (now.strftime("%H:%M:%S.%f")[:-3], ip, re.sub(r"^<\d+>", "", data.decode(errors="replace")).rstrip()))
