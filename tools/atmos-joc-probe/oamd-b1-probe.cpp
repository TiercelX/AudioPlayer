#include "oamd-b1.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

class BitWriter {
public:
    void write(unsigned count, unsigned value)
    {
        for (unsigned bit = 0; bit < count; ++bit) {
            bits_.push_back(static_cast<std::uint8_t>(
                (value >> (count - bit - 1U)) & 1U));
        }
    }

    void variable(unsigned groupBits, unsigned value, bool multiGroup = false)
    {
        if (!multiGroup) {
            write(groupBits, value);
            write(1, 0);
            return;
        }
        write(groupBits, 0);
        write(1, 1);
        write(groupBits, value);
        write(1, 0);
    }

    void variableMax4x4()
    {
        // variable_bits_max(4, 4), all four groups at 0xf.  The first
        // three groups continue; the fourth terminates the value.
        for (unsigned group = 0; group < 4; ++group) {
            write(4, 0xf);
            write(1, group == 3 ? 0U : 1U);
        }
    }

    std::vector<std::uint8_t> bytes(bool nonzeroFinalPadding = false,
                                    bool truncate = false) const
    {
        std::vector<std::uint8_t> result((bits_.size() + 7U) / 8U, 0);
        for (std::size_t index = 0; index < bits_.size(); ++index) {
            if (bits_[index] != 0) {
                result[index / 8U]
                    |= static_cast<std::uint8_t>(1U << (7U - index % 8U));
            }
        }
        if (nonzeroFinalPadding && bits_.size() % 8U == 0) {
            result.push_back(0x80);
        } else if (nonzeroFinalPadding && !result.empty()) {
            result.back() |= 0x01;
        }
        if (truncate && !result.empty()) {
            result.pop_back();
        }
        return result;
    }

private:
    std::vector<std::uint8_t> bits_;
};

std::vector<std::uint8_t> makePayload(const std::vector<unsigned> &ids,
                                      bool extendedCounts = false,
                                      bool multiGroupSize = false,
                                      bool discardUnknown = false,
                                      bool nonzeroPadding = false,
                                      bool truncated = false,
                                      bool sizeOverrun = false)
{
    BitWriter writer;
    writer.write(2, extendedCounts ? 3 : 0);
    if (extendedCounts) {
        writer.write(3, 1); // version = 4
    }
    writer.write(5, extendedCounts ? 31 : 1);
    if (extendedCounts) {
        writer.write(7, 0); // object_count = 32
    }
    writer.write(1, 1); // dynamic-only program
    writer.write(1, 1); // LFE present
    writer.write(1, 0); // no alternate object data id
    if (extendedCounts) {
        writer.write(4, 15);
        writer.write(5, 1); // element_count = 16
    } else {
        writer.write(4, static_cast<unsigned>(ids.size()));
    }

    const std::size_t count = extendedCounts ? 16U : ids.size();
    for (std::size_t index = 0; index < count; ++index) {
        const unsigned id = extendedCounts ? 1U : ids[index];
        writer.write(4, id);
        if (sizeOverrun) {
            writer.variable(4, 7); // declares eight bytes, supplies none
            continue;
        }
        const bool useLargeBody = multiGroupSize && index == 0;
        writer.variable(4, useLargeBody ? 0U : 0U, useLargeBody);
        writer.write(1, id > 5 && discardUnknown ? 1U : 0U);
        const unsigned bodyBits = useLargeBody ? 135U : 7U;
        for (unsigned bit = 0; bit < bodyBits; ++bit) {
            writer.write(1, 0);
        }
    }
    return writer.bytes(nonzeroPadding, truncated);
}

