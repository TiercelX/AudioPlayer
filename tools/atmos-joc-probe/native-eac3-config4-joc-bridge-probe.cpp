// J0A4: probe-only config-4 association and JOC qualification gate.  J0A3
// remains the established config-3 core/session bridge.
//
// The supplied config-4 payloads are carried by the dependent SID0 frame,
// while the existing native J0A1/J0A2 extractor and J0A3 core bridge are
// config-3 independent-core contracts.  This probe therefore stops after
// proving the pair/payload association and returns an explicit blocked result
// rather than inventing a 10-channel-to-7-channel session mapping.

#include "native-eac3-bsi.h"
#include "native-eac3-core.h"
#include "native-eac3-config4-joc-bridge.h"
#include "native-eac3-joc-qualifier.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace config4joc {

using namespace eac3native;

constexpr unsigned kRate = 48000U;
constexpr unsigned kBlocks = 6U;
constexpr unsigned kSamples = 1536U;
constexpr unsigned kBaseChannels = 6U;
constexpr unsigned kDependentChannels = 4U;
constexpr std::uint16_t kChanmap = 0xA010U;
constexpr unsigned kJocConfig = 4U;
constexpr unsigned kJocChannels = 7U;

struct Result {
    Disposition disposition = Disposition::Malformed;
    std::string reason;
    unsigned auIndex = 0U;
    std::int64_t timestamp = 0;
    bool progressed = false;
};

using Callback = std::function<bool(const Association &)>;

const char *blockedReason()
{
    return "CONFIG4_JOC_BRIDGE_BLOCKED_SESSION_CORE_CONFIG3_ONLY";
}

std::vector<std::uint8_t> readFile(const std::string &path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return {};
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(stream), {});
}

NativeEmdfResult scanFrameEmdf(const std::vector<std::uint8_t> &bytes,
                               const FrameHeader &frame, unsigned auIndex)
{
    NativeEmdfResult result;
    result.auIndex = auIndex;
    result.frame = frame;
    result.frameStartByte = frame.offset;
    result.frameEndByte = frame.offset + frame.sizeBytes;
    result.frameStartBit = frame.offset * 8U;
    result.frameEndBit = frame.endBit;
    if (frame.endBit > bytes.size() * 8U) {
        result.reason = "config4-emdf-frame-bounds";
        return result;
    }
    std::size_t bit = result.frameStartBit;
    while (bit + 32U <= result.frameEndBit) {
        unsigned sync = 0U;
        if (eb3ReadBits(bytes, bit, 16U, &sync) && sync == 0x5838U) {
            NativeEmdfResult candidate = parseNativeEmdfContainer(
                bytes, bit, result.frameEndBit);
            if (candidate.disposition == EmdfDisposition::Accepted
                && candidate.containers.size() == 1U) {
                result.containers.push_back(std::move(candidate.containers.front()));
                bit = result.containers.back().endBit;
                continue;
            }
        }
        ++bit;
    }
    result.disposition = EmdfDisposition::Accepted;
    if (result.containers.empty()) result.reason = "config4-emdf-container-missing";
    return result;
}

std::size_t countPayloads(const NativeEmdfResult &emdf, unsigned payloadId)
{
    std::size_t count = 0U;
    for (const NativeEmdfContainer &container : emdf.containers)
        for (const NativeEmdfPayload &payload : container.payloads)
            if (payload.id == payloadId) ++count;
    return count;
}

std::size_t countTargetPayloads(const NativeEmdfResult &emdf)
{
    return countPayloads(emdf, 11U) + countPayloads(emdf, 14U);
}

