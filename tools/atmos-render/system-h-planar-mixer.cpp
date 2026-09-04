#include "system-h-planar-mixer.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace eac3render {
namespace {

bool finite(float value) { return std::isfinite(static_cast<double>(value)); }
bool finite(double value) { return std::isfinite(value); }

SystemHPlanarMixerResult failure(const char *reason)
{
    SystemHPlanarMixerResult result;
    result.reason = reason;
    return result;
}

} // namespace

bool checkedSystemHEvaluatedGainCount(std::size_t samples,
                                      std::size_t *count)
{
    if (!count || samples > std::numeric_limits<std::size_t>::max()
            / kSystemHObjectCount) {
        return false;
    }
    const std::size_t objectSamples = samples * kSystemHObjectCount;
    if (objectSamples > std::numeric_limits<std::size_t>::max()
            / kSystemHSpeakerCount) {
        return false;
    }
    *count = objectSamples * kSystemHSpeakerCount;
    return true;
}

SystemHPlanarMixerResult SystemHPlanarMixer::reset(std::uint64_t generation)
{
    if (generation == 0U) return failure("mixer-invalid-generation");
    generation_ = generation;
    nextSample_ = 0;
    gainFrames_.clear();
    SystemHPlanarMixerResult result;
    result.accepted = true;
    result.reason = "mixer-reset";
    return result;
}

bool SystemHPlanarMixer::appendGainFrame(const SceneAdapterFrame &frame,
                                         std::string *reason)
{
    if (frame.generation != generation_) {
        if (reason) *reason = "mixer-stale-gain-generation";
        return false;
    }
    if (frame.timestampSamples < 0 || frame.durationSamples < 0
        || frame.objects.size() != kSystemHObjectCount) {
        if (reason) *reason = "mixer-gain-frame-shape";
        return false;
    }
    if (!gainFrames_.empty()
        && frame.timestampSamples <= gainFrames_.back().timestampSamples) {
        if (reason) *reason = "mixer-gain-timestamp-not-monotonic";
        return false;
    }
    if (std::any_of(frame.bedGains.begin(), frame.bedGains.end(),
                    [](double gain) { return !finite(gain) || gain != 0.0; })) {
        if (reason) *reason = "mixer-bed-gain-without-bed-pcm";
        return false;
    }
    if (!frame.lfeEnabled || !finite(frame.lfeGain) || frame.lfeGain != 0.0F) {
        if (reason) *reason = "mixer-lfe-sideband-policy-mismatch";
        return false;
    }
    for (std::size_t index = 0U; index < frame.objects.size(); ++index) {
        const ObjectGainFrame &object = frame.objects[index];
        if (object.objectId != index + 1U) {
            if (reason) *reason = "mixer-gain-object-order";
            return false;
        }
        for (double gain : object.speakerGains) {
            if (!finite(gain)) {
                if (reason) *reason = "mixer-nonfinite-gain";
                return false;
            }
        }
    }
    gainFrames_.push_back(frame);
    return true;
}

SystemHPlanarMixerResult SystemHPlanarMixer::process(
    const SystemHPlanarMixerInput &input)
{
    if (input.generation != generation_)
        return failure("mixer-stale-input-generation");
    if (input.sampleStart < 0 || input.sampleStart != nextSample_)
        return failure("mixer-sample-start-not-contiguous");
    if (input.objectPcm.size() != kSystemHObjectCount
        || input.lfePcm.empty())
        return failure("mixer-object-or-lfe-shape");
    const std::size_t samples = input.objectPcm.front().size();
    if (samples == 0U || input.lfePcm.size() != samples
        || samples > static_cast<std::size_t>(
               std::numeric_limits<std::int64_t>::max() - input.sampleStart))
        return failure("mixer-sample-count-invalid");
    for (const auto &object : input.objectPcm) {
        if (object.size() != samples) return failure("mixer-object-size-mismatch");
        for (float value : object)
            if (!finite(value)) return failure("mixer-nonfinite-object-pcm");
    }
    for (float value : input.lfePcm)
        if (!finite(value)) return failure("mixer-nonfinite-lfe-pcm");
    std::string reason;
    if (!appendGainFrame(input.gainFrame, &reason)) return failure(reason.c_str());
    if (gainFrames_.empty()) return failure("mixer-no-gain-frame");
    std::size_t evaluatedGainCount = 0U;
    if (!checkedSystemHEvaluatedGainCount(samples, &evaluatedGainCount))
        return failure("mixer-evaluated-gain-size-overflow");
    if (!input.evaluatedObjectSpeakerGains.empty()
        && input.evaluatedObjectSpeakerGains.size() != evaluatedGainCount) {
        return failure("mixer-evaluated-gain-shape");
    }
    for (double gain : input.evaluatedObjectSpeakerGains) {
        if (!finite(gain)) return failure("mixer-nonfinite-evaluated-gain");
    }

    SystemHPlanarMixerResult result;
    result.batch.generation = generation_;
    result.batch.sampleStart = input.sampleStart;
    result.batch.sampleEnd = input.sampleStart + static_cast<std::int64_t>(samples);
    result.batch.lfeSideband = input.lfePcm;
    for (auto &speaker : result.batch.speakers) speaker.assign(samples, 0.0F);
    for (std::size_t n = 0U; n < samples; ++n) {
        const std::int64_t timestamp = input.sampleStart
            + static_cast<std::int64_t>(n);
        for (std::size_t speaker = 0U; speaker < kSystemHSpeakerCount;
             ++speaker) {
            double sum = 0.0;
            for (std::size_t object = 0U; object < kSystemHObjectCount; ++object) {
                double gain = 0.0;
                if (input.evaluatedObjectSpeakerGains.empty()) {
                    const auto selected = std::upper_bound(
                        gainFrames_.begin(), gainFrames_.end(), timestamp,
                        [](std::int64_t value, const SceneAdapterFrame &frame) {
                            return value < frame.timestampSamples;
                        });
                    if (selected == gainFrames_.begin())
                        return failure("mixer-future-gain-before-first-frame");
                    gain = (selected - 1)->objects[object].speakerGains[speaker];
                } else {
                    const std::size_t index =
                        (n * kSystemHObjectCount + object) * kSystemHSpeakerCount
                        + speaker;
                    gain = input.evaluatedObjectSpeakerGains[index];
                }
                sum += static_cast<double>(input.objectPcm[object][n])
                    * gain;
            }
            if (!finite(sum)) return failure("mixer-nonfinite-sum");
            result.batch.speakers[speaker][n] = static_cast<float>(sum);
        }
    }
    nextSample_ = result.batch.sampleEnd;
    result.accepted = true;
    result.reason = "system-h-planar-bus-mixed";
    return result;
}

} // namespace eac3render
