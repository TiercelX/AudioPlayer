#include "scene-object-stream-grouper.h"

namespace eac3render {
namespace {

SceneObjectGroupingResult failure(SceneAdapterDisposition disposition,
                                  const char *reason, std::size_t record,
                                  std::size_t group)
{
    SceneObjectGroupingResult result;
    result.disposition = disposition;
    result.reason = reason;
    result.failedRecordIndex = record;
    result.failedGroupIndex = group;
    return result;
}

bool samePolicy(const SceneObjectCoordinatePolicy &a,
                const SceneObjectCoordinatePolicy &b)
{
    return a.roomWidthMetres == b.roomWidthMetres
        && a.roomDepthMetres == b.roomDepthMetres
        && a.roomHeightMetres == b.roomHeightMetres
        && a.listenerX == b.listenerX && a.listenerY == b.listenerY
        && a.listenerZ == b.listenerZ;
}

} // namespace

SceneObjectGroupingResult SceneObjectStreamGrouper::append(
    const std::vector<eac3gate6c::MetadataUpdate> &metadata,
    const SceneObjectBatchContext &context)
{
    if (metadata.empty())
        return failure(SceneAdapterDisposition::InvalidInput,
                       "empty-metadata-input", 0U, 0U);
    if (context.property.generation != generation_)
        return failure(SceneAdapterDisposition::Stale,
                       "grouping-generation-mismatch", 0U, 0U);
    if (policy_ && !samePolicy(*policy_, context.coordinatePolicy))
        return failure(SceneAdapterDisposition::InvalidInput,
                       "grouping-coordinate-policy-changed", 0U, 0U);

    std::vector<SceneObjectTargetGroup> staged;
    std::size_t begin = 0U;
    std::size_t groupIndex = 0U;
    std::int64_t previousTimestamp = lastTimestamp_;
    while (begin < metadata.size()) {
        const auto timestamp = metadata[begin].sourcePosition;
        const auto duration = metadata[begin].rampDuration;
        std::size_t end = begin + 1U;
        while (end < metadata.size()
               && metadata[end].sourcePosition == timestamp
               && metadata[end].rampDuration == duration) ++end;
        if (previousTimestamp >= 0 && timestamp <= previousTimestamp)
            return failure(SceneAdapterDisposition::InvalidInput,
                           "grouping-timestamp-not-strictly-monotonic", begin,
                           groupIndex);
        if (end < metadata.size() && metadata[end].sourcePosition == timestamp)
            return failure(SceneAdapterDisposition::Unsupported,
                           "same-timestamp-different-ramp-unsupported", end,
                           groupIndex + 1U);
        const std::vector<eac3gate6c::MetadataUpdate> records(
            metadata.begin() + static_cast<std::ptrdiff_t>(begin),
            metadata.begin() + static_cast<std::ptrdiff_t>(end));
        const auto converted = adaptGate6cBatch(records, context);
        if (converted.disposition != SceneAdapterDisposition::Accepted) {
            const auto disposition = converted.disposition;
            auto result = failure(disposition, converted.reason.c_str(), begin,
                                  groupIndex);
            return result;
        }
        staged.push_back(converted.targetGroup);
        previousTimestamp = timestamp;
        begin = end;
        ++groupIndex;
    }
    if (staged.empty())
        return failure(SceneAdapterDisposition::InvalidInput,
                       "no-groups-produced", 0U, 0U);
    lastTimestamp_ = staged.back().timestampSamples;
    if (!policy_) policy_ = context.coordinatePolicy;
    SceneObjectGroupingResult result;
    result.disposition = SceneAdapterDisposition::Accepted;
    result.reason = "decoded-target-groups-staged";
    result.groups = std::move(staged);
    return result;
}

SceneObjectRampResult SceneObjectStreamGrouper::reset(std::uint64_t generation)
{
    if (generation == 0U || generation <= generation_)
        return {SceneAdapterDisposition::Stale, "grouping-generation-not-advanced"};
    generation_ = generation;
    lastTimestamp_ = -1;
    policy_.reset();
    return {SceneAdapterDisposition::Accepted, "grouping-reset"};
}

SceneObjectCausalFeedResult applyNextCausalGroup(
    const std::vector<SceneObjectTargetGroup> &groups,
    std::size_t *nextGroupIndex, std::int64_t currentSample,
    SceneObjectRampScheduler *scheduler)
{
    if (!nextGroupIndex || !scheduler)
        return {SceneAdapterDisposition::InvalidInput, "null-causal-feed-input", false};
    if (currentSample < 0)
        return {SceneAdapterDisposition::InvalidInput, "negative-current-sample", false};
    if (*nextGroupIndex >= groups.size())
        return {SceneAdapterDisposition::InvalidInput, "no-next-target-group", false};
    const auto &group = groups[*nextGroupIndex];
    if (currentSample < group.timestampSamples)
        return {SceneAdapterDisposition::Accepted, "future-group-not-applied", false};
    const auto applied = scheduler->applyTargetGroup(group);
    if (applied.disposition != SceneAdapterDisposition::Accepted)
        return {applied.disposition, applied.reason, false};
    ++*nextGroupIndex;
    return {SceneAdapterDisposition::Accepted, "one-causal-group-applied", true};
}

} // namespace eac3render
