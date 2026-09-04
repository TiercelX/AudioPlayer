#include "native-eac3-pcm-session.h"

#include "native-eac3-core.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {
using namespace eac3native;

std::uint64_t mix(std::uint64_t digest, std::uint64_t value)
{
    for (unsigned shift = 0U; shift < 64U; shift += 8U) {
        digest ^= (value >> shift) & 0xffU;
        digest *= 1099511628211ULL;
    }
    return digest;
}

std::uint64_t digestSamples(const std::vector<double>& samples)
{
    std::uint64_t digest = 1469598103934665603ULL;
    for (double sample : samples) {
        std::uint64_t bits = 0U;
        static_assert(sizeof(bits) == sizeof(sample), "double must be 64-bit");
        std::memcpy(&bits, &sample, sizeof(bits));
        digest = mix(digest, bits);
    }
    return digest;
}

bool finiteVector(const std::vector<double>& values)
{
    return std::all_of(values.begin(), values.end(), [](double value) {
        return std::isfinite(value);
    });
}

bool selfTest()
{
    Eac3PcmSession session;
    const PcmFlushResult empty = session.flush();
    const bool emptyEos = empty.ok && !empty.hadTail
        && empty.channelsData.empty();
    session.reset();
    const PcmFlushResult resetEos = session.flush();
    const bool resetClears = resetEos.ok && !resetEos.hadTail
        && resetEos.channelsData.empty();
    session.reset();
    const PcmFlushResult secondEos = session.flush();
    const bool eosRepeat = secondEos.ok && !secondEos.hadTail;
    const bool constants = kEac3InherentDelaySamples == 256U
        && kEac3OutputSamples == 256U;
    const bool noImplicitTopology = !session.initialized()
        && session.framesProcessed() == 0U;
    session.reset();
    FrameHeader invalidFrame;
    invalidFrame.streamType = StreamType::Independent;
    invalidFrame.blocks = 1U;
    invalidFrame.channelCount = 1U;
    invalidFrame.sampleRate = 48000U;
    invalidFrame.sizeBytes = 1U;
    invalidFrame.endBit = 8U;
    const PcmFrameResult firstFailure = session.processFrame({}, invalidFrame, {});
    const PcmFrameResult poisonedFailure = session.processFrame({}, invalidFrame, {});
    const PcmFlushResult poisonedFlush = session.flush();
    const bool failStopped = firstFailure.disposition == PcmSessionDisposition::Malformed
        && poisonedFailure.reason == "session-poisoned-reset-required"
        && !poisonedFlush.ok
        && poisonedFlush.reason == "session-poisoned-reset-required"
        && session.poisoned();
    session.reset();
    const bool resetClearsPoison = !session.poisoned();
    const bool pass = emptyEos && resetClears && eosRepeat && constants
        && noImplicitTopology && failStopped && resetClearsPoison;
    std::cout << "selfTest=" << (pass ? "PASS" : "FAIL")
              << " cases=6"
              << " emptyEos=" << (emptyEos ? "YES" : "NO")
              << " resetClears=" << (resetClears ? "YES" : "NO")
              << " eosRepeat=" << (eosRepeat ? "YES" : "NO")
              << " conceptualDelay256=" << (constants ? "YES" : "NO")
              << " noImplicitTopology=" << (noImplicitTopology ? "YES" : "NO")
              << " failStopped=" << (failStopped ? "YES" : "NO")
              << " poisonedFlush=" << ((!poisonedFlush.ok
                                          && poisonedFlush.reason
                                               == "session-poisoned-reset-required")
                                         ? "YES" : "NO")
              << " resetClearsPoison=" << (resetClearsPoison ? "YES" : "NO")
              << " drcApplied=NO ffmpegLinked=NO\n";
    return pass;
}

bool writeInterleaved(std::ofstream& output,
                      const std::vector<PcmChannelFrame>& channels,
                      std::size_t sampleCount)
{
    for (std::size_t sample = 0U; sample < sampleCount; ++sample) {
        for (const PcmChannelFrame& channel : channels) {
            if (sample >= channel.samples.size())
                return false;
            const float value = static_cast<float>(channel.samples[sample]);
            output.write(reinterpret_cast<const char*>(&value), sizeof(value));
            if (!output)
                return false;
        }
    }
    return true;
}

