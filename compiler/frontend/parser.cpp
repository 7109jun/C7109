#include <algorithm>
#include <map>
#include <ostream>

#include "c7.hpp"

namespace c7 {

const char* nkName(NK k) {
  switch (k) {
#define X(n) case NK::n: return #n;
    C7_NK_LIST(X)
#undef X
  }
  return "?";
}

void dump(const Node& n, std::ostream& os, int depth) {
  for (int i = 0; i < depth; i++) os << "  ";
  os << nkName(n.kind);
  if (!n.text.empty()) os << " `" << n.text << "`";
  if (!n.mods.empty()) os << " [" << n.mods << "]";
  os << "\n";
  for (auto& k : n.kids) dump(*k, os, depth + 1);
}

namespace {

struct SpecAbort {};

bool isPrimType(const std::string& s) {
  static const char* p[] = {"i8", "i16", "i32", "i64", "i128", "u8", "u16", "u32", "u64", "u128",
                            "isize", "usize", "f32", "f64", "bool", "Char", "int", "long", "void"};
  for (auto x : p) if (s == x) return true;
  return false;
}

class Parser {
 public:
  Parser(DiagEngine& d, std::vector<Token> toks) : d(d), t(std::move(toks)) {}
  NodeP unit();

 private:
  DiagEngine& d;
  std::vector<Token> t;
  size_t p = 0;
  int spec = 0, errs = 0;

  // ---- token helpers -----------------------------------------------------------------
  const Token& cur() const { return t[p]; }
  const Token& peek(size_t k = 1) const { return t[std::min(p + k, t.size() - 1)]; }
  static bool isTok(const Token& k, const char* s) {
    return (k.kind == TK::Punct || k.kind == TK::Keyword) && k.text == s;
  }
  bool is(const char* s) const { return isTok(cur(), s); }
  bool isAt(size_t k, const char* s) const { return isTok(peek(k), s); }
  bool isIdent() const { return cur().kind == TK::Ident; }
  void advance() { if (cur().kind != TK::Eof) p++; }
  bool accept(const char* s) { if (is(s)) { p++; return true; } return false; }
  std::string desc(const Token& k) const { return k.kind == TK::Eof ? "<eof>" : k.text; }

  void err(const char* code, std::vector<std::string> args) {
    if (spec) throw SpecAbort{};
    if (errs == 50) { d.report("E0199", cur().loc); errs++; }
    if (errs < 50) d.report(code, cur().loc, args);
    errs++;
  }
  bool expect(const char* s) {
    if (accept(s)) return true;
    err("E0101", {std::string("`") + s + "`", desc(cur())});
    return false;
  }
  std::string ident() {
    if (cur().kind == TK::Ident) { std::string s = cur().text; advance(); return s; }
    err("E0105", {desc(cur())});
    return "";
  }
  NodeP mk(NK k, std::string text = "", Loc l = Loc{}) {
    auto n = std::make_unique<Node>();
    n->kind = k; n->text = std::move(text);
    n->loc = l.file >= 0 ? l : cur().loc;
    return n;
  }
  NodeP empty() { return mk(NK::Empty); }
  template <class F> bool lookahead(F f) {  // speculative parse; always restores position
    size_t save = p;
    spec++;
    bool ok = true;
    try { f(); } catch (SpecAbort&) { ok = false; }
    spec--;
    p = save;
    return ok;
  }
  template <class F> NodeP trySpec(F f) {  // speculative parse keeping the result on success
    size_t save = p;
    spec++;
    try { auto r = f(); spec--; return r; } catch (SpecAbort&) { spec--; p = save; return nullptr; }
  }

  // ---- declarations --------------------------------------------------------------------
  NodeP parseNamespace();
  NodeP parseImport();
  NodeP parseAttr();
  NodeP parseItem(bool member);
  NodeP parseGenericParams();
  NodeP parseTypeArgs();
  void parseParams(Node& owner);
  NodeP parseParam();
  std::string parseDeclName();
  NodeP parseEnum(const std::string& mods);
  NodeP parseTagged();
  void parseBody(Node& n, bool member);
  void recoverTop();

  // ---- types -----------------------------------------------------------------------------
  NodeP parseType();
  NodeP parseBaseType();

  // ---- statements --------------------------------------------------------------------------
  NodeP parseBlock();
  NodeP parseStmt();
  bool looksLikeDecl();
  NodeP parseLocalDecl(bool needSemi);
  NodeP parseSimple(bool needSemi);
  NodeP parseFor();
  NodeP parseSwitch();
  NodeP parseMatch();
  NodeP parsePattern();
  NodeP parseCond();
  void sync();

