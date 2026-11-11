#!/usr/bin/env python3
"""Project mode (W8 scheduler + W10 CLI): ordering, concurrency, failure handling, determinism, exit codes, option flow,
packages, clean. Scheduling is checked from the `--verbose` event log (start/done lines), which is emitted at the moment
a child is spawned / reaped, so "both started before either finished" holds even on a single core."""
import hashlib, os, re, shutil, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.abspath(__file__))
C7C = os.path.abspath(os.path.join(ROOT, "..", "c7c"))
W = tempfile.mkdtemp(prefix="c7proj-")
bad = 0; checks = 0
OK = "i32 main() { return 0; }\n"
PRINT = 'extern "C" { void c7_print_i64(i64 v); void c7_newline(); }\n'

def fail(m):
    global bad
    bad += 1; print("FAIL", m)
def ok(cond, m):
    global checks
    checks += 1
    if not cond: fail(m)

def project(name, files, build):
    d = os.path.join(W, name)
    shutil.rmtree(d, ignore_errors=True)
    for f, t in {**files, "Cmake7109.txt": build}.items():
        p = os.path.join(d, f); os.makedirs(os.path.dirname(p), exist_ok=True)
        open(p, "w").write(t)
    return d

def c7(args, cwd=None):
    if args[0] in ("build", "pack") and "--no-cache" not in args and not any(a.endswith(".c7") for a in args): args = [*args, "--no-cache"]   # the cache has its own test (check_cache.py)
    r = subprocess.run([C7C, *args], capture_output=True, text=True, cwd=cwd, timeout=120)
    return r.returncode, r.stdout, r.stderr

def events(err):
    return [l[len("[c7c] "):] for l in err.splitlines() if l.startswith("[c7c] ") and not l.startswith("[c7c]   ")]

def tgt(name, deps=(), kind="executable", src=None, extra=""):
    d = "".join(f'        "{x}"\n' for x in deps)
    return f'target {kind} "{name}":\n    sources:\n        "{src or "src/" + name + ".c7"}"\n' + (f"    depends:\n{d}" if deps else "") + extra

def exe_src(): return OK

# ---- 1. dependency order, library artifact, run result
files = {"src/lib.c7": "i64 triple(i64 x) { return x * 3; }\n",
         "src/main.c7": PRINT + "i32 main() { c7_print_i64(triple(14)); c7_newline(); return 0; }\n"}
d = project("order", files, 'project "Demo":\n    version "1.0.0"\n' + tgt("mylib", kind="library", src="src/lib.c7") + tgt("app", ["mylib"], src="src/main.c7"))
rc, out, err = c7(["build", d, "--verbose", "-j4"])
ev = events(err)
ok(rc == 0 and ev == ["start mylib", "done mylib", "start app", "done app"], f"order: rc={rc} {ev} {err[-200:]}")
ok(subprocess.run([os.path.join(d, "build", "app")], capture_output=True, text=True).stdout == "42\n", "app output")
ok(out == "c7c: built library mylib -> build/mylib-1.0.0.l7\nc7c: built executable app -> build/app\n", f"summary lines: {out!r}")
rc, out, err = c7(["verify", d])
ok(rc == 0 and out.startswith("c7c: ok mylib 1.0.0"), f"verify: {rc} {out} {err}")
h1 = hashlib.sha256(open(os.path.join(d, "build", "mylib-1.0.0.l7"), "rb").read()).hexdigest()
c7(["clean", d]); c7(["build", d])
h2 = hashlib.sha256(open(os.path.join(d, "build", "mylib-1.0.0.l7"), "rb").read()).hexdigest()
ok(h1 == h2, "library package must be reproducible across builds")
rc, out, err = c7(["build", d], cwd=d)   # default path "."
ok(rc == 0, f"build from inside the project: {err}")

