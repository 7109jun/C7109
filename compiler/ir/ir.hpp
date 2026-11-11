// C7 IR (spec 13.1): typed, function-level, block-structured SSA with explicit memory (alloca/load/store).
// Textual form is the `.c7ir` format v0 and is a strict subset of LLVM IR syntax so the LLVM backend
// can consume it directly; the verifier below is C7's own.
#pragma once
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

namespace c7 {
namespace ir {

enum class TyKind { Void, Int, F32, F64, Ptr, Struct, Array };
struct Ty;
using TyP = std::shared_ptr<const Ty>;
struct Ty {
  TyKind k = TyKind::Void;
  int bits = 0;                // Int
  std::string name;            // named struct ("" = literal struct)
  std::vector<TyP> fields;     // Struct
  TyP elem;                    // Array
  uint64_t n = 0;              // Array length
};

TyP tVoid();
TyP tInt(int bits);
TyP tF32();
TyP tF64();
TyP tPtr();
TyP tArray(TyP elem, uint64_t n);
TyP tStruct(std::vector<TyP> fields, const std::string& name = "");
bool tyEq(const TyP& a, const TyP& b);
std::string tyStr(const TyP& t);
uint64_t sizeOf(const TyP& t);
uint64_t alignOf(const TyP& t);
inline bool isInt(const TyP& t) { return t && t->k == TyKind::Int; }
inline bool isFloat(const TyP& t) { return t && (t->k == TyKind::F32 || t->k == TyKind::F64); }
inline bool isAgg(const TyP& t) { return t && (t->k == TyKind::Struct || t->k == TyKind::Array); }

struct Val {
  enum K { None, Reg, CInt, CFloat, Null, Undef, Zero, Global, SizeOf, ConstAgg } k = None;
  TyP ty;
  int reg = -1;
  uint64_t i = 0;
  double f = 0;
  std::string name;  // Global
  TyP of;            // SizeOf
  std::vector<Val> elems;  // ConstAgg: struct/array constant elements
  bool ok() const { return k != None; }
};
Val vReg(int r, TyP t);
Val vInt(TyP t, uint64_t v);
Val vFloat(TyP t, double v);
Val vNull();
Val vUndef(TyP t);
Val vZero(TyP t);
Val vGlobal(const std::string& name);
Val vSizeOf(TyP of);
Val vConstAgg(TyP ty, std::vector<Val> elems);

enum class Op { Alloca, Load, Store, Bin, Icmp, Fcmp, Cast, Gep, Extract, Insert, Call, Br, CondBr, Ret, Unreachable, Select };

struct Instr {
  Op op = Op::Unreachable;
  int dst = -1;
  TyP ty;                    // result type (Alloca: allocated type; Gep: source element type)
  std::vector<Val> a;        // operands
  std::string s;             // opcode name / predicate / calling convention
  std::vector<int> blocks;   // branch targets
  std::vector<int> idx;      // extract/insert indices
  std::vector<TyP> sig;      // variadic call: fixed parameter types
  bool variadic = false;
};

struct Block { std::string label; std::vector<Instr> ins; };

struct Func {
  std::string name;
  TyP ret = tVoid();
  std::vector<TyP> params;
  bool variadic = false, isDecl = true, internal = false;
  std::string cc;  // "" or "fastcc"
  std::vector<Instr> allocas;
  std::vector<Block> blocks;
  std::vector<TyP> regTy;
  int newReg(TyP t) { regTy.push_back(std::move(t)); return (int)regTy.size() - 1; }
};

struct Global {
  std::string name;
  TyP ty;
  Val init;
  bool isConst = false, internal = true, external = false;
  std::string bytes;  // string constants
  bool isString = false;
};

struct Module {
  std::string triple = "x86_64-unknown-linux-gnu";
  std::vector<TyP> structs;  // named struct definitions, in dependency order
  std::vector<Global> globals;
  std::vector<std::unique_ptr<Func>> funcs;
  int strCount = 0;
  Func* find(const std::string& name) const;
  Func* declare(const std::string& name, TyP ret, std::vector<TyP> params, bool variadic = false);
  Val addString(const std::string& bytes);  // NUL-terminated constant; returns ptr value
  void addStruct(const TyP& t);
};

// Emits instructions into a function; allocas are hoisted to the entry block.
class Builder {
 public:
  Builder(Module& m, Func& f) : m(m), f(f) {}
  Module& m;
  Func& f;
  int cur = -1;
  int newBlock(const std::string& label);
  void setBlock(int b) { cur = b; }
  bool terminated() const;
  Val alloca_(TyP t);
  Val allocaDyn(Val bytes);  // runtime-sized stack slot (16-byte aligned), emitted in place
  Val load(TyP t, Val ptr);
  void store(Val v, Val ptr);
  Val bin(const std::string& op, Val a, Val b);
  Val icmp(const std::string& pred, Val a, Val b);
  Val fcmp(const std::string& pred, Val a, Val b);
  Val cast(const std::string& op, Val v, TyP to);
  Val gep(TyP srcTy, Val ptr, std::vector<Val> idx);
  Val gepField(TyP structTy, Val ptr, int field);
  Val extract(Val agg, int idx, TyP resultTy);
  Val insert(Val agg, Val v, int idx);
  Val call(Val callee, std::vector<Val> args, TyP ret, const std::string& cc = "", bool variadic = false, std::vector<TyP> sig = {});
  Val select(Val c, Val a, Val b);
  void br(int target);
  void condBr(Val c, int t, int fl);
  void ret(Val v = Val());
  void unreachable();

 private:
  Instr& emit(Op op);
};

void printModule(const Module& m, std::ostream& os, bool header);
std::vector<std::string> verify(const Module& m);

}  // namespace ir
}  // namespace c7
