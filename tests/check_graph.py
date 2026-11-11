#!/usr/bin/env python3
"""Build graph (W7): topological order, determinism, cycles, output collisions, platform, unknown names.
All expectations are hand-derived from the build files written inline below."""
import json, os, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.abspath(__file__))
C7C = os.path.join(ROOT, "..", "c7c")
TMP = tempfile.mkdtemp(prefix="c7graph-")
bad = 0; checks = 0

def T(name, deps=(), kind="executable", extra=""):
    d = "".join(f'        "{x}"\n' for x in deps)
    return f'target {kind} "{name}":\n    sources:\n        "{name}.c7"\n' + (f"    depends:\n{d}" if deps else "") + extra

def run(text, *flags):
    p = os.path.join(TMP, "g.txt"); open(p, "w").write(text)
    r = subprocess.run([C7C, "tool", "graph-json", p, *flags], capture_output=True, text=True, timeout=30)
    return r.returncode, (json.loads(r.stdout) if r.stdout.strip() else None), r.stderr.replace(p, "g.txt")

def fail(m):
    global bad
    bad += 1; print("FAIL", m)

def order(text, want, *flags, label):
    global checks
    checks += 1
    rc, g, err = run(text, *flags)
    if rc != 0: fail(f"{label}: rc={rc} {err.strip()[:200]}"); return
    if g["order"] != want: fail(f"{label}: order {g['order']} want {want}")
    pos = {n: i for i, n in enumerate(g["order"])}
    for n in g["nodes"]:
        for d in n["deps"]:
            if pos[d] >= pos[n["name"]]: fail(f"{label}: {d} must come before {n['name']}")

def error(text, code, where, *flags, label, frag=""):
    global checks
    checks += 1
    rc, g, err = run(text, *flags)
    line = next((l for l in err.splitlines() if f"error[{code}]" in l), "")
    if rc != 1 or not line.startswith(f"g.txt:{where}: error[{code}]") or frag not in line: fail(f"{label}: rc={rc} want {code} at {where} ({frag}); got {err.strip()[:250]}")

P = 'project "G"\n'
order(P + T("a"), ["a"], label="single")
order(P + T("app", ["lib"]) + T("lib", kind="library"), ["lib", "app"], label="dependency declared later")
order(P + T("a") + T("b") + T("c"), ["a", "b", "c"], label="independent targets keep declaration order")
# diamond: app -> (left, right) -> base   (indices: app0 left1 right2 base3)
order(P + T("app", ["left", "right"]) + T("left", ["base"], "library") + T("right", ["base"], "library") + T("base", kind="library"),
      ["base", "left", "right", "app"], label="diamond")
# chain declared backwards: c0 -> b1 -> a2
order(P + T("c", ["b"]) + T("b", ["a"]) + T("a"), ["a", "b", "c"], label="chain")
# a `dependency` is a node that precedes its users
order(P + 'dependency "Ext" "^1.0"\n' + T("app", ["Ext"]), ["Ext", "app"], label="external dependency")
# determinism: running twice gives identical bytes; a permuted declaration changes the order exactly as predicted
t = P + T("x", ["z"]) + T("y") + T("z")
r1 = run(t); r2 = run(t); checks += 1
if r1[1] != r2[1]: fail("not deterministic")
order(t, ["y", "z", "x"], label="ready set picks the lowest declaration index (y before z)")
order(P + T("y") + T("z") + T("x", ["z"]), ["y", "z", "x"], label="permuted declaration")

# outputs and platform are reported per target; host and target platforms are separate
checks += 1
rc, g, err = run(P + T("app", extra='    output "out/app.bin"\n') + T("lib", kind="library"), "--out-dir", "bld")
n = {x["name"]: x for x in g["nodes"]}
if n["app"]["output"] != "out/app.bin" or n["lib"]["output"] != "bld/lib-0.0.0.l7" or n["app"]["platform"] != "x86_64-linux" or g["host"] != "x86_64-linux": fail(f"outputs: {g}")
checks += 1
rc, g, err = run('project "G":\n    version "2.1.0"\n' + T("lib", kind="library"))
if g["nodes"][0]["output"] != "build/lib-2.1.0.l7": fail(f"library output name: {g['nodes'][0]['output']}")

# errors: line:col of the target header (cycles: the node that closes the loop) or of the depends entry
error(P + T("a", ["a"]), "E0706", "2:1", label="self cycle", frag="a -> a")
error(P + T("a", ["b"]) + T("b", ["a"]), "E0706", "7:1", label="direct cycle", frag="a -> b -> a")
error(P + T("a", ["b"]) + T("b", ["c"]) + T("c", ["a"]), "E0706", "12:1", label="indirect cycle", frag="a -> b -> c -> a")
error(P + T("x") + T("a", ["b"]) + T("b", ["c"]) + T("c", ["b"]), "E0706", "15:1", label="cycle not including the entry node", frag="b -> c -> b")
error(P + T("a", ["nope"]), "E0716", "6:9", label="unknown depends name", frag="`nope`")
error(P + T("a", extra='    output "same"\n') + T("b", extra='    output "same"\n'), "E0707", "6:1", label="two targets, one output", frag="`same` is produced by both `a` and `b`")
error(P + T("a", extra='    output "b.c7"\n') + T("b"), "E0707", "2:1", label="output equals a source", frag="source of `b`")
error(P + T("app") + T("lib", kind="library", extra='    output "build/app"\n'), "E0707", "5:1", label="explicit output equals another default output", frag="`build/app`")
error(P + T("a", extra='    platform "aarch64-linux"\n'), "E0715", "2:1", label="unsupported platform", frag="`aarch64-linux`")
error(P + T("a", extra='    platform "x86_64-macos"\n'), "E0715", "2:1", label="unsupported OS", frag="host `x86_64-linux`")
checks += 1
rc, g, err = run(P + T("a", extra='    platform "x86_64-windows"\n') + T("l", kind="library"))
n = {x["name"]: x for x in g["nodes"]}
if rc != 0 or n["a"]["platform"] != "x86_64-windows" or n["a"]["output"] != "build/a.exe" or n["l"]["output"] != "build/l-0.0.0.l7": fail(f"windows target: {g}")
checks += 1
rc, g, err = run(P + T("a", extra='    platform "x86_64-linux"\n'), "--host", "aarch64-linux")
if rc != 0 or g["host"] != "aarch64-linux" or g["nodes"][0]["platform"] != "x86_64-linux": fail(f"explicit target platform on another host (cross description): {g} {err[:100]}")
# a bigger acyclic graph stays fast and valid
big = P + "".join(T(f"t{i}", [f"t{i-1}"] if i else []) for i in range(2000))
checks += 1
rc, g, err = run(big)
if rc != 0 or g["order"] != [f"t{i}" for i in range(2000)]: fail("2000-node chain")
bigcyc = P + "".join(T(f"t{i}", [f"t{(i+1) % 2000}"]) for i in range(2000))
checks += 1
rc, g, err = run(bigcyc)
if rc != 1 or "E0706" not in err: fail(f"2000-node cycle: rc={rc} {err[:120]}")
print(f"graph: {checks} checks, problems={bad}")
sys.exit(1 if bad else 0)
