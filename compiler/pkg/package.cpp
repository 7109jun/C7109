#include "package.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "semver.hpp"
#include "sha256.hpp"
#include "toml.hpp"

using namespace c7;
namespace fs = std::filesystem;

namespace c7pkg {


bool readFileBytes(const std::string& path, std::vector<uint8_t>& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return true;
}
bool writeFileBytes(const std::string& path, const std::vector<uint8_t>& data) {
  std::ofstream o(path, std::ios::binary);
  if (!o) return false;
  o.write((const char*)data.data(), (std::streamsize)data.size());
  return (bool)o;
}
Loc anchorLoc(DiagEngine& d, const std::string& label) { return Loc{d.addFile(label, ""), 1, 1}; }

std::vector<std::pair<std::string, std::string>> Package::sources() const {
  std::vector<std::pair<std::string, std::string>> out;
  for (auto& e : files)
    if (e.name.rfind("src/", 0) == 0 && e.name.size() > 3 && e.name.substr(e.name.size() - 3) == ".c7")
      out.emplace_back(e.name, std::string(e.data.begin(), e.data.end()));
  std::sort(out.begin(), out.end());
  return out;
}
const ZipEntry* Package::find(const std::string& path) const {
  for (auto& e : files) if (e.name == path) return &e;
  return nullptr;
}

std::string manifestToToml(const Manifest& m) {
  TomlValue root;
  root.set("format_version", TomlValue::int_(m.formatVersion));
  TomlValue pkg;
  pkg.set("name", TomlValue::str_(m.name));
  pkg.set("version", TomlValue::str_(m.version));
  if (!m.license.empty()) pkg.set("license", TomlValue::str_(m.license));
  if (!m.description.empty()) pkg.set("description", TomlValue::str_(m.description));
  pkg.set("abi", TomlValue::str_(m.abi));
  root.set("package", pkg);
  if (!m.compilerReq.empty()) {
    TomlValue c;
    c.set("requires", TomlValue::str_(m.compilerReq));
    root.set("compiler", c);
  }
  if (!m.deps.empty()) {
    TomlValue arr = TomlValue::array_();
    for (auto& d : m.deps) { TomlValue t; t.set("name", TomlValue::str_(d.name)); t.set("version", TomlValue::str_(d.req)); arr.arr.push_back(t); }
    root.set("dependency", arr);
  }
  if (!m.binaries.empty()) {
    TomlValue arr = TomlValue::array_();
    for (auto& b : m.binaries) {
      TomlValue t;
      t.set("os", TomlValue::str_(b.os)); t.set("architecture", TomlValue::str_(b.arch));
      t.set("abi", TomlValue::str_(b.abi)); t.set("path", TomlValue::str_(b.path));
      arr.arr.push_back(t);
    }
    root.set("binary", arr);
  }
  return tomlSerialize(root);
}

namespace {
bool validName(const std::string& n) {
  if (n.empty() || n.size() > 64 || !isalpha((unsigned char)n[0])) return false;
  for (char c : n) if (!isalnum((unsigned char)c) && c != '_' && c != '-') return false;
  return true;
}

bool needString(DiagEngine& d, const std::string& label, const TomlValue& t, const char* key, std::string& out, bool required, const std::string& where) {
  const TomlValue* v = t.get(key);
  if (!v) {
    if (required) d.report("E0709", anchorLoc(d, label), {label, "missing `" + where + key + "`"});
    return !required;
  }
  if (v->kind != TomlValue::String) { d.report("E0709", v->loc, {label, "`" + where + key + "` must be a string"}); return false; }
  out = v->str;
  return true;
}

void warnUnknown(DiagEngine& d, const TomlValue& t, const std::set<std::string>& known, const std::string& where) {
  for (auto& kv : t.tbl) if (!known.count(kv.first)) d.report("W0701", kv.second.loc, {kv.first, where});
}

bool parseManifestText(DiagEngine& d, const std::string& label, const std::string& text, Manifest& m) {
  int fid = d.addFile(label + "!manifest.toml", text);
  TomlValue root;
  if (!parseToml(d, fid, root)) return false;
  bool ok = true;
  warnUnknown(d, root, {"format_version", "package", "compiler", "dependency", "binary"}, "manifest.toml");
  const TomlValue* fv = root.get("format_version");
  if (!fv || fv->kind != TomlValue::Integer) { d.report("E0709", Loc{fid, 1, 1}, {label, "missing integer `format_version`"}); return false; }
  if (fv->i != 1) { d.report("E0709", fv->loc, {label, "unsupported format_version " + std::to_string(fv->i) + " (this compiler reads 1)"}); return false; }
  const TomlValue* pkg = root.get("package");
  if (!pkg || pkg->kind != TomlValue::Table) { d.report("E0709", Loc{fid, 1, 1}, {label, "missing `[package]` table"}); return false; }
  warnUnknown(d, *pkg, {"name", "version", "license", "description", "abi"}, "[package]");
  ok &= needString(d, label, *pkg, "name", m.name, true, "package.");
  ok &= needString(d, label, *pkg, "version", m.version, true, "package.");
  ok &= needString(d, label, *pkg, "license", m.license, false, "package.");
  ok &= needString(d, label, *pkg, "description", m.description, false, "package.");
  ok &= needString(d, label, *pkg, "abi", m.abi, true, "package.");
  if (!ok) return false;
  if (!validName(m.name)) { d.report("E0709", pkg->get("name")->loc, {label, "invalid package name `" + m.name + "` (letters, digits, `_`, `-`; must start with a letter)"}); return false; }
  Version v;
  std::string err;
  if (!parseVersion(m.version, v, err)) { d.report("E0717", pkg->get("version")->loc, {m.version, err}); return false; }
  if (const TomlValue* c = root.get("compiler")) {
    if (c->kind != TomlValue::Table) { d.report("E0709", c->loc, {label, "`[compiler]` must be a table"}); return false; }
    warnUnknown(d, *c, {"requires"}, "[compiler]");
    if (!needString(d, label, *c, "requires", m.compilerReq, false, "compiler.")) return false;
    Requirement r;
    if (!m.compilerReq.empty() && !parseRequirement(m.compilerReq, r, err)) { d.report("E0717", c->get("requires")->loc, {m.compilerReq, err}); return false; }
  }
  if (const TomlValue* deps = root.get("dependency")) {
    if (deps->kind != TomlValue::Array) { d.report("E0709", deps->loc, {label, "`dependency` must be an array of tables"}); return false; }
    for (auto& t : deps->arr) {
      if (t.kind != TomlValue::Table) { d.report("E0709", t.loc, {label, "`dependency` entries must be tables"}); return false; }
      warnUnknown(d, t, {"name", "version"}, "[[dependency]]");
      DepSpec ds;
      if (!needString(d, label, t, "name", ds.name, true, "dependency.") || !needString(d, label, t, "version", ds.req, true, "dependency.")) return false;
      Requirement r;
      if (!validName(ds.name)) { d.report("E0709", t.loc, {label, "invalid dependency name `" + ds.name + "`"}); return false; }
      if (!parseRequirement(ds.req, r, err)) { d.report("E0717", t.get("version")->loc, {ds.req, err}); return false; }
      m.deps.push_back(ds);
    }
  }
  if (const TomlValue* bins = root.get("binary")) {
    if (bins->kind != TomlValue::Array) { d.report("E0709", bins->loc, {label, "`binary` must be an array of tables"}); return false; }
    for (auto& t : bins->arr) {
      if (t.kind != TomlValue::Table) { d.report("E0709", t.loc, {label, "`binary` entries must be tables"}); return false; }
      warnUnknown(d, t, {"os", "architecture", "abi", "path"}, "[[binary]]");
      BinarySlot b;
      if (!needString(d, label, t, "os", b.os, true, "binary.") || !needString(d, label, t, "architecture", b.arch, true, "binary.") ||
          !needString(d, label, t, "abi", b.abi, true, "binary.") || !needString(d, label, t, "path", b.path, true, "binary."))
        return false;
      m.binaries.push_back(b);
    }
  }
  return true;
}
}  // namespace

// The same rules the reader applies, so a package that cannot be loaded is never produced.
static bool validateManifest(DiagEngine& d, const Manifest& m, Loc at) {
  std::string err;
  if (!validName(m.name)) { d.report("E0709", at, {m.name, "invalid package name (letters, digits, `_`, `-`; must start with a letter)"}); return false; }
  Version v;
  if (!parseVersion(m.version, v, err)) { d.report("E0717", at, {m.version, err}); return false; }
  if (m.abi != kAbiId) { d.report("E0709", at, {m.name, "ABI `" + m.abi + "` is not `" + kAbiId + "`"}); return false; }
  Requirement r;
  if (!m.compilerReq.empty() && !parseRequirement(m.compilerReq, r, err)) { d.report("E0717", at, {m.compilerReq, err}); return false; }
  for (auto& dep : m.deps) {
    if (!validName(dep.name)) { d.report("E0709", at, {m.name, "invalid dependency name `" + dep.name + "`"}); return false; }
    if (!parseRequirement(dep.req, r, err)) { d.report("E0717", at, {dep.req, err}); return false; }
  }
  for (auto& b : m.binaries)
    if (b.os.empty() || b.arch.empty() || b.abi.empty() || b.path.empty()) { d.report("E0709", at, {m.name, "incomplete [[binary]] entry"}); return false; }
  return true;
}

bool buildPackage(DiagEngine& d, const Manifest& m, const std::vector<ZipEntry>& content, std::vector<uint8_t>& out) {
  Loc at = anchorLoc(d, m.name + ".l7");
  if (!validateManifest(d, m, at)) return false;
  std::vector<ZipEntry> sorted = content;
  std::sort(sorted.begin(), sorted.end(), [](const ZipEntry& a, const ZipEntry& b) { return a.name < b.name; });
  std::set<std::string> names;
  for (auto& e : sorted) {
    std::string why;
    if (!zipSafeName(e.name, why)) { d.report("E0711", at, {e.name}); return false; }
    if (e.name == "manifest.toml" || e.name == "checksums.sha256") { d.report("E0709", at, {m.name, "`" + e.name + "` is generated; it must not be passed as content"}); return false; }
    if (!names.insert(e.name).second) { d.report("E0709", at, {m.name, "duplicate entry `" + e.name + "`"}); return false; }
  }
  for (auto& b : m.binaries)
    if (!names.count(b.path)) { d.report("E0709", at, {m.name, "binary slot path `" + b.path + "` is not in the package"}); return false; }
  std::vector<ZipEntry> entries;
  std::string mt = manifestToToml(m);
  entries.push_back({"manifest.toml", std::vector<uint8_t>(mt.begin(), mt.end())});
  for (auto& e : sorted) entries.push_back(e);
  std::vector<std::pair<std::string, std::string>> sums;
  for (auto& e : entries) sums.emplace_back(e.name, sha256Hex(e.data));
  std::sort(sums.begin(), sums.end());
  std::string cs;
  for (auto& s : sums) cs += s.second + "  " + s.first + "\n";
  entries.push_back({"checksums.sha256", std::vector<uint8_t>(cs.begin(), cs.end())});
  std::string err;
  if (!zipWrite(entries, out, err)) { d.report("E0718", at, {m.name + ".l7", err}); return false; }
  return true;
}

bool loadPackageBytes(DiagEngine& d, const std::string& label, const std::vector<uint8_t>& bytes, Package& out) {
  out = Package();
  out.label = label;
  Loc at = anchorLoc(d, label);
  std::string err;
  if (!zipRead(bytes, out.files, err)) {
    size_t q1 = err.find('`'), q2 = q1 == std::string::npos ? q1 : err.find('`', q1 + 1);
    if (err.find("unsafe entry name") != std::string::npos && q2 != std::string::npos) d.report("E0711", at, {err.substr(q1 + 1, q2 - q1 - 1)});
    else d.report("E0718", at, {label, err});
    return false;
  }
  out.sha256 = sha256Hex(bytes);
  const ZipEntry* mf = out.find("manifest.toml");
  const ZipEntry* cs = out.find("checksums.sha256");
  if (!mf) { d.report("E0709", at, {label, "missing manifest.toml"}); return false; }
  if (!cs) { d.report("E0709", at, {label, "missing checksums.sha256"}); return false; }
  // every entry except the checksum file itself must be listed exactly once with a matching hash
  std::map<std::string, std::string> listed;
  std::istringstream in(std::string(cs->data.begin(), cs->data.end()));
  std::string line;
  while (std::getline(in, line)) {
    bool good = line.size() > 66 && line[64] == ' ' && line[65] == ' ';
    for (size_t i = 0; good && i < 64; i++) good = (line[i] >= '0' && line[i] <= '9') || (line[i] >= 'a' && line[i] <= 'f');
    if (!good) { d.report("E0710", at, {"checksums.sha256", label}); return false; }
    if (!listed.emplace(line.substr(66), line.substr(0, 64)).second) { d.report("E0710", at, {line.substr(66), label}); return false; }
  }
  for (auto& e : out.files) {
    if (e.name == "checksums.sha256") continue;
    auto it = listed.find(e.name);
    if (it == listed.end() || it->second != sha256Hex(e.data)) { d.report("E0710", at, {e.name, label}); return false; }
  }
  for (auto& kv : listed)
    if (kv.first == "checksums.sha256" || !out.find(kv.first)) { d.report("E0710", at, {kv.first, label}); return false; }
  if (!parseManifestText(d, label, std::string(mf->data.begin(), mf->data.end()), out.m)) return false;
  for (auto& b : out.m.binaries)
    if (!out.find(b.path)) { d.report("E0709", at, {label, "binary slot path `" + b.path + "` is not in the package"}); return false; }
  if (!out.m.compilerReq.empty()) {
    Requirement r; Version cv; std::string e2;
    parseRequirement(out.m.compilerReq, r, e2);
    parseVersion(kCompilerVersion, cv, e2);
    if (!matches(r, cv)) { d.report("E0709", at, {label, "requires compiler `" + out.m.compilerReq + "` but this is c7c " + kCompilerVersion}); return false; }
  }
  if (out.m.abi != kAbiId) { d.report("E0709", at, {label, "built for ABI `" + out.m.abi + "`, this compiler uses `" + kAbiId + "`"}); return false; }
  return true;
}

bool loadPackage(DiagEngine& d, const std::string& path, Package& out) {
  std::vector<uint8_t> bytes;
  if (!readFileBytes(path, bytes)) { d.report("E0709", anchorLoc(d, path), {path, "cannot read the file"}); return false; }
  return loadPackageBytes(d, path, bytes, out);
}

bool extractPackage(DiagEngine& d, const Package& p, const std::string& dest) {
  Loc at = anchorLoc(d, p.label);
  if (fs::exists(dest)) { d.report("E0709", at, {p.label, "destination `" + dest + "` already exists"}); return false; }
  fs::path root = fs::absolute(dest).lexically_normal();
  for (auto& e : p.files) {
    std::string why;
    if (!zipSafeName(e.name, why)) { d.report("E0711", at, {e.name}); return false; }
    fs::path target = (root / e.name).lexically_normal();
    if (target.string().rfind(root.string() + "/", 0) != 0) { d.report("E0711", at, {e.name}); return false; }  // defence in depth
    fs::create_directories(target.parent_path());
    if (!writeFileBytes(target.string(), e.data)) { d.report("E0709", at, {p.label, "cannot write `" + target.string() + "`"}); return false; }
  }
  return true;
}

}  // namespace c7pkg
