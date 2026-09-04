#include "scene-object-properties.h"
#include "scene-object-bridge.h"
#include "scene-object-ramp-scheduler.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <string>

namespace {

struct Report {
    unsigned cases = 0U;
    unsigned failures = 0U;
};

void expect(Report *report, bool condition, const char *name)
{
    ++report->cases;
    if (!condition) {
        ++report->failures;
        std::cerr << "FAIL case=" << name << '\n';
    }
}

eac3gate6c::MetadataUpdate baseMetadata()
{
    eac3gate6c::MetadataUpdate metadata;
    metadata.sourcePosition = 96000;
    metadata.blockIndex = 3U;
    metadata.rampDuration = 128U;
    metadata.objectIndex = 4U;
    metadata.state.objectIndex = 4U;
    metadata.state.active = true;
    metadata.state.basicValid = true;
    metadata.state.renderValid = true;
    metadata.state.position.valid = true;
    metadata.state.position.codedX = 0.75F;
    metadata.state.position.codedY = 0.25F;
    metadata.state.position.codedZ = 0.5F;
    metadata.state.position.x = 0.75F;
    metadata.state.position.y = 0.25F;
    metadata.state.position.z = 0.5F;
    metadata.state.position.standardX = 0.75F;
    metadata.state.position.standardY = 0.25F;
    metadata.state.position.standardZ = 0.5F;
    metadata.state.gainDb = -6.0F;
    metadata.state.gainMinusInfinity = false;
    metadata.state.priority = 0.5F;
    metadata.state.zoneConstraints = {true, false, true, false, true, false};
    metadata.state.elevation = false;
    metadata.state.snap = true;
    metadata.state.raw.sizePresent = false;
    metadata.state.size = {0.0F, 0.0F, 0.0F};
    metadata.state.effectiveSizePresent = false;
    metadata.state.effectiveSizeIndex = 0U;
    return metadata;
}

eac3render::SceneObjectPropertyContext baseContext()
{
    eac3render::SceneObjectPropertyContext context;
    context.generation = 9U;
    context.coordinate = eac3render::ObjectCoordinateRepresentation::RendererUnitDirection;
    context.pointDirectionPresent = true;
    context.pointDirection = {1.0, 0.0, 0.0};
    return context;
}

bool same(const eac3render::SceneObjectProperties &left,
          const eac3render::SceneObjectProperties &right)
{
    return left.generation == right.generation
        && left.objectId == right.objectId
        && left.timestampSamples == right.timestampSamples
        && left.durationSamples == right.durationSamples
        && left.coordinate == right.coordinate
        && left.codedPosition == right.codedPosition
        && left.standardPosition == right.standardPosition
        && left.pointDirection == right.pointDirection
        && left.gainLinear == right.gainLinear
        && left.priority == right.priority
        && left.zoneConstraints == right.zoneConstraints
        && left.elevation == right.elevation
        && left.snap == right.snap
        && left.extent.width == right.extent.width
        && left.extent.height == right.extent.height
        && left.extent.depth == right.extent.depth
        && left.extentPresence == right.extentPresence;
}

} // namespace

