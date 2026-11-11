#!/bin/bash
# Frontend test runner: positive cases must pass `c7c check`; neg_* cases must emit every
# `// expect: CODE` listed in the file (and fail).
cd "$(dirname "$0")/.." || exit 1
pass=0; fail=0
for f in tests/cases/[0-9]*.c7; do
  out=$(./c7c parse "$f" -Itests/cases 2>&1)
  if [ $? -eq 0 ]; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL $f"; echo "$out" | head -8; fi
done
for f in tests/cases/neg_*.c7; do
  out=$(./c7c parse "$f" -Itests/cases 2>&1); rc=$?
  ok=1
  [ $rc -ne 1 ] && ok=0
  for code in $(grep -o 'expect: [EW][0-9]*' "$f" | awk '{print $2}'); do
    echo "$out" | grep -q "\[$code\]" || { ok=0; echo "  missing $code in $f"; }
  done
  if [ $ok -eq 1 ]; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL $f"; echo "$out" | head -12; fi
done
for f in tests/sema/pos_*.c7; do
  out=$(./c7c check "$f" 2>&1)
  if [ $? -eq 0 ] && [ -z "$out" ]; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL $f"; echo "$out" | head -20; fi
done
for f in tests/sema/neg_*.c7; do
  out=$(./c7c check "$f" 2>&1); rc=$?
  ok=1
  [ $rc -ne 1 ] && ok=0
  for code in $(grep -o 'expect: [EW][0-9]*' "$f" | awk '{print $2}'); do
    echo "$out" | grep -q "\[$code\]" || { ok=0; echo "  missing $code in $f"; }
  done
  if [ $ok -eq 1 ]; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL $f"; echo "$out" | head -30; fi
