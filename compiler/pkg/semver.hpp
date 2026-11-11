// Semantic Versioning 2.0.0 versions and version requirements (^, ~, comparison operators, wildcards, comma = AND).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace c7pkg {

struct Version {
  uint64_t major = 0, minor = 0, patch = 0;
  std::vector<std::string> pre;  // pre-release identifiers
  std::string build;             // build metadata (ignored by comparison)
  std::string str() const;
};

// Strict SemVer 2.0.0: exactly MAJOR.MINOR.PATCH, no leading zeros, optional -pre and +build.
bool parseVersion(const std::string& text, Version& out, std::string& err);
int compareVersions(const Version& a, const Version& b);  // build metadata does not take part

struct Requirement {
  enum Op { EQ, LT, LE, GT, GE };
  struct Cmp { Op op; Version v; };
  bool any = false;          // "*"
  std::vector<Cmp> cmps;     // all must hold
  std::string text;          // as written, for messages
};

// A bare version means caret (^1.2.3), as in Cargo. Partial versions (1, 1.2) and wildcards (1.*, 1.2.x) are accepted.
bool parseRequirement(const std::string& text, Requirement& out, std::string& err);
// Pre-release versions only match a requirement that itself names a pre-release of the same MAJOR.MINOR.PATCH.
bool matches(const Requirement& r, const Version& v);

}  // namespace c7pkg
