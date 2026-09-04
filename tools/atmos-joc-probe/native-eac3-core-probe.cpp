#include "native-eac3-core.h"

#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {
using namespace eac3native;

void setBits(std::vector<std::uint8_t> &bytes, std::size_t bit,
             unsigned count, unsigned value)
{
    for (unsigned i = 0; i < count; ++i) {
        const std::size_t pos = bit + i;
        const std::uint8_t mask =
            static_cast<std::uint8_t>(1U << (7U - (pos % 8U)));
        if (((value >> (count - i - 1U)) & 1U) != 0U) {
            bytes[pos / 8U] |= mask;
        } else {
            bytes[pos / 8U] &= static_cast<std::uint8_t>(~mask);
        }
    }
}

std::vector<std::uint8_t> syntheticFrame(unsigned streamType, unsigned sid,
                                          unsigned fscod, unsigned blocksCode,
                                          unsigned acmod = 2, bool lfe = false,
                                          unsigned bsid = 16)
{
    std::vector<std::uint8_t> bytes(12U, 0);
    bytes[0] = 0x0b;
    bytes[1] = 0x77;
    setBits(bytes, 16, 2, streamType);
    setBits(bytes, 18, 3, sid);
    setBits(bytes, 21, 11, 5); // 12 bytes: frmsiz=(5)+1 words.
    setBits(bytes, 32, 2, fscod);
    setBits(bytes, 34, 2, fscod == 3 ? 0 : blocksCode);
    setBits(bytes, 36, 3, acmod);
    setBits(bytes, 39, 1, lfe ? 1 : 0);
    setBits(bytes, 40, 5, bsid);
    return bytes;
}

std::vector<std::uint8_t> syntheticLegacyAc3Frame(unsigned acmod, bool lfe)
{
    // fscod=0/frmsizecod=0/bsid=6 gives the 128-byte 48 kHz AC-3 frame
    // used only for header-position self-tests.
    std::vector<std::uint8_t> bytes(128U, 0);
    bytes[0] = 0x0b;
    bytes[1] = 0x77;
    setBits(bytes, 32, 2, 0); // fscod
    setBits(bytes, 34, 6, 0); // frmsizecod
    setBits(bytes, 40, 5, 6); // legacy AC-3 bsid
    setBits(bytes, 45, 3, 0); // bsmod
    setBits(bytes, 48, 3, acmod);
    std::size_t lfeBit = 51;
    if ((acmod & 1U) && acmod != 1U) {
        lfeBit += 2; // cmixlev
    }
    if (acmod & 4U) {
        lfeBit += 2; // surmixlev
    }
    if (acmod == 2U) {
        lfeBit += 2; // dsurmod
    }
    setBits(bytes, lfeBit, 1, lfe ? 1U : 0U);
    return bytes;
}

void append(std::vector<std::uint8_t> &dst, const std::vector<std::uint8_t> &src)
{
    dst.insert(dst.end(), src.begin(), src.end());
}

bool expect(bool value, const char *name)
{
    if (!value) {
        std::cerr << "selfTest=FAIL case=" << name << '\n';
        return false;
    }
    return true;
}

bool acceptedProcess(const ProcessResult &result)
{
    return result.disposition == Disposition::Accepted
           && result.flow == FlowStatus::None;
}

bool strictFrameSequence(const std::vector<std::uint8_t> &bytes)
{
    std::size_t offset = 0U;
    while (offset < bytes.size()) {
        if (!eb3SyncAt(bytes, offset)) {
            return false;
        }
        const ParseResult parsed = parseSyncframe(bytes, offset);
        if (parsed.disposition != Disposition::Accepted || !parsed.frame
            || parsed.frame->sizeBytes == 0U) {
            return false;
        }
        offset += parsed.frame->sizeBytes;
    }
    return true;
}

bool validFrame(const std::vector<std::uint8_t> &bytes, FrameHeader *out,
                const char *name)
{
    const ParseResult result = parseSyncframe(bytes, 0);
    if (!expect(result.disposition == Disposition::Accepted && result.frame,
                name)) {
        return false;
    }
    *out = *result.frame;
    return true;
}

