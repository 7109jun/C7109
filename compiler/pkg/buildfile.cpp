#include "buildfile.hpp"

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <set>

#include "semver.hpp"
#include "toml.hpp"

using namespace c7;
namespace fs = std::filesystem;

namespace c7pkg {

bool normalizeProjectPath(const std::string& in, std::string& out, std::string& why) {
  if (in.empty()) { why = "empty path"; return false; }
  for (unsigned char c : in) {
    if (c < 0x20 || c == 0x7F) { why = "control character in path"; return false; }
    if (c == '\\') { why = "backslash in path (use `/`)"; return false; }
  }
  if (in[0] == '/') { why = "absolute path"; return false; }
  if (in.size() >= 2 && in[1] == ':' && std::isalpha((unsigned char)in[0])) { why = "drive letter in path"; return false; }
  std::vector<std::string> parts;
  size_t i = 0;
  while (i <= in.size()) {
    size_t j = in.find('/', i);
    if (j == std::string::npos) j = in.size();
    std::string seg = in.substr(i, j - i);
    i = j + 1;
    if (seg.empty() || seg == ".") continue;
    if (seg == "..") {
      if (parts.empty()) { why = "path leaves the project directory"; return false; }
      parts.pop_back();
      continue;
    }
    parts.push_back(seg);
  }
  out.clear();
  for (size_t k = 0; k < parts.size(); k++) out += (k ? "/" : "") + parts[k];
  if (out.empty()) out = ".";
  return true;
}

namespace {

enum class TokKind { Ident, Str, Colon };
struct Tok {
  TokKind kind;
  std::string text;
  Loc loc;
};
struct Line {
  Loc loc;
  std::vector<Tok> toks;
  std::vector<Line> kids;
};

bool isIdStart(char c) { return std::isalpha((unsigned char)c) || c == '_'; }
bool isIdChar(char c) { return std::isalnum((unsigned char)c) || c == '_' || c == '-'; }

// ---- lexing and indentation tree (syntax errors are fatal: E0702)
struct Lexer {
  DiagEngine& d;
  int fid;
  bool syntax(uint32_t line, uint32_t col, const std::string& msg) { d.report("E0702", Loc{fid, line, col}, {msg}); return false; }

  bool lexLine(const std::string& s, uint32_t lineNo, size_t indent, std::vector<Tok>& out) {
    size_t i = indent;
    while (i < s.size()) {
      char c = s[i];
      uint32_t col = (uint32_t)i + 1;
      if (c == ' ' || c == '\t') { i++; continue; }
      if (c == '#') break;
      if (c == ':') { out.push_back({TokKind::Colon, ":", Loc{fid, lineNo, col}}); i++; continue; }
      if (c == '"') {
        std::string v;
        size_t j = i + 1;
        for (;; j++) {
          if (j >= s.size()) return syntax(lineNo, col, "unterminated string");
          unsigned char u = s[j];
          if (u == '"') break;
          if (u < 0x20 || u == 0x7F) return syntax(lineNo, (uint32_t)j + 1, "control character in string");
          if (u == '\\') {
            if (++j >= s.size()) return syntax(lineNo, col, "unterminated string");
            char e = s[j];
            if (e == '"' || e == '\\') v += e;
            else if (e == 'n') v += '\n';
            else if (e == 't') v += '\t';
            else return syntax(lineNo, (uint32_t)j, std::string("unknown escape `\\") + e + "` (allowed: \\\" \\\\ \\n \\t)");
          } else v += (char)u;
        }
        out.push_back({TokKind::Str, v, Loc{fid, lineNo, col}});
        i = j + 1;
        continue;
      }
      if (isIdStart(c)) {
        size_t j = i;
        while (j < s.size() && isIdChar(s[j])) j++;
        out.push_back({TokKind::Ident, s.substr(i, j - i), Loc{fid, lineNo, col}});
        i = j;
        continue;
      }
      return syntax(lineNo, col, "unexpected character");
    }
    return true;
  }

