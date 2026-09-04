#include "scene-adapter.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace eac3render {
namespace {

constexpr double kSnapMaxDistance = 0.4;

void setReason(std::string *reason, const char *value)
{
    if (reason) {
        *reason = value;
    }
}

bool finite(float value)
{
    return std::isfinite(value);
}

bool finiteVector(const UnitVector3 &value)
{
    return std::all_of(value.begin(), value.end(), [](double component) {
        return std::isfinite(component);
    });
}

double vectorPower(const UnitVector3 &value)
{
    return value[0] * value[0] + value[1] * value[1]
        + value[2] * value[2];
}

bool validUnitVector(const UnitVector3 &value)
{
    return finiteVector(value) && std::abs(vectorPower(value) - 1.0) <= 1.0e-6;
}

bool validRoomPosition(const std::array<float, 3> &value)
{
    return finite(value[0]) && finite(value[1]) && finite(value[2])
        && value[0] >= 0.0F && value[0] <= 1.0F
        && value[1] >= 0.0F && value[1] <= 1.0F
        && value[2] >= -1.0F && value[2] <= 1.0F;
}

bool eligibleByZone(const Bs2051SpeakerPosition &speaker,
                    const std::array<bool, 6> &zones)
{
    // TS 103 420 decoder-interface order: screen, side, surround, back,
    // centre-and-back, top-bottom.  A speaker may participate when any of
    // its geometric regions is enabled; this is only candidate filtering.
    const double azimuth = std::abs(speaker.azimuthDegrees);
    const bool screen = azimuth <= 60.0 && speaker.layer != Bs2051Layer::Bottom;
    const bool side = std::abs(azimuth - 90.0) <= 1.0;
    const bool surround = azimuth >= 120.0 && azimuth < 180.0;
    const bool back = std::abs(azimuth - 180.0) <= 1.0;
    const bool centreBack = screen || back;
    const bool topBottom = speaker.layer != Bs2051Layer::Middle;
    return (zones[0] && screen) || (zones[1] && side)
        || (zones[2] && surround) || (zones[3] && back)
        || (zones[4] && centreBack) || (zones[5] && topBottom);
}

bool normalizeDirection(UnitVector3 *value)
{
    if (!value) {
        return false;
    }
    const double squared = vectorPower(*value);
    if (!std::isfinite(squared) || squared <= std::numeric_limits<double>::min()) {
        return false;
    }
    const double inverse = 1.0 / std::sqrt(squared);
    for (double &component : *value) {
        component *= inverse;
    }
    return validUnitVector(*value);
}

bool validNonnegative(float value)
{
    return finite(value) && value >= 0.0F;
}

bool checkedEnd(std::int64_t start, std::int64_t duration,
                std::int64_t *end)
{
    if (start < 0 || duration <= 0
        || duration > std::numeric_limits<std::int64_t>::max() - start) {
        return false;
    }
    if (end) {
        *end = start + duration;
    }
    return true;
}

} // namespace

const char *sceneAdapterDispositionName(SceneAdapterDisposition disposition)
{
    switch (disposition) {
    case SceneAdapterDisposition::Accepted: return "Accepted";
    case SceneAdapterDisposition::InvalidInput: return "InvalidInput";
    case SceneAdapterDisposition::Unsupported: return "Unsupported";
    case SceneAdapterDisposition::Stale: return "Stale";
    }
    return "Unknown";
}

const char *metadataGapPolicyName(MetadataGapPolicy policy)
{
    switch (policy) {
    case MetadataGapPolicy::HoldLast: return "hold-last";
    case MetadataGapPolicy::Terminate: return "terminate";
    }
    return "unknown";
}

SceneAdapter::SceneAdapter(MetadataGapPolicy gapPolicy,
                           const Bs2127TopologyOptions &pannerOptions)
    : gapPolicy_(gapPolicy)
    , panner_(pannerOptions)
{
}

