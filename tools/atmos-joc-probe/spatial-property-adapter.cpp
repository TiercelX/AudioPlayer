#include "spatial-property-adapter.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace eac3gate7b {
namespace {

constexpr double kGainMinimumDb = -49.0;
constexpr double kGainMaximumDb = 15.0;

bool finite(double value) { return std::isfinite(value); }
bool finite(float value) { return std::isfinite(value); }

bool allIncluded(const std::array<bool, 6> &zones)
{
    return std::all_of(zones.begin(), zones.end(), [](bool value) { return value; });
}

bool silent(const WindowsProperty &property)
{
    return !property.active || property.minusInfinity;
}

float volumeForGain(double gainDb, double headroomDb)
{
    return static_cast<float>(std::pow(10.0, (gainDb - headroomDb) / 20.0));
}

WindowsProperty interpolate(const WindowsProperty &start,
                            const WindowsProperty &target,
                            double fraction,
                            double headroomDb)
{
    if (fraction <= 0.0) return start;
    if (fraction >= 1.0) return target;
    WindowsProperty result = target;
    for (unsigned axis = 0; axis < 3U; ++axis) {
        result.position[axis] = static_cast<float>(
            static_cast<double>(start.position[axis])
            + fraction * (static_cast<double>(target.position[axis])
                          - static_cast<double>(start.position[axis])));
    }
    result.active = fraction >= 1.0 ? target.active : start.active;
    result.minusInfinity = fraction >= 1.0 ? target.minusInfinity : start.minusInfinity;
    if (!silent(start) && !silent(target)) {
        result.gainDb = static_cast<float>(
            static_cast<double>(start.gainDb)
            + fraction * (static_cast<double>(target.gainDb)
                          - static_cast<double>(start.gainDb)));
        result.volume = volumeForGain(result.gainDb, headroomDb);
    } else {
        result.gainDb = fraction >= 1.0 ? target.gainDb : start.gainDb;
        result.volume = static_cast<float>(
            static_cast<double>(start.volume)
            + fraction * (static_cast<double>(target.volume)
                          - static_cast<double>(start.volume)));
        // During a silent-to-audible transition, retain a finite active
        // diagnostic state as soon as the interpolated amplitude is nonzero;
        // otherwise the flags would claim silence while volume is audible.
        if (result.volume > 0.0f) {
            result.active = true;
            result.minusInfinity = false;
        }
    }
    return result;
}

bool evaluateRamp(const Adapter::RampState &state, std::int64_t samplePosition,
                  double headroomDb, WindowsProperty *property, std::string *reason)
{
    if (!property || !reason || !state.initialized) {
        if (reason) *reason = "evaluation-before-first-update";
        return false;
    }
    if (samplePosition < state.sourcePosition) {
        *reason = "evaluation-before-ramp-start";
        return false;
    }
    if (state.duration == 0U) {
        *property = state.target;
        return true;
    }
    const std::int64_t elapsed = samplePosition - state.sourcePosition;
    if (elapsed >= static_cast<std::int64_t>(state.duration)) {
        *property = state.target;
        return true;
    }
    const double fraction = static_cast<double>(elapsed)
        / static_cast<double>(state.duration);
    *property = interpolate(state.start, state.target, fraction, headroomDb);
    return true;
}

bool rampOverlaps(const Adapter::RampState &state, std::int64_t samplePosition)
{
    if (!state.initialized || state.duration == 0U
        || samplePosition < state.sourcePosition) return false;
    const std::int64_t duration = static_cast<std::int64_t>(state.duration);
    if (state.sourcePosition > std::numeric_limits<std::int64_t>::max() - duration) {
        return true;
    }
    return samplePosition < state.sourcePosition + duration;
}

bool makeTarget(const Geometry &geometry, const Update &update,
                WindowsProperty *target, bool *screenConversion,
                double *sourceRadius, double *outputRadius,
                std::string *reason, Disposition *disposition)
{
    if (!target || !screenConversion || !sourceRadius || !outputRadius
        || !reason || !disposition) return false;
    const eac3oamd::B2bObjectState &state = update.state;
    if (update.objectIndex < 1U || update.objectIndex > kDynamicObjectCount) {
        *reason = "object-index-out-of-range";
        *disposition = Disposition::Malformed;
        return false;
    }
    if (state.objectIndex != update.objectIndex || state.lfeHelper
        || !state.basicValid || !state.renderValid || !state.position.valid) {
        *reason = "invalid-dynamic-object-state";
        *disposition = Disposition::Malformed;
        return false;
    }
    if (!finite(state.priority) || state.priority < 0.0f || state.priority > 1.0f) {
        *reason = "priority-out-of-range-or-nonfinite";
        *disposition = Disposition::Malformed;
        return false;
    }
    if (!finite(state.gainDb) || state.gainDb < kGainMinimumDb
        || state.gainDb > kGainMaximumDb) {
        *reason = "gain-out-of-range-or-nonfinite";
        *disposition = Disposition::Malformed;
        return false;
    }
    if (state.position.distanceInfinite) {
        *reason = "unsupported-distance-infinity";
        *disposition = Disposition::Unsupported;
        return false;
    }
    for (float value : state.size) {
        if (!finite(value)) {
            *reason = "nonfinite-object-size";
            *disposition = Disposition::Malformed;
            return false;
        }
        if (value != 0.0f) {
            *reason = "unsupported-object-size";
            *disposition = Disposition::Unsupported;
            return false;
        }
    }
    if (!allIncluded(state.zoneConstraints) || !state.elevation) {
        *reason = "unsupported-zone-or-elevation-constraint";
        *disposition = Disposition::Unsupported;
        return false;
    }
    if (state.snap) {
        *reason = "unsupported-channel-lock-or-snap";
        *disposition = Disposition::Unsupported;
        return false;
    }
    const auto &position = state.position;
    if (!finite(position.x) || !finite(position.y) || !finite(position.z)
        || !finite(position.screenFactor) || !finite(position.depthFactor)) {
        *reason = "nonfinite-object-position";
        *disposition = Disposition::Malformed;
        return false;
    }

    double roomX = position.x;
    double roomY = position.y;
    double roomZ = position.z;
    *screenConversion = position.screenAnchored;
    if (position.screenAnchored) {
        if (position.x < 0.0f || position.x > 1.0f || position.y < 0.0f
            || position.y > 1.0f || position.z < -1.0f
            || position.z > 1.0f || position.screenFactor < 0.125f
            || position.screenFactor > 1.0f
            || !(std::abs(position.depthFactor - 0.25f) < 1.0e-5f
                 || std::abs(position.depthFactor - 0.5f) < 1.0e-5f
                 || std::abs(position.depthFactor - 1.0f) < 1.0e-5f
                 || std::abs(position.depthFactor - 2.0f) < 1.0e-5f)) {
            *reason = "screen-coordinate-or-factor-out-of-range";
            *disposition = Disposition::Malformed;
            return false;
        }
        const double csX = position.x;
        const double csY = position.y;
        const double csZ = position.z;
        const double crX = geometry.screenBottomLeftX + geometry.screenWidth * csX;
        const double crY = geometry.screenBottomLeftY + csY;
        const double crZ = geometry.screenBottomLeftZ
            + geometry.screenHeight * 0.5 * (csZ + 1.0);
        const double alpha = geometry.screenWidth >= 0.0
            ? static_cast<double>(position.screenFactor)
                * std::pow(csY, static_cast<double>(position.depthFactor))
            : std::numeric_limits<double>::quiet_NaN();
        roomX = crX + alpha * (csX - crX);
        roomY = csY;
        roomZ = crZ + alpha * (csZ - crZ);
        (void)crY; // The ETSI interpolation keeps P.y at Cs.y.
    }
    const double physicalRoomX = roomX * geometry.roomWidthMetres;
    const double physicalRoomY = roomY * geometry.roomDepthMetres;
    const double physicalRoomZ = (roomZ + 1.0) * 0.5 * geometry.roomHeightMetres;
    const double listenerPhysicalX = geometry.listenerX * geometry.roomWidthMetres;
    const double listenerPhysicalY = geometry.listenerY * geometry.roomDepthMetres;
    const double listenerPhysicalZ = (geometry.listenerZ + 1.0) * 0.5
        * geometry.roomHeightMetres;
    std::array<double, 3> windows {
        physicalRoomX - listenerPhysicalX,
        physicalRoomZ - listenerPhysicalZ,
        physicalRoomY - listenerPhysicalY};
    for (double value : windows) {
        if (!finite(value) || value > std::numeric_limits<float>::max()
            || value < -std::numeric_limits<float>::max()) {
            *reason = "nonfinite-or-overflowed-windows-position";
            *disposition = Disposition::Malformed;
            return false;
        }
    }

    const double sourcePositionRadius = std::sqrt(
        windows[0] * windows[0] + windows[1] * windows[1] + windows[2] * windows[2]);
    if (!finite(sourcePositionRadius)) {
        *reason = "nonfinite-source-position-radius";
        *disposition = Disposition::Malformed;
        return false;
    }
    if (geometry.positionRadiusMode == PositionRadiusMode::Unit
        && sourcePositionRadius > 0.0) {
        for (double &value : windows) value /= sourcePositionRadius;
    }
    if (geometry.positionDirectionMode == PositionDirectionMode::Metadata
        && geometry.azimuthFocus > 0.0f) {
        const double horizontalRadius = std::sqrt(
            windows[0] * windows[0] + windows[2] * windows[2]);
        if (horizontalRadius > 0.0) {
            const double theta = std::atan2(windows[0], -windows[2]);
            const double focusedTheta = (1.0 - static_cast<double>(geometry.azimuthFocus))
                * theta;
            windows[0] = horizontalRadius * std::sin(focusedTheta);
            windows[2] = -horizontalRadius * std::cos(focusedTheta);
        }
    }
    if (geometry.positionDirectionMode == PositionDirectionMode::Front
        && sourcePositionRadius > 0.0) {
        const double selectedRadius = std::sqrt(
            windows[0] * windows[0] + windows[1] * windows[1] + windows[2] * windows[2]);
        windows = {0.0, 0.0, -selectedRadius};
    }
    const double outputPositionRadius = std::sqrt(
        windows[0] * windows[0] + windows[1] * windows[1] + windows[2] * windows[2]);
    if (!finite(outputPositionRadius)) {
        *reason = "nonfinite-output-position-radius";
        *disposition = Disposition::Malformed;
        return false;
    }
    *sourceRadius = sourcePositionRadius;
    *outputRadius = outputPositionRadius;

    target->objectIndex = update.objectIndex;
    target->active = state.active;
    target->minusInfinity = state.gainMinusInfinity;
    target->gainDb = state.gainDb;
    target->volume = (!state.active || state.gainMinusInfinity)
        ? 0.0f : volumeForGain(state.gainDb, geometry.gainHeadroomDb);
    target->position = {static_cast<float>(windows[0]), static_cast<float>(windows[1]),
                        static_cast<float>(windows[2])};
    if (!finite(target->volume) || target->volume < 0.0f || target->volume > 1.0f
        || !finite(target->position[0]) || !finite(target->position[1])
        || !finite(target->position[2])) {
        *reason = "nonfinite-or-out-of-range-windows-property";
        *disposition = Disposition::Malformed;
        return false;
    }
    *disposition = Disposition::Pass;
    return true;
}

void noteFailure(Metrics *metrics, const Update *update, const std::string &reason)
{
    if (!metrics) return;
    if (metrics->firstFailureReason.empty()) {
        metrics->firstFailureReason = reason;
        metrics->firstFailurePosition = update ? update->sourcePosition : -1;
        metrics->firstFailureObject = update ? update->objectIndex : 0U;
    }
}

bool near(double left, double right)
{
    return std::abs(left - right) < 1.0e-5;
}

} // namespace

