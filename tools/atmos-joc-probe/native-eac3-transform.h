#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace eac3native {

// Standalone scalar reference for ETSI TS 102 366 V1.4.1 §6.9.4.
//
// One input block always contains up to 256 frequency coefficients. Missing
// coefficients are zero-filled. A long block uses one 512-sample inverse
// transform; a switched block uses two 256-sample inverse transforms whose
// 128-coefficient results are interleaved in the transmitted vector. Both
// paths produce a 512-sample windowed block, then emit 256 overlap-added
// samples and retain 256 samples for the next block.
constexpr std::size_t kEac3TransformCoefficients = 256;
constexpr std::size_t kEac3TransformSamples = 512;
constexpr std::size_t kEac3OutputSamples = 256;
// TS §6.9.4.1 conceptually retains x[N/2+n], n=0..255, until the next block
// or EOS. This reference keeps that full 256-sample overlap explicitly;
// optimized decoders may pack the symmetric transform into 128 samples, but
// that implementation detail is not this API's state contract.
constexpr std::size_t kEac3InherentDelaySamples = kEac3OutputSamples;

struct TransformOutput {
    bool ok = false;
    bool blockSwitch = false;
    std::vector<double> samples;
    std::string error;
};

struct TransformFlushOutput {
    bool ok = false;
    bool hadTail = false;
    std::vector<double> samples;
    std::string error;
};

class Eac3TransformChannel {
public:
    Eac3TransformChannel();

    // Decode one ordinary channel block. This class deliberately has no
    // coefficient parser, gain, limiter, DRC, rematrix, or renderer policy.
    TransformOutput processBlock(const std::vector<double>& coefficients,
                                 bool blockSwitch);

    // Emit the retained overlap at EOS exactly once. A channel with no
    // processed block has no tail. Calling flush again is an empty EOS.
    TransformFlushOutput flush();

    // Clear overlap, block count, and EOS state.
    void reset();

    bool hasPendingOverlap() const { return hasOverlap_; }
    std::size_t blocksProcessed() const { return blocksProcessed_; }

private:
    std::vector<double> inverseLong(const std::vector<double>& coefficients) const;
    std::vector<double> inverseSwitched(const std::vector<double>& coefficients) const;
    TransformOutput finishBlock(const std::vector<double>& windowed,
                                bool blockSwitch);

    std::vector<double> overlap_;
    std::size_t blocksProcessed_ = 0;
    bool hasOverlap_ = false;
    bool flushed_ = false;
};

} // namespace eac3native
