#!/usr/bin/env python3
"""Options (W6): .cset7 corpus (valid / invalid / warnings), exhaustive five-level precedence, flag semantics against the
real driver, compiler/ABI compatibility checks. Expected values are hand-written (no external oracle exists)."""
import itertools, json, os, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.abspath(__file__))
C7C = os.path.abspath(os.path.join(ROOT, "..", "c7c"))
CS = os.path.join(ROOT, "cset7")
TMP = tempfile.mkdtemp(prefix="c7opt-")
bad = 0; checks = 0

def fail(msg):
    global bad
    bad += 1; print("FAIL", msg)

def opts(*a):
    r = subprocess.run([C7C, "tool", "options-json", *a], capture_output=True, text=True, timeout=30)
    return r.returncode, (json.loads(r.stdout) if r.stdout.strip() else None), r.stderr

def put(name, text):
    p = os.path.join(TMP, name); open(p, "w").write(text); return p

# ---- .cset7 corpus
for n in sorted(os.listdir(os.path.join(CS, "valid"))):
    if not n.endswith(".cset7"): continue
    checks += 1
    rc, got, err = opts("--cset7", os.path.join(CS, "valid", n))
    if rc != 0: fail(f"cset7/valid/{n}: rejected: {err.strip()[:200]}"); continue
    want = json.load(open(os.path.join(CS, "valid", n[:-6] + ".json")))
    if got != want: fail(f"cset7/valid/{n}:\n  got  {got}\n  want {want}")
for n in sorted(os.listdir(os.path.join(CS, "invalid"))):
    if not n.endswith(".cset7"): continue
    checks += 1
    code, loc = open(os.path.join(CS, "invalid", n[:-6] + ".expect")).read().split()
    path = os.path.join(CS, "invalid", n)
    rc, got, err = opts("--cset7", path)
    if rc != 1 or f"{path}:{loc}: error[{code}]" not in err: fail(f"cset7/invalid/{n}: rc={rc} want {code} at {loc}; got {err.strip()[:250]}")
for n in sorted(os.listdir(os.path.join(CS, "warn"))):
    if not n.endswith(".cset7"): continue
    checks += 1
    code, loc = open(os.path.join(CS, "warn", n[:-6] + ".expect")).read().split()
    path = os.path.join(CS, "warn", n)
    rc, got, err = opts("--cset7", path)
    if rc != 0 or f"{path}:{loc}: warning[{code}]" not in err: fail(f"cset7/warn/{n}: rc={rc} want warning {code} at {loc}; got {err.strip()[:250]}")

# ---- precedence: defaults < .cset7 < config < target < command line, every subset of the four settable layers
LAYERS = ["cset7", "config", "target", "cli"]
DEFAULTS = {"optimization": "O0", "mode": "release", "bounds_check": False, "debug_alloc": False}
CLI = {"optimization": lambda v: ["-" + v], "mode": lambda v: ["--mode=" + v],
       "bounds_check": lambda v: ["--bounds-check" if v else "--no-bounds-check"], "debug_alloc": lambda v: ["--debug-alloc" if v else "--no-debug-alloc"]}
def toml_val(v): return json.dumps(v)
def txt_val(v): return {True: "true", False: "false"}.get(v, json.dumps(v))

def check_precedence(prop, values):  # values: one value per layer in LAYERS order
    global checks
    for r in range(len(LAYERS) + 1):
        for subset in itertools.combinations(range(len(LAYERS)), r):
            names = [LAYERS[i] for i in subset]
            args = []
            cset = "[build]\n" + (f"{prop} = {toml_val(values[0])}\n" if "cset7" in names else "")
            bf = 'project "P"\nconfig c:\n' + (f"    {prop} {txt_val(values[1])}\n" if "config" in names else "") + \
                 'target executable "t":\n    sources:\n        "a.c7"\n' + (f"    {prop} {txt_val(values[2])}\n" if "target" in names else "")
            args = ["--cset7", put("p.cset7", cset), "--build", put("p.txt", bf), "--config", "c", "--target", "t"]
            if "cli" in names: args += CLI[prop](values[3])
            want = DEFAULTS[prop]
            for i in subset: want = values[i]   # later (higher) layers override earlier ones
            checks += 1
            rc, got, err = opts(*args)
            if rc != 0: fail(f"precedence {prop} {values} {names}: rc={rc} {err.strip()[:160]}"); continue
            if got[prop] != want: fail(f"precedence {prop} {values} layers={names}: got {got[prop]!r} want {want!r}")

for perm in itertools.permutations(["O0", "O1", "O2", "O3"]): check_precedence("optimization", list(perm))
for bits in itertools.product([False, True], repeat=4):
    check_precedence("bounds_check", list(bits))
    check_precedence("debug_alloc", list(bits))
for bits in itertools.product(["debug", "release"], repeat=4): check_precedence("mode", list(bits))
# overflow_check resolves against `mode`, so test it on its own: explicit value from the highest layer, else mode == debug
for bits in itertools.product([False, True], repeat=4):
    for r in range(5):
        for subset in itertools.combinations(range(4), r):
            names = [LAYERS[i] for i in subset]
            cset = "[build]\n" + (f"overflow_check = {toml_val(bits[0])}\n" if "cset7" in names else "")
            bf = 'project "P"\nconfig c:\n' + (f"    overflow_check {txt_val(bits[1])}\n" if "config" in names else "") + \
                 'target executable "t":\n    sources:\n        "a.c7"\n' + (f"    overflow_check {txt_val(bits[2])}\n" if "target" in names else "")
            args = ["--cset7", put("p.cset7", cset), "--build", put("p.txt", bf), "--config", "c", "--target", "t"]
            if "cli" in names: args += ["--overflow-check" if bits[3] else "--no-overflow-check"]
            want = False
            for i in subset: want = bits[i]
            checks += 1
            rc, got, err = opts(*args)
            if rc != 0 or got["overflow_check"] != want: fail(f"precedence overflow_check {bits} {names}: got {got and got['overflow_check']} want {want}")
            # and with mode=debug at the lowest explicit layer, an unset overflow_check follows the mode
            if not subset:
                rc, got, err = opts("--mode=debug")
                if got["overflow_check"] is not True: fail("mode debug must imply overflow_check")

