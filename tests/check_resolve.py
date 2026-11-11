#!/usr/bin/env python3
"""Dependency resolution (W11): highest satisfying version, transitive constraints, conflicts (E0713), missing (E0716),
registry hygiene, path dependencies, stubs, determinism. Expected results are derived by hand from the package sets below."""
import hashlib, json, os, shutil, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.abspath(__file__))
C7C = os.path.abspath(os.path.join(ROOT, "..", "c7c"))
W = tempfile.mkdtemp(prefix="c7res-")
SRC = os.path.join(W, "x.c7"); open(SRC, "w").write("i64 one() { return 1; }\n")
bad = 0; checks = 0
def fail(m):
    global bad
    bad += 1; print("FAIL", m)
def ok(c, m):
    global checks
    checks += 1
    if not c: fail(m)

def pkg(reg, name, ver, deps=(), fname=None):
    os.makedirs(reg, exist_ok=True)
    out = os.path.join(reg, fname or f"{name}-{ver}.l7")
    a = [C7C, "tool", "l7-pack", out, name, ver]
    for n, r in deps: a += ["--dep", n, r]
    a.append("src/x.c7=" + SRC)
    subprocess.run(a, check=True, capture_output=True)
    return out

n = [0]
def case(build_extra, regs=(), name=None):
    n[0] += 1
    d = os.path.join(W, f"p{n[0]}"); os.makedirs(d)
    open(os.path.join(d, "Cmake7109.txt"), "w").write('project "P"\n' + build_extra)
    args = [C7C, "tool", "resolve-json", os.path.join(d, "Cmake7109.txt")]
    for r in regs: args += ["--registry", r]
    r = subprocess.run(args, capture_output=True, text=True)
    return r.returncode, (json.loads(r.stdout) if r.stdout.strip() else None), r.stderr.replace(d + "/", "")

def versions(res): return {r["name"]: r["version"] for r in res}
def dep(name, req): return f'dependency "{name}" "{req}"\n'

# 1. highest satisfying version
R = os.path.join(W, "r1")
for v in ("1.0.0", "1.2.5", "1.9.0", "2.0.0"): pkg(R, "A", v)
rc, res, err = case(dep("A", "^1.0"), [R]); ok(rc == 0 and versions(res) == {"A": "1.9.0"}, f"highest 1.x: {res} {err}")
rc, res, err = case(dep("A", "~1.2"), [R]); ok(versions(res) == {"A": "1.2.5"}, f"tilde: {res}")
rc, res, err = case(dep("A", ">=1.0, <1.9"), [R]); ok(versions(res) == {"A": "1.2.5"}, f"range: {res}")
rc, res, err = case(dep("A", "*"), [R]); ok(versions(res) == {"A": "2.0.0"}, f"star: {res}")
rc, res, err = case(dep("A", "=1.0.0"), [R]); ok(versions(res) == {"A": "1.0.0"}, f"exact: {res}")
rc, res, err = case(dep("A", "1.*"), [R]); ok(versions(res) == {"A": "1.9.0"}, f"wildcard: {res}")
ok(res[0]["sha256"] == hashlib.sha256(open(os.path.join(R, "A-1.9.0.l7"), "rb").read()).hexdigest() and res[0]["source"] == f"registry:{R}", f"sha256/source: {res}")
rc, res, err = case(dep("A", "^3.0"), [R])
ok(rc == 1 and "[E0713]" in err and "A" in err and "the project requires ^3.0" in err and "available: 1.0.0, 1.2.5, 1.9.0, 2.0.0" in err, f"nothing satisfies: {err}")
ok(err.startswith("Cmake7109.txt:2:1: error[E0713]"), f"E0713 points at the dependency line: {err[:80]}")

# 2. pre-releases only when asked for explicitly
R = os.path.join(W, "r2")
pkg(R, "A", "1.0.0"); pkg(R, "A", "1.1.0-beta.1")
rc, res, err = case(dep("A", "^1.0"), [R]); ok(versions(res) == {"A": "1.0.0"}, f"prerelease excluded: {res}")
rc, res, err = case(dep("A", "=1.1.0-beta.1"), [R]); ok(versions(res) == {"A": "1.1.0-beta.1"}, f"prerelease named explicitly: {res}")
rc, res, err = case(dep("A", ">=1.1.0"), [R]); ok(rc == 1, f">=1.1.0 does not admit 1.1.0-beta.1: {err}")

