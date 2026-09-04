#include "native-eac3-mantissas.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace eac3native {
namespace {

MantissaDecodeResult failure(const char *reason, std::size_t offset)
{
    MantissaDecodeResult result;
    result.disposition = MantissaDisposition::Malformed;
    result.reason = reason;
    result.nextBitOffset = offset;
    return result;
}

bool groupSpec(unsigned bap, unsigned *groupBits, unsigned *groupSize,
               unsigned *maxCode)
{
    switch (bap) {
    case 1:
        *groupBits = 5;
        *groupSize = 3;
        *maxCode = 26;
        return true;
    case 2:
        *groupBits = 7;
        *groupSize = 3;
        *maxCode = 124;
        return true;
    case 4:
        *groupBits = 7;
        *groupSize = 2;
        *maxCode = 120;
        return true;
    default:
        return false;
    }
}

unsigned scalarBits(unsigned bap)
{
    if (bap <= 2U) {
        return 0;
    }
    if (bap == 3U) {
        return 3;
    }
    if (bap == 4U) {
        return 0;
    }
    if (bap == 5U) {
        return 4;
    }
    static constexpr unsigned kBits[16] = {
        0, 0, 0, 3, 0, 4, 5, 6,
        7, 8, 9, 10, 11, 12, 14, 16};
    return kBits[bap];
}

bool quantizedMantissa(unsigned bap, unsigned code, double *value)
{
    switch (bap) {
    case 1:
        if (code > 2U) return false;
        *value = (2.0 * static_cast<double>(code) - 2.0) / 3.0;
        return true;
    case 2:
        if (code > 4U) return false;
        *value = (2.0 * static_cast<double>(code) - 4.0) / 5.0;
        return true;
    case 3:
        if (code > 6U) return false;
        *value = (2.0 * static_cast<double>(code) - 6.0) / 7.0;
        return true;
    case 4:
        if (code > 10U) return false;
        *value = (2.0 * static_cast<double>(code) - 10.0) / 11.0;
        return true;
    case 5:
        if (code > 14U) return false;
        *value = (2.0 * static_cast<double>(code) - 14.0) / 15.0;
        return true;
    default:
        break;
    }

    const unsigned bits = scalarBits(bap);
    if (bits == 0U || bits > 16U) {
        return false;
    }
    const unsigned sign = 1U << (bits - 1U);
    const int signedCode = (code & sign) != 0U
        ? static_cast<int>(code) - static_cast<int>(1U << bits)
        : static_cast<int>(code);
    *value = static_cast<double>(signedCode)
        / static_cast<double>(sign);
    return true;
}

bool groupedMantissa(unsigned bap, std::uint32_t groupCode,
                     unsigned index, double *value)
{
    unsigned code = 0;
    if (bap == 1U) {
        code = index == 0U ? groupCode / 9U
            : index == 1U ? (groupCode % 9U) / 3U
                          : groupCode % 3U;
    } else if (bap == 2U) {
        code = index == 0U ? groupCode / 25U
            : index == 1U ? (groupCode % 25U) / 5U
                          : groupCode % 5U;
    } else if (bap == 4U) {
        code = index == 0U ? groupCode / 11U : groupCode % 11U;
    } else {
        return false;
    }
    return quantizedMantissa(bap, code, value);
}

} // namespace

MantissaBitReader::MantissaBitReader(
    const std::vector<std::uint8_t> &bytes,
                                     std::size_t validBits,
                                     std::size_t bitOffset)
    : data_(bytes.data()),
      byteCount_(bytes.size()),
      validBits_(validBits == 0U ? bytes.size() * 8U
                                 : std::min(validBits, bytes.size() * 8U)),
      bitOffset_(bitOffset)
{
}

bool MantissaBitReader::read(unsigned bitCount, std::uint32_t *value)
{
    if (value == nullptr || bitCount > 32U || bitOffset_ > validBits_
        || bitCount > validBits_ - bitOffset_
        || (bitCount != 0U && (data_ == nullptr || byteCount_ == 0U))) {
        return false;
    }
    std::uint32_t result = 0;
    for (unsigned index = 0; index < bitCount; ++index) {
        const std::size_t absolute = bitOffset_ + index;
        const std::uint8_t byte = data_[absolute / 8U];
        const unsigned bit = (byte >> (7U - (absolute % 8U))) & 1U;
        result = (result << 1U) | bit;
    }
    bitOffset_ += bitCount;
    *value = result;
    return true;
}