  bool run(const std::string& text, std::vector<Line>& top) {
    size_t bad = 0;
    if (!isValidUtf8(text, bad)) return syntax(1, 1, "invalid UTF-8 in file");
    std::vector<int> levels{0};          // indentation widths of the open levels
    std::vector<Line*> open;             // open[k] = most recent line at level k
    size_t pos = 0;
    uint32_t lineNo = 0;
    while (pos <= text.size()) {
      size_t nl = text.find('\n', pos);
      bool last = nl == std::string::npos;
      std::string s = text.substr(pos, last ? std::string::npos : nl - pos);
      pos = last ? text.size() + 1 : nl + 1;
      lineNo++;
      if (lineNo > kBuildMaxLines) return syntax(lineNo, 1, "too many lines");
      if (!s.empty() && s.back() == '\r') s.pop_back();
      if (s.size() > kBuildMaxLineBytes) return syntax(lineNo, 1, "line too long");
      if (s.find('\r') != std::string::npos) return syntax(lineNo, (uint32_t)s.find('\r') + 1, "stray carriage return");
      size_t ind = 0;
      while (ind < s.size() && s[ind] == ' ') ind++;
      if (ind < s.size() && s[ind] == '\t') return syntax(lineNo, (uint32_t)ind + 1, "tab in indentation (use spaces)");
      std::vector<Tok> toks;
      if (!lexLine(s, lineNo, ind, toks)) return false;
      if (toks.empty()) continue;  // blank or comment-only
      if (ind > (size_t)levels.back()) {
        if (open.empty()) return syntax(lineNo, 1, "unexpected indentation");
        if (levels.size() >= kBuildMaxIndentLevels) return syntax(lineNo, 1, "blocks nested too deeply");
        levels.push_back((int)ind);
      } else {
        while ((size_t)levels.back() > ind) { levels.pop_back(); }
        if ((size_t)levels.back() != ind) return syntax(lineNo, 1, "inconsistent indentation (does not match any enclosing block)");
      }
      size_t level = levels.size() - 1;
      Line ln;
      ln.loc = Loc{fid, lineNo, (uint32_t)ind + 1};
      ln.toks = std::move(toks);
      std::vector<Line>& siblings = level == 0 ? top : open[level - 1]->kids;
      siblings.push_back(std::move(ln));
      open.resize(level);
      open.push_back(&siblings.back());
      // pointers into `siblings` stay valid: a vector is only appended to while its owner is the most recent line
    }
    return true;
  }
};

// ---- semantic analysis
struct Analyzer {
  DiagEngine& d;
  int fid;
  std::string baseDir;
  BuildFile& out;
  bool ok = true;

  void err(const std::string& code, Loc l, std::vector<std::string> a) { d.report(code, l, std::move(a)); ok = false; }

  static std::string kw(const Line& l) { return l.toks[0].kind == TokKind::Ident ? l.toks[0].text : std::string(); }

  // The line must be `<keyword> ARGS... [:]`; returns the number of argument tokens (colon excluded) and whether a colon was present.
  bool shape(const Line& l, size_t minArgs, size_t maxArgs, bool& colon, size_t& nargs, const std::string& usage) {
    size_t n = l.toks.size();
    colon = n > 1 && l.toks.back().kind == TokKind::Colon;
    if (colon) n--;
    for (size_t i = 1; i < n; i++)
      if (l.toks[i].kind == TokKind::Colon) { err("E0702", l.toks[i].loc, {"unexpected `:`; expected " + usage}); return false; }
    nargs = n - 1;
    if (nargs > maxArgs) { err("E0702", l.toks[maxArgs + 1].loc, {"unexpected extra token; expected " + usage}); return false; }
    if (nargs < minArgs) { err("E0702", l.loc, {"expected " + usage}); return false; }
    return true;
  }
  bool needStr(const Line& l, size_t idx, const std::string& usage) {
    if (l.toks[idx].kind != TokKind::Str) { err("E0702", l.toks[idx].loc, {"expected a quoted string: " + usage}); return false; }
    return true;
  }
  bool noBlock(const Line& l) {
    if (!l.kids.empty()) { err("E0702", l.kids[0].loc, {"`" + kw(l) + "` does not take an indented block"}); return false; }
    return true;
  }
  bool noColon(const Line& l, bool colon) {
    if (colon) { err("E0702", l.toks.back().loc, {"`" + kw(l) + "` does not take `:`"}); return false; }
    return true;
  }

