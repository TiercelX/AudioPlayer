#include "annex-h-headphone.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

class Writer {
public:
    void put(unsigned count, unsigned value)
    {
        for (unsigned index = 0U; index < count; ++index)
            bits_.push_back(static_cast<std::uint8_t>(
                (value >> (count - index - 1U)) & 1U));
    }
    std::vector<std::uint8_t> bytes() const
    {
        std::vector<std::uint8_t> result((bits_.size() + 7U) / 8U, 0U);
        for (std::size_t bit = 0U; bit < bits_.size(); ++bit)
            if (bits_[bit]) result[bit / 8U] |=
                static_cast<std::uint8_t>(1U << (7U - bit % 8U));
        return result;
    }
private:
    std::vector<std::uint8_t> bits_;
};

std::vector<std::uint8_t> makePayload(bool start, bool end, bool brir)
{
    Writer writer;
    writer.put(3U, 2U); // two full-bandwidth channels
    writer.put(6U, 3U); writer.put(6U, 17U);
    writer.put(1U, 1U); writer.put(6U, 9U); // LFE gain
    writer.put(1U, start ? 1U : 0U);
    writer.put(1U, end ? 1U : 0U);
    writer.put(1U, brir ? 1U : 0U);
    if (brir) {
        for (unsigned index = 0U; index < 2U * 96U + 1536U; ++index)
            writer.put(1U, index == 0U ? 1U : 0U);
    }
    if (end) writer.put(8U, 0xA9U);
    if (start) {
        writer.put(1U, 1U); // propagation delay present
        writer.put(11U, 0U); writer.put(11U, 2047U);
        writer.put(1U, 1U); // RT60 present
        writer.put(4U, 1U); writer.put(11U, 1U);
    }
    return writer.bytes();
}

std::vector<std::uint8_t> makeChunkPayload(unsigned channelCount,
                                           bool start, bool end,
                                           unsigned frameIndex,
                                           unsigned parityDelta = 0U)
{
    Writer writer;
    writer.put(3U, channelCount);
    for (unsigned channel = 0U; channel < channelCount; ++channel)
        writer.put(6U, 3U + channel);
    writer.put(1U, 1U); writer.put(6U, 9U); // LFE gain
    writer.put(1U, start ? 1U : 0U);
    writer.put(1U, end ? 1U : 0U);
    writer.put(1U, 1U); // brir_datae
    std::uint8_t parity = 0U;
    for (unsigned channel = 0U; channel < channelCount; ++channel) {
        for (unsigned byte = 0U; byte < 12U; ++byte) {
            const auto value = static_cast<std::uint8_t>(
                0x11U + frameIndex * 7U + channel * 13U + byte);
            writer.put(8U, value);
            parity ^= value;
        }
    }
    for (unsigned byte = 0U; byte < 192U; ++byte) {
        const auto value = static_cast<std::uint8_t>(
            0x80U + frameIndex * 5U + byte);
        writer.put(8U, value);
        parity ^= value;
    }
    if (end) writer.put(8U, static_cast<unsigned>(parity ^ 0xA9U ^ parityDelta));
    if (start) {
        writer.put(1U, 1U); // propagation delay present
        for (unsigned channel = 0U; channel < channelCount; ++channel)
            writer.put(11U, channel == 0U ? 0U : 2047U);
        writer.put(1U, 1U); // RT60 present
        writer.put(4U, 1U); writer.put(11U, 1U);
    }
    return writer.bytes();
}

std::uint8_t chunkParity(unsigned channelCount, unsigned frameIndex)
{
    std::uint8_t parity = 0U;
    for (unsigned channel = 0U; channel < channelCount; ++channel)
        for (unsigned byte = 0U; byte < 12U; ++byte)
            parity ^= static_cast<std::uint8_t>(
                0x11U + frameIndex * 7U + channel * 13U + byte);
    for (unsigned byte = 0U; byte < 192U; ++byte)
        parity ^= static_cast<std::uint8_t>(0x80U + frameIndex * 5U + byte);
    return parity;
}

std::vector<eac3annexh::HeadphonePayload> parseFrames(
    const std::vector<std::vector<std::uint8_t>> &frames,
    bool *ok)
{
    std::vector<eac3annexh::HeadphonePayload> payloads;
    bool success = true;
    for (std::size_t index = 0U; index < frames.size(); ++index) {
        const auto &frame = frames[index];
        const auto parsed = eac3annexh::parse(frame);
        if (parsed.disposition != eac3annexh::Disposition::Pass) {
            success = false;
            break;
        }
        payloads.push_back(parsed.payload);
    }
    if (ok) *ok = success;
    return payloads;
}

