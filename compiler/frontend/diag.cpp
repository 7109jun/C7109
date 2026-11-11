#include <algorithm>
#include <map>
#include <ostream>

#include "c7.hpp"

namespace c7 {
namespace {
struct Msg {
  const char *en, *ko;
};

// Registry: diag_codes.def. Duplicate codes are a compile error.
struct Entry { const char *code, *en, *ko; };
#ifndef C7_DIAG_DEF
#define C7_DIAG_DEF "diag_codes.def"
#endif
constexpr Entry kEntries[] = {
#define DIAG(code, en, ko) {code, en, ko},
#include C7_DIAG_DEF
#undef DIAG
};
constexpr bool sameStr(const char* a, const char* b) {
  while (*a && *a == *b) { a++; b++; }
  return *a == *b;
}
constexpr bool noDuplicateCodes() {
  for (size_t i = 0; i < sizeof(kEntries) / sizeof(kEntries[0]); i++)
    for (size_t j = i + 1; j < sizeof(kEntries) / sizeof(kEntries[0]); j++)
      if (sameStr(kEntries[i].code, kEntries[j].code)) return false;
  return true;
}
static_assert(noDuplicateCodes(), "duplicate diagnostic code in diag_codes.def");

const std::map<std::string, Msg>& table() {
  static const std::map<std::string, Msg> t = [] {
    std::map<std::string, Msg> m;
    for (const Entry& e : kEntries) m[e.code] = Msg{e.en, e.ko};
    return m;
  }();
  return t;
}

std::string subst(std::string s, const std::vector<std::string>& args) {
  size_t k = 0, pos = 0;
  while ((pos = s.find("{}", pos)) != std::string::npos) {
    std::string a = k < args.size() ? args[k] : "?";
    k++;
    s.replace(pos, 2, a);
    pos += a.size();
  }
  return s;
}

std::string jsonEscape(const std::string& s) {
  std::string o;
  for (unsigned char c : s) {
    if (c == '"') o += "\\\"";
    else if (c == '\\') o += "\\\\";
    else if (c == '\n') o += "\\n";
    else if (c < 0x20) o += ' ';
    else o += c;
  }
  return o;
}
}  // namespace

int DiagEngine::addFile(std::string path, std::string text) {
  auto f = std::make_unique<SourceFile>();
  f->path = std::move(path);
  f->text = std::move(text);
  files.push_back(std::move(f));
  return (int)files.size() - 1;
}

void DiagEngine::report(const std::string& code, Loc loc, const std::vector<std::string>& args) {
  auto it = table().find(code);
  Diag g;
  g.code = code;
  g.sev = code[0] == 'W' ? Sev::Warning : Sev::Error;
  g.loc = loc;
  if (it == table().end()) {
    g.en = g.ko = "internal: unknown diagnostic code";
  } else {
    g.en = subst(it->second.en, args);
    g.ko = subst(it->second.ko, args);
  }
  diags.push_back(std::move(g));
}

int DiagEngine::errorCount() const {
  int n = 0;
  for (auto& g : diags) n += g.sev == Sev::Error;
  return n;
}

void DiagEngine::print(std::ostream& os, bool json) const {
  std::vector<const Diag*> sorted;  // report in source order regardless of analysis phase
  for (auto& g : diags) sorted.push_back(&g);
  std::stable_sort(sorted.begin(), sorted.end(), [](const Diag* a, const Diag* b) {
    if (a->loc.file != b->loc.file) return a->loc.file < b->loc.file;
    if (a->loc.line != b->loc.line) return a->loc.line < b->loc.line;
    return a->loc.col < b->loc.col;
  });
  if (json) {
    os << "[";
    for (size_t i = 0; i < sorted.size(); i++) {
      auto& g = *sorted[i];
      std::string path = g.loc.file >= 0 ? files[g.loc.file]->path : "";
      os << (i ? "," : "") << "\n  {\"code\":\"" << g.code << "\",\"severity\":\""
         << (g.sev == Sev::Error ? "error" : "warning") << "\",\"file\":\"" << jsonEscape(path)
         << "\",\"line\":" << g.loc.line << ",\"column\":" << g.loc.col << ",\"message\":\""
         << jsonEscape(lang == "ko" ? g.ko : g.en) << "\"}";
    }
    os << (diags.empty() ? "]\n" : "\n]\n");
    return;
  }
  for (const Diag* gp : sorted) {
    const Diag& g = *gp;
    std::string path = g.loc.file >= 0 ? files[g.loc.file]->path : "<unknown>";
    os << path << ":" << g.loc.line << ":" << g.loc.col << ": "
       << (g.sev == Sev::Error ? "error" : "warning") << "[" << g.code
       << "]: " << (lang == "ko" ? g.ko : g.en) << "\n";
  }
}

}  // namespace c7
