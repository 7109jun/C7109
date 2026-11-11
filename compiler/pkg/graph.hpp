// Build graph (stage 5, W7): targets and dependencies as nodes, `depends` as edges, deterministic topological order.
// Host and target platform are kept apart even though only x86_64-linux is supported today (E0715 otherwise).
#pragma once
#include <string>
#include <vector>

#include "../frontend/c7.hpp"
#include "buildfile.hpp"

namespace c7pkg {

constexpr const char* kSupportedPlatform = "x86_64-linux";  // the host (the compiler only runs on Linux)
// Platforms a target may be built for. x86_64-windows produces a PE executable by cross-compilation (stage 7).
inline bool isBuildablePlatform(const std::string& p) { return p == "x86_64-linux" || p == "x86_64-windows"; }

struct GraphNode {
  std::string name;
  bool isTarget = true;     // false: a `dependency` (resolved by W11)
  int index = -1;           // index into BuildFile::targets / ::deps
  std::vector<int> deps;    // node indices, in `depends:` order
  std::string platform;     // targets: requested platform (default = host)
  std::string output;       // targets: project-relative output path
};

struct BuildGraph {
  std::string host;
  std::vector<GraphNode> nodes;  // targets in declaration order, then dependencies in declaration order
  std::vector<int> order;        // dependencies before dependents; ties broken by node index (declaration order)
};

// Errors: E0716 (unknown name in `depends`), E0706 (cycle, printed `a -> b -> a`), E0707 (output collisions),
// E0715 (unsupported platform). `outDir` is the project-relative build directory (default outputs live there).
bool buildGraph(c7::DiagEngine& d, const BuildFile& b, const std::string& outDir, const std::string& host, BuildGraph& g);
std::string graphToJson(const BuildGraph& g);

}  // namespace c7pkg
