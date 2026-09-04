#include "native-eac3-audblk.h"
#include "native-eac3-block-state.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace eac3native;

struct BitWriter {
    std::vector<std::uint8_t> bytes;
    std::size_t bits = 0U;

    void put(std::uint32_t value, unsigned width)
    {
        for (unsigned index = 0U; index < width; ++index) {
            if ((bits % 8U) == 0U) bytes.push_back(0U);
            const unsigned shift = width - index - 1U;
            if (((value >> shift) & 1U) != 0U)
                bytes.back() |= static_cast<std::uint8_t>(
                    1U << (7U - (bits % 8U)));
            ++bits;
        }
    }
};

bool expect(bool value, const char *name)
{
    if (!value) std::cerr << "selfTest=FAIL case=" << name << '\n';
    return value;
}

bool finiteVector(const std::vector<double> &values)
{
    for (double value : values)
        if (!std::isfinite(value)) return false;
    return true;
}

DitherSource diagnosticDither()
{
    return [](std::size_t index, unsigned exponent) {
        std::uint64_t value = 0x9e3779b97f4a7c15ULL
            ^ (static_cast<std::uint64_t>(index) * 0xd6e8feb86659fd93ULL)
            ^ (static_cast<std::uint64_t>(exponent) * 0xa0761d6478bd642fULL);
        value ^= value >> 30U;
        value *= 0xbf58476d1ce4e5b9ULL;
        value ^= value >> 27U;
        value *= 0x94d049bb133111ebULL;
        value ^= value >> 31U;
        const std::uint64_t mantissa = value >> 11U;
        const double unit = static_cast<double>(mantissa)
            / 9007199254740991.0;
        return unit * 2.0 - 1.0;
    };
}

MantissaDecodeResult decode(const std::vector<std::uint8_t> &bytes,
                            std::size_t validBits,
                            MantissaDecodeRequest request,
                            MantissaDecodeState *state = nullptr)
{
    MantissaBitReader reader(bytes, validBits);
    MantissaDecodeState local;
    MantissaDecodeState &cursor = state ? *state : local;
    return decodeMantissas(reader, request, cursor);
}