int main()
{
    Report report;
    auto metadata = baseMetadata();
    const auto context = baseContext();
    const auto point = eac3render::adaptGate6cObject(metadata, context);
    expect(&report, point.acceptedPoint()
                        && point.properties.generation == 9U
                        && point.properties.objectId == 4U
                        && point.properties.timestampSamples == 96000
                        && point.properties.durationSamples == 128
                        && point.properties.coordinate
                            == eac3render::ObjectCoordinateRepresentation::RendererUnitDirection,
           "point-contract-and-timeline");
    expect(&report, std::abs(point.properties.gainLinear - 0.5011872F) < 1.0e-5F,
           "db-to-linear-gain");
    expect(&report, point.properties.priority == 0.5F
                        && point.properties.zoneConstraints
                            == std::array<bool, 6>({true, false, true, false, true, false})
                        && !point.properties.elevation && point.properties.snap,
           "priority-zone-elevation-snap-contract");

    auto inactive = metadata;
    inactive.state.active = false;
    const auto inactiveResult = eac3render::adaptGate6cObject(inactive, context);
    expect(&report, inactiveResult.acceptedPoint()
                        && inactiveResult.properties.gainLinear == 0.0F,
           "inactive-gain-is-silent");
    eac3render::ObjectMetadataUpdate sceneUpdate;
    std::string reason;
    expect(&report, eac3render::toSceneAdapterObjectUpdate(
                        point, &sceneUpdate, &reason)
                        && sceneUpdate.objectId == 4U
                        && sceneUpdate.timestampSamples == 96000
                        && sceneUpdate.position == context.pointDirection
                        && sceneUpdate.priority == point.properties.priority
                        && sceneUpdate.zoneConstraints == point.properties.zoneConstraints
                        && sceneUpdate.elevation == point.properties.elevation
                        && sceneUpdate.snap == point.properties.snap
                        && sceneUpdate.extentPresence == point.properties.extentPresence
                        && sceneUpdate.distanceSpecified == point.properties.distanceSpecified
                        && sceneUpdate.unsupportedProperties
                               == point.properties.unsupportedProperties,
           "r1a-point-adapter-input-and-metadata-carriage");

    auto infinity = metadata;
    infinity.state.gainMinusInfinity = true;
    infinity.state.gainDb = -std::numeric_limits<float>::infinity();
    const auto infinityResult = eac3render::adaptGate6cObject(infinity, context);
    expect(&report, infinityResult.acceptedPoint()
                        && infinityResult.properties.gainLinear == 0.0F,
           "minus-infinity-gain-zero");

    auto positiveInfinity = metadata;
    positiveInfinity.state.gainMinusInfinity = true;
    positiveInfinity.state.gainDb = std::numeric_limits<float>::infinity();
    const auto positiveInfinityResult = eac3render::adaptGate6cObject(
        positiveInfinity, context);
    expect(&report, positiveInfinityResult.disposition
                        == eac3render::SceneObjectPropertyDisposition::Malformed,
           "positive-infinity-gain-fails-closed");

    auto nonfiniteGain = metadata;
    nonfiniteGain.state.gainDb = std::numeric_limits<float>::quiet_NaN();
    const auto nonfiniteGainResult = eac3render::adaptGate6cObject(
        nonfiniteGain, context);
    expect(&report, nonfiniteGainResult.disposition
                        == eac3render::SceneObjectPropertyDisposition::Malformed,
           "nonfinite-gain-fails-closed");

    auto explicitZero = metadata;
    explicitZero.state.raw.sizePresent = true;
    explicitZero.state.raw.sizeIndex = 0U;
    explicitZero.state.effectiveSizePresent = true;
    explicitZero.state.effectiveSizeIndex = 0U;
    const auto zeroResult = eac3render::adaptGate6cObject(explicitZero, context);
    expect(&report, zeroResult.acceptedPoint()
                        && zeroResult.properties.extentPresence
                            == eac3render::ObjectExtentPresence::ExplicitZero,
           "explicit-zero-size-is-not-absent");

    auto nonzero = metadata;
    nonzero.state.raw.sizePresent = true;
    nonzero.state.raw.sizeIndex = 2U;
    nonzero.state.raw.widthBits = 10U;
    nonzero.state.raw.depthBits = 20U;
    nonzero.state.raw.heightBits = 30U;
    nonzero.state.size = {10.0F / 31.0F, 20.0F / 31.0F, 30.0F / 31.0F};
    nonzero.state.effectiveSizePresent = true;
    nonzero.state.effectiveSizeIndex = 2U;
    const auto extentResult = eac3render::adaptGate6cObject(nonzero, context);
    expect(&report, extentResult.extentPending()
                        && extentResult.properties.extentPresence
                            == eac3render::ObjectExtentPresence::NonZero
                        && std::abs(extentResult.properties.extent.width
                                    - 10.0F / 31.0F) < 1.0e-6F
                        && std::abs(extentResult.properties.extent.depth
                                    - 20.0F / 31.0F) < 1.0e-6F
                        && std::abs(extentResult.properties.extent.height
                                    - 30.0F / 31.0F) < 1.0e-6F,
           "size-order-and-nonzero-pending");
    reason.clear();
    expect(&report, !eac3render::toSceneAdapterObjectUpdate(
                        extentResult, &sceneUpdate, &reason)
                        && reason == "nonzero-extent-pending-no-extent-math",
           "pending-extent-not-silently-dropped");

    auto presenceMismatch = nonzero;
    presenceMismatch.state.raw.sizePresent = false;
    presenceMismatch.state.effectiveSizePresent = false;
    const auto mismatchResult = eac3render::adaptGate6cObject(
        presenceMismatch, context);
    expect(&report, mismatchResult.disposition
                        == eac3render::SceneObjectPropertyDisposition::Malformed
                        && mismatchResult.reason
                            == "size-nonzero-without-size-presence",
           "size-presence-mismatch-fails-closed");

    auto missingCoordinate = metadata;
    auto unknownContext = context;
    unknownContext.coordinate = eac3render::ObjectCoordinateRepresentation::Unknown;
    const auto unknownResult = eac3render::adaptGate6cObject(
        missingCoordinate, unknownContext);
    expect(&report, unknownResult.disposition
                        == eac3render::SceneObjectPropertyDisposition::Unsupported
                        && unknownResult.reason == "coordinate-representation-required",
           "unknown-coordinate-unsupported");
    auto noDirectionContext = context;
    noDirectionContext.pointDirectionPresent = false;
    const auto noDirection = eac3render::adaptGate6cObject(metadata, noDirectionContext);
    expect(&report, noDirection.disposition
                        == eac3render::SceneObjectPropertyDisposition::Unsupported
                        && noDirection.reason == "point-direction-conversion-required",
           "room-position-needs-explicit-point-conversion");

    auto invalidCoordinateContext = context;
    invalidCoordinateContext.coordinate =
        static_cast<eac3render::ObjectCoordinateRepresentation>(99);
    const auto invalidCoordinate = eac3render::adaptGate6cObject(
        metadata, invalidCoordinateContext);
    expect(&report, invalidCoordinate.disposition
                        == eac3render::SceneObjectPropertyDisposition::Unsupported
                        && invalidCoordinate.reason
                            == "coordinate-representation-required",
           "invalid-coordinate-unsupported");

    auto jumpContext = context;
    jumpContext.jumpPositionPresent = true;
    jumpContext.jumpPosition = true;
    const auto jump = eac3render::adaptGate6cObject(metadata, jumpContext);
    expect(&report, jump.acceptedPoint()
                        && jump.properties.jumpPositionPresent
                        && jump.properties.jumpPosition
                        && (jump.properties.unsupportedProperties
                            & eac3render::UnsupportedJumpPosition) == 0U,
           "explicit-jump-position-retained");

    auto nonfinite = metadata;
    nonfinite.state.position.standardX =
        std::numeric_limits<float>::quiet_NaN();
    const auto nonfiniteResult = eac3render::adaptGate6cObject(nonfinite, context);
    expect(&report, nonfiniteResult.disposition
                        == eac3render::SceneObjectPropertyDisposition::Malformed,
           "nonfinite-position-fails-closed");

    auto invalidPriority = metadata;
    invalidPriority.state.priority = 1.01F;
    expect(&report, eac3render::adaptGate6cObject(invalidPriority, context).disposition
                        == eac3render::SceneObjectPropertyDisposition::Malformed,
           "priority-out-of-range-fails-closed");

    auto negativeTime = metadata;
    negativeTime.sourcePosition = -1;
    const auto negativeResult = eac3render::adaptGate6cObject(negativeTime, context);
    expect(&report, negativeResult.disposition
                        == eac3render::SceneObjectPropertyDisposition::Malformed
                        && negativeResult.reason == "negative-source-position",
           "negative-timestamp-fails-closed");

    auto mismatchObject = metadata;
    mismatchObject.state.objectIndex = 5U;
    const auto mismatchObjectResult = eac3render::adaptGate6cObject(
        mismatchObject, context);
    expect(&report, mismatchObjectResult.disposition
                        == eac3render::SceneObjectPropertyDisposition::Malformed,
           "object-identity-mismatch-fails-closed");

    const auto repeated = eac3render::adaptGate6cObject(metadata, context);
    expect(&report, same(point.properties, repeated.properties),
           "repeatable-property-snapshot");
    expect(&report, (point.properties.unsupportedProperties
                         & eac3render::UnsupportedDiffuse) != 0U
                        && (point.properties.unsupportedProperties
                            & eac3render::UnsupportedDivergence) != 0U
                        && (point.properties.unsupportedProperties
                            & eac3render::UnsupportedJumpPosition) != 0U
                        && !point.properties.completeObjectsSupported,
           "structured-diffuse-divergence-unsupported");

    auto zeroGeneration = baseContext();
    zeroGeneration.generation = 0U;
    const auto zeroGenerationResult = eac3render::adaptGate6cObject(
        metadata, zeroGeneration);
    expect(&report, zeroGenerationResult.disposition
                        == eac3render::SceneObjectPropertyDisposition::Malformed
                        && zeroGenerationResult.reason == "invalid-generation",
           "zero-generation-fails-closed");

    eac3render::SceneObjectBatchContext batchContext;
    batchContext.property = context;
    batchContext.property.coordinate =
        eac3render::ObjectCoordinateRepresentation::OamdCartesian;
    batchContext.property.pointDirectionPresent = false;
    batchContext.coordinatePolicy = {10.0, 10.0, 7.0, 0.5, 0.5, 0.0};
    const auto batch = eac3render::adaptGate6cBatch({metadata}, batchContext);
    if (batch.disposition != eac3render::SceneAdapterDisposition::Accepted) {
        std::cerr << "batch-reason=" << batch.reason << '\n';
    }
    expect(&report, batch.disposition == eac3render::SceneAdapterDisposition::Accepted
                        && batch.targetGroup.objects.size() == 1U
                        && batch.properties.size() == 1U
                        && batch.targetGroup.objects[0].update.coordinateSpace
                            == eac3render::ObjectMetadataUpdate::CoordinateSpace::EtsiRoomCartesian
                        && batch.targetGroup.objects[0].update.roomPosition
                            == batch.targetGroup.objects[0].roomPosition,
           "cartesian-room-position-batch-contract");

    auto centreMetadata = metadata;
    centreMetadata.state.position.x = 0.5F;
    centreMetadata.state.position.y = 0.5F;
    centreMetadata.state.position.z = 0.0F;
    centreMetadata.state.position.standardX = 0.5F;
    centreMetadata.state.position.standardY = 0.5F;
    centreMetadata.state.position.standardZ = 0.0F;
    const auto acceptedCentre = eac3render::adaptGate6cBatch(
        {centreMetadata}, batchContext);
    auto centreProperties = point.properties;
    centreProperties.coordinate =
        eac3render::ObjectCoordinateRepresentation::OamdCartesian;
    centreProperties.cartesianFlagKnown = true;
    centreProperties.cartesian = true;
    centreProperties.roomPosition = {0.5F, 0.5F, 0.0F};
    eac3render::UnitVector3 unusedDirection;
    std::string centreReason;
    expect(&report,
           !eac3render::convertRoomPositionToUnitDirection(
                centreProperties, batchContext.coordinatePolicy,
                &unusedDirection, &centreReason)
               && centreReason == "zero-listener-relative-position"
               && acceptedCentre.disposition
                    == eac3render::SceneAdapterDisposition::Accepted
               && acceptedCentre.targetGroup.objects.front().roomPosition
                    == std::array<float, 3>({0.5F, 0.5F, 0.0F})
               && acceptedCentre.targetGroup.objects.front().update.coordinateSpace
                    == eac3render::ObjectMetadataUpdate::CoordinateSpace::EtsiRoomCartesian,
           "allocentric-centre-retained-without-direction-placeholder");

    const auto cardinal = [&](float x, float y, float z,
                              const eac3render::UnitVector3 &expected,
                              const char *name) {
        eac3render::SceneObjectProperties properties = point.properties;
        properties.coordinate = eac3render::ObjectCoordinateRepresentation::OamdCartesian;
        properties.cartesianFlagKnown = true;
        properties.cartesian = true;
        properties.roomPosition = {x, y, z};
        eac3render::UnitVector3 actual;
        std::string conversionReason;
        expect(&report, eac3render::convertRoomPositionToUnitDirection(
                            properties, batchContext.coordinatePolicy, &actual,
                            &conversionReason)
                        && actual == expected, name);
    };
    cardinal(0.5F, 0.0F, 0.0F, {1.0, 0.0, 0.0}, "cardinal-front");
    cardinal(0.5F, 1.0F, 0.0F, {-1.0, 0.0, 0.0}, "cardinal-back");
    cardinal(0.0F, 0.5F, 0.0F, {0.0, -1.0, 0.0}, "cardinal-left");
    cardinal(1.0F, 0.5F, 0.0F, {0.0, 1.0, 0.0}, "cardinal-right");
    cardinal(0.5F, 0.5F, -1.0F, {0.0, 0.0, -1.0}, "cardinal-down");
    cardinal(0.5F, 0.5F, 1.0F, {0.0, 0.0, 1.0}, "cardinal-up");

    auto zeroRamp = metadata;
    zeroRamp.rampDuration = 0U;
    const auto zeroRampBatch = eac3render::adaptGate6cBatch({zeroRamp}, batchContext);
    expect(&report, zeroRampBatch.disposition
                        == eac3render::SceneAdapterDisposition::Accepted,
           "zero-ramp-accepted-for-scheduler");

    auto screenProperties = point.properties;
    screenProperties.coordinate = eac3render::ObjectCoordinateRepresentation::OamdCartesian;
    screenProperties.cartesianFlagKnown = true;
    screenProperties.cartesian = true;
    screenProperties.screenAnchored = true;
    eac3render::UnitVector3 ignoredDirection;
    std::string positionReason;
    expect(&report, !eac3render::convertRoomPositionToUnitDirection(
                        screenProperties, batchContext.coordinatePolicy,
                        &ignoredDirection, &positionReason)
                        && positionReason == "screen-position-requires-screen-geometry",
           "screen-position-not-treated-as-room");

    auto infiniteProperties = point.properties;
    infiniteProperties.coordinate = eac3render::ObjectCoordinateRepresentation::OamdCartesian;
    infiniteProperties.cartesianFlagKnown = true;
    infiniteProperties.cartesian = true;
    infiniteProperties.distanceInfinite = true;
    positionReason.clear();
    expect(&report, !eac3render::convertRoomPositionToUnitDirection(
                        infiniteProperties, batchContext.coordinatePolicy,
                        &ignoredDirection, &positionReason)
                        && positionReason == "infinite-distance-unsupported",
           "infinite-distance-not-silently-converted");

    auto batchExtent = metadata;
    batchExtent.state.raw.sizePresent = true;
    batchExtent.state.raw.sizeIndex = 2U;
    batchExtent.state.effectiveSizePresent = true;
    batchExtent.state.effectiveSizeIndex = 2U;
    batchExtent.state.size = {1.0F / 31.0F, 2.0F / 31.0F, 3.0F / 31.0F};
    const auto pendingBatch = eac3render::adaptGate6cBatch(
        {batchExtent}, batchContext);
    expect(&report, pendingBatch.disposition
                        == eac3render::SceneAdapterDisposition::Unsupported
                        && pendingBatch.reason
                            == "nonzero-extent-pending-no-extent-math",
           "batch-rejects-nonzero-extent");
    auto lateFailureMetadata = batchExtent;
    lateFailureMetadata.objectIndex = 5U;
    lateFailureMetadata.state.objectIndex = 5U;
    const auto lateFailure = eac3render::adaptGate6cBatch(
        {metadata, lateFailureMetadata}, batchContext);
    expect(&report, lateFailure.disposition
                        == eac3render::SceneAdapterDisposition::Unsupported
                        && lateFailure.targetGroup.objects.empty()
                        && lateFailure.properties.empty(),
           "batch-failure-is-transactional");

    auto polarContext = batchContext;
    polarContext.property.coordinate =
        eac3render::ObjectCoordinateRepresentation::OamdPolar;
    const auto polarBatch = eac3render::adaptGate6cBatch({metadata}, polarContext);
    expect(&report, polarBatch.disposition
                        == eac3render::SceneAdapterDisposition::Unsupported,
           "batch-rejects-unconverted-polar");

    auto screenMetadata = metadata;
    screenMetadata.state.position.screenAnchored = true;
    const auto screenBatch = eac3render::adaptGate6cBatch(
        {screenMetadata}, batchContext);
    expect(&report, screenBatch.disposition
                        == eac3render::SceneAdapterDisposition::Unsupported,
           "batch-rejects-screen-position");

    auto infiniteMetadata = metadata;
    infiniteMetadata.state.position.distanceInfinite = true;
    const auto infiniteBatch = eac3render::adaptGate6cBatch(
        {infiniteMetadata}, batchContext);
    expect(&report, infiniteBatch.disposition
                        == eac3render::SceneAdapterDisposition::Unsupported,
           "batch-rejects-infinite-distance");

    eac3render::SceneObjectRampScheduler scheduler;
    expect(&report, scheduler.reset(batch.targetGroup.generation).disposition
                        == eac3render::SceneAdapterDisposition::Accepted,
           "scheduler-generation-start");
    eac3render::SceneObjectTargetGroup firstGroup = batch.targetGroup;
    const auto firstApplied = scheduler.applyTargetGroup(firstGroup);
    if (firstApplied.disposition != eac3render::SceneAdapterDisposition::Accepted)
        std::cerr << "first-scheduler-reason=" << firstApplied.reason << '\n';
    std::vector<eac3render::SceneObjectSnapshot> snapshots;
    const auto firstSnapshot = scheduler.evaluateSnapshot(batch.targetGroup.generation,
                                                          batch.targetGroup.timestampSamples,
                                                          &snapshots);
    expect(&report, firstApplied.disposition == eac3render::SceneAdapterDisposition::Accepted
                        && firstSnapshot.disposition == eac3render::SceneAdapterDisposition::Accepted
                        && snapshots.size() == 1U
                        && snapshots[0].gain == sceneUpdate.gain,
           "scheduler-first-state-snap");
    eac3render::SceneObjectTargetGroup rampGroup = firstGroup;
    rampGroup.timestampSamples = 96005;
    rampGroup.durationSamples = 10;
    rampGroup.objects[0].update.timestampSamples = 96005;
    rampGroup.objects[0].update.gain = 0.0F;
    rampGroup.objects[0].update.position = {0.0, 1.0, 0.0};
    rampGroup.objects[0].roomPosition = {1.0F, 0.5F, 0.0F};
    rampGroup.objects[0].update.roomPosition = rampGroup.objects[0].roomPosition;
    const auto rampApplied = scheduler.applyTargetGroup(rampGroup);
    const auto midpoint = scheduler.evaluateSnapshot(batch.targetGroup.generation, 96010, &snapshots);
    expect(&report, rampApplied.disposition == eac3render::SceneAdapterDisposition::Accepted
                        && midpoint.disposition == eac3render::SceneAdapterDisposition::Accepted
                        && snapshots.size() == 1U
                        && snapshots[0].gain > 0.0F && snapshots[0].gain < sceneUpdate.gain,
           "scheduler-nonzero-ramp-and-normalization");
    eac3render::SceneObjectTargetGroup zeroGroup = rampGroup;
    zeroGroup.timestampSamples = 96300;
    zeroGroup.durationSamples = 0;
    zeroGroup.objects[0].update.timestampSamples = 96300;
    const auto zeroApplied = scheduler.applyTargetGroup(zeroGroup);
    const auto zeroSnapshot = scheduler.evaluateSnapshot(batch.targetGroup.generation, 96300, &snapshots);
    expect(&report, zeroApplied.disposition == eac3render::SceneAdapterDisposition::Accepted
                        && zeroSnapshot.disposition == eac3render::SceneAdapterDisposition::Accepted
                        && snapshots[0].gain == 0.0F,
           "scheduler-zero-step");
    const auto historical = scheduler.evaluateSnapshot(batch.targetGroup.generation, 96299, &snapshots);
    expect(&report, historical.disposition == eac3render::SceneAdapterDisposition::InvalidInput
                        && historical.reason == "scheduler-historical-evaluation-unsupported",
           "scheduler-rejects-historical-evaluation");
    auto changedPolicy = zeroGroup;
    changedPolicy.timestampSamples = 96301;
    changedPolicy.objects[0].update.timestampSamples = 96301;
    changedPolicy.coordinatePolicy.roomWidthMetres = 11.0;
    expect(&report, scheduler.applyTargetGroup(changedPolicy).disposition
                        == eac3render::SceneAdapterDisposition::InvalidInput,
           "scheduler-policy-invariant");
    auto staleGroup = zeroGroup;
    staleGroup.generation = 1U;
    staleGroup.timestampSamples = 96301;
    staleGroup.objects[0].update.timestampSamples = 96301;
    expect(&report, scheduler.applyTargetGroup(staleGroup).disposition
                        == eac3render::SceneAdapterDisposition::Stale,
           "scheduler-stale-generation");
    expect(&report, scheduler.reset(batch.targetGroup.generation + 1U).disposition
                        == eac3render::SceneAdapterDisposition::Accepted
                        && scheduler.objectCount() == 0U,
           "scheduler-reset-clears-state");

    std::cout << "SceneObjectPropertiesSelfTest="
              << (report.failures == 0U ? "PASS" : "FAIL")
              << " cases=" << report.cases
              << " failures=" << report.failures << '\n';
    return report.failures == 0U ? 0 : 1;
}
