#include "native-eac3-core-decoder-seam.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace eac3native {
namespace {

constexpr unsigned kSampleRate = 48000U;
constexpr unsigned kChannels = 6U;
constexpr std::size_t kAccessUnitSamples = 1536U;
constexpr std::size_t kTailSamples = 256U;
constexpr const char *kLayout = "FL,FC,FR,SL,SR,LFE";

void identity(const NativeCoreConfig &config,
              std::vector<std::string> *ids, std::vector<bool> *lfe)
{
    ids->clear();
    lfe->clear();
    const std::string layout = config.channelLayout;
    std::size_t start = 0U;
    while (start <= layout.size()) {
        const std::size_t end = layout.find(',', start);
        ids->push_back(layout.substr(start, end == std::string::npos
                                              ? std::string::npos
                                              : end - start));
        lfe->push_back(ids->back() == "LFE");
        if (end == std::string::npos) break;
        start = end + 1U;
    }
}

eac3jocsession::Disposition mappedDisposition(NativeCoreDisposition value)
{
    switch (value) {
    case NativeCoreDisposition::Accepted:
        return eac3jocsession::Disposition::OrdinaryEac3;
    case NativeCoreDisposition::Unsupported:
        return eac3jocsession::Disposition::Unsupported;
    case NativeCoreDisposition::Malformed:
        return eac3jocsession::Disposition::Malformed;
    }
    return eac3jocsession::Disposition::Malformed;
}

} // namespace

void NativeEac3CoreDecoderSeam::setOrdinaryResult(
    eac3jocsession::CoreDecodeResult *result)
{
    if (result) result->contentKind = eac3jocsession::CoreContentKind::OrdinaryEac3;
}

eac3jocsession::CoreDecodeResult NativeEac3CoreDecoderSeam::fail(
    eac3jocsession::Disposition disposition, const char *reason,
    eac3jocsession::FlowStatus flow, bool poison)
{
    eac3jocsession::CoreDecodeResult result;
    result.disposition = disposition;
    result.reason = reason ? reason : "";
    result.flow = flow;
    if (opened_) setOrdinaryResult(&result);
    if (poison) poisoned_ = true;
    return result;
}

bool NativeEac3CoreDecoderSeam::makeFrame(
    const DecodedAccessUnit &unit, bool flush, bool eosTail,
    eac3jocsession::CoreFrame *frame, std::string *reason) const
{
    if (!frame || unit.sampleRate != kSampleRate || unit.channels.size() != kChannels
        || unit.sampleCount != (eosTail ? kTailSamples : kAccessUnitSamples)
        || unit.drcApplied) {
        if (reason) *reason = "native-seam-unit-shape-invalid";
        return false;
    }
    frame->sampleRate = unit.sampleRate;
    frame->channels = static_cast<unsigned>(unit.channels.size());
    frame->channelLayout = nativeConfig_.channelLayout;
    frame->sampleCount = unit.sampleCount;
    if (unit.sampleStart > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
        if (reason) *reason = "native-seam-sample-start-overflow";
        return false;
    }
    frame->timestamp = static_cast<std::int64_t>(unit.sampleStart);
    frame->flush = flush;
    frame->eosTail = eosTail;
    frame->contentKind = eac3jocsession::CoreContentKind::OrdinaryEac3;
    frame->drcApplied = false;
    identity(nativeConfig_, &frame->channelIds, &frame->lfeChannels);
    frame->planar.assign(kChannels, std::vector<float>(unit.sampleCount));
    for (unsigned channel = 0U; channel < kChannels; ++channel) {
        const DecodedChannel &source = unit.channels[channel];
        if (source.index != channel || source.id != frame->channelIds[channel]
            || source.lfe != frame->lfeChannels[channel]
            || source.samples.size() != unit.sampleCount) {
            if (reason) *reason = "native-seam-channel-identity-invalid";
            return false;
        }
        for (std::size_t sample = 0U; sample < unit.sampleCount; ++sample) {
            const float value = static_cast<float>(source.samples[sample]);
            if (!std::isfinite(value)) {
                if (reason) *reason = "native-seam-nonfinite-float";
                return false;
            }
            frame->planar[channel][sample] = value;
        }
    }
    return eac3jocsession::validateCoreFrame(*frame, reason);
}