std::size_t MantissaBitReader::remainingBits() const
{
    return bitOffset_ >= validBits_ ? 0U : validBits_ - bitOffset_;
}

void MantissaDecodeState::reset()
{
    hasContext = false;
    channelId = 0;
    audioBlockId = 0;
    hasPendingGroup = false;
    groupBap = 0;
    groupSize = 0;
    groupIndex = 0;
    groupCode = 0;
    groups = {};
    absoluteCoefficientCursor = 0;
}

ReferenceDitherSource::ReferenceDitherSource(std::uint64_t seed)
{
    reset(seed);
}

void ReferenceDitherSource::reset(std::uint64_t seed)
{
    state_ = seed == 0U ? defaultSeed : seed;
    generatedSamples_ = 0U;
}

double ReferenceDitherSource::next(std::size_t coefficientIndex,
                                   unsigned exponent)
{
    // The arguments identify the caller's coefficient for diagnostics; the
    // stream itself advances only when a dithered bap=0 value is requested.
    (void)coefficientIndex;
    (void)exponent;
    std::uint64_t value = state_;
    value ^= value >> 12U;
    value ^= value << 25U;
    value ^= value >> 27U;
    state_ = value;
    const std::uint64_t random = value * 0x2545f4914f6cdd1dULL;
    const std::uint64_t mantissa = random >> 11U;
    const double unit = static_cast<double>(mantissa)
        / 9007199254740991.0;
    ++generatedSamples_;
    return (unit * 2.0 - 1.0) * 0.7071067811865475;
}

DitherSource ReferenceDitherSource::callback()
{
    return [this](std::size_t coefficientIndex, unsigned exponent) {
        return next(coefficientIndex, exponent);
    };
}

namespace {

bool hasPendingGroupedState(const MantissaDecodeState &state)
{
    for (const MantissaDecodeState::Group &group : state.groups)
        if (group.pending) return true;
    return false;
}

void mirrorGroupedState(MantissaDecodeState *state, unsigned bap)
{
    if (!state || bap >= state->groups.size()) return;
    const MantissaDecodeState::Group &group = state->groups[bap];
    state->hasPendingGroup = group.pending;
    state->groupBap = group.pending ? bap : 0U;
    state->groupSize = group.groupSize;
    state->groupIndex = group.groupIndex;
    state->groupCode = group.groupCode;
}

} // namespace

