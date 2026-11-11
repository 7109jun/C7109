#include "sema_impl.hpp"

namespace c7 {
namespace sema {

namespace {
bool isDigits(const std::string& s) { return !s.empty() && s.find_first_not_of("0123456789") == std::string::npos; }
bool isLit(const TypeP& t) { return t && (t->k == TyK::IntLit || t->k == TyK::FloatLit); }
}  // namespace

ExprInfo Sema::expr(Node* n, TypeP expected) {
  ExprInfo r = exprImpl(n, expected);
  n->sty = r.ty;
  return r;
}

ExprInfo Sema::exprImpl(Node* n, TypeP expected) {
  ExprInfo r;
  r.ty = mkErr();
  switch (n->kind) {
    case NK::IntLit: {
      uint64_t v; std::string suf; bool isF;
      parseNumber(n->text, v, suf, isF);
      if (suf == "f32" || suf == "f64") { r.ty = mkPrim(suf); return r; }
      if (!suf.empty()) {
        TypeP t = mkPrim(suf);
        if (!litFits(*mkIntLit(v, false), *t)) rep("E0201", n, {suf});
        r.ty = t;
      } else r.ty = mkIntLit(v, false);
      return r;
    }
    case NK::FloatLit: {
      uint64_t v; std::string suf; bool isF;
      parseNumber(n->text, v, suf, isF);
      r.ty = (suf == "f32" || suf == "f64") ? mkPrim(suf) : mkFloatLit();
      return r;
    }
    case NK::CharLit: r.ty = mkPrim("Char"); return r;
    case NK::StrLit: case NK::RawStrLit: r.ty = strType(); return r;
    case NK::CStrLit: r.ty = mkNamed(dCStr); return r;
    case NK::BoolLit: r.ty = mkPrim("bool"); return r;
    case NK::NullLit: r.ty = mkNull(); return r;
    case NK::Undefined: return r;
    case NK::Empty: return r;
    case NK::Path: return checkPath(n, expected);
    case NK::Call: return checkCall(n, expected);
    case NK::Member: case NK::Arrow: return checkMember(n);
    case NK::Binary: return checkBinary(n, expected);
    case NK::Unary: return checkUnary(n, expected);
    case NK::Index: return checkIndex(n);
    case NK::StructLit: return checkStructLit(n, expected);
    case NK::Lambda: return checkLambda(n);
    case NK::Alloc: return checkAlloc(n);
    case NK::Try: return checkTry(n);
    case NK::Ternary: {
      checkCond(n->kids[0].get());
      ExprInfo a = expr(n->kids[1].get(), expected), b = expr(n->kids[2].get(), expected);
      if (isErr(a.ty)) { r.ty = b.ty; return r; }
      if (isErr(b.ty)) { r.ty = a.ty; return r; }
      if (isLit(a.ty) && !isLit(b.ty) && conv(a.ty, b.ty) == Conv::Ok) r.ty = b.ty;
      else if (isLit(b.ty) && !isLit(a.ty) && conv(b.ty, a.ty) == Conv::Ok) r.ty = a.ty;
      else if (conv(b.ty, a.ty) == Conv::Ok) r.ty = a.ty;
      else if (conv(a.ty, b.ty) == Conv::Ok) r.ty = b.ty;
      else { rep("E0220", n->kids[2].get(), {ts(a.ty), ts(b.ty)}); r.ty = a.ty; }
      return r;
    }
    case NK::Range: {
      TypeP t;
      for (auto& k : n->kids) {
        if (k->kind == NK::Empty) continue;
        ExprInfo e = expr(k.get());
        if (!isErr(e.ty) && !isInt(e.ty)) rep("E0225", k.get(), {ts(e.ty), ts(e.ty), n->text});
        if (!t || (t->k == TyK::IntLit && e.ty->k != TyK::IntLit)) t = e.ty;
      }
      r.ty = mkNamed(dRange, {defaultType(t ? t : mkErr(), n)});
      return r;
    }
    case NK::Cast: {
      ExprInfo e = expr(n->kids[0].get());
      TypeP to = resolveType(n->kids[1].get());
      consume(e, n->kids[0].get());
      if (!castOk(e.ty, to)) rep("E0226", n, {ts(e.ty), ts(to)});
      r.ty = to;
      return r;
    }
    case NK::Sizeof: case NK::Alignof:
      if (isTypeNode(n->kids[0]->kind)) n->kids[0]->sty = resolveType(n->kids[0].get());  // lowering reads it (type parameters are not resolvable there)
      else expr(n->kids[0].get());
      r.ty = mkPrim("usize");
      return r;
    case NK::Tuple: {
      std::vector<TypeP> es;
      bool useExp = expected && expected->k == TyK::Tuple && expected->elems.size() == n->kids.size();
      for (size_t i = 0; i < n->kids.size(); i++) {
        if (useExp) { exprConv(n->kids[i].get(), expected->elems[i]); es.push_back(expected->elems[i]); }
        else { ExprInfo e = expr(n->kids[i].get()); consume(e, n->kids[i].get()); es.push_back(defaultType(e.ty, n->kids[i].get())); }
      }
      r.ty = mkTuple(es);
      return r;
    }
    case NK::InitList: {
      if (expected && expected->k == TyK::Array) {
        if (expected->len >= 0 && (int64_t)n->kids.size() > expected->len) rep("E0215", n, {ts(expected)});
        for (auto& k : n->kids) exprConv(k.get(), expected->elem);
        r.ty = expected;
        return r;
      }
      TypeP el;
      for (auto& k : n->kids) { ExprInfo e = expr(k.get()); if (!el) el = defaultType(e.ty, k.get()); }
      r.ty = mkArray(el ? el : mkErr(), (int64_t)n->kids.size());
      return r;
    }
    case NK::OutArg: return expr(n->kids[0].get(), expected);
    default: return r;
  }
}

ExprInfo Sema::checkUnary(Node* n, TypeP expected) {
  ExprInfo r; r.ty = mkErr();
  const std::string& op = n->text;
  ExprInfo e = expr(n->kids[0].get(), op == "-" ? expected : nullptr);
  TypeP t = e.ty;
  if (op == "&") {
    r.ty = isErr(t) ? t : mkPtr(t, !(e.lv && e.wr));
    return r;
  }
  if (op == "*") {
    if (isErr(t)) return r;
    if (t->k != TyK::Ptr) { rep("E0227", n, {ts(t)}); return r; }
    r.ty = t->elem; r.lv = true; r.wr = !t->constElem;
    return r;
  }
  if (isErr(t)) return r;
  if (op == "-") {
    if (t->k == TyK::IntLit) { r.ty = mkIntLit(t->lit, !t->neg); return r; }
    if ((t->k == TyK::Int && t->sgn) || t->k == TyK::Float || t->k == TyK::FloatLit) { r.ty = t; return r; }
    if (t->k == TyK::Param && !t->bounds.empty()) { r.ty = t; return r; }
    rep("E0225", n, {ts(t), ts(t), "-"});
    return r;
  }
  if (op == "!") { expectConv(t, mkPrim("bool"), n->kids[0].get()); r.ty = mkPrim("bool"); return r; }
  if (op == "~") {
    if (!isInt(t)) { rep("E0225", n, {ts(t), ts(t), "~"}); return r; }
    r.ty = t;
    return r;
  }
  return r;
}

ExprInfo Sema::checkBinary(Node* n, TypeP expected) {
  ExprInfo r; r.ty = mkErr();
  const std::string& op = n->text;
  TypeP tb = mkPrim("bool");
  if (op == "&&" || op == "||") {
    exprConv(n->kids[0].get(), tb); exprConv(n->kids[1].get(), tb);
    r.ty = tb; return r;
  }
  bool cmp = op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=";
  bool shift = op == "<<" || op == ">>";
  bool bitop = op == "&" || op == "|" || op == "^";
  ExprInfo a = expr(n->kids[0].get(), (cmp || shift) ? nullptr : expected);
  ExprInfo b = expr(n->kids[1].get(), nullptr);
  TypeP A = a.ty, B = b.ty;
  if (isErr(A) || isErr(B)) { r.ty = cmp ? tb : (isErr(A) ? B : A); return r; }
  auto bad = [&]() { rep("E0225", n, {ts(A), ts(B), op}); r.ty = cmp ? tb : mkErr(); return r; };

  if (A->k == TyK::Named || B->k == TyK::Named) {  // operator overloading (spec 9.4)
    std::string fname = "operator" + op;
    const std::vector<Decl*>* itf = lookupGlobal(fname);
    if (itf)
      for (Decl* f : *itf) {
        if (f->k != Decl::Func) continue;
        declareDecl(f);
        if (f->ty->elems.size() == 2 && conv(A, f->ty->elems[0]) == Conv::Ok && conv(B, f->ty->elems[1]) == Conv::Ok) { n->decl = f; r.ty = f->ty->ret; return r; }
      }
    for (auto& pr : findMembers(A, fname))
      if (pr.first->ty->elems.size() == 1 && conv(B, subst(pr.first->ty->elems[0], pr.second)) == Conv::Ok) { n->mem = pr.first; r.ty = subst(pr.first->ty->ret, pr.second); return r; }
    bool eqOk = (op == "==" || op == "!=") && A->k == TyK::Named && B->k == TyK::Named && A->decl == B->decl &&
                (A->decl->k == Decl::Enum || A->decl == dStr);
    if (eqOk) { r.ty = tb; return r; }
    return bad();
  }
  if (A->k == TyK::Param || B->k == TyK::Param) {
    const TypeP& P = A->k == TyK::Param ? A : B;
    if (P->bounds.empty()) { rep("E0650", n, {ts(P), "operator" + op}); r.ty = cmp ? tb : P; return r; }
    r.ty = cmp ? tb : P;
    return r;
  }
  if (A->k == TyK::Ptr || B->k == TyK::Ptr || A->k == TyK::Null || B->k == TyK::Null) {
    if (cmp && (A->k == TyK::Ptr || A->k == TyK::Null) && (B->k == TyK::Ptr || B->k == TyK::Null)) { r.ty = tb; return r; }
    if ((op == "+" || op == "-") && A->k == TyK::Ptr && isInt(B)) { r.ty = A; return r; }
    if (op == "+" && B->k == TyK::Ptr && isInt(A)) { r.ty = B; return r; }
    if (op == "-" && A->k == TyK::Ptr && B->k == TyK::Ptr) { r.ty = mkPrim("isize"); return r; }
    return bad();
  }
  if (A->k == TyK::Bool && B->k == TyK::Bool && (op == "==" || op == "!=" || bitop)) { r.ty = tb; if (bitop) r.ty = A; return r; }
  if (A->k == TyK::Char && B->k == TyK::Char && cmp) { r.ty = tb; return r; }
  if (!isNum(A) || !isNum(B)) return bad();

  if (shift) {
    if (!isInt(A) || !isInt(B)) return bad();
    r.ty = A;
    return r;
  }
  TypeP U;
  if (A->k == TyK::IntLit && B->k == TyK::IntLit) {
    U = A;
    if (!cmp) {
      bool known = !A->neg && !B->neg;
      uint64_t v = 0; bool neg = false;
      if (known && op == "+") v = A->lit + B->lit;
      else if (known && op == "*") v = A->lit * B->lit;
      else if (known && op == "-") { if (A->lit >= B->lit) v = A->lit - B->lit; else { v = B->lit - A->lit; neg = true; } }
      else v = A->lit;
      U = mkIntLit(v, neg);
    }
  } else if (isLit(A) && !isLit(B)) { if (!expectConv(A, B, n->kids[0].get())) return bad(); U = B; }
  else if (isLit(B) && !isLit(A)) { if (!expectConv(B, A, n->kids[1].get())) return bad(); U = A; }
  else if (isLit(A) && isLit(B)) { U = A->k == TyK::FloatLit || B->k == TyK::FloatLit ? mkFloatLit() : A; }
  else if (typeEq(A, B)) U = A;
  else if (conv(A, B) == Conv::Ok) U = B;
  else if (conv(B, A) == Conv::Ok) U = A;
  else return bad();
  if ((bitop || op == "%") && !isInt(U)) { if (bitop || U->k == TyK::Float) return bad(); }
  r.ty = cmp ? tb : U;
  return r;
}

ExprInfo Sema::checkIndex(Node* n) {
  ExprInfo r; r.ty = mkErr();
  ExprInfo base = expr(n->kids[0].get());
  Node* ix = n->kids[1].get();
  TypeP bt = base.ty;
  if (isErr(bt)) { if (ix->kind != NK::Range) expr(ix); return r; }
  if (ix->kind == NK::Range) {  // slicing: a[1..3]
    for (auto& k : ix->kids) {
      if (k->kind == NK::Empty) continue;
      ExprInfo e = expr(k.get());
      if (!isErr(e.ty) && !isInt(e.ty)) rep("E0228", k.get(), {ts(e.ty)});
    }
    if (bt->k == TyK::Array) r.ty = mkSlice(bt->elem, false);
    else if (bt->k == TyK::Slice) r.ty = bt;
    else if (bt->k == TyK::Named && bt->decl == dStr) r.ty = bt;
    else rep("E0228", n, {ts(bt)});
    return r;
  }
  ExprInfo ie = expr(ix);
  if (!isErr(ie.ty) && !isInt(ie.ty)) rep("E0228", ix, {ts(ie.ty)});
  if (ie.ty && ie.ty->k == TyK::IntLit && ie.ty->neg && ie.ty->lit != 0) rep("E0201", ix, {"usize"});
  switch (bt->k) {
    case TyK::Array: r.ty = bt->elem; r.lv = base.lv; r.wr = base.wr; r.constSelf = base.constSelf; r.place = base.place; return r;
    case TyK::Slice: r.ty = bt->elem; r.lv = true; r.wr = !bt->constElem; return r;
    case TyK::Ptr: r.ty = bt->elem; r.lv = true; r.wr = !bt->constElem; return r;
    case TyK::Named: {
      if (bt->decl == dStr) { r.ty = mkPrim("u8"); r.lv = true; r.wr = false; return r; }
      for (auto& pr : findMembers(bt, "operator[]")) {
        TypeP rt = subst(pr.first->ty->ret, pr.second);
        if (rt->k == TyK::Ptr) { r.ty = rt->elem; r.lv = true; r.wr = !rt->constElem; return r; }
      }
      break;
    }
    default: break;
  }
  rep("E0228", n, {ts(bt)});
  return r;
}

ExprInfo Sema::checkMember(Node* n) {
  ExprInfo r; r.ty = mkErr();
  ExprInfo base = expr(n->kids[0].get());
  TypeP bt = base.ty;
  bool lv = base.lv, wr = base.wr, constSelf = base.constSelf;
  if (isErr(bt)) return r;
  if (bt->k == TyK::Ptr) { bt = bt->elem; lv = true; wr = !base.ty->constElem; constSelf = false; }
  r.lv = lv; r.wr = wr; r.constSelf = constSelf; r.place = n->text;
  const std::string& name = n->text;
  if (bt->k == TyK::Tuple && isDigits(name)) {
    size_t i = std::stoul(name);
    if (i < bt->elems.size()) { r.ty = bt->elems[i]; return r; }
    rep("E0214", n, {ts(bt), name});
    r.lv = false;
    return r;
  }
  if ((bt->k == TyK::Array || bt->k == TyK::Slice) && name == "len") { r.ty = mkPrim("usize"); r.lv = false; r.wr = false; return r; }
  if (bt->k == TyK::Slice && name == "ptr") { r.ty = mkPtr(bt->elem, bt->constElem); r.lv = false; r.wr = false; return r; }
  auto ms = findMembers(bt, name);
  if (ms.empty()) { rep("E0214", n, {ts(bt), name}); r.lv = false; return r; }
  const Member* m = ms[0].first;
  checkAccess(*m, m->owner, n);
  r.ty = subst(m->ty, ms[0].second);
  if (m->isMethod) { r.lv = false; r.wr = false; }
  return r;
}

ExprInfo Sema::checkStructLit(Node* n, TypeP expected) {
  ExprInfo r; r.ty = mkErr();
  Node* pn = n->kids[0].get();
  Decl* a = findTypeDecl(pn->text);
  std::vector<TypeP> targs;
  for (auto& k : pn->kids) if (k->kind == NK::TypeArgs) for (auto& t : k->kids) targs.push_back(resolveType(t.get()));
  if (a && a->k == Decl::Alias) { declareDecl(a); if (a->ty && a->ty->k == TyK::Named) { targs = a->ty->elems; a = a->ty->decl; } else a = nullptr; }
  if (!a) {
    std::string root = pn->text.substr(0, pn->text.find("::"));
    if (!(extRoots.count(root) || extNames.count(pn->text) || extNames.count("*"))) rep("E0213", pn, {pn->text});
    for (size_t i = 1; i < n->kids.size(); i++) expr(n->kids[i]->kids[0].get());
    return r;
  }
  declareDecl(a);
  if (a->k != Decl::Struct && a->k != Decl::Union && a->k != Decl::Class) {
    rep("E0223", pn, {a->name});
    for (size_t i = 1; i < n->kids.size(); i++) expr(n->kids[i]->kids[0].get());
    return r;
  }
  if (expected && expected->k == TyK::Named && expected->decl == a && targs.empty()) targs = expected->elems;
  while (targs.size() < a->tparams.size()) targs.push_back(mkErr());
  SubMap sm;
  for (size_t i = 0; i < a->tparams.size(); i++) sm[a->tparams[i]] = targs[i];
  std::vector<const Member*> fields;
  for (auto& m : a->members) if (!m.isMethod && !m.isVariant && !m.isEnumerator) fields.push_back(&m);
  size_t count = n->kids.size() - 1;
  std::set<std::string> seen;
  bool positional = count > 0 && n->kids[1]->text.empty();
  if (positional) {
    if (count != fields.size()) rep("E0215", n, {a->name});
    for (size_t i = 0; i < count; i++) {
      Node* v = n->kids[i + 1]->kids[0].get();
      if (i < fields.size()) { checkAccess(*fields[i], a, v); exprConv(v, subst(fields[i]->ty, sm)); }
      else expr(v);
    }
  } else {
    for (size_t i = 1; i < n->kids.size(); i++) {
      Node* fi = n->kids[i].get();
      const Member* fm = nullptr;
      for (auto* f : fields) if (f->name == fi->text) fm = f;
      if (!fm) { rep("E0214", fi, {a->name, fi->text}); expr(fi->kids[0].get()); continue; }
      checkAccess(*fm, a, fi);
      seen.insert(fi->text);
      exprConv(fi->kids[0].get(), subst(fm->ty, sm));
    }
    if (a->k == Decl::Union) { if (count != 1) rep("E0215", n, {a->name}); }
    else for (auto* f : fields) if (!seen.count(f->name)) rep("E0217", n, {f->name, a->name});
  }
  r.ty = mkNamed(a, targs);
  return r;
}

ExprInfo Sema::checkLambda(Node* n) {
  ExprInfo r; r.ty = mkErr();
  auto t = std::make_shared<Type>();
  t->k = TyK::Closure;
  std::vector<Node*> params;
  Node* body = nullptr;
  TypeP ret = mkPrim("void");
  for (auto& k : n->kids) {
    if (k->kind == NK::Capture) {
      if (k->text != "=" && k->text != "&") { Local* l; ExprInfo f; if (!lookupLocal(k->text, l) && !lookupSelfMember(k->text, f)) rep("E0212", k.get(), {k->text}); }
    } else if (k->kind == NK::Param) {
      params.push_back(k.get());
      TypeP pt = resolveType(k->kids[0].get());
      t->elems.push_back(pt); t->pmods.push_back(k->mods);
    } else if (k->kind == NK::Block) body = k.get();
    else if (isTypeNode(k->kind)) ret = resolveType(k.get());
  }
  t->ret = ret;
  r.ty = t;
  FnCtx c;
  if (!ctxs.empty()) c = ctxs.back();
  c.ret = ret; c.loops = c.switches = c.defers = 0; c.labels.clear();
  c.scopeBase = scopes.size();
  collectLabels(body, c.labels);
  ctxs.push_back(c);
  pushScope();
  for (size_t i = 0; i < params.size(); i++) {
    Local l; l.ty = t->elems[i]; l.mut = hasMod(params[i]->mods, "mut"); l.isOut = hasMod(params[i]->mods, "out"); l.name = params[i]->text;
    if (!l.name.empty()) scopes.back()[l.name] = l;
  }
  // captured variables stay visible: lambdas see the enclosing scopes
  ctxs.back().scopeBase = ctxs.size() >= 2 ? ctxs[ctxs.size() - 2].scopeBase : 0;
  if (body) {
    block(body, false);
    if (!(ret->k == TyK::Void) && !terminates(body)) rep("E0410", n, {"<lambda>"});
  }
  popScope();
  ctxs.pop_back();
  return r;
}

ExprInfo Sema::checkAlloc(Node* n) {
  ExprInfo r; r.ty = mkErr();
  Node *typeNode = nullptr, *alloc = nullptr, *count = nullptr, *initArgs = nullptr;
  for (auto& k : n->kids) {
    if (k->kind == NK::Empty && k->text == "allocator") alloc = k.get();
    else if (k->kind == NK::Empty && k->text == "count") count = k.get();
    else if (k->kind == NK::Empty && k->text == "init-args") initArgs = k.get();
    else if (isTypeNode(k->kind)) typeNode = k.get();
  }
  if (alloc) expr(alloc->kids[0].get());
  if (count) { ExprInfo c = expr(count->kids[0].get()); if (!isErr(c.ty) && !isInt(c.ty)) rep("E0220", count, {"usize", ts(c.ty)}); }
  TypeP t = typeNode ? resolveType(typeNode) : mkErr();
  std::vector<Arg> args;
  if (initArgs) {
    for (auto& k : initArgs->kids) { Arg a; ExprInfo e = expr(k.get()); a.ty = e.ty; a.lv = e.lv; a.wr = e.wr; a.node = k.get(); args.push_back(a); }
  }
  if (t->k == TyK::Named) {
    declareDecl(t->decl);
    std::vector<FnCand> cs;
    SubMap base;
    for (size_t i = 0; i < t->decl->tparams.size() && i < t->elems.size(); i++) base[t->decl->tparams[i]] = t->elems[i];
    for (auto& m : t->decl->members) if (m.isMethod && m.node && m.node->kind == NK::Init) cs.push_back(candFromMember(&m, base));
    if (!cs.empty()) resolveCall(cs, args, n, t->decl->name);
    else if (!args.empty()) rep("E0222", n, {"0", std::to_string(args.size())});
  }
  n->ownerTargs.clear();
  if (t->k == TyK::Named) for (auto& e : t->elems) n->ownerTargs.push_back(e);
  r.ty = isErr(t) ? t : mkPtr(t, false);
  return r;
}

ExprInfo Sema::checkTry(Node* n) {
  ExprInfo r; r.ty = mkErr();
  ExprInfo e = expr(n->kids[0].get());
  TypeP t = e.ty;
  if (isErr(t)) return r;
  if (t->k != TyK::Named || t->decl != dResult) { rep("E0612", n, {ts(t)}); return r; }
  TypeP fr = ctxs.empty() ? nullptr : ctxs.back().ret;
  if (!fr || isErr(fr)) { if (!fr) rep("E0611", n); return t->elems.empty() ? r : (r.ty = t->elems[0], r); }
  if (fr->k != TyK::Named || fr->decl != dResult) { rep("E0611", n); }
  else if (t->elems.size() == 2 && fr->elems.size() == 2) {
    TypeP from = t->elems[1], to = fr->elems[1];
    if (!typeEq(from, to) && !(dConvert && implements(to, dConvert))) rep("E0610", n, {ts(from), ts(to)});
  }
  if (!t->elems.empty()) r.ty = t->elems[0];
  return r;
}

}  // namespace sema
}  // namespace c7