bool NativeEac3CoreDecoderSeam::makeTailFrame(
    const NativeCoreFlushResult &tail, eac3jocsession::CoreFrame *frame,
    std::string *reason) const
{
    if (!tail.hadTail) {
        // An empty flush marker is allowed to carry zero planes and is not a
        // 256-sample EOS tail.
        frame->sampleRate = kSampleRate;
        frame->channels = kChannels;
        frame->channelLayout = nativeConfig_.channelLayout;
        frame->sampleCount = 0U;
        frame->timestamp = static_cast<std::int64_t>(tail.sampleStart);
        frame->flush = true;
        frame->eosTail = false;
        frame->contentKind = eac3jocsession::CoreContentKind::OrdinaryEac3;
        frame->drcApplied = false;
        identity(nativeConfig_, &frame->channelIds, &frame->lfeChannels);
        frame->planar.assign(kChannels, {});
        return eac3jocsession::validateCoreFrame(*frame, reason);
    }
    if (!tail.ok || tail.tailSamplesPerChannel != kTailSamples
        || tail.channels.size() != kChannels) {
        if (reason) *reason = "native-seam-tail-shape-invalid";
        return false;
    }
    DecodedAccessUnit unit;
    unit.sampleStart = tail.sampleStart;
    unit.sampleCount = tail.tailSamplesPerChannel;
    unit.sampleRate = kSampleRate;
    unit.drcApplied = false;
    unit.channels = tail.channels;
    return makeFrame(unit, true, true, frame, reason);
}

eac3jocsession::CoreDecodeResult NativeEac3CoreDecoderSeam::open(
    const eac3jocsession::CoreDecoderConfig &config)
{
    reset();
    if (config.sampleRate != kSampleRate || config.channels != kChannels
        || config.channelLayout != kLayout) {
        return fail(eac3jocsession::Disposition::Unsupported,
                    "native-seam-config3-topology-required", {}, false);
    }
    nativeConfig_.sampleRate = config.sampleRate;
    nativeConfig_.channels = config.channels;
    nativeConfig_.channelLayout = config.channelLayout;
    const NativeCoreResult opened = decoder_.open(nativeConfig_);
    if (!opened.accepted) {
        return fail(eac3jocsession::Disposition::Unsupported,
                    "native-seam-native-open-failed", {}, false);
    }
    config_ = config;
    opened_ = true;
    eac3jocsession::CoreDecodeResult result;
    result.disposition = eac3jocsession::Disposition::OrdinaryEac3;
    result.reason = "native-seam-open-ordinary-eac3";
    result.accepted = true;
    setOrdinaryResult(&result);
    return result;
}

eac3jocsession::CoreDecodeResult NativeEac3CoreDecoderSeam::pushPacket(
    const std::vector<std::uint8_t> &packet, std::int64_t timestamp,
    const eac3jocsession::CoreFrameCallback &callback)
{
    if (canceled_) return fail(eac3jocsession::Disposition::OrdinaryEac3,
                               "native-seam-canceled", eac3jocsession::FlowStatus::Canceled,
                               false);
    if (blocked_ || poisoned_) {
        return fail(eac3jocsession::Disposition::Malformed,
                    "native-seam-poisoned-reset-required",
                    eac3jocsession::FlowStatus::CallbackRejected, false);
    }
    if (!opened_) return fail(eac3jocsession::Disposition::Malformed,
                              "native-seam-not-open", {}, false);
    if (!callback) return fail(eac3jocsession::Disposition::Malformed,
                               "native-seam-callback-missing");
    if (timestamp < 0) return fail(eac3jocsession::Disposition::Malformed,
                                   "native-seam-negative-timestamp");
    if (timestamp > std::numeric_limits<std::int64_t>::max()
                      - static_cast<std::int64_t>(kAccessUnitSamples)) {
        return fail(eac3jocsession::Disposition::Malformed,
                    "native-seam-timestamp-overflow");
    }
    const NativeCoreResult decoded = decoder_.processAccessUnit(
        packet, decoder_.framesProcessed() * kAccessUnitSamples);
    if (decoded.disposition != NativeCoreDisposition::Accepted) {
        const eac3jocsession::Disposition disposition = mappedDisposition(decoded.disposition);
        return fail(disposition, decoded.reason.c_str(), {}, true);
    }
    eac3jocsession::CoreFrame frame;
    std::string reason;
    if (!makeFrame(decoded.unit, false, false, &frame, &reason)) {
        return fail(eac3jocsession::Disposition::Malformed, reason.c_str());
    }
    frame.timestamp = timestamp;
    if (!eac3jocsession::validateCoreFrame(frame, &reason)) {
        return fail(eac3jocsession::Disposition::Malformed, reason.c_str());
    }
    if (!callback(frame)) {
        blocked_ = true;
        poisoned_ = true;
        return fail(eac3jocsession::Disposition::OrdinaryEac3,
                    "native-seam-callback-rejected", eac3jocsession::FlowStatus::CallbackRejected,
                    false);
    }
    nextTimestamp_ = timestamp + static_cast<std::int64_t>(kAccessUnitSamples);
    eac3jocsession::CoreDecodeResult result;
    result.disposition = eac3jocsession::Disposition::OrdinaryEac3;
    result.reason = "native-seam-access-unit-accepted";
    result.accepted = true;
    setOrdinaryResult(&result);
    return result;
}

