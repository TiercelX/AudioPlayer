#include "bs2127-system-h-cartesian-panner.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace eac3render {
namespace {
constexpr double kPi = 3.1415926535897932384626433832795;
constexpr double kPlaneEpsilon = 0.001;

const std::array<std::array<double, 3>, kSystemHSpeakerCount> kSpeakers {{
    {{-1.0, 0.414214, 0.0}}, {{1.0, 0.414214, 0.0}},
    {{0.0, 1.0, 0.0}}, {{-1.0, -1.0, 0.0}}, {{1.0, -1.0, 0.0}},
    {{-1.0, 1.0, 0.0}}, {{1.0, 1.0, 0.0}}, {{0.0, -1.0, 0.0}},
    {{-1.0, 0.0, 0.0}}, {{1.0, 0.0, 0.0}},
    {{-1.0, 1.0, 1.0}}, {{1.0, 1.0, 1.0}}, {{0.0, 1.0, 1.0}},
    {{0.0, 0.0, 1.0}}, {{-1.0, -1.0, 1.0}}, {{1.0, -1.0, 1.0}},
    {{-1.0, 0.0, 1.0}}, {{1.0, 0.0, 1.0}}, {{0.0, -1.0, 1.0}},
    {{0.0, 1.0, -1.0}}, {{-1.0, 1.0, -1.0}}, {{1.0, 1.0, -1.0}},
}};

bool finite(double value) { return std::isfinite(value); }

double axisGain(double value, double other)
{
    if (!finite(other)) return 1.0;
    if (value * other > 0.0) return 0.0;
    const double denominator = other - value;
    const double gain = std::cos(value / denominator * kPi * 0.5);
    return finite(gain) && gain > 1.0e-12 ? gain : 0.0;
}

template <typename Coordinate>
bool oppositeNearest(const std::array<std::array<double, 3>, kSystemHSpeakerCount>
                         &speakers,
                     const std::vector<std::size_t> &indices,
                     Coordinate coordinate, double value, double *other)
{
    if (!other) return false;
    bool found = false;
    double best = value >= 0.0 ? -std::numeric_limits<double>::infinity()
                               : std::numeric_limits<double>::infinity();
    for (std::size_t index : indices) {
        const double candidate = coordinate(speakers[index]);
        if (value >= 0.0 ? candidate >= value : candidate <= value) continue;
        if (!found || (value >= 0.0 ? candidate > best : candidate < best)) {
            found = true;
            best = candidate;
        }
    }
    if (found) *other = best;
    return found;
}

} // namespace

const std::array<std::array<double, 3>, kSystemHSpeakerCount> &
Bs2127SystemHCartesianPanner::speakerCoordinates()
{
    return kSpeakers;
}

bool Bs2127SystemHCartesianPanner::mapEtsiRoomToAllocentric(
    const std::array<float, 3> &room, std::array<double, 3> *allocentric,
    std::string *reason)
{
    if (!allocentric) {
        if (reason) *reason = "null-allocentric-output";
        return false;
    }
    for (float value : room) {
        if (!std::isfinite(value)) {
            if (reason) *reason = "nonfinite-etsi-room-position";
            return false;
        }
    }
    // ETSI normalized room: x left->right, y front->back, z floor->ceiling.
    // BS.2127 allocentric cube: x right-positive, y front-positive, z up.
    *allocentric = {2.0 * room[0] - 1.0, 1.0 - 2.0 * room[1], room[2]};
    for (double value : *allocentric) {
        if (!finite(value)) {
            if (reason) *reason = "nonfinite-allocentric-position";
            return false;
        }
    }
    return true;
}

Bs2127CartesianPannerResult Bs2127SystemHCartesianPanner::render(
    const std::array<double, 3> &point) const
{
    Bs2127CartesianPannerResult result;
    for (double value : point) {
        if (!finite(value)) {
            result.reason = "nonfinite-cartesian-point";
            return result;
        }
    }
    double power = 0.0;
    for (double value : point) power += value * value;
    if (!finite(power)) {
        result.reason = "cartesian-point-overflow";
        return result;
    }
    std::vector<std::size_t> speakerIndices;
    speakerIndices.reserve(kSpeakers.size());
    for (std::size_t index = 0U; index < kSpeakers.size(); ++index)
        speakerIndices.push_back(index);
    for (std::size_t speaker = 0U; speaker < kSpeakers.size(); ++speaker) {
        const auto &current = kSpeakers[speaker];
        const std::array<double, 3> shifted {
            current[0] - point[0], current[1] - point[1], current[2] - point[2]};
        double zOther = 0.0;
        const bool hasZ = oppositeNearest(
            kSpeakers, speakerIndices,
            [&point](const auto &value) { return value[2] - point[2]; },
            shifted[2], &zOther);
        const double gz = hasZ ? axisGain(shifted[2], zOther) : 1.0;
        std::vector<std::size_t> plane;
        for (std::size_t index : speakerIndices)
            if (std::abs(kSpeakers[index][2] - current[2]) < kPlaneEpsilon)
                plane.push_back(index);
        double yOther = 0.0;
        const bool hasY = oppositeNearest(
            kSpeakers, plane,
            [&point](const auto &value) { return value[1] - point[1]; },
            shifted[1], &yOther);
        const double gy = hasY ? axisGain(shifted[1], yOther) : 1.0;
        std::vector<std::size_t> row;
        for (std::size_t index : plane)
            if (std::abs(kSpeakers[index][1] - current[1]) < kPlaneEpsilon)
                row.push_back(index);
        double xOther = 0.0;
        const bool hasX = oppositeNearest(
            kSpeakers, row,
            [&point](const auto &value) { return value[0] - point[0]; },
            shifted[0], &xOther);
        const double gx = hasX ? axisGain(shifted[0], xOther) : 1.0;
        const double gain = gx * gy * gz;
        result.gains[speaker] = finite(gain) && gain > 1.0e-12 ? gain : 0.0;
    }
    double gainPower = 0.0;
    for (double gain : result.gains) gainPower += gain * gain;
    if (!(gainPower > 0.0) || !finite(gainPower)) {
        result.reason = "cartesian-point-outside-system-h";
        return result;
    }
    if (std::abs(gainPower - 1.0) > 1.0e-9) {
        result.reason = "cartesian-system-h-power-invalid="
            + std::to_string(gainPower);
        return result;
    }
    result.accepted = true;
    result.reason = "cartesian-system-h-selected";
    return result;
}

} // namespace eac3render
