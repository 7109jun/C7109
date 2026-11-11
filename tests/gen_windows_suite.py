#!/usr/bin/env python3
"""Cross-builds the run/ and dbg/ test programs for x86_64-windows (on any host with c7c + clang + lld) and writes a
manifest. tests/run_windows_suite.py then executes them on a real Windows machine (CI) and compares output.
usage: gen_windows_suite.py OUTDIR"""
import json, os, shutil, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
C7C = os.path.join(ROOT, "c7c")
out = os.path.abspath(sys.argv[1]); shutil.rmtree(out, ignore_errors=True); os.makedirs(out)
cases = []; bad = 0
VARIANTS = [("O0", []), ("O2", ["-O2"]), ("dbgchk", ["--debug", "-fbounds-check"]), ("dbgalloc", ["-fdebug-alloc"])]
def rd(p, default=None):
    return open(p, "rb").read() if os.path.exists(p) else default
def add(name, src, flags, expected, rc, err=None):
    global bad
    exe = f"{name}.exe"
    r = subprocess.run([C7C, "build", src, "--target=x86_64-windows", "-o", os.path.join(out, exe), *flags], capture_output=True, text=True, cwd=ROOT)
    if r.returncode != 0:
        bad += 1; print("BUILD FAIL", name, r.stderr[:300]); return
    c = {"name": name, "exe": exe, "rc": rc, "expected": expected.decode("utf-8", "surrogateescape")}
    if err: c["stderr_contains"] = err.decode("utf-8", "surrogateescape").strip()
    cases.append(c)
for f in sorted(os.listdir(os.path.join(ROOT, "tests/run"))):
    if not f.endswith(".c7"): continue
    n = f[:-3]; base = os.path.join(ROOT, "tests/run", n)
    rc = int((rd(base + ".rc", b"0")).decode().strip())
    for tag, flags in VARIANTS:
        add(f"run-{n}-{tag}", base + ".c7", flags, rd(base + ".expected"), rc)
for f in sorted(os.listdir(os.path.join(ROOT, "tests/dbg"))):
    if not f.endswith(".c7"): continue
    n = f[:-3]; base = os.path.join(ROOT, "tests/dbg", n)
    add(f"dbg-{n}", base + ".c7", ["-fdebug-alloc"], rd(base + ".expected"), int(rd(base + ".rc").decode().strip()), rd(base + ".err"))
json.dump(cases, open(os.path.join(out, "manifest.json"), "w"), indent=1)
print(f"windows suite: {len(cases)} programs built, build failures={bad}")
sys.exit(1 if bad else 0)
