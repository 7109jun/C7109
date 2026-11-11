#include "tools.hpp"

#include <fstream>
#include <iostream>
#include <sstream>

#include <filesystem>

#include "buildfile.hpp"
#include "graph.hpp"
#include "options.hpp"
#include "package.hpp"
#include "resolver.hpp"
#include "semver.hpp"
#include "sha256.hpp"
#include "toml.hpp"
#include "zip.hpp"

using namespace c7;

namespace c7pkg {

static bool readFile(const std::string& path, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::stringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return true;
}

int runTool(int argc, char** argv) {
  std::string sub = argc > 2 ? argv[2] : "";
  std::string file = argc > 3 ? argv[3] : "";
  if (sub == "sha256") {
    std::string hex;
    if (!sha256File(file, hex)) { std::cerr << "cannot read " << file << "\n"; return 2; }
    std::cout << hex << "\n";
    return 0;
  }
  if (sub == "toml-json" || sub == "toml-roundtrip") {
    std::string text;
    if (!readFile(file, text)) { std::cerr << "cannot read " << file << "\n"; return 2; }
    DiagEngine d;
    int fid = d.addFile(file, text);
    TomlValue v;
    if (!parseToml(d, fid, v)) { d.print(std::cerr, false); return 1; }
    if (sub == "toml-roundtrip") {
      DiagEngine d2;
      int f2 = d2.addFile("<serialized>", tomlSerialize(v));
      TomlValue v2;
      if (!parseToml(d2, f2, v2)) { d2.print(std::cerr, false); std::cerr << "serialized text did not parse:\n" << tomlSerialize(v); return 3; }
      v = v2;
    }
    std::cout << tomlToJson(v) << "\n";
    return 0;
  }
  if (sub == "buildfile-json") {  // c7c tool buildfile-json Cmake7109.txt [--check-files]  (sources are looked up beside the file)
    std::string text;
    if (!readFile(file, text)) { std::cerr << "cannot read " << file << "\n"; return 2; }
    DiagEngine d;
    int fid = d.addFile(file, text);
    bool check = false;
    for (int i = 4; i < argc; i++) {
      std::string a = argv[i];
      if (a == "--check-files") check = true;
      else if (a.rfind("--lang=", 0) == 0) d.lang = a.substr(7);
    }
    std::string base = check ? std::filesystem::path(file).parent_path().string() : "";
    if (check && base.empty()) base = ".";
    BuildFile b;
    bool ok = parseBuildFile(d, fid, base, b);
    d.print(std::cerr, false);
    if (!ok) return 1;
    std::cout << buildFileToJson(b) << "\n";
    return 0;
  }
  if (sub == "resolve-json") {  // c7c tool resolve-json Cmake7109.txt [--registry DIR]...   (path dependencies must be .l7 files)
    std::string text;
    if (!readFile(file, text)) { std::cerr << "cannot read " << file << "\n"; return 2; }
    DiagEngine d;
    int fid = d.addFile(file, text);
    BuildFile b;
    std::string base = std::filesystem::path(file).parent_path().string();
    if (base.empty()) base = ".";
    if (!parseBuildFile(d, fid, "", b)) { d.print(std::cerr, false); return 1; }
    std::vector<std::string> regs = b.registries;
    for (int i = 4; i + 1 < argc; i += 2) if (std::string(argv[i]) == "--registry") regs.push_back(argv[i + 1]);
    std::vector<RootDep> roots;
    for (auto& dep : b.deps) {
      std::string file;
      if (dep.isPath) { std::filesystem::path q(dep.path); file = (q.is_relative() ? std::filesystem::path(base) / q : q).lexically_normal().string(); }
      roots.push_back(RootDep{dep.name, dep.req, dep.isPath, file, dep.path, dep.loc});
    }
    std::vector<Resolved> res;
    bool ok = resolveDependencies(d, roots, regs, base, res);
    d.print(std::cerr, false);
    if (!ok) return 1;
    std::cout << resolvedToJson(res) << "\n";
    return 0;
  }
  if (sub == "graph-json") {  // c7c tool graph-json Cmake7109.txt [--out-dir D] [--host ARCH-OS]
    std::string text;
    if (!readFile(file, text)) { std::cerr << "cannot read " << file << "\n"; return 2; }
    DiagEngine d;
    int fid = d.addFile(file, text);
    std::string outDir = "build", host = kSupportedPlatform;
    for (int i = 4; i + 1 < argc; i += 2) {
      std::string a = argv[i];
      if (a == "--out-dir") outDir = argv[i + 1];
      else if (a == "--host") host = argv[i + 1];
    }
    BuildFile b;
    BuildGraph g;
    bool ok = parseBuildFile(d, fid, "", b) && buildGraph(d, b, outDir, host, g);
    d.print(std::cerr, false);
    if (!ok) return 1;
    std::cout << graphToJson(g) << "\n";
    return 0;
  }
  if (sub == "options-json") {
    // c7c tool options-json [--cset7 F] [--build F --config NAME --target NAME] [command-line options...]
    // Layers are merged lowest to highest: defaults < .cset7 < config < target < command line.
    DiagEngine d;
    OptionLayer cset, cfg, tgt, cli;
    std::string cset7, bfile, cname, tname;
    for (int i = 3; i < argc; i++) {
      std::string a = argv[i];
      if (a == "--cset7" && i + 1 < argc) cset7 = argv[++i];
      else if (a == "--build" && i + 1 < argc) bfile = argv[++i];
      else if (a == "--config" && i + 1 < argc) cname = argv[++i];
      else if (a == "--target" && i + 1 < argc) tname = argv[++i];
      else { std::string err; if (!parseOptionArg(a, cli, err)) { std::cerr << "unknown option " << a << "\n"; return 2; } if (!err.empty()) { std::cerr << err << "\n"; return 1; } }
    }
    if (!cset7.empty()) {
      std::string text;
      if (!readFile(cset7, text)) { std::cerr << "cannot read " << cset7 << "\n"; return 2; }
      int fid = d.addFile(cset7, text);
      bool ok = loadCset7(d, fid, cset);
      if (!ok) { d.print(std::cerr, false); return 1; }
    }
    if (!bfile.empty()) {
      std::string text;
      if (!readFile(bfile, text)) { std::cerr << "cannot read " << bfile << "\n"; return 2; }
      int fid = d.addFile(bfile, text);
      BuildFile b;
      if (!parseBuildFile(d, fid, "", b)) { d.print(std::cerr, false); return 1; }
      if (cname.empty()) cname = b.defaultConfig;
      if (!cname.empty()) {
        bool found = false;
        for (auto& c : b.configs) if (c.name == cname) { cfg = layerFromConfig(c); found = true; }
        if (!found) { std::cerr << "no such config: " << cname << "\n"; return 1; }
      }
      if (!tname.empty()) {
        bool found = false;
        for (auto& t : b.targets) if (t.name == tname) { tgt = layerFromTarget(t); found = true; }
        if (!found) { std::cerr << "no such target: " << tname << "\n"; return 1; }
      }
    }
    Options o = mergeOptions({&cset, &cfg, &tgt, &cli});
    bool ok = checkOptionsCompatible(d, Loc{}, o);
    d.print(std::cerr, false);
    if (!ok) return 1;
    std::cout << optionsToJson(o) << "\n";
    return 0;
  }
  if (sub == "inflate" || sub == "deflate") {
    std::string text;
    if (!readFile(file, text)) { std::cerr << "cannot read " << file << "\n"; return 2; }
    const uint8_t* p = (const uint8_t*)text.data();
    if (sub == "deflate") {
      auto z = deflateRaw(p, text.size());
      std::cout.write((const char*)z.data(), (std::streamsize)z.size());
      return 0;
    }
    std::vector<uint8_t> out;
    std::string err;
    size_t used = 0;
    if (!inflateRaw(p, text.size(), used, out, 1ull << 30, err)) { std::cerr << "inflate: " << err << "\n"; return 1; }
    std::cout.write((const char*)out.data(), (std::streamsize)out.size());
    return 0;
  }
  if (sub == "zip-create") {  // c7c tool zip-create out.zip name=path ...
    std::vector<ZipEntry> entries;
    for (int i = 4; i < argc; i++) {
      std::string a = argv[i];
      auto eq = a.find('=');
      std::string name = eq == std::string::npos ? a : a.substr(0, eq), path = eq == std::string::npos ? a : a.substr(eq + 1), text;
      if (!readFile(path, text)) { std::cerr << "cannot read " << path << "\n"; return 2; }
      entries.push_back({name, std::vector<uint8_t>(text.begin(), text.end())});
    }
    std::vector<uint8_t> z;
    std::string err;
    if (!zipWrite(entries, z, err)) { std::cerr << "zip: " << err << "\n"; return 1; }
    std::ofstream o(file, std::ios::binary);
    o.write((const char*)z.data(), (std::streamsize)z.size());
    return 0;
  }
  if (sub == "zip-list" || sub == "zip-cat") {
    std::string text;
    if (!readFile(file, text)) { std::cerr << "cannot read " << file << "\n"; return 2; }
    std::vector<ZipEntry> entries;
    std::string err;
    if (!zipRead(std::vector<uint8_t>(text.begin(), text.end()), entries, err)) { std::cerr << "zip: " << err << "\n"; return 1; }
    for (auto& e : entries) {
      if (sub == "zip-list") std::cout << e.name << "\t" << e.data.size() << "\t" << std::hex << crc32(e.data.data(), e.data.size()) << std::dec << "\n";
      else if (argc > 4 && e.name == argv[4]) std::cout.write((const char*)e.data.data(), (std::streamsize)e.data.size());
    }
    return 0;
  }
  if (sub == "semver-cmp" || sub == "semver-match") {  // c7c tool semver-cmp A B | semver-match REQ VERSION
    std::string a = file, b = argc > 4 ? argv[4] : "", err;
    Version va, vb;
    if (sub == "semver-cmp") {
      if (!parseVersion(a, va, err) || !parseVersion(b, vb, err)) { std::cout << "invalid: " << err << "\n"; return 1; }
      std::cout << compareVersions(va, vb) << "\n";
      return 0;
    }
    Requirement r;
    if (!parseRequirement(a, r, err)) { std::cout << "invalid requirement: " << err << "\n"; return 1; }
    if (!parseVersion(b, vb, err)) { std::cout << "invalid version: " << err << "\n"; return 1; }
    std::cout << (matches(r, vb) ? "yes" : "no") << "\n";
    return 0;
  }
  if (sub == "l7-pack") {  // c7c tool l7-pack out.l7 NAME VERSION [--dep NAME REQ] [--bin os arch abi path] [--license L] archive-path=file ...
    if (argc < 6) { std::cerr << "usage: l7-pack out.l7 NAME VERSION ...\n"; return 2; }
    Manifest m;
    m.name = argv[4]; m.version = argv[5];
    std::vector<ZipEntry> content;
    for (int i = 6; i < argc; i++) {
      std::string a = argv[i];
      if (a == "--dep" && i + 2 < argc) { m.deps.push_back({argv[i + 1], argv[i + 2]}); i += 2; continue; }
      if (a == "--bin" && i + 4 < argc) { m.binaries.push_back({argv[i + 1], argv[i + 2], argv[i + 3], argv[i + 4]}); i += 4; continue; }
      if (a == "--license" && i + 1 < argc) { m.license = argv[++i]; continue; }
      if (a == "--compiler" && i + 1 < argc) { m.compilerReq = argv[++i]; continue; }
      auto eq = a.find('=');
      std::string name = a.substr(0, eq), path = eq == std::string::npos ? a : a.substr(eq + 1), text;
      if (!readFile(path, text)) { std::cerr << "cannot read " << path << "\n"; return 2; }
      content.push_back({name, std::vector<uint8_t>(text.begin(), text.end())});
    }
    DiagEngine d;
    std::vector<uint8_t> out;
    if (!buildPackage(d, m, content, out)) { d.print(std::cerr, false); return 1; }
    if (!writeFileBytes(file, out)) { std::cerr << "cannot write " << file << "\n"; return 2; }
    return 0;
  }
  if (sub == "l7-verify" || sub == "l7-extract") {
    DiagEngine d;
    Package p;
    bool ok = loadPackage(d, file, p);
    if (ok && sub == "l7-extract") ok = extractPackage(d, p, argc > 4 ? argv[4] : "");
    d.print(std::cerr, false);
    if (!ok) return 1;
    std::cout << p.m.name << " " << p.m.version << " " << p.sha256 << " files=" << p.files.size() << " sources=" << p.sources().size() << "\n";
    return 0;
  }
  std::cerr << "usage: c7c tool <sha256|buildfile-json|toml-json|toml-roundtrip|inflate|deflate|zip-create|zip-list|zip-cat> file ...\n";
  return 2;
}

}  // namespace c7pkg
