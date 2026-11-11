#!/usr/bin/env python3
"""Runs the programs built by gen_windows_suite.py (on Windows) and compares stdout, exit status and stderr.
usage: run_windows_suite.py SUITEDIR
Set C7_RUNNER=wine (or any command prefix) to run the programs through an emulator on a non-Windows host."""
import json, os, shlex, subprocess, sys

RUNNER = shlex.split(os.environ.get("C7_RUNNER", ""))

d = sys.argv[1]
cases = json.load(open(os.path.join(d, "manifest.json")))
bad = 0
if RUNNER:  # an emulator's first start creates its prefix and can fail the first program: warm it up, ignoring the result
    subprocess.run([*RUNNER, os.path.join(d, cases[0]["exe"])], capture_output=True, timeout=300)
def norm(b): return b.replace(b"\r\n", b"\n").rstrip(b"\n")
for c in cases:
    try:
        p = subprocess.run([*RUNNER, os.path.join(d, c["exe"])], capture_output=True, timeout=60)
    except Exception as e:
        bad += 1; print("FAIL", c["name"], "could not run:", e); continue
    exp = c["expected"].encode("utf-8", "surrogateescape")
    why = []
    if norm(p.stdout) != norm(exp): why.append(f"stdout differs\n--- expected\n{exp!r}\n--- got\n{p.stdout!r}")
    if p.returncode != c["rc"]: why.append(f"exit status {p.returncode}, want {c['rc']}; stderr={p.stderr[:300]!r}")
    s = c.get("stderr_contains")
    if s and s.encode("utf-8", "surrogateescape") not in p.stderr: why.append(f"stderr lacks {s!r}: {p.stderr!r}")
    if why: bad += 1; print("FAIL", c["name"], *why)
print(f"windows suite: {len(cases)} programs run, problems={bad}")
sys.exit(1 if bad else 0)
