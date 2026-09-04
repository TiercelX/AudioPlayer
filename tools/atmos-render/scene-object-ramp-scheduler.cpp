#include "scene-object-ramp-scheduler.h"

#include <cmath>
#include <limits>

namespace eac3render {
namespace {

bool finite(double value) { return std::isfinite(value); }

bool validUnit(const UnitVector3 &value)
{
    double power = 0.0;
    for (double component : value) {
        if (!finite(component)) return false;
        power += component * component;
    }
    return std::abs(power - 1.0) <= 1.0e-9;
}

bool validRoom(const std::array<float, 3> &value)
{
    return finite(value[0]) && finite(value[1]) && finite(value[2])
        && value[0] >= 0.0F && value[0] <= 1.0F
        && value[1] >= 0.0F && value[1] <= 1.0F
        && value[2] >= -1.0F && value[2] <= 1.0F;
}

bool checkedEnd(std::int64_t start, std::int64_t duration, std::int64_t *end)
{
    if (duration < 0 || start < 0
        || duration > std::numeric_limits<std::int64_t>::max() - start) {
        return false;
    }
    *end = start + duration;
    return true;
}

bool roomVector(const SceneObjectCoordinatePolicy &policy,
                const std::array<float, 3> &room,
                std::array<double, 3> *out)
{
    if (!out || !finite(policy.roomWidthMetres) || !finite(policy.roomDepthMetres)
        || !finite(policy.roomHeightMetres) || policy.roomWidthMetres <= 0.0
        || policy.roomDepthMetres <= 0.0 || policy.roomHeightMetres <= 0.0) return false;
    if (!finite(policy.listenerX) || !finite(policy.listenerY)
        || !finite(policy.listenerZ) || policy.listenerX < 0.0
        || policy.listenerX > 1.0 || policy.listenerY < 0.0
        || policy.listenerY > 1.0 || policy.listenerZ < -1.0
        || policy.listenerZ > 1.0) return false;
    const double x = static_cast<double>(room[0]) * policy.roomWidthMetres
        - policy.listenerX * policy.roomWidthMetres;
    const double y = static_cast<double>(room[1]) * policy.roomDepthMetres
        - policy.listenerY * policy.roomDepthMetres;
    const double z = (static_cast<double>(room[2]) + 1.0) * 0.5 * policy.roomHeightMetres
        - (policy.listenerZ + 1.0) * 0.5 * policy.roomHeightMetres;
    *out = { -y, x, z };
    return finite((*out)[0]) && finite((*out)[1]) && finite((*out)[2]);
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

SceneObjectRampResult failure(SceneAdapterDisposition disposition, const char *reason)
{
    return {disposition, reason};
}

} // namespace

SceneObjectRampResult SceneObjectRampScheduler::reset(std::uint64_t generation)
{
    if (generation == 0U || generation <= generation_) {
        return failure(SceneAdapterDisposition::Stale, "scheduler-generation-not-advanced");
    }
    generation_ = generation;
    lastTimestamp_ = -1;
    ramps_.clear();
    policy_.reset();
    return {SceneAdapterDisposition::Accepted, "scheduler-reset"};
}

SceneObjectRampResult SceneObjectRampScheduler::applyTargetGroup(
    const SceneObjectTargetGroup &group)
{
    if (group.generation != generation_) {
        return failure(SceneAdapterDisposition::Stale, "scheduler-generation-mismatch");
    }
    if (group.objects.empty()) {
        return failure(SceneAdapterDisposition::InvalidInput, "scheduler-empty-target-group");
    }
    if (policy_ && !samePolicy(*policy_, group.coordinatePolicy)) {
        return failure(SceneAdapterDisposition::InvalidInput, "scheduler-coordinate-policy-changed");
    }
    if (group.timestampSamples < 0
        || (lastTimestamp_ >= 0 && group.timestampSamples <= lastTimestamp_)) {
        return failure(SceneAdapterDisposition::InvalidInput,
                       "scheduler-timestamp-not-strictly-monotonic");
    }
    std::int64_t end = 0;
    if (!checkedEnd(group.timestampSamples, group.durationSamples, &end)) {
        return failure(SceneAdapterDisposition::InvalidInput, "scheduler-invalid-ramp-range");
    }
    std::map<std::uint64_t, Ramp> staged = ramps_;
    std::uint64_t previous = 0U;
    for (const auto &target : group.objects) {
        std::array<double, 3> targetCartesian;
        if (target.update.timestampSamples != group.timestampSamples
            || target.update.objectId == 0U
            || !finite(target.update.gain) || target.update.gain < 0.0F
            || (!staged.empty() && target.update.objectId <= previous)) {
            return failure(SceneAdapterDisposition::InvalidInput,
                           "scheduler-invalid-or-unsorted-target");
        }
        if (target.update.coordinateSpace
            == ObjectMetadataUpdate::CoordinateSpace::EtsiRoomCartesian) {
            if (!validRoom(target.update.roomPosition)
                || target.update.roomPosition != target.roomPosition) {
                return failure(SceneAdapterDisposition::InvalidInput,
                               "scheduler-room-position-mismatch");
            }
            for (std::size_t i = 0U; i < 3U; ++i)
                targetCartesian[i] = target.roomPosition[i];
        } else {
            if (!validUnit(target.update.position)
                || !roomVector(group.coordinatePolicy, target.roomPosition,
                               &targetCartesian)) {
                return failure(SceneAdapterDisposition::InvalidInput,
                               "scheduler-invalid-directional-target");
            }
            const double targetNorm = std::sqrt(targetCartesian[0] * targetCartesian[0]
                + targetCartesian[1] * targetCartesian[1]
                + targetCartesian[2] * targetCartesian[2]);
            if (!(targetNorm > 0.0)
                || std::abs(target.update.position[0]
                    - targetCartesian[0] / targetNorm) > 1.0e-5
                || std::abs(target.update.position[1]
                    - targetCartesian[1] / targetNorm) > 1.0e-5
                || std::abs(target.update.position[2]
                    - targetCartesian[2] / targetNorm) > 1.0e-5) {
                return failure(SceneAdapterDisposition::InvalidInput,
                               "scheduler-room-direction-mismatch");
            }
        }
        previous = target.update.objectId;
        ObjectMetadataUpdate start = target.update;
        auto prior = staged.find(target.update.objectId);
        std::array<double, 3> startCartesian = targetCartesian;
        if (prior != staged.end()) {
            const Ramp &old = prior->second;
            if (old.target.coordinateSpace != target.update.coordinateSpace) {
                return failure(SceneAdapterDisposition::InvalidInput,
                               "scheduler-coordinate-space-changed");
            }
            const std::int64_t span = old.endSamples - old.start.timestampSamples;
            const double fraction = span <= 0 || group.timestampSamples >= old.endSamples
                ? 1.0 : static_cast<double>(group.timestampSamples - old.start.timestampSamples)
                    / static_cast<double>(span);
            start.gain = static_cast<float>(old.start.gain
                + (old.target.gain - old.start.gain) * fraction);
            for (std::size_t i = 0; i < 3U; ++i)
                startCartesian[i] = old.startCartesian[i]
                    + (old.targetCartesian[i] - old.startCartesian[i]) * fraction;
        }
        if (target.update.jumpPosition) startCartesian = targetCartesian;
        if (target.update.coordinateSpace
            == ObjectMetadataUpdate::CoordinateSpace::UnitDirection) {
            double startPower = 0.0;
            for (double component : startCartesian) startPower += component * component;
            if (!(startPower > 0.0) || !finite(startPower))
                return failure(SceneAdapterDisposition::InvalidInput,
                               "scheduler-overlap-position-invalid");
        }
        staged[target.update.objectId] = Ramp {start, target.update,
                                               startCartesian, targetCartesian, end};
    }
    ramps_ = std::move(staged);
    if (!policy_) policy_ = group.coordinatePolicy;
    lastTimestamp_ = group.timestampSamples;
    return {SceneAdapterDisposition::Accepted, "scheduler-target-group-applied"};
}

SceneObjectRampResult SceneObjectRampScheduler::evaluateSnapshot(
    std::uint64_t generation, std::int64_t timestampSamples,
    std::vector<SceneObjectSnapshot> *snapshot) const
{
    if (!snapshot) return failure(SceneAdapterDisposition::InvalidInput, "null-snapshot");
    snapshot->clear();
    if (generation != generation_) return failure(SceneAdapterDisposition::Stale, "scheduler-generation-mismatch");
    if (timestampSamples < 0) return failure(SceneAdapterDisposition::InvalidInput, "scheduler-negative-timestamp");
    if (lastTimestamp_ >= 0 && timestampSamples < lastTimestamp_)
        return failure(SceneAdapterDisposition::InvalidInput, "scheduler-historical-evaluation-unsupported");
    std::vector<SceneObjectSnapshot> stagedSnapshot;
    stagedSnapshot.reserve(ramps_.size());
    for (const auto &entry : ramps_) {
        const Ramp &ramp = entry.second;
        const std::int64_t span = ramp.endSamples - ramp.start.timestampSamples;
        const double fraction = span <= 0 || timestampSamples >= ramp.endSamples
            ? 1.0 : timestampSamples <= ramp.start.timestampSamples ? 0.0
                : static_cast<double>(timestampSamples - ramp.start.timestampSamples)
                    / static_cast<double>(span);
        SceneObjectSnapshot value;
        value.objectId = entry.first;
        value.coordinateSpace = ramp.target.coordinateSpace;
        value.priority = ramp.target.priority;
        value.extent = ramp.target.extent;
        value.extentPresence = ramp.target.extentPresence;
        value.sourceSizeIndex = ramp.target.sourceSizeIndex;
        value.zoneConstraints = ramp.target.zoneConstraints;
        value.elevation = ramp.target.elevation;
        value.snap = ramp.target.snap;
        value.screenAnchored = ramp.target.screenAnchored;
        value.distanceSpecified = ramp.target.distanceSpecified;
        value.distanceInfinite = ramp.target.distanceInfinite;
        value.distanceFactor = ramp.target.distanceFactor;
        value.screenFactor = ramp.target.screenFactor;
        value.depthFactor = ramp.target.depthFactor;
        value.extendedPrecisionPresent = ramp.target.extendedPrecisionPresent;
        value.extendedPrecision = ramp.target.extendedPrecision;
        value.unsupportedProperties = ramp.target.unsupportedProperties;
        value.speakerAnchorPresent = ramp.target.speakerAnchorPresent;
        value.speakerAnchor = ramp.target.speakerAnchor;
        value.warpMode = ramp.target.warpMode;
        value.trim = ramp.target.trim;
        value.divergencePresent = ramp.target.divergencePresent;
        value.divergenceReused = ramp.target.divergenceReused;
        value.divergenceIndex = ramp.target.divergenceIndex;
        value.divergence = ramp.target.divergence;
        value.gain = static_cast<float>(ramp.start.gain
            + (ramp.target.gain - ramp.start.gain) * fraction);
        std::array<double, 3> position;
        for (std::size_t i = 0; i < 3U; ++i) position[i] = ramp.startCartesian[i]
            + (ramp.targetCartesian[i] - ramp.startCartesian[i]) * fraction;
        if (value.coordinateSpace
            == ObjectMetadataUpdate::CoordinateSpace::EtsiRoomCartesian) {
            value.pointDirectionPresent = false;
            for (std::size_t i = 0U; i < 3U; ++i)
                value.roomPosition[i] = static_cast<float>(position[i]);
            if (!validRoom(value.roomPosition))
                return failure(SceneAdapterDisposition::InvalidInput,
                               "scheduler-snapshot-room-position-invalid");
        } else {
            double power = 0.0;
            for (double component : position) power += component * component;
            if (!(power > 0.0) || !finite(power))
                return failure(SceneAdapterDisposition::InvalidInput,
                               "scheduler-snapshot-position-invalid");
            const double scale = 1.0 / std::sqrt(power);
            for (double &component : position) component *= scale;
            value.position = position;
        }
        stagedSnapshot.push_back(value);
    }
    *snapshot = std::move(stagedSnapshot);
    return {SceneAdapterDisposition::Accepted, "scheduler-snapshot-evaluated"};
}

} // namespace eac3render
