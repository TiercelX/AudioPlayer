#pragma once

#include "native-eac3-config4-pcm.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace config4mapping {

enum class Disposition { Accepted, Unsupported, Malformed };

struct MappingInput {
    unsigned jocConfig = 4U;
    std::uint16_t dependentChanmap = 0xA010U;
    unsigned dependentChannelCount = 4U;
    unsigned auIndex = 0U;
    std::int64_t timestamp = 0;
    std::size_t baseAcceptedContainers = 0U;
    std::size_t baseTargetPayloads = 0U;
    std::size_t dependentAcceptedContainers = 1U;
    std::size_t dependentTargetPayloads = 2U;
    std::vector<std::string> dependentOrder = {
        "dependent.L", "dependent.R", "dependent.VHL", "dependent.VHR"};
};

struct MappedChannel {
    unsigned slot = 0U;
    std::string slotName;
    std::string provenance;
    bool lfeBypass = false;
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

struct MappingResult {
    Disposition disposition = Disposition::Malformed;
    std::string reason;
    unsigned auOrdinal = 0U;
    std::size_t sampleStart = 0U;
    unsigned jocConfig = 0U;
    unsigned jocChannels = 0U;
    std::uint16_t dependentChanmap = 0U;
    std::vector<MappedChannel> channels;
    MappedChannel lfeBypass;
    std::uint64_t digest = 0U;
};

using MappingCallback = std::function<bool(const MappingResult &)>;

MappingResult mapAssembly(const config4pcm::AssemblyResult &assembly,
                          const MappingInput &input, bool requireEosTail);

struct DecodeReport {
    Disposition disposition = Disposition::Malformed;
    std::size_t mappedAUs = 0U;
    std::string reason;
};

DecodeReport decodeFile(const std::string &path, const std::string &tablePath,
                        std::size_t maxAUs, const MappingCallback &callback);

bool selfTest();

} // namespace config4mapping