# default_config picks the config when --config is not given; the CLI beats everything
bf = put("d.txt", 'project "P"\nconfig a:\n    optimization "O1"\nconfig b:\n    optimization "O2"\ndefault_config b\n')
checks += 2
rc, got, _ = opts("--build", bf)
if got["optimization"] != "O2": fail("default_config not used")
rc, got, _ = opts("--build", bf, "--config", "a")
if got["optimization"] != "O1": fail("--config must beat default_config")
# list options are additive in precedence order, deduplicated
cs = put("l.cset7", '[paths]\ninclude = ["a", "b"]\nregistry = ["r1"]\n')
bf = put("l.txt", 'project "P"\ntarget executable "t":\n    sources:\n        "a.c7"\n    include_dirs:\n        "b"\n        "c"\n')
rc, got, _ = opts("--cset7", cs, "--build", bf, "--target", "t", "--registry=r2", "--registry=r1")
checks += 1
if got["include"] != ["a", "b", "c"] or got["registry"] != ["r1", "r2"]: fail(f"list merge: {got['include']} {got['registry']}")

# ---- compatibility checks
for name, text, ok in [("abi_ok", '[target]\nabi = "c7abi1"\n', True), ("abi_other", '[target]\nabi = "c7abi2"\n', False),
                       ("req_ok", '[compiler]\nrequires = ">=1.0.0-alpha.1"\n', True), ("req_prerelease_rule", '[compiler]\nrequires = ">=1.0.0"\n', False),
                       ("req_future", '[compiler]\nrequires = ">=2.0.0-0"\n', False)]:
    checks += 1
    rc, got, err = opts("--cset7", put(name + ".cset7", text))
    if (rc == 0) != ok or (not ok and "[E0719]" not in err): fail(f"compat {name}: rc={rc} {err.strip()[:160]}")

# ---- flags really mean what they say: run programs from tests/chk through the real driver
def chk(flags, prog, want_rc, label):
    global checks
    checks += 1
    exe = os.path.join(TMP, "chk.out")
    b = subprocess.run([C7C, "build", os.path.join(ROOT, "chk", prog + ".c7"), "-o", exe, *flags], capture_output=True, text=True)
    if b.returncode != 0: fail(f"{label}: build failed {b.stderr[:200]}"); return
    rc = subprocess.run([exe], capture_output=True).returncode
    if rc < 0: rc = 128 - rc   # killed by a signal: shell-style exit status
    if rc != want_rc: fail(f"{label}: {prog} exit {rc}, want {want_rc} (flags {flags})")
ovf = [f[:-3] for f in os.listdir(os.path.join(ROOT, "chk")) if f.startswith("ovf_") and f.endswith(".c7")][0]
bnd = [f[:-3] for f in os.listdir(os.path.join(ROOT, "chk")) if f.startswith("bnd_") and f.endswith(".c7")][0]
trap_rc = int(open(os.path.join(ROOT, "chk", ovf + ".rc")).read())
for args, want in [(["--mode=debug"], trap_rc), ([], 0), (["--overflow-check"], trap_rc), (["--mode=debug", "--no-overflow-check"], 0)]:
    rc, got, err = opts(*args)
    chk(got["flags"], ovf, want, f"overflow via options {args}")
trap_b = int(open(os.path.join(ROOT, "chk", bnd + ".rc")).read())
for args, want in [(["--bounds-check"], trap_b), ([], 0)]:
    rc, got, err = opts(*args)
    chk(got["flags"], bnd, want, f"bounds via options {args}")
# --link: static really produces a static executable (no program interpreter), dynamic a dynamic one; both run correctly
for link, want_interp in (("static", False), ("dynamic", True)):
    for mode, o in itertools.product(("debug", "release"), ("O0", "O2")):
        checks += 1
        rc, got, err = opts("--mode=" + mode, "-" + o, "--link=" + link, "--bounds-check", "--debug-alloc")
        exe = os.path.join(TMP, "hello.out")
        b = subprocess.run([C7C, "build", os.path.join(ROOT, "run", "hello.c7"), "-o", exe, *got["flags"]], capture_output=True, text=True)
        if b.returncode != 0: fail(f"link {link} {mode} {o}: build failed {b.stderr[:200]}"); continue
        out = subprocess.run([exe], capture_output=True, text=True).stdout
        if out != open(os.path.join(ROOT, "run", "hello.expected")).read().rstrip("\n") + "\n" and out.rstrip("\n") != open(os.path.join(ROOT, "run", "hello.expected")).read().rstrip("\n"):
            fail(f"link {link} {mode} {o}: wrong output"); continue
        has = b"INTERP" in subprocess.run(["readelf", "-lW", exe], capture_output=True).stdout
        if has != want_interp: fail(f"--link={link}: executable {'has' if has else 'lacks'} a program interpreter")
print(f"options: {checks} checks, problems={bad}")
sys.exit(1 if bad else 0)
