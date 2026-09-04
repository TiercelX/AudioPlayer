#pragma once

#include "scene-model.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace eac3render {

struct SpeakerPosition {
    float azimuthDegrees = 0.0F;

    static SpeakerPosition fromAzimuthDegrees(float azimuthDegrees);
    static bool fromCartesian(float x, float z, SpeakerPosition *position,
                              std::string *reason = nullptr);
    bool isFiniteAndInRange() const;
};

struct BedRoute {
    BedLabel label = BedLabel::FL;
    SpeakerPosition position;
    bool enabled = true;
};

// This is deliberately a two-speaker diagnostic layout.  It is not a
// BS.2051 production layout and must not be used as a formal layout oracle.
class SpeakerLayout {
public:
    static SpeakerLayout diagnosticStereo();

    bool routeForBed(BedLabel label, BedRoute *route,
                     std::string *reason = nullptr) const;
    std::size_t routeCount() const { return routes_.size(); }

private:
    std::array<BedRoute, 8> routes_ {};
};

class GainInterpolator {
public:
    bool setRamp(float startGain, float endGain, std::uint64_t durationSamples,
                 std::string *reason = nullptr);
    bool render(std::size_t frames, std::vector<float> *gains,
                std::string *reason = nullptr);
    void reset();

    bool initialized() const { return initialized_; }
    std::uint64_t position() const { return position_; }

private:
    float valueAt(std::uint64_t sample) const;

    bool initialized_ = false;
    float startGain_ = 0.0F;
    float endGain_ = 0.0F;
    std::uint64_t durationSamples_ = 0U;
    std::uint64_t position_ = 0U;
};

struct RenderStats {
    std::size_t frames = 0U;
    std::size_t nonFiniteInput = 0U;
    std::size_t nonFiniteOutput = 0U;
    std::size_t overflowCount = 0U;
    std::size_t clippedSamples = 0U;
};

class StereoDiagnosticPanner {
public:
    // The output is interleaved [L, R, L, R, ...].  No limiter or DRC is
    // applied; finite values above +/-1 are retained and counted.
    bool render(const float *mono, std::size_t frames,
                const SpeakerPosition &position, float gain,
                std::vector<float> *stereo,
                RenderStats *stats = nullptr,
                GainInterpolator *gainInterpolator = nullptr,
                std::string *reason = nullptr) const;
};

} // namespace eac3render
