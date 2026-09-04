#pragma once

#include "scene-object-properties.h"

#include <vector>

namespace eac3render {

struct SceneObjectBatchContext {
    SceneObjectPropertyContext property;
    SceneObjectCoordinatePolicy coordinatePolicy;
};

struct SceneObjectTargetGroup {
    std::uint64_t generation = kInitialGeneration;
    std::int64_t timestampSamples = 0;
    std::int64_t durationSamples = 0;
    SceneObjectCoordinatePolicy coordinatePolicy;
    struct Target {
        ObjectMetadataUpdate update;
        std::array<float, 3> roomPosition {0.5F, 0.5F, 0.0F};
    };
    std::vector<Target> objects;
};

struct SceneObjectBatchResult {
    SceneAdapterDisposition disposition = SceneAdapterDisposition::InvalidInput;
    std::string reason;
    std::vector<SceneObjectProperties> properties;
    SceneObjectTargetGroup targetGroup;
};

// Adapts one co-timed metadata group (one SceneAdapter timestamp/ramp), not
// an arbitrary multi-block Gate6C stream.
SceneObjectBatchResult adaptGate6cBatch(
    const std::vector<eac3gate6c::MetadataUpdate> &metadata,
    const SceneObjectBatchContext &context);

} // namespace eac3render