# ---- 2. concurrency: two independent targets are both started before either finishes (-j2); -j1 serialises
d = project("par", {"src/a.c7": OK, "src/b.c7": OK, "src/c.c7": OK}, 'project "P"\n' + tgt("a") + tgt("b") + tgt("c", ["a", "b"]))
rc, out, err = c7(["build", d, "--verbose", "-j2"]); ev = events(err)
first_done = min(i for i, e in enumerate(ev) if e.startswith("done"))
ok(rc == 0 and {"start a", "start b"} <= set(ev[:first_done]), f"-j2 must start a and b together: {ev}")
ok(ev.index("start c") > max(ev.index("done a"), ev.index("done b")), f"c waits for both: {ev}")
rc, out, err = c7(["build", d, "--verbose", "-j1"]); ev = events(err)
ok(ev == ["start a", "done a", "start b", "done b", "start c", "done c"], f"-j1: {ev}")

# ---- 3. failure: a fails, b independent, c depends on a
files = {"src/a.c7": "i64 bad( { }\n", "src/b.c7": OK, "src/c.c7": OK, "src/d.c7": OK}
d = project("fail", files, 'project "P"\n' + tgt("a") + tgt("b") + tgt("c", ["a"]) + tgt("d", ["b"]))
rc, out, err = c7(["build", d, "--verbose", "-j2"]); ev = events(err)
ok(rc == 1, f"failure rc={rc}")
ok(ev[:2] == ["start a", "start b"] and "failed a" in ev and "done b" in ev, f"running target is awaited: {ev}")
ok("start c" not in ev and "start d" not in ev, f"no new work after a failure: {ev}")
ok("error[E0720]" in err and "`a`" in err.split("error[E0720]")[1].splitlines()[0], f"E0720 for a: {err[-300:]}")
ok("skipped executable c" in err and "skipped executable d" in err, f"skipped targets reported: {err[-400:]}")
ok(os.path.exists(os.path.join(d, "build", "b")) and not os.path.exists(os.path.join(d, "build", "a")), "b built, a absent")
rc, out, err = c7(["build", d, "--verbose", "-j1"]); ev = events(err)
ok(ev == ["start a", "failed a"], f"-j1: failure stops everything else: {ev}")
rc2, out2, err2 = c7(["build", d, "-j1"])
ok("a.c7:1:10" in err2 or "src/a.c7:1:" in err2, f"child diagnostics are replayed: {err2[:200]}")
# stale output is removed when a rebuild fails
d = project("stale", {"src/a.c7": OK}, 'project "P"\n' + tgt("a"))
c7(["build", d]); ok(os.path.exists(os.path.join(d, "build", "a")), "built once")
open(os.path.join(d, "src", "a.c7"), "w").write("i64 bad( { }\n")
rc, _, _ = c7(["build", d]); ok(rc == 1 and not os.path.exists(os.path.join(d, "build", "a")), "failed rebuild must not leave the old executable")

# ---- 4. determinism: identical stdout/stderr over repeated parallel runs (a failing build is not compared byte for byte:
#         which independent targets got started before the failure is a matter of timing - only the replay order is fixed)
files = {f"src/t{i}.c7": OK for i in range(6)}
d = project("det", files, 'project "P"\n' + "".join(tgt(f"t{i}", [f"t{i-2}"] if i >= 2 else []) for i in range(6)))
runs = set()
for _ in range(6):
    shutil.rmtree(os.path.join(d, "build"), ignore_errors=True)
    rc, out, err = c7(["build", d, "-j4"]); runs.add((rc, out, err))
ok(len(runs) == 1, f"non-deterministic output across runs: {len(runs)} variants")

files["src/t5.c7"] = "i64 bad( { }\n"
d = project("det2", files, 'project "P"\n' + "".join(tgt(f"t{i}", [f"t{i-2}"] if i >= 2 else []) for i in range(6)))
for _ in range(4):
    rc, out, err = c7(["build", d, "-j4"])
    names = re.findall(r"built executable (t\d)", out)
    ok(rc == 1 and names == sorted(names), f"built lines out of graph order: {names}")
