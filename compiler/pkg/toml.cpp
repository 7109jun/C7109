#include "toml.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace c7;

namespace c7pkg {

namespace {
enum : int { F_IMPLICIT = 1, F_DEFINED = 2, F_DOTTED = 4, F_INLINE = 8, F_AOT = 16, F_STATIC = 32 };

bool validUtf8(const std::string& s, size_t& badAt) {
  size_t i = 0;
  while (i < s.size()) {
    unsigned char c = s[i];
    size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    if (n == 0 || i + n > s.size()) { badAt = i; return false; }
    uint32_t cp = n == 1 ? c : c & (0xFF >> (n + 1));
    for (size_t k = 1; k < n; k++) {
      if ((s[i + k] & 0xC0) != 0x80) { badAt = i; return false; }
      cp = (cp << 6) | (s[i + k] & 0x3F);
    }
    static const uint32_t minCp[5] = {0, 0, 0x80, 0x800, 0x10000};
    if ((n > 1 && cp < minCp[n]) || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) { badAt = i; return false; }
    i += n;
  }
  return true;
}

void appendUtf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) out += (char)cp;
  else if (cp < 0x800) { out += (char)(0xC0 | cp >> 6); out += (char)(0x80 | (cp & 0x3F)); }
  else if (cp < 0x10000) { out += (char)(0xE0 | cp >> 12); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
  else { out += (char)(0xF0 | cp >> 18); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
}

bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isHex(char c) { return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
bool isBare(char c) { return isDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '-'; }

// digits with single underscores strictly between digits
bool cleanDigits(const std::string& s, bool (*ok)(char), std::string& out) {
  out.clear();
  if (s.empty()) return false;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '_') {
      if (i == 0 || i + 1 == s.size() || !ok(s[i - 1]) || !ok(s[i + 1])) return false;
      continue;
    }
    if (!ok(s[i])) return false;
    out += s[i];
  }
  return true;
}
bool isBin(char c) { return c == '0' || c == '1'; }
bool isOct(char c) { return c >= '0' && c <= '7'; }

bool validDatetime(const std::string& t) {
  auto num = [&](size_t at, size_t n) { if (at + n > t.size()) return false; for (size_t k = 0; k < n; k++) if (!isDigit(t[at + k])) return false; return true; };
  auto timePart = [&](size_t at) -> long {  // returns end index or -1
    if (!(num(at, 2) && at + 2 < t.size() && t[at + 2] == ':' && num(at + 3, 2) && at + 5 < t.size() && t[at + 5] == ':' && num(at + 6, 2))) return -1;
    size_t e = at + 8;
    if (e < t.size() && t[e] == '.') { e++; size_t st = e; while (e < t.size() && isDigit(t[e])) e++; if (e == st) return -1; }
    return (long)e;
  };
  size_t i = 0;
  bool date = false;
  if (num(0, 4) && t.size() > 4 && t[4] == '-' && num(5, 2) && t.size() > 7 && t[7] == '-' && num(8, 2)) { date = true; i = 10; }
  if (date) {
    int mo = atoi(t.substr(5, 2).c_str()), da = atoi(t.substr(8, 2).c_str());
    if (mo < 1 || mo > 12 || da < 1 || da > 31) return false;
    if (i == t.size()) return true;
    if (t[i] != 'T' && t[i] != 't' && t[i] != ' ') return false;
    i++;
  }
  long e = timePart(i);
  if (e < 0) return false;
  int hh = atoi(t.substr(i, 2).c_str()), mm = atoi(t.substr(i + 3, 2).c_str()), ss = atoi(t.substr(i + 6, 2).c_str());
  if (hh > 23 || mm > 59 || ss > 60) return false;
  size_t k = (size_t)e;
  if (k == t.size()) return true;
  if (!date) return false;
  if (t[k] == 'Z' || t[k] == 'z') return k + 1 == t.size();
  if ((t[k] == '+' || t[k] == '-') && num(k + 1, 2) && k + 3 < t.size() && t[k + 3] == ':' && num(k + 4, 2)) return k + 6 == t.size();
  return false;
}

struct Parser {
  DiagEngine& d;
  int fid;
  const std::string& s;
  size_t pos = 0;
  uint32_t line = 1, col = 1;
  bool failed = false;

  Parser(DiagEngine& d, int fid, const std::string& s) : d(d), fid(fid), s(s) {}

