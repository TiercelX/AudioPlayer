// Gate 8N-5c: supplied config-4 Legacy AC-3 core plus immediately-following
// dependent E-AC-3 acceptance.  This is a native, FFmpeg-free association
// seam.  Legacy AC-3 coefficient/IMDCT decoding remains explicitly absent;
// accepted units therefore carry stable channel identity and a
// PCM_UNAVAILABLE boundary rather than fabricated samples.

#include "native-eac3-bsi.h"
#include "native-eac3-core.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace eac3config4 {
using namespace eac3native;

enum class Config4Disposition {
    Accepted,
    Unsupported,
    Malformed,
};

enum class Flow {
    None,
    Canceled,
    AlreadyFlushed,
};

enum class PcmAvailability {
    UnavailableLegacyAc3,
};

struct Substream {
    StreamType type = StreamType::Reserved;
    unsigned substreamId = 0U;
    unsigned sampleRate = 0U;
    unsigned blocks = 0U;
    unsigned sampleCount = 0U;
    unsigned channelCount = 0U;
    unsigned acmod = 0U;
    bool lfe = false;
    bool chanmape = false;
    std::uint16_t chanmap = 0U;
    unsigned chanmapWeight = 0U;
};

struct ChannelIdentity {
    unsigned index = 0U;
    std::string id;
    std::string location;
    bool lfe = false;
    StreamType sourceType = StreamType::Reserved;
    unsigned sourceChannel = 0U;
};

struct AccessUnit {
    std::size_t sampleStart = 0U;
    std::size_t sampleCount = 0U;
    unsigned sampleRate = 0U;
    std::vector<Substream> substreams;
    std::vector<ChannelIdentity> channels;
    PcmAvailability pcm = PcmAvailability::UnavailableLegacyAc3;
    bool drcApplied = false;
};

using AccessUnitCallback = std::function<bool(const AccessUnit &)>;

struct Result {
    Config4Disposition disposition = Config4Disposition::Malformed;
    Flow flow = Flow::None;
    std::string reason;
    bool accepted = false;
    bool poisoned = false;
    std::optional<AccessUnit> unit;
};

struct FlushResult {
    bool ok = false;
    Flow flow = Flow::None;
    std::string reason;
    bool emittedUnit = false;
};

namespace {

constexpr unsigned kSampleRate = 48000U;
constexpr unsigned kBlocks = 6U;
constexpr unsigned kSamplesPerAu = 1536U;
constexpr unsigned kBaseChannels = 6U;
constexpr unsigned kDependentChannels = 4U;
constexpr std::uint16_t kSuppliedChanmap = 0xA010U;

const std::array<const char *, kBaseChannels> kBaseLocations = {
    "FL", "FC", "FR", "SL", "SR", "LFE"};
const std::array<const char *, kDependentChannels> kDependentLocations = {
    "L", "R", "VHL", "VHR"};

bool frameShape(const FrameHeader &frame)
{
    return frame.sampleRate == kSampleRate && frame.blocks == kBlocks
        && frame.sampleCount == kSamplesPerAu;
}

Substream describe(const FrameHeader &frame, const BsiInfo &bsi)
{
    Substream value;
    value.type = frame.streamType;
    value.substreamId = frame.substreamId;
    value.sampleRate = frame.sampleRate;
    value.blocks = frame.blocks;
    value.sampleCount = frame.sampleCount;
    value.channelCount = frame.channelCount;
    value.acmod = frame.acmod;
    value.lfe = frame.lfe;
    value.chanmape = bsi.chanmape;
    value.chanmap = bsi.chanmap;
    value.chanmapWeight = bsi.chanmapChannelWeight;
    return value;
}

bool validBase(const FrameHeader &frame, std::string *reason)
{
    if (frame.streamType != StreamType::LegacyAc3
        || frame.substreamId != 0U) {
        if (reason) *reason = "config4-requires-legacy-ac3-sid0-base";
        return false;
    }
    if (!frameShape(frame) || frame.acmod != 7U || !frame.lfe
        || frame.channelCount != kBaseChannels) {
        if (reason) *reason = "config4-legacy-base-topology-required";
        return false;
    }
    return true;
}

bool validDependent(const FrameHeader &frame, const BsiInfo &bsi,
                    std::string *reason)
{
    if (frame.streamType != StreamType::Dependent
        || frame.substreamId != 0U) {
        if (reason) *reason = "config4-requires-dependent-eac3-sid0";
        return false;
    }
    if (!frameShape(frame) || frame.acmod != 5U || frame.lfe
        || frame.channelCount != kDependentChannels) {
        if (reason) *reason = "config4-dependent-topology-required";
        return false;
    }
    if (!bsi.bsiParsed || !bsi.chanmape
        || bsi.chanmap != kSuppliedChanmap
        || bsi.chanmapChannelWeight != kDependentChannels) {
        if (reason) *reason = "config4-dependent-chanmap-a010-required";
        return false;
    }
    return true;
}

} // namespace

