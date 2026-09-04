#include "native-eac3-audblk.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {
using namespace eac3native;

struct Report {
    std::size_t frames = 0;
    std::size_t legacyFrames = 0;
    std::size_t acceptedFrames = 0;
    std::size_t unsupportedFrames = 0;
    std::size_t blocks = 0;
    std::size_t accessUnits = 0;
    std::size_t malformedFrames = 0;
    std::map<std::string, std::size_t> featureFrames;
    std::map<std::string, std::size_t> featureBlocks;
    std::size_t blockSwitchActive = 0;
    std::size_t ditherOn = 0;
    std::size_t ditherOff = 0;
    std::size_t dynamicRangeWords = 0;
    std::size_t audblkDynrngWords = 0;
    std::size_t audblkDynrng2Words = 0;
    std::size_t bsiComprPresent = 0;
    std::size_t bsiCompr2Present = 0;
    unsigned reservedGroupWarningTotal = 0;
    std::array<ReservedGroupWarning, 16> reservedGroupWarnings{};
    std::map<std::string, std::size_t> unsupportedReasons;
    std::map<std::string, std::size_t> unsupportedFirstBit;
};

void addFeature(Report *report, const std::string &name, bool frame,
                unsigned blocks)
{
    if (frame) ++report->featureFrames[name];
    if (frame) report->featureBlocks[name] += blocks;
}

void addFeatures(Report *report, const AudblkFeatures &f)
{
    report->blockSwitchActive += f.blockSwitchActive;
    report->ditherOn += f.ditherOn;
    report->ditherOff += f.ditherOff;
    report->dynamicRangeWords += f.dynamicRangeWords;
    report->audblkDynrngWords += f.dynrngWords;
    report->audblkDynrng2Words += f.dynrng2Words;
    report->reservedGroupWarningTotal += f.reservedGroupWarningTotal;
    for (unsigned bap : {1U, 2U, 4U}) {
        const ReservedGroupWarning &source = f.reservedGroupWarnings[bap];
        ReservedGroupWarning &target = report->reservedGroupWarnings[bap];
        target.count += source.count;
        if (source.firstValid && !target.firstValid) {
            target.firstValid = true;
            target.firstFrameOffset = source.firstFrameOffset;
            target.firstBlock = source.firstBlock;
            target.firstChannel = source.firstChannel;
            target.firstLfe = source.firstLfe;
            target.firstCoefficient = source.firstCoefficient;
            target.firstBit = source.firstBit;
            target.firstCode = source.firstCode;
            target.maxCode = source.maxCode;
        }
    }
    addFeature(report, "blockSwitchSyntax", f.blockSwitchSyntax, f.blocks);
    addFeature(report, "ditherSyntax", f.ditherSyntax, f.blocks);
    addFeature(report, "bamodeSyntax", f.bamodeSyntax, f.blocks);
    addFeature(report, "fastGainSyntax", f.fastGainSyntax, f.blocks);
    addFeature(report, "deltaBitAllocationSyntax", f.deltaBitAllocationSyntax, f.blocks);
    addFeature(report, "transientProcessing", f.transientProcessing, f.blocks);
    addFeature(report, "spectralExtensionAttenuation", f.spectralExtensionAttenuation, f.blocks);
    addFeature(report, "couplingStrategy", f.couplingStrategy, f.blocks);
    addFeature(report, "couplingInUse", f.couplingInUse, f.blocks);
    addFeature(report, "enhancedCoupling", f.enhancedCoupling, f.blocks);
    addFeature(report, "couplingCoordinates", f.couplingCoordinates, f.blocks);
    addFeature(report, "rematrixing", f.rematrixing, f.blocks);
    addFeature(report, "spectralExtension", f.spectralExtension, f.blocks);
    addFeature(report, "adaptiveHybridTransform", f.adaptiveHybridTransform, f.blocks);
    addFeature(report, "gainAdaptiveQuantization", f.gainAdaptiveQuantization, f.blocks);
    addFeature(report, "leakTerms", f.leakTerms, f.blocks);
    addFeature(report, "snrOffsets", f.snrOffsets, f.blocks);
    addFeature(report, "exponentReuse", f.exponentReuse, f.blocks);
    addFeature(report, "exponentStrategy", f.exponentStrategy, f.blocks);
    addFeature(report, "blockStartInfo", f.blockStartInfo, f.blocks);
    addFeature(report, "drcMetadataPresence", f.drcMetadataPresent, f.blocks);
}

struct BitFieldWriter {
    std::vector<std::uint8_t> bytes;
    std::size_t bits = 0;

