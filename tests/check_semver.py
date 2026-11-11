#!/usr/bin/env python3
"""SemVer 2.0.0 precedence examples (semver.org, section 11) and Cargo's documented requirement semantics as test vectors."""
import os, subprocess, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
C7C = os.path.join(ROOT, "..", "c7c")
bad = 0
n = 0

def run(*a):
    return subprocess.run([C7C, "tool", *a], capture_output=True, text=True).stdout.strip()

def check(label, got, want):
    global bad, n
    n += 1
    if got != want:
        bad += 1
        print(f"FAIL {label}: got {got!r}, want {want!r}")

# --- version syntax
for v in ["0.0.0", "1.2.3", "10.20.30", "1.2.3-alpha.1", "1.2.3-0.3.7", "1.2.3-x.7.z.92", "1.2.3+build.5", "1.2.3-rc.1+build-1", "1.0.0-a-b"]:
    check(f"valid {v}", run("semver-cmp", v, v), "0")
for v in ["1.2", "1", "01.2.3", "1.02.3", "1.2.03", "1.2.3.4", "1.2.3-", "1.2.3-01", "v1.2.3", "1.2.3-alpha..1", "1.2.-3", "1.2.3+", "a.b.c",
          "18446744073709551616.0.0", "1.2.3-é", "", " 1.2.3"]:
    check(f"invalid {v!r}", run("semver-cmp", v, "1.0.0").startswith("invalid"), True)

# --- precedence (the chain from the SemVer spec must be strictly increasing)
chain = ["1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta", "1.0.0-beta.2", "1.0.0-beta.11", "1.0.0-rc.1", "1.0.0",
         "2.0.0", "2.1.0", "2.1.1", "10.0.0"]
for i in range(len(chain)):
    for j in range(len(chain)):
        check(f"cmp {chain[i]} vs {chain[j]}", run("semver-cmp", chain[i], chain[j]), str((i > j) - (i < j)))
check("build metadata ignored", run("semver-cmp", "1.0.0+a", "1.0.0+b"), "0")
check("numeric ids compare numerically", run("semver-cmp", "1.0.0-2", "1.0.0-10"), "-1")
check("numeric < alphanumeric", run("semver-cmp", "1.0.0-99", "1.0.0-a"), "-1")

# --- requirements: (requirement, [versions that match], [versions that do not])
cases = [
    ("^1.2.3", ["1.2.3", "1.2.4", "1.9.9"], ["1.2.2", "2.0.0", "0.9.0"]),
    ("^1.2", ["1.2.0", "1.9.9"], ["1.1.9", "2.0.0"]),
    ("^1", ["1.0.0", "1.99.0"], ["0.9.9", "2.0.0"]),
    ("^0.2.3", ["0.2.3", "0.2.9"], ["0.2.2", "0.3.0", "1.0.0"]),
    ("^0.0.3", ["0.0.3"], ["0.0.2", "0.0.4", "0.1.0"]),
    ("^0.0", ["0.0.0", "0.0.9"], ["0.1.0"]),
    ("^0", ["0.0.0", "0.9.9"], ["1.0.0"]),
    ("1.2.3", ["1.2.3", "1.5.0"], ["2.0.0", "1.2.2"]),          # bare version means caret
    ("~1.2.3", ["1.2.3", "1.2.9"], ["1.3.0", "1.2.2"]),
    ("~1.2", ["1.2.0", "1.2.9"], ["1.3.0", "1.1.9"]),
    ("~1", ["1.0.0", "1.9.9"], ["2.0.0"]),
    ("1.*", ["1.0.0", "1.9.9"], ["2.0.0", "0.9.9"]),
    ("1.2.*", ["1.2.0", "1.2.9"], ["1.3.0", "1.1.9"]),
    ("1.x", ["1.4.0"], ["2.0.0"]),
    ("*", ["0.0.1", "99.0.0"], []),
    ("=1.2.3", ["1.2.3"], ["1.2.4", "1.2.2"]),
    ("=1.2", ["1.2.0", "1.2.9"], ["1.3.0"]),
    ("=1", ["1.0.0", "1.9.0"], ["2.0.0"]),
    (">=1.2.0, <1.5.0", ["1.2.0", "1.4.9"], ["1.1.9", "1.5.0"]),
    (">1.2", ["1.3.0", "2.0.0"], ["1.2.9", "1.2.0"]),
    (">1.2.3", ["1.2.4"], ["1.2.3"]),
    ("<1.2", ["1.1.9"], ["1.2.0"]),
    ("<=1.2", ["1.2.9"], ["1.3.0"]),
    ("<=1", ["1.9.9"], ["2.0.0"]),
    (">=1", ["1.0.0", "5.0.0"], ["0.9.9"]),
    # pre-releases
    ("^1.2.3", [], ["1.3.0-beta.1", "2.0.0-rc.1"]),
    (">=1.2.3-alpha.1", ["1.2.3-alpha.2", "1.2.3", "1.5.0"], ["1.2.3-alpha.0", "1.2.4-alpha.1"]),
    ("^1.2.3-beta.1", ["1.2.3-beta.2", "1.2.3", "1.9.0"], ["1.2.3-alpha.9", "2.0.0"]),
]
for req, yes, no in cases:
    for v in yes: check(f"{req} matches {v}", run("semver-match", req, v), "yes")
    for v in no: check(f"{req} rejects {v}", run("semver-match", req, v), "no")
for req in ["", ">=", "^", "1.2.3.4", "~>1.2", "=== 1", ">=1.2,", "a.b.c", ">=*", "1.2.3-", "^1.*.3x"]:
    check(f"invalid requirement {req!r}", run("semver-match", req, "1.0.0").startswith("invalid"), True)

print(f"semver: {n} checks, problems={bad}")
sys.exit(1 if bad else 0)