std::vector<std::uint8_t> makeContentProgramPayload(bool reservedDescription)
{
    BitWriter writer;
    writer.write(2, 0); // supported version 0
    writer.write(5, 1); // object_count_bits -> 2 objects
    writer.write(1, 0); // not dynamic-only
    writer.write(4, reservedDescription ? 1U : 14U);
    if (reservedDescription) {
        writer.write(4, 0); // one byte reserved_data plus padding
        writer.write(8, 0);
    } else {
        writer.write(1, 1); // bed channel distribute
        writer.write(1, 1); // multiple bed instances
        writer.write(3, 0); // two bed instances
        writer.write(1, 0); // first bed is not LFE-only
        writer.write(1, 1); // standard assignment
        writer.write(10, 0x155);
        writer.write(1, 1); // second bed is LFE-only
        writer.write(3, 3); // ISF index
        writer.write(5, 1); // dynamic object count bits -> 2 objects
    }
    writer.write(1, 0); // no alternate object data id
    writer.write(4, 0); // no oa elements
    return writer.bytes();
}

std::vector<std::uint8_t> makeReservedIsfPayload(unsigned isfIndex)
{
    BitWriter writer;
    writer.write(2, 0); // supported version 0
    writer.write(5, 0); // one object
    writer.write(1, 0); // content-description form
    writer.write(4, 4); // ISF only
    writer.write(3, isfIndex);
    writer.write(1, 0); // no alternate object data id
    writer.write(4, 0); // no elements
    return writer.bytes();
}

std::vector<std::uint8_t> makeAlternateIdPayload()
{
    BitWriter writer;
    writer.write(2, 0); // supported version 0
    writer.write(5, 0); // one object
    writer.write(1, 1); // dynamic-only
    writer.write(1, 0); // no LFE
    writer.write(1, 1); // alternate object data present
    writer.write(4, 1); // one element
    writer.write(4, 1); // recognized object element
    writer.variable(4, 0); // one-byte declared boundary
    writer.write(4, 1); // reserved non-default alternate ID
    writer.write(1, 0); // not discardable
    writer.write(3, 0); // opaque body remainder
    return writer.bytes();
}

std::vector<std::uint8_t> makeMaxSyntaxCountsPayload()
{
    BitWriter writer;
    writer.write(2, 0); // supported version 0
    writer.write(5, 31); // escape
    writer.write(7, 127); // object_count = 159
    writer.write(1, 1); // dynamic-only
    writer.write(1, 1); // LFE
    writer.write(1, 0); // no alternate object data id
    writer.write(4, 15); // element-count escape
    writer.write(5, 31); // element-count = 46
    for (unsigned element = 0; element < 46; ++element) {
        writer.write(4, 1);
        writer.variable(4, 0);
        writer.write(1, 0);
        writer.write(7, 0);
    }
    return writer.bytes();
}

std::vector<std::uint8_t> makeMaxDynamicObjectCountPayload()
{
    BitWriter writer;
    writer.write(2, 0); // supported version 0
    writer.write(5, 0); // one declared object
    writer.write(1, 0); // content-description form
    writer.write(4, 2); // dynamic content-description only
    writer.write(5, 31); // dynamic-object-count escape
    writer.write(7, 127); // dynamic-object count = 159
    writer.write(1, 0); // no alternate object data id
    writer.write(4, 0); // no elements
    return writer.bytes();
}

std::vector<std::uint8_t> makeMaxVariableSizePayload()
{
    BitWriter writer;
    writer.write(2, 0); // supported version 0
    writer.write(5, 0); // one object
    writer.write(1, 1); // dynamic-only
    writer.write(1, 0); // no LFE
    writer.write(1, 0); // no alternate object data id
    writer.write(4, 1); // one element
    writer.write(4, 1); // recognized object element
    writer.variableMax4x4(); // size code 69903, then no body bytes
    return writer.bytes();
}

bool expect(const eac3oamd::B1Frame &frame,
            eac3oamd::B1Disposition disposition,
            const char *label)
{
    if (frame.disposition != disposition) {
        std::cerr << "b1SelfTest=FAIL case=" << label
                  << " disposition=" << eac3oamd::dispositionText(frame.disposition)
                  << " reason=" << frame.reason << '\n';
        return false;
    }
    return true;
}