const char *dispositionText(Disposition disposition)
{
    switch (disposition) {
    case Disposition::Pass: return "PASS";
    case Disposition::Unsupported: return "UNSUPPORTED";
    case Disposition::Malformed: return "MALFORMED";
    }
    return "MALFORMED";
}

const char *positionRadiusModeText(PositionRadiusMode mode)
{
    switch (mode) {
    case PositionRadiusMode::Source: return "source";
    case PositionRadiusMode::Unit: return "unit";
    }
    return "source";
}

const char *positionDirectionModeText(PositionDirectionMode mode)
{
    switch (mode) {
    case PositionDirectionMode::Metadata: return "metadata";
    case PositionDirectionMode::Front: return "front";
    }
    return "metadata";
}

Geometry referenceGeometry()
{
    Geometry geometry;
    geometry.roomWidthMetres = 10.0;
    geometry.roomDepthMetres = 10.0;
    geometry.roomHeightMetres = 7.0;
    geometry.listenerX = 0.5;
    geometry.listenerY = 0.5;
    geometry.listenerZ = 0.0;
    geometry.screenBottomLeftX = 0.25;
    geometry.screenBottomLeftY = 0.0;
    geometry.screenBottomLeftZ = -0.5;
    geometry.screenWidth = 0.5;
    geometry.screenHeight = 1.0;
    geometry.gainHeadroomDb = 15.0;
    return geometry;
}

