#include "zip.hpp"

#include <algorithm>
#include <cstring>
#include <set>

namespace c7pkg {

uint32_t crc32(const uint8_t* p, size_t n, uint32_t crc) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    init = true;
  }
  crc = ~crc;
  for (size_t i = 0; i < n; i++) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

// ======================================================= inflate (canonical Huffman decoding, as in RFC 1951)
namespace {
const uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint16_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint16_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

struct BitIn {
  const uint8_t* p;
  size_t n, pos = 0;
  uint32_t buf = 0;
  int cnt = 0;
  bool bits(int need, uint32_t& out) {
    uint32_t v = buf;
    while (cnt < need) {
      if (pos >= n) return false;
      v |= (uint32_t)p[pos++] << cnt;
      cnt += 8;
    }
    buf = v >> need;
    cnt -= need;
    out = v & ((1u << need) - 1);
    return true;
  }
};

struct Huff {
  uint16_t count[16];
  uint16_t symbol[320];
};

// returns 0 for a complete code, >0 incomplete, <0 over-subscribed
int construct(Huff& h, const uint16_t* length, int n) {
  for (int l = 0; l < 16; l++) h.count[l] = 0;
  for (int s = 0; s < n; s++) h.count[length[s]]++;
  if (h.count[0] == n) return 0;
  int left = 1;
  for (int l = 1; l < 16; l++) {
    left <<= 1;
    left -= h.count[l];
    if (left < 0) return left;
  }
  uint16_t offs[16];
  offs[1] = 0;
  for (int l = 1; l < 15; l++) offs[l + 1] = offs[l] + h.count[l];
  for (int s = 0; s < n; s++) if (length[s]) h.symbol[offs[length[s]]++] = (uint16_t)s;
  return left;
}

bool decodeSym(BitIn& in, const Huff& h, int& sym) {
  int code = 0, first = 0, index = 0;
  for (int len = 1; len < 16; len++) {
    uint32_t b;
    if (!in.bits(1, b)) return false;
    code |= (int)b;
    int count = h.count[len];
    if (code - count < first) { sym = h.symbol[index + (code - first)]; return true; }
    index += count;
    first += count;
    first <<= 1;
    code <<= 1;
  }
  return false;
}

bool codes(BitIn& in, std::vector<uint8_t>& out, size_t maxOut, const Huff& lc, const Huff& dc, std::string& err) {
  for (;;) {
    int sym;
    if (!decodeSym(in, lc, sym)) { err = "invalid Huffman code or truncated data"; return false; }
    if (sym < 256) {
      if (out.size() >= maxOut) { err = "output exceeds the size limit"; return false; }
      out.push_back((uint8_t)sym);
    } else if (sym == 256) return true;
    else {
      sym -= 257;
      if (sym >= 29) { err = "invalid length symbol"; return false; }
      uint32_t eb;
      if (!in.bits(kLenExtra[sym], eb)) { err = "truncated data"; return false; }
      size_t len = kLenBase[sym] + eb;
      int ds;
      if (!decodeSym(in, dc, ds) || ds >= 30) { err = "invalid distance symbol"; return false; }
      if (!in.bits(kDistExtra[ds], eb)) { err = "truncated data"; return false; }
      size_t dist = kDistBase[ds] + eb;
      if (dist > out.size()) { err = "distance reaches before the start of the output"; return false; }
      if (out.size() + len > maxOut) { err = "output exceeds the size limit"; return false; }
      size_t from = out.size() - dist;
      for (size_t k = 0; k < len; k++) out.push_back(out[from + k]);
    }
  }
}
}  // namespace