bool sameData(const eac3annexh::HeadphoneRenderingData &left,
              const eac3annexh::HeadphoneRenderingData &right)
{
    return left.channelCount == right.channelCount
        && left.channelGain == right.channelGain
        && left.lfeGainPresent == right.lfeGainPresent
        && left.lfeGain == right.lfeGain
        && left.propagationDelay == right.propagationDelay
        && left.rt60 == right.rt60
        && left.earlyBrirRaw24 == right.earlyBrirRaw24
        && left.lateBrirRaw24 == right.lateBrirRaw24
        && left.sequenceFrames == right.sequenceFrames
        && left.earlyChunkCountPerChannel == right.earlyChunkCountPerChannel
        && left.lateChunkCount == right.lateChunkCount
        && left.parityCheck == right.parityCheck;
}

eac3annexh::PayloadContext contextFor(unsigned index,
                                      unsigned substreamId = 0U,
                                      unsigned blocks = 6U,
                                      bool ac3Frame = false)
{
    eac3annexh::PayloadContext context;
    context.accessUnitOrdinal = index;
    context.frameOrdinal = 0U;
    context.timestampSamples = static_cast<std::int64_t>(index) * 1536;
    context.frameType = ac3Frame ? 2U : 0U;
    context.substreamId = substreamId;
    context.blocks = blocks;
    context.ac3Frame = ac3Frame;
    return context;
}

bool expect(bool value, const char *name, std::string *reason)
{
    if (value) return true;
    if (reason && reason->empty()) *reason = name;
    return false;
}

} // namespace