bool validateGeometry(const Geometry &geometry, std::string *reason)
{
    if (!reason) return false;
    const double values[] = {
        geometry.roomWidthMetres, geometry.roomDepthMetres, geometry.roomHeightMetres,
        geometry.listenerX, geometry.listenerY, geometry.listenerZ,
        geometry.screenBottomLeftX, geometry.screenBottomLeftY,
        geometry.screenBottomLeftZ, geometry.screenWidth, geometry.screenHeight,
        geometry.gainHeadroomDb};
    for (double value : values) {
        if (!finite(value)) { *reason = "geometry-nonfinite"; return false; }
    }
    if (geometry.roomWidthMetres <= 0.0 || geometry.roomDepthMetres <= 0.0
        || geometry.roomHeightMetres <= 0.0 || geometry.screenWidth <= 0.0
        || geometry.screenHeight <= 0.0) {
        *reason = "geometry-dimensions-must-be-positive";
        return false;
    }
    if (geometry.listenerX < 0.0 || geometry.listenerX > 1.0
        || geometry.listenerY < 0.0 || geometry.listenerY > 1.0
        || geometry.listenerZ < -1.0 || geometry.listenerZ > 1.0) {
        *reason = "listener-outside-room";
        return false;
    }
    if (geometry.screenBottomLeftX < 0.0 || geometry.screenBottomLeftX > 1.0
        || geometry.screenBottomLeftY < 0.0 || geometry.screenBottomLeftY > 1.0
        || geometry.screenBottomLeftZ < -1.0 || geometry.screenBottomLeftZ > 1.0
        || geometry.screenBottomLeftX + geometry.screenWidth > 1.0
        || geometry.screenBottomLeftZ + geometry.screenHeight > 1.0) {
        *reason = "screen-rectangle-outside-room";
        return false;
    }
    if (geometry.gainHeadroomDb < kGainMaximumDb) {
        *reason = "gain-headroom-below-15db";
        return false;
    }
    if (geometry.positionRadiusMode != PositionRadiusMode::Source
        && geometry.positionRadiusMode != PositionRadiusMode::Unit) {
        *reason = "position-radius-mode-invalid";
        return false;
    }
    if (geometry.positionDirectionMode != PositionDirectionMode::Metadata
        && geometry.positionDirectionMode != PositionDirectionMode::Front) {
        *reason = "position-direction-mode-invalid";
        return false;
    }
    if (!finite(geometry.azimuthFocus) || geometry.azimuthFocus < 0.0f
        || geometry.azimuthFocus > 1.0f) {
        *reason = "azimuth-focus-out-of-range-or-nonfinite";
        return false;
    }
    return true;
}

Adapter::Adapter(Geometry geometry) : geometry_(geometry) {}

