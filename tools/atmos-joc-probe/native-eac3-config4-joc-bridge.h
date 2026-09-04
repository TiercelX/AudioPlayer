#pragma once

// Probe-local linked contract for the bounded J0A4 config-4 preflight.
// This is not a production decoder or JOC session API.

#include "native-eac3-joc-qualifier.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace config4joc {

enum class Disposition { Accepted, Blocked, Malformed, Canceled };

struct Association {
    unsigned auIndex = 0U;
    std::int64_t timestamp = 0;
    std::size_t baseOffset = 0U;
    std::size_t dependentOffset = 0U;
    std::size_t nextOffset = 0U;
    std::size_t payload11Bytes = 0U;
    std::size_t payload14Bytes = 0U;
    std::size_t payload11StartBit = 0U;
    std::size_t payload14StartBit = 0U;
    std::size_t baseAcceptedContainers = 0U;
    std::size_t baseTargetPayloads = 0U;
    std::size_t dependentAcceptedContainers = 0U;
    std::size_t dependentTargetPayloads = 0U;
    unsigned dependentChannelCount = 0U;
    std::uint16_t dependentChanmap = 0U;
    unsigned jocConfig = 0U;
    unsigned jocChannels = 0U;
    eac3native::NativeJocQualification qualification;
};

using AssociationCallback = std::function<bool(const Association &)>;

struct DecodeReport {
    Disposition disposition = Disposition::Malformed;
    std::string reason;
    unsigned observedAUs = 0U;
    std::size_t callbacks = 0U;
};

DecodeReport decodeFile(const std::string &path, const std::string &tablePath,
                        unsigned maxAUs, const AssociationCallback &callback);

} // namespace config4joc
