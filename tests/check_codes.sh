#!/bin/bash
# Diagnostic-code registry checks (spec 13.4). Exit 0 when every check passes.
cd "$(dirname "$0")/.." || exit 1
def=compiler/frontend/diag_codes.def
rc=0
registered=$(grep -o '^DIAG("[EW][0-9]\{4\}"' "$def" | grep -o '[EW][0-9]\{4\}' | sort)
# 1. every code used in the sources is registered (an unregistered code would print "unknown diagnostic code")
used=$(grep -rhoE '"[EW][0-9]{4}"' compiler --include=*.cpp --include=*.hpp | tr -d '"' | sort -u)
for c in $used; do
  echo "$registered" | grep -qx "$c" || { echo "unregistered diagnostic code: $c"; rc=1; }
done
# 2. no duplicates in the registry file...
dups=$(echo "$registered" | uniq -d)
[ -z "$dups" ] || { echo "duplicate codes in $def: $dups"; rc=1; }
# 3. ...and a duplicate really does fail the build
cp "$def" /tmp/c7-dup.def
echo 'DIAG("E0001", "dup", "dup")' >> /tmp/c7-dup.def
if g++ -std=c++20 -fsyntax-only -DC7_DIAG_DEF='"/tmp/c7-dup.def"' -Icompiler/frontend compiler/frontend/diag.cpp 2>/tmp/c7-dup.err; then
  echo "a duplicate code did not fail the build"; rc=1
elif ! grep -q "duplicate diagnostic code" /tmp/c7-dup.err; then
  echo "duplicate build failed for the wrong reason:"; head -3 /tmp/c7-dup.err; rc=1
fi
exit $rc
