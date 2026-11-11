// C7109 frontend: shared declarations (spec chapters 3, 5, 13.4, 14.2)
#pragma once
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

namespace c7 {

struct Loc {
  int file = -1;
  uint32_t line = 0, col = 0;  // 1-based
};

enum class Sev { Error, Warning };

struct Diag {
  std::string code;
  Sev sev;
  Loc loc;
  std::string en, ko;
};

struct SourceFile {
  std::string path, text;
};

class DiagEngine {
 public:
  std::vector<std::unique_ptr<SourceFile>> files;
  std::vector<Diag> diags;
  std::string lang = "en";  // "en" | "ko"
  int addFile(std::string path, std::string text);
  // Looks the code up in the message table; "{}" placeholders take args in order.
  void report(const std::string& code, Loc loc, const std::vector<std::string>& args = {});
  int errorCount() const;
  void print(std::ostream& os, bool json) const;
};

enum class TK { Eof, Ident, Keyword, Int, Float, Char, Str, RawStr, CStr, Punct };

struct Token {
  TK kind = TK::Eof;
  std::string text;    // exact spelling
  std::string sval;    // decoded string/char value, or digits without '_' for numbers
  std::string suffix;  // numeric literal suffix
  Loc loc;
  uint32_t off = 0, len = 0;
  bool bol = false, space = false;
  uint64_t ival = 0;  // integer value (low 64 bits) or char code point
};

const char* tkName(TK k);
std::vector<Token> lex(DiagEngine& d, int fileId);

// Code-generation switches (all off = Release semantics, spec 4.6).
struct CodegenOptions {
  bool debugAlloc = false;     // -fdebug-alloc: tracking allocator (leaks, double free)
  bool boundsCheck = false;    // -fbounds-check: array/slice/Str index and slice-range checks
  bool overflowCheck = false;  // --debug: integer + - * and unary - panic on overflow instead of wrapping
};

struct PPOptions {
  std::vector<std::string> includeDirs;
  std::vector<std::string> defines;  // "NAME" or "NAME=VALUE"
  std::string os = "linux", arch = "x86_64";
  bool freestanding = false, debug = false;
};
std::vector<Token> preprocess(DiagEngine& d, int fileId, const PPOptions& o);

#define C7_NK_LIST(X)                                                                          \
  X(Unit) X(Namespace) X(Import) X(Alias) X(Attribute) X(Func) X(Param) X(Var) X(Struct)       \
  X(Union) X(Enum) X(Enumerator) X(Tagged) X(Variant) X(Class) X(Interface) X(Impl)            \
  X(ExternBlock) X(Init) X(Drop) X(GenericParams) X(GenericParam) X(TypePrim) X(TypeName)      \
  X(TypePtr) X(TypeArray) X(TypeSlice) X(TypeTuple) X(TypeFn) X(TypeClosure) X(TypeConst)      \
  X(TypeNoalias) X(TypeArgs) X(Block) X(If) X(While) X(DoWhile) X(For) X(ForIn) X(Switch)      \
  X(Case) X(Default) X(Body) X(Match) X(Arm) X(Return) X(Break) X(Continue) X(Goto) X(Defer)   \
  X(Fallthrough) X(Label) X(Free) X(ExprStmt) X(Assign) X(AssignOrDecl) X(TupleDecl)          \
  X(PatWild) X(PatLit) X(PatName) X(PatVariant) X(PatTuple) X(PatRange) X(IntLit) X(FloatLit)  \
  X(CharLit) X(StrLit) X(RawStrLit) X(CStrLit) X(BoolLit) X(NullLit) X(Undefined) X(Path)      \
  X(Unary) X(Binary) X(Ternary) X(Range) X(Cast) X(Try) X(Alloc) X(Sizeof) X(Alignof)          \
  X(Call) X(Index) X(Member) X(Arrow) X(Tuple) X(InitList) X(StructLit) X(FieldInit)           \
  X(Lambda) X(Capture) X(OutArg) X(Empty)

enum class NK {
#define X(n) n,
  C7_NK_LIST(X)
#undef X
};
const char* nkName(NK k);

namespace ir { struct Module; }

struct Node;
using NodeP = std::unique_ptr<Node>;
struct Node {
  NK kind;
  std::string text, mods;
  Loc loc;
  std::vector<NodeP> kids;
  // Annotations written by semantic analysis and read by code generation (stage 3).
  std::shared_ptr<void> sty;  // type of this expression / declared variable (sema::Type)
  std::shared_ptr<void> cty;  // type after implicit conversion, when one was applied
  void* decl = nullptr;       // chosen function Decl for calls
  const void* mem = nullptr;  // chosen method/constructor Member for calls
  int flag = 0;               // AssignOrDecl: 1 when it declares a new variable
  std::vector<std::shared_ptr<void>> targs;       // Call: inferred type arguments of a generic callee (method-level), in tparam order
  std::vector<std::shared_ptr<void>> ownerTargs;  // Call/Alloc: type arguments of the generic owner type (Vec<i32> -> [i32])
};

NodeP parse(DiagEngine& d, std::vector<Token> toks);

// Stage 2: semantic analysis (spec ch.4-10). Returns true when no errors were reported.
// When `out` is non-null and analysis succeeds, the unit is also lowered to C7 IR (stage 3).
bool analyze(DiagEngine& d, Node& unit, ir::Module* out = nullptr, const CodegenOptions& cg = {});
bool analyze(DiagEngine& d, std::vector<Node*>& units, ir::Module* out = nullptr, const CodegenOptions& cg = {});  // units[0] is the user program; the rest is library code
void dump(const Node& n, std::ostream& os, int depth = 0);

}  // namespace c7