bool associatePair(const std::vector<std::uint8_t> &bytes, std::size_t offset,
                   unsigned auIndex, const std::string &tablePath,
                   Association *association, std::string *reason)
{
    if (!association || !reason || offset >= bytes.size()) {
        if (reason) *reason = "config4-pair-offset-invalid";
        return false;
    }
    const ParseResult baseResult = parseSyncframe(bytes, offset);
    if (baseResult.disposition != eac3native::Disposition::Accepted
        || !baseResult.frame) {
        *reason = "config4-base-parse-" + baseResult.reason;
        return false;
    }
    const FrameHeader &base = *baseResult.frame;
    if (base.streamType != StreamType::LegacyAc3 || base.substreamId != 0U
        || base.sampleRate != kRate || base.blocks != kBlocks
        || base.sampleCount != kSamples || base.acmod != 7U || !base.lfe
        || base.channelCount != kBaseChannels) {
        *reason = "config4-base-topology-required";
        return false;
    }
    const NativeEmdfResult baseEmdf = scanFrameEmdf(bytes, base, auIndex);
    if (baseEmdf.disposition != EmdfDisposition::Accepted
        || !baseEmdf.containers.empty()
        || countTargetPayloads(baseEmdf) != 0U) {
        *reason = "config4-base-emdf-container-or-payload-forbidden";
        return false;
    }
    const std::size_t dependentOffset = offset + base.sizeBytes;
    const ParseResult dependentResult = parseSyncframe(bytes, dependentOffset);
    if (dependentResult.disposition != eac3native::Disposition::Accepted
        || !dependentResult.frame) {
        *reason = "config4-dependent-parse-" + dependentResult.reason;
        return false;
    }
    const FrameHeader &dependent = *dependentResult.frame;
    const BsiParseResult bsi = parseEac3Bsi(bytes, dependent);
    if (bsi.disposition != eac3native::Disposition::Accepted || !bsi.info) {
        *reason = "config4-dependent-bsi-parse";
        return false;
    }
    if (dependent.streamType != StreamType::Dependent
        || dependent.substreamId != 0U || dependent.sampleRate != kRate
        || dependent.blocks != kBlocks || dependent.sampleCount != kSamples
        || dependent.acmod != 5U || dependent.lfe
        || dependent.channelCount != kDependentChannels
        || !bsi.info->chanmape || bsi.info->chanmap != kChanmap
        || bsi.info->chanmapChannelWeight != kDependentChannels) {
        *reason = "config4-dependent-topology-required";
        return false;
    }
    const NativeEmdfResult emdf = scanFrameEmdf(bytes, dependent, auIndex);
    if (emdf.disposition != EmdfDisposition::Accepted
        || emdf.containers.size() != 1U) {
        *reason = emdf.containers.empty()
            ? "config4-dependent-emdf-container-missing"
            : "config4-dependent-emdf-container-duplicate";
        return false;
    }
    const std::size_t dependentTargetPayloads = countTargetPayloads(emdf);
    if (countPayloads(emdf, 11U) != 1U || countPayloads(emdf, 14U) != 1U) {
        *reason = "config4-dependent-payload11-or14-count";
        return false;
    }
    const NativeJocQualification qualification = qualifyNativeEac3Emdf(
        emdf, tablePath, auIndex,
        static_cast<std::int64_t>(auIndex) * static_cast<std::int64_t>(kSamples));
    if (qualification.disposition != JocQualificationDisposition::Qualified
        || qualification.payloadSources.size() != 2U
        || !qualification.hasJocReport || !qualification.jocAccepted
        || qualification.joc.downmixConfigIndex != kJocConfig
        || qualification.joc.numChannels != kJocChannels) {
        *reason = "config4-joc-qualification-"
            + (qualification.reason.empty() ? std::string("rejected")
                                             : qualification.reason);
        return false;
    }
    const auto findPayload = [&emdf](unsigned id) -> const NativeEmdfPayload * {
        if (emdf.containers.empty()) return nullptr;
        for (const NativeEmdfPayload &payload : emdf.containers.front().payloads)
            if (payload.id == id) return &payload;
        return nullptr;
    };
    const NativeEmdfPayload *payload11 = findPayload(11U);
    const NativeEmdfPayload *payload14 = findPayload(14U);
    if (!payload11 || !payload14) {
        *reason = "config4-joc-payload-source-missing";
        return false;
    }
    association->auIndex = auIndex;
    association->timestamp = static_cast<std::int64_t>(auIndex)
        * static_cast<std::int64_t>(kSamples);
    association->baseOffset = base.offset;
    association->dependentOffset = dependent.offset;
    association->nextOffset = dependent.offset + dependent.sizeBytes;
    association->payload11Bytes = payload11->bytes.size();
    association->payload14Bytes = payload14->bytes.size();
    association->payload11StartBit = payload11->dataStartBit;
    association->payload14StartBit = payload14->dataStartBit;
    association->baseAcceptedContainers = baseEmdf.containers.size();
    association->baseTargetPayloads = countTargetPayloads(baseEmdf);
    association->dependentAcceptedContainers = emdf.containers.size();
    association->dependentTargetPayloads = dependentTargetPayloads;
    association->dependentChannelCount = dependent.channelCount;
    association->dependentChanmap = bsi.info->chanmap;
    association->jocConfig = qualification.joc.downmixConfigIndex;
    association->jocChannels = qualification.joc.numChannels;
    association->qualification = qualification;
    return true;
}