  Loc loc() const { return Loc{fid, line, col}; }
  bool eof() const { return pos >= s.size(); }
  char peek(size_t k = 0) const { return pos + k < s.size() ? s[pos + k] : '\0'; }
  void adv() { if (s[pos] == '\n') { line++; col = 1; } else col++; pos++; }
  bool fail(const std::string& msg) {
    if (!failed) d.report("E0701", loc(), {msg});
    failed = true;
    return false;
  }
  bool atNewline() const { return peek() == '\n' || (peek() == '\r' && peek(1) == '\n'); }
  void newline() { if (peek() == '\r') adv(); adv(); }
  void skipWs() { while (peek() == ' ' || peek() == '\t') adv(); }
  bool skipComment() {
    if (peek() != '#') return true;
    while (!eof() && !atNewline()) {
      unsigned char c = peek();
      if ((c < 0x20 && c != '\t') || c == 0x7F) return fail("control character in comment");
      adv();
    }
    return true;
  }
  bool skipWsNlComments() {
    for (;;) {
      skipWs();
      if (peek() == '#') { if (!skipComment()) return false; continue; }
      if (atNewline()) { newline(); continue; }
      return true;
    }
  }

  // ---- strings
  bool parseEscape(std::string& out) {
    adv();  // backslash
    char c = peek();
    switch (c) {
      case 'b': out += '\b'; adv(); return true;
      case 't': out += '\t'; adv(); return true;
      case 'n': out += '\n'; adv(); return true;
      case 'f': out += '\f'; adv(); return true;
      case 'r': out += '\r'; adv(); return true;
      case '"': out += '"'; adv(); return true;
      case '\\': out += '\\'; adv(); return true;
      case 'u': case 'U': {
        int n = c == 'u' ? 4 : 8;
        adv();
        uint32_t cp = 0;
        for (int k = 0; k < n; k++) {
          if (!isHex(peek())) return fail("invalid unicode escape");
          char h = peek();
          cp = cp * 16 + (uint32_t)(isDigit(h) ? h - '0' : (h | 32) - 'a' + 10);
          adv();
        }
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return fail("unicode escape is not a scalar value");
        appendUtf8(out, cp);
        return true;
      }
      default: return fail("invalid escape sequence");
    }
  }
  bool parseString(std::string& out) {
    char q = peek();
    bool multi = peek(1) == q && peek(2) == q;
    out.clear();
    if (multi) { adv(); adv(); adv(); if (atNewline()) newline(); }
    else adv();
    for (;;) {
      if (eof()) return fail("unterminated string");
      char c = peek();
      if (c == q) {
        if (!multi) { adv(); return true; }
        int n = 0;
        while (peek(n) == q) n++;
        if (n >= 3) {
          if (n > 5) return fail("too many quotes at the end of a multi-line string");
          for (int k = 0; k < n - 3; k++) out += q;
          for (int k = 0; k < n; k++) adv();
          return true;
        }
        for (int k = 0; k < n; k++) { out += q; adv(); }
        continue;
      }
      if (atNewline()) {
        if (!multi) return fail("newline in a single-line string");
        out += '\n';
        newline();
        continue;
      }
      unsigned char u = (unsigned char)c;
      if ((u < 0x20 && u != '\t') || u == 0x7F) return fail("control character in string");
      if (c == '\\' && q == '"') {
        if (multi) {  // line-ending backslash trims following whitespace and newlines
          size_t k = 1;
          while (peek(k) == ' ' || peek(k) == '\t') k++;
          if (peek(k) == '\n' || (peek(k) == '\r' && peek(k + 1) == '\n')) {
            adv();
            while (peek() == ' ' || peek() == '\t' || atNewline()) { if (atNewline()) newline(); else adv(); }
            continue;
          }
        }
        if (!parseEscape(out)) return false;
        continue;
      }
      out += c;
      adv();
    }
  }

  // ---- keys
  bool parseKey(std::vector<std::string>& parts) {
    parts.clear();
    for (;;) {
      skipWs();
      std::string part;
      char c = peek();
      if (c == '"' || c == '\'') {
        if (peek(1) == c && peek(2) == c) return fail("multi-line string used as a key");
        if (!parseString(part)) return false;
      } else {
        while (isBare(peek())) { part += peek(); adv(); }
        if (part.empty()) return fail("expected a key");
      }
      parts.push_back(part);
      skipWs();
      if (peek() == '.') { adv(); continue; }
      return true;
    }
  }

