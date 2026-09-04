#include "sofa-brir-cache.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>

namespace {
constexpr std::size_t H = 520U, PAYLOAD = 2U * 22U * 16384U * sizeof(float);
struct Report {
  int cases = 0;
  int failures = 0;
};
void expect(Report *r, bool ok, const char *n) {
  ++r->cases;
  std::cout << (ok ? "PASS " : "FAIL ") << n << '\n';
  if (!ok)
    ++r->failures;
}
std::vector<std::uint8_t> readFile(const std::string &p) {
  std::ifstream f(p, std::ios::binary | std::ios::ate);
  if (!f)
    return {};
  auto n = f.tellg();
  std::vector<std::uint8_t> b(static_cast<size_t>(n));
  f.seekg(0);
  if (!b.empty())
    f.read(reinterpret_cast<char *>(b.data()), b.size());
  return b;
}
void writeFile(const std::string &p, const std::vector<std::uint8_t> &b) {
  std::ofstream f(p, std::ios::binary);
  if (!f) {
    std::cerr << "write failed: " << p << '\n';
    std::exit(2);
  }
  f.write(reinterpret_cast<const char *>(b.data()), b.size());
  if (!f) {
    std::cerr << "write failed: " << p << '\n';
    std::exit(2);
  }
}
void u32(std::vector<std::uint8_t> &b, size_t p, uint32_t v) {
  for (int i = 0; i < 4; ++i)
    b[p + i] = uint8_t(v >> (8 * i));
}
void f32(std::vector<std::uint8_t> &b, size_t p, float v) {
  uint32_t x;
  std::memcpy(&x, &v, 4);
  u32(b, p, x);
}
void f64(std::vector<std::uint8_t> &b, size_t p, double v) {
  uint64_t x;
  std::memcpy(&x, &v, 8);
  for (int i = 0; i < 8; ++i)
    b[p + i] = uint8_t(x >> (8 * i));
}
std::vector<std::uint8_t> synthetic() {
  std::vector<std::uint8_t> b(H + PAYLOAD);
  const char *m = "R2A1BRIR";
  std::copy(m, m + 8, b.begin());
  u32(b, 8, 1);
  u32(b, 12, 520);
  u32(b, 16, 48000);
  u32(b, 20, 22);
  u32(b, 24, 2);
  u32(b, 28, 16384);
  u32(b, 32, 0);
  u32(b, 36, 32);
  for (size_t i = 0; i < 32; ++i)
    b[40 + i] = uint8_t(0xa0 + i);
  for (size_t i = 0; i < 22; ++i)
    u32(b, 72 + 4 * i, 21U - uint32_t(i));
  u32(b, 160, 1);
  u32(b, 164, 0);
  for (size_t e = 0; e < 22; ++e) {
    f64(b, 168 + 8 * e, 121 + e);
    f64(b, 168 + 8 * 22 + 8 * e, 200 + e);
  }
  for (size_t ear = 0; ear < 2; ++ear)
    for (size_t e = 0; e < 22; ++e)
      for (size_t s = 0; s < 16384; ++s)
        f32(b, H + ((ear * 22 + e) * 16384 + s) * 4,
            float(1000000 * ear + 1000 * e + s));
  return b;
}
bool empty(const eac3render::SofaBrirCacheLoadResult &r) {
  bool zeroHash =
      std::all_of(r.cache.sourceSha256.begin(), r.cache.sourceSha256.end(),
                  [](std::uint8_t x) { return x == 0; });
  bool zeroMap = std::all_of(r.cache.systemToSofaEmitter.begin(),
                             r.cache.systemToSofaEmitter.end(),
                             [](std::uint32_t x) { return x == 0; });
  bool zeroDelay = true;
  for (auto const &ear : r.cache.delays)
    for (double x : ear)
      zeroDelay = zeroDelay && x == 0.0;
  return r.cache.sampleRate == 0 && r.cache.listenerViewIndex == -1 &&
         r.cache.leftReceiverIndex == 0 && r.cache.rightReceiverIndex == 0 &&
         r.cache.ir[0].empty() && r.cache.ir[1].empty() && zeroHash &&
         zeroMap && zeroDelay;
}
void bad(Report *r, const std::vector<std::uint8_t> &base,
         const std::string &dir, const char *n, size_t off, uint8_t v) {
  auto b = base;
  b[off] = v;
  auto p = dir + "/" + n;
  writeFile(p, b);
  auto x = eac3render::loadSofaBrirCache(p);
  expect(r, !x.accepted && empty(x), n);
}
} // namespace
int main(int argc, char **argv) {
  if (argc < 2) {
    std::cerr
        << "usage: probe --self-test [work-dir] | cache-path [work-dir]\n";
    return 2;
  }
  const bool self = std::string(argv[1]) == "--self-test";
  const std::string dir = argc > 2 ? argv[2] : "tmp/r2a-cache-tests";
  std::filesystem::create_directories(dir);
  const std::string source = self ? dir + "/r2a-synthetic.cache" : argv[1];
  const auto base = self ? synthetic() : readFile(source);
  if (base.size() != H + PAYLOAD) {
    std::cerr << "cache size invalid\n";
    return 2;
  }
  if (self)
    writeFile(source, base);
  Report r;
  const std::string syntheticExpected =
      "a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebf";
  const std::string realExpected =
      "09dc3414a5eb7d9a325e0ad750da87ce63c9d4baf270c980296c726e152c89fa";
  auto valid = eac3render::loadSofaBrirCache(source, self ? syntheticExpected
                                                          : realExpected);
  expect(&r,
         valid.accepted && valid.cache.sampleRate == 48000U &&
             valid.cache.leftReceiverIndex == (self ? 1U : 0U) &&
             valid.cache.rightReceiverIndex == (self ? 0U : 1U),
         self ? "valid-synthetic-cache" : "valid-real-cache");
  if (!valid.accepted)
    return 1;
  bool exact = true;
  for (std::size_t i = 0; i < 22U; ++i)
    exact = exact &&
            (self ? valid.cache.systemToSofaEmitter[i] == 21U - i
                  : valid.cache.systemToSofaEmitter[i] == i) &&
            (self ? valid.cache.delays[0][i] == 121.0 + i
                  : valid.cache.delays[0][i] == 0.0) &&
            (self ? valid.cache.delays[1][i] == 200.0 + i
                  : valid.cache.delays[1][i] == 0.0);
  for (auto const &ear : valid.cache.ir)
    exact = exact && ear.size() == 22U * 16384U &&
            std::all_of(ear.begin(), ear.end(),
                        [](float x) { return std::isfinite(x); });
  expect(&r, exact, "complete-fields-finite");
  expect(&r,
         self ? (valid.cache.systemToSofaEmitter[0] == 21U &&
                 valid.cache.delays[0][0] == 121.0)
              : (valid.cache.systemToSofaEmitter[0] == 0U &&
                 valid.cache.delays[0][0] == 0.0),
         "mapping-delay-provenance");
  expect(&r,
         self ? (valid.cache.ir[0][0] == 0.0F &&
                 valid.cache.ir[1].back() == 1037383.0F)
              : (std::isfinite(valid.cache.ir[0][0]) &&
                 std::isfinite(valid.cache.ir[1].back())),
         "payload-canonical-concrete");
  auto shuffled = base;
  std::swap_ranges(shuffled.begin() + 72, shuffled.begin() + 76,
                   shuffled.begin() + 76);
  auto sp = dir + "/r2a-shuffled.cache";
  writeFile(sp, shuffled);
  auto sr = eac3render::loadSofaBrirCache(sp);
  expect(&r,
         sr.accepted && sr.cache.systemToSofaEmitter[0] == (self ? 20U : 1U),
         "mapping-shuffled");
  auto ears = base;
  const size_t eb = 22U * 16384U * 4U;
  for (size_t i = 0; i < eb; ++i)
    std::swap(ears[H + i], ears[H + eb + i]);
  auto ep = dir + "/r2a-ear-swapped.cache";
  writeFile(ep, ears);
  auto er = eac3render::loadSofaBrirCache(ep);
  expect(&r,
         er.accepted &&
             er.cache.ir[0][0] == (self ? 1000000.0F : valid.cache.ir[1][0]) &&
             er.cache.ir[1].back() ==
                 (self ? 37383.0F : valid.cache.ir[0].back()),
         "payload-order-is-cache-contract");
  bad(&r, base, dir, "r2a-bad-magic.cache", 0, 'X');
  bad(&r, base, dir, "r2a-bad-version.cache", 8, 2);
  bad(&r, base, dir, "r2a-bad-count.cache", 24, 1);
  auto dup = base;
  dup[72] = dup[76];
  dup[73] = dup[77];
  dup[74] = dup[78];
  dup[75] = dup[79];
  auto dp = dir + "/r2a-duplicate.cache";
  writeFile(dp, dup);
  auto dr = eac3render::loadSofaBrirCache(dp);
  expect(&r, !dr.accepted && empty(dr), "duplicate-mapping");
  auto trunc = base;
  trunc.pop_back();
  auto tp = dir + "/r2a-truncated.cache";
  writeFile(tp, trunc);
  auto tr = eac3render::loadSofaBrirCache(tp);
  expect(&r, !tr.accepted && empty(tr), "truncated-cache");
  auto trail = base;
  trail.push_back(0);
  auto tlp = dir + "/r2a-trailing.cache";
  writeFile(tlp, trail);
  auto tl = eac3render::loadSofaBrirCache(tlp);
  expect(&r, !tl.accepted && empty(tl), "trailing-cache");
  bad(&r, base, dir, "r2a-out-of-range.cache", 72, 22);
  auto nd = base;
  f64(nd, 168, std::numeric_limits<double>::quiet_NaN());
  auto ndp = dir + "/r2a-nonfinite-delay.cache";
  writeFile(ndp, nd);
  auto ndr = eac3render::loadSofaBrirCache(ndp);
  expect(&r, !ndr.accepted && empty(ndr), "nonfinite-delay");
  auto ni = base;
  f32(ni, H, std::numeric_limits<float>::quiet_NaN());
  auto nip = dir + "/r2a-nonfinite.cache";
  writeFile(nip, ni);
  auto nir = eac3render::loadSofaBrirCache(nip);
  expect(&r, !nir.accepted && empty(nir), "nonfinite-ir");
  auto wrongHash = eac3render::loadSofaBrirCache(source, std::string(64, '0'));
  expect(&r, !wrongHash.accepted && empty(wrongHash),
         "wrong-expected-source-hash");
  auto badHash = eac3render::loadSofaBrirCache(source, "bad");
  expect(&r, !badHash.accepted && empty(badHash), "bad-expected-hash-length");
  std::cout << "SofaBrirCacheSelfTest=" << (r.failures ? "FAIL" : "PASS")
            << " cases=" << r.cases << " failures=" << r.failures << '\n';
  return r.failures ? 1 : 0;
}
