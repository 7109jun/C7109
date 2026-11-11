#include "ir.hpp"

#include <cstring>
#include <iomanip>
#include <ostream>
#include <set>
#include <sstream>

namespace c7 {
namespace ir {

// ---- types ---------------------------------------------------------------------------------------

namespace {
TyP mk(Ty t) { return std::make_shared<const Ty>(std::move(t)); }
}  // namespace

TyP tVoid() { static TyP t = mk(Ty{TyKind::Void, 0, "", {}, nullptr, 0}); return t; }
TyP tInt(int bits) { Ty t; t.k = TyKind::Int; t.bits = bits; return mk(t); }
TyP tF32() { static TyP t = mk(Ty{TyKind::F32, 0, "", {}, nullptr, 0}); return t; }
TyP tF64() { static TyP t = mk(Ty{TyKind::F64, 0, "", {}, nullptr, 0}); return t; }
TyP tPtr() { static TyP t = mk(Ty{TyKind::Ptr, 0, "", {}, nullptr, 0}); return t; }
TyP tArray(TyP e, uint64_t n) { Ty t; t.k = TyKind::Array; t.elem = std::move(e); t.n = n; return mk(t); }
TyP tStruct(std::vector<TyP> f, const std::string& name) { Ty t; t.k = TyKind::Struct; t.fields = std::move(f); t.name = name; return mk(t); }

bool tyEq(const TyP& a, const TyP& b) {
  if (a == b) return true;
  if (!a || !b || a->k != b->k) return false;
  switch (a->k) {
    case TyKind::Int: return a->bits == b->bits;
    case TyKind::Array: return a->n == b->n && tyEq(a->elem, b->elem);
    case TyKind::Struct:
      if (!a->name.empty() || !b->name.empty()) return a->name == b->name;
      if (a->fields.size() != b->fields.size()) return false;
      for (size_t i = 0; i < a->fields.size(); i++) if (!tyEq(a->fields[i], b->fields[i])) return false;
      return true;
    default: return true;
  }
}

namespace {
bool plainName(const std::string& s) {
  if (s.empty()) return false;
  for (unsigned char c : s) if (!(isalnum(c) || c == '.' || c == '_' || c == '-')) return false;
  return true;
}
std::string quoted(char sigil, const std::string& s) {
  if (plainName(s) && !isdigit((unsigned char)s[0])) return std::string(1, sigil) + s;
  return std::string(1, sigil) + "\"" + s + "\"";
}
}  // namespace

std::string tyStr(const TyP& t) {
  if (!t) return "<null>";
  switch (t->k) {
    case TyKind::Void: return "void";
    case TyKind::Int: return "i" + std::to_string(t->bits);
    case TyKind::F32: return "float";
    case TyKind::F64: return "double";
    case TyKind::Ptr: return "ptr";
    case TyKind::Array: return "[" + std::to_string(t->n) + " x " + tyStr(t->elem) + "]";
    case TyKind::Struct: {
      if (!t->name.empty()) return quoted('%', t->name);
      std::string s = "{ ";
      for (size_t i = 0; i < t->fields.size(); i++) s += (i ? ", " : "") + tyStr(t->fields[i]);
      return t->fields.empty() ? "{}" : s + " }";
    }
  }
  return "?";
}

uint64_t alignOf(const TyP& t) {
  switch (t->k) {
    case TyKind::Void: return 1;
    case TyKind::Int: return t->bits <= 8 ? 1 : t->bits <= 16 ? 2 : t->bits <= 32 ? 4 : t->bits <= 64 ? 8 : 16;
    case TyKind::F32: return 4;
    case TyKind::F64: case TyKind::Ptr: return 8;
    case TyKind::Array: return alignOf(t->elem);
    case TyKind::Struct: { uint64_t a = 1; for (auto& f : t->fields) a = std::max(a, alignOf(f)); return a; }
  }
  return 1;
}

uint64_t sizeOf(const TyP& t) {
  switch (t->k) {
    case TyKind::Void: return 0;
    case TyKind::Int: return alignOf(t);
    case TyKind::F32: return 4;
    case TyKind::F64: case TyKind::Ptr: return 8;
    case TyKind::Array: return t->n * sizeOf(t->elem);
    case TyKind::Struct: {
      uint64_t off = 0;
      for (auto& f : t->fields) { uint64_t a = alignOf(f); off = (off + a - 1) / a * a + sizeOf(f); }
      uint64_t a = alignOf(t);
      return (off + a - 1) / a * a;
    }
  }
  return 0;
}

// ---- values --------------------------------------------------------------------------------------

Val vReg(int r, TyP t) { Val v; v.k = Val::Reg; v.reg = r; v.ty = std::move(t); return v; }
Val vInt(TyP t, uint64_t i) { Val v; v.k = Val::CInt; v.ty = std::move(t); v.i = i; return v; }
Val vFloat(TyP t, double f) { Val v; v.k = Val::CFloat; v.ty = std::move(t); v.f = f; return v; }
Val vNull() { Val v; v.k = Val::Null; v.ty = tPtr(); return v; }
Val vUndef(TyP t) { Val v; v.k = Val::Undef; v.ty = std::move(t); return v; }
Val vZero(TyP t) { Val v; v.k = Val::Zero; v.ty = std::move(t); return v; }
Val vGlobal(const std::string& n) { Val v; v.k = Val::Global; v.ty = tPtr(); v.name = n; return v; }
Val vConstAgg(TyP t, std::vector<Val> e) { Val v; v.k = Val::ConstAgg; v.ty = std::move(t); v.elems = std::move(e); return v; }
Val vSizeOf(TyP of) { Val v; v.k = Val::SizeOf; v.ty = tInt(64); v.of = std::move(of); return v; }

// ---- module ------------------------------------------------------------------------------------

Func* Module::find(const std::string& name) const {
  for (auto& f : funcs) if (f->name == name) return f.get();
  return nullptr;
}

Func* Module::declare(const std::string& name, TyP ret, std::vector<TyP> params, bool variadic) {
  if (Func* f = find(name)) return f;
  auto f = std::make_unique<Func>();
  f->name = name; f->ret = std::move(ret); f->params = std::move(params); f->variadic = variadic;
  for (auto& p : f->params) f->newReg(p);
  funcs.push_back(std::move(f));
  return funcs.back().get();
}

Val Module::addString(const std::string& bytes) {
  Global g;
  g.name = ".str." + std::to_string(strCount++);
  g.bytes = bytes;
  g.isString = true; g.isConst = true; g.internal = true;
  g.ty = tArray(tInt(8), bytes.size() + 1);
  globals.push_back(g);
  return vGlobal(g.name);
}

void Module::addStruct(const TyP& t) {
  if (!t) return;
  if (t->k == TyKind::Array) { addStruct(t->elem); return; }
  if (t->k != TyKind::Struct) return;
  for (auto& f : t->fields) addStruct(f);
  if (t->name.empty()) return;
  for (auto& s : structs) if (s->name == t->name) return;
  structs.push_back(t);
}

// ---- builder -------------------------------------------------------------------------------------

int Builder::newBlock(const std::string& label) {
  f.blocks.push_back(Block{label, {}});
  return (int)f.blocks.size() - 1;
}

bool Builder::terminated() const {
  if (cur < 0 || f.blocks[cur].ins.empty()) return false;
  Op o = f.blocks[cur].ins.back().op;
  return o == Op::Br || o == Op::CondBr || o == Op::Ret || o == Op::Unreachable;
}

Instr& Builder::emit(Op op) {
  f.blocks[cur].ins.emplace_back();
  Instr& i = f.blocks[cur].ins.back();
  i.op = op;
  return i;
}

Val Builder::alloca_(TyP t) {
  Instr i; i.op = Op::Alloca; i.ty = t; i.dst = f.newReg(tPtr());
  f.allocas.push_back(i);
  return vReg(i.dst, tPtr());
}
Val Builder::allocaDyn(Val bytes) {
  int r = f.newReg(tPtr());
  Instr& i = emit(Op::Alloca); i.dst = r; i.ty = tInt(8); i.a = {bytes};
  return vReg(r, tPtr());
}
Val Builder::load(TyP t, Val p) {
  int r = f.newReg(t);
  Instr& i = emit(Op::Load); i.dst = r; i.ty = t; i.a = {p};
  return vReg(r, t);
}
void Builder::store(Val v, Val p) { Instr& i = emit(Op::Store); i.a = {v, p}; }
Val Builder::bin(const std::string& op, Val a, Val b) {
  int r = f.newReg(a.ty);
  Instr& i = emit(Op::Bin); i.dst = r; i.ty = a.ty; i.s = op; i.a = {a, b};
  return vReg(r, a.ty);
}
Val Builder::icmp(const std::string& p, Val a, Val b) {
  int r = f.newReg(tInt(1));
  Instr& i = emit(Op::Icmp); i.dst = r; i.ty = tInt(1); i.s = p; i.a = {a, b};
  return vReg(r, tInt(1));
}
Val Builder::fcmp(const std::string& p, Val a, Val b) {
  int r = f.newReg(tInt(1));
  Instr& i = emit(Op::Fcmp); i.dst = r; i.ty = tInt(1); i.s = p; i.a = {a, b};
  return vReg(r, tInt(1));
}
Val Builder::cast(const std::string& op, Val v, TyP to) {
  int r = f.newReg(to);
  Instr& i = emit(Op::Cast); i.dst = r; i.ty = to; i.s = op; i.a = {v};
  return vReg(r, to);
}
Val Builder::gep(TyP src, Val p, std::vector<Val> idx) {
  int r = f.newReg(tPtr());
  Instr& i = emit(Op::Gep); i.dst = r; i.ty = src; i.a = {p};
  for (auto& x : idx) i.a.push_back(x);
  return vReg(r, tPtr());
}
Val Builder::gepField(TyP st, Val p, int field) {
  return gep(st, p, {vInt(tInt(32), 0), vInt(tInt(32), (uint64_t)field)});
}
Val Builder::extract(Val agg, int idx, TyP rt) {
  int r = f.newReg(rt);
  Instr& i = emit(Op::Extract); i.dst = r; i.ty = rt; i.a = {agg}; i.idx = {idx};
  return vReg(r, rt);
}
Val Builder::insert(Val agg, Val v, int idx) {
  int r = f.newReg(agg.ty);
  Instr& i = emit(Op::Insert); i.dst = r; i.ty = agg.ty; i.a = {agg, v}; i.idx = {idx};
  return vReg(r, agg.ty);
}
Val Builder::call(Val callee, std::vector<Val> args, TyP ret, const std::string& cc, bool variadic, std::vector<TyP> sig) {
  int r = ret->k == TyKind::Void ? -1 : f.newReg(ret);
  Instr& i = emit(Op::Call); i.dst = r; i.ty = ret; i.s = cc; i.variadic = variadic; i.sig = std::move(sig);
  i.a.push_back(callee);
  for (auto& x : args) i.a.push_back(x);
  return r < 0 ? Val() : vReg(r, ret);
}
Val Builder::select(Val c, Val a, Val b) {
  int r = f.newReg(a.ty);
  Instr& i = emit(Op::Select); i.dst = r; i.ty = a.ty; i.a = {c, a, b};
  return vReg(r, a.ty);
}
void Builder::br(int t) { Instr& i = emit(Op::Br); i.blocks = {t}; }
void Builder::condBr(Val c, int t, int fl) { Instr& i = emit(Op::CondBr); i.a = {c}; i.blocks = {t, fl}; }
void Builder::ret(Val v) { Instr& i = emit(Op::Ret); if (v.ok()) i.a = {v}; }
void Builder::unreachable() { emit(Op::Unreachable); }

// ---- printing --------------------------------------------------------------------------------------

namespace {
std::string typed(const Val& v);
std::string valStr(const Val& v) {
  switch (v.k) {
    case Val::None: return "<none>";
    case Val::Reg: return "%r" + std::to_string(v.reg);
    case Val::CInt: {
      if (v.ty->bits == 1) return v.i ? "true" : "false";
      if (v.ty->bits >= 64) return std::to_string((int64_t)v.i);
      uint64_t mask = (1ull << v.ty->bits) - 1, x = v.i & mask;
      if (x >> (v.ty->bits - 1)) return std::to_string((int64_t)(x | ~mask));
      return std::to_string((int64_t)x);
    }
    case Val::CFloat: {
      double d = v.ty->k == TyKind::F32 ? (double)(float)v.f : v.f;
      uint64_t bits; std::memcpy(&bits, &d, 8);
      std::ostringstream o; o << "0x" << std::hex << std::uppercase << std::setfill('0') << std::setw(16) << bits;
      return o.str();
    }
    case Val::Null: return "null";
    case Val::Undef: return "undef";
    case Val::Zero: return isAgg(v.ty) ? "zeroinitializer" : (isFloat(v.ty) ? "0.0" : (v.ty->k == TyKind::Ptr ? "null" : "0"));
    case Val::Global: return quoted('@', v.name);
    case Val::ConstAgg: {
      if (v.elems.empty()) return "zeroinitializer";
      bool st = v.ty->k == TyKind::Struct;
      std::string o = st ? "{ " : "[ ";
      for (size_t i = 0; i < v.elems.size(); i++) o += (i ? ", " : "") + typed(v.elems[i]);
      return o + (st ? " }" : " ]");
    }
    case Val::SizeOf: return "ptrtoint (ptr getelementptr (" + tyStr(v.of) + ", ptr null, i32 1) to i64)";
  }
  return "?";
}
std::string typed(const Val& v) { return tyStr(v.ty) + " " + valStr(v); }

std::string escapeBytes(const std::string& s) {
  std::string o;
  static const char* hex = "0123456789ABCDEF";
  for (unsigned char c : s) {
    if (c >= 32 && c < 127 && c != '"' && c != '\\') o += (char)c;
    else { o += '\\'; o += hex[c >> 4]; o += hex[c & 15]; }
  }
  return o;
}

std::string blk(int b) { return "%b" + std::to_string(b); }

void printInstr(const Instr& i, std::ostream& os) {
  std::string dst = i.dst >= 0 ? "%r" + std::to_string(i.dst) + " = " : "";
  switch (i.op) {
    case Op::Alloca:
      if (!i.a.empty()) os << "  " << dst << "alloca i8, " << typed(i.a[0]) << ", align 16\n";
      else os << "  " << dst << "alloca " << tyStr(i.ty) << ", align " << alignOf(i.ty) << "\n";
      break;
    case Op::Load: os << "  " << dst << "load " << tyStr(i.ty) << ", ptr " << valStr(i.a[0]) << "\n"; break;
    case Op::Store: os << "  store " << typed(i.a[0]) << ", ptr " << valStr(i.a[1]) << "\n"; break;
    case Op::Bin: os << "  " << dst << i.s << " " << tyStr(i.ty) << " " << valStr(i.a[0]) << ", " << valStr(i.a[1]) << "\n"; break;
    case Op::Icmp: os << "  " << dst << "icmp " << i.s << " " << tyStr(i.a[0].ty) << " " << valStr(i.a[0]) << ", " << valStr(i.a[1]) << "\n"; break;
    case Op::Fcmp: os << "  " << dst << "fcmp " << i.s << " " << tyStr(i.a[0].ty) << " " << valStr(i.a[0]) << ", " << valStr(i.a[1]) << "\n"; break;
    case Op::Cast: os << "  " << dst << i.s << " " << typed(i.a[0]) << " to " << tyStr(i.ty) << "\n"; break;
    case Op::Gep:
      os << "  " << dst << "getelementptr inbounds " << tyStr(i.ty) << ", ptr " << valStr(i.a[0]);
      for (size_t k = 1; k < i.a.size(); k++) os << ", " << typed(i.a[k]);
      os << "\n";
      break;
    case Op::Extract:
      os << "  " << dst << "extractvalue " << typed(i.a[0]);
      for (int x : i.idx) os << ", " << x;
      os << "\n";
      break;
    case Op::Insert:
      os << "  " << dst << "insertvalue " << typed(i.a[0]) << ", " << typed(i.a[1]);
      for (int x : i.idx) os << ", " << x;
      os << "\n";
      break;
    case Op::Call: {
      os << "  " << dst << "call " << (i.s.empty() ? "" : i.s + " ") << tyStr(i.ty);
      if (i.variadic) {
        os << " (";
        for (size_t k = 0; k < i.sig.size(); k++) os << (k ? ", " : "") << tyStr(i.sig[k]);
        os << (i.sig.empty() ? "..." : ", ...") << ")";
      }
      os << " " << valStr(i.a[0]) << "(";
      for (size_t k = 1; k < i.a.size(); k++) os << (k > 1 ? ", " : "") << typed(i.a[k]);
      os << ")\n";
      break;
    }
    case Op::Select: os << "  " << dst << "select i1 " << valStr(i.a[0]) << ", " << typed(i.a[1]) << ", " << typed(i.a[2]) << "\n"; break;
    case Op::Br: os << "  br label " << blk(i.blocks[0]) << "\n"; break;
    case Op::CondBr: os << "  br i1 " << valStr(i.a[0]) << ", label " << blk(i.blocks[0]) << ", label " << blk(i.blocks[1]) << "\n"; break;
    case Op::Ret: if (i.a.empty()) os << "  ret void\n"; else os << "  ret " << typed(i.a[0]) << "\n"; break;
    case Op::Unreachable: os << "  unreachable\n"; break;
  }
}
}  // namespace

void printModule(const Module& m, std::ostream& os, bool header) {
  if (header) os << "; c7ir 0.1 (textual C7 IR; LLVM-compatible syntax)\n";
  os << "target triple = \"" << m.triple << "\"\n\n";
  for (auto& s : m.structs) {
    os << quoted('%', s->name) << " = type { ";
    for (size_t i = 0; i < s->fields.size(); i++) os << (i ? ", " : "") << tyStr(s->fields[i]);
    os << (s->fields.empty() ? "}\n" : " }\n");
  }
  if (!m.structs.empty()) os << "\n";
  for (auto& g : m.globals) {
    os << quoted('@', g.name) << " = ";
    if (g.isString) os << "private unnamed_addr constant " << tyStr(g.ty) << " c\"" << escapeBytes(g.bytes) << "\\00\", align 1\n";
    else if (g.external) os << "external global " << tyStr(g.ty) << "\n";
    else os << (g.internal ? "internal " : "") << (g.isConst ? "constant " : "global ") << tyStr(g.ty) << " " << valStr(g.init) << "\n";
  }
  if (!m.globals.empty()) os << "\n";
  for (auto& fp : m.funcs) {
    const Func& f = *fp;
    if (f.isDecl) {
      os << "declare " << tyStr(f.ret) << " " << quoted('@', f.name) << "(";
      for (size_t i = 0; i < f.params.size(); i++) os << (i ? ", " : "") << tyStr(f.params[i]);
      if (f.variadic) os << (f.params.empty() ? "..." : ", ...");
      os << ")\n";
      continue;
    }
    os << "\ndefine " << (f.internal ? "internal " : "") << (f.cc.empty() ? "" : f.cc + " ") << tyStr(f.ret) << " "
       << quoted('@', f.name) << "(";
    for (size_t i = 0; i < f.params.size(); i++) os << (i ? ", " : "") << tyStr(f.params[i]) << " %r" << i;
    os << ") {\n";
    for (size_t b = 0; b < f.blocks.size(); b++) {
      os << "b" << b << ":";
      if (!f.blocks[b].label.empty()) os << "  ; " << f.blocks[b].label;
      os << "\n";
      if (b == 0) for (auto& a : f.allocas) printInstr(a, os);
      for (auto& i : f.blocks[b].ins) printInstr(i, os);
    }
    os << "}\n";
  }
}

// ---- verifier -------------------------------------------------------------------------------------------

std::vector<std::string> verify(const Module& m) {
  std::vector<std::string> errs;
  for (auto& fp : m.funcs) {
    const Func& f = *fp;
    if (f.isDecl) continue;
    auto err = [&](const std::string& s) { errs.push_back("@" + f.name + ": " + s); };
    if (f.blocks.empty()) { err("function has no blocks"); continue; }
    std::set<int> defined;
    for (size_t i = 0; i < f.params.size(); i++) defined.insert((int)i);
    for (auto& a : f.allocas) defined.insert(a.dst);
    for (auto& b : f.blocks) for (auto& i : b.ins) if (i.dst >= 0) defined.insert(i.dst);
    auto checkVal = [&](const Val& v, const std::string& where) {
      if (!v.ok()) { err(where + ": missing operand"); return; }
      if (!v.ty) { err(where + ": operand without type"); return; }
      if (v.k == Val::Reg && !defined.count(v.reg)) err(where + ": use of undefined %r" + std::to_string(v.reg));
      if (v.k == Val::Reg && v.reg < (int)f.regTy.size() && !tyEq(f.regTy[v.reg], v.ty))
        err(where + ": %r" + std::to_string(v.reg) + " used as " + tyStr(v.ty) + " but defined as " + tyStr(f.regTy[v.reg]));
    };
    for (size_t bi = 0; bi < f.blocks.size(); bi++) {
      const Block& b = f.blocks[bi];
      std::string bn = "b" + std::to_string(bi);
      if (b.ins.empty()) { err(bn + ": empty block"); continue; }
      for (size_t k = 0; k < b.ins.size(); k++) {
        const Instr& i = b.ins[k];
        bool term = i.op == Op::Br || i.op == Op::CondBr || i.op == Op::Ret || i.op == Op::Unreachable;
        if (term && k + 1 != b.ins.size()) err(bn + ": terminator in the middle of a block");
        if (!term && k + 1 == b.ins.size()) err(bn + ": block does not end with a terminator");
        for (auto& v : i.a) checkVal(v, bn);
        auto target = [&](int t) { if (t < 0 || t >= (int)f.blocks.size()) err(bn + ": branch to unknown block"); };
        switch (i.op) {
          case Op::Load: if (i.a.size() != 1 || i.a[0].ty->k != TyKind::Ptr) err(bn + ": load from non-pointer"); break;
          case Op::Store: if (i.a.size() != 2 || i.a[1].ty->k != TyKind::Ptr) err(bn + ": store to non-pointer"); break;
          case Op::Bin:
            if (i.a.size() != 2 || !tyEq(i.a[0].ty, i.a[1].ty)) err(bn + ": `" + i.s + "` operand types differ");
            break;
          case Op::Icmp: case Op::Fcmp:
            if (i.a.size() != 2 || !tyEq(i.a[0].ty, i.a[1].ty)) err(bn + ": comparison operand types differ");
            break;
          case Op::Gep: if (i.a.empty() || i.a[0].ty->k != TyKind::Ptr) err(bn + ": gep base is not a pointer"); break;
          case Op::Extract: if (i.a.empty() || !isAgg(i.a[0].ty)) err(bn + ": extractvalue of non-aggregate"); break;
          case Op::Br: target(i.blocks[0]); break;
          case Op::CondBr:
            if (i.a.empty() || !(isInt(i.a[0].ty) && i.a[0].ty->bits == 1)) err(bn + ": condition is not i1");
            target(i.blocks[0]); target(i.blocks[1]);
            break;
          case Op::Ret:
            if (f.ret->k == TyKind::Void ? !i.a.empty() : (i.a.empty() || !tyEq(i.a[0].ty, f.ret))) err(bn + ": ret type does not match `" + tyStr(f.ret) + "`");
            break;
          case Op::Call: {
            const Val& callee = i.a[0];
            if (callee.k == Val::Global) {
              Func* t = m.find(callee.name);
              if (!t) { err(bn + ": call to undeclared @" + callee.name); break; }
              size_t n = i.a.size() - 1;
              if (n < t->params.size() || (n > t->params.size() && !t->variadic)) err(bn + ": call to @" + callee.name + " with wrong argument count");
              else for (size_t k = 0; k < t->params.size(); k++)
                if (!tyEq(i.a[k + 1].ty, t->params[k])) err(bn + ": argument " + std::to_string(k) + " of @" + callee.name + " has type " + tyStr(i.a[k + 1].ty) + ", expected " + tyStr(t->params[k]));
              if (!tyEq(i.ty, t->ret)) err(bn + ": call result type differs from @" + callee.name + " return type");
            }
            break;
          }
          default: break;
        }
      }
    }
  }
  return errs;
}

}  // namespace ir
}  // namespace c7
