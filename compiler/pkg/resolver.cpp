#include "resolver.hpp"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <map>
#include <set>

using namespace c7;
namespace fs = std::filesystem;

namespace c7pkg {

namespace {

class LocalRegistrySource : public PackageSource {
 public:
  LocalRegistrySource(std::string written, std::string dir) : written_(std::move(written)), dir_(std::move(dir)) {}
  std::string id() const override { return "registry:" + written_; }
  bool candidates(DiagEngine& d, const std::string& name, std::vector<Candidate>& out) override {
    std::error_code ec;
    if (!fs::is_directory(dir_, ec)) return true;  // a missing registry simply has nothing
    std::vector<std::string> files;
    for (auto& e : fs::directory_iterator(dir_, ec)) {
      std::string f = e.path().filename().string();
      if (f.size() <= 3 || f.compare(f.size() - 3, 3, ".l7") != 0 || f.rfind(name + "-", 0) != 0) continue;
      // only `<name>-<semver>.l7` can be a version of `name`; `foo-bar-1.0.0.l7` belongs to `foo-bar` and is not even opened
      Version probe; std::string err;
      if (parseVersion(f.substr(name.size() + 1, f.size() - 3 - name.size() - 1), probe, err)) files.push_back(f);
    }
    std::sort(files.begin(), files.end());
    for (auto& f : files) {
      Candidate c;
      std::string path = (fs::path(dir_) / f).string();
      if (!loadPackage(d, path, c.pkg)) return false;           // damaged or tampered: never skipped silently
      if (c.pkg.m.name != name) continue;                        // `foo-bar-1.0.0.l7` while looking for `foo`: someone else's file
      if (f != c.pkg.m.name + "-" + c.pkg.m.version + ".l7") {   // the file name is part of the registry contract
        d.report("E0709", anchorLoc(d, path), {path, "file name does not match its manifest (`" + c.pkg.m.name + "-" + c.pkg.m.version + ".l7` expected)"});
        return false;
      }
      std::string err;
      if (!parseVersion(c.pkg.m.version, c.version, err)) { d.report("E0717", anchorLoc(d, path), {c.pkg.m.version, err}); return false; }
      c.file = path;
      c.sourceId = id();
      out.push_back(std::move(c));
    }
    return true;
  }

 private:
  std::string written_, dir_;
};

class StubSource : public PackageSource {
 public:
  explicit StubSource(std::string spec) : spec_(std::move(spec)) {}
  std::string id() const override { return spec_; }
  bool candidates(DiagEngine& d, const std::string& name, std::vector<Candidate>&) override {
    d.report("E0716", anchorLoc(d, spec_), {name, "the registry type of `" + spec_ + "` is not implemented"});
    return false;
  }

