#!/usr/bin/env python3
"""ABI v1 measurement: same C7 programs, same LLVM IR, only the calling convention differs.
Usage: python3 bench/abi/run.py [runs] [program ...|report]   (needs c7c built, llc, opt-18, gcc)
Results are stored per program in bench/abi/results/*.json; `report` prints the tables from them."""
import os, re, resource, statistics, subprocess, sys, tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
C7C, RT = os.path.join(ROOT, "c7c"), os.path.join(ROOT, "runtime", "c7rt.c")
RUNS = int(sys.argv[1]) if len(sys.argv) > 1 else 7
PROGS = ["scalar", "pair", "triple", "big", "result", "strres"]
# name -> LLVM calling-convention keyword ('' = C calling convention)
CCS = {"fastcc": "fastcc", "c": "", "swiftcc": "swiftcc", "preserve_most": "preserve_mostcc"}
# pipelines: 'raw' = exactly what c7c emits today (llc only); 'opt' = mem2reg/SROA first, still no inlining
OPT = "function(mem2reg,sroa,instcombine,simplifycfg)"
PIPES = ["raw", "opt"]
work = tempfile.mkdtemp(prefix="c7abi-")

def sh(cmd):
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"FAILED: {cmd}\n{r.stdout}{r.stderr}")
    return r.stdout

def cpu_time(exe):
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    r = subprocess.run([exe], capture_output=True, text=True)
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    return (after.ru_utime + after.ru_stime) - (before.ru_utime + before.ru_stime), r.stdout.strip()

import json
RESDIR = os.path.join(ROOT, "bench", "abi", "results")
os.makedirs(RESDIR, exist_ok=True)

def measure(p):
    """Build and time every (pipeline, cc) variant of one program; store results/<p>.json."""
    base = f"{work}/{p}.ll"
    sh(f"{C7C} build {ROOT}/bench/abi/{p}.c7 --emit=llvm -o {base}")
    ir = open(base).read()
    rec = {}
    for pipe in PIPES:
        for cc, kw in CCS.items():
            ll = f"{work}/{p}-{pipe}-{cc}.ll"
            open(ll, "w").write(re.sub(r"\bfastcc\b", kw, ir))
            if pipe == "opt":
                sh(f"opt-18 -passes='{OPT}' -S {ll} -o {ll}.opt && mv {ll}.opt {ll}")
            obj, exe = ll[:-3] + ".o", ll[:-3]
            sh(f"llc -O2 -filetype=obj -relocation-model=pic {ll} -o {obj}")
            sh(f"gcc -O1 {obj} {RT} -o {exe}")
            text = int(sh(f"size -A {obj} | awk '/\\.text/ {{print $2}}'").split()[0])
            times, out = [], None
            for _ in range(RUNS):
                t, o = cpu_time(exe)
                times.append(t); out = o
            rec[f"{pipe}/{cc}"] = {"min": min(times), "median": statistics.median(times), "text": text, "out": out}
    outs = {v["out"] for v in rec.values()}
    if len(outs) != 1:
        sys.exit(f"OUTPUT MISMATCH for {p}: {outs}")
    json.dump({"runs": RUNS, "variants": rec}, open(f"{RESDIR}/{p}.json", "w"), indent=1)

def report():
    data = {p: json.load(open(f"{RESDIR}/{p}.json")) for p in PROGS if os.path.exists(f"{RESDIR}/{p}.json")}
    runs = next(iter(data.values()))["runs"]
    print(f"CPU seconds, min / median of {runs} runs (lower is better); (ratio) = min relative to fastcc\n")
    for pipe in PIPES:
        print(f"### pipeline: {pipe}")
        print("| program | " + " | ".join(CCS) + " |")
        print("|---|" + "---|" * len(CCS))
        for p, d in data.items():
            ref = d["variants"][f"{pipe}/fastcc"]["min"]
            cells = [f"{d['variants'][f'{pipe}/{cc}']['min']:.3f} / {d['variants'][f'{pipe}/{cc}']['median']:.3f} ({d['variants'][f'{pipe}/{cc}']['min'] / ref:.2f}x)" for cc in CCS]
            print(f"| {p} | " + " | ".join(cells) + " |")
        print()
    print("### .text bytes (opt pipeline)")
    print("| program | " + " | ".join(CCS) + " |")
    print("|---|" + "---|" * len(CCS))
    for p, d in data.items():
        print(f"| {p} | " + " | ".join(str(d["variants"][f"opt/{cc}"]["text"]) for cc in CCS) + " |")

targets = sys.argv[2:]
if targets == ["report"]:
    report()
else:
    for p in (targets or PROGS):
        measure(p)
        print("measured", p, flush=True)
    report()
