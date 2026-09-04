#pragma once

// Probe-local contract for the bounded dependent E-AC-3 diagnostic decoder.
// This is not a production decoder API.

#include "native-eac3-bsi.h"
#include "native-eac3-core.h"
#include "native-eac3-mantissas.h"
#include "native-eac3-transform.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dependentpcm {

constexpr unsigned kDiagnosticChannels = 4U;
inline constexpr std::array<const char *, kDiagnosticChannels> kLocations = {
    "L", "R", "VHL", "VHR"};

enum class Disposition { Accepted, Unsupported, Malformed, Canceled };

struct ChannelSummary {
    unsigned channel = 0U;
    const char *location = "";
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

struct PcmResult {
    Disposition disposition = Disposition::Malformed;
    std::string reason;
    std::size_t frameOffset = 0U;
    std::size_t bitPosition = 0U;
    std::size_t baseSampleStart = 0U;
    std::size_t dependentSampleStart = 0U;
    std::uint64_t stateDigest = 0U;
    std::vector<ChannelSummary> channels;
};

// Probe-local owner for cross-frame dither and IMDCT overlap. Parser syntax
// state remains frame-local; callers must explicitly reset for a new stream.
struct StatefulDecoder {
    std::array<eac3native::ReferenceDitherSource, kDiagnosticChannels>
        ditherSources{};
    std::array<eac3native::Eac3TransformChannel, kDiagnosticChannels>
        transformChannels{};

    void reset()
    {
        for (auto &source : ditherSources) source.reset();
        for (auto &transform : transformChannels) transform.reset();
    }
};

PcmResult decodeDependent(const std::vector<std::uint8_t> &bytes,
                          const eac3native::FrameHeader &frame,
                          const eac3native::BsiInfo &bsi);
PcmResult decodeDependent(const std::vector<std::uint8_t> &bytes,
                          const eac3native::FrameHeader &frame,
                          const eac3native::BsiInfo &bsi,
                          StatefulDecoder &state);
PcmResult decodeDependentEnd(const std::vector<std::uint8_t> &bytes,
                             const eac3native::FrameHeader &frame,
                             const eac3native::BsiInfo &bsi,
                             StatefulDecoder &state);

} // namespace dependentpcm
