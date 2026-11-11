#pragma once
#include "sema_core.hpp"

namespace c7 {
namespace sema {

struct Local {
  TypeP ty;
  bool mut = false, isOut = false;
  std::string name;
};

struct ExprInfo {
  TypeP ty;
  bool lv = false, wr = false;
  bool constSelf = false;  // lvalue reached through a `const` self
  bool localName = false;  // a plain local variable (used for move-out of returns)
  std::string place;       // name used in diagnostics
};

struct FnCtx {
  TypeP ret;
  bool hasSelf = false, selfMut = false, inInit = false;
  TypeP selfType;
  Decl* selfDecl = nullptr;
  int loops = 0, switches = 0, defers = 0;
  std::set<std::string> labels;
  size_t scopeBase = 0;
  std::string name;
};

struct FnCand {
  TypeP fn;
  std::vector<std::string> tparams;
  std::vector<std::vector<TypeP>> bounds;
  Decl* decl = nullptr;
  const Member* mem = nullptr;
  SubMap base;
};

struct Arg {
  TypeP ty;
  bool lv = false, wr = false, isOut = false;
  Node* node = nullptr;
};

struct FuncParts {
  std::vector<Node*> attrs, params;
  Node *ret = nullptr, *gp = nullptr, *body = nullptr;
};
FuncParts funcParts(Node* f);
bool isTypeNode(NK k);
bool hasMod(const std::string& mods, const char* m);
void parseNumber(const std::string& text, uint64_t& val, std::string& suffix, bool& isFloat);

bool lowerToIr(class Sema& s, Node& unit, ir::Module& out, const CodegenOptions& cg = {});

class Sema {
 public:
  explicit Sema(DiagEngine& dg) : d(dg) {
    extRoots = {"hw", "cpu", "simd", "intrin", "c"};
  }
  bool run(Node& user) { std::vector<Node*> u{&user}; return run(u); }
  bool run(std::vector<Node*>& units);

  // ---- state ---------------------------------------------------------------------------------
  DiagEngine& d;
  std::vector<std::unique_ptr<Decl>> pool;
  std::map<std::string, std::vector<Decl*>> globals;
  std::map<std::string, Decl*> primImpl;
  std::set<std::pair<std::string, Decl*>> impls;
  struct PendingImpl { Node* node; std::string ns; };
  std::vector<PendingImpl> pendingImpls;
  std::vector<Decl*> allDecls;
  std::set<std::string> extRoots, extNames;
  std::string curNs;  // namespace of the declaration currently being declared / checked / lowered
  std::map<std::string, std::vector<Decl*>> aliases;  // names introduced by `import`
  std::vector<Node*> pendingImports;
  const std::vector<Decl*>* lookupGlobal(const std::string& name);
  void processImports();
  struct Bare { Decl* decl; size_t idx; };
  std::map<std::string, Bare> bareVariants;
  Decl *dStr = nullptr, *dCStr = nullptr, *dResult = nullptr, *dOption = nullptr, *dRange = nullptr,
       *dConvert = nullptr;
  std::vector<NodeP> keep;
  std::vector<std::map<std::string, TypeP>> tpEnv;
  TypeP selfType;
  std::vector<std::map<std::string, Local>> scopes;
  std::vector<FnCtx> ctxs;
  bool inPrelude = false;
  struct ImplCheck { TypeP target; Decl* iface; Node* node; std::set<std::string> provided; };
  std::vector<ImplCheck> implChecks;

  void rep(const char* code, Node* at, std::vector<std::string> args = {}) {
    d.report(code, at ? at->loc : Loc{}, args);
  }

