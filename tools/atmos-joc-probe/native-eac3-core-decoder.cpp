#include "native-eac3-core-decoder.h"

#include "native-eac3-bsi.h"
#include "native-eac3-core.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace eac3native {
namespace {

constexpr std::size_t kSamplesPerAccessUnit = 1536U;
constexpr std::array<const char*, 6> kChannelIds = {
    "FL", "FC", "FR", "SL", "SR", "LFE"};

bool validConfig(const NativeCoreConfig& config)
{
    return config.sampleRate == 48000U && config.channels == 6U
        && config.channelLayout == "FL,FC,FR,SL,SR,LFE";
}

bool finiteSamples(const std::vector<double>& samples)
{
    return std::all_of(samples.begin(), samples.end(),
                       [](double value) { return std::isfinite(value); });
}

} // namespace

NativeCoreResult NativeEac3CoreDecoder::failure(
    NativeCoreDisposition disposition, NativeCoreFlow flow,
    std::string reason, bool poison)
{
    if (poison) {
        poisoned_ = true;
    }
    NativeCoreResult result;
    result.disposition = disposition;
    result.flow = flow;
    result.reason = std::move(reason);
    result.poisoned = poisoned_;
    return result;
}

NativeCoreResult NativeEac3CoreDecoder::open(const NativeCoreConfig& config)
{
    reset();
    if (!validConfig(config)) {
        return failure(NativeCoreDisposition::Unsupported, NativeCoreFlow::None,
                       "native-core-config3-topology-required", false);
    }
    config_ = config;
    opened_ = true;
    NativeCoreResult result;
    result.disposition = NativeCoreDisposition::Accepted;
    result.accepted = true;
    result.reason = "native-core-open-config3";
    return result;
}

NativeCoreResult NativeEac3CoreDecoder::processAccessUnit(
    const std::vector<std::uint8_t>& packet, std::size_t sampleStart)
{
    if (canceled_)
        return failure(NativeCoreDisposition::Malformed,
                       NativeCoreFlow::Canceled, "native-core-canceled", false);
    if (!opened_)
        return failure(NativeCoreDisposition::Malformed, NativeCoreFlow::None,
                       "native-core-not-open", false);
    if (poisoned_)
        return failure(NativeCoreDisposition::Malformed, NativeCoreFlow::None,
                       "native-core-poisoned-reset-required", false);
    if (flushed_)
        return failure(NativeCoreDisposition::Malformed, NativeCoreFlow::AlreadyFlushed,
                       "native-core-frame-after-eos-reset-required", false);
    if (packet.empty())
        return failure(NativeCoreDisposition::Malformed, NativeCoreFlow::None,
                       "native-core-empty-access-unit");

    const ParseResult parsed = parseSyncframe(packet, 0U);
    if (parsed.disposition != Disposition::Accepted || !parsed.frame) {
        const NativeCoreDisposition disposition =
            parsed.disposition == Disposition::Unsupported
                ? NativeCoreDisposition::Unsupported
                : NativeCoreDisposition::Malformed;
        return failure(disposition, NativeCoreFlow::None,
                       "native-core-frame-" + parsed.reason);
    }
    const FrameHeader& frame = *parsed.frame;
    if (frame.streamType != StreamType::Independent)
        return failure(NativeCoreDisposition::Unsupported, NativeCoreFlow::None,
                       "native-core-requires-eac3-type0-independent");
    if (frame.substreamId != 0U)
        return failure(NativeCoreDisposition::Unsupported, NativeCoreFlow::None,
                       "native-core-requires-independent-sid0");
    if (frame.sampleRate != config_.sampleRate)
        return failure(NativeCoreDisposition::Unsupported, NativeCoreFlow::None,
                       "native-core-sample-rate-change");
    if (frame.blocks != 6U || frame.sampleCount != kSamplesPerAccessUnit)
        return failure(NativeCoreDisposition::Unsupported, NativeCoreFlow::None,
                       "native-core-requires-six-block-access-unit");
    if (frame.channelCount != config_.channels || frame.acmod != 7U
        || !frame.lfe || frame.sizeBytes != packet.size())
        return failure(NativeCoreDisposition::Unsupported, NativeCoreFlow::None,
                       "native-core-config3-channel-topology-required");
    if (sampleStart != framesProcessed_ * kSamplesPerAccessUnit)
        return failure(NativeCoreDisposition::Malformed, NativeCoreFlow::None,
                       "native-core-noncontiguous-sample-start");

    const BsiParseResult bsi = parseEac3Bsi(packet, frame);
    if (bsi.disposition != Disposition::Accepted || !bsi.info)
        return failure(bsi.disposition == Disposition::Unsupported
                           ? NativeCoreDisposition::Unsupported
                           : NativeCoreDisposition::Malformed,
                       NativeCoreFlow::None,
                       "native-core-bsi-" + bsi.reason);

    const PcmFrameResult pcm = session_.processFrame(packet, frame, *bsi.info);
    if (pcm.disposition != PcmSessionDisposition::Accepted)
        return failure(pcm.disposition == PcmSessionDisposition::Unsupported
                           ? NativeCoreDisposition::Unsupported
                           : NativeCoreDisposition::Malformed,
                       NativeCoreFlow::None,
                       "native-core-pcm-" + pcm.reason);
    if (pcm.channelsData.size() != config_.channels)
        return failure(NativeCoreDisposition::Malformed, NativeCoreFlow::None,
                       "native-core-channel-count-result");

    NativeCoreResult result;
    result.disposition = NativeCoreDisposition::Accepted;
    result.accepted = true;
    result.reason = "native-core-access-unit-accepted";
    result.unit.sampleStart = sampleStart;
    result.unit.sampleCount = kSamplesPerAccessUnit;
    result.unit.sampleRate = frame.sampleRate;
    result.unit.drcApplied = false;
    result.unit.channels.resize(config_.channels);
    for (unsigned channel = 0U; channel < config_.channels; ++channel) {
        const PcmChannelFrame& source = pcm.channelsData[channel];
        if (source.channel != channel || source.samples.size() != kSamplesPerAccessUnit
            || !finiteSamples(source.samples)) {
            return failure(NativeCoreDisposition::Malformed,
                           NativeCoreFlow::None,
                           "native-core-nonfinite-or-invalid-channel");
        }
        DecodedChannel& target = result.unit.channels[channel];
        target.index = channel;
        target.id = kChannelIds[channel];
        target.lfe = channel == 5U;
        target.samples = source.samples;
    }
    ++framesProcessed_;
    result.poisoned = false;
    return result;
}

