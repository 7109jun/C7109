// ZIP container support for .l7 packages: CRC-32, raw DEFLATE (inflate + a deterministic encoder), and a strict ZIP reader/writer.
// The reader is written for untrusted input: unsafe paths, duplicate names, oversize entries, zip64 and encryption are rejected.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace c7pkg {

uint32_t crc32(const uint8_t* p, size_t n, uint32_t crc = 0);

// Decodes one raw DEFLATE stream. `maxOut` bounds the output (zip-bomb guard). `consumed` = input bytes used.
bool inflateRaw(const uint8_t* in, size_t n, size_t& consumed, std::vector<uint8_t>& out, size_t maxOut, std::string& err);
// Deterministic: the same input always gives the same bytes (LZ77 + fixed Huffman).
std::vector<uint8_t> deflateRaw(const uint8_t* in, size_t n);

struct ZipEntry {
  std::string name;
  std::vector<uint8_t> data;
};

struct ZipLimits {
  size_t maxEntries = 10000;
  uint64_t maxEntryBytes = 256ull << 20;
  uint64_t maxTotalBytes = 1ull << 30;
};

// Entry names must be relative, use `/`, contain no `.`/`..`/empty segments, no backslash, NUL or control characters.
bool zipSafeName(const std::string& name, std::string& why);

// Entries are written in the given order with fixed timestamps (1980-01-01) so output is reproducible.
bool zipWrite(const std::vector<ZipEntry>& entries, std::vector<uint8_t>& out, std::string& err);
bool zipRead(const std::vector<uint8_t>& bytes, std::vector<ZipEntry>& out, std::string& err, const ZipLimits& lim = ZipLimits());

}  // namespace c7pkg