bool runSelfTest()
{
    std::size_t cases = 0U;

    // Exercise every BAP family through the same primitive used by audblk.
    const unsigned scalarBits[16] = {
        0U, 0U, 0U, 3U, 0U, 4U, 5U, 6U,
        7U, 8U, 9U, 10U, 11U, 12U, 14U, 16U};
    bool allBap = true;
    for (unsigned bap = 0U; bap <= 15U && allBap; ++bap) {
        BitWriter writer;
        std::vector<unsigned> baps;
        std::vector<unsigned> exponents;
        unsigned count = bap == 1U || bap == 2U ? 3U : bap == 4U ? 2U : 1U;
        for (unsigned index = 0U; index < count; ++index) {
            baps.push_back(bap);
            exponents.push_back(index);
        }
        writer.put(0U, scalarBits[bap] * (bap == 1U || bap == 2U
                                          ? 0U : 1U));
        if (bap == 1U) writer.put(0U, 5U);
        else if (bap == 2U) writer.put(0U, 7U);
        else if (bap == 4U) writer.put(0U, 7U);
        else if (bap != 0U) writer.put(0U, scalarBits[bap]);
        MantissaDecodeRequest request;
        request.bap = baps;
        request.exponents = exponents;
        request.endOfBlock = true;
        request.ditherSource = diagnosticDither();
        const MantissaDecodeResult result = decode(
            writer.bytes, writer.bits, request);
        allBap = result.disposition == MantissaDisposition::Accepted
            && result.transformCoefficients.size() == baps.size()
            && finiteVector(result.transformCoefficients);
    }
    if (!expect(allBap, "all-bap-families-finite")) return false;
    ++cases;

    // One grouped codeword is owned by the block cursor and may continue
    // into the next ordered channel without another bit read.
    BitWriter groupedWriter;
    groupedWriter.put(0U, 5U);
    MantissaBitReader groupedReader(groupedWriter.bytes, groupedWriter.bits);
    MantissaDecodeState groupedState;
    MantissaDecodeRequest first;
    first.bap = {1U}; first.exponents = {0U}; first.channelId = 0U;
    first.audioBlockId = 0U; first.endOfBlock = false;
    first.sharedBlockCursor = true;
    const MantissaDecodeResult firstGroup = decodeMantissas(
        groupedReader, first, groupedState);
    MantissaDecodeRequest continuation;
    continuation.bap = {1U, 1U}; continuation.exponents = {1U, 2U};
    continuation.channelId = 1U; continuation.audioBlockId = 0U;
    continuation.endOfBlock = true; continuation.sharedBlockCursor = true;
    const MantissaDecodeResult secondGroup = decodeMantissas(
        groupedReader, continuation, groupedState);
    if (!expect(firstGroup.disposition == MantissaDisposition::Accepted
                    && secondGroup.disposition == MantissaDisposition::Accepted
                    && secondGroup.bitsConsumed == 0U
                    && groupedReader.bitOffset() == 5U
                    && !groupedState.hasContext,
                "grouped-continuation-cross-channel")) return false;
    ++cases;

    // LFE ownership follows the same block cursor contract.
    MantissaBitReader lfeReader(groupedWriter.bytes, groupedWriter.bits);
    MantissaDecodeState lfeState;
    MantissaDecodeRequest lfeFirst = first;
    lfeFirst.channelId = 2U;
    const MantissaDecodeResult lfeStart = decodeMantissas(
        lfeReader, lfeFirst, lfeState);
    MantissaDecodeRequest lfeEnd = continuation;
    lfeEnd.channelId = 3U;
    const MantissaDecodeResult lfeFinish = decodeMantissas(
        lfeReader, lfeEnd, lfeState);
    if (!expect(lfeStart.disposition == MantissaDisposition::Accepted
                    && lfeFinish.disposition == MantissaDisposition::Accepted
                    && lfeReader.bitOffset() == 5U,
                "lfe-grouped-ownership")) return false;
    ++cases;

    // bap=0 is exact zero without dither and deterministic with an injected
    // diagnostic source.  This source is intentionally not a normative LFSR.
    MantissaDecodeRequest noDither;
    noDither.bap = {0U, 0U, 0U}; noDither.exponents = {0U, 1U, 2U};
    noDither.endOfBlock = true;
    const MantissaDecodeResult zeros = decode({}, 0U, noDither);
    MantissaDecodeRequest withDither = noDither;
    withDither.dithflag = true;
    withDither.ditherSource = diagnosticDither();
    const MantissaDecodeResult ditherA = decode({}, 0U, withDither);
    const MantissaDecodeResult ditherB = decode({}, 0U, withDither);
    if (!expect(zeros.disposition == MantissaDisposition::Accepted
                    && zeros.transformCoefficients == std::vector<double>(3U, 0.0)
                    && ditherA.transformCoefficients == ditherB.transformCoefficients
                    && ditherA.ditherUsed
                    && finiteVector(ditherA.transformCoefficients),
                "bap0-dither-off-on-deterministic")) return false;
    ++cases;

    // The reference source follows the standard's permitted random-sequence
    // boundary: bounded uniform scaling, explicit reset, and repeatability.
    ReferenceDitherSource reference(0x123456789abcdef1ULL);
    const std::uint64_t initialState = reference.state();
    std::vector<double> referenceValues;
    referenceValues.reserve(4096U);
    for (std::size_t index = 0U; index < 4096U; ++index)
        referenceValues.push_back(reference.next(index, index % 25U));
    const std::uint64_t advancedState = reference.state();
    const double mean = std::accumulate(referenceValues.begin(),
                                        referenceValues.end(), 0.0)
        / static_cast<double>(referenceValues.size());
    reference.reset(0x123456789abcdef1ULL);
    bool repeated = true;
    for (std::size_t index = 0U; index < referenceValues.size(); ++index)
        repeated = repeated
            && reference.next(index, index % 25U) == referenceValues[index];
    const bool referenceRange = std::all_of(
        referenceValues.begin(), referenceValues.end(), [](double value) {
            return std::isfinite(value) && value >= -0.7071067811865475
                && value <= 0.7071067811865475;
        });
    if (!expect(initialState != advancedState && repeated
                    && reference.generatedSamples() == referenceValues.size()
                    && std::abs(mean) < 0.05 && referenceRange,
                "reference-dither-range-distribution-reset")) return false;
    ++cases;

    // Reserved grouped composites are transactional and fail closed in
    // coefficient mode; inventory mode may retain its warning policy.
    BitWriter reservedWriter;
    reservedWriter.put(31U, 5U); // bap=1 max code is 26.
    MantissaBitReader reservedReader(reservedWriter.bytes, reservedWriter.bits);
    MantissaDecodeState reservedState;
    MantissaDecodeRequest reserved;
    reserved.bap = {1U, 1U, 1U}; reserved.exponents = {0U, 0U, 0U};
    reserved.endOfBlock = true; reserved.rejectReservedComposite = true;
    const MantissaDecodeResult reservedResult = decodeMantissas(
        reservedReader, reserved, reservedState);
    if (!expect(reservedResult.disposition == MantissaDisposition::Unsupported
                    && reservedResult.reason == "reserved-group-code"
                    && reservedReader.bitOffset() == 0U
                    && !reservedState.hasContext,
                "reserved-code-structured-failure")) return false;
    ++cases;

    // Truncation is also transactional, retaining no partial cursor state.
    BitWriter shortWriter;
    shortWriter.put(0U, 8U);
    MantissaBitReader shortReader(shortWriter.bytes, shortWriter.bits);
    MantissaDecodeState shortState;
    MantissaDecodeRequest shortRequest;
    shortRequest.bap = {15U}; shortRequest.exponents = {0U};
    const MantissaDecodeResult truncated = decodeMantissas(
        shortReader, shortRequest, shortState);
    if (!expect(truncated.disposition == MantissaDisposition::Malformed
                    && truncated.reason == "mantissa-truncated"
                    && shortReader.bitOffset() == 0U
                    && !shortState.hasContext,
                "truncation-transactional")) return false;
    ++cases;

    std::cout << "selfTest=PASS cases=" << cases
              << " allBapFamilies=YES groupedContinuation=YES"
              << " lfeOwnership=YES bap0Dither=YES reservedStructured=YES"
              << " truncationContext=YES resetDeterminism=YES"
              << " drcApplied=NO ffmpegLinked=NO\n";
    return true;
}

