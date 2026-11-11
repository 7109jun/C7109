#include "mapfile.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <sstream>

#include "sha256.hpp"

namespace c7pkg {

namespace {

std::string shellQuote(const std::string& s) {
  std::string r = "'";
  for (char c : s) { if (c == '\'') r += "'\\''"; else r += c; }
  return r + "'";
}

// Runs a command and captures stdout. `ok` is false if it could not start or exited non-zero.
std::string capture(const std::string& cmd, bool& ok) {
  std::string out;
  FILE* f = popen((cmd + " 2>/dev/null").c_str(), "r");
  if (!f) { ok = false; return out; }
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
  int rc = pclose(f);
  ok = rc == 0;
  return out;
}

std::vector<std::string> tokens(const std::string& line) {
  std::vector<std::string> t;
  std::istringstream is(line);
  std::string w;
  while (is >> w) t.push_back(w);
  return t;
}

unsigned long long hexVal(const std::string& s) {
  try { return std::stoull(s, nullptr, 16); } catch (...) { return 0; }
}

std::string hex0x(unsigned long long v) {
  char b[32];
  snprintf(b, sizeof b, "0x%016llx", v);
  return b;
}

}  // namespace

std::string demangleC7Symbol(const std::string& name) {
  if (name.rfind("_C7$", 0) != 0) return name;
  std::string rest = name.substr(4);
  size_t dd = rest.find("$$");
  if (dd == std::string::npos) return name;
  std::string path = rest.substr(0, dd), args = rest.substr(dd + 2);
  std::string r;
  for (size_t i = 0; i < path.size(); i++) {
    if (path[i] == '$') r += "::"; else r += path[i];
  }
  r += "(";
  for (size_t i = 0; i < args.size(); i++) {
    if (args[i] == '$') r += ", "; else r += args[i];
  }
  return r + ")";
}

std::string buildMapText(const MapFile& m) {
  std::ostringstream o;
  o << "C7109 MAP FILE\nFormat: 1\n";
  o << "Project: " << m.project << "\n";
  o << "Target: " << m.target << " (executable)\n";
  o << "Platform: " << m.platform << " (host: " << m.host << ")\n";
  o << "Configuration: " << (m.config.empty() ? "(none)" : m.config) << " (mode=" << m.mode << ", optimization=" << m.optimization << ")\n";
  o << "Compiler: " << m.compiler << "\n";
  o << "Units: addresses are hexadecimal virtual addresses (0x...); sizes are in bytes\n\n";
  o << "[Inputs]\n";
  for (auto& i : m.inputs) o << i.path << "  sha256=" << i.sha256 << "  bytes=" << i.bytes << "\n";
  o << "\n[Libraries]\n";
  for (auto& l : m.libs) o << l.name << " " << l.version << "  sha256=" << l.sha256 << "\n";
  std::string h;
  struct stat st {};
  bool have = stat(m.outputFile.c_str(), &st) == 0 && sha256File(m.outputFile, h);
  o << "\n[Outputs]\n" << m.outputPath;
  if (have) o << "  bytes=" << (unsigned long long)st.st_size << "  sha256=" << h;
  o << "\n";

  bool ok = false;
  if (m.platform == "x86_64-windows") {  // PE images are not described yet: say so instead of leaving the sections out
    o << "\n[Sections]\n; unavailable: section listing for PE executables is not implemented yet\n";
    o << "\n[Symbols]\n; unavailable: symbol listing for PE executables is not implemented yet\n";
    return o.str();
  }
  std::string sec = capture("readelf -SW " + shellQuote(m.outputFile), ok);
  o << "\n[Sections]\n";
  if (!ok) o << "; unavailable: readelf could not read " << m.outputPath << "\n";
  else {
    std::istringstream is(sec);
    std::string line;
    while (std::getline(is, line)) {
      size_t rb = line.find(']');
      if (line.find('[') == std::string::npos || rb == std::string::npos) continue;
      auto t = tokens(line.substr(rb + 1));
      if (t.size() < 5 || t[0] == "Name") continue;  // name type addr off size (skips the column header)
      unsigned long long addr = hexVal(t[2]);
      if (addr == 0) continue;     // not mapped into memory
      o << t[0] << " addr=" << hex0x(addr) << " size=" << hexVal(t[4]) << "\n";
    }
  }
  o << "\n[Symbols]\n";
  o << "; only C7 symbols, main and c7_* runtime entry points; symbols removed by the optimizer or linker are not listed\n";
  o << "; columns: address size type(nm) name\n";
  ok = false;
  std::string syms = capture("nm -n -S --defined-only " + shellQuote(m.outputFile), ok);
  if (!ok) o << "; unavailable: nm could not read " << m.outputPath << "\n";
  else {
    std::istringstream is(syms);
    std::string line;
    while (std::getline(is, line)) {
      auto t = tokens(line);
      if (t.size() != 4) continue;  // addr size type name
      const std::string& n = t[3];
      if (!(n.rfind("_C7$", 0) == 0 || n == "main" || n.rfind("c7_", 0) == 0)) continue;
      o << hex0x(hexVal(t[0])) << " " << hexVal(t[1]) << " " << t[2] << " " << demangleC7Symbol(n) << "\n";
    }
  }
  return o.str();
}

bool writeMapFile(const std::string& path, const std::string& text) {
  std::string tmp = path + ".tmp" + std::to_string(getpid());
  {
    std::ofstream f(tmp, std::ios::binary);
    f << text;
    if (!f) { std::remove(tmp.c_str()); return false; }
  }
  if (std::rename(tmp.c_str(), path.c_str()) != 0) { std::remove(tmp.c_str()); return false; }
  return true;
}

}  // namespace c7pkg
