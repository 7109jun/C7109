// .l7 library packages: a ZIP container with manifest.toml, checksums.sha256 and the package content.
// Layout (decision recorded in docs/stage5-decisions.md):
//   manifest.toml            format_version, [package], [compiler], [[dependency]], [[binary]]
//   checksums.sha256         `sha256sum` format: "<64 hex>  <path>", sorted, covers every other entry
//   src/**/*.c7              library sources (the compiler has no separate compilation yet)
//   lib/<os>-<arch>/...      reserved slots for prebuilt binaries, declared by [[binary]]
//   include/, licenses/, dependencies/, docs/   free-form content
#pragma once
#include "version.hpp"
#include <string>
#include <utility>
#include <vector>

#include "../frontend/c7.hpp"
#include "zip.hpp"

namespace c7pkg {


struct DepSpec { std::string name, req; };
struct BinarySlot { std::string os, arch, abi, path; };

struct Manifest {
  int formatVersion = 1;
  std::string name, version, license, description, abi = kAbiId;
  std::string compilerReq;  // requirement on the compiler version; empty = any
  std::vector<DepSpec> deps;
  std::vector<BinarySlot> binaries;
};

struct Package {
  Manifest m;
  std::vector<ZipEntry> files;  // every entry, including manifest.toml and checksums.sha256
  std::string sha256;           // SHA-256 of the .l7 file itself
  std::string label;            // path used in messages
  // (archive path, text) of every src/**/*.c7, sorted by path
  std::vector<std::pair<std::string, std::string>> sources() const;
  const ZipEntry* find(const std::string& path) const;
};

// Creates the container: manifest.toml first, content sorted by path, checksums.sha256 last. Reproducible.
bool buildPackage(c7::DiagEngine& d, const Manifest& m, const std::vector<ZipEntry>& content, std::vector<uint8_t>& out);
// Reads and fully verifies a package (ZIP structure, safe paths, checksums, manifest). Reports E0709/E0710/E0711/E0717/E0718.
bool loadPackage(c7::DiagEngine& d, const std::string& path, Package& out);
bool loadPackageBytes(c7::DiagEngine& d, const std::string& label, const std::vector<uint8_t>& bytes, Package& out);
// Safe extraction into a new directory (refuses to touch an existing one).
bool extractPackage(c7::DiagEngine& d, const Package& p, const std::string& dest);

std::string manifestToToml(const Manifest& m);
bool readFileBytes(const std::string& path, std::vector<uint8_t>& out);
bool writeFileBytes(const std::string& path, const std::vector<uint8_t>& data);
// Anchors a diagnostic that has no source position to a named pseudo-file.
c7::Loc anchorLoc(c7::DiagEngine& d, const std::string& label);

}  // namespace c7pkg