# 3. transitive requirements, fixed point
R = os.path.join(W, "r3")
pkg(R, "A", "1.0.0", [("B", "^1")])
for v in ("1.0.0", "1.5.0", "2.0.0"): pkg(R, "B", v)
rc, res, err = case(dep("A", "^1"), [R]); ok(versions(res) == {"A": "1.0.0", "B": "1.5.0"}, f"transitive: {res}")
ok([r["name"] for r in res] == ["B", "A"] and res[1]["deps"] == ["B"], f"dependencies first: {[r['name'] for r in res]}")
rc, res, err = case(dep("A", "^1") + dep("B", "<1.5"), [R]); ok(versions(res) == {"A": "1.0.0", "B": "1.0.0"}, f"root tightens a transitive requirement: {res} {err}")
# a root requirement that the picked package then contradicts
R = os.path.join(W, "r3b")
pkg(R, "A", "1.0.0", [("B", "^2")]); pkg(R, "B", "1.0.0"); pkg(R, "B", "2.0.0")
rc, res, err = case(dep("A", "^1") + dep("B", "^1"), [R])
ok(rc == 1 and "[E0713]" in err and "the project requires ^1; A 1.0.0 requires ^2" in err, f"conflict between project and package: {err}")

# 4. conflict between two packages, requirers are named
R = os.path.join(W, "r4")
pkg(R, "A", "1.0.0", [("C", "^1")]); pkg(R, "B", "1.0.0", [("C", "^2")]); pkg(R, "C", "1.0.0"); pkg(R, "C", "2.0.0")
rc, res, err = case(dep("A", "^1") + dep("B", "^1"), [R])
ok(rc == 1 and "`C`" in err and "A 1.0.0 requires ^1; B 1.0.0 requires ^2" in err, f"E0713 names who asked: {err}")
ok(res is None, "no partial result is printed on a conflict")

# 5. no backtracking in v1: the highest A is chosen even though it leads to a missing package
R = os.path.join(W, "r5")
pkg(R, "A", "1.0.0"); pkg(R, "A", "2.0.0", [("Missing", "^1")])
rc, res, err = case(dep("A", "*"), [R]); ok(rc == 1 and "[E0716]" in err and "Missing" in err, f"v1 does not backtrack: {err}")

# 6. missing package
rc, res, err = case(dep("Nope", "^1"), [os.path.join(W, "r1")]); ok(rc == 1 and "[E0716]" in err and "no package of that name" in err and os.path.join(W, "r1") in err, f"not found: {err}")
rc, res, err = case(dep("Nope", "^1"), []); ok(rc == 1 and "[E0716]" in err and "none configured" in err, f"no registry at all: {err}")
rc, res, err = case("", []); ok(rc == 0 and res == [], f"no dependencies: {res}")

