#!/usr/bin/env python3
# usage: shot.py <out.png> <wait-seconds> <exe> [args...]  (run inside xvfb-run)
import subprocess, sys, time, os
out, wait, cmd = sys.argv[1], float(sys.argv[2]), sys.argv[3:]
p = subprocess.Popen(["wine"] + cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(wait)
subprocess.run(["import", "-window", "root", out])
p.kill(); subprocess.run(["wineserver", "-k"])
