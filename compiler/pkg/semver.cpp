#include "semver.hpp"

#include <algorithm>
#include <cctype>

namespace c7pkg {

std::string Version::str() const {
  std::string s = std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
  for (size_t i = 0; i < pre.size(); i++) s += (i ? "." : "-") + pre[i];
  if (!build.empty()) s += "+" + build;
  return s;
}

namespace {
bool isNum(const std::string& s) { return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; }); }
bool identOk(const std::string& s) {
  return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return isalnum((unsigned char)c) || c == '-'; });
}
bool parseNum(const std::string& s, uint64_t& out, std::string& err) {
  if (!isNum(s)) { err = "`" + s + "` is not a number"; return false; }
  if (s.size() > 1 && s[0] == '0') { err = "leading zero in `" + s + "`"; return false; }
  uint64_t v = 0;
  for (char c : s) {
    uint64_t dg = (uint64_t)(c - '0');
    if (v > (UINT64_MAX - dg) / 10) { err = "number `" + s + "` is too large"; return false; }
    v = v * 10 + dg;
  }
  out = v;
  return true;
}
std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  size_t st = 0;
  for (size_t i = 0; i <= s.size(); i++)
    if (i == s.size() || s[i] == sep) { out.push_back(s.substr(st, i - st)); st = i + 1; }
  return out;
}
std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && isspace((unsigned char)s[a])) a++;
  while (b > a && isspace((unsigned char)s[b - 1])) b--;
  return s.substr(a, b - a);
}
}  // namespace

bool parseVersion(const std::string& text, Version& out, std::string& err) {
  Version v;
  std::string core = text;
  size_t plus = core.find('+');
  if (plus != std::string::npos) {
    v.build = core.substr(plus + 1);
    core = core.substr(0, plus);
    for (auto& id : split(v.build, '.')) if (!identOk(id)) { err = "invalid build metadata"; return false; }
  }
  size_t dash = core.find('-');
  if (dash != std::string::npos) {
    std::string pre = core.substr(dash + 1);
    core = core.substr(0, dash);
    v.pre = split(pre, '.');
    for (auto& id : v.pre) {
      if (!identOk(id)) { err = "invalid pre-release identifier"; return false; }
      if (isNum(id) && id.size() > 1 && id[0] == '0') { err = "leading zero in numeric pre-release identifier `" + id + "`"; return false; }
    }
  }
  auto parts = split(core, '.');
  if (parts.size() != 3) { err = "expected MAJOR.MINOR.PATCH"; return false; }
  if (!parseNum(parts[0], v.major, err) || !parseNum(parts[1], v.minor, err) || !parseNum(parts[2], v.patch, err)) return false;
  out = v;
  return true;
}

int compareVersions(const Version& a, const Version& b) {
  if (a.major != b.major) return a.major < b.major ? -1 : 1;
  if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
  if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
  if (a.pre.empty() != b.pre.empty()) return a.pre.empty() ? 1 : -1;  // a release is greater than its pre-releases
  for (size_t i = 0; i < std::min(a.pre.size(), b.pre.size()); i++) {
    const std::string &x = a.pre[i], &y = b.pre[i];
    bool nx = isNum(x), ny = isNum(y);
    if (nx && ny) {
      uint64_t vx = 0, vy = 0;
      std::string e;
      bool okx = parseNum(x, vx, e), oky = parseNum(y, vy, e);
      if (okx && oky) { if (vx != vy) return vx < vy ? -1 : 1; }
      else if (x.size() != y.size()) return x.size() < y.size() ? -1 : 1;  // numbers beyond 64 bits: longer is larger
      else if (x != y) return x < y ? -1 : 1;
    } else if (nx != ny) return nx ? -1 : 1;  // numeric identifiers sort before alphanumeric ones
    else if (x != y) return x < y ? -1 : 1;
  }
  if (a.pre.size() != b.pre.size()) return a.pre.size() < b.pre.size() ? -1 : 1;
  return 0;
}

namespace {
struct Partial {
  uint64_t n[3] = {0, 0, 0};
  int count = 0;  // how many of MAJOR.MINOR.PATCH were given (wildcards stop the count)
  bool wild = false;
  std::vector<std::string> pre;
};

bool parsePartial(const std::string& text, Partial& p, std::string& err) {
  std::string core = text;
  size_t dash = core.find('-');
  if (dash != std::string::npos) {
    for (auto& id : split(core.substr(dash + 1), '.')) {
      if (!identOk(id) || (isNum(id) && id.size() > 1 && id[0] == '0')) { err = "invalid pre-release identifier"; return false; }
      p.pre.push_back(id);
    }
    core = core.substr(0, dash);
    if (p.pre.empty()) { err = "empty pre-release"; return false; }
  }
  if (core.find('+') != std::string::npos) core = core.substr(0, core.find('+'));
  auto parts = split(core, '.');
  if (parts.size() > 3 || core.empty()) { err = "expected up to three dot-separated numbers"; return false; }
  for (size_t i = 0; i < parts.size(); i++) {
    if (parts[i] == "*" || parts[i] == "x" || parts[i] == "X") {
      if (!p.pre.empty()) { err = "wildcard with pre-release"; return false; }
      for (size_t k = i; k < parts.size(); k++)
        if (parts[k] != "*" && parts[k] != "x" && parts[k] != "X") { err = "only wildcards may follow a wildcard"; return false; }
      p.wild = true;
      break;
    }
    if (!parseNum(parts[i], p.n[i], err)) return false;
    p.count = (int)i + 1;
  }
  if (p.count == 0 && parts[0] != "*" && parts[0] != "x" && parts[0] != "X") { err = "empty version"; return false; }
  if (!p.pre.empty() && p.count != 3) { err = "a pre-release needs a full MAJOR.MINOR.PATCH"; return false; }
  return true;
}

Version make(uint64_t a, uint64_t b, uint64_t c, std::vector<std::string> pre = {}) { Version v; v.major = a; v.minor = b; v.patch = c; v.pre = std::move(pre); return v; }

void add(Requirement& r, Requirement::Op op, Version v) { r.cmps.push_back({op, std::move(v)}); }

// [lo, hi) for a partial version treated as a range (1 -> 1.0.0..2.0.0, 1.2 -> 1.2.0..1.3.0)
void addRange(Requirement& r, const Partial& p) {
  if (p.count == 0) { return; }  // "*": no constraint
  if (p.count == 1) { add(r, Requirement::GE, make(p.n[0], 0, 0)); add(r, Requirement::LT, make(p.n[0] + 1, 0, 0)); }
  else if (p.count == 2) { add(r, Requirement::GE, make(p.n[0], p.n[1], 0)); add(r, Requirement::LT, make(p.n[0], p.n[1] + 1, 0)); }
  else add(r, Requirement::EQ, make(p.n[0], p.n[1], p.n[2], p.pre));
}
}  // namespace

