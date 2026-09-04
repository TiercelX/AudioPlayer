#include "brir-convolver.h"
#include <cmath>
#include <memory>

namespace eac3render {
BrirConvolverResult BrirConvolver::init(const SofaBrirCache &c,
                                        std::size_t block,
                                        std::size_t irLength) {
  BrirConvolverResult r;
  const std::size_t cacheStride = c.ir[0].size() / 22U;
  if (block == 0 || (block & (block - 1)) != 0 || irLength == 0 ||
      c.sampleRate != 48000U || c.leftReceiverIndex >= 2 ||
      c.rightReceiverIndex >= 2 ||
      c.leftReceiverIndex == c.rightReceiverIndex ||
      c.ir[0].size() != c.ir[1].size() || c.ir[0].size() % 22U != 0 ||
      irLength > cacheStride) {
    r.reason = "invalid-convolver-shape";
    return r;
  }
  for (auto const &e : c.delays)
    for (double d : e)
      if (!std::isfinite(d) || d != 0) {
        r.reason = "nonzero-delay-pending-r2b2";
        return r;
      }
  std::array<bool, 22> seen{};
  for (auto mapping : c.systemToSofaEmitter) {
    if (mapping >= 22U || seen[mapping]) {
      r.reason = "invalid-emitter-mapping";
      return r;
    }
    seen[mapping] = true;
  }
  for (std::size_t ear = 0; ear < 2; ++ear)
    for (std::size_t speaker = 0; speaker < 22; ++speaker)
      for (std::size_t k = 0; k < irLength; ++k)
        if (!std::isfinite(c.ir[ear][speaker * cacheStride + k])) {
          r.reason = "nonfinite-ir";
          return r;
        }
  block_ = block;
  irLength_ = irLength;
  fftSize_ = block * 2;
  partitions_ = (irLength + block - 1) / block;
  ownedFft_ = std::make_unique<Radix2Fft>(fftSize_);
  fft_ = ownedFft_.get();
  x_.assign(fftSize_, {});
  y_.assign(2 * fftSize_, {});
  h_.assign(2 * 22 * partitions_ * fftSize_, {});
  current_.assign(22 * fftSize_, {});
  candidate_.assign(2 * fftSize_, {});
  history_.assign(22 * partitions_, std::vector<std::complex<float>>(fftSize_));
  overlap_.assign(2 * block, 0.0F);
  for (size_t ear = 0; ear < 2; ++ear)
    for (size_t s = 0; s < 22; ++s)
      for (size_t p = 0; p < partitions_; ++p) {
        for (size_t i = 0; i < block; ++i) {
          size_t k = p * block + i;
          if (k < irLength_)
            x_[i] = {c.ir[ear][s * cacheStride + k], 0.0F};
          else
            x_[i] = {0.0F, 0.0F};
        }
        std::fill(x_.begin() + block, x_.end(), std::complex<float>{});
        fft_->forward(x_.data());
        for (size_t k = 0; k < fftSize_; ++k)
          h_[((ear * 22 + s) * partitions_ + p) * fftSize_ + k] = x_[k];
      }
  r.accepted = true;
  return r;
}
BrirConvolverResult
BrirConvolver::processFixedBlock(const std::array<const float *, 22> &in,
                                 const std::array<float *, 2> &out) {
  BrirConvolverResult r;
  if (!fft_) {
    r.reason = "not-initialized";
    return r;
  }
  for (auto p : out)
    if (!p) {
      r.reason = "null-output";
      return r;
    }
  for (auto p : in)
    if (p)
      for (size_t i = 0; i < block_; ++i)
        if (!std::isfinite(p[i])) {
          r.reason = "nonfinite-input";
          return r;
        }
  std::fill(y_.begin(), y_.end(), std::complex<float>{});
  for (size_t s = 0; s < 22; ++s) {
    for (size_t i = 0; i < block_; ++i)
      x_[i] = {in[s] ? in[s][i] : 0, 0};
    std::fill(x_.begin() + block_, x_.end(), std::complex<float>{});
    fft_->forward(x_.data());
    std::copy(x_.begin(), x_.end(), current_.begin() + s * fftSize_);
  }
  for (size_t s = 0; s < 22; ++s) {
    for (size_t ear = 0; ear < 2; ++ear)
      for (size_t p = 0; p < partitions_; ++p) {
        size_t hp = (head_ + partitions_ - p) % partitions_;
        for (size_t k = 0; k < fftSize_; ++k)
          y_[ear * fftSize_ + k] +=
              (p == 0 ? current_[s * fftSize_ + k]
                      : history_[s * partitions_ + hp][k]) *
              h_[((ear * 22 + s) * partitions_ + p) * fftSize_ + k];
      }
  }
  for (size_t ear = 0; ear < 2; ++ear) {
    std::copy(y_.begin() + ear * fftSize_, y_.begin() + (ear + 1) * fftSize_,
              x_.begin());
    fft_->inverse(x_.data());
    for (size_t i = 0; i < block_; ++i) {
      float v = x_[i].real() + overlap_[ear * block_ + i];
      float next = x_[i + block_].real();
      if (!std::isfinite(v) || !std::isfinite(next)) {
        r.reason = "nonfinite-output";
        return r;
      }
      candidate_[ear * fftSize_ + i] = {v, next};
    }
  }
  for (size_t s = 0; s < 22; ++s)
    std::copy(current_.begin() + s * fftSize_,
              current_.begin() + (s + 1) * fftSize_,
              history_[s * partitions_ + head_].begin());
  for (size_t ear = 0; ear < 2; ++ear)
    for (size_t i = 0; i < block_; ++i) {
      out[ear][i] = candidate_[ear * fftSize_ + i].real();
      overlap_[ear * block_ + i] = candidate_[ear * fftSize_ + i].imag();
    }
  head_ = (head_ + 1) % partitions_;
  r.accepted = true;
  return r;
}
void BrirConvolver::reset() noexcept {
  head_ = 0;
  std::fill(overlap_.begin(), overlap_.end(), 0.0F);
  for (auto &v : history_)
    std::fill(v.begin(), v.end(), std::complex<float>{});
}

BrirConvolverResult BrirConvolverStream::init(const SofaBrirCache &cache,
                                              std::size_t block,
                                              std::size_t irLength) {
  auto result = core_.init(cache, block, irLength);
  if (!result.accepted)
    return result;
  irLength_ = irLength;
  pendingFrames_ = inputFrames_ = 0;
  for (auto &v : pending_)
    v.assign(block, 0.0F);
  for (auto &v : output_)
    v.clear();
  finished_ = false;
  return result;
}

BrirConvolverResult
BrirConvolverStream::append(const std::array<const float *, 22> &input,
                            std::size_t frames) {
  BrirConvolverResult result;
  const std::size_t block = core_.blockSize();
  if (!block) {
    result.reason = "not-initialized";
    return result;
  }
  if (finished_ && frames != 0) {
    result.reason = "append-after-finish";
    return result;
  }
  for (std::size_t i = 0; i < frames; ++i)
    for (auto p : input)
      if (p && !std::isfinite(p[i])) {
        result.reason = "nonfinite-input";
        return result;
      }
  for (std::size_t i = 0; i < frames; ++i) {
    for (std::size_t s = 0; s < 22; ++s) {
      const float value = input[s] ? input[s][i] : 0.0F;
      pending_[s][pendingFrames_] = value;
    }
    ++pendingFrames_;
    ++inputFrames_;
    if (pendingFrames_ != block)
      continue;
    std::array<const float *, 22> pointers{};
    for (std::size_t s = 0; s < 22; ++s)
      pointers[s] = pending_[s].data();
    std::array<std::vector<float>, 2> blockOutput;
    std::array<float *, 2> outputs{};
    for (std::size_t e = 0; e < 2; ++e) {
      blockOutput[e].resize(block);
      outputs[e] = blockOutput[e].data();
    }
    result = core_.processFixedBlock(pointers, outputs);
    if (!result.accepted)
      return result;
    for (std::size_t e = 0; e < 2; ++e)
      output_[e].insert(output_[e].end(), blockOutput[e].begin(),
                        blockOutput[e].end());
    pendingFrames_ = 0;
  }
  result.accepted = true;
  return result;
}

BrirConvolverResult BrirConvolverStream::finish() {
  BrirConvolverResult result;
  const std::size_t block = core_.blockSize();
  if (!block) {
    result.reason = "not-initialized";
    return result;
  }
  if (finished_) {
    result.accepted = true;
    return result;
  }
  if (inputFrames_ == 0 && pendingFrames_ == 0) {
    finished_ = true;
    result.accepted = true;
    return result;
  }
  while (output_[0].size() < inputFrames_ + irLength_ - 1) {
    std::array<const float *, 22> pointers{};
    std::array<std::vector<float>, 2> blockOutput;
    std::array<float *, 2> outputs{};
    for (std::size_t e = 0; e < 2; ++e) {
      blockOutput[e].resize(block);
      outputs[e] = blockOutput[e].data();
    }
    if (pendingFrames_) {
      for (std::size_t s = 0; s < 22; ++s)
        std::fill(pending_[s].begin() + pendingFrames_, pending_[s].end(),
                  0.0F);
      for (std::size_t s = 0; s < 22; ++s)
        pointers[s] = pending_[s].data();
    }
    result = core_.processFixedBlock(pointers, outputs);
    if (!result.accepted)
      return result;
    for (std::size_t e = 0; e < 2; ++e)
      output_[e].insert(output_[e].end(), blockOutput[e].begin(),
                        blockOutput[e].end());
    pendingFrames_ = 0;
  }
  const std::size_t wanted = inputFrames_ + irLength_ - 1;
  for (auto &v : output_)
    v.resize(wanted);
  finished_ = true;
  result.accepted = true;
  return result;
}

void BrirConvolverStream::reset() noexcept {
  core_.reset();
  pendingFrames_ = inputFrames_ = 0;
  for (auto &v : pending_)
    std::fill(v.begin(), v.end(), 0.0F);
  for (auto &v : output_)
    v.clear();
  finished_ = false;
}
} // namespace eac3render
