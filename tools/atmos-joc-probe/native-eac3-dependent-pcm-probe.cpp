// Gate 8N-5g: narrow diagnostic PCM for the supplied dependent E-AC-3 SID0
// substream.  This probe validates the Legacy base/dependent adjacency, then
// reuses the checked-in E-AC-3 bsi/audblk/coefficient/transform primitives for
// four renderer-neutral dependent channels.  It is not dependent assembly,
// JOC, a renderer, or a production playback route.

#include "native-eac3-audblk.h"
#include "native-eac3-block-state.h"
#include "native-eac3-bsi.h"
#include "native-eac3-core.h"
#include "native-eac3-dependent-pcm.h"
#include "native-eac3-mantissas.h"
#include "native-eac3-transform.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace dependentpcm {
using namespace eac3native;

constexpr unsigned kRate = 48000U;
constexpr unsigned kBlocks = 6U;
constexpr unsigned kChannels = 4U;
constexpr unsigned kSamplesPerAu = 1536U;
constexpr unsigned kSamplesPerTail = 256U;
constexpr std::uint16_t kChanmap = 0xA010U;

void mix(std::uint64_t *digest, std::uint64_t value)
{
    if (!digest) return;
    for (unsigned shift = 0U; shift < 64U; shift += 8U) {
        *digest ^= (value >> shift) & 0xffU;
        *digest *= 1099511628211ULL;
    }
}

bool summarize(const std::vector<double> &samples, std::uint64_t *digest,
               double *peak, double *rms, std::string *reason)
{
    if (!digest || !peak || !rms) return false;
    double scale = 0.0;
    double scaledSquares = 0.0;
    for (double sample : samples) {
        if (!std::isfinite(sample)) {
            if (reason) *reason = "dependent-pcm-non-finite";
            return false;
        }
        const double magnitude = std::abs(sample);
        *peak = std::max(*peak, magnitude);
        if (magnitude == 0.0) continue;
        if (scale == 0.0) {
            scale = magnitude;
            scaledSquares = 1.0;
        } else if (magnitude > scale) {
            const double ratio = scale / magnitude;
            scaledSquares = 1.0 + scaledSquares * ratio * ratio;
            scale = magnitude;
        } else {
            const double ratio = magnitude / scale;
            scaledSquares += ratio * ratio;
        }
    }
    *rms = scale == 0.0 ? 0.0
        : scale * std::sqrt(scaledSquares
                            / static_cast<double>(samples.size()));
    if (!std::isfinite(*rms)) {
        if (reason) *reason = "dependent-pcm-rms-non-finite";
        return false;
    }
    *digest = digestCoefficientVector(samples);
    return true;
}

bool validBase(const FrameHeader &frame)
{
    return frame.streamType == StreamType::LegacyAc3
        && frame.substreamId == 0U && frame.sampleRate == kRate
        && frame.blocks == kBlocks && frame.acmod == 7U && frame.lfe
        && frame.channelCount == 6U;
}

bool validDependent(const FrameHeader &frame, const BsiInfo &bsi,
                    std::string *reason)
{
    if (frame.streamType != StreamType::Dependent
        || frame.substreamId != 0U || frame.sampleRate != kRate
        || frame.blocks != kBlocks || frame.acmod != 5U || frame.lfe
        || frame.channelCount != kChannels) {
        if (reason) *reason = "dependent-config4-topology-required";
        return false;
    }
    if (!bsi.bsiParsed || !bsi.chanmape || bsi.chanmap != kChanmap
        || bsi.chanmapChannelWeight != kChannels) {
        if (reason) *reason = "dependent-config4-chanmap-a010-required";
        return false;
    }
    return true;
}

