// TOML 1.0 parser and serializer (used by .cset7, .cr7 and the .l7 manifest). Datetimes are validated and kept as text.
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "../frontend/c7.hpp"

namespace c7pkg {

// Maximum nesting of arrays / inline tables accepted by the parser (single constant, see B1).
constexpr int kTomlMaxNesting = 128;

struct TomlValue {
  enum Kind { Table, String, Integer, Float, Boolean, Datetime, Array } kind = Table;
  std::string str;  // String / Datetime text
  int64_t i = 0;
  double f = 0;
  bool b = false;
  std::vector<TomlValue> arr;
  std::vector<std::pair<std::string, TomlValue>> tbl;  // insertion order
  c7::Loc loc;
  int flags = 0;  // parser bookkeeping (immutability rules)

  const TomlValue* get(const std::string& key) const;
  TomlValue* get(const std::string& key);
  TomlValue& set(const std::string& key, TomlValue v);  // insert or replace
  static TomlValue str_(const std::string& s) { TomlValue v; v.kind = String; v.str = s; return v; }
  static TomlValue int_(int64_t n) { TomlValue v; v.kind = Integer; v.i = n; return v; }
  static TomlValue bool_(bool x) { TomlValue v; v.kind = Boolean; v.b = x; return v; }
  static TomlValue array_() { TomlValue v; v.kind = Array; return v; }
  static TomlValue table_() { return TomlValue(); }
};

bool isValidUtf8(const std::string& s, size_t& badAt);  // strict: no overlongs, surrogates or > U+10FFFF

// Reports E0701 (with line:col) and returns false on the first error.
bool parseToml(c7::DiagEngine& d, int fileId, TomlValue& out);
std::string tomlToJson(const TomlValue& v);       // for tests: compared against Python's tomllib
std::string tomlSerialize(const TomlValue& root);  // deterministic; scalars first, then [tables] and [[arrays of tables]]

}  // namespace c7pkg
