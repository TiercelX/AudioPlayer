#include "native-eac3-audblk.h"

#include "native-eac3-bit-allocation.h"
#include "native-eac3-block-state.h"
#include "native-eac3-exponents.h"

#include <algorithm>
#include <array>
#include <string>
#include <utility>

namespace eac3native {
namespace {

enum class Strategy : unsigned { Reuse = 0, D15 = 1, D25 = 2, D45 = 3 };

struct ChannelState {
    bool valid = false;
    unsigned chbwcod = 0U;
    unsigned endMant = 0;
    std::vector<unsigned> exponents;
    std::vector<unsigned> bap;
};

struct AudfrmState {
    unsigned nfchans = 0;
    bool lfe = false;
    unsigned blocks = 0;
    bool expstre = false;
    bool ahte = false;
    unsigned snrStrategy = 0;
    bool blkswe = false;
    bool dithflage = false;
    bool bamode = false;
    bool frmfgaincode = false;
    bool dbaflde = false;
    bool skipflde = false;
    unsigned frmcsnroffst = 0;
    unsigned frmfsnroffst = 0;
    bool spxattene = false;
    bool cplInUse[6] = {};
    bool cplStrategy[6] = {};
    Strategy exp[6][8] = {};
    Strategy lfeExp[6] = {};
};

struct BitAllocationState {
    unsigned sdcy = 2U;
    unsigned fdcy = 1U;
    unsigned sgain = 1U;
    unsigned dbpb = 2U;
    unsigned floor = 7U;
};

AudblkParseResult fail(const BoundedBitReader &reader,
                       AudblkDisposition disposition,
                       FailureStage stage,
                       std::string reason,
                       const AudblkFeatures &features)
{
    AudblkParseResult result;
    result.disposition = disposition;
    result.stage = stage;
    result.reason = std::move(reason);
    if (features.state) result.frameOffset = features.state->frameOffset;
    result.bitPosition = reader.position();
    result.blockIndex = features.currentBlock;
    result.channelIndex = features.currentChannel;
    result.channelIsLfe = features.currentLfe;
    result.features = features;
    return result;
}

AudblkParseResult failAt(std::size_t bitPosition,
                         AudblkDisposition disposition,
                         FailureStage stage,
                         std::string reason,
                         const AudblkFeatures &features)
{
    AudblkParseResult result;
    result.disposition = disposition;
    result.stage = stage;
    result.reason = std::move(reason);
    if (features.state) result.frameOffset = features.state->frameOffset;
    result.bitPosition = bitPosition;
    result.blockIndex = features.currentBlock;
    result.channelIndex = features.currentChannel;
    result.channelIsLfe = features.currentLfe;
    result.features = features;
    return result;
}

bool read(BoundedBitReader &reader, unsigned count, unsigned *value)
{
    std::uint32_t raw = 0;
    if (!value || !reader.read(count, &raw)) return false;
    *value = static_cast<unsigned>(raw);
    return true;
}

unsigned ceilLog2(unsigned value)
{
    unsigned bits = 0;
    unsigned n = value > 0U ? value - 1U : 0U;
    while (n != 0U) {
        ++bits;
        n >>= 1U;
    }
    return bits;
}

Strategy strategyFromCode(unsigned value)
{
    switch (value) {
    case 1: return Strategy::D15;
    case 2: return Strategy::D25;
    case 3: return Strategy::D45;
    default: return Strategy::Reuse;
    }
}

ExponentStrategy exponentStrategy(Strategy value)
{
    switch (value) {
    case Strategy::D15: return ExponentStrategy::D15;
    case Strategy::D25: return ExponentStrategy::D25;
    case Strategy::D45: return ExponentStrategy::D45;
    case Strategy::Reuse: return ExponentStrategy::Reuse;
    }
    return ExponentStrategy::Reuse;
}

unsigned strategyGroupSize(Strategy strategy)
{
    switch (strategy) {
    case Strategy::D15: return 1U;
    case Strategy::D25: return 2U;
    case Strategy::D45: return 4U;
    case Strategy::Reuse: return 0U;
    }
    return 0U;
}

std::array<Strategy, 6> frameStrategy(unsigned value)
{
    // TS 102 366 V1.4.1 Table E.1.8/E.1.9 strategy expansion.
    static constexpr std::array<std::array<Strategy, 6>, 32> table = {{
        {{Strategy::D15, Strategy::Reuse, Strategy::Reuse, Strategy::Reuse, Strategy::Reuse, Strategy::Reuse}},
        {{Strategy::D15, Strategy::Reuse, Strategy::Reuse, Strategy::Reuse, Strategy::Reuse, Strategy::D45}},
        {{Strategy::D15, Strategy::Reuse, Strategy::Reuse, Strategy::Reuse, Strategy::D25, Strategy::Reuse}},
        {{Strategy::D15, Strategy::Reuse, Strategy::Reuse, Strategy::Reuse, Strategy::D45, Strategy::D45}},
        {{Strategy::D25, Strategy::Reuse, Strategy::Reuse, Strategy::D25, Strategy::Reuse, Strategy::Reuse}},
        {{Strategy::D25, Strategy::Reuse, Strategy::Reuse, Strategy::D25, Strategy::Reuse, Strategy::D45}},
        {{Strategy::D25, Strategy::Reuse, Strategy::Reuse, Strategy::D45, Strategy::D25, Strategy::Reuse}},
        {{Strategy::D25, Strategy::Reuse, Strategy::Reuse, Strategy::D45, Strategy::D45, Strategy::D45}},
        {{Strategy::D25, Strategy::Reuse, Strategy::D15, Strategy::Reuse, Strategy::Reuse, Strategy::Reuse}},
        {{Strategy::D25, Strategy::Reuse, Strategy::D25, Strategy::Reuse, Strategy::Reuse, Strategy::D45}},
        {{Strategy::D25, Strategy::Reuse, Strategy::D25, Strategy::Reuse, Strategy::D25, Strategy::Reuse}},
        {{Strategy::D25, Strategy::Reuse, Strategy::D25, Strategy::Reuse, Strategy::D45, Strategy::D45}},
        {{Strategy::D25, Strategy::Reuse, Strategy::D45, Strategy::D25, Strategy::Reuse, Strategy::Reuse}},
        {{Strategy::D25, Strategy::Reuse, Strategy::D45, Strategy::D25, Strategy::Reuse, Strategy::D45}},
        {{Strategy::D25, Strategy::Reuse, Strategy::D45, Strategy::D45, Strategy::D25, Strategy::Reuse}},
        {{Strategy::D25, Strategy::Reuse, Strategy::D45, Strategy::D45, Strategy::D45, Strategy::D45}},
        {{Strategy::D45, Strategy::D15, Strategy::Reuse, Strategy::Reuse, Strategy::Reuse, Strategy::Reuse}},
        {{Strategy::D45, Strategy::D15, Strategy::Reuse, Strategy::Reuse, Strategy::Reuse, Strategy::D45}},
        {{Strategy::D45, Strategy::D25, Strategy::Reuse, Strategy::Reuse, Strategy::D25, Strategy::Reuse}},
        {{Strategy::D45, Strategy::D25, Strategy::Reuse, Strategy::Reuse, Strategy::D45, Strategy::D45}},
        {{Strategy::D45, Strategy::D25, Strategy::Reuse, Strategy::D25, Strategy::Reuse, Strategy::Reuse}},
        {{Strategy::D45, Strategy::D25, Strategy::Reuse, Strategy::D25, Strategy::Reuse, Strategy::D45}},
        {{Strategy::D45, Strategy::D25, Strategy::Reuse, Strategy::D45, Strategy::D25, Strategy::Reuse}},
        {{Strategy::D45, Strategy::D25, Strategy::Reuse, Strategy::D45, Strategy::D45, Strategy::D45}},
        {{Strategy::D45, Strategy::D45, Strategy::D15, Strategy::Reuse, Strategy::Reuse, Strategy::Reuse}},
        {{Strategy::D45, Strategy::D45, Strategy::D25, Strategy::Reuse, Strategy::Reuse, Strategy::D45}},
        {{Strategy::D45, Strategy::D45, Strategy::D25, Strategy::Reuse, Strategy::D25, Strategy::Reuse}},
        {{Strategy::D45, Strategy::D45, Strategy::D25, Strategy::Reuse, Strategy::D45, Strategy::D45}},
        {{Strategy::D45, Strategy::D45, Strategy::D45, Strategy::D25, Strategy::Reuse, Strategy::Reuse}},
        {{Strategy::D45, Strategy::D45, Strategy::D45, Strategy::D25, Strategy::Reuse, Strategy::D45}},
        {{Strategy::D45, Strategy::D45, Strategy::D45, Strategy::D45, Strategy::D25, Strategy::Reuse}},
        {{Strategy::D45, Strategy::D45, Strategy::D45, Strategy::D45, Strategy::D45, Strategy::D45}},
    }};
    return table[value & 31U];
}

MantissaDecodeResult consumeMantissas(
    const std::vector<std::uint8_t> &bytes,
    std::size_t frameEndBit,
    BoundedBitReader &reader,
    const std::vector<unsigned> &bap,
    const std::vector<unsigned> &exponents,
    MantissaDecodeState *cursor,
    unsigned channel,
    unsigned block,
    bool endOfBlock,
    bool dithflag,
    bool decodeValues,
    const DitherSource &ditherSource)
{
    MantissaDecodeResult result;
    if (!cursor) {
        result.reason = "mantissa-cursor-null";
        return result;
    }
    MantissaBitReader mantissaReader(bytes, frameEndBit, reader.position());
    MantissaDecodeRequest request;
    request.bap = bap;
    request.exponents = exponents;
    request.channelId = channel;
    request.audioBlockId = block;
    request.endOfBlock = endOfBlock;
    request.dithflag = dithflag;
    request.sharedBlockCursor = true;
    request.decodeValues = decodeValues;
    request.rejectReservedComposite = decodeValues;
    request.ditherSource = ditherSource;
    result = decodeMantissas(mantissaReader, request, *cursor);
    if (result.disposition == MantissaDisposition::Accepted
        && result.bitsConsumed != 0U && !reader.skip(result.bitsConsumed)) {
        result.disposition = MantissaDisposition::Malformed;
        result.reason = "mantissa-cursor-advance";
    }
    return result;
}

void recordReservedComposite(const MantissaDecodeResult &result,
                             AudblkFeatures *features,
                             std::size_t frameOffset,
                             unsigned block,
                             unsigned channel,
                             bool lfe)
{
    if (!features || !result.reservedComposite) return;
    const unsigned count = result.reservedCompositeCount == 0U
        ? 1U : result.reservedCompositeCount;
    features->reservedGroupWarningTotal += count;
    ReservedGroupWarning &warning = features->reservedGroupWarnings[
        result.reservedBap];
    warning.count += count;
    if (!warning.firstValid) {
        warning.firstValid = true;
        warning.firstFrameOffset = frameOffset;
        warning.firstBlock = block;
        warning.firstChannel = channel;
        warning.firstLfe = lfe;
        warning.firstCoefficient = 0U;
        warning.firstBit = result.reservedBitOffset;
        warning.firstCode = result.reservedCode;
        warning.maxCode = result.reservedMaxCode;
    }
}

bool decodeExponents(BoundedBitReader &reader, Strategy strategy,
                     unsigned endMant, ChannelState *state,
                     bool block0, AudblkFeatures *features,
                     std::string *reason)
{
    if (!state || !features || endMant == 0U) return false;
    if (strategy == Strategy::Reuse) {
        if (block0 || !state->valid || state->endMant != endMant) {
            if (reason) *reason = "exponent-reuse-without-matching-prior";
            return false;
        }
        features->exponentReuse = true;
        return true;
    }
    const unsigned groupSize = strategyGroupSize(strategy);
    if (groupSize == 0U) return false;
    unsigned absolute = 0;
    if (!read(reader, 4U, &absolute)) {
        if (reason) *reason = "truncated-absolute-exponent";
        return false;
    }
    const unsigned groups = (endMant - 1U + (3U * groupSize) - 1U)
        / (3U * groupSize);
    std::vector<unsigned> groupedCodes;
    groupedCodes.reserve(groups);
    for (unsigned i = 0; i < groups; ++i) {
        unsigned code = 0;
        if (!read(reader, 7U, &code)) {
            if (reason) *reason = "truncated-exponent-groups";
            return false;
        }
        groupedCodes.push_back(code);
    }
    ExponentDecodeRequest request;
    request.strategy = exponentStrategy(strategy);
    request.absoluteExponent = absolute;
    request.groupedCodes = std::move(groupedCodes);
    request.targetCoefficientCount = endMant;
    request.block0 = block0;
    request.bandwidthCode = endMant;
    const ExponentDecodeResult decoded = decodeExponentSet(request);
    if (decoded.disposition != ExponentDisposition::Accepted) {
        if (reason) *reason = decoded.reason.empty() ? "exponent-syntax" : decoded.reason;
        return false;
    }
    state->valid = true;
    state->endMant = endMant;
    state->exponents = decoded.exponents;
    features->exponentStrategy = true;
    return true;
}

bool decodeLfe(BoundedBitReader &reader, Strategy strategy, bool block0,
               ChannelState *state, AudblkFeatures *features,
               std::string *reason)
{
    if (!state || !features) return false;
    if (strategy == Strategy::Reuse) {
        if (block0 || !state->valid || state->endMant != 7U) {
            if (reason) *reason = "lfe-exponent-reuse-without-prior";
            return false;
        }
        features->exponentReuse = true;
        return true;
    }
    unsigned absolute = 0;
    std::vector<unsigned> codes;
    if (!read(reader, 4U, &absolute)) {
        if (reason) *reason = "truncated-lfe-absolute-exponent";
        return false;
    }
    for (unsigned i = 0; i < 2U; ++i) {
        unsigned code = 0;
        if (!read(reader, 7U, &code)) {
            if (reason) *reason = "truncated-lfe-exponent-groups";
            return false;
        }
        codes.push_back(code);
    }
    const ExponentDecodeResult decoded = decodeLfeExponents(
        absolute, codes, ExponentStrategy::D15, block0, 7U);
    if (decoded.disposition != ExponentDisposition::Accepted) {
        if (reason) *reason = decoded.reason.empty() ? "lfe-exponent-syntax" : decoded.reason;
        return false;
    }
    state->valid = true;
    state->endMant = 7U;
    state->exponents = decoded.exponents;
    features->exponentStrategy = true;
    return true;
}

bool readBa(BoundedBitReader &reader, bool bamode, BitAllocationState *state,
            std::string *reason, bool *reused)
{
    if (!state) return false;
    if (reused) *reused = false;
    if (!bamode) return true;
    unsigned baie = 0;
    if (!read(reader, 1U, &baie)) {
        if (reason) *reason = "truncated-baie";
        return false;
    }
    if (!baie) {
        if (reused) *reused = true;
        return true;
    }
    if (!read(reader, 2U, &state->sdcy) || !read(reader, 2U, &state->fdcy)
        || !read(reader, 2U, &state->sgain) || !read(reader, 2U, &state->dbpb)
        || !read(reader, 3U, &state->floor)) {
        if (reason) *reason = "truncated-bit-allocation-codes";
        return false;
    }
    return true;
}

} // namespace

bool runAudblkReservedGroupSelfTest()
{
    // All-one bytes provide reserved composite codes (31 and 127).  The
    // second call for each codeword is a continuation, proving that the same
    // grouped primitive used by inventory/coefficient parsing does not reset
    // the block-owned cursor or invent a coefficient value.
    const std::vector<std::uint8_t> bytes(4U, 0xffU);
    AudblkFeatures features;
    MantissaBitReader bap1Reader(bytes, 8U);
    MantissaDecodeState cursor;
    MantissaDecodeRequest bap1First;
    bap1First.bap = {1U}; bap1First.exponents = {0U};
    bap1First.channelId = 1U; bap1First.audioBlockId = 2U;
    bap1First.sharedBlockCursor = true; bap1First.decodeValues = false;
    const MantissaDecodeResult bap1Start = decodeMantissas(
        bap1Reader, bap1First, cursor);
    recordReservedComposite(bap1Start, &features, 17U, 2U, 1U, false);
    MantissaDecodeRequest bap1End = bap1First;
    bap1End.bap = {1U, 1U, 3U}; bap1End.exponents = {0U, 0U, 0U};
    bap1End.endOfBlock = true;
    const MantissaDecodeResult bap1Finish = decodeMantissas(
        bap1Reader, bap1End, cursor);
    recordReservedComposite(bap1Finish, &features, 17U, 2U, 1U, false);
    if (bap1Start.disposition != MantissaDisposition::Accepted
        || bap1Finish.disposition != MantissaDisposition::Accepted
        || bap1Reader.bitOffset() != 8U) {
        return false;
    }
    const ReservedGroupWarning &bap1 = features.reservedGroupWarnings[1U];
    if (bap1.count != 1U || !bap1.firstValid || bap1.firstFrameOffset != 17U
        || bap1.firstBlock != 2U || bap1.firstChannel != 1U
        || bap1.firstLfe || bap1.firstCoefficient != 0U
        || bap1.firstBit != 0U || bap1.firstCode != 31U
        || bap1.maxCode != 26U) {
        return false;
    }

    cursor.reset();
    MantissaBitReader bap4Reader(bytes, 11U);
    MantissaDecodeRequest bap4First;
    bap4First.bap = {4U}; bap4First.exponents = {0U};
    bap4First.channelId = 2U; bap4First.audioBlockId = 4U;
    bap4First.sharedBlockCursor = true; bap4First.decodeValues = false;
    const MantissaDecodeResult bap4Start = decodeMantissas(
        bap4Reader, bap4First, cursor);
    recordReservedComposite(bap4Start, &features, 23U, 4U, 2U, true);
    MantissaDecodeRequest bap4End = bap4First;
    bap4End.bap = {4U, 5U}; bap4End.exponents = {0U, 0U};
    bap4End.endOfBlock = true;
    const MantissaDecodeResult bap4Finish = decodeMantissas(
        bap4Reader, bap4End, cursor);
    recordReservedComposite(bap4Finish, &features, 23U, 4U, 2U, true);
    if (bap4Start.disposition != MantissaDisposition::Accepted
        || bap4Finish.disposition != MantissaDisposition::Accepted
        || bap4Reader.bitOffset() != 11U) {
        return false;
    }
    const ReservedGroupWarning &bap4 = features.reservedGroupWarnings[4U];
    return features.reservedGroupWarningTotal == 2U
        && bap4.count == 1U && bap4.firstValid
        && bap4.firstFrameOffset == 23U && bap4.firstBlock == 4U
        && bap4.firstChannel == 2U && bap4.firstLfe
        && bap4.firstCoefficient == 0U && bap4.firstBit == 0U
        && bap4.firstCode == 127U && bap4.maxCode == 120U;
}

AudblkParseResult parseEac3AudblkInternal(
    const std::vector<std::uint8_t> &bytes,
    const FrameHeader &frame,
    const BsiInfo &bsi,
    bool decodeCoefficients,
    const DitherSource &ditherSource,
    ReferenceDitherSource *referenceDither,
    const ChannelDitherSource *channelDitherSource)
{
    AudblkFeatures features;
    if (frame.streamType == StreamType::LegacyAc3) {
        return failAt(frame.offset * 8U, AudblkDisposition::Unsupported,
                      FailureStage::Header, "legacy-ac3-audfrm-not-inventory", features);
    }
    if (!bsi.bsiParsed || frame.endBit <= frame.offset * 8U
        || frame.endBit > bytes.size() * 8U
        || bsi.bsiEndBit >= frame.sizeBytes * 8U) {
        return failAt(frame.offset * 8U, AudblkDisposition::Malformed,
                      FailureStage::Bounds, "audfrm-frame-boundary", features);
    }
    BoundedBitReader reader(bytes.data(), bytes.size(),
                            frame.offset * 8U + bsi.bsiEndBit, frame.endBit);
    AudfrmState state;
    state.nfchans = frame.channelCount - (frame.lfe ? 1U : 0U);
    state.lfe = frame.lfe;
    state.blocks = frame.blocks;
    features.state = AudblkFrameState{};
    AudblkFrameState &frameState = *features.state;
    frameState.frameOffset = frame.offset;
    frameState.frameStartBit = frame.offset * 8U;
    frameState.frameEndBit = frame.endBit;
    frameState.bsiEndBit = frame.offset * 8U + bsi.bsiEndBit;
    frameState.audfrmStartBit = reader.position();
    frameState.blocks = frame.blocks;
    frameState.channels = frame.channelCount;
    frameState.lfe = frame.lfe;
    if (referenceDither) {
        frameState.ditherStateStart = referenceDither->state();
        frameState.ditherSamples = referenceDither->generatedSamples();
    }
    frameState.blockStates.reserve(frame.blocks);
    features.blocks = 0U;
    features.channels = frame.channelCount;
    unsigned value = 0;
    if (frame.blocks == 6U) {
        if (!read(reader, 1U, &value)) return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, "truncated-expstre", features);
        state.expstre = value != 0U;
        if (!read(reader, 1U, &value)) return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, "truncated-ahte", features);
        state.ahte = value != 0U;
    } else {
        state.expstre = true;
    }
    frameState.expstre = state.expstre;
    frameState.ahte = state.ahte;
    if (!read(reader, 2U, &state.snrStrategy) || state.snrStrategy == 3U)
        return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, "reserved-snr-offset-strategy", features);
    frameState.snrStrategy = state.snrStrategy;
    if (!read(reader, 1U, &value)) return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, "truncated-transproce", features);
    const bool transient = value != 0U;
    if (!read(reader, 1U, &value)) return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, "truncated-blkswe", features);
    state.blkswe = value != 0U; features.blockSwitchSyntax = state.blkswe;
    frameState.blockSwitchSyntax = state.blkswe;
    if (!read(reader, 1U, &value)) return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, "truncated-dithflage", features);
    state.dithflage = value != 0U; features.ditherSyntax = state.dithflage;
    frameState.ditherSyntax = state.dithflage;
    if (!read(reader, 1U, &value)) return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, "truncated-bamode", features);
    state.bamode = value != 0U; features.bamodeSyntax = state.bamode;
    frameState.bamodeSyntax = state.bamode;
    if (!read(reader, 1U, &value)) return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, "truncated-frmfgaincode", features);
    state.frmfgaincode = value != 0U; features.fastGainSyntax = state.frmfgaincode;
    frameState.fastGainSyntax = state.frmfgaincode;
    if (!read(reader, 1U, &value)) return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, "truncated-dbaflde", features);
    state.dbaflde = value != 0U; features.deltaBitAllocationSyntax = state.dbaflde;
    frameState.deltaBitAllocationSyntax = state.dbaflde;
    if (!read(reader, 1U, &value)) return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, "truncated-skipflde", features);
    state.skipflde = value != 0U;
    if (!read(reader, 1U, &value)) return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, "truncated-spxattene", features);
    const bool spxattene = value != 0U;

    if (frame.acmod > 1U) {
        state.cplStrategy[0] = true;
        if (!read(reader, 1U, &value)) return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, "truncated-cplinu0", features);
        state.cplInUse[0] = value != 0U;
        for (unsigned blk = 1; blk < state.blocks; ++blk) {
            if (!read(reader, 1U, &value)) return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, "truncated-cplstre", features);
            state.cplStrategy[blk] = value != 0U;
            if (state.cplStrategy[blk]) {
                if (!read(reader, 1U, &value)) return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, "truncated-cplinu", features);
                state.cplInUse[blk] = value != 0U;
            } else {
                state.cplInUse[blk] = state.cplInUse[blk - 1U];
            }
        }
    }
    if (state.expstre) {
        for (unsigned blk = 0; blk < state.blocks; ++blk) {
            if (state.cplInUse[blk]) {
                if (!read(reader, 2U, &value)) return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, "truncated-cplexpstr", features);
                state.exp[blk][0] = strategyFromCode(value);
            }
            for (unsigned ch = 0; ch < state.nfchans; ++ch) {
                if (!read(reader, 2U, &value)) return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, "truncated-chexpstr", features);
                state.exp[blk][ch + 1U] = strategyFromCode(value);
            }
        }
    } else {
        unsigned ncplblks = 0U;
        for (unsigned blk = 0; blk < state.blocks; ++blk) ncplblks += state.cplInUse[blk] ? 1U : 0U;
        if (ncplblks > 0U) {
            if (!read(reader, 5U, &value)) return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, "truncated-frmcplexpstr", features);
            const auto expanded = frameStrategy(value);
            for (unsigned blk = 0; blk < state.blocks; ++blk) state.exp[blk][0] = expanded[blk];
        }
        for (unsigned ch = 0; ch < state.nfchans; ++ch) {
            if (!read(reader, 5U, &value)) return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, "truncated-frmchexpstr", features);
            const auto expanded = frameStrategy(value);
            for (unsigned blk = 0; blk < state.blocks; ++blk) state.exp[blk][ch + 1U] = expanded[blk];
        }
    }
    if (state.lfe) {
        for (unsigned blk = 0; blk < state.blocks; ++blk) {
            if (!read(reader, 1U, &value)) return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, "truncated-lfeexpstr", features);
            state.lfeExp[blk] = value == 0U ? Strategy::Reuse : Strategy::D15;
        }
    }
    if (frame.streamType == StreamType::Independent) {
        if (frame.blocks != 6U) {
            if (!read(reader, 1U, &value)) return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, "truncated-convexpstre", features);
        } else value = 1U;
        if (value != 0U) {
            for (unsigned ch = 0; ch < state.nfchans; ++ch)
                if (!read(reader, 5U, &value)) return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, "truncated-convexpstr", features);
        }
    }
    if (state.ahte) {
        features.adaptiveHybridTransform = true;
        ++features.unsupportedBranches;
        return fail(reader, AudblkDisposition::Unsupported, FailureStage::Header, "aht-active", features);
    }
    if (state.snrStrategy == 0U) {
        if (!read(reader, 6U, &state.frmcsnroffst) || !read(reader, 4U, &state.frmfsnroffst))
            return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, "truncated-frame-snr-offset", features);
        features.snrOffsets = state.frmcsnroffst != 0U || state.frmfsnroffst != 0U;
    }
    if (transient) {
        for (unsigned ch = 0; ch < state.nfchans; ++ch) {
            if (!read(reader, 1U, &value)) return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, "truncated-transient-channel", features);
            if (value != 0U) {
                features.transientProcessing = true;
                if (!reader.skip(18U)) return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, "truncated-transient-data", features);
            }
        }
    }
    if (spxattene) {
        for (unsigned ch = 0; ch < state.nfchans; ++ch) {
            if (!read(reader, 1U, &value)) return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, "truncated-spxatten-channel", features);
            if (value != 0U) {
                features.spectralExtensionAttenuation = true;
                if (!reader.skip(5U)) return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, "truncated-spxatten-code", features);
            }
        }
    }
    if (frame.blocks != 1U) {
        if (!read(reader, 1U, &value)) return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, "truncated-blkstrtinfoe", features);
        if (value != 0U) {
            features.blockStartInfo = true;
            const unsigned bits = (frame.blocks - 1U) * (4U + ceilLog2(frame.sizeBytes / 2U));
            if (!reader.skip(bits)) return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, "truncated-blkstrtinfo", features);
        }
    }
    frameState.audfrmEndBit = reader.position();
    features.audfrmParsed = true;
    for (unsigned blk = 0; blk < state.blocks; ++blk)
        for (unsigned ch = 0; ch < state.nfchans; ++ch)
            if (state.exp[blk][ch + 1U] == Strategy::Reuse) features.exponentReuse = true;
            else features.exponentStrategy = true;
    if (state.cplInUse[0]) {
        features.couplingStrategy = true;
        features.couplingInUse = true;
        ++features.unsupportedBranches;
        return fail(reader, AudblkDisposition::Unsupported, FailureStage::Header, "coupling-active", features);
    }

    std::vector<ChannelState> channels(state.nfchans);
    ChannelState lfe;
    BitAllocationState allocationState;
    std::vector<unsigned> priorCsnr(state.nfchans, state.frmcsnroffst);
    std::vector<unsigned> priorFsnr(state.nfchans, state.frmfsnroffst);
    unsigned priorLfeFsnr = state.frmfsnroffst;
    std::vector<unsigned> priorFgain(state.nfchans, 4U);
    unsigned priorLfeFgain = 4U;
    std::array<bool, 4> priorRematrixFlags{};
    bool priorRematrixValid = false;
    for (unsigned blk = 0; blk < state.blocks; ++blk) {
        features.currentBlock = blk;
        features.currentChannel = static_cast<unsigned>(-1);
        features.currentLfe = false;
        frameState.blockStates.emplace_back();
        AudblkBlockState &blockState = frameState.blockStates.back();
        blockState.block = blk;
        blockState.startBit = reader.position();
        blockState.ditherSyntax = state.dithflage;
        blockState.channels.resize(frame.channelCount);
        blockState.groupedChannelOrder.reserve(frame.channelCount);
        blockState.blockSwitchFlags.reserve(state.nfchans);
        blockState.ditherFlags.reserve(state.nfchans);
        if (state.blkswe) {
            for (unsigned ch = 0; ch < state.nfchans; ++ch) {
                if (!read(reader, 1U, &value)) return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, "truncated-blksw", features);
                const bool active = value != 0U;
                blockState.blockSwitchFlags.push_back(active);
                blockState.blockSwitch = blockState.blockSwitch || active;
                if (active) ++features.blockSwitchActive;
            }
        } else {
            blockState.blockSwitchFlags.assign(state.nfchans, false);
        }
        std::vector<bool> dither(state.nfchans, true);
        if (state.dithflage) {
            for (unsigned ch = 0; ch < state.nfchans; ++ch) {
                if (!read(reader, 1U, &value)) return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, "truncated-dithflag", features);
                dither[ch] = value != 0U;
                blockState.ditherFlags.push_back(dither[ch]);
                if (dither[ch]) ++features.ditherOn; else ++features.ditherOff;
            }
        } else {
            blockState.ditherFlags.assign(state.nfchans, true);
            features.ditherOn += state.nfchans;
        }
        blockState.dither = std::all_of(dither.begin(), dither.end(), [](bool value) { return value; });
        if (!read(reader, 1U, &value)) return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, "truncated-dynrnge", features);
        if (value != 0U) {
            blockState.dynrngPresent = true;
            features.drcMetadataPresent = true;
            ++features.dynamicRangeWords;
            ++features.dynrngWords;
            if (!reader.skip(8U)) return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, "truncated-dynrng", features);
        }
        if (frame.acmod == 0U) {
            if (!read(reader, 1U, &value)) return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, "truncated-dynrng2e", features);
            if (value != 0U) {
                features.drcMetadataPresent = true;
                ++features.dynamicRangeWords;
                ++features.dynrng2Words;
                if (!reader.skip(8U)) return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, "truncated-dynrng2", features);
            }
        }
        unsigned spxstre = blk == 0U ? 1U : 0U;
        if (blk != 0U && !read(reader, 1U, &spxstre)) return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, "truncated-spxstre", features);
        if (spxstre) {
            if (!read(reader, 1U, &value)) return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, "truncated-spxinu", features);
            if (value != 0U) {
                features.spectralExtension = true;
                ++features.unsupportedBranches;
                return fail(reader, AudblkDisposition::Unsupported, FailureStage::Header, "spx-active", features);
            }
        }
        if (state.cplStrategy[blk] && state.cplInUse[blk]) {
            if (!read(reader, 1U, &value)) return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, "truncated-ecplinu", features);
            features.couplingStrategy = true;
            features.couplingInUse = true;
            features.enhancedCoupling = value != 0U;
            ++features.unsupportedBranches;
            return fail(reader, AudblkDisposition::Unsupported, FailureStage::Header,
                        value != 0U ? "enhanced-coupling-active" : "coupling-active", features);
        }
        if (frame.acmod == 2U) {
            unsigned rematstr = blk == 0U ? 1U : 0U;
            if (blk != 0U && !read(reader, 1U, &rematstr)) return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, "truncated-rematstr", features);
            blockState.rematrixUpdated = rematstr != 0U;
            blockState.rematrixReused = rematstr == 0U && blk != 0U
                && priorRematrixValid;
            if (rematstr) {
                features.rematrixing = true;
                for (unsigned i = 0; i < 4U; ++i) {
                    if (!read(reader, 1U, &value)) return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, "truncated-rematflg", features);
                    priorRematrixFlags[i] = value != 0U;
                }
                priorRematrixValid = true;
            }
            blockState.rematrixPresent = priorRematrixValid;
            blockState.rematrixFlags = priorRematrixFlags;
        }
        std::vector<unsigned> endMant(state.nfchans, 0U);
        for (unsigned ch = 0; ch < state.nfchans; ++ch) {
            features.currentChannel = ch;
            features.currentLfe = false;
            AudblkChannelState &snapshot = blockState.channels[ch];
            snapshot.channel = ch;
            snapshot.lfe = false;
            snapshot.startBit = reader.position();
            if (state.exp[blk][ch + 1U] != Strategy::Reuse) {
                unsigned chbwcod = 0;
                if (!read(reader, 6U, &chbwcod)) return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, "truncated-chbwcod", features);
                endMant[ch] = 73U + 3U * chbwcod;
                if (endMant[ch] > 253U) return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, "invalid-chbwcod", features);
                channels[ch].chbwcod = chbwcod;
                snapshot.bandwidthReused = false;
            } else if (channels[ch].valid) {
                endMant[ch] = channels[ch].endMant;
                snapshot.bandwidthReused = true;
            } else return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, "reuse-without-channel-bandwidth", features);
            snapshot.chbwcod = channels[ch].chbwcod;
            snapshot.endMant = endMant[ch];
            snapshot.exponentStrategy = static_cast<unsigned>(state.exp[blk][ch + 1U]);
            snapshot.exponentReused = state.exp[blk][ch + 1U] == Strategy::Reuse;
        }
        for (unsigned ch = 0; ch < state.nfchans; ++ch) {
            features.currentChannel = ch;
            features.currentLfe = false;
            std::string reason;
            if (!decodeExponents(reader, state.exp[blk][ch + 1U], endMant[ch], &channels[ch], blk == 0U, &features, &reason))
                return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, reason.empty() ? "channel-exponent-syntax" : reason, features);
            blockState.channels[ch].exponents = channels[ch].exponents;
            unsigned gainrng = 0;
            if (state.exp[blk][ch + 1U] != Strategy::Reuse && !read(reader, 2U, &gainrng))
                return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, "truncated-gainrng", features);
        }
        if (state.lfe) {
            features.currentChannel = state.nfchans;
            features.currentLfe = true;
            AudblkChannelState &snapshot = blockState.channels[state.nfchans];
            snapshot.channel = state.nfchans;
            snapshot.lfe = true;
            snapshot.startBit = reader.position();
            snapshot.chbwcod = 0U;
            snapshot.endMant = 7U;
            snapshot.exponentStrategy = static_cast<unsigned>(state.lfeExp[blk]);
            snapshot.exponentReused = state.lfeExp[blk] == Strategy::Reuse;
            std::string reason;
            if (!decodeLfe(reader, state.lfeExp[blk], blk == 0U, &lfe, &features, &reason))
                return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, reason.empty() ? "lfe-exponent-syntax" : reason, features);
            snapshot.exponents = lfe.exponents;
        }
        // BA, SNR, fast-gain, converter, DBA, and skip syntax are block-wide
        // (even when their values are copied into per-channel snapshots).
        features.currentChannel = static_cast<unsigned>(-1);
        features.currentLfe = false;
        std::string reason;
        bool baReused = false;
        if (!readBa(reader, state.bamode, &allocationState, &reason, &baReused))
            return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, reason, features);
        if (state.bamode && blk == 0U && baReused)
            return fail(reader, AudblkDisposition::Malformed, FailureStage::Header,
                        "ba-reuse-on-block-zero", features);
        std::vector<unsigned> csnr = priorCsnr;
        std::vector<unsigned> fsnr = priorFsnr;
        unsigned lfeFsnr = priorLfeFsnr;
        bool snrReused = state.snrStrategy != 0U && blk != 0U;
        if (state.snrStrategy != 0U) {
            unsigned snroffste = blk == 0U ? 1U : 0U;
            if (blk != 0U && !read(reader, 1U, &snroffste)) return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, "truncated-snroffste", features);
            snrReused = snroffste == 0U;
            if (snroffste) {
                unsigned csnroffst = 0;
                if (!read(reader, 6U, &csnroffst)) return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, "truncated-csnroffst", features);
                features.snrOffsets = features.snrOffsets || csnroffst != 0U;
                if (state.snrStrategy == 1U) {
                    if (!read(reader, 4U, &lfeFsnr)) return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, "truncated-blkfsnroffst", features);
                    features.snrOffsets = features.snrOffsets || lfeFsnr != 0U;
                    std::fill(fsnr.begin(), fsnr.end(), lfeFsnr);
                } else {
                    for (unsigned ch = 0; ch < state.nfchans; ++ch) {
                        if (!read(reader, 4U, &fsnr[ch])) return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, "truncated-fsnroffst", features);
                        features.snrOffsets = features.snrOffsets || fsnr[ch] != 0U;
                    }
                    if (state.lfe && !read(reader, 4U, &lfeFsnr)) return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, "truncated-lfefsnroffst", features);
                    features.snrOffsets = features.snrOffsets || lfeFsnr != 0U;
                }
                std::fill(csnr.begin(), csnr.end(), csnroffst);
            }
        }
        priorCsnr = csnr;
        priorFsnr = fsnr;
        priorLfeFsnr = lfeFsnr;
        std::vector<unsigned> fgain = priorFgain;
        unsigned lfeFgain = priorLfeFgain;
        bool fastGainReused = false;
        if (state.frmfgaincode) {
            unsigned fgaincode = 0;
            if (!read(reader, 1U, &fgaincode)) return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, "truncated-fgaincode", features);
            if (fgaincode != 0U) {
                for (unsigned ch = 0; ch < state.nfchans; ++ch)
                    if (!read(reader, 3U, &fgain[ch])) return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, "truncated-fgaincod", features);
                if (state.lfe && !read(reader, 3U, &lfeFgain)) return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, "truncated-lfefgaincod", features);
            } else {
                fastGainReused = blk != 0U;
            }
            if (blk == 0U && fgaincode == 0U) {
                std::fill(fgain.begin(), fgain.end(), 4U);
                lfeFgain = 4U;
            }
        } else {
            std::fill(fgain.begin(), fgain.end(), 4U);
            lfeFgain = 4U;
        }
        priorFgain = fgain;
        priorLfeFgain = lfeFgain;
        for (unsigned ch = 0; ch < state.nfchans; ++ch) {
            AudblkChannelState &snapshot = blockState.channels[ch];
            snapshot.snrReused = snrReused;
            snapshot.csnroffst = csnr[ch];
            snapshot.fsnroffst = fsnr[ch];
            snapshot.fastGainReused = fastGainReused;
            snapshot.fgaincod = fgain[ch];
        }
        if (state.lfe) {
            AudblkChannelState &snapshot = blockState.channels[state.nfchans];
            snapshot.snrReused = snrReused;
            snapshot.csnroffst = csnr.empty() ? state.frmcsnroffst : csnr.front();
            snapshot.fsnroffst = lfeFsnr;
            snapshot.fastGainReused = fastGainReused;
            snapshot.fgaincod = lfeFgain;
        }
        if (frame.streamType == StreamType::Independent) {
            if (!read(reader, 1U, &value)) return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, "truncated-convsnroffste", features);
            if (value != 0U && !reader.skip(10U)) return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, "truncated-convsnroffst", features);
        }
        if (state.dbaflde) {
            if (!read(reader, 1U, &value)) return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, "truncated-deltbaie", features);
            if (value != 0U) {
                ++features.unsupportedBranches;
                return fail(reader, AudblkDisposition::Unsupported, FailureStage::Header, "delta-bit-allocation-active", features);
            }
        }
        if (state.skipflde) {
            if (!read(reader, 1U, &value)) return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, "truncated-skiple", features);
            if (value != 0U) {
                unsigned length = 0;
                if (!read(reader, 9U, &length) || !reader.skip(length * 8U)) return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, "truncated-skipfld", features);
            }
        }
        const bool allSnrZero = std::all_of(csnr.begin(), csnr.end(), [](unsigned x) { return x == 0U; })
            && std::all_of(fsnr.begin(), fsnr.end(), [](unsigned x) { return x == 0U; })
            && (!state.lfe || lfeFsnr == 0U);
        MantissaDecodeState groupedCursor;
        blockState.groupedCursorShared = true;
        blockState.groupedCursorStartBit = reader.position();
        for (unsigned ch = 0; ch < state.nfchans; ++ch) {
            features.currentChannel = ch;
            features.currentLfe = false;
            blockState.groupedChannelOrder.push_back(ch);
            BitAllocationRequest allocation;
            allocation.fscod = frame.sampleRate == 48000U ? 0U : frame.sampleRate == 44100U ? 1U : 2U;
            allocation.endMant = endMant[ch]; allocation.exponents = channels[ch].exponents;
            allocation.sdcycod = allocationState.sdcy; allocation.fdcycod = allocationState.fdcy; allocation.sgaincod = allocationState.sgain; allocation.dbpbcod = allocationState.dbpb; allocation.floorcod = allocationState.floor;
            allocation.csnroffst = csnr[ch]; allocation.fsnroffst = fsnr[ch]; allocation.fgaincod = fgain[ch];
            allocation.allActiveSnrOffsetsZero = allSnrZero;
            const BitAllocationResult allocated = computeUncoupledFbwBitAllocation(allocation);
            if (allocated.disposition != BitAllocationDisposition::Accepted)
                return fail(reader, allocated.disposition == BitAllocationDisposition::Unsupported ? AudblkDisposition::Unsupported : AudblkDisposition::Malformed, FailureStage::Header, allocated.reason.empty() ? "bit-allocation-syntax" : allocated.reason, features);
            channels[ch].valid = true; channels[ch].endMant = endMant[ch]; channels[ch].bap = allocated.bap;
            AudblkChannelState &snapshot = blockState.channels[ch];
            snapshot.allocation.reused = baReused;
            snapshot.allocation.sdcycod = allocationState.sdcy;
            snapshot.allocation.fdcycod = allocationState.fdcy;
            snapshot.allocation.sgaincod = allocationState.sgain;
            snapshot.allocation.dbpbcod = allocationState.dbpb;
            snapshot.allocation.floorcod = allocationState.floor;
            snapshot.allocation.bap = allocated.bap;
            snapshot.allocation.bapDigest = digestBapVector(allocated.bap);
            snapshot.coefficientStartBit = reader.position();
            DitherSource channelSource = ditherSource;
            if (channelDitherSource) {
                channelSource = [channelDitherSource, ch](
                                    std::size_t coefficientIndex,
                                    unsigned exponent) {
                    return (*channelDitherSource)(ch, coefficientIndex,
                                                  exponent);
                };
            }
            const MantissaDecodeResult mantissas = consumeMantissas(
                bytes, frame.endBit, reader, channels[ch].bap,
                channels[ch].exponents, &groupedCursor, ch, blk,
                !state.lfe && ch + 1U == state.nfchans, dither[ch],
                decodeCoefficients, channelSource);
            recordReservedComposite(mantissas, &features, frame.offset, blk,
                                     ch, false);
            if (mantissas.disposition != MantissaDisposition::Accepted) {
                const AudblkDisposition disposition =
                    mantissas.disposition == MantissaDisposition::Unsupported
                    ? AudblkDisposition::Unsupported
                    : AudblkDisposition::Malformed;
                return fail(reader, disposition, FailureStage::Header,
                            mantissas.reason.empty() ? "mantissa-syntax"
                                                     : mantissas.reason,
                            features);
            }
            snapshot.coefficientEndBit = reader.position();
            if (decodeCoefficients) {
                snapshot.coefficientsDecoded = true;
                snapshot.coefficients = mantissas.transformCoefficients;
                snapshot.coefficientDigest = digestCoefficientVector(
                    snapshot.coefficients);
            }
            snapshot.endBit = reader.position();
        }
        if (state.lfe) {
            features.currentChannel = state.nfchans;
            features.currentLfe = true;
            blockState.groupedChannelOrder.push_back(state.nfchans);
            BitAllocationRequest allocation;
            allocation.fscod = frame.sampleRate == 48000U ? 0U : frame.sampleRate == 44100U ? 1U : 2U;
            allocation.endMant = 7U; allocation.exponents = lfe.exponents;
            allocation.sdcycod = allocationState.sdcy; allocation.fdcycod = allocationState.fdcy; allocation.sgaincod = allocationState.sgain; allocation.dbpbcod = allocationState.dbpb; allocation.floorcod = allocationState.floor;
            allocation.csnroffst = csnr.empty() ? state.frmcsnroffst : csnr.front();
            allocation.fsnroffst = lfeFsnr; allocation.fgaincod = lfeFgain;
            allocation.allActiveSnrOffsetsZero = allSnrZero;
            const BitAllocationResult allocated = computeUncoupledFbwBitAllocation(allocation);
            if (allocated.disposition != BitAllocationDisposition::Accepted)
                return fail(reader, AudblkDisposition::Malformed, FailureStage::Header, "lfe-bit-allocation-syntax", features);
            lfe.valid = true; lfe.endMant = 7U; lfe.bap = allocated.bap;
            AudblkChannelState &snapshot = blockState.channels[state.nfchans];
            snapshot.allocation.reused = baReused;
            snapshot.allocation.sdcycod = allocationState.sdcy;
            snapshot.allocation.fdcycod = allocationState.fdcy;
            snapshot.allocation.sgaincod = allocationState.sgain;
            snapshot.allocation.dbpbcod = allocationState.dbpb;
            snapshot.allocation.floorcod = allocationState.floor;
            snapshot.allocation.bap = allocated.bap;
            snapshot.allocation.bapDigest = digestBapVector(allocated.bap);
            snapshot.coefficientStartBit = reader.position();
            DitherSource channelSource = ditherSource;
            if (channelDitherSource) {
                channelSource = [channelDitherSource, lfeChannel = state.nfchans](
                                    std::size_t coefficientIndex,
                                    unsigned exponent) {
                    return (*channelDitherSource)(lfeChannel,
                                                  coefficientIndex, exponent);
                };
            }
            const MantissaDecodeResult mantissas = consumeMantissas(
                bytes, frame.endBit, reader, lfe.bap, lfe.exponents,
                &groupedCursor, state.nfchans, blk, true, true,
                decodeCoefficients, channelSource);
            recordReservedComposite(mantissas, &features, frame.offset, blk,
                                     state.nfchans, true);
            if (mantissas.disposition != MantissaDisposition::Accepted) {
                const AudblkDisposition disposition =
                    mantissas.disposition == MantissaDisposition::Unsupported
                    ? AudblkDisposition::Unsupported
                    : AudblkDisposition::Malformed;
                return fail(reader, disposition, FailureStage::Header,
                            mantissas.reason.empty() ? "lfe-mantissa-syntax"
                                                     : mantissas.reason,
                            features);
            }
            snapshot.coefficientEndBit = reader.position();
            if (decodeCoefficients) {
                snapshot.coefficientsDecoded = true;
                snapshot.coefficients = mantissas.transformCoefficients;
                snapshot.coefficientDigest = digestCoefficientVector(
                    snapshot.coefficients);
            }
            snapshot.endBit = reader.position();
        }
        blockState.groupedCursorEndBit = reader.position();
        blockState.groupedCursorResetAtEnd = true;
        blockState.endBit = reader.position();
        features.currentChannel = static_cast<unsigned>(-1);
        features.currentLfe = false;
        ++features.blocks;
    }
    features.audblkParsed = true;
    if (referenceDither) {
        frameState.ditherStateEnd = referenceDither->state();
        frameState.ditherSamples = referenceDither->generatedSamples()
            - frameState.ditherSamples;
    }
    const AudblkStateValidationResult stateValidation =
        validateAudblkState(*features.state);
    if (!stateValidation.valid) {
        features.currentBlock = stateValidation.blockIndex;
        features.currentChannel = stateValidation.channelIndex;
        features.currentLfe = stateValidation.channelIsLfe;
        return failAt(stateValidation.bitPosition, AudblkDisposition::Malformed,
                      FailureStage::Validation,
                      std::string("state-") + stateValidation.reason,
                      features);
    }
    AudblkParseResult result;
    result.disposition = AudblkDisposition::Accepted;
    result.stage = FailureStage::None;
    result.reason = "audfrm-audblk-inventory-parsed";
    result.bitPosition = reader.position();
    result.features = features;
    return result;
}

