#include "stereo-diagnostic.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace eac3render {
namespace {

constexpr float kPi = 3.14159265358979323846F;
constexpr float kHalfSqrtTwo = 0.70710678118654752440F;

void setReason(std::string *reason, const char *value)
{
    if (reason) {
        *reason = value;
    }
}

bool finite(float value)
{
    return std::isfinite(value);
}

float equalPowerLeft(float azimuthDegrees)
{
    const float pan = std::clamp(azimuthDegrees / 90.0F, -1.0F, 1.0F);
    const float angle = (pan + 1.0F) * kPi * 0.25F;
    return std::cos(angle);
}

float equalPowerRight(float azimuthDegrees)
{
    const float pan = std::clamp(azimuthDegrees / 90.0F, -1.0F, 1.0F);
    const float angle = (pan + 1.0F) * kPi * 0.25F;
    return std::sin(angle);
}

} // namespace

SpeakerPosition SpeakerPosition::fromAzimuthDegrees(float azimuthDegrees)
{
    SpeakerPosition position;
    position.azimuthDegrees = azimuthDegrees;
    return position;
}

bool SpeakerPosition::fromCartesian(float x, float z,
                                    SpeakerPosition *position,
                                    std::string *reason)
{
    if (!position) {
        setReason(reason, "null-speaker-position");
        return false;
    }
    if (!finite(x) || !finite(z)) {
        setReason(reason, "nonfinite-cartesian-position");
        return false;
    }
    if (x == 0.0F && z == 0.0F) {
        setReason(reason, "zero-cartesian-position");
        return false;
    }

    const float azimuth = std::atan2(x, z) * 180.0F / kPi;
    if (!finite(azimuth) || std::abs(azimuth) > 90.0F) {
        setReason(reason, "cartesian-position-out-of-diagnostic-range");
        return false;
    }
    position->azimuthDegrees = azimuth;
    return true;
}

bool SpeakerPosition::isFiniteAndInRange() const
{
    return finite(azimuthDegrees) && std::abs(azimuthDegrees) <= 90.0F;
}

SpeakerLayout SpeakerLayout::diagnosticStereo()
{
    SpeakerLayout layout;
    layout.routes_ = {
        BedRoute {BedLabel::FL, SpeakerPosition::fromAzimuthDegrees(-90.0F), true},
        BedRoute {BedLabel::FR, SpeakerPosition::fromAzimuthDegrees(90.0F), true},
        BedRoute {BedLabel::FC, SpeakerPosition::fromAzimuthDegrees(0.0F), true},
        BedRoute {BedLabel::SL, SpeakerPosition::fromAzimuthDegrees(-90.0F), true},
        BedRoute {BedLabel::SR, SpeakerPosition::fromAzimuthDegrees(90.0F), true},
        BedRoute {BedLabel::TFL, SpeakerPosition::fromAzimuthDegrees(-90.0F), true},
        BedRoute {BedLabel::TFR, SpeakerPosition::fromAzimuthDegrees(90.0F), true},
        // LFE is intentionally disabled.  No arbitrary attenuation is applied.
        BedRoute {BedLabel::LFE, SpeakerPosition::fromAzimuthDegrees(0.0F), false},
    };
    return layout;
}

bool SpeakerLayout::routeForBed(BedLabel label, BedRoute *route,
                                std::string *reason) const
{
    if (!route) {
        setReason(reason, "null-bed-route");
        return false;
    }

    const auto found = std::find_if(routes_.begin(), routes_.end(),
                                    [label](const BedRoute &candidate) {
                                        return candidate.label == label;
                                    });
    if (found == routes_.end()) {
        setReason(reason, "unknown-bed-label");
        return false;
    }
    *route = *found;
    return true;
}

bool GainInterpolator::setRamp(float startGain, float endGain,
                               std::uint64_t durationSamples,
                               std::string *reason)
{
    if (!finite(startGain) || !finite(endGain)) {
        setReason(reason, "nonfinite-ramp-gain");
        return false;
    }

    initialized_ = true;
    startGain_ = startGain;
    endGain_ = endGain;
    durationSamples_ = durationSamples;
    position_ = 0U;
    return true;
}

