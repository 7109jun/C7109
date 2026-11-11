#include <cctype>
#include <set>

#include "c7.hpp"

namespace c7 {
namespace {

const std::set<std::string> kKeywords = {
    "mut", "const", "noalias", "volatile", "thread_local", "inline", "extern", "struct", "union",
    "enum", "tagged", "interface", "impl", "class", "init", "drop", "pub", "priv", "import",
    "namespace", "if", "else", "while", "do", "for", "in", "switch", "case", "default", "match",
    "break", "continue", "goto", "return", "defer", "try", "as", "alloc", "free", "sizeof",
    "alignof", "typeof", "true", "false", "null",
    // decided in spec appendix D
    "out", "static", "heap", "type", "fallthrough", "zeroed", "undefined", "fn", "closure",
    "operator",
    // primitive types and aliases (spec 4.1, 4.2)
    "i8", "i16", "i32", "i64", "i128", "u8", "u16", "u32", "u64", "u128", "isize", "usize", "f32",
    "f64", "bool", "Char", "int", "long", "void"};

const std::set<std::string> kIntSuffix = {"i8", "i16", "i32", "i64", "i128", "u8",
                                          "u16", "u32", "u64", "u128", "isize", "usize"};

// Note: '>>' and '>>=' are intentionally NOT lexed; the parser composes them from adjacent '>'
// tokens so that nested generics (Vec<Vec<i32>>) close correctly.
const char* kPunct3[] = {"<<=", "..=", "...", nullptr};
const char* kPunct2[] = {"::", "->", "=>", "==", "!=", "<=", ">=", "&&", "||", "<<", "+=", "-=",
                         "*=", "/=", "%=", "&=", "|=", "^=", "..", nullptr};
const std::string kPunct1 = "+-*/%&|^~!<>=.,;:?()[]{}@#";

void putUtf8(std::string& o, uint32_t c) {
  if (c < 0x80) o += (char)c;
  else if (c < 0x800) { o += (char)(0xC0 | (c >> 6)); o += (char)(0x80 | (c & 0x3F)); }
  else if (c < 0x10000) {
    o += (char)(0xE0 | (c >> 12)); o += (char)(0x80 | ((c >> 6) & 0x3F)); o += (char)(0x80 | (c & 0x3F));
  } else {
    o += (char)(0xF0 | (c >> 18)); o += (char)(0x80 | ((c >> 12) & 0x3F));
    o += (char)(0x80 | ((c >> 6) & 0x3F)); o += (char)(0x80 | (c & 0x3F));
  }
}

// Decodes one code point at s[i]; returns its byte length (0 if malformed).
int decodeUtf8(const std::string& s, size_t i, uint32_t& cp) {
  unsigned char c = s[i];
  int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
  if (!len || i + len > s.size()) return 0;
  cp = len == 1 ? c : c & (0xFF >> (len + 1));
  for (int k = 1; k < len; k++) {
    unsigned char x = s[i + k];
    if ((x >> 6) != 2) return 0;
    cp = (cp << 6) | (x & 0x3F);
  }
  static const uint32_t minv[] = {0, 0, 0x80, 0x800, 0x10000};
  if (cp < minv[len] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
  return len;
}

int hexv(char c) { return isdigit((unsigned char)c) ? c - '0' : (tolower(c) - 'a' + 10); }

}  // namespace

const char* tkName(TK k) {
  switch (k) {
    case TK::Eof: return "Eof"; case TK::Ident: return "Ident"; case TK::Keyword: return "Keyword";
    case TK::Int: return "Int"; case TK::Float: return "Float"; case TK::Char: return "Char";
    case TK::Str: return "Str"; case TK::RawStr: return "RawStr"; case TK::CStr: return "CStr";
    case TK::Punct: return "Punct";
  }
  return "?";
}

std::vector<Token> lex(DiagEngine& d, int fid) {
  const std::string& s = d.files[fid]->text;
  std::vector<Token> out;
  size_t i = 0, n = s.size(), ls = 0;
  uint32_t line = 1;
  bool bol = true, sp = false;
  auto L = [&](size_t p) { return Loc{fid, line, (uint32_t)(p - ls + 1)}; };
  auto newline = [&](size_t at) { line++; ls = at + 1; };

  for (size_t k = 0; k < n;) {  // spec 3.1: sources must be valid UTF-8
    uint32_t cp;
    int l = decodeUtf8(s, k, cp);
    if (!l) { d.report("E0006", Loc{fid, 1, 1}); break; }
    k += l;
  }

  auto push = [&](TK k, size_t a, size_t b) -> Token& {
    Token t;
    t.kind = k; t.text = s.substr(a, b - a); t.loc = L(a); t.off = (uint32_t)a;
    t.len = (uint32_t)(b - a); t.bol = bol; t.space = sp;
    out.push_back(std::move(t));
    bol = sp = false;
    return out.back();
  };

  // Reads an escape at s[j]=='\\'; stores the code point and returns the next index.
  auto esc = [&](size_t j, uint32_t& cp, bool strLit) -> size_t {
    cp = 0;
    if (j + 1 >= n) return j + 1;
    char e = s[j + 1];
    size_t k = j + 2;
    auto bad = [&](const std::string& what) { d.report("E0004", L(j), {what}); return k; };
    switch (e) {
      case 'n': cp = '\n'; return k;  case 'r': cp = '\r'; return k;
      case 't': cp = '\t'; return k;  case '\\': cp = '\\'; return k;
      case '\'': cp = '\''; return k; case '"': cp = '"'; return k;
      case '0': cp = 0; return k;
      case 'x':
        if (k + 1 < n && isxdigit((unsigned char)s[k]) && isxdigit((unsigned char)s[k + 1])) {
          cp = hexv(s[k]) * 16 + hexv(s[k + 1]);
          if (cp > 0x7F) { k += 2; return bad(s.substr(j, 4)); }  // strings must stay UTF-8
          return k + 2;
        }
        return bad("\\x");
      case 'u': {
        if (k < n && s[k] == '{') {
          size_t m = k + 1; uint32_t v = 0; int cnt = 0;
          while (m < n && isxdigit((unsigned char)s[m]) && cnt < 8) { v = v * 16 + hexv(s[m]); m++; cnt++; }
          if (m < n && s[m] == '}' && cnt >= 1 && cnt <= 6 && v <= 0x10FFFF && !(v >= 0xD800 && v <= 0xDFFF)) {
            cp = v; return m + 1;
          }
        }
        return bad("\\u");
      }
      default: (void)strLit; return bad(std::string("\\") + e);
    }
  };

  auto lexString = [&](size_t start, size_t q, bool raw, TK kind) {
    size_t j = q + 1;
    std::string val;
    bool ok = false;
    while (j < n) {
      char ch = s[j];
      if (ch == '"') { ok = true; j++; break; }
      if (ch == '\n' || ch == '\r') break;
      if (ch == '\\' && !raw) {
        uint32_t cp; j = esc(j, cp, true); putUtf8(val, cp); continue;
      }
      val += ch; j++;
    }
    if (!ok) d.report("E0002", L(start));
    Token& t = push(kind, start, j);
    t.sval = val;
    i = j;
  };

  while (i < n) {
    char c = s[i];
    if (c == '\n') { newline(i); i++; bol = sp = true; continue; }
    if (c == '\r') {
      if (i + 1 < n && s[i + 1] == '\n') { i++; continue; }
      newline(i); i++; bol = sp = true; continue;
    }
    if (c == ' ' || c == '\t' || c == '\f' || c == '\v') { i++; sp = true; continue; }
    if (c == '\\' && i + 1 < n && (s[i + 1] == '\n' || s[i + 1] == '\r')) {  // line splice
      size_t j = i + 1;
      if (s[j] == '\r' && j + 1 < n && s[j + 1] == '\n') j++;
      newline(j); i = j + 1; sp = true; continue;
    }
    if (c == '/' && i + 1 < n && s[i + 1] == '/') {
      while (i < n && s[i] != '\n' && s[i] != '\r') i++;
      sp = true; continue;
    }
    if (c == '/' && i + 1 < n && s[i + 1] == '*') {  // no nesting (spec 3.3)
      Loc st = L(i);
      i += 2;
      bool closed = false;
      while (i < n) {
        if (s[i] == '*' && i + 1 < n && s[i + 1] == '/') { i += 2; closed = true; break; }
        if (s[i] == '\n') newline(i);
        i++;
      }
      if (!closed) d.report("E0001", st);
      sp = true; continue;
    }

    if (isalpha((unsigned char)c) || c == '_') {
      if ((c == 'r' || c == 'c') && i + 1 < n && s[i + 1] == '"') {
        lexString(i, i + 1, c == 'r', c == 'r' ? TK::RawStr : TK::CStr);
        continue;
      }
      size_t j = i;
      while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '_')) j++;
      push(kKeywords.count(s.substr(i, j - i)) ? TK::Keyword : TK::Ident, i, j);
      i = j; continue;
    }

