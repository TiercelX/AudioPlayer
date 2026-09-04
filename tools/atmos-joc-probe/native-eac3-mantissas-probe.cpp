#include "native-eac3-mantissas.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
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

std::vector<std::uint8_t> pack(
    const std::vector<std::pair<unsigned, unsigned>> &fields,
    std::size_t *validBits)
{
    std::vector<std::uint8_t> bytes;
    std::size_t bitCount = 0;
    for (const auto &[value, width] : fields) {
        for (unsigned index = 0; index < width; ++index) {
            if ((bitCount % 8U) == 0U) {
                bytes.push_back(0U);
            }
            const unsigned shift = width - index - 1U;
            if (((value >> shift) & 1U) != 0U) {
                bytes.back() |= static_cast<std::uint8_t>(
                    1U << (7U - (bitCount % 8U)));
            }
            ++bitCount;
        }
    }
    *validBits = bitCount;
    return bytes;
}

MantissaDecodeRequest request(std::vector<unsigned> bap,
                              std::vector<unsigned> exponents,
                              bool endOfBlock = true)
{
    MantissaDecodeRequest value;
    value.bap = std::move(bap);
    value.exponents = std::move(exponents);
    value.endOfBlock = endOfBlock;
    return value;
}

bool close(double actual, double expected)
{
    return std::abs(actual - expected) < 1.0e-12;
}

