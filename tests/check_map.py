#!/usr/bin/env python3
"""`.mp7` map files (W13). Oracle: the independent `nm`/`readelf`/`sha256sum` tools and hand-written expectations."""
import hashlib, os, re, shutil, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.abspath(__file__))
C7C = os.path.abspath(os.path.join(ROOT, "..", "c7c"))
W = tempfile.mkdtemp(prefix="c7map-")
bad = 0; checks = 0
def ok(c, m):
    global bad, checks
    checks += 1
    if not c: bad += 1; print("FAIL", m)
def write(p, t):
    os.makedirs(os.path.dirname(p), exist_ok=True); open(p, "w").write(t)
def build(d, *a):
    r = subprocess.run([C7C, "build", d, "-j1", *a], capture_output=True, text=True); return r.returncode, r.stdout + r.stderr
def rd(p): return open(p, "rb").read() if os.path.exists(p) else None

CODE = 'extern "C" { void c7_print_i64(i64 v); }\ni64 square(i64 x) { return x * x; }\ni32 main() { c7_print_i64(square(3)); return 0; }\n'
d = os.path.join(W, "p"); MAP = d + "/build/app.mp7"; EXE = d + "/build/app"
BF = 'project "Demo"\ntarget executable "app":\n    sources:\n        "src/main.c7"\n'
write(d + "/src/main.c7", CODE); write(d + "/Cmake7109.txt", BF)
rc, o = build(d); ok(rc == 0 and rd(MAP) is not None, o)
m = rd(MAP).decode()
ok(m.startswith("C7109 MAP FILE\nFormat: 1\nProject: Demo\nTarget: app (executable)\nPlatform: x86_64-linux (host: x86_64-linux)\n"), m[:200])
ok("\r" not in m and m.endswith("\n"), "LF only")
m.encode("utf-8")
ok("Configuration: (none) (mode=release, optimization=O0)" in m, m[:300])
src = CODE.encode()
ok(f"src/main.c7  sha256={hashlib.sha256(src).hexdigest()}  bytes={len(src)}" in m, "[Inputs] line")
exe = rd(EXE)
ok(f"build/app  bytes={len(exe)}  sha256={hashlib.sha256(exe).hexdigest()}" in m, "[Outputs] line")
for sec in ("[Inputs]", "[Libraries]", "[Outputs]", "[Sections]", "[Symbols]"): ok(f"\n{sec}\n" in m, sec)
# symbols agree with nm (independent oracle): every listed address/size/type matches an nm line
nm = {}
for l in subprocess.run(["nm", "-n", "-S", "--defined-only", EXE], capture_output=True, text=True).stdout.splitlines():
    t = l.split()
    if len(t) == 4: nm[t[3]] = (int(t[0], 16), int(t[1], 16), t[2])
syms = m.split("[Symbols]\n")[1]
lines = [l for l in syms.splitlines() if l and not l.startswith(";")]
got = {}
for l in lines:
    mm = re.fullmatch(r"0x([0-9a-f]{16}) (\d+) (\w) (.+)", l); ok(mm is not None, f"symbol line shape: {l}")
    if mm: got[mm.group(4)] = (int(mm.group(1), 16), int(mm.group(2)), mm.group(3))
ok(got.get("square(i64)") == nm.get("_C7$square$$i64") and got.get("square(i64)") is not None, f"square vs nm: {got.get('square(i64)')} {nm.get('_C7$square$$i64')}")
ok(got.get("main") == nm.get("main") and "c7_print_i64" in got, "main and runtime symbol")
ok(not any(k in got for k in ("puts", "_start", "printf")), "libc/crt symbols not listed")
ok(any(k.startswith("std::") for k in got), "demangled std symbols")
ok(all(a <= b for (a, _, _), (b, _, _) in zip(list(got.values()), list(got.values())[1:])), "sorted by address")
# sections agree with readelf
re_out = subprocess.run(["readelf", "-SW", EXE], capture_output=True, text=True).stdout
ok(re.search(r"\.text\s+PROGBITS\s+([0-9a-f]+)\s+[0-9a-f]+\s+([0-9a-f]+)", re_out) is not None, "readelf has .text")
tx = re.search(r"\.text\s+PROGBITS\s+([0-9a-f]+)\s+[0-9a-f]+\s+([0-9a-f]+)", re_out)
ok(f".text addr=0x{int(tx.group(1),16):016x} size={int(tx.group(2),16)}\n" in m, ".text line")
ok("addr=0x0000000000000000" not in m, "unmapped sections not listed")
ok("Name addr" not in m and "[Nr]" not in m, "no readelf header junk")

# determinism + cache hit rewrites the map identically
first = rd(MAP)
rc, o = build(d, "--no-cache"); ok(rc == 0 and rd(MAP) == first, "rebuild gives byte-identical map")
rc, o = build(d); ok("cached" in o and rd(MAP) == first, "cache hit recreates the map")
os.remove(MAP); rc, o = build(d); ok("cached" in o and rd(MAP) == first, "cached build restores a deleted map")

# policy: stale map is removed when the build fails, nothing is left behind
write(d + "/src/main.c7", "i32 main() { return nope; }\n")
rc, o = build(d); ok(rc != 0 and rd(MAP) is None and not os.path.exists(EXE), f"failed build leaves no map: {o[-200:]}")
write(d + "/src/main.c7", CODE)
rc, o = build(d); ok(rc == 0 and rd(MAP) == first, "good again")
# library targets get no map; libraries listed in the executable's map
write(d + "/src/lib.c7", "i64 triple(i64 x) { return x * 3; }\n")
write(d + "/Cmake7109.txt", 'project "Demo"\n    version "2.0.0"\ntarget library "lib":\n    sources:\n        "src/lib.c7"\n' + BF.split("\n", 1)[1].replace('"src/main.c7"\n', '"src/main.c7"\n    depends:\n        "lib"\n'))
rc, o = build(d); ok(rc == 0, o)
m2 = rd(MAP).decode()
ok(re.search(r"\[Libraries\]\nlib 2\.0\.0  sha256=[0-9a-f]{64}\n", m2) is not None, "library listed: " + m2.split("[Outputs]")[0][-200:])
ok(not any(f.endswith(".mp7") and "lib" in f for f in os.listdir(d + "/build")), "no map for a library target")
# missing nm/readelf: explicit fallback text
bindir = os.path.join(W, "restricted-path"); os.makedirs(bindir)
for n in os.listdir("/usr/bin"):
    if n not in ("nm", "readelf", "x86_64-linux-gnu-nm", "x86_64-linux-gnu-readelf"):
        try: os.symlink("/usr/bin/" + n, os.path.join(bindir, n))
        except OSError: pass
env = dict(os.environ, PATH=bindir)
r = subprocess.run([C7C, "build", d, "-j1", "--no-cache"], env=env, capture_output=True, text=True)
mt = rd(MAP)
ok(r.returncode == 0 and mt is not None, f"build works without nm/readelf: {r.stderr[-200:]}")
if mt:
    t = mt.decode()
    ok("[Sections]\n; unavailable: readelf" in t and "; unavailable: nm" in t, "explicit fallback text")
    ok("[Inputs]\nsrc/main.c7" in t and "[Outputs]\nbuild/app  bytes=" in t, "rest of the map still written")
shutil.rmtree(W, ignore_errors=True)
print(f"map: {checks} checks, problems={bad}")
sys.exit(1 if bad else 0)