eac3oamd::B1SelfTestReport runSynthetic()
{
    eac3oamd::B1SelfTestReport report;
    if (!expect(eac3oamd::parseB1(makePayload({1, 2, 5})),
                eac3oamd::B1Disposition::Pass, "normal-recognized")) {
        report.reason = "normal-recognized-failed";
        return report;
    }
    ++report.cases;
    ++report.normalCases;
    ++report.recognizedCases;
    ++report.programAssignmentCases;

    const auto content = eac3oamd::parseB1(makeContentProgramPayload(false));
    if (!expect(content, eac3oamd::B1Disposition::Pass, "content-program-assignment")
        || content.program.dynamicOnly
        || content.program.bedInstances != 2
        || content.program.intermediateSpatialFormat != 3
        || content.program.dynamicObjects != 2
        || content.program.programType != "bed+isf+dynamic") {
        report.reason = "content-program-assignment-failed";
        return report;
    }
    ++report.cases;
    ++report.normalCases;
    ++report.programAssignmentCases;

    const auto reservedProgram = eac3oamd::parseB1(makeContentProgramPayload(true));
    if (!expect(reservedProgram, eac3oamd::B1Disposition::Pass,
                "reserved-content-description")) {
        report.reason = "reserved-content-description-failed";
        return report;
    }
    ++report.cases;
    ++report.normalCases;
    ++report.programAssignmentCases;

    const auto extended = eac3oamd::parseB1(makePayload({}, true));
    if (!expect(extended, eac3oamd::B1Disposition::Unsupported, "unsupported-extended-version")
        || extended.version != 4 || extended.objectCount != 32
        || extended.elementCount != 16) {
        report.reason = "unsupported-extended-version-failed:disp="
            + std::string(eac3oamd::dispositionText(extended.disposition))
            + ":reason=" + extended.reason
            + ":version=" + std::to_string(extended.version)
            + ":objects=" + std::to_string(extended.objectCount)
            + ":elements=" + std::to_string(extended.elementCount);
        return report;
    }
    ++report.cases;
    ++report.unsupportedVersionCases;

    const auto maxCounts = eac3oamd::parseB1(makeMaxSyntaxCountsPayload());
    if (!expect(maxCounts, eac3oamd::B1Disposition::Pass, "syntax-max-counts")
        || maxCounts.version != 0 || maxCounts.objectCount != 159
        || maxCounts.elementCount != 46 || maxCounts.elements.size() != 46) {
        report.reason = "syntax-max-counts-failed:disp="
            + std::string(eac3oamd::dispositionText(maxCounts.disposition))
            + ":reason=" + maxCounts.reason
            + ":objects=" + std::to_string(maxCounts.objectCount)
            + ":elements=" + std::to_string(maxCounts.elementCount);
        return report;
    }
    ++report.cases;
    ++report.extendedCases;
    ++report.syntaxBoundaryCases;

    const auto maxDynamic = eac3oamd::parseB1(makeMaxDynamicObjectCountPayload());
    if (!expect(maxDynamic, eac3oamd::B1Disposition::Pass,
                "syntax-max-dynamic-object-count")
        || maxDynamic.program.dynamicObjects != 159) {
        report.reason = "syntax-max-dynamic-object-count-failed:disp="
            + std::string(eac3oamd::dispositionText(maxDynamic.disposition))
            + ":reason=" + maxDynamic.reason
            + ":dynamicObjects=" + std::to_string(maxDynamic.program.dynamicObjects);
        return report;
    }
    ++report.cases;
    ++report.syntaxBoundaryCases;

    const auto variable = eac3oamd::parseB1(makePayload({1}, false, true));
    if (!expect(variable, eac3oamd::B1Disposition::Pass, "variable-groups")
        || variable.elements.size() != 1 || variable.elements[0].sizeBytes != 17) {
        report.reason = "variable-groups-failed";
        return report;
    }
    ++report.cases;
    ++report.variableGroupCases;

    const auto maxVariable = eac3oamd::parseB1(makeMaxVariableSizePayload());
    if (!expect(maxVariable, eac3oamd::B1Disposition::Malformed,
                "variable-groups-maximum-boundary")
        || maxVariable.reason != "element-size-overrun") {
        report.reason = "variable-groups-maximum-boundary-failed:disp="
            + std::string(eac3oamd::dispositionText(maxVariable.disposition))
            + ":reason=" + maxVariable.reason;
        return report;
    }
    ++report.cases;
    ++report.maxVariableSizeCases;

    if (!expect(eac3oamd::parseB1(makePayload({1, 2}, false, false, false, false, true)),
                eac3oamd::B1Disposition::Malformed, "truncated")
        || !expect(eac3oamd::parseB1(makePayload({1}, false, false, false, false, false, true)),
                   eac3oamd::B1Disposition::Malformed, "size-overrun")
        || !expect(eac3oamd::parseB1(makePayload({}, false, false, false, true)),
                   eac3oamd::B1Disposition::Malformed, "nonzero-padding")) {
        report.reason = "malformed-boundary-cases-failed";
        return report;
    }
    report.cases += 3;
    report.truncatedCases = 1;
    report.sizeOverrunCases = 1;
    report.nonzeroPaddingCases = 1;

    const auto discardable = eac3oamd::parseB1(
        makePayload({1, 9, 2}, false, false, true));
    if (!expect(discardable, eac3oamd::B1Disposition::Pass,
                "known-discardable-unknown-known")
        || discardable.elements.size() != 3U
        || !discardable.elements[0].recognized
        || !discardable.elements[1].discardUnknown
        || discardable.elements[1].recognized
        || !discardable.elements[2].recognized
        || !expect(eac3oamd::parseB1(makePayload({9}, false, false, false)),
                   eac3oamd::B1Disposition::Unsupported, "nondiscardable-unknown")) {
        report.reason = "unknown-element-cases-failed";
        return report;
    }
    report.cases += 2;
    report.discardableUnknownCases = 1;
    report.nondiscardableUnknownCases = 1;

    const auto reservedIsf = eac3oamd::parseB1(makeReservedIsfPayload(6));
    if (!expect(reservedIsf, eac3oamd::B1Disposition::Unsupported,
                "reserved-isf-index")) {
        report.reason = "reserved-isf-index-failed";
        return report;
    }
    const auto reservedAlternate = eac3oamd::parseB1(makeAlternateIdPayload());
    if (!expect(reservedAlternate, eac3oamd::B1Disposition::Unsupported,
                "reserved-alternate-id")) {
        report.reason = "reserved-alternate-id-failed";
        return report;
    }
    report.cases += 2;
    report.reservedValueCases = 2;
    report.pass = true;
    report.reason = "bounded-count-program-element-and-reserved-value-cases";
    return report;
}

} // namespace

