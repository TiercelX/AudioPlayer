#include "native-eac3-pcm-session.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace eac3native {
namespace {

bool finiteVector(const std::vector<double>& values)
{
    return std::all_of(values.begin(), values.end(), [](double value) {
        return std::isfinite(value);
    });
}

bool anyRematrix(const AudblkFrameState& state)
{
    for (const AudblkBlockState& block : state.blockStates) {
        if (!block.rematrixPresent && !block.rematrixUpdated)
            continue;
        if (block.rematrixUpdated
            || std::any_of(block.rematrixFlags.begin(),
                           block.rematrixFlags.end(),
                           [](bool value) { return value; }))
            return true;
    }
    return false;
}

} // namespace

PcmFrameResult Eac3PcmSession::failure(
    PcmSessionDisposition disposition, const FrameHeader& frame,
    std::string reason, unsigned block, unsigned channel,
    bool channelIsLfe)
{
    poisoned_ = true;
    PcmFrameResult result;
    result.disposition = disposition;
    result.reason = std::move(reason);
    result.frameOffset = frame.offset;
    result.bitPosition = frame.offset * 8U;
    result.blockIndex = block;
    result.channelIndex = channel;
    result.channelIsLfe = channelIsLfe;
    result.blocks = frame.blocks;
    result.channels = frame.channelCount;
    result.sampleRate = frame.sampleRate;
    result.drcApplied = false;
    return result;
}

PcmFrameResult Eac3PcmSession::processFrame(
    const std::vector<std::uint8_t>& bytes, const FrameHeader& frame,
    const BsiInfo& bsi)
{
    if (poisoned_)
        return failure(PcmSessionDisposition::Malformed, frame,
                       "session-poisoned-reset-required");
    if (flushed_)
        return failure(PcmSessionDisposition::Malformed, frame,
                       "frame-after-eos-reset-required");
    if (frame.streamType != StreamType::Independent
        && frame.streamType != StreamType::Dependent)
        return failure(PcmSessionDisposition::Unsupported, frame,
                       "pcm-session-requires-eac3-independent-or-dependent");
    if (frame.blocks == 0U || frame.channelCount == 0U)
        return failure(PcmSessionDisposition::Malformed, frame,
                       "pcm-session-invalid-frame-topology");
    if (!initialized_) {
        initialized_ = true;
        lfe_ = frame.lfe;
        channelCount_ = frame.channelCount;
        sampleRate_ = frame.sampleRate;
        ditherSources_.assign(channelCount_, ReferenceDitherSource{});
        transforms_.resize(channelCount_);
    } else if (frame.channelCount != channelCount_ || frame.lfe != lfe_
               || frame.sampleRate != sampleRate_) {
        return failure(PcmSessionDisposition::Malformed, frame,
                       "pcm-session-topology-change");
    }

    ChannelDitherSource channelDither =
        [this](unsigned channel, std::size_t coefficientIndex,
               unsigned exponent) {
            if (channel >= ditherSources_.size())
                return 0.0;
            if (ditherMode_ == PcmDitherMode::ZeroDiagnosticOnly)
                return 0.0;
            return ditherSources_[channel].next(coefficientIndex, exponent);
        };
    std::vector<std::uint64_t> ditherStateStarts(channelCount_);
    std::vector<std::size_t> ditherSampleStarts(channelCount_);
    for (unsigned channel = 0U; channel < channelCount_; ++channel) {
        ditherStateStarts[channel] = ditherSources_[channel].state();
        ditherSampleStarts[channel] = ditherSources_[channel].generatedSamples();
    }
    const AudblkParseResult parsed = parseEac3AudblkCoefficients(
        bytes, frame, bsi, channelDither);
    if (parsed.disposition != AudblkDisposition::Accepted
        || !parsed.features || !parsed.features->state) {
        const PcmSessionDisposition disposition =
            parsed.disposition == AudblkDisposition::Unsupported
            ? PcmSessionDisposition::Unsupported
            : PcmSessionDisposition::Malformed;
        PcmFrameResult result = failure(disposition, frame, parsed.reason,
                                        parsed.blockIndex, parsed.channelIndex,
                                        parsed.channelIsLfe);
        result.bitPosition = parsed.bitPosition;
        return result;
    }

    const AudblkFrameState& state = *parsed.features->state;
    if (anyRematrix(state)) {
        PcmFrameResult result = failure(
            PcmSessionDisposition::Unsupported, frame,
            "rematrix-transform-not-connected");
        result.bitPosition = parsed.bitPosition;
        return result;
    }

    PcmFrameResult result;
    result.disposition = PcmSessionDisposition::Accepted;
    result.reason = "ordinary-uncoupled-pcm-session-frame";
    result.frameOffset = frame.offset;
    result.bitPosition = parsed.bitPosition;
    result.blocks = frame.blocks;
    result.channels = frame.channelCount;
    result.sampleRate = frame.sampleRate;
    result.outputSamplesPerChannel =
        static_cast<std::size_t>(frame.blocks) * kEac3OutputSamples;
    result.drcApplied = false;
    result.channelsData.resize(channelCount_);
    for (unsigned channel = 0U; channel < channelCount_; ++channel) {
        result.channelsData[channel].channel = channel;
        result.channelsData[channel].lfe = lfe_ && channel + 1U == channelCount_;
        result.channelsData[channel].ditherStateStart = ditherStateStarts[channel];
        result.channelsData[channel].ditherSamplesStart =
            ditherSampleStarts[channel];
        result.channelsData[channel].ditherSamples =
            ditherSampleStarts[channel];
    }

    for (unsigned block = 0U; block < state.blockStates.size(); ++block) {
        const AudblkBlockState& blockState = state.blockStates[block];
        if (blockState.channels.size() != channelCount_)
            return failure(PcmSessionDisposition::Malformed, frame,
                           "pcm-session-channel-count", block);
        for (unsigned channel = 0U; channel < channelCount_; ++channel) {
            const AudblkChannelState& channelState =
                blockState.channels[channel];
            if (!channelState.coefficientsDecoded
                || channelState.coefficients.size() != channelState.endMant
                || !finiteVector(channelState.coefficients))
                return failure(PcmSessionDisposition::Malformed, frame,
                               "pcm-session-coefficient-vector", block,
                               channel, channelState.lfe);
            result.channelsData[channel].coefficientSpans.emplace_back(
                channelState.coefficientStartBit,
                channelState.coefficientEndBit);
            const bool blockSwitch =
                !channelState.lfe && channel < blockState.blockSwitchFlags.size()
                ? blockState.blockSwitchFlags[channel]
                : false;
            const TransformOutput transformed = transforms_[channel].processBlock(
                channelState.coefficients, blockSwitch);
            if (!transformed.ok || transformed.samples.size() != kEac3OutputSamples
                || !finiteVector(transformed.samples))
                return failure(PcmSessionDisposition::Malformed, frame,
                               transformed.error.empty()
                               ? "pcm-session-transform-failure"
                               : transformed.error,
                               block, channel, channelState.lfe);
            auto& samples = result.channelsData[channel].samples;
            samples.insert(samples.end(), transformed.samples.begin(),
                           transformed.samples.end());
        }
    }
    for (unsigned channel = 0U; channel < channelCount_; ++channel) {
        result.channelsData[channel].ditherStateEnd =
            ditherSources_[channel].state();
        result.channelsData[channel].ditherSamples =
            ditherSources_[channel].generatedSamples()
            - ditherSampleStarts[channel];
        if (result.channelsData[channel].samples.size()
            != result.outputSamplesPerChannel)
            return failure(PcmSessionDisposition::Malformed, frame,
                           "pcm-session-sample-count", 0U, channel,
                           result.channelsData[channel].lfe);
    }
    ++framesProcessed_;
    return result;
}

