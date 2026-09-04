#include "native-eac3-exponents.h"

#include <iostream>
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

ExponentDecodeResult decode(ExponentStrategy strategy, unsigned absolute,
                            std::vector<unsigned> groups, std::size_t target,
                            bool block0 = false, unsigned bandwidth = 0,
                            const ExponentReuseState *prior = nullptr)
{
    ExponentDecodeRequest request;
    request.strategy = strategy;
    request.absoluteExponent = absolute;
    request.groupedCodes = std::move(groups);
    request.targetCoefficientCount = target;
    request.block0 = block0;
    request.bandwidthCode = bandwidth;
    request.prior = prior;
    return decodeExponentSet(request);
}

bool runSelfTest()
{
    std::size_t cases = 0;
    const ExponentDecodeResult d15Low =
        decode(ExponentStrategy::D15, 6, {0}, 4, true);
    if (!expect(d15Low.disposition == ExponentDisposition::Accepted
                    && d15Low.exponents == std::vector<unsigned>({6, 4, 2, 0}),
                "d15-group-code-zero")) {
        return false;
    }
    ++cases;

    const ExponentDecodeResult d15High =
        decode(ExponentStrategy::D15, 0, {124}, 4, true);
    if (!expect(d15High.disposition == ExponentDisposition::Accepted
                    && d15High.exponents == std::vector<unsigned>({0, 2, 4, 6}),
                "d15-group-code-124")) {
        return false;
    }
    ++cases;

    const ExponentDecodeResult d25 =
        decode(ExponentStrategy::D25, 4, {62}, 7, true);
    const ExponentDecodeResult d45 =
        decode(ExponentStrategy::D45, 4, {62}, 13, true);
    if (!expect(d25.disposition == ExponentDisposition::Accepted
                    && d25.exponents.size() == 7U
                    && d45.disposition == ExponentDisposition::Accepted
                    && d45.exponents.size() == 13U,
                "d25-d45-expansion")) {
        return false;
    }
    ++cases;

    const ExponentDecodeResult partialD25 =
        decode(ExponentStrategy::D25, 4, {0}, 5, true);
    const ExponentDecodeResult partialD45 =
        decode(ExponentStrategy::D45, 4, {0}, 5, true);
    if (!expect(partialD25.disposition == ExponentDisposition::Accepted
                    && partialD25.exponents
                           == std::vector<unsigned>({4, 2, 2, 0, 0})
                    && partialD45.disposition == ExponentDisposition::Accepted
                    && partialD45.exponents
                           == std::vector<unsigned>({4, 2, 2, 2, 2}),
                "partial-d25-d45-tail")) {
        return false;
    }
    ++cases;

    const ExponentDecodeResult oracleD15 =
        decode(ExponentStrategy::D15, 5, {86}, 4, true);
    const ExponentDecodeResult oracleD25 =
        decode(ExponentStrategy::D25, 5, {86}, 7, true);
    const ExponentDecodeResult oracleD45 =
        decode(ExponentStrategy::D45, 5, {86}, 13, true);
    if (!expect(oracleD15.disposition == ExponentDisposition::Accepted
                    && oracleD15.exponents
                           == std::vector<unsigned>({5, 6, 6, 5})
                    && oracleD25.disposition == ExponentDisposition::Accepted
                    && oracleD25.exponents
                           == std::vector<unsigned>({5, 6, 6, 6, 6, 5, 5})
                    && oracleD45.disposition == ExponentDisposition::Accepted
                    && oracleD45.exponents.size() == 13U
                    && oracleD45.exponents[0] == 5U
                    && oracleD45.exponents[4] == 6U
                    && oracleD45.exponents[12] == 5U,
                "grouped-exponent-oracle-86")) {
        return false;
    }
    ++cases;

    const ExponentDecodeResult invalidCode =
        decode(ExponentStrategy::D15, 4, {125}, 4, true);
    if (!expect(invalidCode.disposition == ExponentDisposition::Malformed
                    && invalidCode.reason == "group-code-out-of-range",
                "group-code-125-reject")) {
        return false;
    }
    ++cases;

    const ExponentDecodeResult underflow =
        decode(ExponentStrategy::D15, 1, {0}, 4, true);
    const ExponentDecodeResult overflow =
        decode(ExponentStrategy::D15, 15, {124, 124}, 7, true);
    const ExponentDecodeResult transmittedOutOfRange =
        decode(ExponentStrategy::D15, 16, {62}, 4, true);
    const ExponentDecodeResult lfeTransmittedOutOfRange =
        decodeLfeExponents(16, {62, 62}, ExponentStrategy::D15, true, 3);
    if (!expect(underflow.disposition == ExponentDisposition::Malformed
                    && underflow.reason == "exponent-underflow"
                    && overflow.disposition == ExponentDisposition::Malformed
                    && overflow.reason == "exponent-overflow",
                "exponent-range-reject")) {
        return false;
    }
    if (!expect(transmittedOutOfRange.disposition
                        == ExponentDisposition::Malformed
                    && transmittedOutOfRange.reason
                           == "absolute-exponent-out-of-range"
                    && lfeTransmittedOutOfRange.disposition
                           == ExponentDisposition::Malformed
                    && lfeTransmittedOutOfRange.reason
                           == "absolute-exponent-out-of-range",
                "transmitted-absolute-range-reject")) {
        return false;
    }
    ++cases;

    CouplingExponentDecodeRequest couplingRequest;
    couplingRequest.encodedAbsoluteExponent = 6;
    couplingRequest.groupedCodes = {62, 87};
    couplingRequest.strategy = ExponentStrategy::D15;
    couplingRequest.couplingStartMant = 37;
    couplingRequest.couplingEndMant = 41;
    couplingRequest.targetCoefficientCount = 4;
    couplingRequest.block0 = true;
    const ExponentDecodeResult coupling =
        decodeCouplingExponentSet(couplingRequest);
    CouplingExponentDecodeRequest couplingOverflowRequest = couplingRequest;
    couplingOverflowRequest.encodedAbsoluteExponent = 13;
    const ExponentDecodeResult couplingOverflow =
        decodeCouplingExponentSet(couplingOverflowRequest);
    if (!expect(coupling.disposition == ExponentDisposition::Accepted
                    && coupling.hasReferenceExponent
                    && coupling.referenceExponent == 12U
                    && coupling.exponents
                           == std::vector<unsigned>({12, 12, 12, 13})
                    && couplingOverflow.disposition
                           == ExponentDisposition::Malformed
                    && couplingOverflow.reason
                           == "coupling-absolute-exponent-out-of-range",
                "coupling-absolute-shift")) {
        return false;
    }

    CouplingReuseState couplingPrior;
    couplingPrior.valid = true;
    couplingPrior.couplingStartMant = couplingRequest.couplingStartMant;
    couplingPrior.couplingEndMant = couplingRequest.couplingEndMant;
    couplingPrior.exponents = coupling.exponents;
    CouplingExponentDecodeRequest couplingReuseRequest = couplingRequest;
    couplingReuseRequest.strategy = ExponentStrategy::Reuse;
    couplingReuseRequest.encodedAbsoluteExponent = 0;
    couplingReuseRequest.groupedCodes.clear();
    couplingReuseRequest.block0 = false;
    couplingReuseRequest.prior = &couplingPrior;
    const ExponentDecodeResult couplingReuse =
        decodeCouplingExponentSet(couplingReuseRequest);
    CouplingExponentDecodeRequest couplingRangeChange = couplingReuseRequest;
    couplingRangeChange.couplingEndMant++;
    couplingRangeChange.targetCoefficientCount++;
    const ExponentDecodeResult couplingRangeChanged =
        decodeCouplingExponentSet(couplingRangeChange);
    CouplingExponentDecodeRequest couplingBlock0 = couplingReuseRequest;
    couplingBlock0.block0 = true;
    const ExponentDecodeResult couplingBlock0Result =
        decodeCouplingExponentSet(couplingBlock0);
    CouplingExponentDecodeRequest couplingNoPrior = couplingReuseRequest;
    couplingNoPrior.prior = nullptr;
    const ExponentDecodeResult couplingNoPriorResult =
        decodeCouplingExponentSet(couplingNoPrior);
    CouplingExponentDecodeRequest couplingCountMismatch = couplingRequest;
    couplingCountMismatch.targetCoefficientCount = 3;
    const ExponentDecodeResult couplingCountMismatchResult =
        decodeCouplingExponentSet(couplingCountMismatch);
    if (!expect(couplingReuse.disposition == ExponentDisposition::Accepted
                    && couplingReuse.exponents == coupling.exponents
                    && !couplingReuse.hasReferenceExponent
                    && couplingRangeChanged.disposition
                           == ExponentDisposition::Malformed
                    && couplingRangeChanged.reason
                           == "coupling-reuse-range-change",
                "coupling-reuse-state-contract")) {
        return false;
    }
    if (!expect(couplingBlock0Result.disposition
                        == ExponentDisposition::Malformed
                    && couplingBlock0Result.reason
                           == "coupling-reuse-on-block-zero"
                    && couplingNoPriorResult.disposition
                           == ExponentDisposition::Malformed
                    && couplingNoPriorResult.reason
                           == "coupling-reuse-without-prior-block"
                    && couplingCountMismatchResult.disposition
                           == ExponentDisposition::Malformed
                    && couplingCountMismatchResult.reason
                           == "coupling-bin-count-mismatch",
                "coupling-reuse-state-contract")) {
        return false;
    }
    ++cases;

    const ExponentDecodeResult lfe = decodeLfeExponents(
        2, {62, 62}, ExponentStrategy::D15, true, 3);
    if (!expect(lfe.disposition == ExponentDisposition::Accepted
                    && lfe.exponents.size() == 7U,
                "lfe-seven-exponents")) {
        return false;
    }
    ++cases;

    ExponentReuseState prior;
    prior.valid = true;
    prior.bandwidthCode = 7;
    prior.exponents = d25.exponents;
    const ExponentDecodeResult reused = decode(
        ExponentStrategy::Reuse, 0, {}, 7, false, 7, &prior);
    const ExponentDecodeResult block0Reuse = decode(
        ExponentStrategy::Reuse, 0, {}, 7, true, 7, &prior);
    const ExponentDecodeResult bandwidthChange = decode(
        ExponentStrategy::Reuse, 0, {}, 7, false, 8, &prior);
    const ExponentDecodeResult noPrior = decode(
        ExponentStrategy::Reuse, 0, {}, 7, false, 7, nullptr);
    if (!expect(reused.disposition == ExponentDisposition::Accepted
                    && reused.exponents == prior.exponents
                    && block0Reuse.disposition == ExponentDisposition::Malformed
                    && block0Reuse.reason == "reuse-on-block-zero"
                    && bandwidthChange.disposition == ExponentDisposition::Malformed
                    && bandwidthChange.reason == "reuse-bandwidth-change"
                    && noPrior.disposition == ExponentDisposition::Malformed
                    && noPrior.reason == "reuse-without-prior-block",
                "reuse-state-contract")) {
        return false;
    }
    ++cases;

    std::cout << "selfTest=PASS cases=" << cases
              << " implementation=native-eac3-exponents-gate8n-2a\n";
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc == 2 && std::string(argv[1]) == "--self-test") {
        return runSelfTest() ? 0 : 1;
    }
    std::cerr << "Usage: Eac3NativeExponentsProbe --self-test\n";
    return 2;
}