# ---- 5. exit codes and usage
d = project("ok1", {"src/a.c7": OK}, 'project "P"\n' + tgt("a"))
ok(c7(["build", d])[0] == 0, "rc 0")
ok(c7(["build", d, "--bogus"])[0] == 2, "unknown option -> 2")
ok(c7(["build", d, "--target", "nope"])[0] == 2, "unknown target -> 2")
ok(c7(["build", d, "-j"])[0] == 2, "-j without value -> 2")
ok(c7(["build", os.path.join(W, "nowhere")])[0] == 1, "missing project -> 1")
ok(c7(["install", d])[0] == 1 and c7(["lock", d])[0] == 0, "install (nothing built, no library) -> 1; lock works")
d2 = project("synerr", {}, 'project "P"\nfrob\n')
rc, out, err = c7(["build", d2]); ok(rc == 1 and "Cmake7109.txt:2:1: error[E0703]" in err, f"build-file error: {rc} {err}")
d2 = project("nosrc", {}, 'project "P"\n' + tgt("a"))
rc, out, err = c7(["build", d2]); ok(rc == 1 and "error[E0704]" in err, f"missing source: {err}")
d2 = project("cyc", {"src/a.c7": OK, "src/b.c7": OK}, 'project "P"\n' + tgt("a", ["b"]) + tgt("b", ["a"]))
rc, out, err = c7(["build", d2]); ok(rc == 1 and "a -> b -> a" in err, f"cycle: {err}")
rc, out, err = c7(["build", d, "--out-dir", "../x"]); ok(rc == 1 and "E0719" in err, f"out-dir escape: {err}")
d2 = project("extdep", {"src/a.c7": OK}, 'project "P"\ndependency "Ext" "^1"\n' + tgt("a", ["Ext"]))
rc, out, err = c7(["build", d2]); ok(rc == 1 and "E0716" in err, f"unresolved external dependency is refused: {err}")
# the single-file mode is unaffected (any .c7 argument)
open(os.path.join(W, "single.c7"), "w").write(PRINT + "i32 main() { c7_print_i64(7); c7_newline(); return 0; }\n")
rc, _, err = c7(["build", os.path.join(W, "single.c7"), "-o", os.path.join(W, "single")])
ok(rc == 0 and subprocess.run([os.path.join(W, "single")], capture_output=True, text=True).stdout == "7\n", f"single-file mode: {err}")

# ---- 6. options flow from config / target / command line into the child compiler (checked on real behaviour)
ovf = open(os.path.join(ROOT, "chk", "ovf_add.c7")).read() if os.path.exists(os.path.join(ROOT, "chk", "ovf_add.c7")) else None
ovf_name = sorted(f for f in os.listdir(os.path.join(ROOT, "chk")) if f.startswith("ovf_") and f.endswith(".c7"))[0]
ovf = open(os.path.join(ROOT, "chk", ovf_name)).read()
trap = int(open(os.path.join(ROOT, "chk", ovf_name[:-3] + ".rc")).read())
def exit_of(d, name="app"):
    rc = subprocess.run([os.path.join(d, "build", name)], capture_output=True).returncode
    return 128 - rc if rc < 0 else rc
