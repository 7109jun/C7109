// SHA-256 (FIPS 180-4). Self-contained: the toolchain must not depend on an external hashing library.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace c7pkg {

class Sha256 {
 public:
  Sha256();
  void update(const void* data, size_t n);
  std::string hexDigest();  // finishes the hash; the object must not be reused afterwards
 private:
  void block(const uint8_t* p);
  uint32_t h_[8];
  uint8_t buf_[64];
  size_t used_ = 0;
  uint64_t total_ = 0;
};

std::string sha256Hex(const std::string& data);
std::string sha256Hex(const std::vector<uint8_t>& data);
bool sha256File(const std::string& path, std::string& hex);  // false when the file cannot be read

}  // namespace c7pkg