std::uint64_t combine(std::uint64_t digest, std::uint64_t value)
{
    for (unsigned shift = 0U; shift < 64U; shift += 8U) {
        digest ^= (value >> shift) & 0xffU;
        digest *= 1099511628211ULL;
    }
    return digest;
}

bool sameBoundaries(const AudblkFrameState &inventory,
                    const AudblkFrameState &coefficients)
{
    if (inventory.frameStartBit != coefficients.frameStartBit
        || inventory.frameEndBit != coefficients.frameEndBit
        || inventory.blockStates.size() != coefficients.blockStates.size())
        return false;
    for (std::size_t block = 0U; block < inventory.blockStates.size(); ++block) {
        const AudblkBlockState &left = inventory.blockStates[block];
        const AudblkBlockState &right = coefficients.blockStates[block];
        if (left.startBit != right.startBit || left.endBit != right.endBit
            || left.channels.size() != right.channels.size()) return false;
        for (std::size_t channel = 0U; channel < left.channels.size(); ++channel) {
            const AudblkChannelState &a = left.channels[channel];
            const AudblkChannelState &b = right.channels[channel];
            if (a.coefficientStartBit != b.coefficientStartBit
                || a.coefficientEndBit != b.coefficientEndBit
                || a.endMant != b.endMant)
                return false;
        }
    }
    return true;
}

