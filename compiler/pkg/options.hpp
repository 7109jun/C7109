// Build options (stage 5, W6): the .cset7 schema and the five-level merge used by the project build.
//
// Precedence, lowest to highest (spec: "the command line overrides file settings"):
//   built-in defaults < .cset7 < selected `config` < target properties < command line
// Scalars: the highest layer that sets the option wins. Path lists ([paths], --registry): concatenated low -> high with
// duplicates dropped (first occurrence kept) - additive like -I; a layer cannot remove an entry. (⚠ Q1)
// `mode "debug"` means `--debug`: it turns on the overflow check, and `overflow_check` set explicitly wins over that. (⚠ Q3)
#pragma once
#include <optional>
#include <string>
#include <vector>

#include "../frontend/c7.hpp"
#include "buildfile.hpp"

namespace c7pkg {

struct OptionLayer {
  std::optional<std::string> mode, optimization, language, link, os, arch, abi, compilerReq;
  std::optional<bool> boundsCheck, overflowCheck, debugAlloc;
  std::vector<std::string> include, library, registry;
};

struct Options {  // fully resolved
  std::string mode = "release";
  std::string optimization = "O0";  // the built-in default matches a plain `c7c build file.c7`
  bool boundsCheck = false, debugAlloc = false;
  bool overflowCheck = false;  // resolved: explicit value if any layer set one, else (mode == "debug")
  std::string language = "en", link = "dynamic";
  std::optional<std::string> os, arch;  // unset = host; only an explicit platform produces --target
  std::string abi = "c7abi1";
  std::string compilerReq;  // empty = any
  std::vector<std::string> include, library, registry;

  // The flags a child `c7c build` needs. One source for both the child command line and applyToDriver (they must agree).
  std::vector<std::string> toFlags() const;
};

// Parses a .cset7 (TOML). Unknown keys/tables: W0701. Bad values/types: E0719. Returns false if any error was reported.
bool loadCset7(c7::DiagEngine& d, int fileId, OptionLayer& out);
OptionLayer layerFromConfig(const BuildConfig& c);
OptionLayer layerFromTarget(const BuildTarget& t);
// One command-line option (`-O2`, `--mode=debug`, `--bounds-check`, ...). Returns false if `arg` is not one of ours;
// a recognised option with a bad value sets `error`.
bool parseOptionArg(const std::string& arg, OptionLayer& layer, std::string& error);
Options mergeOptions(const std::vector<const OptionLayer*>& lowToHigh);
// Checks that need the compiler identity: [compiler] requires (E0719) and [target] abi (E0719).
bool checkOptionsCompatible(c7::DiagEngine& d, c7::Loc at, const Options& o);

std::string optionsToJson(const Options& o);

}  // namespace c7pkg
