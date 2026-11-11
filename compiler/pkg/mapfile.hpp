// `.mp7` map file (stage 5, W13). Human-readable UTF-8/LF text written next to an executable target's output.
// Format: docs/specs/file-formats/mp7.md (draft ⚠ Q8). Addresses are hex (0x...), sizes are bytes.
// Policy: the map is written only after the target succeeded, via temp file + rename; stale maps are deleted before a build.
#pragma once
#include <string>
#include <vector>

namespace c7pkg {

struct MapFile {
  std::string project, target, platform, host, config, mode, optimization, compiler;
  struct Input { std::string path, sha256; unsigned long long bytes = 0; };
  struct Lib { std::string name, version, sha256; };
  std::vector<Input> inputs;  // listed in the given order
  std::vector<Lib> libs;
  std::string outputPath;     // as shown in [Outputs]
  std::string outputFile;     // file read to measure the output and its symbols
};

// `_C7$a$b$$T1$T2` -> `a::b(T1, T2)`; anything that is not a C7 symbol is returned unchanged.
std::string demangleC7Symbol(const std::string& name);

// Builds the map text. Symbols/sections come from `nm` and `readelf`; when a tool is missing the section holds
// `; unavailable: <why>` instead (never a silent omission).
std::string buildMapText(const MapFile& m);

// Writes `text` to `path` atomically (temp + rename). Returns false on failure.
bool writeMapFile(const std::string& path, const std::string& text);

}  // namespace c7pkg
