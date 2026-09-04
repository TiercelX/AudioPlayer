#include "annex-h-headphone.h"

#include <algorithm>
#include <utility>

namespace eac3annexh {
namespace {

struct Reader {
    const std::uint8_t *data = nullptr;
    std::size_t bit = 0U;
    std::size_t limit = 0U;

    bool read(unsigned count, unsigned *value)
    {
        if (!value || count > 32U || bit > limit
            || static_cast<std::size_t>(count) > limit - bit) return false;
        unsigned result = 0U;
        for (unsigned index = 0U; index < count; ++index) {
            result = (result << 1U)
                | ((data[bit / 8U] >> (7U - bit % 8U)) & 1U);
            ++bit;
        }
        *value = result;
        return true;
    }

    bool skip(std::size_t count)
    {
        if (bit > limit || count > limit - bit) return false;
        bit += count;
        return true;
    }

    bool readBytes(std::size_t count, std::vector<std::uint8_t> *bytes)
    {
        if (!bytes || count > remaining() / 8U
            || (count * 8U) > remaining()) return false;
        bytes->clear();
        bytes->reserve(count);
        for (std::size_t index = 0U; index < count; ++index) {
            unsigned value = 0U;
            if (!read(8U, &value)) return false;
            bytes->push_back(static_cast<std::uint8_t>(value));
        }
        return true;
    }

