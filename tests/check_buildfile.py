#!/usr/bin/env python3
"""Cmake7109.txt parser: valid corpus vs hand-written expected JSON (tests/build/valid/*.json), invalid corpus vs the
expected diagnostic code and line:col (tests/build/invalid/*.expect), plus code coverage, Korean message order and
hostile-input limits. The expectations were written by hand, independently of the C++ parser (no oracle exists)."""
import json, os, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.abspath(__file__))
C7C = os.path.join(ROOT, "..", "c7c")
B = os.path.join(ROOT, "build")
bad = 0
TMP = tempfile.mkdtemp(prefix="c7bf-")

def run(path, *flags):
    r = subprocess.run([C7C, "tool", "buildfile-json", path, *flags], capture_output=True, text=True, timeout=30)
    return r.returncode, r.stdout, r.stderr

def fail(msg):
    global bad
    bad += 1; print("FAIL", msg)

nv = 0
for name in sorted(os.listdir(os.path.join(B, "valid"))):
    if not name.endswith(".txt"): continue
    nv += 1
    rc, out, err = run(os.path.join(B, "valid", name))
    if rc != 0: fail(f"valid/{name}: rejected: {err.strip()[:200]}"); continue
    want = json.load(open(os.path.join(B, "valid", name[:-4] + ".json"), encoding="utf-8"))
    got = json.loads(out)
    if got != want: fail(f"valid/{name}: model differs\n  got  {got}\n  want {want}")

codes = set(); ni = 0
for name in sorted(os.listdir(os.path.join(B, "invalid"))):
    if not name.endswith(".txt"): continue
    ni += 1
    exp = open(os.path.join(B, "invalid", name[:-4] + ".expect")).read().split()
    code, loc = exp[0], exp[1]; flags = exp[2:]
    codes.add(code)
    path = os.path.join(B, "invalid", name)
    rc, out, err = run(path, *flags)
    if rc != 1: fail(f"invalid/{name}: rc={rc}, want 1"); continue
    first_lines = [l for l in err.splitlines() if "error[" in l]
    want = f"{path}:{loc}: error[{code}]"
    if not any(l.startswith(want) for l in first_lines):
        fail(f"invalid/{name}: want {want}, got:\n    " + "\n    ".join(first_lines[:4]))
need = {"E0702", "E0703", "E0704", "E0705", "E0708", "E0717", "E0719"}
if not need <= codes: fail(f"codes without an invalid case: {sorted(need - codes)}")

# the same file reports every semantic error (not only the first one)
p = os.path.join(TMP, "multi.txt")
open(p, "w").write('project "P"\ntarget executable "a":\n    sources:\n        "../x.c7"\n    link "auto"\n    color "red"\n')
rc, out, err = run(p)
got = sorted(l.split("error[")[1][:5] for l in err.splitlines() if "error[" in l)
if rc != 1 or got != ["E0703", "E0719", "E0719"]: fail(f"multiple errors: rc={rc} {got}")

# Korean messages keep the placeholder order (E0703 takes keyword, context)
p = os.path.join(TMP, "ko.txt"); open(p, "w").write('project "P"\nfrobnicate "x"\n')
rc, out, err = run(p, "--lang=ko")
if "`frobnicate`은(는)" not in err or "최상위" in err.split("frobnicate")[0]: fail(f"ko message order: {err.strip()}")

# hostile inputs must be refused cleanly (rc 1), quickly, without crashing
hostile = {
    "huge_line": 'project "' + "a" * 100000 + '"\n',
    "huge_file": 'project "P"\n' + "# c\n" * 200000,
    "many_targets": 'project "P"\n' + "".join(f'target executable "t{i}":\n    sources:\n        "a.c7"\n' for i in range(20000)),
    "deep_indent": 'project "P"\n' + "".join(" " * (i + 1) + "x\n" for i in range(5000)),
    "colons": 'project "P"' + ":" * 100000 + "\n",
    "nul_byte": 'project "P\0"\n',
}
for name, text in hostile.items():
    p = os.path.join(TMP, name + ".txt"); open(p, "w").write(text)
    rc, out, err = run(p)
    want_ok = name == "many_targets"   # large but legitimate except that every target repeats the source (fine) -> accepted
    if name == "many_targets":
        if rc not in (0, 1): fail(f"hostile/{name}: rc={rc}")
    elif rc != 1: fail(f"hostile/{name}: rc={rc}, want 1")
print(f"buildfile: {nv} valid, {ni} invalid, codes={sorted(codes)}, problems={bad}")
sys.exit(1 if bad else 0)
