// c7c driver (stage 1: frontend only). Subcommands: check | lex | pp | ast
#include <fstream>
#include <iostream>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <optional>
#include <cstdlib>
#include <filesystem>

#include "../frontend/c7.hpp"
#include "../ir/ir.hpp"
#include "../pkg/package.hpp"
#include "../pkg/tools.hpp"

using namespace c7;

// The standard library is compiled together with every program. It comes from std.l7 beside the executable (a verified
// .l7 package), from the directory in C7_STDLIB, or - while bootstrapping - from the stdlib/ directory beside the executable.
static std::vector<NodeP> parseSources(DiagEngine& d, const std::vector<std::pair<std::string, std::string>>& srcs) {
  std::vector<NodeP> units;
  for (auto& s : srcs) {
    int fid = d.addFile(s.first, s.second);
    units.push_back(parse(d, lex(d, fid)));
  }
  return units;
}

static std::vector<NodeP> loadStdlib(DiagEngine& d) {
  namespace fs = std::filesystem;
  const char* env = std::getenv("C7_STDLIB");
  std::string exeDir = fs::read_symlink("/proc/self/exe").parent_path().string();
  std::error_code ec;
  if (!env && fs::exists(exeDir + "/std.l7", ec)) {
    c7pkg::Package pkg;
    if (!c7pkg::loadPackage(d, exeDir + "/std.l7", pkg)) return {};
    std::vector<std::pair<std::string, std::string>> srcs;
    for (auto& s : pkg.sources()) srcs.emplace_back(pkg.label + "!" + s.first, s.second);
    return parseSources(d, srcs);
  }
  std::string dir = env ? env : exeDir + "/stdlib";
  std::vector<std::string> files;
  for (auto& e : fs::directory_iterator(dir, ec)) if (e.path().extension() == ".c7") files.push_back(e.path().string());
  std::sort(files.begin(), files.end());
  std::vector<std::pair<std::string, std::string>> srcs;
  for (auto& f : files) {
    std::ifstream in(f, std::ios::binary);
    std::stringstream ss; ss << in.rdbuf();
    srcs.emplace_back(f, ss.str());
  }
  return parseSources(d, srcs);
}

// Extra user sources (`c7c build a.c7 b.c7`) and library packages (`--lib x.l7`) become additional compilation units.
static std::vector<NodeP> loadExtraUnits(DiagEngine& d, const PPOptions& po, const std::vector<std::string>& extra, const std::vector<std::string>& libs) {
  std::vector<NodeP> units;
  for (auto& f : extra) {
    std::ifstream in(f, std::ios::binary);
    if (!in) { d.report("E0704", c7pkg::anchorLoc(d, f), {f}); continue; }
    std::stringstream ss; ss << in.rdbuf();
    int fid = d.addFile(f, ss.str());
    units.push_back(parse(d, preprocess(d, fid, po)));
  }
  for (auto& l : libs) {
    c7pkg::Package pkg;
    if (!c7pkg::loadPackage(d, l, pkg)) continue;
    PPOptions lo;  // a library is preprocessed with the builtin macros only, not the user's -D options
    lo.os = po.os; lo.arch = po.arch; lo.debug = po.debug; lo.freestanding = po.freestanding;
    for (auto& s : pkg.sources()) {
      int fid = d.addFile(pkg.label + "!" + s.first, s.second);
      units.push_back(parse(d, preprocess(d, fid, lo)));
    }
  }
  return units;
}

static bool isL7Path(const std::string& p) { return p.size() > 3 && p.compare(p.size() - 3, 3, ".l7") == 0; }

static void usage() {
  std::cerr << "usage: c7c <build|test|check|parse|lex|pp|ast> file.c7 [-Idir] [-DNAME[=V]] [--lang=en|ko]\n"
               "           [--diag-format=text|json] [--target=ARCH-OS] [-ffreestanding] [--debug] [-fdebug-alloc] [-fbounds-check]\n"
               "           [-foverflow-check|-fno-overflow-check] [--link=static|dynamic] [--lib x.l7] [--depfile f]\n";
}

