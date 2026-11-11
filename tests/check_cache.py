#!/usr/bin/env python3
"""Build cache (W9). Every invalidation scenario is an independent test on a fresh project and cache; a rebuild is
detected from the `--verbose` event log (`start X` = a child compiler ran, `cached X` = served from the cache)."""
import hashlib, os, shutil, subprocess, sys, tempfile, threading

ROOT = os.path.dirname(os.path.abspath(__file__))
C7C = os.path.abspath(os.path.join(ROOT, "..", "c7c"))
W = tempfile.mkdtemp(prefix="c7cache-")
bad = 0; checks = 0
PRINT = 'extern "C" { void c7_print_i64(i64 v); void c7_newline(); }\n'

def fail(m):
    global bad
    bad += 1; print("FAIL", m)
def ok(c, m):
    global checks
    checks += 1
    if not c: fail(m)

def write(d, files):
    for f, t in files.items():
        p = os.path.join(d, f); os.makedirs(os.path.dirname(p), exist_ok=True)
        open(p, "w").write(t)

BUILD = '''project "P":
    version "1.0.0"
target library "lib":
    sources:
        "src/lib.c7"
target executable "app":
    sources:
        "src/main.c7"
    include_dirs:
        "inc"
    depends:
        "lib"
target executable "other":
    sources:
        "src/other.c7"
    include_dirs:
        "inc"
'''
FILES = {"Cmake7109.txt": BUILD,
         "src/lib.c7": "i64 triple(i64 x) { return x * 3; }\n",
         "src/main.c7": '#include "val.h7"\n' + PRINT + "i32 main() { c7_print_i64(triple(VALUE)); c7_newline(); return 0; }\n",
         "src/other.c7": '#include "unrelated.h7"\ni32 main() { return OTHER; }\n',
         "inc/val.h7": "#define VALUE 14\n", "inc/unrelated.h7": "#define OTHER 0\n"}

def project(name, files=None):
    d = os.path.join(W, name); shutil.rmtree(d, ignore_errors=True)
    write(d, files or FILES)
    return d

def build(d, *a, c7c=C7C):
    r = subprocess.run([c7c, "build", d, "--verbose", "-j1", *a], capture_output=True, text=True, timeout=120)
    ev = [l[6:] for l in r.stderr.splitlines() if l.startswith("[c7c] ") and not l.startswith("[c7c]   ")]
    ran = sorted(e[6:] for e in ev if e.startswith("start "))
    hit = sorted(e[7:] for e in ev if e.startswith("cached "))
    return r.returncode, ran, hit, r.stderr

def expect(d, ran, hit, label, *a, c7c=C7C):
    rc, r, h, err = build(d, *a, c7c=c7c)
    ok(rc == 0 and r == sorted(ran) and h == sorted(hit), f"{label}: rc={rc} ran={r} cached={h}; want ran={sorted(ran)} cached={sorted(hit)} {err[-200:] if rc else ''}")

ALL = ["lib", "app", "other"]
def cold_then(name, files=None):
    d = project(name, files)
    expect(d, ALL, [], f"{name}: cold build")
    return d

# 0. unchanged -> everything from the cache, no child started, bytes identical, still runs
d = cold_then("warm")
h1 = {n: hashlib.sha256(open(os.path.join(d, "build", n), "rb").read()).hexdigest() for n in ("app", "other")}
shutil.rmtree(os.path.join(d, "build"))
expect(d, [], ALL, "warm: second build")
h2 = {n: hashlib.sha256(open(os.path.join(d, "build", n), "rb").read()).hexdigest() for n in ("app", "other")}
ok(h1 == h2, "cache hit must reproduce the executable byte for byte")
ok(subprocess.run([os.path.join(d, "build", "app")], capture_output=True, text=True).stdout == "42\n", "cached executable runs")
ok(os.access(os.path.join(d, "build", "app"), os.X_OK), "cached executable keeps its permissions")
ok(subprocess.run([C7C, "verify", d], capture_output=True).returncode == 0, "cached library package verifies")