done
for g in tests/golden/*.ast; do
  src=tests/cases/$(basename "${g%.ast}").c7
  if ./c7c ast "$src" 2>&1 | diff -q - "$g" >/dev/null; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL golden $g"; fi
done
for f in tests/run/*.c7; do
  name=$(basename "${f%.c7}")
  want_rc=0; [ -f "tests/run/$name.rc" ] && want_rc=$(cat "tests/run/$name.rc")
  for o in 0 2; do
    if ./c7c build "$f" -o /tmp/c7t-$name -O$o 2>/tmp/c7t-err; then
      got=$(/tmp/c7t-$name 2>/dev/null); rc=$?
      if [ "$got" == "$(cat tests/run/$name.expected)" ] && [ $rc -eq $want_rc ]; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL run $name -O$o (rc=$rc want $want_rc)"; fi
    else fail=$((fail+1)); echo "FAIL build $name -O$o"; head -5 /tmp/c7t-err; fi
  done
done
# debug allocator: every run program must behave identically (no false positives); tests/dbg programs must be caught
for f in tests/run/*.c7; do
  name=$(basename "${f%.c7}")
  want_rc=0; [ -f "tests/run/$name.rc" ] && want_rc=$(cat "tests/run/$name.rc")
  if ./c7c build "$f" -o /tmp/c7t-$name -fdebug-alloc 2>/tmp/c7t-err; then
    got=$(/tmp/c7t-$name 2>/dev/null); rc=$?
    if [ "$got" == "$(cat tests/run/$name.expected)" ] && [ $rc -eq $want_rc ]; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL run $name -fdebug-alloc (rc=$rc want $want_rc)"; fi
  else fail=$((fail+1)); echo "FAIL build $name -fdebug-alloc"; head -5 /tmp/c7t-err; fi
done
for f in tests/dbg/*.c7; do
  name=$(basename "${f%.c7}")
  if ./c7c build "$f" -o /tmp/c7t-dbg-$name -fdebug-alloc 2>/tmp/c7t-err; then
    got=$(/tmp/c7t-dbg-$name 2>/tmp/c7t-stderr); rc=$?
    if [ "$got" == "$(cat tests/dbg/$name.expected)" ] && [ $rc -eq "$(cat tests/dbg/$name.rc)" ] && grep -qF "$(cat tests/dbg/$name.err)" /tmp/c7t-stderr; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL dbg $name (rc=$rc)"; fi
  else fail=$((fail+1)); echo "FAIL build dbg $name"; head -5 /tmp/c7t-err; fi
done
# Debug semantics: every run program must behave identically with overflow and bounds checks on (no false positives)
for f in tests/run/*.c7; do
  name=$(basename "${f%.c7}")
  want_rc=0; [ -f "tests/run/$name.rc" ] && want_rc=$(cat "tests/run/$name.rc")
  if ./c7c build "$f" -o /tmp/c7t-$name --debug -fbounds-check 2>/tmp/c7t-err; then
    got=$(/tmp/c7t-$name 2>/dev/null); rc=$?
    if [ "$got" == "$(cat tests/run/$name.expected)" ] && [ $rc -eq $want_rc ]; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL run $name --debug -fbounds-check (rc=$rc want $want_rc)"; fi
  else fail=$((fail+1)); echo "FAIL build $name --debug -fbounds-check"; head -5 /tmp/c7t-err; fi
done
# overflow / bounds checks: tests/chk (flags chosen by name: ovf_* --debug, bnd_* -fbounds-check, others none)
for f in tests/chk/*.c7; do
  name=$(basename "${f%.c7}")
  case $name in ovf_*) flags="--debug";; bnd_*) flags="-fbounds-check";; *) flags="";; esac
  if ./c7c build "$f" $flags -o /tmp/c7t-chk-$name 2>/tmp/c7t-err; then
    got=$(/tmp/c7t-chk-$name 2>/tmp/c7t-stderr); rc=$?
    ok=1
    [ "$got" == "$(cat tests/chk/$name.expected)" ] || ok=0
    [ $rc -eq "$(cat tests/chk/$name.rc)" ] || ok=0
    [ ! -f tests/chk/$name.err ] || grep -qF "$(cat tests/chk/$name.err)" /tmp/c7t-stderr || ok=0
    if [ $ok -eq 1 ]; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL chk $name (rc=$rc)"; fi
  else fail=$((fail+1)); echo "FAIL build chk $name"; head -5 /tmp/c7t-err; fi
done
# stdlib unit tests (@test) in Debug (with the debug allocator) and Release
for f in tests/std/*.c7; do
  name=$(basename "${f%.c7}")
  for flags in "-O0 -fdebug-alloc --debug -fbounds-check" "-O2"; do
    if ./c7c test "$f" $flags >/tmp/c7t-out 2>&1; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL std $name ($flags)"; tail -8 /tmp/c7t-out; fi
  done
done
# the test runner itself must report failures: 1 pass, 1 assert failure, 1 leak
out=$(./c7c test tests/testrunner/failing.c7 -fdebug-alloc 2>/dev/null); rc=$?
if [ $rc -eq 1 ] && echo "$out" | grep -q "1 passed; 2 failed" && echo "$out" | grep -q "FAILED (exit 134)" && echo "$out" | grep -q "exit 70"; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL testrunner failing.c7 (rc=$rc)"; echo "$out" | head; fi
# preprocessor: frozen output (verified equal to gcc) + live cross-check against gcc
for f in tests/pp/*.c7; do
  if ./c7c pp "$f" 2>/dev/null | diff -q - "${f%.c7}.expected" >/dev/null; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL pp $f"; fi
done
if out=$(bash tests/check_pp_gcc.sh 2>&1); then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL check_pp_gcc"; echo "$out" | head; fi
# diagnostic-code registry
if out=$(bash tests/check_codes.sh 2>&1); then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL check_codes"; echo "$out" | head; fi
# driver extensions (--lib, --depfile, several inputs) and the single version source
if out=$(bash tests/check_driver.sh 2>&1) && echo "$out" | tail -1 | grep -q "problems=0"; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL check_driver"; echo "$out" | tail -8; fi
if [ "$(./c7c --version)" == "c7c 1.0.0-alpha.1 (ABI c7abi1)" ]; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL --version: $(./c7c --version)"; fi
if out=$(bash tests/check_std.sh 2>&1) && echo "$out" | tail -1 | grep -q "problems=0"; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL check_std"; echo "$out" | tail -8; fi
# stage-5 module checks (independent oracles). A missing prerequisite is reported, never silently ignored.
skipped=""
if python3 -c 'import sys; sys.exit(0 if sys.version_info >= (3, 11) else 1)' 2>/dev/null; then
  for chk in check_sha256 check_toml check_zip check_semver check_l7 check_buildfile check_options check_graph check_project check_cache check_resolve check_deps check_lock check_map check_install; do
    if [ "$chk" = check_l7 ] && ! command -v sha256sum >/dev/null; then skipped="$skipped $chk(no sha256sum)"; continue; fi
    if [ "$chk" = check_zip ] && ! command -v unzip >/dev/null; then skipped="$skipped $chk(no unzip)"; continue; fi
    if out=$(python3 -I tests/$chk.py 2>&1) && echo "$out" | tail -1 | grep -q "problems=0"; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL $chk"; echo "$out" | tail -8; fi
  done
  # short fuzz run (the long run is `make fuzz`)
  if out=$(python3 -I tests/fuzz_pkg.py 500 1 2>&1); then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL fuzz_pkg"; echo "$out" | tail -8; fi
else
  skipped="$skipped python-checks(need python3>=3.11)"
fi
[ -z "$skipped" ] || echo "SKIPPED:$skipped"
echo "passed=$pass failed=$fail"
[ $fail -eq 0 ]
