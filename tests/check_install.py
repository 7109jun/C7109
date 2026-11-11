#!/usr/bin/env python3
"""`c7c install` and platform compatibility (W16)."""
import os, shutil, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.abspath(__file__))
C7C = os.path.abspath(os.path.join(ROOT, "..", "c7c"))
W = tempfile.mkdtemp(prefix="c7inst-")
bad = 0; checks = 0
def ok(c, m):
    global bad, checks
    checks += 1
    if not c: bad += 1; print("FAIL", m)
def write(p, t):
    os.makedirs(os.path.dirname(p), exist_ok=True); open(p, "w").write(t)
def pack(out, name, ver, files, *extra):
    a = [C7C, "tool", "l7-pack", out, name, ver, *extra]
    for arc, txt in files:
        src = os.path.join(W, "content", arc); write(src, txt); a.append(f"{arc}={src}")
    subprocess.run(a, check=True, capture_output=True)
def inst(*a, env=None):
    r = subprocess.run([C7C, "install", *a], capture_output=True, text=True, env=env); return r.returncode, r.stdout + r.stderr

PFX = os.path.join(W, "prefix")
src = os.path.join(W, "Src-1.0.0.l7"); pack(src, "Src", "1.0.0", [("src/a.c7", "i64 f() { return 1; }\n")])
rc, o = inst(src, "--prefix", PFX)
dst = PFX + "/Src-1.0.0"
ok(rc == 0 and "installed Src 1.0.0" in o, o)
ok(open(dst + "/src/a.c7").read() == "i64 f() { return 1; }\n", "content extracted")
ok(os.path.isfile(dst + "/manifest.toml") and os.path.isfile(dst + "/checksums.sha256"), "manifest and checksums installed")
ok(not [n for n in os.listdir(PFX) if n.startswith(".install")], "no temp leftovers")
# existing target is never overwritten; nothing else installed either
before = open(dst + "/src/a.c7").read(); open(dst + "/src/a.c7", "w").write("changed\n")
src2 = os.path.join(W, "Other-2.0.0.l7"); pack(src2, "Other", "2.0.0", [("src/o.c7", "i64 g() { return 2; }\n")])
rc, o = inst(src2, src, "--prefix", PFX)
ok(rc != 0 and "already exists" in o and open(dst + "/src/a.c7").read() == "changed\n" and not os.path.exists(PFX + "/Other-2.0.0"), f"all-or-nothing: {o}")
# two copies of the same package in one call
rc, o = inst(src2, src2, "--prefix", os.path.join(W, "p2")); ok(rc != 0 and not os.path.exists(W + "/p2/Other-2.0.0"), o)
# tampered package rejected, nothing installed
bad_l7 = os.path.join(W, "Bad-1.0.0.l7"); b = bytearray(open(src, "rb").read()); b[len(b) // 2] ^= 0xFF; open(bad_l7, "wb").write(b)
rc, o = inst(bad_l7, "--prefix", os.path.join(W, "p3")); ok(rc != 0 and "E07" in o and not os.path.exists(W + "/p3/Bad-1.0.0"), o)
# compatibility: binary-only packages need a matching [[binary]]
def binpkg(name, os_, arch, abi):
    out = os.path.join(W, f"{name}-1.0.0.l7")
    pack(out, name, "1.0.0", [(f"lib/{os_}-{arch}/x.a", "bin\n")], "--bin", os_, arch, abi, f"lib/{os_}-{arch}/x.a"); return out
for name, os_, arch, abi, good in [("BinOk", "linux", "x86_64", "c7abi1", True), ("BinWin", "windows", "x86_64", "c7abi1", False),
                                   ("BinArm", "linux", "aarch64", "c7abi1", False), ("BinAbi", "linux", "x86_64", "c7abi9", False)]:
    p4 = os.path.join(W, "p4-" + name)
    rc, o = inst(binpkg(name, os_, arch, abi), "--prefix", p4)
    if good: ok(rc == 0 and os.path.isfile(f"{p4}/{name}-1.0.0/lib/linux-x86_64/x.a"), f"{name}: {o}")
    else: ok(rc != 0 and "E0712" in o and not os.path.exists(f"{p4}/{name}-1.0.0"), f"{name} must be E0712, never substituted: {o}")
# source package carrying a foreign binary still installs (source is portable)
mix = os.path.join(W, "Mix-1.0.0.l7")
pack(mix, "Mix", "1.0.0", [("src/m.c7", "i64 m() { return 0; }\n"), ("lib/windows-x86_64/m.lib", "x\n")], "--bin", "windows", "x86_64", "c7abi1", "lib/windows-x86_64/m.lib")
rc, o = inst(mix, "--prefix", os.path.join(W, "p5")); ok(rc == 0, o)
# project mode installs the project's own library packages
d = os.path.join(W, "proj"); write(d + "/src/l.c7", "i64 l() { return 1; }\n")
write(d + "/Cmake7109.txt", 'project "PR"\n    version "3.1.4"\ntarget library "pl":\n    sources:\n        "src/l.c7"\n')
rc, o = inst(d, "--prefix", os.path.join(W, "p6")); ok(rc != 0 and "E0704" in o, "not built yet: " + o)
subprocess.run([C7C, "pack", d], capture_output=True)
rc, o = inst(d, "--prefix", os.path.join(W, "p6")); ok(rc == 0 and os.path.isfile(W + "/p6/pl-3.1.4/src/l.c7"), o)
# default prefix comes from HOME; without HOME it is an error
home = os.path.join(W, "home"); os.makedirs(home)
rc, o = inst(src, env=dict(os.environ, HOME=home)); ok(rc == 0 and os.path.isdir(home + "/.c7/packages/Src-1.0.0"), o)
env = {k: v for k, v in os.environ.items() if k != "HOME"}
rc, o = inst(src, env=env); ok(rc != 0 and "HOME" in o, o)
shutil.rmtree(W, ignore_errors=True)
print(f"install: {checks} checks, problems={bad}")
sys.exit(1 if bad else 0)