bool runSelfTest()
{
    std::size_t cases = 0;

    std::size_t validBits = 0;
    const auto scalarBytes =
        pack({{0U, 3U}, {3U, 3U}, {6U, 3U}}, &validBits);
    MantissaBitReader scalarReader(scalarBytes, validBits);
    MantissaDecodeState scalarState;
    const MantissaDecodeResult scalar = decodeMantissas(
        scalarReader, request({3U, 3U, 3U}, {0U, 0U, 0U}), scalarState);
    if (!expect(scalar.disposition == MantissaDisposition::Accepted
                    && scalar.bitsConsumed == 9U
                    && close(scalar.mantissas[0], -6.0 / 7.0)
                    && close(scalar.mantissas[1], 0.0)
                    && close(scalar.mantissas[2], 6.0 / 7.0)
                    && scalar.transformCoefficients == scalar.mantissas,
                "scalar-bap3-symmetry")) {
        return false;
    }
    ++cases;

    const auto checkGroupedEdge =
        [&](unsigned bap, unsigned width, unsigned groupSize,
            unsigned lowCode, unsigned highCode, double lowValue,
            double highValue) {
            for (const auto &[code, expected] : {
                     std::pair<unsigned, double>{lowCode, lowValue},
                     std::pair<unsigned, double>{highCode, highValue}}) {
                validBits = 0;
                const auto bytes = pack({{code, width}}, &validBits);
                MantissaBitReader edgeReader(bytes, validBits);
                MantissaDecodeState edgeState;
                const MantissaDecodeResult edge = decodeMantissas(
                    edgeReader,
                    request(std::vector<unsigned>(groupSize, bap),
                            std::vector<unsigned>(groupSize, 0U)),
                    edgeState);
                if (edge.disposition != MantissaDisposition::Accepted
                    || edge.bitsConsumed != width
                    || edge.mantissas.size() != groupSize
                    || edgeState.hasPendingGroup) {
                    return false;
                }
                for (double value : edge.mantissas) {
                    if (!close(value, expected)) {
                        return false;
                    }
                }
            }
            return true;
        };
    if (!expect(checkGroupedEdge(1U, 5U, 3U, 0U, 26U,
                                 -2.0 / 3.0, 2.0 / 3.0)
                    && checkGroupedEdge(2U, 7U, 3U, 0U, 124U,
                                        -4.0 / 5.0, 4.0 / 5.0)
                    && checkGroupedEdge(4U, 7U, 2U, 0U, 120U,
                                        -10.0 / 11.0, 10.0 / 11.0),
                "grouped-code-boundary-oracles")) {
        return false;
    }
    ++cases;

    const auto checkInvalidGroup = [&](unsigned bap, unsigned code) {
        validBits = 0;
        const auto bytes = pack({{code, 7U}}, &validBits);
        MantissaBitReader invalidGroupReader(bytes, validBits);
        MantissaDecodeState invalidGroupState;
        const MantissaDecodeResult invalidGroup = decodeMantissas(
            invalidGroupReader,
            request(std::vector<unsigned>(bap == 2U ? 3U : 2U, bap),
                    std::vector<unsigned>(bap == 2U ? 3U : 2U, 0U)),
            invalidGroupState);
        return invalidGroup.disposition == MantissaDisposition::Malformed
            && invalidGroup.reason == "group-code-invalid"
            && invalidGroupReader.bitOffset() == 0U
            && !invalidGroupState.hasPendingGroup;
    };
    if (!expect(checkInvalidGroup(2U, 125U)
                    && checkInvalidGroup(4U, 121U),
                "grouped-invalid-code-rollback")) {
        return false;
    }
    ++cases;

    std::vector<std::pair<unsigned, unsigned>> bap3Fields;
    std::vector<unsigned> bap3Baps;
    std::vector<unsigned> bap3Exponents;
    for (unsigned code = 0U; code <= 6U; ++code) {
        bap3Fields.emplace_back(code, 3U);
        bap3Baps.push_back(3U);
        bap3Exponents.push_back(0U);
    }
    validBits = 0;
    const auto bap3GoldenBytes = pack(bap3Fields, &validBits);
    MantissaBitReader bap3GoldenReader(bap3GoldenBytes, validBits);
    MantissaDecodeState bap3GoldenState;
    const MantissaDecodeResult bap3Golden = decodeMantissas(
        bap3GoldenReader, request(bap3Baps, bap3Exponents), bap3GoldenState);

    std::vector<std::pair<unsigned, unsigned>> bap5Fields;
    std::vector<unsigned> bap5Baps;
    std::vector<unsigned> bap5Exponents;
    for (unsigned code = 0U; code <= 14U; ++code) {
        bap5Fields.emplace_back(code, 4U);
        bap5Baps.push_back(5U);
        bap5Exponents.push_back(0U);
    }
    validBits = 0;
    const auto bap5GoldenBytes = pack(bap5Fields, &validBits);
    MantissaBitReader bap5GoldenReader(bap5GoldenBytes, validBits);
    MantissaDecodeState bap5GoldenState;
    const MantissaDecodeResult bap5Golden = decodeMantissas(
        bap5GoldenReader, request(bap5Baps, bap5Exponents), bap5GoldenState);
    bool bap3GoldenValues = bap3Golden.disposition
        == MantissaDisposition::Accepted;
    for (unsigned code = 0U; code <= 6U && bap3GoldenValues; ++code) {
        bap3GoldenValues = close(
            bap3Golden.mantissas[code],
            (2.0 * static_cast<double>(code) - 6.0) / 7.0);
    }
    bool bap5GoldenValues = bap5Golden.disposition
        == MantissaDisposition::Accepted;
    for (unsigned code = 0U; code <= 14U && bap5GoldenValues; ++code) {
        bap5GoldenValues = close(
            bap5Golden.mantissas[code],
            (2.0 * static_cast<double>(code) - 14.0) / 15.0);
    }
    if (!expect(bap3GoldenValues && bap5GoldenValues
                    && bap3Golden.bitsConsumed == 21U
                    && bap5Golden.bitsConsumed == 60U
                    && !bap3GoldenState.hasPendingGroup
                    && !bap5GoldenState.hasPendingGroup,
                "scalar-bap3-bap5-full-golden")) {
        return false;
    }
    ++cases;

    validBits = 0;
    const auto asymmetricBytes = pack(
        {{16U, 5U}, {15U, 5U}, {32768U, 16U}, {32767U, 16U}}, &validBits);
    MantissaBitReader asymmetricReader(asymmetricBytes, validBits);
    MantissaDecodeState asymmetricState;
    const MantissaDecodeResult asymmetric = decodeMantissas(
        asymmetricReader, request({6U, 6U, 15U, 15U}, {0U, 0U, 4U, 4U}),
        asymmetricState);
    if (!expect(asymmetric.disposition == MantissaDisposition::Accepted
                    && close(asymmetric.mantissas[0], -1.0)
                    && close(asymmetric.mantissas[1], 15.0 / 16.0)
                    && close(asymmetric.transformCoefficients[2], -1.0 / 16.0)
                    && close(asymmetric.transformCoefficients[3], 32767.0 / 524288.0),
                "asymmetric-extremes-and-exponent")) {
        return false;
    }
    ++cases;

    const unsigned scalarBapBits[16] = {
        0, 0, 0, 3, 0, 4, 5, 6,
        7, 8, 9, 10, 11, 12, 14, 16};
    std::vector<std::pair<unsigned, unsigned>> scalarFields;
    std::vector<unsigned> scalarBaps;
    std::vector<unsigned> scalarExponents;
    for (unsigned bap = 5U; bap <= 15U; ++bap) {
        scalarBaps.push_back(bap);
        scalarExponents.push_back(0U);
        scalarFields.emplace_back(0U, scalarBapBits[bap]);
    }
    validBits = 0;
    const auto scalarRangeBytes = pack(scalarFields, &validBits);
    MantissaBitReader scalarRangeReader(scalarRangeBytes, validBits);
    MantissaDecodeState scalarRangeState;
    const MantissaDecodeResult scalarRange = decodeMantissas(
        scalarRangeReader, request(scalarBaps, scalarExponents),
        scalarRangeState);
    if (!expect(scalarRange.disposition == MantissaDisposition::Accepted
                    && scalarRange.mantissas.size() == scalarBaps.size()
                    && scalarRange.transformCoefficients.size()
                           == scalarBaps.size(),
                "scalar-bap5-to-15-widths")) {
        return false;
    }
    ++cases;

    validBits = 0;
    const auto groupedBytes = pack({{5U, 5U}}, &validBits);
    MantissaBitReader groupedReader(groupedBytes, validBits);
    MantissaDecodeState groupedState;
    MantissaDecodeRequest first = request({1U}, {0U}, false);
    first.channelId = 4;
    first.audioBlockId = 2;
    const MantissaDecodeResult firstGroup =
        decodeMantissas(groupedReader, first, groupedState);
    const bool pendingAfterFirst = groupedState.hasPendingGroup;
    MantissaDecodeRequest continuation = request({1U, 1U}, {1U, 2U}, true);
    continuation.channelId = 4;
    continuation.audioBlockId = 2;
    const MantissaDecodeResult secondGroup =
        decodeMantissas(groupedReader, continuation, groupedState);
    if (!expect(firstGroup.disposition == MantissaDisposition::Accepted
                    && firstGroup.bitsConsumed == 5U
                    && pendingAfterFirst
                    && secondGroup.disposition == MantissaDisposition::Accepted
                    && secondGroup.bitsConsumed == 0U
                    && close(firstGroup.mantissas[0], -2.0 / 3.0)
                    && close(secondGroup.mantissas[0], 0.0)
                    && close(secondGroup.mantissas[1], 2.0 / 3.0)
                    && !groupedState.hasPendingGroup,
                "grouped-bap1-cache-same-channel-block")) {
        return false;
    }
    ++cases;

    validBits = 0;
    const auto groupedAllBytes = pack({{14U, 7U}, {5U, 7U}}, &validBits);
    MantissaBitReader groupedAllReader(groupedAllBytes, validBits);
    MantissaDecodeState groupedAllState;
    const MantissaDecodeResult groupedAll = decodeMantissas(
        groupedAllReader, request({2U, 2U, 2U, 4U, 4U},
                                  {0U, 0U, 0U, 0U, 0U}), groupedAllState);
    if (!expect(groupedAll.disposition == MantissaDisposition::Accepted
                    && groupedAll.bitsConsumed == 14U
                    && close(groupedAll.mantissas[0], -4.0 / 5.0)
                    && close(groupedAll.mantissas[1], 0.0)
                    && close(groupedAll.mantissas[2], 4.0 / 5.0)
                    && close(groupedAll.mantissas[3], -10.0 / 11.0)
                    && close(groupedAll.mantissas[4], 0.0),
                "grouped-bap2-and-bap4-degroup")) {
        return false;
    }
    ++cases;

    validBits = 0;
    MantissaBitReader ditherReader;
    MantissaDecodeState ditherState;
    MantissaDecodeRequest noDither = request({0U, 0U}, {0U, 5U});
    noDither.dithflag = false;
    const MantissaDecodeResult zero =
        decodeMantissas(ditherReader, noDither, ditherState);
    MantissaBitReader injectedReader;
    MantissaDecodeState injectedState;
    MantissaDecodeRequest injected = noDither;
    injected.dithflag = true;
    injected.ditherSource = [](std::size_t index, unsigned) {
        return index == 0U ? 0.5 : -0.25;
    };
    const MantissaDecodeResult dither =
        decodeMantissas(injectedReader, injected, injectedState);
    if (!expect(zero.disposition == MantissaDisposition::Accepted
                    && !zero.ditherUsed
                    && zero.transformCoefficients[0] == 0.0
                    && zero.transformCoefficients[1] == 0.0
                    && dither.disposition == MantissaDisposition::Accepted
                    && dither.ditherUsed
                    && close(dither.transformCoefficients[0], 0.5)
                    && close(dither.transformCoefficients[1], -0.25 / 32.0),
                "bap0-injected-dither-and-zero")) {
        return false;
    }
    ++cases;

    validBits = 0;
    const auto cursorBytes = pack({{0U, 5U}}, &validBits);
    MantissaBitReader cursorReader(cursorBytes, validBits);
    MantissaDecodeState cursorState;
    std::vector<std::size_t> ditherIndices;
    const DitherSource cursorSource =
        [&ditherIndices](std::size_t index, unsigned) {
            ditherIndices.push_back(index);
            return index == 0U ? 0.25 : index == 2U ? 0.5 : 0.75;
        };
    MantissaDecodeRequest cursorFirst = request({0U, 6U, 0U},
                                                {0U, 0U, 1U}, false);
    cursorFirst.channelId = 7U;
    cursorFirst.audioBlockId = 3U;
    cursorFirst.dithflag = true;
    cursorFirst.ditherSource = cursorSource;
    const MantissaDecodeResult cursorFirstResult = decodeMantissas(
        cursorReader, cursorFirst, cursorState);
    MantissaDecodeRequest cursorSecond = request({0U}, {0U}, false);
    cursorSecond.channelId = 7U;
    cursorSecond.audioBlockId = 3U;
    cursorSecond.dithflag = true;
    cursorSecond.ditherSource = cursorSource;
    const MantissaDecodeResult cursorSecondResult = decodeMantissas(
        cursorReader, cursorSecond, cursorState);
    const std::size_t cursorBeforeReset =
        cursorState.absoluteCoefficientCursor;
    cursorState.reset();
    if (!expect(cursorFirstResult.disposition == MantissaDisposition::Accepted
                    && cursorSecondResult.disposition
                           == MantissaDisposition::Accepted
                    && cursorBeforeReset == 4U
                    && cursorState.absoluteCoefficientCursor == 0U
                    && ditherIndices == std::vector<std::size_t>{0U, 2U, 3U}
                    && close(cursorFirstResult.transformCoefficients[0], 0.25)
                    && close(cursorFirstResult.transformCoefficients[2], 0.25)
                    && close(cursorSecondResult.transformCoefficients[0], 0.75),
                "dither-absolute-cursor-across-calls")) {
        return false;
    }
    ++cases;

    MantissaBitReader missingDitherReader;
    MantissaDecodeState missingDitherState;
    MantissaDecodeRequest missingDither = noDither;
    missingDither.dithflag = true;
    const MantissaDecodeResult missingDitherResult = decodeMantissas(
        missingDitherReader, missingDither, missingDitherState);
    if (!expect(missingDitherResult.disposition
                        == MantissaDisposition::Unsupported
                    && missingDitherResult.reason == "dither-source-required",
                "bap0-dither-source-required")) {
        return false;
    }
    ++cases;

    validBits = 0;
    const auto invalidBytes = pack({{31U, 5U}}, &validBits);
    MantissaBitReader invalidReader(invalidBytes, validBits);
    MantissaDecodeState invalidState;
    const MantissaDecodeResult invalid = decodeMantissas(
        invalidReader, request({1U}, {0U}), invalidState);
    if (!expect(invalid.disposition == MantissaDisposition::Malformed
                    && invalid.reason == "group-code-invalid"
                    && invalidReader.bitOffset() == 0U
                    && !invalidState.hasPendingGroup,
                "invalid-group-code-transaction")) {
        return false;
    }
    ++cases;

    validBits = 0;
    const auto scalarInvalidBytes = pack({{15U, 4U}}, &validBits);
    MantissaBitReader scalarInvalidReader(scalarInvalidBytes, validBits);
    MantissaDecodeState scalarInvalidState;
    const MantissaDecodeResult scalarInvalid = decodeMantissas(
        scalarInvalidReader, request({5U}, {0U}), scalarInvalidState);
    validBits = 0;
    const auto bap3InvalidBytes = pack({{7U, 3U}}, &validBits);
    MantissaBitReader bap3InvalidReader(bap3InvalidBytes, validBits);
    MantissaDecodeState bap3InvalidState;
    const MantissaDecodeResult bap3Invalid = decodeMantissas(
        bap3InvalidReader, request({3U}, {0U}), bap3InvalidState);
    if (!expect(scalarInvalid.disposition == MantissaDisposition::Malformed
                    && scalarInvalid.reason == "scalar-code-invalid"
                    && scalarInvalidReader.bitOffset() == 0U
                    && !scalarInvalidState.hasPendingGroup
                    && bap3Invalid.disposition
                           == MantissaDisposition::Malformed
                    && bap3Invalid.reason == "scalar-code-invalid"
                    && bap3InvalidReader.bitOffset() == 0U
                    && !bap3InvalidState.hasPendingGroup,
                "invalid-bap3-bap5-rollback")) {
        return false;
    }
    ++cases;

    validBits = 0;
    const auto truncatedBytes = pack({{5U, 4U}}, &validBits);
    MantissaBitReader truncatedReader(truncatedBytes, validBits);
    MantissaDecodeState truncatedState;
    const MantissaDecodeResult truncated = decodeMantissas(
        truncatedReader, request({1U}, {0U}), truncatedState);
    if (!expect(truncated.disposition == MantissaDisposition::Malformed
                    && truncated.reason == "mantissa-truncated"
                    && truncatedReader.bitOffset() == 0U
                    && truncated.mantissas.empty(),
                "truncated-no-partial-output")) {
        return false;
    }
    ++cases;

    validBits = 0;
    const auto contextBytes = pack({{5U, 5U}}, &validBits);
    MantissaBitReader contextReader(contextBytes, validBits);
    MantissaDecodeState contextState;
    MantissaDecodeRequest pending = request({1U}, {0U}, false);
    pending.channelId = 1;
    pending.audioBlockId = 1;
    const MantissaDecodeResult pendingResult =
        decodeMantissas(contextReader, pending, contextState);
    MantissaDecodeRequest changed = request({1U, 1U}, {0U, 0U}, true);
    changed.channelId = 2;
    changed.audioBlockId = 1;
    const MantissaDecodeResult changedResult =
        decodeMantissas(contextReader, changed, contextState);
    contextState.reset();
    if (!expect(pendingResult.disposition == MantissaDisposition::Accepted
                    && changedResult.disposition == MantissaDisposition::Malformed
                    && changedResult.reason
                           == "group-context-change-with-pending"
                    && contextReader.bitOffset() == 5U
                    && !contextState.hasContext
                    && !contextState.hasPendingGroup,
                "group-cache-context-and-reset")) {
        return false;
    }
    ++cases;

    validBits = 0;
    const auto dummyBytes = pack({{5U, 5U}}, &validBits);
    MantissaBitReader dummyReader(dummyBytes, validBits);
    MantissaDecodeState dummyState;
    const MantissaDecodeResult dummy = decodeMantissas(
        dummyReader, request({1U}, {0U}, true), dummyState);
    if (!expect(dummy.disposition == MantissaDisposition::Accepted
                    && dummy.bitsConsumed == 5U
                    && !dummyState.hasContext
                    && !dummyState.hasPendingGroup,
                "block-end-dummy-discard")) {
        return false;
    }
    ++cases;

    std::cout << "selfTest=PASS cases=" << cases
              << " implementation=native-eac3-mantissas-gate8n-2b-3\n";
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc == 2 && std::string(argv[1]) == "--self-test") {
        return runSelfTest() ? 0 : 1;
    }
    std::cerr << "Usage: Eac3NativeMantissasProbe --self-test\n";
    return 2;
}