class Config4JocBridgePreflight final {
public:
    explicit Config4JocBridgePreflight(std::string tablePath)
        : tablePath_(std::move(tablePath)) {}

    Result open()
    {
        reset();
        opened_ = true;
        return {Disposition::Accepted, "config4-preflight-open", 0U, 0, true};
    }

    Result process(const std::vector<std::uint8_t> &bytes, std::size_t *offset,
                   unsigned auIndex, const Callback &callback)
    {
        if (canceled_) return fail(Disposition::Canceled,
                                   "config4-preflight-canceled", false);
        if (poisoned_) return fail(Disposition::Malformed,
                                   "config4-preflight-poisoned-reset-required", false);
        if (!opened_) return fail(Disposition::Malformed,
                                  "config4-preflight-not-open", false);
        if (!offset || !callback) return fail(Disposition::Malformed,
                                              "config4-preflight-input-or-callback", true);
        Association association;
        std::string reason;
        if (!associatePair(bytes, *offset, auIndex, tablePath_, &association, &reason))
            return fail(Disposition::Malformed, reason, true);
        if (!callback(association))
            return fail(Disposition::Malformed, "config4-preflight-callback-rejected", true);
        *offset = association.nextOffset;
        ++processed_;
        return {Disposition::Blocked, blockedReason(), auIndex,
                association.timestamp, true};
    }

    Result flush(const Callback &callback)
    {
        if (canceled_) return fail(Disposition::Canceled,
                                   "config4-preflight-canceled", false);
        if (poisoned_) return fail(Disposition::Malformed,
                                   "config4-preflight-poisoned-reset-required", false);
        if (!opened_) return fail(Disposition::Malformed,
                                  "config4-preflight-not-open", false);
        if (flushed_) return fail(Disposition::Malformed,
                                  "config4-preflight-flushed", false);
        if (!callback) return fail(Disposition::Malformed,
                                   "config4-preflight-callback-missing", true);
        flushed_ = true;
        return {Disposition::Blocked, blockedReason(),
                static_cast<unsigned>(processed_),
                static_cast<std::int64_t>(processed_ * kSamples), true};
    }

    void cancel() { canceled_ = true; }

    void reset()
    {
        opened_ = false;
        poisoned_ = false;
        canceled_ = false;
        flushed_ = false;
        processed_ = 0U;
    }

    bool poisoned() const { return poisoned_; }
    std::size_t processed() const { return processed_; }

private:
    Result fail(Disposition disposition, std::string reason, bool poison)
    {
        if (poison) poisoned_ = true;
        return {disposition, std::move(reason),
                static_cast<unsigned>(processed_),
                static_cast<std::int64_t>(processed_ * kSamples), false};
    }

    std::string tablePath_;
    bool opened_ = false;
    bool poisoned_ = false;
    bool canceled_ = false;
    bool flushed_ = false;
    std::size_t processed_ = 0U;
};

DecodeReport decodeFile(const std::string &path, const std::string &tablePath,
                        unsigned maxAUs, const AssociationCallback &callback)
{
    DecodeReport report;
    if (maxAUs == 0U || !callback) {
        report.reason = maxAUs == 0U ? "config4-preflight-max-aus-invalid"
                                     : "config4-preflight-callback-missing";
        return report;
    }
    const std::vector<std::uint8_t> input = readFile(path);
    const Eb3FramingResult framing = normalizeEb3Framing(input);
    if (!framing.ok || framing.normalized.empty()) {
        report.reason = "config4-preflight-framing-" + framing.reason;
        return report;
    }
    Config4JocBridgePreflight bridge(tablePath);
    if (bridge.open().disposition != Disposition::Accepted) {
        report.reason = "config4-preflight-open";
        return report;
    }
    std::size_t offset = 0U;
    for (; report.observedAUs < maxAUs; ++report.observedAUs) {
        const Result processed = bridge.process(
            framing.normalized, &offset, report.observedAUs, callback);
        if (processed.disposition != Disposition::Blocked) {
            report.disposition = processed.disposition;
            report.reason = processed.reason;
            return report;
        }
        ++report.callbacks;
    }
    report.disposition = Disposition::Accepted;
    report.reason = "config4-preflight-associations-accepted";
    return report;
}

