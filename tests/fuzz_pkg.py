#!/usr/bin/env python3
"""Mutation fuzzer for the stage-5 parsers (TOML, ZIP/DEFLATE, .l7, and the build-file/.cset7/.cr7 parsers once they
exist). It feeds randomly damaged copies of valid inputs to `c7c tool ...` and fails on anything that is not a clean
accept (rc 0) or clean reject (rc 1): crashes, aborts, sanitizer reports, timeouts, runaway memory.
Usage: fuzz_pkg.py [iterations=5000] [seed=1] [compiler=../c7c]   (use ../c7c_asan to also catch memory errors)"""
import os, random, resource, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.abspath(__file__))
N = int(sys.argv[1]) if len(sys.argv) > 1 else 5000
SEED = int(sys.argv[2]) if len(sys.argv) > 2 else 1
C7C = os.path.abspath(sys.argv[3]) if len(sys.argv) > 3 else os.path.join(ROOT, "..", "c7c")
TMP = tempfile.mkdtemp(prefix="c7fuzz-")
rnd = random.Random(SEED)
MEM = 3 << 30  # ASan reserves a lot of address space, so this is generous; a bomb still trips it
bad = 0

def limits():
    if "asan" not in os.path.basename(C7C):  # ASan maps terabytes of shadow memory, so an address-space cap cannot be used with it
        resource.setrlimit(resource.RLIMIT_AS, (MEM, MEM))
    resource.setrlimit(resource.RLIMIT_CPU, (10, 10))

def run(sub, path, *extra):
    sub, _, pre = sub.partition(" ")   # "options-json --cset7" -> the file follows the option
    try:
        r = subprocess.run([C7C, "tool", sub, *([pre] if pre else []), path, *extra], capture_output=True, timeout=20, preexec_fn=limits)
    except subprocess.TimeoutExpired:
        return "timeout", b""
    return r.returncode, r.stderr

def tool_rc(*a):
    return subprocess.run([C7C, "tool", *a], capture_output=True).returncode

def read_all(d, exts):
    out = []
    for dp, _, fs in os.walk(d):
        for f in sorted(fs):
            if f.endswith(exts): out.append(open(os.path.join(dp, f), "rb").read())
    return out

INTERESTING = [b"[", b"]", b"{", b"}", b'"', b"'", b"\\", b"\n", b"\r", b"\0", b"=", b",", b".", b"#", b"\xff", b"\xc3", b"0x", b"9" * 40,
               b"[" * 300, b"{a=" * 300, b'"""', b"'''", b"\t", b"-", b":", b"\\u", b"\\U", b"e+", b"1_", b"inf", b"nan"]

def mutate(data):
    b = bytearray(data)
    for _ in range(rnd.randint(1, 4)):
        k = rnd.random()
        if not b: b += rnd.choice(INTERESTING); continue
        i = rnd.randrange(len(b))
        if k < 0.25: b[i] ^= 1 << rnd.randrange(8)
        elif k < 0.40: b[i] = rnd.randrange(256)
        elif k < 0.55: del b[i:i + rnd.randint(1, 16)]
        elif k < 0.75: b[i:i] = rnd.choice(INTERESTING)
        elif k < 0.85:
            j = rnd.randrange(len(b)); n = rnd.randint(1, 32); b[i:i] = b[j:j + n]
        elif k < 0.92: b[i:] = b""  # truncate
        else:
            j = rnd.randrange(len(b)); b[i], b[j] = b[j], b[i]
    return bytes(b)

# ---- seeds
toml_seeds = read_all(os.path.join(ROOT, "toml", "valid"), (".toml",))
tdir = os.path.join(ROOT, "build")
build_seeds = read_all(os.path.join(tdir, "valid"), (".txt",)) if os.path.isdir(tdir) else []
cset_seeds = read_all(os.path.join(ROOT, "cset7", "valid"), (".cset7",)) if os.path.isdir(os.path.join(ROOT, "cset7")) else []

def make_l7():
    src = os.path.join(TMP, "seed.c7"); open(src, "w").write("i32 one() { return 1; }\n")
    out = os.path.join(TMP, "seed.l7")
    assert tool_rc("l7-pack", out, "Seed", "1.0.0", "--dep", "Other", "^1.2", "src/lib.c7=" + src) == 0, "cannot build seed .l7"
    return open(out, "rb").read()
l7_seed = make_l7()
zip_path = os.path.join(TMP, "seed.zip")
assert tool_rc("zip-create", zip_path, "a.txt=" + src if (src := os.path.join(TMP, "seed.c7")) else "") == 0
zip_seed = open(zip_path, "rb").read()
raw = b"hello hello hello hello " * 50 + bytes(range(256))
rp = os.path.join(TMP, "raw.bin"); open(rp, "wb").write(raw)
dp = os.path.join(TMP, "raw.def"); open(dp, "wb").write(subprocess.run([C7C, "tool", "deflate", rp], capture_output=True).stdout)
deflate_seed = open(dp, "rb").read()

targets = [("toml-json", toml_seeds, ".toml"), ("toml-roundtrip", toml_seeds, ".toml"),
           ("zip-list", [zip_seed], ".zip"), ("l7-verify", [l7_seed], ".l7"), ("inflate", [deflate_seed], ".bin")]
if build_seeds: targets += [("buildfile-json", build_seeds, ".txt"), ("graph-json", build_seeds, ".txt")]
if cset_seeds: targets.append(("options-json --cset7", cset_seeds, ".cset7"))

for it in range(N):
    sub, seeds, ext = rnd.choice(targets)
    data = mutate(rnd.choice(seeds))
    p = os.path.join(TMP, "case" + ext); open(p, "wb").write(data)
    rc, err = run(sub, p)
    if rc not in (0, 1) or b"Sanitizer" in err or b"runtime error" in err:
        bad += 1
        keep = os.path.join(TMP, f"crash{bad}{ext}"); open(keep, "wb").write(data)
        print(f"FAIL iteration {it}: {sub} rc={rc} -> {keep}\n  {err[-300:]!r}")
        if bad >= 10: break
print(f"fuzz: {it + 1 if N else 0} iterations seed={SEED} problems={bad}")
sys.exit(1 if bad else 0)