int main()
{
    std::string reason;
    const auto start = eac3annexh::parse(makePayload(true, false, true));
    const auto end = eac3annexh::parse(makePayload(false, true, true));
    const auto ordinary = eac3annexh::parse(makePayload(false, false, false));
    unsigned cases = 0U;
    if (expect(start.disposition == eac3annexh::Disposition::Pass
                   && start.payload.channelCount == 2U
                   && start.payload.lfeGainPresent
                   && start.payload.sequenceStart
                   && start.payload.brirDataPresent
                   && start.payload.propagationDelay.size() == 2U
                   && start.payload.rt60Bands == 1U,
               "annex-h-header-presence", &reason)) ++cases;
    if (expect(end.disposition == eac3annexh::Disposition::Pass
                   && end.payload.sequenceEnd,
               "annex-h-sequence-end-header", &reason)) ++cases;
    if (expect(ordinary.disposition == eac3annexh::Disposition::Pass
                   && !ordinary.payload.brirDataPresent,
               "annex-h-absence-branch", &reason)) ++cases;
    if (expect(eac3annexh::validateSequence(
                   {start.payload, end.payload}).disposition
                   == eac3annexh::Disposition::Pass,
               "annex-h-sequence-start-end", &reason)) ++cases;
    if (expect(eac3annexh::validateSequence(
                   {end.payload}).disposition
                   == eac3annexh::Disposition::Malformed,
               "annex-h-sequence-without-start", &reason)) ++cases;
    if (expect(eac3annexh::validateSequence(
                   {start.payload}).disposition
                   == eac3annexh::Disposition::Malformed,
               "annex-h-sequence-missing-end", &reason)) ++cases;
    auto truncated = makePayload(true, false, true);
    truncated.pop_back();
    if (expect(eac3annexh::parse(truncated).disposition
                   == eac3annexh::Disposition::Malformed,
               "annex-h-truncation", &reason)) ++cases;

    std::vector<std::vector<std::uint8_t>> fullFrames;
    fullFrames.reserve(32U);
    std::uint8_t sequenceXor = 0U;
    for (unsigned index = 0U; index < 32U; ++index)
        sequenceXor ^= chunkParity(2U, index);
    for (unsigned index = 0U; index < 32U; ++index)
        fullFrames.push_back(makeChunkPayload(2U, index == 0U,
                                              index == 31U, index,
                                              index == 31U
                                                  ? chunkParity(2U, index)
                                                      ^ sequenceXor
                                                  : 0U));
    bool fullParseOk = false;
    const auto fullPayloads = parseFrames(fullFrames, &fullParseOk);
    const auto firstAssembly = eac3annexh::reassembleSequence(fullPayloads);
    const auto secondAssembly = eac3annexh::reassembleSequence(fullPayloads);
    if (expect(fullParseOk
                   && firstAssembly.disposition
                          == eac3annexh::ReassemblyDisposition::Pass
                   && firstAssembly.payloads == 32U
                   && firstAssembly.brirPayloads == 32U
                   && firstAssembly.data.channelCount == 2U
                   && firstAssembly.data.sequenceFrames == 32U
                   && firstAssembly.data.earlyChunkCountPerChannel == 32U
                   && firstAssembly.data.lateChunkCount == 32U
                   && firstAssembly.data.earlyBrirRaw24.size() == 2U
                   && firstAssembly.data.earlyBrirRaw24[0].size() == 128U
                   && firstAssembly.data.lateBrirRaw24.size() == 2048U
                   && firstAssembly.data.earlyBrirRaw24[0][0] == 0x111213U
                   && firstAssembly.data.earlyBrirRaw24[1][0] == 0x1e1f20U
                   && firstAssembly.data.lateBrirRaw24[0] == 0x808182U
                   && firstAssembly.data.parityCheck
                          == static_cast<std::uint8_t>(sequenceXor ^ 0xA9U)
                   && secondAssembly.disposition
                          == eac3annexh::ReassemblyDisposition::Pass
                   && sameData(firstAssembly.data, secondAssembly.data),
               "annex-h-32-frame-reassembly-deterministic", &reason)) ++cases;

    auto badParityFrames = fullFrames;
    badParityFrames.back() = makeChunkPayload(
        2U, false, true, 31U,
        static_cast<unsigned>(chunkParity(2U, 31U) ^ sequenceXor ^ 1U));
    bool badParityParseOk = false;
    const auto badParityPayloads = parseFrames(badParityFrames, &badParityParseOk);
    const auto badParity = eac3annexh::reassembleSequence(badParityPayloads);
    if (expect(badParityParseOk
                   && badParity.disposition
                          == eac3annexh::ReassemblyDisposition::Malformed
                   && badParity.reason == "annex-h-bad-parity"
                   && badParity.data.channelCount == 0U,
               "annex-h-bad-parity-empty-on-failure", &reason)) ++cases;

    auto shortFrames = fullFrames;
    shortFrames.pop_back();
    std::uint8_t shortSequenceXor = 0U;
    for (unsigned index = 0U; index < 31U; ++index)
        shortSequenceXor ^= chunkParity(2U, index);
    shortFrames.back() = makeChunkPayload(
        2U, false, true, 30U,
        static_cast<unsigned>(chunkParity(2U, 30U) ^ shortSequenceXor));
    bool shortParseOk = false;
    const auto shortPayloads = parseFrames(shortFrames, &shortParseOk);
    const auto shortResult = eac3annexh::reassembleSequence(shortPayloads);
    if (expect(shortParseOk
                   && shortResult.disposition
                          == eac3annexh::ReassemblyDisposition::Malformed
                   && shortResult.reason == "annex-h-sequence-missing-chunks",
               "annex-h-missing-chunks", &reason)) ++cases;

    auto layoutFrames = fullFrames;
    layoutFrames[7] = makeChunkPayload(3U, false, false, 7U);
    bool layoutParseOk = false;
    const auto layoutPayloads = parseFrames(layoutFrames, &layoutParseOk);
    const auto layoutResult = eac3annexh::reassembleSequence(layoutPayloads);
    if (expect(layoutParseOk
                   && layoutResult.disposition
                          == eac3annexh::ReassemblyDisposition::Malformed
                   && layoutResult.reason == "annex-h-sequence-layout-changed",
               "annex-h-layout-change", &reason)) ++cases;

    auto duplicateStartFrames = fullFrames;
    duplicateStartFrames[1] = makeChunkPayload(2U, true, false, 1U);
    bool duplicateStartParseOk = false;
    const auto duplicateStartPayloads = parseFrames(duplicateStartFrames,
                                                    &duplicateStartParseOk);
    const auto duplicateStart =
        eac3annexh::reassembleSequence(duplicateStartPayloads);
    if (expect(duplicateStartParseOk
                   && duplicateStart.disposition
                          == eac3annexh::ReassemblyDisposition::Malformed
                   && duplicateStart.reason
                          == "annex-h-sequence-start-while-active",
               "annex-h-duplicate-start", &reason)) ++cases;

    const auto endOnly = eac3annexh::reassembleSequence({end.payload});
    if (expect(endOnly.disposition == eac3annexh::ReassemblyDisposition::Malformed
                   && endOnly.reason == "annex-h-sequence-end-without-start",
               "annex-h-end-without-start", &reason)) ++cases;

    eac3annexh::HeadphoneRenderingCache cache;
    const auto pending = cache.append(fullPayloads.front());
    const auto cancelled = cache.cancel();
    if (expect(pending.disposition == eac3annexh::ReassemblyDisposition::Pending
                   && cancelled.disposition
                          == eac3annexh::ReassemblyDisposition::Cancelled
                   && !cache.active()
                   && cache.committed().channelCount == 0U,
               "annex-h-cache-cancel-empty", &reason)) ++cases;
    cache.reset();
    if (expect(!cache.active() && cache.committed().lateBrirRaw24.empty(),
               "annex-h-cache-reset-empty", &reason)) ++cases;

    eac3annexh::HeadphonePayloadRouter router;
    eac3annexh::RoutedResult routedLast;
    std::size_t unrelatedPayloads = 0U;
    for (unsigned index = 0U; index < 32U; ++index) {
        // Payload IDs 11/14 are deliberately not sent to this ID-7 router.
        if (index == 8U || index == 20U) ++unrelatedPayloads;
        routedLast = router.ingest(contextFor(index), fullFrames[index]);
    }
    if (expect(unrelatedPayloads == 2U
                   && routedLast.disposition
                          == eac3annexh::ReassemblyDisposition::Pass
                   && routedLast.context.accessUnitOrdinal == 31U
                   && routedLast.context.substreamId == 0U
                   && routedLast.data.sequenceFrames == 32U
                   && router.completedSequences() == 1U
                   && router.activeStreams() == 0U,
               "annex-h-outer-au-router-complete", &reason)) ++cases;

    eac3annexh::HeadphonePayloadRouter ac3Router;
    eac3annexh::RoutedResult ac3Last;
    for (unsigned index = 0U; index < 32U; ++index)
        ac3Last = ac3Router.ingest(contextFor(index, 0U, 6U, true),
                                   fullFrames[index]);
    if (expect(ac3Last.disposition == eac3annexh::ReassemblyDisposition::Pass
                   && ac3Last.context.frameType == 2U
                   && ac3Router.completedSequences() == 1U,
               "annex-h-outer-ac3-carrier-complete", &reason)) ++cases;

    eac3annexh::HeadphonePayloadRouter droppedRouter;
    auto dropped = droppedRouter.ingest(contextFor(0U), fullFrames[0]);
    dropped = droppedRouter.ingest(contextFor(1U), fullFrames[1]);
    dropped = droppedRouter.ingest(contextFor(3U), fullFrames[3]);
    if (expect(dropped.disposition == eac3annexh::ReassemblyDisposition::Malformed
                   && dropped.reason == "annex-h-sequence-noncontiguous"
                   && droppedRouter.activeStreams() == 0U,
               "annex-h-outer-au-drop-fail-closed", &reason)) ++cases;

    eac3annexh::HeadphonePayloadRouter duplicateRouter;
    duplicateRouter.ingest(contextFor(0U), fullFrames[0]);
    duplicateRouter.ingest(contextFor(1U), fullFrames[1]);
    const auto duplicate = duplicateRouter.ingest(contextFor(1U), fullFrames[1]);
    if (expect(duplicate.disposition == eac3annexh::ReassemblyDisposition::Malformed
                   && duplicate.reason == "annex-h-sequence-noncontiguous"
                   && duplicateRouter.activeStreams() == 0U,
               "annex-h-outer-au-duplicate-fail-closed", &reason)) ++cases;

    eac3annexh::HeadphonePayloadRouter seekRouter;
    seekRouter.ingest(contextFor(0U), fullFrames[0]);
    const auto seekStart = seekRouter.ingest(contextFor(40U), fullFrames[0]);
    if (expect(seekStart.disposition == eac3annexh::ReassemblyDisposition::Pending
                   && seekRouter.activeStreams() == 1U,
               "annex-h-outer-au-seek-new-generation", &reason)) ++cases;
    const auto seekCanceled = seekRouter.cancel();
    if (expect(seekCanceled == 1U && seekRouter.activeStreams() == 0U,
               "annex-h-outer-au-cancel-generation", &reason)) ++cases;

    eac3annexh::HeadphonePayloadRouter wrongStreamRouter;
    wrongStreamRouter.ingest(contextFor(0U, 1U), fullFrames[0]);
    const auto wrongStream = wrongStreamRouter.ingest(contextFor(1U, 0U), fullFrames[1]);
    if (expect(wrongStream.disposition == eac3annexh::ReassemblyDisposition::Malformed
                   && wrongStream.reason == "annex-h-sequence-end-without-start",
               "annex-h-outer-au-wrong-substream", &reason)) ++cases;

    const auto truncatedOuter = eac3annexh::HeadphonePayloadRouter {}.ingest(
        contextFor(0U), std::vector<std::uint8_t>(fullFrames[0].begin(),
                                                  fullFrames[0].end() - 1));
    if (expect(truncatedOuter.disposition
                   == eac3annexh::ReassemblyDisposition::Malformed,
               "annex-h-outer-au-payload-truncation", &reason)) ++cases;
    const auto wrongBlocks = eac3annexh::HeadphonePayloadRouter {}.ingest(
        contextFor(0U, 0U, 1U), fullFrames[0]);
    if (expect(wrongBlocks.disposition
                   == eac3annexh::ReassemblyDisposition::Unsupported,
               "annex-h-outer-au-frame-classification", &reason)) ++cases;
    std::cout << "annexHSelfTest=" << (reason.empty() ? "PASS" : "FAIL")
              << " cases=" << cases << " reason="
              << (reason.empty() ? "bounded-annex-h-header-and-sequence" : reason)
              << " brirReassembly=PASS raw24NoScale=YES\n";
    return reason.empty() ? 0 : 1;
}
