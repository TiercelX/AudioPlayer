#include "bs2051-layout.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace eac3render {
namespace {

constexpr double kPi = 3.1415926535897932384626433832795;

void setReason(std::string *reason, const char *value)
{
    if (reason) {
        *reason = value;
    }
}

constexpr Bs2051SpeakerPosition position(Bs2051Label label, Bs2051Layer layer,
                                         double azimuth, double elevation)
{
    return Bs2051SpeakerPosition {label, layer, azimuth, elevation};
}

const std::array<Bs2051SpeakerPosition, kSystemHSpeakerCount> kSystemH {
    // Middle layer: 10 speakers.
    position(Bs2051Label::MPlus060, Bs2051Layer::Middle, 60.0, 0.0),
    position(Bs2051Label::MMinus060, Bs2051Layer::Middle, -60.0, 0.0),
    position(Bs2051Label::MPlus000, Bs2051Layer::Middle, 0.0, 0.0),
    position(Bs2051Label::MPlus135, Bs2051Layer::Middle, 135.0, 0.0),
    position(Bs2051Label::MMinus135, Bs2051Layer::Middle, -135.0, 0.0),
    position(Bs2051Label::MPlus030, Bs2051Layer::Middle, 30.0, 0.0),
    position(Bs2051Label::MMinus030, Bs2051Layer::Middle, -30.0, 0.0),
    position(Bs2051Label::MPlus180, Bs2051Layer::Middle, 180.0, 0.0),
    position(Bs2051Label::MPlus090, Bs2051Layer::Middle, 90.0, 0.0),
    position(Bs2051Label::MMinus090, Bs2051Layer::Middle, -90.0, 0.0),

    // Upper layer: 9 speakers, including the top speaker T+000.
    position(Bs2051Label::UPlus045, Bs2051Layer::Upper, 45.0, 30.0),
    position(Bs2051Label::UMinus045, Bs2051Layer::Upper, -45.0, 30.0),
    position(Bs2051Label::UPlus000, Bs2051Layer::Upper, 0.0, 30.0),
    position(Bs2051Label::TPlus000, Bs2051Layer::Upper, 0.0, 90.0),
    position(Bs2051Label::UPlus135, Bs2051Layer::Upper, 135.0, 30.0),
    position(Bs2051Label::UMinus135, Bs2051Layer::Upper, -135.0, 30.0),
    position(Bs2051Label::UPlus090, Bs2051Layer::Upper, 90.0, 30.0),
    position(Bs2051Label::UMinus090, Bs2051Layer::Upper, -90.0, 30.0),
    position(Bs2051Label::UPlus180, Bs2051Layer::Upper, 180.0, 30.0),

    // Bottom layer: 3 speakers.
    position(Bs2051Label::BPlus000, Bs2051Layer::Bottom, 0.0, -30.0),
    position(Bs2051Label::BPlus045, Bs2051Layer::Bottom, 45.0, -30.0),
    position(Bs2051Label::BMinus045, Bs2051Layer::Bottom, -45.0, -30.0),
};

bool finite(double value)
{
    return std::isfinite(value);
}

double absoluteAngleError(double expected, double actual)
{
    double error = std::abs(expected - actual);
    while (error > 360.0) {
        error -= 360.0;
    }
    return std::min(error, 360.0 - error);
}

} // namespace

