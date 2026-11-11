// Cmake7109.cr7: the lock file (stage 5, W12). Only `c7c lock` writes it; a build reads it and NEVER updates it.
// A lock pins the exact version, source and SHA-256 of every external package (guide 4.3 e). (⚠ Q6, Q7)
#pragma once
#include <map>
#include <string>
#include <vector>

#include "../frontend/c7.hpp"
#include "resolver.hpp"

namespace c7pkg {

struct LockDep {
  std::string name, version, source, sha256;
};

struct LockFile {
  std::string compilerName, compilerVersion, abi, os, arch, config;  // informational: what produced the lock
  std::vector<LockDep> deps;
};

// Reports E0701 (TOML), E0714 (wrong format / missing or ill-typed fields / duplicates), W0701 (unknown keys).
bool loadLock(c7::DiagEngine& d, int fileId, LockFile& out);
// Deterministic text for a resolution result (dependencies first, as resolved).
std::string lockToToml(const LockFile& l);
LockFile makeLock(const std::vector<Resolved>& r, const std::string& config);

}  // namespace c7pkg