class NativeConfig4CoreSubstreamAdapter final {
public:
    Result open()
    {
        reset();
        opened_ = true;
        Result result;
        result.disposition = Config4Disposition::Accepted;
        result.accepted = true;
        result.reason = "config4-core-substream-open";
        return result;
    }

    Result pushFrame(const std::vector<std::uint8_t> &packet,
                     const AccessUnitCallback &callback)
    {
        if (canceled_) return failure(Config4Disposition::Malformed, Flow::Canceled,
                                      "config4-adapter-canceled", false);
        if (!opened_) return failure(Config4Disposition::Malformed, Flow::None,
                                     "config4-adapter-not-open", false);
        if (poisoned_) return failure(Config4Disposition::Malformed, Flow::None,
                                      "config4-adapter-poisoned-reset-required",
                                      false);
        if (flushed_) return failure(Config4Disposition::Malformed,
                                     Flow::AlreadyFlushed,
                                     "config4-frame-after-eos-reset-required",
                                     false);
        if (!callback) return failure(Config4Disposition::Malformed, Flow::None,
                                      "config4-callback-missing");
        if (packet.empty()) return failure(Config4Disposition::Malformed, Flow::None,
                                           "config4-empty-frame");

        const ParseResult parsed = parseSyncframe(packet, 0U);
        if (parsed.disposition != eac3native::Disposition::Accepted || !parsed.frame) {
            return failure(parsed.disposition == eac3native::Disposition::Unsupported
                               ? Config4Disposition::Unsupported
                               : Config4Disposition::Malformed,
                           Flow::None,
                           "config4-frame-" + parsed.reason);
        }
        const FrameHeader &frame = *parsed.frame;
        if (frame.sizeBytes != packet.size()) {
            return failure(Config4Disposition::Malformed, Flow::None,
                           "config4-frame-must-be-complete");
        }
        const BsiParseResult bsi = parseEac3Bsi(packet, frame);
        if (bsi.disposition != eac3native::Disposition::Accepted || !bsi.info) {
            return failure(bsi.disposition == eac3native::Disposition::Unsupported
                               ? Config4Disposition::Unsupported
                               : Config4Disposition::Malformed,
                           Flow::None, "config4-bsi-" + bsi.reason);
        }

        std::string reason;
        if (!pendingBase_) {
            if (!validBase(frame, &reason)) {
                return failure(Config4Disposition::Unsupported, Flow::None, reason);
            }
            pendingBase_ = SubstreamRecord{frame, *bsi.info};
            return accepted("config4-legacy-base-accepted-pending-dependent");
        }

        if (!validDependent(frame, *bsi.info, &reason)) {
            return failure(Config4Disposition::Unsupported, Flow::None, reason);
        }
        AccessUnit unit = makeUnit(*pendingBase_, SubstreamRecord{frame, *bsi.info});
        if (!callback(unit)) {
            return failure(Config4Disposition::Malformed, Flow::None,
                           "config4-callback-rejected");
        }
        pendingBase_.reset();
        ++unitsProcessed_;
        Result result = accepted("config4-core-substream-au-accepted");
        result.unit = std::move(unit);
        return result;
    }