ApplyResult Adapter::applyBatch(const std::vector<Update> &updates)
{
    ApplyResult result;
    result.disposition = Disposition::Malformed;
    Metrics stagedMetrics = metrics_;
    stagedMetrics.updatesAttempted += updates.size();
    const auto reject = [&](Disposition disposition, const Update *update,
                            const std::string &reason) {
        Metrics failureMetrics = metrics_;
        failureMetrics.updatesAttempted += updates.size();
        if (disposition == Disposition::Unsupported) ++failureMetrics.unsupportedCount;
        else ++failureMetrics.rejectedCount;
        noteFailure(&failureMetrics, update, reason);
        metrics_ = std::move(failureMetrics);
        ApplyResult failure;
        failure.disposition = disposition;
        failure.reason = reason;
        return failure;
    };
    if (updates.empty()) {
        return reject(Disposition::Malformed, nullptr, "empty-update-batch");
    }
    std::string geometryReason;
    if (!validateGeometry(geometry_, &geometryReason)) {
        return reject(Disposition::Malformed, &updates.front(), geometryReason);
    }
    for (std::size_t index = 0; index < updates.size(); ++index) {
        const Update &update = updates[index];
        if (update.sourcePosition < 0) {
            return reject(Disposition::Malformed, &update, "negative-source-position");
        }
        if (update.rampDuration != 0U
            && update.sourcePosition > std::numeric_limits<std::int64_t>::max()
                - static_cast<std::int64_t>(update.rampDuration)) {
            return reject(Disposition::Malformed, &update,
                          "ramp-end-position-overflow");
        }
        if (index > 0U && update.sourcePosition < updates[index - 1U].sourcePosition) {
            return reject(Disposition::Malformed, &update, "non-monotonic-source-position");
        }
        if (index == 0U || update.sourcePosition != updates[index - 1U].sourcePosition) {
            std::size_t end = index + 1U;
            while (end < updates.size()
                   && updates[end].sourcePosition == update.sourcePosition) ++end;
            for (std::size_t object = index + 1U; object < end; ++object) {
                if (updates[object].objectIndex <= updates[object - 1U].objectIndex) {
                    return reject(Disposition::Malformed, &updates[object],
                                  "object-index-order-or-duplicate-at-source-position");
                }
            }
            index = end - 1U;
        }
    }

    auto stagedStates = states_;
    result.records.reserve(updates.size());
    for (const Update &update : updates) {
        WindowsProperty target;
        bool screenConversion = false;
        double sourceRadius = 0.0;
        double outputRadius = 0.0;
        Disposition disposition = Disposition::Malformed;
        std::string reason;
        if (!makeTarget(geometry_, update, &target, &screenConversion,
                        &sourceRadius, &outputRadius,
                        &reason, &disposition)) {
            return reject(disposition, &update, reason);
        }
        RampState &state = stagedStates[update.objectIndex];
        RampRecord record;
        record.update = update;
        record.target = target;
        record.screenConversion = screenConversion;
        if (!state.initialized) {
            record.firstStateSnap = true;
            record.start = target;
            record.immediateStep = true;
            state.initialized = true;
            state.sourcePosition = update.sourcePosition;
            state.duration = 0U;
            state.start = target;
            state.target = target;
            ++stagedMetrics.firstStateSnaps;
        } else {
            const bool overlap = rampOverlaps(state, update.sourcePosition);
            record.overlap = overlap;
            if (overlap) ++stagedMetrics.overlapCount;
            if (!evaluateRamp(state, update.sourcePosition, geometry_.gainHeadroomDb,
                              &record.start, &reason)) {
                return reject(Disposition::Malformed, &update, reason);
            }
            record.immediateStep = update.rampDuration == 0U;
            state.initialized = true;
            state.sourcePosition = update.sourcePosition;
            state.duration = update.rampDuration;
            state.start = record.start;
            state.target = target;
            if (record.immediateStep) ++stagedMetrics.stepCount;
            else ++stagedMetrics.rampCount;
        }
        if (record.firstStateSnap) {
            // A first update is deliberately an immediate snap even if OAMD
            // carries a non-zero duration: there is no prior property to ramp.
        }
        result.records.push_back(std::move(record));
        ++stagedMetrics.updatesCommitted;
        ++stagedMetrics.updatesByObject[update.objectIndex];
        stagedMetrics.objectIndexMin = std::min(stagedMetrics.objectIndexMin,
                                                update.objectIndex);
        stagedMetrics.objectIndexMax = std::max(stagedMetrics.objectIndexMax,
                                                update.objectIndex);
        if (screenConversion) ++stagedMetrics.screenConversions;
        else ++stagedMetrics.roomConversions;
        if (silent(target)) ++stagedMetrics.silentTargets;
        for (unsigned axis = 0; axis < 3U; ++axis) {
            stagedMetrics.positionMin[axis] = std::min(stagedMetrics.positionMin[axis],
                                                       target.position[axis]);
            stagedMetrics.positionMax[axis] = std::max(stagedMetrics.positionMax[axis],
                                                       target.position[axis]);
        }
        stagedMetrics.volumeMin = std::min(stagedMetrics.volumeMin, target.volume);
        stagedMetrics.volumeMax = std::max(stagedMetrics.volumeMax, target.volume);
        stagedMetrics.sourceRadiusMin = std::min(stagedMetrics.sourceRadiusMin, sourceRadius);
        stagedMetrics.sourceRadiusMax = std::max(stagedMetrics.sourceRadiusMax, sourceRadius);
        stagedMetrics.sourceRadiusSum += sourceRadius;
        stagedMetrics.outputRadiusMin = std::min(stagedMetrics.outputRadiusMin, outputRadius);
        stagedMetrics.outputRadiusMax = std::max(stagedMetrics.outputRadiusMax, outputRadius);
        stagedMetrics.outputRadiusSum += outputRadius;
        ++stagedMetrics.radiusSamples;
    }
    states_ = std::move(stagedStates);
    metrics_ = std::move(stagedMetrics);
    result.disposition = Disposition::Pass;
    return result;
}

bool Adapter::evaluate(unsigned objectIndex, std::int64_t samplePosition,
                       WindowsProperty *property, std::string *reason) const
{
    if (objectIndex < 1U || objectIndex > kDynamicObjectCount) {
        if (reason) *reason = "object-index-out-of-range";
        return false;
    }
    return evaluateRamp(states_[objectIndex], samplePosition,
                        geometry_.gainHeadroomDb, property, reason);
}

void Adapter::reset()
{
    states_ = {};
    metrics_ = Metrics {};
    ++metrics_.resetCount;
}

