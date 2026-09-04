#include "native-eac3-bsi.h"

#include <algorithm>
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
        const std::size_t position = bit + i;
        const std::uint8_t mask = static_cast<std::uint8_t>(
            1U << (7U - (position % 8U)));
        if (((value >> (count - i - 1U)) & 1U) != 0U) {
            bytes[position / 8U] |= mask;
        } else {
            bytes[position / 8U] &= static_cast<std::uint8_t>(~mask);
        }
    }
}

class BitWriter {
public:
    explicit BitWriter(std::vector<std::uint8_t> *bytes, std::size_t bit)
        : bytes_(bytes), bit_(bit) {}

    void put(unsigned count, unsigned value)
    {
        setBits(*bytes_, bit_, count, value);
        bit_ += count;
    }

    std::size_t position() const { return bit_; }

private:
    std::vector<std::uint8_t> *bytes_;
    std::size_t bit_;
};

std::vector<std::uint8_t> synthetic(unsigned streamType, unsigned sid,
                                    unsigned blocksCode, unsigned acmod,
                                    bool lfe, const char *body)
{
    std::vector<std::uint8_t> bytes(256U, 0);
    bytes[0] = 0x0b;
    bytes[1] = 0x77;
    setBits(bytes, 16, 2, streamType);
    setBits(bytes, 18, 3, sid);
    setBits(bytes, 21, 11, 127); // 256-byte bounded test frame.
    setBits(bytes, 32, 2, 0); // 48 kHz
    setBits(bytes, 34, 2, blocksCode);
    setBits(bytes, 36, 3, acmod);
    setBits(bytes, 39, 1, lfe ? 1U : 0U);
    setBits(bytes, 40, 5, 16);
    (void)body;
    return bytes;
}

std::vector<std::uint8_t> dependentConfig4()
{
    auto bytes = synthetic(1, 0, 3, 6, false, "config4");
    BitWriter writer(&bytes, 45U);
    writer.put(5, 22); // dialnorm
    writer.put(1, 1); // compre
    writer.put(8, 255); // compr
    writer.put(1, 1); // chanmape
    writer.put(16, 0xA010); // L, R, Vhl/Vhr = four coded channels
    writer.put(1, 0); // mixmdate
    writer.put(1, 0); // infomdate
    writer.put(1, 1); // addbsie
    writer.put(6, 1); // addbsil => 16 bits
    writer.put(16, 0x55aa); // addbsi payload
    return bytes;
}

std::vector<std::uint8_t> mixdef3Frame()
{
    auto bytes = synthetic(0, 0, 3, 2, false, "mixdef3");
    BitWriter writer(&bytes, 45U);
    writer.put(5, 0); // dialnorm
    writer.put(1, 0); // compre
    writer.put(1, 1); // mixmdate
    writer.put(1, 0); // pgmscle
    writer.put(1, 0); // extpgmscle
    writer.put(2, 3); // mixdef option 4
    writer.put(5, 0); // mixdeflen => 16 total bits
    writer.put(1, 0); // mixdata2e
    writer.put(1, 0); // mixdata3e
    writer.put(14, 0); // residual mixdata
    writer.put(3, 0); // mixdatafill: absolute cursor 77 -> byte 80
    writer.put(1, 0); // frmmixcfginfoe
    writer.put(1, 0); // infomdate
    writer.put(1, 0); // addbsie
    return bytes;
}

std::vector<std::uint8_t> mixdef3AlignedFrame()
{
    auto bytes = synthetic(0, 0, 3, 0, false, "mixdef3-aligned");
    BitWriter writer(&bytes, 45U);
    writer.put(5, 0); // dialnorm
    writer.put(1, 0); // compre
    writer.put(5, 0); // dialnorm2
    writer.put(1, 0); // compr2e
    writer.put(1, 1); // mixmdate
    writer.put(1, 1); // pgmscle
    writer.put(6, 0); // pgmscl
    writer.put(1, 1); // pgmscl2e
    writer.put(6, 0); // pgmscl2
    writer.put(1, 0); // extpgmscle
    writer.put(2, 3); // mixdef option 4
    writer.put(5, 0); // mixdeflen => 16 total bits
    writer.put(1, 0); // mixdata2e
    writer.put(1, 0); // mixdata3e
    writer.put(14, 0); // residual mixdata
    // No fill bits: the absolute cursor is already byte aligned.
    writer.put(1, 0); // paninfoe
    writer.put(1, 0); // paninfo2e
    writer.put(1, 0); // frmmixcfginfoe
    writer.put(1, 0); // infomdate
    writer.put(1, 0); // addbsie
    return bytes;
}