MantissaDecodeResult decodeMantissas(MantissaBitReader &reader,
                                     const MantissaDecodeRequest &request,
                                     MantissaDecodeState &state)
{
    const std::size_t startOffset = reader.bitOffset();
    MantissaDecodeResult result;
    result.nextBitOffset = startOffset;
    if (request.bap.size() != request.exponents.size()) {
        return failure("bap-exponent-count", startOffset);
    }

    MantissaBitReader readerCopy = reader;
    MantissaDecodeState stateCopy = state;
    if (stateCopy.hasContext
        && (stateCopy.channelId != request.channelId
            || stateCopy.audioBlockId != request.audioBlockId)) {
        if (stateCopy.audioBlockId != request.audioBlockId) {
            if (hasPendingGroupedState(stateCopy)) {
                return failure("group-context-change-with-pending", startOffset);
            }
            stateCopy.reset();
        } else if (!request.sharedBlockCursor && hasPendingGroupedState(stateCopy)) {
            return failure("group-context-change-with-pending", startOffset);
        } else if (!request.sharedBlockCursor) {
            stateCopy.reset();
        } else {
            // The grouped cursor and absolute coefficient cursor belong to
            // the block; channel identity advances with coded ownership.
            stateCopy.channelId = request.channelId;
        }
    }
    if (!stateCopy.hasContext) {
        stateCopy.hasContext = true;
        stateCopy.channelId = request.channelId;
        stateCopy.audioBlockId = request.audioBlockId;
    }
    if (request.bap.size()
        > std::numeric_limits<std::size_t>::max()
            - stateCopy.absoluteCoefficientCursor) {
        return failure("coefficient-cursor-overflow", startOffset);
    }

    result.mantissas.reserve(request.bap.size());
    result.transformCoefficients.reserve(request.bap.size());
    for (std::size_t index = 0; index < request.bap.size(); ++index) {
        const unsigned bap = request.bap[index];
        const unsigned exponent = request.exponents[index];
        if (bap > 15U) {
            return failure("bap-out-of-range", startOffset);
        }
        if (exponent > 24U) {
            return failure("exponent-out-of-range", startOffset);
        }

        double mantissa = 0.0;
        if (bap == 0U) {
            if (request.dithflag && request.decodeValues) {
                if (!request.ditherSource) {
                    result.disposition = MantissaDisposition::Unsupported;
                    result.reason = "dither-source-required";
                    result.nextBitOffset = startOffset;
                    return result;
                }
                mantissa = request.ditherSource(
                    stateCopy.absoluteCoefficientCursor + index, exponent);
                if (!std::isfinite(mantissa) || mantissa < -1.0
                    || mantissa > 1.0) {
                    return failure("dither-value-out-of-range", startOffset);
                }
                result.ditherUsed = true;
            }
        } else {
            unsigned groupBits = 0;
            unsigned groupSize = 0;
            unsigned maxCode = 0;
            if (groupSpec(bap, &groupBits, &groupSize, &maxCode)) {
                MantissaDecodeState::Group &group = stateCopy.groups[bap];
                if (!group.pending) {
                    std::uint32_t groupCode = 0;
                    if (!readerCopy.read(groupBits, &groupCode)) {
                        return failure("mantissa-truncated", startOffset);
                    }
                    if (groupCode > maxCode) {
                        result.reservedComposite = true;
                        ++result.reservedCompositeCount;
                        result.reservedBap = bap;
                        result.reservedCode = groupCode;
                        result.reservedMaxCode = maxCode;
                        result.reservedBitOffset = readerCopy.bitOffset()
                            - groupBits;
                        if (request.rejectReservedComposite) {
                            result.disposition = MantissaDisposition::Unsupported;
                            result.reason = "reserved-group-code";
                            result.nextBitOffset = startOffset;
                            return result;
                        }
                    }
                    group.pending = true;
                    group.groupSize = groupSize;
                    group.groupIndex = 0;
                    group.groupCode = groupCode;
                }
                if (request.decodeValues
                    && !groupedMantissa(bap, group.groupCode,
                                         group.groupIndex, &mantissa)) {
                    return failure("group-code-invalid", startOffset);
                }
                ++group.groupIndex;
                if (group.groupIndex == group.groupSize) {
                    group = {};
                }
                mirrorGroupedState(&stateCopy, bap);
            } else {
                const unsigned bits = scalarBits(bap);
                std::uint32_t code = 0;
                if (!readerCopy.read(bits, &code)) {
                    return failure("mantissa-truncated", startOffset);
                }
                if (request.decodeValues && !quantizedMantissa(bap, code, &mantissa)) {
                    return failure("scalar-code-invalid", startOffset);
                }
            }
        }
        if (request.decodeValues) {
            result.mantissas.push_back(mantissa);
            result.transformCoefficients.push_back(
                std::ldexp(mantissa, -static_cast<int>(exponent)));
        }
    }

    if (request.endOfBlock) {
        // A pending group at block end represents standard-prescribed dummy
        // mantissas.  Do not consume bits or expose dummy output.
        stateCopy.reset();
    } else {
        stateCopy.absoluteCoefficientCursor += request.bap.size();
    }
    result.disposition = MantissaDisposition::Accepted;
    result.bitsConsumed = readerCopy.bitOffset() - startOffset;
    result.nextBitOffset = readerCopy.bitOffset();
    reader = readerCopy;
    state = stateCopy;
    return result;
}

const char *toString(MantissaDisposition value)
{
    switch (value) {
    case MantissaDisposition::Accepted: return "accepted";
    case MantissaDisposition::Malformed: return "malformed";
    case MantissaDisposition::Unsupported: return "unsupported";
    }
    return "unknown";
}

} // namespace eac3native
