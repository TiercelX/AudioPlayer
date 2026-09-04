#pragma once

// Gate 8N-1c/N0: bounded E-AC-3 audfrm()/audblk() feature inventory.
// This parser consumes all six ordinary audblk syntax blocks, including
// exponent, allocation, and mantissa cursor widths, but never reconstructs
// coefficient values.  It never performs IMDCT, PCM, DRC, or renderer work.
// Advanced coding tools are reported as structured Unsupported at their first
// active branch.

#include "native-eac3-bsi.h"
#include "native-eac3-mantissas.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace eac3native {

enum class AudblkDisposition {
    Accepted,
    Unsupported,
    Malformed,
};

// A reserved grouped mantissa composite is still consumed at its normative
// fixed width by the N0 cursor inventory.  This record is deliberately a
// conformance warning only: no coefficient value is assigned or decoded.
struct ReservedGroupWarning {
    unsigned count = 0;
    bool firstValid = false;
    std::size_t firstFrameOffset = 0;
    unsigned firstBlock = 0;
    unsigned firstChannel = 0;
    bool firstLfe = false;
    unsigned firstCoefficient = 0;
    std::size_t firstBit = 0;
    unsigned firstCode = 0;
    unsigned maxCode = 0;
};

// N1A/N2A snapshot.  These records expose syntax/state and cursor boundaries;
// N2A may additionally materialize ordinary uncoupled spectral coefficients.
// Bit positions are absolute in the supplied byte buffer and every vector is
// deterministic in coded order.
struct AudblkAllocationState {
    bool reused = false;
    unsigned sdcycod = 2U;
    unsigned fdcycod = 1U;
    unsigned sgaincod = 1U;
    unsigned dbpbcod = 2U;
    unsigned floorcod = 7U;
    std::vector<unsigned> bap;
    std::uint64_t bapDigest = 0U;
};

struct AudblkChannelState {
    unsigned channel = 0U;
    bool lfe = false;
    // Enclosing syntax-to-mantissa span for this coded channel.  E-AC-3
    // channel fields are interleaved, so this is not a contiguous ownership
    // slice for every field in the span.
    std::size_t startBit = 0U;
    std::size_t endBit = 0U;
    bool bandwidthReused = false;
    unsigned chbwcod = 0U;
    unsigned endMant = 0U;
    unsigned exponentStrategy = 0U;
    bool exponentReused = false;
    std::vector<unsigned> exponents;
    bool snrReused = false;
    unsigned csnroffst = 0U;
    unsigned fsnroffst = 0U;
    bool fastGainReused = false;
    unsigned fgaincod = 4U;
    // Enclosing syntax-to-mantissa span, not a claim that interleaved channel
    // fields form one contiguous per-channel ownership slice.
    std::size_t coefficientStartBit = 0U;
    std::size_t coefficientEndBit = 0U;
    bool coefficientsDecoded = false;
    std::vector<double> coefficients;
    std::uint64_t coefficientDigest = 0U;
    AudblkAllocationState allocation;
};

struct AudblkBlockState {
    unsigned block = 0U;
    std::size_t startBit = 0U;
    std::size_t endBit = 0U;
    bool blockSwitch = false;
    bool dither = true;
    std::vector<bool> blockSwitchFlags;
    std::vector<bool> ditherFlags;
    bool ditherSyntax = false;
    bool dynrngPresent = false;
    bool dynrng2Present = false;
    bool rematrixPresent = false;
    bool rematrixUpdated = false;
    bool rematrixReused = false;
    std::array<bool, 4> rematrixFlags{};
    bool groupedCursorShared = false;
    bool groupedCursorResetAtEnd = false;
    std::size_t groupedCursorStartBit = 0U;
    std::size_t groupedCursorEndBit = 0U;
    std::vector<unsigned> groupedChannelOrder;
    std::vector<AudblkChannelState> channels;
};

struct AudblkFrameState {
    std::size_t frameOffset = 0U;
    std::size_t frameStartBit = 0U;
    std::size_t frameEndBit = 0U;
    std::size_t bsiEndBit = 0U;
    std::size_t audfrmStartBit = 0U;
    std::size_t audfrmEndBit = 0U;
    unsigned blocks = 0U;
    unsigned channels = 0U;
    bool lfe = false;
    bool expstre = false;
    bool ahte = false;
    unsigned snrStrategy = 0U;
    bool blockSwitchSyntax = false;
    bool ditherSyntax = false;
    bool bamodeSyntax = false;
    bool fastGainSyntax = false;
    bool deltaBitAllocationSyntax = false;
    // Dynamic-range words are inventory-only in N1A; this remains false.
    bool drcApplied = false;
    // Reference dither bookkeeping is populated when coefficient mode is
    // driven by ReferenceDitherSource; injection-only tests leave these zero.
    std::uint64_t ditherStateStart = 0U;
    std::uint64_t ditherStateEnd = 0U;
    std::size_t ditherSamples = 0U;
    std::vector<AudblkBlockState> blockStates;
};

