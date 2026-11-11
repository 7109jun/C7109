#include "options.hpp"

#include <set>

#include "semver.hpp"
#include "toml.hpp"
#include "version.hpp"

using namespace c7;

namespace c7pkg {

std::vector<std::string> Options::toFlags() const {
  std::vector<std::string> f;
  f.push_back("-" + optimization);
  if (mode == "debug") f.push_back("--debug");  // --debug also turns the overflow check on, so only a differing value needs a flag
  bool debugImplies = mode == "debug";
  if (overflowCheck != debugImplies) f.push_back(overflowCheck ? "-foverflow-check" : "-fno-overflow-check");
  if (boundsCheck) f.push_back("-fbounds-check");
  if (debugAlloc) f.push_back("-fdebug-alloc");
  if (link == "static") f.push_back("--link=static");
  if (language != "en") f.push_back("--lang=" + language);
  if (os || arch) f.push_back("--target=" + arch.value_or("x86_64") + "-" + os.value_or("linux"));
  for (auto& i : include) f.push_back("-I" + i);
  return f;
}

namespace {

void warnUnknown(DiagEngine& d, const TomlValue& t, const std::set<std::string>& known, const std::string& where) {
  for (auto& kv : t.tbl) if (!known.count(kv.first)) d.report("W0701", kv.second.loc, {kv.first, where});
}

bool strKey(DiagEngine& d, const TomlValue& t, const char* key, const std::string& where, std::optional<std::string>& out, const std::set<std::string>* allowed = nullptr) {
  const TomlValue* v = t.get(key);
  if (!v) return true;
  if (v->kind != TomlValue::String) { d.report("E0719", v->loc, {where + key, "expected a string"}); return false; }
  if (allowed && !allowed->count(v->str)) {
    std::string list;
    for (auto& a : *allowed) list += (list.empty() ? "" : ", ") + std::string("\"") + a + "\"";
    d.report("E0719", v->loc, {where + key, "expected one of " + list});
    return false;
  }
  out = v->str;
  return true;
}
bool boolKey(DiagEngine& d, const TomlValue& t, const char* key, const std::string& where, std::optional<bool>& out) {
  const TomlValue* v = t.get(key);
  if (!v) return true;
  if (v->kind != TomlValue::Boolean) { d.report("E0719", v->loc, {where + key, "expected true or false"}); return false; }
  out = v->b;
  return true;
}
bool listKey(DiagEngine& d, const TomlValue& t, const char* key, const std::string& where, std::vector<std::string>& out) {
  const TomlValue* v = t.get(key);
  if (!v) return true;
  if (v->kind != TomlValue::Array) { d.report("E0719", v->loc, {where + key, "expected an array of strings"}); return false; }
  bool ok = true;
  for (auto& e : v->arr) {
    if (e.kind != TomlValue::String || e.str.empty() || e.str.find('\0') != std::string::npos) { d.report("E0719", e.loc, {where + key, "expected non-empty strings"}); ok = false; continue; }
    out.push_back(e.str);
  }
  return ok;
}
const TomlValue* tableKey(DiagEngine& d, const TomlValue& root, const char* key, bool& ok) {
  const TomlValue* v = root.get(key);
  if (!v) return nullptr;
  if (v->kind != TomlValue::Table) { d.report("E0719", v->loc, {key, "expected a table"}); ok = false; return nullptr; }
  return v;
}

const std::set<std::string> kModes{"debug", "release"}, kOpts{"O0", "O1", "O2", "O3"}, kLangs{"en", "ko"}, kLinks{"static", "dynamic"};

void dedupe(std::vector<std::string>& v) {
  std::set<std::string> seen;
  std::vector<std::string> o;
  for (auto& s : v) if (seen.insert(s).second) o.push_back(s);
  v = std::move(o);
}

}  // namespace

bool loadCset7(DiagEngine& d, int fileId, OptionLayer& out) {
  out = OptionLayer();
  TomlValue root;
  if (!parseToml(d, fileId, root)) return false;
  bool ok = true;
  warnUnknown(d, root, {"compiler", "target", "build", "diagnostics", "link", "paths"}, ".cset7");
  if (auto* t = tableKey(d, root, "compiler", ok)) {
    warnUnknown(d, *t, {"requires"}, "[compiler]");
    ok &= strKey(d, *t, "requires", "compiler.", out.compilerReq);
    if (out.compilerReq) {
      Requirement r; std::string e;
      if (!parseRequirement(*out.compilerReq, r, e)) { d.report("E0717", t->get("requires")->loc, {*out.compilerReq, e}); ok = false; out.compilerReq.reset(); }
    }
  }
  if (auto* t = tableKey(d, root, "target", ok)) {
    warnUnknown(d, *t, {"os", "architecture", "abi"}, "[target]");
    for (const char* k : {"os", "architecture", "abi"}) {
      const TomlValue* v = t->get(k);
      if (!v) continue;
      bool good = v->kind == TomlValue::String && !v->str.empty();
      if (good) for (char c : v->str) if (!(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9') && c != '_') good = false;
      if (!good) { d.report("E0719", v->loc, {std::string("target.") + k, "expected lowercase letters, digits and `_`"}); ok = false; }
    }
    ok &= strKey(d, *t, "os", "target.", out.os);
    ok &= strKey(d, *t, "architecture", "target.", out.arch);
    ok &= strKey(d, *t, "abi", "target.", out.abi);
  }
  if (auto* t = tableKey(d, root, "build", ok)) {
    warnUnknown(d, *t, {"mode", "optimization", "bounds_check", "overflow_check", "debug_alloc"}, "[build]");
    ok &= strKey(d, *t, "mode", "build.", out.mode, &kModes);
    ok &= strKey(d, *t, "optimization", "build.", out.optimization, &kOpts);
    ok &= boolKey(d, *t, "bounds_check", "build.", out.boundsCheck);
    ok &= boolKey(d, *t, "overflow_check", "build.", out.overflowCheck);
    ok &= boolKey(d, *t, "debug_alloc", "build.", out.debugAlloc);
  }
  if (auto* t = tableKey(d, root, "diagnostics", ok)) {
    warnUnknown(d, *t, {"language"}, "[diagnostics]");
    ok &= strKey(d, *t, "language", "diagnostics.", out.language, &kLangs);
  }
  if (auto* t = tableKey(d, root, "link", ok)) {
    warnUnknown(d, *t, {"mode"}, "[link]");
    ok &= strKey(d, *t, "mode", "link.", out.link, &kLinks);
  }
  if (auto* t = tableKey(d, root, "paths", ok)) {
    warnUnknown(d, *t, {"include", "library", "registry"}, "[paths]");
    ok &= listKey(d, *t, "include", "paths.", out.include);
    ok &= listKey(d, *t, "library", "paths.", out.library);
    ok &= listKey(d, *t, "registry", "paths.", out.registry);
  }
  return ok;
}

OptionLayer layerFromConfig(const BuildConfig& c) {
  OptionLayer l;
  l.mode = c.mode; l.optimization = c.optimization;
  l.boundsCheck = c.boundsCheck; l.debugAlloc = c.debugAlloc; l.overflowCheck = c.overflowCheck;
  return l;
}

OptionLayer layerFromTarget(const BuildTarget& t) {
  OptionLayer l;
  l.mode = t.mode; l.optimization = t.optimization;
  l.boundsCheck = t.boundsCheck; l.debugAlloc = t.debugAlloc; l.overflowCheck = t.overflowCheck;
  l.link = t.link;
  if (t.platform) {  // "<arch>-<os>" (validated by the build-file parser)
    auto dash = t.platform->find('-');
    l.arch = t.platform->substr(0, dash);
    l.os = t.platform->substr(dash + 1);
  }
  l.include = t.includeDirs;
  return l;
}

bool parseOptionArg(const std::string& a, OptionLayer& l, std::string& error) {
  auto value = [&](const char* prefix) { return a.substr(std::string(prefix).size()); };
  if (a.size() == 3 && a.rfind("-O", 0) == 0) {
    if (a[2] < '0' || a[2] > '3') { error = "optimization level must be -O0..-O3"; return true; }
    l.optimization = a.substr(1);
    return true;
  }
  if (a.rfind("--mode=", 0) == 0) {
    std::string v = value("--mode=");
    if (!kModes.count(v)) { error = "--mode must be debug or release"; return true; }
    l.mode = v;
    return true;
  }
  if (a == "--debug") { l.mode = "debug"; return true; }
  if (a == "--bounds-check") { l.boundsCheck = true; return true; }
  if (a == "--no-bounds-check") { l.boundsCheck = false; return true; }
  if (a == "--debug-alloc") { l.debugAlloc = true; return true; }
  if (a == "--no-debug-alloc") { l.debugAlloc = false; return true; }
  if (a == "--overflow-check") { l.overflowCheck = true; return true; }
  if (a == "--no-overflow-check") { l.overflowCheck = false; return true; }
  if (a.rfind("--lang=", 0) == 0) {
    std::string v = value("--lang=");
    if (!kLangs.count(v)) { error = "--lang must be en or ko"; return true; }
    l.language = v;
    return true;
  }
  if (a.rfind("--link=", 0) == 0) {
    std::string v = value("--link=");
    if (!kLinks.count(v)) { error = "--link must be static or dynamic"; return true; }
    l.link = v;
    return true;
  }
  if (a.rfind("--registry=", 0) == 0) { l.registry.push_back(value("--registry=")); return true; }
  return false;
}

Options mergeOptions(const std::vector<const OptionLayer*>& layers) {
  Options o;
  std::optional<bool> overflow;
  for (const OptionLayer* l : layers) {
    if (l->mode) o.mode = *l->mode;
    if (l->optimization) o.optimization = *l->optimization;
    if (l->boundsCheck) o.boundsCheck = *l->boundsCheck;
    if (l->debugAlloc) o.debugAlloc = *l->debugAlloc;
    if (l->overflowCheck) overflow = l->overflowCheck;
    if (l->language) o.language = *l->language;
    if (l->link) o.link = *l->link;
    if (l->os) o.os = l->os;
    if (l->arch) o.arch = l->arch;
    if (l->abi) o.abi = *l->abi;
    if (l->compilerReq) o.compilerReq = *l->compilerReq;
    o.include.insert(o.include.end(), l->include.begin(), l->include.end());
    o.library.insert(o.library.end(), l->library.begin(), l->library.end());
    o.registry.insert(o.registry.end(), l->registry.begin(), l->registry.end());
  }
  o.overflowCheck = overflow ? *overflow : o.mode == "debug";
  dedupe(o.include); dedupe(o.library); dedupe(o.registry);
  return o;
}

bool checkOptionsCompatible(DiagEngine& d, Loc at, const Options& o) {
  bool ok = true;
  if (o.abi != kAbiId) { d.report("E0719", at, {"target.abi", "`" + o.abi + "` is not the ABI of this compiler (`" + kAbiId + "`)"}); ok = false; }
  if (!o.compilerReq.empty()) {
    Requirement r; std::string e;
    Version cv;
    parseVersion(kCompilerVersion, cv, e);
    if (parseRequirement(o.compilerReq, r, e) && !matches(r, cv)) {
      d.report("E0719", at, {"compiler.requires", "`" + o.compilerReq + "` is not satisfied by c7c " + kCompilerVersion});
      ok = false;
    }
  }
  return ok;
}

namespace {
void jstr(std::string& o, const std::string& s) {
  o += '"';
  for (unsigned char c : s) {
    if (c == '"') o += "\\\"";
    else if (c == '\\') o += "\\\\";
    else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
    else o += (char)c;
  }
  o += '"';
}
void jlist(std::string& o, const std::vector<std::string>& v) {
  o += '[';
  for (size_t i = 0; i < v.size(); i++) { if (i) o += ','; jstr(o, v[i]); }
  o += ']';
}
}  // namespace

std::string optionsToJson(const Options& o) {
  std::string s = "{\"mode\":";
  jstr(s, o.mode);
  s += ",\"optimization\":"; jstr(s, o.optimization);
  s += std::string(",\"bounds_check\":") + (o.boundsCheck ? "true" : "false");
  s += std::string(",\"overflow_check\":") + (o.overflowCheck ? "true" : "false");
  s += std::string(",\"debug_alloc\":") + (o.debugAlloc ? "true" : "false");
  s += ",\"language\":"; jstr(s, o.language);
  s += ",\"link\":"; jstr(s, o.link);
  if (o.os) { s += ",\"os\":"; jstr(s, *o.os); }
  if (o.arch) { s += ",\"architecture\":"; jstr(s, *o.arch); }
  s += ",\"abi\":"; jstr(s, o.abi);
  s += ",\"include\":"; jlist(s, o.include);
  s += ",\"library\":"; jlist(s, o.library);
  s += ",\"registry\":"; jlist(s, o.registry);
  s += ",\"flags\":"; jlist(s, o.toFlags());
  return s + "}";
}

}  // namespace c7pkg