bool parseRequirement(const std::string& text, Requirement& out, std::string& err) {
  Requirement r;
  r.text = text;
  if (trim(text).empty()) { err = "empty requirement"; return false; }
  for (auto& raw : split(text, ',')) {
    std::string c = trim(raw);
    if (c.empty()) { err = "empty comparator"; return false; }
    if (c == "*") { r.any = r.cmps.empty(); continue; }
    std::string op;
    size_t k = 0;
    while (k < c.size() && std::string("^~<>=").find(c[k]) != std::string::npos) k++;
    op = c.substr(0, k);
    std::string rest = trim(c.substr(k));
    static const char* known[] = {"", "^", "~", "=", ">", "<", ">=", "<="};
    if (std::find(std::begin(known), std::end(known), op) == std::end(known)) { err = "unknown operator `" + op + "`"; return false; }
    Partial p;
    if (!parsePartial(rest, p, err)) return false;
    if (p.count == 0 && op != "" && op != "=" && op != "^" && op != "~") { err = "operator `" + op + "` needs a version"; return false; }
    if (p.count == 0) { continue; }  // "1.*" style handled below; bare "*" was handled above
    if (p.wild && (op == "" || op == "=")) addRange(r, p);  // 1.* / 1.2.* are ranges, not carets
    else if (op == "" || op == "^") {
      if (p.count == 3 && !p.pre.empty()) { add(r, Requirement::GE, make(p.n[0], p.n[1], p.n[2], p.pre)); }
      else add(r, Requirement::GE, make(p.n[0], p.count >= 2 ? p.n[1] : 0, p.count >= 3 ? p.n[2] : 0));
      if (p.n[0] > 0 || p.count == 1) add(r, Requirement::LT, make(p.n[0] + 1, 0, 0));
      else if (p.n[1] > 0 || p.count == 2) add(r, Requirement::LT, make(0, p.n[1] + 1, 0));
      else add(r, Requirement::LT, make(0, 0, p.n[2] + 1));
    } else if (op == "~") {
      add(r, Requirement::GE, make(p.n[0], p.count >= 2 ? p.n[1] : 0, p.count >= 3 ? p.n[2] : 0, p.pre));
      if (p.count == 1) add(r, Requirement::LT, make(p.n[0] + 1, 0, 0));
      else add(r, Requirement::LT, make(p.n[0], p.n[1] + 1, 0));
    } else if (op == "=") addRange(r, p);
    else if (op == ">=") add(r, Requirement::GE, make(p.n[0], p.count >= 2 ? p.n[1] : 0, p.count >= 3 ? p.n[2] : 0, p.pre));
    else if (op == "<") add(r, Requirement::LT, make(p.n[0], p.count >= 2 ? p.n[1] : 0, p.count >= 3 ? p.n[2] : 0, p.pre));
    else if (op == ">") {
      if (p.count == 1) add(r, Requirement::GE, make(p.n[0] + 1, 0, 0));
      else if (p.count == 2) add(r, Requirement::GE, make(p.n[0], p.n[1] + 1, 0));
      else add(r, Requirement::GT, make(p.n[0], p.n[1], p.n[2], p.pre));
    } else if (op == "<=") {
      if (p.count == 1) add(r, Requirement::LT, make(p.n[0] + 1, 0, 0));
      else if (p.count == 2) add(r, Requirement::LT, make(p.n[0], p.n[1] + 1, 0));
      else add(r, Requirement::LE, make(p.n[0], p.n[1], p.n[2], p.pre));
    }
  }
  r.any = r.cmps.empty();
  out = r;
  return true;
}

bool matches(const Requirement& r, const Version& v) {
  for (auto& c : r.cmps) {
    int cmp = compareVersions(v, c.v);
    bool ok = c.op == Requirement::EQ ? cmp == 0 : c.op == Requirement::LT ? cmp < 0 : c.op == Requirement::LE ? cmp <= 0 : c.op == Requirement::GT ? cmp > 0 : cmp >= 0;
    if (!ok) return false;
  }
  if (!v.pre.empty()) {  // pre-releases only match when the requirement names a pre-release of the same version triple
    bool allowed = false;
    for (auto& c : r.cmps)
      if (!c.v.pre.empty() && c.v.major == v.major && c.v.minor == v.minor && c.v.patch == v.patch) allowed = true;
    if (!allowed) return false;
  }
  return true;
}

}  // namespace c7pkg
