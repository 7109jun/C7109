#!/bin/bash
# Cross-check the preprocessor against gcc (the reference for C macro semantics) on tests/pp/*.c7.
# Token lists must be identical (whitespace between tokens ignored, inside string literals preserved).
# Skipped (exit 0) when gcc or python3 is missing.
cd "$(dirname "$0")/.." || exit 1
command -v gcc >/dev/null && command -v python3 >/dev/null || { echo "skipped: gcc/python3 not available"; exit 0; }
rc=0
for f in tests/pp/*.c7; do
  ./c7c pp "$f" > /tmp/c7-pp-ours.txt 2>/dev/null
  gcc -std=c2x -E -P -x c "$f" > /tmp/c7-pp-gcc.txt 2>/dev/null
  python3 - /tmp/c7-pp-ours.txt /tmp/c7-pp-gcc.txt <<'PY' || { echo "pp differs from gcc: $f"; rc=1; }
import re, sys
rx = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[A-Za-z0-9_.]+|->|\+=|<<|\S')
tok = lambda p: rx.findall(open(p, encoding="utf-8").read())
a, b = tok(sys.argv[1]), tok(sys.argv[2])
if a != b:
    i = next((k for k, (x, y) in enumerate(zip(a, b)) if x != y), min(len(a), len(b)))
    print("first difference at token", i, "ours:", a[i:i+4], "gcc:", b[i:i+4]); sys.exit(1)
PY
done
exit $rc
