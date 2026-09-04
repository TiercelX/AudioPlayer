#include "bs2127-nominal-to-actual.h"

#include <algorithm>
#include <cmath>

namespace eac3render {
namespace {

constexpr double kPi = 3.1415926535897932384626433832795;
constexpr double kUnitTolerance = 1.0e-6;
constexpr double kAngleToleranceDegrees = 1.0e-6;

bool finite(double value)
{
    return std::isfinite(value);
}

bool finiteVector(const UnitVector3 &vector)
{
    return std::all_of(vector.begin(), vector.end(),
                       [](double value) { return finite(value); });
}

double normSquared(const UnitVector3 &vector)
{
    return vector[0] * vector[0] + vector[1] * vector[1]
        + vector[2] * vector[2];
}

double angleDegrees(double radians)
{
    return radians * 180.0 / kPi;
}

double azimuthDegrees(const UnitVector3 &vector)
{
    return angleDegrees(std::atan2(vector[1], vector[0]));
}

double elevationDegrees(const UnitVector3 &vector)
{
    return angleDegrees(std::atan2(vector[2],
                                   std::hypot(vector[0], vector[1])));
}

double circularAngleError(double expected, double actual)
{
    double error = std::fmod(std::abs(expected - actual), 360.0);
    if (error > 180.0) {
        error = 360.0 - error;
    }
    return error;
}

struct AngleRule {
    double minimumAzimuth;
    double maximumAzimuth;
    double minimumElevation;
    double maximumElevation;
};

AngleRule ruleFor(Bs2051Label label)
{
    using L = Bs2051Label;
    switch (label) {
    case L::MPlus060:  return {45.0, 60.0, 0.0, 5.0};
    case L::MMinus060: return {-60.0, -45.0, 0.0, 5.0};
    case L::MPlus000:  return {0.0, 0.0, 0.0, 5.0};
    case L::MPlus135:  return {110.0, 135.0, 0.0, 15.0};
    case L::MMinus135: return {-135.0, -110.0, 0.0, 15.0};
    case L::MPlus030:  return {22.5, 30.0, 0.0, 5.0};
    case L::MMinus030: return {-30.0, -22.5, 0.0, 5.0};
    case L::MPlus180:  return {180.0, 180.0, 0.0, 15.0};
    case L::MPlus090:  return {90.0, 90.0, 0.0, 15.0};
    case L::MMinus090: return {-90.0, -90.0, 0.0, 15.0};
    case L::UPlus045:  return {45.0, 60.0, 30.0, 45.0};
    case L::UMinus045: return {-60.0, -45.0, 30.0, 45.0};
    case L::UPlus000:  return {0.0, 0.0, 30.0, 45.0};
    case L::TPlus000:  return {0.0, 0.0, 90.0, 90.0};
    case L::UPlus135:  return {110.0, 135.0, 30.0, 45.0};
    case L::UMinus135: return {-135.0, -110.0, 30.0, 45.0};
    case L::UPlus090:  return {90.0, 90.0, 30.0, 45.0};
    case L::UMinus090: return {-90.0, -90.0, 30.0, 45.0};
    case L::UPlus180:  return {180.0, 180.0, 30.0, 45.0};
    case L::BPlus000:  return {0.0, 0.0, -30.0, -15.0};
    case L::BPlus045:  return {45.0, 60.0, -30.0, -15.0};
    case L::BMinus045: return {-60.0, -45.0, -30.0, -15.0};
    }
    return {1.0, 0.0, 1.0, 0.0};
}

bool inRange(double value, double minimum, double maximum)
{
    return value >= minimum - kAngleToleranceDegrees
        && value <= maximum + kAngleToleranceDegrees;
}

Bs2127SystemHRealLayout nominalLayout()
{
    Bs2127SystemHRealLayout result;
    const auto &layout = Bs2051SystemHLayout::systemH();
    for (std::size_t index = 0U; index < layout.size(); ++index) {
        result.realVectors[index] = layout[index].unitVector();
    }
    result.valid = true;
    result.reason = "none";
    return result;
}

} // namespace

Bs2127SystemHRealLayout makeSystemHRealLayout(
    const std::vector<Bs2127RealLoudspeaker> &speakers)
{
    if (speakers.empty()) {
        return nominalLayout();
    }

    Bs2127SystemHRealLayout result;
    std::array<bool, kSystemHSpeakerCount> seen {};
    const auto &nominal = Bs2051SystemHLayout::systemH();
    if (speakers.size() != kSystemHSpeakerCount) {
        result.reason = "system-h-real-layout-count-mismatch";
        return result;
    }
    for (const Bs2127RealLoudspeaker &speaker : speakers) {
        const auto found = std::find_if(
            nominal.begin(), nominal.end(), [&speaker](const auto &candidate) {
                return candidate.label == speaker.label;
            });
        if (found == nominal.end()) {
            result.reason = "real-layout-label-not-in-system-h";
            return result;
        }
        const std::size_t index = static_cast<std::size_t>(
            std::distance(nominal.begin(), found));
        if (seen[index]) {
            result.reason = "real-layout-duplicate-label";
            return result;
        }
        seen[index] = true;
        if (!finiteVector(speaker.unitVector)
            || std::abs(normSquared(speaker.unitVector) - 1.0)
                > kUnitTolerance) {
            result.reason = "real-layout-vector-not-finite-unit";
            return result;
        }
        const AngleRule rule = ruleFor(speaker.label);
        const double azimuth = azimuthDegrees(speaker.unitVector);
        const double elevation = elevationDegrees(speaker.unitVector);
        if (!finite(azimuth) || !finite(elevation)
            || (!inRange(azimuth, rule.minimumAzimuth, rule.maximumAzimuth)
                && !(rule.minimumAzimuth == rule.maximumAzimuth
                     && circularAngleError(rule.minimumAzimuth, azimuth)
                         <= kAngleToleranceDegrees))
            || !inRange(elevation, rule.minimumElevation,
                        rule.maximumElevation)) {
            result.reason = "real-layout-position-out-of-bs2051-range";
            return result;
        }
        result.realVectors[index] = speaker.unitVector;
    }
    if (!std::all_of(seen.begin(), seen.end(), [](bool value) { return value; })) {
        result.reason = "real-layout-missing-label";
        return result;
    }
    for (std::size_t first = 0U; first < kSystemHSpeakerCount; ++first) {
        for (std::size_t second = first + 1U;
             second < kSystemHSpeakerCount; ++second) {
            double distanceSquared = 0.0;
            for (std::size_t component = 0U; component < 3U; ++component) {
                const double delta = result.realVectors[first][component]
                    - result.realVectors[second][component];
                distanceSquared += delta * delta;
            }
            if (!finite(distanceSquared) || distanceSquared <= 1.0e-12) {
                result.reason = "real-layout-duplicate-position";
                return result;
            }
        }
    }
    result.valid = true;
    result.reason = "none";
    return result;
}

Bs2127SystemHNominalToActualPanner::Bs2127SystemHNominalToActualPanner(
    const Bs2127TopologyOptions &options)
    : layout_(makeSystemHRealLayout({})), panner_(layout_.realVectors, options)
{
    reason_ = layout_.valid && panner_.valid() ? "none"
                                               : layout_.reason;
    if (reason_ == "none" && !panner_.valid()) {
        reason_ = panner_.reason();
    }
}

Bs2127SystemHNominalToActualPanner::Bs2127SystemHNominalToActualPanner(
    const std::vector<Bs2127RealLoudspeaker> &speakers,
    const Bs2127TopologyOptions &options)
    : layout_(makeSystemHRealLayout(speakers)),
      panner_(layout_.realVectors, options)
{
    reason_ = layout_.valid && panner_.valid() ? "none"
                                               : layout_.reason;
    if (reason_ == "none" && !panner_.valid()) {
        reason_ = panner_.reason();
    }
}

bool Bs2127SystemHNominalToActualPanner::valid() const
{
    return layout_.valid && panner_.valid();
}

const std::string &Bs2127SystemHNominalToActualPanner::reason() const
{
    return reason_;
}

const Bs2127SystemHRealLayout &
Bs2127SystemHNominalToActualPanner::layout() const
{
    return layout_;
}

const Bs2127SystemHPointSourcePanner &
Bs2127SystemHNominalToActualPanner::nominalPanner() const
{
    return panner_;
}

Bs2127PointSourcePannerResult
Bs2127SystemHNominalToActualPanner::render(
    const UnitVector3 &sourceVector) const
{
    if (!valid()) {
        Bs2127PointSourcePannerResult result;
        result.status = Bs2127PointSourcePannerStatus::Unsupported;
        result.reason = reason_;
        return result;
    }
    return panner_.render(sourceVector);
}

} // namespace eac3render
