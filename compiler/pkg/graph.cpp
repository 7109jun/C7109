#include "graph.hpp"

#include <functional>
#include <map>
#include <set>

using namespace c7;

namespace c7pkg {

bool buildGraph(DiagEngine& d, const BuildFile& b, const std::string& outDir, const std::string& host, BuildGraph& g) {
  g = BuildGraph();
  g.host = host;
  bool ok = true;
  std::map<std::string, int> byName;
  for (size_t i = 0; i < b.targets.size(); i++) {
    GraphNode n;
    n.name = b.targets[i].name; n.index = (int)i;
    byName[n.name] = (int)g.nodes.size();
    g.nodes.push_back(n);
  }
  for (size_t i = 0; i < b.deps.size(); i++) {
    GraphNode n;
    n.name = b.deps[i].name; n.isTarget = false; n.index = (int)i;
    byName[n.name] = (int)g.nodes.size();
    g.nodes.push_back(n);
  }
  std::string base = outDir;
  while (base.size() > 1 && base.back() == '/') base.pop_back();
  std::string ver = b.version.empty() ? "0.0.0" : b.version;
  for (size_t i = 0; i < b.targets.size(); i++) {
    const BuildTarget& t = b.targets[i];
    GraphNode& n = g.nodes[i];
    n.platform = t.platform ? *t.platform : host;
    if (t.output) n.output = *t.output;
    else {
      std::string file = t.kind == BuildTarget::Executable ? t.name : t.name + "-" + ver + ".l7";
      n.output = base == "." || base.empty() ? file : base + "/" + file;
    }
    if (n.platform != kSupportedPlatform) { d.report("E0715", t.loc, {n.platform, host}); ok = false; }
    for (size_t k = 0; k < t.depends.size(); k++) {
      auto f = byName.find(t.depends[k]);
      if (f == byName.end()) {
        d.report("E0716", t.dependsLocs[k], {t.depends[k], "it is neither a target nor a `dependency` of this project"});
        ok = false;
        continue;
      }
      n.deps.push_back(f->second);
    }
  }
  // output collisions: between targets, and with a source file (declaration order decides who is "first")
  std::map<std::string, std::string> owner;
  for (size_t i = 0; i < b.targets.size(); i++) {
    for (const std::string& s : b.targets[i].sources) owner.emplace(s, "source of `" + b.targets[i].name + "`");
  }
  std::map<std::string, std::string> outOwner;
  for (size_t i = 0; i < b.targets.size(); i++) {
    const std::string& o = g.nodes[i].output;
    auto f = outOwner.find(o);
    if (f != outOwner.end()) { d.report("E0707", b.targets[i].loc, {o, f->second, b.targets[i].name}); ok = false; continue; }
    outOwner[o] = b.targets[i].name;
    auto s = owner.find(o);
    if (s != owner.end()) { d.report("E0707", b.targets[i].loc, {o, s->second, b.targets[i].name}); ok = false; }
  }
  // cycles: depth-first from each node in declaration order; the first back edge is reported
  size_t N = g.nodes.size();
  std::vector<int> color(N, 0), stack;
  bool cyc = false;
  std::function<void(int)> dfs = [&](int u) {
    if (cyc) return;
    color[u] = 1; stack.push_back(u);
    for (int v : g.nodes[u].deps) {
      if (cyc) break;
      if (color[v] == 1) {
        std::string path;
        size_t k = 0;
        while (stack[k] != v) k++;
        for (; k < stack.size(); k++) path += g.nodes[stack[k]].name + " -> ";
        path += g.nodes[v].name;
        d.report("E0706", b.targets[u].loc, {path});
        cyc = true;
      } else if (color[v] == 0) dfs(v);
    }
    stack.pop_back(); color[u] = 2;
  };
  for (size_t i = 0; i < N && !cyc; i++) if (color[i] == 0) dfs((int)i);
  if (cyc) ok = false;
  if (!ok) return false;
  // Kahn with the lowest ready index first: the order is a pure function of the declaration order
  std::vector<int> pending(N, 0);
  std::vector<std::vector<int>> users(N);
  for (size_t u = 0; u < N; u++) for (int v : g.nodes[u].deps) { pending[u]++; users[v].push_back((int)u); }
  std::set<int> ready;
  for (size_t u = 0; u < N; u++) if (!pending[u]) ready.insert((int)u);
  while (!ready.empty()) {
    int u = *ready.begin(); ready.erase(ready.begin());
    g.order.push_back(u);
    for (int w : users[u]) if (--pending[w] == 0) ready.insert(w);
  }
  return true;
}

std::string graphToJson(const BuildGraph& g) {
  auto q = [](const std::string& s) { return "\"" + s + "\""; };  // names/paths here contain no characters that need escaping (validated upstream)
  std::string o = "{\"host\":" + q(g.host) + ",\"order\":[";
  for (size_t i = 0; i < g.order.size(); i++) o += (i ? "," : "") + q(g.nodes[g.order[i]].name);
  o += "],\"nodes\":[";
  for (size_t i = 0; i < g.nodes.size(); i++) {
    const GraphNode& n = g.nodes[i];
    o += (i ? "," : "") + std::string("{\"name\":") + q(n.name) + ",\"kind\":" + q(n.isTarget ? "target" : "dependency") + ",\"deps\":[";
    for (size_t k = 0; k < n.deps.size(); k++) o += (k ? "," : "") + q(g.nodes[n.deps[k]].name);
    o += "]";
    if (n.isTarget) o += ",\"platform\":" + q(n.platform) + ",\"output\":" + q(n.output);
    o += "}";
  }
  return o + "]}";
}

}  // namespace c7pkg