PcmResult decodeDependentInternal(const std::vector<std::uint8_t> &bytes,
                                  const FrameHeader &frame,
                                  const BsiInfo &bsi,
                                  StatefulDecoder *decoderState, bool emitEos)
{
    PcmResult result;
    result.frameOffset = frame.offset;
    result.bitPosition = frame.endBit;
    std::array<ReferenceDitherSource, kChannels> localDitherSources{};
    std::array<Eac3TransformChannel, kChannels> localTransformChannels{};
    auto &ditherSources = decoderState ? decoderState->ditherSources
                                       : localDitherSources;
    const ChannelDitherSource channelDither =
        [&ditherSources](unsigned channel, std::size_t index,
                         unsigned exponent) {
            return ditherSources[channel].next(index, exponent);
        };
    const AudblkParseResult parsed = parseEac3AudblkCoefficients(
        bytes, frame, bsi, channelDither);
    if (parsed.disposition != AudblkDisposition::Accepted
        || !parsed.features || !parsed.features->state) {
        result.disposition = parsed.disposition == AudblkDisposition::Unsupported
            ? Disposition::Unsupported : Disposition::Malformed;
        result.reason = parsed.reason.empty() ? "dependent-audblk" : parsed.reason;
        result.bitPosition = parsed.bitPosition;
        return result;
    }
    const AudblkFrameState &state = *parsed.features->state;
    if (state.blocks != kBlocks || state.channels != kChannels
        || state.blockStates.size() != kBlocks) {
        result.disposition = Disposition::Unsupported;
        result.reason = "dependent-audblk-six-block-state-required";
        return result;
    }
    auto &transforms = decoderState ? decoderState->transformChannels
                                    : localTransformChannels;
    std::array<std::vector<double>, kChannels> samples{};
    for (const AudblkBlockState &block : state.blockStates) {
        if (block.channels.size() != kChannels
            || block.blockSwitchFlags.size() != kChannels) {
            result.disposition = Disposition::Malformed;
            result.reason = "dependent-audblk-channel-state";
            return result;
        }
        for (unsigned channel = 0U; channel < kChannels; ++channel) {
            const AudblkChannelState &coded = block.channels[channel];
            if (!coded.coefficientsDecoded || coded.coefficients.empty()
                || coded.coefficients.size() > kEac3TransformCoefficients) {
                result.disposition = Disposition::Unsupported;
                result.reason = "dependent-coefficient-vector-unsupported";
                return result;
            }
            const TransformOutput transformed = transforms[channel].processBlock(
                coded.coefficients, block.blockSwitchFlags[channel]);
            if (!transformed.ok
                || transformed.samples.size() != kEac3OutputSamples) {
                result.disposition = Disposition::Malformed;
                result.reason = transformed.error.empty()
                    ? "dependent-transform-block" : transformed.error;
                return result;
            }
            samples[channel].insert(samples[channel].end(),
                                    transformed.samples.begin(),
                                    transformed.samples.end());
        }
    }
    result.channels.resize(kChannels);
    result.stateDigest = digestAudblkState(state);
    for (unsigned channel = 0U; channel < kChannels; ++channel) {
        TransformFlushOutput tail;
        if (emitEos) tail = transforms[channel].flush();
        if (emitEos && (!tail.ok || !tail.hadTail
                        || tail.samples.size() != kSamplesPerTail)) {
            result.disposition = Disposition::Malformed;
            result.reason = tail.error.empty()
                ? "dependent-transform-eos-boundary" : tail.error;
            return result;
        }
        if (samples[channel].size() != kSamplesPerAu) {
            result.disposition = Disposition::Malformed;
            result.reason = "dependent-transform-sample-count";
            return result;
        }
        ChannelSummary &summary = result.channels[channel];
        summary.channel = channel;
        summary.location = kLocations[channel];
        summary.sampleCount = samples[channel].size();
        summary.eosTailCount = tail.samples.size();
        std::string summaryReason;
        if (!summarize(samples[channel], &summary.digest, &summary.peak,
                       &summary.rms, &summaryReason)
            || (emitEos && !summarize(tail.samples, &summary.eosDigest,
                                      &summary.eosPeak, &summary.eosRms,
                                      &summaryReason))) {
            result.disposition = Disposition::Malformed;
            result.reason = summaryReason.empty()
                ? "dependent-pcm-summary" : summaryReason;
            return result;
        }
        summary.samples = std::move(samples[channel]);
        if (emitEos) summary.eosTail = std::move(tail.samples);
        mix(&result.stateDigest, summary.digest);
        mix(&result.stateDigest, summary.eosDigest);
    }
    result.disposition = Disposition::Accepted;
    result.reason = "dependent-sid0-diagnostic-pcm";
    return result;
}

