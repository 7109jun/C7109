#!/usr/bin/env python3
"""Verify .l7 handling: our packer vs an independent Python implementation of the format, reproducibility, extraction,
and a set of tampered packages that must all be rejected."""
import hashlib, io, os, subprocess, sys, tempfile, tomllib, warnings, zipfile
warnings.simplefilter("ignore")

ROOT = os.path.dirname(os.path.abspath(__file__))
C7C = os.path.join(ROOT, "..", "c7c")
TMP = tempfile.mkdtemp(prefix="c7l7-")
bad = 0

def fail(msg):
    global bad
    bad += 1
    print("FAIL", msg)

def tool(*a):
    r = subprocess.run([C7C, "tool", *a], capture_output=True, text=True)
    return r.returncode, r.stdout, r.stderr

def write(name, data):
    p = os.path.join(TMP, name)
    open(p, "wb").write(data if isinstance(data, bytes) else data.encode())
    return p

MANIFEST = '''format_version = 1

[package]
name = "Pyref"
version = "2.3.4-rc.1"
license = "MIT"
abi = "c7abi1"

[[dependency]]
name = "Other"
version = "^1.2"
'''

def build(entries=None, manifest=MANIFEST, checksums="auto", extra_zip=None, drop=()):
    """An independent implementation of the container format, used to build good and deliberately broken packages."""
    entries = dict({"src/lib.c7": "i32 one() { return 1; }\n", "licenses/LICENSE": "MIT\n"} if entries is None else entries)
    files = {"manifest.toml": manifest, **entries}
    for d in drop: files.pop(d, None)
    files = {k: (v if isinstance(v, bytes) else v.encode()) for k, v in files.items()}
    if checksums == "auto":
        checksums = "".join(f"{hashlib.sha256(v).hexdigest()}  {k}\n" for k, v in sorted(files.items()))
    if checksums is not None: files["checksums.sha256"] = checksums if isinstance(checksums, bytes) else checksums.encode()
    b = io.BytesIO()
    with zipfile.ZipFile(b, "w", zipfile.ZIP_DEFLATED) as z:
        for k, v in files.items(): z.writestr(zipfile.ZipInfo(k, (1980, 1, 1, 0, 0, 0)), v)
        for k, v in (extra_zip or {}).items(): z.writestr(zipfile.ZipInfo(k, (1980, 1, 1, 0, 0, 0)), v)
    return b.getvalue()

def verify(data, name="p.l7"):
    return tool("l7-verify", write(name, data))

# 1. a package built independently in Python must be accepted
rc, out, err = verify(build())
if rc != 0 or "Pyref 2.3.4-rc.1" not in out: fail(f"independent package rejected: {out} {err}")
# unknown manifest keys are only warnings
rc, out, err = verify(build(manifest=MANIFEST + '\n[extras]\nx = 1\n'))
if rc != 0 or "W0701" not in err: fail(f"unknown key should warn but pass: rc={rc} {err}")

# 2. our packer: structure, manifest round-trip, checksum file usable by sha256sum, reproducibility
srcs = {"src/main.c7": "i32 main() { return 0; }\n", "src/util/helpers.c7": "void h() {}\n", "licenses/LICENSE": "GPL\n"}
args = [f"{k}={write('s' + str(i), v)}" for i, (k, v) in enumerate(srcs.items())]
p1 = os.path.join(TMP, "ours1.l7")
rc, _, err = tool("l7-pack", p1, "Ours", "1.2.3", "--license", "GPL-3.0-only", "--dep", "Dep", "^2.0", "--bin", "linux", "x86_64", "c7abi1", "lib/linux-x86_64/libours.a", *args, f"lib/linux-x86_64/libours.a={write('lib', b'AR')}")
if rc != 0: fail(f"l7-pack failed: {err}")
else:
    with zipfile.ZipFile(p1) as z:
        names = z.namelist()
        if names[0] != "manifest.toml" or names[-1] != "checksums.sha256" or names[1:-1] != sorted(names[1:-1]): fail(f"entry order: {names}")
        m = tomllib.loads(z.read("manifest.toml").decode())
        if m["package"] != {"name": "Ours", "version": "1.2.3", "license": "GPL-3.0-only", "abi": "c7abi1"}: fail(f"manifest package table: {m['package']}")
        if m["dependency"] != [{"name": "Dep", "version": "^2.0"}]: fail("manifest dependency table")
        if m["binary"] != [{"os": "linux", "architecture": "x86_64", "abi": "c7abi1", "path": "lib/linux-x86_64/libours.a"}]: fail("manifest binary table")
        ex = os.path.join(TMP, "extracted")
        z.extractall(ex)
    r = subprocess.run(["sha256sum", "-c", "checksums.sha256"], cwd=ex, capture_output=True, text=True)
    if r.returncode != 0: fail(f"sha256sum -c rejects our checksums.sha256: {r.stdout}{r.stderr}")
    rc, out, err = tool("l7-verify", p1)
    if rc != 0 or "sources=2" not in out: fail(f"verify of our package: {out} {err}")
    # same content in a different argument order -> identical bytes
    p2 = os.path.join(TMP, "ours2.l7")
    tool("l7-pack", p2, "Ours", "1.2.3", "--license", "GPL-3.0-only", "--dep", "Dep", "^2.0", "--bin", "linux", "x86_64", "c7abi1", "lib/linux-x86_64/libours.a", *reversed(args), f"lib/linux-x86_64/libours.a={write('lib', b'AR')}")
    if open(p1, "rb").read() != open(p2, "rb").read(): fail("package is not reproducible / depends on argument order")
    # safe extraction
    dest = os.path.join(TMP, "x1")
    rc, _, err = tool("l7-extract", p1, dest)
    if rc != 0 or open(os.path.join(dest, "src/util/helpers.c7")).read() != srcs["src/util/helpers.c7"]: fail(f"extract: {err}")
    rc, _, err = tool("l7-extract", p1, dest)
    if rc == 0: fail("extract overwrote an existing directory")

