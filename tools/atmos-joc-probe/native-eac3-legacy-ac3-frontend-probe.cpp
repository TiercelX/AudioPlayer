// Gate 8N-5d: a deliberately narrow Legacy AC-3 audfrm/audblk frontend.
//
// This is a probe-local diagnostic acceptance seam, not a production decoder.
// It accepts only the supplied six-block 48 kHz acmod=7+LFE shape and only an
// explicitly conservative uncoupled profile.  It reuses the native bounded
// reader, exponent, bit-allocation, and mantissa cursor primitives, then
// materializes deterministic renderer-neutral coefficient values.  No IMDCT,
// overlap, DRC, JOC, renderer, dependent assembly, or production playback
// claim is made here; the bounded PCM result is diagnostic-only.

#include "native-eac3-bit-allocation.h"
#include "native-eac3-block-state.h"
#include "native-eac3-core.h"
#include "native-eac3-legacy-ac3-diagnostic.h"
#include "native-eac3-exponents.h"
#include "native-eac3-mantissas.h"
#include "native-eac3-transform.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace legacyac3 {
using namespace eac3native;

struct ChannelState {
    bool valid = false;
    unsigned bandwidth = 0U;
    std::vector<unsigned> exponents;
    std::vector<unsigned> bap;
};

namespace {

constexpr unsigned kRate = 48000U;
constexpr unsigned kBlocks = 6U;
constexpr unsigned kFullChannels = 5U;
constexpr unsigned kTotalChannels = 6U;

void hash(std::uint64_t *digest, std::uint64_t value)
{
    if (!digest) return;
    *digest ^= value + 0x9e3779b97f4a7c15ULL + (*digest << 6U)
        + (*digest >> 2U);
}

bool summarizeSamples(const std::vector<double> &samples,
                      std::uint64_t *digest, double *peak, double *rms,
                      std::string *reason)
{
    if (!digest || !peak || !rms) return false;
    double scale = 0.0;
    double scaledSquares = 0.0;
    for (double sample : samples) {
        if (!std::isfinite(sample)) {
            if (reason) *reason = "legacy-pcm-non-finite";
            return false;
        }
        const double magnitude = std::abs(sample);
        if (magnitude > *peak) *peak = magnitude;
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
        if (reason) *reason = "legacy-pcm-rms-non-finite";
        return false;
    }
    *digest = digestCoefficientVector(samples);
    return true;
}

bool read(BoundedBitReader *reader, unsigned count, unsigned *value)
{
    std::uint32_t raw = 0U;
    if (!reader || !value || !reader->read(count, &raw)) return false;
    *value = static_cast<unsigned>(raw);
    return true;
}

bool skip(BoundedBitReader *reader, unsigned count)
{
    return reader && reader->skip(count);
}

void setBits(std::vector<std::uint8_t> *bytes, std::size_t bitOffset,
             unsigned count, unsigned value)
{
    if (!bytes || count > 32U || bitOffset + count > bytes->size() * 8U)
        return;
    for (unsigned index = 0U; index < count; ++index) {
        const unsigned shift = count - index - 1U;
        const std::uint8_t bit = static_cast<std::uint8_t>(
            (value >> shift) & 1U);
        (*bytes)[(bitOffset + index) / 8U] = static_cast<std::uint8_t>(
            (*bytes)[(bitOffset + index) / 8U]
            | (bit << (7U - ((bitOffset + index) % 8U))));
    }
}

enum class Strategy { D15, D25, D45, Reuse };

unsigned groupSize(Strategy strategy)
{
    switch (strategy) {
    case Strategy::D15: return 1U;
    case Strategy::D25: return 2U;
    case Strategy::D45: return 4U;
    case Strategy::Reuse: return 0U;
    }
    return 0U;
}

ExponentStrategy nativeStrategy(Strategy strategy)
{
    switch (strategy) {
    case Strategy::D15: return ExponentStrategy::D15;
    case Strategy::D25: return ExponentStrategy::D25;
    case Strategy::D45: return ExponentStrategy::D45;
    case Strategy::Reuse: return ExponentStrategy::Reuse;
    }
    return ExponentStrategy::Reuse;
}

// AC-3 chexpstr uses the same compact strategy numbering as the native
// E-AC-3 exponent primitive: 00 is reuse, then D15/D25/D45.
Strategy strategy(unsigned code)
{
    switch (code) {
    case 1U: return Strategy::D15;
    case 2U: return Strategy::D25;
    case 3U: return Strategy::D45;
    default: return Strategy::Reuse;
    }
}

bool decodeExponents(BoundedBitReader *reader, Strategy mode,
                     unsigned endMant, ChannelState *state,
                     bool block0, std::string *reason)
{
    if (!reader || !state || endMant == 0U) return false;
    if (mode == Strategy::Reuse) {
        if (block0 || !state->valid || state->bandwidth + 73U != endMant) {
            if (reason) *reason = "legacy-exponent-reuse-without-prior";
            return false;
        }
        return true;
    }
    const unsigned size = groupSize(mode);
    unsigned absolute = 0U;
    if (!read(reader, 4U, &absolute)) {
        if (reason) *reason = "legacy-truncated-absolute-exponent";
        return false;
    }
    const unsigned groups = (endMant - 1U + (3U * size) - 1U)
        / (3U * size);
    std::vector<unsigned> codes;
    codes.reserve(groups);
    for (unsigned i = 0U; i < groups; ++i) {
        unsigned code = 0U;
        if (!read(reader, 7U, &code)) {
            if (reason) *reason = "legacy-truncated-exponent-groups";
            return false;
        }
        codes.push_back(code);
    }
    ExponentDecodeRequest request;
    request.strategy = nativeStrategy(mode);
    request.absoluteExponent = absolute;
    request.groupedCodes = std::move(codes);
    request.targetCoefficientCount = endMant;
    request.block0 = block0;
    request.bandwidthCode = state->bandwidth;
    const ExponentDecodeResult decoded = decodeExponentSet(request);
    if (decoded.disposition != ExponentDisposition::Accepted) {
        if (reason) *reason = decoded.reason.empty()
            ? "legacy-exponent-syntax" : decoded.reason;
        return false;
    }
    state->valid = true;
    state->exponents = decoded.exponents;
    return true;
}

bool decodeLfeBody(BoundedBitReader *reader, bool block0, bool newSet,
                   ChannelState *state, std::string *reason)
{
    if (!reader || !state) return false;
    if (!newSet) {
        if (block0 || !state->valid) {
            if (reason) *reason = "legacy-lfe-exponent-reuse-without-prior";
            return false;
        }
        return true;
    }
    unsigned absolute = 0U;
    if (!read(reader, 4U, &absolute)) {
        if (reason) *reason = "legacy-truncated-lfe-absolute-exponent";
        return false;
    }
    std::vector<unsigned> codes;
    for (unsigned i = 0U; i < 2U; ++i) {
        unsigned code = 0U;
        if (!read(reader, 7U, &code)) {
            if (reason) *reason = "legacy-truncated-lfe-exponents";
            return false;
        }
        codes.push_back(code);
    }
    const ExponentDecodeResult decoded = decodeLfeExponents(
        absolute, codes, ExponentStrategy::D15, block0, 7U);
    if (decoded.disposition != ExponentDisposition::Accepted) {
        if (reason) *reason = decoded.reason.empty()
            ? "legacy-lfe-exponent-syntax" : decoded.reason;
        return false;
    }
    state->valid = true;
    state->bandwidth = 0U;
    state->exponents = decoded.exponents;
    return true;
}

bool parseBsi(const std::vector<std::uint8_t> &bytes, const FrameHeader &frame,
              std::size_t *endBit, std::string *reason)
{
    if (!endBit || frame.streamType != StreamType::LegacyAc3
        || frame.offset > bytes.size() || frame.endBit > bytes.size() * 8U
        || frame.endBit <= frame.offset * 8U + 56U) {
        if (reason) *reason = "legacy-bsi-boundary";
        return false;
    }
    BoundedBitReader reader(bytes.data(), bytes.size(),
                            frame.offset * 8U + 56U, frame.endBit);
    unsigned value = 0U;
    if (!read(&reader, 5U, &value) || !read(&reader, 1U, &value)) {
        if (reason) *reason = "legacy-truncated-dialnorm";
        return false;
    }
    if (value != 0U && !skip(&reader, 8U)) {
        if (reason) *reason = "legacy-truncated-compr";
        return false;
    }
    if (!read(&reader, 1U, &value)) {
        if (reason) *reason = "legacy-truncated-langcode";
        return false;
    }
    if (value != 0U && !skip(&reader, 8U)) {
        if (reason) *reason = "legacy-truncated-langcod";
        return false;
    }
    if (!read(&reader, 1U, &value)) {
        if (reason) *reason = "legacy-truncated-audprodie";
        return false;
    }
    if (value != 0U && (!skip(&reader, 5U) || !skip(&reader, 2U))) {
        if (reason) *reason = "legacy-truncated-audprodi";
        return false;
    }
    if (!read(&reader, 1U, &value) || !read(&reader, 1U, &value)) {
        if (reason) *reason = "legacy-truncated-copyright";
        return false;
    }
    if (!read(&reader, 1U, &value)) {
        if (reason) *reason = "legacy-truncated-timecod1e";
        return false;
    }
    if (value != 0U && !skip(&reader, 14U)) {
        if (reason) *reason = "legacy-truncated-timecod1";
        return false;
    }
    if (!read(&reader, 1U, &value)) {
        if (reason) *reason = "legacy-truncated-timecod2e";
        return false;
    }
    if (value != 0U && !skip(&reader, 14U)) {
        if (reason) *reason = "legacy-truncated-timecod2";
        return false;
    }
    if (!read(&reader, 1U, &value)) {
        if (reason) *reason = "legacy-truncated-addbsie";
        return false;
    }
    if (value != 0U) {
        unsigned addbsil = 0U;
        if (!read(&reader, 6U, &addbsil)
            || !skip(&reader, (addbsil + 1U) * 8U)) {
            if (reason) *reason = "legacy-truncated-addbsi";
            return false;
        }
    }
    *endBit = reader.position();
    return true;
}

enum class PreambleDisposition { Accepted, Unsupported, Malformed };

struct PreambleResult {
    PreambleDisposition disposition = PreambleDisposition::Malformed;
    std::string reason;
    unsigned blockSwitchMask = 0U;
    unsigned ditherMask = 0U;
    bool dynrngPresent = false;
    bool rematrix = false;
    bool cplstre = false;
    bool coupling = false;
};

PreambleResult parseBlockPreamble(BoundedBitReader *reader, unsigned acmod,
                                  unsigned block, bool priorCoupling)
{
    PreambleResult result;
    unsigned value = 0U;
    if (!read(reader, kFullChannels, &result.blockSwitchMask)
        || !read(reader, kFullChannels, &result.ditherMask)) {
        result.reason = "legacy-truncated-block-switch-dither";
        return result;
    }
    if (!read(reader, 1U, &value)) {
        result.reason = "legacy-truncated-dynrnge";
        return result;
    }
    result.dynrngPresent = value != 0U;
    if (result.dynrngPresent && !skip(reader, 8U)) {
        result.reason = "legacy-truncated-dynrng";
        return result;
    }
    if (acmod == 2U) {
        if (!read(reader, 1U, &value)) {
            result.reason = "legacy-truncated-rematrix";
            return result;
        }
        result.rematrix = value != 0U;
        if (result.rematrix) {
            result.disposition = PreambleDisposition::Unsupported;
            result.reason = "legacy-rematrix-not-in-config4-profile";
            return result;
        }
    }
    if (acmod <= 1U) {
        result.disposition = PreambleDisposition::Accepted;
        return result;
    }
    if (!read(reader, 1U, &value)) {
        result.reason = "legacy-truncated-cplstre";
        return result;
    }
    result.cplstre = value != 0U;
    if (!result.cplstre) {
        if (block == 0U) {
            result.reason = "legacy-cplstre-reuse-on-block-zero";
            return result;
        }
        result.coupling = priorCoupling;
    } else {
        if (!read(reader, 1U, &value)) {
            result.reason = "legacy-truncated-cplinu";
            return result;
        }
        result.coupling = value != 0U;
        if (result.coupling) {
            result.disposition = PreambleDisposition::Unsupported;
            result.reason = "legacy-coupling-active";
            return result;
        }
    }
    result.disposition = PreambleDisposition::Accepted;
    return result;
}

struct ExponentPlan {
    std::array<unsigned, kFullChannels> strategyCodes{};
    std::array<unsigned, kFullChannels> bandwidthCodes{};
    bool lfeNew = false;
};

bool readExponentPlan(BoundedBitReader *reader, unsigned block,
                      ExponentPlan *plan, std::string *reason)
{
    if (!reader || !plan) return false;
    for (unsigned ch = 0U; ch < kFullChannels; ++ch) {
        if (!read(reader, 2U, &plan->strategyCodes[ch])) {
            if (reason) *reason = "legacy-truncated-chexpstr";
            return false;
        }
    }
    unsigned value = 0U;
    if (!read(reader, 1U, &value)) {
        if (reason) *reason = "legacy-truncated-lfeexpstr";
        return false;
    }
    plan->lfeNew = value != 0U;
    if (block == 0U && !plan->lfeNew) {
        if (reason) *reason = "legacy-lfe-exponent-reuse-without-prior";
        return false;
    }
    for (unsigned ch = 0U; ch < kFullChannels; ++ch) {
        if (strategy(plan->strategyCodes[ch]) == Strategy::Reuse) continue;
        if (!read(reader, 6U, &plan->bandwidthCodes[ch])) {
            if (reason) *reason = "legacy-truncated-chbwcod";
            return false;
        }
        if (plan->bandwidthCodes[ch] > 60U) {
            if (reason) *reason = "legacy-invalid-chbwcod";
            return false;
        }
    }
    return true;
}

bool readSnrAndGain(BoundedBitReader *reader,
                    std::array<unsigned, kTotalChannels> *csnr,
                    std::array<unsigned, kTotalChannels> *fsnr,
                    std::array<unsigned, kTotalChannels> *fgain,
                    bool *present, std::string *reason)
{
    if (!reader || !csnr || !fsnr || !fgain || !present) return false;
    unsigned value = 0U;
    if (!read(reader, 1U, &value)) {
        if (reason) *reason = "legacy-truncated-snroffste";
        return false;
    }
    *present = value != 0U;
    if (!*present) return true;
    if (!read(reader, 6U, &(*csnr)[0])) {
        if (reason) *reason = "legacy-truncated-csnroffst";
        return false;
    }
    for (unsigned ch = 0U; ch < kFullChannels; ++ch) {
        if (!read(reader, 4U, &(*fsnr)[ch])
            || !read(reader, 3U, &(*fgain)[ch])) {
            if (reason) *reason = "legacy-truncated-fsnroffst-fgaincod";
            return false;
        }
    }
    if (!read(reader, 4U, &(*fsnr)[kFullChannels])
        || !read(reader, 3U, &(*fgain)[kFullChannels])) {
        if (reason) *reason = "legacy-truncated-lfefsnroffst-lfefgaincod";
        return false;
    }
    csnr->fill((*csnr)[0]);
    return true;
}

bool decodeChannelMantissas(const std::vector<std::uint8_t> &bytes,
                            std::size_t frameEndBit, BoundedBitReader *reader,
                            ChannelState *state, unsigned channel,
                            unsigned block, bool dither, bool endOfBlock,
                            MantissaDecodeState *sharedCursor,
                            ReferenceDitherSource *ditherSource,
                            ChannelBoundary *boundary,
                            Disposition *failureDisposition,
                            std::string *reason)
{
    if (!reader || !state || !sharedCursor || !boundary) return false;
    MantissaBitReader mantissa(bytes, frameEndBit, reader->position());
    MantissaDecodeRequest request;
    request.bap = state->bap;
    request.exponents = state->exponents;
    request.channelId = channel;
    request.audioBlockId = block;
    request.endOfBlock = endOfBlock;
    request.dithflag = dither;
    request.decodeValues = true;
    request.rejectReservedComposite = true;
    request.sharedBlockCursor = true;
    if (ditherSource) request.ditherSource = ditherSource->callback();
    const MantissaDecodeResult result = decodeMantissas(
        mantissa, request, *sharedCursor);
    if (result.disposition != MantissaDisposition::Accepted) {
        if (failureDisposition) *failureDisposition =
            result.disposition == MantissaDisposition::Unsupported
                ? Disposition::Unsupported : Disposition::Malformed;
        if (reason) *reason = result.reason.empty()
            ? "legacy-mantissa-cursor" : result.reason;
        return false;
    }
    if (!reader->skip(static_cast<unsigned>(result.bitsConsumed))) {
        if (failureDisposition) *failureDisposition = Disposition::Malformed;
        if (reason) *reason = "legacy-mantissa-reader-advance";
        return false;
    }
    if (result.transformCoefficients.size() != state->exponents.size()) {
        if (failureDisposition) *failureDisposition = Disposition::Malformed;
        if (reason) *reason = "legacy-coefficient-count";
        return false;
    }
    for (double coefficient : result.transformCoefficients) {
        if (!std::isfinite(coefficient) || std::abs(coefficient) > 1.0) {
            if (failureDisposition) *failureDisposition = Disposition::Malformed;
            if (reason) *reason = "legacy-coefficient-range";
            return false;
        }
    }
    boundary->coefficients = result.transformCoefficients;
    boundary->coefficientDigest = digestCoefficientVector(
        boundary->coefficients);
    return true;
}

Result parseInternal(const std::vector<std::uint8_t> &bytes,
                     StatefulDecoder *state, bool emitEos)
{
    Result result;
    const ParseResult parsed = parseSyncframe(bytes, 0U);
    if (parsed.disposition != eac3native::Disposition::Accepted || !parsed.frame) {
        result.disposition = parsed.disposition == eac3native::Disposition::Unsupported
            ? Disposition::Unsupported : Disposition::Malformed;
        result.reason = "legacy-frame-" + parsed.reason;
        return result;
    }
    const FrameHeader &frame = *parsed.frame;
    if (frame.sizeBytes != bytes.size()) {
        result.disposition = Disposition::Malformed;
        result.reason = "legacy-frame-must-be-complete";
        return result;
    }
    if (frame.streamType != StreamType::LegacyAc3 || frame.substreamId != 0U
        || frame.sampleRate != kRate || frame.blocks != kBlocks
        || frame.acmod != 7U || !frame.lfe
        || frame.channelCount != kTotalChannels) {
        result.disposition = Disposition::Unsupported;
        result.reason = "legacy-config4-six-block-topology-required";
        return result;
    }
    std::size_t bsiEnd = 0U;
    if (!parseBsi(bytes, frame, &bsiEnd, &result.reason)) {
        result.disposition = Disposition::Malformed;
        return result;
    }
    BoundedBitReader reader(bytes.data(), bytes.size(), bsiEnd, frame.endBit);
    std::array<ChannelState, kTotalChannels> channels{};
    result.stateDigest = 0xcbf29ce484222325ULL;
    hash(&result.stateDigest, bsiEnd);
    hash(&result.stateDigest, frame.endBit);
    std::array<ReferenceDitherSource, kTotalChannels> localDitherSources{};
    std::array<Eac3TransformChannel, kTotalChannels> localTransformChannels{};
    auto &ditherSources = state ? state->ditherSources : localDitherSources;
    auto &transformChannels = state ? state->transformChannels
                                    : localTransformChannels;
    std::array<std::vector<double>, kTotalChannels> pcmSamples{};
    unsigned baSdcy = 2U, baFdcy = 1U, baSgain = 1U;
    unsigned baDbpb = 2U, baFloor = 7U;
    std::array<unsigned, kTotalChannels> csnr{};
    std::array<unsigned, kTotalChannels> fsnr{};
    std::array<unsigned, kTotalChannels> fgain{};
    fgain.fill(4U);
    bool priorCoupling = false;
    for (unsigned block = 0U; block < kBlocks; ++block) {
        BlockBoundary state;
        state.block = block;
        state.startBit = reader.position();
        const PreambleResult preamble = parseBlockPreamble(
            &reader, frame.acmod, block, priorCoupling);
        if (preamble.disposition != PreambleDisposition::Accepted) {
            result.disposition = preamble.disposition == PreambleDisposition::Unsupported
                ? Disposition::Unsupported : Disposition::Malformed;
            result.reason = preamble.reason;
            state.blockSwitchMask = preamble.blockSwitchMask;
            state.ditherMask = preamble.ditherMask;
            state.dynrngPresent = preamble.dynrngPresent;
            state.rematrix = preamble.rematrix;
            state.coupling = preamble.coupling;
            state.endBit = reader.position();
            result.blocks.push_back(std::move(state));
            result.bitPosition = reader.position();
            return result;
        }
        state.blockSwitchMask = preamble.blockSwitchMask;
        state.ditherMask = preamble.ditherMask;
        state.dynrngPresent = preamble.dynrngPresent;
        state.rematrix = preamble.rematrix;
        state.coupling = preamble.coupling;
        priorCoupling = preamble.coupling;
        unsigned value = 0U;
        ExponentPlan exponentPlan;
        std::string planReason;
        if (!readExponentPlan(&reader, block, &exponentPlan, &planReason)) {
            result.disposition = Disposition::Malformed;
            result.reason = planReason;
            state.endBit = reader.position();
            result.blocks.push_back(std::move(state));
            result.bitPosition = reader.position();
            return result;
        }
        for (unsigned ch = 0U; ch < kFullChannels; ++ch) {
            const unsigned code = exponentPlan.strategyCodes[ch];
            const Strategy mode = strategy(code);
            ChannelState &channel = channels[ch];
            const bool reused = mode == Strategy::Reuse;
            ChannelBoundary boundary;
            boundary.channel = ch;
            boundary.exponentReused = reused;
            boundary.bandwidthReused = reused;
            state.channels.push_back(boundary);
            if (!reused) {
                channel.bandwidth = exponentPlan.bandwidthCodes[ch];
                const unsigned endMant = 73U + 3U * channel.bandwidth;
                std::string reason;
                if (!decodeExponents(&reader, mode, endMant, &channel,
                                     block == 0U, &reason)) {
                    result.disposition = Disposition::Unsupported;
                    result.reason = "legacy-exponent-profile-unsupported-"
                        + reason;
                    state.endBit = reader.position();
                    result.blocks.push_back(std::move(state));
                    result.bitPosition = reader.position();
                    return result;
                }
            } else if (!channel.valid) {
                result.disposition = Disposition::Malformed;
                result.reason = "legacy-channel-exponent-reuse-without-prior";
                result.bitPosition = reader.position();
                return result;
            }
            if (!reused && !read(&reader, 2U, &value)) {
                result.reason = "legacy-truncated-gainrng";
                result.bitPosition = reader.position();
                return result;
            }
            state.channels.back().coefficientStartBit = reader.position();
            hash(&result.stateDigest, code);
            hash(&result.stateDigest, channel.bandwidth);
            hash(&result.stateDigest, channel.exponents.size());
        }
        ChannelState &lfe = channels[kFullChannels];
        std::string lfeReason;
        if (!decodeLfeBody(&reader, block == 0U, exponentPlan.lfeNew,
                           &lfe, &lfeReason)) {
            result.disposition = Disposition::Malformed;
            result.reason = lfeReason;
            result.bitPosition = reader.position();
            return result;
        }
        ChannelBoundary lfeBoundary;
        lfeBoundary.channel = kFullChannels;
        lfeBoundary.lfe = true;
        lfeBoundary.coefficientStartBit = reader.position();
        state.channels.push_back(lfeBoundary);
        if (!read(&reader, 1U, &value)) {
            result.reason = "legacy-truncated-baie";
            result.bitPosition = reader.position();
            return result;
        }
        if (value != 0U) {
            if (!read(&reader, 2U, &baSdcy) || !read(&reader, 2U, &baFdcy)
                || !read(&reader, 2U, &baSgain) || !read(&reader, 2U, &baDbpb)
                || !read(&reader, 3U, &baFloor)) {
                result.reason = "legacy-truncated-bit-allocation";
                result.bitPosition = reader.position();
                return result;
            }
        }
        bool snrPresent = false;
        std::string snrReason;
        if (!readSnrAndGain(&reader, &csnr, &fsnr, &fgain,
                            &snrPresent, &snrReason)) {
            result.reason = snrReason;
            result.bitPosition = reader.position();
            return result;
        }
        unsigned dba = 0U;
        if (!read(&reader, 1U, &dba)) {
            result.reason = "legacy-truncated-deltbaie";
            result.bitPosition = reader.position();
            return result;
        }
        if (dba != 0U) {
            for (unsigned ch = 0U; ch < kFullChannels; ++ch) {
                unsigned mode = 0U;
                if (!read(&reader, 2U, &mode)) {
                    result.reason = "legacy-truncated-deltbae";
                    result.bitPosition = reader.position();
                    return result;
                }
                if (mode != 2U) {
                    result.disposition = Disposition::Unsupported;
                    result.reason = mode == 0U
                        ? "legacy-deltbae-reuse-unsupported-channel-"
                        : mode == 1U
                        ? "legacy-deltbae-new-unsupported-channel-"
                        : "legacy-deltbae-reserved-channel-";
                    result.reason += std::to_string(ch);
                    result.bitPosition = reader.position();
                    return result;
                }
            }
        }
        unsigned skipField = 0U;
        if (!read(&reader, 1U, &skipField)) {
            result.reason = "legacy-truncated-skiple";
            result.bitPosition = reader.position();
            return result;
        }
        if (skipField) {
            unsigned length = 0U;
            if (!read(&reader, 9U, &length) || !skip(&reader, length * 8U)) {
                result.reason = "legacy-truncated-skipfld";
                result.bitPosition = reader.position();
                return result;
            }
        }
        MantissaDecodeState blockMantissaCursor;
        for (unsigned ch = 0U; ch < kTotalChannels; ++ch) {
            ChannelState &channel = channels[ch];
            BitAllocationRequest request;
            request.fscod = 0U;
            request.endMant = ch == kFullChannels ? 7U
                                                   : 73U + 3U * channel.bandwidth;
            request.exponents = channel.exponents;
            request.sdcycod = baSdcy;
            request.fdcycod = baFdcy;
            request.sgaincod = baSgain;
            request.dbpbcod = baDbpb;
            request.floorcod = baFloor;
            request.csnroffst = csnr[ch];
            request.fsnroffst = fsnr[ch];
            request.fgaincod = fgain[ch];
            request.allActiveSnrOffsetsZero = true;
            for (unsigned offset : csnr)
                request.allActiveSnrOffsetsZero =
                    request.allActiveSnrOffsetsZero && offset == 0U;
            for (unsigned offset : fsnr)
                request.allActiveSnrOffsetsZero =
                    request.allActiveSnrOffsetsZero && offset == 0U;
            const BitAllocationResult allocated =
                computeUncoupledFbwBitAllocation(request);
            if (allocated.disposition != BitAllocationDisposition::Accepted) {
                result.disposition = allocated.disposition == BitAllocationDisposition::Unsupported
                    ? Disposition::Unsupported : Disposition::Malformed;
                result.reason = allocated.reason.empty()
                    ? "legacy-bit-allocation" : allocated.reason;
                result.bitPosition = reader.position();
                return result;
            }
            channel.bap = allocated.bap;
            ChannelBoundary &boundary = state.channels[ch];
            boundary.coefficientStartBit = reader.position();
            std::string mantissaReason;
            Disposition mantissaDisposition = Disposition::Malformed;
            if (!decodeChannelMantissas(bytes, frame.endBit, &reader, &channel,
                                        ch, block, (state.ditherMask & (1U << ch)) != 0U,
                                        ch + 1U == kTotalChannels,
                                        &blockMantissaCursor,
                                        &ditherSources[ch], &boundary,
                                        &mantissaDisposition,
                                        &mantissaReason)) {
                result.disposition = mantissaDisposition;
                result.reason = mantissaReason;
                result.bitPosition = reader.position();
                return result;
            }
            const std::size_t expectedCoefficientCount = ch == kFullChannels
                ? 7U : 217U;
            if (boundary.coefficients.size() != expectedCoefficientCount) {
                result.disposition = Disposition::Unsupported;
                result.reason = "legacy-transform-coefficient-shape";
                result.bitPosition = reader.position();
                return result;
            }
            const bool blockSwitch = ch < kFullChannels
                && (state.blockSwitchMask & (1U << ch)) != 0U;
            const TransformOutput transformed = transformChannels[ch].processBlock(
                boundary.coefficients, blockSwitch);
            if (!transformed.ok
                || transformed.samples.size() != kEac3OutputSamples) {
                result.disposition = Disposition::Malformed;
                result.reason = transformed.error.empty()
                    ? "legacy-transform-block" : "legacy-transform-"
                        + transformed.error;
                result.bitPosition = reader.position();
                return result;
            }
            pcmSamples[ch].insert(pcmSamples[ch].end(),
                                  transformed.samples.begin(),
                                  transformed.samples.end());
            boundary.coefficientEndBit = reader.position();
            result.coefficientCount += boundary.coefficients.size();
            hash(&result.stateDigest, boundary.coefficientStartBit);
            hash(&result.stateDigest, boundary.coefficientEndBit);
            hash(&result.stateDigest, boundary.coefficientDigest);
            hash(&result.stateDigest, boundary.coefficients.size());
        }
        state.endBit = reader.position();
        result.blocks.push_back(std::move(state));
        hash(&result.stateDigest, result.blocks.back().endBit);
    }
    result.pcm.resize(kTotalChannels);
    for (unsigned ch = 0U; ch < kTotalChannels; ++ch) {
        TransformFlushOutput tail;
        if (emitEos) tail = transformChannels[ch].flush();
        if (emitEos && (!tail.ok || !tail.hadTail
                        || tail.samples.size() != kEac3OutputSamples)) {
            result.disposition = Disposition::Malformed;
            result.reason = tail.error.empty()
                ? "legacy-transform-eos-boundary" : "legacy-transform-"
                    + tail.error;
            result.bitPosition = reader.position();
            return result;
        }
        if (pcmSamples[ch].size() != kBlocks * kEac3OutputSamples) {
            result.disposition = Disposition::Malformed;
            result.reason = "legacy-transform-sample-count";
            result.bitPosition = reader.position();
            return result;
        }
        PcmSummary &summary = result.pcm[ch];
        summary.channel = ch;
        summary.lfe = ch == kFullChannels;
        summary.sampleCount = pcmSamples[ch].size();
        summary.eosTailCount = tail.samples.size();
        std::string pcmReason;
        if (!summarizeSamples(pcmSamples[ch], &summary.digest, &summary.peak,
                              &summary.rms, &pcmReason)
            || (emitEos && !summarizeSamples(tail.samples, &summary.eosDigest,
                                             &summary.eosPeak, &summary.eosRms,
                                             &pcmReason))) {
            result.disposition = Disposition::Malformed;
            result.reason = pcmReason.empty() ? "legacy-pcm-summary" : pcmReason;
            result.bitPosition = reader.position();
            return result;
        }
        summary.samples = std::move(pcmSamples[ch]);
        if (emitEos) summary.eosTail = std::move(tail.samples);
        hash(&result.stateDigest, summary.sampleCount);
        hash(&result.stateDigest, summary.eosTailCount);
        hash(&result.stateDigest, summary.digest);
        hash(&result.stateDigest, summary.eosDigest);
    }
    result.disposition = Disposition::Accepted;
    result.reason = "legacy-six-block-diagnostic-pcm";
    result.bitPosition = reader.position();
    return result;
}

std::string hex(std::uint64_t value)
{
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << value;
    return output.str();
}

bool finiteSamples(const std::vector<double> &samples)
{
    return std::all_of(samples.begin(), samples.end(),
                       [](double value) { return std::isfinite(value); });
}

bool zeroSamples(const std::vector<double> &samples)
{
    return std::all_of(samples.begin(), samples.end(),
                       [](double value) { return value == 0.0; });
}

bool nonZeroSamples(const std::vector<double> &samples)
{
    return std::any_of(samples.begin(), samples.end(),
                       [](double value) { return value != 0.0; });
}

bool transformSelfTest()
{
    const std::vector<double> zeros(kEac3TransformCoefficients, 0.0);
    {
        Eac3TransformChannel channel;
        const TransformOutput block = channel.processBlock(zeros, false);
        const TransformFlushOutput tail = channel.flush();
        if (!block.ok || block.samples.size() != kEac3OutputSamples
            || !zeroSamples(block.samples) || !tail.ok || !tail.hadTail
            || tail.samples.size() != kEac3OutputSamples
            || !zeroSamples(tail.samples)) {
            std::cerr << "selfTest=FAIL case=transform-all-zero\n";
            return false;
        }
    }
    {
        Eac3TransformChannel channel;
        const TransformOutput block = channel.processBlock({1.0}, false);
        const TransformFlushOutput tail = channel.flush();
        if (!block.ok || !nonZeroSamples(block.samples) || !tail.ok
            || !tail.hadTail || !finiteSamples(tail.samples)) {
            std::cerr << "selfTest=FAIL case=transform-impulse\n";
            return false;
        }
    }
    {
        Eac3TransformChannel channel;
        std::vector<double> singleBin(kEac3TransformCoefficients, 0.0);
        singleBin[37U] = 1.0;
        const TransformOutput block = channel.processBlock(singleBin, true);
        const TransformFlushOutput tail = channel.flush();
        if (!block.ok || !finiteSamples(block.samples) || !tail.ok
            || !tail.hadTail || !finiteSamples(tail.samples)
            || (!nonZeroSamples(block.samples) && !nonZeroSamples(tail.samples))) {
            std::cerr << "selfTest=FAIL case=transform-single-bin\n";
            return false;
        }
    }
    {
        std::vector<double> coefficients(kEac3TransformCoefficients, 0.0);
        coefficients[0U] = 1.0;
        Eac3TransformChannel longChannel;
        Eac3TransformChannel shortChannel;
        const TransformOutput longBlock = longChannel.processBlock(
            coefficients, false);
        const TransformOutput shortBlock = shortChannel.processBlock(
            coefficients, true);
        if (!longBlock.ok || !shortBlock.ok
            || longBlock.samples == shortBlock.samples) {
            std::cerr << "selfTest=FAIL case=transform-long-short\n";
            return false;
        }
    }
    {
        Eac3TransformChannel active;
        Eac3TransformChannel silent;
        const TransformOutput activeBlock = active.processBlock({1.0}, false);
        const TransformOutput silentBlock = silent.processBlock(zeros, false);
        const TransformFlushOutput activeTail = active.flush();
        const TransformFlushOutput silentTail = silent.flush();
        if (!activeBlock.ok || !silentBlock.ok
            || !nonZeroSamples(activeBlock.samples)
            || !zeroSamples(silentBlock.samples) || !activeTail.ok
            || !silentTail.ok || !nonZeroSamples(activeTail.samples)
            || !zeroSamples(silentTail.samples)) {
            std::cerr << "selfTest=FAIL case=transform-channel-isolation\n";
            return false;
        }
    }
    {
        Eac3TransformChannel channel;
        const TransformOutput first = channel.processBlock({1.0}, false);
        channel.reset();
        const TransformFlushOutput noTail = channel.flush();
        channel.reset();
        const TransformOutput afterReset = channel.processBlock(zeros, false);
        if (!first.ok || !noTail.ok || noTail.hadTail || !noTail.samples.empty()
            || !afterReset.ok || !zeroSamples(afterReset.samples)) {
            std::cerr << "selfTest=FAIL case=transform-reset\n";
            return false;
        }
    }
    {
        Eac3TransformChannel channel;
        const TransformOutput block = channel.processBlock(zeros, false);
        const TransformFlushOutput first = channel.flush();
        const TransformFlushOutput second = channel.flush();
        const TransformOutput afterEos = channel.processBlock(zeros, false);
        if (!block.ok || !first.ok || !first.hadTail
            || first.samples.size() != kEac3OutputSamples || !second.ok
            || second.hadTail || !second.samples.empty() || afterEos.ok) {
            std::cerr << "selfTest=FAIL case=transform-eos-one-shot\n";
            return false;
        }
    }
    return true;
}

bool selfTest()
{
    // These cases prove exact optional-field/coupling cursor behavior, native
    // coefficient materialization, and the fail-closed contract at this
    // probe-local diagnostic boundary.  IMDCT/PCM remain intentionally absent.
    std::vector<std::uint8_t> malformed(7U, 0U);
    const Result malformedResult = parse(malformed);
    if (malformedResult.disposition != Disposition::Malformed) {
        std::cerr << "selfTest=FAIL case=truncated-frame\n";
        return false;
    }
    std::vector<std::uint8_t> wrong(128U, 0U);
    wrong[0] = 0x0b; wrong[1] = 0x77;
    const Result wrongResult = parse(wrong);
    if (wrongResult.disposition == Disposition::Accepted) {
        std::cerr << "selfTest=FAIL case=wrong-topology\n";
        return false;
    }
    std::vector<std::uint8_t> bsiBytes(32U, 0U);
    FrameHeader bsiFrame;
    bsiFrame.streamType = StreamType::LegacyAc3;
    bsiFrame.offset = 0U;
    bsiFrame.endBit = bsiBytes.size() * 8U;
    setBits(&bsiBytes, 56U, 5U, 0U); // dialnorm
    setBits(&bsiBytes, 61U, 1U, 0U); // compre
    setBits(&bsiBytes, 62U, 1U, 0U); // langcode
    setBits(&bsiBytes, 63U, 1U, 0U); // audprodie
    setBits(&bsiBytes, 64U, 1U, 0U); // copyrightb
    setBits(&bsiBytes, 65U, 1U, 0U); // origbs
    setBits(&bsiBytes, 66U, 1U, 1U); // timecod1e
    setBits(&bsiBytes, 67U, 14U, 0x1234U);
    setBits(&bsiBytes, 81U, 1U, 1U); // timecod2e
    setBits(&bsiBytes, 82U, 14U, 0x2345U);
    setBits(&bsiBytes, 96U, 1U, 1U); // addbsie
    setBits(&bsiBytes, 97U, 6U, 0U); // addbsil => one byte
    setBits(&bsiBytes, 103U, 8U, 0xa5U);
    std::size_t bsiEnd = 0U;
    std::string bsiReason;
    if (!parseBsi(bsiBytes, bsiFrame, &bsiEnd, &bsiReason)
        || bsiEnd != 111U) {
        std::cerr << "selfTest=FAIL case=optional-bsi-tail end="
                  << bsiEnd << " reason=" << bsiReason << '\n';
        return false;
    }
    std::vector<std::uint8_t> preambleBytes(8U, 0U);
    setBits(&preambleBytes, 10U, 1U, 1U); // dynrnge
    setBits(&preambleBytes, 11U, 8U, 0x5aU); // dynrng
    setBits(&preambleBytes, 19U, 1U, 1U); // cplstre
    setBits(&preambleBytes, 20U, 1U, 0U); // cplinu=0
    BoundedBitReader preambleReader(preambleBytes.data(), preambleBytes.size());
    const PreambleResult preamble = parseBlockPreamble(
        &preambleReader, 7U, 0U, false);
    if (preamble.disposition != PreambleDisposition::Accepted
        || !preamble.dynrngPresent || !preamble.cplstre
        || preamble.coupling || preambleReader.position() != 21U) {
        std::cerr << "selfTest=FAIL case=dynrng-cplstre\n";
        return false;
    }
    std::vector<std::uint8_t> invalidPreamble(4U, 0U);
    setBits(&invalidPreamble, 11U, 1U, 0U); // cplstre reuse on block zero
    BoundedBitReader invalidReader(invalidPreamble.data(), invalidPreamble.size());
    const PreambleResult invalid = parseBlockPreamble(
        &invalidReader, 7U, 0U, false);
    if (invalid.disposition != PreambleDisposition::Malformed
        || invalid.reason != "legacy-cplstre-reuse-on-block-zero") {
        std::cerr << "selfTest=FAIL case=invalid-cplstre-block0\n";
        return false;
    }
    std::vector<std::uint8_t> activePreamble(4U, 0U);
    setBits(&activePreamble, 11U, 1U, 1U); // cplstre
    setBits(&activePreamble, 12U, 1U, 1U); // cplinu=1
    BoundedBitReader activeReader(activePreamble.data(), activePreamble.size());
    const PreambleResult active = parseBlockPreamble(
        &activeReader, 7U, 0U, false);
    if (active.disposition != PreambleDisposition::Unsupported
        || active.reason != "legacy-coupling-active"
        || activeReader.position() != 13U) {
        std::cerr << "selfTest=FAIL case=active-coupling\n";
        return false;
    }
    std::vector<std::uint8_t> staged(8U, 0U);
    const std::array<unsigned, kFullChannels> stagedStrategies =
        {{1U, 2U, 3U, 0U, 1U}};
    std::size_t stagedBit = 0U;
    for (unsigned code : stagedStrategies) {
        setBits(&staged, stagedBit, 2U, code);
        stagedBit += 2U;
    }
    setBits(&staged, stagedBit, 1U, 1U); // lfeexpstr
    ++stagedBit;
    const std::array<unsigned, 4> stagedBandwidths = {{5U, 6U, 7U, 8U}};
    for (unsigned bandwidth : stagedBandwidths) {
        setBits(&staged, stagedBit, 6U, bandwidth);
        stagedBit += 6U;
    }
    BoundedBitReader stagedReader(staged.data(), staged.size());
    ExponentPlan stagedPlan;
    std::string stagedReason;
    if (!readExponentPlan(&stagedReader, 0U, &stagedPlan, &stagedReason)
        || stagedPlan.strategyCodes != stagedStrategies
        || stagedPlan.bandwidthCodes[0] != 5U
        || stagedPlan.bandwidthCodes[1] != 6U
        || stagedPlan.bandwidthCodes[2] != 7U
        || stagedPlan.bandwidthCodes[3] != 0U
        || stagedPlan.bandwidthCodes[4] != 8U
        || !stagedPlan.lfeNew || stagedReader.position() != 35U) {
        std::cerr << "selfTest=FAIL case=staged-exponent-plan\n";
        return false;
    }
    std::vector<std::uint8_t> reusePlanBytes(2U, 0U);
    BoundedBitReader reusePlanReader(reusePlanBytes.data(), reusePlanBytes.size());
    ExponentPlan reusePlan;
    if (!readExponentPlan(&reusePlanReader, 1U, &reusePlan, &stagedReason)
        || reusePlan.lfeNew || reusePlanReader.position() != 11U) {
        std::cerr << "selfTest=FAIL case=staged-lfe-reuse\n";
        return false;
    }
    std::vector<std::uint8_t> snrBytes(8U, 0U);
    setBits(&snrBytes, 0U, 1U, 1U); // snroffste
    setBits(&snrBytes, 1U, 6U, 17U); // csnroffst
    std::size_t snrBit = 7U;
    for (unsigned ch = 0U; ch < kFullChannels; ++ch) {
        setBits(&snrBytes, snrBit, 4U, ch + 1U);
        snrBit += 4U;
        setBits(&snrBytes, snrBit, 3U, ch + 2U);
        snrBit += 3U;
    }
    setBits(&snrBytes, snrBit, 4U, 9U);
    snrBit += 4U;
    setBits(&snrBytes, snrBit, 3U, 3U);
    snrBit += 3U;
    BoundedBitReader snrReader(snrBytes.data(), snrBytes.size());
    std::array<unsigned, kTotalChannels> snrCsnr{};
    std::array<unsigned, kTotalChannels> snrFsnr{};
    std::array<unsigned, kTotalChannels> snrFgain{};
    bool snrPresent = false;
    if (!readSnrAndGain(&snrReader, &snrCsnr, &snrFsnr, &snrFgain,
                        &snrPresent, &stagedReason)
        || !snrPresent || snrReader.position() != snrBit
        || snrCsnr[0] != 17U || snrCsnr[5] != 17U
        || snrFsnr[0] != 1U || snrFgain[0] != 2U
        || snrFsnr[1] != 2U || snrFgain[1] != 3U
        || snrFsnr[4] != 5U || snrFgain[4] != 6U
        || snrFsnr[5] != 9U || snrFgain[5] != 3U) {
        std::cerr << "selfTest=FAIL case=interleaved-snr-gain\n";
        return false;
    }
    std::vector<std::uint8_t> snrReuseBytes(1U, 0U);
    BoundedBitReader snrReuseReader(snrReuseBytes.data(), snrReuseBytes.size());
    snrCsnr.fill(11U);
    snrFsnr.fill(12U);
    snrFgain.fill(13U);
    snrPresent = true;
    if (!readSnrAndGain(&snrReuseReader, &snrCsnr, &snrFsnr, &snrFgain,
                        &snrPresent, &stagedReason)
        || snrPresent || snrCsnr[0] != 11U || snrFsnr[5] != 12U
        || snrFgain[5] != 13U) {
        std::cerr << "selfTest=FAIL case=snr-reuse\n";
        return false;
    }
    const std::vector<std::uint8_t> groupedBytes(1U, 0x68U);
    MantissaBitReader groupedReader(groupedBytes, 8U);
    MantissaDecodeState groupedCursor;
    MantissaDecodeRequest groupedFirst;
    groupedFirst.bap = {1U};
    groupedFirst.exponents = {0U};
    groupedFirst.channelId = 0U;
    groupedFirst.audioBlockId = 0U;
    groupedFirst.sharedBlockCursor = true;
    groupedFirst.decodeValues = true;
    groupedFirst.rejectReservedComposite = true;
    const MantissaDecodeResult groupedStart = decodeMantissas(
        groupedReader, groupedFirst, groupedCursor);
    const bool pendingAfterFirst = groupedCursor.groups[1U].pending;
    MantissaDecodeRequest groupedSecond = groupedFirst;
    groupedSecond.bap = {1U, 1U, 3U};
    groupedSecond.exponents = {0U, 0U, 0U};
    groupedSecond.channelId = 1U;
    groupedSecond.endOfBlock = true;
    const MantissaDecodeResult groupedFinish = decodeMantissas(
        groupedReader, groupedSecond, groupedCursor);
    if (groupedStart.disposition != MantissaDisposition::Accepted
        || !pendingAfterFirst
        || groupedFinish.disposition != MantissaDisposition::Accepted
        || groupedStart.transformCoefficients.size() != 1U
        || groupedFinish.transformCoefficients.size() != 3U
        || groupedCursor.groups[1U].pending
        || groupedReader.bitOffset() != 8U) {
        std::cerr << "selfTest=FAIL case=shared-grouped-cursor\n";
        return false;
    }
    const std::vector<std::uint8_t> materializeBytes(2U, 0U);
    MantissaDecodeRequest materializeRequest;
    materializeRequest.bap = {0U, 0U, 0U, 3U};
    materializeRequest.exponents = {0U, 1U, 2U, 3U};
    materializeRequest.channelId = 4U;
    materializeRequest.audioBlockId = 9U;
    materializeRequest.decodeValues = true;
    materializeRequest.dithflag = true;
    ReferenceDitherSource materializeDither;
    materializeRequest.ditherSource = materializeDither.callback();
    MantissaBitReader materializeReader(materializeBytes);
    MantissaDecodeState materializeState;
    const MantissaDecodeResult ditherValues = decodeMantissas(
        materializeReader, materializeRequest, materializeState);
    MantissaDecodeRequest noDitherRequest = materializeRequest;
    noDitherRequest.dithflag = false;
    noDitherRequest.ditherSource = {};
    MantissaBitReader noDitherReader(materializeBytes);
    MantissaDecodeState noDitherState;
    const MantissaDecodeResult noDitherValues = decodeMantissas(
        noDitherReader, noDitherRequest, noDitherState);
    const std::uint64_t ditherDigest = digestCoefficientVector(
        ditherValues.transformCoefficients);
    const bool finiteAndBounded = std::all_of(
        ditherValues.transformCoefficients.begin(),
        ditherValues.transformCoefficients.end(),
        [](double value) { return std::isfinite(value) && std::abs(value) <= 1.0; });
    materializeDither.reset();
    materializeState.reset();
    MantissaBitReader repeatReader(materializeBytes);
    const MantissaDecodeResult repeatValues = decodeMantissas(
        repeatReader, materializeRequest, materializeState);
    if (ditherValues.disposition != MantissaDisposition::Accepted
        || !ditherValues.ditherUsed
        || ditherValues.transformCoefficients.size() != 4U
        || !finiteAndBounded
        || noDitherValues.disposition != MantissaDisposition::Accepted
        || noDitherValues.ditherUsed
        || noDitherValues.transformCoefficients.size() != 4U
        || noDitherValues.transformCoefficients[0] != 0.0
        || repeatValues.disposition != MantissaDisposition::Accepted
        || repeatValues.transformCoefficients != ditherValues.transformCoefficients
        || digestCoefficientVector(repeatValues.transformCoefficients)
               != ditherDigest
        || materializeDither.generatedSamples() != 3U) {
        std::cerr << "selfTest=FAIL case=coefficient-materialization-dither\n";
        return false;
    }
    if (!transformSelfTest()) return false;
    std::cout << "selfTest=PASS cases=19\n";
    return true;
}

std::vector<std::uint8_t> readFile(const std::string &path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    if (size <= 0) return {};
    input.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char *>(bytes.data()), size);
    if (!input) return {};
    return bytes;
}

} // namespace

