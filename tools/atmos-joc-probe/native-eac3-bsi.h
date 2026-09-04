#pragma once

// Gate 8N-1b: bounded E-AC-3 bsi() parser.  This consumes the complete
// E.1.2.2 bit-stream-information syntax up to audfrm(), but never enters
// exponent, bit-allocation, mantissa, coupling, SPX, AHT, or IMDCT syntax.

#include "native-eac3-core.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace eac3native {

struct BsiInfo {
    bool bsiParsed = false;
    bool legacyAc3 = false;
    // Bit offset from the start of this syncframe; this is the first bit of
    // audfrm(), not an absolute offset in the containing byte buffer.
    std::size_t bsiEndBit = 0;

    unsigned strmtyp = 0;
    unsigned substreamId = 0;
    unsigned acmod = 0;
    bool lfeon = false;
    unsigned bsid = 0;
    unsigned dialnorm = 0;
    bool compre = false;
    bool comprPresent = false;
    unsigned compr = 0;
    bool dualMono = false;
    bool compr2e = false;

    bool chanmape = false;
    std::uint16_t chanmap = 0;
    unsigned chanmapChannelWeight = 0;

    bool mixmdate = false;
    // TS 102 366 E.1.2.2/E.1.3.1.10-11.  This is the presence flag
    // (lfemixlevcode), not the five-bit level value (lfemixlevcod).
    bool lfemixlevcode = false;
    unsigned lfemixlevcod = 0;
    unsigned mixdef = 0;
    unsigned mixdeflen = 0;
    unsigned mixdataFillBits = 0;
    bool paninfoe = false;
    bool paninfo2e = false;
    bool frmmixcfginfoe = false;
    bool infomdate = false;
    bool convsyncPresent = false;
    bool convsync = false;
    bool blkidPresent = false;
    bool blkid = false;
    bool frmsizecodPresent = false;
    unsigned frmsizecod = 0;
    bool addbsie = false;
    unsigned addbsil = 0;
};

struct BsiParseResult {
    Disposition disposition = Disposition::Malformed;
    FailureStage stage = FailureStage::Validation;
    std::string reason;
    std::optional<BsiInfo> info;
};

BsiParseResult parseEac3Bsi(const std::vector<std::uint8_t> &bytes,
                            const FrameHeader &frame);

} // namespace eac3native
