#pragma once
#include "radix2-fft.h"
#include "sofa-brir-cache.h"
#include <array>
#include <complex>
#include <memory>
#include <string>
#include <vector>

namespace eac3render {
struct BrirConvolverResult {
  bool accepted = false;
  std::string reason;
};
class BrirConvolver {
public:
  BrirConvolver() = default;
  BrirConvolverResult init(const SofaBrirCache &cache, std::size_t block = 1024,
                           std::size_t irLength = kR2aBrirIrLength);
  BrirConvolverResult
  processFixedBlock(const std::array<const float *, 22> &input,
                    const std::array<float *, 2> &output);
  void reset() noexcept;
  std::size_t blockSize() const noexcept { return block_; }

private:
  std::size_t block_ = 0, fftSize_ = 0, partitions_ = 0, irLength_ = 0,
              head_ = 0;
  Radix2Fft *fft_ = nullptr;
  std::unique_ptr<Radix2Fft> ownedFft_;
  std::vector<std::complex<float>> x_, y_, h_;
  std::vector<std::complex<float>> current_, candidate_;
  std::vector<float> overlap_;
  std::vector<std::vector<std::complex<float>>> history_;
};

class BrirConvolverStream {
public:
  BrirConvolverResult init(const SofaBrirCache &cache, std::size_t block = 1024,
                           std::size_t irLength = kR2aBrirIrLength);
  BrirConvolverResult append(const std::array<const float *, 22> &input,
                             std::size_t frames);
  BrirConvolverResult finish();
  void reset() noexcept;
  const std::array<std::vector<float>, 2> &output() const noexcept {
    return output_;
  }

private:
  BrirConvolver core_;
  std::array<std::vector<float>, 22> pending_;
  std::size_t pendingFrames_ = 0;
  std::size_t inputFrames_ = 0;
  std::size_t irLength_ = 0;
  std::array<std::vector<float>, 2> output_;
  bool finished_ = false;
};
} // namespace eac3render