PcmFlushResult Eac3PcmSession::flush()
{
    PcmFlushResult result;
    if (poisoned_)
        return PcmFlushResult{false, false,
                              "session-poisoned-reset-required", 0U, {}};
    result.ok = true;
    if (flushed_)
        return result;
    flushed_ = true;
    if (!initialized_)
        return result;

    result.channelsData.resize(channelCount_);
    for (unsigned channel = 0U; channel < channelCount_; ++channel) {
        const TransformFlushOutput tail = transforms_[channel].flush();
        if (!tail.ok)
            return PcmFlushResult{false, false, tail.error, 0U, {}};
        result.channelsData[channel].channel = channel;
        result.channelsData[channel].lfe = lfe_ && channel + 1U == channelCount_;
        result.channelsData[channel].samples = tail.samples;
        if (tail.hadTail) {
            if (tail.samples.size() != kEac3OutputSamples)
                return PcmFlushResult{false, false,
                                      "pcm-session-invalid-eos-tail", 0U, {}};
            result.hadTail = true;
        } else if (!tail.samples.empty()) {
            return PcmFlushResult{false, false,
                                  "pcm-session-unexpected-eos-samples", 0U,
                                  {}};
        }
    }
    result.tailSamplesPerChannel =
        result.hadTail ? kEac3OutputSamples : 0U;
    return result;
}

void Eac3PcmSession::reset()
{
    for (ReferenceDitherSource& source : ditherSources_)
        source.reset();
    for (Eac3TransformChannel& transform : transforms_)
        transform.reset();
    initialized_ = false;
    lfe_ = false;
    flushed_ = false;
    poisoned_ = false;
    channelCount_ = 0U;
    sampleRate_ = 0U;
    framesProcessed_ = 0U;
    ditherSources_.clear();
    transforms_.clear();
}

bool Eac3PcmSession::hasPendingOverlap() const
{
    return std::any_of(transforms_.begin(), transforms_.end(),
                       [](const Eac3TransformChannel& transform) {
                           return transform.hasPendingOverlap();
                       });
}

const char* toString(PcmSessionDisposition value)
{
    switch (value) {
    case PcmSessionDisposition::Accepted: return "accepted";
    case PcmSessionDisposition::Unsupported: return "unsupported";
    case PcmSessionDisposition::Malformed: return "malformed";
    }
    return "unknown";
}

} // namespace eac3native
