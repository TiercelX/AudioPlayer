#include "native-eac3-core-decoder.h"

#include "native-eac3-core.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
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
        std::memcpy(&bits, &sample, sizeof(bits));
        digest = mix(digest, bits);
    }
    return digest;
}

bool finite(const std::vector<double>& samples)
{
    return std::all_of(samples.begin(), samples.end(),
                       [](double value) { return std::isfinite(value); });
}

bool selfTest()
{
    NativeEac3CoreDecoder decoder;
    const NativeCoreResult opened = decoder.open();
    const bool openCase = opened.accepted && decoder.opened();
    const NativeCoreResult empty = decoder.processAccessUnit({}, 0U);
    const NativeCoreResult poisoned = decoder.processAccessUnit({}, 0U);
    const NativeCoreFlushResult poisonedFlush = decoder.flush();
    const bool failStopped = empty.disposition == NativeCoreDisposition::Malformed
        && empty.reason == "native-core-empty-access-unit"
        && poisoned.reason == "native-core-poisoned-reset-required"
        && !poisonedFlush.ok
        && poisonedFlush.reason == "native-core-poisoned-reset-required";
    decoder.reset();
    const NativeCoreResult reopened = decoder.open();
    const NativeCoreFlushResult emptyEos = decoder.flush();
    const bool resetCase = reopened.accepted && emptyEos.ok && !emptyEos.hadTail;
    decoder.reset();
    decoder.open();
    decoder.cancel();
    const NativeCoreResult canceled = decoder.processAccessUnit({}, 0U);
    const bool cancelCase = canceled.flow == NativeCoreFlow::Canceled;
    const bool pass = openCase && failStopped && resetCase && cancelCase;
    std::cout << "selfTest=" << (pass ? "PASS" : "FAIL")
              << " cases=4 openConfig3=" << (openCase ? "YES" : "NO")
              << " failStopped=" << (failStopped ? "YES" : "NO")
              << " poisonedFlush=" << ((!poisonedFlush.ok
                                           && poisonedFlush.reason
                                               == "native-core-poisoned-reset-required")
                                              ? "YES" : "NO")
              << " resetEmptyEos=" << (resetCase ? "YES" : "NO")
              << " cancel=" << (cancelCase ? "YES" : "NO")
              << " drcApplied=NO ffmpegLinked=NO\n";
    return pass;
}