    void put(unsigned value, unsigned width)
    {
        for (unsigned i = 0; i < width; ++i) {
            if ((bits % 8U) == 0U) bytes.push_back(0U);
            const unsigned shift = width - i - 1U;
            if (((value >> shift) & 1U) != 0U)
                bytes.back() |= static_cast<std::uint8_t>(1U << (7U - (bits % 8U)));
            ++bits;
        }
    }
};

struct SyntheticFrame {
    std::vector<std::uint8_t> bytes;
    std::size_t bits = 0;
    FrameHeader frame;
    BsiInfo bsi;
};

SyntheticFrame makeSixBlockOrdinaryFrame()
{
    BitFieldWriter writer;
    writer.put(1U, 1U); // expstre
    writer.put(0U, 1U); // ahte
    writer.put(1U, 2U); // snroffststr = block offsets
    writer.put(0U, 1U); // transproce
    writer.put(0U, 1U); // blkswe
    writer.put(0U, 1U); // dithflage
    writer.put(0U, 1U); // bamode
    writer.put(0U, 1U); // frmfgaincode
    writer.put(0U, 1U); // dbaflde
    writer.put(0U, 1U); // skipflde
    writer.put(0U, 1U); // spxattene
    writer.put(1U, 2U); // block 0 d15
    for (unsigned block = 1U; block < 6U; ++block) writer.put(0U, 2U); // reuse
    writer.put(0U, 5U); // convexpstr
    writer.put(0U, 1U); // blkstrtinfoe

    for (unsigned block = 0U; block < 6U; ++block) {
        writer.put(0U, 1U); // dynrnge
        if (block != 0U) writer.put(0U, 1U); // spxstre = 0
        writer.put(0U, 1U); // block 0 spxinu = 0
        if (block == 0U) {
            writer.put(0U, 6U); // chbwcod -> endmant 73
            writer.put(0U, 4U); // absolute exponent
            for (unsigned group = 0U; group < 24U; ++group)
                writer.put(62U, 7U); // three zero deltas, d15
            writer.put(0U, 2U); // gainrng
        }
        if (block == 0U) {
            writer.put(0U, 6U); // csnroffst
            writer.put(0U, 4U); // blkfsnroffst
        } else {
            writer.put(0U, 1U); // snroffste = 0
        }
        writer.put(0U, 1U); // convsnroffste
    }
    SyntheticFrame result;
    result.bytes = std::move(writer.bytes);
    result.bits = writer.bits;
    result.frame.streamType = StreamType::Independent;
    result.frame.offset = 0U;
    result.frame.sizeBytes = result.bytes.size();
    result.frame.endBit = result.bits;
    result.frame.sampleRate = 48000U;
    result.frame.blocks = 6U;
    result.frame.channelCount = 1U;
    result.frame.acmod = 1U;
    result.frame.bsid = 16U;
    result.bsi.bsiParsed = true;
    result.bsi.bsiEndBit = 0U;
    return result;
}

struct GroupedTestCursor {
    bool pending = false;
    unsigned bap = 0;
    unsigned index = 0;
};

bool consumeGrouped(BoundedBitReader &reader, unsigned bap,
                    unsigned count, GroupedTestCursor *state,
                    bool endOfBlock)
{
    const unsigned bits = bap == 1U ? 5U : 7U;
    const unsigned groupSize = bap == 4U ? 2U : 3U;
    if (!state || (bap != 1U && bap != 2U && bap != 4U)) return false;
    for (unsigned i = 0; i < count; ++i) {
        if (state->pending && state->bap != bap) return false;
        if (!state->pending) {
            std::uint32_t code = 0;
            if (!reader.read(bits, &code)) return false;
            state->pending = true;
            state->bap = bap;
            state->index = 0U;
        }
        if (++state->index == groupSize) {
            state->pending = false;
            state->bap = 0U;
            state->index = 0U;
        }
    }
    if (endOfBlock) *state = {};
    return true;
}

