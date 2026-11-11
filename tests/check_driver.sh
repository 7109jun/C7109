#!/bin/bash
# Driver extensions: several .c7 inputs, --lib x.l7, --depfile, tampered/hostile packages, diagnostics for library code.
# Prints "driver: N checks, problems=P". Run from anywhere; uses ./c7c.
cd "$(dirname "$0")/.." || exit 1
C=./c7c; T=tests/driver; W=$(mktemp -d /tmp/c7drv.XXXXXX); trap 'rm -rf "$W"' EXIT
n=0; bad=0
ok()   { n=$((n+1)); }
fail() { n=$((n+1)); bad=$((bad+1)); echo "FAIL $*"; }
expect_out() { # name want got
  if [ "$2" == "$3" ]; then ok; else fail "$1: want '$2' got '$3'"; fi; }
expect_diag() { # name rc-want code text-file [fragment]
  local name=$1 want=$2 code=$3 file=$4 frag=$5
  if [ "$RC" -ne "$want" ]; then fail "$name: rc=$RC want $want"; return; fi
  grep -q "\[$code\]" "$file" || { fail "$name: missing $code: $(head -3 "$file")"; return; }
  [ -z "$frag" ] || grep -qF -- "$frag" "$file" || { fail "$name: diagnostic lacks '$frag': $(head -3 "$file")"; return; }
  ok; }

# --- positive: multiple inputs, at every optimization level
for o in 0 2; do
  $C build $T/main.c7 $T/triple.c7 -O$o -o $W/p 2>$W/err && expect_out "multi-file -O$o" 42 "$($W/p)" || fail "multi-file -O$o build: $(head -3 $W/err)"
done
# --- positive: library package as --lib (both spellings), also with the debug checks on
$C tool l7-pack $W/triple.l7 Triple 1.0.0 src/triple.c7=$T/triple.c7 >/dev/null || fail "l7-pack"
for flags in "-O0" "-O2" "-fdebug-alloc --debug -fbounds-check"; do
  $C build $T/main.c7 --lib $W/triple.l7 $flags -o $W/p 2>$W/err && expect_out "--lib $flags" 42 "$($W/p)" || fail "--lib $flags build: $(head -3 $W/err)"
done
$C build $T/main.c7 --lib=$W/triple.l7 -o $W/p 2>$W/err && expect_out "--lib=" 42 "$($W/p)" || fail "--lib= build"
# the same package may not be combined with a duplicate definition
$C check $T/main.c7 $T/triple.c7 --lib $W/triple.l7 2>$W/err; RC=$?; expect_diag "lib duplicates source" 1 E0211 $W/err

# --- --depfile lists real sources and #include targets only (B3: never .l7 packages or their anchor files)
$C build $T/inc_main.c7 -I$T -o $W/p --depfile $W/dep.txt 2>$W/err && expect_out "include build" 77 "$($W/p)" || fail "include build"
want="$(cd $T && pwd)/inc_main.c7
$(cd $T && pwd)/inc_value.h7"
expect_out "depfile (include)" "$(echo "$want" | sort)" "$(sort $W/dep.txt)"
$C build $T/main.c7 --lib $W/triple.l7 -o $W/p --depfile $W/dep.txt 2>/dev/null
expect_out "depfile (--lib not listed)" "$(cd $T && pwd)/main.c7" "$(cat $W/dep.txt)"
$C build $T/main.c7 $T/triple.c7 -o $W/p --depfile $W/dep.txt 2>/dev/null
expect_out "depfile (extra source listed)" "$(printf '%s\n%s\n' "$(cd $T && pwd)/main.c7" "$(cd $T && pwd)/triple.c7")" "$(sort $W/dep.txt)"
rm -f $W/dep.txt; $C build $T/main.c7 -o $W/p --depfile $W/dep.txt 2>/dev/null; RC=$?
[ $RC -eq 1 ] && [ ! -s $W/dep.txt ] && ok || fail "failed build must not write a depfile"

# --- negative: diagnostics
$C build $T/main.c7 $T/triple.c7 $T/triple_dup.c7 -o $W/p 2>$W/err; RC=$?; expect_diag "duplicate declaration across units" 1 E0211 $W/err "triple_dup.c7:1:1"
$C build $T/main.c7 $T/nope.c7 -o $W/p 2>$W/err; RC=$?; expect_diag "missing extra source" 1 E0704 $W/err
$C tool l7-pack $W/m.l7 Mainlib 1.0.0 src/m.c7=$T/lib_with_main.c7 >/dev/null
$C build $T/main.c7 $T/triple.c7 --lib $W/m.l7 -o $W/p 2>$W/err; RC=$?; expect_diag "main in a library" 1 E0211 $W/err "m.l7!src/m.c7:1:1"
$C tool l7-pack $W/s.l7 Synerr 1.0.0 src/a.c7=$T/lib_syntax_err.c7 >/dev/null
$C build $T/main.c7 --lib $W/s.l7 -o $W/p 2>$W/err; RC=$?; expect_diag "syntax error inside a library" 1 E0103 $W/err "s.l7!src/a.c7:1:10"
$C tool l7-pack $W/e.l7 Semerr 1.0.0 src/a.c7=$T/lib_sema_err.c7 >/dev/null
$C build $T/main.c7 $T/triple.c7 --lib $W/e.l7 -o $W/p 2>$W/err; RC=$?; expect_diag "semantic error inside a library" 1 E0212 $W/err "e.l7!src/a.c7:1:"
$C build $T/main.c7 --lib $W/absent.l7 -o $W/p 2>$W/err; RC=$?; [ $RC -eq 1 ] && ok || fail "missing --lib file must fail (rc=$RC)"
$C build $T/main.c7 --lib $T/main.c7 -o $W/p 2>$W/err; RC=$?; expect_diag "--lib that is not a package" 1 E0718 $W/err
$C build $T/main.c7 --bogus -o $W/p 2>$W/err; RC=$?; [ $RC -eq 2 ] && ok || fail "unknown option must be a usage error (rc=$RC)"

# --- tampered packages are rejected before any code is compiled
python3 -I - "$W/triple.l7" "$W" <<'PY'
import sys, zipfile
src, w = sys.argv[1:]
z = zipfile.ZipFile(src)
def rewrite(name, edit):
    with zipfile.ZipFile(f"{w}/{name}", "w", zipfile.ZIP_DEFLATED) as o:
        for i in z.infolist():
            data = z.read(i.filename)
            r = edit(i.filename, data)
            if r is not None: o.writestr(zipfile.ZipInfo(i.filename, (1980, 1, 1, 0, 0, 0)), r)
rewrite("t_content.l7", lambda n, d: d.replace(b"x * 3", b"x * 4") if n == "src/triple.c7" else d)   # content no longer matches checksums
rewrite("t_extra.l7", lambda n, d: d)                                                             # then add an unlisted file below
with zipfile.ZipFile(f"{w}/t_extra.l7", "a") as o: o.writestr("src/evil.c7", "i64 evil() { return 1; }\n")
rewrite("t_nochk.l7", lambda n, d: None if n == "checksums.sha256" else d)                      # checksum list missing
PY
for t in content:E0710 extra:E0710 nochk:E0709; do
  name=${t%%:*}; code=${t##*:}
  $C build $T/main.c7 --lib $W/t_$name.l7 -o $W/p 2>$W/err; RC=$?; expect_diag "tampered package ($name)" 1 $code $W/err
done
echo "driver: $n checks, problems=$bad"
[ $bad -eq 0 ]
