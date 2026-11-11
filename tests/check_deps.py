#!/usr/bin/env python3
"""Dependencies inside real project builds (W11 wiring): registry and path dependencies reach the compiler, transitive
packages are passed on, library packages record their dependencies, sub-projects build first, the cache notices
changed packages."""
import os, shutil, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.abspath(__file__))
C7C = os.path.abspath(os.path.join(ROOT, "..", "c7c"))
W = tempfile.mkdtemp(prefix="c7deps-")
PRINT = 'extern "C" { void c7_print_i64(i64 v); void c7_newline(); }\n'
bad = 0; checks = 0
def fail(m):
    global bad
    bad += 1; print("FAIL", m)
def ok(c, m):
    global checks
    checks += 1
    if not c: fail(m)
def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True); open(path, "w").write(text)
def pkg(reg, name, ver, code, deps=()):
    src = os.path.join(W, f"{name}-{ver}.c7"); write(src, code)
    os.makedirs(reg, exist_ok=True)
    a = [C7C, "tool", "l7-pack", os.path.join(reg, f"{name}-{ver}.l7"), name, ver]
    for n, r in deps: a += ["--dep", n, r]
    subprocess.run(a + [f"src/{name}.c7=" + src], check=True, capture_output=True)
def build(d, *a, cwd=None):
    r = subprocess.run([C7C, "build", d, "--verbose", "-j1", *a], capture_output=True, text=True, cwd=cwd)
    ev = [l[6:] for l in r.stderr.splitlines() if l.startswith("[c7c] ") and not l.startswith("[c7c]   ")]
    return r.returncode, sorted(e[6:] for e in ev if e.startswith("start ")), sorted(e[7:] for e in ev if e.startswith("cached ")), r.stderr
def run_app(d, name="app"):
    p = os.path.join(d, "build", name)
    return subprocess.run([p], capture_output=True, text=True).stdout if os.path.exists(p) else "<no executable>"
MAIN = PRINT + "i32 main() { c7_print_i64(triple(14)); c7_newline(); return 0; }\n"
def app(name, deps, extra=""):
    d = "".join(f'        "{x}"\n' for x in deps)
    return f'target executable "{name}":\n    sources:\n        "src/main.c7"\n    depends:\n{d}{extra}'

# 1+2. a registry package; the highest 1.x is used
REG = os.path.join(W, "reg")
pkg(REG, "Mathlib", "1.0.0", "i64 triple(i64 x) { return x * 3; }\n")
pkg(REG, "Mathlib", "1.1.0", "i64 triple(i64 x) { return x * 4; }\n")
pkg(REG, "Mathlib", "2.0.0", "i64 triple(i64 x) { return x * 5; }\n")
d = os.path.join(W, "p1"); write(d + "/src/main.c7", MAIN)
write(d + "/Cmake7109.txt", f'project "P"\nregistry "{REG}"\ndependency "Mathlib" "^1.0"\n' + app("app", ["Mathlib"]))
rc, ran, hit, err = build(d)
ok(rc == 0 and run_app(d) == "56\n", f"registry dependency: rc={rc} out={run_app(d)!r} {err[-300:]}")
write(d + "/Cmake7109.txt", f'project "P"\nregistry "{REG}"\ndependency "Mathlib" "=1.0.0"\n' + app("app", ["Mathlib"]))
rc, ran, hit, err = build(d); ok(rc == 0 and ran == ["app"] and run_app(d) == "42\n", f"different version -> rebuilt, new behaviour: {ran} {run_app(d)!r}")
rc, ran, hit, err = build(d); ok(hit == ["app"] and ran == [], f"same package again -> cached: {ran} {hit}")

# the cache notices a new higher version appearing in the registry, and changed content under the same version
write(d + "/Cmake7109.txt", f'project "P"\nregistry "{REG}"\ndependency "Mathlib" "^1.0"\n' + app("app", ["Mathlib"]))
build(d); pkg(REG, "Mathlib", "1.2.0", "i64 triple(i64 x) { return x * 6; }\n")
rc, ran, hit, err = build(d); ok(ran == ["app"] and run_app(d) == "84\n", f"new registry version: {ran} {run_app(d)!r}")
pkg(REG, "Mathlib", "1.2.0", "i64 triple(i64 x) { return x * 7; }\n")  # same name and version, different bytes
rc, ran, hit, err = build(d); ok(ran == ["app"] and run_app(d) == "98\n", f"same version, new content: {ran} {run_app(d)!r}")

# 3. transitive packages are passed to the compiler although the project never names them
REG2 = os.path.join(W, "reg2")
pkg(REG2, "Base", "1.0.0", "i64 base_mul(i64 a, i64 b) { return a * b; }\n")
pkg(REG2, "Mid", "1.0.0", "i64 triple(i64 x) { return base_mul(x, 3); }\n", [("Base", "^1")])
d = os.path.join(W, "p3"); write(d + "/src/main.c7", MAIN)
write(d + "/Cmake7109.txt", f'project "P"\nregistry "{REG2}"\ndependency "Mid" "^1"\n' + app("app", ["Mid"]))
rc, ran, hit, err = build(d); ok(rc == 0 and run_app(d) == "42\n", f"transitive package: {err[-300:]}")

# 4. a library target records its external dependencies in its manifest
write(d + "/src/lib.c7", "i64 six(i64 x) { return triple(x) * 2; }\n")
write(d + "/Cmake7109.txt", f'project "P":\n    version "2.0.0"\nregistry "{REG2}"\ndependency "Mid" "^1"\ntarget library "mylib":\n    sources:\n        "src/lib.c7"\n    depends:\n        "Mid"\n')
rc, ran, hit, err = build(d)
man = subprocess.run([C7C, "tool", "zip-cat", os.path.join(d, "build", "mylib-2.0.0.l7"), "manifest.toml"], capture_output=True, text=True).stdout
ok(rc == 0 and 'name = "Mid"' in man and 'version = "^1"' in man, f"library manifest lists its dependency: rc={rc} {man} {err[-200:]}")

