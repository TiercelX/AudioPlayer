#include "scene-object-gain-frame.h"

#include <algorithm>
#include <cmath>

namespace eac3render {
namespace {
bool finite(double value) { return std::isfinite(value); }
bool unit(const UnitVector3 &v)
{
    double power = 0.0;
    for (double x : v) { if (!finite(x)) return false; power += x * x; }
    return std::abs(power - 1.0) <= 1.0e-8;
}
bool room(const std::array<float, 3> &v)
{
    return finite(v[0]) && finite(v[1]) && finite(v[2])
        && v[0] >= 0.0F && v[0] <= 1.0F
        && v[1] >= 0.0F && v[1] <= 1.0F
        && v[2] >= -1.0F && v[2] <= 1.0F;
}

bool eligibleByZone(const Bs2051SpeakerPosition &speaker,
                    const std::array<bool, 6> &zones)
{
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

bool anchorIndex(BedLabel label, std::size_t *index)
{
    Bs2051Label target;
    switch (label) {
    case BedLabel::FL: target = Bs2051Label::MMinus030; break;
    case BedLabel::FR: target = Bs2051Label::MPlus030; break;
    case BedLabel::FC: target = Bs2051Label::MPlus000; break;
    case BedLabel::SL: target = Bs2051Label::MMinus090; break;
    case BedLabel::SR: target = Bs2051Label::MPlus090; break;
    case BedLabel::TFL: target = Bs2051Label::UMinus045; break;
    case BedLabel::TFR: target = Bs2051Label::UPlus045; break;
    case BedLabel::LFE: return false;
    }
    const auto &layout = Bs2051SystemHLayout::systemH();
    for (std::size_t i = 0; i < layout.size(); ++i)
        if (layout[i].label == target) { *index = i; return true; }
    return false;
}
}

SceneObjectGainFrameResult makeSystemHGainFrame(
    const std::vector<SceneObjectSnapshot> &snapshot,
    const Bs2127SystemHConfiguredPanner &panner)
{
    SceneObjectGainFrameResult result;
    if (!panner.valid()) {
        result.reason = panner.reason();
        return result;
    }
    std::uint64_t previous = 0U;
    result.objects.reserve(snapshot.size());
    for (const auto &object : snapshot) {
        if (object.objectId == 0U || object.objectId <= previous
            || !finite(object.gain) || object.gain < 0.0F
            || (object.coordinateSpace
                    == ObjectMetadataUpdate::CoordinateSpace::UnitDirection
                ? !object.pointDirectionPresent || !unit(object.position)
                : object.pointDirectionPresent || !room(object.roomPosition))) {
            result.objects.clear();
            result.summedGains.fill(0.0);
            result.reason = "invalid-snapshot-object";
            return result;
        }
        SceneObjectSpeakerGain frame;
        frame.objectId = object.objectId;
        if (object.gain > 0.0F) {
            if (object.warpMode == ObjectWarpMode::Reserved) {
                result.objects.clear(); result.summedGains.fill(0.0);
                result.reason = "reserved-warp-mode";
                return result;
            }
            std::array<double, kSystemHSpeakerCount> gains {};
            if (object.coordinateSpace
                == ObjectMetadataUpdate::CoordinateSpace::UnitDirection) {
                UnitVector3 position = object.position;
                if (object.warpMode == ObjectWarpMode::PositionY2) {
                    position[1] *= 2.0;
                    if (!unit(position)) {
                        result.objects.clear(); result.summedGains.fill(0.0);
                        result.reason = "warp-position-y2-invalid";
                        return result;
                    }
                    const double norm = std::sqrt(position[0] * position[0]
                        + position[1] * position[1] + position[2] * position[2]);
                    for (double &component : position) component /= norm;
                }
                const auto rendered = panner.render(position);
                if (rendered.status != Bs2127PointSourcePannerStatus::Selected) {
                    result.objects.clear();
                    result.summedGains.fill(0.0);
                    result.reason = rendered.reason;
                    return result;
                }
                gains = rendered.gains;
            } else {
                std::array<double, 3> point {};
                auto roomPosition = object.roomPosition;
                if (object.warpMode == ObjectWarpMode::PositionY2)
                    roomPosition[1] = std::clamp(roomPosition[1] * 2.0F, 0.0F, 1.0F);
                if (!Bs2127SystemHCartesianPanner::mapEtsiRoomToAllocentric(
                        roomPosition, &point, &result.reason)) {
                    result.objects.clear();
                    result.summedGains.fill(0.0);
                    return result;
                }
                const auto rendered = Bs2127SystemHCartesianPanner {}.render(point);
                if (!rendered.accepted) {
                    result.objects.clear();
                    result.summedGains.fill(0.0);
                    result.reason = rendered.reason;
                    return result;
                }
                gains = rendered.gains;
            }
            const auto &layout = Bs2051SystemHLayout::systemH();
            if (object.speakerAnchorPresent) {
                std::size_t anchor = kSystemHSpeakerCount;
                anchorIndex(object.speakerAnchor, &anchor);
                if (anchor >= layout.size()) anchor = kSystemHSpeakerCount;
                if (anchor == kSystemHSpeakerCount) {
                    result.objects.clear(); result.summedGains.fill(0.0);
                    result.reason = "speaker-anchor-unmapped";
                    return result;
                }
                gains.fill(0.0); gains[anchor] = 1.0;
            } else if (object.snap) {
                if (object.coordinateSpace != ObjectMetadataUpdate::CoordinateSpace::UnitDirection) {
                    result.objects.clear(); result.summedGains.fill(0.0);
                    result.reason = "snap-requires-unit-direction";
                    return result;
                }
                std::size_t nearest = kSystemHSpeakerCount; double best = 1.0e30;
                for (std::size_t i = 0; i < layout.size(); ++i) {
                    if (!eligibleByZone(layout[i], object.zoneConstraints)) continue;
                    double d = 0.0; const auto direction = layout[i].unitVector();
                    for (std::size_t axis = 0; axis < 3; ++axis) {
                        const double delta = object.position[axis] - direction[axis]; d += delta * delta;
                    }
                    d = std::sqrt(d); if (d < best) { best = d; nearest = i; }
                }
                if (nearest == kSystemHSpeakerCount || best > 0.4) {
                    result.objects.clear(); result.summedGains.fill(0.0);
                    result.reason = "snap-no-eligible-speaker-within-0.4";
                    return result;
                }
                gains.fill(0.0); gains[nearest] = 1.0;
            } else if (!std::all_of(object.zoneConstraints.begin(), object.zoneConstraints.end(),
                                    [](bool enabled) { return enabled; })) {
                double power = 0.0;
                for (std::size_t i = 0; i < layout.size(); ++i) {
                    power += gains[i] * gains[i];
                    if (!eligibleByZone(layout[i], object.zoneConstraints)) gains[i] = 0.0;
                }
                double kept = 0.0; for (double gain : gains) kept += gain * gain;
                if (!(kept > 0.0)) {
                    result.objects.clear(); result.summedGains.fill(0.0);
                    result.reason = "zone-constraints-remove-all-speakers";
                    return result;
                }
                const double scale = std::sqrt(power / kept);
                for (double &gain : gains) gain *= scale;
            }
            for (std::size_t i = 0U; i < kSystemHSpeakerCount; ++i) {
                frame.gains[i] = gains[i] * object.gain;
                result.summedGains[i] += frame.gains[i];
                if (!finite(frame.gains[i]) || !finite(result.summedGains[i])) {
                    result.objects.clear();
                    result.summedGains.fill(0.0);
                    result.reason = "nonfinite-gain-frame";
                    return result;
                }
            }
        }
        result.objects.push_back(frame);
        previous = object.objectId;
    }
    result.disposition = SceneAdapterDisposition::Accepted;
    result.reason = snapshot.empty() ? "empty-snapshot-silence" : "system-h-gain-frame";
    return result;
}

} // namespace eac3render