const char *bs2051LabelName(Bs2051Label label)
{
    switch (label) {
    case Bs2051Label::MPlus060: return "M+060";
    case Bs2051Label::MMinus060: return "M-060";
    case Bs2051Label::MPlus000: return "M+000";
    case Bs2051Label::MPlus135: return "M+135";
    case Bs2051Label::MMinus135: return "M-135";
    case Bs2051Label::MPlus030: return "M+030";
    case Bs2051Label::MMinus030: return "M-030";
    case Bs2051Label::MPlus180: return "M+180";
    case Bs2051Label::MPlus090: return "M+090";
    case Bs2051Label::MMinus090: return "M-090";
    case Bs2051Label::UPlus045: return "U+045";
    case Bs2051Label::UMinus045: return "U-045";
    case Bs2051Label::UPlus000: return "U+000";
    case Bs2051Label::TPlus000: return "T+000";
    case Bs2051Label::UPlus135: return "U+135";
    case Bs2051Label::UMinus135: return "U-135";
    case Bs2051Label::UPlus090: return "U+090";
    case Bs2051Label::UMinus090: return "U-090";
    case Bs2051Label::UPlus180: return "U+180";
    case Bs2051Label::BPlus000: return "B+000";
    case Bs2051Label::BPlus045: return "B+045";
    case Bs2051Label::BMinus045: return "B-045";
    }
    return "unknown";
}

std::array<double, 3> Bs2051SpeakerPosition::unitVector() const
{
    const double azimuth = azimuthDegrees * kPi / 180.0;
    const double elevation = elevationDegrees * kPi / 180.0;
    const double horizontal = std::cos(elevation);
    return {
        horizontal * std::cos(azimuth),
        horizontal * std::sin(azimuth),
        std::sin(elevation),
    };
}

const std::array<Bs2051SpeakerPosition, kSystemHSpeakerCount> &
Bs2051SystemHLayout::systemH()
{
    return kSystemH;
}

bool Bs2051SystemHLayout::find(Bs2051Label label,
                               Bs2051SpeakerPosition *position,
                               std::string *reason)
{
    if (!position) {
        setReason(reason, "null-speaker-position");
        return false;
    }

    const auto found = std::find_if(kSystemH.begin(), kSystemH.end(),
                                    [label](const Bs2051SpeakerPosition &candidate) {
                                        return candidate.label == label;
                                    });
    if (found == kSystemH.end()) {
        setReason(reason, "speaker-label-not-in-system-h");
        return false;
    }
    *position = *found;
    return true;
}

bool Bs2051SystemHLayout::validateSystemH(std::string *reason)
{
    std::size_t upperCount = 0U;
    std::size_t middleCount = 0U;
    std::size_t bottomCount = 0U;
    for (std::size_t index = 0U; index < kSystemH.size(); ++index) {
        const Bs2051SpeakerPosition &current = kSystemH[index];
        if (!finite(current.azimuthDegrees) || !finite(current.elevationDegrees)
            || std::abs(current.azimuthDegrees) > 180.0
            || std::abs(current.elevationDegrees) > 90.0) {
            setReason(reason, "system-h-position-out-of-range");
            return false;
        }
        const std::array<double, 3> vector = current.unitVector();
        double norm = 0.0;
        for (double component : vector) {
            if (!finite(component)) {
                setReason(reason, "system-h-nonfinite-unit-vector");
                return false;
            }
            norm += component * component;
        }
        if (std::abs(norm - 1.0) > 1.0e-12) {
            setReason(reason, "system-h-unit-vector-not-normalized");
            return false;
        }
        for (std::size_t prior = 0U; prior < index; ++prior) {
            if (kSystemH[prior].label == current.label) {
                setReason(reason, "system-h-duplicate-label");
                return false;
            }
        }
        switch (current.layer) {
        case Bs2051Layer::Upper: ++upperCount; break;
        case Bs2051Layer::Middle: ++middleCount; break;
        case Bs2051Layer::Bottom: ++bottomCount; break;
        }
    }

    if (upperCount != 9U || middleCount != 10U || bottomCount != 3U) {
        setReason(reason, "system-h-layer-count-mismatch");
        return false;
    }

    // Check the expected left/right mirrors.  Centre and 180-degree labels
    // are self-symmetric and are checked by the finite/unit-vector checks.
    constexpr std::array<std::pair<Bs2051Label, Bs2051Label>, 8> mirrors {{
        {Bs2051Label::MPlus060, Bs2051Label::MMinus060},
        {Bs2051Label::MPlus135, Bs2051Label::MMinus135},
        {Bs2051Label::MPlus030, Bs2051Label::MMinus030},
        {Bs2051Label::MPlus090, Bs2051Label::MMinus090},
        {Bs2051Label::UPlus045, Bs2051Label::UMinus045},
        {Bs2051Label::UPlus135, Bs2051Label::UMinus135},
        {Bs2051Label::UPlus090, Bs2051Label::UMinus090},
        {Bs2051Label::BPlus045, Bs2051Label::BMinus045},
    }};
    for (const auto &mirror : mirrors) {
        Bs2051SpeakerPosition left;
        Bs2051SpeakerPosition right;
        if (!find(mirror.first, &left, reason)
            || !find(mirror.second, &right, reason)
            || std::abs(left.azimuthDegrees + right.azimuthDegrees) > 1.0e-12
            || std::abs(left.elevationDegrees - right.elevationDegrees) > 1.0e-12) {
            setReason(reason, "system-h-mirror-mismatch");
            return false;
        }
    }
    return true;
}

