#include "native-eac3-bit-allocation.h"

#include <algorithm>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

namespace {
using namespace eac3native;

bool expect(bool value, const char *name)
{
    if (!value) {
        std::cerr << "selfTest=FAIL case=" << name << '\n';
        return false;
    }
    return true;
}

BitAllocationRequest baseRequest(unsigned endMant = 253U,
                                 unsigned exponent = 12U)
{
    BitAllocationRequest request;
    request.fscod = 0;
    request.startMant = 0;
    request.endMant = endMant;
    request.exponents.assign(endMant, exponent);
    request.sdcycod = 2;
    request.fdcycod = 1;
    request.sgaincod = 1;
    request.dbpbcod = 2;
    request.floorcod = 7;
    request.csnroffst = 1;
    request.fsnroffst = 0;
    request.fgaincod = 0;
    return request;
}

bool runSelfTest()
{
    std::size_t cases = 0;

    BitAllocationRequest zero = baseRequest(253U, 24U);
    zero.csnroffst = 0;
    zero.fsnroffst = 0;
    zero.allActiveSnrOffsetsZero = true;
    const BitAllocationResult zeroResult =
        computeUncoupledFbwBitAllocation(zero);
    if (!expect(zeroResult.disposition == BitAllocationDisposition::Accepted
                    && zeroResult.zeroSnr
                    && zeroResult.bap.size() == 253U
                    && std::all_of(zeroResult.bap.begin(), zeroResult.bap.end(),
                                   [](unsigned value) { return value == 0U; }),
                "zero-snr-fast-path")) {
        return false;
    }
    ++cases;

    // Synthetic regression for the TS 102 366 critical-band boundary at
    // bins 133..156 (band 45): the band spans 24 bins, not 12.
    BitAllocationRequest band45 = baseRequest(217U, 12U);
    band45.csnroffst = 63U;
    band45.fsnroffst = 15U;
    band45.exponents[145U] = 0U;
    const BitAllocationResult band45Result =
        computeUncoupledFbwBitAllocation(band45);
    if (!expect(band45Result.disposition == BitAllocationDisposition::Accepted
                    && band45Result.bap[144U] == 14U
                    && band45Result.bap[145U] == 15U
                    && band45Result.bap[156U] == 14U,
                "critical-band-45-width-24")) {
        return false;
    }
    ++cases;

    BitAllocationRequest localZero = baseRequest(7U, 24U);
    localZero.csnroffst = 0U;
    localZero.fsnroffst = 0U;
    const BitAllocationResult localZeroResult =
        computeUncoupledFbwBitAllocation(localZero);
    if (!expect(localZeroResult.disposition == BitAllocationDisposition::Accepted
                    && !localZeroResult.zeroSnr
                    && localZeroResult.psd.size() == 7U,
                "local-zero-global-nonzero-computes")) {
        return false;
    }
    ++cases;

    bool allSampleRatesAccepted = true;
    for (unsigned fscod = 0; fscod < 3U; ++fscod) {
        BitAllocationRequest request = baseRequest(73U, 12U);
        request.fscod = fscod;
        const BitAllocationResult result =
            computeUncoupledFbwBitAllocation(request);
        allSampleRatesAccepted = allSampleRatesAccepted
            && result.disposition == BitAllocationDisposition::Accepted
            && !result.zeroSnr && result.psd.size() == 73U
            && result.bap.size() == 73U;
    }
    if (!expect(allSampleRatesAccepted, "fscod-0-1-2")) {
        return false;
    }
    ++cases;

    // Table 6.15's PDF two-column rows are checked directly through the raw
    // diagnostic intermediate, while retaining quantized-mask checks.
    BitAllocationRequest hthFs0 = baseRequest(253U, 24U);
    hthFs0.fscod = 0;
    BitAllocationRequest hthFs1 = hthFs0;
    hthFs1.fscod = 1;
    BitAllocationRequest hthFs2 = hthFs0;
    hthFs2.fscod = 2;
    const BitAllocationResult hth0 =
        computeUncoupledFbwBitAllocation(hthFs0);
    const BitAllocationResult hth1 =
        computeUncoupledFbwBitAllocation(hthFs1);
    const BitAllocationResult hth2 =
        computeUncoupledFbwBitAllocation(hthFs2);
    if (!expect(hth0.disposition == BitAllocationDisposition::Accepted
                    && hth1.disposition == BitAllocationDisposition::Accepted
                    && hth2.disposition == BitAllocationDisposition::Accepted
                    && hth0.hearingThreshold[1] == 0x4d0
                    && hth1.hearingThreshold[0] == 0x4f0
                    && hth2.hearingThreshold[49] == 0x4e0
                    && hth0.mask[1] == 2112
                    && hth1.mask[0] == 2144
                    && hth2.mask[49] == 2144,
                "hearing-threshold-band-major-oracles")) {
        return false;
    }
    ++cases;

    const BitAllocationResult constant =
        computeUncoupledFbwBitAllocation(baseRequest(73U, 12U));
    BitAllocationRequest fullRange = baseRequest(253U, 0U);
    for (unsigned i = 0; i < fullRange.exponents.size(); ++i) {
        fullRange.exponents[i] = (i * 7U + 3U) % 25U;
    }
    fullRange.csnroffst = 63U;
    fullRange.fsnroffst = 15U;
    const BitAllocationResult fullRangeResult =
        computeUncoupledFbwBitAllocation(fullRange);
    if (!expect(constant.disposition == BitAllocationDisposition::Accepted
                    && constant.psd.front() == 1536
                    && constant.psd.back() == 1536
                    && constant.bndpsd[0] == 1536
                    && constant.bndpsd[28] == 1637
                    && constant.bndpsd[29] == 1637
                    && constant.bandCount == 39U,
                "constant-exponent-psd-banding")) {
        return false;
    }
    ++cases;

    // Non-constant full-range vector fixes the leak transition at bands
    // 21->22->23 and checks an actual final BAP fragment, not just existence
    // of a nonzero value.
    if (!expect(fullRangeResult.disposition == BitAllocationDisposition::Accepted
                    && fullRangeResult.excite[21] == 2944
                    && fullRangeResult.mask[21] == -192
                    && fullRangeResult.bap[21] == 15U
                    && fullRangeResult.excite[22] == 2893
                    && fullRangeResult.mask[22] == -256
                    && fullRangeResult.bap[22] == 15U
                    && fullRangeResult.excite[23] == 3034
                    && fullRangeResult.mask[23] == -128
                    && fullRangeResult.bap[23] == 13U
                    && fullRangeResult.bap[0] == 15U
                    && fullRangeResult.bap.back() == 10U,
                "full-range-leak-and-bap-oracle")) {
        return false;
    }
    ++cases;

    BitAllocationRequest lowcompBin6 = baseRequest(7U, 8U);
    lowcompBin6.exponents[0] = 10U;
    const BitAllocationResult lowcompBin6Result =
        computeUncoupledFbwBitAllocation(lowcompBin6);
    BitAllocationRequest lowcompBin7 = baseRequest(20U, 8U);
    lowcompBin7.exponents[0] = 10U;
    lowcompBin7.exponents[7] = 10U;
    const BitAllocationResult lowcompBin7Result =
        computeUncoupledFbwBitAllocation(lowcompBin7);
    BitAllocationRequest lowcompBin20 = baseRequest(22U, 8U);
    lowcompBin20.exponents[0] = 10U;
    const BitAllocationResult lowcompBin20Result =
        computeUncoupledFbwBitAllocation(lowcompBin20);
    if (!expect(lowcompBin6Result.disposition
                        == BitAllocationDisposition::Accepted
                    && lowcompBin6Result.lowcomp == 384
                    && lowcompBin7Result.disposition
                           == BitAllocationDisposition::Accepted
                    && lowcompBin7Result.lowcomp == 320
                    && lowcompBin20Result.disposition
                           == BitAllocationDisposition::Accepted
                    && lowcompBin20Result.lowcomp == 256,
                "lowcomp-band-boundaries")) {
        return false;
    }
    ++cases;

    BitAllocationRequest highPsd = baseRequest(73U, 0U);
    highPsd.csnroffst = 63U;
    const BitAllocationResult highPsdResult =
        computeUncoupledFbwBitAllocation(highPsd);
    BitAllocationRequest lowPsd = baseRequest(73U, 24U);
    const BitAllocationResult lowPsdResult =
        computeUncoupledFbwBitAllocation(lowPsd);
    if (!expect(highPsdResult.disposition == BitAllocationDisposition::Accepted
                    && lowPsdResult.disposition == BitAllocationDisposition::Accepted
                    && std::any_of(highPsdResult.bap.begin(),
                                   highPsdResult.bap.end(),
                                   [](unsigned value) { return value == 15U; })
                    && std::all_of(lowPsdResult.bap.begin(),
                                   lowPsdResult.bap.end(),
                                   [](unsigned value) { return value == 0U; }),
                "bap-address-clamp")) {
        return false;
    }
    ++cases;

    BitAllocationRequest malformed = baseRequest(73U, 12U);
    malformed.exponents.pop_back();
    const BitAllocationResult exponentCount =
        computeUncoupledFbwBitAllocation(malformed);
    malformed = baseRequest(73U, 12U);
    malformed.exponents[0] = 25U;
    const BitAllocationResult exponentRange =
        computeUncoupledFbwBitAllocation(malformed);
    malformed = baseRequest(73U, 12U);
    malformed.fscod = 3U;
    const BitAllocationResult reducedRate =
        computeUncoupledFbwBitAllocation(malformed);
    if (!expect(exponentCount.disposition == BitAllocationDisposition::Malformed
                    && exponentCount.reason == "exponent-count"
                    && exponentRange.disposition
                           == BitAllocationDisposition::Malformed
                    && exponentRange.reason == "exponent-out-of-range"
                    && reducedRate.disposition
                           == BitAllocationDisposition::Unsupported,
                "invalid-input-rejection")) {
        return false;
    }
    ++cases;

    bool unsupportedFlags = true;
    for (unsigned flag = 0; flag < 4U; ++flag) {
        BitAllocationRequest request = baseRequest(73U, 12U);
        if (flag == 0U) request.ahte = true;
        if (flag == 1U) request.spx = true;
        if (flag == 2U) request.enhancedCoupling = true;
        if (flag == 3U) request.gaq = true;
        unsupportedFlags = unsupportedFlags
            && computeUncoupledFbwBitAllocation(request).disposition
                   == BitAllocationDisposition::Unsupported;
    }
    if (!expect(unsupportedFlags, "unsupported-aht-spx-coupling-gaq")) {
        return false;
    }
    ++cases;

    bool unsupportedDbaModes = true;
    for (const DbaMode mode : {DbaMode::ReusePrevious, DbaMode::NewInfo,
                               DbaMode::Reserved}) {
        BitAllocationRequest dba = baseRequest(73U, 12U);
        dba.dbaMode = mode;
        const BitAllocationResult dbaResult =
            computeUncoupledFbwBitAllocation(dba);
        unsupportedDbaModes = unsupportedDbaModes
            && dbaResult.disposition == BitAllocationDisposition::Unsupported
            && dbaResult.reason == "delta-bit-allocation-mode-unsupported";
    }
    const BitAllocationResult noDbaResult =
        computeUncoupledFbwBitAllocation(baseRequest(73U, 12U));
    if (!expect(unsupportedDbaModes
                    && noDbaResult.disposition
                           == BitAllocationDisposition::Accepted,
                "delta-bit-allocation-mode-boundary")) {
        return false;
    }
    ++cases;

    std::cout << "selfTest=PASS cases=" << cases
              << " implementation=native-eac3-bit-allocation-gate8n-2b-1\n";
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc == 2 && std::string(argv[1]) == "--self-test") {
        return runSelfTest() ? 0 : 1;
    }
    std::cerr << "Usage: Eac3NativeBitAllocationProbe --self-test\n";
    return 2;
}