bool SceneAdapter::bedSpeakerIndex(BedLabel label, std::size_t *speakerIndex,
                                   std::string *reason)
{
    if (!speakerIndex) {
        setReason(reason, "null-bed-speaker-index");
        return false;
    }
    Bs2051Label target = Bs2051Label::MPlus000;
    switch (label) {
    case BedLabel::FL: target = Bs2051Label::MMinus030; break;
    case BedLabel::FR: target = Bs2051Label::MPlus030; break;
    case BedLabel::FC: target = Bs2051Label::MPlus000; break;
    case BedLabel::SL: target = Bs2051Label::MMinus090; break;
    case BedLabel::SR: target = Bs2051Label::MPlus090; break;
    case BedLabel::TFL: target = Bs2051Label::UMinus045; break;
    case BedLabel::TFR: target = Bs2051Label::UPlus045; break;
    case BedLabel::LFE:
        setReason(reason, "lfe-separate-from-point-layout");
        return false;
    }
    const auto &layout = Bs2051SystemHLayout::systemH();
    const auto found = std::find_if(layout.begin(), layout.end(),
                                    [target](const Bs2051SpeakerPosition &item) {
                                        return item.label == target;
                                    });
    if (found == layout.end()) {
        setReason(reason, "bed-target-not-in-system-h");
        return false;
    }
    *speakerIndex = static_cast<std::size_t>(found - layout.begin());
    return true;
}

SceneAdapterResult SceneAdapter::makeFailure(
    SceneAdapterDisposition disposition, const char *reason) const
{
    SceneAdapterResult output;
    output.disposition = disposition;
    output.reason = reason ? reason : "";
    output.frame.generation = generation_;
    return output;
}

SceneAdapterResult SceneAdapter::reset(std::uint64_t generation)
{
    if (generation == 0U || generation <= generation_) {
        return makeFailure(SceneAdapterDisposition::Stale,
                           "scene-generation-not-advanced");
    }
    generation_ = generation;
    lastUpdateTimestamp_ = -1;
    bedGains_.fill(0.0F);
    lfeEnabled_ = false;
    lfeGain_ = 0.0F;
    objects_.clear();
    updateCount_ = 0U;
    SceneAdapterResult output;
    output.disposition = SceneAdapterDisposition::Accepted;
    output.reason = "scene-adapter-reset";
    output.frame.generation = generation_;
    return output;
}

