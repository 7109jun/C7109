#!/usr/bin/env python3
"""SHA-256 against Python's hashlib (the independent oracle): every length 0..300 (covers all padding boundaries
55/56/63/64/65/119/120/128), a few large sizes, and all-0xFF / random content."""
import hashlib, os, random, subprocess, sys, tempfile

C7C = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "c7c")
tmp = tempfile.mkdtemp(prefix="c7sha-")
rnd = random.Random(5)
bad = n = 0
lengths = list(range(0, 301)) + [511, 512, 513, 1000, 4096, 65535, 65536, 1 << 20, (1 << 20) + 1]
for L in lengths:
    for data in ((bytes(rnd.randrange(256) for _ in range(L)) if L < 70000 else os.urandom(L)),) + ((b"\xff" * L,) if L in (55, 56, 64, 1000) else ()):
        p = os.path.join(tmp, "x.bin"); open(p, "wb").write(data)
        got = subprocess.run([C7C, "tool", "sha256", p], capture_output=True, text=True).stdout.strip()
        n += 1
        if got != hashlib.sha256(data).hexdigest():
            bad += 1; print(f"FAIL length {L}: {got}")
print(f"sha256: {n} inputs, problems={bad}")
sys.exit(1 if bad else 0)
