#!/usr/bin/env python3
"""Verifies every code example in docs/*.md against the real compiler (run from the project root, after `make`).

Fence kinds (the info string of a ``` block):
  c7 run          compile, run; the NEXT ```text block is the expected stdout (exact)
  c7 run rc=N     same, expected exit status N (default 0)
  c7 check        must pass `c7c check`
  c7 error E0xxx  must be rejected with exactly that diagnostic code
  c7 test         `c7c test` must report no failure
  buildfile       must parse as a Cmake7109.txt (`c7c tool buildfile-json`)
  toml            must parse as TOML (`c7c tool toml-json`)
  c7              illustration only (not compiled)
  sh / text / other  never checked
Exit status 0 only if every checked example behaves as documented."""
import glob, os, re, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
C7C = os.path.join(ROOT, "c7c")
W = tempfile.mkdtemp(prefix="c7doc-")
FENCE = re.compile(r"^```(.*)$")
bad = 0; checked = 0

def fail(f, line, msg):
    global bad
    bad += 1
    print(f"FAIL {f}:{line}: {msg}")

def blocks(path):
    out = []; cur = None
    for n, l in enumerate(open(path, encoding="utf-8").read().split("\n"), 1):
        m = FENCE.match(l)
        if cur is None:
            if m: cur = [m.group(1).strip(), n, []]
        elif m and m.group(1).strip() == "":
            out.append(tuple(cur)); cur = None
        else:
            cur[2].append(l)
    return out

for path in sorted(glob.glob(os.path.join(ROOT, "docs", "*.md"))):
    bl = blocks(path); name = os.path.relpath(path, ROOT)
    for i, (info, line, body) in enumerate(bl):
        words = info.split()
        if words and words[0] in ("buildfile", "toml"):
            checked += 1
            f = os.path.join(W, f"ex{line}.txt"); open(f, "w", encoding="utf-8").write("\n".join(body) + "\n")
            r = subprocess.run([C7C, "tool", "buildfile-json" if words[0] == "buildfile" else "toml-json", f], capture_output=True, text=True, cwd=ROOT)
            if r.returncode != 0: fail(name, line, f"{words[0]} does not parse: " + (r.stdout + r.stderr)[:300])
            continue
        if not words or words[0] != "c7" or len(words) < 2: continue
        kind = words[1]
        src = os.path.join(W, f"ex{line}.c7"); open(src, "w", encoding="utf-8").write("\n".join(body) + "\n")
        checked += 1
        if kind == "run":
            rc = 0
            for w in words[2:]:
                if w.startswith("rc="): rc = int(w[3:])
            exe = os.path.join(W, f"ex{line}")
            r = subprocess.run([C7C, "build", src, "-o", exe], capture_output=True, text=True, cwd=ROOT)
            if r.returncode != 0: fail(name, line, "does not compile: " + r.stderr[:300]); continue
            p = subprocess.run([exe], capture_output=True, text=True)
            exp = None
            if i + 1 < len(bl) and bl[i + 1][0] == "text": exp = ("\n".join(bl[i + 1][2]) + "\n") if bl[i + 1][2] else ""
            if exp is None: fail(name, line, "`c7 run` must be followed by a ```text block with the output"); continue
            if p.stdout != exp: fail(name, line, f"output differs\n--- expected\n{exp}--- got\n{p.stdout}")
            got = p.returncode if p.returncode >= 0 else 128 - p.returncode
            if got != rc: fail(name, line, f"exit status {got}, documented {rc}")
        elif kind == "check":
            r = subprocess.run([C7C, "check", src], capture_output=True, text=True, cwd=ROOT)
            if r.returncode != 0: fail(name, line, "check failed: " + r.stderr[:300])
        elif kind == "error":
            code = words[2] if len(words) > 2 else ""
            r = subprocess.run([C7C, "check", src], capture_output=True, text=True, cwd=ROOT)
            if r.returncode == 0 or code not in r.stderr: fail(name, line, f"expected {code}, got rc={r.returncode}: {r.stderr[:300]}")
        elif kind == "test":
            r = subprocess.run([C7C, "test", src], capture_output=True, text=True, cwd=ROOT)
            if r.returncode != 0: fail(name, line, "tests failed: " + (r.stdout + r.stderr)[:300])
        else:
            fail(name, line, f"unknown fence kind `{kind}`")
print(f"doccheck: {checked} examples checked, problems={bad}")
sys.exit(1 if bad else 0)