SceneAdapterResult SceneAdapter::pushUpdate(const SceneAdapterUpdate &update)
{
    if (!panner_.valid()) {
        return makeFailure(SceneAdapterDisposition::Unsupported,
                           "configured-panner-invalid");
    }
    if (update.generation != generation_) {
        return makeFailure(SceneAdapterDisposition::Stale,
                           "scene-update-generation-mismatch");
    }
    if (update.timestampSamples < 0
        || (lastUpdateTimestamp_ >= 0
            && update.timestampSamples < lastUpdateTimestamp_)) {
        return makeFailure(SceneAdapterDisposition::InvalidInput,
                           "scene-update-timestamp-not-monotonic");
    }
    std::int64_t updateEnd = 0;
    if (!checkedEnd(update.timestampSamples, update.durationSamples, &updateEnd)) {
        return makeFailure(SceneAdapterDisposition::InvalidInput,
                           "scene-update-invalid-time-range");
    }
    if (!validNonnegative(update.lfeGain)) {
        return makeFailure(SceneAdapterDisposition::InvalidInput,
                           "scene-update-invalid-lfe-gain");
    }

    std::array<float, 8> nextBedGains = bedGains_;
    std::vector<std::pair<std::uint64_t, StoredObject>> nextObjects = objects_;
    std::vector<BedLabel> updateBeds;
    updateBeds.reserve(update.beds.size());
    for (const BedGainUpdate &bed : update.beds) {
        std::size_t speakerIndex = 0U;
        std::string routeReason;
        if (!bedSpeakerIndex(bed.label, &speakerIndex, &routeReason)) {
            return makeFailure(SceneAdapterDisposition::Unsupported,
                               routeReason.c_str());
        }
        if (!validNonnegative(bed.gain)
            || std::find(updateBeds.begin(), updateBeds.end(), bed.label)
                   != updateBeds.end()) {
            return makeFailure(SceneAdapterDisposition::InvalidInput,
                               !validNonnegative(bed.gain)
                                   ? "scene-bed-invalid-gain"
                                   : "scene-bed-duplicate-label");
        }
        updateBeds.push_back(bed.label);
        nextBedGains[static_cast<std::size_t>(bed.label)] = bed.gain;
    }

    std::vector<std::uint64_t> updateObjects;
    updateObjects.reserve(update.objects.size());
    for (const ObjectMetadataUpdate &object : update.objects) {
        if (object.objectId == 0U
            || std::find(updateObjects.begin(), updateObjects.end(),
                        object.objectId) != updateObjects.end()) {
            return makeFailure(SceneAdapterDisposition::InvalidInput,
                               object.objectId == 0U
                                   ? "scene-object-id-zero"
                                   : "scene-object-duplicate-id");
        }
        if (object.timestampSamples < update.timestampSamples
            || object.timestampSamples >= updateEnd
            || !validNonnegative(object.gain)
            || !finite(object.priority) || object.priority < 0.0F
            || object.priority > 1.0F
            || (object.coordinateSpace
                    == ObjectMetadataUpdate::CoordinateSpace::UnitDirection
                ? !validUnitVector(object.position)
                : !validRoomPosition(object.roomPosition))) {
            return makeFailure(SceneAdapterDisposition::InvalidInput,
                               object.timestampSamples < update.timestampSamples
                                   || object.timestampSamples >= updateEnd
                                   ? "scene-object-timestamp-out-of-block"
                                   : !validNonnegative(object.gain)
                                       ? "scene-object-invalid-gain"
                                       : !finite(object.priority)
                                            || object.priority < 0.0F
                                            || object.priority > 1.0F
                                           ? "scene-object-priority-invalid"
                                       : object.coordinateSpace
                                                == ObjectMetadataUpdate::CoordinateSpace::UnitDirection
                                           ? "scene-object-position-not-unit"
                                           : "scene-object-room-position-invalid");
        }
        const auto findNextObject = [&nextObjects](std::uint64_t objectId) {
            return std::find_if(
                nextObjects.begin(), nextObjects.end(),
                [objectId](const auto &entry) { return entry.first == objectId; });
        };
        const auto found = findNextObject(object.objectId);
        const std::size_t objectIndex = found == nextObjects.end()
            ? std::numeric_limits<std::size_t>::max()
            : static_cast<std::size_t>(found - nextObjects.begin());
        if (objectIndex == std::numeric_limits<std::size_t>::max()) {
            nextObjects.push_back({object.objectId, StoredObject {}});
        }
        const auto resolved = findNextObject(object.objectId);
        const std::size_t resolvedIndex =
            static_cast<std::size_t>(resolved - nextObjects.begin());
        StoredObject &stored = nextObjects[resolvedIndex].second;
        if (!stored.points.empty()
            && object.coordinateSpace != stored.points.back().coordinateSpace) {
            return makeFailure(SceneAdapterDisposition::InvalidInput,
                               "scene-object-coordinate-space-changed");
        }
        if (!stored.points.empty()
            && object.timestampSamples <= stored.points.back().timestampSamples) {
            return makeFailure(SceneAdapterDisposition::InvalidInput,
                               object.timestampSamples
                                       == stored.points.back().timestampSamples
                                   ? "scene-object-duplicate-timestamp"
                                   : "scene-object-timestamp-not-monotonic");
        }
        stored.points.push_back(object);
        updateObjects.push_back(object.objectId);
    }

    // Record omissions as historical half-open intervals.  A later object
    // update must not erase an earlier gap, because renderAt() is explicitly
    // allowed to query an already-pushed timestamp.
    for (auto &entry : nextObjects) {
        StoredObject &stored = entry.second;
        if (stored.points.empty()
            || std::find(updateObjects.begin(), updateObjects.end(), entry.first)
                   != updateObjects.end()) {
            continue;
        }
        if (!stored.gaps.empty()
            && stored.gaps.back().endSamples >= update.timestampSamples) {
            stored.gaps.back().endSamples =
                std::max(stored.gaps.back().endSamples, updateEnd);
        } else {
            stored.gaps.push_back(
                GapInterval {update.timestampSamples, updateEnd});
        }
    }

    bedGains_ = nextBedGains;
    objects_ = std::move(nextObjects);
    lfeEnabled_ = update.lfeEnabled;
    lfeGain_ = update.lfeGain;
    lastUpdateTimestamp_ = update.timestampSamples;
    ++updateCount_;
    return renderInternal(generation_, update.timestampSamples,
                          update.durationSamples);
}

SceneAdapterResult SceneAdapter::renderAt(
    std::uint64_t generation, std::int64_t timestampSamples) const
{
    if (generation != generation_) {
        return makeFailure(SceneAdapterDisposition::Stale,
                           "scene-render-generation-mismatch");
    }
    if (timestampSamples < 0) {
        return makeFailure(SceneAdapterDisposition::InvalidInput,
                           "scene-render-negative-timestamp");
    }
    return renderInternal(generation, timestampSamples, 0);
}