  // ---- values
  bool parseNumberOrBool(TomlValue& v) {
    std::string tok;
    while (!eof()) {
      char c = peek();
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ',' || c == ']' || c == '}' || c == '#') break;
      tok += c;
      adv();
    }
    if (tok.empty()) return fail("expected a value");
    if (tok == "true" || tok == "false") { v.kind = TomlValue::Boolean; v.b = tok == "true"; return true; }
    if (tok == "inf" || tok == "+inf") { v.kind = TomlValue::Float; v.f = INFINITY; return true; }
    if (tok == "-inf") { v.kind = TomlValue::Float; v.f = -INFINITY; return true; }
    if (tok == "nan" || tok == "+nan" || tok == "-nan") { v.kind = TomlValue::Float; v.f = NAN; return true; }
    bool dateLike = (tok.size() >= 10 && isDigit(tok[0]) && tok[4] == '-') || (tok.size() >= 8 && tok[2] == ':' && isDigit(tok[0]));
    if (dateLike) {
      if (tok.size() == 10 && peek() == ' ' && isDigit(peek(1))) {  // "1979-05-27 07:32:00"
        tok += ' '; adv();
        while (!eof() && peek() != ' ' && peek() != '\t' && peek() != '\n' && peek() != '\r' && peek() != ',' && peek() != ']' && peek() != '}' && peek() != '#') { tok += peek(); adv(); }
      }
      if (!validDatetime(tok)) return fail("invalid date or time");
      v.kind = TomlValue::Datetime; v.str = tok;
      return true;
    }
    std::string body = tok;
    bool neg = false;
    if (!body.empty() && (body[0] == '+' || body[0] == '-')) { neg = body[0] == '-'; body = body.substr(1); if (body.size() > 1 && body[0] == '0' && (body[1] == 'x' || body[1] == 'o' || body[1] == 'b')) return fail("sign on a prefixed integer"); }
    if (body.size() > 1 && body[0] == '0' && (body[1] == 'x' || body[1] == 'o' || body[1] == 'b')) {
      std::string digits;
      bool (*ok)(char) = body[1] == 'x' ? isHex : body[1] == 'o' ? isOct : isBin;
      if (!cleanDigits(body.substr(2), ok, digits)) return fail("invalid integer");
      int base = body[1] == 'x' ? 16 : body[1] == 'o' ? 8 : 2;
      uint64_t acc = 0;
      for (char c : digits) {
        uint64_t dgt = isDigit(c) ? (uint64_t)(c - '0') : (uint64_t)((c | 32) - 'a' + 10);
        if (acc > (UINT64_MAX - dgt) / base) return fail("integer out of range");
        acc = acc * base + dgt;
      }
      if (acc > (uint64_t)INT64_MAX) return fail("integer out of range");
      v.kind = TomlValue::Integer; v.i = (int64_t)acc;
      return true;
    }
    bool isFloat = body.find('.') != std::string::npos || body.find('e') != std::string::npos || body.find('E') != std::string::npos;
    if (!isFloat) {
      std::string digits;
      if (!cleanDigits(body, isDigit, digits)) return fail("invalid integer");
      if (digits.size() > 1 && digits[0] == '0') return fail("leading zeros are not allowed");
      uint64_t acc = 0;
      for (char c : digits) {
        uint64_t dgt = (uint64_t)(c - '0');
        if (acc > (UINT64_MAX - dgt) / 10) return fail("integer out of range");
        acc = acc * 10 + dgt;
      }
      if (neg ? acc > (uint64_t)INT64_MAX + 1 : acc > (uint64_t)INT64_MAX) return fail("integer out of range");
      v.kind = TomlValue::Integer; v.i = neg ? (int64_t)(0 - acc) : (int64_t)acc;
      return true;
    }
    size_t k = 0;
    std::string intPart, frac, exp;
    while (k < body.size() && (isDigit(body[k]) || body[k] == '_')) intPart += body[k++];
    if (k < body.size() && body[k] == '.') { k++; while (k < body.size() && (isDigit(body[k]) || body[k] == '_')) frac += body[k++]; if (frac.empty()) return fail("invalid float"); }
    if (k < body.size() && (body[k] == 'e' || body[k] == 'E')) {
      k++;
      if (k < body.size() && (body[k] == '+' || body[k] == '-')) exp += body[k++];
      std::string ed;
      while (k < body.size() && (isDigit(body[k]) || body[k] == '_')) ed += body[k++];
      std::string cleaned;
      if (!cleanDigits(ed, isDigit, cleaned)) return fail("invalid float exponent");
      exp += cleaned;
    }
    if (k != body.size()) return fail("invalid number");
    std::string ci, cf;
    if (!cleanDigits(intPart, isDigit, ci)) return fail("invalid float");
    if (ci.size() > 1 && ci[0] == '0') return fail("leading zeros are not allowed");
    if (!frac.empty() && !cleanDigits(frac, isDigit, cf)) return fail("invalid float");
    std::string text = (neg ? "-" : "") + ci + (cf.empty() ? "" : "." + cf) + (exp.empty() ? "" : "e" + exp);
    v.kind = TomlValue::Float; v.f = strtod(text.c_str(), nullptr);
    return true;
  }