# 1. source content change
d = cold_then("src_change")
write(d, {"src/other.c7": '#include "unrelated.h7"\ni32 main() { return OTHER + 0; }\n'})
expect(d, ["other"], ["lib", "app"], "source change rebuilds only that target")
d = cold_then("lib_src_change")
write(d, {"src/lib.c7": "i64 triple(i64 x) { return x * 3 + 0; }\n"})
expect(d, ["lib", "app"], ["other"], "library source change rebuilds the library and its users")

# 2. source added / removed (edit the build file)
d = cold_then("src_added")
write(d, {"src/extra.c7": "i64 extra() { return 1; }\n"})
write(d, {"Cmake7109.txt": BUILD.replace('        "src/other.c7"\n', '        "src/other.c7"\n        "src/extra.c7"\n')})
expect(d, ["other"], ["lib", "app"], "source added")
write(d, {"Cmake7109.txt": BUILD})
expect(d, [], ALL, "source removed again -> the original entry is still valid")

# 3. #included header (direct mode)
d = cold_then("header")
write(d, {"inc/val.h7": "#define VALUE 15\n"})
expect(d, ["app"], ["lib", "other"], "included header change rebuilds exactly the target that reads it")
ok(subprocess.run([os.path.join(d, "build", "app")], capture_output=True, text=True).stdout == "45\n", "rebuilt app uses the new header")
d = cold_then("header_touch")
os.utime(os.path.join(d, "inc", "val.h7"), (1, 1))
expect(d, [], ALL, "touching a header without changing it is still a hit")
d = cold_then("header_unrelated")
write(d, {"inc/unrelated.h7": "#define OTHER 0 \n"})
expect(d, ["other"], ["lib", "app"], "a header only `other` includes")

# 4. options
d = cold_then("opt_flag")
expect(d, ALL, [], "-O2 changes every target's options", "-O2")
expect(d, [], ALL, "same options again -> hit", "-O2")
d = cold_then("opt_define")
write(d, {"Cmake7109.txt": BUILD.replace('    depends:\n        "lib"\n', '    depends:\n        "lib"\n    defines:\n        "X=1"\n')})
expect(d, ["app"], ["lib", "other"], "defines of one target")

# 5. dependencies: change in the library's compiled form (an option of the library) rebuilds users
d = cold_then("lib_opt")
write(d, {"Cmake7109.txt": BUILD.replace('target library "lib":\n    sources:\n        "src/lib.c7"\n', 'target library "lib":\n    sources:\n        "src/lib.c7"\n    optimization "O2"\n')})
expect(d, ["lib"], ["app", "other"], "library option change: the library is rebuilt, but its package is byte-identical so its users stay cached")

# 6-9. compiler identity: a private copy of the installation layout, changed one piece at a time
def install(name):
    d = os.path.join(W, name); shutil.rmtree(d, ignore_errors=True)
    os.makedirs(os.path.join(d, "runtime"))
    shutil.copy(C7C, d + "/c7c"); shutil.copy(os.path.join(ROOT, "..", "runtime", "c7rt.c"), d + "/runtime/")
    shutil.copy(os.path.join(ROOT, "..", "std.l7"), d + "/std.l7")
    return d, d + "/c7c"
def with_install(name, mutate, label):
    inst, exe = install(name)
    d = project(name + "_p")
    expect(d, ALL, [], f"{label}: cold", c7c=exe)
    expect(d, [], ALL, f"{label}: warm", c7c=exe)
    mutate(inst)
    expect(d, ALL, [], f"{label}: rebuilt after the change", c7c=exe)
    expect(d, [], ALL, f"{label}: hits again afterwards", c7c=exe)
with_install("fp_bin", lambda i: open(i + "/c7c", "ab").write(b"\0"), "compiler binary changed")
with_install("fp_rt", lambda i: open(i + "/runtime/c7rt.c", "a").write("\n/* changed */\n"), "runtime changed")
def new_std(i):
    subprocess.run([i + "/c7c", "tool", "l7-pack", i + "/std.l7", "std", "1.0.1", "src/m.c7=" + os.path.join(ROOT, "stdl7", "marker.c7")], check=True)
with_install("fp_std", new_std, "std.l7 changed")