bool selfTest(const std::vector<std::uint8_t> &bytes,
              const std::string &tablePath)
{
    std::size_t offset = 0U;
    Config4JocBridgePreflight bridge(tablePath);
    if (bridge.open().disposition != Disposition::Accepted) return false;
    const Result rejected = bridge.process(bytes, &offset, 0U,
                                           [](const Association &) { return false; });
    if (rejected.disposition != Disposition::Malformed || !bridge.poisoned()) return false;
    if (bridge.flush([](const Association &) { return true; }).reason
            != "config4-preflight-poisoned-reset-required") return false;
    bridge.reset();
    if (bridge.open().disposition != Disposition::Accepted) return false;
    std::size_t acceptedCallbacks = 0U;
    const ParseResult baseParsed = parseSyncframe(bytes, 0U);
    const std::size_t dependentOffset = baseParsed.frame
        ? baseParsed.frame->sizeBytes : 0U;
    const ParseResult dependentParsed = parseSyncframe(bytes, dependentOffset);
    const NativeEmdfResult baseEmdf = baseParsed.frame
        ? scanFrameEmdf(bytes, *baseParsed.frame, 0U) : NativeEmdfResult{};
    const NativeEmdfResult dependentEmdf = dependentParsed.frame
        ? scanFrameEmdf(bytes, *dependentParsed.frame, 0U)
        : NativeEmdfResult{};
    const bool carrierEvidence = baseEmdf.disposition == EmdfDisposition::Accepted
        && baseEmdf.containers.empty()
        && countTargetPayloads(baseEmdf) == 0U
        && dependentEmdf.disposition == EmdfDisposition::Accepted
        && dependentEmdf.containers.size() == 1U
        && countTargetPayloads(dependentEmdf) == 2U;
    NativeEmdfResult missingDependent = dependentEmdf;
    missingDependent.containers.clear();
    NativeEmdfResult duplicateDependent = dependentEmdf;
    if (!duplicateDependent.containers.empty())
        duplicateDependent.containers.push_back(
            duplicateDependent.containers.front());
    const NativeJocQualification missingQualification = qualifyNativeEac3Emdf(
        missingDependent, tablePath, 0U, 0);
    const NativeJocQualification duplicateQualification = qualifyNativeEac3Emdf(
        duplicateDependent, tablePath, 0U, 0);
    const bool containerRejects = missingQualification.reason
            == "joc-qualifier-emdf-container-missing"
        && duplicateQualification.reason
            == "joc-qualifier-emdf-container-duplicate";
    const Result blocked = bridge.process(
        bytes, &offset, 0U, [&acceptedCallbacks](const Association &value) {
            ++acceptedCallbacks;
            return value.auIndex == 0U && value.timestamp == 0
                && value.payload11Bytes == 67U && value.payload14Bytes == 216U
                && value.jocConfig == kJocConfig
                && value.jocChannels == kJocChannels
                && value.baseAcceptedContainers == 0U
                && value.baseTargetPayloads == 0U
                && value.dependentAcceptedContainers == 1U
                && value.dependentTargetPayloads == 2U
                && value.dependentOffset > value.baseOffset;
        });
    if (blocked.disposition != Disposition::Blocked
        || blocked.reason != blockedReason() || acceptedCallbacks != 1U
        || !carrierEvidence || !containerRejects) return false;
    if (bridge.flush([](const Association &) { return true; }).disposition
            != Disposition::Blocked) return false;
    bridge.reset();
    if (bridge.open().disposition != Disposition::Accepted) return false;
    bridge.cancel();
    std::size_t canceledOffset = 0U;
    if (bridge.process(bytes, &canceledOffset, 0U,
                       [](const Association &) { return true; }).disposition
            != Disposition::Canceled) return false;
    bridge.reset();
    return bridge.open().disposition == Disposition::Accepted;
}