  TomlValue* findKey(TomlValue& t, const std::string& k) {
    for (auto& kv : t.tbl) if (kv.first == k) return &kv.second;
    return nullptr;
  }

  // Inserts value at a (possibly dotted) key below `t`. `inInline` marks every table created on the way as immutable.
  bool insertKey(TomlValue& t, const std::vector<std::string>& parts, TomlValue v, bool inInline) {
    TomlValue* cur = &t;
    for (size_t i = 0; i + 1 < parts.size(); i++) {
      TomlValue* child = findKey(*cur, parts[i]);
      if (!child) {
        TomlValue nt; nt.loc = loc(); nt.flags = F_DOTTED | (inInline ? F_INLINE : 0);
        cur->tbl.emplace_back(parts[i], std::move(nt));
        child = &cur->tbl.back().second;
      } else if (child->kind != TomlValue::Table || !(child->flags & F_DOTTED) || (child->flags & F_INLINE && !inInline)) {
        return fail("key `" + parts[i] + "` is already defined and cannot be extended");
      }
      cur = child;
    }
    if (findKey(*cur, parts.back())) return fail("duplicate key `" + parts.back() + "`");
    cur->tbl.emplace_back(parts.back(), std::move(v));
    return true;
  }

  // Why a hard limit: parseValue/parseArray/parseInline recurse, and the TomlValue destructor and the
  // JSON/serialize walkers recurse too. Untrusted manifests must not be able to overflow the stack (B1).
  int depth_ = 0;

  bool parseValue(TomlValue& v) {
    v = TomlValue();
    v.loc = loc();
    char c = peek();
    if (c == '"' || c == '\'') { v.kind = TomlValue::String; return parseString(v.str); }
    if (c == '[' || c == '{') {
      if (depth_ >= kTomlMaxNesting) return fail("nesting too deep (limit " + std::to_string(kTomlMaxNesting) + ")");
      depth_++;
      bool ok = c == '[' ? parseArray(v) : parseInline(v);
      depth_--;
      return ok;
    }
    return parseNumberOrBool(v);
  }

  bool parseArray(TomlValue& v) {
    v.kind = TomlValue::Array;
    v.flags = F_STATIC;
    adv();
    for (;;) {
      if (!skipWsNlComments()) return false;
      if (eof()) return fail("unterminated array");
      if (peek() == ']') { adv(); return true; }
      TomlValue e;
      if (!parseValue(e)) return false;
      v.arr.push_back(std::move(e));
      if (!skipWsNlComments()) return false;
      if (peek() == ',') { adv(); continue; }
      if (peek() == ']') { adv(); return true; }
      return fail("expected `,` or `]` in array");
    }
  }

  bool parseInline(TomlValue& v) {
    v.kind = TomlValue::Table;
    v.flags = F_INLINE;
    adv();
    skipWs();
    if (peek() == '}') { adv(); return true; }
    for (;;) {
      std::vector<std::string> parts;
      if (!parseKey(parts)) return false;
      skipWs();
      if (peek() != '=') return fail("expected `=` after key");
      adv(); skipWs();
      TomlValue e;
      if (!parseValue(e)) return false;
      if (!insertKey(v, parts, std::move(e), true)) return false;
      skipWs();
      if (peek() == ',') { adv(); skipWs(); if (peek() == '}') return fail("trailing comma in inline table"); continue; }
      if (peek() == '}') { adv(); return true; }
      return fail("expected `,` or `}` in inline table");
    }
  }