SceneAdapterResult SceneAdapter::renderInternal(
    std::uint64_t generation, std::int64_t timestampSamples,
    std::int64_t durationSamples) const
{
    SceneAdapterResult output;
    output.disposition = SceneAdapterDisposition::Accepted;
    output.reason = "scene-adapter-frame-accepted";
    output.frame.generation = generation;
    output.frame.timestampSamples = timestampSamples;
    output.frame.durationSamples = durationSamples;
    output.frame.lfeEnabled = lfeEnabled_;
    output.frame.lfeGain = lfeEnabled_ ? lfeGain_ : 0.0F;
    output.frame.extentApplied = false;

    const auto &layout = Bs2051SystemHLayout::systemH();
    for (std::size_t bedIndex = 0U; bedIndex < 8U; ++bedIndex) {
        const BedLabel label = static_cast<BedLabel>(bedIndex);
        if (label == BedLabel::LFE || bedGains_[bedIndex] == 0.0F) {
            continue;
        }
        std::size_t speakerIndex = 0U;
        if (!bedSpeakerIndex(label, &speakerIndex, nullptr)) {
            output.disposition = SceneAdapterDisposition::Unsupported;
            output.reason = "bed-route-resolution-failed";
            return output;
        }
        const auto panned = panner_.render(layout[speakerIndex].unitVector());
        if (panned.status != Bs2127PointSourcePannerStatus::Selected
            || !std::all_of(
                panned.gains.begin(), panned.gains.end(),
                [](double value) { return std::isfinite(value); })) {
            output.disposition = SceneAdapterDisposition::Unsupported;
            output.reason = "bed-point-source-panning-failed";
            return output;
        }
        for (std::size_t channel = 0U; channel < kSystemHSpeakerCount;
             ++channel) {
            output.frame.bedGains[channel] +=
                static_cast<double>(bedGains_[bedIndex]) * panned.gains[channel];
        }
    }
    output.frame.speakerGains = output.frame.bedGains;

    for (const auto &entry : objects_) {
        const StoredObject &stored = entry.second;
        if (stored.points.empty()) {
            continue;
        }
        const auto next = std::upper_bound(
            stored.points.begin(), stored.points.end(), timestampSamples,
            [](std::int64_t timestamp, const ObjectMetadataUpdate &point) {
                return timestamp < point.timestampSamples;
            });
        if (next == stored.points.begin()) {
            continue;
        }
        const ObjectMetadataUpdate &left = *(next - 1);
        ObjectMetadataUpdate state = left;
        bool interpolated = false;
        bool held = false;
        const auto gap = std::find_if(
            stored.gaps.begin(), stored.gaps.end(),
            [timestampSamples](const GapInterval &interval) {
                return timestampSamples >= interval.startSamples
                    && timestampSamples < interval.endSamples;
            });
        const bool pairCrossesGap = next != stored.points.end()
            && std::any_of(
                stored.gaps.begin(), stored.gaps.end(),
                [&left, &next](const GapInterval &interval) {
                    return interval.startSamples < next->timestampSamples
                        && interval.endSamples > left.timestampSamples;
                });
        if (gap != stored.gaps.end()) {
            if (gapPolicy_ == MetadataGapPolicy::Terminate) {
                output.frame.metadataTerminated = true;
                continue;
            }
            held = true;
        } else if (next != stored.points.end() && !pairCrossesGap) {
            const ObjectMetadataUpdate &right = *next;
            const std::int64_t span = right.timestampSamples
                - left.timestampSamples;
            if (span <= 0) {
                output.disposition = SceneAdapterDisposition::InvalidInput;
                output.reason = "scene-object-timestamp-order-invalid";
                return output;
            }
            const double fraction = static_cast<double>(timestampSamples
                                                        - left.timestampSamples)
                / static_cast<double>(span);
            state.gain = static_cast<float>(
                static_cast<double>(left.gain)
                + (static_cast<double>(right.gain)
                   - static_cast<double>(left.gain)) * fraction);
            if (right.jumpPosition) {
                state.position = left.position;
                state.roomPosition = left.roomPosition;
            } else {
                if (state.coordinateSpace
                    == ObjectMetadataUpdate::CoordinateSpace::UnitDirection) {
                    for (std::size_t axis = 0U; axis < 3U; ++axis) {
                        state.position[axis] = left.position[axis]
                            + (right.position[axis] - left.position[axis]) * fraction;
                    }
                    if (!normalizeDirection(&state.position)) {
                        output.disposition = SceneAdapterDisposition::InvalidInput;
                        output.reason = "scene-object-interpolated-position-invalid";
                        return output;
                    }
                } else {
                    for (std::size_t axis = 0U; axis < 3U; ++axis) {
                        state.roomPosition[axis] = static_cast<float>(
                            static_cast<double>(left.roomPosition[axis])
                            + (static_cast<double>(right.roomPosition[axis])
                               - static_cast<double>(left.roomPosition[axis]))
                                * fraction);
                    }
                }
            }
            interpolated = fraction > 0.0 && fraction < 1.0;
        }
        if (!validNonnegative(state.gain)
            || (state.coordinateSpace
                    == ObjectMetadataUpdate::CoordinateSpace::UnitDirection
                ? !validUnitVector(state.position)
                : !validRoomPosition(state.roomPosition))) {
            output.disposition = SceneAdapterDisposition::InvalidInput;
            output.reason = "scene-object-interpolated-state-invalid";
            return output;
        }
        if (state.warpMode == ObjectWarpMode::Reserved) {
            output.disposition = SceneAdapterDisposition::Unsupported;
            output.reason = "reserved-warp-mode";
            return output;
        }
        if (state.warpMode == ObjectWarpMode::PositionY2) {
            if (state.coordinateSpace
                == ObjectMetadataUpdate::CoordinateSpace::EtsiRoomCartesian) {
                state.roomPosition[1] = std::clamp(state.roomPosition[1] * 2.0F,
                                                   0.0F, 1.0F);
            } else {
                state.position[1] *= 2.0;
                if (!normalizeDirection(&state.position)) {
                    output.disposition = SceneAdapterDisposition::InvalidInput;
                    output.reason = "warp-position-y2-invalid";
                    return output;
                }
            }
        }
        ObjectGainFrame objectFrame;
        objectFrame.objectId = entry.first;
        objectFrame.gain = state.gain;
        objectFrame.position = state.position;
        objectFrame.coordinateSpace = state.coordinateSpace;
        objectFrame.roomPosition = state.roomPosition;
        objectFrame.priority = state.priority;
        objectFrame.extent = state.extent;
        objectFrame.extentPresence = state.extentPresence;
        objectFrame.sourceSizeIndex = state.sourceSizeIndex;
        objectFrame.zoneConstraints = state.zoneConstraints;
        objectFrame.elevation = state.elevation;
        objectFrame.snap = state.snap;
        objectFrame.screenAnchored = state.screenAnchored;
        objectFrame.distanceSpecified = state.distanceSpecified;
        objectFrame.distanceInfinite = state.distanceInfinite;
        objectFrame.distanceFactor = state.distanceFactor;
        objectFrame.screenFactor = state.screenFactor;
        objectFrame.depthFactor = state.depthFactor;
        objectFrame.extendedPrecisionPresent = state.extendedPrecisionPresent;
        objectFrame.extendedPrecision = state.extendedPrecision;
        objectFrame.unsupportedProperties = state.unsupportedProperties;
        objectFrame.speakerAnchorPresent = state.speakerAnchorPresent;
        objectFrame.speakerAnchor = state.speakerAnchor;
        objectFrame.interpolated = interpolated;
        objectFrame.held = held;
        if (state.coordinateSpace
            == ObjectMetadataUpdate::CoordinateSpace::UnitDirection) {
            const auto panned = panner_.render(state.position);
            if (panned.status != Bs2127PointSourcePannerStatus::Selected) {
                output.disposition = SceneAdapterDisposition::Unsupported;
                output.reason = "object-point-source-panning-failed";
                return output;
            }
            for (std::size_t channel = 0U; channel < kSystemHSpeakerCount;
                 ++channel) {
                objectFrame.speakerGains[channel] =
                    static_cast<double>(state.gain) * panned.gains[channel];
            }
        } else {
            std::array<double, 3> point {};
            std::string cartesianReason;
            if (!Bs2127SystemHCartesianPanner::mapEtsiRoomToAllocentric(
                    state.roomPosition, &point, &cartesianReason)) {
                output.disposition = SceneAdapterDisposition::InvalidInput;
                output.reason = cartesianReason.empty()
                    ? "object-room-to-allocentric-failed" : cartesianReason;
                return output;
            }
            const auto panned = Bs2127SystemHCartesianPanner {}.render(point);
            if (!panned.accepted) {
                output.disposition = SceneAdapterDisposition::Unsupported;
                output.reason = panned.reason.empty()
                    ? "object-cartesian-panning-failed" : panned.reason;
                return output;
            }
            for (std::size_t channel = 0U; channel < kSystemHSpeakerCount;
                 ++channel) {
                objectFrame.speakerGains[channel] =
                    static_cast<double>(state.gain) * panned.gains[channel];
            }
        }
        const auto &layout = Bs2051SystemHLayout::systemH();
        if (state.speakerAnchorPresent) {
            std::size_t speakerIndex = 0U;
            std::string anchorReason;
            if (!bedSpeakerIndex(state.speakerAnchor, &speakerIndex,
                                 &anchorReason)) {
                output.disposition = SceneAdapterDisposition::Unsupported;
                output.reason = anchorReason.empty() ? "speaker-anchor-unmapped"
                                                     : anchorReason;
                return output;
            }
            objectFrame.speakerGains.fill(0.0);
            objectFrame.speakerGains[speakerIndex] = state.gain;
            objectFrame.speakerAnchorPresent = true;
            objectFrame.speakerAnchor = state.speakerAnchor;
        } else if (state.snap) {
            if (state.coordinateSpace
                != ObjectMetadataUpdate::CoordinateSpace::UnitDirection) {
                output.disposition = SceneAdapterDisposition::Unsupported;
                output.reason = "snap-requires-unit-direction";
                return output;
            }
            std::size_t nearest = kSystemHSpeakerCount;
            double nearestDistance = std::numeric_limits<double>::max();
            for (std::size_t channel = 0U; channel < layout.size(); ++channel) {
                if (!eligibleByZone(layout[channel], state.zoneConstraints)) continue;
                double distance = 0.0;
                for (std::size_t axis = 0U; axis < 3U; ++axis) {
                    const double delta = state.position[axis]
                        - layout[channel].unitVector()[axis];
                    distance += delta * delta;
                }
                distance = std::sqrt(distance);
                if (distance < nearestDistance) {
                    nearestDistance = distance;
                    nearest = channel;
                }
            }
            if (nearest == kSystemHSpeakerCount
                || nearestDistance > kSnapMaxDistance) {
                output.disposition = SceneAdapterDisposition::Unsupported;
                output.reason = "snap-no-eligible-speaker-within-0.4";
                return output;
            }
            objectFrame.speakerGains.fill(0.0);
            objectFrame.speakerGains[nearest] = state.gain;
            objectFrame.snapApplied = true;
        } else {
            const bool allZonesEnabled = std::all_of(
                state.zoneConstraints.begin(), state.zoneConstraints.end(),
                [](bool enabled) { return enabled; });
            if (allZonesEnabled) {
                // No candidate filtering is needed for the unrestricted mask.
            } else {
            double originalPower = 0.0;
            double filteredPower = 0.0;
            for (std::size_t channel = 0U; channel < layout.size(); ++channel) {
                originalPower += objectFrame.speakerGains[channel]
                    * objectFrame.speakerGains[channel];
                if (!eligibleByZone(layout[channel], state.zoneConstraints))
                    objectFrame.speakerGains[channel] = 0.0;
                filteredPower += objectFrame.speakerGains[channel]
                    * objectFrame.speakerGains[channel];
            }
            if (!(filteredPower > 0.0) || !std::isfinite(filteredPower)) {
                output.disposition = SceneAdapterDisposition::Unsupported;
                output.reason = "zone-constraints-remove-all-speakers";
                return output;
            }
            if (std::abs(filteredPower - originalPower) > 1.0e-12) {
                const double scale = std::sqrt(originalPower / filteredPower);
                for (double &gain : objectFrame.speakerGains) gain *= scale;
                objectFrame.zoneFiltered = true;
            }
            }
        }
        objectFrame.speakerAnchorPresent = state.speakerAnchorPresent;
        objectFrame.speakerAnchor = state.speakerAnchor;
        for (std::size_t channel = 0U; channel < kSystemHSpeakerCount;
             ++channel) {
            output.frame.speakerGains[channel] += objectFrame.speakerGains[channel];
        }
        output.frame.metadataHeld = output.frame.metadataHeld || held;
        output.frame.objects.push_back(objectFrame);
    }
    return output;
}

} // namespace eac3render