bool runFile(const std::string &path, std::size_t maxFrames,
             std::size_t dumpFrameOffset, std::size_t dumpBlock)
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
    ReferenceDitherSource dither;
    const std::vector<std::uint8_t> &framed = framing.normalized;
    std::size_t offset = 0U;
    std::size_t frames = 0U;
    std::size_t accepted = 0U;
    std::size_t unsupported = 0U;
    std::size_t compared = 0U;
    bool exactBitEnds = true;
    bool finite = true;
    bool ditherContinuity = true;
    std::uint64_t coefficientDigest = 1469598103934665603ULL;
    std::uint64_t stateDigest = 1469598103934665603ULL;
    while (offset < framed.size() && (maxFrames == 0U || frames < maxFrames)) {
        const ParseResult parsed = parseSyncframe(framed, offset);
        if (parsed.disposition != Disposition::Accepted || !parsed.frame) {
            std::cerr << "probeResult=FAIL stage=" << toString(parsed.stage)
                      << " reason=" << parsed.reason << '\n';
            return false;
        }
        ++frames;
        const FrameHeader &frame = *parsed.frame;
        if (frame.streamType == StreamType::LegacyAc3) {
            offset += frame.sizeBytes;
            continue;
        }
        const BsiParseResult bsi = parseEac3Bsi(framed, frame);
        if (bsi.disposition != Disposition::Accepted || !bsi.info) {
            std::cerr << "probeResult=FAIL stage=" << toString(bsi.stage)
                      << " reason=" << bsi.reason << '\n';
            return false;
        }
        const AudblkParseResult inventory =
            parseEac3Audblk(framed, frame, *bsi.info);
        if (inventory.disposition == AudblkDisposition::Unsupported) {
            ++unsupported;
            std::cout << "unsupportedFrameOffset=" << frame.offset
                      << " reason=" << inventory.reason
                      << " bit=" << inventory.bitPosition << '\n';
            offset += frame.sizeBytes;
            continue;
        }
        if (inventory.disposition != AudblkDisposition::Accepted
            || !inventory.features || !inventory.features->state) {
            std::cerr << "probeResult=FAIL stage=" << toString(inventory.stage)
                      << " reason=" << inventory.reason
                      << " frameOffset=" << inventory.frameOffset
                      << " block=" << inventory.blockIndex
                      << " channel=" << inventory.channelIndex
                      << " bit=" << inventory.bitPosition << '\n';
            return false;
        }
        const std::uint64_t ditherStateBefore = dither.state();
        const std::size_t ditherSamplesBefore = dither.generatedSamples();
        const AudblkParseResult coefficients = parseEac3AudblkCoefficients(
            framed, frame, *bsi.info, dither);
        if (coefficients.disposition != AudblkDisposition::Accepted
            || !coefficients.features || !coefficients.features->state) {
            std::cerr << "probeResult=FAIL stage=" << toString(coefficients.stage)
                      << " reason=" << coefficients.reason
                      << " frameOffset=" << coefficients.frameOffset
                      << " block=" << coefficients.blockIndex
                      << " channel=" << coefficients.channelIndex
                      << " bit=" << coefficients.bitPosition << '\n';
            return false;
        }
        ++accepted;
        const AudblkFrameState &inventoryState = *inventory.features->state;
        const AudblkFrameState &coefficientState = *coefficients.features->state;
        if (frame.offset == dumpFrameOffset
            && dumpFrameOffset != static_cast<std::size_t>(-1)
            && dumpBlock < coefficientState.blockStates.size()) {
            const AudblkBlockState &dump = coefficientState.blockStates[dumpBlock];
            for (const AudblkChannelState &channel : dump.channels) {
                for (std::size_t index = 0U; index < channel.coefficients.size();
                     ++index) {
                    std::cout << "N2A2_NATIVE frameOffset=" << frame.offset
                              << " block=" << dumpBlock
                              << " ch=" << channel.channel
                              << " freq=" << index
                              << " bap=" << channel.allocation.bap[index]
                              << " exp=" << channel.exponents[index]
                              << " coeff=" << channel.coefficients[index]
                              << " scaled=" << channel.coefficients[index]
                                   * 16777216.0 << '\n';
                }
            }
        }
        ditherContinuity = ditherContinuity
            && coefficientState.ditherStateStart == ditherStateBefore
            && coefficientState.ditherStateEnd == dither.state()
            && coefficientState.ditherSamples
                   == dither.generatedSamples() - ditherSamplesBefore;
        exactBitEnds = exactBitEnds && inventory.bitPosition == coefficients.bitPosition
            && sameBoundaries(inventoryState, coefficientState);
        stateDigest = combine(stateDigest, digestAudblkState(inventoryState));
        for (const AudblkBlockState &block : coefficientState.blockStates) {
            for (const AudblkChannelState &channel : block.channels) {
                finite = finite && channel.coefficientsDecoded
                    && channel.coefficients.size() == channel.endMant
                    && finiteVector(channel.coefficients);
                coefficientDigest = combine(coefficientDigest,
                                            channel.coefficientDigest);
                coefficientDigest = combine(coefficientDigest,
                                            static_cast<std::uint64_t>(channel.coefficientStartBit));
                coefficientDigest = combine(coefficientDigest,
                                            static_cast<std::uint64_t>(channel.coefficientEndBit));
            }
        }
        ++compared;
        offset += frame.sizeBytes;
    }
    if (frames == 0U || accepted == 0U || compared == 0U) return false;
    std::cout << "inputBytes=" << bytes.size() << '\n'
              << "normalizedBytes=" << framed.size() << '\n'
              << "frames=" << frames << '\n'
              << "acceptedFrames=" << accepted << '\n'
              << "unsupportedFrames=" << unsupported << '\n'
              << "comparedFrames=" << compared << '\n'
              << "bitEndEquality=" << (exactBitEnds ? "PASS" : "FAIL") << '\n'
              << "coefficientFinite=" << (finite ? "YES" : "NO") << '\n'
              << "ditherStateContinuity="
              << (ditherContinuity ? "PASS" : "FAIL") << '\n'
              << "stateDigest=" << formatAudblkStateDigest(stateDigest) << '\n'
              << "coefficientDigest=" << formatAudblkStateDigest(coefficientDigest) << '\n'
              << "drcApplied=NO\nffmpegLinked=NO\n"
              << "probeResult="
              << (exactBitEnds && finite && ditherContinuity ? "PASS" : "FAIL")
              << " stage=gate8n-2b-n2a2-coefficient-state\n";
    return exactBitEnds && finite && ditherContinuity;
}

