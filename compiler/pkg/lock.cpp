#include "lock.hpp"

#include <set>

#include "toml.hpp"
#include "version.hpp"

using namespace c7;

namespace c7pkg {

namespace {
bool bad(DiagEngine& d, const std::string& label, Loc l, const std::string& why) {
  d.report("E0714", l, {label, why});
  return false;
}
bool isHex64(const std::string& s) {
  if (s.size() != 64) return false;
  for (char c : s) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}
}  // namespace

bool loadLock(DiagEngine& d, int fileId, LockFile& out) {
  out = LockFile();
  std::string label = d.files[fileId]->path;
  TomlValue root;
  if (!parseToml(d, fileId, root)) return false;
  Loc top{fileId, 1, 1};
  for (auto& kv : root.tbl)
    if (kv.first != "lock" && kv.first != "compiler" && kv.first != "target" && kv.first != "build" && kv.first != "dependency") d.report("W0701", kv.second.loc, {kv.first, "the lock file"});
  const TomlValue* lock = root.get("lock");
  if (!lock || lock->kind != TomlValue::Table) return bad(d, label, top, "missing [lock] table");
  const TomlValue* fv = lock->get("format_version");
  if (!fv || fv->kind != TomlValue::Integer) return bad(d, label, lock->loc, "missing integer lock.format_version");
  if (fv->i != 1) return bad(d, label, fv->loc, "unsupported format_version " + std::to_string(fv->i) + " (this compiler reads 1)");
  auto str = [&](const char* table, const char* key, std::string& dst, bool required) -> bool {
    const TomlValue* t = root.get(table);
    if (!t) return !required || bad(d, label, top, std::string("missing [") + table + "]");
    if (t->kind != TomlValue::Table) return bad(d, label, t->loc, std::string(table) + " must be a table");
    const TomlValue* v = t->get(key);
    if (!v) return !required || bad(d, label, t->loc, std::string("missing ") + table + "." + key);
    if (v->kind != TomlValue::String) return bad(d, label, v->loc, std::string(table) + "." + key + " must be a string");
    dst = v->str;
    return true;
  };
  if (!str("compiler", "name", out.compilerName, true) || !str("compiler", "version", out.compilerVersion, true) || !str("compiler", "abi", out.abi, true) ||
      !str("target", "os", out.os, true) || !str("target", "architecture", out.arch, true) || !str("build", "config", out.config, false)) return false;
  const TomlValue* deps = root.get("dependency");
  if (deps) {
    if (deps->kind != TomlValue::Array) return bad(d, label, deps->loc, "dependency must be an array of tables ([[dependency]])");
    std::set<std::string> seen;
    for (auto& e : deps->arr) {
      if (e.kind != TomlValue::Table) return bad(d, label, e.loc, "dependency entries must be tables");
      LockDep ld;
      for (auto& kv : e.tbl)
        if (kv.first != "name" && kv.first != "version" && kv.first != "source" && kv.first != "sha256") d.report("W0701", kv.second.loc, {kv.first, "[[dependency]]"});
      for (auto [key, dst] : {std::pair<const char*, std::string*>{"name", &ld.name}, {"version", &ld.version}, {"source", &ld.source}, {"sha256", &ld.sha256}}) {
        const TomlValue* v = e.get(key);
        if (!v || v->kind != TomlValue::String || v->str.empty()) return bad(d, label, e.loc, std::string("[[dependency]] needs a non-empty string `") + key + "`");
        *dst = v->str;
      }
      if (!isHex64(ld.sha256)) return bad(d, label, e.get("sha256")->loc, "sha256 must be 64 lowercase hex digits");
      if (!seen.insert(ld.name).second) return bad(d, label, e.loc, "package `" + ld.name + "` is listed twice");
      out.deps.push_back(ld);
    }
  }
  return true;
}

LockFile makeLock(const std::vector<Resolved>& r, const std::string& config) {
  LockFile l;
  l.compilerName = kCompilerName; l.compilerVersion = kCompilerVersion; l.abi = kAbiId;
  l.os = "linux"; l.arch = "x86_64"; l.config = config;
  for (auto& p : r) l.deps.push_back({p.name, p.version, p.sourceId, p.sha256});
  return l;
}

std::string lockToToml(const LockFile& l) {
  TomlValue root = TomlValue::table_();
  TomlValue lock = TomlValue::table_();
  lock.set("format_version", TomlValue::int_(1));
  root.set("lock", lock);
  TomlValue c = TomlValue::table_();
  c.set("name", TomlValue::str_(l.compilerName)); c.set("version", TomlValue::str_(l.compilerVersion)); c.set("abi", TomlValue::str_(l.abi));
  root.set("compiler", c);
  TomlValue t = TomlValue::table_();
  t.set("os", TomlValue::str_(l.os)); t.set("architecture", TomlValue::str_(l.arch));
  root.set("target", t);
  if (!l.config.empty()) {
    TomlValue b = TomlValue::table_();
    b.set("config", TomlValue::str_(l.config));
    root.set("build", b);
  }
  if (!l.deps.empty()) {
    TomlValue arr = TomlValue::array_();
    for (auto& dp : l.deps) {
      TomlValue e = TomlValue::table_();
      e.set("name", TomlValue::str_(dp.name)); e.set("version", TomlValue::str_(dp.version));
      e.set("source", TomlValue::str_(dp.source)); e.set("sha256", TomlValue::str_(dp.sha256));
      arr.arr.push_back(e);
    }
    root.set("dependency", arr);
  }
  return "# Cmake7109.cr7 - written by `c7c lock`; builds only read it.\n" + tomlSerialize(root);
}

}  // namespace c7pkg