bool inflateRaw(const uint8_t* data, size_t n, size_t& consumed, std::vector<uint8_t>& out, size_t maxOut, std::string& err) {
  BitIn in{data, n};
  out.clear();
  uint32_t last;
  do {
    uint32_t type;
    if (!in.bits(1, last) || !in.bits(2, type)) { err = "truncated data"; return false; }
    if (type == 0) {
      in.buf = 0; in.cnt = 0;
      if (in.pos + 4 > n) { err = "truncated stored block"; return false; }
      uint32_t len = in.p[in.pos] | (uint32_t)in.p[in.pos + 1] << 8, nlen = in.p[in.pos + 2] | (uint32_t)in.p[in.pos + 3] << 8;
      in.pos += 4;
      if (len != (~nlen & 0xFFFF)) { err = "stored block length check failed"; return false; }
      if (in.pos + len > n) { err = "truncated stored block"; return false; }
      if (out.size() + len > maxOut) { err = "output exceeds the size limit"; return false; }
      out.insert(out.end(), in.p + in.pos, in.p + in.pos + len);
      in.pos += len;
    } else if (type == 1) {
      static Huff lc, dc;
      static bool init = false;
      if (!init) {
        uint16_t l[288];
        int s = 0;
        for (; s < 144; s++) l[s] = 8;
        for (; s < 256; s++) l[s] = 9;
        for (; s < 280; s++) l[s] = 7;
        for (; s < 288; s++) l[s] = 8;
        construct(lc, l, 288);
        for (s = 0; s < 30; s++) l[s] = 5;
        construct(dc, l, 30);
        init = true;
      }
      if (!codes(in, out, maxOut, lc, dc, err)) return false;
    } else if (type == 2) {
      static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
      uint32_t nlen, ndist, ncode;
      if (!in.bits(5, nlen) || !in.bits(5, ndist) || !in.bits(4, ncode)) { err = "truncated data"; return false; }
      nlen += 257; ndist += 1; ncode += 4;
      if (nlen > 286 || ndist > 30) { err = "too many length or distance codes"; return false; }
      uint16_t lengths[320];
      uint32_t v;
      for (uint32_t i = 0; i < ncode; i++) { if (!in.bits(3, v)) { err = "truncated data"; return false; } lengths[order[i]] = (uint16_t)v; }
      for (uint32_t i = ncode; i < 19; i++) lengths[order[i]] = 0;
      Huff lc, dc;
      if (construct(lc, lengths, 19) != 0) { err = "incomplete code-length code"; return false; }
      uint32_t idx = 0;
      while (idx < nlen + ndist) {
        int sym;
        if (!decodeSym(in, lc, sym)) { err = "invalid code-length code"; return false; }
        if (sym < 16) lengths[idx++] = (uint16_t)sym;
        else {
          uint16_t prev = 0;
          uint32_t rep;
          if (sym == 16) {
            if (idx == 0) { err = "repeat with no previous length"; return false; }
            prev = lengths[idx - 1];
            if (!in.bits(2, rep)) { err = "truncated data"; return false; }
            rep += 3;
          } else if (sym == 17) { if (!in.bits(3, rep)) { err = "truncated data"; return false; } rep += 3; }
          else { if (!in.bits(7, rep)) { err = "truncated data"; return false; } rep += 11; }
          if (idx + rep > nlen + ndist) { err = "too many code lengths"; return false; }
          while (rep--) lengths[idx++] = prev;
        }
      }
      if (lengths[256] == 0) { err = "missing end-of-block code"; return false; }
      int e1 = construct(lc, lengths, (int)nlen);
      if (e1 < 0 || (e1 > 0 && (int)nlen != lc.count[0] + lc.count[1])) { err = "invalid literal/length code"; return false; }
      int e2 = construct(dc, lengths + nlen, (int)ndist);
      if (e2 < 0 || (e2 > 0 && (int)ndist != dc.count[0] + dc.count[1])) { err = "invalid distance code"; return false; }
      if (!codes(in, out, maxOut, lc, dc, err)) return false;
    } else { err = "invalid block type"; return false; }
  } while (!last);
  consumed = in.pos;
  return true;
}

// ======================================================= deflate encoder
namespace {
struct BitOut {
  std::vector<uint8_t> out;
  uint32_t acc = 0;
  int nbits = 0;
  void put(uint32_t v, int n) {
    acc |= v << nbits;
    nbits += n;
    while (nbits >= 8) { out.push_back((uint8_t)acc); acc >>= 8; nbits -= 8; }
  }
  void huff(uint32_t code, int len) {  // Huffman codes are sent most-significant bit first
    uint32_t r = 0;
    for (int i = 0; i < len; i++) r |= ((code >> i) & 1) << (len - 1 - i);
    put(r, len);
  }
  void flush() { if (nbits > 0) { out.push_back((uint8_t)acc); acc = 0; nbits = 0; } }
};

void putLitLen(BitOut& b, int sym) {
  if (sym < 144) b.huff(0x30 + sym, 8);
  else if (sym < 256) b.huff(0x190 + (sym - 144), 9);
  else if (sym < 280) b.huff(sym - 256, 7);
  else b.huff(0xC0 + (sym - 280), 8);
}
}  // namespace

