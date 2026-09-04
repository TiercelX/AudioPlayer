#pragma once

#include "bs2127-system-h-configured-panner.h"
#include "scene-object-ramp-scheduler.h"

namespace eac3render {

struct SceneObjectSpeakerGain {
    std::uint64_t objectId = 0U;
    std::array<double, kSystemHSpeakerCount> gains {};
};

struct SceneObjectGainFrameResult {
    SceneAdapterDisposition disposition = SceneAdapterDisposition::InvalidInput;
    std::string reason;
    std::vector<SceneObjectSpeakerGain> objects;
    std::array<double, kSystemHSpeakerCount> summedGains {};
};

// Converts one already-evaluated snapshot into one renderer-neutral gain frame.
// No object PCM or playback state crosses this boundary.
SceneObjectGainFrameResult makeSystemHGainFrame(
    const std::vector<SceneObjectSnapshot> &snapshot,
    const Bs2127SystemHConfiguredPanner &panner);

} // namespace eac3render