Result parse(const std::vector<std::uint8_t> &bytes)
{
    return parseInternal(bytes, nullptr, true);
}

Result parse(const std::vector<std::uint8_t> &bytes, StatefulDecoder &state)
{
    return parseInternal(bytes, &state, false);
}

Result parseEnd(const std::vector<std::uint8_t> &bytes,
                StatefulDecoder &state)
{
    return parseInternal(bytes, &state, true);
}
} // namespace legacyac3

#ifndef EAC3_CONFIG4_PCM_COMPOSITION_TU
int main(int argc, char **argv)
{
    if (argc == 2 && std::string(argv[1]) == "--self-test")
        return legacyac3::selfTest() ? 0 : 1;
    if (argc != 2) {
        std::cerr << "Usage: Eac3NativeLegacyAc3FrontendProbe <raw-ac3-frame>\n"
                     "       Eac3NativeLegacyAc3FrontendProbe --self-test\n";
        return 2;
    }
    const std::vector<std::uint8_t> bytes = legacyac3::readFile(argv[1]);
    if (bytes.empty()) {
        std::cerr << "probeResult=FAIL reason=input-read\n";
        return 1;
    }
    const eac3native::Eb3FramingResult framing =
        eac3native::normalizeEb3Framing(bytes);
    if (!framing.ok || framing.normalized.empty()) {
        std::cerr << "probeResult=INCONCLUSIVE disposition=MALFORMED reason=eb3-"
                  << framing.reason << '\n';
        return 1;
    }
    const eac3native::ParseResult first =
        eac3native::parseSyncframe(framing.normalized, 0U);
    if (first.disposition != eac3native::Disposition::Accepted || !first.frame
        || first.frame->sizeBytes > framing.normalized.size()) {
        std::cerr << "probeResult=INCONCLUSIVE disposition=MALFORMED"
                     " reason=first-legacy-frame-boundary\n";
        return 1;
    }
    const std::vector<std::uint8_t> firstFrame(
        framing.normalized.begin(),
        framing.normalized.begin()
            + static_cast<std::ptrdiff_t>(first.frame->sizeBytes));
    const legacyac3::Result result = legacyac3::parse(firstFrame);
    std::cout << "probeResult="
              << (result.disposition == legacyac3::Disposition::Accepted
                      ? "PASS" : "INCONCLUSIVE")
              << " disposition="
              << (result.disposition == legacyac3::Disposition::Accepted
                      ? "ACCEPTED" : result.disposition == legacyac3::Disposition::Unsupported
                      ? "UNSUPPORTED" : "MALFORMED")
              << " reason=" << result.reason
              << " bitPosition=" << result.bitPosition
              << " blocks=" << result.blocks.size()
              << " coefficientCount=" << result.coefficientCount
              << " coefficientValues=YES"
              << " stateDigest=0x" << legacyac3::hex(result.stateDigest)
              << " pcmAvailability=PCM_AVAILABLE_DIAGNOSTIC_LEGACY_AC3"
              << " productionAcceptance=INCONCLUSIVE\n";
    for (const auto &block : result.blocks) {
        std::cout << "block=" << block.block
                  << " startBit=" << block.startBit
                  << " endBit=" << block.endBit
                  << " channels=" << block.channels.size() << '\n';
        for (const auto &channel : block.channels)
            std::cout << "  channel=" << channel.channel
                      << " lfe=" << (channel.lfe ? "YES" : "NO")
                      << " coefficientStartBit=" << channel.coefficientStartBit
                      << " coefficientEndBit=" << channel.coefficientEndBit
                      << " coefficientCount=" << channel.coefficients.size()
                      << " finite=YES"
                      << " digest=0x" << legacyac3::hex(channel.coefficientDigest)
                      << '\n';
    }
    std::cout << std::setprecision(12);
    for (const auto &pcm : result.pcm)
        std::cout << "pcm channel=" << pcm.channel
                  << " lfe=" << (pcm.lfe ? "YES" : "NO")
                  << " sampleCount=" << pcm.sampleCount
                  << " digest=0x" << legacyac3::hex(pcm.digest)
                  << " peak=" << pcm.peak
                  << " rms=" << pcm.rms
                  << " eosTailCount=" << pcm.eosTailCount
                  << " eosDigest=0x" << legacyac3::hex(pcm.eosDigest)
                  << " eosPeak=" << pcm.eosPeak
                  << " eosRms=" << pcm.eosRms << '\n';
    return result.disposition == legacyac3::Disposition::Accepted ? 0 : 1;
}
#endif