std::vector<std::uint8_t> infomdateFrame()
{
    auto bytes = synthetic(0, 0, 3, 2, false, "infomdate");
    BitWriter writer(&bytes, 45U);
    writer.put(5, 0); // dialnorm
    writer.put(1, 0); // compre
    writer.put(1, 0); // mixmdate
    writer.put(1, 1); // infomdate
    writer.put(3, 0); // bsmod
    writer.put(1, 0); // copyrightb
    writer.put(1, 0); // origbs
    writer.put(2, 0); // dsurmod
    writer.put(2, 0); // dheadphonmod
    writer.put(1, 1); // audprodie
    writer.put(5, 0); // mixlevel
    writer.put(2, 0); // roomtyp
    writer.put(1, 0); // adconvtyp
    writer.put(1, 0); // sourcefscod
    writer.put(1, 0); // addbsie
    return bytes;
}

std::vector<std::uint8_t> lfeMixFrame(bool present, unsigned code)
{
    auto bytes = synthetic(0, 0, 3, 2, true, "lfe-mix");
    BitWriter writer(&bytes, 45U);
    writer.put(5, 0); // dialnorm
    writer.put(1, 0); // compre
    writer.put(1, 1); // mixmdate
    writer.put(1, present ? 1U : 0U); // lfemixlevcode
    if (present) writer.put(5, code); // lfemixlevcod
    writer.put(1, 0); // pgmscle
    writer.put(1, 0); // extpgmscle
    writer.put(2, 0); // mixdef
    writer.put(1, 0); // frmmixcfginfoe
    writer.put(1, 0); // infomdate
    writer.put(1, 0); // addbsie
    return bytes;
}

bool expect(bool value, const char *name)
{
    if (!value) {
        std::cerr << "selfTest=FAIL case=" << name << '\n';
        return false;
    }
    return true;
}