float GainInterpolator::valueAt(std::uint64_t sample) const
{
    if (durationSamples_ == 0U || sample >= durationSamples_) {
        return endGain_;
    }

    const double fraction = static_cast<double>(sample)
        / static_cast<double>(durationSamples_);
    return static_cast<float>(static_cast<double>(startGain_)
                             + (static_cast<double>(endGain_)
                                - static_cast<double>(startGain_)) * fraction);
}

bool GainInterpolator::render(std::size_t frames, std::vector<float> *gains,
                              std::string *reason)
{
    if (!initialized_) {
        setReason(reason, "interpolator-uninitialized");
        return false;
    }
    if (!gains) {
        setReason(reason, "null-gain-output");
        return false;
    }
    if (position_ > std::numeric_limits<std::uint64_t>::max() - frames) {
        setReason(reason, "gain-position-overflow");
        return false;
    }

    gains->resize(frames);
    for (std::size_t index = 0U; index < frames; ++index) {
        (*gains)[index] = valueAt(position_ + index);
    }
    position_ += frames;
    return true;
}

void GainInterpolator::reset()
{
    initialized_ = false;
    startGain_ = 0.0F;
    endGain_ = 0.0F;
    durationSamples_ = 0U;
    position_ = 0U;
}

bool StereoDiagnosticPanner::render(const float *mono, std::size_t frames,
                                    const SpeakerPosition &position,
                                    float gain, std::vector<float> *stereo,
                                    RenderStats *stats,
                                    GainInterpolator *gainInterpolator,
                                    std::string *reason) const
{
    RenderStats localStats;
    RenderStats &result = stats ? *stats : localStats;
    result = {};
    result.frames = frames;

    if (!stereo) {
        setReason(reason, "null-stereo-output");
        return false;
    }
    if (frames > 0U && !mono) {
        setReason(reason, "null-mono-input");
        return false;
    }
    if (!position.isFiniteAndInRange() || !finite(gain)) {
        setReason(reason, "invalid-panner-input");
        return false;
    }
    if (frames > std::numeric_limits<std::size_t>::max() / 2U) {
        setReason(reason, "stereo-output-size-overflow");
        return false;
    }
    for (std::size_t index = 0U; index < frames; ++index) {
        if (!finite(mono[index])) {
            ++result.nonFiniteInput;
        }
    }
    if (result.nonFiniteInput != 0U) {
        setReason(reason, "nonfinite-mono-input");
        return false;
    }

    std::vector<float> rampGains;
    if (gainInterpolator) {
        if (!gainInterpolator->render(frames, &rampGains, reason)) {
            return false;
        }
    }

    stereo->assign(frames * 2U, 0.0F);
    const float leftPan = equalPowerLeft(position.azimuthDegrees);
    const float rightPan = equalPowerRight(position.azimuthDegrees);
    for (std::size_t index = 0U; index < frames; ++index) {
        const float frameGain = gain * (gainInterpolator ? rampGains[index] : 1.0F);
        const float left = static_cast<float>(
            static_cast<double>(mono[index]) * static_cast<double>(frameGain)
            * static_cast<double>(leftPan));
        const float right = static_cast<float>(
            static_cast<double>(mono[index]) * static_cast<double>(frameGain)
            * static_cast<double>(rightPan));
        (*stereo)[index * 2U] = left;
        (*stereo)[index * 2U + 1U] = right;
        for (float value : {left, right}) {
            if (!finite(value)) {
                ++result.nonFiniteOutput;
                ++result.overflowCount;
            } else if (std::abs(value) > 1.0F) {
                ++result.clippedSamples;
            }
        }
    }

    if (result.nonFiniteOutput != 0U) {
        setReason(reason, "nonfinite-stereo-output");
        return false;
    }
    return true;
}

} // namespace eac3render