struct AudblkFeatures {
    bool audfrmParsed = false;
    bool audblkParsed = false;
    bool drcMetadataPresent = false;
    bool blockSwitchSyntax = false;
    bool ditherSyntax = false;
    bool bamodeSyntax = false;
    bool fastGainSyntax = false;
    bool deltaBitAllocationSyntax = false;
    bool transientProcessing = false;
    bool spectralExtensionAttenuation = false;
    bool couplingStrategy = false;
    bool couplingInUse = false;
    bool enhancedCoupling = false;
    bool couplingCoordinates = false;
    bool rematrixing = false;
    bool spectralExtension = false;
    bool adaptiveHybridTransform = false;
    bool gainAdaptiveQuantization = false;
    bool leakTerms = false;
    bool snrOffsets = false;
    bool exponentReuse = false;
    bool exponentStrategy = false;
    bool blockStartInfo = false;
    unsigned blockSwitchActive = 0;
    unsigned ditherOn = 0;
    unsigned ditherOff = 0;
    unsigned dynamicRangeWords = 0;
    unsigned dynrngWords = 0;
    unsigned dynrng2Words = 0;
    unsigned blocks = 0;
    unsigned channels = 0;
    unsigned unsupportedBranches = 0;
    unsigned reservedGroupWarningTotal = 0;
    std::array<ReservedGroupWarning, 16> reservedGroupWarnings{};
    std::optional<AudblkFrameState> state;
    unsigned currentBlock = static_cast<unsigned>(-1);
    unsigned currentChannel = static_cast<unsigned>(-1);
    bool currentLfe = false;
};

struct AudblkParseResult {
    AudblkDisposition disposition = AudblkDisposition::Malformed;
    FailureStage stage = FailureStage::Validation;
    std::string reason;
    std::size_t frameOffset = 0U;
    std::size_t bitPosition = 0; // absolute bit position in the input
    unsigned blockIndex = static_cast<unsigned>(-1);
    unsigned channelIndex = static_cast<unsigned>(-1);
    bool channelIsLfe = false;
    std::optional<AudblkFeatures> features;
};

// Session integration hook.  The parser still owns the single audfrm/audblk
// cursor; this callback only selects the stateful dither stream for the
// channel currently being consumed.  It is intentionally separate from the
// legacy single-source callback so callers cannot accidentally imply one
// shared channel state.
using ChannelDitherSource = std::function<double(unsigned channelId,
                                                  std::size_t coefficientIndex,
                                                  unsigned exponent)>;

AudblkParseResult parseEac3Audblk(
    const std::vector<std::uint8_t> &bytes,
    const FrameHeader &frame,
    const BsiInfo &bsi);

// N2A coefficient mode follows the same audfrm/audblk cursor and grouped
// ownership path as inventory mode.  Callers must provide either a
// session-owned ReferenceDitherSource (with explicit reset/EOS policy) or an
// explicit source callback.  No per-frame implicit reset or normative LFSR is
// assumed.
AudblkParseResult parseEac3AudblkCoefficients(
    const std::vector<std::uint8_t> &bytes,
    const FrameHeader &frame,
    const BsiInfo &bsi,
    const DitherSource &ditherSource);

// Session-aware per-channel dither injection.  The parser does not reset any
// source; the owning session defines AU/EOS reset policy explicitly.
AudblkParseResult parseEac3AudblkCoefficients(
    const std::vector<std::uint8_t> &bytes,
    const FrameHeader &frame,
    const BsiInfo &bsi,
    const ChannelDitherSource &ditherSource);

// Session-aware reference dither overload.  The source is not reset by the
// parser; callers own reset/EOS policy and may retain state across frames.
AudblkParseResult parseEac3AudblkCoefficients(
    const std::vector<std::uint8_t> &bytes,
    const FrameHeader &frame,
    const BsiInfo &bsi,
    ReferenceDitherSource &ditherSource);

const char *toString(AudblkDisposition value);

// Diagnostic-only cursor policy self-test used by the bounded probe.  It
// exercises the same parser path without exposing mantissa values.
bool runAudblkReservedGroupSelfTest();

} // namespace eac3native
