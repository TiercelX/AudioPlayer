#include "native-eac3-bit-allocation.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>

namespace eac3native {
namespace {

BitAllocationResult failure(BitAllocationDisposition disposition,
                            const char *reason)
{
    BitAllocationResult result;
    result.disposition = disposition;
    result.reason = reason;
    return result;
}

constexpr std::array<int, 50> kBandStart = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9,
    10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
    20, 21, 22, 23, 24, 25, 26, 27, 28, 31,
    34, 37, 40, 43, 46, 49, 55, 61, 67, 73,
    79, 85, 97, 109, 121, 133, 157, 181, 205, 229};

constexpr std::array<int, 50> kBandSize = {
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 3, 3,
    3, 3, 3, 3, 3, 6, 6, 6, 6, 6,
    6, 12, 12, 12, 12, 24, 24, 24, 24, 24};

constexpr std::array<int, 64> kBapTable = {
    0, 1, 1, 1, 1, 1, 2, 2, 3, 3, 3, 4, 4, 5, 5, 6,
    6, 6, 6, 7, 7, 7, 7, 8, 8, 8, 8, 9, 9, 9, 9, 10,
    10, 10, 10, 11, 11, 11, 11, 12, 12, 12, 12, 13, 13,
    13, 13, 14, 14, 14, 14, 14, 14, 14, 14, 15, 15, 15,
    15, 15, 15, 15, 15, 15};

// Table 6.15, reordered from the PDF's two-column presentation into the
// normative lookup shape [band][fscod].
constexpr std::array<std::array<int, 3>, 50> kHearingThreshold = {{
    {{0x4d0, 0x4f0, 0x580}}, {{0x4d0, 0x4f0, 0x580}},
    {{0x440, 0x460, 0x4b0}}, {{0x400, 0x410, 0x450}},
    {{0x3e0, 0x3e0, 0x420}}, {{0x3c0, 0x3d0, 0x3f0}},
    {{0x3b0, 0x3c0, 0x3e0}}, {{0x3b0, 0x3b0, 0x3d0}},
    {{0x3a0, 0x3b0, 0x3c0}}, {{0x3a0, 0x3a0, 0x3b0}},
    {{0x3a0, 0x3a0, 0x3b0}}, {{0x3a0, 0x3a0, 0x3b0}},
    {{0x3a0, 0x3a0, 0x3a0}}, {{0x390, 0x3a0, 0x3a0}},
    {{0x390, 0x390, 0x3a0}}, {{0x390, 0x390, 0x3a0}},
    {{0x380, 0x390, 0x3a0}}, {{0x380, 0x380, 0x3a0}},
    {{0x370, 0x380, 0x3a0}}, {{0x370, 0x380, 0x3a0}},
    {{0x360, 0x370, 0x390}}, {{0x360, 0x370, 0x390}},
    {{0x350, 0x360, 0x390}}, {{0x350, 0x360, 0x390}},
    {{0x340, 0x350, 0x380}}, {{0x340, 0x350, 0x380}},
    {{0x330, 0x340, 0x380}}, {{0x320, 0x340, 0x370}},
    {{0x310, 0x320, 0x360}}, {{0x300, 0x310, 0x350}},
    {{0x2f0, 0x300, 0x340}}, {{0x2f0, 0x2f0, 0x330}},
    {{0x2f0, 0x2f0, 0x320}}, {{0x2f0, 0x2f0, 0x310}},
    {{0x300, 0x2f0, 0x300}}, {{0x310, 0x300, 0x2f0}},
    {{0x340, 0x320, 0x2f0}}, {{0x390, 0x350, 0x2f0}},
    {{0x3e0, 0x390, 0x300}}, {{0x420, 0x3e0, 0x310}},
    {{0x460, 0x420, 0x330}}, {{0x490, 0x450, 0x350}},
    {{0x4a0, 0x4a0, 0x3c0}}, {{0x460, 0x490, 0x410}},
    {{0x440, 0x460, 0x470}}, {{0x440, 0x440, 0x4a0}},
    {{0x520, 0x480, 0x460}}, {{0x800, 0x630, 0x440}},
    {{0x840, 0x840, 0x450}}, {{0x840, 0x840, 0x4e0}},
}};

// Table 6.14 is compressed as 66 value/run pairs covering exactly the 256
// address positions used by the log-add lookup (0..255).
constexpr std::array<std::array<int, 2>, 66> kLogAddRuns = {{
    {{64, 1}}, {{63, 1}}, {{62, 1}}, {{61, 1}}, {{60, 1}}, {{59, 1}},
    {{58, 1}}, {{57, 1}}, {{56, 1}}, {{55, 1}}, {{54, 1}}, {{53, 1}},
    {{52, 2}}, {{51, 1}}, {{50, 1}}, {{49, 1}}, {{48, 1}}, {{47, 2}},
    {{46, 1}}, {{45, 1}}, {{44, 2}}, {{43, 1}}, {{42, 1}}, {{41, 2}},
    {{40, 1}}, {{39, 1}}, {{38, 2}}, {{37, 1}}, {{36, 2}}, {{35, 2}},
    {{34, 1}}, {{33, 2}}, {{32, 2}}, {{31, 1}}, {{30, 2}}, {{29, 2}},
    {{28, 2}}, {{27, 2}}, {{26, 2}}, {{25, 2}}, {{24, 2}}, {{23, 2}},
    {{22, 2}}, {{21, 3}}, {{20, 2}}, {{19, 3}}, {{18, 3}}, {{17, 3}},
    {{16, 3}}, {{15, 3}}, {{14, 3}}, {{13, 4}}, {{12, 4}}, {{11, 4}},
    {{10, 5}}, {{9, 5}}, {{8, 6}}, {{7, 6}}, {{6, 8}}, {{5, 8}},
    {{4, 11}}, {{3, 14}}, {{2, 19}}, {{1, 32}}, {{0, 46}},
}};

constexpr int kSlowDecay[4] = {0x0f, 0x11, 0x13, 0x15};
constexpr int kFastDecay[4] = {0x3f, 0x53, 0x67, 0x7b};
constexpr int kSlowGain[4] = {0x540, 0x4d8, 0x478, 0x410};
constexpr int kDbPerBit[4] = {0x000, 0x700, 0x900, 0xb00};
constexpr int kFloor[8] = {
    0x2f0, 0x2b0, 0x270, 0x230, 0x1f0, 0x170, 0x0f0, -2048};
constexpr int kFastGain[8] = {
    0x080, 0x100, 0x180, 0x200, 0x280, 0x300, 0x380, 0x400};

int bandForBin(unsigned bin)
{
    for (std::size_t band = 0; band < kBandStart.size(); ++band) {
        const int start = kBandStart[band];
        const int end = start + kBandSize[band];
        if (static_cast<int>(bin) >= start && static_cast<int>(bin) < end) {
            return static_cast<int>(band);
        }
    }
    return -1;
}

int logAddTable(unsigned address)
{
    unsigned cursor = 0;
    for (const auto &run : kLogAddRuns) {
        if (address < cursor + static_cast<unsigned>(run[1])) {
            return run[0];
        }
        cursor += static_cast<unsigned>(run[1]);
    }
    return 0;
}

int logAdd(int a, int b)
{
    const int difference = a - b;
    const unsigned address = static_cast<unsigned>(
        std::min(255, std::abs(difference) >> 1));
    const int correction = logAddTable(address);
    return difference >= 0 ? a + correction : b + correction;
}

int calcLowcomp(int current, int b0, int b1, unsigned bin)
{
    if (bin < 7U) {
        if (b0 + 256 == b1) {
            return 384;
        }
        if (b0 > b1) {
            return std::max(0, current - 64);
        }
    } else if (bin < 20U) {
        if (b0 + 256 == b1) {
            return 320;
        }
        if (b0 > b1) {
            return std::max(0, current - 64);
        }
    } else {
        return std::max(0, current - 128);
    }
    return current;
}

} // namespace