    FlushResult flush()
    {
        if (canceled_) return FlushResult{false, Flow::Canceled,
                                          "config4-adapter-canceled", false};
        if (!opened_) return FlushResult{false, Flow::None,
                                         "config4-adapter-not-open", false};
        if (poisoned_) return FlushResult{false, Flow::None,
                                           "config4-adapter-poisoned-reset-required",
                                           false};
        if (flushed_) return FlushResult{true, Flow::AlreadyFlushed,
                                          "config4-adapter-eos-already-flushed",
                                          false};
        if (pendingBase_) {
            poisoned_ = true;
            return FlushResult{false, Flow::None,
                               "config4-incomplete-base-without-dependent", false};
        }
        flushed_ = true;
        return FlushResult{true, Flow::None, "config4-adapter-eos", false};
    }

    void reset()
    {
        pendingBase_.reset();
        opened_ = false;
        poisoned_ = false;
        canceled_ = false;
        flushed_ = false;
        unitsProcessed_ = 0U;
    }

    void cancel()
    {
        if (!canceled_) {
            canceled_ = true;
            pendingBase_.reset();
        }
    }

    bool poisoned() const { return poisoned_; }
    std::size_t unitsProcessed() const { return unitsProcessed_; }

private:
    struct SubstreamRecord {
        FrameHeader frame;
        BsiInfo bsi;
    };

    Result accepted(const char *reason) const
    {
        Result result;
        result.disposition = Config4Disposition::Accepted;
        result.accepted = true;
        result.reason = reason;
        result.poisoned = poisoned_;
        return result;
    }

    Result failure(Config4Disposition disposition, Flow flow, std::string reason,
                   bool poison = true)
    {
        if (poison) poisoned_ = true;
        Result result;
        result.disposition = disposition;
        result.flow = flow;
        result.reason = std::move(reason);
        result.poisoned = poisoned_;
        return result;
    }

    AccessUnit makeUnit(const SubstreamRecord &base,
                        const SubstreamRecord &dependent) const
    {
        AccessUnit unit;
        unit.sampleStart = unitsProcessed_ * kSamplesPerAu;
        unit.sampleCount = kSamplesPerAu;
        unit.sampleRate = kSampleRate;
        unit.substreams = {describe(base.frame, base.bsi),
                           describe(dependent.frame, dependent.bsi)};
        unit.channels.reserve(kBaseChannels + kDependentChannels);
        for (unsigned index = 0U; index < kBaseChannels; ++index) {
            unit.channels.push_back(ChannelIdentity{
                index, std::string("base.") + kBaseLocations[index],
                kBaseLocations[index], index == kBaseChannels - 1U,
                StreamType::LegacyAc3, index});
        }
        for (unsigned index = 0U; index < kDependentChannels; ++index) {
            unit.channels.push_back(ChannelIdentity{
                kBaseChannels + index,
                std::string("dependent.") + kDependentLocations[index],
                kDependentLocations[index], false, StreamType::Dependent,
                index});
        }
        unit.pcm = PcmAvailability::UnavailableLegacyAc3;
        unit.drcApplied = false;
        return unit;
    }

    std::optional<SubstreamRecord> pendingBase_;
    bool opened_ = false;
    bool poisoned_ = false;
    bool canceled_ = false;
    bool flushed_ = false;
    std::size_t unitsProcessed_ = 0U;
};

const char *toString(Config4Disposition value)
{
    switch (value) {
    case Config4Disposition::Accepted: return "accepted";
    case Config4Disposition::Unsupported: return "unsupported";
    case Config4Disposition::Malformed: return "malformed";
    }
    return "unknown";
}

const char *toString(Flow value)
{
    switch (value) {
    case Flow::None: return "none";
    case Flow::Canceled: return "canceled";
    case Flow::AlreadyFlushed: return "already-flushed";
    }
    return "unknown";
}