void usage()
{
    std::cerr << "Usage: Eac3NativeCoefficientProbe --self-test\n"
                 "       Eac3NativeCoefficientProbe <raw.eac3|raw.ec3|raw.eb3> [--max-frames N] [--dump-frame-offset N --dump-block N]\n";
}

} // namespace

int main(int argc, char **argv)
{
    if (argc == 2 && std::string(argv[1]) == "--self-test")
        return runSelfTest() ? 0 : 1;
    if (argc < 2 || argc > 8) {
        usage();
        return 2;
    }
    std::size_t maxFrames = 0U;
    std::size_t dumpFrameOffset = static_cast<std::size_t>(-1);
    std::size_t dumpBlock = 0U;
    for (int index = 2; index < argc; ++index) {
        const std::string option = argv[index];
        if ((option == "--max-frames" || option == "--dump-frame-offset"
             || option == "--dump-block") && index + 1 < argc) {
            try {
                const std::size_t value =
                    static_cast<std::size_t>(std::stoull(argv[++index]));
                if (option == "--max-frames") maxFrames = value;
                else if (option == "--dump-frame-offset") dumpFrameOffset = value;
                else dumpBlock = value;
            } catch (...) {
                usage();
                return 2;
            }
        } else {
            usage();
            return 2;
        }
    }
    return runFile(argv[1], maxFrames, dumpFrameOffset, dumpBlock) ? 0 : 1;
}
