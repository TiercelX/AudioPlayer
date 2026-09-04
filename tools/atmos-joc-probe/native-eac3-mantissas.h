#pragma once

// Gate 8N-2b-3: dependency-free AC-3/E-AC-3 mantissa degrouping and
// dequantization primitive.  This slice stops before audblk integration and
// IMDCT.  It uses double ratios: mantissa value, then value * 2^-exponent.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <array>
#include <string>
#include <vector>

namespace eac3native {

enum class MantissaDisposition {
    Accepted,
    Malformed,
    Unsupported,
};

class MantissaBitReader {
public:
    MantissaBitReader() = default;
    // Non-owning view.  The caller must keep bytes alive for the reader's
    // lifetime; decodeMantissas copies only this view and its bit cursor when
    // it performs a transactional decode.
    explicit MantissaBitReader(const std::vector<std::uint8_t> &bytes,
                               std::size_t validBits = 0,
                               std::size_t bitOffset = 0);

    bool read(unsigned bitCount, std::uint32_t *value);
    std::size_t bitOffset() const { return bitOffset_; }
    std::size_t validBits() const { return validBits_; }
    std::size_t remainingBits() const;

private:
    const std::uint8_t *data_ = nullptr;
    std::size_t byteCount_ = 0;
    std::size_t validBits_ = 0;
    std::size_t bitOffset_ = 0;
};

struct MantissaDecodeState {
    struct Group {
        bool pending = false;
        unsigned groupSize = 0;
        unsigned groupIndex = 0;
        std::uint32_t groupCode = 0;
    };

    // Grouped codes may continue across ordered channels within one audio
    // block; a channel/block context still identifies the owning sequence.
    bool hasContext = false;
    std::size_t channelId = 0;
    std::size_t audioBlockId = 0;
    bool hasPendingGroup = false;
    unsigned groupBap = 0;
    unsigned groupSize = 0;
    unsigned groupIndex = 0;
    std::uint32_t groupCode = 0;
    // E-AC-3 may retain independent partial groups for BAP 1, 2, and 4 as
    // ordered channel syntax interleaves.  The legacy fields above mirror
    // the most recently visited group for focused primitive diagnostics.
    std::array<Group, 16> groups{};
    // Absolute coefficient position within this channel/audio block.  It
    // advances for every bap, including bap=0, and is reset at block end.
    std::size_t absoluteCoefficientCursor = 0;

    void reset();
};

// The source returns a normalized dither value in the accepted range
// [-1, +1].  coefficientIndex is absolute within the channel/audio block.
// It is intentionally injected: TS 102 366 specifies the dither
// distribution/scaling tolerance, not an interoperable LFSR sequence.
using DitherSource = std::function<double(std::size_t coefficientIndex,
                                           unsigned exponent)>;

// Reference-quality, dependency-free dither source.  TS 102 366 permits any
// reasonably random sequence and does not prescribe a seed or LFSR.  This
// source therefore uses a documented xorshift64* stream and the standard's
// recommended uniform [-1,+1] * 0.707 scaling.  The object is session state:
// callers explicitly reset it at session/EOS boundaries and may retain it
// across ordered channels, blocks, and frames.
class ReferenceDitherSource {
public:
    static constexpr std::uint64_t defaultSeed = 0x6a09e667f3bcc909ULL;

    explicit ReferenceDitherSource(std::uint64_t seed = defaultSeed);

    void reset(std::uint64_t seed = defaultSeed);
    double next(std::size_t coefficientIndex, unsigned exponent);
    DitherSource callback();

    std::uint64_t state() const { return state_; }
    std::size_t generatedSamples() const { return generatedSamples_; }

private:
    std::uint64_t state_ = defaultSeed;
    std::size_t generatedSamples_ = 0U;
};

struct MantissaDecodeRequest {
    std::vector<unsigned> bap;
    std::vector<unsigned> exponents;
    std::size_t channelId = 0;
    std::size_t audioBlockId = 0;
    bool endOfBlock = false;
    bool dithflag = false;
    // Permit the shared grouped cursor to continue across ordered channels
    // in one audio block (including the LFE channel).
    bool sharedBlockCursor = false;
    // Inventory mode consumes and validates syntax without materializing
    // coefficient values.  Coefficient mode leaves this true.
    bool decodeValues = true;
    // N0 reports reserved grouped composites; coefficient mode must fail
    // closed before exposing a value for one.
    bool rejectReservedComposite = false;
    DitherSource ditherSource;
};

struct MantissaDecodeResult {
    MantissaDisposition disposition = MantissaDisposition::Malformed;
    std::string reason;
    std::vector<double> mantissas;
    std::vector<double> transformCoefficients;
    std::size_t bitsConsumed = 0;
    std::size_t nextBitOffset = 0;
    bool ditherUsed = false;
    bool reservedComposite = false;
    unsigned reservedCompositeCount = 0U;
    unsigned reservedBap = 0U;
    unsigned reservedCode = 0U;
    unsigned reservedMaxCode = 0U;
    std::size_t reservedBitOffset = 0U;
};

MantissaDecodeResult decodeMantissas(MantissaBitReader &reader,
                                     const MantissaDecodeRequest &request,
                                     MantissaDecodeState &state);

const char *toString(MantissaDisposition value);

} // namespace eac3native