# 5. path dependency on a .l7 file
pkg(os.path.join(W, "loose"), "Mathlib", "9.9.9", "i64 triple(i64 x) { return x * 3; }\n")
d = os.path.join(W, "p5"); write(d + "/src/main.c7", MAIN)
write(d + "/Cmake7109.txt", f'project "P"\ndependency "Mathlib" path "{W}/loose/Mathlib-9.9.9.l7"\n' + app("app", ["Mathlib"]))
rc, ran, hit, err = build(d); ok(rc == 0 and run_app(d) == "42\n", f"path dependency (.l7): {err[-300:]}")

# 6. path dependency on a sub-project directory (built first, by a child build)
sub = os.path.join(W, "subproj"); write(sub + "/src/sublib.c7", "i64 triple(i64 x) { return x * 3; }\n")
write(sub + "/Cmake7109.txt", 'project "Sub":\n    version "0.5.0"\ntarget library "sublib":\n    sources:\n        "src/sublib.c7"\n')
d = os.path.join(W, "p6"); write(d + "/src/main.c7", MAIN)
write(d + "/Cmake7109.txt", f'project "P"\ndependency "sublib" path "../subproj"\n' + app("app", ["sublib"]))
rc, ran, hit, err = build(d, "--no-cache"); ok(rc == 0 and run_app(d) == "42\n" and os.path.exists(sub + "/build/sublib-0.5.0.l7"), f"sub-project path dependency: {err[-300:]}")
write(sub + "/src/sublib.c7", "i64 triple(i64 x) { return x * 3 + 1; }\n")
rc, ran, hit, err = build(d); ok(rc == 0 and run_app(d) == "43\n", f"sub-project change is picked up: {run_app(d)!r}")
write(d + "/Cmake7109.txt", f'project "P"\ndependency "nolib" path "../subproj"\n' + app("app", ["nolib"]))
rc, ran, hit, err = build(d); ok(rc == 1 and "[E0716]" in err and "no library target named `nolib`" in err, f"no such library in the sub-project: {err[-300:]}")
write(sub + "/src/sublib.c7", "i64 bad( { }\n")
write(d + "/Cmake7109.txt", f'project "P"\ndependency "sublib" path "../subproj"\n' + app("app", ["sublib"]))
rc, ran, hit, err = build(d, "--no-cache"); ok(rc == 1 and "[E0720]" in err, f"a failing sub-project fails the build: {err[-300:]}")
# cycle of sub-projects
for a, b in (("cyca", "cycb"), ("cycb", "cyca")):
    write(f"{W}/{a}/src/{a}.c7", f"i64 f_{a}() {{ return 1; }}\n")
    write(f"{W}/{a}/Cmake7109.txt", f'project "{a}"\ndependency "{b}" path "../{b}"\ntarget library "{a}":\n    sources:\n        "src/{a}.c7"\n    depends:\n        "{b}"\n')
rc, ran, hit, err = build(f"{W}/cyca", "--no-cache"); ok(rc == 1 and "[E0706]" in err, f"cycle of path dependencies terminates with an error: {err[-300:]}")

# 7. conflicts and missing packages fail the build before anything is compiled
d = os.path.join(W, "p7"); write(d + "/src/main.c7", MAIN)
write(d + "/Cmake7109.txt", f'project "P"\nregistry "{REG}"\ndependency "Mathlib" "^3"\n' + app("app", ["Mathlib"]))
rc, ran, hit, err = build(d); ok(rc == 1 and "[E0713]" in err and ran == [], f"unsatisfiable requirement: {err[-300:]}")
write(d + "/Cmake7109.txt", f'project "P"\ndependency "Ghost" "^1"\n' + app("app", []))
rc, ran, hit, err = build(d); ok(rc == 1 and "[E0716]" in err and ran == [], f"declared but unreferenced dependency is still resolved: {err[-300:]}")

# 8. registry search paths: --registry (relative to the working directory) and .cset7 [paths] registry (relative to the project)
d = os.path.join(W, "p8"); write(d + "/src/main.c7", MAIN)
write(d + "/Cmake7109.txt", 'project "P"\ndependency "Mathlib" "=1.0.0"\n' + app("app", ["Mathlib"]))
rc, ran, hit, err = build(d); ok(rc == 1 and "[E0716]" in err, "no registry configured")
rc, ran, hit, err = build(d, "--registry", REG); ok(rc == 0 and run_app(d) == "42\n", f"--registry: {err[-200:]}")
shutil.rmtree(d + "/build")
rc, ran, hit, err = build(os.path.relpath(d, W), "--registry", os.path.relpath(REG, W), "--no-cache", cwd=W); ok(rc == 0 and run_app(d) == "42\n", f"--registry relative to the working directory: {err[-200:]}")
write(d + "/.cset7", f'[paths]\nregistry = ["{os.path.relpath(REG, d)}"]\n')
rc, ran, hit, err = build(d); ok(rc == 0, f".cset7 registry relative to the project directory: {err[-200:]}")
rc, ran, hit, err = build(d, "--registry", os.path.join(W, "empty")); ok(rc == 0, "the command line adds to, and does not replace, other registries")
print(f"deps: {checks} checks, problems={bad}")
sys.exit(1 if bad else 0)