std::vector<uint8_t> deflateRaw(const uint8_t* p, size_t n) {
  BitOut b;
  b.put(1, 1);  // final block
  b.put(1, 2);  // fixed Huffman
  const int kHash = 1 << 15;
  std::vector<int32_t> head(kHash, -1), prev(n ? n : 1, -1);
  auto hash = [&](size_t i) { return ((p[i] << 10) ^ (p[i + 1] << 5) ^ p[i + 2]) & (kHash - 1); };
  auto insert = [&](size_t i) { if (i + 3 <= n) { int h = hash(i); prev[i] = head[h]; head[h] = (int32_t)i; } };
  size_t i = 0;
  while (i < n) {
    size_t bestLen = 0, bestDist = 0;
    if (i + 3 <= n) {
      int cand = head[hash(i)], chain = 0;
      while (cand >= 0 && i - (size_t)cand <= 32768 && chain < 64) {
        size_t l = 0;
        while (l < 258 && i + l < n && p[cand + l] == p[i + l]) l++;
        if (l > bestLen) { bestLen = l; bestDist = i - (size_t)cand; if (l == 258) break; }
        cand = prev[cand];
        chain++;
      }
    }
    if (bestLen >= 3) {
      int ls = 28;
      while (kLenBase[ls] > bestLen) ls--;
      putLitLen(b, 257 + ls);
      if (kLenExtra[ls]) b.put((uint32_t)(bestLen - kLenBase[ls]), kLenExtra[ls]);
      int ds = 29;
      while (kDistBase[ds] > bestDist) ds--;
      b.huff((uint32_t)ds, 5);
      if (kDistExtra[ds]) b.put((uint32_t)(bestDist - kDistBase[ds]), kDistExtra[ds]);
      for (size_t k = 0; k < bestLen; k++) insert(i + k);
      i += bestLen;
    } else {
      putLitLen(b, p[i]);
      insert(i);
      i++;
    }
  }
  putLitLen(b, 256);
  b.flush();
  return b.out;
}

// ======================================================= ZIP
bool zipSafeName(const std::string& name, std::string& why) {
  if (name.empty()) { why = "empty name"; return false; }
  if (name.size() > 1024) { why = "name too long"; return false; }
  if (name[0] == '/') { why = "absolute path"; return false; }
  if (name.size() >= 2 && name[1] == ':') { why = "drive letter"; return false; }
  for (unsigned char c : name) {
    if (c == '\\') { why = "backslash in path"; return false; }
    if (c < 0x20 || c == 0x7F) { why = "control character in path"; return false; }
  }
  std::string body = name.back() == '/' ? name.substr(0, name.size() - 1) : name;  // a directory name may end in one slash
  if (body.empty()) { why = "empty name"; return false; }
  size_t start = 0;
  for (size_t i = 0; i <= body.size(); i++) {
    if (i != body.size() && body[i] != '/') continue;
    std::string seg = body.substr(start, i - start);
    if (seg.empty()) { why = "empty path segment"; return false; }
    if (seg == "." || seg == "..") { why = "`" + seg + "` segment"; return false; }
    start = i + 1;
  }
  return true;
}

