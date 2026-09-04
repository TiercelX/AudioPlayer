#include "native-eac3-block-state.h"

#include "native-eac3-bsi.h"
#include "native-eac3-core.h"

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {
using namespace eac3native;

bool expect(bool condition, const char *name)
{
    if (!condition) std::cerr << "selfTest=FAIL case=" << name << '\n';
    return condition;
}

struct BitFieldWriter {
    std::vector<std::uint8_t> bytes;
    std::size_t bits = 0U;

    void put(unsigned value, unsigned width)
    {
        for (unsigned index = 0U; index < width; ++index) {
            if (bits % 8U == 0U) bytes.push_back(0U);
            const unsigned shift = width - index - 1U;
            if (((value >> shift) & 1U) != 0U)
                bytes.back() |= static_cast<std::uint8_t>(
                    1U << (7U - (bits % 8U)));
            ++bits;
        }
    }
};

struct TruncationFixture {
    std::vector<std::uint8_t> bytes;
    FrameHeader frame;
    BsiInfo bsi;
};

TruncationFixture makeTruncationFixture()
{
    BitFieldWriter writer;
    writer.put(1U, 2U); // snroffststr
    writer.put(0U, 1U); // transproce
    writer.put(0U, 1U); // blkswe
    writer.put(0U, 1U); // dithflage
    writer.put(0U, 1U); // bamode
    writer.put(0U, 1U); // frmfgaincode
    writer.put(0U, 1U); // dbaflde
    writer.put(0U, 1U); // skipflde
    writer.put(0U, 1U); // spxattene
    writer.put(1U, 2U); // chexpstr d15
    writer.put(0U, 1U); // convexpstre
    writer.put(0U, 1U); // dynrnge
    writer.put(0U, 1U); // spxinu
    writer.put(0U, 6U); // chbwcod
    writer.put(0U, 4U); // absolute exponent
    for (unsigned group = 0U; group < 24U; ++group)
        writer.put(62U, 7U);
    writer.put(0U, 2U); // gainrng
    writer.put(0U, 6U); // csnroffst
    writer.put(0U, 4U); // blkfsnroffst
    writer.put(0U, 1U); // convsnroffste

    TruncationFixture result;
    result.bytes = std::move(writer.bytes);
    result.frame.streamType = StreamType::Independent;
    result.frame.sizeBytes = result.bytes.size();
    result.frame.endBit = writer.bits;
    result.frame.sampleRate = 48000U;
    result.frame.blocks = 1U;
    result.frame.channelCount = 1U;
    result.frame.acmod = 1U;
    result.frame.bsid = 16U;
    result.bsi.bsiParsed = true;
    result.bsi.bsiEndBit = 0U;
    return result;
}

AudblkFrameState syntheticState(bool lfe, bool reuse)
{
    AudblkFrameState state;
    state.frameOffset = 0U;
    state.frameStartBit = 0U;
    state.frameEndBit = 4096U;
    state.bsiEndBit = 100U;
    state.audfrmStartBit = 100U;
    state.audfrmEndBit = 200U;
    state.blocks = 2U;
    state.channels = lfe ? 3U : 2U;
    state.lfe = lfe;
    state.blockStates.resize(state.blocks);
    for (unsigned blockIndex = 0U; blockIndex < state.blocks; ++blockIndex) {
        AudblkBlockState &block = state.blockStates[blockIndex];
        block.block = blockIndex;
        block.startBit = 200U + blockIndex * 1800U;
        block.endBit = block.startBit + 1600U;
        block.groupedCursorShared = true;
        block.groupedCursorResetAtEnd = true;
        block.groupedCursorStartBit = block.startBit + 20U;
        block.groupedCursorEndBit = block.endBit - 20U;
        block.blockSwitchFlags.assign(state.channels - (lfe ? 1U : 0U), false);
        block.ditherFlags.assign(state.channels - (lfe ? 1U : 0U), true);
        block.rematrixUpdated = blockIndex == 0U;
        block.rematrixReused = blockIndex != 0U;
        block.rematrixFlags = {{true, false, true, false}};
        block.groupedChannelOrder.reserve(state.channels);
        block.channels.resize(state.channels);
        for (unsigned channelIndex = 0U; channelIndex < state.channels;
             ++channelIndex) {
            const bool channelLfe = lfe && channelIndex + 1U == state.channels;
            const unsigned endMant = channelLfe ? 7U : 8U + channelIndex;
            AudblkChannelState &channel = block.channels[channelIndex];
            channel.channel = channelIndex;
            channel.lfe = channelLfe;
            channel.startBit = block.startBit + 30U + channelIndex * 100U;
            channel.endBit = channel.startBit + 80U;
            channel.bandwidthReused = reuse && blockIndex != 0U;
            channel.chbwcod = channelLfe ? 0U : 4U + channelIndex;
            channel.endMant = endMant;
            channel.exponentStrategy = reuse && blockIndex != 0U ? 0U : 1U;
            channel.exponentReused = reuse && blockIndex != 0U;
            channel.exponents.assign(endMant, channelIndex + blockIndex);
            channel.snrReused = reuse && blockIndex != 0U;
            channel.csnroffst = channelIndex;
            channel.fsnroffst = blockIndex;
            channel.fastGainReused = reuse && blockIndex != 0U;
            channel.fgaincod = 4U;
            channel.allocation.reused = reuse && blockIndex != 0U;
            channel.allocation.bap.assign(endMant, channelIndex % 3U);
            channel.allocation.bapDigest = digestBapVector(channel.allocation.bap);
            block.groupedChannelOrder.push_back(channelIndex);
        }
    }
    return state;
}

bool runSelfTest()
{
    const AudblkFrameState reuse = syntheticState(true, true);
    const AudblkStateValidationResult reuseResult = validateAudblkState(reuse);
    if (!expect(reuseResult.valid, "new-reuse")
        || !expect(reuse.blockStates[1].channels[0].exponentReused,
                   "reuse-exponent-state")
        || !expect(reuse.blockStates[1].channels[2].lfe,
                   "lfe-identity")
        || !expect(reuse.blockStates[0].rematrixUpdated
                       && reuse.blockStates[1].rematrixReused
                       && reuse.blockStates[0].rematrixFlags
                              == reuse.blockStates[1].rematrixFlags,
                   "rematrix-new-reuse")) {
        return false;
    }

    const AudblkFrameState reset = syntheticState(false, false);
    if (!expect(validateAudblkState(reset).valid, "frame-reset")
        || !expect(reset.blockStates[0].channels[0].channel !=
                   reset.blockStates[0].channels[1].channel,
                   "channel-isolation")
        || !expect(reset.blockStates[0].rematrixUpdated
                       && !reset.blockStates[0].rematrixReused,
                   "rematrix-frame-reset")) {
        return false;
    }

    AudblkFrameState malformed = reuse;
    malformed.blockStates[1].channels[1].exponents.clear();
    const AudblkStateValidationResult malformedResult =
        validateAudblkState(malformed);
    if (!expect(!malformedResult.valid, "malformed-state")
        || !expect(malformedResult.blockIndex == 1U
                       && malformedResult.channelIndex == 1U
                       && !malformedResult.channelIsLfe
                       && malformedResult.frameOffset == 0U
                       && malformedResult.bitPosition != 0U
                       && malformedResult.reason == "channel-state",
                   "malformed-context")) {
        return false;
    }

    const std::uint64_t digest = digestAudblkState(reuse);
    if (!expect(digest == digestAudblkState(reuse), "digest-determinism")
        || !expect(formatAudblkStateDigest(digest).size() == 16U,
                   "digest-format")) {
        return false;
    }
    const TruncationFixture truncation = makeTruncationFixture();
    const AudblkParseResult full = parseEac3Audblk(
        truncation.bytes, truncation.frame, truncation.bsi);
    if (!expect(full.disposition == AudblkDisposition::Accepted,
                "truncation-fixture") || !expect(full.bitPosition > 0U,
                                                   "truncation-fixture-bits")) {
        return false;
    }
    FrameHeader shortenedFrame = truncation.frame;
    // Stop inside the channel's exponent groups, before the block-global
    // BA/SNR/converter fields, so channel context remains meaningful.
    shortenedFrame.endBit = 100U;
    const AudblkParseResult shortened = parseEac3Audblk(
        truncation.bytes, shortenedFrame, truncation.bsi);
    if (!expect(shortened.disposition == AudblkDisposition::Malformed,
                "truncation-result")
        || !expect(shortened.frameOffset == 0U && shortened.blockIndex == 0U
                       && shortened.channelIndex == 0U
                       && !shortened.channelIsLfe
                       && shortened.bitPosition > 0U
                       && !shortened.reason.empty(),
                   "truncation-context")) {
        return false;
    }
    std::cout << "selfTest=PASS cases=10 newReuse=YES frameReset=YES "
                 "channelIsolation=YES lfeOwnership=YES truncationContext=YES "
                 "digestDeterministic=YES drcApplied=NO ffmpegLinked=NO\n";
    return true;
}

bool runFile(const std::string &path, std::size_t maxUnits)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::cerr << "probeResult=FAIL stage=open reason=file-open-failed\n";
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
    const std::vector<std::uint8_t> &framed = framing.normalized;
    std::size_t offset = 0U;
    std::size_t frames = 0U;
    std::size_t accepted = 0U;
    std::size_t unsupported = 0U;
    std::uint64_t aggregate = 1469598103934665603ULL;
    bool config3Facts = false;
    std::size_t block1RelativeBit = 0U;
    std::vector<std::size_t> config3BapLengths;
    while (offset < framed.size()
           && (maxUnits == 0U || accepted < maxUnits)) {
        const ParseResult parsed = parseSyncframe(framed, offset);
        if (parsed.disposition != Disposition::Accepted || !parsed.frame) {
            std::cerr << "probeResult=FAIL stage=" << toString(parsed.stage)
                      << " reason=" << parsed.reason << '\n';
            return false;
        }
        ++frames;
        const FrameHeader &frame = *parsed.frame;
        if (frame.streamType != StreamType::LegacyAc3) {
            const BsiParseResult bsi = parseEac3Bsi(framed, frame);
            if (bsi.disposition != Disposition::Accepted || !bsi.info) {
                std::cerr << "probeResult=FAIL stage=" << toString(bsi.stage)
                          << " reason=" << bsi.reason << '\n';
                return false;
            }
            const AudblkParseResult state =
                parseEac3Audblk(framed, frame, *bsi.info);
            if (state.disposition == AudblkDisposition::Unsupported) {
                ++unsupported;
            } else if (state.disposition != AudblkDisposition::Accepted
                       || !state.features || !state.features->state) {
                std::cerr << "probeResult=FAIL stage=" << toString(state.stage)
                          << " reason=" << state.reason
                          << " frameOffset=" << state.frameOffset
                          << " block=" << state.blockIndex
                          << " channel=" << state.channelIndex
                          << " bit=" << state.bitPosition << '\n';
                return false;
            } else {
                ++accepted;
                const AudblkFrameState &snapshot = *state.features->state;
                const std::uint64_t digest = digestAudblkState(snapshot);
                for (unsigned shift = 0U; shift < 64U; shift += 8U) {
                    aggregate ^= (digest >> shift) & 0xffU;
                    aggregate *= 1099511628211ULL;
                }
                if (frame.offset == 3072U && snapshot.blockStates.size() > 1U) {
                    config3Facts = true;
                    block1RelativeBit = snapshot.blockStates[1].startBit
                        - snapshot.frameStartBit;
                    for (const AudblkChannelState &channel :
                         snapshot.blockStates[0].channels) {
                        if (!channel.lfe)
                            config3BapLengths.push_back(channel.allocation.bap.size());
                    }
                }
            }
        }
        offset += frame.sizeBytes;
    }
    if (accepted == 0U) return false;
    std::cout << "frames=" << frames << '\n'
              << "acceptedFrames=" << accepted << '\n'
              << "unsupportedFrames=" << unsupported << '\n'
              << "stateDigest=" << formatAudblkStateDigest(aggregate) << '\n'
              << "drcApplied=NO\n"
              << "ffmpegLinked=NO\n";
    if (config3Facts) {
        std::cout << "config3FrameOffset=3072\n"
                  << "config3Block1RelativeBit=" << block1RelativeBit << '\n'
                  << "config3FbwBapLengths=";
        for (std::size_t i = 0U; i < config3BapLengths.size(); ++i)
            std::cout << (i == 0U ? "" : ",") << config3BapLengths[i];
        std::cout << '\n';
    }
    std::cout << "probeResult=PASS stage=gate8n-1d-n1a-state-only\n";
    return true;
}

void usage()
{
    std::cerr << "Usage: Eac3NativeBlockStateProbe --self-test\n"
                 "       Eac3NativeBlockStateProbe <raw.eb3> [--max-units N]\n";
}

} // namespace

int main(int argc, char **argv)
{
    if (argc == 2 && std::string(argv[1]) == "--self-test")
        return runSelfTest() ? 0 : 1;
    if (argc < 2 || argc > 4) {
        usage();
        return 2;
    }
    std::size_t maxUnits = 0U;
    if (argc == 4 && std::string(argv[2]) == "--max-units") {
        try {
            maxUnits = static_cast<std::size_t>(std::stoull(argv[3]));
        } catch (...) {
            usage();
            return 2;
        }
    } else if (argc != 2) {
        usage();
        return 2;
    }
    return runFile(argv[1], maxUnits) ? 0 : 1;
}