  // ---- declaration phase (sema_decl.cpp) -------------------------------------------------------
  void loadPrelude();
  void collect(Node& unit);
  void collectItem(Node* n, const std::string& ns, const std::string& abi);
  Decl* newDecl(Decl::K k, const std::string& name, const std::string& ns, Node* n);
  void declareAll();
  void declareDecl(Decl* dd);
  void declareImpl(const PendingImpl& pi);
  void finishImplChecks();
  Member makeMethod(Node* f, Decl* owner, bool defaultPub);
  TypeP resolveType(Node* n);
  TypeP fnTypeOf(Node* f, std::vector<std::string>* tp, std::vector<std::vector<TypeP>>* bounds);
  Decl* findTypeDecl(const std::string& name);
  std::vector<std::pair<const Member*, SubMap>> findMembers(const TypeP& t, const std::string& name);
  bool implements(const TypeP& t, Decl* iface);
  // Bounds on generic type parameters (`HashMap<K : Hashable, V>`) are checked once every impl is known.
  struct PendingBound { Decl* decl; std::vector<TypeP> args; Node* at; };
  bool implsReady = false;
  std::vector<PendingBound> pendingBounds;
  void checkTypeArgBounds(Decl* dd, const std::vector<TypeP>& args, Node* at);
  bool isDropType(const TypeP& t, std::set<Decl*>& seen);
  bool isDrop(const TypeP& t) { std::set<Decl*> s; return isDropType(t, s); }
  int64_t constEval(Node* n);
  std::string implKey(const TypeP& t);

  // ---- expression phase (sema_expr.cpp) --------------------------------------------------------
  ExprInfo expr(Node* n, TypeP expected = nullptr);
  ExprInfo exprImpl(Node* n, TypeP expected);
  Conv conv(const TypeP& from, const TypeP& to);
  bool expectConv(const TypeP& from, const TypeP& to, Node* at);
  TypeP defaultType(const TypeP& t, Node* at);
  void consume(const ExprInfo& e, Node* at, bool moveOut = false);
  ExprInfo exprConv(Node* n, const TypeP& to, bool moveOut = false);
  ExprInfo checkPath(Node* n, TypeP expected);
  ExprInfo checkCall(Node* n, TypeP expected);
  ExprInfo checkMember(Node* n);
  ExprInfo checkBinary(Node* n, TypeP expected);
  ExprInfo checkUnary(Node* n, TypeP expected);
  ExprInfo checkIndex(Node* n);
  ExprInfo checkStructLit(Node* n, TypeP expected);
  ExprInfo checkLambda(Node* n);
  ExprInfo checkAlloc(Node* n);
  ExprInfo checkTry(Node* n);
  std::vector<Arg> evalArgs(Node* call, size_t from, const std::vector<TypeP>* hints);
  TypeP resolveCall(std::vector<FnCand>& cands, std::vector<Arg>& args, Node* at, const std::string& name);
  bool lookupLocal(const std::string& name, Local*& out);
  bool lookupSelfMember(const std::string& name, ExprInfo& out);
  void checkAccess(const Member& m, Decl* owner, Node* at);
  bool inOwner(Decl* owner);
  TypeP strType() { return mkNamed(dStr); }
  FnCand candFromDecl(Decl* fd);
  FnCand candFromMember(const Member* m, const SubMap& base);
  void bindLocal(Node* at, const std::string& name, TypeP ty, bool mut, bool warnShadow = true);

  // ---- statement phase (sema_stmt.cpp) ----------------------------------------------------------
  void stmt(Node* n);
  void block(Node* n, bool newScope = true);
  void checkCond(Node* c);
  void localVar(Node* n);
  void assignStmt(Node* n);
  void forIn(Node* n);
  void checkMatch(Node* n);
  void checkSwitch(Node* n);
  void bindPattern(Node* pat, const TypeP& scrut, bool& catchAll, std::set<std::string>& covered, bool top);
  bool irrefutable(Node* pat, const TypeP& t);
  void checkDeferBody(Node* n, int loopDepth, bool& bad);
  bool terminates(Node* n);
  void collectLabels(Node* n, std::set<std::string>& out);
  void checkGotoScopes(Node* body);
  void checkFunc(Decl* fd);
  void checkBodyOf(Node* f, Decl* owner, const TypeP& self, bool selfMut, bool isInit, const TypeP& fnTy, const std::string& name);
  bool containsBreak(Node* n, int depth);
  void pushScope() { scopes.emplace_back(); }
  void popScope() { scopes.pop_back(); }
};

}  // namespace sema
}  // namespace c7
