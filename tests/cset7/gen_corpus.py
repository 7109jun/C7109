# Generator for tests/cset7/{valid,invalid}. Expectations are written by hand here, independent of the C++ loader
# (the merged Options as JSON for valid files; for invalid files `@@` marks the value the diagnostic must point at).
import json, os, sys
root = sys.argv[1]
V = os.path.join(root, "valid"); I = os.path.join(root, "invalid")
os.makedirs(V, exist_ok=True); os.makedirs(I, exist_ok=True)
D = {"mode": "release", "optimization": "O0", "bounds_check": False, "overflow_check": False, "debug_alloc": False,
     "language": "en", "link": "dynamic", "abi": "c7abi1", "include": [], "library": [], "registry": [], "flags": ["-O0"]}
def want(**kw):
    d = dict(D); d.update(kw); return d
valid = {}
valid["empty"] = ("", want())
valid["comments_only"] = ("# nothing\n", want())
valid["full"] = ('''[compiler]
requires = ">=1.0.0-alpha.1"
[target]
os = "linux"
architecture = "x86_64"
abi = "c7abi1"
[build]
mode = "debug"
optimization = "O1"
bounds_check = true
overflow_check = true
debug_alloc = true
[diagnostics]
language = "ko"
[link]
mode = "static"
[paths]
include = ["inc", "vendor/inc"]
library = ["lib"]
registry = ["../reg"]
''', want(mode="debug", optimization="O1", bounds_check=True, overflow_check=True, debug_alloc=True, language="ko", link="static",
          os="linux", architecture="x86_64", include=["inc", "vendor/inc"], library=["lib"], registry=["../reg"],
          flags=["-O1", "--debug", "-fbounds-check", "-fdebug-alloc", "--link=static", "--lang=ko", "--target=x86_64-linux", "-Iinc", "-Ivendor/inc"]))
valid["debug_mode_implies_overflow"] = ('[build]\nmode = "debug"\n', want(mode="debug", overflow_check=True, flags=["-O0", "--debug"]))
valid["debug_mode_overflow_off"] = ('[build]\nmode = "debug"\noverflow_check = false\n', want(mode="debug", overflow_check=False, flags=["-O0", "--debug", "-fno-overflow-check"]))
valid["release_overflow_on"] = ('[build]\noverflow_check = true\n', want(overflow_check=True, flags=["-O0", "-foverflow-check"]))
valid["optimization_only"] = ('[build]\noptimization = "O3"\n', want(optimization="O3", flags=["-O3"]))
valid["duplicate_paths_dropped"] = ('[paths]\ninclude = ["a", "b", "a"]\n', want(include=["a", "b"], flags=["-O0", "-Ia", "-Ib"]))
valid["unknown_keys_warn"] = ('mystery = 1\n[build]\nmode = "release"\nfuture = true\n', want())
valid["empty_tables"] = ('[build]\n[paths]\n[link]\n', want())
for n, (t, e) in valid.items():
    open(f"{V}/{n}.cset7", "w", encoding="utf-8", newline="").write(t)
    open(f"{V}/{n}.json", "w").write(json.dumps(e, sort_keys=True, indent=1) + "\n")
inv = {
 "toml_syntax": ("E0701", '[build@@\nmode = "debug"\n'),
 "mode_bad": ("E0719", '[build]\nmode = @@"fast"\n'),
 "mode_type": ("E0719", '[build]\nmode = @@1\n'),
 "optimization_bad": ("E0719", '[build]\noptimization = @@"O4"\n'),
 "optimization_lower": ("E0719", '[build]\noptimization = @@"o2"\n'),
 "bounds_type": ("E0719", '[build]\nbounds_check = @@"yes"\n'),
 "overflow_type": ("E0719", '[build]\noverflow_check = @@1\n'),
 "debug_alloc_type": ("E0719", '[build]\ndebug_alloc = @@"true"\n'),
 "language_bad": ("E0719", '[diagnostics]\nlanguage = @@"fr"\n'),
 "link_bad": ("E0719", '[link]\nmode = @@"shared"\n'),
 "os_bad": ("E0719", '[target]\nos = @@"Linux"\n'),
 "arch_type": ("E0719", '[target]\narchitecture = @@64\n'),
 "abi_empty": ("E0719", '[target]\nabi = @@""\n'),
 "paths_not_array": ("E0719", '[paths]\ninclude = @@"inc"\n'),
 "paths_not_strings": ("E0719", '[paths]\nlibrary = ["a", @@1]\n'),
 "paths_empty_entry": ("E0719", '[paths]\nregistry = [@@""]\n'),
 "table_is_scalar": ("E0719", 'build = @@1\n'),
 "requires_not_string": ("E0719", '[compiler]\nrequires = @@1\n'),
 "requires_bad": ("E0717", '[compiler]\nrequires = @@"^^1"\n'),
}
for n, (code, text) in inv.items():
    i = text.index("@@"); before = text[:i].encode("utf-8")
    line = before.count(b"\n") + 1; col = len(before) - (before.rfind(b"\n") + 1) + 1
    open(f"{I}/{n}.cset7", "w", encoding="utf-8", newline="").write(text.replace("@@", "", 1))
    open(f"{I}/{n}.expect", "w").write(f"{code} {line}:{col}\n")
# warnings (the file is accepted): code and location of the first W0701
warn = {"unknown_top": ('mystery = @@1\n', "W0701"), "unknown_in_build": ('[build]\nfuture = @@true\n', "W0701"),
        "unknown_in_paths": ('[paths]\nextra = @@["x"]\n', "W0701")}
os.makedirs(os.path.join(root, "warn"), exist_ok=True)
for n, (text, code) in warn.items():
    i = text.index("@@"); before = text[:i].encode("utf-8")
    line = before.count(b"\n") + 1; col = len(before) - (before.rfind(b"\n") + 1) + 1
    open(f"{root}/warn/{n}.cset7", "w").write(text.replace("@@", "", 1))
    open(f"{root}/warn/{n}.expect", "w").write(f"{code} {line}:{col}\n")
print(len(valid), "valid,", len(inv), "invalid,", len(warn), "warn")