  bool validName(const std::string& s) {
    if (s.empty() || s.size() > 64 || !std::isalpha((unsigned char)s[0])) return false;
    for (char c : s) if (!std::isalnum((unsigned char)c) && c != '_' && c != '-') return false;
    return true;
  }
  bool projectPath(const Tok& t, const std::string& what, std::string& norm, bool allowDot) {
    std::string why;
    if (!normalizeProjectPath(t.text, norm, why) || (!allowDot && norm == ".")) {
      err("E0719", t.loc, {what, why.empty() ? "expected a file path" : why + " (paths must stay inside the project; \xE2\x9A\xA0 Q13)"});
      return false;
    }
    return true;
  }

  // property lines of a block: `key VALUE` with a fixed set of keys
  using Handler = std::function<void(const Line&)>;
  void block(const Line& parent, const std::string& context, const std::map<std::string, Handler>& keys) {
    std::set<std::string> seen;
    for (const Line& l : parent.kids) {
      std::string k = kw(l);
      if (k.empty()) { err("E0702", l.loc, {"expected a keyword at the start of the line"}); continue; }
      auto it = keys.find(k);
      if (it == keys.end()) { err("E0703", l.toks[0].loc, {k, context}); continue; }
      if (!seen.insert(k).second) { err("E0708", l.toks[0].loc, {"`" + k + "` is set twice in " + context}); continue; }
      it->second(l);
    }
  }

  bool oneString(const Line& l, std::string& v) {
    bool colon; size_t n;
    if (!shape(l, 1, 1, colon, n, "`" + kw(l) + " \"value\"`") || !needStr(l, 1, "`" + kw(l) + " \"value\"`") || !noColon(l, colon) || !noBlock(l)) return false;
    v = l.toks[1].text;
    return true;
  }
  bool oneBool(const Line& l, bool& v) {
    bool colon; size_t n;
    if (!shape(l, 1, 1, colon, n, "`" + kw(l) + " true|false`") || !noColon(l, colon) || !noBlock(l)) return false;
    const Tok& t = l.toks[1];
    if (t.kind == TokKind::Ident && (t.text == "true" || t.text == "false")) { v = t.text == "true"; return true; }
    err("E0719", t.loc, {kw(l), "expected `true` or `false`"});
    return false;
  }
  // `name:` followed by one quoted string per line
  bool stringList(const Line& l, std::vector<const Tok*>& items) {
    bool colon; size_t n;
    if (!shape(l, 0, 0, colon, n, "`" + kw(l) + ":` and an indented list")) return false;
    if (!colon) { err("E0702", l.loc, {"`" + kw(l) + "` needs a `:` before its list"}); return false; }
    bool good = true;
    for (const Line& e : l.kids) {
      if (e.toks.size() != 1 || e.toks[0].kind != TokKind::Str || !e.kids.empty()) { err("E0702", e.loc, {"list entries are single quoted strings"}); good = false; continue; }
      items.push_back(&e.toks[0]);
    }
    return good;
  }

  void doProject(const Line& l) {
    bool colon; size_t n;
    if (!shape(l, 1, 1, colon, n, "`project \"Name\"`") || !needStr(l, 1, "`project \"Name\"`")) return;
    if (out.hasProject) { err("E0708", l.loc, {"more than one `project`"}); return; }
    out.hasProject = true;
    out.projectName = l.toks[1].text;
    if (out.projectName.empty()) err("E0719", l.toks[1].loc, {"project", "the name must not be empty"});
    block(l, "`project` block", {
      {"version", [&](const Line& p) {
         if (!oneString(p, out.version)) return;
         Version v; std::string e;
         if (!parseVersion(out.version, v, e)) err("E0717", p.toks[1].loc, {out.version, e});
       }},
      {"license", [&](const Line& p) { oneString(p, out.license); }},
      {"description", [&](const Line& p) { oneString(p, out.description); }},
    });
  }

