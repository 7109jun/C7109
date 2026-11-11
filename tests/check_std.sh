#!/bin/bash
# Standard-library loader paths and the std.l7 package (W4). Each scenario gets a private directory laid out like an
# installation (c7c, runtime/, and some combination of std.l7 / stdlib/) because c7c looks beside its own executable.
cd "$(dirname "$0")/.." || exit 1
W=$(mktemp -d /tmp/c7std.XXXXXX); trap 'rm -rf "$W"' EXIT
n=0; bad=0
ok()   { n=$((n+1)); }
fail() { n=$((n+1)); bad=$((bad+1)); echo "FAIL $*"; }
[ -f std.l7 ] || { echo "std.l7 missing (run make)"; echo "std: 0 checks, problems=1"; exit 1; }
layout() { # dir [std.l7 file|-] [with-stdlib: yes|no]
  mkdir -p "$1/runtime"; cp c7c "$1/c7c"; cp runtime/c7rt.c "$1/runtime/"
  [ "$2" == "-" ] || cp "$2" "$1/std.l7"
  [ "$3" != yes ] || cp -r stdlib "$1/stdlib"
}
# a program that exercises Vec, Str, StrBuf, println: stdlib content really has to be present
PROG=tests/run/vec.c7; WANT=tests/run/vec.expected
runs() { # name dir [env...]  -> builds PROG with that layout and compares the output
  local name=$1 dir=$2; shift 2
  if env "$@" "$dir/c7c" build $PROG -o $dir/p 2>$W/err && [ "$($dir/p)" == "$(cat $WANT)" ]; then ok; else fail "$name: $(head -3 $W/err)"; fi
}

# 1. std.l7 only (no stdlib/ directory exists, so the package is the only possible source)
layout $W/a std.l7 no;  runs "std.l7 loader" $W/a
# 2. C7_STDLIB directory (std.l7 absent)
layout $W/b - no;  runs "C7_STDLIB loader" $W/b C7_STDLIB=$PWD/stdlib
# 3. stdlib/ fallback beside the executable (no std.l7)
layout $W/c - yes; runs "stdlib/ fallback" $W/c
# 4. precedence: C7_STDLIB beats std.l7, std.l7 beats stdlib/
cp -r stdlib $W/stdmarker; cp tests/stdl7/marker.c7 $W/stdmarker/
./c7c tool l7-pack $W/marker.l7 std 1.0.0 $(for f in stdlib/*.c7; do echo src/$(basename $f)=$f; done) src/marker.c7=tests/stdl7/marker.c7 >/dev/null
layout $W/d $W/marker.l7 yes
if $W/d/c7c build tests/stdl7/use_marker.c7 -o $W/d/p 2>$W/err && [ "$($W/d/p | tail -1)" == 4242 ]; then ok; else fail "std.l7 must win over stdlib/: $(head -3 $W/err)"; fi
if C7_STDLIB=$PWD/stdlib $W/d/c7c build tests/stdl7/use_marker.c7 -o $W/d/p2 2>$W/err; then fail "C7_STDLIB must win over std.l7"; else ok; fi
if C7_STDLIB=$W/stdmarker $W/d/c7c build tests/stdl7/use_marker.c7 -o $W/d/p3 2>$W/err && [ "$($W/d/p3 | tail -1)" == 4242 ]; then ok; else fail "C7_STDLIB marker dir: $(head -3 $W/err)"; fi

# 5. a tampered std.l7 is refused (changed content / extra file / dropped checksum list), and nothing is compiled
python3 -I - std.l7 $W <<'PY'
import sys, zipfile
src, w = sys.argv[1:]
z = zipfile.ZipFile(src)
def rewrite(name, edit, extra=None):
    with zipfile.ZipFile(f"{w}/{name}", "w", zipfile.ZIP_DEFLATED) as o:
        for i in z.infolist():
            r = edit(i.filename, z.read(i.filename))
            if r is not None: o.writestr(zipfile.ZipInfo(i.filename, (1980, 1, 1, 0, 0, 0)), r)
        if extra: o.writestr(*extra)
rewrite("t_content.l7", lambda n, d: d + b"\n// tampered\n" if n == "src/core.c7" else d)
rewrite("t_extra.l7", lambda n, d: d, ("src/evil.c7", "namespace std;\n"))
rewrite("t_nochk.l7", lambda n, d: None if n == "checksums.sha256" else d)
open(f"{w}/t_garbage.l7", "wb").write(b"PK\x03\x04 not a zip at all")
PY
i=0; for t in content:E0710 extra:E0710 nochk:E0709 garbage:E0718; do
  name=${t%%:*}; code=${t##*:}; i=$((i+1)); layout $W/t$i $W/t_$name.l7 yes   # stdlib/ is present too: it must NOT be used as a fallback
  $W/t$i/c7c build $PROG -o $W/t$i/p 2>$W/err; rc=$?
  if [ $rc -eq 1 ] && grep -q "\[$code\]" $W/err && [ ! -e $W/t$i/p ]; then ok; else fail "tampered std.l7 ($name): rc=$rc want [$code]: $(head -2 $W/err)"; fi
done

# 6. the package itself: identity, license, reproducibility, Makefile output equals a fresh pack
out=$(./c7c tool l7-verify std.l7); [[ "$out" == std\ 1.0.0\ * ]] && ok || fail "std.l7 identity: $out"
./c7c tool zip-cat std.l7 manifest.toml > $W/manifest.toml
grep -qF 'license = "GPL-3.0-only WITH C7109-runtime-library-exception"' $W/manifest.toml && ok || fail "std.l7 license line"
grep -q '^abi = "c7abi1"' $W/manifest.toml && ok || fail "std.l7 abi"
! grep -q '^\[compiler\]' $W/manifest.toml && ok || fail "std.l7 must not declare [compiler] requires (Q5: pre-release compiler)"
# dogfooding: the package comes from stdlib/Cmake7109.txt; a fresh build in a private copy must equal std.l7, twice
for k in 1 2; do rm -rf $W/sl$k; cp -r stdlib $W/sl$k; rm -f $W/sl$k/std.l7; ./c7c pack $W/sl$k --no-cache >/dev/null 2>$W/err || fail "pack stdlib: $(head -3 $W/err)"; done
cmp -s $W/sl1/std.l7 $W/sl2/std.l7 && ok || fail "std.l7 not reproducible"
cmp -s $W/sl1/std.l7 std.l7 && ok || fail "std.l7 is stale or differs from a fresh pack (run make)"
echo "std: $n checks, problems=$bad"
[ $bad -eq 0 ]
