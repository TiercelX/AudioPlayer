#pragma once

#include "scene-adapter.h"

#include "../atmos-joc-probe/joc-gate6c.h"

#include <array>
#include <cstdint>
#include <string>

namespace eac3render {

// B2bPosition is an OAMD-normalized room/standard position, not a renderer
// UnitVector3.  The decoder does not carry the BS.2127 cartesian flag, so a
// caller must select the representation explicitly and, for the bounded R1A
// point path, provide an already-converted renderer-space unit direction.
enum class ObjectCoordinateRepresentation {
    Unknown,
    OamdCartesian,
    OamdPolar,
    RendererUnitDirection,
};

const char *objectCoordinateRepresentationName(
    ObjectCoordinateRepresentation value);

const char *objectExtentPresenceName(ObjectExtentPresence value);

struct SceneObjectPropertyContext {
    std::uint64_t generation = kInitialGeneration;
    ObjectCoordinateRepresentation coordinate =
        ObjectCoordinateRepresentation::Unknown;
    // Required because B2bPosition's normalized room coordinates do not
    // uniquely define a listener-relative UnitVector3 without a scene/room
    // policy.  This is an explicit conversion boundary, never an inference.
    bool pointDirectionPresent = false;
    UnitVector3 pointDirection {0.0, 0.0, 1.0};
    // Gate6C carries ramp duration but not jumpPosition.  Keep this absent
    // by default; callers with an independently decoded jump flag may pass it
    // explicitly for the R1A point contract.
    bool jumpPositionPresent = false;
    bool jumpPosition = false;
    ObjectWarpMode warpMode = ObjectWarpMode::None;
    ObjectTrimMetadata trim;
    bool divergencePresent = false;
    bool divergenceReused = false;
    unsigned divergenceIndex = 0U;
    float divergence = 0.0F;
};

struct SceneObjectCoordinatePolicy {
    double roomWidthMetres = 0.0;
    double roomDepthMetres = 0.0;
    double roomHeightMetres = 0.0;
    double listenerX = 0.0;
    double listenerY = 0.0;
    double listenerZ = 0.0;
};

struct SceneObjectProperties {
    std::uint64_t generation = kInitialGeneration;
    std::uint64_t objectId = 0U;
    unsigned blockIndex = 0U;
    std::int64_t timestampSamples = 0;
    std::int64_t durationSamples = 0;

    ObjectCoordinateRepresentation coordinate =
        ObjectCoordinateRepresentation::Unknown;
    bool cartesianFlagKnown = false;
    bool cartesian = false;
    std::array<float, 3> codedPosition {0.5F, 0.5F, 0.0F};
    std::array<float, 3> standardPosition {0.5F, 0.5F, 0.0F};
    std::array<float, 3> roomPosition {0.5F, 0.5F, 0.0F};
    bool screenAnchored = false;
    bool distanceSpecified = false;
    bool distanceInfinite = false;
    float distanceFactor = 0.0F;
    float screenFactor = 0.0F;
    float depthFactor = 0.0F;
    std::array<bool, 3> extendedPrecisionPresent {};
    std::array<int, 3> extendedPrecision {};

    bool pointDirectionPresent = false;
    UnitVector3 pointDirection {0.0, 0.0, 1.0};

    bool active = false;
    bool gainMinusInfinity = true;
    float gainDb = 0.0F;
    float gainLinear = 0.0F;
    float priority = 0.0F;
    std::array<bool, 6> zoneConstraints {true, true, true, true, true, true};
    bool elevation = true;
    bool snap = false;

    ObjectExtent extent;
    ObjectExtentPresence extentPresence = ObjectExtentPresence::Absent;
    unsigned sourceSizeIndex = 0U;
    bool extentPresenceDecoded = false;

    bool jumpPositionPresent = false;
    bool jumpPosition = false;
    bool diffusePresent = false;
    std::uint32_t unsupportedProperties = UnsupportedDiffuse
        | UnsupportedDivergence | UnsupportedJumpPosition;
    bool completeObjectsSupported = false;
    ObjectWarpMode warpMode = ObjectWarpMode::None;
    ObjectTrimMetadata trim;
    bool divergencePresent = false;
    bool divergenceReused = false;
    unsigned divergenceIndex = 0U;
    float divergence = 0.0F;
};

bool convertRoomPositionToUnitDirection(
    const SceneObjectProperties &properties,
    const SceneObjectCoordinatePolicy &policy,
    UnitVector3 *direction,
    std::string *reason = nullptr);

enum class SceneObjectPropertyDisposition {
    AcceptedPoint,
    ExtentPending,
    Unsupported,
    Malformed,
};

const char *sceneObjectPropertyDispositionName(
    SceneObjectPropertyDisposition value);

struct SceneObjectPropertyResult {
    SceneObjectPropertyDisposition disposition =
        SceneObjectPropertyDisposition::Malformed;
    std::string reason;
    SceneObjectProperties properties;

    bool acceptedPoint() const
    {
        return disposition == SceneObjectPropertyDisposition::AcceptedPoint;
    }

    bool extentPending() const
    {
        return disposition == SceneObjectPropertyDisposition::ExtentPending;
    }
};

// Converts one Gate6C/J0 metadata record into a renderer-neutral property
// snapshot.  It does not perform extent math.  Non-zero extent is retained as
// ExtentPending and cannot be silently passed to SceneAdapter's point-only
// input.  Missing diffuse/divergence are explicit bounded-support flags.
SceneObjectPropertyResult adaptGate6cObject(
    const eac3gate6c::MetadataUpdate &metadata,
    const SceneObjectPropertyContext &context);

// Only an AcceptedPoint result (including Absent or ExplicitZero extent) can
// be converted into R1A's point-only input.  ExtentPending is rejected.
bool toSceneAdapterObjectUpdate(const SceneObjectPropertyResult &result,
                                ObjectMetadataUpdate *update,
                                std::string *reason = nullptr);

} // namespace eac3render