# 9b. platform: adding an explicit platform property changes the options of that target
d = cold_then("platform")
write(d, {"Cmake7109.txt": BUILD.replace('target executable "other":\n    sources:\n        "src/other.c7"\n', 'target executable "other":\n    sources:\n        "src/other.c7"\n    platform "x86_64-linux"\n')})
expect(d, ["other"], ["lib", "app"], "platform property")

# 10. damaged entries are ignored and repaired
def entries(d): return [os.path.join(d, ".c7cache", e) for e in sorted(os.listdir(os.path.join(d, ".c7cache"))) if not e.startswith(".")]
for what in ("artifact", "meta", "deps.txt"):
    d = cold_then("damage_" + what)
    for e in entries(d):
        p = os.path.join(e, what)
        if what == "artifact": data = bytearray(open(p, "rb").read()); data[len(data) // 2] ^= 0xFF; open(p, "wb").write(data)
        elif what == "meta": open(p, "w").write("garbage\n")
        else: os.remove(p)
    expect(d, ALL, [], f"damaged {what}: all entries ignored and rebuilt")
    expect(d, [], ALL, f"damaged {what}: repaired")
d = cold_then("damage_dir")
for e in entries(d): shutil.rmtree(e)
expect(d, ALL, [], "deleted entries")
d = cold_then("damage_junk")
open(os.path.join(d, ".c7cache", "not-an-entry"), "w").write("x")
for e in entries(d)[:1]: shutil.rmtree(e); open(e, "w").write("a file where a directory should be")
rc, ran, hit, err = build(d)
ok(rc == 0, f"a file where an entry directory should be: rc={rc} {err[-200:]}")

# 11. --no-cache, --cache-dir, clean
d = project("nocache")
rc, ran, hit, _ = build(d, "--no-cache"); ok(ran == sorted(ALL) and not os.path.exists(os.path.join(d, ".c7cache")), "--no-cache writes nothing")
rc, ran, hit, _ = build(d, "--no-cache"); ok(ran == sorted(ALL) and hit == [], "--no-cache never reads")
d = project("cachedir"); cdir = os.path.join(W, "elsewhere")
rc, ran, hit, _ = build(d, "--cache-dir", cdir); ok(ran == sorted(ALL) and os.path.isdir(cdir) and not os.path.exists(os.path.join(d, ".c7cache")), "--cache-dir location")
rc, ran, hit, _ = build(d, "--cache-dir", cdir); ok(hit == sorted(ALL), "--cache-dir is reused")
d = cold_then("clean")
subprocess.run([C7C, "clean", d], capture_output=True)
expect(d, ALL, [], "clean removes the cache")
# a failed build is never stored
d = project("failed")
write(d, {"src/other.c7": "i64 bad( { }\n"})
rc, ran, hit, _ = build(d); ok(rc == 1, "failing build")
write(d, {"src/other.c7": "i32 main() { return 0; }\n"})
expect(d, ["other"], ["lib", "app"], "a failed target left nothing in the cache; the good ones were kept")

# 12. concurrent builds (separate project copies, one shared cache directory)
shared = os.path.join(W, "shared")
codes = []
dirs = [project(f"concurrent{i}") for i in range(4)]
def go(d):
    for _ in range(4):
        shutil.rmtree(os.path.join(d, "build"), ignore_errors=True)
        r = subprocess.run([C7C, "build", d, "--cache-dir", shared, "-j3"], capture_output=True, text=True)
        codes.append(r.returncode)
        if r.returncode: print(r.stderr[-300:])
ths = [threading.Thread(target=go, args=(d,)) for d in dirs]
[t.start() for t in ths]; [t.join() for t in ths]
ok(all(c == 0 for c in codes) and len(codes) == 16, f"concurrent builds with a shared cache: {codes}")
for d in dirs:
    shutil.rmtree(os.path.join(d, "build"), ignore_errors=True)
    rc, ran, hit, err = build(d, "--cache-dir", shared)
    ok(rc == 0 and hit == sorted(ALL), f"the shared cache is intact afterwards: ran={ran} hit={hit}")
    ok(subprocess.run([os.path.join(d, "build", "app")], capture_output=True, text=True).stdout == "42\n", "output after concurrent builds")
print(f"cache: {checks} checks, problems={bad}")
sys.exit(1 if bad else 0)
