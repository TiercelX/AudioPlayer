#include "native-eac3-core-decoder-seam.h"

#include "native-eac3-core.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {
using namespace eac3jocsession;
using namespace eac3native;
using JocDisposition = eac3jocsession::Disposition;
using JocFlow = eac3jocsession::FlowStatus;

std::uint64_t mix(std::uint64_t digest, std::uint64_t value)
{
    for (unsigned shift = 0U; shift < 64U; shift += 8U) {
        digest ^= (value >> shift) & 0xffU;
        digest *= 1099511628211ULL;
    }
    return digest;
}

void digestFrame(std::uint64_t *digest, const CoreFrame &frame)
{
    for (const auto &plane : frame.planar) {
        for (float value : plane) {
            std::uint32_t bits = 0U;
            std::memcpy(&bits, &value, sizeof(bits));
            *digest = mix(*digest, bits);
        }
    }
}

bool frameIdentity(const CoreFrame &frame, bool tail)
{
    return frame.contentKind == CoreContentKind::OrdinaryEac3
        && frame.channelIds == std::vector<std::string>({"FL", "FC", "FR", "SL", "SR", "LFE"})
        && frame.lfeChannels == std::vector<bool>({false, false, false, false, false, true})
        && !frame.drcApplied && frame.eosTail == tail
        && frame.sampleCount == (tail ? 256U : 1536U)
        && frame.planar.size() == 6U
        && std::all_of(frame.planar.begin(), frame.planar.end(),
                       [tail](const std::vector<float> &plane) {
                           return plane.size() == (tail ? 256U : 1536U)
                               && std::all_of(plane.begin(), plane.end(),
                                              [](float value) { return std::isfinite(value); });
                       });
}