bool runSelfTest()
{
    std::size_t cases = 0;
    const auto config4 = dependentConfig4();
    const ParseResult parsed = parseSyncframe(config4, 0);
    if (!expect(parsed.disposition == Disposition::Accepted && parsed.frame,
                "config4-core-parse")) {
        return false;
    }
    const BsiParseResult config4Bsi = parseEac3Bsi(config4, *parsed.frame);
    if (!expect(config4Bsi.disposition == Disposition::Accepted
                    && config4Bsi.info && config4Bsi.info->bsiParsed
                    && config4Bsi.info->bsiEndBit == 101U
                    && config4Bsi.info->dialnorm == 22U
                    && config4Bsi.info->compre
                    && config4Bsi.info->comprPresent
                    && config4Bsi.info->compr == 255U
                    && config4Bsi.info->chanmap == 0xA010U
                    && config4Bsi.info->chanmapChannelWeight == 4U
                    && config4Bsi.info->mixmdate == false
                    && config4Bsi.info->infomdate == false
                    && config4Bsi.info->addbsie
                    && config4Bsi.info->addbsil == 1U,
                "config4-bsi-contract")) {
        return false;
    }
    ++cases;

    const auto lfeMixPresent = lfeMixFrame(true, 0U);
    const ParseResult lfeMixHeader = parseSyncframe(lfeMixPresent, 0);
    const BsiParseResult lfeMixResult =
        parseEac3Bsi(lfeMixPresent, *lfeMixHeader.frame);
    if (!expect(lfeMixResult.disposition == Disposition::Accepted
                    && lfeMixResult.info
                    && lfeMixResult.info->lfemixlevcode
                    && lfeMixResult.info->lfemixlevcod == 0U
                    && lfeMixResult.info->bsiEndBit == 65U,
                "lfe-mix-level-presence-code-zero")) {
        return false;
    }
    ++cases;

    const auto lfeMixAbsent = lfeMixFrame(false, 0U);
    const ParseResult lfeMixAbsentHeader = parseSyncframe(lfeMixAbsent, 0);
    const BsiParseResult lfeMixAbsentResult =
        parseEac3Bsi(lfeMixAbsent, *lfeMixAbsentHeader.frame);
    if (!expect(lfeMixAbsentResult.disposition == Disposition::Accepted
                    && lfeMixAbsentResult.info
                    && !lfeMixAbsentResult.info->lfemixlevcode
                    && lfeMixAbsentResult.info->lfemixlevcod == 0U
                    && lfeMixAbsentResult.info->bsiEndBit == 60U,
                "lfe-mix-level-absent-disables-downmix")) {
        return false;
    }
    ++cases;

    const auto lfeMixRange = lfeMixFrame(true, 31U);
    const ParseResult lfeMixRangeHeader = parseSyncframe(lfeMixRange, 0);
    const BsiParseResult lfeMixRangeResult =
        parseEac3Bsi(lfeMixRange, *lfeMixRangeHeader.frame);
    if (!expect(lfeMixRangeResult.disposition == Disposition::Accepted
                    && lfeMixRangeResult.info
                    && lfeMixRangeResult.info->lfemixlevcod == 31U,
                "lfe-mix-level-five-bit-range")) {
        return false;
    }
    ++cases;

    const BsiParseResult lfeMixReuseResult =
        parseEac3Bsi(lfeMixPresent, *lfeMixHeader.frame);
    if (!expect(lfeMixReuseResult.disposition == Disposition::Accepted
                    && lfeMixReuseResult.info
                    && lfeMixReuseResult.info->lfemixlevcode
                    && lfeMixReuseResult.info->lfemixlevcod
                        == lfeMixResult.info->lfemixlevcod,
                "lfe-mix-level-frame-reuse-stable")) {
        return false;
    }
    ++cases;

    std::vector<std::uint8_t> prefixed(3U, 0x5aU);
    prefixed.insert(prefixed.end(), config4.begin(), config4.end());
    const ParseResult prefixedHeader = parseSyncframe(prefixed, 3U);
    const BsiParseResult prefixedBsi =
        parseEac3Bsi(prefixed, *prefixedHeader.frame);
    if (!expect(prefixedBsi.disposition == Disposition::Accepted
                    && prefixedBsi.info
                    && prefixedBsi.info->bsiEndBit == 101U,
                "frame-relative-bsi-end")) {
        return false;
    }
    ++cases;

    auto badChanmap = config4;
    setBits(badChanmap, 45U + 5U + 1U + 8U + 1U, 16, 0xA000);
    const ParseResult badChanmapHeader = parseSyncframe(badChanmap, 0);
    const BsiParseResult badChanmapResult =
        parseEac3Bsi(badChanmap, *badChanmapHeader.frame);
    if (!expect(badChanmapResult.disposition == Disposition::Malformed
                    && badChanmapResult.reason
                           == "chanmap-channel-count-mismatch",
                "chanmap-weight-mismatch")) {
        return false;
    }
    ++cases;

    auto truncated = config4;
    FrameHeader truncatedFrame = *parsed.frame;
    truncatedFrame.endBit = 100U;
    const BsiParseResult truncatedResult =
        parseEac3Bsi(truncated, truncatedFrame);
    if (!expect(truncatedResult.disposition == Disposition::Malformed,
                "bsi-boundary-truncation")) {
        return false;
    }
    ++cases;

    const auto mixdef3 = mixdef3Frame();
    const ParseResult mixHeader = parseSyncframe(mixdef3, 0);
    const BsiParseResult mixResult = parseEac3Bsi(mixdef3, *mixHeader.frame);
    if (!expect(mixResult.disposition == Disposition::Accepted
                    && mixResult.info && mixResult.info->mixdef == 3U
                    && mixResult.info->mixdeflen == 0U
                    && mixResult.info->mixdataFillBits == 3U
                    && mixResult.info->bsiEndBit == 83U,
                "mixdef3-boundary")) {
        return false;
    }
    ++cases;

    const auto alignedMixdef3 = mixdef3AlignedFrame();
    const ParseResult alignedHeader = parseSyncframe(alignedMixdef3, 0);
    const BsiParseResult alignedResult =
        parseEac3Bsi(alignedMixdef3, *alignedHeader.frame);
    if (!expect(alignedResult.disposition == Disposition::Accepted
                    && alignedResult.info
                    && alignedResult.info->mixdataFillBits == 0U
                    && alignedResult.info->bsiEndBit == 101U,
                "mixdef3-zero-fill")) {
        return false;
    }
    ++cases;

    const auto infomdate = infomdateFrame();
    const ParseResult infoHeader = parseSyncframe(infomdate, 0);
    const BsiParseResult infoResult =
        parseEac3Bsi(infomdate, *infoHeader.frame);
    if (!expect(infoResult.disposition == Disposition::Accepted
                    && infoResult.info && infoResult.info->infomdate
                    && infoResult.info->bsiEndBit == 73U,
                "infomdate-branch")) {
        return false;
    }
    ++cases;

    auto type2 = synthetic(2, 0, 3, 2, false, "type2");
    {
        BitWriter writer(&type2, 45U);
        writer.put(5, 0); // dialnorm
        writer.put(1, 0); // compre
        writer.put(1, 0); // mixmdate
        writer.put(1, 0); // infomdate
        writer.put(6, 0); // frmsizecod follows implicit blkid=1
        writer.put(1, 0); // addbsie; blkid is implicit for six blocks
    }
    const ParseResult type2Header = parseSyncframe(type2, 0);
    const BsiParseResult type2Result = parseEac3Bsi(type2, *type2Header.frame);
    if (!expect(type2Result.disposition == Disposition::Accepted
                    && type2Result.info && !type2Result.info->blkidPresent
                    && type2Result.info->blkid
                    && type2Result.info->frmsizecodPresent
                    && type2Result.info->frmsizecod == 0U,
                "type2-six-block-implicit-blkid")) {
        return false;
    }
    ++cases;

    FrameHeader legacy = *parsed.frame;
    legacy.streamType = StreamType::LegacyAc3;
    legacy.bsid = 6;
    const BsiParseResult legacyResult = parseEac3Bsi(config4, legacy);
    if (!expect(legacyResult.disposition == Disposition::Accepted
                    && legacyResult.info && !legacyResult.info->bsiParsed
                    && legacyResult.info->legacyAc3
                    && legacyResult.reason == "legacy-ac3-bsi-not-parsed",
                "legacy-bsi-not-parsed")) {
        return false;
    }
    ++cases;

    std::cout << "selfTest=PASS cases=" << cases
              << " implementation=native-eac3-bsi-gate8n-1b\n";
    return true;
}