 private:
  std::string spec_;
};

struct Con { std::string by; Requirement req; };

std::string describe(std::vector<Con> cs) {  // by value: sorted for a deterministic message (the project first, then by requirer)
  std::stable_sort(cs.begin(), cs.end(), [](const Con& a, const Con& b) {
    bool pa = a.by == "the project", pb = b.by == "the project";
    return pa != pb ? pa : a.by < b.by;
  });
  std::string s;
  for (auto& c : cs) s += (s.empty() ? "" : "; ") + c.by + " requires " + c.req.text;
  return s;
}

}  // namespace

std::unique_ptr<PackageSource> makeRegistrySource(const std::string& spec, const std::string& baseDir) {
  for (const char* p : {"central:", "git:", "enterprise:"})
    if (spec.rfind(p, 0) == 0) return std::make_unique<StubSource>(spec);
  std::string dir = spec.rfind("registry:", 0) == 0 ? spec.substr(9) : spec;
  std::string written = dir;
  fs::path q(dir);
  if (q.is_relative()) q = fs::path(baseDir) / q;
  return std::make_unique<LocalRegistrySource>(written, q.lexically_normal().string());
}

bool resolveDependencies(DiagEngine& d, const std::vector<RootDep>& roots, const std::vector<std::string>& registries,
                         const std::string& baseDir, std::vector<Resolved>& out, const LockPins* pins) {
  out.clear();
  std::vector<std::unique_ptr<PackageSource>> sources;
  for (auto& r : registries) sources.push_back(makeRegistrySource(r, baseDir));
  std::map<std::string, Candidate> pinned;                       // path dependencies
  std::map<std::string, Requirement> rootReq;
  std::map<std::string, Loc> rootLoc;
  for (const RootDep& r : roots) {
    rootLoc[r.name] = r.loc;
    if (r.isPath) {
      Candidate c;
      if (!loadPackage(d, r.file, c.pkg)) return false;
      if (c.pkg.m.name != r.name) { d.report("E0709", anchorLoc(d, r.file), {r.file, "it is package `" + c.pkg.m.name + "`, but the dependency is named `" + r.name + "`"}); return false; }
      std::string err;
      if (!parseVersion(c.pkg.m.version, c.version, err)) { d.report("E0717", anchorLoc(d, r.file), {c.pkg.m.version, err}); return false; }
      c.file = r.file; c.sourceId = "path:" + r.pathText;
      pinned[r.name] = std::move(c);
    } else {
      Requirement q; std::string err;
      if (!parseRequirement(r.req, q, err)) { d.report("E0717", r.loc, {r.req, err}); return false; }
      rootReq[r.name] = q;
    }
  }
  std::map<std::string, std::vector<Candidate>> cache;  // candidates per name, loaded once
  std::map<std::string, Candidate> selected;
  auto lookup = [&](const std::string& name) -> std::vector<Candidate>* {
    auto f = cache.find(name);
    if (f != cache.end()) return &f->second;
    std::vector<Candidate> all;
    for (auto& s : sources) if (!s->candidates(d, name, all)) return nullptr;
    // the same name and version in several registries: the first registry wins
    std::vector<Candidate> uniq;
    for (auto& c : all) {
      bool dup = false;
      for (auto& u : uniq) if (compareVersions(u.version, c.version) == 0 && u.version.build == c.version.build) dup = true;
      if (!dup) uniq.push_back(std::move(c));
    }
    return &(cache[name] = std::move(uniq));
  };
  for (int iter = 0; iter < 200; iter++) {
    // reachable set and constraints from the roots and the packages selected so far
    std::map<std::string, std::vector<Con>> cons;
    std::set<std::string> reach;
    std::vector<std::string> work;
    for (auto& r : roots) {
      if (reach.insert(r.name).second) work.push_back(r.name);
      if (!r.isPath) cons[r.name].push_back({"the project", rootReq[r.name]});
      else cons[r.name];  // present, unconstrained
    }
    while (!work.empty()) {
      std::string n = work.back(); work.pop_back();
      auto s = selected.find(n);
      if (s == selected.end()) continue;
      for (auto& dep : s->second.pkg.m.deps) {
        Requirement q; std::string err;
        if (!parseRequirement(dep.req, q, err)) { d.report("E0717", anchorLoc(d, s->second.file), {dep.req, err}); return false; }
        cons[dep.name].push_back({n + " " + s->second.pkg.m.version, q});
        if (reach.insert(dep.name).second) work.push_back(dep.name);
      }
    }
    bool changed = false;
    for (auto it = selected.begin(); it != selected.end();) {
      if (!reach.count(it->first)) { it = selected.erase(it); changed = true; } else ++it;
    }
    for (const std::string& name : reach) {  // std::set: alphabetical, so the result does not depend on discovery order
      const std::vector<Con>& cs = cons[name];
      const Candidate* pick = nullptr;
      std::vector<Candidate>* all;
      std::vector<Candidate> one;
      auto p = pinned.find(name);
      if (p != pinned.end()) { one.push_back(p->second); all = &one; }
      else { all = lookup(name); if (!all) return false; }
      if (all->empty()) {
        Loc l = rootLoc.count(name) ? rootLoc[name] : anchorLoc(d, name);
        std::string where;
        for (auto& r : registries) where += (where.empty() ? "" : ", ") + r;
        d.report("E0716", l, {name, "no package of that name in " + (where.empty() ? std::string("any registry (none configured)") : where) + " (" + describe(cs) + ")"});
        return false;
      }
      const std::string* pin = nullptr;
      if (pins) { auto pv = pins->versions.find(name); if (pv != pins->versions.end()) pin = &pv->second; }
      if (pin) {
        bool present = false;
        for (auto& c : *all) if (c.version.str() == *pin) present = true;
        if (!present) {
          d.report("E0714", anchorLoc(d, pins->label), {pins->label, "locked `" + name + " " + *pin + "` is no longer available (run `c7c lock` to update the lock)"});
          return false;
        }
      }
      for (auto& c : *all) {
        if (pin && c.version.str() != *pin) continue;
        bool good = true;
        for (auto& con : cs) if (!matches(con.req, c.version)) good = false;
        if (good && (!pick || compareVersions(c.version, pick->version) > 0)) pick = &c;
      }
      if (!pick && pin) {
        d.report("E0714", anchorLoc(d, pins->label), {pins->label, "locked `" + name + " " + *pin + "` does not satisfy: " + describe(cs) + " (run `c7c lock` to update the lock)"});
        return false;
      }
      if (!pick) {
        Loc l = rootLoc.count(name) ? rootLoc[name] : anchorLoc(d, name);
        std::string have;
        for (auto& c : *all) have += (have.empty() ? "" : ", ") + c.version.str();
        d.report("E0713", l, {name, describe(cs) + " (available: " + have + ")"});
        return false;
      }
      auto cur = selected.find(name);
      if (cur == selected.end() || cur->second.file != pick->file) { selected[name] = *pick; changed = true; }
    }
    if (!changed) break;
    if (iter == 199) { d.report("E0713", Loc{}, {"(all)", "resolution did not settle (oscillating requirements)"}); return false; }
  }
  // cycles among the selected packages
  std::map<std::string, int> color;
  std::vector<std::string> stack;
  std::string cycle;
  std::function<void(const std::string&)> dfs = [&](const std::string& n) {
    if (!cycle.empty()) return;
    color[n] = 1; stack.push_back(n);
    for (auto& dep : selected[n].pkg.m.deps) {
      if (!cycle.empty()) break;
      if (color[dep.name] == 1) {
        size_t k = 0;
        while (stack[k] != dep.name) k++;
        for (; k < stack.size(); k++) cycle += stack[k] + " -> ";
        cycle += dep.name;
      } else if (!color[dep.name]) dfs(dep.name);
    }
    stack.pop_back(); color[n] = 2;
  };
  for (auto& kv : selected) if (!color[kv.first]) dfs(kv.first);
  if (!cycle.empty()) { d.report("E0706", Loc{}, {cycle}); return false; }
  // dependencies first, alphabetical among the ready
  std::set<std::string> done;
  while (done.size() < selected.size()) {
    for (auto& kv : selected) {
      if (done.count(kv.first)) continue;
      bool ready = true;
      for (auto& dep : kv.second.pkg.m.deps) if (!done.count(dep.name)) ready = false;
      if (!ready) continue;
      Resolved r;
      r.name = kv.first; r.version = kv.second.pkg.m.version; r.sourceId = kv.second.sourceId;
      r.file = kv.second.file; r.sha256 = kv.second.pkg.sha256;
      for (auto& dep : kv.second.pkg.m.deps) r.deps.push_back(dep.name);
      out.push_back(r);
      done.insert(kv.first);
      break;  // restart the scan: keeps the order alphabetical among everything that is ready
    }
  }
  return true;
}

std::string resolvedToJson(const std::vector<Resolved>& r) {
  auto q = [](const std::string& s) { std::string o = "\""; for (char c : s) { if (c == '"' || c == '\\') o += '\\'; o += c; } return o + "\""; };
  std::string o = "[";
  for (size_t i = 0; i < r.size(); i++) {
    o += (i ? "," : "") + std::string("{\"name\":") + q(r[i].name) + ",\"version\":" + q(r[i].version) + ",\"source\":" + q(r[i].sourceId) +
         ",\"file\":" + q(fs::path(r[i].file).filename().string()) + ",\"sha256\":" + q(r[i].sha256) + ",\"deps\":[";
    for (size_t k = 0; k < r[i].deps.size(); k++) o += (k ? "," : "") + q(r[i].deps[k]);
    o += "]}";
  }
  return o + "]";
}

}  // namespace c7pkg