  // ---- table headers
  TomlValue* openTable(TomlValue& root, const std::vector<std::string>& parts, bool aot) {
    TomlValue* cur = &root;
    for (size_t i = 0; i < parts.size(); i++) {
      bool last = i + 1 == parts.size();
      TomlValue* child = findKey(*cur, parts[i]);
      if (!child) {
        TomlValue nt; nt.loc = loc();
        if (last && aot) {
          nt.kind = TomlValue::Array; nt.flags = F_AOT;
          TomlValue el; el.loc = loc(); el.flags = F_DEFINED;
          nt.arr.push_back(std::move(el));
          cur->tbl.emplace_back(parts[i], std::move(nt));
          return &cur->tbl.back().second.arr.back();
        }
        nt.flags = last ? F_DEFINED : F_IMPLICIT;
        cur->tbl.emplace_back(parts[i], std::move(nt));
        cur = &cur->tbl.back().second;
        continue;
      }
      if (child->kind == TomlValue::Array && (child->flags & F_AOT)) {
        if (last) {
          if (!aot) { fail("`" + parts[i] + "` is an array of tables"); return nullptr; }
          TomlValue el; el.loc = loc(); el.flags = F_DEFINED;
          child->arr.push_back(std::move(el));
          return &child->arr.back();
        }
        cur = &child->arr.back();
        continue;
      }
      if (child->kind != TomlValue::Table || (child->flags & (F_INLINE | F_STATIC))) { fail("key `" + parts[i] + "` is already defined and cannot be reopened"); return nullptr; }
      if (last) {
        if (aot) { fail("`" + parts[i] + "` is already defined as a table"); return nullptr; }
        if (child->flags & (F_DEFINED | F_DOTTED)) { fail("table `" + parts[i] + "` is defined twice"); return nullptr; }
        child->flags = (child->flags & ~F_IMPLICIT) | F_DEFINED;
      }
      cur = child;
    }
    return cur;
  }

  bool run(TomlValue& root) {
    root = TomlValue();
    root.flags = F_DEFINED;
    TomlValue* cur = &root;
    for (;;) {
      if (!skipWsNlComments()) return false;
      if (eof()) return true;
      if (peek() == '[') {
        bool aot = peek(1) == '[';
        adv(); if (aot) adv();
        std::vector<std::string> parts;
        if (!parseKey(parts)) return false;
        skipWs();
        if (peek() != ']') return fail("expected `]`");
        adv();
        if (aot) { if (peek() != ']') return fail("expected `]]`"); adv(); }
        cur = openTable(root, parts, aot);
        if (!cur) return false;
      } else {
        std::vector<std::string> parts;
        if (!parseKey(parts)) return false;
        skipWs();
        if (peek() != '=') return fail("expected `=` after key");
        adv(); skipWs();
        TomlValue v;
        if (!parseValue(v)) return false;
        if (!insertKey(*cur, parts, std::move(v), false)) return false;
      }
      skipWs();
      if (peek() == '#') { if (!skipComment()) return false; }
      if (eof()) return true;
      if (!atNewline()) return fail("expected a newline after the value");
      newline();
    }
  }
};

void jsonString(std::string& out, const std::string& s) {
  out += '"';
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); out += b; }
        else out += (char)c;
    }
  }
  out += '"';
}

std::string floatText(double f) {
  if (std::isnan(f)) return "nan";
  if (std::isinf(f)) return f < 0 ? "-inf" : "inf";
  char buf[40];
  for (int prec = 15; prec <= 17; prec++) {
    snprintf(buf, sizeof buf, "%.*g", prec, f);
    if (strtod(buf, nullptr) == f) break;
  }
  std::string t = buf;
  if (t.find_first_of(".en") == std::string::npos) t += ".0";
  return t;
}

void json(std::string& out, const TomlValue& v) {
  switch (v.kind) {
    case TomlValue::String: jsonString(out, v.str); break;
    case TomlValue::Integer: out += std::to_string(v.i); break;
    case TomlValue::Float:
      if (std::isnan(v.f)) out += "NaN";
      else if (std::isinf(v.f)) out += v.f < 0 ? "-Infinity" : "Infinity";
      else out += floatText(v.f);
      break;
    case TomlValue::Boolean: out += v.b ? "true" : "false"; break;
    case TomlValue::Datetime: out += "{\"$datetime\":"; jsonString(out, v.str); out += "}"; break;
    case TomlValue::Array:
      out += '[';
      for (size_t i = 0; i < v.arr.size(); i++) { if (i) out += ','; json(out, v.arr[i]); }
      out += ']';
      break;
    case TomlValue::Table:
      out += '{';
      for (size_t i = 0; i < v.tbl.size(); i++) { if (i) out += ','; jsonString(out, v.tbl[i].first); out += ':'; json(out, v.tbl[i].second); }
      out += '}';
      break;
  }
}