  // ---- expressions ---------------------------------------------------------------------------
  NodeP parseExpr() { return parseTernary(); }
  NodeP parseTernary();
  NodeP parseRange();
  NodeP parseBinary(int minPrec);
  NodeP parseCast();
  NodeP parseUnary();
  NodeP parsePostfix();
  NodeP parsePrimary();
  NodeP parsePathExpr();
  NodeP parseLambda();
  NodeP parseAlloc();
  bool binOp(std::string& op, int& prec, int& ntok);
  bool assignOp(std::string& op, int& ntok);
  bool adjacent(size_t a, size_t b) const {
    return t[a].kind == TK::Punct && t[b].kind == TK::Punct && t[a].loc.file == t[b].loc.file &&
           t[b].off == t[a].off + t[a].len;
  }
  bool atBoundary() const { return p > 0 && (isTok(t[p - 1], ";") || isTok(t[p - 1], "}")); }
  bool startsExpr() const {
    return !(is(")") || is("]") || is(";") || is(",") || is("}") || is("{") || is(":") ||
             cur().kind == TK::Eof);
  }
};

// ======================= declarations =======================================================

NodeP Parser::unit() {
  auto u = mk(NK::Unit, d.files[cur().loc.file >= 0 ? cur().loc.file : 0]->path);
  if (is("namespace")) u->kids.push_back(parseNamespace());
  while (cur().kind != TK::Eof && errs < 50) {
    size_t before = p;
    int eb = errs;
    NodeP it = is("import") ? parseImport() : parseItem(false);
    if (it) u->kids.push_back(std::move(it));
    if (errs > eb && !atBoundary()) recoverTop();
    if (p == before) advance();
  }
  return u;
}

void Parser::recoverTop() {
  int depth = 0;
  while (cur().kind != TK::Eof) {
    if (is("{")) depth++;
    else if (is("}")) { if (--depth <= 0) { advance(); return; } }
    else if (is(";") && depth <= 0) { advance(); return; }
    advance();
  }
}

NodeP Parser::parseNamespace() {
  auto n = mk(NK::Namespace);
  advance();
  std::string path = ident();
  while (accept("::")) path += "::" + ident();
  n->text = path;
  expect(";");
  return n;
}

NodeP Parser::parseImport() {
  auto n = mk(NK::Import);
  advance();
  if (isIdent() && cur().text == "c" && peek().kind == TK::Str) { n->mods = "c"; advance(); }
  if (cur().kind == TK::Str) { n->text = cur().sval; advance(); }
  else {
    std::string path = ident();
    while (is("::")) {
      advance();
      if (accept("*")) { n->mods += (n->mods.empty() ? "" : " ") + std::string("glob"); break; }
      path += "::" + ident();
    }
    n->text = path;
  }
  if (accept("as")) n->kids.push_back(mk(NK::Path, ident()));
  expect(";");
  return n;
}

NodeP Parser::parseAttr() {
  auto n = mk(NK::Attribute);
  advance();  // '@'
  std::string name;
  if (cur().kind == TK::Ident || cur().kind == TK::Keyword) { name = cur().text; advance(); }
  else err("E0105", {desc(cur())});
  if (is("(")) {
    int depth = 0;
    std::string args;
    do {
      if (is("(")) depth++;
      else if (is(")")) depth--;
      if (cur().kind == TK::Eof) break;
      args += (cur().kind == TK::Str ? "\"" + cur().sval + "\"" : cur().text);
      if (peek().space && depth > 0) args += " ";
      advance();
    } while (depth > 0);
    name += args;
  }
  n->text = name;
  return n;
}

NodeP Parser::parseTypeArgs() {
  auto n = mk(NK::TypeArgs);
  expect("<");
  while (!is(">") && cur().kind != TK::Eof) {
    n->kids.push_back(parseType());
    if (!accept(",")) break;
  }
  expect(">");
  return n;
}

NodeP Parser::parseGenericParams() {
  auto n = mk(NK::GenericParams);
  expect("<");
  while (!is(">") && cur().kind != TK::Eof) {
    auto g = mk(NK::GenericParam);
    g->text = ident();
    if (accept(":")) {
      g->kids.push_back(parseType());
      while (accept("+")) g->kids.push_back(parseType());
    }
    n->kids.push_back(std::move(g));
    if (!accept(",")) break;
  }
  expect(">");
  return n;
}

NodeP Parser::parseParam() {
  auto n = mk(NK::Param);
  while (is("out") || is("mut") || is("noalias")) {
    n->mods += (n->mods.empty() ? "" : " ") + cur().text;
    advance();
  }
  n->kids.push_back(parseType());
  if (isIdent()) { n->text = cur().text; advance(); }
  else if (!(is(",") || is(")"))) err("E0105", {desc(cur())});
  return n;
}

void Parser::parseParams(Node& owner) {
  expect("(");
  while (!is(")") && cur().kind != TK::Eof) {
    owner.kids.push_back(parseParam());
    if (!accept(",")) break;
  }
  expect(")");
}

std::string Parser::parseDeclName() {
  std::string name;
  auto opName = [&]() {
    advance();  // 'operator'
    if (is("[")) { advance(); expect("]"); return std::string("operator[]"); }
    if (is("(")) { advance(); expect(")"); return std::string("operator()"); }
    if (cur().kind == TK::Punct) {
      std::string s = cur().text;
      if (s == ">" && adjacent(p, p + 1) && isAt(1, ">")) { advance(); s = ">>"; }
      advance();
      return "operator" + s;
    }
    err("E0105", {desc(cur())});
    return std::string();
  };
  if (is("operator")) return opName();
  if (!isIdent() && !is("init") && !is("drop")) { err("E0105", {desc(cur())}); return ""; }
  name = cur().text;
  advance();
  while (is("::")) {
    advance();
    if (is("operator")) { name += "::" + opName(); break; }
    name += "::" + ident();
  }
  return name;
}

NodeP Parser::parseEnum(const std::string& mods) {
  auto n = mk(NK::Enum);
  n->mods = mods;
  advance();
  n->text = ident();
  if (accept(":")) n->kids.push_back(parseType());
  expect("{");
  while (!is("}") && cur().kind != TK::Eof) {
    auto e = mk(NK::Enumerator, ident());
    if (accept("=")) e->kids.push_back(parseExpr());
    n->kids.push_back(std::move(e));
    if (!accept(",")) break;
  }
  expect("}");
  return n;
}

NodeP Parser::parseTagged() {
  auto n = mk(NK::Tagged);
  advance();  // 'tagged'
  expect("union");
  n->text = ident();
  if (is("<")) n->kids.push_back(parseGenericParams());
  if (accept(":")) n->kids.push_back(parseType());
  expect("{");
  while (!is("}") && cur().kind != TK::Eof) {
    auto v = mk(NK::Variant, ident());
    if (accept("(")) {
      while (!is(")") && cur().kind != TK::Eof) {
        v->kids.push_back(parseParam());
        if (!accept(",")) break;
      }
      expect(")");
    }
    n->kids.push_back(std::move(v));
    if (!accept(",")) break;
  }
  expect("}");
  return n;
}

void Parser::parseBody(Node& n, bool member) {
  expect("{");
  while (!is("}") && cur().kind != TK::Eof && errs < 50) {
    size_t before = p;
    int eb = errs;
    auto it = parseItem(member);
    if (it) n.kids.push_back(std::move(it));
    if (errs > eb && !atBoundary()) {  // skip to the end of this member
      int depth = 0;
      while (cur().kind != TK::Eof) {
        if (is("{")) depth++;
        else if (is("}")) { if (depth == 0) break; if (--depth == 0) { advance(); break; } }
        else if (is(";") && depth == 0) { advance(); break; }
        advance();
      }
    }
    if (p == before) advance();
  }
  expect("}");
}

NodeP Parser::parseItem(bool member) {
  if (accept(";")) return nullptr;
  std::vector<NodeP> attrs;
  while (is("@")) attrs.push_back(parseAttr());
  std::string mods;
  std::string abi;
  Loc l = cur().loc;
  auto addMod = [&](const std::string& m) { mods += (mods.empty() ? "" : " ") + m; };
  for (;;) {
    if (is("pub") || is("priv") || is("static") || is("inline") || is("thread_local") ||
        is("volatile") || is("mut")) {
      addMod(cur().text); advance(); continue;
    }
    if (is("extern") && peek().kind == TK::Str) {
      abi = peek().sval; addMod("extern:" + abi); advance(); advance(); continue;
    }
    break;
  }
  NodeP n;
  auto finish = [&](NodeP x) {
    if (!x) return x;
    if (x->mods.empty()) x->mods = mods;
    else if (!mods.empty()) x->mods = mods + " " + x->mods;
    std::vector<NodeP> k;
    for (auto& a : attrs) k.push_back(std::move(a));
    for (auto& c : x->kids) k.push_back(std::move(c));
    x->kids = std::move(k);
    x->loc = l;
    return x;
  };

  if (is("{") && !abi.empty()) {  // extern "C" { ... }
    auto b = mk(NK::ExternBlock, abi);
    mods.clear();
    parseBody(*b, true);
    return finish(std::move(b));
  }
  if (is("type")) {
    auto a = mk(NK::Alias);
    advance();
    a->text = ident();
    expect("=");
    a->kids.push_back(parseType());
    expect(";");
    return finish(std::move(a));
  }
  if (is("struct") || is("union") || is("class") || is("interface")) {
    NK k = is("struct") ? NK::Struct : is("union") ? NK::Union : is("class") ? NK::Class : NK::Interface;
    auto s = mk(k);
    advance();
    s->text = ident();
    if (is("<")) s->kids.push_back(parseGenericParams());
    if (k == NK::Interface && accept(":")) {
      s->kids.push_back(parseType());
      while (accept("+")) s->kids.push_back(parseType());
    }
    parseBody(*s, true);
    return finish(std::move(s));
  }
  if (is("enum")) return finish(parseEnum(""));
  if (is("tagged")) return finish(parseTagged());
  if (is("impl")) {
    auto s = mk(NK::Impl);
    advance();
    if (is("<")) s->kids.push_back(parseGenericParams());
    s->kids.push_back(parseType());
    if (accept("for")) { s->mods = "for"; s->kids.push_back(parseType()); }
    parseBody(*s, true);
    return finish(std::move(s));
  }
  if (is("init") || is("drop")) {
    auto s = mk(is("init") ? NK::Init : NK::Drop);
    advance();
    parseParams(*s);
    s->kids.push_back(parseBlock());
    return finish(std::move(s));
  }
  if (member && false) return nullptr;

  // function or variable/field
  if (!(isIdent() || cur().kind == TK::Keyword || is("(") )) {
    err("E0107", {desc(cur())});
    return nullptr;
  }
  if (is("}") || is(")")) { err("E0107", {desc(cur())}); return nullptr; }
  NodeP ty = parseType();
  std::string name = parseDeclName();
  NodeP gp;
  if (is("<")) gp = parseGenericParams();
  if (is("(")) {
    auto f = mk(NK::Func, name, l);
    f->kids.push_back(std::move(ty));
    if (gp) f->kids.push_back(std::move(gp));
    parseParams(*f);
    if (is("{")) f->kids.push_back(parseBlock());
    else expect(";");
    return finish(std::move(f));
  }
  auto v = mk(NK::Var, name, l);
  v->kids.push_back(std::move(ty));
  if (accept(":")) { auto w = mk(NK::Empty, "bits"); w->kids.push_back(parseExpr()); v->kids.push_back(std::move(w)); }
  if (accept("=")) {
    if (is("undefined")) { v->kids.push_back(mk(NK::Undefined)); advance(); }
    else v->kids.push_back(parseExpr());
  }
  expect(";");
  return finish(std::move(v));
}

// ======================= types ================================================================

NodeP Parser::parseBaseType() {
  Loc l = cur().loc;
  if (cur().kind == TK::Keyword && isPrimType(cur().text)) {
    auto n = mk(NK::TypePrim, cur().text); advance(); return n;
  }
  if (is("(")) {
    auto n = mk(NK::TypeTuple);
    advance();
    while (!is(")") && cur().kind != TK::Eof) {
      n->kids.push_back(parseType());
      if (!accept(",")) break;
    }
    expect(")");
    return n;
  }
  if (is("fn") || is("closure")) {
    auto n = mk(is("fn") ? NK::TypeFn : NK::TypeClosure);
    advance();
    expect("(");
    while (!is(")") && cur().kind != TK::Eof) {
      n->kids.push_back(parseType());
      if (!accept(",")) break;
    }
    expect(")");
    expect("->");
    n->kids.push_back(parseType());
    return n;
  }
  if (isIdent()) {
    auto n = mk(NK::TypeName, "", l);
    std::string path = cur().text;
    advance();
    while (is("::") && peek().kind == TK::Ident) { advance(); path += "::" + cur().text; advance(); }
    n->text = path;
    if (is("<")) n->kids.push_back(parseTypeArgs());
    return n;
  }
  err("E0103", {desc(cur())});
  return mk(NK::Empty);
}

NodeP Parser::parseType() {
  std::vector<NK> pre;
  while (is("const") || is("noalias")) { pre.push_back(is("const") ? NK::TypeConst : NK::TypeNoalias); advance(); }
  NodeP ty = parseBaseType();
  for (auto it = pre.rbegin(); it != pre.rend(); ++it) {
    auto w = mk(*it); w->kids.push_back(std::move(ty)); ty = std::move(w);
  }
  for (;;) {
    if (is("*")) {
      auto w = mk(NK::TypePtr); advance(); w->kids.push_back(std::move(ty)); ty = std::move(w);
    } else if (is("[")) {
      advance();
      if (accept("]")) {
        auto w = mk(NK::TypeSlice); w->kids.push_back(std::move(ty)); ty = std::move(w);
      } else {
        auto w = mk(NK::TypeArray); w->kids.push_back(std::move(ty));
        w->kids.push_back(parseExpr());
        expect("]");
        ty = std::move(w);
      }
    } else break;
  }
  return ty;
}

// ======================= statements ==============================================================

void Parser::sync() {
  while (cur().kind != TK::Eof) {
    if (is(";")) { advance(); return; }
    if (is("}")) return;
    advance();
  }
}

NodeP Parser::parseBlock() {
  auto b = mk(NK::Block);
  expect("{");
  while (!is("}") && cur().kind != TK::Eof && errs < 50) {
    size_t before = p;
    int eb = errs;
    b->kids.push_back(parseStmt());
    if (errs > eb && !atBoundary()) sync();
    if (p == before) advance();
  }
  expect("}");
  return b;
}

NodeP Parser::parseCond() {
  expect("(");
  auto e = parseExpr();
  if (is("=") && !spec) { err("E0102", {}); advance(); parseExpr(); }  // spec 3.7
  expect(")");
  return e;
}

bool Parser::looksLikeDecl() {
  return lookahead([&] {
    parseType();
    if (!isIdent()) throw SpecAbort{};
    advance();
    if (!(is("=") || is(";"))) throw SpecAbort{};
  });
}

NodeP Parser::parseLocalDecl(bool needSemi) {
  auto v = mk(NK::Var);
  while (is("mut") || is("volatile") || is("static") || is("thread_local")) {
    v->mods += (v->mods.empty() ? "" : " ") + cur().text;
    advance();
  }
  if (v->mods.find("mut") != std::string::npos && isIdent() && isAt(1, "=")) {
    v->text = cur().text; advance();  // inferred: mut x = expr;
  } else {
    v->kids.push_back(parseType());
    v->text = ident();
  }
  if (accept("=")) {
    if (is("undefined")) { v->kids.push_back(mk(NK::Undefined)); advance(); }
    else v->kids.push_back(parseExpr());
  }
  if (needSemi) expect(";");
  return v;
}

bool Parser::assignOp(std::string& op, int& ntok) {
  const Token& a = cur();
  if (a.kind != TK::Punct) return false;
  static const char* ops[] = {"=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", nullptr};
  for (const char** o = ops; *o; o++) if (a.text == *o) { op = a.text; ntok = 1; return true; }
  if (a.text == ">" && peek().kind == TK::Punct && peek().text == ">=" && adjacent(p, p + 1)) {
    op = ">>="; ntok = 2; return true;
  }
  return false;
}

NodeP Parser::parseSimple(bool needSemi) {
  Loc l = cur().loc;
  // (a, mut b) = expr;
  if (is("(")) {
    auto td = trySpec([&]() -> NodeP {
      auto n = mk(NK::TupleDecl);
      advance();
      for (;;) {
        std::string m;
        if (accept("mut")) m = "mut";
        auto e = mk(NK::Path, ident()); e->mods = m; n->kids.push_back(std::move(e));
        if (!accept(",")) break;
      }
      if (!accept(")")) throw SpecAbort{};
      if (!is("=")) throw SpecAbort{};
      return n;
    });
    if (td) {
      advance();  // '='
      td->kids.push_back(parseExpr());
      if (needSemi) expect(";");
      return td;
    }
  }
  auto lhs = parseExpr();
  std::string op; int n = 1;
  if (assignOp(op, n)) {
    for (int i = 0; i < n; i++) advance();
    auto rhs = parseExpr();
    NodeP r = mk(lhs->kind == NK::Path && lhs->kids.empty() && lhs->text.find("::") == std::string::npos && op == "=" ? NK::AssignOrDecl : NK::Assign, op, l);
    r->kids.push_back(std::move(lhs));
    r->kids.push_back(std::move(rhs));
    if (needSemi) expect(";");
    return r;
  }
  auto s = mk(NK::ExprStmt, "", l);
  s->kids.push_back(std::move(lhs));
  if (needSemi) expect(";");
  return s;
}

NodeP Parser::parseFor() {
  auto f = mk(NK::For);
  advance();
  expect("(");
  if (isIdent() && isAt(1, "in")) {
    f->kind = NK::ForIn;
    f->text = cur().text;
    advance(); advance();
    f->kids.push_back(parseExpr());
    expect(")");
    f->kids.push_back(parseBlock());
    return f;
  }
  if (!is(";")) f->kids.push_back(is("mut") || looksLikeDecl() ? parseLocalDecl(false) : parseSimple(false));
  else f->kids.push_back(empty());
  expect(";");
  f->kids.push_back(is(";") ? empty() : parseExpr());
  expect(";");
  f->kids.push_back(is(")") ? empty() : parseSimple(false));
  expect(")");
  f->kids.push_back(parseBlock());
  return f;
}

NodeP Parser::parseSwitch() {
  auto s = mk(NK::Switch);
  advance();
  expect("(");
  s->kids.push_back(parseExpr());
  expect(")");
  expect("{");
  while (!is("}") && cur().kind != TK::Eof && errs < 50) {
    NodeP c;
    if (is("case")) {
      c = mk(NK::Case);
      advance();
      do { c->kids.push_back(parseRange()); } while (accept(","));
    } else if (is("default")) {
      c = mk(NK::Default);
      advance();
    } else {
      err("E0101", {"`case` or `default`", desc(cur())});
      sync();
      continue;
    }
    expect(":");
    auto body = mk(NK::Body);
    while (!is("case") && !is("default") && !is("}") && cur().kind != TK::Eof && errs < 50) {
      size_t before = p; int eb = errs;
      body->kids.push_back(parseStmt());
      if (errs > eb && !atBoundary()) sync();
      if (p == before) advance();
    }
    c->kids.push_back(std::move(body));
    s->kids.push_back(std::move(c));
  }
  expect("}");
  return s;
}

NodeP Parser::parsePattern() {
  Loc l = cur().loc;
  auto lit = [&]() -> NodeP {
    auto n = mk(NK::PatLit, "", l);
    if (is("-")) { advance(); n->text = "-"; }
    n->text += cur().text;
    advance();
    return n;
  };
  NodeP pat;
  if (isIdent() && cur().text == "_") { pat = mk(NK::PatWild); advance(); }
  else if (cur().kind == TK::Int || cur().kind == TK::Char || cur().kind == TK::Str ||
           cur().kind == TK::Float || is("true") || is("false") || is("-")) {
    pat = lit();
    if (is("..") || is("..=")) {
      auto r = mk(NK::PatRange, cur().text, l);
      advance();
      r->kids.push_back(std::move(pat));
      r->kids.push_back(lit());
      pat = std::move(r);
    }
  } else if (is("(")) {
    pat = mk(NK::PatTuple);
    advance();
    while (!is(")") && cur().kind != TK::Eof) {
      pat->kids.push_back(parsePattern());
      if (!accept(",")) break;
    }
    expect(")");
  } else if (isIdent()) {
    std::string path = cur().text;
    advance();
    while (is("::") && peek().kind == TK::Ident) { advance(); path += "::" + cur().text; advance(); }
    if (is("(")) {
      pat = mk(NK::PatVariant, path, l);
      advance();
      while (!is(")") && cur().kind != TK::Eof) {
        pat->kids.push_back(parsePattern());
        if (!accept(",")) break;
      }
      expect(")");
    } else pat = mk(NK::PatName, path, l);  // binding or payload-less variant; resolved by sema
  } else {
    err("E0108", {desc(cur())});
    pat = empty();
  }
  return pat;
}

NodeP Parser::parseMatch() {
  auto m = mk(NK::Match);
  advance();
  expect("(");
  m->kids.push_back(parseExpr());
  expect(")");
  expect("{");
  while (!is("}") && cur().kind != TK::Eof && errs < 50) {
    size_t before = p; int eb = errs;
    auto arm = mk(NK::Arm);
    arm->kids.push_back(parsePattern());
    expect("=>");
    arm->kids.push_back(parseStmt());
    m->kids.push_back(std::move(arm));
    if (errs > eb && !atBoundary()) sync();
    if (p == before) advance();
  }
  expect("}");
  return m;
}

NodeP Parser::parseStmt() {
  Loc l = cur().loc;
  if (is("{")) return parseBlock();
  if (is("if")) {
    auto n = mk(NK::If);
    advance();
    n->kids.push_back(parseCond());
    n->kids.push_back(parseBlock());
    if (accept("else")) n->kids.push_back(is("if") ? parseStmt() : parseBlock());
    return n;
  }
  if (is("while")) {
    auto n = mk(NK::While); advance();
    n->kids.push_back(parseCond()); n->kids.push_back(parseBlock());
    return n;
  }
  if (is("do")) {
    auto n = mk(NK::DoWhile); advance();
    n->kids.push_back(parseBlock());
    expect("while");
    n->kids.push_back(parseCond());
    expect(";");
    return n;
  }
  if (is("for")) return parseFor();
  if (is("switch")) return parseSwitch();
  if (is("match")) return parseMatch();
  if (is("return")) {
    auto n = mk(NK::Return); advance();
    if (!is(";")) n->kids.push_back(parseExpr());
    expect(";");
    return n;
  }
  if (is("break") || is("continue") || is("goto")) {
    auto n = mk(is("break") ? NK::Break : is("continue") ? NK::Continue : NK::Goto);
    bool g = is("goto");
    advance();
    if (isIdent()) { n->text = cur().text; advance(); }
    else if (g) err("E0105", {desc(cur())});
    expect(";");
    return n;
  }
  if (is("fallthrough")) { auto n = mk(NK::Fallthrough); advance(); expect(";"); return n; }
  if (is("defer")) {
    auto n = mk(NK::Defer); advance();
    n->kids.push_back(parseStmt());
    return n;
  }
  if (is("free")) {
    auto n = mk(NK::Free);
    advance();
    if (is("[") && isAt(1, "]")) { advance(); advance(); n->mods = "array"; }
    NodeP first;
    if (is("(")) {  // free(a) p;  or  free (p);
      advance();
      first = parseExpr();
      expect(")");
      if (is(";")) { n->kids.push_back(std::move(first)); expect(";"); return n; }
      n->kids.push_back(std::move(first));
      n->kids.push_back(parseExpr());
    } else {
      n->kids.push_back(parseExpr());
    }
    expect(";");
    return n;
  }
  if (isIdent() && isAt(1, ":") && !isAt(1, "::")) {
    auto n = mk(NK::Label, cur().text, l);
    advance(); advance();
    n->kids.push_back(parseStmt());
    return n;
  }
  if (is("mut") || is("volatile") || is("static") || is("thread_local") || is("const") ||
      is("fn") || is("closure") ||
      (cur().kind == TK::Keyword && isPrimType(cur().text) && !isAt(1, "::") && !isAt(1, ".")) ||
      ((isIdent() || is("(")) && looksLikeDecl()))
    return parseLocalDecl(true);
  return parseSimple(true);
}

// ======================= expressions ===============================================================

bool Parser::binOp(std::string& op, int& prec, int& ntok) {
  const Token& a = cur();
  if (a.kind != TK::Punct) return false;
  ntok = 1;
  const std::string& s = a.text;
  if (s == ">") {
    const Token& b = peek();
    bool adj = adjacent(p, std::min(p + 1, t.size() - 1));
    if (adj && b.kind == TK::Punct && b.text == ">") { op = ">>"; prec = 8; ntok = 2; return true; }
    if (adj && b.kind == TK::Punct && b.text == ">=") return false;  // '>>=' assignment
    op = ">"; prec = 4; return true;
  }
  static const std::map<std::string, int> P = {
      {"||", 1}, {"&&", 2}, {"==", 3}, {"!=", 3}, {"<", 4}, {"<=", 4}, {">=", 4}, {"|", 5},
      {"^", 6}, {"&", 7}, {"<<", 8}, {"+", 9}, {"-", 9}, {"*", 10}, {"/", 10}, {"%", 10}};
  auto it = P.find(s);
  if (it == P.end()) return false;
  op = s; prec = it->second;
  return true;
}

NodeP Parser::parseTernary() {
  auto c = parseRange();
  if (is("?")) {
    auto n = mk(NK::Ternary);
    advance();
    n->kids.push_back(std::move(c));
    n->kids.push_back(parseExpr());
    expect(":");
    n->kids.push_back(parseTernary());
    return n;
  }
  return c;
}

NodeP Parser::parseRange() {
  Loc l = cur().loc;
  NodeP lhs;
  if (is("..") || is("..=")) lhs = empty();
  else lhs = parseBinary(1);
  if (is("..") || is("..=")) {
    auto r = mk(NK::Range, cur().text, l);
    advance();
    r->kids.push_back(std::move(lhs));
    r->kids.push_back(startsExpr() ? parseBinary(1) : empty());
    return r;
  }
  return lhs;
}

NodeP Parser::parseBinary(int minPrec) {
  auto lhs = parseCast();
  for (;;) {
    std::string op; int prec = 0, nt = 1;
    if (!binOp(op, prec, nt) || prec < minPrec) break;
    Loc l = cur().loc;
    for (int i = 0; i < nt; i++) advance();
    auto rhs = parseBinary(prec + 1);
    auto n = mk(NK::Binary, op, l);
    n->kids.push_back(std::move(lhs));
    n->kids.push_back(std::move(rhs));
    lhs = std::move(n);
  }
  return lhs;
}

NodeP Parser::parseCast() {
  auto e = parseUnary();
  while (is("as")) {
    auto n = mk(NK::Cast);
    advance();
    n->kids.push_back(std::move(e));
    n->kids.push_back(parseType());
    e = std::move(n);
  }
  return e;
}

NodeP Parser::parseAlloc() {
  auto n = mk(NK::Alloc);
  advance();
  if (accept("zeroed")) n->mods = "zeroed";
  if (is("(")) {  // alloc(allocator) T
    advance();
    auto a = mk(NK::Empty, "allocator");
    a->kids.push_back(parseExpr());
    expect(")");
    n->kids.push_back(std::move(a));
  }
  NodeP base = parseBaseType();
  while (is("*")) { auto w = mk(NK::TypePtr); advance(); w->kids.push_back(std::move(base)); base = std::move(w); }
  n->kids.push_back(std::move(base));
  if (is("[")) {
    advance();
    auto c = mk(NK::Empty, "count");
    c->kids.push_back(parseExpr());
    expect("]");
    n->kids.push_back(std::move(c));
  }
  if (is("(")) {
    advance();
    auto a = mk(NK::Empty, "init-args");
    while (!is(")") && cur().kind != TK::Eof) {
      a->kids.push_back(parseExpr());
      if (!accept(",")) break;
    }
    expect(")");
    n->kids.push_back(std::move(a));
  }
  return n;
}

NodeP Parser::parseUnary() {
  Loc l = cur().loc;
  if (is("-") || is("!") || is("~") || is("*") || is("&")) {
    auto n = mk(NK::Unary, cur().text, l);
    advance();
    n->kids.push_back(parseUnary());
    return n;
  }
  if (is("try")) {
    auto n = mk(NK::Try, "", l);
    advance();
    n->kids.push_back(parseUnary());
    return n;
  }
  if (is("sizeof") || is("alignof")) {
    auto n = mk(is("sizeof") ? NK::Sizeof : NK::Alignof, "", l);
    advance();
    if (is("(")) {
      auto ty = trySpec([&]() -> NodeP { advance(); auto x = parseType(); if (!is(")")) throw SpecAbort{}; advance(); return x; });
      if (ty) { n->kids.push_back(std::move(ty)); return n; }
    }
    n->kids.push_back(parseUnary());
    return n;
  }
  if (is("alloc")) return parseAlloc();
  return parsePostfix();
}

NodeP Parser::parsePostfix() {
  auto e = parsePrimary();
  for (;;) {
    Loc l = cur().loc;
    if (is(".") || is("->")) {
      bool arrow = is("->");
      advance();
      auto accessor = [&](const std::string& name) {
        auto m = mk(arrow ? NK::Arrow : NK::Member, name, l);
        m->kids.push_back(std::move(e));
        e = std::move(m);
      };
      if (cur().kind == TK::Ident || cur().kind == TK::Keyword || cur().kind == TK::Int) {
        accessor(cur().text); advance();
      } else if (cur().kind == TK::Float && cur().text.find('.') != std::string::npos &&
                 cur().text.find_first_not_of("0123456789.") == std::string::npos) {
        std::string a = cur().text.substr(0, cur().text.find('.')), b = cur().text.substr(cur().text.find('.') + 1);
        accessor(a); accessor(b); advance();  // t.0.1 lexes as t . 0.1
      } else {
        err("E0105", {desc(cur())});
        break;
      }
    } else if (is("(")) {
      auto c = mk(NK::Call, "", l);
      advance();
      c->kids.push_back(std::move(e));
      while (!is(")") && cur().kind != TK::Eof) {
        if (is("out")) {
          auto o = mk(NK::OutArg); advance(); o->kids.push_back(parseExpr()); c->kids.push_back(std::move(o));
        } else c->kids.push_back(parseExpr());
        if (!accept(",")) break;
      }
      expect(")");
      e = std::move(c);
    } else if (is("[")) {
      auto c = mk(NK::Index, "", l);
      advance();
      c->kids.push_back(std::move(e));
      c->kids.push_back(parseExpr());
      expect("]");
      e = std::move(c);
    } else break;
  }
  return e;
}

NodeP Parser::parsePathExpr() {
  Loc l = cur().loc;
  auto n = mk(NK::Path, cur().text, l);
  std::string path = cur().text;
  advance();
  for (;;) {
    if (is("<")) {
      auto ga = trySpec([&]() -> NodeP {
        auto a = parseTypeArgs();
        if (!(is("::") || is("(") || is("{"))) throw SpecAbort{};
        return a;
      });
      if (!ga) break;
      n->kids.push_back(std::move(ga));
      continue;
    }
    if (is("::") && (peek().kind == TK::Ident || peek().kind == TK::Keyword)) {
      advance(); path += "::" + cur().text; advance(); continue;
    }
    break;
  }
  n->text = path;
  if (is("{")) {  // struct literal: Name{ a: 1, b: 2 } or Name{ 1, 2 }
    auto s = mk(NK::StructLit, "", l);
    advance();
    s->kids.push_back(std::move(n));
    while (!is("}") && cur().kind != TK::Eof) {
      auto f = mk(NK::FieldInit);
      if (isIdent() && isAt(1, ":")) { f->text = cur().text; advance(); advance(); }
      f->kids.push_back(parseExpr());
      s->kids.push_back(std::move(f));
      if (!accept(",")) break;
    }
    expect("}");
    return s;
  }
  return n;
}

NodeP Parser::parseLambda() {
  auto n = mk(NK::Lambda);
  if (accept("heap")) n->mods = "heap";
  if (is("[")) {
    advance();
    if ((is("=") || is("&")) && isAt(1, "]")) { n->kids.push_back(mk(NK::Capture, cur().text)); advance(); }
    else {
      while (!is("]") && cur().kind != TK::Eof) {
        auto c = mk(NK::Capture);
        if (accept("&")) c->mods = "&";
        c->text = ident();
        n->kids.push_back(std::move(c));
        if (!accept(",")) break;
      }
    }
    expect("]");
  }
  parseParams(*n);
  expect("->");
  n->kids.push_back(parseType());
  n->kids.push_back(parseBlock());
  return n;
}

NodeP Parser::parsePrimary() {
  const Token& k = cur();
  Loc l = k.loc;
  switch (k.kind) {
    case TK::Int: { auto n = mk(NK::IntLit, k.text, l); advance(); return n; }
    case TK::Float: { auto n = mk(NK::FloatLit, k.text, l); advance(); return n; }
    case TK::Char: { auto n = mk(NK::CharLit, k.text, l); advance(); return n; }
    case TK::Str: { auto n = mk(NK::StrLit, k.sval, l); advance(); return n; }
    case TK::RawStr: { auto n = mk(NK::RawStrLit, k.sval, l); advance(); return n; }
    case TK::CStr: { auto n = mk(NK::CStrLit, k.sval, l); advance(); return n; }
    case TK::Ident: return parsePathExpr();
    case TK::Keyword:
      if (k.text == "true" || k.text == "false") { auto n = mk(NK::BoolLit, k.text, l); advance(); return n; }
      if (k.text == "null") { auto n = mk(NK::NullLit, "", l); advance(); return n; }
      if (k.text == "undefined") { auto n = mk(NK::Undefined, "", l); advance(); return n; }
      if (k.text == "heap") return parseLambda();
      if (isPrimType(k.text)) return parsePathExpr();
      break;
    case TK::Punct:
      if (k.text == "[") return parseLambda();
      if (k.text == "{") {
        auto n = mk(NK::InitList, "", l);
        advance();
        while (!is("}") && cur().kind != TK::Eof) {
          n->kids.push_back(parseExpr());
          if (!accept(",")) break;
        }
        expect("}");
        return n;
      }
      if (k.text == "(") {
        bool lam = lookahead([&] { parseParams(*mk(NK::Empty)); if (!is("->")) throw SpecAbort{}; });
        if (lam) return parseLambda();
        advance();
        auto e = parseExpr();
        if (is(",")) {
          auto tup = mk(NK::Tuple, "", l);
          tup->kids.push_back(std::move(e));
          while (accept(",")) { if (is(")")) break; tup->kids.push_back(parseExpr()); }
          expect(")");
          return tup;
        }
        expect(")");
        return e;
      }
      break;
    default: break;
  }
  err("E0104", {desc(k)});
  if (!(is(";") || is(")") || is("}") || is("]") || is(",") || cur().kind == TK::Eof)) advance();
  return empty();
}

}  // namespace

NodeP parse(DiagEngine& d, std::vector<Token> toks) {
  Parser ps(d, std::move(toks));
  return ps.unit();
}

}  // namespace c7