NativeCoreFlushResult NativeEac3CoreDecoder::flush()
{
    NativeCoreFlushResult result;
    if (canceled_) {
        result.flow = NativeCoreFlow::Canceled;
        result.reason = "native-core-canceled";
        return result;
    }
    if (!opened_) {
        result.reason = "native-core-not-open";
        return result;
    }
    if (poisoned_) {
        result.reason = "native-core-poisoned-reset-required";
        return result;
    }
    if (flushed_) {
        result.ok = true;
        result.flow = NativeCoreFlow::AlreadyFlushed;
        result.reason = "native-core-eos-already-flushed";
        return result;
    }
    const PcmFlushResult pcm = session_.flush();
    if (!pcm.ok) {
        poisoned_ = true;
        result.reason = "native-core-pcm-" + pcm.reason;
        return result;
    }
    flushed_ = true;
    result.ok = true;
    result.reason = "native-core-eos";
    result.sampleStart = framesProcessed_ * kSamplesPerAccessUnit;
    result.hadTail = pcm.hadTail;
    result.tailSamplesPerChannel = pcm.tailSamplesPerChannel;
    if (!pcm.hadTail) {
        return result;
    }
    if (pcm.channelsData.size() != config_.channels
        || pcm.tailSamplesPerChannel != kEac3OutputSamples) {
        poisoned_ = true;
        result.ok = false;
        result.reason = "native-core-invalid-eos-tail";
        result.hadTail = false;
        result.tailSamplesPerChannel = 0U;
        return result;
    }
    result.channels.resize(config_.channels);
    for (unsigned channel = 0U; channel < config_.channels; ++channel) {
        const PcmChannelFrame& source = pcm.channelsData[channel];
        if (source.samples.size() != kEac3OutputSamples
            || !finiteSamples(source.samples)) {
            poisoned_ = true;
            result.ok = false;
            result.reason = "native-core-invalid-eos-channel";
            result.hadTail = false;
            result.tailSamplesPerChannel = 0U;
            result.channels.clear();
            return result;
        }
        result.channels[channel].index = channel;
        result.channels[channel].id = kChannelIds[channel];
        result.channels[channel].lfe = channel == 5U;
        result.channels[channel].samples = source.samples;
    }
    return result;
}

void NativeEac3CoreDecoder::reset()
{
    session_.reset();
    opened_ = false;
    poisoned_ = false;
    canceled_ = false;
    flushed_ = false;
    framesProcessed_ = 0U;
}

void NativeEac3CoreDecoder::cancel()
{
    if (!canceled_) {
        canceled_ = true;
        session_.reset();
    }
}

const char* toString(NativeCoreDisposition value)
{
    switch (value) {
    case NativeCoreDisposition::Accepted: return "accepted";
    case NativeCoreDisposition::Unsupported: return "unsupported";
    case NativeCoreDisposition::Malformed: return "malformed";
    }
    return "unknown";
}

const char* toString(NativeCoreFlow value)
{
    switch (value) {
    case NativeCoreFlow::None: return "none";
    case NativeCoreFlow::Canceled: return "canceled";
    case NativeCoreFlow::AlreadyFlushed: return "already-flushed";
    }
    return "unknown";
}

} // namespace eac3native
