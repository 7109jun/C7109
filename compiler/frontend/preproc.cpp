// Preprocessor (spec 14.2). Implemented: #include #define #undef #if/#ifdef/#ifndef/#elif/
// #elifdef/#elifndef/#else/#endif #error #warning #pragma once #line(ignored), defined,
// __has_include, object- and function-like macros (+ __VA_ARGS__, __VA_OPT__), the '#' (stringize) and '##' (paste)
// operators (C11 6.10.5, C23 for __VA_OPT__), __FILE__/__LINE__.
// Not yet implemented (reported, never silently wrong): #embed.
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "c7.hpp"

namespace c7 {
namespace fs = std::filesystem;
namespace {

struct Macro {
  bool func = false, variadic = false;
  std::vector<std::string> params;
  std::vector<Token> body;
};
struct Cond {
  bool parentActive, active, taken, sawElse;
  Loc loc;
};

Token intTok(int64_t v, Loc l) {
  Token t;
  t.kind = TK::Int; t.text = std::to_string(v); t.ival = (uint64_t)v; t.loc = l;
  return t;
}

class PP {
 public:
  PP(DiagEngine& d, const PPOptions& o) : d(d), o(o) {}
  std::vector<Token> out;
  void file(int fid);
  void defineText(const std::string& name, const std::string& value);

 private:
  DiagEngine& d;
  const PPOptions& o;
  std::map<std::string, Macro> macros;
  std::set<std::string> once;
  int depth = 0;

