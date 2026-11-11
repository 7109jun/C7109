# One-off generator for tests/build corpus. Expected values are written by hand here (independent of the C++ parser);
# for invalid files `@@` marks where the diagnostic must point, and line:col are computed from the marker.
import json, os, sys
root = sys.argv[1]
V = os.path.join(root, "valid"); I = os.path.join(root, "invalid")
os.makedirs(V, exist_ok=True); os.makedirs(I, exist_ok=True)

def tgt(kind, name, sources, inc=(), defs=(), deps=(), **kw):
    d = {"kind": kind, "name": name, "sources": list(sources), "include_dirs": list(inc), "defines": list(defs), "depends": list(deps)}
    d.update(kw); return d
def exp(name, projver=None, regs=(), deps=(), cfgs=(), default=None, targets=(), **proj):
    p = {"name": name}; p.update(proj)
    if projver: p["version"] = projver
    d = {"project": p, "registries": list(regs), "dependencies": list(deps), "configs": list(cfgs), "targets": list(targets)}
    if default: d["default_config"] = default
    return d

valid = {}
valid["minimal"] = ('project "Example"\ntarget executable "app":\n    sources:\n        "src/main.c7"\n',
    exp("Example", targets=[tgt("executable", "app", ["src/main.c7"])]))
valid["spec_example"] = ('project "Example"\ntarget executable "app":\n    sources:\n        "src/main.c7"\n        "src/math.c7"\n',
    exp("Example", targets=[tgt("executable", "app", ["src/main.c7", "src/math.c7"])]))
valid["full"] = ('''# full example (guide 4.3 a)
project "Example":
    version "1.2.3"
    license "GPL-3.0-only"
    description "demo"

registry "../registry"
dependency "Other" "^1.2"
dependency "Local" path "../local"

config debug:
    mode "debug"
    optimization "O0"
    bounds_check true
    debug_alloc true
config release:
    mode "release"
    optimization "O2"
default_config release

target library "mylib":
    sources:
        "src/lib.c7"
target executable "app":
    sources:
        "src/main.c7"
    include_dirs:
        "include"
    defines:
        "VERSION=2"
    depends:
        "mylib"
    output "build/app"
    platform "x86_64-linux"
    link "static"
    std "none"
''', exp("Example", "1.2.3", regs=["../registry"],
    deps=[{"name": "Other", "version": "^1.2"}, {"name": "Local", "path": "../local"}],
    cfgs=[{"name": "debug", "mode": "debug", "optimization": "O0", "bounds_check": True, "debug_alloc": True},
          {"name": "release", "mode": "release", "optimization": "O2"}], default="release",
    targets=[tgt("library", "mylib", ["src/lib.c7"]),
             tgt("executable", "app", ["src/main.c7"], inc=["include"], defs=["VERSION=2"], deps=["mylib"], output="build/app", platform="x86_64-linux", link="static", std="none")],
    license="GPL-3.0-only", description="demo"))
valid["comments_blank"] = ('# header\n\n   \n# indented comment\nproject "C" # trailing\n\ntarget executable "a": # why\n    # inside\n    sources:  # list\n        "a.c7" # one\n\n        "b#.c7"\n',
    exp("C", targets=[tgt("executable", "a", ["a.c7", "b#.c7"])]))
valid["crlf"] = ('project "Crlf"\r\ntarget executable "a":\r\n    sources:\r\n        "a.c7"\r\n', exp("Crlf", targets=[tgt("executable", "a", ["a.c7"])]))
valid["escapes"] = ('project "E":\n    description "q\\" b\\\\ n\\n t\\t"\ntarget executable "a":\n    sources:\n        "a.c7"\n',
    exp("E", targets=[tgt("executable", "a", ["a.c7"])], description='q" b\\ n\n t\t'))
valid["path_normalize"] = ('project "P"\ntarget executable "a":\n    sources:\n        "./src//a.c7"\n        "src/../src/b.c7"\n        "x/./y/../z.c7"\n',
    exp("P", targets=[tgt("executable", "a", ["src/a.c7", "src/b.c7", "x/z.c7"])]))
valid["include_dot"] = ('project "P"\ntarget executable "a":\n    sources:\n        "a.c7"\n    include_dirs:\n        "."\n        "inc/"\n        "a/../b"\n',
    exp("P", targets=[tgt("executable", "a", ["a.c7"], inc=[".", "inc", "b"])]))
