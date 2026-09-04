#include "sofa-brir-cache.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <utility>

namespace eac3render {
namespace {
constexpr std::array<std::uint8_t, 8> kMagic{
    {'R', '2', 'A', '1', 'B', 'R', 'I', 'R'}};
constexpr std::uint32_t kVersion = 1U;
constexpr std::uint32_t kHeaderSize = 520U;
constexpr std::size_t kPayloadFloats =
    2U * kR2aBrirEmitterCount * kR2aBrirIrLength;

bool readBytes(const std::vector<std::uint8_t> &b, std::size_t *p, void *out,
               std::size_t n) {
  if (!out || *p + n > b.size())
    return false;
  std::memcpy(out, b.data() + *p, n);
  *p += n;
  return true;
}
bool readU32(const std::vector<std::uint8_t> &b, std::size_t *p,
             std::uint32_t *out) {
  std::uint32_t v = 0;
  if (*p + 4 > b.size())
    return false;
  for (int i = 0; i < 4; ++i)
    v |= static_cast<std::uint32_t>(b[*p + i]) << (8 * i);
  *p += 4;
  *out = v;
  return true;
}
bool readI32(const std::vector<std::uint8_t> &b, std::size_t *p,
             std::int32_t *out) {
  std::uint32_t v = 0;
  if (!readU32(b, p, &v))
    return false;
  std::memcpy(out, &v, 4);
  return true;
}
bool readF32(const std::vector<std::uint8_t> &b, std::size_t *p, float *out) {
  std::uint32_t v = 0;
  if (!readU32(b, p, &v))
    return false;
  std::memcpy(out, &v, 4);
  return true;
}
bool readF64(const std::vector<std::uint8_t> &b, std::size_t *p, double *out) {
  if (*p + 8 > b.size())
    return false;
  std::uint64_t v = 0;
  for (int i = 0; i < 8; ++i)
    v |= static_cast<std::uint64_t>(b[*p + i]) << (8 * i);
  *p += 8;
  std::memcpy(out, &v, 8);
  return true;
}
bool finite(double x) { return std::isfinite(x); }
bool hex64(const std::string &s) {
  if (s.size() != 64U)
    return false;
  for (char c : s)
    if (!std::isxdigit(static_cast<unsigned char>(c)))
      return false;
  return true;
}
int hexValue(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}
} // namespace

SofaBrirCacheLoadResult loadSofaBrirCache(const std::string &path,
                                          const std::string &expected) {
  SofaBrirCacheLoadResult result;
  if (!expected.empty() && !hex64(expected)) {
    result.reason = "invalid-expected-sha256";
    return result;
  }
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) {
    result.reason = "cache-open-failed";
    return result;
  }
  const auto size = file.tellg();
  if (size < 0) {
    result.reason = "cache-size-failed";
    return result;
  }
  const std::size_t expectedSize = kHeaderSize + kPayloadFloats * sizeof(float);
  const auto actualSize = static_cast<std::uintmax_t>(size);
  if (actualSize != expectedSize) {
    result.reason = actualSize < kHeaderSize ? "cache-truncated-header"
                                             : "cache-size-invalid";
    return result;
  }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  file.seekg(0);
  if (!bytes.empty())
    file.read(reinterpret_cast<char *>(bytes.data()), bytes.size());
  if (!file && !bytes.empty()) {
    result.reason = "cache-read-failed";
    return result;
  }
  std::size_t p = 0U;
  SofaBrirCache cache;
  std::array<std::uint8_t, 8> magic{};
  std::uint32_t version = 0, header = 0, sr = 0, ec = 0, rc = 0, n = 0,
                hashLen = 0, left = 0, right = 0;
  std::int32_t m = 0;
  if (!readBytes(bytes, &p, magic.data(), 8) || magic != kMagic ||
      !readU32(bytes, &p, &version) || version != kVersion ||
      !readU32(bytes, &p, &header) || header != kHeaderSize ||
      !readU32(bytes, &p, &sr) || !readU32(bytes, &p, &ec) ||
      !readU32(bytes, &p, &rc) || !readU32(bytes, &p, &n) ||
      !readI32(bytes, &p, &m) || !readU32(bytes, &p, &hashLen) ||
      hashLen != 32U || p + 32U > bytes.size()) {
    result.reason = "cache-header-invalid";
    return result;
  }
  if (ec != kR2aBrirEmitterCount || rc != kR2aBrirReceiverCount ||
      n != kR2aBrirIrLength || sr == 0U || m < 0 || m >= 180) {
    result.reason = "cache-dimensions-invalid";
    return result;
  }
  std::copy(bytes.begin() + static_cast<std::ptrdiff_t>(p),
            bytes.begin() + static_cast<std::ptrdiff_t>(p + 32U),
            cache.sourceSha256.begin());
  p += 32U;
  if (!expected.empty()) {
    for (std::size_t i = 0; i < 32U; ++i) {
      const int hi = hexValue(expected[2U * i]),
                lo = hexValue(expected[2U * i + 1U]);
      if (cache.sourceSha256[i] != static_cast<std::uint8_t>((hi << 4) | lo)) {
        result.reason = "cache-source-hash-mismatch";
        return result;
      }
    }
  }
  for (auto &mapping : cache.systemToSofaEmitter)
    if (!readU32(bytes, &p, &mapping)) {
      result.reason = "cache-mapping-truncated";
      return result;
    }
  std::array<bool, kSystemHSpeakerCount> seen{};
  for (auto mapping : cache.systemToSofaEmitter) {
    if (mapping >= kSystemHSpeakerCount || seen[mapping]) {
      result.reason = "cache-mapping-invalid";
      return result;
    }
    seen[mapping] = true;
  }
  if (!readU32(bytes, &p, &left) || !readU32(bytes, &p, &right) || left >= 2U ||
      right >= 2U || left == right) {
    result.reason = "cache-receiver-mapping-invalid";
    return result;
  }
  cache.leftReceiverIndex = left;
  cache.rightReceiverIndex = right;
  for (auto &ear : cache.delays)
    for (double &delay : ear)
      if (!readF64(bytes, &p, &delay) || !finite(delay)) {
        result.reason = "cache-delay-invalid";
        return result;
      }
  if (p != kHeaderSize) {
    result.reason = "cache-size-or-header-invalid";
    return result;
  }
  cache.sampleRate = sr;
  cache.listenerViewIndex = m;
  for (auto &ear : cache.ir)
    ear.resize(kR2aBrirEmitterCount * kR2aBrirIrLength);
  for (auto &ear : cache.ir)
    for (float &sample : ear)
      if (!readF32(bytes, &p, &sample) || !std::isfinite(sample)) {
        result.reason = "cache-ir-invalid";
        return result;
      }
  if (p != bytes.size()) {
    result.reason = "cache-trailing-bytes";
    return result;
  }
  result.cache = std::move(cache);
  result.accepted = true;
  result.reason = "sofa-brir-cache-loaded";
  return result;
}
} // namespace eac3render
