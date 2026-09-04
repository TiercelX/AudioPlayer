#pragma once

#include "scene-object-bridge.h"
#include "scene-object-ramp-scheduler.h"

#include <optional>

namespace eac3render {

struct SceneObjectGroupingResult {
    SceneAdapterDisposition disposition = SceneAdapterDisposition::InvalidInput;
    std::string reason;
    std::size_t failedRecordIndex = 0U;
    std::size_t failedGroupIndex = 0U;
    std::vector<SceneObjectTargetGroup> groups;
};

class SceneObjectStreamGrouper final {
public:
    SceneObjectGroupingResult append(
        const std::vector<eac3gate6c::MetadataUpdate> &metadata,
        const SceneObjectBatchContext &context);
    SceneObjectRampResult reset(std::uint64_t generation);
    std::uint64_t generation() const { return generation_; }
    std::int64_t lastTimestamp() const { return lastTimestamp_; }

private:
    std::uint64_t generation_ = kInitialGeneration;
    std::int64_t lastTimestamp_ = -1;
    std::optional<SceneObjectCoordinatePolicy> policy_;
};

struct SceneObjectCausalFeedResult {
    SceneAdapterDisposition disposition = SceneAdapterDisposition::InvalidInput;
    std::string reason;
    bool applied = false;
};

// Applies at most one next group. A future group is left untouched.
SceneObjectCausalFeedResult applyNextCausalGroup(
    const std::vector<SceneObjectTargetGroup> &groups,
    std::size_t *nextGroupIndex, std::int64_t currentSample,
    SceneObjectRampScheduler *scheduler);

} // namespace eac3render