bool runFile(const std::string& path, std::size_t maxFrames)
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
    NativeEac3CoreDecoder decoder;
    const NativeCoreResult opened = decoder.open();
    if (!opened.accepted) {
        std::cerr << "probeResult=FAIL stage=open reason=" << opened.reason << '\n';
        return false;
    }
    std::size_t offset = 0U;
    std::size_t accepted = 0U;
    std::size_t syncframes = 0U;
    std::size_t pcmSamplesPerChannel = 0U;
    std::size_t tailSamplesPerChannel = 0U;
    bool finiteOutput = true;
    bool identity = true;
    bool timeline = true;
    std::uint64_t pcmDigest = 1469598103934665603ULL;
    while (offset < framed.size() && accepted < maxFrames) {
        const ParseResult parsed = parseSyncframe(framed, offset);
        if (parsed.disposition != Disposition::Accepted || !parsed.frame) {
            std::cerr << "probeResult=FAIL stage=" << toString(parsed.stage)
                      << " reason=" << parsed.reason << " frame=" << syncframes
                      << " bit=" << offset * 8U << '\n';
            return false;
        }
        const FrameHeader& frame = *parsed.frame;
        if (frame.sizeBytes > framed.size() - offset) {
            std::cerr << "probeResult=FAIL stage=bounds reason=truncated-frame"
                      << " frame=" << syncframes << '\n';
            return false;
        }
        std::vector<std::uint8_t> packet(
            framed.begin() + static_cast<std::ptrdiff_t>(offset),
            framed.begin() + static_cast<std::ptrdiff_t>(offset + frame.sizeBytes));
        ++syncframes;
        const NativeCoreResult result = decoder.processAccessUnit(
            packet, accepted * 1536U);
        if (result.disposition != NativeCoreDisposition::Accepted) {
            std::cout << "firstFailureFrame=" << (syncframes - 1U)
                      << " firstFailureBit=" << offset * 8U
                      << " firstFailureDisposition=" << toString(result.disposition)
                      << " firstFailureReason=" << result.reason << '\n'
                      << "acceptedFrames=" << accepted << '\n'
                      << "pcmSamplesPerChannel=" << pcmSamplesPerChannel << '\n'
                      << "drcApplied=NO\nffmpegLinked=NO\n"
                      << "probeResult="
                      << ((accepted == 0U && frame.streamType == StreamType::LegacyAc3
                               && result.disposition == NativeCoreDisposition::Unsupported)
                              ? "PASS" : "FAIL")
                      << " stage=gate8n-5a-native-core-decoder\n";
            return accepted == 0U && frame.streamType == StreamType::LegacyAc3
                && result.disposition == NativeCoreDisposition::Unsupported;
        }
        const DecodedAccessUnit& unit = result.unit;
        timeline = timeline && unit.sampleStart == accepted * 1536U
            && unit.sampleCount == 1536U && unit.sampleRate == 48000U;
        identity = identity && unit.channels.size() == 6U
            && unit.channels[0].id == "FL" && unit.channels[1].id == "FC"
            && unit.channels[2].id == "FR" && unit.channels[3].id == "SL"
            && unit.channels[4].id == "SR" && unit.channels[5].id == "LFE"
            && unit.channels[5].lfe && !unit.channels[0].lfe;
        for (const DecodedChannel& channel : unit.channels) {
            finiteOutput = finiteOutput && channel.samples.size() == 1536U
                && finite(channel.samples);
            pcmDigest = mix(pcmDigest, digestSamples(channel.samples));
        }
        pcmSamplesPerChannel += unit.sampleCount;
        ++accepted;
        offset += frame.sizeBytes;
    }
    if (accepted == 0U) {
        std::cerr << "probeResult=FAIL stage=summary reason=no-accepted-frames\n";
        return false;
    }
    const bool overlapBeforeEos = decoder.hasPendingOverlap();
    const NativeCoreFlushResult flushed = decoder.flush();
    const bool eos = flushed.ok && flushed.hadTail
        && flushed.tailSamplesPerChannel == 256U
        && flushed.channels.size() == 6U;
    if (eos) {
        for (const DecodedChannel& channel : flushed.channels) {
            finiteOutput = finiteOutput && channel.samples.size() == 256U
                && finite(channel.samples);
            pcmDigest = mix(pcmDigest, digestSamples(channel.samples));
        }
        tailSamplesPerChannel = flushed.tailSamplesPerChannel;
    }
    const bool pass = finiteOutput && identity && timeline && eos
        && decoder.framesProcessed() == accepted
        && overlapBeforeEos;
    std::cout << "syncframes=" << syncframes << '\n'
              << "acceptedFrames=" << accepted << '\n'
              << "sampleStartLast=" << ((accepted - 1U) * 1536U) << '\n'
              << "pcmSamplesPerChannel=" << pcmSamplesPerChannel << '\n'
              << "tailSamplesPerChannel=" << tailSamplesPerChannel << '\n'
              << "pcmSamplesIncludingEosTail="
              << pcmSamplesPerChannel + tailSamplesPerChannel << '\n'
              << "channelIdentity=" << (identity ? "PASS" : "FAIL") << '\n'
              << "sampleTimeline=" << (timeline ? "PASS" : "FAIL") << '\n'
              << "statePolicy=continuous-reference-dither-overlap\n"
              << "overlapContinuity="
              << (overlapBeforeEos ? "PASS" : "FAIL") << '\n'
              << "finitePcm=" << (finiteOutput ? "YES" : "NO") << '\n'
              << "eosTail=" << (eos ? "PASS" : "FAIL") << '\n'
              << "drcApplied=NO\nffmpegLinked=NO\n"
              << "pcmDigest=" << std::hex << pcmDigest << std::dec << '\n'
              << "probeResult=" << (pass ? "PASS" : "FAIL")
              << " stage=gate8n-5a-native-core-decoder\n";
    return pass;
}

void usage()
{
    std::cerr << "Usage: Eac3NativeCoreDecoderProbe --self-test\n"
                 "       Eac3NativeCoreDecoderProbe <raw.eac3|raw.eb3>"
                 " [--max-frames N]\n";
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 2 && std::string(argv[1]) == "--self-test")
        return selfTest() ? 0 : 1;
    if (argc < 2 || argc > 4) {
        usage();
        return 2;
    }
    std::size_t maxFrames = 10U;
    for (int index = 2; index < argc; ++index) {
        if (std::string(argv[index]) == "--max-frames" && index + 1 < argc) {
            try {
                maxFrames = static_cast<std::size_t>(std::stoull(argv[++index]));
            } catch (...) {
                usage();
                return 2;
            }
        } else {
            usage();
            return 2;
        }
    }
    return runFile(argv[1], maxFrames) ? 0 : 1;
}