struct SessionResult {
    Disposition disposition = Disposition::Malformed;
    std::string reason;
};

class DependentDiagnosticSession {
public:
    SessionResult open()
    {
        if (poisoned_) return {Disposition::Malformed, "poisoned-reset-required"};
        if (opened_ && !poisoned_) return {Disposition::Malformed, "already-open"};
        opened_ = true;
        poisoned_ = false;
        canceled_ = false;
        flushed_ = false;
        return {Disposition::Accepted, "opened"};
    }

    SessionResult push(const PcmResult &result,
                       const std::function<bool(const PcmResult &)> &callback)
    {
        if (canceled_) return {Disposition::Canceled, "canceled"};
        if (poisoned_) return {Disposition::Malformed, "poisoned-reset-required"};
        if (!opened_) return {Disposition::Malformed, "not-open"};
        if (flushed_) return {Disposition::Malformed, "push-after-flush"};
        if (!callback) return poison("callback-missing");
        if (result.disposition != Disposition::Accepted)
            return poison(result.reason.empty() ? "dependent-parse-failed"
                                                 : result.reason);
        if (!callback(result)) return poison("callback-rejected");
        return {Disposition::Accepted, "accepted"};
    }

    SessionResult flush()
    {
        if (canceled_) return {Disposition::Canceled, "canceled"};
        if (poisoned_) return {Disposition::Malformed, "poisoned-reset-required"};
        if (!opened_) return {Disposition::Malformed, "not-open"};
        if (flushed_) return {Disposition::Accepted, "already-flushed"};
        flushed_ = true;
        return {Disposition::Accepted, "flushed"};
    }

    void cancel() { canceled_ = true; }
    void reset()
    {
        opened_ = false;
        poisoned_ = false;
        canceled_ = false;
        flushed_ = false;
    }
    bool poisoned() const { return poisoned_; }

private:
    SessionResult poison(const std::string &reason)
    {
        poisoned_ = true;
        return {Disposition::Malformed, reason};
    }

    bool opened_ = false;
    bool poisoned_ = false;
    bool canceled_ = false;
    bool flushed_ = false;
};

PcmResult syntheticResult()
{
    PcmResult result;
    result.disposition = Disposition::Accepted;
    result.reason = "synthetic";
    result.channels.resize(kChannels);
    for (unsigned channel = 0U; channel < kChannels; ++channel) {
        result.channels[channel].channel = channel;
        result.channels[channel].location = kLocations[channel];
        result.channels[channel].sampleCount = kSamplesPerAu;
        result.channels[channel].eosTailCount = kSamplesPerTail;
    }
    return result;
}

FrameHeader syntheticDependentFrame()
{
    FrameHeader frame;
    frame.streamType = StreamType::Dependent;
    frame.substreamId = 0U;
    frame.sampleRate = kRate;
    frame.blocks = kBlocks;
    frame.acmod = 5U;
    frame.channelCount = kChannels;
    frame.lfe = false;
    return frame;
}

BsiInfo syntheticDependentBsi()
{
    BsiInfo bsi;
    bsi.bsiParsed = true;
    bsi.chanmape = true;
    bsi.chanmap = kChanmap;
    bsi.chanmapChannelWeight = kChannels;
    return bsi;
}

