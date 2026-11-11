// AST -> C7 IR lowering. Stage-3 subset (see README): functions, primitive types, control flow,
// pointers, arrays/slices/Str, structs, enums, classes (methods, init, drop RAII), alloc/free,
// defer, extern "C". Anything else raises E0901 instead of producing wrong code.
#include <cmath>

#include "../frontend/sema_impl.hpp"
#include "../ir/ir.hpp"

namespace c7 {
namespace sema {
namespace {

struct Unsupported { std::string what; Loc loc; };

TypeP st(Node* n) { return n && n->sty ? std::static_pointer_cast<Type>(n->sty) : mkErr(); }
TypeP ctOf(Node* n) { return n && n->cty ? std::static_pointer_cast<Type>(n->cty) : nullptr; }

struct V { ir::Val v; TypeP ty; };
struct LocalVar { ir::Val ptr; TypeP ty; bool indirect = false, mut = false; };
struct Cleanup { bool isDefer = false; Node* node = nullptr; ir::Val ptr; Decl* cls = nullptr; TypeP ty; std::string var; };
struct Scope { int id = 0; std::map<std::string, LocalVar> vars; std::vector<Cleanup> cleanups; };
// `goto` leaves scopes: the cleanups (drop/defer) of every scope the jump exits run first, in reverse order, exactly like
// return/break. A forward goto does not yet know the label's scopes, so it parks a trampoline block that is filled in when
// the label is lowered.
struct LabelState { std::vector<std::pair<int, size_t>> chain; };  // (scope id, cleanups registered so far), outer -> inner
struct PendingGoto { int tramp; std::string label; std::vector<Scope> snap; Node* at; };
struct GotoState { std::map<std::string, LabelState> labels; std::vector<PendingGoto> pending; };
struct LoopCtx { int brk = -1, cont = -1; size_t scopeDepth = 0; std::string label; bool isSwitch = false; };

std::string sanitize(const std::string& s) {
  std::string o;
  for (char c : s) {
    switch (c) {
      case ' ': o += '_'; break; case '*': o += 'P'; break; case '[': o += 'A'; break; case ']': break;
      case '(': o += 'T'; break; case ')': break; case ',': o += '_'; break; case '<': o += 'L'; break;
      case '>': o += 'R'; break; case '-': o += '_'; break; case ':': o += '_'; break; default: o += c;
    }
  }
  return o;
}
std::string dollar(const std::string& full) {
  std::string o;
  for (size_t i = 0; i < full.size(); i++) {
    if (full[i] == ':' && i + 1 < full.size() && full[i + 1] == ':') { o += '$'; i++; } else o += full[i];
  }
  return o;
}

uint32_t decodeCharText(const std::string& text) {
  std::string s = text.size() >= 2 ? text.substr(1, text.size() - 2) : "";
  if (s.empty()) return 0;
  if (s[0] == '\\' && s.size() >= 2) {
    switch (s[1]) {
      case 'n': return '\n'; case 'r': return '\r'; case 't': return '\t'; case '\\': return '\\';
      case '\'': return '\''; case '"': return '"'; case '0': return 0;
      case 'x': return (uint32_t)std::stoul(s.substr(2), nullptr, 16);
      case 'u': return (uint32_t)std::stoul(s.substr(3, s.size() - 4), nullptr, 16);
    }
  }
  unsigned char c = s[0];
  if (c < 0x80) return c;
  int len = (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
  uint32_t cp = c & (0xFF >> (len + 1));
  for (int i = 1; i < len && i < (int)s.size(); i++) cp = (cp << 6) | (s[i] & 0x3F);
  return cp;
}

// ABI v1 (provisional, x86-64 Linux): every C7-to-C7 call uses this LLVM calling convention; `extern "C"` and `main`
// use the C convention (empty). It is the ONLY place the convention is named, so changing the ABI is a one-line edit.
// Measured against C / swiftcc / preserve_most in bench/abi (see STATUS.md section 6.6).
static const char* const kC7CallConv = "fastcc";

class Lower {
 public:
  Lower(Sema& s, ir::Module& m, const CodegenOptions& cg) : S(s), M(m), cg(cg) {}
  bool run(Node& unit);

 private:
  Sema& S;
  ir::Module& M;
  CodegenOptions cg;
  std::string heapFn(const char* base) { return cg.debugAlloc ? std::string("c7_dbg_") + base : std::string(base); }
  std::map<std::string, ir::TyP> structCache;
  struct TagInfo {
    ir::TyP tagTy, full;
    std::vector<ir::TyP> payStruct;                // per variant: struct of non-void payload fields
    std::vector<std::vector<TypeP>> sema;          // per variant: substituted payload types
    std::vector<std::vector<int>> fidx;            // per variant: field index in payStruct (-1 for void)
  };
  std::map<std::string, TagInfo> tagCache;
  bool failed = false;

  // per-function state
  ir::Func* F = nullptr;
  std::unique_ptr<ir::Builder> B;
  std::vector<Scope> scopes;
  std::vector<LoopCtx> loops;
  std::map<std::string, int> labelBlocks;
  GotoState gs;
  int scopeCounter = 0;
  std::string pendingLabel;
  TypeP retType;
  bool haveSelf = false;
  ir::Val selfPtr;
  Decl* selfDecl = nullptr;
  bool inInit = false;
  struct SwitchCtx { std::vector<int> bodies; size_t cur = 0; };
  std::vector<SwitchCtx> switches;

  [[noreturn]] void unsupported(const std::string& what, Node* at) { throw Unsupported{what, at ? at->loc : Loc{}}; }

  // ---- types ---------------------------------------------------------------------------------------
  ir::TyP irTy(const TypeP& t, Node* at);
  ir::TyP namedTy(const TypeP& t, Node* at);
  SubMap argMap(const TypeP& t, Node* at);
  std::string typeKey(const TypeP& t);
  TagInfo& tagInfo(const TypeP& t, Node* at);
  V makeVariant(const TypeP& t, size_t idx, const std::vector<V>& vals, Node* at);
  V variantPayload(const TypeP& t, ir::Val slot, size_t variant, size_t field, Node* at);
  std::vector<const Member*> fieldsOf(Decl* d);
  int fieldIdx(Decl* d, const std::string& name);
  std::string symbolOf(Decl* fd);
  std::string symbolOfMember(const Member* m, Decl* owner);
  std::string cc(Decl* fd) { return (fd && !fd->abi.empty()) ? "" : kC7CallConv; }
  ir::Func* declareFn(const std::string& sym, const TypeP& fnTy, bool hasSelf, const std::string& ccs, Node* at);
  void declareC(const char* name, ir::TyP ret, std::vector<ir::TyP> params);
  bool isSigned(const TypeP& t);
  int64_t enumValue(Decl* d, const std::string& name);

  // ---- functions ---------------------------------------------------------------------------------------
  void lowerFunctionBody(Node* fnode, const std::string& sym, const TypeP& fnTy, Decl* owner, bool hasSelf, bool init, const std::string& ccs, bool internal);
  void lowerGlobals();
  void pushScope() { scopes.emplace_back(); scopes.back().id = ++scopeCounter; }
  void emitGotoCleanups(const std::vector<Scope>& snap, const LabelState& ls);
  void popScopeNoEmit() { scopes.pop_back(); }
  void emitCleanups(size_t lo, const std::string& skipVar);
  void deadBlock() { B->setBlock(B->newBlock("dead")); }
  int labelBlock(const std::string& n);
  LocalVar* findLocal(const std::string& n);
  LocalVar& addLocal(const std::string& n, const TypeP& t, ir::Val ptr, bool indirect = false, bool mut = false);
  ir::Val allocaFor(const TypeP& t, Node* at) {
    if (t && t->k == TyK::Param) return B->allocaDyn(paramSize(t, at));
    return B->alloca_(irTy(t, at));
  }
  // ---- generics: shared code with type descriptors (spec 10.1) ----
  std::map<std::string, ir::Val> genericDescs;  // type-parameter name -> descriptor pointer (current function)
  ir::Val sretPtr;
  bool genericRet = false;
  std::map<std::string, ir::Val> descCache;
  std::map<Decl*, int> ifaceIds;
  bool containsParam(const TypeP& t);
  bool okGenericType(const TypeP& t);
  ir::Func* declareGenericFn(const std::string& sym, const std::string& nm, const TypeP& fnTy, const std::vector<std::string>& tparams, bool hasSelf, Node* at);
  void lowerGenericBody(Node* fnode, const std::string& sym, const std::string& nm, const TypeP& fnTy, const std::vector<std::string>& tparams, Decl* owner, bool hasSelf, bool isInit);
  V emitGenericCall(ir::Func* f, const std::vector<std::string>& tparams, const SubMap& sm, const TypeP& fnTy, bool hasSelf, ir::Val self, const std::vector<Node*>& argNodes, Node* at);
  V genGenericMemberCall(Node* n, const Member* m);
  void callDrop(const TypeP& cls, ir::Val ptr, Node* at);
  ir::Val descOf(const TypeP& t, Node* at);
  int ifaceId(Decl* d);
  ir::Val paramSize(const TypeP& t, Node* at);
  V loadVal(const TypeP& t, ir::Val addr, Node* at);
  void storeVal(const TypeP& t, V v, ir::Val addr, Node* at);
  void memcpyVal(ir::Val dst, ir::Val src, ir::Val size);
  ir::Val elemGep(const TypeP& elem, ir::Val p, ir::Val idx, Node* at);
  V genGenericCall(Node* n, Decl* fd);
  // ---- closures (spec 10.2) ----
  int lambdaCount = 0;
  struct FnState {
    ir::Func* F; std::unique_ptr<ir::Builder> B; std::vector<Scope> scopes; std::vector<LoopCtx> loops;
    std::map<std::string, int> labelBlocks; GotoState gs; std::string pendingLabel; TypeP retType; bool haveSelf; ir::Val selfPtr;
    Decl* selfDecl; bool inInit; std::vector<SwitchCtx> switches; bool genericRet; ir::Val sretPtr;
  };
  FnState saveState();
  void restoreState(FnState&& st);
  void freeNames(Node* n, std::set<std::string>& out);
  V genLambda(Node* n);
  V genBoundMethodCall(Node* n, const Member* m);
  void registerDrop(const std::string& name, const TypeP& t, ir::Val ptr);
  void panicIf(ir::Val cond, const std::string& msg);

  // ---- statements --------------------------------------------------------------------------------------
  void stmt(Node* n);
  void block(Node* n, bool newScope = true);
  void localDecl(Node* n, const std::string& name, const TypeP& ty, Node* init);
  void assign(Node* n);
  void forStmt(Node* n);
  void forInStmt(Node* n);
  void iterLoop(Node* n, Node* it, const TypeP& et, const std::string& lbl);
  void switchStmt(Node* n);
  void matchStmt(Node* n);
  ir::Val patTest(Node* pat, const V& scrut, std::vector<std::pair<std::string, V>>& binds);
  void freeStmt(Node* n);
  ir::Val cond(Node* n);

  // ---- expressions ---------------------------------------------------------------------------------------
  V gen(Node* n, TypeP hint = nullptr);
  V genRaw(Node* n, TypeP hint);
  V coerce(V v, const TypeP& to);
  V genCall(Node* n, TypeP hint);
  V genBinary(Node* n, TypeP hint);
  V genUnary(Node* n, TypeP hint);
  V genIndex(Node* n);
  V genMember(Node* n);
  V genAlloc(Node* n);
  V genStructLit(Node* n, TypeP hint);
  V arith(const std::string& op, V a, V b, const TypeP& t, Node* at);
  V checkedArith(const std::string& op, V a, V b, const TypeP& t, const char* what = nullptr);
  void boundsCheck(ir::Val idx, ir::Val len);
  V compare(const std::string& op, V a, V b, const TypeP& t);
  V castValue(V v, const TypeP& to, Node* at);
  ir::Val addrOf(Node* n);
  ir::Val addrOrSpill(Node* n);
  bool addressable(Node* n);
  bool varAddr(const std::string& name, ir::Val& out);
  ir::Val toI64(V v);
  ir::Val strConst(const std::string& s, size_t len);
  V mk(ir::Val v, TypeP t) { return V{v, std::move(t)}; }
  ir::Val ci(const ir::TyP& t, int64_t v) { return ir::vInt(t, (uint64_t)v); }
  TypeP commonType(const TypeP& a, const TypeP& b);
  ir::Val callMember(const Member* m, Decl* owner, ir::Val self, bool hasSelf, std::vector<ir::Val> args, ir::TyP ret);
};

// ======================== types & symbols ====================================================================

std::vector<const Member*> Lower::fieldsOf(Decl* d) {
  S.declareDecl(d);
  std::vector<const Member*> out;
  for (auto& m : d->members) if (!m.isMethod && !m.isVariant && !m.isEnumerator) out.push_back(&m);
  return out;
}

int Lower::fieldIdx(Decl* d, const std::string& name) {
  auto f = fieldsOf(d);
  for (size_t i = 0; i < f.size(); i++) if (f[i]->name == name) return (int)i;
  return -1;
}

bool Lower::isSigned(const TypeP& t) {
  if (t->k == TyK::Int) return t->sgn;
  if (t->k == TyK::IntLit) return true;
  if (t->k == TyK::Named && t->decl->k == Decl::Enum && t->decl->ty) return t->decl->ty->k == TyK::Int && t->decl->ty->sgn;
  return false;
}

SubMap Lower::argMap(const TypeP& t, Node* at) {
  SubMap sm;
  Decl* d = t->decl;
  for (size_t i = 0; i < d->tparams.size(); i++) {
    if (i >= t->elems.size() || isErr(t->elems[i])) unsupported("unresolved type argument for `" + d->name + "` (add a type annotation)", at);
    sm[d->tparams[i]] = t->elems[i];
  }
  return sm;
}

std::string Lower::typeKey(const TypeP& t) {
  std::string k = t->decl->full;
  if (!t->elems.empty()) { k += "<"; for (auto& e : t->elems) k += ts(e) + ","; k += ">"; }
  return k;
}

Lower::TagInfo& Lower::tagInfo(const TypeP& t, Node* at) {
  std::string key = typeKey(t);
  auto it = tagCache.find(key);
  if (it != tagCache.end()) return it->second;
  Decl* d = t->decl;
  S.declareDecl(d);
  SubMap sm = argMap(t, at);
  TagInfo ti;
  TypeP tagSema;
  for (auto& k : d->node->kids) if (isTypeNode(k->kind)) tagSema = S.resolveType(k.get());
  size_t nv = 0;
  for (auto& m : d->members) if (m.isVariant) nv++;
  if (tagSema && tagSema->k == TyK::Int) ti.tagTy = ir::tInt(tagSema->bits);
  else ti.tagTy = ir::tInt(nv <= 256 ? 8 : nv <= 65536 ? 16 : 32);
  uint64_t maxSz = 0, maxAl = 1;
  for (auto& m : d->members) {
    if (!m.isVariant) continue;
    std::vector<TypeP> sema;
    std::vector<ir::TyP> fields;
    std::vector<int> fidx;
    for (auto& p : m.payload) {
      TypeP ps = subst(p, sm);
      sema.push_back(ps);
      ir::TyP f = irTy(ps, at);
      if (f->k == ir::TyKind::Void) fidx.push_back(-1); else { fidx.push_back((int)fields.size()); fields.push_back(f); }
    }
    ir::TyP ps = ir::tStruct(fields);
    if (!fields.empty()) { maxSz = std::max(maxSz, ir::sizeOf(ps)); maxAl = std::max(maxAl, ir::alignOf(ps)); }
    ti.payStruct.push_back(ps);
    ti.sema.push_back(sema);
    ti.fidx.push_back(fidx);
  }
  std::vector<ir::TyP> fs{ti.tagTy};
  if (maxSz > 0) {
    uint64_t unit = std::min<uint64_t>(maxAl, 8);
    fs.push_back(ir::tArray(ir::tInt((int)unit * 8), (maxSz + unit - 1) / unit));
  }
  ti.full = ir::tStruct(fs, "T." + dollar(sanitize(key)));
  M.addStruct(ti.full);
  return tagCache[key] = ti;
}

V Lower::makeVariant(const TypeP& t, size_t idx, const std::vector<V>& vals, Node* at) {
  TagInfo& ti = tagInfo(t, at);
  ir::Val slot = B->alloca_(ti.full);
  B->store(ir::vZero(ti.full), slot);
  B->store(ci(ti.tagTy, (int64_t)idx), B->gepField(ti.full, slot, 0));
  ir::Val pay = ir::vUndef(ti.payStruct[idx]);
  bool any = false;
  for (size_t i = 0; i < vals.size() && i < ti.fidx[idx].size(); i++) {
    if (ti.fidx[idx][i] < 0) continue;
    pay = B->insert(pay, vals[i].v, ti.fidx[idx][i]);
    any = true;
  }
  if (any) B->store(pay, B->gepField(ti.full, slot, 1));
  return mk(B->load(ti.full, slot), t);
}

V Lower::variantPayload(const TypeP& t, ir::Val slot, size_t variant, size_t field, Node* at) {
  TagInfo& ti = tagInfo(t, at);
  TypeP pt = ti.sema[variant][field];
  int fi = ti.fidx[variant][field];
  if (fi < 0) return V{ir::Val(), pt};
  ir::Val area = B->gepField(ti.full, slot, 1);
  ir::Val fp = B->gepField(ti.payStruct[variant], area, fi);
  return mk(B->load(irTy(pt, at), fp), pt);
}

ir::TyP Lower::namedTy(const TypeP& t, Node* at) {
  Decl* d = t->decl;
  S.declareDecl(d);
  switch (d->k) {
    case Decl::Enum: return irTy(d->ty ? d->ty : mkPrim("i32"), at);
    case Decl::Struct: case Decl::Class: {
      std::string key = typeKey(t);
      auto it = structCache.find(key);
      if (it != structCache.end()) return it->second;
      bool inGeneric = false;  // some type argument is a type parameter: shared-code view of the layout
      for (auto& e : t->elems) if (containsParam(e)) inGeneric = true;
      if (inGeneric) {
        key = d->full;  // one layout for every instantiation; a field that embeds T by value is rejected by irTy
        auto it2 = structCache.find(key);
        if (it2 != structCache.end()) return it2->second;
      }
      SubMap sm;
      if (!inGeneric) sm = argMap(t, at);
      std::vector<ir::TyP> fs;
      for (auto* f : fieldsOf(d)) fs.push_back(irTy(inGeneric ? f->ty : subst(f->ty, sm), at));
      ir::TyP s = ir::tStruct(fs, "S." + dollar(sanitize(key)));
      structCache[key] = s;
      M.addStruct(s);
      return s;
    }
    case Decl::Union: {
      std::string key = typeKey(t);
      auto it = structCache.find(key);
      if (it != structCache.end()) return it->second;
      uint64_t sz = 1, al = 1;
      for (auto* f : fieldsOf(d)) { ir::TyP ft = irTy(f->ty, at); sz = std::max(sz, ir::sizeOf(ft)); al = std::max(al, ir::alignOf(ft)); }
      sz = (sz + al - 1) / al * al;
      ir::TyP elem = ir::tInt((int)al * 8);
      ir::TyP s = ir::tStruct({ir::tArray(elem, sz / al)}, "U." + dollar(sanitize(key)));
      structCache[key] = s;
      M.addStruct(s);
      return s;
    }
    case Decl::Tagged: return tagInfo(t, at).full;
    case Decl::Interface: unsupported("interface type `" + d->name + "`", at);
    default: unsupported("type `" + d->name + "`", at);
  }
}

ir::TyP Lower::irTy(const TypeP& t, Node* at) {
  if (!t) unsupported("unresolved type", at);
  switch (t->k) {
    case TyK::Error: unsupported("unresolved type", at);
    case TyK::Void: return ir::tVoid();
    case TyK::Bool: return ir::tInt(1);
    case TyK::Int: return ir::tInt(t->bits);
    case TyK::Char: return ir::tInt(32);
    case TyK::Float: return t->bits == 32 ? ir::tF32() : ir::tF64();
    case TyK::IntLit: return ir::tInt(32);
    case TyK::FloatLit: return ir::tF64();
    case TyK::Ptr:
      if (t->elem && t->elem->k == TyK::Named && t->elem->decl->k == Decl::Interface) unsupported("interface pointer", at);
      return ir::tPtr();
    case TyK::Null: case TyK::Fn: return ir::tPtr();
    case TyK::Slice: return ir::tStruct({ir::tPtr(), ir::tInt(64)});
    case TyK::Array:
      if (t->len < 0) unsupported("array with non-constant length", at);
      return ir::tArray(irTy(t->elem, at), (uint64_t)t->len);
    case TyK::Tuple: { std::vector<ir::TyP> f; for (auto& e : t->elems) f.push_back(irTy(e, at)); return ir::tStruct(f); }
    case TyK::Closure: return ir::tStruct({ir::tPtr(), ir::tPtr()});
    case TyK::Param: unsupported("generics", at);
    case TyK::Named: return namedTy(t, at);
  }
  unsupported("type", at);
}

std::string Lower::symbolOf(Decl* fd) {
  if (!fd->abi.empty()) return fd->name;
  if (fd->name == "main" && fd->full == "main") return "main";
  std::string s = "_C7$" + dollar(fd->full) + "$$";
  S.declareDecl(fd);
  for (size_t i = 0; i < fd->ty->elems.size(); i++) s += (i ? "$" : "") + sanitize(ts(fd->ty->elems[i]));
  return s;
}

std::string Lower::symbolOfMember(const Member* m, Decl* owner) {
  std::string s = "_C7$" + dollar(owner->full) + "$" + m->name + "$$";
  for (size_t i = 0; i < m->ty->elems.size(); i++) s += (i ? "$" : "") + sanitize(ts(m->ty->elems[i]));
  return s;
}

ir::Func* Lower::declareFn(const std::string& sym, const TypeP& fnTy, bool hasSelf, const std::string& ccs, Node* at) {
  if (ir::Func* f = M.find(sym)) return f;
  std::vector<ir::TyP> ps;
  if (hasSelf) ps.push_back(ir::tPtr());
  for (size_t i = 0; i < fnTy->elems.size(); i++) {
    bool out = i < fnTy->pmods.size() && hasMod(fnTy->pmods[i], "out");
    ps.push_back(out ? ir::tPtr() : irTy(fnTy->elems[i], at));
  }
  ir::Func* f = M.declare(sym, irTy(fnTy->ret, at), ps);
  f->cc = ccs;
  return f;
}

void Lower::declareC(const char* name, ir::TyP ret, std::vector<ir::TyP> params) { M.declare(name, ret, params); }

int64_t Lower::enumValue(Decl* d, const std::string& name) {
  int64_t next = 0;
  for (auto& m : d->members) {
    if (!m.isEnumerator) continue;
    int64_t v = next;
    if (m.node && !m.node->kids.empty()) { int64_t c = S.constEval(m.node->kids[0].get()); if (c >= 0) v = c; }
    if (m.name == name) return v;
    next = v + 1;
  }
  return 0;
}

// ======================== scopes & cleanups ====================================================================

LocalVar* Lower::findLocal(const std::string& n) {
  for (size_t i = scopes.size(); i > 0; i--) {
    auto it = scopes[i - 1].vars.find(n);
    if (it != scopes[i - 1].vars.end()) return &it->second;
  }
  return nullptr;
}

LocalVar& Lower::addLocal(const std::string& n, const TypeP& t, ir::Val ptr, bool indirect, bool mut) {
  LocalVar lv; lv.ptr = ptr; lv.ty = t; lv.indirect = indirect; lv.mut = mut;
  scopes.back().vars[n] = lv;
  return scopes.back().vars[n];
}

int Lower::labelBlock(const std::string& n) {
  auto it = labelBlocks.find(n);
  if (it != labelBlocks.end()) return it->second;
  int b = B->newBlock("label." + n);
  labelBlocks[n] = b;
  return b;
}

void Lower::registerDrop(const std::string& name, const TypeP& t, ir::Val ptr) {
  if (t && t->k == TyK::Named && t->decl->hasDrop && t->decl->k == Decl::Class) {
    Cleanup c; c.ptr = ptr; c.cls = t->decl; c.ty = t; c.var = name;
    scopes.back().cleanups.push_back(c);
  }
}

void Lower::emitCleanups(size_t lo, const std::string& skipVar) {
  for (size_t i = scopes.size(); i > lo; i--) {
    auto& cl = scopes[i - 1].cleanups;
    for (size_t k = cl.size(); k > 0; k--) {
      Cleanup c = cl[k - 1];
      if (c.isDefer) { stmt(c.node); continue; }
      if (!skipVar.empty() && c.var == skipVar) continue;
      callDrop(c.ty, c.ptr, nullptr);
    }
  }
}

// Runs the cleanups a jump from the scope state `snap` to a label with state `ls` leaves behind: every scope not on the
// label's chain completely, and in a shared scope only what was registered after the label (a backward jump).
void Lower::emitGotoCleanups(const std::vector<Scope>& snap, const LabelState& ls) {
  std::vector<Scope> saved = std::move(scopes);
  scopes = snap;  // defer bodies resolve their names as they did at the goto
  for (size_t i = scopes.size(); i > 0; i--) {
    size_t keep = 0;
    for (auto& p : ls.chain) if (p.first == scopes[i - 1].id) keep = p.second;
    std::vector<Cleanup> cl = scopes[i - 1].cleanups;
    for (size_t k = cl.size(); k > keep; k--) {
      Cleanup c = cl[k - 1];
      if (c.isDefer) stmt(c.node);
      else callDrop(c.ty, c.ptr, nullptr);
    }
  }
  scopes = std::move(saved);
}

ir::Val Lower::callMember(const Member* m, Decl* owner, ir::Val self, bool hasSelf, std::vector<ir::Val> args, ir::TyP ret) {
  std::string sym = symbolOfMember(m, owner);
  TypeP fty = m->ty;
  if (m->node && m->node->kind == NK::Init) { fty = std::make_shared<Type>(*m->ty); fty->ret = mkPrim("void"); }  // constructors return nothing
  declareFn(sym, fty, hasSelf, kC7CallConv, nullptr);
  std::vector<ir::Val> all;
  if (hasSelf) all.push_back(self);
  for (auto& a : args) all.push_back(a);
  return B->call(ir::vGlobal(sym), all, ret, kC7CallConv);
}

// Runs the destructor of class type `cls` (possibly a generic instance such as Vec<i32>) on the object at `ptr`.
void Lower::callDrop(const TypeP& cls, ir::Val ptr, Node* at) {
  Decl* d = cls->decl;
  for (auto& m : d->members) {
    if (!m.isMethod || m.name != "drop") continue;
    if (d->tparams.empty()) { callMember(&m, d, ptr, true, {}, ir::tVoid()); return; }
    SubMap sm = argMap(cls, at);
    ir::Func* f = declareGenericFn(symbolOfMember(&m, d), d->name + "::drop", m.ty, d->tparams, true, at);
    emitGenericCall(f, d->tparams, sm, m.ty, true, ptr, {}, at);
    return;
  }
}

void Lower::panicIf(ir::Val c, const std::string& msg) {
  declareC("c7_panic", ir::tVoid(), {ir::tPtr()});
  int pb = B->newBlock("panic"), cont = B->newBlock("cont");
  B->condBr(c, pb, cont);
  B->setBlock(pb);
  B->call(ir::vGlobal("c7_panic"), {M.addString(msg)}, ir::tVoid());
  B->unreachable();
  B->setBlock(cont);
}

// -fbounds-check: `idx` is already 64-bit; the unsigned compare also rejects negative indices.
void Lower::boundsCheck(ir::Val idx, ir::Val len) {
  if (!cg.boundsCheck) return;
  panicIf(B->icmp("uge", idx, len), "index out of bounds");
}

// --debug: + - * trap on overflow (spec 4.6) via llvm.{s,u}{add,sub,mul}.with.overflow. 128-bit multiplication is left
// wrapping: the signed form needs a compiler-rt helper (__muloti4) that the libgcc link does not provide.
V Lower::checkedArith(const std::string& op, V a, V b, const TypeP& t, const char* what) {
  ir::TyP it = a.v.ty;
  const char* kind = op == "+" ? "add" : op == "-" ? "sub" : "mul";
  if (op == "*" && it->bits > 64) return mk(B->bin("mul", a.v, b.v), t);
  std::string name = std::string("llvm.") + (isSigned(t) ? "s" : "u") + kind + ".with.overflow.i" + std::to_string(it->bits);
  ir::TyP rt = ir::tStruct({it, ir::tInt(1)});
  declareC(name.c_str(), rt, {it, it});
  ir::Val r = B->call(ir::vGlobal(name), {a.v, b.v}, rt);
  if (!what) what = op == "+" ? "addition" : op == "-" ? "subtraction" : "multiplication";
  panicIf(B->extract(r, 1, ir::tInt(1)), std::string("integer overflow in ") + what);
  return mk(B->extract(r, 0, it), t);
}

// ======================== module driver =================================================================================

bool Lower::run(Node& unit) {
  (void)unit;
  auto guard = [&](Node* at, const std::string& what, auto&& fn) {
    try { fn(); }
    catch (Unsupported& u) { S.d.report("E0901", u.loc.file >= 0 ? u.loc : (at ? at->loc : Loc{}), {u.what}); failed = true; }
    (void)what;
  };
  lowerGlobals();
  std::vector<Decl*> decls = S.allDecls;
  for (Decl* d : decls) {
    if (d->prelude || d->k != Decl::Func) continue;
    guard(d->node, d->name, [&] {
      S.declareDecl(d);
      S.curNs = d->ns;
      if (!d->tparams.empty()) { lowerGenericBody(d->node, symbolOf(d), d->name, d->ty, d->tparams, nullptr, false, false); return; }
      FuncParts fp = funcParts(d->node);
      if (!fp.body) { declareFn(symbolOf(d), d->ty, false, cc(d), d->node); return; }
      lowerFunctionBody(d->node, symbolOf(d), d->ty, nullptr, false, false, cc(d), d->name != "main");
    });
  }
  for (auto& kv : S.primImpl) decls.push_back(kv.second);  // impl Interface for i32 { ... }
  for (Decl* d : decls) {
    if (d->k == Decl::Func || d->k == Decl::Var || d->k == Decl::Alias) continue;  // prelude types may carry library-defined methods
    for (auto& m : d->members) {
      if (!m.isMethod || !m.node) continue;
      guard(m.node, m.name, [&] {
        S.curNs = d->ns;
        if (d->k == Decl::Interface) return;
        FuncParts fp = funcParts(m.node);
        bool init = m.node->kind == NK::Init;
        TypeP fty = m.ty;
        if (init) { fty = std::make_shared<Type>(*m.ty); fty->ret = mkPrim("void"); }
        bool self = !m.isStatic || init;
        if (!d->tparams.empty() || !m.tparams.empty()) {  // generic owner or generic method: one shared body + descriptors
          std::vector<std::string> tps = d->tparams;
          for (auto& t : m.tparams) tps.push_back(t);
          lowerGenericBody(m.node, symbolOfMember(&m, d), d->name + "::" + m.name, fty, tps, d, self, init);
          return;
        }
        if (!fp.body) { declareFn(symbolOfMember(&m, d), fty, self, kC7CallConv, m.node); return; }
        lowerFunctionBody(m.node, symbolOfMember(&m, d), fty, d, self, init, kC7CallConv, true);
      });
    }
  }
  return !failed;
}

void Lower::lowerGlobals() {
  for (Decl* d : S.allDecls) {
    if (d->prelude || d->k != Decl::Var) continue;
    try {
      S.declareDecl(d);
      if (hasMod(d->node->mods, "thread_local")) unsupported("thread_local variables", d->node);
      ir::TyP t = irTy(d->ty, d->node);
      ir::Global g;
      g.name = d->abi.empty() ? "_C7$" + dollar(d->full) : d->name;
      g.ty = t;
      Node* init = nullptr;
      for (auto& k : d->node->kids) if (!isTypeNode(k->kind) && k->kind != NK::Attribute) init = k.get();
      if (!d->abi.empty()) { g.external = true; M.globals.push_back(g); continue; }
      g.internal = true; g.isConst = !hasMod(d->node->mods, "mut");
      if (!init || init->kind == NK::Undefined) g.init = ir::vZero(t);
      else {
        bool neg = false;
        Node* l = init;
        if (l->kind == NK::Unary && l->text == "-" && l->kids[0]->kind == NK::IntLit) { neg = true; l = l->kids[0].get(); }
        uint64_t v; std::string suf; bool isF;
        if (l->kind == NK::IntLit || l->kind == NK::FloatLit) {
          parseNumber(l->text, v, suf, isF);
          if (ir::isFloat(t)) g.init = ir::vFloat(t, l->kind == NK::FloatLit ? std::strtod(l->text.c_str(), nullptr) * (neg ? -1 : 1) : (double)v * (neg ? -1 : 1));
          else g.init = ir::vInt(t, neg ? (uint64_t)(-(int64_t)v) : v);
        } else if (l->kind == NK::BoolLit) g.init = ir::vInt(t, l->text == "true");
        else if (l->kind == NK::NullLit) g.init = ir::vNull();
        else unsupported("non-constant global initializer", init);
      }
      M.globals.push_back(g);
    } catch (Unsupported& u) { S.d.report("E0901", u.loc, {u.what}); failed = true; }
  }
}

void Lower::lowerFunctionBody(Node* fnode, const std::string& sym, const TypeP& fnTy, Decl* owner, bool hasSelf, bool init, const std::string& ccs, bool internal) {
  FuncParts fp = funcParts(fnode);
  ir::Func* f = declareFn(sym, fnTy, hasSelf, ccs, fnode);
  if (!f->isDecl) return;
  f->isDecl = false; f->internal = internal && sym != "main";
  F = f;
  B = std::make_unique<ir::Builder>(M, *f);
  scopes.clear(); loops.clear(); labelBlocks.clear(); gs = GotoState(); switches.clear(); pendingLabel.clear();
  retType = fnTy->ret;
  haveSelf = hasSelf; selfDecl = owner; inInit = init;
  genericRet = false; genericDescs.clear();
  B->setBlock(B->newBlock("entry"));
  pushScope();
  size_t reg = 0;
  if (hasSelf) selfPtr = ir::vReg((int)reg++, ir::tPtr());
  for (size_t i = 0; i < fp.params.size() && i < fnTy->elems.size(); i++) {
    bool out = hasMod(fp.params[i]->mods, "out");
    ir::TyP pt = out ? ir::tPtr() : irTy(fnTy->elems[i], fnode);
    ir::Val slot = B->alloca_(pt);
    B->store(ir::vReg((int)reg++, pt), slot);
    if (!fp.params[i]->text.empty()) addLocal(fp.params[i]->text, fnTy->elems[i], slot, out, hasMod(fp.params[i]->mods, "mut"));
  }
  block(fp.body, true);
  if (!B->terminated()) {
    if (retType->k == TyK::Void) { emitCleanups(0, ""); B->ret(); }
    else B->unreachable();
  }
  scopes.clear();
  F = nullptr;
}

// ======================== statements ===================================================================================

void Lower::block(Node* n, bool newScope) {
  if (newScope) pushScope();
  for (auto& k : n->kids) stmt(k.get());
  if (newScope) {
    if (!B->terminated()) emitCleanups(scopes.size() - 1, "");
    popScopeNoEmit();
  }
}

ir::Val Lower::cond(Node* n) {
  V v = gen(n);
  if (!ir::isInt(v.v.ty) || v.v.ty->bits != 1) unsupported("non-bool condition", n);
  return v.v;
}

void Lower::localDecl(Node* n, const std::string& name, const TypeP& ty, Node* init) {
  ir::Val slot = allocaFor(ty, n);
  if (init && init->kind != NK::Undefined) {
    V v = gen(init, ty);
    storeVal(ty, v, slot, n);
  }
  addLocal(name, ty, slot, false, hasMod(n->mods, "mut"));
  registerDrop(name, ty, slot);
}

void Lower::stmt(Node* n) {
  switch (n->kind) {
    case NK::Block: block(n); return;
    case NK::Var: {
      Node* init = nullptr;
      for (auto& k : n->kids) if (!isTypeNode(k->kind)) init = k.get();
      localDecl(n, n->text, st(n), init);
      return;
    }
    case NK::AssignOrDecl:
      if (n->flag == 1) { localDecl(n, n->kids[0]->text, st(n), n->kids[1].get()); return; }
      assign(n);
      return;
    case NK::Assign: assign(n); return;
    case NK::TupleDecl: {
      Node* rhs = n->kids.back().get();
      V v = gen(rhs);
      for (size_t i = 0; i + 1 < n->kids.size(); i++) {
        TypeP et = st(n->kids[i].get());
        ir::Val slot = allocaFor(et, n);
        B->store(B->extract(v.v, (int)i, irTy(et, n)), slot);
        addLocal(n->kids[i]->text, et, slot, false, hasMod(n->kids[i]->mods, "mut"));
      }
      return;
    }
    case NK::ExprStmt: gen(n->kids[0].get()); return;
    case NK::If: {
      int th = B->newBlock("if.then"), el = n->kids.size() > 2 ? B->newBlock("if.else") : -1, end = B->newBlock("if.end");
      B->condBr(cond(n->kids[0].get()), th, el >= 0 ? el : end);
      B->setBlock(th);
      block(n->kids[1].get());
      if (!B->terminated()) B->br(end);
      if (el >= 0) {
        B->setBlock(el);
        stmt(n->kids[2].get());
        if (!B->terminated()) B->br(end);
      }
      B->setBlock(end);
      return;
    }
    case NK::While: {
      std::string lbl = pendingLabel; pendingLabel.clear();
      int c = B->newBlock("while.cond"), body = B->newBlock("while.body"), end = B->newBlock("while.end");
      B->br(c);
      B->setBlock(c);
      B->condBr(cond(n->kids[0].get()), body, end);
      B->setBlock(body);
      loops.push_back({end, c, scopes.size(), lbl, false});
      block(n->kids[1].get());
      loops.pop_back();
      if (!B->terminated()) B->br(c);
      B->setBlock(end);
      return;
    }
    case NK::DoWhile: {
      std::string lbl = pendingLabel; pendingLabel.clear();
      int body = B->newBlock("do.body"), c = B->newBlock("do.cond"), end = B->newBlock("do.end");
      B->br(body);
      B->setBlock(body);
      loops.push_back({end, c, scopes.size(), lbl, false});
      block(n->kids[0].get());
      loops.pop_back();
      if (!B->terminated()) B->br(c);
      B->setBlock(c);
      B->condBr(cond(n->kids[1].get()), body, end);
      B->setBlock(end);
      return;
    }
    case NK::For: forStmt(n); return;
    case NK::ForIn: forInStmt(n); return;
    case NK::Switch: switchStmt(n); return;
    case NK::Match: matchStmt(n); return;
    case NK::Return: {
      std::string skip;
      V rv;
      bool hasVal = !n->kids.empty();
      if (genericRet && hasVal) {  // T result: copy into the caller-provided slot
        rv = gen(n->kids[0].get(), retType);
        memcpyVal(sretPtr, rv.v, paramSize(retType, n));
        if (n->kids[0]->kind == NK::Path) skip = n->kids[0]->text;
        emitCleanups(0, skip);
        B->ret();
        deadBlock();
        return;
      }
      if (hasVal) {
        rv = gen(n->kids[0].get(), retType);
        if (n->kids[0]->kind == NK::Path) skip = n->kids[0]->text;
      }
      emitCleanups(0, skip);
      if (hasVal && !inInit) B->ret(rv.v); else B->ret();
      deadBlock();
      return;
    }
    case NK::Break: case NK::Continue: {
      bool isBreak = n->kind == NK::Break;
      LoopCtx* target = nullptr;
      for (size_t i = loops.size(); i > 0; i--) {
        LoopCtx& l = loops[i - 1];
        if (!n->text.empty()) { if (l.label == n->text) { target = &l; break; } continue; }
        if (!isBreak && l.isSwitch) continue;
        target = &l; break;
      }
      if (!target) unsupported("break/continue target", n);
      emitCleanups(target->scopeDepth, "");
      B->br(isBreak ? target->brk : target->cont);
      deadBlock();
      return;
    }
    case NK::Goto: {
      bool anyCleanup = false;
      for (auto& sc : scopes) if (!sc.cleanups.empty()) anyCleanup = true;
      auto known = gs.labels.find(n->text);
      if (known != gs.labels.end()) {  // backward jump: the label's scopes are known now
        std::vector<Scope> snap = scopes;
        emitGotoCleanups(snap, known->second);
        B->br(labelBlock(n->text));
      } else if (!anyCleanup) {
        B->br(labelBlock(n->text));    // nothing to run on the way out
      } else {                         // forward jump: park a trampoline, filled in at the label
        int t = B->newBlock("goto." + n->text);
        B->br(t);
        gs.pending.push_back({t, n->text, scopes, n});
      }
      deadBlock();
      return;
    }
    case NK::Label: {
      int lb = labelBlock(n->text);
      if (!B->terminated()) B->br(lb);
      LabelState st;
      for (auto& sc : scopes) st.chain.push_back({sc.id, sc.cleanups.size()});
      gs.labels[n->text] = st;
      for (size_t pi = 0; pi < gs.pending.size();) {
        if (gs.pending[pi].label != n->text) { pi++; continue; }
        PendingGoto pg = std::move(gs.pending[pi]);
        gs.pending.erase(gs.pending.begin() + pi);
        B->setBlock(pg.tramp);
        emitGotoCleanups(pg.snap, st);
        B->br(lb);
      }
      B->setBlock(lb);
      Node* child = n->kids[0].get();
      if (child->kind == NK::While || child->kind == NK::DoWhile || child->kind == NK::For || child->kind == NK::ForIn) pendingLabel = n->text;
      stmt(child);
      return;
    }
    case NK::Fallthrough: {
      if (switches.empty()) unsupported("fallthrough outside switch", n);
      SwitchCtx& sc = switches.back();
      if (sc.cur + 1 >= sc.bodies.size()) unsupported("fallthrough from last case", n);
      B->br(sc.bodies[sc.cur + 1]);
      deadBlock();
      return;
    }
    case NK::Defer: {
      Cleanup c; c.isDefer = true; c.node = n->kids[0].get();
      scopes.back().cleanups.push_back(c);
      return;
    }
    case NK::Free: freeStmt(n); return;
    default: unsupported(std::string("statement ") + nkName(n->kind), n);
  }
}

void Lower::freeStmt(Node* n) {
  if (n->kids.size() > 1) unsupported("free with custom allocator", n);
  V p = gen(n->kids[0].get());
  TypeP pt = st(n->kids[0].get());
  bool isArray = n->mods == "array";
  if (pt->k == TyK::Ptr && pt->elem->k == TyK::Named && pt->elem->decl->hasDrop) {
    if (isArray) unsupported("free[] of drop-type array", n);
    callDrop(pt->elem, p.v, n);
  }
  declareC(heapFn("free").c_str(), ir::tVoid(), {ir::tPtr()});
  B->call(ir::vGlobal(heapFn("free")), {p.v}, ir::tVoid());
}

void Lower::assign(Node* n) {
  Node* lhs = n->kids[0].get();
  Node* rhs = n->kids[1].get();
  TypeP lt = st(lhs);
  ir::Val addr = addrOf(lhs);
  const std::string& op = n->text;
  if (op == "=") { V v = gen(rhs, lt); storeVal(lt, v, addr, n); return; }
  if (lt->k == TyK::Param) unsupported("compound assignment on a generic value", n);
  std::string bop = op.substr(0, op.size() - 1);
  V cur = mk(B->load(irTy(lt, n), addr), lt);
  V r;
  if (lt->k == TyK::Ptr) {
    r = gen(rhs, mkPrim("isize"));
    ir::Val idx = toI64(r);
    if (bop == "-") idx = B->bin("sub", ci(ir::tInt(64), 0), idx);
    ir::Val p = elemGep(lt->elem, cur.v, idx, n);
    B->store(p, addr);
    return;
  }
  r = gen(rhs, lt);
  if (bop == "<<" || bop == ">>") { r = coerce(r, lt); }
  V res = arith(bop, cur, r, lt, n);
  B->store(res.v, addr);
}

void Lower::forStmt(Node* n) {
  std::string lbl = pendingLabel; pendingLabel.clear();
  pushScope();
  if (n->kids[0]->kind != NK::Empty) stmt(n->kids[0].get());
  int c = B->newBlock("for.cond"), body = B->newBlock("for.body"), step = B->newBlock("for.step"), end = B->newBlock("for.end");
  B->br(c);
  B->setBlock(c);
  if (n->kids[1]->kind != NK::Empty) B->condBr(cond(n->kids[1].get()), body, end); else B->br(body);
  B->setBlock(body);
  loops.push_back({end, step, scopes.size(), lbl, false});
  block(n->kids[3].get());
  loops.pop_back();
  if (!B->terminated()) B->br(step);
  B->setBlock(step);
  if (n->kids[2]->kind != NK::Empty) stmt(n->kids[2].get());
  B->br(c);
  B->setBlock(end);
  emitCleanups(scopes.size() - 1, "");
  popScopeNoEmit();
}

void Lower::forInStmt(Node* n) {
  std::string lbl = pendingLabel; pendingLabel.clear();
  Node* it = n->kids[0].get();
  TypeP et = st(n);
  pushScope();
  ir::TyP ety = et->k == TyK::Param ? ir::tPtr() : irTy(et, n);
  ir::Val idx = B->alloca_(ir::tInt(64));
  ir::Val var = allocaFor(et, n);
  ir::Val endv;  // i64 end (exclusive) as a register value stored in a slot
  ir::Val endSlot = B->alloca_(ir::tInt(64));
  ir::Val basePtr = ir::Val();
  bool range = it->kind == NK::Range;
  bool signedR = false;
  ir::TyP rty = ir::tInt(64);
  if (range) {
    if (it->kids[0]->kind == NK::Empty || it->kids[1]->kind == NK::Empty) unsupported("open-ended range in for", n);
    V a = gen(it->kids[0].get(), et), b = gen(it->kids[1].get(), et);
    a = coerce(a, et); b = coerce(b, et);
    signedR = isSigned(et);
    rty = ety;
    B->store(a.v, idx = B->alloca_(rty));
    endSlot = B->alloca_(rty);
    ir::Val e = b.v;
    if (it->text == "..=") e = B->bin("add", e, ci(rty, 1));
    B->store(e, endSlot);
  } else {
    TypeP bt = st(it);
    B->store(ci(ir::tInt(64), 0), idx);
    if (bt->k == TyK::Array) {
      basePtr = addrOrSpill(it);
      B->store(ci(ir::tInt(64), bt->len), endSlot);
    } else if (bt->k == TyK::Slice) {
      V s = gen(it);
      basePtr = B->extract(s.v, 0, ir::tPtr());
      B->store(B->extract(s.v, 1, ir::tInt(64)), endSlot);
    } else if (bt->k == TyK::Named) {
      iterLoop(n, it, et, lbl);
      return;
    } else unsupported("for-in over this type", n);
    // keep the base pointer in a slot so it survives block boundaries
    ir::Val bslot = B->alloca_(ir::tPtr());
    B->store(basePtr, bslot);
    basePtr = bslot;
  }
  int c = B->newBlock("forin.cond"), body = B->newBlock("forin.body"), step = B->newBlock("forin.step"), end = B->newBlock("forin.end");
  B->br(c);
  B->setBlock(c);
  ir::Val cur = B->load(rty, idx), lim = B->load(rty, endSlot);
  B->condBr(B->icmp(range && signedR ? "slt" : "ult", cur, lim), body, end);
  B->setBlock(body);
  pushScope();
  if (range) B->store(B->load(rty, idx), var);
  else {
    ir::Val bp = B->load(ir::tPtr(), basePtr);
    ir::Val ep = elemGep(et, bp, B->load(ir::tInt(64), idx), n);
    storeVal(et, loadVal(et, ep, n), var, n);
  }
  addLocal(n->text, et, var);
  loops.push_back({end, step, scopes.size() - 1, lbl, false});
  for (auto& k : n->kids[1]->kids) stmt(k.get());
  loops.pop_back();
  if (!B->terminated()) emitCleanups(scopes.size() - 1, "");
  popScopeNoEmit();
  if (!B->terminated()) B->br(step);
  B->setBlock(step);
  B->store(B->bin("add", B->load(rty, idx), ci(rty, 1)), idx);
  B->br(c);
  B->setBlock(end);
  popScopeNoEmit();
}

// for (x in iterator): the iterator is copied into a temporary and `next()` is called until it yields None.
void Lower::iterLoop(Node* n, Node* it, const TypeP& et, const std::string& lbl) {
  V iv = gen(it);
  TypeP itT = iv.ty;
  auto ms = S.findMembers(itT, "next");
  const Member* next = nullptr;
  for (auto& pr : ms) if (pr.first->isMethod) next = pr.first;
  if (!next) unsupported("iterator without next()", n);
  TypeP optT = next->ty->ret;
  if (optT->k != TyK::Named || optT->decl != S.dOption) unsupported("next() must return Option<T>", n);
  ir::Val tmp = B->alloca_(irTy(itT, n));
  B->store(iv.v, tmp);
  ir::Val var = allocaFor(et, n);
  int c = B->newBlock("iter.next"), body = B->newBlock("iter.body"), end = B->newBlock("iter.end");
  B->br(c);
  B->setBlock(c);
  ir::Val r = callMember(next, next->owner, tmp, true, {}, irTy(optT, n));
  TagInfo& ti = tagInfo(optT, n);
  ir::Val slot = B->alloca_(ti.full);
  B->store(r, slot);
  ir::Val tag = B->load(ti.tagTy, B->gepField(ti.full, slot, 0));
  B->condBr(B->icmp("eq", tag, ci(ti.tagTy, 0)), body, end);  // Some is variant 0
  B->setBlock(body);
  pushScope();
  V pv = variantPayload(optT, slot, 0, 0, n);
  storeVal(et, pv, var, n);
  addLocal(n->text, et, var);
  loops.push_back({end, c, scopes.size() - 1, lbl, false});
  for (auto& k : n->kids[1]->kids) stmt(k.get());
  loops.pop_back();
  if (!B->terminated()) emitCleanups(scopes.size() - 1, "");
  popScopeNoEmit();
  if (!B->terminated()) B->br(c);
  B->setBlock(end);
  popScopeNoEmit();  // the scope opened by forInStmt
}

void Lower::switchStmt(Node* n) {
  V sv = gen(n->kids[0].get());
  TypeP stp = st(n->kids[0].get());
  int end = B->newBlock("switch.end");
  std::vector<Node*> cases;
  for (size_t i = 1; i < n->kids.size(); i++) cases.push_back(n->kids[i].get());
  SwitchCtx sc;
  for (size_t i = 0; i < cases.size(); i++) sc.bodies.push_back(B->newBlock("case"));
  int dflt = -1;
  for (size_t i = 0; i < cases.size(); i++) if (cases[i]->kind == NK::Default) dflt = sc.bodies[i];
  // dispatch chain
  for (size_t i = 0; i < cases.size(); i++) {
    if (cases[i]->kind == NK::Default) continue;
    for (auto& v : cases[i]->kids) {
      if (v->kind == NK::Body) continue;
      int next = B->newBlock("case.test");
      ir::Val hit;
      if (v->kind == NK::Range) {
        V lo = coerce(gen(v->kids[0].get(), stp), stp), hi = coerce(gen(v->kids[1].get(), stp), stp);
        bool sg = isSigned(stp);
        ir::Val a = B->icmp(sg ? "sge" : "uge", sv.v, lo.v);
        ir::Val b2 = B->icmp(v->text == "..=" ? (sg ? "sle" : "ule") : (sg ? "slt" : "ult"), sv.v, hi.v);
        hit = B->bin("and", a, b2);
      } else {
        V cv = coerce(gen(v.get(), stp), stp);
        hit = B->icmp("eq", sv.v, cv.v);
      }
      B->condBr(hit, sc.bodies[i], next);
      B->setBlock(next);
    }
  }
  B->br(dflt >= 0 ? dflt : end);
  switches.push_back(sc);
  loops.push_back({end, -1, scopes.size(), "", true});
  for (size_t i = 0; i < cases.size(); i++) {
    switches.back().cur = i;
    B->setBlock(sc.bodies[i]);
    pushScope();
    for (auto& v : cases[i]->kids) if (v->kind == NK::Body) for (auto& s : v->kids) stmt(s.get());
    if (!B->terminated()) { emitCleanups(scopes.size() - 1, ""); B->br(end); }
    popScopeNoEmit();
  }
  loops.pop_back();
  switches.pop_back();
  B->setBlock(end);
}

ir::Val Lower::patTest(Node* pat, const V& scrut, std::vector<std::pair<std::string, V>>& binds) {
  const TypeP& t = scrut.ty;
  ir::Val tru = ci(ir::tInt(1), 1);
  if (t->k == TyK::Named && t->decl->k == Decl::Tagged && (pat->kind == NK::PatVariant || pat->kind == NK::PatName)) {
    std::string nm = pat->text.substr(pat->text.rfind(':') == std::string::npos ? 0 : pat->text.rfind(':') + 1);
    size_t vi = 0;
    bool found = false;
    for (auto& mm : t->decl->members) { if (!mm.isVariant) continue; if (mm.name == nm) { found = true; break; } vi++; }
    if (!found) {
      if (pat->kind == NK::PatName) { binds.push_back({pat->text, scrut}); return tru; }
      unsupported("pattern `" + nm + "`", pat);
    }
    TagInfo& ti = tagInfo(t, pat);
    ir::Val slot = B->alloca_(ti.full);
    B->store(scrut.v, slot);
    ir::Val tag = B->load(ti.tagTy, B->gepField(ti.full, slot, 0));
    ir::Val hit = B->icmp("eq", tag, ci(ti.tagTy, (int64_t)vi));
    if (pat->kind == NK::PatVariant)
      for (size_t i = 0; i < pat->kids.size() && i < ti.sema[vi].size(); i++) {
        V pv = variantPayload(t, slot, vi, i, pat);
        if (pv.v.k == ir::Val::None) continue;
        hit = B->bin("and", hit, patTest(pat->kids[i].get(), pv, binds));
      }
    return hit;
  }
  switch (pat->kind) {
    case NK::PatWild: return tru;
    case NK::PatName: {
      if (t->k == TyK::Named && t->decl->k == Decl::Enum) {
        std::string nm = pat->text.substr(pat->text.rfind(':') == std::string::npos ? 0 : pat->text.rfind(':') + 1);
        for (auto& m : t->decl->members)
          if (m.isEnumerator && m.name == nm) return B->icmp("eq", scrut.v, ci(scrut.v.ty, enumValue(t->decl, nm)));
      }
      binds.push_back({pat->text, scrut});
      return tru;
    }
    case NK::PatLit: {
      const std::string& s = pat->text;
      if (s == "true" || s == "false") return B->icmp("eq", scrut.v, ci(ir::tInt(1), s == "true"));
      if (!s.empty() && s[0] == '\'') return B->icmp("eq", scrut.v, ci(ir::tInt(32), decodeCharText(s)));
      bool neg = !s.empty() && s[0] == '-';
      uint64_t v; std::string suf; bool f;
      parseNumber(neg ? s.substr(1) : s, v, suf, f);
      if (f) unsupported("float pattern", pat);
      return B->icmp("eq", scrut.v, ci(scrut.v.ty, neg ? -(int64_t)v : (int64_t)v));
    }
    case NK::PatRange: {
      auto litv = [&](Node* l) {
        bool neg = l->text[0] == '-';
        uint64_t v; std::string suf; bool f;
        parseNumber(neg ? l->text.substr(1) : l->text, v, suf, f);
        return ci(scrut.v.ty, neg ? -(int64_t)v : (int64_t)v);
      };
      bool sg = isSigned(t);
      ir::Val a = B->icmp(sg ? "sge" : "uge", scrut.v, litv(pat->kids[0].get()));
      ir::Val b2 = B->icmp(pat->text == "..=" ? (sg ? "sle" : "ule") : (sg ? "slt" : "ult"), scrut.v, litv(pat->kids[1].get()));
      return B->bin("and", a, b2);
    }
    default: unsupported("this pattern kind (tagged union / tuple match)", pat);
  }
}

void Lower::matchStmt(Node* n) {
  V sv = gen(n->kids[0].get());
  int end = B->newBlock("match.end");
  for (size_t i = 1; i < n->kids.size(); i++) {
    Node* arm = n->kids[i].get();
    std::vector<std::pair<std::string, V>> binds;
    ir::Val hit = patTest(arm->kids[0].get(), sv, binds);
    int body = B->newBlock("arm"), next = B->newBlock("arm.next");
    B->condBr(hit, body, next);
    B->setBlock(body);
    pushScope();
    for (auto& bd : binds) {
      ir::Val slot = allocaFor(bd.second.ty, arm);
      B->store(bd.second.v, slot);
      addLocal(bd.first, bd.second.ty, slot);
    }
    stmt(arm->kids[1].get());
    if (!B->terminated()) { emitCleanups(scopes.size() - 1, ""); B->br(end); }
    popScopeNoEmit();
    B->setBlock(next);
  }
  B->unreachable();  // sema proved exhaustiveness
  B->setBlock(end);
}


// ======================== closures ======================================================================================

Lower::FnState Lower::saveState() {
  FnState st;
  st.F = F; st.B = std::move(B); st.scopes = std::move(scopes); st.loops = std::move(loops);
  st.labelBlocks = std::move(labelBlocks); st.gs = std::move(gs); st.pendingLabel = pendingLabel; st.retType = retType;
  st.haveSelf = haveSelf; st.selfPtr = selfPtr; st.selfDecl = selfDecl; st.inInit = inInit;
  st.switches = std::move(switches); st.genericRet = genericRet; st.sretPtr = sretPtr;
  scopes.clear(); loops.clear(); labelBlocks.clear(); gs = GotoState(); switches.clear(); pendingLabel.clear();
  return st;
}

void Lower::restoreState(FnState&& st) {
  F = st.F; B = std::move(st.B); scopes = std::move(st.scopes); loops = std::move(st.loops);
  labelBlocks = std::move(st.labelBlocks); gs = std::move(st.gs); pendingLabel = st.pendingLabel; retType = st.retType;
  haveSelf = st.haveSelf; selfPtr = st.selfPtr; selfDecl = st.selfDecl; inInit = st.inInit;
  switches = std::move(st.switches); genericRet = st.genericRet; sretPtr = st.sretPtr;
}

void Lower::freeNames(Node* n, std::set<std::string>& out) {
  if (!n) return;
  if (n->kind == NK::Path && n->text.find("::") == std::string::npos) out.insert(n->text);
  for (auto& k : n->kids) freeNames(k.get(), out);
}

V Lower::genLambda(Node* n) {
  if (!genericDescs.empty()) unsupported("closures inside generic functions", n);
  std::vector<Node*> params, caps;
  Node* body = nullptr;
  TypeP ret = mkPrim("void");
  for (auto& k : n->kids) {
    if (k->kind == NK::Capture) caps.push_back(k.get());
    else if (k->kind == NK::Param) params.push_back(k.get());
    else if (k->kind == NK::Block) body = k.get();
    else if (isTypeNode(k->kind)) ret = S.resolveType(k.get());
  }
  std::vector<TypeP> ptypes;
  for (Node* p : params) ptypes.push_back(S.resolveType(p->kids[0].get()));
  std::set<std::string> pnames;
  for (Node* p : params) pnames.insert(p->text);

  struct Cap { std::string name; TypeP ty; bool byRef; bool mut; };
  std::vector<Cap> list;
  std::set<std::string> names;
  freeNames(body, names);
  auto consider = [&](const std::string& nm, int mode /*0 auto,1 value,2 ref*/) {
    if (pnames.count(nm)) return;
    LocalVar* lv = findLocal(nm);
    if (!lv) return;
    if (lv->ty->k == TyK::Param) unsupported("capturing a generic value", n);
    bool byRef = mode == 0 ? lv->mut : mode == 2;
    list.push_back({nm, lv->ty, byRef, lv->mut});
  };
  if (caps.empty()) { for (auto& nm : names) consider(nm, 0); }
  else if (caps[0]->text == "=") { for (auto& nm : names) consider(nm, 1); }
  else if (caps[0]->text == "&") { for (auto& nm : names) consider(nm, 2); }
  else {
    for (Node* c : caps) {
      if (!findLocal(c->text)) unsupported("capture of `" + c->text + "`", c);
      consider(c->text, c->mods == "&" ? 2 : 1);
    }
  }

  // environment layout: [self pointer] + captured variables (by value, or as pointers when by reference)
  std::vector<ir::TyP> fields;
  if (haveSelf) fields.push_back(ir::tPtr());
  size_t selfOff = fields.size();
  for (auto& c : list) fields.push_back(c.byRef ? ir::tPtr() : irTy(c.ty, n));
  ir::TyP envTy = ir::tStruct(fields);
  bool heap = n->mods == "heap";
  ir::Val env;
  if (heap) {
    declareC(heapFn("malloc").c_str(), ir::tPtr(), {ir::tInt(64)});
    env = B->call(ir::vGlobal(heapFn("malloc")), {ci(ir::tInt(64), (int64_t)std::max<uint64_t>(ir::sizeOf(envTy), 1))}, ir::tPtr());
  } else env = B->alloca_(envTy);
  if (haveSelf) B->store(selfPtr, B->gepField(envTy, env, 0));
  for (size_t i = 0; i < list.size(); i++) {
    ir::Val addr;
    varAddr(list[i].name, addr);
    ir::Val slot = B->gepField(envTy, env, (int)(selfOff + i));
    if (list[i].byRef) B->store(addr, slot);
    else B->store(B->load(irTy(list[i].ty, n), addr), slot);
  }

  // the lambda body becomes its own function: (env, params...)
  std::string sym = "_C7$lambda$" + std::to_string(lambdaCount++);
  std::vector<ir::TyP> ips{ir::tPtr()};
  for (size_t i = 0; i < params.size(); i++) ips.push_back(hasMod(params[i]->mods, "out") ? ir::tPtr() : irTy(ptypes[i], n));
  ir::TyP retIr = irTy(ret, n);
  bool wasSelf = haveSelf;
  Decl* sd = selfDecl;
  FnState saved = saveState();
  ir::Func* f = M.declare(sym, retIr, ips);
  f->cc = kC7CallConv; f->isDecl = false; f->internal = true;
  F = f;
  B = std::make_unique<ir::Builder>(M, *f);
  retType = ret; genericRet = false; inInit = false;
  haveSelf = wasSelf; selfDecl = sd;
  B->setBlock(B->newBlock("entry"));
  pushScope();
  ir::Val envIn = ir::vReg(0, ir::tPtr());
  if (wasSelf) selfPtr = B->load(ir::tPtr(), B->gepField(envTy, envIn, 0));
  for (size_t i = 0; i < list.size(); i++) {
    ir::Val fld = B->gepField(envTy, envIn, (int)(selfOff + i));
    if (list[i].byRef) {
      ir::Val slot = B->alloca_(ir::tPtr());
      B->store(B->load(ir::tPtr(), fld), slot);
      addLocal(list[i].name, list[i].ty, slot, true, list[i].mut);
    } else addLocal(list[i].name, list[i].ty, fld, false, list[i].mut);  // by-value captures live in the environment
  }
  size_t reg = 1;
  for (size_t i = 0; i < params.size(); i++) {
    bool out = hasMod(params[i]->mods, "out");
    ir::TyP pt = ips[i + 1];
    ir::Val slot = B->alloca_(pt);
    B->store(ir::vReg((int)reg++, pt), slot);
    if (!params[i]->text.empty()) addLocal(params[i]->text, ptypes[i], slot, out, hasMod(params[i]->mods, "mut"));
  }
  block(body, true);
  if (!B->terminated()) {
    if (ret->k == TyK::Void) { emitCleanups(0, ""); B->ret(); } else B->unreachable();
  }
  restoreState(std::move(saved));

  ir::Val cl = ir::vUndef(ir::tStruct({ir::tPtr(), ir::tPtr()}));
  cl = B->insert(cl, ir::vGlobal(sym), 0);
  cl = B->insert(cl, env, 1);
  return mk(cl, st(n));
}

// ======================== generics (shared code, spec 10.1) =====================================================

bool Lower::containsParam(const TypeP& t) {
  if (!t) return false;
  if (t->k == TyK::Param) return true;
  if (t->elem && containsParam(t->elem)) return true;
  for (auto& e : t->elems) if (containsParam(e)) return true;
  return t->ret && containsParam(t->ret);
}

// In shared code a type parameter may appear directly (passed by reference), behind a pointer, or as a
// slice element. Aggregates that embed T by value would need dynamic layouts and are not supported yet.
bool Lower::okGenericType(const TypeP& t) {
  if (!containsParam(t)) return true;
  return t->k == TyK::Param || t->k == TyK::Ptr || t->k == TyK::Slice;
}

int Lower::ifaceId(Decl* d) {
  auto it = ifaceIds.find(d);
  if (it != ifaceIds.end()) return it->second;
  int id = (int)ifaceIds.size() + 1;
  ifaceIds[d] = id;
  return id;
}

ir::Val Lower::paramSize(const TypeP& t, Node* at) {
  auto it = genericDescs.find(t->name);
  if (it == genericDescs.end()) unsupported("type parameter `" + t->name + "` outside its generic function", at);
  ir::TyP hdr = ir::tStruct({ir::tInt(64), ir::tInt(64), ir::tPtr(), ir::tInt(64)});
  return B->load(ir::tInt(64), B->gepField(hdr, it->second, 0));
}

void Lower::memcpyVal(ir::Val dst, ir::Val src, ir::Val size) {
  M.declare("llvm.memcpy.p0.p0.i64", ir::tVoid(), {ir::tPtr(), ir::tPtr(), ir::tInt(64), ir::tInt(1)});
  B->call(ir::vGlobal("llvm.memcpy.p0.p0.i64"), {dst, src, size, ci(ir::tInt(1), 0)}, ir::tVoid());
}

V Lower::loadVal(const TypeP& t, ir::Val addr, Node* at) {
  if (t->k == TyK::Param) return mk(addr, t);  // a generic value is represented by the address of its storage
  return mk(B->load(irTy(t, at), addr), t);
}

void Lower::storeVal(const TypeP& t, V v, ir::Val addr, Node* at) {
  if (t->k == TyK::Param) { memcpyVal(addr, v.v, paramSize(t, at)); return; }
  B->store(v.v, addr);
}

ir::Val Lower::elemGep(const TypeP& elem, ir::Val p, ir::Val idx, Node* at) {
  if (elem->k == TyK::Param) return B->gep(ir::tInt(8), p, {B->bin("mul", idx, paramSize(elem, at))});
  return B->gep(irTy(elem, at), p, {idx});
}

// IR parameter order: [sret?][self?][one descriptor per type parameter][ordinary arguments].
// `tparams` = the owner type's parameters followed by the method's own.
ir::Func* Lower::declareGenericFn(const std::string& sym, const std::string& nm, const TypeP& fnTy, const std::vector<std::string>& tparams, bool hasSelf, Node* at) {
  if (ir::Func* f = M.find(sym)) return f;
  if (!okGenericType(fnTy->ret)) unsupported("generic function `" + nm + "` returning an aggregate that embeds T", at);
  std::vector<ir::TyP> ps;
  bool sret = fnTy->ret->k == TyK::Param;
  if (sret) ps.push_back(ir::tPtr());
  if (hasSelf) ps.push_back(ir::tPtr());
  for (size_t i = 0; i < tparams.size(); i++) ps.push_back(ir::tPtr());  // one descriptor per type parameter
  for (size_t i = 0; i < fnTy->elems.size(); i++) {
    const TypeP& pt = fnTy->elems[i];
    if (!okGenericType(pt)) unsupported("generic function `" + nm + "` taking an aggregate that embeds T by value", at);
    bool out = i < fnTy->pmods.size() && hasMod(fnTy->pmods[i], "out");
    ps.push_back((out || pt->k == TyK::Param) ? ir::tPtr() : irTy(pt, at));
  }
  ir::Func* f = M.declare(sym, sret ? ir::tVoid() : irTy(fnTy->ret, at), ps);
  f->cc = kC7CallConv;
  return f;
}

void Lower::lowerGenericBody(Node* fnode, const std::string& sym, const std::string& nm, const TypeP& fnTy, const std::vector<std::string>& tparams, Decl* owner, bool hasSelf, bool isInit) {
  FuncParts fp = funcParts(fnode);
  ir::Func* f = declareGenericFn(sym, nm, fnTy, tparams, hasSelf, fnode);
  if (!fp.body || !f->isDecl) return;
  f->isDecl = false; f->internal = true;
  F = f;
  B = std::make_unique<ir::Builder>(M, *f);
  scopes.clear(); loops.clear(); labelBlocks.clear(); gs = GotoState(); switches.clear(); pendingLabel.clear();
  retType = fnTy->ret;
  haveSelf = hasSelf; selfDecl = owner; inInit = isInit;
  genericDescs.clear();
  genericRet = retType->k == TyK::Param;
  B->setBlock(B->newBlock("entry"));
  pushScope();
  size_t reg = 0;
  if (genericRet) sretPtr = ir::vReg((int)reg++, ir::tPtr());
  if (hasSelf) selfPtr = ir::vReg((int)reg++, ir::tPtr());
  for (auto& tp : tparams) genericDescs[tp] = ir::vReg((int)reg++, ir::tPtr());
  for (size_t i = 0; i < fp.params.size() && i < fnTy->elems.size(); i++) {
    const TypeP& pt = fnTy->elems[i];
    bool out = hasMod(fp.params[i]->mods, "out");
    const std::string& name = fp.params[i]->text;
    if (out) {
      ir::Val slot = B->alloca_(ir::tPtr());
      B->store(ir::vReg((int)reg++, ir::tPtr()), slot);
      if (!name.empty()) addLocal(name, pt, slot, true);
    } else if (pt->k == TyK::Param) {  // copy the caller's value so the callee owns a private slot
      ir::Val in = ir::vReg((int)reg++, ir::tPtr());
      ir::Val slot = allocaFor(pt, fnode);
      memcpyVal(slot, in, paramSize(pt, fnode));
      if (!name.empty()) addLocal(name, pt, slot);
    } else {
      ir::TyP it = irTy(pt, fnode);
      ir::Val slot = B->alloca_(it);
      B->store(ir::vReg((int)reg++, it), slot);
      if (!name.empty()) addLocal(name, pt, slot);
    }
  }
  block(fp.body, true);
  if (!B->terminated()) {
    if (retType->k == TyK::Void || genericRet) { emitCleanups(0, ""); B->ret(); }
    else B->unreachable();
  }
  scopes.clear();
  genericDescs.clear();
  genericRet = false;
  F = nullptr;
}

ir::Val Lower::descOf(const TypeP& t, Node* at) {
  if (t->k == TyK::Param) {
    auto it = genericDescs.find(t->name);
    if (it == genericDescs.end()) unsupported("type parameter `" + t->name + "` is not in scope", at);
    return it->second;  // forwarded unchanged: descriptors list every interface the type implements
  }
  std::string key = ts(t);
  auto c = descCache.find(key);
  if (c != descCache.end()) return c->second;
  if (S.isDrop(t)) unsupported("generic instantiation with a drop type `" + key + "` (needs descriptor drop support)", at);
  ir::TyP it = irTy(t, at);
  ir::TyP i64 = ir::tInt(64), pt = ir::tPtr();
  ir::TyP entryTy = ir::tStruct({i64, pt});
  std::string k = sanitize(key);
  std::vector<ir::Val> entries;
  for (auto& p : S.impls) {
    if (p.first != S.implKey(t)) continue;
    Decl* iface = p.second;
    S.declareDecl(iface);
    std::vector<ir::Val> slots;
    for (auto& im : iface->members) {
      if (!im.isMethod) continue;
      if (im.isStatic) unsupported("static interface method `" + im.name + "` in a generic instantiation", at);
      for (auto& ep : im.ty->elems) if (ep->k == TyK::Param && ep->name == "Self") unsupported("interface method `" + im.name + "` taking Self by value", at);
      if (im.ty->ret->k == TyK::Param && im.ty->ret->name == "Self") unsupported("interface method `" + im.name + "` returning Self by value", at);
      const Member* impl = nullptr;
      for (auto& pr : S.findMembers(t, im.name)) if (pr.first->owner->k != Decl::Interface) impl = pr.first;
      if (!impl) unsupported("`" + key + "` has no implementation of `" + im.name + "`", at);
      std::string sym = symbolOfMember(impl, impl->owner);
      declareFn(sym, impl->ty, true, kC7CallConv, at);
      slots.push_back(ir::vGlobal(sym));
    }
    ir::TyP vtTy = ir::tArray(pt, slots.size());
    ir::Global vg;
    vg.name = "_C7$vt$" + k + "$" + sanitize(dollar(iface->full));
    vg.ty = vtTy; vg.isConst = true; vg.internal = true; vg.init = ir::vConstAgg(vtTy, slots);
    M.globals.push_back(vg);
    entries.push_back(ir::vConstAgg(entryTy, {ci(i64, ifaceId(iface)), ir::vGlobal(vg.name)}));
  }
  ir::TyP entsTy = ir::tArray(entryTy, entries.size());
  ir::TyP descTy = ir::tStruct({i64, i64, pt, i64, entsTy});
  ir::Global g;
  g.name = "_C7$desc$" + k;
  g.ty = descTy; g.isConst = true; g.internal = true;
  g.init = ir::vConstAgg(descTy, {ci(i64, (int64_t)ir::sizeOf(it)), ci(i64, (int64_t)ir::alignOf(it)), ir::vNull(), ci(i64, (int64_t)entries.size()), ir::vConstAgg(entsTy, entries)});
  M.globals.push_back(g);
  ir::Val v = ir::vGlobal(g.name);
  descCache[key] = v;
  return v;
}

V Lower::emitGenericCall(ir::Func* f, const std::vector<std::string>& tparams, const SubMap& sm, const TypeP& fnTy, bool hasSelf, ir::Val self, const std::vector<Node*>& argNodes, Node* at) {
  TypeP retC = subst(fnTy->ret, sm);
  std::vector<ir::Val> args;
  ir::Val retSlot;
  bool sret = fnTy->ret->k == TyK::Param;
  if (sret) {
    retSlot = retC->k == TyK::Param ? B->allocaDyn(paramSize(retC, at)) : B->alloca_(irTy(retC, at));
    args.push_back(retSlot);
  }
  if (hasSelf) args.push_back(self);
  for (auto& tp : tparams) args.push_back(descOf(sm.at(tp), at));
  for (size_t i = 0; i < fnTy->elems.size() && i < argNodes.size(); i++) {
    Node* a = argNodes[i];
    const TypeP& pt = fnTy->elems[i];
    if (a->kind == NK::OutArg) { args.push_back(addrOf(a->kids[0].get())); continue; }
    TypeP conc = subst(pt, sm);
    V v = gen(a, conc);
    if (pt->k == TyK::Param) {
      if (conc->k == TyK::Param) args.push_back(v.v);  // already an address (we are generic code ourselves)
      else {
        ir::Val slot = B->alloca_(irTy(conc, at));
        B->store(coerce(v, conc).v, slot);
        args.push_back(slot);
      }
    } else args.push_back(coerce(v, conc).v);
  }
  ir::Val r = B->call(ir::vGlobal(f->name), args, f->ret, kC7CallConv);
  if (sret) return retC->k == TyK::Param ? mk(retSlot, retC) : mk(B->load(irTy(retC, at), retSlot), retC);
  return mk(r, retC);
}

V Lower::genGenericCall(Node* n, Decl* fd) {
  ir::Func* f = declareGenericFn(symbolOf(fd), fd->name, fd->ty, fd->tparams, false, n);
  if (n->targs.size() != fd->tparams.size()) unsupported("generic call without inferred type arguments", n);
  SubMap sm;
  for (size_t i = 0; i < fd->tparams.size(); i++) {
    TypeP ta = std::static_pointer_cast<Type>(n->targs[i]);
    if (isErr(ta)) unsupported("cannot infer type argument `" + fd->tparams[i] + "` (add a type annotation)", n);
    sm[fd->tparams[i]] = ta;
  }
  std::vector<Node*> argNodes;
  for (size_t i = 1; i < n->kids.size(); i++) argNodes.push_back(n->kids[i].get());
  return emitGenericCall(f, fd->tparams, sm, fd->ty, false, ir::Val(), argNodes, n);
}

// A call to a member of a generic type (Vec<i32>::push) or to a generic method. Owner type arguments come from
// n->ownerTargs, method-level ones from n->targs; inside generic code they are Param types and descOf forwards the descriptor.
V Lower::genGenericMemberCall(Node* n, const Member* m) {
  Node* callee = n->kids[0].get();
  Decl* owner = m->owner;
  TypeP rty = st(n);
  bool isCtor = m->node && m->node->kind == NK::Init;
  std::vector<std::string> tparams = owner->tparams;
  for (auto& t : m->tparams) tparams.push_back(t);
  if (n->ownerTargs.size() != owner->tparams.size() || n->targs.size() != m->tparams.size())
    unsupported("generic member call without inferred type arguments", n);
  SubMap sm;
  for (size_t i = 0; i < owner->tparams.size(); i++) {
    TypeP ta = std::static_pointer_cast<Type>(n->ownerTargs[i]);
    if (isErr(ta)) unsupported("cannot infer type argument `" + owner->tparams[i] + "` of `" + owner->name + "` (add a type annotation)", n);
    sm[owner->tparams[i]] = ta;
  }
  for (size_t i = 0; i < m->tparams.size(); i++) {
    TypeP ta = std::static_pointer_cast<Type>(n->targs[i]);
    if (isErr(ta)) unsupported("cannot infer type argument `" + m->tparams[i] + "` (add a type annotation)", n);
    sm[m->tparams[i]] = ta;
  }
  TypeP fty = m->ty;
  if (isCtor) { fty = std::make_shared<Type>(*m->ty); fty->ret = mkPrim("void"); }  // constructors return nothing
  bool hasSelf = !m->isStatic || isCtor;
  ir::Func* f = declareGenericFn(symbolOfMember(m, owner), owner->name + "::" + m->name, fty, tparams, hasSelf, n);
  std::vector<Node*> argNodes;
  for (size_t i = 1; i < n->kids.size(); i++) argNodes.push_back(n->kids[i].get());
  if (isCtor) {
    ir::TyP sty = irTy(rty, n);
    ir::Val tmp = B->alloca_(sty);
    B->store(ir::vZero(sty), tmp);
    emitGenericCall(f, tparams, sm, fty, true, tmp, argNodes, n);
    return mk(B->load(sty, tmp), rty);
  }
  ir::Val self;
  if (hasSelf) {
    if (callee->kind == NK::Member || callee->kind == NK::Arrow) {
      Node* recv = callee->kids[0].get();
      self = st(recv)->k == TyK::Ptr ? gen(recv).v : addrOrSpill(recv);
    } else if (haveSelf) self = selfPtr;
    else unsupported("method call without receiver", n);
  }
  return emitGenericCall(f, tparams, sm, fty, hasSelf, self, argNodes, n);
}

V Lower::genBoundMethodCall(Node* n, const Member* m) {
  Node* callee = n->kids[0].get();
  if (callee->kind != NK::Member && callee->kind != NK::Arrow) unsupported("interface method call without receiver", n);
  Node* recv = callee->kids[0].get();
  TypeP rt = st(recv);
  TypeP base = rt->k == TyK::Ptr ? rt->elem : rt;
  if (base->k != TyK::Param) unsupported("dynamic interface dispatch through an interface pointer", n);
  if (m->isStatic) unsupported("static interface method call", n);
  for (auto& ep : m->ty->elems) if (ep->k == TyK::Param && ep->name == "Self") unsupported("interface method taking Self by value", n);
  if (m->ty->ret->k == TyK::Param) unsupported("interface method returning a type parameter", n);
  ir::Val self = gen(recv).v;  // address of the T value (or the pointer itself)
  ir::Val desc = descOf(base, n);
  Decl* iface = m->owner;
  int slot = 0;
  for (auto& im : iface->members) { if (&im == m) break; if (im.isMethod) slot++; }
  declareC("c7_find_vtable", ir::tPtr(), {ir::tPtr(), ir::tInt(64)});
  ir::Val vt = B->call(ir::vGlobal("c7_find_vtable"), {desc, ci(ir::tInt(64), ifaceId(iface))}, ir::tPtr());
  ir::Val fp = B->load(ir::tPtr(), B->gep(ir::tPtr(), vt, {ci(ir::tInt(64), slot)}));
  std::vector<ir::Val> args{self};
  for (size_t i = 1; i < n->kids.size(); i++) {
    TypeP pt = i - 1 < m->ty->elems.size() ? m->ty->elems[i - 1] : nullptr;
    args.push_back(gen(n->kids[i].get(), pt).v);
  }
  ir::Val r = B->call(fp, args, irTy(m->ty->ret, n), kC7CallConv);
  return mk(r, m->ty->ret);
}

// ======================== expressions ===================================================================================

namespace {
bool isLit(const TypeP& t) { return t && (t->k == TyK::IntLit || t->k == TyK::FloatLit); }
}  // namespace

TypeP Lower::commonType(const TypeP& a, const TypeP& b) {
  if (isLit(a) && !isLit(b)) return b;
  if (isLit(b) && !isLit(a)) return a;
  if (isLit(a) && isLit(b)) return (a->k == TyK::FloatLit || b->k == TyK::FloatLit) ? mkPrim("f64") : mkPrim("i32");
  if (typeEq(a, b)) return a;
  if (S.conv(a, b) == Conv::Ok) return b;
  return a;
}

ir::Val Lower::toI64(V v) {
  ir::TyP i64 = ir::tInt(64);
  int bits = v.v.ty->bits;
  if (bits == 64) return v.v;
  if (bits > 64) return B->cast("trunc", v.v, i64);
  return B->cast(isSigned(v.ty) ? "sext" : "zext", v.v, i64);
}

V Lower::coerce(V v, const TypeP& to) {
  if (!v.ty || isErr(to) || isErr(v.ty)) return v;
  if (v.v.k == ir::Val::None) return v;
  const TypeP& from = v.ty;
  if (from->k == TyK::Int && to->k == TyK::Int && from->bits != to->bits) {
    if (to->bits > from->bits) return mk(B->cast(from->sgn ? "sext" : "zext", v.v, ir::tInt(to->bits)), to);
    return mk(B->cast("trunc", v.v, ir::tInt(to->bits)), to);
  }
  if (from->k == TyK::Int && to->k == TyK::Float) return mk(B->cast(from->sgn ? "sitofp" : "uitofp", v.v, irTy(to, nullptr)), to);
  if (from->k == TyK::Float && to->k == TyK::Float && from->bits != to->bits)
    return mk(B->cast(to->bits > from->bits ? "fpext" : "fptrunc", v.v, irTy(to, nullptr)), to);
  v.ty = to;
  return v;
}

V Lower::gen(Node* n, TypeP hint) {
  TypeP target = ctOf(n);
  TypeP own = st(n);
  if (target && target->k == TyK::Slice && own->k == TyK::Array) {  // implicit T[N] -> T[]
    ir::Val p = addrOrSpill(n);
    ir::Val s = ir::vUndef(irTy(target, n));
    s = B->insert(s, p, 0);
    s = B->insert(s, ci(ir::tInt(64), own->len), 1);
    return mk(s, target);
  }
  V raw = genRaw(n, target ? target : hint);
  if (target && !isErr(target)) raw = coerce(raw, target);
  return raw;
}

ir::Val Lower::strConst(const std::string& s, size_t len) {
  ir::TyP sty = irTy(mkNamed(S.dStr), nullptr);
  ir::Val v = ir::vUndef(sty);
  v = B->insert(v, M.addString(s), 0);
  v = B->insert(v, ci(ir::tInt(64), (int64_t)len), 1);
  return v;
}

bool Lower::varAddr(const std::string& name, ir::Val& out) {
  if (LocalVar* lv = findLocal(name)) {
    out = lv->indirect ? B->load(ir::tPtr(), lv->ptr) : lv->ptr;
    return true;
  }
  if (haveSelf && selfDecl) {
    int idx = fieldIdx(selfDecl, name);
    if (idx >= 0) {
      if (selfDecl->k == Decl::Union) { out = selfPtr; return true; }
      out = B->gepField(irTy(mkNamed(selfDecl, selfDecl->tparamTypes), nullptr), selfPtr, idx);
      return true;
    }
  }
  const std::vector<Decl*>* itv = S.lookupGlobal(name);
  if (itv)
    for (Decl* d : *itv)
      if (d->k == Decl::Var) {
        out = ir::vGlobal(d->abi.empty() ? "_C7$" + dollar(d->full) : d->name);
        return true;
      }
  return false;
}

bool Lower::addressable(Node* n) {
  switch (n->kind) {
    case NK::Path: {
      if (findLocal(n->text)) return true;
      if (haveSelf && selfDecl && fieldIdx(selfDecl, n->text) >= 0) return true;
      const std::vector<Decl*>* itv = S.lookupGlobal(n->text);
      if (itv) for (Decl* d : *itv) if (d->k == Decl::Var) return true;
      return false;
    }
    case NK::Member: case NK::Arrow: {
      TypeP bt = st(n->kids[0].get());
      if (n->kind == NK::Arrow || bt->k == TyK::Ptr) return true;
      if ((bt->k == TyK::Array || bt->k == TyK::Slice) && (n->text == "len" || n->text == "ptr")) return false;
      return addressable(n->kids[0].get());
    }
    case NK::Index: return n->kids[1]->kind != NK::Range;
    case NK::Unary: return n->text == "*";
    default: return false;
  }
}

ir::Val Lower::addrOrSpill(Node* n) {
  if (addressable(n)) return addrOf(n);
  V v = gen(n);
  ir::Val slot = B->alloca_(v.v.ty);
  B->store(v.v, slot);
  return slot;
}

ir::Val Lower::addrOf(Node* n) {
  switch (n->kind) {
    case NK::Path: {
      ir::Val a;
      if (varAddr(n->text, a)) return a;
      unsupported("address of `" + n->text + "`", n);
    }
    case NK::Member: case NK::Arrow: {
      Node* base = n->kids[0].get();
      TypeP bt = st(base);
      ir::Val p;
      TypeP agg = bt;
      if (bt->k == TyK::Ptr) { p = gen(base).v; agg = bt->elem; }
      else p = addrOrSpill(base);
      if (agg->k == TyK::Tuple) return B->gepField(irTy(agg, n), p, (int)std::stoul(n->text));
      if (agg->k == TyK::Named) {
        if (agg->decl->k == Decl::Union) return p;
        int idx = fieldIdx(agg->decl, n->text);
        if (idx < 0) unsupported("member `" + n->text + "`", n);
        return B->gepField(irTy(agg, n), p, idx);
      }
      unsupported("member access on this type", n);
    }
    case NK::Index: {
      Node* base = n->kids[0].get();
      TypeP bt = st(base);
      Node* ix = n->kids[1].get();
      ir::Val idx = toI64(gen(ix, mkPrim("usize")));
      if (bt->k == TyK::Array) { boundsCheck(idx, ci(ir::tInt(64), bt->len)); return B->gep(irTy(bt, n), addrOrSpill(base), {ci(ir::tInt(64), 0), idx}); }
      if (bt->k == TyK::Slice) { V s = gen(base); boundsCheck(idx, B->extract(s.v, 1, ir::tInt(64))); return elemGep(bt->elem, B->extract(s.v, 0, ir::tPtr()), idx, n); }
      if (bt->k == TyK::Ptr) return elemGep(bt->elem, gen(base).v, idx, n);
      if (bt->k == TyK::Named && bt->decl == S.dStr) { V s = gen(base); boundsCheck(idx, B->extract(s.v, 1, ir::tInt(64))); return B->gep(ir::tInt(8), B->extract(s.v, 0, ir::tPtr()), {idx}); }
      unsupported("indexing this type", n);
    }
    case NK::Unary:
      if (n->text == "*") return gen(n->kids[0].get()).v;
      [[fallthrough]];
    default: unsupported(std::string("address of ") + nkName(n->kind), n);
  }
}

V Lower::genMember(Node* n) {
  Node* base = n->kids[0].get();
  TypeP bt = st(base);
  TypeP rt = st(n);
  if ((bt->k == TyK::Array || bt->k == TyK::Slice) && (n->text == "len" || n->text == "ptr")) {
    if (bt->k == TyK::Array) return mk(ci(ir::tInt(64), bt->len), rt);
    V s = gen(base);
    return n->text == "len" ? mk(B->extract(s.v, 1, ir::tInt(64)), rt) : mk(B->extract(s.v, 0, ir::tPtr()), rt);
  }
  if (addressable(n)) return mk(B->load(irTy(rt, n), addrOf(n)), rt);
  V agg = gen(base);
  if (agg.ty->k == TyK::Tuple) return mk(B->extract(agg.v, (int)std::stoul(n->text), irTy(rt, n)), rt);
  if (agg.ty->k == TyK::Named && agg.ty->decl->k != Decl::Union) {
    int idx = fieldIdx(agg.ty->decl, n->text);
    if (idx >= 0) return mk(B->extract(agg.v, idx, irTy(rt, n)), rt);
  }
  unsupported("member access on this expression", n);
}

V Lower::genIndex(Node* n) {
  Node* ix = n->kids[1].get();
  if (ix->kind == NK::Range) {  // slicing: a[lo..hi]
    Node* base = n->kids[0].get();
    TypeP bt = st(base);
    ir::Val ptr, len;
    if (bt->k == TyK::Array) { ptr = addrOrSpill(base); len = ci(ir::tInt(64), bt->len); }
    else if (bt->k == TyK::Slice) { V s = gen(base); ptr = B->extract(s.v, 0, ir::tPtr()); len = B->extract(s.v, 1, ir::tInt(64)); }
    else unsupported("slicing this type", n);
    ir::Val lo = ix->kids[0]->kind == NK::Empty ? ci(ir::tInt(64), 0) : toI64(gen(ix->kids[0].get(), mkPrim("usize")));
    ir::Val hi = ix->kids[1]->kind == NK::Empty ? len : toI64(gen(ix->kids[1].get(), mkPrim("usize")));
    if (ix->text == "..=") hi = B->bin("add", hi, ci(ir::tInt(64), 1));
    if (cg.boundsCheck) {
      panicIf(B->icmp("ugt", hi, len), "slice end out of bounds");
      panicIf(B->icmp("ugt", lo, hi), "slice start after end");
    }
    ir::TyP sty = ir::tStruct({ir::tPtr(), ir::tInt(64)});
    ir::Val s = ir::vUndef(sty);
    s = B->insert(s, B->gep(irTy(bt->elem, n), ptr, {lo}), 0);
    s = B->insert(s, B->bin("sub", hi, lo), 1);
    return mk(s, st(n));
  }
  TypeP rt = st(n);
  return loadVal(rt, addrOf(n), n);
}

V Lower::castValue(V v, const TypeP& to, Node* at) {
  const TypeP& from = v.ty;
  if (isErr(from) || isErr(to)) unsupported("cast of unresolved type", at);
  auto intLike = [](const TypeP& t) { return t->k == TyK::Int || t->k == TyK::Char || t->k == TyK::Bool || (t->k == TyK::Named && t->decl->k == Decl::Enum); };
  ir::TyP tt = irTy(to, at);
  if (intLike(from) && intLike(to)) {
    int fb = v.v.ty->bits, tb = tt->bits;
    if (to->k == TyK::Bool && from->k != TyK::Bool) return mk(B->icmp("ne", v.v, ci(v.v.ty, 0)), to);
    if (tb > fb) return mk(B->cast(from->k == TyK::Bool ? "zext" : (isSigned(from) ? "sext" : "zext"), v.v, tt), to);
    if (tb < fb) return mk(B->cast("trunc", v.v, tt), to);
    return mk(v.v, to);
  }
  if (intLike(from) && to->k == TyK::Float) return mk(B->cast(isSigned(from) ? "sitofp" : "uitofp", v.v, tt), to);
  if (from->k == TyK::Float && intLike(to)) {
    bool sg = isSigned(to);
    std::string name = std::string("llvm.fpto") + (sg ? "si" : "ui") + ".sat.i" + std::to_string(tt->bits) + ".f" + std::to_string(from->bits);
    M.declare(name, tt, {v.v.ty});
    return mk(B->call(ir::vGlobal(name), {v.v}, tt), to);
  }
  if (from->k == TyK::Float && to->k == TyK::Float) {
    if (from->bits == to->bits) return mk(v.v, to);
    return mk(B->cast(to->bits > from->bits ? "fpext" : "fptrunc", v.v, tt), to);
  }
  if ((from->k == TyK::Ptr || from->k == TyK::Fn || from->k == TyK::Null) && (to->k == TyK::Ptr || to->k == TyK::Fn)) return mk(v.v, to);
  if (from->k == TyK::Ptr && to->k == TyK::Int) return mk(B->cast("ptrtoint", v.v, tt), to);
  if (from->k == TyK::Int && to->k == TyK::Ptr) return mk(B->cast("inttoptr", v.v, tt), to);
  unsupported("cast from `" + ts(from) + "` to `" + ts(to) + "`", at);
}

V Lower::arith(const std::string& op, V a, V b, const TypeP& t, Node* at) {
  ir::TyP it = a.v.ty;
  bool fl = t->k == TyK::Float;
  bool sg = isSigned(t);
  std::string o;
  if (op == "+") o = fl ? "fadd" : "add";
  else if (op == "-") o = fl ? "fsub" : "sub";
  else if (op == "*") o = fl ? "fmul" : "mul";
  else if (op == "/") o = fl ? "fdiv" : (sg ? "sdiv" : "udiv");
  else if (op == "%") o = fl ? "frem" : (sg ? "srem" : "urem");
  else if (op == "&") o = "and";
  else if (op == "|") o = "or";
  else if (op == "^") o = "xor";
  else if (op == "<<" || op == ">>") o = op == "<<" ? "shl" : (sg ? "ashr" : "lshr");
  else unsupported("operator `" + op + "`", at);
  if (!fl && (op == "/" || op == "%")) panicIf(B->icmp("eq", b.v, ci(it, 0)), "division by zero");
  if (op == "<<" || op == ">>") {  // Release semantics: the shift amount wraps (spec 4.6)
    if (!ir::tyEq(b.v.ty, it)) b = mk(B->cast(b.v.ty->bits > it->bits ? "trunc" : "zext", b.v, it), t);
    b = mk(B->bin("and", b.v, ci(it, it->bits - 1)), t);
  }
  if (cg.overflowCheck && !fl && (op == "+" || op == "-" || op == "*")) return checkedArith(op, a, b, t);
  return mk(B->bin(o, a.v, b.v), t);
}

V Lower::compare(const std::string& op, V a, V b, const TypeP& t) {
  bool bl = mkPrim("bool")->k == TyK::Bool;
  (void)bl;
  TypeP bt = mkPrim("bool");
  if (t->k == TyK::Float) {
    std::string p = op == "==" ? "oeq" : op == "!=" ? "une" : op == "<" ? "olt" : op == "<=" ? "ole" : op == ">" ? "ogt" : "oge";
    return mk(B->fcmp(p, a.v, b.v), bt);
  }
  bool sg = isSigned(t);
  std::string p = op == "==" ? "eq" : op == "!=" ? "ne" : op == "<" ? (sg ? "slt" : "ult") : op == "<=" ? (sg ? "sle" : "ule") : op == ">" ? (sg ? "sgt" : "ugt") : (sg ? "sge" : "uge");
  return mk(B->icmp(p, a.v, b.v), bt);
}

V Lower::genBinary(Node* n, TypeP hint) {
  const std::string& op = n->text;
  Node *l = n->kids[0].get(), *r = n->kids[1].get();
  TypeP bt = mkPrim("bool");
  if (op == "&&" || op == "||") {
    ir::Val slot = B->alloca_(ir::tInt(1));
    V a = gen(l);
    B->store(a.v, slot);
    int rhs = B->newBlock("logic.rhs"), end = B->newBlock("logic.end");
    if (op == "&&") B->condBr(a.v, rhs, end); else B->condBr(a.v, end, rhs);
    B->setBlock(rhs);
    V b = gen(r);
    B->store(b.v, slot);
    B->br(end);
    B->setBlock(end);
    return mk(B->load(ir::tInt(1), slot), bt);
  }
  bool cmp = op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=";
  bool shift = op == "<<" || op == ">>";
  TypeP la = st(l), ra = st(r);
  if (la->k == TyK::Param || ra->k == TyK::Param) unsupported("operators on generic values (use interface methods)", n);
  if (n->decl || n->mem) {  // overloaded operator chosen by semantic analysis
    if (n->decl) {
      Decl* f = static_cast<Decl*>(n->decl);
      if (!f->tparams.empty()) unsupported("generic operator overload", n);
      declareFn(symbolOf(f), f->ty, false, cc(f), n);
      V a = gen(l, f->ty->elems[0]), b = gen(r, f->ty->elems[1]);
      return mk(B->call(ir::vGlobal(symbolOf(f)), {coerce(a, f->ty->elems[0]).v, coerce(b, f->ty->elems[1]).v}, irTy(f->ty->ret, n), cc(f)), f->ty->ret);
    }
    const Member* mm = static_cast<const Member*>(n->mem);
    ir::Val self = addrOrSpill(l);
    V b = gen(r, mm->ty->elems[0]);
    return mk(callMember(mm, mm->owner, self, true, {coerce(b, mm->ty->elems[0]).v}, irTy(mm->ty->ret, n)), mm->ty->ret);
  }
  if (la->k == TyK::Named && la->decl->k != Decl::Enum) unsupported("struct comparison without an operator overload", n);
  if (la->k == TyK::Ptr || ra->k == TyK::Ptr || la->k == TyK::Null || ra->k == TyK::Null) {
    if (cmp) {
      V a = gen(l), b = gen(r);
      return compare(op, mk(a.v, mkPrim("usize")), mk(b.v, mkPrim("usize")), mkPrim("usize"));
    }
    if (la->k == TyK::Ptr && ra->k == TyK::Ptr) {  // pointer difference
      ir::Val a = B->cast("ptrtoint", gen(l).v, ir::tInt(64)), b = B->cast("ptrtoint", gen(r).v, ir::tInt(64));
      ir::Val diff = B->bin("sub", a, b);
      uint64_t es = ir::sizeOf(irTy(la->elem, n));
      return mk(B->bin("sdiv", diff, ci(ir::tInt(64), (int64_t)es ? (int64_t)es : 1)), mkPrim("isize"));
    }
    bool pl = la->k == TyK::Ptr;
    TypeP pt = pl ? la : ra;
    ir::Val p = gen(pl ? l : r).v;
    V iv = gen(pl ? r : l, mkPrim("isize"));
    ir::Val idx = toI64(iv);
    if (op == "-") idx = B->bin("sub", ci(ir::tInt(64), 0), idx);
    return mk(elemGep(pt->elem, p, idx, n), pt);
  }
  TypeP U;
  TypeP A = ctOf(l) ? ctOf(l) : la, Bt = ctOf(r) ? ctOf(r) : ra;
  if (shift) U = isLit(la) ? (ctOf(n) ? ctOf(n) : (hint && hint->k == TyK::Int ? hint : mkPrim("i32"))) : A;
  else if (isLit(A) && isLit(Bt)) U = ctOf(n) ? ctOf(n) : ((hint && !cmp && (hint->k == TyK::Int || hint->k == TyK::Float)) ? hint : commonType(A, Bt));
  else U = commonType(A, Bt);
  V a = coerce(gen(l, U), U);
  V b = shift ? gen(r, U) : coerce(gen(r, U), U);
  if (cmp) return compare(op, a, b, U);
  return arith(op, a, b, U, n);
}

V Lower::genUnary(Node* n, TypeP hint) {
  const std::string& op = n->text;
  Node* k = n->kids[0].get();
  if (op == "&") return mk(addrOrSpill(k), st(n));
  if (op == "*") { V p = gen(k); TypeP rt = st(n); return loadVal(rt, p.v, n); }
  V x = gen(k, hint);
  if (op == "!") return mk(B->bin("xor", x.v, ci(ir::tInt(1), 1)), x.ty);
  if (op == "~") return mk(B->bin("xor", x.v, ci(x.v.ty, -1)), x.ty);
  if (op == "-") {
    if (x.ty->k == TyK::Float || x.ty->k == TyK::FloatLit) return mk(B->bin("fsub", ir::vFloat(x.v.ty, -0.0), x.v), x.ty);
    // A negated integer literal is a constant whose range sema already checked (`-128` as i8 is not an overflow).
    if (cg.overflowCheck && isSigned(x.ty) && k->kind != NK::IntLit) {
      V r = checkedArith("-", mk(ci(x.v.ty, 0), x.ty), x, x.ty, "negation");
      return r;
    }
    return mk(B->bin("sub", ci(x.v.ty, 0), x.v), x.ty);
  }
  unsupported("unary `" + op + "`", n);
}

V Lower::genStructLit(Node* n, TypeP hint) {
  (void)hint;
  TypeP t = st(n);
  if (t->k != TyK::Named) unsupported("struct literal", n);
  Decl* d = t->decl;
  ir::TyP sty = irTy(t, n);
  auto fields = fieldsOf(d);
  if (d->k == Decl::Union) {
    ir::Val slot = B->alloca_(sty);
    B->store(ir::vZero(sty), slot);
    Node* fi = n->kids[1].get();
    const Member* fm = nullptr;
    for (auto* f : fields) if (f->name == fi->text) fm = f;
    if (!fm) unsupported("union literal", n);
    V v = gen(fi->kids[0].get(), fm->ty);
    B->store(v.v, slot);
    return mk(B->load(sty, slot), t);
  }
  ir::Val agg = ir::vUndef(sty);
  for (size_t i = 1; i < n->kids.size(); i++) {
    Node* fi = n->kids[i].get();
    int idx = fi->text.empty() ? (int)(i - 1) : fieldIdx(d, fi->text);
    if (idx < 0) unsupported("field in literal", fi);
    V v = gen(fi->kids[0].get(), fields[idx]->ty);
    agg = B->insert(agg, v.v, idx);
  }
  return mk(agg, t);
}

V Lower::genAlloc(Node* n) {
  TypeP pt = st(n);
  if (pt->k != TyK::Ptr) unsupported("alloc", n);
  TypeP et = pt->elem;
  Node *count = nullptr, *args = nullptr;
  for (auto& k : n->kids) {
    if (k->kind == NK::Empty && k->text == "allocator") unsupported("alloc with custom allocator", n);
    if (k->kind == NK::Empty && k->text == "count") count = k.get();
    if (k->kind == NK::Empty && k->text == "init-args") args = k.get();
  }
  ir::TyP i64 = ir::tInt(64);
  ir::Val cnt = count ? toI64(gen(count->kids[0].get(), mkPrim("usize"))) : ci(i64, 1);
  ir::Val size = et->k == TyK::Param ? paramSize(et, n) : ci(i64, (int64_t)ir::sizeOf(irTy(et, n)));
  ir::Val p;
  if (n->mods == "zeroed") {
    declareC(heapFn("calloc").c_str(), ir::tPtr(), {i64, i64});
    p = B->call(ir::vGlobal(heapFn("calloc")), {cnt, size}, ir::tPtr());
  } else {
    declareC(heapFn("malloc").c_str(), ir::tPtr(), {i64});
    p = B->call(ir::vGlobal(heapFn("malloc")), {B->bin("mul", cnt, size)}, ir::tPtr());
  }
  const Member* init = (const Member*)n->mem;
  if (init && et->k == TyK::Named) {
    ir::Val isNull = B->icmp("eq", p, ir::vNull());
    int doInit = B->newBlock("alloc.init"), done = B->newBlock("alloc.done");
    B->condBr(isNull, done, doInit);
    B->setBlock(doInit);
    TypeP fty = init->ty;
    if (!et->decl->tparams.empty() || !init->tparams.empty()) {  // constructor of a generic class: Vec<i32>
      std::vector<std::string> tps = et->decl->tparams;
      for (auto& t : init->tparams) tps.push_back(t);
      SubMap sm = argMap(et, n);
      if (!init->tparams.empty()) unsupported("generic constructor", n);
      fty = std::make_shared<Type>(*init->ty); fty->ret = mkPrim("void");
      ir::Func* gf = declareGenericFn(symbolOfMember(init, et->decl), et->decl->name + "::init", fty, tps, true, n);
      std::vector<Node*> argNodes;
      if (args) for (auto& a : args->kids) argNodes.push_back(a.get());
      emitGenericCall(gf, tps, sm, fty, true, p, argNodes, n);
    } else {
      std::vector<ir::Val> av;
      size_t i = 0;
      if (args) for (auto& a : args->kids) av.push_back(gen(a.get(), i < fty->elems.size() ? fty->elems[i] : nullptr).v), i++;
      callMember(init, et->decl, p, true, av, ir::tVoid());
    }
    B->br(done);
    B->setBlock(done);
  }
  return mk(p, pt);
}

V Lower::genCall(Node* n, TypeP hint) {
  (void)hint;
  Node* callee = n->kids[0].get();
  TypeP rty = st(n);
  Decl* fd = static_cast<Decl*>(n->decl);
  const Member* m = static_cast<const Member*>(n->mem);
  if (m && m->isVariant) {  // tagged-union variant constructor
    Decl* owner = m->owner;
    TypeP tt = (hint && hint->k == TyK::Named && hint->decl == owner) ? hint : rty;
    TagInfo& ti = tagInfo(tt, n);
    size_t vi = 0;
    for (auto& mm : owner->members) { if (&mm == m) break; if (mm.isVariant) vi++; }
    std::vector<V> vals;
    for (size_t i = 1; i < n->kids.size(); i++) {
      TypeP pt = (i - 1 < ti.sema[vi].size()) ? ti.sema[vi][i - 1] : nullptr;
      V v = gen(n->kids[i].get(), pt);
      if (pt) v = coerce(v, pt);
      vals.push_back(v);
    }
    return makeVariant(tt, vi, vals, n);
  }
  auto argsFor = [&](const TypeP& fnTy) {
    std::vector<ir::Val> out;
    for (size_t i = 1; i < n->kids.size(); i++) {
      Node* a = n->kids[i].get();
      if (a->kind == NK::OutArg) { out.push_back(addrOf(a->kids[0].get())); continue; }
      TypeP pt = (i - 1 < fnTy->elems.size()) ? fnTy->elems[i - 1] : nullptr;
      out.push_back(gen(a, pt).v);
    }
    return out;
  };
  if (fd && fd->prelude && fd->name == "free_closure") {  // free_closure(f): release a heap environment
    if (n->kids.size() != 2) unsupported("free_closure with a custom allocator", n);
    V cl = gen(n->kids[1].get());
    declareC(heapFn("free").c_str(), ir::tVoid(), {ir::tPtr()});
    B->call(ir::vGlobal(heapFn("free")), {B->extract(cl.v, 1, ir::tPtr())}, ir::tVoid());
    return V{ir::Val(), mkPrim("void")};
  }
  if (fd) {
    if (!fd->tparams.empty()) return genGenericCall(n, fd);
    declareFn(symbolOf(fd), fd->ty, false, cc(fd), n);
    std::vector<ir::Val> av = argsFor(fd->ty);
    ir::Val r = B->call(ir::vGlobal(symbolOf(fd)), av, irTy(fd->ty->ret, n), cc(fd));
    return mk(r, fd->ty->ret);
  }
  if (m) {
    Decl* owner = m->owner;
    if (owner->k == Decl::Interface) return genBoundMethodCall(n, m);
    if (owner->prelude && !(m->node && funcParts(m->node).body)) unsupported("standard library member `" + owner->name + "::" + m->name + "` has no implementation", n);
    if (!owner->tparams.empty() || !m->tparams.empty()) return genGenericMemberCall(n, m);
    bool isCtor = m->node && m->node->kind == NK::Init;
    if (isCtor) {
      ir::TyP sty = irTy(rty, n);
      ir::Val tmp = B->alloca_(sty);
      B->store(ir::vZero(sty), tmp);
      callMember(m, owner, tmp, true, argsFor(m->ty), ir::tVoid());
      return mk(B->load(sty, tmp), rty);
    }
    ir::Val self;
    bool hasSelf = !m->isStatic;
    if (hasSelf) {
      if (callee->kind == NK::Member || callee->kind == NK::Arrow) {
        Node* recv = callee->kids[0].get();
        self = st(recv)->k == TyK::Ptr ? gen(recv).v : addrOrSpill(recv);
      } else if (haveSelf) self = selfPtr;
      else unsupported("method call without receiver", n);
    }
    std::vector<ir::Val> av = argsFor(m->ty);
    ir::Val r = callMember(m, owner, self, hasSelf, av, irTy(m->ty->ret, n));
    return mk(r, m->ty->ret);
  }
  TypeP ct = st(callee);
  if (!isErr(ct) && ct->k == TyK::Closure) {  // code(env, args...)
    V cl = gen(callee);
    ir::Val code = B->extract(cl.v, 0, ir::tPtr()), env = B->extract(cl.v, 1, ir::tPtr());
    std::vector<ir::Val> av{env};
    for (auto& a : argsFor(ct)) av.push_back(a);
    return mk(B->call(code, av, irTy(ct->ret, n), kC7CallConv), ct->ret);
  }
  if (!isErr(ct) && ct->k == TyK::Fn) {
    V f = gen(callee);
    std::vector<ir::Val> av = argsFor(ct);
    return mk(B->call(f.v, av, irTy(ct->ret, n), kC7CallConv), ct->ret);
  }
  unsupported("this kind of call (closure, variant constructor, or external module function)", n);
}

V Lower::genRaw(Node* n, TypeP hint) {
  TypeP own = st(n);
  switch (n->kind) {
    case NK::IntLit: case NK::FloatLit: {
      uint64_t v; std::string suf; bool isF;
      parseNumber(n->text, v, suf, isF);
      TypeP t = (own->k == TyK::Int || own->k == TyK::Float) ? own : ((hint && (hint->k == TyK::Int || hint->k == TyK::Float)) ? hint : (n->kind == NK::FloatLit ? mkPrim("f64") : mkPrim("i32")));
      if (t->k == TyK::Float) {
        double d;
        if (n->kind == NK::FloatLit || isF) {
          std::string c;
          for (char ch : n->text) { if (ch == '_') continue; if ((ch == 'f' || ch == 'F') && !c.empty() && c.find("0x") == std::string::npos) break; c += ch; }
          d = std::strtod(c.c_str(), nullptr);
        } else d = (double)v;
        return mk(ir::vFloat(irTy(t, n), d), t);
      }
      if (t->k != TyK::Int) unsupported("integer literal in this context", n);
      return mk(ir::vInt(irTy(t, n), v), t);
    }
    case NK::CharLit: return mk(ci(ir::tInt(32), decodeCharText(n->text)), mkPrim("Char"));
    case NK::BoolLit: return mk(ci(ir::tInt(1), n->text == "true"), mkPrim("bool"));
    case NK::NullLit: return mk(ir::vNull(), own);
    case NK::StrLit: case NK::RawStrLit: return mk(strConst(n->text, n->text.size()), mkNamed(S.dStr));
    case NK::CStrLit: {
      ir::Val v = ir::vUndef(irTy(mkNamed(S.dCStr), n));
      v = B->insert(v, M.addString(n->text), 0);
      return mk(v, mkNamed(S.dCStr));
    }
    case NK::Path: {
      ir::Val a;
      if (n->text == "self" && haveSelf) return mk(selfPtr, own);
      if (varAddr(n->text, a)) return loadVal(own, a, n);
      size_t cp = n->text.rfind("::");
      if (cp != std::string::npos) {
        Decl* e = S.findTypeDecl(n->text.substr(0, cp));
        if (e && e->k == Decl::Enum) {
          TypeP et = mkNamed(e);
          return mk(ci(irTy(et, n), enumValue(e, n->text.substr(cp + 2))), et);
        }
      }
      {  // payload-less tagged-union variant: None, Shape::Empty
        Decl* td = nullptr;
        std::string vn;
        auto bv = S.bareVariants.find(n->text);
        if (bv != S.bareVariants.end()) { td = bv->second.decl; vn = n->text; }
        else if (cp != std::string::npos) {
          Decl* c = S.findTypeDecl(n->text.substr(0, cp));
          if (c && c->k == Decl::Tagged) { td = c; vn = n->text.substr(cp + 2); }
        }
        if (td) {
          size_t vi = 0;
          bool found = false;
          for (auto& mm : td->members) { if (!mm.isVariant) continue; if (mm.name == vn) { found = true; break; } vi++; }
          if (found) {
            TypeP tt = (hint && hint->k == TyK::Named && hint->decl == td) ? hint : own;
            return makeVariant(tt, vi, {}, n);
          }
        }
      }
      const std::vector<Decl*>* itf = S.lookupGlobal(n->text);
      if (itf)
        for (Decl* d : *itf)
          if (d->k == Decl::Func) { S.declareDecl(d); declareFn(symbolOf(d), d->ty, false, cc(d), n); return mk(ir::vGlobal(symbolOf(d)), d->ty); }
      unsupported("name `" + n->text + "` (module or generic reference)", n);
    }
    case NK::Unary: return genUnary(n, hint);
    case NK::Binary: return genBinary(n, hint);
    case NK::Index: return genIndex(n);
    case NK::Member: case NK::Arrow: return genMember(n);
    case NK::Call: return genCall(n, hint);
    case NK::Alloc: return genAlloc(n);
    case NK::StructLit: return genStructLit(n, hint);
    case NK::Cast: {
      TypeP from = st(n->kids[0].get());
      V v = gen(n->kids[0].get(), from->k == TyK::IntLit ? mkPrim("i64") : from->k == TyK::FloatLit ? mkPrim("f64") : nullptr);
      return castValue(v, own, n);
    }
    case NK::Sizeof: case NK::Alignof: {
      Node* k = n->kids[0].get();
      TypeP t = k->sty ? st(k) : S.resolveType(k);
      if (t->k == TyK::Param) {
        if (n->kind == NK::Sizeof) return mk(paramSize(t, n), mkPrim("usize"));
        auto dit = genericDescs.find(t->name);
        if (dit == genericDescs.end()) unsupported("type parameter `" + t->name + "` outside its generic function", n);
        ir::TyP hdr = ir::tStruct({ir::tInt(64), ir::tInt(64), ir::tPtr(), ir::tInt(64)});
        return mk(B->load(ir::tInt(64), B->gepField(hdr, dit->second, 1)), mkPrim("usize"));
      }
      ir::TyP it = irTy(t, n);
      return mk(ci(ir::tInt(64), (int64_t)(n->kind == NK::Sizeof ? ir::sizeOf(it) : ir::alignOf(it))), mkPrim("usize"));
    }
    case NK::Ternary: {
      TypeP t = own;
      if (isLit(t)) t = ctOf(n) ? ctOf(n) : (hint && (hint->k == TyK::Int || hint->k == TyK::Float) ? hint : (t->k == TyK::FloatLit ? mkPrim("f64") : mkPrim("i32")));
      ir::Val slot = allocaFor(t, n);
      int th = B->newBlock("sel.then"), el = B->newBlock("sel.else"), end = B->newBlock("sel.end");
      B->condBr(cond(n->kids[0].get()), th, el);
      B->setBlock(th); storeVal(t, coerce(gen(n->kids[1].get(), t), t), slot, n); B->br(end);
      B->setBlock(el); storeVal(t, coerce(gen(n->kids[2].get(), t), t), slot, n); B->br(end);
      B->setBlock(end);
      return loadVal(t, slot, n);
    }
    case NK::Tuple: {
      if (own->k != TyK::Tuple) unsupported("tuple", n);
      ir::Val agg = ir::vUndef(irTy(own, n));
      for (size_t i = 0; i < n->kids.size(); i++) agg = B->insert(agg, gen(n->kids[i].get(), own->elems[i]).v, (int)i);
      return mk(agg, own);
    }
    case NK::InitList: {
      TypeP at = ctOf(n) ? ctOf(n) : own;
      if (at->k != TyK::Array) unsupported("initializer list", n);
      ir::Val agg = ir::vZero(irTy(at, n));
      for (size_t i = 0; i < n->kids.size(); i++) agg = B->insert(agg, gen(n->kids[i].get(), at->elem).v, (int)i);
      return mk(agg, at);
    }
    case NK::Lambda: return genLambda(n);
    case NK::Try: {
      V r = gen(n->kids[0].get());
      TypeP rt = r.ty;
      if (rt->k != TyK::Named || rt->decl != S.dResult || !retType || retType->k != TyK::Named || retType->decl != S.dResult) unsupported("`try` outside a Result-returning function", n);
      TagInfo& ti = tagInfo(rt, n);
      if (rt->elems.size() == 2 && retType->elems.size() == 2 && !typeEq(rt->elems[1], retType->elems[1])) unsupported("`try` with error-type conversion (Convert)", n);
      ir::Val slot = B->alloca_(ti.full);
      B->store(r.v, slot);
      ir::Val tag = B->load(ti.tagTy, B->gepField(ti.full, slot, 0));
      int errB = B->newBlock("try.err"), okB = B->newBlock("try.ok");
      B->condBr(B->icmp("eq", tag, ci(ti.tagTy, 1)), errB, okB);
      B->setBlock(errB);
      V ev = variantPayload(rt, slot, 1, 0, n);
      V rv = makeVariant(retType, 1, {ev}, n);
      emitCleanups(0, "");
      B->ret(rv.v);
      B->setBlock(okB);
      return variantPayload(rt, slot, 0, 0, n);
    }
    case NK::Range: unsupported("range values", n);
    default: unsupported(std::string("expression ") + nkName(n->kind), n);
  }
}

}  // namespace

bool lowerToIr(Sema& s, Node& unit, ir::Module& out, const CodegenOptions& cg) {
  Lower l(s, out, cg);
  return l.run(unit);
}

}  // namespace sema
}  // namespace c7
