#include <cctype>

#include "sema_impl.hpp"

namespace c7 {
namespace sema {

namespace {
// Built-in declarations visible everywhere (spec 8.1, 9.1, 9.5, 5.5). Bodies are provided by the
// runtime in stage 4; here they only describe signatures.
const char* kPrelude = R"C7(
type CChar = i8;
type CInt = i32;
type CUInt = u32;
type CLong = i64;
type CULong = u64;
type CLongLong = i64;
type CULongLong = u64;
type CSize = usize;
struct Str { const u8* ptr; usize len; }
struct CStr { const CChar* ptr; }
struct Chars { Str s; usize pos; }
struct Range<T> { T start; T end; }
tagged union Option<T> { Some(T value), None }
tagged union Result<T, E> { Ok(T value), Err(E error) }
interface Iterator<T> { mut Option<T> next(); }
interface Convert<From> { static Self convert(From v); }
interface Allocator {
    void* alloc_raw(usize size, usize align);
    void free_raw(void* p, usize size, usize align);
    void* realloc_raw(void* p, usize old_size, usize new_size, usize align);
}
void free_closure<T>(T f);
)C7";
}  // namespace

bool isTypeNode(NK k) {
  switch (k) {
    case NK::TypePrim: case NK::TypeName: case NK::TypePtr: case NK::TypeArray: case NK::TypeSlice:
    case NK::TypeTuple: case NK::TypeFn: case NK::TypeClosure: case NK::TypeConst: case NK::TypeNoalias:
      return true;
    default: return false;
  }
}

bool hasMod(const std::string& mods, const char* m) {
  std::istringstream is(mods);
  std::string w;
  while (is >> w) if (w == m) return true;
  return false;
}

FuncParts funcParts(Node* f) {
  FuncParts p;
  for (auto& k : f->kids) {
    if (k->kind == NK::Attribute) p.attrs.push_back(k.get());
    else if (k->kind == NK::GenericParams) p.gp = k.get();
    else if (k->kind == NK::Param) p.params.push_back(k.get());
    else if (k->kind == NK::Block) p.body = k.get();
    else if (isTypeNode(k->kind) && !p.ret && f->kind == NK::Func) p.ret = k.get();
  }
  return p;
}

void parseNumber(const std::string& text, uint64_t& val, std::string& suffix, bool& isFloat) {
  val = 0; suffix.clear(); isFloat = false;
  size_t i = 0, n = text.size();
  int base = 10;
  if (n > 1 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) { base = 16; i = 2; }
  else if (n > 1 && text[0] == '0' && (text[1] == 'b' || text[1] == 'B')) { base = 2; i = 2; }
  else if (n > 1 && text[0] == '0' && (text[1] == 'o' || text[1] == 'O')) { base = 8; i = 2; }
  if (base == 10) {
    size_t j = i;
    while (j < n && (isdigit((unsigned char)text[j]) || text[j] == '_')) j++;
    if (j < n && text[j] == '.') isFloat = true;
    if (j < n && (text[j] == 'e' || text[j] == 'E') && j + 1 < n && (isdigit((unsigned char)text[j + 1]) || text[j + 1] == '+' || text[j + 1] == '-')) isFloat = true;
    if (isFloat) {
      size_t k = text.find_first_of("fF", i);
      if (k != std::string::npos) suffix = text.substr(k);
      return;
    }
    for (; i < j; i++) if (text[i] != '_') val = val * 10 + (text[i] - '0');
    suffix = text.substr(j);
    if (suffix == "f32" || suffix == "f64") isFloat = true;
    return;
  }
  for (; i < n; i++) {
    char c = text[i];
    if (c == '_') continue;
    int dv = isdigit((unsigned char)c) ? c - '0' : (isxdigit((unsigned char)c) && base == 16) ? tolower(c) - 'a' + 10 : -1;
    if (dv < 0 || dv >= base) break;
    val = val * base + dv;
  }
  suffix = text.substr(i);
}

// ---- prelude, collection --------------------------------------------------------------------

void Sema::loadPrelude() {
  int fid = d.addFile("<core>", kPrelude);
  NodeP u = parse(d, lex(d, fid));
  inPrelude = true;
  collect(*u);
  keep.push_back(std::move(u));
  inPrelude = false;
  auto get = [&](const char* n) -> Decl* { auto it = globals.find(n); return it == globals.end() ? nullptr : it->second[0]; };
  dStr = get("Str"); dCStr = get("CStr"); dResult = get("Result"); dOption = get("Option");
  dRange = get("Range"); dConvert = get("Convert");
}

Decl* Sema::newDecl(Decl::K k, const std::string& name, const std::string& ns, Node* n) {
  auto dd = std::make_unique<Decl>();
  dd->k = k; dd->name = name; dd->full = ns.empty() ? name : ns + "::" + name;
  dd->node = n; dd->prelude = inPrelude; dd->ns = ns;
  Decl* p = dd.get();
  pool.push_back(std::move(dd));
  if (k != Decl::Func) {
    for (Decl* e : globals[p->full])
      if (e->k != Decl::Func) { rep("E0211", n, {name}); return p; }  // not registered: avoids cascades
    if (!inPrelude && k != Decl::Var && !name.empty() && !isupper((unsigned char)name[0])) rep("W0101", n, {name});
  }
  globals[p->full].push_back(p);  // registered under its full name only; short names resolve via curNs/imports
  allDecls.push_back(p);
  return p;
}

void Sema::collectItem(Node* n, const std::string& ns, const std::string& abi) {
  switch (n->kind) {
    case NK::Struct: newDecl(Decl::Struct, n->text, ns, n); break;
    case NK::Union: newDecl(Decl::Union, n->text, ns, n); break;
    case NK::Enum: newDecl(Decl::Enum, n->text, ns, n); break;
    case NK::Tagged: newDecl(Decl::Tagged, n->text, ns, n); break;
    case NK::Class: newDecl(Decl::Class, n->text, ns, n); break;
    case NK::Interface: newDecl(Decl::Interface, n->text, ns, n); break;
    case NK::Alias: newDecl(Decl::Alias, n->text, ns, n); break;
    case NK::Var: { Decl* v = newDecl(Decl::Var, n->text, ns, n); v->abi = abi; break; }
    case NK::Func: {
      Decl* f = newDecl(Decl::Func, n->text, ns, n);
      f->abi = abi;
      for (Node* a : funcParts(n).attrs) if (a->text.rfind("noreturn", 0) == 0) f->noreturn = true;
      break;
    }
    case NK::ExternBlock:
      for (auto& k : n->kids) if (k->kind == NK::Func || k->kind == NK::Var) collectItem(k.get(), ns, n->text);
      break;
    case NK::Impl: pendingImpls.push_back({n, ns}); break;
    default: break;
  }
}

void Sema::collect(Node& unit) {
  std::string ns;
  for (auto& k : unit.kids) {
    if (k->kind == NK::Namespace) { ns = k->text; continue; }
    if (k->kind == NK::Import) { pendingImports.push_back(k.get()); continue; }
    collectItem(k.get(), ns, "");
  }
}

// ---- type resolution ---------------------------------------------------------------------------

const std::vector<Decl*>* Sema::lookupGlobal(const std::string& name) {
  auto tryFind = [&](const std::string& k) -> const std::vector<Decl*>* {
    auto it = globals.find(k);
    return (it != globals.end() && !it->second.empty()) ? &it->second : nullptr;
  };
  std::string ns = curNs;
  while (!ns.empty()) {  // innermost enclosing namespace first
    if (auto* r = tryFind(ns + "::" + name)) return r;
    size_t p = ns.rfind("::");
    ns = p == std::string::npos ? "" : ns.substr(0, p);
  }
  if (auto* r = tryFind(name)) return r;
  auto a = aliases.find(name);
  if (a != aliases.end() && !a->second.empty()) return &a->second;
  return tryFind("std::" + name);  // implicit std prelude, lowest priority
}

void Sema::processImports() {
  for (Node* imp : pendingImports) {
    if (hasMod(imp->mods, "c")) { extRoots.insert(imp->kids.empty() ? "c" : imp->kids[0]->text); continue; }
    const std::string& path = imp->text;
    std::string root = path.substr(0, path.find("::"));
    std::string alias = imp->kids.empty() ? "" : imp->kids[0]->text;
    if (hasMod(imp->mods, "glob")) {
      std::string prefix = path + "::";
      bool any = false;
      for (auto& kv : globals) {
        if (kv.first.rfind(prefix, 0) != 0) continue;
        std::string rest = kv.first.substr(prefix.size());
        if (rest.find("::") != std::string::npos) continue;
        for (Decl* d : kv.second) aliases[rest].push_back(d);
        any = true;
      }
      if (!any) { extRoots.insert(root); extNames.insert("*"); }
    } else {
      std::string leaf = alias.empty() ? path.substr(path.rfind(':') == std::string::npos ? 0 : path.rfind(':') + 1) : alias;
      auto it = globals.find(path);
      if (it != globals.end()) for (Decl* d : it->second) aliases[leaf].push_back(d);
      else { extRoots.insert(root); extNames.insert(leaf); }
    }
  }
  pendingImports.clear();
}

Decl* Sema::findTypeDecl(const std::string& name) {
  const std::vector<Decl*>* v = lookupGlobal(name);
  if (!v) return nullptr;
  for (Decl* x : *v) if (x->k != Decl::Func && x->k != Decl::Var) return x;
  return nullptr;
}

int64_t Sema::constEval(Node* n) {
  if (n->kind == NK::IntLit) { uint64_t v; std::string s; bool f; parseNumber(n->text, v, s, f); return (int64_t)v; }
  if (n->kind == NK::Binary && n->kids.size() == 2) {
    int64_t a = constEval(n->kids[0].get()), b = constEval(n->kids[1].get());
    if (a < 0 || b < 0) return -1;
    if (n->text == "+") return a + b;
    if (n->text == "*") return a * b;
    if (n->text == "-") return a - b;
    if (n->text == "<<") return a << b;
  }
  return -1;
}

TypeP Sema::resolveType(Node* n) {
  auto strip = [&](Node*& x, bool& isConst) {
    while (x->kind == NK::TypeConst || x->kind == NK::TypeNoalias) {
      if (x->kind == NK::TypeConst) isConst = true;
      x = x->kids[0].get();
    }
  };
  switch (n->kind) {
    case NK::TypePrim: return mkPrim(n->text);
    case NK::TypeConst: case NK::TypeNoalias: return resolveType(n->kids[0].get());
    case NK::TypePtr: { Node* b = n->kids[0].get(); bool c = false; strip(b, c); return mkPtr(resolveType(b), c); }
    case NK::TypeSlice: { Node* b = n->kids[0].get(); bool c = false; strip(b, c); return mkSlice(resolveType(b), c); }
    case NK::TypeArray: { Node* b = n->kids[0].get(); bool c = false; strip(b, c); return mkArray(resolveType(b), constEval(n->kids[1].get())); }
    case NK::TypeTuple: { std::vector<TypeP> e; for (auto& k : n->kids) e.push_back(resolveType(k.get())); return mkTuple(std::move(e)); }
    case NK::TypeFn: case NK::TypeClosure: {
      auto t = std::make_shared<Type>();
      t->k = n->kind == NK::TypeFn ? TyK::Fn : TyK::Closure;
      for (size_t i = 0; i + 1 < n->kids.size(); i++) { t->elems.push_back(resolveType(n->kids[i].get())); t->pmods.push_back(""); }
      t->ret = resolveType(n->kids.back().get());
      return t;
    }
    case NK::TypeName: {
      const std::string& name = n->text;
      if (name.find("::") == std::string::npos) {
        for (auto it = tpEnv.rbegin(); it != tpEnv.rend(); ++it) { auto f = it->find(name); if (f != it->end()) return f->second; }
        if (name == "Self" && selfType) return selfType;
      }
      Decl* dd = findTypeDecl(name);
      if (dd) {
        if (dd->k == Decl::Alias) { declareDecl(dd); return dd->ty ? dd->ty : mkErr(); }
        std::vector<TypeP> args;
        for (auto& k : n->kids) if (k->kind == NK::TypeArgs) for (auto& a : k->kids) args.push_back(resolveType(a.get()));
        if (!args.empty()) checkTypeArgBounds(dd, args, n);
        return mkNamed(dd, std::move(args));
      }
      std::string root = name.substr(0, name.find("::"));
      if (extRoots.count(root) || extNames.count(name) || extNames.count("*")) return mkErr();
      rep("E0213", n, {name});
      return mkErr();
    }
    default: return mkErr();
  }
}

TypeP Sema::fnTypeOf(Node* f, std::vector<std::string>* tp, std::vector<std::vector<TypeP>>* bounds) {
  FuncParts fp = funcParts(f);
  std::map<std::string, TypeP> env;
  std::vector<TypeP> pts;
  if (fp.gp) {
    for (auto& g : fp.gp->kids) { auto p = std::make_shared<Type>(); p->k = TyK::Param; p->name = g->text; env[g->text] = p; pts.push_back(p); }
    tpEnv.push_back(env);
    for (size_t i = 0; i < fp.gp->kids.size(); i++)
      for (auto& b : fp.gp->kids[i]->kids) pts[i]->bounds.push_back(resolveType(b.get()));
  }
  auto t = std::make_shared<Type>();
  t->k = TyK::Fn;
  t->ret = fp.ret ? resolveType(fp.ret) : mkPrim("void");
  for (Node* p : fp.params) {
    t->elems.push_back(p->kids.empty() ? mkErr() : resolveType(p->kids[0].get()));
    t->pmods.push_back(p->mods);
  }
  if (fp.gp) tpEnv.pop_back();
  if (tp) for (auto& p : pts) { tp->push_back(p->name); if (bounds) bounds->push_back(p->bounds); }
  return t;
}

Member Sema::makeMethod(Node* f, Decl* owner, bool defaultPub) {
  Member m;
  m.node = f; m.owner = owner; m.isMethod = true;
  // `init` and `drop` are public unless marked `priv` (spec 6.5 calls them from outside the class).
  if (f->kind == NK::Init || f->kind == NK::Drop) defaultPub = true;
  m.pub = hasMod(f->mods, "pub") ? true : hasMod(f->mods, "priv") ? false : defaultPub;
  if (f->kind == NK::Init) {
    m.name = "init"; m.isStatic = true; m.mutSelf = true;
    m.ty = fnTypeOf(f, nullptr, nullptr);
    m.ty->ret = mkNamed(owner, owner->tparamTypes);
  } else if (f->kind == NK::Drop) {
    m.name = "drop"; m.mutSelf = true;
    m.ty = fnTypeOf(f, nullptr, nullptr);
    owner->hasDrop = true;
  } else {
    m.name = f->text;
    m.isStatic = hasMod(f->mods, "static");
    m.mutSelf = hasMod(f->mods, "mut");
    m.ty = fnTypeOf(f, &m.tparams, &m.tbounds);
  }
  return m;
}

// ---- declaration ---------------------------------------------------------------------------------

struct NsGuard {
  std::string& ref; std::string saved;
  NsGuard(std::string& r, const std::string& n) : ref(r), saved(r) { ref = n; }
  ~NsGuard() { ref = saved; }
};

void Sema::declareDecl(Decl* dd) {
  if (dd->declared) return;
  dd->declared = true;
  NsGuard nsg(curNs, dd->ns);
  Node* n = dd->node;
  inPrelude = dd->prelude;
  if (dd->k == Decl::Func) {
    dd->ty = fnTypeOf(n, &dd->tparams, nullptr);
    FuncParts fp = funcParts(n);
    if (fp.gp) {  // keep Param types with bounds for call-site checking
      std::map<std::string, TypeP> env;
      for (auto& g : fp.gp->kids) { auto p = std::make_shared<Type>(); p->k = TyK::Param; p->name = g->text; env[g->text] = p; dd->tparamTypes.push_back(p); }
      tpEnv.push_back(env);
      for (size_t i = 0; i < fp.gp->kids.size(); i++)
        for (auto& b : fp.gp->kids[i]->kids) dd->tparamTypes[i]->bounds.push_back(resolveType(b.get()));
      tpEnv.pop_back();
    }
    return;
  }
  std::map<std::string, TypeP> env;
  for (auto& k : n->kids) {
    if (k->kind != NK::GenericParams) continue;
    for (auto& g : k->kids) {
      auto p = std::make_shared<Type>(); p->k = TyK::Param; p->name = g->text;
      env[g->text] = p; dd->tparams.push_back(g->text); dd->tparamTypes.push_back(p);
    }
  }
  tpEnv.push_back(env);
  size_t gi = 0;
  for (auto& k : n->kids)
    if (k->kind == NK::GenericParams)
      for (auto& g : k->kids) { for (auto& b : g->kids) dd->tparamTypes[gi]->bounds.push_back(resolveType(b.get())); gi++; }
  TypeP savedSelf = selfType;
  switch (dd->k) {
    case Decl::Alias:
      for (auto& k : n->kids) if (isTypeNode(k->kind)) dd->ty = resolveType(k.get());
      break;
    case Decl::Var:
      for (auto& k : n->kids) if (isTypeNode(k->kind)) { dd->ty = resolveType(k.get()); break; }
      if (!dd->ty) dd->ty = mkErr();
      break;
    case Decl::Struct: case Decl::Union: case Decl::Class: {
      selfType = mkNamed(dd, dd->tparamTypes);
      bool isClass = dd->k == Decl::Class;
      for (auto& k : n->kids) {
        if (k->kind == NK::Var) {
          Member m; m.name = k->text; m.owner = dd; m.node = k.get();
          m.pub = hasMod(k->mods, "pub") ? true : hasMod(k->mods, "priv") ? false : !isClass;
          m.ty = (!k->kids.empty() && isTypeNode(k->kids[0]->kind)) ? resolveType(k->kids[0].get()) : mkErr();
          dd->members.push_back(std::move(m));
        } else if (k->kind == NK::Func || k->kind == NK::Init || k->kind == NK::Drop) {
          dd->members.push_back(makeMethod(k.get(), dd, !isClass));
        }
      }
      break;
    }
    case Decl::Interface: {
      auto sp = std::make_shared<Type>(); sp->k = TyK::Param; sp->name = "Self";
      sp->bounds.push_back(mkNamed(dd, dd->tparamTypes));
      selfType = sp;
      for (auto& k : n->kids) {
        if (k->kind == NK::Func) dd->members.push_back(makeMethod(k.get(), dd, true));
        else if (isTypeNode(k->kind)) {
          TypeP e = resolveType(k.get());
          if (e->k == TyK::Named && e->decl->k == Decl::Interface) dd->extends.push_back(e->decl);
          else if (!isErr(e)) rep("E0641", k.get(), {ts(e)});
        }
      }
      break;
    }
    case Decl::Enum: {
      for (auto& k : n->kids) {
        if (isTypeNode(k->kind)) dd->ty = resolveType(k.get());
        else if (k->kind == NK::Enumerator) { Member m; m.name = k->text; m.isEnumerator = true; m.owner = dd; m.node = k.get(); dd->members.push_back(std::move(m)); }
      }
      if (!dd->ty) dd->ty = mkPrim("i32");
      break;
    }
    case Decl::Tagged: {
      selfType = mkNamed(dd, dd->tparamTypes);
      for (auto& k : n->kids) {
        if (k->kind != NK::Variant) continue;
        Member m; m.name = k->text; m.isVariant = true; m.owner = dd; m.node = k.get();
        for (auto& p : k->kids) { m.payload.push_back(resolveType(p->kids[0].get())); m.payloadNames.push_back(p->text); }
        dd->members.push_back(std::move(m));
      }
      break;
    }
    default: break;
  }
  selfType = savedSelf;
  tpEnv.pop_back();
}

void Sema::declareImpl(const PendingImpl& pi) {
  Node* n = pi.node;
  NsGuard nsg(curNs, pi.ns);
  inPrelude = false;
  std::vector<Node*> types;
  Node* gp = nullptr;
  for (auto& k : n->kids) {
    if (k->kind == NK::GenericParams) gp = k.get();
    else if (isTypeNode(k->kind)) types.push_back(k.get());
  }
  if (types.empty()) return;
  bool isFor = n->mods.find("for") != std::string::npos && types.size() >= 2;
  Node* targetNode = isFor ? types[1] : types[0];
  std::map<std::string, TypeP> env;
  if (gp) for (auto& g : gp->kids) { auto p = std::make_shared<Type>(); p->k = TyK::Param; p->name = g->text; env[g->text] = p; }
  tpEnv.push_back(env);
  TypeP target = resolveType(targetNode);
  TypeP ifaceT = isFor ? resolveType(types[0]) : nullptr;
  tpEnv.pop_back();
  if (isErr(target)) return;

  Decl* owner = nullptr;
  std::map<std::string, TypeP> env2;
  TypeP self = target;
  if (target->k == TyK::Named) {
    owner = target->decl;
    declareDecl(owner);
    for (size_t i = 0; i < target->elems.size() && i < owner->tparamTypes.size(); i++)
      if (target->elems[i]->k == TyK::Param) env2[target->elems[i]->name] = owner->tparamTypes[i];
    self = mkNamed(owner, owner->tparamTypes);
  } else {
    Decl*& pd = primImpl[ts(target)];
    if (!pd) { auto up = std::make_unique<Decl>(); up->k = Decl::Struct; up->name = ts(target); up->full = up->name; up->declared = true; up->ns = pi.ns; pd = up.get(); pool.push_back(std::move(up)); }
    owner = pd;
  }
  tpEnv.push_back(env2);
  TypeP savedSelf = selfType;
  selfType = self;
  ImplCheck ic;
  ic.target = target; ic.iface = nullptr; ic.node = n;
  if (isFor) {
    if (!ifaceT || ifaceT->k != TyK::Named || ifaceT->decl->k != Decl::Interface) {
      if (ifaceT && !isErr(ifaceT)) rep("E0641", types[0], {ts(ifaceT)});
    } else {
      declareDecl(ifaceT->decl);
      impls.insert({implKey(target), ifaceT->decl});
      ic.iface = ifaceT->decl;
    }
  }
  for (auto& k : n->kids) {
    if (k->kind == NK::Func || k->kind == NK::Init || k->kind == NK::Drop) {
      Member m = makeMethod(k.get(), owner, true);
      ic.provided.insert(m.name);
      owner->members.push_back(std::move(m));
    }
  }
  selfType = savedSelf;
  tpEnv.pop_back();
  if (ic.iface) implChecks.push_back(std::move(ic));
}

void Sema::finishImplChecks() {
  for (auto& ic : implChecks) {
    for (auto& m : ic.iface->members) {
      if (!m.isMethod || ic.provided.count(m.name)) continue;
      rep("E0640", ic.node, {ts(ic.target), ic.iface->name, m.name});
    }
  }
}

void Sema::declareAll() {
  for (size_t i = 0; i < allDecls.size(); i++) declareDecl(allDecls[i]);
  for (auto& pi : pendingImpls) declareImpl(pi);
  finishImplChecks();
  implsReady = true;
  for (auto& pb : pendingBounds) checkTypeArgBounds(pb.decl, pb.args, pb.at);
  pendingBounds.clear();
  for (auto& kv : globals) {  // identical overloads
    std::vector<Decl*> fs;
    for (Decl* x : kv.second) if (x->k == Decl::Func && !x->prelude && x->full == kv.first) fs.push_back(x);
    for (size_t i = 0; i < fs.size(); i++)
      for (size_t j = i + 1; j < fs.size(); j++) {
        auto &a = fs[i]->ty, &b = fs[j]->ty;
        if (!a || !b || a->elems.size() != b->elems.size()) continue;
        bool same = true;
        for (size_t k = 0; k < a->elems.size() && same; k++) same = !isErr(a->elems[k]) && !isErr(b->elems[k]) && typeEq(a->elems[k], b->elems[k]);
        if (same) rep("E0211", fs[j]->node, {fs[j]->name});
      }
  }
}

// ---- queries -----------------------------------------------------------------------------------------

std::string Sema::implKey(const TypeP& t) {
  if (t->k == TyK::Named) return t->decl->full;
  return ts(t);
}

void Sema::checkTypeArgBounds(Decl* dd, const std::vector<TypeP>& args, Node* at) {
  if (!implsReady) { pendingBounds.push_back({dd, args, at}); return; }
  for (size_t i = 0; i < args.size() && i < dd->tparamTypes.size(); i++)
    for (auto& b : dd->tparamTypes[i]->bounds) {
      if (b->k != TyK::Named || isErr(args[i])) continue;
      if (!implements(args[i], b->decl)) rep("E0650", at, {ts(args[i]), b->decl->name});
    }
}

bool Sema::implements(const TypeP& t, Decl* iface) {
  if (isErr(t)) return true;
  std::function<bool(Decl*)> viaExt = [&](Decl* x) {
    if (x == iface) return true;
    for (Decl* e : x->extends) if (viaExt(e)) return true;
    return false;
  };
  if (t->k == TyK::Param) {
    for (auto& b : t->bounds) if (b->k == TyK::Named && viaExt(b->decl)) return true;
    return false;
  }
  std::string key = implKey(t);
  for (auto& p : impls) if (p.first == key && viaExt(p.second)) return true;
  return false;
}

std::vector<std::pair<const Member*, SubMap>> Sema::findMembers(const TypeP& t0, const std::string& name) {
  std::vector<std::pair<const Member*, SubMap>> out;
  TypeP t = t0;
  if (!t) return out;
  std::function<void(Decl*, const std::vector<TypeP>&, const SubMap&)> addFrom = [&](Decl* dd, const std::vector<TypeP>& args, const SubMap& extra) {
    declareDecl(dd);
    SubMap sm = extra;
    for (size_t i = 0; i < dd->tparams.size() && i < args.size(); i++) sm[dd->tparams[i]] = args[i];
    for (auto& m : dd->members) if (m.name == name && !m.isVariant && !m.isEnumerator) out.push_back({&m, sm});
    for (Decl* e : dd->extends) addFrom(e, {}, extra);
  };
  if (t->k == TyK::Named) addFrom(t->decl, t->elems, {});
  else if (t->k == TyK::Param) {
    SubMap self; self["Self"] = t;
    for (auto& b : t->bounds) if (b->k == TyK::Named) addFrom(b->decl, b->elems, self);
  } else {
    auto it = primImpl.find(ts(t));
    if (it != primImpl.end()) addFrom(it->second, {}, {});
  }
  return out;
}

bool Sema::isDropType(const TypeP& t, std::set<Decl*>& seen) {
  if (!t) return false;
  switch (t->k) {
    case TyK::Named: {
      Decl* dd = t->decl;
      if (!seen.insert(dd).second) return false;
      declareDecl(dd);
      if (dd->hasDrop) return true;
      for (auto& m : dd->members) {
        if (m.isMethod || m.isEnumerator) continue;
        if (m.isVariant) { for (auto& p : m.payload) if (isDropType(p, seen)) return true; }
        else if (isDropType(m.ty, seen)) return true;
      }
      return false;
    }
    case TyK::Tuple: for (auto& e : t->elems) if (isDropType(e, seen)) return true; return false;
    case TyK::Array: return isDropType(t->elem, seen);
    default: return false;
  }
}

}  // namespace sema
}  // namespace c7