struct Report {
    std::size_t frames = 0;
    std::size_t dependentBsi = 0;
    std::size_t legacyFrames = 0;
    std::size_t chanmapA010 = 0;
    std::size_t accessUnits = 0;
    std::size_t bsiFailures = 0;
    std::size_t lfeMixPresent = 0;
    std::size_t lfeMixAbsent = 0;
    unsigned lfeMixMinimum = 32U;
    unsigned lfeMixMaximum = 0U;
    bool lfeMixStable = true;
    bool havePreviousLfeMix = false;
    unsigned previousLfeMix = 0U;
};

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
    const std::vector<std::uint8_t> &framedBytes = framing.normalized;
    Report report;
    AccessUnitAssembler assembler;
    std::size_t offset = 0;
    bool boundedStop = false;
    while (offset < framedBytes.size() && report.accessUnits < maxUnits) {
        const ParseResult parsed = parseSyncframe(framedBytes, offset);
        if (parsed.disposition != Disposition::Accepted || !parsed.frame) {
            std::cerr << "probeResult=FAIL stage=core reason=" << parsed.reason
                      << " offset=" << offset << '\n';
            return false;
        }
        const FrameHeader &frame = *parsed.frame;
        const BsiParseResult bsi = parseEac3Bsi(framedBytes, frame);
        if (bsi.disposition != Disposition::Accepted || !bsi.info) {
            ++report.bsiFailures;
            std::cerr << "probeResult=FAIL stage=bsi reason=" << bsi.reason
                      << " offset=" << offset << '\n';
            return false;
        }
        const std::size_t framesBefore = report.frames;
        const std::size_t dependentBefore = report.dependentBsi;
        const std::size_t legacyBefore = report.legacyFrames;
        const std::size_t chanmapBefore = report.chanmapA010;
        const std::size_t lfeMixPresentBefore = report.lfeMixPresent;
        const std::size_t lfeMixAbsentBefore = report.lfeMixAbsent;
        const unsigned lfeMixMinimumBefore = report.lfeMixMinimum;
        const unsigned lfeMixMaximumBefore = report.lfeMixMaximum;
        const bool lfeMixStableBefore = report.lfeMixStable;
        const bool havePreviousLfeMixBefore = report.havePreviousLfeMix;
        const unsigned previousLfeMixBefore = report.previousLfeMix;
        ++report.frames;
        if (bsi.info->legacyAc3) {
            ++report.legacyFrames;
        } else if (parsed.frame->dependent) {
            ++report.dependentBsi;
            if (bsi.info->chanmap == 0xA010U) {
                ++report.chanmapA010;
            }
        }
        if (bsi.info->lfeon && bsi.info->mixmdate) {
            if (bsi.info->lfemixlevcode) {
                ++report.lfeMixPresent;
                report.lfeMixMinimum = std::min(report.lfeMixMinimum,
                                                bsi.info->lfemixlevcod);
                report.lfeMixMaximum = std::max(report.lfeMixMaximum,
                                                bsi.info->lfemixlevcod);
                if (report.havePreviousLfeMix
                    && report.previousLfeMix != bsi.info->lfemixlevcod) {
                    report.lfeMixStable = false;
                }
                report.previousLfeMix = bsi.info->lfemixlevcod;
                report.havePreviousLfeMix = true;
            } else {
                ++report.lfeMixAbsent;
            }
        }
        const ProcessResult processed = assembler.process(parsed);
        if (processed.disposition != Disposition::Accepted
            || processed.flow != FlowStatus::None) {
            std::cerr << "probeResult=FAIL stage=assembly reason="
                      << processed.reason << " offset=" << offset << '\n';
            return false;
        }
        if (processed.completed) {
            ++report.accessUnits;
        }
        offset += frame.sizeBytes;
        if (processed.completed && report.accessUnits >= maxUnits) {
            // The boundary frame is the first frame of the next AU.  Keep it
            // in the assembler's pending state, but exclude it from the
            // bounded report exactly as the N1a probe does.
            report.frames = framesBefore;
            report.dependentBsi = dependentBefore;
            report.legacyFrames = legacyBefore;
            report.chanmapA010 = chanmapBefore;
            report.lfeMixPresent = lfeMixPresentBefore;
            report.lfeMixAbsent = lfeMixAbsentBefore;
            report.lfeMixMinimum = lfeMixMinimumBefore;
            report.lfeMixMaximum = lfeMixMaximumBefore;
            report.lfeMixStable = lfeMixStableBefore;
            report.havePreviousLfeMix = havePreviousLfeMixBefore;
            report.previousLfeMix = previousLfeMixBefore;
            boundedStop = true;
            break;
        }
    }
    if (!boundedStop) {
        const ProcessResult flushed = assembler.flush();
        if (flushed.disposition != Disposition::Accepted
            || flushed.flow != FlowStatus::None) {
            std::cerr << "probeResult=FAIL stage=flush reason="
                      << flushed.reason << '\n';
            return false;
        }
        if (flushed.completed) {
            ++report.accessUnits;
        }
    }
    if (report.accessUnits != maxUnits || report.bsiFailures != 0U) {
        std::cerr << "probeResult=FAIL stage=summary reason=short-input\n";
        return false;
    }
    std::cout << "frames=" << report.frames << '\n'
              << "accessUnits=" << report.accessUnits << '\n'
              << "eb3WrapperCountTotal="
              << (framing.wrapped ? framing.wrapperCount : 0U) << '\n'
              << "eb3WrapperCountCovered="
              << (framing.wrapped
                      ? std::min(framing.wrapperCount, report.accessUnits)
                      : 0U)
              << '\n'
              << "dependentBsi=" << report.dependentBsi << '\n'
              << "legacyFrames=" << report.legacyFrames << '\n'
              << "chanmapA010=" << report.chanmapA010 << '\n'
              << "lfeMixLevelCodePresent=" << report.lfeMixPresent << '\n'
              << "lfeMixLevelCodeAbsent=" << report.lfeMixAbsent << '\n'
              << "lfeMixLevelCodeRange="
              << (report.lfeMixPresent == 0U ? "NONE"
                  : std::to_string(report.lfeMixMinimum) + '-'
                      + std::to_string(report.lfeMixMaximum)) << '\n'
              << "lfeMixLevelCodeStable="
              << (report.lfeMixStable ? "PASS" : "FAIL") << '\n'
              << "lfeDownmixPolicy=NO_MIX reason=output-mode-or-lfe-disabled-not-proven\n"
              << "probeResult=PASS stage=gate8n-1b-bounded-bsi\n";
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc == 2 && std::string(argv[1]) == "--self-test") {
        return runSelfTest() ? 0 : 1;
    }
    if (argc < 2) {
        std::cerr << "Usage: Eac3NativeBsiProbe <raw.eb3> [--max-units N]\n"
                     "       Eac3NativeBsiProbe --self-test\n";
        return 2;
    }
    std::size_t maxUnits = 1000;
    if (argc == 4 && std::string(argv[2]) == "--max-units") {
        maxUnits = static_cast<std::size_t>(std::stoull(argv[3]));
    }
    return runFile(argv[1], maxUnits) ? 0 : 1;
}