BitAllocationResult computeUncoupledFbwBitAllocation(
    const BitAllocationRequest &request)
{
    if (request.fscod > 2U) {
        return failure(BitAllocationDisposition::Unsupported,
                       "reduced-sample-rate-unsupported");
    }
    if (!request.uncoupledFbw) {
        return failure(BitAllocationDisposition::Unsupported,
                       "coupled-fbw-unsupported");
    }
    if (request.ahte) {
        return failure(BitAllocationDisposition::Unsupported,
                       "aht-unsupported");
    }
    if (request.spx) {
        return failure(BitAllocationDisposition::Unsupported,
                       "spx-unsupported");
    }
    if (request.enhancedCoupling) {
        return failure(BitAllocationDisposition::Unsupported,
                       "enhanced-coupling-unsupported");
    }
    if (request.gaq) {
        return failure(BitAllocationDisposition::Unsupported,
                       "gaq-unsupported");
    }
    if (request.dbaMode != DbaMode::None) {
        return failure(BitAllocationDisposition::Unsupported,
                       "delta-bit-allocation-mode-unsupported");
    }
    if (request.startMant != 0U || request.endMant <= request.startMant
        || request.endMant > 253U) {
        return failure(BitAllocationDisposition::Malformed,
                       "fbw-mantissa-range");
    }
    if (request.exponents.size() != request.endMant - request.startMant) {
        return failure(BitAllocationDisposition::Malformed,
                       "exponent-count");
    }
    for (unsigned exponent : request.exponents) {
        if (exponent > 24U) {
            return failure(BitAllocationDisposition::Malformed,
                           "exponent-out-of-range");
        }
    }
    if (request.sdcycod >= 4U || request.fdcycod >= 4U
        || request.sgaincod >= 4U || request.dbpbcod >= 4U
        || request.floorcod >= 8U || request.csnroffst >= 64U
        || request.fsnroffst >= 16U || request.fgaincod >= 8U) {
        return failure(BitAllocationDisposition::Malformed,
                       "bit-allocation-code-out-of-range");
    }

    const int firstBand = bandForBin(request.startMant);
    const int lastBand = bandForBin(request.endMant - 1U);
    if (firstBand < 0 || lastBand < firstBand) {
        return failure(BitAllocationDisposition::Malformed,
                       "fbw-band-range");
    }
    const std::size_t bandCount = static_cast<std::size_t>(lastBand + 1);

    BitAllocationResult result;
    result.disposition = BitAllocationDisposition::Accepted;
    result.bandCount = bandCount;
    result.snroffset = (((static_cast<int>(request.csnroffst) - 15) << 4)
                        + static_cast<int>(request.fsnroffst)) << 2;
    result.psd.resize(request.endMant);
    result.bndpsd.assign(bandCount, 0);
    result.excite.assign(bandCount, 0);
    result.mask.assign(bandCount, 0);
    result.hearingThreshold.assign(bandCount, 0);
    result.bap.assign(request.endMant, 0U);

    const bool localSnrOffsetsZero = request.csnroffst == 0U
        && request.fsnroffst == 0U;
    if (request.allActiveSnrOffsetsZero && !localSnrOffsetsZero) {
        return failure(BitAllocationDisposition::Malformed,
                       "global-zero-snr-contract-mismatch");
    }
    if (request.allActiveSnrOffsetsZero) {
        result.zeroSnr = true;
        return result;
    }

    for (unsigned bin = request.startMant; bin < request.endMant; ++bin) {
        result.psd[bin] = 3072 - static_cast<int>(request.exponents[bin]) * 128;
    }

    for (int band = firstBand; band <= lastBand; ++band) {
        const int start = std::max<int>(request.startMant, kBandStart[band]);
        const int end = std::min<int>(request.endMant,
                                      kBandStart[band] + kBandSize[band]);
        if (start >= end) {
            return failure(BitAllocationDisposition::Malformed,
                           "empty-band-range");
        }
        int value = result.psd[start];
        for (int bin = start + 1; bin < end; ++bin) {
            value = logAdd(value, result.psd[bin]);
        }
        result.bndpsd[band] = value;
    }

    const int fgain = kFastGain[request.fgaincod];
    const int sgain = kSlowGain[request.sgaincod];
    const int fdecay = kFastDecay[request.fdcycod];
    const int sdecay = kSlowDecay[request.sdcycod];
    int lowcomp = 0;
    int begin = 0;
    int fastleak = 0;
    int slowleak = 0;
    const int bndend = lastBand + 1;

    if (firstBand == 0) {
        lowcomp = calcLowcomp(lowcomp, result.bndpsd[0],
                              result.bndpsd[std::min(1, bndend - 1)], 0);
        result.excite[0] = result.bndpsd[0] - fgain - lowcomp;
        if (bndend > 1) {
            lowcomp = calcLowcomp(lowcomp, result.bndpsd[1],
                                  result.bndpsd[std::min(2, bndend - 1)], 1);
            result.excite[1] = result.bndpsd[1] - fgain - lowcomp;
        }
        begin = std::min(2, bndend);
        for (int band = 2; band < std::min(bndend, 7); ++band) {
            if (band + 1 < bndend) {
                lowcomp = calcLowcomp(lowcomp, result.bndpsd[band],
                                      result.bndpsd[band + 1], band);
            }
            fastleak = result.bndpsd[band] - fgain;
            slowleak = result.bndpsd[band] - sgain;
            result.excite[band] = fastleak - lowcomp;
            if (band + 1 < bndend
                && result.bndpsd[band] <= result.bndpsd[band + 1]) {
                begin = band + 1;
                break;
            }
            begin = band + 1;
        }
        for (int band = begin; band < std::min(bndend, 22); ++band) {
            if (band + 1 < bndend) {
                lowcomp = calcLowcomp(lowcomp, result.bndpsd[band],
                                      result.bndpsd[band + 1], band);
            }
            fastleak -= fdecay;
            fastleak = std::max(fastleak, result.bndpsd[band] - fgain);
            slowleak -= sdecay;
            slowleak = std::max(slowleak, result.bndpsd[band] - sgain);
            result.excite[band] = std::max(fastleak - lowcomp, slowleak);
        }
        begin = 22;
    } else {
        begin = firstBand;
    }

    // When FBW reaches band 22, continue the leak state established over
    // bands 0..21.  Re-seeding from bndpsd[22] would discard the §6.2.2.4
    // fast/slow decay history.  A nonzero first band has no prior state.
    if (firstBand != 0) {
        fastleak = result.bndpsd[std::min(begin, bndend - 1)] - fgain;
        slowleak = result.bndpsd[std::min(begin, bndend - 1)] - sgain;
    }
    for (int band = begin; band < bndend; ++band) {
        fastleak -= fdecay;
        fastleak = std::max(fastleak, result.bndpsd[band] - fgain);
        slowleak -= sdecay;
        slowleak = std::max(slowleak, result.bndpsd[band] - sgain);
        result.excite[band] = std::max(fastleak, slowleak);
    }
    result.lowcomp = lowcomp;

    const int dbknee = kDbPerBit[request.dbpbcod];
    const int floor = kFloor[request.floorcod];
    for (int band = firstBand; band <= lastBand; ++band) {
        result.hearingThreshold[band] =
            kHearingThreshold[band][request.fscod];
        if (result.bndpsd[band] < dbknee) {
            result.excite[band] += (dbknee - result.bndpsd[band]) >> 2;
        }
        result.mask[band] = std::max(result.excite[band],
                                     result.hearingThreshold[band]);
    }

    int mantissa = request.startMant;
    for (int band = firstBand; band <= lastBand; ++band) {
        int adjusted = result.mask[band] - result.snroffset - floor;
        adjusted = std::max(0, adjusted);
        adjusted &= 0x1fe0;
        adjusted += floor;
        result.mask[band] = adjusted;
        const int lastBin = std::min<int>(request.endMant,
                                          kBandStart[band] + kBandSize[band]);
        for (; mantissa < lastBin; ++mantissa) {
            int address = (result.psd[mantissa] - adjusted) >> 5;
            address = std::clamp(address, 0, 63);
            result.bap[mantissa] = static_cast<unsigned>(kBapTable[address]);
        }
    }
    return result;
}

const char *toString(BitAllocationDisposition value)
{
    switch (value) {
    case BitAllocationDisposition::Accepted: return "accepted";
    case BitAllocationDisposition::Malformed: return "malformed";
    case BitAllocationDisposition::Unsupported: return "unsupported";
    }
    return "unknown";
}

} // namespace eac3native