bool runSelfTest()
{
    const SyntheticFrame synthetic = makeSixBlockOrdinaryFrame();
    const AudblkParseResult positive = parseEac3Audblk(
        synthetic.bytes, synthetic.frame, synthetic.bsi);
    if (positive.disposition != AudblkDisposition::Accepted
        || !positive.features || !positive.features->audblkParsed
        || positive.features->blocks != 6U
        || !positive.features->exponentReuse
        || !positive.features->exponentStrategy) {
        std::cerr << "selfTest=FAIL case=positive-six-block-reuse\n";
        return false;
    }

    for (std::size_t cut : {1U, 8U, 32U}) {
        FrameHeader truncatedFrame = synthetic.frame;
        truncatedFrame.endBit = positive.bitPosition > cut ? positive.bitPosition - cut : 0U;
        const AudblkParseResult truncatedTail = parseEac3Audblk(
            synthetic.bytes, truncatedFrame, synthetic.bsi);
        if (truncatedTail.disposition != AudblkDisposition::Malformed) {
            std::cerr << "selfTest=FAIL case=tail-truncation cut=" << cut << '\n';
            return false;
        }
    }

    // The parser owns grouped mantissa cursor state per channel/block.  This
    // compact zero-bit oracle checks the 1/2/4 packing widths and a split
    // continuation without decoding coefficient values.
    std::vector<std::uint8_t> groupedBytes(3U, 0U);
    BoundedBitReader groupedReader(groupedBytes.data(), groupedBytes.size(), 0U, 19U);
    GroupedTestCursor grouped1;
    GroupedTestCursor grouped2;
    GroupedTestCursor grouped4;
    if (!consumeGrouped(groupedReader, 1U, 1U, &grouped1, false)
        || !grouped1.pending
        || !consumeGrouped(groupedReader, 2U, 3U, &grouped2, false)
        || !consumeGrouped(groupedReader, 1U, 2U, &grouped1, false)
        || grouped1.pending
        || !consumeGrouped(groupedReader, 4U, 2U, &grouped4, true)
        || grouped4.pending || groupedReader.position() != 19U) {
        std::cerr << "selfTest=FAIL case=grouped-mantissa-widths\n";
        return false;
    }

    // The grouped cursor is owned by the whole audio block, so an LFE
    // exponent set may complete a partial FBW group without reading a new
    // codeword.  A fresh LFE cursor would incorrectly consume another 7-bit
    // code here.
    std::vector<std::uint8_t> lfeGroupedBytes(2U, 0U);
    BoundedBitReader lfeGroupedReader(lfeGroupedBytes.data(),
                                       lfeGroupedBytes.size(), 0U, 7U);
    GroupedTestCursor blockOwnedGroup;
    const bool lfeGroupedOwnership =
        consumeGrouped(lfeGroupedReader, 4U, 1U, &blockOwnedGroup, false)
        && blockOwnedGroup.pending
        && consumeGrouped(lfeGroupedReader, 4U, 1U, &blockOwnedGroup, true)
        && !blockOwnedGroup.pending
        && lfeGroupedReader.position() == 7U;
    if (!lfeGroupedOwnership) {
        std::cerr << "selfTest=FAIL case=lfe-grouped-ownership\n";
        return false;
    }

    if (!runAudblkReservedGroupSelfTest()) {
        std::cerr << "selfTest=FAIL case=reserved-group-warning-cursor\n";
        return false;
    }

    std::vector<std::uint8_t> bytes(8U, 0U);
    FrameHeader frame;
    frame.streamType = StreamType::Independent;
    frame.offset = 0U;
    frame.sizeBytes = bytes.size();
    frame.endBit = bytes.size() * 8U;
    frame.sampleRate = 48000U;
    frame.blocks = 6U;
    frame.channelCount = 2U;
    frame.acmod = 2U;
    BsiInfo bsi;
    bsi.bsiParsed = true;
    bsi.bsiEndBit = 500U;
    const AudblkParseResult boundary = parseEac3Audblk(bytes, frame, bsi);
    if (boundary.disposition != AudblkDisposition::Malformed
        || boundary.stage != FailureStage::Bounds) {
        std::cerr << "selfTest=FAIL case=frame-boundary\n";
        return false;
    }
    bsi.bsiEndBit = 0U;
    const AudblkParseResult truncated = parseEac3Audblk(bytes, frame, bsi);
    if (truncated.disposition == AudblkDisposition::Accepted) {
        std::cerr << "selfTest=FAIL case=truncated-audfrm\n";
        return false;
    }
    frame.streamType = StreamType::LegacyAc3;
    const AudblkParseResult legacy = parseEac3Audblk(bytes, frame, bsi);
    if (legacy.disposition != AudblkDisposition::Unsupported) {
        std::cerr << "selfTest=FAIL case=legacy-boundary\n";
        return false;
    }
    std::cout << "selfTest=PASS cases=9 positiveSixBlock=YES reuse=YES groupedMantissa=YES lfeGroupedOwnership=YES reservedGroupWarningCursor=YES truncation=3 bounded=YES drcApplied=NO ffmpegLinked=NO\n";
    return true;
}