bool loadFramed(const std::string &path, std::vector<std::uint8_t> *framed)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    const std::vector<std::uint8_t> bytes(
        (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const Eb3FramingResult result = normalizeEb3Framing(bytes);
    if (!result.ok) {
        std::cerr << "probeResult=FAIL stage=" << toString(result.stage)
                  << " reason=" << result.reason << '\n';
        return false;
    }
    *framed = result.normalized;
    return true;
}

bool runFile(const std::string &path, std::size_t maxFrames,
             std::size_t rejectAt, std::int64_t timestampBase)
{
    std::vector<std::uint8_t> framed;
    if (!loadFramed(path, &framed)) {
        std::cerr << "probeResult=FAIL stage=open reason=file-open-failed\n";
        return false;
    }
    NativeEac3CoreDecoderSeam decoder;
    CoreDecoderConfig config;
    config.sampleRate = 48000U;
    config.channels = 6U;
    config.channelLayout = "FL,FC,FR,SL,SR,LFE";
    const CoreDecodeResult opened = decoder.open(config);
    if (!opened.accepted) {
        std::cerr << "probeResult=FAIL stage=open reason=" << opened.reason << '\n';
        return false;
    }
    std::size_t offset = 0U;
    std::size_t accepted = 0U;
    std::size_t syncframes = 0U;
    std::size_t pcmSamples = 0U;
    std::size_t tailSamples = 0U;
    bool valid = true;
    bool timeline = true;
    bool callbackBackpressure = rejectAt == static_cast<std::size_t>(-1);
    std::int64_t firstTimestamp = -1;
    std::int64_t eosTimestamp = -1;
    std::uint64_t digest = 1469598103934665603ULL;
    while (offset < framed.size() && accepted < maxFrames) {
        const ParseResult parsed = parseSyncframe(framed, offset);
        if (parsed.disposition != eac3native::Disposition::Accepted || !parsed.frame) {
            std::cerr << "probeResult=FAIL stage=" << toString(parsed.stage)
                      << " reason=" << parsed.reason << " frame=" << syncframes
                      << " bit=" << offset * 8U << '\n';
            return false;
        }
        const FrameHeader &header = *parsed.frame;
        if (header.sizeBytes > framed.size() - offset) {
            std::cerr << "probeResult=FAIL stage=bounds reason=truncated-frame\n";
            return false;
        }
        const std::vector<std::uint8_t> packet(
            framed.begin() + static_cast<std::ptrdiff_t>(offset),
            framed.begin() + static_cast<std::ptrdiff_t>(offset + header.sizeBytes));
        const std::size_t frameIndex = syncframes++;
        const bool reject = frameIndex == rejectAt;
        bool callbackCalled = false;
        const CoreDecodeResult result = decoder.pushPacket(
            packet, timestampBase + static_cast<std::int64_t>(accepted * 1536U),
            [&callbackCalled, reject, &digest, &valid, &timeline, accepted,
             timestampBase, &firstTimestamp](const CoreFrame &frame) {
                callbackCalled = true;
                if (accepted == 0U) firstTimestamp = frame.timestamp;
                valid = valid && frameIdentity(frame, false)
                    && frame.timestamp == timestampBase
                        + static_cast<std::int64_t>(accepted * 1536U);
                timeline = timeline && frame.timestamp == timestampBase
                    + static_cast<std::int64_t>(accepted * 1536U);
                if (!reject) digestFrame(&digest, frame);
                return !reject;
            });
        if (reject) {
            const CoreDecodeResult blocked = decoder.pushPacket(
                packet, timestampBase + static_cast<std::int64_t>(accepted * 1536U),
                [](const CoreFrame &) { return true; });
            const CoreDecodeResult blockedFlush = decoder.flush(
                [](const CoreFrame &) { return true; });
            callbackBackpressure = callbackCalled
                && !result.accepted && result.flow == JocFlow::CallbackRejected
                && blocked.flow == JocFlow::CallbackRejected
                && blockedFlush.flow == JocFlow::CallbackRejected;
            std::cout << "acceptedFrames=" << accepted << '\n'
                      << "callbackBackpressure=" << (callbackBackpressure ? "PASS" : "FAIL") << '\n'
                      << "probeResult=" << (callbackBackpressure ? "PASS" : "FAIL")
                      << " stage=gate8n-5b-native-core-seam\n";
            return callbackBackpressure;
        }
        if (result.disposition != JocDisposition::OrdinaryEac3
            || result.contentKind != CoreContentKind::OrdinaryEac3
            || !result.accepted) {
            const bool rawConfig4Gate = accepted == 0U
                && header.streamType == StreamType::LegacyAc3
                && result.disposition == JocDisposition::Unsupported
                && !callbackCalled;
            std::cout << "firstFailureFrame=" << frameIndex << '\n'
                      << "firstFailureDisposition=" << dispositionText(result.disposition) << '\n'
                      << "firstFailureReason=" << result.reason << '\n'
                      << "acceptedFrames=" << accepted << '\n'
                      << "pcmSamplesPerChannel=" << pcmSamples << '\n'
                      << "callbackFrames=0\n"
                      << "probeResult=" << (rawConfig4Gate ? "PASS" : "FAIL")
                      << " stage=gate8n-5b-native-core-seam\n";
            return rawConfig4Gate;
        }
        if (!callbackCalled) valid = false;
        pcmSamples += 1536U;
        ++accepted;
        offset += header.sizeBytes;
    }
    if (accepted == 0U) return false;
    const CoreDecodeResult flushed = decoder.flush(
        [&valid, &tailSamples, &digest, &eosTimestamp](const CoreFrame &frame) {
            const bool tail = frame.eosTail;
            eosTimestamp = frame.timestamp;
            valid = valid && frameIdentity(frame, true) && frame.flush && tail;
            if (tail) {
                tailSamples += frame.sampleCount;
                digestFrame(&digest, frame);
            }
            return true;
        });
    const CoreDecodeResult secondFlush = decoder.flush(
        [](const CoreFrame &) { return true; });
    const bool eos = flushed.accepted && flushed.terminal
        && secondFlush.flow == JocFlow::AlreadyFlushed && tailSamples == 256U;
    const bool pass = valid && timeline && eos && callbackBackpressure
        && decoder.framesProcessed() == accepted;
    std::cout << "syncframes=" << syncframes << '\n'
              << "acceptedFrames=" << accepted << '\n'
              << "timestampBase=" << timestampBase << '\n'
              << "firstFrameTimestamp=" << firstTimestamp << '\n'
              << "eosTimestamp=" << eosTimestamp << '\n'
              << "pcmSamplesPerChannel=" << pcmSamples << '\n'
              << "eosTailSamplesPerChannel=" << tailSamples << '\n'
              << "pcmSamplesIncludingEosTail=" << pcmSamples + tailSamples << '\n'
              << "contentKind=OrdinaryEac3\n"
              << "channelIdentity=" << (valid ? "PASS" : "FAIL") << '\n'
              << "sampleTimeline=" << (timeline ? "PASS" : "FAIL") << '\n'
              << "callbackBackpressure=NOT-RUN\n"
              << "eosTail=" << (eos ? "PASS" : "FAIL") << '\n'
              << "drcApplied=NO\nstreamResetPolicy=continuous-adjacent-syncframes\n"
              << "pcmDigestFloat=" << std::hex << digest << std::dec << '\n'
              << "probeResult=" << (pass ? "PASS" : "FAIL")
              << " stage=gate8n-5b-native-core-seam\n";
    return pass;
}

bool selfTest()
{
    NativeEac3CoreDecoderSeam decoder;
    CoreDecoderConfig config;
    config.sampleRate = 48000U;
    config.channels = 6U;
    config.channelLayout = "FL,FC,FR,SL,SR,LFE";
    const CoreDecodeResult opened = decoder.open(config);
    const bool openCase = opened.accepted
        && opened.disposition == JocDisposition::OrdinaryEac3
        && opened.contentKind == CoreContentKind::OrdinaryEac3
        && decoder.opened();
    const CoreDecodeResult malformed = decoder.pushPacket(
        {}, 0, [](const CoreFrame &) { return true; });
    const CoreDecodeResult blocked = decoder.pushPacket(
        {}, 0, [](const CoreFrame &) { return true; });
    const CoreDecodeResult poisonedFlush = decoder.flush(
        [](const CoreFrame &) { return true; });
    const bool poisonCase = malformed.disposition == JocDisposition::Malformed
        && blocked.reason == "native-seam-poisoned-reset-required"
        && poisonedFlush.reason == "native-seam-poisoned-reset-required";
    decoder.reset();
    const CoreDecodeResult reopened = decoder.open(config);
    std::size_t emptyCallbacks = 0U;
    const CoreDecodeResult emptyFlush = decoder.flush([&emptyCallbacks](const CoreFrame &frame) {
        ++emptyCallbacks;
        return frame.flush && !frame.eosTail && frame.sampleCount == 0U
            && frame.contentKind == CoreContentKind::OrdinaryEac3;
    });
    const bool resetCase = reopened.accepted && emptyFlush.accepted
        && emptyFlush.terminal && emptyCallbacks == 1U;
    decoder.reset();
    decoder.open(config);
    decoder.cancel();
    const CoreDecodeResult canceled = decoder.pushPacket(
        {}, 0, [](const CoreFrame &) { return true; });
    const bool cancelCase = canceled.flow == JocFlow::Canceled;
    CoreFrame invalidFlush;
    invalidFlush.sampleRate = 48000U;
    invalidFlush.channels = 6U;
    invalidFlush.channelLayout = "FL,FC,FR,SL,SR,LFE";
    invalidFlush.sampleCount = 1U;
    invalidFlush.flush = true;
    invalidFlush.contentKind = CoreContentKind::OrdinaryEac3;
    invalidFlush.channelIds = {"FL", "FC", "FR", "SL", "SR", "LFE"};
    invalidFlush.lfeChannels = {false, false, false, false, false, true};
    invalidFlush.planar.assign(6U, std::vector<float>(1U, 0.0f));
    std::string invalidReason;
    const bool invalidShape = !validateCoreFrame(invalidFlush, &invalidReason)
        && invalidReason == "ordinary-core-empty-flush-shape-invalid";
    const bool pass = openCase && poisonCase && resetCase && cancelCase && invalidShape;
    std::cout << "selfTest=" << (pass ? "PASS" : "FAIL")
              << " cases=5 ordinaryDisposition=" << (openCase ? "PASS" : "FAIL")
              << " poisonReset=" << (poisonCase ? "PASS" : "FAIL")
              << " emptyEos=" << (resetCase ? "PASS" : "FAIL")
              << " cancel=" << (cancelCase ? "PASS" : "FAIL")
              << " invalidOrdinaryFlush=" << (invalidShape ? "PASS" : "FAIL")
              << " drcApplied=NO ffmpegLinked=NO\n";
    return pass;
}

void usage()
{
    std::cerr << "Usage: Eac3NativeCoreDecoderSeamProbe --self-test\n"
                 "       Eac3NativeCoreDecoderSeamProbe <raw.eac3|raw.eb3>"
                 " [--max-frames N] [--reject-at N] [--timestamp-base N]\n";
}

} // namespace

int main(int argc, char **argv)
{
    if (argc == 2 && std::string(argv[1]) == "--self-test")
        return selfTest() ? 0 : 1;
    if (argc < 2 || argc > 8) {
        usage();
        return 2;
    }
    std::size_t maxFrames = 10U;
    std::size_t rejectAt = static_cast<std::size_t>(-1);
    std::int64_t timestampBase = 0;
    for (int index = 2; index < argc; ++index) {
        const std::string option = argv[index];
        if ((option == "--max-frames" || option == "--reject-at"
             || option == "--timestamp-base")
            && index + 1 < argc) {
            try {
                const std::string valueText = argv[++index];
                const std::size_t value = static_cast<std::size_t>(std::stoull(valueText));
                if (option == "--max-frames") maxFrames = value;
                else if (option == "--reject-at") rejectAt = value;
                else timestampBase = std::stoll(valueText);
            } catch (...) {
                usage();
                return 2;
            }
        } else {
            usage();
            return 2;
        }
    }
    return runFile(argv[1], maxFrames, rejectAt, timestampBase) ? 0 : 1;
}