bool runFile(const std::string& path, std::size_t maxEac3,
             const std::string& dumpPath, PcmDitherMode ditherMode)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::cerr << "probeResult=FAIL stage=open reason=file-open-failed\n";
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

    const std::vector<std::uint8_t>& framed = framing.normalized;
    Eac3PcmSession session(ditherMode);
    session.reset();
    std::ofstream dump;
    if (!dumpPath.empty()) {
        dump.open(dumpPath, std::ios::binary | std::ios::trunc);
        if (!dump) {
            std::cerr << "probeResult=FAIL stage=dump-open reason=file-open-failed\n";
            return false;
        }
    }
    std::size_t offset = 0U;
    std::size_t syncframes = 0U;
    std::size_t accepted = 0U;
    std::size_t legacy = 0U;
    std::size_t unsupported = 0U;
    bool finite = true;
    bool identity = true;
    bool bitEnd = true;
    bool eos = false;
    bool dither = true;
    bool streamResetOnce = true;
    bool allFramesSixBlocks = true;
    bool samplesPerFrameConsistent = true;
    unsigned observedBlocksPerFrame = 0U;
    std::size_t observedSamplesPerChannelPerFrame = 0U;
    std::size_t observedPcmSamplesPerChannel = 0U;
    std::size_t observedTailPerChannel = 0U;
    bool observedFrameShape = false;
    bool haveDitherPredecessor = false;
    std::vector<std::uint64_t> priorDitherState;
    std::vector<std::size_t> priorDitherSamples;
    std::uint64_t stateDigest = 1469598103934665603ULL;
    std::uint64_t pcmDigest = 1469598103934665603ULL;
    std::size_t pcmSamples = 0U;
    std::size_t tailSamples = 0U;
    while (offset < framed.size() && (maxEac3 == 0U || accepted < maxEac3)) {
        const ParseResult parsed = parseSyncframe(framed, offset);
        if (parsed.disposition != Disposition::Accepted || !parsed.frame) {
            std::cerr << "probeResult=FAIL stage=" << toString(parsed.stage)
                      << " reason=" << parsed.reason << '\n';
            return false;
        }
        ++syncframes;
        const FrameHeader& frame = *parsed.frame;
        offset += frame.sizeBytes;
        if (frame.streamType == StreamType::LegacyAc3) {
            ++legacy;
            continue;
        }
        const BsiParseResult bsi = parseEac3Bsi(framed, frame);
        if (bsi.disposition != Disposition::Accepted || !bsi.info) {
            std::cerr << "probeResult=FAIL stage=" << toString(bsi.stage)
                      << " reason=" << bsi.reason << '\n';
            return false;
        }

        const PcmFrameResult result = session.processFrame(
            framed, frame, *bsi.info);
        if (result.disposition == PcmSessionDisposition::Unsupported) {
            ++unsupported;
            std::cerr << "unsupportedFrameOffset=" << frame.offset
                      << " reason=" << result.reason << " bit="
                      << result.bitPosition << '\n';
            return false;
        }
        if (result.disposition != PcmSessionDisposition::Accepted) {
            std::cerr << "probeResult=FAIL stage=pcm-session reason="
                      << result.reason << " frameOffset=" << result.frameOffset
                      << " block=" << result.blockIndex << " channel="
                      << result.channelIndex << " bit=" << result.bitPosition
                      << '\n';
            return false;
        }
        ++accepted;
        streamResetOnce = streamResetOnce
            && session.initialized() && !session.poisoned()
            && session.framesProcessed() == accepted;
        allFramesSixBlocks = allFramesSixBlocks && result.blocks == 6U;
        if (!observedFrameShape) {
            observedFrameShape = true;
            observedBlocksPerFrame = result.blocks;
            observedSamplesPerChannelPerFrame = result.outputSamplesPerChannel;
        } else {
            samplesPerFrameConsistent = samplesPerFrameConsistent
                && observedBlocksPerFrame == result.blocks
                && observedSamplesPerChannelPerFrame
                       == result.outputSamplesPerChannel;
        }
        bitEnd = bitEnd && result.bitPosition <= frame.endBit;
        identity = identity && result.channelsData.size() == frame.channelCount;
        stateDigest = mix(stateDigest, result.bitPosition);
        stateDigest = mix(stateDigest, result.blocks);
        stateDigest = mix(stateDigest, result.channels);
        for (const PcmChannelFrame& channel : result.channelsData) {
            identity = identity && channel.channel < frame.channelCount
                && channel.lfe == (frame.lfe
                                   && channel.channel + 1U == frame.channelCount);
            identity = identity && channel.coefficientSpans.size() == frame.blocks;
            finite = finite && channel.samples.size() == result.outputSamplesPerChannel
                && finiteVector(channel.samples);
            if (channel.channel == 0U)
                observedPcmSamplesPerChannel += channel.samples.size();
            pcmSamples += channel.samples.size();
            pcmDigest = mix(pcmDigest, digestSamples(channel.samples));
            stateDigest = mix(stateDigest, channel.ditherStateStart);
            stateDigest = mix(stateDigest, channel.ditherStateEnd);
            for (const auto& span : channel.coefficientSpans) {
                stateDigest = mix(stateDigest, span.first);
                stateDigest = mix(stateDigest, span.second);
            }
            if (!haveDitherPredecessor) {
                dither = dither
                    && channel.ditherStateStart
                           == ReferenceDitherSource::defaultSeed
                    && channel.ditherSamplesStart == 0U;
            } else {
                dither = dither
                    && channel.ditherStateStart == priorDitherState[channel.channel]
                    && channel.ditherSamplesStart == priorDitherSamples[channel.channel];
            }
            if (priorDitherState.size() < result.channelsData.size()) {
                priorDitherState.resize(result.channelsData.size());
                priorDitherSamples.resize(result.channelsData.size());
            }
            priorDitherState[channel.channel] = channel.ditherStateEnd;
            priorDitherSamples[channel.channel] =
                channel.ditherSamplesStart + channel.ditherSamples;
        }
        if (dump.is_open() && !writeInterleaved(
                dump, result.channelsData, result.outputSamplesPerChannel)) {
            std::cerr << "probeResult=FAIL stage=dump-write reason=short-write\n";
            return false;
        }
        haveDitherPredecessor = true;
    }
    const bool overlapContinuity = accepted > 0U && session.hasPendingOverlap();
    streamResetOnce = streamResetOnce && session.initialized()
        && !session.poisoned() && session.framesProcessed() == accepted;
    const PcmFlushResult flushed = session.flush();
    eos = flushed.ok && flushed.hadTail
        && flushed.tailSamplesPerChannel == kEac3OutputSamples
        && flushed.channelsData.size() == session.channelCount();
    if (eos) {
        for (const PcmChannelFrame& channel : flushed.channelsData) {
            finite = finite && channel.samples.size() == kEac3OutputSamples
                && finiteVector(channel.samples);
            tailSamples += channel.samples.size();
            if (channel.channel == 0U)
                observedPcmSamplesPerChannel += channel.samples.size();
            if (channel.channel == 0U)
                observedTailPerChannel = channel.samples.size();
            pcmDigest = mix(pcmDigest, digestSamples(channel.samples));
        }
        if (dump.is_open() && !writeInterleaved(
                dump, flushed.channelsData, flushed.tailSamplesPerChannel)) {
            std::cerr << "probeResult=FAIL stage=dump-write reason=short-write\n";
            return false;
        }
    }
    if (dump.is_open()) {
        dump.flush();
        if (!dump) {
            std::cerr << "probeResult=FAIL stage=dump-flush reason=flush-failed\n";
            return false;
        }
        dump.close();
        if (!dump) {
            std::cerr << "probeResult=FAIL stage=dump-close reason=close-failed\n";
            return false;
        }
    }
    const bool counts = accepted > 0U && unsupported == 0U;
    const bool pass = counts && finite && identity && bitEnd && eos && dither
        && overlapContinuity && streamResetOnce && allFramesSixBlocks
        && samplesPerFrameConsistent;
    if (!dumpPath.empty()) {
        std::cout << "dumpFormat=f32le-interleaved\n"
                  << "dumpChannelOrder=coded-order\n"
                  << "dumpChannels=" << session.channelCount() << '\n'
                  << "dumpIncludesEosTail=" << (eos ? "YES" : "NO") << '\n';
    }
    std::cout << "inputBytes=" << bytes.size() << '\n'
              << "normalizedBytes=" << framed.size() << '\n'
              << "syncframes=" << syncframes << '\n'
              << "acceptedFrames=" << accepted << '\n'
              << "unsupportedFrames=" << unsupported << '\n'
              << "legacyFrames=" << legacy << '\n'
              << "observedBlocksPerFrame=" << observedBlocksPerFrame << '\n'
              << "pcmSamplesIncludingEosTail=" << pcmSamples + tailSamples << '\n'
              << "observedSamplesPerChannelIncludingEosTail="
              << observedPcmSamplesPerChannel << '\n'
              << "observedSamplesPerChannelPerFrame="
              << observedSamplesPerChannelPerFrame << '\n'
              << "allFramesSixBlocks="
              << (allFramesSixBlocks ? "PASS" : "FAIL") << '\n'
              << "samplesPerFrameConsistent="
              << (samplesPerFrameConsistent ? "PASS" : "FAIL") << '\n'
              << "observedEosTailPerChannel=" << observedTailPerChannel << '\n'
              << "conceptualDelaySamples=256\n"
              << "ditherMode="
              << (ditherMode == PcmDitherMode::Reference
                      ? "reference" : "zero-diagnostic-only NONCONFORMING_ORACLE_ONLY")
              << '\n'
              << "ditherStatePolicy="
              << (ditherMode == PcmDitherMode::Reference
                      ? "continuous-reference" : "frozen-zero-diagnostic")
              << '\n'
              << "channelIdentity=" << (identity ? "PASS" : "FAIL") << '\n'
              << "bitEndBounded=" << (bitEnd ? "PASS" : "FAIL") << '\n'
              << "coefficientFinite=" << (finite ? "YES" : "NO") << '\n'
              << "ditherContinuity=" << (dither ? "PASS" : "FAIL") << '\n'
              << "overlapContinuity=" << (overlapContinuity ? "PASS" : "FAIL") << '\n'
              << "streamResetPolicy=" << (streamResetOnce ? "PASS" : "FAIL") << '\n'
              << "eosTail=" << (eos ? "PASS" : "FAIL") << '\n'
              << "stateDigest=" << std::hex << stateDigest << std::dec << '\n'
              << "pcmDigest=" << std::hex << pcmDigest << std::dec << '\n'
              << "drcApplied=NO\nffmpegLinked=NO\n"
              << "probeResult=" << (pass ? "PASS" : "FAIL")
              << " stage=gate8n-3b-n3b-pcm-session\n";
    return pass;
}