# 3. packing refuses bad input
rc, _, err = tool("l7-pack", os.path.join(TMP, "bad.l7"), "Bad", "1.0.0", f"../escape.c7={write('e', 'x')}")
if rc == 0 or "E0711" not in err: fail(f"packer accepted an unsafe path: {err}")
rc, _, err = tool("l7-pack", os.path.join(TMP, "bad.l7"), "Bad", "not-a-version", f"src/a.c7={write('e', 'x')}")
if rc == 0: fail("packer accepted an invalid version")
rc, _, err = tool("l7-pack", os.path.join(TMP, "bad.l7"), "Bad", "1.0.0", "--bin", "linux", "x86_64", "c7abi1", "lib/none.a", f"src/a.c7={write('e', 'x')}")
if rc == 0: fail("packer accepted a binary slot whose file is missing")

# 4. tampered packages: every one must be rejected, with the expected diagnostic
def rejects(label, data, code):
    rc, _, err = verify(data)
    if rc == 0: fail(f"{label}: accepted")
    elif code not in err: fail(f"{label}: expected {code}, got: {err.strip()[:200]}")

good_entries = {"src/lib.c7": "i32 one() { return 1; }\n", "licenses/LICENSE": "MIT\n"}
base = build()
rejects("modified source", build(entries={**good_entries, "src/lib.c7": "i32 one() { return 2; }\n"}, checksums="\n".join(
    f"{hashlib.sha256(v.encode() if isinstance(v, str) else v).hexdigest()}  {k}" for k, v in sorted({"manifest.toml": MANIFEST, **good_entries}.items())) + "\n"), "E0710")
orig_sums = "".join(f"{hashlib.sha256(v.encode()).hexdigest()}  {k}\n" for k, v in sorted({"manifest.toml": MANIFEST, **good_entries}.items()))
rejects("file missing from the checksum list", build(entries={**good_entries, "src/extra.c7": "x"}, checksums=orig_sums), "E0710")
rejects("listed file missing from the archive", build(entries=good_entries, checksums=orig_sums + "0" * 64 + "  src/ghost.c7\n"), "E0710")
rejects("malformed checksum line", build(checksums="not a checksum line\n"), "E0710")
rejects("duplicate checksum line", build(checksums=orig_sums + orig_sums.splitlines()[0] + "\n"), "E0710")
rejects("checksums.sha256 missing", build(checksums=None), "E0709")
rejects("manifest.toml missing", build(drop=["manifest.toml"], checksums=""), "E0709")
rejects("unsupported format_version", build(manifest=MANIFEST.replace("format_version = 1", "format_version = 2")), "E0709")
rejects("missing [package]", build(manifest="format_version = 1\n"), "E0709")
rejects("bad version", build(manifest=MANIFEST.replace("2.3.4-rc.1", "2.3")), "E0717")
rejects("bad dependency requirement", build(manifest=MANIFEST.replace("^1.2", "^^1")), "E0717")
rejects("invalid TOML", build(manifest=MANIFEST + "\nbroken = \n"), "E0701")
rejects("ABI mismatch", build(manifest=MANIFEST.replace("c7abi1", "c7abi9")), "E0709")
rejects("invalid package name", build(manifest=MANIFEST.replace('"Pyref"', '"../evil"')), "E0709")
rejects("unsatisfied compiler requirement", build(manifest=MANIFEST + '\n[compiler]\nrequires = ">=2.0.0"\n'), "E0709")
rejects("binary slot without its file", build(manifest=MANIFEST + '\n[[binary]]\nos = "linux"\narchitecture = "x86_64"\nabi = "c7abi1"\npath = "lib/missing.a"\n'), "E0709")
rejects("path traversal entry", build(extra_zip={"../evil.txt": "x"}), "E0711")
rejects("not a zip", b"definitely not a zip", "E0718")
rejects("truncated", base[: len(base) // 2], "E0718")
rc, _, err = tool("l7-verify", os.path.join(TMP, "does-not-exist.l7"))
if rc == 0 or "E0709" not in err: fail("missing file not diagnosed")

print(f"l7: packer, independent builder, extraction and 20 tampered packages checked, problems={bad}")
sys.exit(1 if bad else 0)