SelfTestReport runSelfTest()
{
    SelfTestReport report;
    auto check = [&report](bool pass, const char *reason) {
        ++report.cases;
        if (!pass && report.reason.empty()) report.reason = reason;
    };
    const Geometry geometry = referenceGeometry();
    std::string reason;
    check(validateGeometry(geometry, &reason), "reference-geometry");
    Geometry bad = geometry;
    bad.roomHeightMetres = 0.0;
    check(!validateGeometry(bad, &reason), "invalid-geometry");
    bad = geometry;
    bad.gainHeadroomDb = 14.9;
    check(!validateGeometry(bad, &reason), "invalid-headroom");
    bad = geometry;
    bad.positionRadiusMode = static_cast<PositionRadiusMode>(99);
    check(!validateGeometry(bad, &reason), "invalid-position-radius-mode");
    bad = geometry;
    bad.positionDirectionMode = static_cast<PositionDirectionMode>(99);
    check(!validateGeometry(bad, &reason), "invalid-position-direction-mode");
    bad = geometry;
    bad.azimuthFocus = -0.1f;
    check(!validateGeometry(bad, &reason), "invalid-azimuth-focus");

    auto state = [](unsigned index, float x, float y, float z, bool screen,
                    float gain, bool minusInfinity = false, bool active = true) {
        eac3oamd::B2bObjectState value;
        value.objectIndex = index;
        value.active = active;
        value.basicValid = true;
        value.renderValid = true;
        value.gainMinusInfinity = minusInfinity;
        value.gainDb = gain;
        value.position.valid = true;
        value.position.x = x;
        value.position.y = y;
        value.position.z = z;
        value.position.screenAnchored = screen;
        value.position.screenFactor = screen ? 0.5f : 0.0f;
        value.position.depthFactor = screen ? 1.0f : 0.0f;
        value.zoneConstraints = {true, true, true, true, true, true};
        value.elevation = true;
        return value;
    };
    auto batch = [&](std::int64_t position, float x, float y, float z,
                     float gain, unsigned duration = 0U, bool screen = false,
                     bool active = true, bool minusInfinity = false) {
        std::vector<Update> updates;
        updates.reserve(kDynamicObjectCount);
        for (unsigned index = 1U; index <= kDynamicObjectCount; ++index) {
            updates.push_back(Update {position, 0U, duration, index,
                                      state(index, x, y, z, screen, gain,
                                            minusInfinity, active)});
        }
        return updates;
    };
    Adapter adapter(geometry);
    ApplyResult first = adapter.applyBatch(batch(0, 0.5f, 0.5f, 0.0f, 0.0f));
    check(first.disposition == Disposition::Pass && first.records.size() == 15U,
          "first-state-snap");
    WindowsProperty property;
    check(adapter.evaluate(1U, 0, &property, &reason)
              && near(property.position[0], 0.0)
              && near(property.position[1], 0.0)
              && near(property.position[2], 0.0)
              && near(property.volume, 0.177827941),
          "room-centre-and-zero-gain");
    ApplyResult ramp = adapter.applyBatch(batch(10, 1.0f, 0.5f, 1.0f, 15.0f, 10U));
    check(ramp.disposition == Disposition::Pass
              && adapter.evaluate(1U, 15, &property, &reason)
              && near(property.position[0], 2.5)
              && near(property.position[1], 1.75)
              && near(property.position[2], 0.0),
          "ramp-middle-and-end");
    check(adapter.evaluate(1U, 20, &property, &reason)
              && near(property.position[0], 5.0)
              && near(property.position[1], 3.5)
              && near(property.volume, 1.0), "ramp-end-exact");
    ApplyResult overlap = adapter.applyBatch(batch(15, 0.0f, 0.5f, -1.0f, -49.0f, 5U));
    check(overlap.disposition == Disposition::Pass && overlap.records[0].overlap,
          "overlapping-ramp");
    ApplyResult screen = adapter.applyBatch(batch(20, 0.5f, 0.0f, -1.0f, 0.0f, 0U, true));
    check(screen.disposition == Disposition::Pass
              && near(screen.records[0].target.position[0], 0.0)
              && near(screen.records[0].target.position[1], -1.75),
          "screen-y-zero-transform");
    auto screenTop = batch(21, 0.5f, 1.0f, 1.0f, 0.0f, 0U, true);
    screenTop[0].state.position.screenFactor = 0.5f;
    screenTop[0].state.position.depthFactor = 0.25f;
    ApplyResult screenTopResult = adapter.applyBatch(screenTop);
    check(screenTopResult.disposition == Disposition::Pass
              && near(screenTopResult.records[0].target.position[1], 2.625),
          "screen-y-one-and-fractional-depth");
    auto fractionalScreen = batch(22, 0.5f, 0.25f, 1.0f, 0.0f, 0U, true);
    fractionalScreen[0].state.position.x = 0.0f;
    fractionalScreen[0].state.position.screenFactor = 0.5f;
    fractionalScreen[0].state.position.depthFactor = 0.5f;
    ApplyResult fractionalScreenResult = adapter.applyBatch(fractionalScreen);
    check(fractionalScreenResult.disposition == Disposition::Pass
              && near(fractionalScreenResult.records[0].target.position[0], -3.125)
              && near(fractionalScreenResult.records[0].target.position[1], 2.1875)
              && near(fractionalScreenResult.records[0].target.position[2], -2.5),
          "screen-fractional-depth");

    Adapter outsideRoom(geometry);
    auto outside = batch(0, 1.5f, 0.5f, 0.0f, 0.0f);
    ApplyResult outsideResult = outsideRoom.applyBatch(outside);
    check(outsideResult.disposition == Disposition::Pass
              && near(outsideResult.records[0].target.position[0], 10.0),
          "outside-room-no-clamp");

    Adapter basis(geometry);
    auto basisCheck = [&](std::int64_t source, float x, float y, float z,
                          float expectedX, float expectedY, float expectedZ) {
        ApplyResult applied = basis.applyBatch(batch(source, x, y, z, 0.0f));
        WindowsProperty basisProperty;
        return applied.disposition == Disposition::Pass
            && basis.evaluate(1U, source, &basisProperty, &reason)
            && near(basisProperty.position[0], expectedX)
            && near(basisProperty.position[1], expectedY)
            && near(basisProperty.position[2], expectedZ);
    };
    check(basisCheck(0, 0.0f, 0.5f, 0.0f, -5.0, 0.0, 0.0)
              && basisCheck(1, 1.0f, 0.5f, 0.0f, 5.0, 0.0, 0.0)
              && basisCheck(2, 0.5f, 0.5f, -1.0f, 0.0, -3.5, 0.0)
              && basisCheck(3, 0.5f, 0.5f, 1.0f, 0.0, 3.5, 0.0)
              && basisCheck(4, 0.5f, 0.0f, 0.0f, 0.0, 0.0, -5.0)
              && basisCheck(5, 0.5f, 1.0f, 0.0f, 0.0, 0.0, 5.0),
           "room-basis-and-handedness");

    Geometry unitGeometry = geometry;
    unitGeometry.positionRadiusMode = PositionRadiusMode::Unit;
    Adapter unitRadius(unitGeometry);
    ApplyResult unitResult = unitRadius.applyBatch(
        batch(0, 0.0f, 0.5f, 0.0f, 0.0f));
    WindowsProperty unitProperty;
    const Metrics &unitMetrics = unitRadius.metrics();
    check(unitResult.disposition == Disposition::Pass
              && unitRadius.evaluate(1U, 0, &unitProperty, &reason)
              && near(unitProperty.position[0], -1.0)
              && near(unitProperty.position[1], 0.0)
              && near(unitProperty.position[2], 0.0)
              && unitMetrics.radiusSamples == kDynamicObjectCount
              && near(unitMetrics.sourceRadiusMin, 5.0)
              && near(unitMetrics.sourceRadiusMax, 5.0)
              && near(unitMetrics.outputRadiusMin, 1.0)
              && near(unitMetrics.outputRadiusMax, 1.0),
          "unit-radius-preserves-direction");
    Adapter unitCentre(unitGeometry);
    ApplyResult unitCentreResult = unitCentre.applyBatch(
        batch(0, 0.5f, 0.5f, 0.0f, 0.0f));
    WindowsProperty unitCentreProperty;
    check(unitCentreResult.disposition == Disposition::Pass
              && unitCentre.evaluate(1U, 0, &unitCentreProperty, &reason)
              && std::isfinite(unitCentreProperty.position[0])
              && std::isfinite(unitCentreProperty.position[1])
              && std::isfinite(unitCentreProperty.position[2])
              && near(unitCentreProperty.position[0], 0.0)
              && near(unitCentreProperty.position[1], 0.0)
              && near(unitCentreProperty.position[2], 0.0)
              && near(unitCentre.metrics().outputRadiusMax, 0.0),
          "unit-radius-preserves-zero-vector");

    Geometry frontGeometry = geometry;
    frontGeometry.positionDirectionMode = PositionDirectionMode::Front;
    frontGeometry.azimuthFocus = 0.5f;
    Adapter front(frontGeometry);
    ApplyResult frontResult = front.applyBatch(
        batch(0, 0.0f, 0.5f, 0.0f, 0.0f));
    WindowsProperty frontProperty;
    check(frontResult.disposition == Disposition::Pass
              && front.evaluate(1U, 0, &frontProperty, &reason)
              && near(frontProperty.position[0], 0.0)
              && near(frontProperty.position[1], 0.0)
              && near(frontProperty.position[2], -5.0)
              && near(front.metrics().sourceRadiusMax, 5.0)
              && near(front.metrics().outputRadiusMax, 5.0),
          "front-direction-preserves-source-radius");
    ApplyResult frontRampResult = front.applyBatch(
        batch(10, 1.0f, 0.5f, 1.0f, 0.0f, 10U));
    WindowsProperty frontRampProperty;
    check(frontRampResult.disposition == Disposition::Pass
              && front.evaluate(1U, 15, &frontRampProperty, &reason)
              && near(frontRampProperty.position[0], 0.0)
              && near(frontRampProperty.position[1], 0.0)
              && frontRampProperty.position[2] < -5.0f
              && frontRampProperty.position[2] > -6.2f,
          "front-direction-ramp-stays-on-axis");
    Geometry unitFrontGeometry = frontGeometry;
    unitFrontGeometry.positionRadiusMode = PositionRadiusMode::Unit;
    Adapter unitFront(unitFrontGeometry);
    ApplyResult unitFrontResult = unitFront.applyBatch(
        batch(0, 1.0f, 0.5f, 0.0f, 0.0f));
    WindowsProperty unitFrontProperty;
    check(unitFrontResult.disposition == Disposition::Pass
              && unitFront.evaluate(1U, 0, &unitFrontProperty, &reason)
              && near(unitFrontProperty.position[0], 0.0)
              && near(unitFrontProperty.position[1], 0.0)
              && near(unitFrontProperty.position[2], -1.0),
          "unit-front-direction");
    Adapter frontCentre(frontGeometry);
    ApplyResult frontCentreResult = frontCentre.applyBatch(
        batch(0, 0.5f, 0.5f, 0.0f, 0.0f));
    WindowsProperty frontCentreProperty;
    check(frontCentreResult.disposition == Disposition::Pass
              && frontCentre.evaluate(1U, 0, &frontCentreProperty, &reason)
              && near(frontCentreProperty.position[0], 0.0)
              && near(frontCentreProperty.position[1], 0.0)
              && near(frontCentreProperty.position[2], 0.0),
          "front-direction-preserves-zero-vector");

    Geometry focusGeometry = geometry;
    focusGeometry.azimuthFocus = 0.5f;
    Adapter halfFocus(focusGeometry);
    ApplyResult halfFocusResult = halfFocus.applyBatch(
        batch(0, 0.0f, 0.5f, 0.0f, 0.0f));
    WindowsProperty halfFocusProperty;
    check(halfFocusResult.disposition == Disposition::Pass
              && halfFocus.evaluate(1U, 0, &halfFocusProperty, &reason)
              && near(halfFocusProperty.position[0], -3.5355339)
              && near(halfFocusProperty.position[1], 0.0)
              && near(halfFocusProperty.position[2], -3.5355339)
              && near(std::hypot(halfFocusProperty.position[0],
                                 halfFocusProperty.position[2]), 5.0),
          "azimuth-half-focus-left-angle");
    ApplyResult halfFocusRampResult = halfFocus.applyBatch(
        batch(10, 1.0f, 0.5f, 1.0f, 0.0f, 10U));
    WindowsProperty halfFocusRampProperty;
    check(halfFocusRampResult.disposition == Disposition::Pass
              && halfFocus.evaluate(1U, 15, &halfFocusRampProperty, &reason)
              && near(halfFocusRampProperty.position[0], 0.0)
              && near(halfFocusRampProperty.position[1], 1.75)
              && near(halfFocusRampProperty.position[2], -3.5355339),
          "azimuth-focus-ramp-evaluates-focused-targets");
    Geometry fullFocusGeometry = geometry;
    fullFocusGeometry.azimuthFocus = 1.0f;
    Adapter fullFocus(fullFocusGeometry);
    ApplyResult fullFocusResult = fullFocus.applyBatch(
        batch(0, 0.0f, 0.0f, 1.0f, 0.0f));
    WindowsProperty fullFocusProperty;
    check(fullFocusResult.disposition == Disposition::Pass
              && fullFocus.evaluate(1U, 0, &fullFocusProperty, &reason)
              && near(fullFocusProperty.position[0], 0.0)
              && near(fullFocusProperty.position[1], 3.5)
              && near(fullFocusProperty.position[2], -7.0710678)
              && near(std::sqrt(fullFocusProperty.position[0]
                                * fullFocusProperty.position[0]
                                + fullFocusProperty.position[1]
                                * fullFocusProperty.position[1]
                                + fullFocusProperty.position[2]
                                * fullFocusProperty.position[2]),
                      std::sqrt(62.25)),
          "azimuth-full-focus-retains-elevation-and-radius");
    Geometry focusZeroGeometry = geometry;
    focusZeroGeometry.azimuthFocus = 0.0f;
    Adapter focusZero(focusZeroGeometry);
    ApplyResult focusZeroResult = focusZero.applyBatch(
        batch(0, 0.0f, 0.5f, 0.0f, 0.0f));
    WindowsProperty focusZeroProperty;
    check(focusZeroResult.disposition == Disposition::Pass
              && focusZero.evaluate(1U, 0, &focusZeroProperty, &reason)
              && near(focusZeroProperty.position[0], -5.0)
              && near(focusZeroProperty.position[2], 0.0),
          "azimuth-zero-identity");

    Adapter gain(geometry);
    check(gain.applyBatch(batch(0, 0.5f, 0.5f, 0.0f, 15.0f)).disposition
              == Disposition::Pass
              && gain.evaluate(1U, 0, &property, &reason)
              && near(property.volume, 1.0), "gain-plus-15-endpoint");
    check(gain.applyBatch(batch(1, 0.5f, 0.5f, 0.0f, -49.0f)).disposition
              == Disposition::Pass
              && gain.evaluate(1U, 1, &property, &reason)
              && near(property.volume, 0.000630957), "gain-minus-49-endpoint");
    check(gain.applyBatch(batch(2, 0.5f, 0.5f, 0.0f, 0.0f, 4U)).disposition
              == Disposition::Pass
              && gain.evaluate(1U, 6, &property, &reason)
              && near(property.volume, 0.177827941), "gain-zero-endpoint");
    check(gain.applyBatch(batch(7, 0.5f, 0.5f, 0.0f, 0.0f, 0U, false,
                               false)).disposition == Disposition::Pass
              && gain.evaluate(1U, 7, &property, &reason)
              && near(property.volume, 0.0), "inactive-is-silent");
    check(gain.applyBatch(batch(8, 0.5f, 0.5f, 0.0f, 0.0f, 0U, false,
                               true, true)).disposition == Disposition::Pass
              && gain.evaluate(1U, 8, &property, &reason)
              && near(property.volume, 0.0), "minus-infinity-is-silent");

    Adapter finiteGainRamp(geometry);
    check(finiteGainRamp.applyBatch(batch(0, 0.5f, 0.5f, 0.0f, 0.0f)).disposition
              == Disposition::Pass
              && finiteGainRamp.applyBatch(
                     batch(10, 0.5f, 0.5f, 0.0f, 15.0f, 10U)).disposition
              == Disposition::Pass
              && finiteGainRamp.evaluate(1U, 15, &property, &reason)
              && near(property.gainDb, 7.5)
              && near(property.volume, 0.4216965),
          "finite-gain-db-midpoint");

    Adapter activeToInactive(geometry);
    check(activeToInactive.applyBatch(batch(0, 0.5f, 0.5f, 0.0f, 0.0f)).disposition
              == Disposition::Pass
              && activeToInactive.applyBatch(
                     batch(10, 0.5f, 0.5f, 0.0f, 0.0f, 10U, false, false)).disposition
              == Disposition::Pass
              && activeToInactive.evaluate(1U, 15, &property, &reason)
              && near(property.volume, 0.08891397)
              && property.active && !property.minusInfinity,
          "active-to-inactive-midpoint");
    Adapter inactiveToActive(geometry);
    check(inactiveToActive.applyBatch(
                     batch(0, 0.5f, 0.5f, 0.0f, 0.0f, 0U, false, false)).disposition
              == Disposition::Pass
              && inactiveToActive.applyBatch(
                     batch(10, 0.5f, 0.5f, 0.0f, 0.0f, 10U)).disposition
              == Disposition::Pass
              && inactiveToActive.evaluate(1U, 15, &property, &reason)
              && near(property.volume, 0.08891397)
              && property.active && !property.minusInfinity,
          "inactive-to-active-midpoint");

    auto silentRamp = batch(9, 0.5f, 0.5f, 0.0f, 0.0f, 4U, false, false);
    check(gain.applyBatch(silentRamp).disposition == Disposition::Pass
              && gain.evaluate(1U, 11, &property, &reason)
              && near(property.volume, 0.0), "silence-amplitude-ramp");

    Adapter independent(geometry);
    auto independentFirst = batch(0, 0.5f, 0.5f, 0.0f, 0.0f);
    check(independent.applyBatch(independentFirst).disposition == Disposition::Pass,
          "all-fifteen-identities");
    auto independentSecond = batch(10, 0.5f, 0.5f, 0.0f, 0.0f, 10U);
    independentSecond[0].state.position.x = 0.0f;
    independentSecond[1].state.position.x = 1.0f;
    ApplyResult independentResult = independent.applyBatch(independentSecond);
    WindowsProperty firstIndependent;
    WindowsProperty secondIndependent;
    check(independentResult.disposition == Disposition::Pass
              && independent.evaluate(1U, 15, &firstIndependent, &reason)
              && independent.evaluate(2U, 15, &secondIndependent, &reason)
              && firstIndependent.position[0] < 0.0f
              && secondIndependent.position[0] > 0.0f,
          "independent-ramp-histories");

    Adapter sparse(geometry);
    std::vector<Update> sparseUpdates {
        Update {0, 0U, 0U, 1U, state(1U, 0.0f, 0.5f, 0.0f, false, 0.0f)},
        Update {0, 0U, 0U, 3U, state(3U, 1.0f, 0.5f, 0.0f, false, 0.0f)},
        Update {4, 0U, 0U, 2U, state(2U, 0.5f, 0.5f, 0.0f, false, 0.0f)}};
    check(sparse.applyBatch(sparseUpdates).disposition == Disposition::Pass,
          "sparse-valid-multi-position-batch");
    WindowsProperty sparseProperty;
    check(sparse.evaluate(1U, 0, &sparseProperty, &reason)
              && !sparse.evaluate(2U, 0, &sparseProperty, &reason),
          "sparse-untouched-object-state");
    auto duplicateSparse = sparseUpdates;
    duplicateSparse[1].objectIndex = 1U;
    duplicateSparse[1].state.objectIndex = 1U;
    check(sparse.applyBatch(duplicateSparse).disposition == Disposition::Malformed,
          "sparse-duplicate-equal-position-rejection");
    auto descendingSparse = sparseUpdates;
    descendingSparse[1].objectIndex = 0U;
    descendingSparse[1].state.objectIndex = 0U;
    check(sparse.applyBatch(descendingSparse).disposition == Disposition::Malformed,
          "sparse-descending-equal-position-rejection");
    auto lateSparse = sparseUpdates;
    lateSparse[0].sourcePosition = 8;
    lateSparse[1].sourcePosition = 8;
    lateSparse[2].sourcePosition = 9;
    lateSparse[1].state.snap = true;
    const Metrics sparseBefore = sparse.metrics();
    WindowsProperty sparseBeforeProperty;
    sparse.evaluate(1U, 4, &sparseBeforeProperty, &reason);
    ApplyResult lateSparseResult = sparse.applyBatch(lateSparse);
    WindowsProperty sparseAfterProperty;
    sparse.evaluate(1U, 4, &sparseAfterProperty, &reason);
    check(lateSparseResult.disposition == Disposition::Unsupported
              && lateSparseResult.records.empty()
              && sparse.metrics().updatesCommitted == sparseBefore.updatesCommitted
              && sparseBeforeProperty.position == sparseAfterProperty.position,
          "sparse-late-failure-rollback");

    auto badPriority = batch(23, 0.5f, 0.5f, 0.0f, 0.0f);
    badPriority[0].state.priority = std::numeric_limits<float>::quiet_NaN();
    check(adapter.applyBatch(badPriority).disposition == Disposition::Malformed,
          "nonfinite-priority");
    badPriority[0].state.priority = 1.1f;
    check(adapter.applyBatch(badPriority).disposition == Disposition::Malformed,
          "priority-above-one");
    auto overflowRamp = batch(std::numeric_limits<std::int64_t>::max(),
                               0.5f, 0.5f, 0.0f, 0.0f, 1U);
    check(adapter.applyBatch(overflowRamp).disposition == Disposition::Malformed,
          "ramp-end-overflow");

    auto unsupported = batch(30, 0.5f, 0.5f, 0.0f, 0.0f);
    unsupported[0].state.snap = true;
    ApplyResult unsupportedResult = adapter.applyBatch(unsupported);
    check(unsupportedResult.disposition == Disposition::Unsupported,
          "unsupported-snap");
    auto rollback = batch(40, 0.5f, 0.5f, 0.0f, 0.0f);
    rollback[14].state.size[0] = 0.1f;
    WindowsProperty beforeRollbackProperty;
    adapter.evaluate(1U, 22, &beforeRollbackProperty, &reason);
    const Metrics beforeRollback = adapter.metrics();
    ApplyResult rollbackResult = adapter.applyBatch(rollback);
    const Metrics afterRollback = adapter.metrics();
    WindowsProperty afterRollbackProperty;
    const bool rollbackStateUnchanged = adapter.evaluate(
        1U, 22, &afterRollbackProperty, &reason)
        && beforeRollbackProperty.position == afterRollbackProperty.position
        && beforeRollbackProperty.volume == afterRollbackProperty.volume;
    bool rollbackMetricsUnchanged = beforeRollback.updatesCommitted
        == afterRollback.updatesCommitted
        && beforeRollback.screenConversions == afterRollback.screenConversions
        && beforeRollback.roomConversions == afterRollback.roomConversions
        && beforeRollback.updatesByObject == afterRollback.updatesByObject
        && beforeRollback.positionMin == afterRollback.positionMin
        && beforeRollback.positionMax == afterRollback.positionMax
        && beforeRollback.volumeMin == afterRollback.volumeMin
        && beforeRollback.volumeMax == afterRollback.volumeMax;
    check(rollbackResult.disposition == Disposition::Unsupported
              && rollbackResult.records.empty() && rollbackMetricsUnchanged
              && rollbackStateUnchanged,
          "transactional-rollback");
    auto infinity = batch(41, 0.5f, 0.5f, 0.0f, 0.0f);
    infinity[0].state.position.distanceInfinite = true;
    check(adapter.applyBatch(infinity).disposition == Disposition::Unsupported,
          "unsupported-infinity");
    auto badZone = batch(42, 0.5f, 0.5f, 0.0f, 0.0f);
    badZone[0].state.zoneConstraints[0] = false;
    check(adapter.applyBatch(badZone).disposition == Disposition::Unsupported,
          "unsupported-zone");
    auto badIndex = batch(43, 0.5f, 0.5f, 0.0f, 0.0f);
    badIndex[0].objectIndex = 16U;
    badIndex[0].state.objectIndex = 16U;
    check(adapter.applyBatch(badIndex).disposition == Disposition::Malformed,
          "invalid-object-index");
    auto nonfinite = batch(44, 0.5f, 0.5f, 0.0f, 0.0f);
    nonfinite[0].state.position.x = std::numeric_limits<float>::quiet_NaN();
    check(adapter.applyBatch(nonfinite).disposition == Disposition::Malformed,
          "nonfinite-property");
    auto nonmonotonic = batch(50, 0.5f, 0.5f, 0.0f, 0.0f);
    auto late = batch(49, 0.5f, 0.5f, 0.0f, 0.0f);
    nonmonotonic.insert(nonmonotonic.end(), late.begin(), late.end());
    check(adapter.applyBatch(nonmonotonic).disposition == Disposition::Malformed,
          "nonmonotonic-update");
    Geometry badGeometry = geometry;
    badGeometry.gainHeadroomDb = 14.0;
    Adapter badGeometryAdapter(badGeometry);
    check(badGeometryAdapter.applyBatch(batch(0, 0.5f, 0.5f, 0.0f, 0.0f)).disposition
              == Disposition::Malformed,
          "bad-headroom-apply-rejection");
    auto badOrder = batch(50, 0.5f, 0.5f, 0.0f, 0.0f);
    badOrder[1].objectIndex = 1U;
    badOrder[1].state.objectIndex = 1U;
    check(adapter.applyBatch(badOrder).disposition == Disposition::Malformed,
          "duplicate-object-rejection");
    Adapter fresh(geometry);
    adapter.reset();
    ApplyResult resetResult = adapter.applyBatch(batch(0, 0.5f, 0.5f, 0.0f, 0.0f));
    ApplyResult freshResult = fresh.applyBatch(batch(0, 0.5f, 0.5f, 0.0f, 0.0f));
    check(resetResult.disposition == freshResult.disposition
              && resetResult.records[0].target.position == freshResult.records[0].target.position,
          "reset-fresh-equivalence");
    report.pass = report.reason.empty();
    return report;
}

} // namespace eac3gate7b
