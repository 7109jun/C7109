#include "sema_core.hpp"

namespace c7 {
namespace sema {

TypeP mkErr() { auto t = std::make_shared<Type>(); t->k = TyK::Error; return t; }

TypeP mkPrim(const std::string& n0) {
  std::string n = n0 == "int" ? "i32" : n0 == "long" ? "i64" : n0;
  auto t = std::make_shared<Type>();
  t->name = n;
  if (n == "void") t->k = TyK::Void;
  else if (n == "bool") { t->k = TyK::Bool; t->bits = 8; }
  else if (n == "Char") { t->k = TyK::Char; t->bits = 32; }
  else if (n == "f32" || n == "f64") { t->k = TyK::Float; t->bits = n == "f32" ? 32 : 64; }
  else {
    t->k = TyK::Int;
    t->sgn = n[0] == 'i';
    // isize/usize are 64-bit on the first-class targets (x86-64, AArch64); x86 32-bit is stage 11.
    t->bits = (n == "isize" || n == "usize") ? 64 : std::stoi(n.substr(1));
  }
  return t;
}
TypeP mkPtr(TypeP e, bool c) { auto t = std::make_shared<Type>(); t->k = TyK::Ptr; t->elem = std::move(e); t->constElem = c; return t; }
TypeP mkSlice(TypeP e, bool c) { auto t = std::make_shared<Type>(); t->k = TyK::Slice; t->elem = std::move(e); t->constElem = c; return t; }
TypeP mkArray(TypeP e, int64_t n) { auto t = std::make_shared<Type>(); t->k = TyK::Array; t->elem = std::move(e); t->len = n; return t; }
TypeP mkTuple(std::vector<TypeP> e) { auto t = std::make_shared<Type>(); t->k = TyK::Tuple; t->elems = std::move(e); return t; }
TypeP mkNamed(Decl* d, std::vector<TypeP> args) { auto t = std::make_shared<Type>(); t->k = TyK::Named; t->decl = d; t->elems = std::move(args); return t; }
TypeP mkIntLit(uint64_t v, bool neg) { auto t = std::make_shared<Type>(); t->k = TyK::IntLit; t->lit = v; t->neg = neg; return t; }
TypeP mkFloatLit() { auto t = std::make_shared<Type>(); t->k = TyK::FloatLit; return t; }
TypeP mkNull() { auto t = std::make_shared<Type>(); t->k = TyK::Null; return t; }

bool isErr(const TypeP& t) { return !t || t->k == TyK::Error; }
bool isInt(const TypeP& t) { return t && (t->k == TyK::Int || t->k == TyK::IntLit); }
bool isNum(const TypeP& t) { return t && (t->k == TyK::Int || t->k == TyK::Float || t->k == TyK::IntLit || t->k == TyK::FloatLit); }

std::string ts(const TypeP& t) {
  if (!t) return "<error>";
  auto list = [&](const std::vector<TypeP>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); i++) s += (i ? ", " : "") + ts(v[i]);
    return s;
  };
  switch (t->k) {
    case TyK::Error: return "<error>";
    case TyK::Void: case TyK::Bool: case TyK::Int: case TyK::Float: case TyK::Char: return t->name;
    case TyK::Ptr: return (t->constElem ? "const " : "") + ts(t->elem) + "*";
    case TyK::Slice: return (t->constElem ? "const " : "") + ts(t->elem) + "[]";
    case TyK::Array: return ts(t->elem) + "[" + (t->len >= 0 ? std::to_string(t->len) : "?") + "]";
    case TyK::Tuple: return "(" + list(t->elems) + ")";
    case TyK::Fn: return "fn(" + list(t->elems) + ") -> " + ts(t->ret);
    case TyK::Closure: return "closure(" + list(t->elems) + ") -> " + ts(t->ret);
    case TyK::Named: return t->decl->name + (t->elems.empty() ? "" : "<" + list(t->elems) + ">");
    case TyK::Null: return "null";
    case TyK::IntLit: return "{integer}";
    case TyK::FloatLit: return "{float}";
    case TyK::Param: return t->name;
  }
  return "?";
}

bool typeEq(const TypeP& a, const TypeP& b) {
  if (isErr(a) || isErr(b)) return true;  // error type is a wildcard: suppresses cascades
  if (a->k != b->k) return false;
  auto all = [](const std::vector<TypeP>& x, const std::vector<TypeP>& y) {
    if (x.size() != y.size()) return false;
    for (size_t i = 0; i < x.size(); i++) if (!typeEq(x[i], y[i])) return false;
    return true;
  };
  switch (a->k) {
    case TyK::Void: case TyK::Bool: case TyK::Int: case TyK::Float: case TyK::Char: return a->name == b->name;
    case TyK::Ptr: case TyK::Slice: return a->constElem == b->constElem && typeEq(a->elem, b->elem);
    case TyK::Array: return typeEq(a->elem, b->elem) && (a->len < 0 || b->len < 0 || a->len == b->len);
    case TyK::Tuple: return all(a->elems, b->elems);
    case TyK::Fn: case TyK::Closure: return all(a->elems, b->elems) && typeEq(a->ret, b->ret);
    case TyK::Named: return a->decl == b->decl && (a->elems.empty() || b->elems.empty() || all(a->elems, b->elems));
    case TyK::Param: return a->name == b->name;
    case TyK::Null: return true;
    default: return false;
  }
}