AudblkParseResult parseEac3Audblk(
    const std::vector<std::uint8_t> &bytes,
    const FrameHeader &frame,
    const BsiInfo &bsi)
{
    // Inventory mode intentionally does not require a dither source: bap=0
    // syntax is still consumed/validated, but no coefficient values are
    // materialized.
    return parseEac3AudblkInternal(bytes, frame, bsi, false, DitherSource{},
                                   nullptr, nullptr);
}

AudblkParseResult parseEac3AudblkCoefficients(
    const std::vector<std::uint8_t> &bytes,
    const FrameHeader &frame,
    const BsiInfo &bsi,
    const DitherSource &ditherSource)
{
    return parseEac3AudblkInternal(bytes, frame, bsi, true, ditherSource,
                                   nullptr, nullptr);
}

AudblkParseResult parseEac3AudblkCoefficients(
    const std::vector<std::uint8_t> &bytes,
    const FrameHeader &frame,
    const BsiInfo &bsi,
    ReferenceDitherSource &ditherSource)
{
    return parseEac3AudblkInternal(bytes, frame, bsi, true,
                                   ditherSource.callback(), &ditherSource,
                                   nullptr);
}

AudblkParseResult parseEac3AudblkCoefficients(
    const std::vector<std::uint8_t> &bytes,
    const FrameHeader &frame,
    const BsiInfo &bsi,
    const ChannelDitherSource &ditherSource)
{
    if (!ditherSource) {
        AudblkFeatures features;
        return failAt(frame.offset * 8U, AudblkDisposition::Unsupported,
                      FailureStage::Validation,
                      "channel-dither-source-required", features);
    }
    return parseEac3AudblkInternal(bytes, frame, bsi, true, DitherSource{},
                                   nullptr, &ditherSource);
}

const char *toString(AudblkDisposition value)
{
    switch (value) {
    case AudblkDisposition::Accepted: return "accepted";
    case AudblkDisposition::Unsupported: return "unsupported";
    case AudblkDisposition::Malformed: return "malformed";
    }
    return "unknown";
}

} // namespace eac3native