B = 'project "P"\nconfig debug:\n    mode "debug"\nconfig release:\n    mode "release"\n    optimization "O2"\ndefault_config release\n'
d = project("opt", {"src/app.c7": ovf}, B + tgt("app"))
c7(["build", d]); ok(exit_of(d) == 0, "default_config release: no overflow trap")
c7(["build", d, "--config", "debug"]); ok(exit_of(d) == trap, "--config debug: overflow traps")
c7(["build", d, "--config", "debug", "--mode=release"]); ok(exit_of(d) == 0, "command line beats config")
d = project("opt2", {"src/app.c7": ovf}, B + tgt("app", extra='    mode "debug"\n'))
c7(["build", d]); ok(exit_of(d) == trap, "target property beats the default config")
c7(["build", d, "--mode=release"]); ok(exit_of(d) == 0, "command line beats target")
rc, out, err = c7(["build", d, "--verbose", "-O3", "--mode=debug"])
ok("-O3" in err and "--debug" in err, f"flags forwarded: {err}")
d = project("cset", {"src/app.c7": OK}, 'project "P"\n' + tgt("app"))
open(os.path.join(d, ".cset7"), "w").write('[build]\noptimization = "O1"\n')
rc, out, err = c7(["build", d, "--verbose"]); ok("-O1" in err, f".cset7 next to the build file is used: {err}")
rc, out, err = c7(["build", d, "--verbose", "-O3"]); ok("-O3" in err and "-O1" not in err, f"command line beats .cset7: {err}")
rc, out, err = c7(["build", d, "--verbose", "--cset7", os.path.join(W, "nonexistent.cset7")]); ok(rc == 1, "missing --cset7 file is an error")
# defines, include dirs, std "none"
files = {"src/app.c7": '#include "val.h7"\n' + PRINT + "i32 main() { c7_print_i64(VALUE + EXTRA); c7_newline(); return 0; }\n", "inc/val.h7": "#define VALUE 40\n"}
d = project("defs", files, 'project "P"\n' + tgt("app", extra='    include_dirs:\n        "inc"\n    defines:\n        "EXTRA=2"\n'))
rc, out, err = c7(["build", d]); ok(rc == 0 and subprocess.run([os.path.join(d, "build", "app")], capture_output=True, text=True).stdout == "42\n", f"include_dirs + defines: {err}")
d = project("nostd", {"src/app.c7": OK}, 'project "P"\n' + tgt("app", extra='    std "none"\n'))
rc, out, err = c7(["build", d, "--verbose"]); ok("--no-std" in err, f'std "none" -> --no-std: {err}')

# ---- 7. target selection, pack, clean, verify
d = project("sel", {"src/l.c7": "i64 f() { return 1; }\n", "src/a.c7": OK, "src/b.c7": OK}, 'project "P"\nversion_ignored_marker\n'.replace("version_ignored_marker\n", "") + tgt("l", kind="library") + tgt("a", ["l"]) + tgt("b"))
rc, out, err = c7(["build", d, "--target", "a", "--verbose"]); ev = events(err)
ok(ev == ["start l", "done l", "start a", "done a"] and not os.path.exists(os.path.join(d, "build", "b")), f"--target builds the closure only: {ev}")
shutil.rmtree(os.path.join(d, "build"))
rc, out, err = c7(["pack", d]); ok(rc == 0 and os.listdir(os.path.join(d, "build")) == ["l-0.0.0.l7"], f"pack builds libraries only: {os.listdir(os.path.join(d, 'build'))}")
l7 = os.path.join(d, "build", "l-0.0.0.l7")
rc, out, err = c7(["verify", l7]); ok(rc == 0, "verify a file")
data = bytearray(open(l7, "rb").read()); data[len(data) // 2] ^= 0x55; open(l7, "wb").write(data)
rc, out, err = c7(["verify", l7]); ok(rc == 1, "verify rejects a damaged package")
rc, out, err = c7(["verify", d]); ok(rc == 1, "verify on the project sees the damaged package")
os.makedirs(os.path.join(d, ".c7cache", "x")); 
rc, out, err = c7(["clean", d]); ok(rc == 0 and not os.path.exists(os.path.join(d, "build")) and not os.path.exists(os.path.join(d, ".c7cache")) and os.path.exists(os.path.join(d, "Cmake7109.txt")) and os.path.exists(os.path.join(d, "src", "a.c7")), "clean removes build and cache only")
ok(c7(["clean", d, "--out-dir", "."])[0] == 2 and c7(["clean", d, "--out-dir", ".."])[0] == 2 and c7(["clean", d, "--out-dir", "/etc"])[0] == 2, "clean refuses dangerous --out-dir")
outside = os.path.join(W, "precious"); os.makedirs(outside); open(os.path.join(outside, "keep"), "w").write("x")
os.symlink(outside, os.path.join(d, "build"))
c7(["clean", d]); ok(os.path.exists(os.path.join(outside, "keep")), "clean must not follow a symlink out of the project")
ok(c7(["clean", os.path.join(W, "nowhere")])[0] == 1, "clean without a build file fails")
print(f"project: {checks} checks, problems={bad}")
sys.exit(1 if bad else 0)
