#include "scene-object-bridge.h"

#include <cmath>

namespace eac3render {
namespace {

void fail(SceneObjectBatchResult *result, SceneAdapterDisposition disposition,
          const char *reason)
{
    result->disposition = disposition;
    result->reason = reason;
}

bool finite(double value)
{
    return std::isfinite(value);
}

} // namespace

SceneObjectBatchResult adaptGate6cBatch(
    const std::vector<eac3gate6c::MetadataUpdate> &metadata,
    const SceneObjectBatchContext &context)
{
    SceneObjectBatchResult result;
    if (metadata.empty()) {
        fail(&result, SceneAdapterDisposition::InvalidInput,
             "empty-co-timed-metadata-group");
        return result;
    }
    if (context.property.coordinate
        != ObjectCoordinateRepresentation::OamdCartesian) {
        fail(&result, SceneAdapterDisposition::Unsupported,
             "batch-requires-oamd-cartesian-coordinate");
        return result;
    }
    const auto &p = context.coordinatePolicy;
    if (!finite(p.roomWidthMetres) || !finite(p.roomDepthMetres)
        || !finite(p.roomHeightMetres) || p.roomWidthMetres <= 0.0
        || p.roomDepthMetres <= 0.0 || p.roomHeightMetres <= 0.0
        || !finite(p.listenerX) || !finite(p.listenerY) || !finite(p.listenerZ)
        || p.listenerX < 0.0 || p.listenerX > 1.0 || p.listenerY < 0.0
        || p.listenerY > 1.0 || p.listenerZ < -1.0 || p.listenerZ > 1.0) {
        fail(&result, SceneAdapterDisposition::InvalidInput,
             "invalid-coordinate-policy");
        return result;
    }

    result.properties.reserve(metadata.size());
    result.targetGroup.generation = context.property.generation;
    result.targetGroup.timestampSamples = metadata.front().sourcePosition;
    result.targetGroup.durationSamples = metadata.front().rampDuration;
    result.targetGroup.coordinatePolicy = context.coordinatePolicy;
    result.targetGroup.objects.reserve(metadata.size());
    const auto reject = [&](SceneAdapterDisposition disposition,
                            const char *reason) {
        result.properties.clear();
        result.targetGroup.objects.clear();
        fail(&result, disposition, reason);
    };
    std::uint64_t previousObject = 0U;
    for (const auto &record : metadata) {
        if (record.sourcePosition != result.targetGroup.timestampSamples
            || record.rampDuration != static_cast<unsigned>(result.targetGroup.durationSamples)) {
            reject(SceneAdapterDisposition::InvalidInput,
                   "mixed-batch-timestamps-or-ramp-durations");
            return result;
        }
        if (!result.properties.empty() && record.objectIndex <= previousObject) {
            reject(SceneAdapterDisposition::InvalidInput,
                   "non-deterministic-object-order");
            return result;
        }
        const auto property = adaptGate6cObject(record, context.property);
        if (!property.acceptedPoint()) {
            reject(property.extentPending() ? SceneAdapterDisposition::Unsupported
                                            : SceneAdapterDisposition::InvalidInput,
                   property.reason.empty() ? "object-property-rejected"
                                           : property.reason.c_str());
            return result;
        }
        if (property.properties.screenAnchored) {
            reject(SceneAdapterDisposition::Unsupported,
                   "screen-position-requires-screen-geometry");
            return result;
        }
        if (property.properties.distanceInfinite) {
            reject(SceneAdapterDisposition::Unsupported,
                   "infinite-distance-unsupported");
            return result;
        }
        std::string reason;
        SceneObjectPropertyResult converted = property;
        ObjectMetadataUpdate update;
        if (!toSceneAdapterObjectUpdate(converted, &update, &reason)) {
            reject(SceneAdapterDisposition::InvalidInput,
                   reason.empty() ? "scene-object-update-rejected" : reason.c_str());
            return result;
        }
        result.properties.push_back(converted.properties);
        result.targetGroup.objects.push_back({update, converted.properties.roomPosition});
        previousObject = record.objectIndex;
    }
    result.disposition = SceneAdapterDisposition::Accepted;
    result.reason = "decoded-object-target-group";
    return result;
}

} // namespace eac3render