void usage()
{
    std::cerr << "Usage: Eac3NativePcmSessionProbe --self-test\n"
                 "       Eac3NativePcmSessionProbe <raw.eac3|raw.eb3> [--max-eac3 N]"
                 " [--dump-f32 path] [--dither zero]\n";
}
} // namespace

int main(int argc, char** argv)
{
    if (argc == 2 && std::string(argv[1]) == "--self-test")
        return selfTest() ? 0 : 1;
    if (argc < 2) {
        usage();
        return 2;
    }
    std::size_t maxEac3 = 1U;
    std::string dumpPath;
    PcmDitherMode ditherMode = PcmDitherMode::Reference;
    for (int index = 2; index < argc; ++index) {
        const std::string option = argv[index];
        if (option == "--max-eac3" && index + 1 < argc) {
            try {
                maxEac3 = static_cast<std::size_t>(std::stoull(argv[++index]));
            } catch (...) {
                usage();
                return 2;
            }
        } else if (option == "--dump-f32" && index + 1 < argc) {
            dumpPath = argv[++index];
        } else if (option == "--dither" && index + 1 < argc
                   && std::string(argv[index + 1]) == "zero") {
            ditherMode = PcmDitherMode::ZeroDiagnosticOnly;
            ++index;
        } else {
            usage();
            return 2;
        }
    }
    return runFile(argv[1], maxEac3, dumpPath, ditherMode) ? 0 : 1;
}
