// Semantic analysis internals (spec ch.4-10). Not part of the public API.
#pragma once
#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <sstream>

#include "c7.hpp"

namespace c7 {
namespace sema {

enum class TyK { Error, Void, Bool, Int, Float, Char, Ptr, Slice, Array, Tuple, Named, Fn, Closure, Null, IntLit, FloatLit, Param };

struct Decl;
struct Type;
using TypeP = std::shared_ptr<Type>;

struct Type {
  TyK k = TyK::Error;
  std::string name;  // primitive / parameter name
  int bits = 0;
  bool sgn = false;
  TypeP elem;
  bool constElem = false;
  int64_t len = -1;
  std::vector<TypeP> elems;  // tuple elems, fn/closure params, generic args of Named
  std::vector<std::string> pmods;
  TypeP ret;
  Decl* decl = nullptr;
  std::vector<TypeP> bounds;  // Param bounds
  uint64_t lit = 0;
  bool neg = false;
};

struct Member {
  std::string name;
  TypeP ty;  // field type, or Fn type for methods
  bool pub = true, isMethod = false, isStatic = false, mutSelf = false;
  bool isVariant = false, isEnumerator = false;
  std::vector<TypeP> payload;
  std::vector<std::string> payloadNames;
  std::vector<std::string> tparams;  // method-level generics
  std::vector<std::vector<TypeP>> tbounds;
  Decl* owner = nullptr;
  Node* node = nullptr;
};

struct Decl {
  enum K { Struct, Union, Enum, Tagged, Class, Interface, Alias, Func, Var } k = Struct;
  std::string name, full, ns;
  Node* node = nullptr;
  std::vector<std::string> tparams;
  std::vector<TypeP> tparamTypes;
  std::vector<Member> members;
  TypeP ty;  // alias target / var type / function type / enum base
  std::vector<Decl*> extends;
  bool hasDrop = false, prelude = false, noreturn = false, external = false, declared = false;
  std::string abi;
};

// ---- type constructors & utilities ---------------------------------------------------------
TypeP mkErr();
TypeP mkPrim(const std::string& name);  // i32, f64, bool, Char, void, ...
TypeP mkPtr(TypeP elem, bool isConst);
TypeP mkSlice(TypeP elem, bool isConst);
TypeP mkArray(TypeP elem, int64_t len);
TypeP mkTuple(std::vector<TypeP> e);
TypeP mkNamed(Decl* d, std::vector<TypeP> args = {});
TypeP mkIntLit(uint64_t v, bool neg);
TypeP mkFloatLit();
TypeP mkNull();
bool isErr(const TypeP& t);
bool isInt(const TypeP& t);
bool isNum(const TypeP& t);
std::string ts(const TypeP& t);
bool typeEq(const TypeP& a, const TypeP& b);
using SubMap = std::map<std::string, TypeP>;
TypeP subst(const TypeP& t, const SubMap& m);
void unify(const TypeP& pat, const TypeP& act, SubMap& m, const std::set<std::string>& vars);

enum class Conv { Ok, NeedCast, ConstDiscard, Mismatch, OutOfRange };
bool litFits(const Type& lit, const Type& to);
bool castOk(const TypeP& from, const TypeP& to);

}  // namespace sema
}  // namespace c7