namespace {
void le16(std::vector<uint8_t>& o, uint32_t v) { o.push_back(v & 0xFF); o.push_back((v >> 8) & 0xFF); }
void le32(std::vector<uint8_t>& o, uint32_t v) { le16(o, v & 0xFFFF); le16(o, v >> 16); }
uint32_t rd16(const std::vector<uint8_t>& b, size_t at) { return b[at] | (uint32_t)b[at + 1] << 8; }
uint32_t rd32(const std::vector<uint8_t>& b, size_t at) { return rd16(b, at) | rd16(b, at + 2) << 16; }

bool validUtf8(const std::string& s) {
  size_t i = 0;
  while (i < s.size()) {
    unsigned char c = s[i];
    size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    if (n == 0 || i + n > s.size()) return false;
    for (size_t k = 1; k < n; k++) if ((s[i + k] & 0xC0) != 0x80) return false;
    i += n;
  }
  return true;
}
std::string lower(std::string s) { for (auto& c : s) if (c >= 'A' && c <= 'Z') c += 32; return s; }
}  // namespace

bool zipWrite(const std::vector<ZipEntry>& entries, std::vector<uint8_t>& out, std::string& err) {
  if (entries.size() > 0xFFFE) { err = "too many entries (zip64 is not supported)"; return false; }
  out.clear();
  struct Rec { uint32_t crc, csize, usize, offset, method, flags; };
  std::vector<Rec> recs;
  std::set<std::string> seen;
  for (auto& e : entries) {
    std::string why;
    if (!zipSafeName(e.name, why)) { err = "unsafe entry name `" + e.name + "`: " + why; return false; }
    if (!seen.insert(lower(e.name)).second) { err = "duplicate entry name `" + e.name + "`"; return false; }
    if (e.data.size() > 0xFFFFFFFEull) { err = "entry too large (zip64 is not supported)"; return false; }
    std::vector<uint8_t> comp = deflateRaw(e.data.data(), e.data.size());
    uint32_t method = 8;
    if (comp.size() >= e.data.size()) { comp = e.data; method = 0; }
    bool ascii = true;
    for (unsigned char c : e.name) if (c >= 0x80) ascii = false;
    Rec r{crc32(e.data.data(), e.data.size()), (uint32_t)comp.size(), (uint32_t)e.data.size(), (uint32_t)out.size(), method, ascii ? 0u : 0x0800u};
    recs.push_back(r);
    le32(out, 0x04034b50);
    le16(out, method == 8 ? 20 : 10);
    le16(out, r.flags);
    le16(out, method);
    le16(out, 0);       // time 00:00:00
    le16(out, 0x0021);  // date 1980-01-01
    le32(out, r.crc); le32(out, r.csize); le32(out, r.usize);
    le16(out, (uint32_t)e.name.size()); le16(out, 0);
    out.insert(out.end(), e.name.begin(), e.name.end());
    out.insert(out.end(), comp.begin(), comp.end());
  }
  size_t cdStart = out.size();
  for (size_t k = 0; k < entries.size(); k++) {
    const Rec& r = recs[k];
    le32(out, 0x02014b50);
    le16(out, 0x0314);  // made by Unix, version 2.0
    le16(out, r.method == 8 ? 20 : 10);
    le16(out, r.flags); le16(out, r.method); le16(out, 0); le16(out, 0x0021);
    le32(out, r.crc); le32(out, r.csize); le32(out, r.usize);
    le16(out, (uint32_t)entries[k].name.size()); le16(out, 0); le16(out, 0);
    le16(out, 0); le16(out, 0);
    le32(out, 0x81A40000u);  // regular file, 0644
    le32(out, r.offset);
    out.insert(out.end(), entries[k].name.begin(), entries[k].name.end());
  }
  size_t cdSize = out.size() - cdStart;
  le32(out, 0x06054b50);
  le16(out, 0); le16(out, 0);
  le16(out, (uint32_t)entries.size()); le16(out, (uint32_t)entries.size());
  le32(out, (uint32_t)cdSize); le32(out, (uint32_t)cdStart);
  le16(out, 0);
  return true;
}