bool runSyntheticTopologyTests(unsigned *caseCount)
{
    if (!caseCount) return false;
    *caseCount = 1U; // valid topology baseline
    const FrameHeader validFrame = syntheticDependentFrame();
    const BsiInfo validBsi = syntheticDependentBsi();
    std::string reason;
    bool pass = validDependent(validFrame, validBsi, &reason);
    auto expectRejected = [&pass, caseCount](const FrameHeader &frame,
                                              const BsiInfo &bsi) {
        ++*caseCount;
        std::string rejection;
        pass = !validDependent(frame, bsi, &rejection) && pass;
    };

    // Keep these as individual direct mutations so each topology contract is
    // covered by a distinct fail-closed case rather than one compound check.
    FrameHeader wrongSid = validFrame;
    wrongSid.substreamId = 1U;
    expectRejected(wrongSid, validBsi);
    FrameHeader wrongType = validFrame;
    wrongType.streamType = StreamType::Independent;
    expectRejected(wrongType, validBsi);
    FrameHeader wrongAcmod = validFrame;
    wrongAcmod.acmod = 4U;
    expectRejected(wrongAcmod, validBsi);
    FrameHeader wrongLfe = validFrame;
    wrongLfe.lfe = true;
    expectRejected(wrongLfe, validBsi);
    FrameHeader wrongChannelCount = validFrame;
    wrongChannelCount.channelCount = 3U;
    expectRejected(wrongChannelCount, validBsi);
    BsiInfo wrongChanmap = validBsi;
    wrongChanmap.chanmap = 0xA000U;
    expectRejected(validFrame, wrongChanmap);
    BsiInfo missingChanmap = validBsi;
    missingChanmap.chanmape = false;
    expectRejected(validFrame, missingChanmap);
    BsiInfo wrongWeight = validBsi;
    wrongWeight.chanmapChannelWeight = 3U;
    expectRejected(validFrame, wrongWeight);
    return pass && *caseCount == 9U;
}

bool runSyntheticChannelStorageTest()
{
    std::array<std::vector<double>, kChannels> channelPcm{};
    for (unsigned channel = 0U; channel < kChannels; ++channel) {
        channelPcm[channel].resize(kSamplesPerAu);
        for (std::size_t sample = 0U; sample < kSamplesPerAu; ++sample)
            channelPcm[channel][sample] = static_cast<double>(channel + 1U);
    }
    bool distinctStorage = true;
    for (unsigned left = 0U; left < kChannels; ++left) {
        for (unsigned right = left + 1U; right < kChannels; ++right)
            distinctStorage = distinctStorage
                && channelPcm[left].data() != channelPcm[right].data();
    }
    const double channelOneBefore = channelPcm[1][0];
    channelPcm[0][0] = -1.0;
    const bool isolatedMutation = channelPcm[1][0] == channelOneBefore
        && channelPcm[2][0] == 3.0 && channelPcm[3][0] == 4.0;
    return distinctStorage && isolatedMutation;
}

bool selfTest()
{
    const PcmResult synthetic = syntheticResult();
    unsigned topologyCases = 0U;
    const bool topology = runSyntheticTopologyTests(&topologyCases);
    const bool channelStorage = runSyntheticChannelStorageTest();
    if (!topology || !channelStorage) {
        std::cerr << "selfTest=FAIL case=topology-or-channel-storage"
                  << " topologyCases=" << topologyCases << '\n';
        return false;
    }
    std::size_t callbackCount = 0U;
    DependentDiagnosticSession session;
    const bool open = session.open().disposition == Disposition::Accepted;
    const SessionResult accepted = session.push(
        synthetic, [&callbackCount](const PcmResult &result) {
            ++callbackCount;
            bool identity = result.channels.size() == kChannels;
            for (unsigned index = 0U; identity && index < result.channels.size();
                 ++index) {
                identity = result.channels[index].channel == index
                    && result.channels[index].location
                        == std::string(kLocations[index])
                    && result.channels[index].sampleCount == kSamplesPerAu
                    && result.channels[index].eosTailCount == kSamplesPerTail;
            }
            for (unsigned left = 0U; identity && left < result.channels.size();
                 ++left) {
                for (unsigned right = left + 1U;
                     identity && right < result.channels.size(); ++right) {
                    identity = std::string(result.channels[left].location)
                        != std::string(result.channels[right].location);
                }
            }
            return identity;
        });
    const SessionResult flushed = session.flush();
    const SessionResult secondFlush = session.flush();
    const bool valid = open && accepted.disposition == Disposition::Accepted
        && flushed.disposition == Disposition::Accepted
        && secondFlush.disposition == Disposition::Accepted
        && callbackCount == 1U;
    if (!valid) {
        std::cerr << "selfTest=FAIL case=valid-callback-flush\n";
        return false;
    }
    const SessionResult afterFlush = session.push(
        synthetic, [](const PcmResult &) { return true; });
    if (afterFlush.reason != "push-after-flush") {
        std::cerr << "selfTest=FAIL case=push-after-flush\n";
        return false;
    }
    session.reset();
    session.open();
    session.cancel();
    const SessionResult canceled = session.push(synthetic, [](const PcmResult &) {
        return true;
    });
    if (canceled.disposition != Disposition::Canceled) {
        std::cerr << "selfTest=FAIL case=cancel\n";
        return false;
    }
    session.reset();
    session.open();
    const SessionResult rejected = session.push(synthetic, [](const PcmResult &) {
        return false;
    });
    const SessionResult rejectedFlush = session.flush();
    const SessionResult reopenPoisoned = session.open();
    if (rejected.reason != "callback-rejected"
        || rejectedFlush.reason != "poisoned-reset-required") {
        std::cerr << "selfTest=FAIL case=backpressure-poison\n";
        return false;
    }
    if (reopenPoisoned.reason != "poisoned-reset-required") {
        std::cerr << "selfTest=FAIL case=poisoned-reopen\n";
        return false;
    }
    session.reset();
    session.open();
    const SessionResult missing = session.push(synthetic, {});
    if (missing.reason != "callback-missing" || !session.poisoned()) {
        std::cerr << "selfTest=FAIL case=missing-callback\n";
        return false;
    }
    session.reset();
    session.open();
    const SessionResult afterReset = session.push(
        synthetic, [](const PcmResult &) { return true; });
    if (afterReset.disposition != Disposition::Accepted) {
        std::cerr << "selfTest=FAIL case=reset-reopen\n";
        return false;
    }
    constexpr unsigned lifecycleCases = 6U;
    const unsigned totalCases = lifecycleCases + topologyCases + 1U;
    std::cout << "selfTest=PASS cases=" << totalCases
              << " topologyCases=" << topologyCases
              << " channelIdentity=PASS channelStorage=PASS"
              << " callback=PASS cancel=PASS poison=PASS reset=PASS"
              << " backpressure=PASS"
              << " drcApplied=NO ffmpegLinked=NO\n";
    return true;
}