// ---- serializer
std::string quoted(const std::string& s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    if (c == '"') o += "\\\"";
    else if (c == '\\') o += "\\\\";
    else if (c == '\n') o += "\\n";
    else if (c == '\t') o += "\\t";
    else if (c == '\r') o += "\\r";
    else if (c < 0x20 || c == 0x7F) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
    else o += (char)c;
  }
  return o + "\"";
}

std::string keyText(const std::string& k) {
  bool bare = !k.empty();
  for (char c : k) if (!isBare(c)) bare = false;
  return bare ? k : quoted(k);
}

// An array is written as [[tables]] only when every element is a table; anything else (including mixed arrays) is inline.
bool allTables(const TomlValue& v) {
  if (v.kind != TomlValue::Array || v.arr.empty()) return false;
  for (auto& e : v.arr) if (e.kind != TomlValue::Table) return false;
  return true;
}
bool inlineable(const TomlValue& v) { return v.kind != TomlValue::Table && !allTables(v); }

void scalar(std::string& out, const TomlValue& v) {
  switch (v.kind) {
    case TomlValue::String: out += quoted(v.str); break;
    case TomlValue::Integer: out += std::to_string(v.i); break;
    case TomlValue::Float: out += floatText(v.f); break;
    case TomlValue::Boolean: out += v.b ? "true" : "false"; break;
    case TomlValue::Datetime: out += v.str; break;
    case TomlValue::Array:
      out += '[';
      for (size_t i = 0; i < v.arr.size(); i++) { if (i) out += ", "; scalar(out, v.arr[i]); }
      out += ']';
      break;
    case TomlValue::Table:  // inline table (only reached for tables inside inline arrays)
      out += '{';
      for (size_t i = 0; i < v.tbl.size(); i++) { out += i ? ", " : " "; out += keyText(v.tbl[i].first) + " = "; scalar(out, v.tbl[i].second); }
      out += v.tbl.empty() ? "}" : " }";
      break;
  }
}

void emitTable(std::string& out, const TomlValue& t, const std::string& path) {
  for (auto& kv : t.tbl) if (inlineable(kv.second)) { out += keyText(kv.first) + " = "; scalar(out, kv.second); out += "\n"; }
  for (auto& kv : t.tbl) {
    std::string sub = path.empty() ? keyText(kv.first) : path + "." + keyText(kv.first);
    if (kv.second.kind == TomlValue::Table) {
      out += "\n[" + sub + "]\n";
      emitTable(out, kv.second, sub);
    } else if (allTables(kv.second)) {
      for (auto& el : kv.second.arr) { out += "\n[[" + sub + "]]\n"; emitTable(out, el, sub); }
    }
  }
}
}  // namespace

const TomlValue* TomlValue::get(const std::string& key) const {
  for (auto& kv : tbl) if (kv.first == key) return &kv.second;
  return nullptr;
}
TomlValue* TomlValue::get(const std::string& key) {
  for (auto& kv : tbl) if (kv.first == key) return &kv.second;
  return nullptr;
}
TomlValue& TomlValue::set(const std::string& key, TomlValue v) {
  if (TomlValue* e = get(key)) { *e = std::move(v); return *e; }
  tbl.emplace_back(key, std::move(v));
  return tbl.back().second;
}

bool isValidUtf8(const std::string& s, size_t& badAt) { return validUtf8(s, badAt); }

bool parseToml(DiagEngine& d, int fileId, TomlValue& out) {
  const std::string& text = d.files[fileId]->text;
  size_t bad = 0;
  if (!validUtf8(text, bad)) { d.report("E0701", Loc{fileId, 1, 1}, {"invalid UTF-8 in file"}); return false; }
  Parser p(d, fileId, text);
  return p.run(out);
}

std::string tomlToJson(const TomlValue& v) { std::string o; json(o, v); return o; }

std::string tomlSerialize(const TomlValue& root) {
  std::string out;
  emitTable(out, root, "");
  if (!out.empty() && out[0] == '\n') out.erase(0, 1);
  return out;
}

}  // namespace c7pkg