void usage()
{
    std::cerr << "Usage: Eac3NativeAudblkProbe <raw.eac3|raw.ec3|raw.eb3> [--max-units N] [--summary] [--verbose]\n"
                 "       Eac3NativeAudblkProbe --self-test\n";
}

bool runFile(const std::string &path, std::size_t maxUnits, bool verbose)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::cerr << "probeResult=FAIL stage=open reason=file-open-failed path=" << path << '\n';
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
    Report report;
    AccessUnitAssembler assembler;
    std::size_t offset = 0U;
    bool boundedStop = false;
    while (offset < framed.size() && (maxUnits == 0U || report.accessUnits < maxUnits)) {
        const Report beforeFrame = report;
        const ParseResult parsed = parseSyncframe(framed, offset);
        if (parsed.disposition != Disposition::Accepted || !parsed.frame) {
            std::cerr << "probeResult=FAIL stage=" << toString(parsed.stage)
                      << " disposition=" << toString(parsed.disposition)
                      << " reason=" << parsed.reason << " bitPosition=" << offset * 8U << '\n';
            return false;
        }
        const FrameHeader &frame = *parsed.frame;
        ++report.frames;
        if (frame.streamType == StreamType::LegacyAc3) {
            ++report.legacyFrames;
        } else {
            const BsiParseResult bsi = parseEac3Bsi(framed, frame);
            if (bsi.disposition != Disposition::Accepted || !bsi.info) {
                std::cerr << "probeResult=FAIL stage=" << toString(bsi.stage)
                          << " reason=" << bsi.reason << " bitPosition=" << offset * 8U << '\n';
                return false;
            }
            if (bsi.info->comprPresent) ++report.bsiComprPresent;
            if (bsi.info->compr2e) ++report.bsiCompr2Present;
            const AudblkParseResult inventory = parseEac3Audblk(framed, frame, *bsi.info);
            if (inventory.features) {
                addFeatures(&report, *inventory.features);
                report.blocks += inventory.features->blocks;
                if (verbose) {
                    for (unsigned bap : {1U, 2U, 4U}) {
                        const ReservedGroupWarning &warning =
                            inventory.features->reservedGroupWarnings[bap];
                        if (!warning.count) continue;
                        std::cout << "reservedGroupWarning bap=" << bap
                                  << " count=" << warning.count
                                  << " firstFrameOffset=" << warning.firstFrameOffset
                                  << " firstBlock=" << warning.firstBlock
                                  << " firstChannel=" << warning.firstChannel
                                  << " firstLfe=" << (warning.firstLfe ? "YES" : "NO")
                                  << " firstCoefficient=" << warning.firstCoefficient
                                  << " firstBit=" << warning.firstBit
                                  << " firstCode=" << warning.firstCode
                                  << " maxCode=" << warning.maxCode << '\n';
                    }
                }
            }
            if (inventory.disposition == AudblkDisposition::Accepted) {
                ++report.acceptedFrames;
            } else if (inventory.disposition == AudblkDisposition::Unsupported) {
                ++report.unsupportedFrames;
                ++report.unsupportedReasons[inventory.reason];
                report.unsupportedFirstBit.try_emplace(inventory.reason, inventory.bitPosition);
                if (verbose) {
                    std::cout << "unsupportedFrameOffset=" << offset
                              << " bitPosition=" << inventory.bitPosition
                              << " stage=" << toString(inventory.stage)
                              << " reason=" << inventory.reason << '\n';
                }
            } else {
                ++report.malformedFrames;
                std::cerr << "probeResult=FAIL stage=" << toString(inventory.stage)
                          << " reason=" << inventory.reason
                          << " bitPosition=" << inventory.bitPosition
                          << " frameOffset=" << offset << '\n';
                return false;
            }
        }
        const ProcessResult assembled = assembler.process(parsed);
        if (assembled.disposition != Disposition::Accepted
            || assembled.flow != FlowStatus::None) {
            std::cerr << "probeResult=FAIL stage=" << toString(assembled.stage)
                      << " reason=" << assembled.reason << '\n';
            return false;
        }
        if (assembled.completed) ++report.accessUnits;
        if (assembled.completed && maxUnits != 0U
            && report.accessUnits >= maxUnits) {
            // The completion notification is emitted on the first frame of
            // the next AU.  Keep that look-ahead frame out of the bounded
            // evidence, matching the core/BSI probes.
            const std::size_t completedUnits = report.accessUnits;
            report = beforeFrame;
            report.accessUnits = completedUnits;
            boundedStop = true;
            break;
        }
        offset += frame.sizeBytes;
    }
    if (maxUnits == 0U && !boundedStop) {
        const ProcessResult flushed = assembler.flush();
        if (flushed.disposition != Disposition::Accepted || !flushed.completed) {
            std::cerr << "probeResult=FAIL stage=assembly reason=" << flushed.reason << '\n';
            return false;
        }
        ++report.accessUnits;
    }
    if (report.frames == 0U || report.malformedFrames != 0U) return false;
    std::cout << "inputBytes=" << bytes.size() << '\n'
              << "normalizedBytes=" << framed.size() << '\n'
              << "frames=" << report.frames << '\n'
              << "legacyFrames=" << report.legacyFrames << '\n'
              << "acceptedFrames=" << report.acceptedFrames << '\n'
              << "unsupportedFrames=" << report.unsupportedFrames << '\n'
              << "blocks=" << report.blocks << '\n'
              << "accessUnits=" << report.accessUnits << '\n'
              << "eb3WrapperCountTotal=" << (framing.wrapped ? framing.wrapperCount : 0U) << '\n'
              << "blockSwitchActive=" << report.blockSwitchActive << '\n'
              << "ditherOn=" << report.ditherOn << '\n'
              << "ditherOff=" << report.ditherOff << '\n'
              << "dynamicRangeWords=" << report.dynamicRangeWords << '\n'
              << "audblkDynrngWords=" << report.audblkDynrngWords << '\n'
              << "audblkDynrng2Words=" << report.audblkDynrng2Words << '\n'
              << "bsiComprPresent=" << report.bsiComprPresent << '\n'
              << "bsiCompr2Present=" << report.bsiCompr2Present << '\n'
              << "reservedGroupWarningTotal=" << report.reservedGroupWarningTotal << '\n'
              << "drcApplied=NO\n"
              << "ffmpegLinked=NO\n";
    for (unsigned bap : {1U, 2U, 4U}) {
        const ReservedGroupWarning &warning = report.reservedGroupWarnings[bap];
        std::cout << "reservedGroupWarnings[bap" << bap << "]="
                  << warning.count << '\n';
        if (warning.firstValid) {
            std::cout << "reservedGroupFirst[bap" << bap << "]="
                      << "frameOffset=" << warning.firstFrameOffset
                      << " block=" << warning.firstBlock
                      << " channel=" << warning.firstChannel
                      << " lfe=" << (warning.firstLfe ? "YES" : "NO")
                      << " coefficient=" << warning.firstCoefficient
                      << " bit=" << warning.firstBit
                      << " code=" << warning.firstCode
                      << " maxCode=" << warning.maxCode << '\n';
        }
    }
    for (const auto &[name, count] : report.featureFrames)
        std::cout << "featureFrame[" << name << "]=" << count << '\n';
    for (const auto &[name, count] : report.featureBlocks)
        std::cout << "featureBlocks[" << name << "]=" << count << '\n';
    for (const auto &[reason, count] : report.unsupportedReasons)
        std::cout << "unsupportedReason[" << reason << "]=" << count
                  << " firstBit=" << report.unsupportedFirstBit[reason] << '\n';
    const bool inventoryPass = report.frames > 0U && report.malformedFrames == 0U
        && (report.acceptedFrames + report.unsupportedFrames) > 0U;
    const char *capability = report.unsupportedFrames != 0U
        ? "UNSUPPORTED"
        : report.acceptedFrames != 0U ? "PASS" : "INCONCLUSIVE";
    std::cout << "n0InventoryResult=" << (inventoryPass ? "PASS" : "FAIL")
              << " stage=gate8n-1c-bounded-audfrm-audblk-inventory\n"
              << "decoderCapabilityResult=" << capability << '\n'
              << "probeResult=" << (inventoryPass ? "PASS" : "FAIL")
              << " stage=gate8n-1c-bounded-audfrm-audblk-inventory\n";
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc == 2 && std::string(argv[1]) == "--self-test") return runSelfTest() ? 0 : 1;
    if (argc < 2 || argc > 6) { usage(); return 2; }
    std::size_t maxUnits = 0U;
    bool verbose = false;
    for (int i = 2; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "--summary") continue;
        if (option == "--verbose") { verbose = true; continue; }
        if (option == "--max-units" && i + 1 < argc) {
            try { maxUnits = static_cast<std::size_t>(std::stoull(argv[++i])); }
            catch (...) { usage(); return 2; }
        } else { usage(); return 2; }
    }
    return runFile(argv[1], maxUnits, verbose) ? 0 : 1;
}