    std::size_t remaining() const { return bit <= limit ? limit - bit : 0U; }
};

ParseResult fail(Disposition disposition, const char *reason)
{
    ParseResult result;
    result.disposition = disposition;
    result.reason = reason;
    return result;
}

bool finish(Reader *reader)
{
    unsigned value = 0U;
    while (reader->remaining() != 0U) {
        if (!reader->read(1U, &value) || value != 0U) return false;
    }
    return true;
}

std::uint32_t raw24(const std::vector<std::uint8_t> &bytes, std::size_t offset)
{
    return (static_cast<std::uint32_t>(bytes[offset]) << 16U)
        | (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U)
        | static_cast<std::uint32_t>(bytes[offset + 2U]);
}

void appendRaw24(const std::vector<std::uint8_t> &bytes,
                 std::vector<std::uint32_t> *values)
{
    if (!values) return;
    for (std::size_t offset = 0U; offset + 2U < bytes.size(); offset += 3U)
        values->push_back(raw24(bytes, offset));
}

std::uint8_t xorBytes(const std::vector<std::uint8_t> &bytes)
{
    std::uint8_t value = 0U;
    for (const std::uint8_t byte : bytes) value ^= byte;
    return value;
}

ReassemblyResult failure(ReassemblyDisposition disposition, const char *reason,
                         std::size_t payloads = 0U,
                         std::size_t brirPayloads = 0U)
{
    ReassemblyResult result;
    result.disposition = disposition;
    result.reason = reason;
    result.payloads = payloads;
    result.brirPayloads = brirPayloads;
    return result;
}

constexpr std::size_t kEarlyCoefficientsPerChannel = 2U * 64U;
constexpr std::size_t kLateCoefficients = 2U * 1024U;
constexpr std::size_t kEarlyChunkBits = 24U * 2U * 64U / 32U;
constexpr std::size_t kLateChunkBits = 24U * 2U * 1024U / 32U;
static_assert(kEarlyChunkBits == 96U, "H.3.7 early chunk size");
static_assert(kLateChunkBits == 1536U, "H.3.7 late chunk size");
constexpr std::size_t kEarlyChunks =
    (kEarlyCoefficientsPerChannel * 24U) / kEarlyChunkBits;
constexpr std::size_t kLateChunks =
    (kLateCoefficients * 24U) / kLateChunkBits;
static_assert(kEarlyChunks == 32U, "H.3.7 early sequence chunks");
static_assert(kLateChunks == 32U, "H.3.7 late sequence chunks");

} // namespace

ParseResult parse(const std::vector<std::uint8_t> &bytes)
{
    if (bytes.empty()) return fail(Disposition::Malformed, "empty-annex-h-payload");
    Reader reader {bytes.data(), 0U, bytes.size() * 8U};
    HeadphonePayload payload;
    if (!reader.read(3U, &payload.channelCount))
        return fail(Disposition::Malformed, "truncated-annex-h-channel-count");
    if (payload.channelCount == 0U || payload.channelCount > 7U)
        return fail(Disposition::Unsupported, "reserved-annex-h-channel-count");
    payload.channelGain.resize(payload.channelCount);
    for (unsigned &gain : payload.channelGain) {
        if (!reader.read(6U, &gain))
            return fail(Disposition::Malformed, "truncated-annex-h-channel-gain");
    }
    unsigned flag = 0U;
    if (!reader.read(1U, &flag))
        return fail(Disposition::Malformed, "truncated-annex-h-lfe-gain-flag");
    payload.lfeGainPresent = flag != 0U;
    if (payload.lfeGainPresent && !reader.read(6U, &payload.lfeGain))
        return fail(Disposition::Malformed, "truncated-annex-h-lfe-gain");
    if (!reader.read(1U, &flag))
        return fail(Disposition::Malformed, "truncated-annex-h-sequence-start");
    payload.sequenceStart = flag != 0U;
    if (!reader.read(1U, &flag))
        return fail(Disposition::Malformed, "truncated-annex-h-sequence-end");
    payload.sequenceEnd = flag != 0U;
    if (!reader.read(1U, &flag))
        return fail(Disposition::Malformed, "truncated-annex-h-brir-flag");
    payload.brirDataPresent = flag != 0U;
    if (payload.brirDataPresent) {
        payload.earlyBrirChunks.resize(payload.channelCount);
        for (auto &chunk : payload.earlyBrirChunks) {
            if (!reader.readBytes(kEarlyChunkBits / 8U, &chunk))
                return fail(Disposition::Malformed, "truncated-annex-h-brir-chunk");
        }
        if (!reader.readBytes(kLateChunkBits / 8U, &payload.lateBrirChunk))
            return fail(Disposition::Malformed, "truncated-annex-h-late-brir-chunk");
    }
    if (payload.sequenceEnd) {
        unsigned parity = 0U;
        if (!reader.read(8U, &parity))
            return fail(Disposition::Malformed, "truncated-annex-h-parity");
        payload.parityPresent = true;
        payload.parityCheck = static_cast<std::uint8_t>(parity);
    }
    if (payload.sequenceStart) {
        if (!reader.read(1U, &flag))
            return fail(Disposition::Malformed, "truncated-annex-h-propagation-flag");
        payload.propagationDelayPresent = flag != 0U;
        if (payload.propagationDelayPresent) {
            payload.propagationDelay.resize(payload.channelCount);
            for (unsigned &delay : payload.propagationDelay) {
                if (!reader.read(11U, &delay))
                    return fail(Disposition::Malformed,
                                "truncated-annex-h-propagation-delay");
            }
        }
        if (!reader.read(1U, &flag))
            return fail(Disposition::Malformed, "truncated-annex-h-rt60-flag");
        payload.rt60Present = flag != 0U;
        if (payload.rt60Present) {
            if (!reader.read(4U, &payload.rt60Bands))
                return fail(Disposition::Malformed, "truncated-annex-h-rt60-band-count");
            if (payload.rt60Bands == 0U || payload.rt60Bands > 11U)
                return fail(Disposition::Unsupported, "reserved-annex-h-rt60-band-count");
            payload.rt60.resize(payload.rt60Bands);
            for (unsigned &value : payload.rt60) {
                if (!reader.read(11U, &value))
                    return fail(Disposition::Malformed, "truncated-annex-h-rt60");
                if (value == 0U)
                    return fail(Disposition::Unsupported, "reserved-annex-h-rt60-value");
            }
        }
    }
    if (!finish(&reader)) return fail(Disposition::Malformed, "nonzero-annex-h-padding");
    ParseResult result;
    result.disposition = Disposition::Pass;
    result.reason = "bounded-annex-h-headphone-header-and-presence";
    result.payload = std::move(payload);
    return result;
}

SequenceResult validateSequence(const std::vector<HeadphonePayload> &payloads)
{
    SequenceResult result;
    result.payloads = payloads.size();
    bool active = false;
    for (const HeadphonePayload &payload : payloads) {
        if (payload.brirDataPresent) {
            ++result.brirPayloads;
            if (payload.sequenceStart) {
                if (active)
                    return {Disposition::Malformed, "annex-h-sequence-start-while-active",
                            result.payloads, result.brirPayloads};
                active = true;
            } else if (!active) {
                return {Disposition::Malformed, "annex-h-sequence-continuation-without-start",
                        result.payloads, result.brirPayloads};
            }
            if (payload.sequenceEnd) active = false;
        } else if (payload.sequenceStart || payload.sequenceEnd) {
            return {Disposition::Malformed, "annex-h-sequence-flag-without-brir",
                    result.payloads, result.brirPayloads};
        }
    }
    if (active)
        return {Disposition::Malformed, "annex-h-sequence-missing-end",
                result.payloads, result.brirPayloads};
    result.disposition = Disposition::Pass;
    result.reason = "annex-h-sequence-boundary-valid";
    return result;
}

void HeadphoneRenderingCache::clearWorking()
{
    active_ = false;
    working_ = {};
    payloads_ = 0U;
    brirPayloads_ = 0U;
    earlyChunkCount_ = 0U;
    lateChunkCount_ = 0U;
    parityXor_ = 0U;
}

void HeadphoneRenderingCache::reset()
{
    clearWorking();
    committed_ = {};
}

ReassemblyResult HeadphoneRenderingCache::cancel()
{
    const std::size_t payloads = payloads_;
    const std::size_t brirPayloads = brirPayloads_;
    reset();
    return failure(ReassemblyDisposition::Cancelled, "annex-h-sequence-cancelled",
                   payloads, brirPayloads);
}

ReassemblyResult HeadphoneRenderingCache::append(const HeadphonePayload &payload)
{
    auto failAndClear = [this](const char *reason) {
        const std::size_t payloads = payloads_;
        const std::size_t brirPayloads = brirPayloads_;
        reset();
        return failure(ReassemblyDisposition::Malformed, reason, payloads,
                       brirPayloads);
    };

    if (!active_) {
        if (!payload.sequenceStart) {
            reset();
            return failure(ReassemblyDisposition::Malformed,
                           "annex-h-sequence-end-without-start");
        }
        if (payload.channelCount == 0U || payload.channelCount > 7U) {
            reset();
            return failure(ReassemblyDisposition::Unsupported,
                           "reserved-annex-h-channel-count");
        }
        if (payload.channelGain.size() != payload.channelCount) {
            reset();
            return failure(ReassemblyDisposition::Malformed,
                           "annex-h-channel-gain-layout-mismatch");
        }
        working_.channelCount = payload.channelCount;
        working_.channelGain = payload.channelGain;
        working_.lfeGainPresent = payload.lfeGainPresent;
        working_.lfeGain = payload.lfeGain;
        working_.propagationDelay = payload.propagationDelay;
        working_.rt60 = payload.rt60;
        working_.earlyBrirRaw24.resize(payload.channelCount);
        active_ = true;
    } else {
        if (payload.sequenceStart) return failAndClear(
            "annex-h-sequence-start-while-active");
        if (payload.channelCount != working_.channelCount
            || payload.channelGain != working_.channelGain
            || payload.lfeGainPresent != working_.lfeGainPresent
            || (payload.lfeGainPresent && payload.lfeGain != working_.lfeGain)) {
            return failAndClear("annex-h-sequence-layout-changed");
        }
    }

    if (!payload.brirDataPresent)
        return failAndClear("annex-h-missing-brir-chunk");
    if (payload.earlyBrirChunks.size() != working_.channelCount
        || payload.lateBrirChunk.size() != kLateChunkBits / 8U) {
        return failAndClear("annex-h-brir-chunk-layout-mismatch");
    }
    for (const auto &chunk : payload.earlyBrirChunks) {
        if (chunk.size() != kEarlyChunkBits / 8U)
            return failAndClear("annex-h-early-brir-chunk-size");
    }
    if (payloads_ >= kEarlyChunks)
        return failAndClear("annex-h-sequence-too-many-chunks");

    ++payloads_;
    ++brirPayloads_;
    ++earlyChunkCount_;
    ++lateChunkCount_;
    for (std::size_t channel = 0U; channel < working_.channelCount; ++channel) {
        appendRaw24(payload.earlyBrirChunks[channel],
                    &working_.earlyBrirRaw24[channel]);
        parityXor_ ^= xorBytes(payload.earlyBrirChunks[channel]);
    }
    appendRaw24(payload.lateBrirChunk, &working_.lateBrirRaw24);
    parityXor_ ^= xorBytes(payload.lateBrirChunk);

    if (!payload.sequenceEnd) {
        if (payloads_ == kEarlyChunks)
            return failAndClear("annex-h-sequence-missing-end");
        ReassemblyResult result;
        result.disposition = ReassemblyDisposition::Pending;
        result.reason = "annex-h-sequence-pending";
        result.payloads = payloads_;
        result.brirPayloads = brirPayloads_;
        return result;
    }

    if (payloads_ != kEarlyChunks || earlyChunkCount_ != kEarlyChunks
        || lateChunkCount_ != kLateChunks
        || working_.lateBrirRaw24.size() != kLateCoefficients) {
        return failAndClear("annex-h-sequence-missing-chunks");
    }
    if (!payload.parityPresent)
        return failAndClear("annex-h-missing-parity");
    const std::uint8_t expectedParity =
        static_cast<std::uint8_t>(parityXor_ ^ 0xA9U);
    if (payload.parityCheck != expectedParity)
        return failAndClear("annex-h-bad-parity");
    for (const auto &channel : working_.earlyBrirRaw24) {
        if (channel.size() != kEarlyCoefficientsPerChannel)
            return failAndClear("annex-h-early-brir-coefficient-count");
    }
    working_.sequenceFrames = payloads_;
    working_.earlyChunkCountPerChannel = earlyChunkCount_;
    working_.lateChunkCount = lateChunkCount_;
    working_.parityCheck = payload.parityCheck;
    committed_ = working_;
    ReassemblyResult result;
    result.disposition = ReassemblyDisposition::Pass;
    result.reason = "annex-h-brir-sequence-reassembled";
    result.payloads = payloads_;
    result.brirPayloads = brirPayloads_;
    result.data = committed_;
    clearWorking();
    return result;
}

ReassemblyResult reassembleSequence(const std::vector<HeadphonePayload> &payloads)
{
    if (payloads.empty())
        return failure(ReassemblyDisposition::Malformed,
                       "annex-h-empty-sequence");
    HeadphoneRenderingCache cache;
    for (const HeadphonePayload &payload : payloads) {
        const ReassemblyResult result = cache.append(payload);
        if (result.disposition != ReassemblyDisposition::Pending)
            return result;
    }
    const std::size_t count = payloads.size();
    cache.reset();
    return failure(ReassemblyDisposition::Malformed,
                   "annex-h-sequence-missing-end", count, count);
}

RoutedResult routedFailure(const PayloadContext &context,
                           ReassemblyDisposition disposition,
                           const char *reason)
{
    RoutedResult result;
    result.disposition = disposition;
    result.reason = reason;
    result.context = context;
    return result;
}

RoutedResult HeadphonePayloadRouter::ingest(
    const PayloadContext &context, const std::vector<std::uint8_t> &bytes)
{
    const std::pair<unsigned, unsigned> key {context.frameType,
                                               context.substreamId};
    StreamState &state = streams_[key];
    if (!context.ac3Frame && context.blocks != 6U) {
        state.cache.reset();
        state.haveTimestamp = false;
        return routedFailure(context, ReassemblyDisposition::Unsupported,
                             "annex-h-requires-ac3-or-six-block-eac3");
    }
    const ParseResult parsed = parse(bytes);
    if (parsed.disposition != Disposition::Pass) {
        state.cache.reset();
        state.haveTimestamp = false;
        return routedFailure(
            context,
            parsed.disposition == Disposition::Unsupported
                ? ReassemblyDisposition::Unsupported
                : ReassemblyDisposition::Malformed,
            parsed.reason.c_str());
    }
    if (state.cache.active()) {
        const std::int64_t expected = state.lastTimestampSamples + 1536;
        if (context.timestampSamples != expected) {
            if (parsed.payload.sequenceStart
                && context.timestampSamples > state.lastTimestampSamples) {
                // A new sequence after a seek/reset supersedes an incomplete
                // generation; never carry its partial chunks forward.
                state.cache.cancel();
                state.haveTimestamp = false;
            } else {
                state.cache.reset();
                state.haveTimestamp = false;
                return routedFailure(context, ReassemblyDisposition::Malformed,
                                     "annex-h-sequence-noncontiguous");
            }
        } else if (parsed.payload.sequenceStart) {
            state.cache.reset();
            state.haveTimestamp = false;
            return routedFailure(context, ReassemblyDisposition::Malformed,
                                 "annex-h-sequence-duplicate-start");
        }
    }
    const ReassemblyResult assembled = state.cache.append(parsed.payload);
    RoutedResult result;
    result.disposition = assembled.disposition;
    result.reason = assembled.reason;
    result.context = context;
    result.data = assembled.data;
    if (assembled.disposition == ReassemblyDisposition::Pending) {
        state.lastTimestampSamples = context.timestampSamples;
        state.haveTimestamp = true;
    } else if (assembled.disposition == ReassemblyDisposition::Pass) {
        ++completedSequences_;
        state.haveTimestamp = false;
    } else {
        state.haveTimestamp = false;
    }
    return result;
}

std::size_t HeadphonePayloadRouter::reset()
{
    const std::size_t count = activeStreams();
    streams_.clear();
    completedSequences_ = 0U;
    return count;
}

std::size_t HeadphonePayloadRouter::cancel()
{
    std::size_t count = 0U;
    for (auto &[key, state] : streams_) {
        (void)key;
        if (state.cache.active()) ++count;
        state.cache.cancel();
    }
    streams_.clear();
    return count;
}

std::size_t HeadphonePayloadRouter::activeStreams() const
{
    std::size_t count = 0U;
    for (const auto &[key, state] : streams_) {
        (void)key;
        if (state.cache.active()) ++count;
    }
    return count;
}

} // namespace eac3annexh
