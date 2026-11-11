#!/usr/bin/env python3
"""Cross-check c7c's TOML parser and serializer against Python's tomllib (the reference implementation).
valid/*.toml   -> parse result and serialize->parse result must equal tomllib's (types included)
invalid/*.toml -> both must reject
Files containing datetimes only have their accept/reject behaviour compared."""
import json, os, subprocess, sys, tomllib, datetime

ROOT = os.path.dirname(os.path.abspath(__file__))
C7C = os.path.join(ROOT, "..", "c7c")

def has_datetime(o):
    if isinstance(o, (datetime.datetime, datetime.date, datetime.time)): return True
    if isinstance(o, dict): return any(has_datetime(v) for v in o.values())
    if isinstance(o, list): return any(has_datetime(v) for v in o)
    return False

def same(a, b):
    if type(a) is not type(b): return False
    if isinstance(a, dict): return a.keys() == b.keys() and all(same(a[k], b[k]) for k in a)
    if isinstance(a, list): return len(a) == len(b) and all(same(x, y) for x, y in zip(a, b))
    if isinstance(a, float):
        if a != a: return b != b
        return a == b and (str(a) == str(b))   # distinguishes -0.0 from 0.0
    return a == b

def ours(sub, path):
    r = subprocess.run([C7C, "tool", sub, path], capture_output=True, text=True)
    return r.returncode, r.stdout

bad = 0
for name in sorted(os.listdir(os.path.join(ROOT, "toml", "valid"))):
    path = os.path.join(ROOT, "toml", "valid", name)
    ref = tomllib.loads(open(path, "rb").read().decode("utf-8"))
    for sub in ("toml-json", "toml-roundtrip"):
        rc, out = ours(sub, path)
        if rc != 0:
            print(f"FAIL {name} [{sub}]: rejected a valid file (rc={rc})"); bad += 1; continue
        if has_datetime(ref): continue
        got = json.loads(out)
        if not same(got, ref):
            print(f"FAIL {name} [{sub}]: result differs from tomllib\n  ours: {got}\n  ref:  {ref}"); bad += 1
for name in sorted(os.listdir(os.path.join(ROOT, "toml", "invalid"))):
    path = os.path.join(ROOT, "toml", "invalid", name)
    try:
        tomllib.loads(open(path, "rb").read().decode("utf-8")); ref_ok = True
    except Exception:
        ref_ok = False
    rc, _ = ours("toml-json", path)
    if ref_ok:
        print(f"NOTE {name}: tomllib accepts this file, so it is not a valid negative case"); bad += 1
    elif rc == 0:
        print(f"FAIL {name}: accepted an invalid file"); bad += 1
# the spec requires 64-bit signed integers and rejects the rest; tomllib is more lenient, so only we must reject these
for name in sorted(os.listdir(os.path.join(ROOT, "toml", "stricter"))):
    rc, _ = ours("toml-json", os.path.join(ROOT, "toml", "stricter", name))
    if rc == 0:
        print(f"FAIL stricter/{name}: accepted a value outside the TOML spec"); bad += 1
n_valid = len(os.listdir(os.path.join(ROOT, "toml", "valid"))); n_inv = len(os.listdir(os.path.join(ROOT, "toml", "invalid")))
n_strict = len(os.listdir(os.path.join(ROOT, "toml", "stricter")))
print(f"toml: {n_valid} valid, {n_inv} invalid, {n_strict} spec-stricter than tomllib, problems={bad}")
sys.exit(1 if bad else 0)