eac3jocsession::CoreDecodeResult NativeEac3CoreDecoderSeam::flush(
    const eac3jocsession::CoreFrameCallback &callback)
{
    if (canceled_) return fail(eac3jocsession::Disposition::OrdinaryEac3,
                               "native-seam-canceled", eac3jocsession::FlowStatus::Canceled,
                               false);
    if (blocked_ || poisoned_) return fail(eac3jocsession::Disposition::Malformed,
                                           "native-seam-poisoned-reset-required",
                                           eac3jocsession::FlowStatus::CallbackRejected, false);
    if (!opened_) return fail(eac3jocsession::Disposition::Malformed,
                              "native-seam-not-open", {}, false);
    if (!callback) return fail(eac3jocsession::Disposition::Malformed,
                               "native-seam-callback-missing");
    const NativeCoreFlushResult tail = decoder_.flush();
    if (tail.flow == NativeCoreFlow::AlreadyFlushed) {
        eac3jocsession::CoreDecodeResult result;
        result.disposition = eac3jocsession::Disposition::OrdinaryEac3;
        result.reason = tail.reason;
        result.flow = eac3jocsession::FlowStatus::AlreadyFlushed;
        setOrdinaryResult(&result);
        return result;
    }
    if (!tail.ok) {
        return fail(eac3jocsession::Disposition::Malformed, tail.reason.c_str(), {}, true);
    }
    eac3jocsession::CoreFrame frame;
    std::string reason;
    if (!makeTailFrame(tail, &frame, &reason)) {
        return fail(eac3jocsession::Disposition::Malformed, reason.c_str());
    }
    if (nextTimestamp_ < 0) {
        return fail(eac3jocsession::Disposition::Malformed,
                    "native-seam-eos-timestamp-invalid");
    }
    frame.timestamp = nextTimestamp_;
    if (!eac3jocsession::validateCoreFrame(frame, &reason)) {
        return fail(eac3jocsession::Disposition::Malformed, reason.c_str());
    }
    if (!callback(frame)) {
        blocked_ = true;
        poisoned_ = true;
        return fail(eac3jocsession::Disposition::OrdinaryEac3,
                    "native-seam-callback-rejected-flush",
                    eac3jocsession::FlowStatus::CallbackRejected, false);
    }
    eac3jocsession::CoreDecodeResult result;
    result.disposition = eac3jocsession::Disposition::OrdinaryEac3;
    result.reason = "native-seam-eos";
    result.accepted = true;
    result.terminal = true;
    setOrdinaryResult(&result);
    return result;
}

void NativeEac3CoreDecoderSeam::reset()
{
    decoder_.reset();
    nativeConfig_ = NativeCoreConfig{};
    config_ = eac3jocsession::CoreDecoderConfig{};
    opened_ = false;
    poisoned_ = false;
    canceled_ = false;
    blocked_ = false;
    nextTimestamp_ = 0;
}

void NativeEac3CoreDecoderSeam::cancel()
{
    canceled_ = true;
    decoder_.cancel();
}

} // namespace eac3native
