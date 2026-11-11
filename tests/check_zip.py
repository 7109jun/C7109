#!/usr/bin/env python3
"""Cross-check c7c's inflate/deflate/ZIP code against zlib, zipfile and unzip, and feed the reader hostile archives."""
import io, os, random, struct, subprocess, sys, tempfile, warnings, zipfile, zlib
warnings.simplefilter('ignore')

ROOT = os.path.dirname(os.path.abspath(__file__))
C7C = os.path.join(ROOT, "..", "c7c")
TMP = tempfile.mkdtemp(prefix="c7zip-")
bad = 0

def fail(msg):
    global bad
    bad += 1
    print("FAIL", msg)

def tool(*args, binary_out=False):
    r = subprocess.run([C7C, "tool", *args], capture_output=True)
    return r.returncode, r.stdout, r.stderr.decode("utf-8", "replace")

def write(name, data):
    p = os.path.join(TMP, name)
    open(p, "wb").write(data)
    return p

random.seed(12345)
text = (b"The quick brown fox jumps over the lazy dog. " * 40) + bytes(random.randrange(256) for _ in range(300))
datasets = {
    "empty": b"", "one": b"x", "short": b"hello hello hello",
    "text": text, "zeros_1m": bytes(1 << 20), "random_20k": bytes(random.randrange(256) for _ in range(20000)),
    "runs": b"".join(bytes([i % 7]) * (i % 300 + 1) for i in range(400)),
    "words": b" ".join(random.choice([b"alpha", b"beta", b"gamma", b"delta", b"epsilon"]) for _ in range(5000)),
}

# 1. inflate: every zlib level and strategy (stored, fixed and dynamic blocks all occur)
n_inf = 0
for name, data in datasets.items():
    for level in range(0, 10):
        for strat in (zlib.Z_DEFAULT_STRATEGY, zlib.Z_FILTERED, zlib.Z_HUFFMAN_ONLY, zlib.Z_RLE, zlib.Z_FIXED):
            c = zlib.compressobj(level, zlib.DEFLATED, -15, 9, strat)
            raw = c.compress(data) + c.flush()
            rc, out, err = tool("inflate", write("in.raw", raw))
            n_inf += 1
            if rc != 0 or out != data:
                fail(f"inflate {name} level={level} strategy={strat}: rc={rc} {err.strip()}")

# 2. deflate: zlib must decode our output; the encoder must be deterministic and actually compress
for name, data in datasets.items():
    p = write("in.bin", data)
    rc, z1, _ = tool("deflate", p)
    rc2, z2, _ = tool("deflate", p)
    if rc != 0 or z1 != z2: fail(f"deflate {name}: not deterministic"); continue
    try:
        if zlib.decompress(z1, -15) != data: fail(f"deflate {name}: zlib decodes different data")
    except Exception as e:
        fail(f"deflate {name}: zlib rejects our stream: {e}")
    if name in ("text", "zeros_1m", "words") and len(z1) > len(data) * 0.5: fail(f"deflate {name}: poor ratio {len(z1)}/{len(data)}")

# 3. our ZIP writer: zipfile, unzip -t and the reader must all agree
files = {"manifest.toml": b'name = "x"\n', "src/a.c7": text, "src/deep/b.c7": b"", "lib/한국어.txt": "유니코드".encode(), "big.bin": datasets["random_20k"]}
specs = []
for n, d in files.items():
    specs.append(f"{n}={write('f' + str(len(specs)), d)}")
zp = os.path.join(TMP, "ours.zip")
rc, _, err = tool("zip-create", zp, *specs)
if rc != 0: fail(f"zip-create: {err}")
else:
    with zipfile.ZipFile(zp) as z:
        if z.testzip() is not None: fail("zipfile.testzip reports a bad entry")
        if z.namelist() != list(files): fail(f"entry order/names differ: {z.namelist()}")
        for n, d in files.items():
            if z.read(n) != d: fail(f"zipfile reads different data for {n}")
        if any(i.date_time != (1980, 1, 1, 0, 0, 0) for i in z.infolist()): fail("timestamps are not fixed")
    if subprocess.run(["unzip", "-tq", zp], capture_output=True).returncode != 0: fail("unzip -t rejects our archive")
    rc, out, _ = tool("zip-list", zp)
    got = [l.split("\t") for l in out.decode().splitlines()]
    if [(g[0], int(g[1])) for g in got] != [(n, len(d)) for n, d in files.items()]: fail("zip-list of our own archive differs")
    # reproducible: a second run must produce the identical bytes
    zp2 = os.path.join(TMP, "ours2.zip")
    tool("zip-create", zp2, *specs)
    if open(zp, "rb").read() != open(zp2, "rb").read(): fail("zip output is not reproducible")

# 4. our ZIP reader on archives made by zipfile
def make(entries, comp=zipfile.ZIP_DEFLATED, **kw):
    b = io.BytesIO()
    with zipfile.ZipFile(b, "w", comp, **kw) as z:
        for e in entries:
            if isinstance(e, zipfile.ZipInfo): z.writestr(e, b"payload")
            else: z.writestr(e[0], e[1])
    return b.getvalue()