# 7. registry hygiene
R = os.path.join(W, "r7"); good = pkg(R, "A", "1.0.0")
shutil.copy(good, os.path.join(R, "A-1.0.1.l7"))
rc, res, err = case(dep("A", "^1"), [R]); ok(rc == 1 and "[E0709]" in err and "file name does not match its manifest" in err and "A-1.0.0.l7" in err, f"file name vs manifest: {err}")
R = os.path.join(W, "r7b"); good = pkg(R, "A", "1.0.0")
data = bytearray(open(good, "rb").read()); data[len(data) // 2] ^= 0x42; open(good, "wb").write(data)
rc, res, err = case(dep("A", "^1"), [R]); ok(rc == 1 and any(c in err for c in ("[E0709]", "[E0710]", "[E0718]")), f"damaged registry file: {err}")
R = os.path.join(W, "r7c"); pkg(R, "foo", "1.0.0"); pkg(R, "foo-bar", "2.0.0")
open(os.path.join(R, "foo-bar-9.9.9.l7"), "wb").write(b"garbage that must not even be opened for foo")
open(os.path.join(R, "foo-notes.l7"), "wb").write(b"not a version either")
rc, res, err = case(dep("foo", "*"), [R]); ok(rc == 0 and versions(res) == {"foo": "1.0.0"}, f"`foo-bar-*` files are not versions of foo: {res} {err}")
rc, res, err = case(dep("foo-bar", "*"), [R]); ok(rc == 1, "...but resolving foo-bar does open its own damaged file")
R = os.path.join(W, "r7d"); pkg(R, "a", "1.0.0"); pkg(R, "A", "2.0.0")
rc, res, err = case(dep("a", "*") + dep("A", "*"), [R]); ok(versions(res) == {"a": "1.0.0", "A": "2.0.0"}, f"names are case sensitive: {res}")

# 8. several registries: union of versions, first one wins a tie
R1, R2 = os.path.join(W, "m1"), os.path.join(W, "m2")
pkg(R1, "A", "1.0.0"); pkg(R1, "A", "1.1.0"); pkg(R2, "A", "1.1.0"); pkg(R2, "A", "1.2.0")
rc, res, err = case(dep("A", "^1"), [R1, R2]); ok(res[0]["version"] == "1.2.0" and res[0]["source"] == f"registry:{R2}", f"union: {res}")
rc, res, err = case(dep("A", "=1.1.0"), [R1, R2]); ok(res[0]["source"] == f"registry:{R1}", f"first registry wins a tie: {res}")
# registries listed in the build file are used too
rc, res, err = case(f'registry "{R2}"\n' + dep("A", "^1")); ok(rc == 0 and res[0]["version"] == "1.2.0", f"registry from the build file: {res} {err}")
rel = os.path.relpath(R2, W)
n[0] += 1; d = os.path.join(W, f"p{n[0]}"); os.makedirs(d)
open(os.path.join(d, "Cmake7109.txt"), "w").write(f'project "P"\nregistry "../{rel}"\n' + dep("A", "^1"))
r = subprocess.run([C7C, "tool", "resolve-json", os.path.join(d, "Cmake7109.txt")], capture_output=True, text=True)
ok(r.returncode == 0 and json.loads(r.stdout)[0]["version"] == "1.2.0", f"relative registry path is taken against the project directory: {r.stderr}")

# 9. path dependencies
P = os.path.join(W, "paths"); L = pkg(P, "Local", "0.3.0", [("B", "^1")])
R = os.path.join(W, "r9"); pkg(R, "B", "1.0.0"); pkg(R, "B", "1.4.0")
rc, res, err = case(f'dependency "Local" path "{L}"\n', [R])
ok(rc == 0 and versions(res) == {"B": "1.4.0", "Local": "0.3.0"} and [r["source"] for r in res] == [f"registry:{R}", f"path:{L}"], f"path dependency + its registry requirement: {res} {err}")
rc, res, err = case(f'dependency "Wrong" path "{L}"\n', [R]); ok(rc == 1 and "[E0709]" in err and "`Local`" in err, f"path package of another name: {err}")
rc, res, err = case(f'dependency "Local" path "{os.path.join(P, "absent.l7")}"\n', [R]); ok(rc == 1, "missing path dependency")
Q = pkg(os.path.join(W, "paths2"), "Pin", "1.0.0"); Z = pkg(os.path.join(W, "r9b"), "User", "1.0.0", [("Pin", "^2")])
rc, res, err = case(f'dependency "Pin" path "{Q}"\n' + dep("User", "^1"), [os.path.join(W, "r9b")])
ok(rc == 1 and "[E0713]" in err and "User 1.0.0 requires ^2" in err, f"a path dependency still has to satisfy others' requirements: {err}")
rc, res, err = case(f'dependency "Local" path "{L}"\n' + dep("B", "<1.4"), [R]); ok(versions(res) == {"B": "1.0.0", "Local": "0.3.0"}, f"root requirement beats the highest: {res}")

# 10. cycles
R = os.path.join(W, "r10"); pkg(R, "A", "1.0.0", [("B", "^1")]); pkg(R, "B", "1.0.0", [("A", "^1")])
rc, res, err = case(dep("A", "^1"), [R]); ok(rc == 1 and "[E0706]" in err and "A -> B -> A" in err, f"package cycle: {err}")

# 11. stubs for the planned registry kinds
for kind in ("central:https://registry.example/", "git:https://example.org/r.git", "enterprise:corp"):
    rc, res, err = case(dep("A", "^1"), [kind]); ok(rc == 1 and "[E0716]" in err and "not implemented" in err, f"stub {kind}: {err}")

# 12. determinism and ordering
R = os.path.join(W, "r12")
for nme in ("Z", "M", "A"): pkg(R, nme, "1.0.0")
out1 = case(dep("Z", "*") + dep("A", "*") + dep("M", "*"), [R]); out2 = case(dep("M", "*") + dep("Z", "*") + dep("A", "*"), [R])
ok([r["name"] for r in out1[1]] == ["A", "M", "Z"] and out1[1] == out2[1], f"order independent of declaration order: {[r['name'] for r in out1[1]]}")
# a diamond resolved once
R = os.path.join(W, "r12b"); pkg(R, "Top", "1.0.0", [("L", "^1"), ("Rr", "^1")]); pkg(R, "L", "1.0.0", [("Base", "^1")]); pkg(R, "Rr", "1.0.0", [("Base", "^1")]); pkg(R, "Base", "1.0.0"); pkg(R, "Base", "1.1.0")
rc, res, err = case(dep("Top", "*"), [R]); ok([r["name"] for r in res] == ["Base", "L", "Rr", "Top"] and versions(res)["Base"] == "1.1.0", f"diamond: {[r['name'] for r in res]}")
print(f"resolve: {checks} checks, problems={bad}")
sys.exit(1 if bad else 0)