  // The five build-option keys shared by `config` blocks and `target` blocks (T has the same member names in both).
  template <class T>
  void buildProps(std::map<std::string, Handler>& keys, T& c) {
    keys["mode"] = [this, &c](const Line& p) {
      std::string v;
      if (!oneString(p, v)) return;
      if (v != "debug" && v != "release") { err("E0719", p.toks[1].loc, {"mode", "expected \"debug\" or \"release\""}); return; }
      c.mode = v;
    };
    keys["optimization"] = [this, &c](const Line& p) {
      std::string v;
      if (!oneString(p, v)) return;
      if (v.size() != 2 || v[0] != 'O' || v[1] < '0' || v[1] > '3') { err("E0719", p.toks[1].loc, {"optimization", "expected \"O0\", \"O1\", \"O2\" or \"O3\""}); return; }
      c.optimization = v;
    };
    keys["bounds_check"] = [this, &c](const Line& p) { bool v; if (oneBool(p, v)) c.boundsCheck = v; };
    keys["debug_alloc"] = [this, &c](const Line& p) { bool v; if (oneBool(p, v)) c.debugAlloc = v; };
    keys["overflow_check"] = [this, &c](const Line& p) { bool v; if (oneBool(p, v)) c.overflowCheck = v; };
  }

  void doConfig(const Line& l) {
    bool colon; size_t n;
    if (!shape(l, 1, 1, colon, n, "`config NAME:`")) return;
    if (l.toks[1].kind != TokKind::Ident) { err("E0702", l.toks[1].loc, {"expected a configuration name (an identifier)"}); return; }
    BuildConfig c;
    c.name = l.toks[1].text;
    c.loc = l.loc;
    for (auto& o : out.configs) if (o.name == c.name) { err("E0708", l.toks[1].loc, {"configuration `" + c.name + "` is defined twice"}); return; }
    std::map<std::string, Handler> keys;
    buildProps(keys, c);
    block(l, "`config` block", keys);
    out.configs.push_back(std::move(c));
  }

