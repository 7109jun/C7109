// Project mode of the driver (stage 5, W8 + W10): `c7c build|pack|verify|clean [path] ...` on a Cmake7109.txt project.
// The single-file mode (`c7c build prog.c7`) is untouched: any `.c7` argument selects it (see isProjectCommand).
//
// Execution model: the unit of work is a target. Each target runs as a child `c7c` (library targets: `c7c check`, then
// the package is written in-process). Targets start only after everything they depend on succeeded; independent targets
// run concurrently (-jN). A failure stops *starting* new work, running targets are awaited, output is replayed per target
// in graph order so it is deterministic whatever the scheduling was.
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <thread>

#include "buildfile.hpp"
#include "graph.hpp"
#include "options.hpp"
#include "lock.hpp"
#include "mapfile.hpp"
#include "package.hpp"
#include "resolver.hpp"
#include "sha256.hpp"
#include "tools.hpp"
#include "version.hpp"

extern char** environ;
using namespace c7;
namespace fs = std::filesystem;

namespace c7pkg {

bool isProjectCommand(int argc, char** argv) {
  if (argc < 2) return false;
  std::string cmd = argv[1];
  if (cmd != "build" && cmd != "pack" && cmd != "verify" && cmd != "install" && cmd != "lock" && cmd != "clean") return false;
  for (int i = 2; i < argc; i++) {
    std::string a = argv[i];
    if (a.size() > 3 && a.compare(a.size() - 3, 3, ".c7") == 0 && cmd == "build") return false;  // single-file mode
    if (a.size() > 3 && a.compare(a.size() - 3, 3, ".c7") == 0 && (cmd == "pack" || cmd == "lock" || cmd == "install")) return false;
  }
  return true;
}

namespace {

struct Cli {
  std::string cmd, path = ".", cset7, outDir = "build", cacheDir;
  std::vector<std::string> targets, registries;
  std::string config, prefix;
  int jobs = 0;
  bool noCache = false, verbose = false;
  OptionLayer layer;
  std::vector<std::string> files;  // verify: .l7 files given on the command line
};

void usageProject() {
  std::cerr << "usage: c7c build [path] [--target NAME]... [--config NAME] [-jN] [-O<n>] [--mode=debug|release] [--cset7 FILE]\n"
               "                 [--registry=DIR] [--cache-dir DIR] [--no-cache] [--out-dir DIR] [--verbose]\n"
               "       c7c pack [path] ...    build the library targets only\n"
               "       c7c verify [path|file.l7...]   check the packages a project produced (or the given files)\n"
               "       c7c lock [path]        write Cmake7109.cr7 (the only command that does)\n"
               "       c7c install [path|file.l7...] [--prefix DIR]   install packages to <prefix>/<name>-<version>/\n"
               "       c7c clean [path] [--out-dir DIR]\n"
               "       exit status: 0 success, 1 build error, 2 usage error\n";
}

bool parseCli(int argc, char** argv, Cli& c) {
  c.cmd = argv[1];
  bool havePath = false;
  for (int i = 2; i < argc; i++) {
    std::string a = argv[i];
    auto need = [&](std::string& out) { if (i + 1 >= argc) { std::cerr << "c7c: " << a << " needs a value\n"; return false; } out = argv[++i]; return true; };
    std::string err;
    if (a == "--target") { std::string v; if (!need(v)) return false; c.targets.push_back(v); }
    else if (a == "--config") { if (!need(c.config)) return false; }
    else if (a == "--prefix") { if (!need(c.prefix)) return false; }
    else if (a == "--cset7") { if (!need(c.cset7)) return false; }
    else if (a == "--cache-dir") { if (!need(c.cacheDir)) return false; }
    else if (a == "--out-dir") { if (!need(c.outDir)) return false; }
    else if (a == "--registry") { std::string v; if (!need(v)) return false; c.layer.registry.push_back(v); }
    else if (a == "--no-cache") c.noCache = true;
    else if (a == "--verbose") c.verbose = true;
    else if (a.rfind("-j", 0) == 0 && a.size() > 2 && std::all_of(a.begin() + 2, a.end(), ::isdigit) && a.size() < 8) c.jobs = std::atoi(a.c_str() + 2);
    else if (a == "-j") { std::string v; if (!need(v) || v.empty() || !std::all_of(v.begin(), v.end(), ::isdigit) || v.size() > 5) { std::cerr << "c7c: -j needs a number\n"; return false; } c.jobs = std::atoi(v.c_str()); }
    else if (parseOptionArg(a, c.layer, err)) { if (!err.empty()) { std::cerr << "c7c: " << err << "\n"; return false; } }
    else if (!a.empty() && a[0] == '-') { std::cerr << "c7c: unknown option " << a << "\n"; return false; }
    else if (a.size() > 3 && a.compare(a.size() - 3, 3, ".l7") == 0 && (c.cmd == "verify" || c.cmd == "install")) c.files.push_back(a);
    else if (!havePath) { c.path = a; havePath = true; }
    else { std::cerr << "c7c: unexpected argument " << a << "\n"; return false; }
  }
  return true;
}

std::string selfPath() {
  char buf[4096];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
  return n > 0 ? std::string(buf, n) : "c7c";
}

struct Project {
  std::string dir;  // project directory (absolute)
  std::string file;
  int fid = -1;
  BuildFile b;
  BuildGraph g;
  std::vector<Options> opts;  // per target
  std::vector<std::vector<std::string>> libsOf;  // per target: transitive library targets (graph order) whose .l7 must be passed
  std::vector<Resolved> resolved;                  // external packages, dependencies first
  std::vector<std::vector<size_t>> pkgsOf;         // per target: indices into `resolved` (transitive, dependencies first)
  std::string lockPath, config;                    // lock file location; selected configuration name
};

// Runs a child and waits for it (used for path-dependency sub-projects). Output goes to `log`.
int runAndWait(const std::vector<std::string>& argv, const std::string& cwd, const std::string& log) {
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 1, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  posix_spawn_file_actions_adddup2(&fa, 1, 2);
  posix_spawn_file_actions_addchdir_np(&fa, cwd.c_str());
  std::vector<char*> av;
  for (auto& a : argv) av.push_back(const_cast<char*>(a.c_str()));
  av.push_back(nullptr);
  pid_t pid;
  int rc = posix_spawn(&pid, argv[0].c_str(), &fa, nullptr, av.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  if (rc != 0) return 127;
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
  return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
std::string slurp(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss; ss << f.rdbuf();
  return ss.str();
}



// Builds the library target `dep.name` of the sub-project in `dir` (a path dependency that is a directory) and returns its .l7.
bool buildSubprojectLibrary(DiagEngine& d, const Cli& c, const Project& parent, const BuildDependency& dep, const std::string& dir, int depth, std::string& file) {
  DiagEngine sd;
  fs::path bf = fs::path(dir) / "Cmake7109.txt";
  std::error_code ec;
  if (!fs::is_regular_file(bf, ec)) { d.report("E0716", dep.loc, {dep.name, "`" + dep.path + "` has no Cmake7109.txt"}); return false; }
  std::string text = slurp(bf.string());
  int fid = sd.addFile(bf.string(), text);
  BuildFile sb;
  BuildGraph sg;
  if (!parseBuildFile(sd, fid, dir, sb) || !buildGraph(sd, sb, "build", kSupportedPlatform, sg)) {
    std::ostringstream os; sd.print(os, false);
    std::cerr << os.str();
    d.report("E0716", dep.loc, {dep.name, "the project in `" + dep.path + "` does not load"});
    return false;
  }
  size_t ti = sb.targets.size();
  for (size_t i = 0; i < sb.targets.size(); i++) if (sb.targets[i].name == dep.name && sb.targets[i].kind == BuildTarget::Library) ti = i;
  if (ti == sb.targets.size()) { d.report("E0716", dep.loc, {dep.name, "the project in `" + dep.path + "` has no library target named `" + dep.name + "`"}); return false; }
  std::vector<std::string> argv{selfPath(), "build", dir, "--target", dep.name};
  if (c.noCache) argv.push_back("--no-cache");
  if (!c.cacheDir.empty()) { argv.push_back("--cache-dir"); argv.push_back(c.cacheDir); }
  if (c.jobs > 0) argv.push_back("-j" + std::to_string(c.jobs));
  setenv("C7_PROJECT_DEPTH", std::to_string(depth + 1).c_str(), 1);
  char tmpl[] = "/tmp/c7sub.XXXXXX";
  if (!mkdtemp(tmpl)) return false;
  std::string log = std::string(tmpl) + "/log";
  int rc = runAndWait(argv, parent.dir, log);
  setenv("C7_PROJECT_DEPTH", std::to_string(depth).c_str(), 1);
  std::string out = slurp(log);
  fs::remove_all(tmpl, ec);
  if (rc != 0) {
    std::cerr << out;
    d.report("E0720", dep.loc, {dep.name});
    return false;
  }
  file = (fs::path(dir) / sg.nodes[ti].output).string();
  return true;
}

// Loads and checks everything that does not need a child process. Prints diagnostics; returns false on error.
bool loadProject(DiagEngine& d, const Cli& c, Project& p, bool useLock = true) {
  std::error_code ec;
  fs::path in = fs::path(c.path);
  fs::path file = fs::is_directory(in, ec) ? in / "Cmake7109.txt" : in;
  if (!fs::is_regular_file(file, ec)) { std::cerr << "c7c: no build file: " << file.string() << "\n"; return false; }
  p.dir = fs::absolute(file.parent_path()).lexically_normal().string();
  if (p.dir.size() > 1 && p.dir.back() == '/') p.dir.pop_back();
  p.file = file.string();
  std::string text;
  { std::ifstream f(file, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); text = ss.str(); }
  p.fid = d.addFile(p.file, text);
  if (!parseBuildFile(d, p.fid, p.dir, p.b)) return false;
  std::string outDir, why;
  if (!normalizeProjectPath(c.outDir, outDir, why)) { d.report("E0719", Loc{p.fid, 1, 1}, {"--out-dir", why}); return false; }
  if (!buildGraph(d, p.b, outDir, kSupportedPlatform, p.g)) return false;
  // option layers: defaults < .cset7 < config < target < command line
  OptionLayer cset;
  std::string cs = c.cset7;
  if (cs.empty() && fs::is_regular_file(fs::path(p.dir) / ".cset7", ec)) cs = (fs::path(p.dir) / ".cset7").string();
  if (!cs.empty()) {
    std::ifstream f(cs, std::ios::binary);
    if (!f) { std::cerr << "c7c: cannot read " << cs << "\n"; return false; }
    std::stringstream ss; ss << f.rdbuf();
    int cf = d.addFile(cs, ss.str());
    if (!loadCset7(d, cf, cset)) return false;
  }
  OptionLayer cfg;
  std::string cname = c.config.empty() ? p.b.defaultConfig : c.config;
  p.config = cname;
  if (!cname.empty()) {
    bool found = false;
    for (auto& k : p.b.configs) if (k.name == cname) { cfg = layerFromConfig(k); found = true; }
    if (!found) { d.report("E0719", Loc{p.fid, 1, 1}, {"--config", "no `config " + cname + "` is defined"}); return false; }
  }
  bool ok = true;
  for (size_t i = 0; i < p.b.targets.size(); i++) {
    OptionLayer tl = layerFromTarget(p.b.targets[i]);
    Options o = mergeOptions({&cset, &cfg, &tl, &c.layer});
    if (!checkOptionsCompatible(d, p.b.targets[i].loc, o)) ok = false;
    p.opts.push_back(o);
  }
  if (!ok) return false;
  // external dependencies: registries (command line, .cset7, build file; first wins a tie) and path dependencies
  {
    std::vector<std::string> regs;
    for (auto& r : c.layer.registry) regs.push_back(fs::absolute(r).string());
    OptionLayer none;
    Options all = mergeOptions({&cset, &none});
    for (auto& r : all.registry) regs.push_back(r);
    for (auto& r : p.b.registries) regs.push_back(r);
    std::vector<std::string> uniq;
    for (auto& r : regs) if (std::find(uniq.begin(), uniq.end(), r) == uniq.end()) uniq.push_back(r);
    std::vector<RootDep> roots;
    int depth = std::getenv("C7_PROJECT_DEPTH") ? std::atoi(std::getenv("C7_PROJECT_DEPTH")) : 0;
    for (auto& dep : p.b.deps) {
      RootDep r{dep.name, dep.req, dep.isPath, "", dep.path, dep.loc};
      if (dep.isPath) {
        fs::path q(dep.path);
        if (q.is_relative()) q = fs::path(p.dir) / q;
        q = q.lexically_normal();
        if (fs::is_directory(q, ec)) {  // a sub-project: build its library target of the same name first
          if (depth >= 8) { d.report("E0706", dep.loc, {"project dependencies nested more than 8 deep (a cycle of path dependencies?)"}); return false; }
          std::string sub;
          if (!buildSubprojectLibrary(d, c, p, dep, q.string(), depth, sub)) return false;
          r.file = sub;
        } else if (fs::is_regular_file(q, ec)) r.file = q.string();
        else { d.report("E0716", dep.loc, {dep.name, "path `" + dep.path + "` is neither a package file nor a project directory"}); return false; }
      }
      roots.push_back(r);
    }
    p.lockPath = (fs::path(p.dir) / "Cmake7109.cr7").string();
    LockFile lk;
    LockPins pins;
    bool locked = false;
    if (useLock && fs::is_regular_file(p.lockPath, ec)) {  // read only: a build never rewrites the lock
      int lf = d.addFile(p.lockPath, slurp(p.lockPath));
      if (!loadLock(d, lf, lk)) return false;
      pins.label = p.lockPath;
      for (auto& ld : lk.deps) pins.versions[ld.name] = ld.version;
      locked = true;
    }
    if (!resolveDependencies(d, roots, uniq, p.dir, p.resolved, locked ? &pins : nullptr)) return false;
    if (locked) {
      Loc at = anchorLoc(d, p.lockPath);
      bool good = true;
      std::map<std::string, const LockDep*> byName;
      for (auto& ld : lk.deps) byName[ld.name] = &ld;
      for (auto& r : p.resolved) {
        auto f = byName.find(r.name);
        if (f == byName.end()) { d.report("E0714", at, {p.lockPath, "`" + r.name + "` is required but not in the lock file (run `c7c lock`)"}); good = false; continue; }
        if (f->second->source != r.sourceId) { d.report("E0714", at, {p.lockPath, "`" + r.name + "` now comes from `" + r.sourceId + "`, the lock says `" + f->second->source + "` (run `c7c lock`)"}); good = false; continue; }
        if (f->second->sha256 != r.sha256) { d.report("E0710", at, {r.name + " " + r.version, p.lockPath}); good = false; }
      }
      for (auto& ld : lk.deps) {
        bool found = false;
        for (auto& r : p.resolved) if (r.name == ld.name) found = true;
        if (!found) { d.report("E0714", at, {p.lockPath, "it lists `" + ld.name + "`, which the project no longer requires (run `c7c lock`)"}); good = false; }
      }
      if (!good) return false;
    }
  }
  // transitive library closure per target, in graph order
  p.libsOf.assign(p.b.targets.size(), {});
  p.pkgsOf.assign(p.b.targets.size(), {});
  for (size_t i = 0; i < p.b.targets.size(); i++) {
    std::set<int> seen;
    std::vector<int> st(p.g.nodes[i].deps.begin(), p.g.nodes[i].deps.end());
    while (!st.empty()) {
      int u = st.back(); st.pop_back();
      if (!seen.insert(u).second) continue;
      for (int v : p.g.nodes[u].deps) st.push_back(v);
    }
    for (int u : p.g.order)
      if (seen.count(u) && (size_t)u < p.b.targets.size() && p.b.targets[u].kind == BuildTarget::Library) p.libsOf[i].push_back(p.g.nodes[u].name);
    // external packages needed: the direct `dependency` nodes plus everything they require
    std::set<std::string> need;
    std::vector<std::string> st2;
    for (int u : seen) if (!p.g.nodes[u].isTarget) st2.push_back(p.g.nodes[u].name);
    while (!st2.empty()) {
      std::string n = st2.back(); st2.pop_back();
      if (!need.insert(n).second) continue;
      for (auto& r : p.resolved) if (r.name == n) for (auto& dn : r.deps) st2.push_back(dn);
    }
    for (size_t k = 0; k < p.resolved.size(); k++) if (need.count(p.resolved[k].name)) p.pkgsOf[i].push_back(k);
  }
  return true;
}

enum class St { Pending, Running, Ok, Failed, Skipped };
struct Job {
  St st = St::Pending;
  pid_t pid = 0;
  std::string log;
  std::vector<std::string> argv;
  bool wanted = false;
  std::string key, dep;
};



// ---- cache (W9). Entry: <cache>/<key>/{artifact, deps.txt, meta}. The key covers everything that decides the output
// except the contents of #included files; those are checked on lookup ("direct mode") from the depfile of the build
// that created the entry, so changing a header invalidates exactly the entries that read it.
constexpr const char* kCacheVersion = "c7cache-1";

struct CacheCtx {
  bool enabled = false;
  fs::path dir;
  std::string fingerprint;  // hash of c7c itself, the runtime, and the standard library actually in use
};

std::string fileHash(const fs::path& p) {
  std::string h;
  return sha256File(p.string(), h) ? h : std::string("-");
}

std::string compilerFingerprint(const std::string& self) {
  fs::path exeDir = fs::path(self).parent_path();
  std::string s = std::string(kCompilerName) + " " + kCompilerVersion + " " + kAbiId + "\n";
  s += "c7c " + fileHash(self) + "\n";
  s += "runtime " + fileHash(exeDir / "runtime" / "c7rt.c") + "\n";
  const char* env = std::getenv("C7_STDLIB");  // same choice as the loader: C7_STDLIB > std.l7 > stdlib/
  std::error_code ec;
  if (env) {
    std::vector<std::string> names;
    for (auto& e : fs::directory_iterator(env, ec)) if (e.path().extension() == ".c7") names.push_back(e.path().filename().string());
    std::sort(names.begin(), names.end());
    for (auto& n : names) s += "std-src " + n + " " + fileHash(fs::path(env) / n) + "\n";
  } else if (fs::exists(exeDir / "std.l7", ec)) {
    s += "std.l7 " + fileHash(exeDir / "std.l7") + "\n";
  } else {
    std::vector<std::string> names;
    for (auto& e : fs::directory_iterator(exeDir / "stdlib", ec)) if (e.path().extension() == ".c7") names.push_back(e.path().filename().string());
    std::sort(names.begin(), names.end());
    for (auto& n : names) s += "std-src " + n + " " + fileHash(exeDir / "stdlib" / n) + "\n";
  }
  return sha256Hex(s);
}

std::string cacheKey(const Project& p, size_t ti, const std::vector<std::string>& argv, const CacheCtx& cc) {
  const BuildTarget& t = p.b.targets[ti];
  std::string in;
  auto field = [&](const std::string& k, const std::string& v) { in += k + ":" + std::to_string(v.size()) + ":" + v + "\n"; };
  field("cache", kCacheVersion);
  field("compiler", cc.fingerprint);
  field("kind", t.kind == BuildTarget::Executable ? "executable" : "library");
  field("name", t.name);
  field("platform", p.g.nodes[ti].platform);
  field("host", p.g.host);
  std::string opts;  // the normalized option string: the command line without the compiler, the output path and the sources
  std::set<std::string> srcs(t.sources.begin(), t.sources.end());
  for (size_t k = 1; k < argv.size(); k++) {
    if (argv[k] == "-o") { k++; continue; }
    if (argv[k] == "--lib") { k++; continue; }
    if (srcs.count(argv[k])) continue;
    opts += argv[k] + "\x1f";
  }
  field("options", opts);
  std::vector<std::pair<std::string, std::string>> src;
  for (auto& s : srcs) src.push_back({s, fileHash(fs::path(p.dir) / s)});
  for (auto& kv : src) field("source", kv.first + " " + kv.second);
  std::vector<std::string> libs;
  for (auto& l : p.libsOf[ti])
    for (size_t k = 0; k < p.b.targets.size(); k++)
      if (p.b.targets[k].name == l) libs.push_back(l + " " + (p.b.version.empty() ? "0.0.0" : p.b.version) + " " + fileHash(fs::path(p.dir) / p.g.nodes[k].output));
  for (size_t k : p.pkgsOf[ti]) libs.push_back(p.resolved[k].name + " " + p.resolved[k].version + " " + p.resolved[k].sha256 + " " + p.resolved[k].sourceId);
  std::sort(libs.begin(), libs.end());
  for (auto& l : libs) field("lib", l);
  return sha256Hex(in);
}

bool cacheLookup(const CacheCtx& cc, const std::string& key, const Project& p, size_t ti) {
  std::error_code ec;
  fs::path e = cc.dir / key;
  if (!fs::is_regular_file(e / "artifact", ec)) return false;
  if (!fs::is_regular_file(e / "deps.txt", ec)) return false;  // an entry without its dependency list cannot be trusted
  std::string meta = slurp((e / "meta").string());
  if (meta.rfind("c7cache-entry 1\n", 0) != 0) return false;
  auto pos = meta.find("artifact_sha256=");
  if (pos == std::string::npos || meta.substr(pos + 16, 64) != fileHash(e / "artifact")) return false;  // corrupted: ignore, rebuild
  std::istringstream deps(slurp((e / "deps.txt").string()));
  std::string line;
  while (std::getline(deps, line)) {
    if (line.size() < 67 || line.substr(64, 2) != "  ") return false;
    if (fileHash(fs::path(p.dir) / line.substr(66)) != line.substr(0, 64)) return false;  // an #included file changed
  }
  fs::path out = fs::path(p.dir) / p.g.nodes[ti].output;
  fs::create_directories(out.parent_path(), ec);
  std::string tmp = out.string() + ".tmp" + std::to_string(getpid());
  fs::copy_file(e / "artifact", tmp, fs::copy_options::overwrite_existing, ec);
  if (ec) return false;
  fs::permissions(tmp, fs::status(e / "artifact", ec).permissions(), ec);
  fs::rename(tmp, out, ec);
  if (ec) { fs::remove(tmp, ec); return false; }
  return true;
}

void cacheStore(const CacheCtx& cc, const std::string& key, const Project& p, size_t ti, const std::string& depfile) {
  std::error_code ec;
  fs::path out = fs::path(p.dir) / p.g.nodes[ti].output;
  fs::path tmp = cc.dir / (".tmp-" + std::to_string(getpid()) + "-" + p.b.targets[ti].name);
  fs::remove_all(tmp, ec);
  fs::create_directories(tmp, ec);
  if (ec) return;
  fs::copy_file(out, tmp / "artifact", fs::copy_options::overwrite_existing, ec);
  if (ec) { fs::remove_all(tmp, ec); return; }
  fs::permissions(tmp / "artifact", fs::status(out, ec).permissions(), ec);
  std::string deps;
  std::istringstream df(slurp(depfile));
  std::string line;
  while (std::getline(df, line)) if (!line.empty()) deps += fileHash(fs::path(p.dir) / line) + "  " + line + "\n";
  std::ofstream(tmp / "deps.txt", std::ios::binary) << deps;
  std::ofstream(tmp / "meta", std::ios::binary) << "c7cache-entry 1\nartifact_sha256=" << fileHash(tmp / "artifact") << "\n";
  fs::path dst = cc.dir / key;
  fs::rename(tmp, dst, ec);                       // atomic publish
  if (ec) {                                       // an entry exists (stale, damaged, or written by a parallel build): replace it
    fs::remove_all(dst, ec);
    fs::rename(tmp, dst, ec);
    if (ec) fs::remove_all(tmp, ec);              // lost a race: the other entry is as good as ours
  }
}

bool writePackageFor(DiagEngine& d, const Project& p, size_t ti, std::string& error) {
  const BuildTarget& t = p.b.targets[ti];
  Manifest m;
  m.name = t.name;
  m.version = p.b.version.empty() ? "0.0.0" : p.b.version;
  m.license = p.b.license;
  m.description = p.b.description;
  for (const std::string& dn : t.depends) {
    for (auto& ed : p.b.deps)
      if (ed.name == dn) {
        std::string req = ed.req;
        if (ed.isPath) for (auto& r : p.resolved) if (r.name == dn) req = "=" + r.version;
        m.deps.push_back({dn, req});
      }
  }
  for (const std::string& dn : t.depends)
    for (auto& o : p.b.targets)
      if (o.name == dn && o.kind == BuildTarget::Library) m.deps.push_back({dn, "=" + (p.b.version.empty() ? std::string("0.0.0") : p.b.version)});
  std::vector<ZipEntry> content;
  for (const std::string& s : t.sources) {
    std::ifstream f(fs::path(p.dir) / s, std::ios::binary);
    if (!f) { error = "cannot read " + s; return false; }
    std::stringstream ss; ss << f.rdbuf();
    std::string txt = ss.str();
    std::string arc = s.rfind("src/", 0) == 0 ? s : "src/" + s;
    content.push_back({arc, std::vector<uint8_t>(txt.begin(), txt.end())});
  }
  std::vector<uint8_t> bytes;
  DiagEngine pd;
  if (!buildPackage(pd, m, content, bytes)) {
    std::ostringstream os; pd.print(os, false); error = os.str();
    return false;
  }
  fs::path out = fs::path(p.dir) / p.g.nodes[ti].output;
  std::error_code ec;
  fs::create_directories(out.parent_path(), ec);
  std::string tmp = out.string() + ".tmp" + std::to_string(getpid());
  if (!writeFileBytes(tmp, bytes)) { error = "cannot write " + out.string(); return false; }
  fs::rename(tmp, out, ec);  // atomic: a failed build never leaves a half-written package
  if (ec) { fs::remove(tmp, ec); error = "cannot write " + out.string(); return false; }
  return true;
}

std::vector<std::string> commandFor(const Project& p, size_t ti, const std::string& self) {
  const BuildTarget& t = p.b.targets[ti];
  const Options& o = p.opts[ti];
  std::vector<std::string> a{self, t.kind == BuildTarget::Executable ? "build" : "check"};
  for (auto& s : t.sources) a.push_back(s);
  for (size_t k : p.pkgsOf[ti]) { a.push_back("--lib"); a.push_back(p.resolved[k].file); }
  for (auto& l : p.libsOf[ti]) {
    a.push_back("--lib");
    for (size_t k = 0; k < p.b.targets.size(); k++) if (p.b.targets[k].name == l) a.push_back(p.g.nodes[k].output);
  }
  for (auto& f : o.toFlags()) a.push_back(f);
  for (auto& dd : t.defines) a.push_back("-D" + dd);
  if (t.std && *t.std == "none") a.push_back("--no-std");
  if (t.kind == BuildTarget::Executable) {
    a.push_back("-o");
    a.push_back(p.g.nodes[ti].output);
  }
  return a;
}

bool spawnJob(const Project& p, Job& j, const std::string& logPath) {
  j.log = logPath;
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 1, logPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  posix_spawn_file_actions_adddup2(&fa, 1, 2);
  posix_spawn_file_actions_addchdir_np(&fa, p.dir.c_str());
  std::vector<char*> av;
  for (auto& s : j.argv) av.push_back(const_cast<char*>(s.c_str()));
  av.push_back(nullptr);
  int rc = posix_spawn(&j.pid, j.argv[0].c_str(), &fa, nullptr, av.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  return rc == 0;
}


// W13: `<output>.mp7` next to every executable that built (or came from the cache). Failure to write it is a warning
// on stderr, never a build failure, but a map is never left stale: the old one is deleted before the build starts.
std::string mapPathFor(const Project& p, size_t ti) { return (fs::path(p.dir) / (p.g.nodes[ti].output + ".mp7")).string(); }

void writeMapFor(const Project& p, size_t ti) {
  const BuildTarget& t = p.b.targets[ti];
  const Options& o = p.opts[ti];
  MapFile m;
  m.project = p.b.projectName;
  m.target = t.name;
  m.platform = p.g.nodes[ti].platform;
  m.host = p.g.host;
  m.config = p.config;
  m.mode = o.mode;
  m.optimization = o.optimization;
  m.compiler = std::string("c7c ") + kCompilerVersion;
  for (auto& s : t.sources) {
    MapFile::Input in;
    in.path = s;
    std::string h;
    in.sha256 = sha256File((fs::path(p.dir) / s).string(), h) ? h : "-";
    std::error_code ec;
    in.bytes = fs::file_size(fs::path(p.dir) / s, ec);
    m.inputs.push_back(in);
  }
  std::vector<MapFile::Lib> libs;
  for (auto& l : p.libsOf[ti])
    for (size_t k = 0; k < p.b.targets.size(); k++)
      if (p.b.targets[k].name == l) {
        std::string h;
        sha256File((fs::path(p.dir) / p.g.nodes[k].output).string(), h);
        libs.push_back({l, p.b.version.empty() ? "0.0.0" : p.b.version, h});
      }
  for (size_t k : p.pkgsOf[ti]) libs.push_back({p.resolved[k].name, p.resolved[k].version, p.resolved[k].sha256});
  std::sort(libs.begin(), libs.end(), [](const MapFile::Lib& a, const MapFile::Lib& b) { return std::tie(a.name, a.version) < std::tie(b.name, b.version); });
  m.libs = libs;
  m.outputPath = p.g.nodes[ti].output;
  m.outputFile = (fs::path(p.dir) / p.g.nodes[ti].output).string();
  if (!writeMapFile(mapPathFor(p, ti), buildMapText(m))) std::cerr << "c7c: warning: cannot write " << mapPathFor(p, ti) << "\n";
}

int cmdBuild(const Cli& c, bool librariesOnly) {
  DiagEngine d;
  Project p;
  if (!loadProject(d, c, p)) { d.print(std::cerr, false); return 1; }
  size_t N = p.b.targets.size();
  std::vector<Job> jobs(N);
  // selection: named targets plus what they need; default all (pack: libraries)
  std::vector<std::string> want = c.targets;
  for (auto& n : want) {
    bool f = false;
    for (auto& t : p.b.targets) if (t.name == n) f = true;
    if (!f) { std::cerr << "c7c: no such target: " << n << "\n"; return 2; }
  }
  for (size_t i = 0; i < N; i++) {
    bool w = want.empty() ? (!librariesOnly || p.b.targets[i].kind == BuildTarget::Library) : std::count(want.begin(), want.end(), p.b.targets[i].name) > 0;
    jobs[i].wanted = w;
  }
  for (size_t k = 0; k < N; k++)  // pull in dependencies (closure; targets are few, so repeat until stable)
    for (size_t i = 0; i < N; i++) if (jobs[i].wanted) for (int v : p.g.nodes[i].deps) if ((size_t)v < N) jobs[v].wanted = true;  // nodes >= N are external packages (already resolved)
  int maxJobs = c.jobs > 0 ? c.jobs : (int)std::max(1u, std::thread::hardware_concurrency());
  std::string self = selfPath();
  char tmpl[] = "/tmp/c7build.XXXXXX";
  if (!mkdtemp(tmpl)) { std::cerr << "c7c: cannot create a temporary directory\n"; return 1; }
  std::string tmpDir = tmpl;
  std::error_code ec;
  CacheCtx cc;
  if (!c.noCache) {
    cc.enabled = true;
    cc.dir = c.cacheDir.empty() ? fs::path(p.dir) / ".c7cache" : fs::absolute(c.cacheDir);
    fs::create_directories(cc.dir, ec);
    if (ec) cc.enabled = false;
    else cc.fingerprint = compilerFingerprint(self);
  }
  for (size_t i = 0; i < N; i++) {  // outputs of wanted targets: remove stale ones so a failed build cannot look like a success
    if (!jobs[i].wanted) continue;
    fs::path o = fs::path(p.dir) / p.g.nodes[i].output;
    fs::create_directories(o.parent_path(), ec);
    fs::remove(o, ec);  // a failed rebuild must not leave the previous output looking current
    if (p.b.targets[i].kind == BuildTarget::Executable) fs::remove(mapPathFor(p, i), ec);  // ... nor its map
  }
  std::map<std::string, std::string> failures;  // target -> reason, for E0720
  std::set<int> cachedNames;
  bool failed = false;
  int running = 0;
  size_t remaining = 0;
  for (auto& j : jobs) if (j.wanted) remaining++;
  auto depsOk = [&](size_t i) { for (int v : p.g.nodes[i].deps) if ((size_t)v < N && jobs[v].st != St::Ok) return false; return true; };
  auto event = [&](const char* what, size_t i) { if (c.verbose) std::cerr << "[c7c] " << what << " " << p.g.nodes[i].name << "\n"; };
  while (remaining > 0) {
    // start every ready job (graph order) while slots are free
    bool hit = false;
    if (!failed) {
      for (int u : p.g.order) {
        if (running >= maxJobs) break;
        if ((size_t)u >= N) continue;  // external package node
        size_t i = (size_t)u;
        if (jobs[i].st != St::Pending || !jobs[i].wanted || !depsOk(i)) continue;
        jobs[i].argv = commandFor(p, i, self);
        if (cc.enabled) {
          jobs[i].key = cacheKey(p, i, jobs[i].argv, cc);
          if (cacheLookup(cc, jobs[i].key, p, i)) { jobs[i].st = St::Ok; remaining--; event("cached", i); cachedNames.insert((int)i); hit = true; if (p.b.targets[i].kind == BuildTarget::Executable) writeMapFor(p, i); continue; }
          jobs[i].dep = tmpDir + "/" + p.b.targets[i].name + ".dep";
          jobs[i].argv.push_back("--depfile");
          jobs[i].argv.push_back(jobs[i].dep);
        }
        if (!spawnJob(p, jobs[i], tmpDir + "/" + p.b.targets[i].name + ".log")) { jobs[i].st = St::Failed; failures[p.b.targets[i].name] = "cannot start " + self; failed = true; remaining--; continue; }
        jobs[i].st = St::Running; running++;
        event("start", i);
        if (c.verbose) { std::cerr << "[c7c]   run:"; for (size_t k = 1; k < jobs[i].argv.size(); k++) std::cerr << " " << jobs[i].argv[k]; std::cerr << "\n"; }
      }
    }
    if (running == 0 && hit && remaining > 0) continue;  // cache hits can make further targets ready
    if (running == 0) break;  // nothing running: either everything finished or a failure blocked the rest
    int status = 0;
    pid_t pid = waitpid(-1, &status, 0);
    if (pid < 0) break;
    for (size_t i = 0; i < N; i++) {
      if (jobs[i].st != St::Running || jobs[i].pid != pid) continue;
      running--; remaining--;
      int rc = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
      std::string why;
      if (rc == 0 && p.b.targets[i].kind == BuildTarget::Library) {
        std::string err;
        if (!writePackageFor(d, p, i, err)) { rc = 1; why = err; }
      }
      if (rc == 0) {
        jobs[i].st = St::Ok; event("done", i);
        if (p.b.targets[i].kind == BuildTarget::Executable) writeMapFor(p, i);
        if (cc.enabled) cacheStore(cc, jobs[i].key, p, i, jobs[i].dep);
      }
      else {
        jobs[i].st = St::Failed; failed = true; event("failed", i);
        failures[p.b.targets[i].name] = why.empty() ? "exit status " + std::to_string(rc) : why;
      }
      break;
    }
  }
  for (size_t i = 0; i < N; i++) if (jobs[i].wanted && jobs[i].st == St::Pending) { jobs[i].st = St::Skipped; if (!failed) { failed = true; std::cerr << "c7c: internal error: target `" << p.b.targets[i].name << "` was never ready\n"; } }
  // replay in graph order: deterministic whatever the scheduling was
  for (int u : p.g.order) {
    if ((size_t)u >= N) continue;
    size_t i = (size_t)u;
    if (!jobs[i].wanted) continue;
    const char* kind = p.b.targets[i].kind == BuildTarget::Executable ? "executable" : "library";
    if (jobs[i].st == St::Skipped) { std::cerr << "c7c: skipped " << kind << " " << p.b.targets[i].name << " (not started: an earlier target failed)\n"; continue; }
    std::string log = jobs[i].log.empty() ? "" : slurp(jobs[i].log);
    if (!log.empty()) std::cerr << log;
    if (jobs[i].st == St::Ok && cachedNames.count(u)) std::cout << "c7c: cached " << kind << " " << p.b.targets[i].name << " -> " << p.g.nodes[i].output << "\n";
    else if (jobs[i].st == St::Ok) std::cout << "c7c: built " << kind << " " << p.b.targets[i].name << " -> " << p.g.nodes[i].output << "\n";
  }
  for (int u : p.g.order) {
    if ((size_t)u >= N) continue;
    auto f = failures.find(p.b.targets[u].name);
    if (f != failures.end()) {
      if (f->second.rfind("exit status", 0) != 0) std::cerr << "c7c: " << p.b.targets[u].name << ": " << f->second << "\n";
      d.report("E0720", p.b.targets[u].loc, {p.b.targets[u].name});
    }
  }
  fs::remove_all(tmpDir, ec);
  d.print(std::cerr, false);
  return failed ? 1 : 0;
}

int cmdClean(const Cli& c) {
  DiagEngine d;
  std::error_code ec;
  fs::path in(c.path);
  fs::path dir = fs::is_directory(in, ec) ? in : in.parent_path();
  if (dir.empty()) dir = ".";
  std::string outDir, why;
  if (!normalizeProjectPath(c.outDir, outDir, why) || outDir == ".") { std::cerr << "c7c: refusing to clean `" << c.outDir << "`: " << (why.empty() ? "that is the project directory" : why) << "\n"; return 2; }
  if (!fs::is_regular_file(dir / "Cmake7109.txt", ec) && !fs::is_regular_file(in, ec)) { std::cerr << "c7c: no build file in " << dir.string() << "\n"; return 1; }
  size_t n = 0;
  for (const std::string& sub : {outDir, std::string(".c7cache")}) {
    fs::path q = dir / sub;
    if (fs::is_symlink(q, ec)) { fs::remove(q, ec); n++; continue; }  // never follow a link out of the project
    if (fs::exists(q, ec)) { fs::remove_all(q, ec); n++; }
  }
  std::cout << "c7c: cleaned " << n << " director" << (n == 1 ? "y" : "ies") << "\n";
  return 0;
}

int cmdLock(const Cli& c) {
  DiagEngine d;
  Project p;
  if (!loadProject(d, c, p, false)) { d.print(std::cerr, false); return 1; }  // resolves afresh: the old lock is ignored
  std::string text = lockToToml(makeLock(p.resolved, p.config));
  std::error_code ec;
  if (slurp(p.lockPath) == text && fs::is_regular_file(p.lockPath, ec)) { std::cout << "c7c: " << fs::path(p.lockPath).filename().string() << " is up to date (" << p.resolved.size() << " package(s))\n"; return 0; }
  std::string tmp = p.lockPath + ".tmp" + std::to_string(getpid());
  { std::ofstream f(tmp, std::ios::binary); f << text; if (!f) { std::cerr << "c7c: cannot write " << p.lockPath << "\n"; return 1; } }
  fs::rename(tmp, p.lockPath, ec);  // atomic
  if (ec) { fs::remove(tmp, ec); std::cerr << "c7c: cannot write " << p.lockPath << "\n"; return 1; }
  std::cout << "c7c: wrote " << fs::path(p.lockPath).filename().string() << " (" << p.resolved.size() << " package(s))\n";
  return 0;
}

int cmdVerify(const Cli& c) {
  DiagEngine d;
  bool ok = true;
  std::vector<std::string> files = c.files;
  if (files.empty()) {
    Project p;
    if (!loadProject(d, c, p)) { d.print(std::cerr, false); return 1; }
    for (size_t i = 0; i < p.b.targets.size(); i++) {
      if (p.b.targets[i].kind != BuildTarget::Library) continue;
      std::string f = (fs::path(p.dir) / p.g.nodes[i].output).string();
      std::error_code ec;
      if (!fs::is_regular_file(f, ec)) { d.report("E0704", p.b.targets[i].loc, {p.g.nodes[i].output}); ok = false; continue; }
      files.push_back(f);
    }
  }
  for (auto& f : files) {
    Package pk;
    if (loadPackage(d, f, pk)) std::cout << "c7c: ok " << pk.m.name << " " << pk.m.version << " (" << pk.sha256.substr(0, 12) << ")\n";
    else ok = false;
  }
  d.print(std::cerr, false);
  return ok ? 0 : 1;
}

// W16: can this package be used on os/arch? Source packages only need the ABI (checked by the loader); a package with
// binaries but no sources needs a [[binary]] for exactly this os, architecture and ABI - never another platform's.
bool checkInstallable(DiagEngine& d, const Package& pk, const std::string& os, const std::string& arch) {
  bool hasSrc = !pk.sources().empty();
  if (hasSrc) return true;
  for (auto& b : pk.m.binaries) if (b.os == os && b.arch == arch && b.abi == kAbiId) return true;
  d.report("E0712", anchorLoc(d, pk.label), {pk.m.name, os + "-" + arch + " (ABI " + kAbiId + ")"});
  return false;
}

int cmdInstall(const Cli& c) {
  DiagEngine d;
  std::vector<std::string> files = c.files;
  if (files.empty()) {
    Project p;
    if (!loadProject(d, c, p)) { d.print(std::cerr, false); return 1; }
    for (size_t i = 0; i < p.b.targets.size(); i++) {
      if (p.b.targets[i].kind != BuildTarget::Library) continue;
      std::string f = (fs::path(p.dir) / p.g.nodes[i].output).string();
      std::error_code ec;
      if (!fs::is_regular_file(f, ec)) { d.report("E0704", p.b.targets[i].loc, {p.g.nodes[i].output}); d.print(std::cerr, false); return 1; }
      files.push_back(f);
    }
    if (files.empty()) { std::cerr << "c7c: nothing to install: the project has no library target\n"; return 1; }
  }
  fs::path prefix = c.prefix;
  if (prefix.empty()) {
    const char* home = std::getenv("HOME");
    if (!home || !*home) { std::cerr << "c7c: cannot choose an install prefix: HOME is not set (use --prefix)\n"; return 1; }
    prefix = fs::path(home) / ".c7" / "packages";
  }
  // validate everything first: either all packages install or none does
  std::vector<Package> pks(files.size());
  bool ok = true;
  for (size_t i = 0; i < files.size(); i++)
    if (!loadPackage(d, files[i], pks[i]) || !checkInstallable(d, pks[i], "linux", "x86_64")) ok = false;
  std::error_code ec;
  std::set<std::string> dests;
  if (ok)
    for (auto& pk : pks) {
      fs::path dst = prefix / (pk.m.name + "-" + pk.m.version);
      if (!dests.insert(dst.string()).second || fs::exists(fs::symlink_status(dst, ec))) {
        d.print(std::cerr, false);
        std::cerr << "c7c: error: " << dst.string() << " already exists; nothing was installed (remove it first)\n";
        return 1;
      }
    }
  if (!ok) { d.print(std::cerr, false); return 1; }
  fs::create_directories(prefix, ec);
  if (ec) { std::cerr << "c7c: cannot create " << prefix.string() << ": " << ec.message() << "\n"; return 1; }
  for (auto& pk : pks) {
    fs::path dst = prefix / (pk.m.name + "-" + pk.m.version);
    fs::path tmp = prefix / (".install-" + std::to_string(getpid()) + "-" + pk.m.name);
    fs::remove_all(tmp, ec);
    if (!extractPackage(d, pk, tmp.string())) { fs::remove_all(tmp, ec); d.print(std::cerr, false); return 1; }
    fs::rename(tmp, dst, ec);  // atomic publish: a half-installed package is never visible
    if (ec) { fs::remove_all(tmp, ec); std::cerr << "c7c: cannot install into " << dst.string() << ": " << ec.message() << "\n"; return 1; }
    std::cout << "c7c: installed " << pk.m.name << " " << pk.m.version << " -> " << dst.string() << "\n";
  }
  d.print(std::cerr, false);
  return 0;
}

}  // namespace

int runProjectCommand(int argc, char** argv) {
  Cli c;
  if (!parseCli(argc, argv, c)) { usageProject(); return 2; }
  if (c.cmd == "build") return cmdBuild(c, false);
  if (c.cmd == "pack") return cmdBuild(c, true);
  if (c.cmd == "clean") return cmdClean(c);
  if (c.cmd == "verify") return cmdVerify(c);
  if (c.cmd == "lock") return cmdLock(c);
  if (c.cmd == "install") return cmdInstall(c);
  return 2;
}

}  // namespace c7pkg