SofaLayoutComparison compareSystemHToSofa(
    const std::vector<SofaEmitterRecord> &emitters,
    double azimuthToleranceDegrees, double elevationToleranceDegrees)
{
    SofaLayoutComparison result;
    if (!finite(azimuthToleranceDegrees) || azimuthToleranceDegrees < 0.0
        || !finite(elevationToleranceDegrees) || elevationToleranceDegrees < 0.0) {
        result.reason = "invalid-angle-tolerance";
        return result;
    }
    if (emitters.size() != kSystemHSpeakerCount) {
        result.reason = "sofa-emitter-count-mismatch";
        return result;
    }

    result.compared = emitters.size();
    result.usedEmitterOrder = true;
    result.perEmitterDescriptionMissing = true;
    result.minimumDistanceMetres = std::numeric_limits<double>::infinity();
    result.maximumDistanceMetres = 0.0;
    const auto &catalog = Bs2051SystemHLayout::systemH();
    for (std::size_t index = 0U; index < emitters.size(); ++index) {
        const SofaEmitterRecord &measured = emitters[index];
        const Bs2051SpeakerPosition &expected = catalog[index];
        if (measured.emitterIndex != index) {
            ++result.mismatches;
        }
        if (!measured.description.empty()) {
            result.usedEmitterOrder = false;
            result.perEmitterDescriptionMissing = false;
            if (measured.description != bs2051LabelName(expected.label)) {
                ++result.mismatches;
            }
        }
        if (!finite(measured.azimuthDegrees)
            || !finite(measured.elevationDegrees)
            || !finite(measured.distanceMetres)
            || measured.distanceMetres <= 0.0) {
            ++result.mismatches;
            continue;
        }
        const double azimuthError = absoluteAngleError(
            expected.azimuthDegrees, measured.azimuthDegrees);
        const double elevationError = std::abs(
            expected.elevationDegrees - measured.elevationDegrees);
        result.maxAzimuthErrorDegrees =
            std::max(result.maxAzimuthErrorDegrees, azimuthError);
        result.maxElevationErrorDegrees =
            std::max(result.maxElevationErrorDegrees, elevationError);
        result.minimumDistanceMetres =
            std::min(result.minimumDistanceMetres, measured.distanceMetres);
        result.maximumDistanceMetres =
            std::max(result.maximumDistanceMetres, measured.distanceMetres);
        if (azimuthError > azimuthToleranceDegrees
            || elevationError > elevationToleranceDegrees) {
            ++result.mismatches;
        }
    }

    if (!finite(result.minimumDistanceMetres)) {
        result.minimumDistanceMetres = 0.0;
    }
    result.pass = result.mismatches == 0U;
    if (!result.pass) {
        result.reason = "sofa-angle-or-record-mismatch";
    }
    return result;
}

} // namespace eac3render
