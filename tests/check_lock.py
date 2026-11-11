#!/usr/bin/env python3
"""Lock file Cmake7109.cr7 (W12): written only by `c7c lock`; builds read it and never change it."""
import os, re, shutil, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.abspath(__file__))
C7C = os.path.abspath(os.path.join(ROOT, "..", "c7c"))
W = tempfile.mkdtemp(prefix="c7lock-")
PRINT = 'extern "C" { void c7_print_i64(i64 v); void c7_newline(); }\n'
bad = 0; checks = 0
def ok(c, m):
    global bad, checks
    checks += 1
    if not c: bad += 1; print("FAIL", m)
def write(p, t):
    os.makedirs(os.path.dirname(p), exist_ok=True); open(p, "w").write(t)
def pkg(reg, name, ver, code):
    src = os.path.join(W, f"{name}-{ver}.c7"); write(src, code); os.makedirs(reg, exist_ok=True)
    subprocess.run([C7C, "tool", "l7-pack", os.path.join(reg, f"{name}-{ver}.l7"), name, ver, f"src/{name}.c7=" + src], check=True, capture_output=True)
def c7(cmd, d, *a):
    r = subprocess.run([C7C, cmd, d, "-j1", *a], capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr
def out(d):
    p = os.path.join(d, "build", "app")
    return subprocess.run([p], capture_output=True, text=True).stdout if os.path.exists(p) else "<none>"
def rd(p): return open(p, "rb").read() if os.path.exists(p) else None

REG = os.path.join(W, "reg")
pkg(REG, "Mathlib", "1.0.0", "i64 triple(i64 x) { return x * 3; }\n")
d = os.path.join(W, "p"); LOCK = d + "/Cmake7109.cr7"
write(d + "/src/main.c7", PRINT + "i32 main() { c7_print_i64(triple(14)); c7_newline(); return 0; }\n")
def bf(req="^1.0"):
    return f'project "P"\nregistry "{REG}"\ndependency "Mathlib" "{req}"\ntarget executable "app":\n    sources:\n        "src/main.c7"\n    depends:\n        "Mathlib"\n'
write(d + "/Cmake7109.txt", bf())

# no lock yet: build works and creates none
rc, o = c7("build", d); ok(rc == 0 and rd(LOCK) is None and out(d) == "42\n", f"build without lock: {o[-200:]}")
# lock writes the file; second run is a no-op
rc, o = c7("lock", d); ok(rc == 0 and "wrote" in o and rd(LOCK) is not None, o)
t = rd(LOCK).decode()
ok('name = "Mathlib"' in t and 'version = "1.0.0"' in t and re.search(r'sha256 = "[0-9a-f]{64}"', t), t)
rc, o = c7("lock", d); ok(rc == 0 and "up to date" in o and rd(LOCK).decode() == t, o)

# MANDATORY 1: build does not change .cr7
before = rd(LOCK); os.utime(LOCK, (1, 1))
rc, o = c7("build", d, "--no-cache"); ok(rc == 0 and rd(LOCK) == before and os.stat(LOCK).st_mtime == 1, "build must not touch the lock")
# MANDATORY 2: a newer registry version is ignored while locked
pkg(REG, "Mathlib", "1.1.0", "i64 triple(i64 x) { return x * 4; }\n")
rc, o = c7("build", d); ok(rc == 0 and out(d) == "42\n" and rd(LOCK) == before, f"locked ignores 1.1.0: {out(d)!r} {o[-200:]}")
# MANDATORY 3: re-running lock picks it up
rc, o = c7("lock", d); ok(rc == 0 and 'version = "1.1.0"' in rd(LOCK).decode(), o)
rc, o = c7("build", d); ok(rc == 0 and out(d) == "56\n", f"after relock: {out(d)!r}")
# MANDATORY 4: requirement no longer matches the lock -> E0714, no silent update
lk = rd(LOCK)
write(d + "/Cmake7109.txt", bf("=1.0.0"))
rc, o = c7("build", d); ok(rc != 0 and "E0714" in o and rd(LOCK) == lk, o)
write(d + "/Cmake7109.txt", bf("^1.0"))
# lock names a package the project no longer needs / project needs one the lock lacks
write(d + "/Cmake7109.txt", f'project "P"\ntarget executable "app":\n    sources:\n        "src/main.c7"\n    std "none"\n')
rc, o = c7("build", d); ok(rc != 0 and "E0714" in o and "Mathlib" in o, o)
write(d + "/Cmake7109.txt", bf()); os.remove(LOCK)
rc, o = c7("lock", d); lk = rd(LOCK)
write(d + "/extra.txt", "")
# locked version vanished from the registry
os.remove(REG + "/Mathlib-1.1.0.l7")
rc, o = c7("build", d); ok(rc != 0 and "E0714" in o, o)
pkg(REG, "Mathlib", "1.1.0", "i64 triple(i64 x) { return x * 4; }\n")
rc, o = c7("build", d); ok(rc == 0, o)
# tampered registry file: same version, different bytes -> E0710
pkg(REG, "Mathlib", "1.1.0", "i64 triple(i64 x) { return x * 9; }\n")
rc, o = c7("build", d); ok(rc != 0 and "E0710" in o and rd(LOCK) == lk, o)
pkg(REG, "Mathlib", "1.1.0", "i64 triple(i64 x) { return x * 4; }\n")
# a hand-edited lock is validated
for name, f in [("garbage", lambda s: "not toml ["), ("bad hash", lambda s: s.replace(re.search(r'sha256 = "([0-9a-f]+)"', s).group(1), "zz")),
                ("dup", lambda s: s + s[s.index("[[dependency]]"):]), ("no version", lambda s: s.replace("format_version", "fv"))]:
    write(LOCK, f(lk.decode()))
    rc, o = c7("build", d); ok(rc != 0 and "E07" in o, f"{name}: rc={rc} {o[-200:]}")
write(LOCK, lk.decode())
rc, o = c7("build", d); ok(rc == 0, "restored lock works again")
# lock failure leaves the old lock alone
write(d + "/Cmake7109.txt", bf("^9.0")); rc, o = c7("lock", d); ok(rc != 0 and rd(LOCK) == lk, o)
shutil.rmtree(W, ignore_errors=True)
print(f"lock: {checks} checks, problems={bad}")
sys.exit(1 if bad else 0)