valid["registries"] = ('project "R"\nregistry "../one"\nregistry "/abs/two"\nregistry "three"\n', exp("R", regs=["../one", "/abs/two", "three"]))
valid["requirements"] = ('project "D"\ndependency "A" "^1.2"\ndependency "B" ">=1.0, <2.0"\ndependency "C" "1.*"\ndependency "D-x" "=1.2.3"\ndependency "E_y" "~1.4"\ndependency "F" "1.2.3"\n',
    exp("D", deps=[{"name": n, "version": v} for n, v in [("A", "^1.2"), ("B", ">=1.0, <2.0"), ("C", "1.*"), ("D-x", "=1.2.3"), ("E_y", "~1.4"), ("F", "1.2.3")]]))
valid["path_dep"] = ('project "D"\ndependency "L" path "../../x/l.l7"\ndependency "M" path "/abs/m"\n', exp("D", deps=[{"name": "L", "path": "../../x/l.l7"}, {"name": "M", "path": "/abs/m"}]))
valid["config_all"] = ('project "C"\nconfig dev:\n    mode "debug"\n    optimization "O1"\n    bounds_check false\n    debug_alloc true\n    overflow_check true\nconfig bare:\nconfig fast-3:\n    optimization "O3"\n',
    exp("C", cfgs=[{"name": "dev", "mode": "debug", "optimization": "O1", "bounds_check": False, "debug_alloc": True, "overflow_check": True}, {"name": "bare"}, {"name": "fast-3", "optimization": "O3"}]))
valid["default_config"] = ('project "C"\ndefault_config b\nconfig a:\n    mode "debug"\nconfig b:\n    mode "release"\n',
    exp("C", cfgs=[{"name": "a", "mode": "debug"}, {"name": "b", "mode": "release"}], default="b"))
valid["lib_exe_deps"] = ('project "G"\ntarget library "core":\n    sources:\n        "core.c7"\ntarget library "util":\n    sources:\n        "util.c7"\n    depends:\n        "core"\ntarget executable "app":\n    sources:\n        "main.c7"\n    depends:\n        "util"\n        "core"\n',
    exp("G", targets=[tgt("library", "core", ["core.c7"]), tgt("library", "util", ["util.c7"], deps=["core"]), tgt("executable", "app", ["main.c7"], deps=["util", "core"])]))
valid["no_colons"] = ('project "N"\n    version "0.1.0"\nconfig c\n    mode "debug"\ntarget executable "a"\n    sources:\n        "a.c7"\n',
    exp("N", "0.1.0", cfgs=[{"name": "c", "mode": "debug"}], targets=[tgt("executable", "a", ["a.c7"])]))
valid["tabs_between_tokens"] = ('project\t"T"\ntarget\texecutable\t"a"\t:\n    sources\t:\n        "a.c7"\n', exp("T", targets=[tgt("executable", "a", ["a.c7"])]))
valid["unicode"] = ('project "예제 프로젝트":\n    description "한국어 설명 ✓"\ntarget executable "앱":\n    sources:\n        "소스/메인.c7"\n'.replace('target executable "앱"', 'target executable "app"'),
    exp("예제 프로젝트", targets=[tgt("executable", "app", ["소스/메인.c7"])], description="한국어 설명 ✓"))
valid["defines"] = ('project "D"\ntarget executable "a":\n    sources:\n        "a.c7"\n    defines:\n        "DEBUG"\n        "LEVEL=3"\n        "EMPTY="\n        "S=a=b"\n',
    exp("D", targets=[tgt("executable", "a", ["a.c7"], defs=["DEBUG", "LEVEL=3", "EMPTY=", "S=a=b"])]))
valid["target_props"] = ('project "T"\ntarget library "l":\n    sources:\n        "l.c7"\n    output "out/l.l7"\n    platform "x86_64-linux"\n    link "dynamic"\n    std "default"\n',
    exp("T", targets=[tgt("library", "l", ["l.c7"], output="out/l.l7", platform="x86_64-linux", link="dynamic", std="default")]))
valid["indent_widths"] = ('project "I"\ntarget executable "a":\n  sources:\n          "a.c7"\ntarget executable "b":\n        sources:\n         "b.c7"\n',
    exp("I", targets=[tgt("executable", "a", ["a.c7"]), tgt("executable", "b", ["b.c7"])]))