  std::string rangeText(const std::vector<Token>& v, size_t a, size_t b, int fid) {
    if (a >= b) return "";
    const std::string& s = d.files[fid]->text;
    return s.substr(v[a].off, v[b - 1].off + v[b - 1].len - v[a].off);
  }
  const Token& at(const std::vector<Token>& v, size_t i) {
    static Token eof;
    return i < v.size() ? v[i] : eof;
  }
  void directive(int fid, const Token& hash, std::vector<Token>& line, std::vector<Cond>& conds);
  bool evalCond(std::vector<Token> line, size_t from, int fid, Loc loc);
  bool findInclude(const std::string& name, bool angle, const std::string& fromPath, std::string& found);
  std::vector<Token> expandAt(std::vector<Token>& v, size_t& i, const std::set<std::string>& hide);
  std::vector<Token> expandList(std::vector<Token> v, const std::set<std::string>& hide);
  std::vector<Token> substitute(const Macro& m, const std::vector<Token>& seq, const std::vector<std::vector<Token>>& args,
                                const std::set<std::string>& hide, Loc at, bool top);
  Token stringize(const std::vector<Token>& arg, Loc at);
  bool pasteTokens(const Token& l, const Token& r, Loc at, std::vector<Token>& out);
};

// `##` is lexed as two adjacent `#` tokens.
bool isPasteAt(const std::vector<Token>& v, size_t i) {
  return i + 1 < v.size() && v[i].kind == TK::Punct && v[i].text == "#" && v[i + 1].kind == TK::Punct && v[i + 1].text == "#" && !v[i + 1].space;
}
Token marker(const char* text, Loc at) {  // internal tokens: "\x01" placemarker, "\x02" paste operator
  Token t; t.kind = TK::Punct; t.text = text; t.loc = at;
  return t;
}
bool isMarker(const Token& t, const char* text) { return t.kind == TK::Punct && t.text == text; }

void PP::defineText(const std::string& name, const std::string& value) {
  int fid = d.addFile("<predefined>", "#define " + name + " " + value + "\n");
  file(fid);
}

bool PP::findInclude(const std::string& name, bool angle, const std::string& fromPath, std::string& found) {
  std::vector<fs::path> cands;
  if (!angle) cands.push_back(fs::path(fromPath).parent_path() / name);
  for (auto& dir : o.includeDirs) cands.push_back(fs::path(dir) / name);
  for (auto& c : cands)
    if (fs::is_regular_file(c)) { found = c.string(); return true; }
  return false;
}

std::vector<Token> PP::expandList(std::vector<Token> v, const std::set<std::string>& hide) {
  std::vector<Token> res;
  size_t i = 0;
  while (i < v.size()) {
    if (v[i].kind == TK::Ident && macros.count(v[i].text) && !hide.count(v[i].text)) {
      auto r = expandAt(v, i, hide);
      res.insert(res.end(), r.begin(), r.end());
    } else {
      res.push_back(v[i]);
      i++;
    }
  }
  return res;
}

// `#param`: the unexpanded argument spelled as one string literal. A token keeps one space if it had whitespace before it;
// `"` and `\` inside string/character literals are escaped (C11 6.10.3.2).
Token PP::stringize(const std::vector<Token>& arg, Loc at) {
  std::string value, text;
  for (size_t i = 0; i < arg.size(); i++) {
    const Token& t = arg[i];
    if (i > 0 && (t.space || t.bol)) { value += ' '; text += ' '; }
    value += t.text;
    bool lit = t.kind == TK::Str || t.kind == TK::Char || t.kind == TK::RawStr || t.kind == TK::CStr;
    for (char c : t.text) { if (lit && (c == '"' || c == '\\')) text += '\\'; text += c; }
  }
  Token r;
  r.kind = TK::Str; r.text = "\"" + text + "\""; r.sval = value; r.loc = at;
  return r;
}

// `l ## r`: the concatenated spelling must lex as exactly one token. A placemarker (empty argument) yields the other side.
bool PP::pasteTokens(const Token& l, const Token& r, Loc at, std::vector<Token>& out) {
  if (isMarker(l, "\x01")) { out.push_back(r); return true; }
  if (isMarker(r, "\x01")) { out.push_back(l); return true; }
  DiagEngine scratch;  // lexing errors here mean "not a valid token", not a diagnostic of the user's file
  int f = scratch.addFile("<paste>", l.text + r.text);
  std::vector<Token> toks = lex(scratch, f);
  if (!toks.empty() && toks.back().kind == TK::Eof) toks.pop_back();
  bool two = toks.size() == 2 && toks[0].kind == TK::Punct && toks[0].text == "#" && toks[1].kind == TK::Punct && toks[1].text == "#";
  if (scratch.errorCount() != 0 || !(toks.size() == 1 || two)) {
    d.report("E0614", at, {l.text, r.text});
    out.push_back(l); out.push_back(r);
    return false;
  }
  for (Token& t : toks) { t.loc = at; t.bol = false; t.space = false; t.off = 0; t.len = 0; }
  toks[0].space = l.space;
  out.insert(out.end(), toks.begin(), toks.end());
  return true;
}

// Builds a macro's replacement before rescanning: parameters are replaced (unexpanded next to `##`, fully expanded
// elsewhere), `#param` is stringized, `__VA_OPT__(...)` keeps its content only when variadic arguments were given, and
// finally every `##` pastes its neighbours (C11 6.10.3).
std::vector<Token> PP::substitute(const Macro& m, const std::vector<Token>& seq, const std::vector<std::vector<Token>>& args,
                                  const std::set<std::string>& hide, Loc at, bool top) {
  auto paramIdx = [&](const Token& t) -> int {
    if (t.kind != TK::Ident) return -1;
    for (size_t k = 0; k < m.params.size(); k++) if (m.params[k] == t.text) return (int)k;
    return -1;
  };
  std::vector<Token> out;
  for (size_t i = 0; i < seq.size(); i++) {
    const Token& bt = seq[i];
    if (isPasteAt(seq, i)) { out.push_back(marker("\x02", at)); i++; continue; }
    if (m.func && bt.kind == TK::Punct && bt.text == "#" && i + 1 < seq.size()) {
      int k = paramIdx(seq[i + 1]);
      if (k >= 0) { out.push_back(stringize(args[k], at)); i++; continue; }
    }
    if (m.variadic && bt.kind == TK::Ident && bt.text == "__VA_OPT__" && i + 1 < seq.size() && seq[i + 1].kind == TK::Punct && seq[i + 1].text == "(") {
      size_t j = i + 2; int depth = 1;
      for (; j < seq.size(); j++) {
        if (seq[j].kind == TK::Punct && seq[j].text == "(") depth++;
        else if (seq[j].kind == TK::Punct && seq[j].text == ")" && --depth == 0) break;
      }
      if (depth != 0) { d.report("E0606", at, {"missing `)` after `__VA_OPT__(`"}); break; }
      if (!args.back().empty()) {
        std::vector<Token> content(seq.begin() + i + 2, seq.begin() + j);
        std::vector<Token> sub = substitute(m, content, args, hide, at, false);
        out.insert(out.end(), sub.begin(), sub.end());
      } else out.push_back(marker("\x01", at));
      i = j;
      continue;
    }
    int k = paramIdx(bt);
    if (k >= 0) {
      bool pasteOperand = (i >= 2 && isPasteAt(seq, i - 2)) || isPasteAt(seq, i + 1);
      if (pasteOperand) {
        if (args[k].empty()) out.push_back(marker("\x01", at));
        else out.insert(out.end(), args[k].begin(), args[k].end());
      } else {
        auto ex = expandList(args[k], hide);
        out.insert(out.end(), ex.begin(), ex.end());
      }
      continue;
    }
    Token c = bt;
    c.loc = at; c.bol = false;
    out.push_back(c);
  }
  if (!top) return out;  // pastes may straddle a __VA_OPT__ boundary: finish at the outermost level
  std::vector<Token> res;
  for (size_t i = 0; i < out.size(); i++) {
    if (isMarker(out[i], "\x02")) {
      if (res.empty() || i + 1 >= out.size()) continue;
      Token l = res.back(); res.pop_back();
      pasteTokens(l, out[++i], at, res);
    } else res.push_back(out[i]);
  }
  std::vector<Token> fin;
  for (auto& t : res) if (!isMarker(t, "\x01")) fin.push_back(t);
  return fin;
}

std::vector<Token> PP::expandAt(std::vector<Token>& v, size_t& i, const std::set<std::string>& hide) {
  Token name = v[i];
  const Macro m = macros[name.text];
  std::vector<std::vector<Token>> args;
  if (m.func) {
    size_t j = i + 1;
    if (!(at(v, j).kind == TK::Punct && at(v, j).text == "(")) { i++; return {name}; }
    int depth = 0;
    std::vector<Token> cur;
    bool closed = false;
    for (; j < v.size(); j++) {
      const Token& t = v[j];
      if (t.kind == TK::Eof) break;
      if (t.kind == TK::Punct && t.text == "(") { if (depth++ > 0) cur.push_back(t); continue; }
      if (t.kind == TK::Punct && t.text == ")") {
        if (--depth == 0) { closed = true; j++; break; }
        cur.push_back(t); continue;
      }
      if (depth == 1 && t.kind == TK::Punct && t.text == "," &&
          !(m.variadic && args.size() + 1 >= m.params.size() && !m.params.empty())) {
        args.push_back(cur); cur.clear(); continue;
      }
      cur.push_back(t);
    }
    if (!closed) { d.report("E0607", name.loc, {name.text}); i = v.size() ? v.size() : 0; return {}; }
    args.push_back(cur);
    if (m.params.empty() && args.size() == 1 && args[0].empty()) args.clear();
    size_t want = m.params.size();
    if (m.variadic && args.size() + 1 == want) args.push_back({});  // empty variadic part
    if (args.size() != want) { d.report("E0607", name.loc, {name.text}); i = j; return {}; }
    i = j;
  } else {
    i++;
  }
  std::vector<Token> body = substitute(m, m.body, args, hide, name.loc, true);
  std::set<std::string> inner = hide;
  inner.insert(name.text);
  return expandList(std::move(body), inner);
}

// ---- #if expression evaluation ----------------------------------------------------------
struct Eval {
  const std::vector<Token>& t;
  size_t p = 0;
  bool err = false;
  std::string why;
  explicit Eval(const std::vector<Token>& tt) : t(tt) {}
  const Token& cur() { static Token eof; return p < t.size() ? t[p] : eof; }
  bool isP(const char* s) { return cur().kind == TK::Punct && cur().text == s; }
  void fail(const std::string& w) { if (!err) { err = true; why = w; } }
  int64_t primary() {
    const Token& k = cur();
    if (k.kind == TK::Int || k.kind == TK::Char) { p++; return (int64_t)k.ival; }
    if (k.kind == TK::Keyword && (k.text == "true" || k.text == "false")) { p++; return k.text == "true"; }
    if (k.kind == TK::Ident || k.kind == TK::Keyword) { p++; return 0; }
    if (isP("(")) {
      p++; int64_t v = ternary();
      if (isP(")")) p++; else fail("expected `)`");
      return v;
    }
    fail("unexpected token `" + k.text + "`");
    p++;
    return 0;
  }
  int64_t unary() {
    if (isP("!")) { p++; return !unary(); }
    if (isP("~")) { p++; return ~unary(); }
    if (isP("-")) { p++; return -unary(); }
    if (isP("+")) { p++; return unary(); }
    return primary();
  }
  bool shiftRight() {  // '>' '>' adjacent
    return isP(">") && p + 1 < t.size() && t[p + 1].kind == TK::Punct && t[p + 1].text == ">" &&
           t[p + 1].off == cur().off + 1;
  }
  int64_t bin(int prec) {
    int64_t l = unary();
    for (;;) {
      std::string op; int pr = 0, n = 1;
      if (cur().kind != TK::Punct) break;
      const std::string& s = cur().text;
      if (shiftRight()) { op = ">>"; pr = 8; n = 2; }
      else if (s == "*" || s == "/" || s == "%") pr = 10, op = s;
      else if (s == "+" || s == "-") pr = 9, op = s;
      else if (s == "<<") pr = 8, op = s;
      else if (s == "<" || s == "<=" || s == ">" || s == ">=") pr = 4, op = s;
      else if (s == "==" || s == "!=") pr = 3, op = s;
      else if (s == "&") pr = 7, op = s;
      else if (s == "^") pr = 6, op = s;
      else if (s == "|") pr = 5, op = s;
      else if (s == "&&") pr = 2, op = s;
      else if (s == "||") pr = 1, op = s;
      else break;
      if (pr < prec) break;
      p += n;
      int64_t r = bin(pr + 1);
      if (op == "*") l = l * r;
      else if (op == "/") { if (!r) { fail("division by zero"); r = 1; } l = l / r; }
      else if (op == "%") { if (!r) { fail("division by zero"); r = 1; } l = l % r; }
      else if (op == "+") l = l + r; else if (op == "-") l = l - r;
      else if (op == "<<") l = (int64_t)((uint64_t)l << (r & 63));
      else if (op == ">>") l = l >> (r & 63);
      else if (op == "<") l = l < r; else if (op == "<=") l = l <= r;
      else if (op == ">") l = l > r; else if (op == ">=") l = l >= r;
      else if (op == "==") l = l == r; else if (op == "!=") l = l != r;
      else if (op == "&") l = l & r; else if (op == "^") l = l ^ r; else if (op == "|") l = l | r;
      else if (op == "&&") l = l && r; else if (op == "||") l = l || r;
    }
    return l;
  }
  int64_t ternary() {
    int64_t c = bin(1);
    if (isP("?")) {
      p++; int64_t a = ternary();
      if (isP(":")) p++; else fail("expected `:`");
      int64_t b = ternary();
      return c ? a : b;
    }
    return c;
  }
};

bool PP::evalCond(std::vector<Token> line, size_t from, int fid, Loc loc) {
  std::vector<Token> v;
  for (size_t i = from; i < line.size(); i++) {
    const Token& t = line[i];
    if (t.kind == TK::Ident && t.text == "defined") {
      bool paren = at(line, i + 1).kind == TK::Punct && at(line, i + 1).text == "(";
      size_t k = i + (paren ? 2 : 1);
      std::string nm = at(line, k).text;
      v.push_back(intTok(macros.count(nm) ? 1 : 0, loc));
      i = k + (paren ? 1 : 0);
      continue;
    }
    if (t.kind == TK::Ident && t.text == "__has_include") {
      size_t k = i + 2;  // after '('
      std::string nm; bool angle = false;
      if (at(line, k).kind == TK::Str) { nm = line[k].sval; k++; }
      else if (at(line, k).kind == TK::Punct && at(line, k).text == "<") {
        size_t e = k + 1;
        while (e < line.size() && !(line[e].kind == TK::Punct && line[e].text == ">")) e++;
        nm = rangeText(line, k + 1, e, fid); angle = true; k = e + 1;
      }
      std::string found;
      v.push_back(intTok(findInclude(nm, angle, d.files[fid]->path, found) ? 1 : 0, loc));
      i = k;  // at ')'
      continue;
    }
    v.push_back(t);
  }
  v = expandList(std::move(v), {});
  Eval ev(v);
  int64_t r = ev.ternary();
  if (!ev.err && ev.p < v.size()) ev.fail("unexpected token `" + v[ev.p].text + "`");
  if (ev.err) { d.report("E0608", loc, {ev.why}); return false; }
  return r != 0;
}

void PP::directive(int fid, const Token& hash, std::vector<Token>& line, std::vector<Cond>& conds) {
  if (line.empty()) return;
  const std::string name = line[0].text;
  bool act = conds.empty() || conds.back().active;
  auto macroDefined = [&](size_t idx) {
    if (idx >= line.size() || (line[idx].kind != TK::Ident)) { d.report("E0606", hash.loc, {"macro name expected"}); return false; }
    return macros.count(line[idx].text) > 0;
  };

  if (name == "if" || name == "ifdef" || name == "ifndef") {
    Cond c{act, false, false, false, hash.loc};
    bool v = false;
    if (act) {
      if (name == "if") v = evalCond(line, 1, fid, hash.loc);
      else v = macroDefined(1) == (name == "ifdef");
    }
    c.active = act && v; c.taken = c.active;
    conds.push_back(c);
    return;
  }
  if (name == "elif" || name == "elifdef" || name == "elifndef") {
    if (conds.empty()) { d.report("E0605", hash.loc, {name}); return; }
    Cond& c = conds.back();
    if (c.sawElse) { d.report("E0605", hash.loc, {name}); return; }
    if (!c.parentActive || c.taken) c.active = false;
    else {
      bool v = name == "elif" ? evalCond(line, 1, fid, hash.loc) : (macroDefined(1) == (name == "elifdef"));
      c.active = v; if (v) c.taken = true;
    }
    return;
  }
  if (name == "else") {
    if (conds.empty() || conds.back().sawElse) { d.report("E0605", hash.loc, {"else"}); return; }
    Cond& c = conds.back();
    c.sawElse = true; c.active = c.parentActive && !c.taken; if (c.active) c.taken = true;
    return;
  }
  if (name == "endif") {
    if (conds.empty()) d.report("E0605", hash.loc, {"endif"}); else conds.pop_back();
    return;
  }
  if (!act) return;

  if (name == "define") {
    if (line.size() < 2 || line[1].kind != TK::Ident) { d.report("E0606", hash.loc, {"macro name expected"}); return; }
    Macro m;
    size_t b = 2;
    if (line.size() > 2 && line[2].kind == TK::Punct && line[2].text == "(" && !line[2].space) {
      m.func = true;
      size_t k = 3;
      bool closed = false;
      while (k < line.size()) {
        if (line[k].kind == TK::Punct && line[k].text == ")") { closed = true; k++; break; }
        if (line[k].kind == TK::Ident) m.params.push_back(line[k].text);
        else if (line[k].kind == TK::Punct && line[k].text == "...") { m.variadic = true; m.params.push_back("__VA_ARGS__"); }
        else if (!(line[k].kind == TK::Punct && line[k].text == ",")) { d.report("E0606", hash.loc, {"bad parameter list"}); return; }
        k++;
      }
      if (!closed) { d.report("E0606", hash.loc, {"missing `)` in parameter list"}); return; }
      b = k;
    }
    m.body.assign(line.begin() + b, line.end());
    for (size_t j = 0; j < m.body.size(); j++) {  // C11 6.10.3.2/6.10.3.3 constraints
      if (isPasteAt(m.body, j)) {
        if (j == 0 || j + 2 >= m.body.size()) { d.report("E0606", hash.loc, {"`##` cannot appear at either end of a macro expansion"}); return; }
        j++;
      } else if (m.func && m.body[j].kind == TK::Punct && m.body[j].text == "#") {
        bool isParam = false;
        if (j + 1 < m.body.size() && m.body[j + 1].kind == TK::Ident)
          for (auto& pn : m.params) if (pn == m.body[j + 1].text) isParam = true;
        if (!isParam) { d.report("E0606", hash.loc, {"`#` is not followed by a macro parameter"}); return; }
      }
    }
    macros[line[1].text] = std::move(m);
    return;
  }
  if (name == "undef") {
    if (line.size() >= 2) macros.erase(line[1].text);
    return;
  }
  if (name == "include") {
    std::string nm; bool angle = false;
    if (line.size() >= 2 && line[1].kind == TK::Str) nm = line[1].sval;
    else if (line.size() >= 2 && line[1].kind == TK::Punct && line[1].text == "<") {
      size_t e = 2;
      while (e < line.size() && !(line[e].kind == TK::Punct && line[e].text == ">")) e++;
      nm = rangeText(line, 2, e, fid); angle = true;
    } else { d.report("E0606", hash.loc, {"expected \"file\" or <file>"}); return; }
    std::string found;
    if (!findInclude(nm, angle, d.files[fid]->path, found)) { d.report("E0603", hash.loc, {nm}); return; }
    std::error_code ec;
    std::string canon = fs::weakly_canonical(found, ec).string();
    if (once.count(canon)) return;
    if (depth >= 64) { d.report("E0613", hash.loc); return; }
    std::ifstream in(found, std::ios::binary);
    std::stringstream ss; ss << in.rdbuf();
    int nf = d.addFile(found, ss.str());
    depth++; file(nf); depth--;
    return;
  }
  if (name == "error" || name == "warning") {
    d.report(name == "error" ? "E0601" : "W0601", hash.loc, {rangeText(line, 1, line.size(), fid)});
    return;
  }
  if (name == "pragma") {
    if (line.size() >= 2 && line[1].text == "once") {
      std::error_code ec;
      once.insert(fs::weakly_canonical(d.files[fid]->path, ec).string());
    }
    return;
  }
  if (name == "line") return;
  if (name == "embed") { d.report("E0609", hash.loc, {"embed"}); return; }
  d.report("E0602", hash.loc, {name});
}

void PP::file(int fid) {
  std::vector<Token> toks = lex(d, fid);
  std::vector<Cond> conds;
  size_t i = 0;
  while (toks[i].kind != TK::Eof) {
    const Token& t = toks[i];
    if (t.kind == TK::Punct && t.text == "#" && t.bol) {
      size_t j = i + 1;
      while (toks[j].kind != TK::Eof && !toks[j].bol) j++;
      std::vector<Token> line(toks.begin() + i + 1, toks.begin() + j);
      Token hash = t;
      directive(fid, hash, line, conds);
      i = j;
      continue;
    }
    if (!(conds.empty() || conds.back().active)) { i++; continue; }
    if (t.kind == TK::Ident && t.text == "__LINE__") {
      out.push_back(intTok(t.loc.line, t.loc)); i++; continue;
    }
    if (t.kind == TK::Ident && t.text == "__FILE__") {
      Token s = t; s.kind = TK::Str; s.sval = d.files[fid]->path; s.text = "\"" + s.sval + "\"";
      out.push_back(s); i++; continue;
    }
    if (t.kind == TK::Ident && macros.count(t.text)) {
      auto r = expandAt(toks, i, {});
      out.insert(out.end(), r.begin(), r.end());
      continue;
    }
    out.push_back(t);
    i++;
  }
  if (!conds.empty()) d.report("E0604", conds.back().loc, {"if"});
}

}  // namespace

std::vector<Token> preprocess(DiagEngine& d, int fid, const PPOptions& o) {
  PP pp(d, o);
  std::string osn = o.os == "windows" ? "WINDOWS" : "LINUX";
  std::string arch = o.arch == "aarch64" ? "AARCH64" : o.arch == "x86" ? "X86" : "X86_64";
  pp.defineText("__C7109__", "1");
  pp.defineText("__C7109_VERSION__", "10000");
  pp.defineText("__C7_OS_" + osn + "__", "1");
  pp.defineText("__C7_ARCH_" + arch + "__", "1");
  if (o.freestanding) pp.defineText("__C7_FREESTANDING__", "1");
  if (o.debug) pp.defineText("__C7_DEBUG__", "1");
  for (auto& def : o.defines) {
    auto eq = def.find('=');
    pp.defineText(eq == std::string::npos ? def : def.substr(0, eq),
                  eq == std::string::npos ? "1" : def.substr(eq + 1));
  }
  pp.file(fid);
  Token e;
  e.kind = TK::Eof; e.bol = true; e.loc = Loc{fid, 1, 1};
  if (!pp.out.empty()) e.loc = pp.out.back().loc;
  pp.out.push_back(e);
  return std::move(pp.out);
}

}  // namespace c7
