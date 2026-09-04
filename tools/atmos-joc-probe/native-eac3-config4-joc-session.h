#pragma once

// Probe-local linked contract for the bounded J0A6 config-4 session output.
// This is not a production decoder or scene/session API.

#include "joc-gate6c.h"

#include <functional>
#include <string>

namespace config4session {

using BatchCallback = std::function<bool(const eac3gate6c::Batch &)>;

struct DecodeReport {
    bool accepted = false;
    unsigned batches = 0U;
    unsigned metadataBatches = 0U;
    std::string reason;
};

DecodeReport decodeFile(const std::string &path, unsigned maxAUs,
                        const BatchCallback &callback);

} // namespace config4session
