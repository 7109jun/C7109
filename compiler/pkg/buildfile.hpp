// Cmake7109.txt: the declarative, indentation-based build file (stage 5, W5). Grammar: docs/Cmake7109-grammar.md.
// Pure declaration: no variables, conditions, loops or user commands (those are undecided, see guide Q11).
// External input is hostile: line length, line count and nesting are bounded, and every path is checked.
#pragma once
#include <optional>
#include <string>
#include <vector>

#include "../frontend/c7.hpp"

namespace c7pkg {

constexpr size_t kBuildMaxLineBytes = 16 * 1024;
constexpr size_t kBuildMaxLines = 100000;
constexpr size_t kBuildMaxIndentLevels = 8;

struct BuildConfig {
  std::string name;
  c7::Loc loc;
  std::optional<std::string> mode, optimization;  // mode: debug|release, optimization: O0..O3
  std::optional<bool> boundsCheck, debugAlloc, overflowCheck;
};

struct BuildDependency {
  std::string name, req, path;  // exactly one of req / path is set
  bool isPath = false;
  c7::Loc loc;
};

struct BuildTarget {
  enum Kind { Executable, Library } kind = Executable;
  std::string name;
  c7::Loc loc, nameLoc;
  std::vector<std::string> sources, includeDirs, defines, depends;  // sources/includeDirs are normalized, project-relative
  std::vector<c7::Loc> sourceLocs, dependsLocs;
  std::optional<std::string> output, platform, link, std;  // link: static|dynamic, std: none|default
  // Build options that override the selected `config` for this target (same keys as `config`; precedence: see options.hpp).
  std::optional<std::string> mode, optimization;
  std::optional<bool> boundsCheck, debugAlloc, overflowCheck;
};

struct BuildFile {
  bool hasProject = false;
  std::string projectName, version, license, description;
  std::vector<std::string> registries;
  std::vector<BuildDependency> deps;
  std::vector<BuildConfig> configs;
  std::string defaultConfig;
  std::vector<BuildTarget> targets;
};

// Reports E0702/E0703/E0704/E0705/E0708/E0717/E0719 with line:column (columns count bytes). Returns false if any error
// was reported. Syntax errors stop the parse; semantic errors are collected. When `baseDir` is non-empty, every listed
// source must exist as a regular file below it (E0704).
bool parseBuildFile(c7::DiagEngine& d, int fileId, const std::string& baseDir, BuildFile& out);

// Deterministic dump of the resolved model (only properties that were set are emitted); used by tests.
std::string buildFileToJson(const BuildFile& b);

// Lexical path normalization shared by the build-file code: "a/./b//c/" -> "a/b/c", "." for the empty path.
// Returns false for absolute paths, drive letters, backslashes, control characters, or `..` leaving the root.
bool normalizeProjectPath(const std::string& in, std::string& out, std::string& why);

}  // namespace c7pkg
