// Dependency resolution (stage 5, W11). Sources of packages are abstracted (PackageSource); implemented: local directory
// registries (`<name>-<version>.l7` files) and path dependencies. Central / Git / enterprise registries are stubs that
// answer E0716 "not implemented" (interface only, by decision).
//
// Algorithm (guide 4.3 f): collect every requirement, pick for each package the HIGHEST version satisfying all of them,
// expand with the picked versions' own dependencies, repeat to a fixed point. No backtracking in v1 and never an automatic
// relaxation: if nothing satisfies everyone the result is E0713 naming who asked for what.
#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../frontend/c7.hpp"
#include "package.hpp"
#include "semver.hpp"

namespace c7pkg {

struct Candidate {
  Package pkg;
  Version version;
  std::string file;      // path of the .l7
  std::string sourceId;  // "registry:<dir as written>" or "path:<path as written>"
};

class PackageSource {
 public:
  virtual ~PackageSource() = default;
  virtual std::string id() const = 0;
  // All versions this source has of `name`. Hard problems (damaged package, file name != manifest) are reported and return false.
  virtual bool candidates(c7::DiagEngine& d, const std::string& name, std::vector<Candidate>& out) = 0;
};

// `spec` is a directory (relative paths are taken against `baseDir`), optionally prefixed `registry:`. Prefixes `central:`,
// `git:` and `enterprise:` give a source that reports E0716 (not implemented) when asked.
std::unique_ptr<PackageSource> makeRegistrySource(const std::string& spec, const std::string& baseDir);

struct RootDep {
  std::string name;
  std::string req;       // registry dependency: requirement text
  bool isPath = false;
  std::string file;      // path dependency: the .l7 file
  std::string pathText;  // as written (for the source id)
  c7::Loc loc;
};

struct Resolved {
  std::string name, version, sourceId, file, sha256;
  std::vector<std::string> deps;  // names of the packages it requires (from its manifest)
};

// Exact versions a lock file pins. With pins, a package named here only considers that version; if it is missing or the
// requirements reject it the result is E0714 (the lock is stale) instead of E0713. Packages not named stay free.
struct LockPins {
  std::string label;  // lock file path, for messages
  std::map<std::string, std::string> versions;
};

// `out` is ordered dependencies-first (ties alphabetical). Errors: E0713, E0716, E0706, E0709/E0710/E0718 from packages.
bool resolveDependencies(c7::DiagEngine& d, const std::vector<RootDep>& roots, const std::vector<std::string>& registries,
                         const std::string& baseDir, std::vector<Resolved>& out, const LockPins* pins = nullptr);
std::string resolvedToJson(const std::vector<Resolved>& r);

}  // namespace c7pkg