bool runSelfTest()
{
    const std::array<unsigned, 4> expectedBlocks = {1, 2, 3, 6};
    std::size_t cases = 0;
    for (unsigned code = 0; code < expectedBlocks.size(); ++code) {
        FrameHeader frame;
        if (!validFrame(syntheticFrame(0, 0, 0, code), &frame, "valid-frame")) {
            return false;
        }
        if (!expect(frame.blocks == expectedBlocks[code]
                        && frame.sampleCount == expectedBlocks[code] * 256U,
                    "valid-block-shape")) {
            return false;
        }
        ++cases;
    }
    for (unsigned acmod : {1U, 2U}) {
        FrameHeader legacy;
        if (!validFrame(syntheticLegacyAc3Frame(acmod, true), &legacy,
                        "legacy-ac3-acmod")) {
            return false;
        }
        if (!expect(legacy.streamType == StreamType::LegacyAc3
                        && legacy.acmod == acmod && legacy.lfe
                        && legacy.channelCount
                               == (acmod == 1U ? 2U : 3U),
                    "legacy-ac3-lfe-position")) {
            return false;
        }
        ++cases;
    }

    const ParseResult notEac3 = parseSyncframe({0, 1, 2}, 0);
    if (!expect(notEac3.disposition == Disposition::NotEac3
                    && notEac3.stage == FailureStage::None,
                "not-eac3")) {
        return false;
    }
    const auto full = syntheticFrame(0, 0, 0, 0);
    std::vector<std::uint8_t> shortFrame(full.begin(), full.begin() + 8);
    const ParseResult truncated = parseSyncframe(shortFrame, 0);
    if (!expect(truncated.disposition == Disposition::Malformed
                    && truncated.stage == FailureStage::Bounds,
                "truncated")) {
        return false;
    }
    std::vector<std::uint8_t> reserved = syntheticFrame(3, 0, 0, 3);
    const ParseResult reservedResult = parseSyncframe(reserved, 0);
    if (!expect(reservedResult.disposition == Disposition::Unsupported
                    && reservedResult.stage == FailureStage::Header,
                "reserved-stream-type")) {
        return false;
    }
    std::vector<std::uint8_t> badRate = syntheticFrame(0, 0, 3, 0);
    setBits(badRate, 34, 2, 3);
    const ParseResult badRateResult = parseSyncframe(badRate, 0);
    if (!expect(badRateResult.disposition == Disposition::Unsupported
                    && badRateResult.reason == "reserved-eac3-fscod3",
                "reserved-fscod3")) {
        return false;
    }
    for (unsigned bsid : {9U, 10U, 17U}) {
        const ParseResult badBsid = parseSyncframe(
            syntheticFrame(0, 0, 0, 3, 2, false, bsid), 0);
        if (!expect(badBsid.disposition == Disposition::Unsupported,
                    "reserved-bsid")) {
            return false;
        }
    }
    cases += 7;

    std::vector<std::uint8_t> six;
    append(six, syntheticFrame(0, 0, 0, 0));
    append(six, syntheticFrame(0, 0, 0, 1));
    append(six, syntheticFrame(0, 0, 0, 2));
    AccessUnitAssembler assembler;
    std::size_t offset = 0;
    for (unsigned blocks : expectedBlocks) {
        if (blocks == 6) {
            break;
        }
        const ParseResult result = parseSyncframe(six, offset);
        if (!expect(result.disposition == Disposition::Accepted
                        && result.frame->blocks == blocks,
                    "six-block-input")) {
            return false;
        }
        if (!expect(acceptedProcess(assembler.process(result)),
                    "six-block-process")) {
            return false;
        }
        offset += result.frame->sizeBytes;
    }
    const ProcessResult sixResult = assembler.flush();
    if (!expect(sixResult.completed
                    && sixResult.completed->sampleCount == 1536U
                    && sixResult.completed->frames.size() == 3U,
                "six-block-assembly")) {
        return false;
    }
    AccessUnitAssembler threePlusThree;
    if (!expect(acceptedProcess(threePlusThree.process(
                        parseSyncframe(syntheticFrame(0, 0, 0, 2), 0)))
                    && acceptedProcess(threePlusThree.process(
                        parseSyncframe(syntheticFrame(0, 0, 0, 2), 0))),
                "three-plus-three-process")) {
        return false;
    }
    if (!expect(threePlusThree.flush().completed.has_value(),
                "three-plus-three")) {
        return false;
    }
    AccessUnitAssembler twoPlusTwoPlusTwo;
    for (unsigned i = 0; i < 3U; ++i) {
        if (!expect(acceptedProcess(twoPlusTwoPlusTwo.process(
                            parseSyncframe(syntheticFrame(0, 0, 0, 1), 0))),
                    "two-plus-two-plus-two-process")) {
            return false;
        }
    }
    if (!expect(twoPlusTwoPlusTwo.flush().completed.has_value(),
                "two-plus-two-plus-two")) {
        return false;
    }
    cases += 3;

    AccessUnitAssembler topology;
    const std::array<std::vector<std::uint8_t>, 11> topologyFrames = {
        syntheticFrame(0, 0, 0, 2), syntheticFrame(1, 0, 0, 2),
        syntheticFrame(1, 1, 0, 2), syntheticFrame(0, 1, 0, 2),
        syntheticFrame(1, 0, 0, 2), syntheticFrame(0, 0, 0, 2),
        syntheticFrame(1, 0, 0, 2), syntheticFrame(1, 1, 0, 2),
        syntheticFrame(0, 1, 0, 2), syntheticFrame(1, 0, 0, 2),
        syntheticFrame(0, 0, 0, 2)};
    for (std::size_t i = 0; i + 1U < topologyFrames.size(); ++i) {
        const auto &bytes = topologyFrames[i];
        if (!expect(acceptedProcess(topology.process(parseSyncframe(bytes, 0))),
                    "topology-process")) {
            return false;
        }
    }
    const ProcessResult topologyResult = topology.process(parseSyncframe(
        topologyFrames.back(), 0));
    if (!expect(acceptedProcess(topologyResult)
                    && topologyResult.completed
                    && topologyResult.completed->frames.size() == 10U
                    && topologyResult.completed->blocksByStream.size() == 5U,
                "dependent-additional-grouping")) {
        return false;
    }
    const ParseResult marker = parseSyncframe(syntheticFrame(1, 0, 0, 3), 0);
    if (!expect(marker.frame && marker.frame->dependent
                    && !marker.frame->additional,
                "dependent-marker")) {
        return false;
    }
    const ParseResult additional =
        parseSyncframe(syntheticFrame(0, 1, 0, 3), 0);
    if (!expect(additional.frame && !additional.frame->dependent
                    && additional.frame->additional,
                "additional-marker")) {
        return false;
    }
    cases += 3;

    AccessUnitAssembler orphan;
    const ProcessResult orphanResult = orphan.process(
        parseSyncframe(syntheticFrame(1, 0, 0, 3), 0));
    if (!expect(orphanResult.disposition == Disposition::Malformed
                    && orphanResult.reason == "au-start-not-base-sid0",
                "orphan-dependent")) {
        return false;
    }
    if (!expect(acceptedProcess(orphan.process(
                        parseSyncframe(syntheticFrame(0, 0, 0, 2), 0)))
                    && acceptedProcess(orphan.process(
                        parseSyncframe(syntheticFrame(0, 1, 0, 2), 0))),
                "orphan-recovery-process")) {
        return false;
    }
    const ProcessResult nonImmediate = orphan.process(
        parseSyncframe(syntheticFrame(1, 1, 0, 2), 0));
    if (!expect(nonImmediate.disposition == Disposition::Malformed
                    && nonImmediate.reason == "dependent-local-id-gap",
                "dependent-not-immediate")) {
        return false;
    }
    AccessUnitAssembler shape;
    if (!expect(acceptedProcess(shape.process(
                        parseSyncframe(syntheticFrame(0, 0, 0, 2), 0))),
                "shape-base-process")) {
        return false;
    }
    const ProcessResult shapeMismatch = shape.process(
        parseSyncframe(syntheticFrame(1, 0, 0, 1), 0));
    if (!expect(shapeMismatch.disposition == Disposition::Malformed
                    && shapeMismatch.reason == "dependent-shape-mismatch",
                "dependent-shape-mismatch")) {
        return false;
    }
    AccessUnitAssembler sidGap;
    if (!expect(acceptedProcess(sidGap.process(
                        parseSyncframe(syntheticFrame(0, 0, 0, 2), 0))),
                "sid-gap-base-process")) {
        return false;
    }
    const ProcessResult gap = sidGap.process(
        parseSyncframe(syntheticFrame(0, 2, 0, 2), 0));
    if (!expect(gap.disposition == Disposition::Malformed
                    && gap.reason == "additional-substream-id-gap",
                "additional-sid-gap")) {
        return false;
    }
    AccessUnitAssembler rollbackAdditional;
    if (!expect(acceptedProcess(rollbackAdditional.process(parseSyncframe(
                        syntheticFrame(0, 0, 0, 3), 0))),
                "rollback-additional-base")) {
        return false;
    }
    const ProcessResult rejectedAdditional = rollbackAdditional.process(
        parseSyncframe(syntheticFrame(0, 2, 0, 3), 0));
    if (!expect(rejectedAdditional.disposition == Disposition::Malformed
                    && rejectedAdditional.reason
                           == "additional-substream-id-gap"
                    && acceptedProcess(rollbackAdditional.process(
                        parseSyncframe(syntheticFrame(0, 1, 0, 3), 0))),
                "rollback-additional-state")) {
        return false;
    }
    AccessUnitAssembler rollbackDependent;
    if (!expect(acceptedProcess(rollbackDependent.process(parseSyncframe(
                        syntheticFrame(0, 0, 0, 3), 0))),
                "rollback-dependent-base")) {
        return false;
    }
    const ProcessResult rejectedDependent = rollbackDependent.process(
        parseSyncframe(syntheticFrame(1, 1, 0, 3), 0));
    if (!expect(rejectedDependent.disposition == Disposition::Malformed
                    && rejectedDependent.reason == "dependent-local-id-gap"
                    && acceptedProcess(rollbackDependent.process(
                        parseSyncframe(syntheticFrame(1, 0, 0, 3), 0))),
                "rollback-dependent-state")) {
        return false;
    }
    AccessUnitAssembler complete;
    if (!expect(acceptedProcess(complete.process(
                        parseSyncframe(syntheticFrame(0, 0, 0, 3), 0))),
                "complete-base-process")) {
        return false;
    }
    const ProcessResult extra = complete.process(
        parseSyncframe(syntheticFrame(0, 1, 0, 3), 0));
    if (!expect(acceptedProcess(extra),
                "complete-au-boundary")) {
        return false;
    }
    const ProcessResult completeFlush = complete.flush();
    if (!expect(completeFlush.completed
                    && completeFlush.completed->sampleCount == 1536U,
                "complete-au-with-additional")) {
        return false;
    }
    AccessUnitAssembler type2;
    if (!expect(acceptedProcess(type2.process(parseSyncframe(
                        syntheticFrame(2, 0, 0, 3), 0))),
                "eac3-strmtyp2-alone")) {
        return false;
    }
    if (!expect(type2.flush().completed.has_value(), "eac3-strmtyp2-flush")) {
        return false;
    }
    AccessUnitAssembler type2Dependent;
    if (!expect(acceptedProcess(type2Dependent.process(
                        parseSyncframe(syntheticFrame(2, 0, 0, 3), 0))),
                "eac3-strmtyp2-base-process")) {
        return false;
    }
    const ProcessResult type2Result = type2Dependent.process(
        parseSyncframe(syntheticFrame(1, 0, 0, 3), 0));
    if (!expect(type2Result.disposition == Disposition::Unsupported
                    && type2Result.reason
                           == "eac3-strmtyp2-dependent-forbidden",
                "eac3-strmtyp2-dependent-reject")) {
        return false;
    }
    AccessUnitAssembler disappear;
    if (!expect(acceptedProcess(disappear.process(
                        parseSyncframe(syntheticFrame(0, 0, 0, 3), 0)))
                    && acceptedProcess(disappear.process(
                        parseSyncframe(syntheticFrame(1, 0, 0, 3), 0))),
                "topology-disappear-first-au")) {
        return false;
    }
    const ProcessResult topologyBoundary = disappear.process(parseSyncframe(
        syntheticFrame(0, 0, 0, 3), 0));
    if (!expect(acceptedProcess(topologyBoundary)
                    && topologyBoundary.completed.has_value(),
                "topology-boundary")) {
        return false;
    }
    const ProcessResult missingDependent = disappear.flush();
    if (!expect(missingDependent.disposition == Disposition::Malformed
                    && missingDependent.reason
                           == "access-unit-topology-mismatch",
                "topology-disappear")) {
        return false;
    }
    AccessUnitAssembler changedTopology;
    if (!expect(acceptedProcess(changedTopology.process(
                        parseSyncframe(syntheticFrame(0, 0, 0, 3), 0)))
                    && acceptedProcess(changedTopology.process(
                        parseSyncframe(syntheticFrame(1, 0, 0, 3), 0)))
                    && acceptedProcess(changedTopology.process(
                        parseSyncframe(syntheticFrame(0, 0, 0, 3), 0)))
                    && acceptedProcess(changedTopology.process(
                        parseSyncframe(syntheticFrame(0, 1, 0, 3), 0))),
                "topology-change-process")) {
        return false;
    }
    const ProcessResult changedTopologyResult = changedTopology.flush();
    if (!expect(changedTopologyResult.disposition == Disposition::Malformed
                    && changedTopologyResult.reason
                           == "access-unit-topology-mismatch",
                "topology-change")) {
        return false;
    }
    cases += 9;

    AccessUnitAssembler sequence;
    const ParseResult rate48 = parseSyncframe(syntheticFrame(0, 0, 0, 0), 0);
    const ParseResult rate44 = parseSyncframe(syntheticFrame(0, 0, 1, 0), 0);
    if (!expect(acceptedProcess(sequence.process(rate48)),
                "sequence-rate48-process")) {
        return false;
    }
    const ProcessResult changed = sequence.process(rate44);
    if (!expect(changed.disposition == Disposition::Malformed
                    && changed.stage == FailureStage::Sequence,
                "sample-rate-change")) {
        return false;
    }
    FrameHeader four = *rate48.frame;
    four.blocks = 4;
    four.sampleCount = 1024;
    const ProcessResult fourResult = sequence.process(four);
    if (!expect(fourResult.disposition == Disposition::Malformed
                    && fourResult.stage == FailureStage::Validation,
                "block-four-reject")) {
        return false;
    }
    FrameHeader five = *rate48.frame;
    five.blocks = 5;
    five.sampleCount = 1280;
    const ProcessResult fiveResult = sequence.process(five);
    if (!expect(fiveResult.disposition == Disposition::Malformed
                    && fiveResult.stage == FailureStage::Validation,
                "block-five-reject")) {
        return false;
    }
    FrameHeader invalidBlocks = *rate48.frame;
    invalidBlocks.blocks = 0;
    invalidBlocks.sampleCount = 0;
    const ProcessResult invalidBlockResult = sequence.process(invalidBlocks);
    if (!expect(invalidBlockResult.disposition == Disposition::Malformed
                    && invalidBlockResult.stage == FailureStage::Validation,
                "reserved-invalid-block-shape")) {
        return false;
    }
    cases += 3;

    const std::vector<std::uint8_t> oneByte = {0xff};
    BoundedBitReader reader(oneByte.data(), oneByte.size());
    std::uint32_t value = 0;
    if (!expect(!reader.read(9, &value) && reader.overrun(), "bitreader-overrun")) {
        return false;
    }
    ++cases;

    std::vector<std::uint8_t> wrapped = {
        0x01, 0x10, 0x00, 0x01, 0x12, 0x34, 0x56, 0x78,
        0xde, 0xad, 0xbe, 0xef, 0x00, 0x08, 0x80, 0x00,
    };
    append(wrapped, syntheticFrame(0, 0, 0, 3));
    if (!expect(normalizeEb3Framing(wrapped).ok,
                "wrapper-valid-opaque-header")) {
        return false;
    }
    auto badMagic = wrapped;
    badMagic[0] = 0;
    const Eb3FramingResult badMagicResult = normalizeEb3Framing(badMagic);
    auto badTail = wrapped;
    badTail[12] = 1;
    const Eb3FramingResult badTailResult = normalizeEb3Framing(badTail);
    auto missingSync = wrapped;
    missingSync[16] = 0;
    const Eb3FramingResult missingSyncResult = normalizeEb3Framing(missingSync);
    if (!expect(!badMagicResult.ok
                    && badMagicResult.reason == "eb3-wrapper-bad-magic"
                    && !badTailResult.ok
                    && badTailResult.reason == "eb3-wrapper-bad-tail"
                    && !missingSyncResult.ok
                    && missingSyncResult.reason
                           == "eb3-wrapper-missing-syncword",
                "wrapper-negative-fields")) {
        return false;
    }
    auto payloadPseudoSync = syntheticFrame(0, 0, 0, 3);
    payloadPseudoSync[8] = 0x0b;
    payloadPseudoSync[9] = 0x77;
    auto rawTail = payloadPseudoSync;
    rawTail.push_back(0x99);
    if (!expect(strictFrameSequence(payloadPseudoSync)
                    && !strictFrameSequence(rawTail),
                "strict-payload-sync-and-tail")) {
        return false;
    }
    ++cases;

    AccessUnitAssembler flow;
    const ParseResult sixFrame = parseSyncframe(syntheticFrame(0, 0, 0, 3), 0);
    if (!expect(acceptedProcess(flow.process(sixFrame)),
                "flush-base-process")) {
        return false;
    }
    if (!expect(flow.flush().completed.has_value(), "flush-success")) {
        return false;
    }
    const ProcessResult repeatFlush = flow.flush();
    const ProcessResult postFlush = flow.process(sixFrame);
    if (!expect(repeatFlush.flow == FlowStatus::AlreadyFlushed
                    && postFlush.flow == FlowStatus::AlreadyFlushed,
                "flush-once")) {
        return false;
    }
    flow.reset();
    const ProcessResult cancelResult = flow.cancel();
    const ProcessResult canceledProcess = flow.process(sixFrame);
    if (!expect(cancelResult.disposition == Disposition::Accepted
                    && cancelResult.flow == FlowStatus::Canceled
                    && canceledProcess.disposition == Disposition::Accepted
                    && canceledProcess.flow == FlowStatus::Canceled,
                "cancel-stops")) {
        return false;
    }
    flow.reset();
    if (!expect(acceptedProcess(flow.process(sixFrame))
                    && flow.flush().completed,
                "reset-fresh")) {
        return false;
    }
    ++cases;

    FrameHeader crcFrame;
    if (!validFrame(syntheticFrame(0, 0, 0, 3), &crcFrame, "crc-input")) {
        return false;
    }
    if (!expect(crcFrame.capabilities.frameBoundaryRangeChecked
                    && !crcFrame.capabilities.crcRangeChecked
                    && !crcFrame.capabilities.crcVerified,
                "crc-not-verified")) {
        return false;
    }
    ++cases;
    std::cout << "selfTest=PASS cases=" << cases
              << " crcVerified=NO implementation=native-eac3-core-gate8n-1a\n";
    return true;
}

