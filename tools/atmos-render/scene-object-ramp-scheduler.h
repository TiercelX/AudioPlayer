#pragma once

#include "scene-object-bridge.h"

#include <map>
#include <optional>

namespace eac3render {

struct SceneObjectSnapshot {
    std::uint64_t objectId = 0U;
    float gain = 0.0F;
    UnitVector3 position {0.0, 0.0, 1.0};
    ObjectMetadataUpdate::CoordinateSpace coordinateSpace =
        ObjectMetadataUpdate::CoordinateSpace::UnitDirection;
    bool pointDirectionPresent = true;
    std::array<float, 3> roomPosition {0.5F, 0.5F, 0.0F};
    ObjectExtent extent;
    ObjectExtentPresence extentPresence = ObjectExtentPresence::Absent;
    unsigned sourceSizeIndex = 0U;
    std::array<bool, 6> zoneConstraints {true, true, true, true, true, true};
    bool elevation = true;
    bool snap = false;
    bool screenAnchored = false;
    bool distanceSpecified = false;
    bool distanceInfinite = false;
    float distanceFactor = 0.0F;
    float screenFactor = 0.0F;
    float depthFactor = 0.0F;
    std::array<bool, 3> extendedPrecisionPresent {};
    std::array<int, 3> extendedPrecision {};
    float priority = 0.0F;
    std::uint32_t unsupportedProperties = 0U;
    bool speakerAnchorPresent = false;
    BedLabel speakerAnchor = BedLabel::FL;
    ObjectWarpMode warpMode = ObjectWarpMode::None;
    ObjectTrimMetadata trim;
    bool divergencePresent = false;
    bool divergenceReused = false;
    unsigned divergenceIndex = 0U;
    float divergence = 0.0F;
};

struct SceneObjectRampResult {
    SceneAdapterDisposition disposition = SceneAdapterDisposition::InvalidInput;
    std::string reason;
};

class SceneObjectRampScheduler final {
public:
    SceneObjectRampResult reset(std::uint64_t generation);
    SceneObjectRampResult applyTargetGroup(const SceneObjectTargetGroup &group);
    SceneObjectRampResult evaluateSnapshot(
        std::uint64_t generation, std::int64_t timestampSamples,
        std::vector<SceneObjectSnapshot> *snapshot) const;

    std::uint64_t generation() const { return generation_; }
    std::size_t objectCount() const { return ramps_.size(); }

private:
    struct Ramp {
        ObjectMetadataUpdate start;
        ObjectMetadataUpdate target;
        std::array<double, 3> startCartesian {};
        std::array<double, 3> targetCartesian {};
        std::int64_t endSamples = 0;
    };

    std::uint64_t generation_ = kInitialGeneration;
    std::int64_t lastTimestamp_ = -1;
    std::map<std::uint64_t, Ramp> ramps_;
    std::optional<SceneObjectCoordinatePolicy> policy_;
};

} // namespace eac3render