valid["names"] = ('project "N"\ntarget executable "a-b_c9":\n    sources:\n        "a.c7"\n', exp("N", targets=[tgt("executable", "a-b_c9", ["a.c7"])]))
valid["project_only"] = ('project "Only"\n', exp("Only"))
valid["no_trailing_newline"] = ('project "X"\ntarget executable "a":\n    sources:\n        "a.c7"', exp("X", targets=[tgt("executable", "a", ["a.c7"])]))
for name, (text, e) in valid.items():
    open(f"{V}/{name}.txt", "w", encoding="utf-8", newline="").write(text)
    open(f"{V}/{name}.json", "w", encoding="utf-8").write(json.dumps(e, ensure_ascii=False, sort_keys=True, indent=1) + "\n")

BASE = 'project "P"\ntarget executable "a":\n    sources:\n        "a.c7"\n'
inv = {}  # name -> (code, text with @@ marker, extra flags)
def tg(extra, sources='        "a.c7"\n'): return 'project "P"\ntarget executable "a":\n    sources:\n' + sources + extra
inv["tab_indent"] = ("E0702", 'project "P"\ntarget executable "a":\n@@\tsources:\n        "a.c7"\n')
inv["unterminated_string"] = ("E0702", 'project @@"P\n')
inv["bad_escape"] = ("E0702", 'project "a@@\\qb"\n')
inv["bad_char"] = ("E0702", 'project "P"\n@@@ target\n')
inv["dedent_mismatch"] = ("E0702", 'project "P"\ntarget executable "a":\n        sources:\n            "a.c7"\n@@    output "x"\n')
inv["indent_first_line"] = ("E0702", '@@  project "P"\n')
inv["no_project"] = ("E0702", '@@target executable "a":\n    sources:\n        "a.c7"\n')
inv["project_no_name"] = ("E0702", '@@project\n')
inv["target_no_sources"] = ("E0702", 'project "P"\n@@target executable "a":\n    output "x"\n')
inv["sources_no_colon"] = ("E0702", 'project "P"\ntarget executable "a":\n    @@sources\n        "a.c7"\n')
inv["list_entry_not_string"] = ("E0702", 'project "P"\ntarget executable "a":\n    sources:\n        @@a_c7\n')
inv["list_entry_two_tokens"] = ("E0702", 'project "P"\ntarget executable "a":\n    sources:\n        @@"a.c7" "b.c7"\n')
inv["stray_cr"] = ("E0702", 'project "P"\ntarget executable "a":\n    sources:\n        "a.c7@@\r"\n'.replace('@@\r"\n', '"\n'))  # placeholder replaced below
inv["bad_utf8"] = ("E0702", None)
inv["too_deep"] = ("E0702", 'project "P"\n' + "".join(" " * (i + 1) + "x\n" for i in range(8)).replace(" x\n", "x\n", 0) )
inv["extra_after_project_name"] = ("E0702", 'project "P" @@"Q"\n')
inv["colon_in_middle"] = ("E0702", 'project "P"\ntarget executable @@: "a"\n')
inv["block_on_registry"] = ("E0702", 'project "P"\nregistry "r"\n    @@x\n')
inv["unknown_top"] = ("E0703", 'project "P"\n@@frobnicate "x"\n')
inv["unknown_in_target"] = ("E0703", tg('    @@color "red"\n'))
inv["unknown_target_kind"] = ("E0703", 'project "P"\ntarget @@shared "a":\n    sources:\n        "a.c7"\n')
inv["unknown_in_project"] = ("E0703", 'project "P":\n    @@author "me"\n')
inv["unknown_in_config"] = ("E0703", 'project "P"\nconfig c:\n    @@sanitize true\n')
inv["missing_source_file"] = ("E0704", 'project "P"\ntarget executable "a":\n    sources:\n        @@"nope/missing.c7"\n', "--check-files")
inv["duplicate_target"] = ("E0705", BASE + 'target library @@"a":\n    sources:\n        "b.c7"\n')
inv["target_equals_dependency"] = ("E0705", 'project "P"\ndependency "a" "1.0.0"\ntarget executable @@"a":\n    sources:\n        "a.c7"\n')
inv["duplicate_project"] = ("E0708", 'project "P"\n@@project "Q"\n')
inv["duplicate_source"] = ("E0708", tg('', '        "a.c7"\n        @@"./a.c7"\n'))
inv["conflicting_define"] = ("E0708", tg('    defines:\n        "N=1"\n        @@"N=2"\n'))
inv["duplicate_define"] = ("E0708", tg('    defines:\n        "N=1"\n        @@"N=1"\n'))
inv["duplicate_property"] = ("E0708", tg('    output "x"\n    @@output "y"\n'))
inv["duplicate_config"] = ("E0708", 'project "P"\nconfig c:\nconfig @@c:\n')
inv["duplicate_dependency"] = ("E0708", 'project "P"\ndependency "A" "1.0.0"\ndependency @@"A" "2.0.0"\n')
inv["duplicate_depends"] = ("E0708", tg('    depends:\n        "x"\n        @@"x"\n'))
inv["bad_project_version"] = ("E0717", 'project "P":\n    version @@"1.x.y"\n')
inv["bad_requirement"] = ("E0717", 'project "P"\ndependency "A" @@"^^1"\n')
inv["bad_mode"] = ("E0719", 'project "P"\nconfig c:\n    mode @@"fast"\n')
inv["bad_optimization"] = ("E0719", 'project "P"\nconfig c:\n    optimization @@"O9"\n')
inv["bad_bool"] = ("E0719", 'project "P"\nconfig c:\n    bounds_check @@yes\n')
inv["bad_platform"] = ("E0719", tg('    platform @@"linux"\n'))
inv["bad_link"] = ("E0719", tg('    link @@"auto"\n'))
inv["bad_std"] = ("E0719", tg('    std @@"yes"\n'))
inv["source_escapes_project"] = ("E0719", 'project "P"\ntarget executable "a":\n    sources:\n        @@"../x.c7"\n')
inv["source_absolute"] = ("E0719", 'project "P"\ntarget executable "a":\n    sources:\n        @@"/etc/x.c7"\n')
inv["source_backslash"] = ("E0719", 'project "P"\ntarget executable "a":\n    sources:\n        @@"src\\\\x.c7"\n')
inv["source_is_dot"] = ("E0719", 'project "P"\ntarget executable "a":\n    sources:\n        @@"./"\n')
inv["source_directory"] = ("E0719", 'project "P"\ntarget executable "a":\n    sources:\n        @@"src/"\n')
inv["source_drive_letter"] = ("E0719", 'project "P"\ntarget executable "a":\n    sources:\n        @@"C:/x.c7"\n')
inv["output_escapes"] = ("E0719", tg('    output @@"../out"\n'))
inv["include_escapes"] = ("E0719", tg('    include_dirs:\n        @@"a/../../b"\n'))
inv["default_config_unknown"] = ("E0719", 'project "P"\ndefault_config @@nothing\n')
inv["bad_define_name"] = ("E0719", tg('    defines:\n        @@"1X=2"\n'))
inv["bad_target_name"] = ("E0719", 'project "P"\ntarget executable @@"1app":\n    sources:\n        "a.c7"\n')
inv["empty_project_name"] = ("E0719", 'project @@""\n')
inv["bad_depends_name"] = ("E0719", tg('    depends:\n        @@"not a name"\n'))
inv["escaped_dotdot_in_sources_deep"] = ("E0719", 'project "P"\ntarget executable "a":\n    sources:\n        @@"a/b/../../../c.c7"\n')

