#pragma once

#include "bs2127-system-h-configured-panner.h"
#include "bs2127-system-h-cartesian-panner.h"
#include "scene-model.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace eac3render {

enum class ObjectExtentPresence { Absent, ExplicitZero, NonZero };

struct ObjectExtent {
    float width = 0.0F;
    float height = 0.0F;
    float depth = 0.0F;
};

enum ObjectPropertyUnsupported : std::uint32_t {
    UnsupportedNone = 0U,
    UnsupportedDiffuse = 1U << 0U,
    UnsupportedDivergence = 1U << 1U,
    UnsupportedJumpPosition = 1U << 2U,
};

enum class MetadataGapPolicy {
    HoldLast,
    Terminate,
};

enum class ObjectWarpMode { None, PositionY2, Reserved };

struct ObjectTrimMetadata {
    bool present = false;
    bool enabled = true;
    unsigned mode = 0U;
    float centre = 0.0F;
    float surround = 0.0F;
    float height = 0.0F;
    float balance = 0.0F;
};

struct BedGainUpdate {
    BedLabel label = BedLabel::FL;
    float gain = 0.0F;
};

struct ObjectMetadataUpdate {
    std::uint64_t objectId = 0U;
    std::int64_t timestampSamples = 0;
    float gain = 1.0F;
    UnitVector3 position {0.0, 0.0, 1.0};
    // Position jumps at this timestamp instead of interpolating from the
    // preceding position. Otherwise position components and gain are linearly
    // interpolated; the interpolated direction is projected back to unit
    // length before the point-source panner is called.
    bool jumpPosition = false;
    enum class CoordinateSpace {
        UnitDirection,
        EtsiRoomCartesian,
    };
    CoordinateSpace coordinateSpace = CoordinateSpace::UnitDirection;
    // Used only for EtsiRoomCartesian. Listener centre is a valid allocentric
    // point and must not be replaced by a fabricated unit direction.
    std::array<float, 3> roomPosition {0.5F, 0.5F, 0.0F};
    // Retained renderer-neutral metadata. BS.2127 point panning defines no
    // priority-dependent gain rule, so this must not alter amplitude.
    float priority = 0.0F;
    // TS 103 420 renderer-neutral carriage. These fields are deliberately
    // not interpreted by the BS.2127 point-source gain path.
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

struct SceneAdapterUpdate {
    std::uint64_t generation = kInitialGeneration;
    std::int64_t timestampSamples = 0;
    std::int64_t durationSamples = 0;
    std::vector<BedGainUpdate> beds;
    std::vector<ObjectMetadataUpdate> objects;
    bool lfeEnabled = false;
    float lfeGain = 0.0F;
};

struct ObjectGainFrame {
    std::uint64_t objectId = 0U;
    float gain = 0.0F;
    UnitVector3 position {0.0, 0.0, 1.0};
    ObjectMetadataUpdate::CoordinateSpace coordinateSpace =
        ObjectMetadataUpdate::CoordinateSpace::UnitDirection;
    std::array<float, 3> roomPosition {0.5F, 0.5F, 0.0F};
    float priority = 0.0F;
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
    std::uint32_t unsupportedProperties = 0U;
    bool speakerAnchorPresent = false;
    BedLabel speakerAnchor = BedLabel::FL;
    ObjectWarpMode warpMode = ObjectWarpMode::None;
    ObjectTrimMetadata trim;
    bool divergencePresent = false;
    bool divergenceReused = false;
    unsigned divergenceIndex = 0U;
    float divergence = 0.0F;
    bool snapApplied = false;
    bool zoneFiltered = false;
    std::array<double, kSystemHSpeakerCount> speakerGains {};
    bool interpolated = false;
    bool held = false;
};

struct SceneAdapterFrame {
    std::uint64_t generation = kInitialGeneration;
    std::int64_t timestampSamples = 0;
    std::int64_t durationSamples = 0;
    std::array<double, kSystemHSpeakerCount> bedGains {};
    std::array<double, kSystemHSpeakerCount> speakerGains {};
    std::vector<ObjectGainFrame> objects;
    bool lfeEnabled = false;
    float lfeGain = 0.0F;
    bool metadataHeld = false;
    bool metadataTerminated = false;
    bool extentApplied = false;
};

enum class SceneAdapterDisposition {
    Accepted,
    InvalidInput,
    Unsupported,
    Stale,
};

struct SceneAdapterResult {
    SceneAdapterDisposition disposition = SceneAdapterDisposition::InvalidInput;
    std::string reason;
    SceneAdapterFrame frame;

    bool accepted() const { return disposition == SceneAdapterDisposition::Accepted; }
};

const char *sceneAdapterDispositionName(SceneAdapterDisposition disposition);
const char *metadataGapPolicyName(MetadataGapPolicy policy);

// Renderer-neutral bridge from decoded bed/object scene updates to the
// fixed System H R0D point-source gain engine. A missing object update in a
// pushed block is recorded as a historical half-open metadata-gap interval:
// HoldLast retains the preceding state inside that interval and Terminate
// removes it. Reappearance does not erase old intervals. It emits gain frames
// only: no PCM mixing, extent, SOFA/BRIR, playback, or DRC is performed here.
class SceneAdapter final {
public:
    explicit SceneAdapter(
        MetadataGapPolicy gapPolicy = MetadataGapPolicy::HoldLast,
        const Bs2127TopologyOptions &pannerOptions = {});

    SceneAdapterResult reset(std::uint64_t generation);
    SceneAdapterResult pushUpdate(const SceneAdapterUpdate &update);
    SceneAdapterResult renderAt(std::uint64_t generation,
                                std::int64_t timestampSamples) const;

    bool valid() const { return panner_.valid(); }
    const std::string &reason() const { return panner_.reason(); }
    std::uint64_t generation() const { return generation_; }
    MetadataGapPolicy gapPolicy() const { return gapPolicy_; }
    std::size_t updateCount() const { return updateCount_; }

    static bool bedSpeakerIndex(BedLabel label, std::size_t *speakerIndex,
                                std::string *reason = nullptr);

private:
    struct GapInterval {
        std::int64_t startSamples = 0;
        std::int64_t endSamples = 0;
    };

    struct StoredObject {
        std::vector<ObjectMetadataUpdate> points;
        std::vector<GapInterval> gaps;
    };

    SceneAdapterResult makeFailure(SceneAdapterDisposition disposition,
                                   const char *reason) const;
    SceneAdapterResult renderInternal(std::uint64_t generation,
                                      std::int64_t timestampSamples,
                                      std::int64_t durationSamples) const;

    MetadataGapPolicy gapPolicy_ = MetadataGapPolicy::HoldLast;
    Bs2127SystemHConfiguredPanner panner_;
    std::uint64_t generation_ = kInitialGeneration;
    std::int64_t lastUpdateTimestamp_ = -1;
    std::array<float, 8> bedGains_ {};
    bool lfeEnabled_ = false;
    float lfeGain_ = 0.0F;
    std::vector<BedLabel> seenBeds_;
    std::vector<std::pair<std::uint64_t, StoredObject>> objects_;
    std::size_t updateCount_ = 0U;
};

} // namespace eac3render
