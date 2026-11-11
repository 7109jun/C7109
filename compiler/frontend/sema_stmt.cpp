#include "sema_impl.hpp"

namespace c7 {
namespace sema {

namespace {
std::string lastSeg(const std::string& s) {
  size_t i = s.rfind("::");
  return i == std::string::npos ? s : s.substr(i + 2);
}
bool isLoop(NK k) { return k == NK::While || k == NK::DoWhile || k == NK::For || k == NK::ForIn; }
}  // namespace

void Sema::checkCond(Node* c) {
  ExprInfo e = expr(c);
  if (isErr(e.ty)) return;
  if (e.ty->k != TyK::Bool) rep("E0224", c, {ts(e.ty)});
}

void Sema::block(Node* n, bool newScope) {
  if (newScope) pushScope();
  for (auto& k : n->kids) stmt(k.get());
  if (newScope) popScope();
}

void Sema::localVar(Node* n) {
  Node *typeNode = nullptr, *init = nullptr;
  for (auto& k : n->kids) {
    if (!typeNode && !init && isTypeNode(k->kind)) typeNode = k.get();
    else init = k.get();
  }
  bool mut = hasMod(n->mods, "mut");
  TypeP ty;
  if (typeNode) {
    ty = resolveType(typeNode);
    if (!init) rep("E0305", n, {n->text});
    else if (init->kind != NK::Undefined) exprConv(init, ty);
  } else if (init) {
    ExprInfo e = expr(init);
    consume(e, init);
    ty = defaultType(e.ty, init);
  } else {
    rep("E0305", n, {n->text});
    ty = mkErr();
  }
  n->sty = ty;
  bindLocal(n, n->text, ty, mut);
}

void Sema::assignStmt(Node* n) {
  Node* lhs = n->kids[0].get();
  Node* rhs = n->kids[1].get();
  if (n->kind == NK::AssignOrDecl) {
    const std::string& name = lhs->text;
    Local* l = nullptr;
    ExprInfo f;
    bool visible = lookupLocal(name, l) || lookupSelfMember(name, f);
    if (!visible) {
      const std::vector<Decl*>* itg = lookupGlobal(name);
      if (itg) for (Decl* x : *itg) if (x->k == Decl::Var) visible = true;
    }
    if (!visible) {  // appendix D #4 (amended): an unseen name introduces an immutable variable
      ExprInfo e = expr(rhs);
      consume(e, rhs);
      n->flag = 1;
      n->sty = defaultType(e.ty, rhs);
      bindLocal(n, name, n->sty ? std::static_pointer_cast<Type>(n->sty) : mkErr(), false, false);
      return;
    }
  }
  ExprInfo l = expr(lhs);
  const std::string& op = n->text;
  if (op == "=") exprConv(rhs, l.ty);
  else {
    ExprInfo r = expr(rhs);
    TypeP L = l.ty, R = r.ty;
    if (!isErr(L) && !isErr(R)) {
      bool ok = false;
      if (isNum(L) && isNum(R)) ok = conv(R, L) == Conv::Ok;
      else if (L->k == TyK::Ptr && isInt(R) && (op == "+=" || op == "-=")) ok = true;
      else if (L->k == TyK::Param && !L->bounds.empty()) ok = true;
      else if (L->k == TyK::Bool && R->k == TyK::Bool) ok = true;
      else if (L->k == TyK::Named) {
        std::string base = "operator" + op.substr(0, op.size() - 1);
        ok = !findMembers(L, "operator" + op).empty() || !findMembers(L, base).empty() || globals.count("operator" + op) || globals.count(base);
      }
      if (!ok) rep("E0225", n, {ts(L), ts(R), op});
    }
  }
  if (!isErr(l.ty) && !(l.lv && l.wr)) {
    if (l.constSelf) rep("E0303", lhs, {l.place.empty() ? "value" : l.place});
    else rep("E0301", lhs, {l.place.empty() ? "value" : l.place});
  }
}

void Sema::forIn(Node* n) {
  Node* it = n->kids[0].get();
  TypeP elem = mkErr();
  if (it->kind == NK::Range) {
    TypeP t;
    for (auto& k : it->kids) {
      if (k->kind == NK::Empty) continue;
      ExprInfo e = expr(k.get());
      if (!isErr(e.ty) && !isInt(e.ty)) rep("E0225", k.get(), {ts(e.ty), ts(e.ty), it->text});
      if (!t || (t->k == TyK::IntLit && e.ty->k != TyK::IntLit)) t = e.ty;
    }
    elem = defaultType(t ? t : mkErr(), it);
  } else {
    ExprInfo e = expr(it);
    TypeP t = e.ty;
    if (t->k == TyK::Ptr && t->elem->k == TyK::Named) t = t->elem;
    if (isErr(t)) {}
    else if (t->k == TyK::Array || t->k == TyK::Slice) elem = t->elem;
    else if (t->k == TyK::Named) {
      auto ms = findMembers(t, "next");
      if (!ms.empty() && ms[0].first->isMethod) {
        TypeP rt = subst(ms[0].first->ty->ret, ms[0].second);
        if (rt->k == TyK::Named && rt->decl == dOption && !rt->elems.empty()) elem = rt->elems[0];
        else rep("E0232", it, {ts(t)});
      } else rep("E0232", it, {ts(t)});
    } else rep("E0232", it, {ts(t)});
  }
  n->sty = elem;
  pushScope();
  bindLocal(n, n->text, elem, false);
  ctxs.back().loops++;
  block(n->kids[1].get(), false);
  ctxs.back().loops--;
  popScope();
}

void Sema::checkSwitch(Node* n) {
  ExprInfo e = expr(n->kids[0].get());
  TypeP st = e.ty;
  if (!isErr(st) && !(st->k == TyK::Int || st->k == TyK::Char || (st->k == TyK::Named && st->decl->k == Decl::Enum)))
    rep("E0220", n->kids[0].get(), {"integer, `Char` or enum", ts(st)});
  TypeP target = isErr(st) ? st : (st->k == TyK::IntLit ? mkPrim("i32") : st);
  for (size_t i = 1; i < n->kids.size(); i++) {
    Node* c = n->kids[i].get();
    for (auto& v : c->kids) {
      if (v->kind == NK::Body) continue;
      if (v->kind == NK::Range) {
        for (auto& rk : v->kids) if (rk->kind != NK::Empty) { ExprInfo x = expr(rk.get(), target); expectConv(x.ty, target, rk.get()); }
      } else { ExprInfo x = expr(v.get(), target); expectConv(x.ty, target, v.get()); }
    }
    ctxs.back().switches++;
    pushScope();
    for (auto& v : c->kids) if (v->kind == NK::Body) for (auto& s : v->kids) stmt(s.get());
    popScope();
    ctxs.back().switches--;
  }
}

// ---- match ----------------------------------------------------------------------------------------------

bool Sema::irrefutable(Node* pat, const TypeP& t) {
  switch (pat->kind) {
    case NK::PatWild: return true;
    case NK::PatName: {
      if (t && t->k == TyK::Named) {
        std::string nm = lastSeg(pat->text);
        for (auto& m : t->decl->members) if ((m.isVariant || m.isEnumerator) && m.name == nm) return false;
      }
      return true;
    }
    case NK::PatTuple: {
      for (size_t i = 0; i < pat->kids.size(); i++)
        if (!irrefutable(pat->kids[i].get(), (t && t->k == TyK::Tuple && i < t->elems.size()) ? t->elems[i] : nullptr)) return false;
      return true;
    }
    default: return false;
  }
}

void Sema::bindPattern(Node* pat, const TypeP& scrut, bool& catchAll, std::set<std::string>& covered, bool top) {
  switch (pat->kind) {
    case NK::PatWild: if (top) catchAll = true; return;
    case NK::PatName: {
      std::string nm = lastSeg(pat->text);
      if (!isErr(scrut) && scrut->k == TyK::Named) {
        declareDecl(scrut->decl);
        for (auto& m : scrut->decl->members) {
          if (m.name != nm) continue;
          if (m.isEnumerator) { covered.insert(nm); return; }
          if (m.isVariant) {
            if (!m.payload.empty() && !(m.payload.size() == 1 && (m.payload[0]->k == TyK::Void || (m.payload[0]->k == TyK::Param && scrut->elems.size() > 0 && scrut->decl->tparams.size() == scrut->elems.size() && [&] { for (size_t q = 0; q < scrut->decl->tparams.size(); q++) if (scrut->decl->tparams[q] == m.payload[0]->name) return scrut->elems[q]->k == TyK::Void; return false; }()))))
              rep("E0222", pat, {std::to_string(m.payload.size()), "0"});
            covered.insert(nm);
            return;
          }
        }
      }
      if (pat->text.find("::") != std::string::npos) { rep("E0214", pat, {ts(scrut), nm}); return; }
      bindLocal(pat, nm, scrut, false, false);
      pat->flag = 1;  // a binding (not a variant name): E0460 needs to know
      if (top) catchAll = true;
      return;
    }
    case NK::PatLit: case NK::PatRange: {
      auto litType = [&](Node* p) -> TypeP {
        const std::string& t = p->text;
        if (t == "true" || t == "false") return mkPrim("bool");
        if (!t.empty() && t[0] == '\'') return mkPrim("Char");
        if (!t.empty() && t[0] == '"') return strType();
        uint64_t v; std::string suf; bool isF;
        bool neg = !t.empty() && t[0] == '-';
        parseNumber(neg ? t.substr(1) : t, v, suf, isF);
        return isF ? mkFloatLit() : mkIntLit(v, neg);
      };
      std::vector<Node*> lits;
      if (pat->kind == NK::PatLit) lits.push_back(pat);
      else for (auto& k : pat->kids) lits.push_back(k.get());
      for (Node* l : lits) {
        expectConv(litType(l), scrut, l);
        if (!isErr(scrut) && scrut->k == TyK::Bool && pat->kind == NK::PatLit) covered.insert(l->text);
      }
      return;
    }
    case NK::PatTuple: {
      bool sub = true;
      if (!isErr(scrut) && (scrut->k != TyK::Tuple || scrut->elems.size() != pat->kids.size())) {
        rep("E0220", pat, {ts(scrut), "tuple pattern"});
        for (auto& k : pat->kids) { bool c = false; std::set<std::string> cv; bindPattern(k.get(), mkErr(), c, cv, false); }
        return;
      }
      for (size_t i = 0; i < pat->kids.size(); i++) {
        TypeP et = isErr(scrut) ? mkErr() : scrut->elems[i];
        bool c = false; std::set<std::string> cv;
        bindPattern(pat->kids[i].get(), et, c, cv, false);
        if (!irrefutable(pat->kids[i].get(), et)) sub = false;
      }
      if (top && sub) catchAll = true;
      return;
    }
    case NK::PatVariant: {
      std::string nm = lastSeg(pat->text);
      const Member* vm = nullptr;
      SubMap sm;
      if (!isErr(scrut) && scrut->k == TyK::Named && scrut->decl->k == Decl::Tagged) {
        declareDecl(scrut->decl);
        for (auto& m : scrut->decl->members) if (m.isVariant && m.name == nm) vm = &m;
        for (size_t i = 0; i < scrut->decl->tparams.size() && i < scrut->elems.size(); i++) sm[scrut->decl->tparams[i]] = scrut->elems[i];
        if (!vm) rep("E0214", pat, {ts(scrut), nm});
      } else if (!isErr(scrut)) rep("E0220", pat, {ts(scrut), nm});
      bool unitPat = vm && vm->payload.size() == 1 && pat->kids.empty() && vm->payload[0]->k == TyK::Param;
      if (vm && vm->payload.size() != pat->kids.size() && !unitPat) rep("E0222", pat, {std::to_string(vm->payload.size()), std::to_string(pat->kids.size())});
      bool all = true;
      for (size_t i = 0; i < pat->kids.size(); i++) {
        TypeP pt = (vm && i < vm->payload.size()) ? subst(vm->payload[i], sm) : mkErr();
        bool c = false; std::set<std::string> cv;
        bindPattern(pat->kids[i].get(), pt, c, cv, false);
        if (!irrefutable(pat->kids[i].get(), pt)) all = false;
      }
      if (vm && all) covered.insert(nm);
      return;
    }
    default: return;
  }
}

void Sema::checkMatch(Node* n) {
  ExprInfo e = expr(n->kids[0].get());
  TypeP st = e.ty;
  bool catchAll = false;
  std::set<std::string> covered;
  for (size_t i = 1; i < n->kids.size(); i++) {
    Node* arm = n->kids[i].get();
    pushScope();
    bindPattern(arm->kids[0].get(), st, catchAll, covered, true);
    stmt(arm->kids[1].get());
    popScope();
  }
  if (isErr(st) || catchAll) return;
  std::vector<std::string> missing;
  if (st->k == TyK::Named && (st->decl->k == Decl::Enum || st->decl->k == Decl::Tagged)) {
    for (auto& m : st->decl->members) if ((m.isEnumerator || m.isVariant) && !covered.count(m.name)) missing.push_back("`" + m.name + "`");
  } else if (st->k == TyK::Bool) {
    for (const char* b : {"true", "false"}) if (!covered.count(b)) missing.push_back(std::string("`") + b + "`");
  } else missing.push_back("`_`");
  if (!missing.empty()) {
    std::string s;
    for (size_t i = 0; i < missing.size(); i++) s += (i ? ", " : "") + missing[i];
    rep("E0450", n, {s});
  }
}

// ---- control-flow helpers ----------------------------------------------------------------------------------------

void Sema::collectLabels(Node* n, std::set<std::string>& out) {
  if (!n || n->kind == NK::Lambda) return;
  if (n->kind == NK::Label) out.insert(n->text);
  for (auto& k : n->kids) collectLabels(k.get(), out);
}

void Sema::checkDeferBody(Node* n, int loopDepth, bool& bad) {
  if (!n || n->kind == NK::Lambda) return;
  if (n->kind == NK::Return || n->kind == NK::Goto) { rep("E0470", n, {n->kind == NK::Return ? "return" : "goto"}); bad = true; }
  if ((n->kind == NK::Break || n->kind == NK::Continue) && loopDepth == 0) { rep("E0470", n, {n->kind == NK::Break ? "break" : "continue"}); bad = true; }
  int next = loopDepth + ((isLoop(n->kind) || n->kind == NK::Switch) ? 1 : 0);
  for (auto& k : n->kids) checkDeferBody(k.get(), next, bad);
}

bool Sema::containsBreak(Node* n, int depth) {
  if (!n || n->kind == NK::Lambda) return false;
  if (n->kind == NK::Break && (depth == 0 || !n->text.empty())) return true;
  int next = depth + ((isLoop(n->kind) || n->kind == NK::Switch) ? 1 : 0);
  for (auto& k : n->kids) if (containsBreak(k.get(), next)) return true;
  return false;
}

bool Sema::terminates(Node* n) {
  if (!n) return false;
  switch (n->kind) {
    case NK::Return: case NK::Goto: return true;
    case NK::Block: case NK::Body: for (auto& k : n->kids) if (terminates(k.get())) return true; return false;
    case NK::Label: return terminates(n->kids[0].get());
    case NK::If: return n->kids.size() == 3 && terminates(n->kids[1].get()) && terminates(n->kids[2].get());
    case NK::While: return n->kids[0]->kind == NK::BoolLit && n->kids[0]->text == "true" && !containsBreak(n->kids[1].get(), 0);
    case NK::For: return n->kids[1]->kind == NK::Empty && !containsBreak(n->kids[3].get(), 0);
    case NK::Match: {
      if (n->kids.size() < 2) return false;
      for (size_t i = 1; i < n->kids.size(); i++) if (!terminates(n->kids[i]->kids[1].get())) return false;
      return true;
    }
    case NK::Switch: {
      bool hasDefault = false;
      for (size_t i = 1; i < n->kids.size(); i++) {
        Node* c = n->kids[i].get();
        if (c->kind == NK::Default) hasDefault = true;
        if (!terminates(c->kids.back().get())) return false;
      }
      return hasDefault;
    }
    case NK::ExprStmt: {
      Node* e = n->kids[0].get();
      if (e->kind == NK::Call && e->kids[0]->kind == NK::Path) {
        const std::vector<Decl*>* itn = lookupGlobal(e->kids[0]->text);
        if (itn) for (Decl* d2 : *itn) if (d2->k == Decl::Func && d2->noreturn) return true;
      }
      return false;
    }
    default: return false;
  }
}

void Sema::stmt(Node* n) {
  if (!n) return;
  FnCtx& c = ctxs.back();
  switch (n->kind) {
    case NK::Block: block(n); return;
    case NK::Var: localVar(n); return;
    case NK::Assign: case NK::AssignOrDecl: assignStmt(n); return;
    case NK::TupleDecl: {
      Node* rhs = n->kids.back().get();
      ExprInfo e = expr(rhs);
      size_t cnt = n->kids.size() - 1;
      if (!isErr(e.ty) && (e.ty->k != TyK::Tuple || e.ty->elems.size() != cnt)) rep("E0220", rhs, {"tuple of " + std::to_string(cnt), ts(e.ty)});
      for (size_t i = 0; i < cnt; i++) {
        TypeP t = (!isErr(e.ty) && e.ty->k == TyK::Tuple && i < e.ty->elems.size()) ? defaultType(e.ty->elems[i], rhs) : mkErr();
        n->kids[i]->sty = t;
        bindLocal(n->kids[i].get(), n->kids[i]->text, t, hasMod(n->kids[i]->mods, "mut"));
      }
      return;
    }
    case NK::ExprStmt: { ExprInfo e = expr(n->kids[0].get()); (void)e; return; }
    case NK::If:
      checkCond(n->kids[0].get());
      block(n->kids[1].get());
      if (n->kids.size() > 2) stmt(n->kids[2].get());
      return;
    case NK::While:
      checkCond(n->kids[0].get());
      c.loops++; block(n->kids[1].get()); ctxs.back().loops--;
      return;
    case NK::DoWhile:
      c.loops++; block(n->kids[0].get()); ctxs.back().loops--;
      checkCond(n->kids[1].get());
      return;
    case NK::For: {
      pushScope();
      Node* init = n->kids[0].get();
      if (init->kind != NK::Empty) stmt(init);
      if (n->kids[1]->kind != NK::Empty) checkCond(n->kids[1].get());
      if (n->kids[2]->kind != NK::Empty) stmt(n->kids[2].get());
      ctxs.back().loops++;
      block(n->kids[3].get());
      ctxs.back().loops--;
      popScope();
      return;
    }
    case NK::ForIn: forIn(n); return;
    case NK::Switch: checkSwitch(n); return;
    case NK::Match: checkMatch(n); return;
    case NK::Return: {
      TypeP curRet = c.ret;  // copy: evaluating a lambda inside the expression may reallocate `ctxs`
      bool isVoid = !curRet || curRet->k == TyK::Void;
      if (n->kids.empty()) { if (!isVoid && !isErr(curRet)) rep("E0412", n, {"missing return value"}); return; }
      if (isVoid) { rep("E0412", n, {"value returned from a function returning `void`"}); expr(n->kids[0].get()); return; }
      exprConv(n->kids[0].get(), curRet, true);
      return;
    }
    case NK::Break: case NK::Continue: {
      bool isBreak = n->kind == NK::Break;
      if (!n->text.empty()) { if (!c.labels.count(n->text)) rep("E0230", n, {n->text}); return; }
      if (isBreak ? (c.loops == 0 && c.switches == 0) : c.loops == 0) rep("E0229", n, {isBreak ? "break" : "continue"});
      return;
    }
    case NK::Goto: if (!c.labels.count(n->text)) rep("E0230", n, {n->text}); return;
    case NK::Label: stmt(n->kids[0].get()); return;
    case NK::Fallthrough: if (c.switches == 0) rep("E0229", n, {"fallthrough"}); return;
    case NK::Defer: {
      bool bad = false;
      checkDeferBody(n->kids[0].get(), 0, bad);
      c.defers++;
      stmt(n->kids[0].get());
      ctxs.back().defers--;
      return;
    }
    case NK::Free: {
      for (size_t i = 0; i + 1 < n->kids.size(); i++) expr(n->kids[i].get());
      ExprInfo p = expr(n->kids.back().get());
      if (!isErr(p.ty) && p.ty->k != TyK::Ptr && p.ty->k != TyK::Null) rep("E0227", n->kids.back().get(), {ts(p.ty)});
      return;
    }
    default: return;
  }
}

// ---- function bodies and entry point --------------------------------------------------------------------------

namespace {
// E0460 support: which variable declarations are in scope at every `goto` and every label of one function body.
struct GotoScan {
  std::vector<Node*> live;                                   // declarations in scope at the current point, in order
  std::map<std::string, std::vector<Node*>> atLabel;
  std::vector<std::pair<Node*, std::vector<Node*>>> gotos;
  void bindings(Node* pat) {
    if (pat->kind == NK::PatName && pat->flag == 1) live.push_back(pat);
    for (auto& k : pat->kids) bindings(k.get());
  }
  void walk(Node* n) {
    if (!n) return;
    size_t mark = live.size();
    switch (n->kind) {
      case NK::Block: case NK::Case: case NK::Default: case NK::Body:
        for (auto& k : n->kids) walk(k.get());
        live.resize(mark);
        return;
      case NK::Var: live.push_back(n); return;
      case NK::Defer: live.push_back(n); return;  // a skipped `defer` would still run at scope end
      case NK::AssignOrDecl: if (n->flag == 1) live.push_back(n->kids[0].get()); return;
      case NK::TupleDecl: for (size_t i = 0; i + 1 < n->kids.size(); i++) live.push_back(n->kids[i].get()); return;
      case NK::If: walk(n->kids[1].get()); if (n->kids.size() > 2) walk(n->kids[2].get()); return;
      case NK::While: walk(n->kids[1].get()); return;
      case NK::DoWhile: walk(n->kids[0].get()); return;
      case NK::For: walk(n->kids[0].get()); walk(n->kids[3].get()); live.resize(mark); return;
      case NK::ForIn: live.push_back(n); walk(n->kids[1].get()); live.resize(mark); return;
      case NK::Switch: case NK::Match: for (size_t i = 1; i < n->kids.size(); i++) walk(n->kids[i].get()); return;
      case NK::Arm: bindings(n->kids[0].get()); walk(n->kids[1].get()); live.resize(mark); return;
      case NK::Label: atLabel[n->text] = live; walk(n->kids[0].get()); return;
      case NK::Goto: gotos.push_back({n, live}); return;
      default: return;  // expressions and simple statements declare nothing (a lambda body is its own function)
    }
  }
  static std::string nameOf(Node* d) { return d->kind == NK::Defer ? "defer" : d->text; }
};
}  // namespace

// A goto may not enter the scope of a variable: every declaration live at the label must already be live at the goto.
// This one rule covers forward jumps over a declaration, jumps into a nested block, and jumps into a loop or match arm.
void Sema::checkGotoScopes(Node* body) {
  GotoScan g;
  g.walk(body);
  for (auto& gt : g.gotos) {
    auto it = g.atLabel.find(gt.first->text);
    if (it == g.atLabel.end()) continue;  // E0230 already reported
    for (Node* d : it->second)
      if (std::find(gt.second.begin(), gt.second.end(), d) == gt.second.end()) { rep("E0460", gt.first, {GotoScan::nameOf(d)}); break; }
  }
}

void Sema::checkBodyOf(Node* f, Decl* owner, const TypeP& self, bool selfMut, bool isInit, const TypeP& fnTy, const std::string& name) {
  FuncParts fp = funcParts(f);
  if (!fp.body || !fnTy) return;
  std::map<std::string, TypeP> env;
  if (owner) for (auto& p : owner->tparamTypes) env[p->name] = p;
  if (fp.gp) {
    std::vector<TypeP> ps;
    for (auto& g : fp.gp->kids) { auto p = std::make_shared<Type>(); p->k = TyK::Param; p->name = g->text; env[g->text] = p; ps.push_back(p); }
    tpEnv.push_back(env);
    for (size_t i = 0; i < fp.gp->kids.size(); i++) for (auto& b : fp.gp->kids[i]->kids) ps[i]->bounds.push_back(resolveType(b.get()));
    tpEnv.pop_back();
  }
  tpEnv.push_back(env);
  TypeP savedSelf = selfType;
  selfType = self;
  FnCtx c;
  c.ret = fnTy->ret;
  c.name = name;
  c.scopeBase = scopes.size();
  if (self && owner) { c.hasSelf = true; c.selfType = self; c.selfDecl = owner; c.selfMut = selfMut; c.inInit = isInit; }
  collectLabels(fp.body, c.labels);
  ctxs.push_back(c);
  pushScope();
  for (size_t i = 0; i < fp.params.size() && i < fnTy->elems.size(); i++) {
    Local l; l.ty = fnTy->elems[i]; l.mut = hasMod(fp.params[i]->mods, "mut"); l.isOut = hasMod(fp.params[i]->mods, "out"); l.name = fp.params[i]->text;
    if (l.name.empty()) continue;
    if (scopes.back().count(l.name)) rep("E0211", fp.params[i], {l.name});
    else scopes.back()[l.name] = l;
  }
  block(fp.body, false);
  checkGotoScopes(fp.body);
  if (fnTy->ret && fnTy->ret->k != TyK::Void && !isErr(fnTy->ret) && !terminates(fp.body)) rep("E0410", f, {name});
  popScope();
  ctxs.pop_back();
  selfType = savedSelf;
  tpEnv.pop_back();
}

void Sema::checkFunc(Decl* fd) {
  declareDecl(fd);
  checkBodyOf(fd->node, nullptr, nullptr, false, false, fd->ty, fd->name);
}

bool Sema::run(std::vector<Node*>& units) {
  int before = d.errorCount();
  loadPrelude();
  for (Node* u : units) collect(*u);
  processImports();
  declareAll();
  for (Decl* td : {dOption, dResult}) {
    if (!td) continue;
    declareDecl(td);
    for (size_t i = 0; i < td->members.size(); i++) if (td->members[i].isVariant) bareVariants[td->members[i].name] = {td, i};
  }
  for (Decl* x : allDecls) {
    if (x->prelude) continue;
    declareDecl(x);
    curNs = x->ns;
    if (x->k == Decl::Func) checkFunc(x);
    else if (x->k == Decl::Var && x->node) {
      for (size_t i = 0; i < x->node->kids.size(); i++) {
        Node* k = x->node->kids[i].get();
        if (isTypeNode(k->kind) || k->kind == NK::Attribute || k->kind == NK::Empty) continue;
        FnCtx c; c.ret = mkPrim("void"); c.scopeBase = scopes.size();
        ctxs.push_back(c);
        if (k->kind != NK::Undefined) exprConv(k, x->ty);
        ctxs.pop_back();
      }
    }
  }
  std::vector<Decl*> owners = allDecls;
  for (auto& kv : primImpl) owners.push_back(kv.second);
  for (Decl* x : owners) {
    if (x->k == Decl::Func || x->k == Decl::Var || x->k == Decl::Alias) continue;
    curNs = x->ns;
    TypeP self;
    if (primImpl.count(x->name) && primImpl[x->name] == x) {
      static const char* prims[] = {"i8","i16","i32","i64","i128","u8","u16","u32","u64","u128","isize","usize","f32","f64","bool","Char"};
      for (auto pn : prims) if (x->name == pn) self = mkPrim(pn);
    } else self = mkNamed(x, x->tparamTypes);
    for (auto& m : x->members) {
      if (!m.isMethod || !m.node) continue;
      bool isInit = m.node->kind == NK::Init;
      TypeP fty = m.ty;
      if (isInit) { fty = std::make_shared<Type>(*m.ty); fty->ret = mkPrim("void"); }  // constructors return the object implicitly
      checkBodyOf(m.node, x, self ? self : mkErr(), m.mutSelf, isInit, fty, x->name + "::" + m.name);
    }
  }
  return d.errorCount() == before;
}

}  // namespace sema

bool analyze(DiagEngine& d, std::vector<Node*>& units, ir::Module* out, const CodegenOptions& cg) {
  sema::Sema s(d);
  bool ok = s.run(units);
  if (ok && out) return sema::lowerToIr(s, *units[0], *out, cg);
  return ok;
}

bool analyze(DiagEngine& d, Node& unit, ir::Module* out, const CodegenOptions& cg) {
  std::vector<Node*> u{&unit};
  return analyze(d, u, out, cg);
}

}  // namespace c7