def place(text):
    i = text.index("@@")
    before = text[:i].encode("utf-8")
    line = before.count(b"\n") + 1
    col = len(before) - (before.rfind(b"\n") + 1) + 1
    return text.replace("@@", "", 1), line, col

for name, tup in inv.items():
    code, text = tup[0], tup[1]
    flags = tup[2] if len(tup) > 2 else ""
    if name == "bad_utf8":
        open(f"{I}/{name}.txt", "wb").write(b'project "P\xff"\n'); loc = (1, 1)
    elif name == "stray_cr":
        open(f"{I}/{name}.txt", "wb").write(b'project "P"\ntarget executable "a":\n    sources:\n        "a.c7" \r "b"\n'); loc = (4, 16)
    elif name == "too_deep":
        t = 'project "P"\n' + "".join(" " * (2 * (i + 1)) + "x\n" for i in range(10))
        open(f"{I}/{name}.txt", "w").write(t); loc = (9, 1)  # up to 8 open levels (7 indents) are allowed; the 8th indent, on line 9, is refused
    else:
        t, ln, col = place(text)
        open(f"{I}/{name}.txt", "w", encoding="utf-8", newline="").write(t); loc = (ln, col)
    open(f"{I}/{name}.expect", "w").write(f"{code} {loc[0]}:{loc[1]}" + (f" {flags}" if flags else "") + "\n")
print(len(valid), "valid,", len(inv), "invalid")
