#pragma once

// Gate 8N-2b-1: native parametric bit allocation for one uncoupled FBW
// channel.  This is a diagnostic primitive only; it does not parse audblk,
// mantissas, coupling, SPX, AHT, or enhanced/GAQ syntax.

#include <cstddef>
#include <string>
#include <vector>

namespace eac3native {

enum class BitAllocationDisposition {
    Accepted,
    Malformed,
    Unsupported,
};

// TS 102 366 deltbae coding: 00=reuse previous delta allocation, 01=new
// delta allocation information, 10=none, 11=reserved.  The parser owns the
// raw bit syntax; this primitive receives the resolved mode only.
enum class DbaMode {
    ReusePrevious = 0,
    NewInfo = 1,
    None = 2,
    Reserved = 3,
};

struct BitAllocationRequest {
    // fscod: 0 = 48 kHz, 1 = 44.1 kHz, 2 = 32 kHz.  Reduced fscod 3 is
    // intentionally unsupported by this gate.
    unsigned fscod = 0;
    unsigned startMant = 0;
    unsigned endMant = 0;
    std::vector<unsigned> exponents;

    unsigned sdcycod = 0;
    unsigned fdcycod = 0;
    unsigned sgaincod = 0;
    unsigned dbpbcod = 0;
    unsigned floorcod = 0;
    unsigned csnroffst = 0;
    unsigned fsnroffst = 0;
    unsigned fgaincod = 0;

    // This is a frame-level audblk property. A single-channel caller must
    // not infer it merely because this channel's two offsets are zero.
    bool allActiveSnrOffsetsZero = false;

    // The primitive currently supports only resolved DBA=none. Reuse/new/
    // reserved modes remain structured Unsupported until parser state owns
    // their syntax and stop/mute policy.
    DbaMode dbaMode = DbaMode::None;

    bool uncoupledFbw = true;
    bool ahte = false;
    bool spx = false;
    bool enhancedCoupling = false;
    bool gaq = false;
};

struct BitAllocationResult {
    BitAllocationDisposition disposition = BitAllocationDisposition::Malformed;
    std::string reason;
    std::vector<int> psd;
    std::vector<int> bndpsd;
    std::vector<int> excite;
    std::vector<int> mask;
    // Raw Table 6.15 values used before rounded mask derivation.  This is a
    // diagnostic read-only intermediate, indexed by band.  The zero-SNR
    // shortcut leaves the preallocated entries at zero.
    std::vector<int> hearingThreshold;
    std::vector<unsigned> bap;
    int snroffset = 0;
    int lowcomp = 0;
    std::size_t bandCount = 0;
    bool zeroSnr = false;
};

BitAllocationResult computeUncoupledFbwBitAllocation(
    const BitAllocationRequest &request);

const char *toString(BitAllocationDisposition value);

} // namespace eac3native
