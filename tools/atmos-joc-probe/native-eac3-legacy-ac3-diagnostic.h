#pragma once

// Probe-local contract for the bounded Legacy AC-3 diagnostic decoder.
// This is not a production decoder API.

#include "native-eac3-mantissas.h"
#include "native-eac3-transform.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace legacyac3 {

enum class Disposition { Accepted, Unsupported, Malformed };

struct ChannelBoundary {
    unsigned channel = 0U;
    bool lfe = false;
    bool exponentReused = false;
    bool bandwidthReused = false;
    std::size_t coefficientStartBit = 0U;
    std::size_t coefficientEndBit = 0U;
    std::vector<double> coefficients;
    std::uint64_t coefficientDigest = 0U;
};

struct BlockBoundary {
    unsigned block = 0U;
    std::size_t startBit = 0U;
    std::size_t endBit = 0U;
    unsigned blockSwitchMask = 0U;
    unsigned ditherMask = 0U;
    bool dynrngPresent = false;
    bool rematrix = false;
    bool coupling = false;
    std::vector<ChannelBoundary> channels;
};

struct PcmSummary {
    unsigned channel = 0U;
    bool lfe = false;
    std::size_t sampleCount = 0U;
    std::size_t eosTailCount = 0U;
    std::uint64_t digest = 0U;
    std::uint64_t eosDigest = 0U;
    double peak = 0.0;
    double rms = 0.0;
    double eosPeak = 0.0;
    double eosRms = 0.0;
    std::vector<double> samples;
    std::vector<double> eosTail;
};

struct Result {
    Disposition disposition = Disposition::Malformed;
    std::string reason;
    std::size_t bitPosition = 0U;
    std::uint64_t stateDigest = 0U;
    std::size_t coefficientCount = 0U;
    std::vector<PcmSummary> pcm;
    std::vector<BlockBoundary> blocks;
};

// Probe-local owner for cross-frame dither and IMDCT overlap. Parser syntax
// state remains frame-local; callers must explicitly reset for a new stream.
struct StatefulDecoder {
    std::array<eac3native::ReferenceDitherSource, 6U> ditherSources{};
    std::array<eac3native::Eac3TransformChannel, 6U> transformChannels{};

    void reset()
    {
        for (auto &source : ditherSources) source.reset();
        for (auto &transform : transformChannels) transform.reset();
    }
};

Result parse(const std::vector<std::uint8_t> &bytes);
Result parse(const std::vector<std::uint8_t> &bytes, StatefulDecoder &state);
Result parseEnd(const std::vector<std::uint8_t> &bytes,
                StatefulDecoder &state);

} // namespace legacyac3