good = make([("a.txt", b"hello"), ("dir/", b""), ("dir/b.txt", text), ("ünï.txt", b"u")])
rc, out, err = tool("zip-list", write("py.zip", good))
names = [l.split("\t")[0] for l in out.decode().splitlines()]
if rc != 0 or names != ["a.txt", "dir/b.txt", "ünï.txt"]: fail(f"reader on a zipfile archive: rc={rc} names={names} {err}")
for comp in (zipfile.ZIP_STORED,):
    rc, out, err = tool("zip-cat", write("st.zip", make([("s.bin", datasets["random_20k"])], comp)), "s.bin")
    if rc != 0 or out != datasets["random_20k"]: fail("reader on a stored entry")

# 5. hostile archives: every one must be rejected, with a message, without crashing
def rejects(label, data, expect=None):
    rc, _, err = tool("zip-list", write("evil.zip", data))
    if rc != 1: fail(f"{label}: expected rejection, rc={rc}")
    elif expect and expect not in err: fail(f"{label}: wrong reason: {err.strip()}")

def zi(name):
    i = zipfile.ZipInfo(name); i.compress_type = zipfile.ZIP_STORED; return i

rejects("path traversal", make([zi("../evil.txt")]), "unsafe")
rejects("nested traversal", make([zi("a/../../evil.txt")]), "unsafe")
rejects("absolute path", make([zi("/etc/passwd")]), "unsafe")
rejects("backslash", make([zi("a\\b.txt")]), "unsafe")
rejects("drive letter", make([zi("C:evil.txt")]), "unsafe")
rejects("empty segment", make([zi("a//b.txt")]), "unsafe")
rejects("duplicate", make([zi("same.txt"), zi("same.txt")]), "duplicate")
rejects("case collision", make([zi("README.md"), zi("readme.md")]), "duplicate")
rejects("file vs dir collision", make([zi("a"), zi("a/")]), "duplicate")
rejects("bzip2", make([("x", b"data")], zipfile.ZIP_BZIP2), "method")
# encrypted flag: flip general-purpose bit 0 in both headers
enc = bytearray(make([("x", b"data")]))
for sig in (b"PK\x03\x04", b"PK\x01\x02"):
    i = enc.find(sig); off = 6 if sig == b"PK\x03\x04" else 8
    enc[i + off] |= 1
rejects("encrypted flag", bytes(enc), "encrypted")
# corrupt CRC / data
st = bytearray(make([("x.bin", b"A" * 100)], zipfile.ZIP_STORED))
st[st.find(b"A" * 100) + 5] ^= 0xFF
rejects("bad CRC", bytes(st), "CRC")
dfl = bytearray(make([("x.txt", text)]))
dfl[40] ^= 0x55
rejects("corrupt deflate data", bytes(dfl))
good_zip = make([("x.txt", text)])
rejects("truncated", good_zip[: len(good_zip) // 2])
rejects("not a zip", b"this is not a zip file at all, just text")
rejects("empty file", b"")
# lying size: central directory claims 10 bytes, the stream inflates to 1000
lie = bytearray(make([("z.bin", bytes(1000))]))
for sig, off in ((b"PK\x01\x02", 24), (b"PK\x03\x04", 22)):
    i = lie.find(sig); lie[i + off:i + off + 4] = struct.pack("<I", 10)
rejects("declared size smaller than content", bytes(lie))
# oversize entry: 257 MiB of zeros compresses to ~250 KiB but must be refused by the size limit
big = io.BytesIO()
with zipfile.ZipFile(big, "w", zipfile.ZIP_DEFLATED) as z: z.writestr("bomb.bin", bytes(257 << 20))
rejects("size limit", big.getvalue(), "size limit")
# zip64: a central directory entry whose sizes are the 0xFFFFFFFF "see zip64 extra field" marker
z64 = bytearray(make([("x", b"data")], zipfile.ZIP_STORED))
i = z64.find(b"PK\x01\x02")
z64[i + 20:i + 28] = b"\xff" * 8
rejects("zip64 entry", bytes(z64), "zip64")
# a local-header-only zip64 field (what zipfile's force_zip64 writes) is a normal archive and must still be readable
lz = io.BytesIO()
with zipfile.ZipFile(lz, "w", zipfile.ZIP_STORED) as z:
    with z.open("x", "w", force_zip64=True) as f: f.write(b"data")
rc, out, err = tool("zip-cat", write("lz.zip", lz.getvalue()), "x")
if rc != 0 or out != b"data": fail(f"archive with a local-only zip64 field should be readable: {err}")
# our own writer must refuse unsafe names too
rc, _, err = tool("zip-create", os.path.join(TMP, "w.zip"), f"../up={write('u', b'x')}")
if rc == 0: fail("zip writer accepted `../up`")

print(f"zip: {n_inf} inflate cases, {len(datasets)} deflate cases, hostile archives checked, problems={bad}")
sys.exit(1 if bad else 0)