int main()
{
    const eac3oamd::B1SelfTestReport report = runSynthetic();
    std::cout << "oamdB1SelfTest=" << (report.pass ? "PASS" : "FAIL")
              << " totalCases=" << report.cases
              << " reason=" << report.reason << '\n'
              << "oamdB1NormalCases=" << report.normalCases << '\n'
              << "oamdB1ProgramAssignmentCases="
              << report.programAssignmentCases << '\n'
              << "oamdB1ExtendedCases=" << report.extendedCases << '\n'
              << "oamdB1UnsupportedVersionCases=" << report.unsupportedVersionCases << '\n'
              << "oamdB1SyntaxBoundaryCases=" << report.syntaxBoundaryCases << '\n'
              << "oamdB1ReservedValueCases=" << report.reservedValueCases << '\n'
              << "oamdB1VariableGroupCases=" << report.variableGroupCases << '\n'
              << "oamdB1MaxVariableSizeCases=" << report.maxVariableSizeCases << '\n'
              << "oamdB1TruncatedCases=" << report.truncatedCases << '\n'
              << "oamdB1SizeOverrunCases=" << report.sizeOverrunCases << '\n'
              << "oamdB1NonzeroPaddingCases=" << report.nonzeroPaddingCases << '\n'
              << "oamdB1RecognizedCases=" << report.recognizedCases << '\n'
              << "oamdB1DiscardableUnknownCases=" << report.discardableUnknownCases << '\n'
              << "oamdB1NondiscardableUnknownCases=" << report.nondiscardableUnknownCases << '\n'
              << "oamdB1Result=" << (report.pass ? "PASS" : "FAIL")
              << " stage=gate6b1-oamd-framing-inventory\n";
    return report.pass ? 0 : 1;
}
