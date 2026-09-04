#include "brir-convolver.h"
#include <array>
#include <cmath>
#include <complex>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

namespace {
struct Report {
  int cases = 0;
  int failures = 0;
};
void expect(Report &r, bool ok, const char *name) {
  ++r.cases;
  std::cout << (ok ? "PASS " : "FAIL ") << name << '\n';
  if (!ok)
    ++r.failures;
}
eac3render::SofaBrirCache makeCache(std::size_t n = 8) {
  eac3render::SofaBrirCache c;
  c.sampleRate = 48000;
  c.leftReceiverIndex = 0;
  c.rightReceiverIndex = 1;
  for (std::size_t s = 0; s < 22; ++s)
    c.systemToSofaEmitter[s] = static_cast<std::uint32_t>(s);
  for (auto &ear : c.ir)
    ear.assign(22 * n, 0.0F);
  for (std::size_t s = 0; s < 22; ++s)
    for (std::size_t k = 0; k < n; ++k) {
      c.ir[0][s * n + k] = float(s + 1) * float(k + 1);
      c.ir[1][s * n + k] = float(2 * s + 1) * float(k + 1);
    }
  return c;
}
std::array<std::vector<float>, 2>
run(eac3render::SofaBrirCache const &cache,
    std::array<std::vector<float>, 22> const &input,
    std::vector<std::size_t> splits) {
  eac3render::BrirConvolverStream stream;
  if (!stream.init(cache, 4, 8).accepted)
    return {};
  std::size_t offset = 0;
  for (auto count : splits) {
    std::array<const float *, 22> p{};
    for (std::size_t s = 0; s < 22; ++s)
      p[s] = input[s].data() + offset;
    if (!stream.append(p, count).accepted)
      return {};
    offset += count;
  }
  if (!stream.finish().accepted)
    return {};
  return stream.output();
}
std::array<std::vector<float>, 2>
oracle(eac3render::SofaBrirCache const &c,
       std::array<std::vector<float>, 22> const &in, std::size_t frames,
       std::size_t n) {
  std::array<std::vector<float>, 2> out;
  for (auto &v : out)
    v.assign(frames + n - 1, 0.0F);
  for (std::size_t e = 0; e < 2; ++e)
    for (std::size_t s = 0; s < 22; ++s)
      for (std::size_t i = 0; i < frames; ++i)
        for (std::size_t k = 0; k < n; ++k)
          out[e][i + k] += in[s][i] * c.ir[e][s * n + k];
  return out;
}
bool close(std::array<std::vector<float>, 2> const &a,
           std::array<std::vector<float>, 2> const &b) {
  if (a[0].size() != b[0].size())
    return false;
  for (std::size_t e = 0; e < 2; ++e)
    for (std::size_t i = 0; i < a[e].size(); ++i)
      if (std::abs(a[e][i] - b[e][i]) > 2e-3F)
        return false;
  return true;
}
} // namespace
int main(int argc, char **argv) {
  bool realCache = false;
  if (argc > 2 || (argc == 2 && std::string(argv[1]).rfind("--", 0) == 0 &&
                   std::string(argv[1]) != "--self-test")) {
    std::cerr << "usage: Eac3BrirConvolverProbe [--self-test|cache-path]\n";
    return 2;
  }
  if (argc == 2 && std::string(argv[1]) != "--self-test")
    realCache = true;
  Report r;
  try {
    eac3render::Radix2Fft bad(3);
    (void)bad;
    expect(r, false, "fft-invalid-size");
  } catch (...) {
    expect(r, true, "fft-invalid-size");
  }
  eac3render::Radix2Fft fft(8);
  std::array<std::complex<float>, 8> x{};
  for (std::size_t i = 0; i < 8; ++i)
    x[i] = float(i + 1);
  auto original = x;
  fft.forward(x.data());
  bool dft = true;
  for (std::size_t k = 0; k < 8; ++k) {
    std::complex<float> expectedDft{};
    for (std::size_t n = 0; n < 8; ++n)
      expectedDft +=
          original[n] * std::polar(1.0F, -2.0F * 3.14159265358979323846F *
                                             float(k * n) / 8.0F);
    dft &= std::abs(x[k] - expectedDft) < 1e-3F;
  }
  expect(r, dft, "fft-direct-dft");
  fft.inverse(x.data());
  bool round = true;
  for (size_t i = 0; i < 8; ++i)
    round &= std::abs(x[i].real() - original[i].real()) < 1e-4f;
  expect(r, round, "fft-roundtrip");
  auto strideCache = makeCache(12);
  eac3render::BrirConvolver strideConvolver;
  auto strideResult = strideConvolver.init(strideCache, 4, 8);
  expect(r, strideResult.accepted, "cache-stride-truncation");
  auto c = makeCache();
  eac3render::BrirConvolverStream stream;
  auto init = stream.init(c, 4, 8);
  expect(r, init.accepted, "short-config");
  std::array<std::vector<float>, 22> in;
  std::array<const float *, 22> p{};
  for (auto &v : in)
    v.assign(8, 0.0F);
  in[0][0] = 1.0F;
  for (size_t s = 0; s < 22; ++s)
    p[s] = in[s].data();
  auto a = stream.append(p, 3);
  auto p2 = p;
  for (std::size_t s = 0; s < 22; ++s)
    p2[s] = in[s].data() + 3;
  auto b = stream.append(p2, 5);
  auto f = stream.finish();
  bool tail = f.accepted && a.accepted && b.accepted &&
              stream.output()[0].size() == 15 &&
              std::abs(stream.output()[0][0] - 1) < 1e-3f &&
              std::abs(stream.output()[0][7] - 8) < 1e-3f;
  expect(r, tail, "impulse-tail-exact");
  auto out1 = stream.output();
  stream.reset();
  auto again = stream.append(p, 8);
  auto fin = stream.finish();
  bool deterministic =
      again.accepted && fin.accepted && out1 == stream.output();
  expect(r, deterministic, "reset-determinism");
  std::array<std::vector<float>, 22> signal;
  for (auto &v : signal)
    v.assign(8, 0.0F);
  signal[0][0] = 1.0F;
  signal[0][1] = -0.5F;
  auto expected = oracle(makeCache(), signal, 8, 8);
  expect(r, close(run(makeCache(), signal, {8}), expected),
         "direct-oracle-distinct-ears");
  expect(r, close(run(makeCache(), signal, {1, 2, 3, 2}), expected),
         "split-pattern-1");
  expect(r, close(run(makeCache(), signal, {5, 1, 2}), expected),
         "split-pattern-2");
  expect(r, close(run(makeCache(), signal, {2, 2, 1, 3}), expected),
         "split-pattern-3");
  std::array<std::vector<float>, 22> silence;
  for (auto &v : silence)
    v.assign(8, 0.0F);
  auto silent = run(makeCache(), silence, {3, 5});
  bool allZero = true;
  for (auto const &v : silent)
    for (float x : v)
      allZero &= x == 0.0F;
  expect(r, allZero, "silence");
  std::array<std::vector<float>, 22> all;
  for (auto &v : all)
    v.assign(8, 0.0F);
  for (auto &v : all)
    v[0] = 1.0F;
  auto summed = run(makeCache(), all, {8});
  bool sumOk = summed[0][0] == 253.0F && summed[1][0] == 484.0F;
  expect(r, sumOk, "22-speaker-linear-sum");
  eac3render::BrirConvolverStream partial;
  partial.init(makeCache(), 4, 8);
  partial.append(p, 2);
  partial.reset();
  auto clean = partial.append(p, 8);
  auto cleanFinish = partial.finish();
  auto expectedReset = oracle(makeCache(), in, 8, 8);
  bool resetClean = clean.accepted && cleanFinish.accepted &&
                    close(partial.output(), expectedReset);
  expect(r, resetClean, "reset-clears-partial-pending");
  auto before = partial.output();
  auto twice = partial.finish();
  expect(r, twice.accepted && partial.output() == before, "finish-idempotent");
  c.delays[0][0] = 0.5;
  eac3render::BrirConvolver reject;
  auto badDelay = reject.init(c, 4, 8);
  expect(r,
         !badDelay.accepted && badDelay.reason == "nonzero-delay-pending-r2b2",
         "nonzero-delay-reject");
  c = makeCache();
  eac3render::BrirConvolver nonfinite;
  auto ok = nonfinite.init(c, 4, 8);
  std::array<float, 4> nanInput{1, 2, NAN, 4};
  std::array<const float *, 22> np{};
  np[0] = nanInput.data();
  std::array<float, 4> no{}, ne{};
  auto nr = nonfinite.processFixedBlock(np, {no.data(), ne.data()});
  expect(r, ok.accepted && !nr.accepted && nr.reason == "nonfinite-input",
         "nonfinite-input-transaction");
  if (realCache) {
    auto loaded = eac3render::loadSofaBrirCache(
        argv[1],
        "09dc3414a5eb7d9a325e0ad750da87ce63c9d4baf270c980296c726e152c89fa");
    expect(r, loaded.accepted, "real-cache-load");
    if (loaded.accepted)
      for (std::size_t speaker : {2U, 8U}) {
        std::array<std::vector<float>, 22> realIn;
        for (auto &v : realIn)
          v.assign(1, 0.0F);
        realIn[speaker][0] = 1.0F;
        std::array<const float *, 22> realPointers{};
        for (std::size_t s = 0; s < 22; ++s)
          realPointers[s] = realIn[s].data();
        eac3render::BrirConvolverStream realStream;
        auto ri = realStream.init(loaded.cache);
        auto ra = realStream.append(realPointers, 1);
        auto rf = realStream.finish();
        bool same = ri.accepted && ra.accepted && rf.accepted &&
                    realStream.output()[0].size() == 16384;
        if (same)
          for (std::size_t k = 0; k < 16384; ++k)
            same &= std::abs(realStream.output()[0][k] -
                             loaded.cache.ir[0][speaker * 16384 + k]) < 1e-5F &&
                    std::abs(realStream.output()[1][k] -
                             loaded.cache.ir[1][speaker * 16384 + k]) < 1e-5F;
        expect(r, same,
               speaker == 2 ? "real-cache-M+000-impulse"
                            : "real-cache-M+090-impulse");
      }
  }
  std::cout << "BrirConvolverSelfTest=" << (r.failures ? "FAIL" : "PASS")
            << " cases=" << r.cases << " failures=" << r.failures << '\n';
  return r.failures ? 1 : 0;
}
