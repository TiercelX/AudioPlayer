#pragma once

// Probe-local J0A7 prepared-scene contract.  PCM ownership and the
// SceneAdapter metadata/gain frame remain separate; this is not production.

#include "scene-adapter.h"
#include "scene-object-properties.h"
#include "../atmos-joc-probe/joc-gate6c.h"

#include <cstdint>
#include <array>
#include <functional>
#include <string>
#include <vector>

namespace config4scene {

struct PreparedObject {
    std::uint64_t generation = eac3render::kInitialGeneration;
    std::uint64_t objectId = 0U;
    std::int64_t timestampSamples = 0;
    std::int64_t durationSamples = 0;
    std::vector<float> samples;
    eac3render::ObjectMetadataUpdate sceneUpdate;
    eac3render::SceneObjectProperties sceneProperties;
    std::array<float, 3> roomPosition {0.5F, 0.5F, 0.0F};
};

struct PreparedLfe {
    std::uint64_t generation = eac3render::kInitialGeneration;
    std::int64_t timestampSamples = 0;
    std::int64_t durationSamples = 0;
    std::vector<float> samples;
};

struct PreparedBatch {
    std::size_t unitIndex = 0U;
    bool flush = false;
    std::int64_t outputStart = 0;
    std::int64_t outputEnd = 0;
    std::vector<PreparedObject> objects;
    PreparedLfe lfe;
    eac3render::SceneAdapterFrame sceneFrame;
    eac3render::SceneObjectCoordinatePolicy coordinatePolicy;
};

using PreparedCallback = std::function<bool(const PreparedBatch &)>;

bool decodeFile(const std::string &path, unsigned maxAUs,
                const PreparedCallback &callback, std::string *reason);

// Config-3 uses the existing native ordinary-core/JOC session bridge and then
// enters the same renderer-neutral prepared-scene contract as config-4.
bool decodeConfig3File(const std::string &path, unsigned maxAUs,
                       const PreparedCallback &callback, std::string *reason);

} // namespace config4scene