struct Report {
    std::size_t frames = 0;
    std::size_t accessUnits = 0;
    std::size_t framedBytes = 0;
    std::size_t timelineSamples = 0;
    std::size_t dependent = 0;
    std::size_t additional = 0;
    std::map<unsigned, std::size_t> blocks;
    std::map<unsigned, std::size_t> rates;
    std::map<unsigned, std::size_t> types;
    std::map<unsigned, std::size_t> substreams;
};

void usage()
{
    std::cerr << "Usage: Eac3NativeCoreProbe <raw.eac3|raw.ec3|raw.eb3>"
                 " [--max-units N]\n"
                 "       Eac3NativeCoreProbe --self-test\n";
}

bool runFile(const std::string &path, std::size_t maxUnits)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::cerr << "probeResult=FAIL stage=open reason=file-open-failed path="
                  << path << '\n';
        return false;
    }
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)),
                                          std::istreambuf_iterator<char>());
    const Eb3FramingResult framing = normalizeEb3Framing(bytes);
    if (!framing.ok) {
        std::cerr << "probeResult=FAIL stage=" << toString(framing.stage)
                  << " reason=" << framing.reason << '\n';
        return false;
    }
    const std::vector<std::uint8_t> &framedBytes = framing.normalized;
    Report report;
    AccessUnitAssembler assembler;
    std::size_t offset = 0;
    bool stopped = false;
    while (offset < framedBytes.size()) {
        if (!eb3SyncAt(framedBytes, offset)) {
            std::cerr << "probeResult=FAIL stage=sequence reason="
                      << "non-syncframe-at-offset offset=" << offset << '\n';
            return false;
        }
        const ParseResult parsed = parseSyncframe(framedBytes, offset);
        if (parsed.disposition != Disposition::Accepted || !parsed.frame) {
            std::cerr << "probeResult=FAIL stage=" << toString(parsed.stage)
                      << " disposition=" << toString(parsed.disposition)
                      << " reason=" << parsed.reason << " offset=" << offset << '\n';
            return false;
        }
        const FrameHeader &frame = *parsed.frame;
        const ProcessResult result = assembler.process(parsed);
        if (result.flow != FlowStatus::None
            || result.disposition != Disposition::Accepted) {
            std::cerr << "probeResult=FAIL stage=" << toString(result.stage)
                      << " flow=" << toString(result.flow)
                      << " reason=" << result.reason << " offset=" << offset << '\n';
            return false;
        }
        // A completion is emitted when the first frame of the next repeated
        // stream key arrives.  Keep the --max-units evidence bounded to the
        // same complete-AU prefix as the legacy Gate 1 probe; do not count
        // that look-ahead frame in the selected prefix.
        if (result.completed && maxUnits != 0U
            && report.accessUnits + 1U >= maxUnits) {
            ++report.accessUnits;
            report.timelineSamples += result.completed->sampleCount;
            stopped = true;
            break;
        }
        ++report.frames;
        report.framedBytes += frame.sizeBytes;
        ++report.blocks[frame.blocks];
        ++report.rates[frame.sampleRate];
        ++report.types[static_cast<unsigned>(frame.streamType)];
        ++report.substreams[frame.substreamId];
        report.dependent += frame.dependent ? 1U : 0U;
        report.additional += frame.additional ? 1U : 0U;
        if (result.completed) {
            ++report.accessUnits;
            report.timelineSamples += result.completed->sampleCount;
        }
        offset += frame.sizeBytes;
    }
    if (!stopped) {
        const ProcessResult result = assembler.flush();
        if (result.disposition != Disposition::Accepted
            || result.flow != FlowStatus::None || !result.completed) {
            std::cerr << "probeResult=FAIL stage=" << toString(result.stage)
                      << " reason=" << result.reason << '\n';
            return false;
        }
        ++report.accessUnits;
        report.timelineSamples += result.completed->sampleCount;
    }
    std::cout << "inputBytes=" << bytes.size() << '\n'
              << "frames=" << report.frames << '\n'
              << "accessUnits=" << report.accessUnits << '\n'
              << "framedBytes=" << report.framedBytes << '\n'
              << "timelineSamples=" << report.timelineSamples << '\n'
              << "eb3WrapperCountTotal="
              << (framing.wrapped ? framing.wrapperCount : 0U) << '\n'
              << "eb3WrapperCountCovered="
              << (framing.wrapped
                      ? std::min(framing.wrapperCount, report.accessUnits)
                      : 0U)
              << '\n'
              << "dependentFrames=" << report.dependent << '\n'
              << "additionalFrames=" << report.additional << '\n'
              << "frameBoundaryRangeChecked=YES\n"
              << "crcRangeChecked=NO\n"
              << "crcVerified=NO\n"
              << "ffmpegLinked=NO\n";
    for (const auto &[blocks, count] : report.blocks) {
        std::cout << "blocks[" << blocks << "]=" << count << '\n';
    }
    for (const auto &[rate, count] : report.rates) {
        std::cout << "sampleRate[" << rate << "]=" << count << '\n';
    }
    for (const auto &[type, count] : report.types) {
        std::cout << "streamType[" << type << "]=" << count << '\n';
    }
    for (const auto &[sid, count] : report.substreams) {
        std::cout << "substreamId[" << sid << "]=" << count << '\n';
    }
    const bool pass = report.frames > 0U && report.accessUnits > 0U;
    std::cout << "probeResult=" << (pass ? "PASS" : "FAIL")
              << " stage=gate8n-1a-bounded-eac3-parser"
              << " implementation=native-eac3-core\n";
    return pass;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc == 2 && std::string(argv[1]) == "--self-test") {
        return runSelfTest() ? 0 : 1;
    }
    if (argc < 2 || argc > 4) {
        usage();
        return 2;
    }
    std::size_t maxUnits = 0;
    for (int i = 2; i < argc; ++i) {
        const std::string option = argv[i];
        if ((option == "--max-units" || option == "--max-frames")
            && i + 1 < argc) {
            try {
                maxUnits = static_cast<std::size_t>(std::stoull(argv[++i]));
            } catch (...) {
                usage();
                return 2;
            }
        } else {
            usage();
            return 2;
        }
    }
    return runFile(argv[1], maxUnits) ? 0 : 1;
}
