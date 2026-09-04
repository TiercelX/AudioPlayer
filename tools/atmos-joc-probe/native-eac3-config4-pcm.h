#pragma once

// Probe-local linked contract for the bounded N5J config-4 PCM assembly.
// This is not a production decoder or playback API.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace config4pcm {

enum class AssemblyDisposition { Accepted, Unsupported, Malformed, Canceled };

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

struct AssemblyResult {
    AssemblyDisposition disposition = AssemblyDisposition::Malformed;
    std::string reason;
    unsigned auOrdinal = 0U;
    std::size_t baseSampleStart = 0U;
    std::size_t dependentSampleStart = 0U;
    std::uint64_t digest = 0U;
    std::vector<ChannelSummary> channels;
};

using AssemblyCallback = std::function<bool(const AssemblyResult &)>;

struct DecodeReport {
    AssemblyDisposition disposition = AssemblyDisposition::Malformed;
    std::string reason;
    unsigned observedAUs = 0U;
    std::size_t callbacks = 0U;
};

// Decode a bounded number of adjacent N5J base/dependent AUs and hand the
// actual owned vectors to the callback. EOS is emitted only for the last AU.
DecodeReport decodeFile(const std::string &path, unsigned maxAUs,
                        const AssemblyCallback &callback);

} // namespace config4pcm