TypeP subst(const TypeP& t, const SubMap& m) {
  if (m.empty() || !t) return t;
  switch (t->k) {
    case TyK::Param: { auto it = m.find(t->name); return it == m.end() ? t : it->second; }
    case TyK::Ptr: case TyK::Slice: case TyK::Array: {
      auto c = std::make_shared<Type>(*t); c->elem = subst(t->elem, m); return c;
    }
    case TyK::Tuple: case TyK::Fn: case TyK::Closure: case TyK::Named: {
      auto c = std::make_shared<Type>(*t);
      for (auto& e : c->elems) e = subst(e, m);
      if (c->ret) c->ret = subst(c->ret, m);
      return c;
    }
    default: return t;
  }
}

void unify(const TypeP& pat, const TypeP& act, SubMap& m, const std::set<std::string>& vars) {
  if (!pat || !act) return;
  if (pat->k == TyK::Param && vars.count(pat->name)) {
    if (m.count(pat->name)) return;
    TypeP bound = act;
    if (act->k == TyK::IntLit) bound = mkPrim("i32");
    else if (act->k == TyK::FloatLit) bound = mkPrim("f64");
    else if (act->k == TyK::Null || act->k == TyK::Error) return;
    m[pat->name] = bound;
    return;
  }
  if (pat->k == TyK::Slice && act->k == TyK::Array) { unify(pat->elem, act->elem, m, vars); return; }  // T[N] converts to T[]
  if (pat->k != act->k) return;
  switch (pat->k) {
    case TyK::Ptr: case TyK::Slice: case TyK::Array: unify(pat->elem, act->elem, m, vars); break;
    case TyK::Tuple: case TyK::Fn: case TyK::Closure: case TyK::Named:
      if (pat->k == TyK::Named && pat->decl != act->decl) break;
      for (size_t i = 0; i < pat->elems.size() && i < act->elems.size(); i++) unify(pat->elems[i], act->elems[i], m, vars);
      if (pat->ret && act->ret) unify(pat->ret, act->ret, m, vars);
      break;
    default: break;
  }
}

bool litFits(const Type& lit, const Type& to) {
  if (to.k != TyK::Int) return false;
  if (to.bits > 64) return !(lit.neg && !to.sgn) || lit.lit == 0;
  unsigned __int128 v = lit.lit;
  if (lit.neg) {
    if (!to.sgn) return lit.lit == 0;
    return v <= ((unsigned __int128)1 << (to.bits - 1));
  }
  unsigned __int128 mx = to.sgn ? (((unsigned __int128)1 << (to.bits - 1)) - 1) : (((unsigned __int128)1 << to.bits) - 1);
  return v <= mx;
}

bool castOk(const TypeP& from, const TypeP& to) {
  if (isErr(from) || isErr(to)) return true;
  if (typeEq(from, to)) return true;
  auto numeric = [](const TypeP& t) {
    return t->k == TyK::Int || t->k == TyK::Float || t->k == TyK::IntLit || t->k == TyK::FloatLit;
  };
  auto intLike = [](const TypeP& t) {
    return t->k == TyK::Int || t->k == TyK::IntLit || t->k == TyK::Char || t->k == TyK::Bool ||
           (t->k == TyK::Named && t->decl->k == Decl::Enum);
  };
  if (numeric(from) && numeric(to)) return true;
  if (intLike(from) && intLike(to)) return true;
  if (intLike(from) && to->k == TyK::Float) return from->k != TyK::Named;
  if (from->k == TyK::Ptr && to->k == TyK::Ptr) return true;
  if ((from->k == TyK::Ptr && to->k == TyK::Int) || (from->k == TyK::Int && to->k == TyK::Ptr)) return true;
  if (from->k == TyK::IntLit && to->k == TyK::Ptr) return true;
  if (from->k == TyK::Null && to->k == TyK::Ptr) return true;
  if ((from->k == TyK::Fn || from->k == TyK::Closure) && to->k == TyK::Fn) return true;
  if (from->k == TyK::Array && (to->k == TyK::Slice || to->k == TyK::Ptr)) return true;
  return false;
}

}  // namespace sema
}  // namespace c7