bool zipRead(const std::vector<uint8_t>& b, std::vector<ZipEntry>& out, std::string& err, const ZipLimits& lim) {
  out.clear();
  if (b.size() < 22) { err = "file too small to be a ZIP archive"; return false; }
  size_t eocd = std::string::npos;
  size_t lowest = b.size() > 22 + 65535 ? b.size() - 22 - 65535 : 0;
  for (size_t i = b.size() - 22 + 1; i-- > lowest;) if (rd32(b, i) == 0x06054b50) { eocd = i; break; }
  if (eocd == std::string::npos) { err = "end of central directory not found"; return false; }
  uint32_t diskNo = rd16(b, eocd + 4), cdDisk = rd16(b, eocd + 6), entriesDisk = rd16(b, eocd + 8), total = rd16(b, eocd + 10);
  uint32_t cdSize = rd32(b, eocd + 12), cdOff = rd32(b, eocd + 16);
  if (diskNo != 0 || cdDisk != 0 || entriesDisk != total) { err = "multi-disk archives are not supported"; return false; }
  if (total == 0xFFFF || cdSize == 0xFFFFFFFFu || cdOff == 0xFFFFFFFFu) { err = "zip64 archives are not supported"; return false; }
  if (total > lim.maxEntries) { err = "too many entries"; return false; }
  if ((uint64_t)cdOff + cdSize > eocd) { err = "central directory out of range"; return false; }
  size_t at = cdOff;
  uint64_t totalBytes = 0;
  std::set<std::string> seen;
  for (uint32_t k = 0; k < total; k++) {
    if (at + 46 > b.size() || rd32(b, at) != 0x02014b50) { err = "bad central directory entry"; return false; }
    uint32_t flags = rd16(b, at + 8), method = rd16(b, at + 10), crc = rd32(b, at + 16), csize = rd32(b, at + 20), usize = rd32(b, at + 24);
    uint32_t nlen = rd16(b, at + 28), elen = rd16(b, at + 30), clen = rd16(b, at + 32), lho = rd32(b, at + 42);
    if (at + 46 + nlen + elen + clen > cdOff + (uint64_t)cdSize) { err = "central directory entry overruns the directory"; return false; }
    std::string name((const char*)&b[at + 46], nlen);
    at += 46 + nlen + elen + clen;
    if (flags & 0x41) { err = "encrypted entries are not supported (`" + name + "`)"; return false; }
    if (csize == 0xFFFFFFFFu || usize == 0xFFFFFFFFu || lho == 0xFFFFFFFFu) { err = "zip64 entries are not supported (`" + name + "`)"; return false; }
    if (method != 0 && method != 8) { err = "unsupported compression method " + std::to_string(method) + " (`" + name + "`)"; return false; }
    if (!validUtf8(name)) { err = "entry name is not valid UTF-8"; return false; }
    bool isDir = !name.empty() && name.back() == '/';
    std::string why;
    if (!zipSafeName(name, why)) { err = "unsafe entry name `" + name + "`: " + why; return false; }
    if (!seen.insert(lower(isDir ? name.substr(0, name.size() - 1) : name)).second) { err = "duplicate entry name `" + name + "`"; return false; }
    if (isDir) { if (usize != 0) { err = "directory entry with content (`" + name + "`)"; return false; } continue; }
    if (usize > lim.maxEntryBytes) { err = "entry `" + name + "` exceeds the size limit"; return false; }
    totalBytes += usize;
    if (totalBytes > lim.maxTotalBytes) { err = "archive exceeds the total size limit"; return false; }
    if ((uint64_t)lho + 30 > cdOff || rd32(b, lho) != 0x04034b50) { err = "bad local header for `" + name + "`"; return false; }
    uint64_t dataStart = (uint64_t)lho + 30 + rd16(b, lho + 26) + rd16(b, lho + 28);
    if (dataStart + csize > cdOff) { err = "entry data out of range for `" + name + "`"; return false; }
    ZipEntry e;
    e.name = name;
    if (method == 0) {
      if (csize != usize) { err = "stored entry size mismatch (`" + name + "`)"; return false; }
      e.data.assign(b.begin() + dataStart, b.begin() + dataStart + csize);
    } else {
      size_t used = 0;
      std::string ierr;
      if (!inflateRaw(&b[dataStart], csize, used, e.data, usize, ierr)) { err = "cannot decompress `" + name + "`: " + ierr; return false; }
      if (e.data.size() != usize) { err = "size mismatch after decompression (`" + name + "`)"; return false; }
    }
    if (crc32(e.data.data(), e.data.size()) != crc) { err = "CRC-32 mismatch (`" + name + "`)"; return false; }
    out.push_back(std::move(e));
  }
  return true;
}

}  // namespace c7pkg
