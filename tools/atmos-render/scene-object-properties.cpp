#include "scene-object-properties.h"

#include <cmath>
#include <limits>

namespace eac3render {
namespace {

constexpr unsigned kFirstDynamicObject = 1U;
constexpr unsigned kLastDynamicObject = eac3gate6c::kDynamicObjectCount;
constexpr double kPositionTolerance = 1.0e-6;

bool finite(float value)
{
    return std::isfinite(static_cast<double>(value));
}

bool finite(const UnitVector3 &value)
{
    for (double component : value) {
        if (!std::isfinite(component)) return false;
    }
    return true;
}

bool unitVector(const UnitVector3 &value)
{
    if (!finite(value)) return false;
    double norm = 0.0;
    for (double component : value) norm += component * component;
    return std::abs(norm - 1.0) <= 1.0e-12;
}

bool inRange(float value, float low, float high)
{
    return finite(value) && value >= low - static_cast<float>(kPositionTolerance)
        && value <= high + static_cast<float>(kPositionTolerance);
}

void setFailure(SceneObjectPropertyResult *result,
                SceneObjectPropertyDisposition disposition,
                const char *reason)
{
    result->disposition = disposition;
    result->reason = reason;
}

bool allZero(const ObjectExtent &extent)
{
    return extent.width == 0.0F && extent.height == 0.0F
        && extent.depth == 0.0F;
}

bool validExtent(const ObjectExtent &extent)
{
    return finite(extent.width) && finite(extent.height)
        && finite(extent.depth) && extent.width >= 0.0F
        && extent.width <= 1.0F && extent.height >= 0.0F
        && extent.height <= 1.0F && extent.depth >= 0.0F
        && extent.depth <= 1.0F;
}

bool validCoordinateRepresentation(ObjectCoordinateRepresentation value)
{
    return value == ObjectCoordinateRepresentation::OamdCartesian
        || value == ObjectCoordinateRepresentation::OamdPolar
        || value == ObjectCoordinateRepresentation::RendererUnitDirection;
}

} // namespace

bool convertRoomPositionToUnitDirection(
    const SceneObjectProperties &properties,
    const SceneObjectCoordinatePolicy &policy,
    UnitVector3 *direction,
    std::string *reason)
{
    if (!direction) {
        if (reason) *reason = "null-point-direction";
        return false;
    }
    if (properties.coordinate != ObjectCoordinateRepresentation::OamdCartesian
        || !properties.cartesianFlagKnown || !properties.cartesian) {
        if (reason) *reason = "cartesian-coordinate-required";
        return false;
    }
    if (properties.screenAnchored) {
        if (reason) *reason = "screen-position-requires-screen-geometry";
        return false;
    }
    if (properties.distanceInfinite) {
        if (reason) *reason = "infinite-distance-unsupported";
        return false;
    }
    const auto &position = properties.roomPosition;
    if (!finite(position[0]) || !finite(position[1]) || !finite(position[2])
        || !finite(policy.roomWidthMetres) || !finite(policy.roomDepthMetres)
        || !finite(policy.roomHeightMetres) || policy.roomWidthMetres <= 0.0
        || policy.roomDepthMetres <= 0.0 || policy.roomHeightMetres <= 0.0
        || !finite(policy.listenerX) || !finite(policy.listenerY)
        || !finite(policy.listenerZ) || policy.listenerX < 0.0
        || policy.listenerX > 1.0 || policy.listenerY < 0.0
        || policy.listenerY > 1.0 || policy.listenerZ < -1.0
        || policy.listenerZ > 1.0) {
        if (reason) *reason = "invalid-coordinate-policy-or-position";
        return false;
    }
    // ETSI room axes are left-handed: X left/right, Y front/back, Z floor/ceiling.
    // Windows Spatial Audio is right-handed listener-relative: +X right, +Y up,
    // +Z behind. The explicit mapping is X/Z/Y; no second-axis negation or clamp.
    const double roomX = static_cast<double>(position[0]) * policy.roomWidthMetres;
    const double roomY = static_cast<double>(position[1]) * policy.roomDepthMetres;
    const double roomZ = (static_cast<double>(position[2]) + 1.0) * 0.5
        * policy.roomHeightMetres;
    const double listenerX = policy.listenerX * policy.roomWidthMetres;
    const double listenerY = policy.listenerY * policy.roomDepthMetres;
    const double listenerZ = (policy.listenerZ + 1.0) * 0.5
        * policy.roomHeightMetres;
    // SceneAdapter/System H UnitVector3 is [front, right, up].
    UnitVector3 value {listenerY - roomY, roomX - listenerX, roomZ - listenerZ};
    double norm = 0.0;
    for (double component : value) {
        if (!std::isfinite(component)) {
            if (reason) *reason = "nonfinite-listener-relative-position";
            return false;
        }
        norm += component * component;
    }
    if (!(norm > 0.0) || !std::isfinite(norm)) {
        if (reason) *reason = "zero-listener-relative-position";
        return false;
    }
    const double scale = 1.0 / std::sqrt(norm);
    for (double &component : value) component *= scale;
    *direction = value;
    return true;
}

const char *objectCoordinateRepresentationName(
    ObjectCoordinateRepresentation value)
{
    switch (value) {
    case ObjectCoordinateRepresentation::Unknown: return "unknown";
    case ObjectCoordinateRepresentation::OamdCartesian: return "oamd-cartesian";
    case ObjectCoordinateRepresentation::OamdPolar: return "oamd-polar";
    case ObjectCoordinateRepresentation::RendererUnitDirection:
        return "renderer-unit-direction";
    }
    return "unknown";
}

const char *objectExtentPresenceName(ObjectExtentPresence value)
{
    switch (value) {
    case ObjectExtentPresence::Absent: return "absent";
    case ObjectExtentPresence::ExplicitZero: return "explicit-zero";
    case ObjectExtentPresence::NonZero: return "nonzero";
    }
    return "absent";
}

const char *sceneObjectPropertyDispositionName(
    SceneObjectPropertyDisposition value)
{
    switch (value) {
    case SceneObjectPropertyDisposition::AcceptedPoint: return "ACCEPTED_POINT";
    case SceneObjectPropertyDisposition::ExtentPending: return "EXTENT_PENDING";
    case SceneObjectPropertyDisposition::Unsupported: return "UNSUPPORTED";
    case SceneObjectPropertyDisposition::Malformed: return "MALFORMED";
    }
    return "MALFORMED";
}

SceneObjectPropertyResult adaptGate6cObject(
    const eac3gate6c::MetadataUpdate &metadata,
    const SceneObjectPropertyContext &context)
{
    SceneObjectPropertyResult result;
    SceneObjectProperties &properties = result.properties;
    properties.generation = context.generation;

    if (context.generation == 0U) {
        setFailure(&result, SceneObjectPropertyDisposition::Malformed,
                   "invalid-generation");
        return result;
    }
    if (metadata.objectIndex < kFirstDynamicObject
        || metadata.objectIndex > kLastDynamicObject
        || metadata.state.objectIndex != metadata.objectIndex) {
        setFailure(&result, SceneObjectPropertyDisposition::Malformed,
                   "object-index-mismatch-or-out-of-range");
        return result;
    }
    if (metadata.sourcePosition < 0) {
        setFailure(&result, SceneObjectPropertyDisposition::Malformed,
                   "negative-source-position");
        return result;
    }
    const auto &state = metadata.state;
    if (!state.position.valid || !state.basicValid || !state.renderValid) {
        setFailure(&result, SceneObjectPropertyDisposition::Malformed,
                   "incomplete-b2b-object-state");
        return result;
    }
    const auto &position = state.position;
    if (!finite(position.x) || !finite(position.y) || !finite(position.z)) {
        setFailure(&result, SceneObjectPropertyDisposition::Malformed,
                   "nonfinite-effective-room-position");
        return result;
    }
    if (!inRange(position.codedX, 0.0F, 1.0F)
        || !inRange(position.codedY, 0.0F, 1.0F)
        || !inRange(position.codedZ, -1.0F, 1.0F)
        || !inRange(position.standardX, 0.0F, 1.0F)
        || !inRange(position.standardY, 0.0F, 1.0F)
        || !inRange(position.standardZ, -1.0F, 1.0F)) {
        setFailure(&result, SceneObjectPropertyDisposition::Malformed,
                   "b2b-position-out-of-range");
        return result;
    }
    if ((!state.gainMinusInfinity && !finite(state.gainDb))
        || (state.gainMinusInfinity && !finite(state.gainDb)
            && !(std::isinf(static_cast<double>(state.gainDb))
                 && state.gainDb < 0.0F))
        || !finite(state.priority) || state.priority < 0.0F
        || state.priority > 1.0F
        || !validExtent(ObjectExtent {
            state.size[0], state.size[2], state.size[1]})) {
        setFailure(&result, SceneObjectPropertyDisposition::Malformed,
                   "nonfinite-or-out-of-range-object-property");
        return result;
    }
    if (state.effectiveSizeIndex > 2U) {
        setFailure(&result, SceneObjectPropertyDisposition::Malformed,
                   "reserved-size-index");
        return result;
    }
    if (!finite(context.pointDirection) ||
        (context.pointDirectionPresent && !unitVector(context.pointDirection))) {
        setFailure(&result, SceneObjectPropertyDisposition::Malformed,
                   "point-direction-not-unit-finite");
        return result;
    }
    if (!validCoordinateRepresentation(context.coordinate)) {
        setFailure(&result, SceneObjectPropertyDisposition::Unsupported,
                   "coordinate-representation-required");
        return result;
    }
    if (context.warpMode == ObjectWarpMode::Reserved
        || !finite(context.divergence) || context.divergence < 0.0F
        || context.divergence > 1.0F) {
        setFailure(&result, SceneObjectPropertyDisposition::Unsupported,
                   context.warpMode == ObjectWarpMode::Reserved
                       ? "reserved-warp-mode" : "divergence-out-of-range");
        return result;
    }
    if (!context.pointDirectionPresent
        && context.coordinate != ObjectCoordinateRepresentation::OamdCartesian) {
        setFailure(&result, SceneObjectPropertyDisposition::Unsupported,
                   "point-direction-conversion-required");
        return result;
    }

    properties.objectId = metadata.objectIndex;
    properties.blockIndex = metadata.blockIndex;
    properties.timestampSamples = metadata.sourcePosition;
    properties.durationSamples = static_cast<std::int64_t>(metadata.rampDuration);
    properties.coordinate = context.coordinate;
    properties.cartesianFlagKnown = context.coordinate
        == ObjectCoordinateRepresentation::OamdCartesian
        || context.coordinate == ObjectCoordinateRepresentation::OamdPolar;
    properties.cartesian = context.coordinate
        == ObjectCoordinateRepresentation::OamdCartesian;
    properties.codedPosition = {position.codedX, position.codedY, position.codedZ};
    properties.standardPosition = {
        position.standardX, position.standardY, position.standardZ};
    properties.roomPosition = {position.x, position.y, position.z};
    properties.screenAnchored = position.screenAnchored;
    properties.distanceSpecified = position.distanceSpecified;
    properties.distanceInfinite = position.distanceInfinite;
    properties.distanceFactor = position.distanceFactor;
    properties.screenFactor = position.screenFactor;
    properties.depthFactor = position.depthFactor;
    properties.extendedPrecisionPresent = position.extendedPrecisionPresent;
    properties.extendedPrecision = position.extendedPrecision;
    properties.pointDirectionPresent = context.pointDirectionPresent;
    properties.pointDirection = context.pointDirection;
    properties.active = state.active;
    properties.gainMinusInfinity = state.gainMinusInfinity;
    properties.gainDb = state.gainDb;
    properties.gainLinear = (!state.active || state.gainMinusInfinity)
        ? 0.0F
        : static_cast<float>(std::pow(10.0, static_cast<double>(state.gainDb) / 20.0));
    if (!finite(properties.gainLinear) || properties.gainLinear < 0.0F) {
        setFailure(&result, SceneObjectPropertyDisposition::Malformed,
                   "gain-db-to-linear-overflow");
        return result;
    }
    properties.priority = state.priority;
    properties.zoneConstraints = state.zoneConstraints;
    properties.elevation = state.elevation;
    properties.snap = state.snap;
    properties.jumpPositionPresent = context.jumpPositionPresent;
    properties.jumpPosition = context.jumpPosition;
    properties.warpMode = context.warpMode;
    properties.trim = context.trim;
    properties.divergencePresent = context.divergencePresent;
    properties.divergenceReused = context.divergenceReused;
    properties.divergenceIndex = context.divergenceIndex;
    properties.divergence = context.divergence;
    properties.extent = {state.size[0], state.size[2], state.size[1]};
    properties.sourceSizeIndex = state.effectiveSizeIndex;
    properties.extentPresenceDecoded = state.effectiveSizePresent;
    if (!state.effectiveSizePresent) {
        if (!allZero(properties.extent)) {
            setFailure(&result, SceneObjectPropertyDisposition::Malformed,
                       "size-nonzero-without-size-presence");
            return result;
        }
        properties.extentPresence = ObjectExtentPresence::Absent;
    } else if (allZero(properties.extent)) {
        properties.extentPresence = ObjectExtentPresence::ExplicitZero;
    } else {
        properties.extentPresence = ObjectExtentPresence::NonZero;
    }

    if (properties.extentPresence == ObjectExtentPresence::NonZero) {
        result.disposition = SceneObjectPropertyDisposition::ExtentPending;
        result.reason = "nonzero-extent-pending-no-extent-math";
    } else {
        result.disposition = SceneObjectPropertyDisposition::AcceptedPoint;
        result.reason = "bounded-point-contract-missing-diffuse-divergence";
    }
    if (properties.jumpPositionPresent) {
        properties.unsupportedProperties &= ~UnsupportedJumpPosition;
    }
    return result;
}

bool toSceneAdapterObjectUpdate(const SceneObjectPropertyResult &result,
                                ObjectMetadataUpdate *update,
                                std::string *reason)
{
    if (!update) {
        if (reason) *reason = "null-object-update";
        return false;
    }
    if (!result.acceptedPoint()) {
        if (reason) {
            *reason = result.extentPending()
                ? "nonzero-extent-pending-no-extent-math"
                : (result.reason.empty() ? "property-contract-not-accepted"
                                         : result.reason);
        }
        return false;
    }
    if (!result.properties.pointDirectionPresent
        && result.properties.coordinate
            != ObjectCoordinateRepresentation::OamdCartesian) {
        if (reason) *reason = "point-direction-conversion-required";
        return false;
    }
    *update = ObjectMetadataUpdate {
        result.properties.objectId,
        result.properties.timestampSamples,
        result.properties.gainLinear,
        result.properties.pointDirection,
        result.properties.jumpPosition,
    };
    update->extent = result.properties.extent;
    update->extentPresence = result.properties.extentPresence;
    update->sourceSizeIndex = result.properties.sourceSizeIndex;
    update->zoneConstraints = result.properties.zoneConstraints;
    update->elevation = result.properties.elevation;
    update->snap = result.properties.snap;
    update->screenAnchored = result.properties.screenAnchored;
    update->distanceSpecified = result.properties.distanceSpecified;
    update->distanceInfinite = result.properties.distanceInfinite;
    update->distanceFactor = result.properties.distanceFactor;
    update->screenFactor = result.properties.screenFactor;
    update->depthFactor = result.properties.depthFactor;
    update->extendedPrecisionPresent = result.properties.extendedPrecisionPresent;
    update->extendedPrecision = result.properties.extendedPrecision;
    update->unsupportedProperties = result.properties.unsupportedProperties;
    update->warpMode = result.properties.warpMode;
    update->trim = result.properties.trim;
    update->divergencePresent = result.properties.divergencePresent;
    update->divergenceReused = result.properties.divergenceReused;
    update->divergenceIndex = result.properties.divergenceIndex;
    update->divergence = result.properties.divergence;
    if (result.properties.coordinate
        == ObjectCoordinateRepresentation::OamdCartesian) {
        update->coordinateSpace =
            ObjectMetadataUpdate::CoordinateSpace::EtsiRoomCartesian;
        update->roomPosition = result.properties.roomPosition;
    }
    update->priority = result.properties.priority;
    return true;
}

} // namespace eac3render