int main(int argc, char** argv) {
  if (argc >= 2 && std::string(argv[1]) == "--version") {
    std::cout << "c7c " << c7pkg::kCompilerVersion << " (ABI " << c7pkg::kAbiId << ")\n";
    return 0;
  }
  if (argc >= 3 && std::string(argv[1]) == "tool") return c7pkg::runTool(argc, argv);
  if (c7pkg::isProjectCommand(argc, argv)) return c7pkg::runProjectCommand(argc, argv);
  if (argc < 3) { usage(); return 2; }
  std::string cmd = argv[1], file, depfile;
  std::vector<std::string> extraFiles, libs;
  PPOptions po;
  bool json = false, keep = false, noStd = false, staticLink = false;
  std::optional<bool> overflowOverride;  // -foverflow-check / -fno-overflow-check (applied last: they beat --debug's implied check)
  CodegenOptions cg;
  std::string outPath, emit = "exe", optLevel = "0";
  DiagEngine d;
  for (int i = 2; i < argc; i++) {
    std::string a = argv[i];
    if (a.rfind("-I", 0) == 0) po.includeDirs.push_back(a.size() > 2 ? a.substr(2) : (i + 1 < argc ? argv[++i] : ""));
    else if (a.rfind("-D", 0) == 0) po.defines.push_back(a.substr(2));
    else if (a.rfind("--lang=", 0) == 0) d.lang = a.substr(7);
    else if (a == "--diag-format=json") json = true;
    else if (a.rfind("--target=", 0) == 0) {
      std::string t = a.substr(9);
      auto dash = t.find('-');
      std::string arch = t.substr(0, dash), os = dash == std::string::npos ? "linux" : t.substr(dash + 1);
      po.arch = (arch == "i686") ? "x86" : arch;
      po.os = os;
    } else if (a == "-ffreestanding") po.freestanding = true;
    else if (a == "--debug") { po.debug = true; cg.overflowCheck = true; }
    else if (a == "-o" && i + 1 < argc) outPath = argv[++i];
    else if (a.size() == 3 && a.rfind("-O", 0) == 0 && isdigit((unsigned char)a[2])) optLevel = a.substr(2);
    else if (a.rfind("--emit=", 0) == 0) emit = a.substr(7);
    else if (a == "--keep") keep = true;
    else if (a == "--no-std") noStd = true;
    else if (a == "-fdebug-alloc") cg.debugAlloc = true;
    else if (a == "-fbounds-check") cg.boundsCheck = true;
    else if (a == "-foverflow-check") overflowOverride = true;
    else if (a == "-fno-overflow-check") overflowOverride = false;
    else if (a == "--link=static") staticLink = true;
    else if (a == "--link=dynamic") staticLink = false;
    else if (a == "--lib" && i + 1 < argc) libs.push_back(argv[++i]);
    else if (a.rfind("--lib=", 0) == 0) libs.push_back(a.substr(6));
    else if (a == "--depfile" && i + 1 < argc) depfile = argv[++i];
    else if (!a.empty() && a[0] == '-') { std::cerr << "c7c: unknown option " << a << "\n"; return 2; }
    else if (file.empty()) file = a;
    else extraFiles.push_back(a);
  }
  if (overflowOverride) cg.overflowCheck = *overflowOverride;
  if (file.empty()) { usage(); return 2; }
  std::ifstream in(file, std::ios::binary);
  if (!in) { std::cerr << "c7c: cannot open " << file << "\n"; return 2; }
  std::stringstream ss;
  ss << in.rdbuf();
  int fid = d.addFile(file, ss.str());

  // `c7c test`: collect top-level @test functions, append a main that runs the one chosen by C7_TEST_INDEX, build once,
  // then run every test in its own process so a panic/abort fails only that test.
  bool isTest = cmd == "test";
  std::vector<std::string> testNames;
  std::string testExe;
  bool testExeTemp = false;
  if (isTest) {
    po.defines.push_back("C7_TEST");  // lets a file keep its own main under #ifndef C7_TEST
    {
      auto toks = preprocess(d, fid, po);
      auto ast = parse(d, std::move(toks));
      if (d.errorCount() != 0) { d.print(json ? std::cout : std::cerr, json); return 1; }
      for (auto& k : ast->kids)
        if (k->kind == NK::Func)
          for (auto& a : k->kids) if (a->kind == NK::Attribute && a->text == "test") testNames.push_back(k->text);
    }
    if (testNames.empty()) { std::cout << "c7c test: no @test functions found in " << file << "\n"; return 0; }
    std::string src = ss.str() + "\nextern \"C\" { i64 c7_test_index(); }\ni32 main() {\n    i64 idx = c7_test_index();\n";
    for (size_t i = 0; i < testNames.size(); i++) src += "    if (idx == " + std::to_string(i) + ") { " + testNames[i] + "(); }\n";
    src += "    return 0;\n}\n";
    fid = d.addFile(file, src);  // same display name; appended text keeps the original line numbers
    cmd = "build";
    testExeTemp = outPath.empty();
    testExe = outPath.empty() ? (std::filesystem::temp_directory_path() / ("c7test-" + std::to_string(::getpid()))).string() : outPath;
    outPath = testExe;
  }

  if (cmd == "lex") {
    for (auto& t : lex(d, fid)) {
      if (t.kind == TK::Eof) break;
      std::cout << t.loc.line << ":" << t.loc.col << " " << tkName(t.kind) << " " << t.text << "\n";
    }
  } else if (cmd == "build") {
    auto toks = preprocess(d, fid, po);
    auto ast = parse(d, std::move(toks));
    ir::Module mod;
    std::vector<NodeP> lib = loadExtraUnits(d, po, extraFiles, libs);
    if (!noStd) { auto stdUnits = loadStdlib(d); for (auto& u : stdUnits) lib.push_back(std::move(u)); }
    std::vector<Node*> unitsV{ast.get()};
    for (auto& u : lib) unitsV.push_back(u.get());
    if (d.errorCount() == 0 && analyze(d, unitsV, &mod, cg)) {
      auto errs = ir::verify(mod);
      for (auto& e : errs) d.report("E0902", Loc{fid, 1, 1}, {e});
      if (errs.empty()) {
        namespace fs = std::filesystem;
        if (emit == "ir") { if (outPath.empty()) ir::printModule(mod, std::cout, true); else { std::ofstream o(outPath); ir::printModule(mod, o, true); } }
        else {
          fs::path dir = fs::temp_directory_path() / ("c7c-" + std::to_string(::getpid()));
          fs::create_directories(dir);
          std::string ll = (dir / "out.ll").string(), obj = (dir / "out.o").string();
          { std::ofstream o(ll); ir::printModule(mod, o, false); }
          if (emit == "llvm") { std::ifstream i(ll); std::stringstream b; b << i.rdbuf(); if (outPath.empty()) std::cout << b.str(); else std::ofstream(outPath) << b.str(); }
          else {
            std::string cmdl = "llc -O" + optLevel + " -filetype=obj -relocation-model=pic " + ll + " -o " + obj + " 2>&1";
            if (std::system(cmdl.c_str()) != 0) d.report("E0903", Loc{fid, 1, 1}, {"llc"});
            else if (emit == "obj") { fs::copy_file(obj, outPath.empty() ? "out.o" : outPath, fs::copy_options::overwrite_existing); }
            else {
              std::string exeDir = fs::read_symlink("/proc/self/exe").parent_path().string();
              const char* rt = std::getenv("C7_RUNTIME");
              std::string rtPath = rt ? rt : exeDir + "/runtime/c7rt.c";
              std::string out = outPath.empty() ? "a.out" : outPath;
              std::string link = std::string("gcc -O1 ") + (staticLink ? "-static " : "") + obj + " " + rtPath + " -o " + out + " 2>&1";
              if (std::system(link.c_str()) != 0) d.report("E0903", Loc{fid, 1, 1}, {"gcc (system linker; own linker arrives in stage 6)"});
            }
          }
          if (!keep) fs::remove_all(dir); else std::cerr << "kept " << dir << "\n";
        }
      }
    }
  } else if (cmd == "pp" || cmd == "ast" || cmd == "parse" || cmd == "check") {
    auto toks = preprocess(d, fid, po);
    if (cmd == "pp") {
      for (auto& t : toks) if (t.kind != TK::Eof) std::cout << t.text << " ";
      std::cout << "\n";
    } else {
      auto ast = parse(d, std::move(toks));
      if (cmd == "ast" && d.errorCount() == 0) dump(*ast, std::cout);
      if (cmd == "check" && d.errorCount() == 0) {
        std::vector<NodeP> lib = loadExtraUnits(d, po, extraFiles, libs);
        if (!noStd) { auto stdUnits = loadStdlib(d); for (auto& u : stdUnits) lib.push_back(std::move(u)); }
        std::vector<Node*> unitsV{ast.get()};
        for (auto& u : lib) unitsV.push_back(u.get());
        if (d.errorCount() == 0) analyze(d, unitsV);
      }
    }
  } else { usage(); return 2; }
  if (isTest && d.errorCount() == 0) {
    int passed = 0, failed = 0;
    for (size_t i = 0; i < testNames.size(); i++) {
      std::cout << "test " << testNames[i] << " ... " << std::flush;
      int st = std::system(("C7_TEST_INDEX=" + std::to_string(i) + " " + testExe).c_str());
      if (st == 0) { std::cout << "ok\n"; passed++; continue; }
      failed++;
      if (WIFEXITED(st)) {
        int rc = WEXITSTATUS(st);
        std::cout << "FAILED (exit " << rc << (rc == 70 && cg.debugAlloc ? ": debug allocator reported leaks" : "") << ")\n";
      } else std::cout << "FAILED (" << (WIFSIGNALED(st) ? "signal " + std::to_string(WTERMSIG(st)) : std::string("abnormal")) << ")\n";
    }
    if (testExeTemp) std::filesystem::remove(testExe);
    std::cout << "test result: " << passed << " passed; " << failed << " failed\n";
    if (!d.diags.empty()) d.print(json ? std::cout : std::cerr, json);
    return failed ? 1 : 0;
  }
  if (!depfile.empty() && d.errorCount() == 0) {  // the real files this build read: sources and #include targets
    namespace fs = std::filesystem;
    std::string stdDir = fs::read_symlink("/proc/self/exe").parent_path().string() + "/stdlib/";
    std::ofstream df(depfile);
    for (auto& f : d.files)
      // `.l7` packages are not listed (B3): they are explicit inputs (--lib, std.l7) that a build system hashes itself, and
      // their diagnostic anchor files (`x.l7`, `x.l7!src/a.c7`) are not real sources.
      if (f->path.find('!') == std::string::npos && f->path.rfind(stdDir, 0) != 0 && !isL7Path(f->path) && fs::is_regular_file(f->path))
        df << fs::absolute(f->path).lexically_normal().string() << "\n";
  }
  if (!d.diags.empty()) d.print(json ? std::cout : std::cerr, json);
  return d.errorCount() ? 1 : 0;
}