bool runFile(const std::string &path, const std::string &tablePath,
             std::size_t maxAUs, bool runSelfTest)
{
    const std::vector<std::uint8_t> input = readFile(path);
    const Eb3FramingResult framing = normalizeEb3Framing(input);
    if (!framing.ok || framing.normalized.empty()) {
        std::cerr << "input=FAIL reason=" << framing.reason << '\n';
        return false;
    }
    if (runSelfTest && !selfTest(framing.normalized, tablePath)) {
        std::cerr << "bridgeSelfTest=FAIL\n";
        return false;
    }
    if (runSelfTest) {
        std::cout << "bridgeSelfTest=PASS cases=8 callbackReject=YES"
                     " poisonedFlush=YES carrierEvidence=YES"
                     " containerRejects=YES blockedQualification=YES"
                     " reset=YES cancel=YES\n";
    }
    Config4JocBridgePreflight bridge(tablePath);
    if (bridge.open().disposition != Disposition::Accepted) return false;
    std::size_t offset = 0U;
    std::size_t callbacks = 0U;
    bool associationPass = true;
    for (std::size_t au = 0U; au < maxAUs; ++au) {
        const Result result = bridge.process(
            framing.normalized, &offset, static_cast<unsigned>(au),
            [&callbacks, &associationPass](const Association &value) {
                ++callbacks;
                associationPass = associationPass
                    && value.auIndex == callbacks - 1U
                    && value.timestamp == static_cast<std::int64_t>(value.auIndex * kSamples)
                    && value.payload11Bytes == 67U
                    && value.payload14Bytes == 216U
                    && value.qualification.frame.streamType == StreamType::Dependent
                    && value.qualification.frame.substreamId == 0U
                    && value.jocConfig == kJocConfig
                    && value.jocChannels == kJocChannels
                    && value.baseTargetPayloads == 0U
                    && value.dependentAcceptedContainers == 1U
                    && value.dependentTargetPayloads == 2U;
                std::cout << "au=" << value.auIndex
                          << " baseOffset=" << value.baseOffset
                          << " dependentOffset=" << value.dependentOffset
                          << " payloadCarrier=dependent/sid0"
                          << " payload11Bytes=" << value.payload11Bytes
                          << " payload14Bytes=" << value.payload14Bytes
                          << " payload11DataBit=" << value.payload11StartBit
                          << " payload14DataBit=" << value.payload14StartBit
                          << " carrierEvidence=BASE_NONE_DEPENDENT_ONE"
                          << " jocConfig=" << value.jocConfig
                          << " jocChannels=" << value.jocChannels << '\n';
                return true;
            });
        if (result.disposition != Disposition::Blocked) {
            std::cerr << "firstFailure=au" << result.auIndex
                      << " reason=" << result.reason << '\n';
            return false;
        }
    }
    const Result flushed = bridge.flush([](const Association &) { return true; });
    const bool pass = associationPass && callbacks == maxAUs
        && flushed.disposition == Disposition::Blocked
        && flushed.reason == blockedReason();
    std::cout << "associatedAUs=" << callbacks << '\n'
              << "payloadCarrier=dependent/sid0\n"
              << "carrierEvidence=BASE_NONE_DEPENDENT_ONE\n"
              << "jocQualification=config4-channels7-PASS\n"
              << "n5jPcmInput=NOT_CONSUMED\n"
              << "sessionInput=NOT_ENTERED\n"
              << "bridgeResult=" << (pass ? "BLOCKED" : "FAIL")
              << " reason=" << (pass ? blockedReason() : "association-failed") << '\n'
              << "drcApplied=NO rendererApplied=NO ffmpegLinked=NO\n";
    return pass;
}

} // namespace config4joc

#ifndef EAC3_CONFIG4_JOC_BRIDGE_NO_MAIN
int main(int argc, char **argv)
{
    if (argc < 2 || argc > 8) {
        std::cerr << "Usage: Eac3NativeConfig4JocBridgeProbe <raw.eb3>"
                     " [--joc-table path] [--max-aus N] [--self-test]\n";
        return 2;
    }
    std::string tablePath = "docs/dev/ts_103420_tables.c";
    std::size_t maxAUs = 3U;
    bool selfTest = false;
    for (int index = 2; index < argc; ++index) {
        const std::string option = argv[index];
        if (option == "--joc-table" && index + 1 < argc) {
            tablePath = argv[++index];
        } else if (option == "--max-aus" && index + 1 < argc) {
            maxAUs = static_cast<std::size_t>(std::stoull(argv[++index]));
        } else if (option == "--self-test") {
            selfTest = true;
        } else {
            return 2;
        }
    }
    if (maxAUs == 0U) return 2;
    return config4joc::runFile(argv[1], tablePath, maxAUs, selfTest) ? 0 : 1;
}
#endif
