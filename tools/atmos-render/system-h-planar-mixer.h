#pragma once

#include "scene-adapter.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace eac3render {

constexpr std::size_t kSystemHObjectCount = 15U;

struct SystemHPlanarMixerInput {
    std::uint64_t generation = kInitialGeneration;
    std::int64_t sampleStart = 0;
    std::vector<std::vector<float>> objectPcm;
    std::vector<float> lfePcm;
    SceneAdapterFrame gainFrame;
    // Optional offline-evaluated gains, flattened as
    // [sample][object][speaker].  When present, these are the exact
    // renderer-neutral gains to use for each PCM sample; gainFrame still
    // carries the source frame identity/validation contract.
    std::vector<double> evaluatedObjectSpeakerGains;
};

struct SystemHPlanarMixerBatch {
    std::uint64_t generation = kInitialGeneration;
    std::int64_t sampleStart = 0;
    std::int64_t sampleEnd = 0;
    std::array<std::vector<float>, kSystemHSpeakerCount> speakers;
    std::vector<float> lfeSideband;
    LfePolicy lfePolicy = kSystemHLfePolicy;
};

struct SystemHPlanarMixerResult {
    bool accepted = false;
    std::string reason;
    SystemHPlanarMixerBatch batch;
};

bool checkedSystemHEvaluatedGainCount(std::size_t samples,
                                      std::size_t *count);

class SystemHPlanarMixer final {
public:
    SystemHPlanarMixerResult reset(std::uint64_t generation);
    SystemHPlanarMixerResult process(const SystemHPlanarMixerInput &input);

    std::uint64_t generation() const { return generation_; }
    std::int64_t nextSample() const { return nextSample_; }

private:
    bool appendGainFrame(const SceneAdapterFrame &frame, std::string *reason);

    std::uint64_t generation_ = kInitialGeneration;
    std::int64_t nextSample_ = 0;
    std::vector<SceneAdapterFrame> gainFrames_;
};

} // namespace eac3render
