#include "native-eac3-block-state.h"

#include <array>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace eac3native {
namespace {

class Digest {
public:
    void add(bool value) { addUnsigned(value ? 1U : 0U); }
    void addUnsigned(unsigned value)
    {
        for (unsigned shift = 0U; shift < 32U; shift += 8U)
            addByte(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
    void addSize(std::size_t value)
    {
        for (unsigned shift = 0U; shift < sizeof(std::size_t); ++shift)
            addByte(static_cast<std::uint8_t>((value >> (shift * 8U)) & 0xffU));
    }
    void addVector(const std::vector<unsigned> &values)
    {
        addSize(values.size());
        for (unsigned value : values) addUnsigned(value);
    }
    std::uint64_t value() const { return value_; }

private:
    void addByte(std::uint8_t byte)
    {
        value_ ^= byte;
        value_ *= 1099511628211ULL;
    }
    std::uint64_t value_ = 1469598103934665603ULL;
};

AudblkStateValidationResult invalid(const AudblkFrameState &state,
                                    unsigned block,
                                    unsigned channel,
                                    bool lfe,
                                    std::size_t bit,
                                    const char *reason)
{
    AudblkStateValidationResult result;
    result.blockIndex = block;
    result.channelIndex = channel;
    result.channelIsLfe = lfe;
    result.frameOffset = state.frameOffset;
    result.bitPosition = bit;
    result.reason = reason;
    return result;
}

bool validBoundary(std::size_t start, std::size_t end)
{
    return start < end;
}

void addChannel(Digest *digest, const AudblkChannelState &channel)
{
    digest->addUnsigned(channel.channel);
    digest->add(channel.lfe);
    digest->addSize(channel.startBit);
    digest->addSize(channel.endBit);
    digest->add(channel.bandwidthReused);
    digest->addUnsigned(channel.chbwcod);
    digest->addUnsigned(channel.endMant);
    digest->addUnsigned(channel.exponentStrategy);
    digest->add(channel.exponentReused);
    digest->addVector(channel.exponents);
    digest->add(channel.snrReused);
    digest->addUnsigned(channel.csnroffst);
    digest->addUnsigned(channel.fsnroffst);
    digest->add(channel.fastGainReused);
    digest->addUnsigned(channel.fgaincod);
    digest->addSize(channel.coefficientStartBit);
    digest->addSize(channel.coefficientEndBit);
    digest->add(channel.coefficientsDecoded);
    digest->addSize(channel.coefficients.size());
    for (double value : channel.coefficients) {
        std::uint64_t bits = 0U;
        static_assert(sizeof(bits) == sizeof(value), "double-width");
        std::memcpy(&bits, &value, sizeof(bits));
        for (unsigned shift = 0U; shift < 64U; shift += 8U)
            digest->addUnsigned(static_cast<unsigned>((bits >> shift) & 0xffffffffU));
    }
    digest->addSize(channel.coefficientDigest);
    digest->add(channel.allocation.reused);
    digest->addUnsigned(channel.allocation.sdcycod);
    digest->addUnsigned(channel.allocation.fdcycod);
    digest->addUnsigned(channel.allocation.sgaincod);
    digest->addUnsigned(channel.allocation.dbpbcod);
    digest->addUnsigned(channel.allocation.floorcod);
    digest->addVector(channel.allocation.bap);
    digest->addSize(channel.allocation.bapDigest);
}

} // namespace

AudblkStateValidationResult validateAudblkState(const AudblkFrameState &state)
{
    if (state.frameEndBit <= state.frameStartBit)
        return invalid(state, static_cast<unsigned>(-1), static_cast<unsigned>(-1),
                       false, state.frameStartBit, "frame-boundary");
    if (state.bsiEndBit < state.frameStartBit
        || state.audfrmStartBit < state.bsiEndBit
        || state.audfrmEndBit < state.audfrmStartBit
        || state.audfrmEndBit > state.frameEndBit) {
        return invalid(state, static_cast<unsigned>(-1), static_cast<unsigned>(-1),
                       false, state.audfrmStartBit, "audfrm-boundary");
    }
    if (state.blocks == 0U || state.blockStates.size() != state.blocks)
        return invalid(state, static_cast<unsigned>(-1), static_cast<unsigned>(-1),
                       false, state.audfrmStartBit, "block-count");
    if (state.channels == 0U)
        return invalid(state, static_cast<unsigned>(-1), static_cast<unsigned>(-1),
                       false, state.audfrmStartBit, "channel-count");
    const std::size_t nonLfeChannels =
        state.channels - (state.lfe ? 1U : 0U);

    for (unsigned blockIndex = 0U; blockIndex < state.blocks; ++blockIndex) {
        const AudblkBlockState &block = state.blockStates[blockIndex];
        if (block.block != blockIndex || !validBoundary(block.startBit, block.endBit)
            || block.startBit < state.audfrmStartBit
            || block.endBit > state.frameEndBit) {
            return invalid(state, blockIndex, static_cast<unsigned>(-1), false,
                           block.startBit, "block-boundary");
        }
        if (block.channels.size() != state.channels
            || block.groupedChannelOrder.size() != state.channels
            || block.blockSwitchFlags.size() != nonLfeChannels
            || block.ditherFlags.size() != nonLfeChannels) {
            return invalid(state, blockIndex, static_cast<unsigned>(-1), false,
                           block.startBit, "channel-order");
        }
        if (block.groupedCursorStartBit < block.startBit
            || block.groupedCursorEndBit > block.endBit
            || block.groupedCursorStartBit > block.groupedCursorEndBit
            || !block.groupedCursorShared
            || !block.groupedCursorResetAtEnd) {
            return invalid(state, blockIndex, static_cast<unsigned>(-1), false,
                           block.groupedCursorStartBit, "grouped-cursor-ownership");
        }
        for (unsigned channelIndex = 0U; channelIndex < state.channels;
             ++channelIndex) {
            const AudblkChannelState &channel = block.channels[channelIndex];
            if (channel.channel != channelIndex || channel.lfe != (state.lfe
                && channelIndex + 1U == state.channels)) {
                return invalid(state, blockIndex, channelIndex, channel.lfe,
                               channel.startBit, "channel-identity");
            }
            if (!validBoundary(channel.startBit, channel.endBit)
                || channel.startBit < block.startBit
                || channel.endBit > block.endBit
                || channel.endMant == 0U || channel.endMant > 253U
                || channel.exponents.empty()) {
                return invalid(state, blockIndex, channelIndex, channel.lfe,
                               channel.startBit, "channel-state");
            }
            if (channel.exponents.size() != channel.endMant) {
                return invalid(state, blockIndex, channelIndex, channel.lfe,
                               channel.startBit, "exponent-state");
            }
            if (channel.allocation.bap.size() != channel.endMant
                || channel.allocation.bapDigest == 0U) {
                return invalid(state, blockIndex, channelIndex, channel.lfe,
                               channel.startBit, "bit-allocation-state");
            }
            if (channel.coefficientsDecoded) {
                if (channel.coefficients.size() != channel.endMant
                    || channel.coefficientEndBit < channel.coefficientStartBit
                    || channel.coefficientStartBit < block.startBit
                    || channel.coefficientEndBit > block.endBit
                    || channel.coefficientDigest == 0U
                    || !std::all_of(channel.coefficients.begin(),
                                    channel.coefficients.end(),
                                    [](double value) {
                                        return std::isfinite(value);
                                    })) {
                    return invalid(state, blockIndex, channelIndex, channel.lfe,
                                   channel.coefficientStartBit,
                                   "coefficient-state");
                }
            }
            if (channel.lfe && channel.endMant != 7U) {
                return invalid(state, blockIndex, channelIndex, true,
                               channel.startBit, "lfe-bandwidth");
            }
            if (blockIndex == 0U
                && (channel.bandwidthReused || channel.exponentReused
                    || channel.snrReused || channel.fastGainReused
                    || channel.allocation.reused)) {
                return invalid(state, blockIndex, channelIndex, channel.lfe,
                               channel.startBit, "frame-reset-reuse");
            }
        }
    }
    AudblkStateValidationResult result;
    result.valid = true;
    return result;
}

std::uint64_t digestAudblkState(const AudblkFrameState &state)
{
    Digest digest;
    digest.addSize(state.frameOffset);
    digest.addSize(state.frameStartBit);
    digest.addSize(state.frameEndBit);
    digest.addSize(state.bsiEndBit);
    digest.addSize(state.audfrmStartBit);
    digest.addSize(state.audfrmEndBit);
    digest.addUnsigned(state.blocks);
    digest.addUnsigned(state.channels);
    digest.add(state.lfe);
    digest.add(state.expstre);
    digest.add(state.ahte);
    digest.addUnsigned(state.snrStrategy);
    digest.add(state.blockSwitchSyntax);
    digest.add(state.ditherSyntax);
    digest.add(state.bamodeSyntax);
    digest.add(state.fastGainSyntax);
    digest.add(state.deltaBitAllocationSyntax);
    digest.add(state.drcApplied);
    digest.addSize(state.ditherStateStart);
    digest.addSize(state.ditherStateEnd);
    digest.addSize(state.ditherSamples);
    digest.addSize(state.blockStates.size());
    for (const AudblkBlockState &block : state.blockStates) {
        digest.addUnsigned(block.block);
        digest.addSize(block.startBit);
        digest.addSize(block.endBit);
        digest.add(block.blockSwitch);
        digest.add(block.dither);
        for (bool value : block.blockSwitchFlags) digest.add(value);
        for (bool value : block.ditherFlags) digest.add(value);
        digest.add(block.ditherSyntax);
        digest.add(block.dynrngPresent);
        digest.add(block.dynrng2Present);
        digest.add(block.rematrixPresent);
        digest.add(block.rematrixUpdated);
        digest.add(block.rematrixReused);
        for (bool value : block.rematrixFlags) digest.add(value);
        digest.add(block.groupedCursorShared);
        digest.add(block.groupedCursorResetAtEnd);
        digest.addSize(block.groupedCursorStartBit);
        digest.addSize(block.groupedCursorEndBit);
        for (unsigned value : block.groupedChannelOrder) digest.addUnsigned(value);
        for (const AudblkChannelState &channel : block.channels)
            addChannel(&digest, channel);
    }
    return digest.value();
}

std::uint64_t digestBapVector(const std::vector<unsigned> &bap)
{
    Digest digest;
    digest.addVector(bap);
    return digest.value();
}

std::uint64_t digestCoefficientVector(const std::vector<double> &coefficients)
{
    Digest digest;
    digest.addSize(coefficients.size());
    for (double value : coefficients) {
        std::uint64_t bits = 0U;
        std::memcpy(&bits, &value, sizeof(bits));
        for (unsigned shift = 0U; shift < 64U; shift += 8U)
            digest.addUnsigned(static_cast<unsigned>((bits >> shift) & 0xffffffffU));
    }
    return digest.value();
}

std::string formatAudblkStateDigest(std::uint64_t digest)
{
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << digest;
    return output.str();
}

} // namespace eac3native