  void doTarget(const Line& l) {
    bool colon; size_t n;
    if (!shape(l, 2, 2, colon, n, "`target executable|library \"name\":`")) return;
    BuildTarget t;
    t.loc = l.loc;
    const Tok& k = l.toks[1];
    if (k.kind != TokKind::Ident || (k.text != "executable" && k.text != "library")) { err("E0703", k.loc, {k.kind == TokKind::Ident ? k.text : "\"" + k.text + "\"", "`target` kind (expected `executable` or `library`)"}); return; }
    t.kind = k.text == "executable" ? BuildTarget::Executable : BuildTarget::Library;
    if (!needStr(l, 2, "`target " + k.text + " \"name\":`")) return;
    t.name = l.toks[2].text;
    t.nameLoc = l.toks[2].loc;
    if (!validName(t.name)) err("E0719", l.toks[2].loc, {"target name", "must start with a letter and use only letters, digits, `_` and `-` (max 64)"});
    for (auto& o : out.targets) if (o.name == t.name) { err("E0705", l.toks[2].loc, {t.name}); return; }
    bool hasSources = false, listedAny = false;
    std::map<std::string, Handler> tkeys;
    buildProps(tkeys, t);
    auto addKeys = [&](std::map<std::string, Handler> more) { for (auto& kv : more) tkeys[kv.first] = kv.second; };
    addKeys({
      {"sources", [&](const Line& p) {
         hasSources = true;
         std::vector<const Tok*> items;
         bool wellFormed = stringList(p, items);
         listedAny = !items.empty() || !wellFormed || !p.kids.empty();  // an already-reported bad list must not also count as "empty"
         if (!wellFormed) return;
         std::set<std::string> seen;
         for (auto* it : items) {
           std::string norm;
           if (!projectPath(*it, "sources", norm, false)) continue;
           if (it->text.back() == '/') { err("E0719", it->loc, {"sources", "a source must be a file, not a directory"}); continue; }
           if (!seen.insert(norm).second) { err("E0708", it->loc, {"source `" + norm + "` is listed twice in target `" + t.name + "`"}); continue; }
           t.sources.push_back(norm);
           t.sourceLocs.push_back(it->loc);
         }
       }},
      {"include_dirs", [&](const Line& p) {
         std::vector<const Tok*> items;
         if (!stringList(p, items)) return;
         std::set<std::string> seen;
         for (auto* it : items) {
           std::string norm;
           if (!projectPath(*it, "include_dirs", norm, true)) continue;
           if (!seen.insert(norm).second) { err("E0708", it->loc, {"include directory `" + norm + "` is listed twice in target `" + t.name + "`"}); continue; }
           t.includeDirs.push_back(norm);
         }
       }},
      {"defines", [&](const Line& p) {
         std::vector<const Tok*> items;
         if (!stringList(p, items)) return;
         std::map<std::string, std::string> seen;  // NAME -> full text as written
         for (auto* it : items) {
           std::string name = it->text.substr(0, it->text.find('='));
           bool good = !name.empty() && isIdStart(name[0]);
           for (char c : name) if (!std::isalnum((unsigned char)c) && c != '_') good = false;
           if (!good) { err("E0719", it->loc, {"defines", "expected NAME or NAME=VALUE with an identifier NAME"}); continue; }
           auto f = seen.find(name);
           if (f != seen.end()) {
             err("E0708", it->loc, {f->second == it->text ? "define `" + name + "` is listed twice in target `" + t.name + "`"
                                                          : "define `" + name + "` has conflicting values (`" + f->second + "` and `" + it->text + "`) in target `" + t.name + "`"});
             continue;
           }
           seen[name] = it->text;
           t.defines.push_back(it->text);
         }
       }},
      {"depends", [&](const Line& p) {
         std::vector<const Tok*> items;
         if (!stringList(p, items)) return;
         std::set<std::string> seen;
         for (auto* it : items) {
           if (!validName(it->text)) { err("E0719", it->loc, {"depends", "expected a target or dependency name"}); continue; }
           if (!seen.insert(it->text).second) { err("E0708", it->loc, {"`" + it->text + "` is listed twice in `depends` of target `" + t.name + "`"}); continue; }
           t.depends.push_back(it->text);
           t.dependsLocs.push_back(it->loc);
         }
       }},
      {"output", [&](const Line& p) {
         std::string v;
         if (!oneString(p, v)) return;
         std::string norm;
         if (projectPath(p.toks[1], "output", norm, false)) t.output = norm;
       }},
      {"platform", [&](const Line& p) {
         std::string v;
         if (!oneString(p, v)) return;
         auto dash = v.find('-');
         bool good = dash != std::string::npos && dash > 0 && dash + 1 < v.size() && v.find('-', dash + 1) == std::string::npos;
         for (char c : v) if (c != '-' && c != '_' && !std::isdigit((unsigned char)c) && !(c >= 'a' && c <= 'z')) good = false;
         if (!good) { err("E0719", p.toks[1].loc, {"platform", "expected \"<arch>-<os>\", e.g. \"x86_64-linux\""}); return; }
         t.platform = v;
       }},
      {"link", [&](const Line& p) {
         std::string v;
         if (!oneString(p, v)) return;
         if (v != "static" && v != "dynamic") { err("E0719", p.toks[1].loc, {"link", "expected \"static\" or \"dynamic\""}); return; }
         t.link = v;
       }},
      {"std", [&](const Line& p) {
         std::string v;
         if (!oneString(p, v)) return;
         if (v != "none" && v != "default") { err("E0719", p.toks[1].loc, {"std", "expected \"none\" or \"default\""}); return; }
         t.std = v;
       }},
    });
    block(l, "`target` block", tkeys);
    if (!hasSources || !listedAny) err("E0702", l.loc, {"target `" + t.name + "` needs a non-empty `sources:` list"});
    out.targets.push_back(std::move(t));
  }