namespace {

void setBits(std::vector<std::uint8_t> &bytes, std::size_t bit,
             unsigned count, unsigned value)
{
    for (unsigned index = 0U; index < count; ++index) {
        const std::size_t position = bit + index;
        const std::uint8_t mask = static_cast<std::uint8_t>(
            1U << (7U - (position % 8U)));
        if (((value >> (count - index - 1U)) & 1U) != 0U)
            bytes[position / 8U] |= mask;
        else
            bytes[position / 8U] &= static_cast<std::uint8_t>(~mask);
    }
}

class BitWriter {
public:
    BitWriter(std::vector<std::uint8_t> *bytes, std::size_t bit)
        : bytes_(bytes), bit_(bit) {}

    void put(unsigned count, unsigned value)
    {
        setBits(*bytes_, bit_, count, value);
        bit_ += count;
    }

private:
    std::vector<std::uint8_t> *bytes_;
    std::size_t bit_;
};

std::vector<std::uint8_t> syntheticLegacyBase()
{
    // frmsizecod=36/fscod=0 is the supplied base's 2560-byte frame shape.
    std::vector<std::uint8_t> bytes(2560U, 0U);
    bytes[0] = 0x0b;
    bytes[1] = 0x77;
    setBits(bytes, 32U, 2U, 0U); // fscod=48 kHz
    setBits(bytes, 34U, 6U, 36U);
    setBits(bytes, 40U, 5U, 6U); // legacy AC-3 bsid
    setBits(bytes, 45U, 3U, 0U); // bsmod
    setBits(bytes, 48U, 3U, 7U); // 3/2
    setBits(bytes, 51U, 2U, 0U); // cmixlev
    setBits(bytes, 53U, 2U, 0U); // surmixlev
    setBits(bytes, 55U, 1U, 1U); // lfeon
    return bytes;
}

std::vector<std::uint8_t> syntheticDependent()
{
    std::vector<std::uint8_t> bytes(4096U, 0U);
    bytes[0] = 0x0b;
    bytes[1] = 0x77;
    setBits(bytes, 16U, 2U, 1U); // dependent
    setBits(bytes, 18U, 3U, 0U); // SID0
    setBits(bytes, 21U, 11U, 2047U); // 4096-byte frame
    setBits(bytes, 32U, 2U, 0U); // fscod=48 kHz
    setBits(bytes, 34U, 2U, 3U); // six blocks
    setBits(bytes, 36U, 3U, 5U); // 3/2 coded channels
    setBits(bytes, 39U, 1U, 0U); // no LFE
    setBits(bytes, 40U, 5U, 16U);
    BitWriter writer(&bytes, 45U);
    writer.put(5U, 22U); // dialnorm
    writer.put(1U, 0U); // compre
    writer.put(1U, 1U); // chanmape
    writer.put(16U, kSuppliedChanmap);
    writer.put(1U, 0U); // mixmdate
    writer.put(1U, 0U); // infomdate
    writer.put(1U, 0U); // addbsie
    return bytes;
}

bool expect(bool value, const char *name)
{
    if (!value) std::cerr << "selfTest=FAIL case=" << name << '\n';
    return value;
}

bool validUnit(const AccessUnit &unit)
{
    if (unit.sampleCount != kSamplesPerAu || unit.sampleRate != kSampleRate
        || unit.substreams.size() != 2U
        || unit.channels.size() != kBaseChannels + kDependentChannels
        || unit.pcm != PcmAvailability::UnavailableLegacyAc3
        || unit.drcApplied) {
        return false;
    }
    const Substream &base = unit.substreams[0];
    const Substream &dependent = unit.substreams[1];
    if (base.type != StreamType::LegacyAc3 || base.substreamId != 0U
        || dependent.type != StreamType::Dependent
        || dependent.substreamId != 0U || !dependent.chanmape
        || dependent.chanmap != kSuppliedChanmap
        || dependent.chanmapWeight != kDependentChannels) {
        return false;
    }
    for (unsigned index = 0U; index < kBaseChannels; ++index) {
        const ChannelIdentity &channel = unit.channels[index];
        if (channel.index != index
            || channel.id != std::string("base.") + kBaseLocations[index]
            || channel.location != kBaseLocations[index]
            || channel.sourceType != StreamType::LegacyAc3
            || channel.sourceChannel != index
            || channel.lfe != (index == kBaseChannels - 1U)) {
            return false;
        }
    }
    for (unsigned index = 0U; index < kDependentChannels; ++index) {
        const ChannelIdentity &channel = unit.channels[kBaseChannels + index];
        if (channel.index != kBaseChannels + index
            || channel.id != std::string("dependent.")
                                  + kDependentLocations[index]
            || channel.location != kDependentLocations[index]
            || channel.sourceType != StreamType::Dependent
            || channel.sourceChannel != index || channel.lfe) {
            return false;
        }
    }
    return true;
}

bool runSelfTest()
{
    const std::vector<std::uint8_t> base = syntheticLegacyBase();
    const std::vector<std::uint8_t> dependent = syntheticDependent();
    std::size_t cases = 0U;
    NativeConfig4CoreSubstreamAdapter adapter;
    const Result opened = adapter.open();
    bool callbackCalled = false;
    const Result baseResult = adapter.pushFrame(
        base, [&](const AccessUnit &) { callbackCalled = true; return true; });
    const Result dependentResult = adapter.pushFrame(
        dependent, [&](const AccessUnit &unit) {
            callbackCalled = true;
            return validUnit(unit);
        });
    const FlushResult flushed = adapter.flush();
    if (!expect(opened.accepted && baseResult.accepted
                    && baseResult.reason
                        == "config4-legacy-base-accepted-pending-dependent"
                    && dependentResult.accepted && dependentResult.unit
                    && callbackCalled && flushed.ok && adapter.unitsProcessed() == 1U,
                "valid-legacy-base-dependent")) {
        return false;
    }
    ++cases;

    NativeConfig4CoreSubstreamAdapter orphan;
    orphan.open();
    const Result orphanResult = orphan.pushFrame(
        dependent, [](const AccessUnit &) { return true; });
    if (!expect(orphanResult.disposition == Config4Disposition::Unsupported
                    && orphanResult.reason
                        == "config4-requires-legacy-ac3-sid0-base"
                    && orphan.poisoned(),
                "dependent-before-base-fail-closed")) {
        return false;
    }
    ++cases;

    NativeConfig4CoreSubstreamAdapter incomplete;
    incomplete.open();
    if (!expect(incomplete.pushFrame(
                    base, [](const AccessUnit &) { return true; })
                        .accepted
                    && !incomplete.flush().ok
                    && incomplete.poisoned(),
                "incomplete-base-flush")) {
        return false;
    }
    ++cases;

    NativeConfig4CoreSubstreamAdapter rejected;
    rejected.open();
    rejected.pushFrame(base, [](const AccessUnit &) { return true; });
    const Result callbackRejected = rejected.pushFrame(
        dependent, [](const AccessUnit &) { return false; });
    if (!expect(callbackRejected.reason == "config4-callback-rejected"
                    && rejected.poisoned(),
                "callback-backpressure-poison")) {
        return false;
    }
    ++cases;

    std::vector<std::uint8_t> wrongMap = dependent;
    setBits(wrongMap, 52U, 16U, 0xA020U);
    NativeConfig4CoreSubstreamAdapter wrongMapAdapter;
    wrongMapAdapter.open();
    wrongMapAdapter.pushFrame(base, [](const AccessUnit &) { return true; });
    const Result wrongMapResult = wrongMapAdapter.pushFrame(
        wrongMap, [](const AccessUnit &) { return true; });
    if (!expect(wrongMapResult.disposition == Config4Disposition::Unsupported
                    && wrongMapResult.reason
                        == "config4-dependent-chanmap-a010-required"
                    && wrongMapAdapter.poisoned(),
                "wrong-dependent-chanmap-fail-closed")) {
        return false;
    }
    ++cases;

    std::vector<std::uint8_t> wrongTopology = dependent;
    setBits(wrongTopology, 36U, 3U, 4U);
    setBits(wrongTopology, 52U, 16U, 0x8010U);
    NativeConfig4CoreSubstreamAdapter wrongTopologyAdapter;
    wrongTopologyAdapter.open();
    wrongTopologyAdapter.pushFrame(base, [](const AccessUnit &) { return true; });
    const Result wrongTopologyResult = wrongTopologyAdapter.pushFrame(
        wrongTopology, [](const AccessUnit &) { return true; });
    if (!expect(wrongTopologyResult.disposition == Config4Disposition::Unsupported
                    && wrongTopologyResult.reason
                        == "config4-dependent-topology-required"
                    && wrongTopologyAdapter.poisoned(),
                "wrong-dependent-topology-fail-closed")) {
        return false;
    }
    ++cases;

    NativeConfig4CoreSubstreamAdapter canceled;
    canceled.open();
    canceled.cancel();
    const Result canceledPush = canceled.pushFrame(
        base, [](const AccessUnit &) { return true; });
    const FlushResult canceledFlush = canceled.flush();
    if (!expect(canceledPush.flow == Flow::Canceled
                    && canceledFlush.flow == Flow::Canceled
                    && !canceledFlush.ok,
                "cancel-push-flush")) {
        return false;
    }
    ++cases;

    NativeConfig4CoreSubstreamAdapter eos;
    eos.open();
    const FlushResult eosResult = eos.flush();
    const Result afterEos = eos.pushFrame(
        base, [](const AccessUnit &) { return true; });
    if (!expect(eosResult.ok && afterEos.flow == Flow::AlreadyFlushed
                    && afterEos.reason
                        == "config4-frame-after-eos-reset-required",
                "push-after-successful-flush")) {
        return false;
    }
    ++cases;

    NativeConfig4CoreSubstreamAdapter missingCallback;
    missingCallback.open();
    const Result missingCallbackResult = missingCallback.pushFrame(
        base, AccessUnitCallback{});
    if (!expect(missingCallbackResult.disposition == Config4Disposition::Malformed
                    && missingCallbackResult.reason == "config4-callback-missing"
                    && missingCallback.poisoned(),
                "missing-callback-fail-closed")) {
        return false;
    }
    ++cases;

    adapter.reset();
    const Result reopened = adapter.open();
    if (!expect(reopened.accepted && !adapter.poisoned(), "reset-reopen"))
        return false;
    ++cases;
    std::cout << "selfTest=PASS cases=" << cases
              << " callback=PASS stableIdentity=PASS"
              << " pcm=UNAVAILABLE_LEGACY_AC3 ffmpegLinked=NO\n";
    return true;
}

bool validReportedUnit(const AccessUnit &unit, std::size_t expectedStart)
{
    return unit.sampleStart == expectedStart && validUnit(unit);
}

bool runFile(const std::string &path, std::size_t maxUnits)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::cerr << "probeResult=FAIL stage=open reason=file-open-failed path="
                  << path << '\n';
        return false;
    }
    const std::vector<std::uint8_t> bytes(
        (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const Eb3FramingResult framing = normalizeEb3Framing(bytes);
    if (!framing.ok) {
        std::cerr << "probeResult=FAIL stage=" << toString(framing.stage)
                  << " reason=" << framing.reason << '\n';
        return false;
    }

    NativeConfig4CoreSubstreamAdapter adapter;
    if (!adapter.open().accepted) {
        std::cerr << "probeResult=FAIL stage=open reason=adapter-open-failed\n";
        return false;
    }
    const std::vector<std::uint8_t> &framed = framing.normalized;
    std::size_t offset = 0U;
    std::size_t syncframes = 0U;
    std::size_t units = 0U;
    std::size_t callbackCount = 0U;
    bool identities = true;
    bool noPcm = true;
    bool timeline = true;
    std::string firstFailure;
    while (offset < framed.size()
           && (maxUnits == 0U || units < maxUnits)) {
        const ParseResult parsed = parseSyncframe(framed, offset);
        if (parsed.disposition != eac3native::Disposition::Accepted || !parsed.frame) {
            std::cerr << "probeResult=FAIL stage=" << toString(parsed.stage)
                      << " reason=" << parsed.reason << " frame=" << syncframes
                      << " bit=" << offset * 8U << '\n';
            return false;
        }
        const FrameHeader &frame = *parsed.frame;
        if (frame.sizeBytes > framed.size() - offset) {
            std::cerr << "probeResult=FAIL stage=bounds reason=truncated-frame"
                      << " frame=" << syncframes << '\n';
            return false;
        }
        const std::vector<std::uint8_t> packet(
            framed.begin() + static_cast<std::ptrdiff_t>(offset),
            framed.begin() + static_cast<std::ptrdiff_t>(offset + frame.sizeBytes));
        const Result result = adapter.pushFrame(
            packet, [&](const AccessUnit &unit) {
                const bool valid = validReportedUnit(unit, units * kSamplesPerAu);
                ++callbackCount;
                identities = identities && valid && unit.channels.size() == 10U;
                noPcm = noPcm
                    && unit.pcm == PcmAvailability::UnavailableLegacyAc3;
                timeline = timeline && unit.sampleStart == units * kSamplesPerAu
                    && unit.sampleCount == kSamplesPerAu;
                if (!valid && firstFailure.empty()) {
                    firstFailure = "config4-unit-contract";
                    std::cerr << "unitContractFailure sampleStart="
                              << unit.sampleStart << " expectedStart="
                              << units * kSamplesPerAu
                              << " substreams=" << unit.substreams.size()
                              << " channels=" << unit.channels.size() << '\n';
                }
                return valid;
            });
        ++syncframes;
        if (result.disposition != Config4Disposition::Accepted) {
            std::cout << "firstFailureFrame=" << (syncframes - 1U)
                      << " firstFailureDisposition=" << toString(result.disposition)
                      << " firstFailureReason=" << result.reason << '\n'
                      << "acceptedUnits=" << units << '\n'
                      << "pcmAvailability=UNAVAILABLE_LEGACY_AC3\n"
                      << "probeResult=FAIL stage=gate8n-5c-config4-core-substream\n";
            return false;
        }
        if (result.unit) ++units;
        offset += frame.sizeBytes;
    }
    const FlushResult flushed = adapter.flush();
    const bool enough = maxUnits == 0U || units >= maxUnits;
    const bool callbackBoundary = callbackCount == units;
    const bool pass = flushed.ok && enough && units != 0U && callbackBoundary
        && identities && noPcm && timeline && firstFailure.empty();
    std::cout << "inputBytes=" << bytes.size() << '\n'
              << "normalizedBytes=" << framed.size() << '\n'
              << "eb3Wrapped=" << (framing.wrapped ? "YES" : "NO") << '\n'
              << "syncframes=" << syncframes << '\n'
              << "acceptedUnits=" << units << '\n'
              << "sampleCountPerUnit=" << kSamplesPerAu << '\n'
              << "channelIdentity=" << (identities ? "PASS" : "FAIL") << '\n'
              << "sampleTimeline=" << (timeline ? "PASS" : "FAIL") << '\n'
              << "callbackCount=" << callbackCount << '\n'
              << "callbackBoundary=" << (callbackBoundary ? "PASS" : "FAIL") << '\n'
              << "pcmAvailability=UNAVAILABLE_LEGACY_AC3\n"
              << "drcApplied=NO\nffmpegLinked=NO\n"
              << "probeResult=" << (pass ? "PASS" : "FAIL")
              << " stage=gate8n-5c-config4-core-substream"
              << " reason=legacy-ac3-pcm-decoder-not-in-slice\n";
    return pass;
}

void usage()
{
    std::cerr << "Usage: Eac3NativeConfig4CoreProbe <raw.eac3|raw.eb3>"
                 " [--max-units N]\n"
                 "       Eac3NativeConfig4CoreProbe --self-test\n";
}

} // namespace
} // namespace eac3config4

int main(int argc, char **argv)
{
    using namespace eac3config4;
    if (argc == 2 && std::string(argv[1]) == "--self-test")
        return runSelfTest() ? 0 : 1;
    if (argc < 2 || argc > 4) {
        usage();
        return 2;
    }
    std::size_t maxUnits = 10U;
    for (int index = 2; index < argc; ++index) {
        if (std::string(argv[index]) == "--max-units" && index + 1 < argc) {
            try {
                maxUnits = static_cast<std::size_t>(std::stoull(argv[++index]));
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
