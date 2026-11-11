#include "sema_impl.hpp"

namespace c7 {
namespace sema {

namespace {
std::vector<std::string> splitSegs(const std::string& s) {
  std::vector<std::string> out;
  size_t i = 0;
  for (;;) {
    size_t j = s.find("::", i);
    if (j == std::string::npos) { out.push_back(s.substr(i)); break; }
    out.push_back(s.substr(i, j - i));
    i = j + 2;
  }
  return out;
}
std::string joinSegs(const std::vector<std::string>& v, size_t n) {
  std::string s;
  for (size_t i = 0; i < n; i++) s += (i ? "::" : "") + v[i];
  return s;
}
bool isLiteral(const TypeP& t) { return t && (t->k == TyK::IntLit || t->k == TyK::FloatLit); }

struct PathHit {
  enum K { None, LocalVar, SelfField, SelfMethod, GVar, GFuncs, TypeMember, Variant, TParam, Ext, SelfRef } k = None;
  Local* local = nullptr;
  ExprInfo field;
  Decl* decl = nullptr;
  std::vector<Decl*> funcs;
  std::string member;
  std::vector<TypeP> targs;
  size_t vidx = 0;
  TypeP tparam;
  std::vector<std::pair<const Member*, SubMap>> methods;
};

PathHit resolvePathHit(Sema& s, Node* n, TypeP expected) {
  PathHit h;
  std::vector<std::string> segs = splitSegs(n->text);
  std::vector<TypeP> targs;
  for (auto& k : n->kids) if (k->kind == NK::TypeArgs) for (auto& a : k->kids) targs.push_back(s.resolveType(a.get()));

  if (segs.size() == 1) {
    const std::string& name = segs[0];
    Local* lc = nullptr;
    if (name == "self" && !s.ctxs.empty() && s.ctxs.back().hasSelf && !s.lookupLocal(name, lc)) { h.k = PathHit::SelfRef; return h; }
    if (s.lookupLocal(name, lc)) { h.k = PathHit::LocalVar; h.local = lc; return h; }
    ExprInfo f;
    if (s.lookupSelfMember(name, f)) { h.k = PathHit::SelfField; h.field = f; return h; }
    if (!s.ctxs.empty() && s.ctxs.back().hasSelf && s.ctxs.back().selfDecl) {
      auto ms = s.findMembers(s.ctxs.back().selfType, name);
      if (!ms.empty()) { h.k = PathHit::SelfMethod; h.methods = ms; return h; }
    }
    const std::vector<Decl*>* itv = s.lookupGlobal(name);
    if (itv) {
      for (Decl* x : *itv) if (x->k == Decl::Func) h.funcs.push_back(x);
      if (!h.funcs.empty()) { h.k = PathHit::GFuncs; return h; }
      for (Decl* x : *itv) {
        if (x->k == Decl::Var) { h.k = PathHit::GVar; h.decl = x; return h; }
        h.k = PathHit::TypeMember; h.decl = x; h.targs = targs; return h;  // a type name (ctor call, struct literal)
      }
    }
    auto bv = s.bareVariants.find(name);
    if (bv != s.bareVariants.end()) { h.k = PathHit::Variant; h.decl = bv->second.decl; h.vidx = bv->second.idx; h.targs = targs; return h; }
    if (expected && expected->k == TyK::Named && expected->decl->k == Decl::Tagged) {
      s.declareDecl(expected->decl);
      for (size_t i = 0; i < expected->decl->members.size(); i++)
        if (expected->decl->members[i].isVariant && expected->decl->members[i].name == name) {
          h.k = PathHit::Variant; h.decl = expected->decl; h.vidx = i; h.targs = expected->elems; return h;
        }
    }
    for (auto it2 = s.tpEnv.rbegin(); it2 != s.tpEnv.rend(); ++it2) if (it2->count(name)) { h.k = PathHit::TParam; h.tparam = it2->at(name); return h; }
    if (s.extRoots.count(name) || s.extNames.count(name) || s.extNames.count("*")) { h.k = PathHit::Ext; return h; }
    s.rep("E0212", n, {name});
    return h;
  }

  // qualified: longest prefix that names a declaration
  for (size_t i = segs.size(); i >= 1; i--) {
    const std::vector<Decl*>* itq = s.lookupGlobal(joinSegs(segs, i));
    if (!itq) continue;
    if (i == segs.size()) {
      for (Decl* x : *itq) if (x->k == Decl::Func) h.funcs.push_back(x);
      if (!h.funcs.empty()) { h.k = PathHit::GFuncs; return h; }
      for (Decl* x : *itq) if (x->k == Decl::Var) { h.k = PathHit::GVar; h.decl = x; return h; }
      h.k = PathHit::TypeMember; h.decl = (*itq)[0]; h.targs = targs; return h;
    }
    Decl* a = nullptr;
    for (Decl* x : *itq) if (x->k != Decl::Func && x->k != Decl::Var) a = x;
    if (!a || i != segs.size() - 1) continue;
    s.declareDecl(a);
    h.k = PathHit::TypeMember; h.decl = a; h.member = segs.back(); h.targs = targs;
    if (a->k == Decl::Alias) {
      if (a->ty && a->ty->k == TyK::Named) { h.decl = a->ty->decl; h.targs = a->ty->elems; }
    }
    return h;
  }
  const std::string& root = segs[0];
  for (auto it2 = s.tpEnv.rbegin(); it2 != s.tpEnv.rend(); ++it2)
    if (it2->count(root) && segs.size() == 2) {
      h.k = PathHit::TParam; h.tparam = it2->at(root); h.member = segs[1];
      h.methods = s.findMembers(h.tparam, segs[1]);
      if (h.methods.empty()) s.rep("E0214", n, {root, segs[1]});
      return h;
    }
  if (s.extRoots.count(root) || s.extNames.count(root) || s.extNames.count("*")) { h.k = PathHit::Ext; return h; }
  // primitive type used as a path root: i32::MIN etc.
  static const char* prims[] = {"i8","i16","i32","i64","i128","u8","u16","u32","u64","u128","isize","usize","f32","f64","bool","Char"};
  for (auto p : prims) if (root == p) { h.k = PathHit::Ext; return h; }
  s.rep("E0212", n, {n->text});
  return h;
}
}  // namespace

// ---- conversions -----------------------------------------------------------------------------------------

Conv Sema::conv(const TypeP& from, const TypeP& to) {
  if (isErr(from) || isErr(to)) return Conv::Ok;
  if (from->k == TyK::IntLit) {
    if (to->k == TyK::Int) return litFits(*from, *to) ? Conv::Ok : Conv::OutOfRange;
    if (to->k == TyK::Float) return Conv::Ok;
    if (to->k == TyK::IntLit) return Conv::Ok;
    return Conv::Mismatch;
  }
  if (from->k == TyK::FloatLit) return to->k == TyK::Float || to->k == TyK::FloatLit ? Conv::Ok : Conv::Mismatch;
  if (from->k == TyK::Null) return to->k == TyK::Ptr || to->k == TyK::Null || to->k == TyK::Fn ? Conv::Ok : Conv::Mismatch;
  if (typeEq(from, to)) return Conv::Ok;
  if (from->k == TyK::Param || to->k == TyK::Param) return Conv::Mismatch;
  if (from->k == TyK::Int && to->k == TyK::Int) {
    if (to->bits > from->bits && (from->sgn == to->sgn || (!from->sgn && to->sgn))) return Conv::Ok;
    return Conv::NeedCast;
  }
  if (from->k == TyK::Int && to->k == TyK::Float) {
    if ((from->bits <= 16 && to->bits >= 32) || (from->bits <= 32 && to->bits == 64)) return Conv::Ok;
    return Conv::NeedCast;
  }
  if (from->k == TyK::Float && to->k == TyK::Float) return from->bits < to->bits ? Conv::Ok : Conv::NeedCast;
  auto numLike = [](const TypeP& t) { return t->k == TyK::Int || t->k == TyK::Float || t->k == TyK::Bool || t->k == TyK::Char || (t->k == TyK::Named && t->decl->k == Decl::Enum); };
  if (numLike(from) && numLike(to)) return Conv::NeedCast;
  if (from->k == TyK::Ptr && to->k == TyK::Ptr) {
    if (typeEq(from->elem, to->elem)) return (from->constElem && !to->constElem) ? Conv::ConstDiscard : Conv::Ok;
    if (to->elem->k == TyK::Named && to->elem->decl->k == Decl::Interface && implements(from->elem, to->elem->decl))
      return (from->constElem && !to->constElem) ? Conv::ConstDiscard : Conv::Ok;
    return Conv::NeedCast;
  }
  if (from->k == TyK::Array && to->k == TyK::Slice) return typeEq(from->elem, to->elem) ? Conv::Ok : Conv::Mismatch;
  if (from->k == TyK::Slice && to->k == TyK::Slice && typeEq(from->elem, to->elem))
    return (from->constElem && !to->constElem) ? Conv::ConstDiscard : Conv::Ok;
  if (from->k == TyK::Tuple && to->k == TyK::Tuple && from->elems.size() == to->elems.size()) {
    for (size_t i = 0; i < from->elems.size(); i++) if (conv(from->elems[i], to->elems[i]) != Conv::Ok) return Conv::Mismatch;
    return Conv::Ok;
  }
  if ((from->k == TyK::Fn || from->k == TyK::Closure) && (to->k == TyK::Fn || to->k == TyK::Closure)) {
    if (from->elems.size() == to->elems.size() && typeEq(from->ret, to->ret)) {
      for (size_t i = 0; i < from->elems.size(); i++) if (!typeEq(from->elems[i], to->elems[i])) return Conv::Mismatch;
      return Conv::Ok;
    }
  }
  return Conv::Mismatch;
}

bool Sema::expectConv(const TypeP& from, const TypeP& to, Node* at) {
  switch (conv(from, to)) {
    case Conv::Ok: if (at && !isErr(to)) at->cty = to; return true;
    case Conv::NeedCast: rep("E0221", at, {ts(from), ts(to)}); return false;
    case Conv::ConstDiscard: rep("E0302", at, {ts(from), ts(to)}); return false;
    case Conv::OutOfRange: rep("E0201", at, {ts(to)}); return false;
    case Conv::Mismatch: rep("E0220", at, {ts(to), ts(from)}); return false;
  }
  return false;
}

TypeP Sema::defaultType(const TypeP& t, Node* at) {
  if (!t) return mkErr();
  if (t->k == TyK::IntLit) {
    TypeP i32 = mkPrim("i32");
    if (!litFits(*t, *i32)) rep("E0201", at, {"i32"});
    if (at) at->cty = i32;
    return i32;
  }
  if (t->k == TyK::FloatLit) { TypeP f = mkPrim("f64"); if (at) at->cty = f; return f; }
  if (t->k == TyK::Null) return mkErr();
  return t;
}

void Sema::consume(const ExprInfo& e, Node* at, bool moveOut) {
  if (!e.lv || !isDrop(e.ty)) return;
  if (moveOut && e.localName) return;
  rep("E0480", at, {ts(e.ty)});
}

ExprInfo Sema::exprConv(Node* n, const TypeP& to, bool moveOut) {
  ExprInfo e = expr(n, to);
  consume(e, n, moveOut);
  expectConv(e.ty, to, n);
  return e;
}

// ---- scopes ---------------------------------------------------------------------------------------------

bool Sema::lookupLocal(const std::string& name, Local*& out) {
  size_t base = ctxs.empty() ? 0 : ctxs.back().scopeBase;
  for (size_t i = scopes.size(); i > base; i--) {
    auto it = scopes[i - 1].find(name);
    if (it != scopes[i - 1].end()) { out = &it->second; return true; }
  }
  return false;
}

bool Sema::lookupSelfMember(const std::string& name, ExprInfo& out) {
  if (ctxs.empty() || !ctxs.back().hasSelf || !ctxs.back().selfDecl) return false;
  FnCtx& c = ctxs.back();
  for (auto& m : c.selfDecl->members) {
    if (m.name != name || m.isMethod || m.isVariant || m.isEnumerator) continue;
    out.ty = m.ty; out.lv = true;
    out.wr = c.selfMut || c.inInit;
    out.constSelf = !out.wr;
    out.place = name;
    return true;
  }
  return false;
}

bool Sema::inOwner(Decl* owner) { return !ctxs.empty() && ctxs.back().selfDecl == owner; }

void Sema::checkAccess(const Member& m, Decl* owner, Node* at) {
  if (!m.pub && !inOwner(owner) && !owner->prelude) rep("E0621", at, {m.name});
}

void Sema::bindLocal(Node* at, const std::string& name, TypeP ty, bool mut, bool warnShadow) {
  if (scopes.empty()) pushScope();
  if (scopes.back().count(name)) { rep("E0211", at, {name}); return; }
  if (warnShadow) {
    Local* o = nullptr;
    if (lookupLocal(name, o)) rep("W0106", at, {name});
  }
  Local l; l.ty = ty; l.mut = mut; l.name = name;
  scopes.back()[name] = l;
}

FnCand Sema::candFromDecl(Decl* fd) {
  declareDecl(fd);
  FnCand c;
  c.fn = fd->ty; c.tparams = fd->tparams; c.decl = fd;
  for (auto& p : fd->tparamTypes) c.bounds.push_back(p->bounds);
  return c;
}

FnCand Sema::candFromMember(const Member* m, const SubMap& base) {
  FnCand c;
  c.fn = m->ty; c.tparams = m->tparams; c.bounds = m->tbounds; c.mem = m; c.base = base;
  return c;
}

// ---- arguments and overload resolution ------------------------------------------------------------------------

std::vector<Arg> Sema::evalArgs(Node* call, size_t from, const std::vector<TypeP>* hints) {
  std::vector<Arg> out;
  for (size_t i = from; i < call->kids.size(); i++) {
    Node* a = call->kids[i].get();
    Arg r; r.node = a;
    size_t hi = i - from;
    TypeP hint = (hints && hi < hints->size()) ? (*hints)[hi] : nullptr;
    if (a->kind == NK::OutArg) {
      ExprInfo e = expr(a->kids[0].get(), hint);
      r.isOut = true; r.ty = e.ty; r.lv = e.lv; r.wr = e.wr;
      if (!e.lv || !e.wr) rep("E0231", a, {"argument must be a mutable place"});
    } else {
      ExprInfo e = expr(a, hint);
      r.ty = e.ty; r.lv = e.lv; r.wr = e.wr;
    }
    out.push_back(r);
  }
  return out;
}

TypeP Sema::resolveCall(std::vector<FnCand>& cands, std::vector<Arg>& args, Node* at, const std::string& name) {
  struct Ev { bool ok = false; int rank = 0; SubMap m; std::vector<TypeP> params; };
  std::vector<Ev> evs(cands.size());
  for (size_t ci = 0; ci < cands.size(); ci++) {
    FnCand& c = cands[ci];
    Ev& ev = evs[ci];
    ev.m = c.base;
    std::set<std::string> vars(c.tparams.begin(), c.tparams.end());
    for (auto& p : c.fn->elems) ev.params.push_back(subst(p, c.base));
    if (!vars.empty()) {
      for (size_t j = 0; j < ev.params.size() && j < args.size(); j++) unify(ev.params[j], args[j].ty, ev.m, vars);
      for (auto& v : vars) if (!ev.m.count(v)) ev.m[v] = mkErr();
      for (auto& p : ev.params) p = subst(p, ev.m);
    }
    if (ev.params.size() != args.size()) continue;
    bool ok = true, exact = true;
    for (size_t j = 0; j < args.size() && ok; j++) {
      bool wantOut = hasMod(c.fn->pmods[j], "out");
      if (wantOut != args[j].isOut) { ok = false; break; }
      if (conv(args[j].ty, ev.params[j]) != Conv::Ok) ok = false;
      else {
        bool ex = typeEq(args[j].ty, ev.params[j]) && !isLiteral(args[j].ty) && args[j].ty->k != TyK::Null;
        if (args[j].ty->k == TyK::IntLit && ev.params[j]->k == TyK::Int && ev.params[j]->name == "i32") ex = true;
        if (args[j].ty->k == TyK::FloatLit && ev.params[j]->k == TyK::Float && ev.params[j]->name == "f64") ex = true;
        if (isErr(args[j].ty)) ex = true;
        exact = exact && ex;
      }
    }
    ev.ok = ok;
    ev.rank = (exact ? 0 : 1) + (vars.empty() ? 0 : 2);
  }
  int best = -1;
  for (size_t i = 0; i < evs.size(); i++) if (evs[i].ok && (best < 0 || evs[i].rank < evs[best].rank)) best = (int)i;
  if (best < 0) {
    if (cands.size() == 1) {
      FnCand& c = cands[0];
      Ev& ev = evs[0];
      if (ev.params.size() != args.size()) {
        rep("E0222", at, {std::to_string(ev.params.size()), std::to_string(args.size())});
      } else {
        for (size_t j = 0; j < args.size(); j++) {
          if (hasMod(c.fn->pmods[j], "out") != args[j].isOut) {
            rep("E0231", args[j].node, {hasMod(c.fn->pmods[j], "out") ? "parameter is `out`; pass `out <place>`" : "argument is `out` but parameter is not"});
          } else expectConv(args[j].ty, ev.params[j], args[j].node);
        }
      }
      return subst(c.fn->ret, ev.m);
    }
    rep("E0631", at, {name});
    return mkErr();
  }
  for (size_t i = best + 1; i < evs.size(); i++)
    if (evs[i].ok && evs[i].rank == evs[best].rank) { rep("E0630", at, {name}); break; }
  FnCand& c = cands[best];
  Ev& ev = evs[best];
  if (at) { at->decl = c.decl; at->mem = c.mem; }
  if (at && !c.tparams.empty()) { at->targs.clear(); for (auto& tp : c.tparams) at->targs.push_back(ev.m.count(tp) ? ev.m[tp] : mkErr()); }
  for (size_t j = 0; j < args.size() && j < ev.params.size(); j++)
    if (!args[j].isOut && args[j].node && !isErr(ev.params[j])) args[j].node->cty = ev.params[j];
  for (size_t i = 0; i < c.tparams.size(); i++) {
    if (i >= c.bounds.size()) break;
    TypeP actual = ev.m[c.tparams[i]];
    for (auto& b : c.bounds[i]) {
      if (b->k != TyK::Named || isErr(actual)) continue;
      if (!implements(actual, b->decl)) rep("E0650", at, {ts(actual), b->decl->name});
    }
  }
  if (name != "bitcopy")
    for (size_t j = 0; j < args.size(); j++)
      if (!args[j].isOut && args[j].lv && isDrop(args[j].ty)) rep("E0480", args[j].node, {ts(args[j].ty)});
  return subst(c.fn->ret, ev.m);
}

// ---- expressions -------------------------------------------------------------------------------------------

ExprInfo Sema::checkPath(Node* n, TypeP expected) {
  ExprInfo r; r.ty = mkErr();
  PathHit h = resolvePathHit(*this, n, expected);
  switch (h.k) {
    case PathHit::LocalVar:
      r.ty = h.local->ty; r.lv = true; r.wr = h.local->mut || h.local->isOut; r.localName = true; r.place = n->text;
      break;
    case PathHit::SelfField: r = h.field; break;
    case PathHit::SelfRef: r.ty = mkPtr(ctxs.back().selfType, !(ctxs.back().selfMut || ctxs.back().inInit)); break;
    case PathHit::SelfMethod: if (h.methods[0].first->ty) r.ty = subst(h.methods[0].first->ty, h.methods[0].second); break;
    case PathHit::GVar: {
      declareDecl(h.decl);
      r.ty = h.decl->ty; r.lv = true; r.place = n->text;
      r.wr = hasMod(h.decl->node->mods, "mut");
      break;
    }
    case PathHit::GFuncs: { FnCand c = candFromDecl(h.funcs[0]); r.ty = c.fn; break; }
    case PathHit::TypeMember: {
      Decl* a = h.decl;
      declareDecl(a);
      if (h.member.empty()) { rep("E0212", n, {n->text}); break; }  // bare type name used as a value
      std::vector<TypeP> args = h.targs;
      while (args.size() < a->tparams.size()) args.push_back(mkErr());
      if (a->k == Decl::Enum) {
        for (auto& m : a->members) if (m.isEnumerator && m.name == h.member) { r.ty = mkNamed(a); return r; }
        rep("E0214", n, {a->name, h.member});
        break;
      }
      if (a->k == Decl::Tagged) {
        for (auto& m : a->members) if (m.isVariant && m.name == h.member) { if (m.payload.empty()) r.ty = mkNamed(a, args); return r; }
      }
      auto ms = findMembers(mkNamed(a, args), h.member);
      if (ms.empty()) { rep("E0214", n, {a->name, h.member}); break; }
      r.ty = subst(ms[0].first->ty, ms[0].second);
      break;
    }
    case PathHit::Variant: {
      Member& m = h.decl->members[h.vidx];
      std::vector<TypeP> args = h.targs;
      while (args.size() < h.decl->tparams.size()) args.push_back(mkErr());
      if (m.payload.empty()) r.ty = mkNamed(h.decl, args);
      break;
    }
    case PathHit::TParam:
      if (!h.methods.empty()) r.ty = subst(h.methods[0].first->ty, h.methods[0].second);
      break;
    default: break;
  }
  return r;
}

ExprInfo Sema::checkCall(Node* n, TypeP expected) {
  ExprInfo r; r.ty = mkErr();
  Node* callee = n->kids[0].get();
  auto callValue = [&](const TypeP& ft, const std::string& nm) {
    if (isErr(ft)) { evalArgs(n, 1, nullptr); return mkErr(); }
    if (ft->k != TyK::Fn && ft->k != TyK::Closure) { rep("E0223", callee, {nm}); evalArgs(n, 1, nullptr); return mkErr(); }
    std::vector<Arg> args = evalArgs(n, 1, &ft->elems);
    FnCand c; c.fn = ft;
    if (c.fn->pmods.size() < ft->elems.size()) c.fn = std::make_shared<Type>(*ft), c.fn->pmods.assign(ft->elems.size(), "");
    std::vector<FnCand> cs{c};
    return resolveCall(cs, args, n, nm);
  };
  auto hintsOf = [&](const std::vector<FnCand>& cs) -> std::vector<TypeP> {
    if (cs.size() != 1 || !cs[0].tparams.empty()) return {};
    std::vector<TypeP> h;
    for (auto& p : cs[0].fn->elems) h.push_back(subst(p, cs[0].base));
    return h;
  };

  if (callee->kind == NK::Path) {
    PathHit h = resolvePathHit(*this, callee, expected);
    switch (h.k) {
      case PathHit::LocalVar: callee->sty = h.local->ty; r.ty = callValue(h.local->ty, callee->text); return r;
      case PathHit::SelfField: callee->sty = h.field.ty; r.ty = callValue(h.field.ty, callee->text); return r;
      case PathHit::GVar: declareDecl(h.decl); callee->sty = h.decl->ty; r.ty = callValue(h.decl->ty, callee->text); return r;
      case PathHit::GFuncs: {
        std::vector<FnCand> cs;
        for (Decl* f : h.funcs) cs.push_back(candFromDecl(f));
        auto hints = hintsOf(cs);
        std::vector<Arg> args = evalArgs(n, 1, hints.empty() ? nullptr : &hints);
        r.ty = resolveCall(cs, args, n, callee->text);
        return r;
      }
      case PathHit::SelfMethod: {
        std::vector<FnCand> cs;
        for (auto& pr : h.methods) {
          cs.push_back(candFromMember(pr.first, pr.second));
          if (pr.first->mutSelf && !ctxs.back().selfMut && !ctxs.back().inInit) rep("E0303", callee, {callee->text});
        }
        auto hints = hintsOf(cs);
        std::vector<Arg> args = evalArgs(n, 1, hints.empty() ? nullptr : &hints);
        n->ownerTargs.clear();
        if (ctxs.back().selfType && ctxs.back().selfType->k == TyK::Named) for (auto& e : ctxs.back().selfType->elems) n->ownerTargs.push_back(e);
        r.ty = resolveCall(cs, args, n, callee->text);
        return r;
      }
      case PathHit::TParam: {
        std::vector<FnCand> cs;
        for (auto& pr : h.methods) cs.push_back(candFromMember(pr.first, pr.second));
        std::vector<Arg> args = evalArgs(n, 1, nullptr);
        if (cs.empty()) return r;
        r.ty = resolveCall(cs, args, n, callee->text);
        return r;
      }
      case PathHit::Variant: {
        Decl* td = h.decl;
        Member& m = td->members[h.vidx];
        std::vector<TypeP> targs = h.targs;
        if (expected && expected->k == TyK::Named && expected->decl == td && targs.empty()) targs = expected->elems;
        while (targs.size() < td->tparams.size()) targs.push_back(mkErr());
        SubMap sm; std::set<std::string> vars(td->tparams.begin(), td->tparams.end());
        for (size_t i = 0; i < td->tparams.size(); i++) if (!isErr(targs[i])) sm[td->tparams[i]] = targs[i];
        std::vector<TypeP> hints;
        for (auto& p : m.payload) hints.push_back(subst(p, sm));
        std::vector<Arg> args = evalArgs(n, 1, &hints);
        bool unit = args.empty() && m.payload.size() == 1 && m.payload[0]->k == TyK::Param && vars.count(m.payload[0]->name) &&
                    (!sm.count(m.payload[0]->name) || sm[m.payload[0]->name]->k == TyK::Void);
        if (unit) sm[m.payload[0]->name] = mkPrim("void");  // Ok() carries no value
        if (args.size() != m.payload.size() && !unit) { rep("E0222", n, {std::to_string(m.payload.size()), std::to_string(args.size())}); }
        for (size_t j = 0; j < args.size() && j < m.payload.size(); j++) unify(m.payload[j], args[j].ty, sm, vars);
        for (size_t j = 0; j < args.size() && j < m.payload.size(); j++) {
          if (args[j].lv && isDrop(args[j].ty)) rep("E0480", args[j].node, {ts(args[j].ty)});
          expectConv(args[j].ty, subst(m.payload[j], sm), args[j].node);
        }
        std::vector<TypeP> fin;
        for (auto& v : td->tparams) fin.push_back(sm.count(v) ? sm[v] : mkErr());
        r.ty = mkNamed(td, fin);
        n->mem = &m;  // variant constructor: lowering reads the Member
        return r;
      }
      case PathHit::TypeMember: {
        Decl* a = h.decl;
        declareDecl(a);
        std::vector<TypeP> targs = h.targs;
        if (expected && expected->k == TyK::Named && expected->decl == a && targs.empty()) targs = expected->elems;
        while (targs.size() < a->tparams.size()) targs.push_back(mkErr());
        checkTypeArgBounds(a, targs, callee);
        TypeP self = mkNamed(a, targs);
        n->ownerTargs.clear();
        for (auto& e : targs) n->ownerTargs.push_back(e);
        SubMap base;
        for (size_t i = 0; i < a->tparams.size(); i++) base[a->tparams[i]] = targs[i];
        std::vector<FnCand> cs;
        if (h.member.empty()) {  // constructor call: Name(args)
          for (auto& m : a->members) if (m.isMethod && m.node && m.node->kind == NK::Init) cs.push_back(candFromMember(&m, base));
          if (cs.empty()) {
            if (a->k == Decl::Class && !a->hasDrop) { std::vector<Arg> args = evalArgs(n, 1, nullptr); if (!args.empty()) rep("E0222", n, {"0", std::to_string(args.size())}); r.ty = self; return r; }
            rep("E0223", callee, {a->name});
            evalArgs(n, 1, nullptr);
            return r;
          }
          auto hints = hintsOf(cs);
          std::vector<Arg> args = evalArgs(n, 1, hints.empty() ? nullptr : &hints);
          resolveCall(cs, args, n, a->name);
          r.ty = self;
          return r;
        }
        if (a->k == Decl::Tagged) {
          for (size_t i = 0; i < a->members.size(); i++)
            if (a->members[i].isVariant && a->members[i].name == h.member) {
              // reuse the bare-variant path
              std::vector<TypeP> hs;
              for (auto& p : a->members[i].payload) hs.push_back(p);
              SubMap sm; std::set<std::string> vars(a->tparams.begin(), a->tparams.end());
              for (size_t k = 0; k < a->tparams.size(); k++) if (!isErr(targs[k])) sm[a->tparams[k]] = targs[k];
              for (auto& p : hs) p = subst(p, sm);
              std::vector<Arg> args = evalArgs(n, 1, &hs);
              bool unit = args.empty() && hs.size() == 1 && a->members[i].payload[0]->k == TyK::Param && vars.count(a->members[i].payload[0]->name) &&
                          (!sm.count(a->members[i].payload[0]->name) || sm[a->members[i].payload[0]->name]->k == TyK::Void);
              if (unit) sm[a->members[i].payload[0]->name] = mkPrim("void");
              if (args.size() != hs.size() && !unit) rep("E0222", n, {std::to_string(hs.size()), std::to_string(args.size())});
              for (size_t j = 0; j < args.size() && j < hs.size(); j++) unify(a->members[i].payload[j], args[j].ty, sm, vars);
              for (size_t j = 0; j < args.size() && j < hs.size(); j++) expectConv(args[j].ty, subst(a->members[i].payload[j], sm), args[j].node);
              std::vector<TypeP> fin;
              for (auto& v : a->tparams) fin.push_back(sm.count(v) ? sm[v] : mkErr());
              r.ty = mkNamed(a, fin);
              n->mem = &a->members[i];
              return r;
            }
        }
        if (a->k == Decl::Enum) { rep("E0223", callee, {callee->text}); evalArgs(n, 1, nullptr); return r; }
        auto ms = findMembers(self, h.member);
        if (ms.empty()) { rep("E0214", callee, {a->name, h.member}); evalArgs(n, 1, nullptr); return r; }
        for (auto& pr : ms) cs.push_back(candFromMember(pr.first, pr.second));
        for (auto& pr : ms) checkAccess(*pr.first, a, callee);
        auto hints = hintsOf(cs);
        std::vector<Arg> args = evalArgs(n, 1, hints.empty() ? nullptr : &hints);
        // Type::method(&obj, ...) style is not supported; static methods only.
        if (!cs.empty() && cs[0].mem && !cs[0].mem->isStatic) { rep("E0223", callee, {callee->text}); return r; }
        r.ty = resolveCall(cs, args, n, callee->text);
        return r;
      }
      case PathHit::Ext: evalArgs(n, 1, nullptr); return r;
      default: evalArgs(n, 1, nullptr); return r;
    }
  }

  if (callee->kind == NK::Member || callee->kind == NK::Arrow) {
    ExprInfo recv = expr(callee->kids[0].get());
    TypeP rt = recv.ty;
    bool writable = recv.wr, constSelf = recv.constSelf;
    if (!isErr(rt) && rt->k == TyK::Ptr) { writable = !rt->constElem; constSelf = rt->constElem; rt = rt->elem; }
    if (isErr(rt)) { evalArgs(n, 1, nullptr); return r; }
    auto ms = findMembers(rt, callee->text);
    if (ms.empty()) {
      rep("E0214", callee, {ts(rt), callee->text});
      evalArgs(n, 1, nullptr);
      return r;
    }
    const Member* first = ms[0].first;
    if (!first->isMethod) { r.ty = callValue(subst(first->ty, ms[0].second), callee->text); return r; }
    std::vector<FnCand> cs;
    for (auto& pr : ms) {
      checkAccess(*pr.first, pr.first->owner, callee);
      cs.push_back(candFromMember(pr.first, pr.second));
      if (pr.first->mutSelf && !pr.first->isStatic && !writable) {
        if (constSelf) rep("E0303", callee, {callee->text});
        else rep("E0301", callee, {recv.place.empty() ? callee->kids[0]->text : recv.place});
      }
    }
    auto hints = hintsOf(cs);
    std::vector<Arg> args = evalArgs(n, 1, hints.empty() ? nullptr : &hints);
    n->ownerTargs.clear();
    if (rt->k == TyK::Named) for (auto& e : rt->elems) n->ownerTargs.push_back(e);
    r.ty = resolveCall(cs, args, n, callee->text);
    return r;
  }

  ExprInfo ce = expr(callee);
  r.ty = callValue(ce.ty, "expression");
  return r;
}

}  // namespace sema
}  // namespace c7