  void run(const std::vector<Line>& top) {
    std::vector<std::pair<std::string, Loc>> defaultConfig;
    for (const Line& l : top) {
      std::string k = kw(l);
      bool colon; size_t n;
      if (k.empty()) { err("E0702", l.loc, {"expected a keyword at the start of the line"}); continue; }
      if (k == "project") doProject(l);
      else if (k == "registry") {
        if (!shape(l, 1, 1, colon, n, "`registry \"path\"`") || !needStr(l, 1, "`registry \"path\"`") || !noColon(l, colon) || !noBlock(l)) continue;
        if (l.toks[1].text.empty() || l.toks[1].text.find('\0') != std::string::npos) { err("E0719", l.toks[1].loc, {"registry", "empty path"}); continue; }
        out.registries.push_back(l.toks[1].text);
      } else if (k == "dependency") {
        if (!shape(l, 2, 3, colon, n, "`dependency \"Name\" \"requirement\"` or `dependency \"Name\" path \"dir\"`") || !noColon(l, colon) || !noBlock(l)) continue;
        BuildDependency dep;
        dep.loc = l.loc;
        if (!needStr(l, 1, "dependency name")) continue;
        dep.name = l.toks[1].text;
        if (!validName(dep.name)) { err("E0719", l.toks[1].loc, {"dependency name", "must start with a letter and use only letters, digits, `_` and `-` (max 64)"}); continue; }
        if (n == 2) {
          if (!needStr(l, 2, "version requirement")) continue;
          dep.req = l.toks[2].text;
          Requirement r; std::string e;
          if (!parseRequirement(dep.req, r, e)) { err("E0717", l.toks[2].loc, {dep.req, e}); continue; }
        } else {
          if (l.toks[2].kind != TokKind::Ident || l.toks[2].text != "path") { err("E0702", l.toks[2].loc, {"expected `path` before the dependency directory"}); continue; }
          if (!needStr(l, 3, "dependency path")) continue;
          dep.isPath = true;
          dep.path = l.toks[3].text;
          if (dep.path.empty() || dep.path.find('\0') != std::string::npos) { err("E0719", l.toks[3].loc, {"dependency path", "empty path"}); continue; }
        }
        bool dup = false;
        for (auto& o : out.deps) if (o.name == dep.name) dup = true;
        if (dup) { err("E0708", l.toks[1].loc, {"dependency `" + dep.name + "` is declared twice"}); continue; }
        out.deps.push_back(std::move(dep));
      } else if (k == "config") doConfig(l);
      else if (k == "default_config") {
        if (!shape(l, 1, 1, colon, n, "`default_config NAME`") || !noColon(l, colon) || !noBlock(l)) continue;
        if (l.toks[1].kind != TokKind::Ident) { err("E0702", l.toks[1].loc, {"expected a configuration name (an identifier)"}); continue; }
        if (!defaultConfig.empty()) { err("E0708", l.loc, {"`default_config` is set twice"}); continue; }
        defaultConfig.push_back({l.toks[1].text, l.toks[1].loc});
      } else if (k == "target") doTarget(l);
      else err("E0703", l.toks[0].loc, {k, "the top level"});
    }
    if (!defaultConfig.empty()) {
      bool found = false;
      for (auto& c : out.configs) if (c.name == defaultConfig[0].first) found = true;
      if (!found) err("E0719", defaultConfig[0].second, {"default_config", "no `config " + defaultConfig[0].first + "` is defined"});
      else out.defaultConfig = defaultConfig[0].first;
    }
    if (!out.hasProject && ok) err("E0702", Loc{fid, 1, 1}, {"missing `project \"Name\"`"});
    // the same name must not be both a target and a dependency: `depends` could not tell them apart
    for (auto& t : out.targets)
      for (auto& dep : out.deps)
        if (t.name == dep.name) err("E0705", t.nameLoc, {t.name});  // `depends` could not tell the target and the dependency apart
    if (ok && !baseDir.empty()) {
      for (auto& t : out.targets)
        for (size_t i = 0; i < t.sources.size(); i++) {
          std::error_code ec;
          if (!fs::is_regular_file(fs::path(baseDir) / t.sources[i], ec)) err("E0704", t.sourceLocs[i], {t.sources[i]});
        }
    }
  }
};

void jstr(std::string& o, const std::string& s) {
  o += '"';
  for (unsigned char c : s) {
    if (c == '"') o += "\\\"";
    else if (c == '\\') o += "\\\\";
    else if (c == '\n') o += "\\n";
    else if (c == '\t') o += "\\t";
    else if (c < 0x20 || c == 0x7F) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
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

bool parseBuildFile(DiagEngine& d, int fileId, const std::string& baseDir, BuildFile& out) {
  out = BuildFile();
  std::vector<Line> top;
  Lexer lx{d, fileId};
  if (!lx.run(d.files[fileId]->text, top)) return false;
  Analyzer a{d, fileId, baseDir, out};
  a.run(top);
  return a.ok;
}

std::string buildFileToJson(const BuildFile& b) {
  std::string o = "{\"project\":{\"name\":";
  jstr(o, b.projectName);
  if (!b.version.empty()) { o += ",\"version\":"; jstr(o, b.version); }
  if (!b.license.empty()) { o += ",\"license\":"; jstr(o, b.license); }
  if (!b.description.empty()) { o += ",\"description\":"; jstr(o, b.description); }
  o += "},\"registries\":";
  jlist(o, b.registries);
  o += ",\"dependencies\":[";
  for (size_t i = 0; i < b.deps.size(); i++) {
    if (i) o += ',';
    o += "{\"name\":"; jstr(o, b.deps[i].name);
    o += b.deps[i].isPath ? ",\"path\":" : ",\"version\":";
    jstr(o, b.deps[i].isPath ? b.deps[i].path : b.deps[i].req);
    o += '}';
  }
  o += "],\"configs\":[";
  for (size_t i = 0; i < b.configs.size(); i++) {
    const BuildConfig& c = b.configs[i];
    if (i) o += ',';
    o += "{\"name\":"; jstr(o, c.name);
    if (c.mode) { o += ",\"mode\":"; jstr(o, *c.mode); }
    if (c.optimization) { o += ",\"optimization\":"; jstr(o, *c.optimization); }
    if (c.boundsCheck) o += std::string(",\"bounds_check\":") + (*c.boundsCheck ? "true" : "false");
    if (c.debugAlloc) o += std::string(",\"debug_alloc\":") + (*c.debugAlloc ? "true" : "false");
    if (c.overflowCheck) o += std::string(",\"overflow_check\":") + (*c.overflowCheck ? "true" : "false");
    o += '}';
  }
  o += ']';
  if (!b.defaultConfig.empty()) { o += ",\"default_config\":"; jstr(o, b.defaultConfig); }
  o += ",\"targets\":[";
  for (size_t i = 0; i < b.targets.size(); i++) {
    const BuildTarget& t = b.targets[i];
    if (i) o += ',';
    o += "{\"kind\":"; jstr(o, t.kind == BuildTarget::Executable ? "executable" : "library");
    o += ",\"name\":"; jstr(o, t.name);
    o += ",\"sources\":"; jlist(o, t.sources);
    o += ",\"include_dirs\":"; jlist(o, t.includeDirs);
    o += ",\"defines\":"; jlist(o, t.defines);
    o += ",\"depends\":"; jlist(o, t.depends);
    if (t.output) { o += ",\"output\":"; jstr(o, *t.output); }
    if (t.platform) { o += ",\"platform\":"; jstr(o, *t.platform); }
    if (t.link) { o += ",\"link\":"; jstr(o, *t.link); }
    if (t.std) { o += ",\"std\":"; jstr(o, *t.std); }
    if (t.mode) { o += ",\"mode\":"; jstr(o, *t.mode); }
    if (t.optimization) { o += ",\"optimization\":"; jstr(o, *t.optimization); }
    if (t.boundsCheck) o += std::string(",\"bounds_check\":") + (*t.boundsCheck ? "true" : "false");
    if (t.debugAlloc) o += std::string(",\"debug_alloc\":") + (*t.debugAlloc ? "true" : "false");
    if (t.overflowCheck) o += std::string(",\"overflow_check\":") + (*t.overflowCheck ? "true" : "false");
    o += '}';
  }
  o += "]}";
  return o;
}

}  // namespace c7pkg