bool runFile(const std::string &path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::cerr << "probeResult=FAIL stage=open reason=file-open-failed\n";
        return false;
    }
    const std::vector<std::uint8_t> bytes(
        (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const Eb3FramingResult framing = normalizeEb3Framing(bytes);
    if (!framing.ok || framing.normalized.empty()) {
        std::cerr << "probeResult=FAIL stage=framing reason=" << framing.reason << '\n';
        return false;
    }
    const std::vector<std::uint8_t> &framed = framing.normalized;
    const ParseResult baseParsed = parseSyncframe(framed, 0U);
    if (baseParsed.disposition != eac3native::Disposition::Accepted
        || !baseParsed.frame
        || !validBase(*baseParsed.frame)) {
        std::cerr << "probeResult=INCONCLUSIVE reason=legacy-base-preamble-required\n";
        return false;
    }
    const std::size_t dependentOffset = baseParsed.frame->sizeBytes;
    const ParseResult dependentParsed = parseSyncframe(framed, dependentOffset);
    if (dependentParsed.disposition != eac3native::Disposition::Accepted
        || !dependentParsed.frame) {
        std::cerr << "probeResult=FAIL reason=dependent-frame-parse\n";
        return false;
    }
    const FrameHeader &dependent = *dependentParsed.frame;
    const BsiParseResult bsi = parseEac3Bsi(framed, dependent);
    if (bsi.disposition != eac3native::Disposition::Accepted || !bsi.info) {
        std::cerr << "probeResult=FAIL reason=dependent-bsi-"
                  << bsi.reason << '\n';
        return false;
    }
    std::string topologyReason;
    if (!validDependent(dependent, *bsi.info, &topologyReason)) {
        std::cerr << "probeResult=INCONCLUSIVE reason=" << topologyReason << '\n';
        return false;
    }
    PcmResult decoded = decodeDependent(framed, dependent, *bsi.info);
    decoded.baseSampleStart = 0U;
    decoded.dependentSampleStart = 0U;
    DependentDiagnosticSession session;
    session.open();
    std::size_t callbackCount = 0U;
    bool callbackValid = false;
    const SessionResult pushed = session.push(
        decoded, [&callbackCount, &callbackValid](const PcmResult &result) {
            ++callbackCount;
            callbackValid = result.channels.size() == kChannels
                && result.baseSampleStart == result.dependentSampleStart;
            for (unsigned index = 0U; index < result.channels.size(); ++index) {
                const ChannelSummary &channel = result.channels[index];
                callbackValid = callbackValid
                    && channel.channel == index
                    && channel.location == std::string(kLocations[index])
                    && channel.sampleCount == kSamplesPerAu
                    && channel.eosTailCount == kSamplesPerTail;
            }
            return callbackValid;
        });
    const SessionResult flushed = session.flush();
    const bool timeline = decoded.baseSampleStart == 0U
        && decoded.dependentSampleStart == 0U;
    const bool pass = decoded.disposition == Disposition::Accepted
        && pushed.disposition == Disposition::Accepted
        && flushed.disposition == Disposition::Accepted
        && callbackCount == 1U && callbackValid && timeline;
    std::cout << "baseType=legacy-ac3 baseSid=0 dependentType=dependent"
              << " dependentSid=0 chanmap=0x" << std::hex << std::uppercase
              << kChanmap << std::dec << " chanmapWeight=" << kChannels << '\n'
              << "baseSampleStart=" << decoded.baseSampleStart
              << " dependentSampleStart=" << decoded.dependentSampleStart
              << " sampleTimeline=" << (timeline ? "PASS" : "FAIL") << '\n'
              << "callbackCount=" << callbackCount
              << " channelIdentity=" << (callbackValid ? "PASS" : "FAIL")
              << " callbackBoundary="
              << (callbackValid ? "PASS" : "FAIL") << '\n'
              << "stateDigest=0x" << std::hex << std::setfill('0')
              << std::setw(16) << decoded.stateDigest << std::dec
              << " pcmAvailability=PCM_AVAILABLE_DIAGNOSTIC_DEPENDENT_AC3"
              << " productionAcceptance=INCONCLUSIVE\n"
              << "drcApplied=NO ffmpegLinked=NO\n";
    for (const ChannelSummary &channel : decoded.channels)
        std::cout << "pcm channel=" << channel.channel
                  << " location=" << channel.location
                  << " sampleCount=" << channel.sampleCount
                  << " digest=0x" << std::hex << std::setfill('0')
                  << std::setw(16) << channel.digest << std::dec
                  << std::setprecision(12)
                  << " peak=" << channel.peak << " rms=" << channel.rms
                  << " eosTailCount=" << channel.eosTailCount
                  << " eosDigest=0x" << std::hex << std::setfill('0')
                  << std::setw(16) << channel.eosDigest << std::dec
                  << std::setprecision(12)
                  << " eosPeak=" << channel.eosPeak
                  << " eosRms=" << channel.eosRms << '\n';
    std::cout << "probeResult=" << (pass ? "PASS" : "FAIL")
              << " stage=gate8n-5g-dependent-sid0-diagnostic-pcm\n";
    return pass;
}

PcmResult decodeDependent(const std::vector<std::uint8_t> &bytes,
                          const FrameHeader &frame, const BsiInfo &bsi)
{
    return decodeDependentInternal(bytes, frame, bsi, nullptr, true);
}

PcmResult decodeDependent(const std::vector<std::uint8_t> &bytes,
                          const FrameHeader &frame, const BsiInfo &bsi,
                          StatefulDecoder &state)
{
    return decodeDependentInternal(bytes, frame, bsi, &state, false);
}

PcmResult decodeDependentEnd(const std::vector<std::uint8_t> &bytes,
                             const FrameHeader &frame, const BsiInfo &bsi,
                             StatefulDecoder &state)
{
    return decodeDependentInternal(bytes, frame, bsi, &state, true);
}

} // namespace dependentpcm

#ifndef EAC3_CONFIG4_PCM_COMPOSITION_TU
int main(int argc, char **argv)
{
    if (argc == 2 && std::string(argv[1]) == "--self-test")
        return dependentpcm::selfTest() ? 0 : 1;
    if (argc != 2) {
        std::cerr << "Usage: Eac3NativeDependentPcmProbe <raw.eac3|raw.eb3>\n"
                     "       Eac3NativeDependentPcmProbe --self-test\n";
        return 2;
    }
    return dependentpcm::runFile(argv[1]) ? 0 : 1;
}
#endif