    if (c == '"') { lexString(i, i, false, TK::Str); continue; }

    if (c == '\'') {
      size_t j = i + 1;
      uint32_t cp = 0;
      bool bad = false;
      if (j < n && s[j] == '\'') { d.report("E0007", L(i)); j++; bad = true; }
      else if (j < n && s[j] == '\\') j = esc(j, cp, false);
      else if (j < n) {
        int l = decodeUtf8(s, j, cp);
        j += l ? l : 1;
      }
      if (!bad) {
        if (j < n && s[j] == '\'') j++;
        else d.report("E0002", L(i));
      }
      Token& t = push(TK::Char, i, j);
      t.ival = cp;
      putUtf8(t.sval, cp);
      i = j; continue;
    }

    if (isdigit((unsigned char)c)) {
      size_t j = i;
      int base = 10;
      if (c == '0' && j + 1 < n) {
        char x = (char)tolower(s[j + 1]);
        if (x == 'x') base = 16; else if (x == 'b') base = 2; else if (x == 'o') base = 8;
        if (base != 10) j += 2;
      }
      uint64_t val = 0;
      bool isFloat = false, bad = false;
      std::string digits, why;
      size_t ds = j;
      if (base != 10) {
        while (j < n && ((base == 16 ? isxdigit((unsigned char)s[j]) : isdigit((unsigned char)s[j])) || s[j] == '_')) {
          if (s[j] != '_') {
            int dv = hexv(s[j]);
            if (dv >= base) { bad = true; why = std::string("digit `") + s[j] + "` out of range for base"; }
            val = val * base + dv; digits += s[j];
          }
          j++;
        }
        if (j == ds || digits.empty()) { bad = true; why = "missing digits after prefix"; }
      } else {
        while (j < n && (isdigit((unsigned char)s[j]) || s[j] == '_')) {
          if (s[j] != '_') { val = val * 10 + (s[j] - '0'); digits += s[j]; }
          j++;
        }
        if (j + 1 < n && s[j] == '.' && isdigit((unsigned char)s[j + 1])) {
          isFloat = true; digits += '.'; j++;
          while (j < n && (isdigit((unsigned char)s[j]) || s[j] == '_')) { if (s[j] != '_') digits += s[j]; j++; }
        }
        if (j < n && (s[j] == 'e' || s[j] == 'E')) {
          size_t m = j + 1;
          if (m < n && (s[m] == '+' || s[m] == '-')) m++;
          if (m < n && isdigit((unsigned char)s[m])) {
            isFloat = true; digits += 'e';
            if (s[j + 1] == '+' || s[j + 1] == '-') digits += s[j + 1];
            j = m;
            while (j < n && (isdigit((unsigned char)s[j]) || s[j] == '_')) { if (s[j] != '_') digits += s[j]; j++; }
          }
        }
        if (!isFloat && digits.size() > 1 && digits[0] == '0') { bad = true; why = "octal literals use the `0o` prefix"; }
      }
      std::string suf;
      if (j < n && (isalpha((unsigned char)s[j]) || s[j] == '_')) {
        size_t m = j;
        while (m < n && (isalnum((unsigned char)s[m]) || s[m] == '_')) m++;
        suf = s.substr(j, m - j);
        j = m;
        bool okSuf = isFloat ? (suf == "f32" || suf == "f64") : (kIntSuffix.count(suf) > 0 || suf == "f32" || suf == "f64");
        if (!okSuf) { bad = true; why = "invalid suffix `" + suf + "`"; }
        if (!isFloat && (suf == "f32" || suf == "f64") && base == 10) isFloat = true;
      }
      if (bad) d.report("E0005", L(i), {why});
      Token& t = push(isFloat ? TK::Float : TK::Int, i, j);
      t.ival = val; t.sval = digits; t.suffix = suf;
      i = j; continue;
    }

    bool matched = false;
    for (const char** p = kPunct3; *p && !matched; p++)
      if (s.compare(i, 3, *p) == 0) { push(TK::Punct, i, i + 3); i += 3; matched = true; }
    for (const char** p = kPunct2; *p && !matched; p++)
      if (s.compare(i, 2, *p) == 0) { push(TK::Punct, i, i + 2); i += 2; matched = true; }
    if (!matched && kPunct1.find(c) != std::string::npos) { push(TK::Punct, i, i + 1); i++; matched = true; }
    if (!matched) {
      d.report("E0003", L(i), {std::string(1, c)});
      i++; sp = true;
    }
  }
  Token e;
  e.kind = TK::Eof; e.loc = L(n); e.off = (uint32_t)n; e.bol = true;
  out.push_back(e);
  return out;
}

}  // namespace c7
