#include "oamd-additional.h"
#include "oamd-b2b.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <utility>
#include <vector>

namespace {

class Writer {
public:
    void write(unsigned count, unsigned value)
    {
        for (unsigned bit = 0U; bit < count; ++bit)
            bits_.push_back(static_cast<std::uint8_t>((value >> (count - bit - 1U)) & 1U));
    }
    std::vector<std::uint8_t> bytes() const
    {
        std::vector<std::uint8_t> bytes((bits_.size() + 7U) / 8U, 0U);
        for (std::size_t bit = 0U; bit < bits_.size(); ++bit)
            if (bits_[bit]) bytes[bit / 8U] |= static_cast<std::uint8_t>(1U << (7U - bit % 8U));
        return bytes;
    }
    std::size_t bits() const { return bits_.size(); }
private:
    std::vector<std::uint8_t> bits_;
};

eac3oamd::B2aFrame makeB2a()
{
    eac3oamd::B2aFrame frame;
    frame.disposition = eac3oamd::B2aDisposition::Pass;
    frame.objectInfoBlockCount = 2U;
    frame.blocks.resize(2U);
    for (unsigned object = 0U; object < 2U; ++object) {
        for (unsigned block = 0U; block < 2U; ++block) {
            eac3oamd::B2aObjectInfo info;
            info.objectIndex = object;
            info.blockIndex = block;
            info.basicInfoStatus = block == 0U ? 1U : 2U;
            info.gainIndexPresent = block == 0U;
            info.gainIndex = 2U;
            info.gainBitsPresent = block == 0U;
            info.gainBits = 10U;
            info.defaultPriorityPresent = block == 0U;
            info.defaultPriority = false;
            info.priorityBitsPresent = block == 0U;
            info.priorityBits = 16U;
            info.renderInfoStatus = object == 0U ? (block == 0U ? 1U : 2U) : 0U;
            if (object == 0U && block == 0U) {
                info.renderInfoPresence = {true, true, true, true};
                info.absolutePositionPresent = true;
                info.absoluteXBits = 32U;
                info.absoluteYBits = 32U;
                info.absoluteZSignBits = 1U;
                info.absoluteZBits = 8U;
                info.differentialPositionPresent = true;
                info.zonePresent = true;
                info.zoneConstraintsIndex = 0U;
                info.enableElevation = true;
                info.sizePresent = true;
                info.sizeIndex = 1U;
                info.sizeBits = 8U;
                info.screenReferencePresent = true;
                info.snapPresent = true;
            }
            info.objectNotActive = false;
            frame.objectInfo.push_back(info);
        }
    }
    return frame;
}

eac3oamd::B1Frame makeB1(const std::vector<std::uint8_t> &trim,
                         std::size_t trimBits,
                         const std::vector<std::uint8_t> &extended,
                         std::size_t extendedBits,
                         bool discardableUnknown = false)
{
    eac3oamd::B1Frame frame;
    frame.disposition = eac3oamd::B1Disposition::Pass;
    frame.version = 0U;
    frame.objectCount = 2U;
    if (trimBits != 0U) {
        frame.elements.push_back(eac3oamd::ElementInventory {
            2U, 1U, false, 0U, false, true, trimBits, trim});
    }
    if (discardableUnknown) {
        frame.elements.push_back(eac3oamd::ElementInventory {
            9U, 1U, false, 0U, true, false, 7U, std::vector<std::uint8_t> {0U}});
    }
    if (extendedBits != 0U) {
        frame.elements.push_back(eac3oamd::ElementInventory {
            5U, 1U, false, 0U, false, true, extendedBits, extended});
    }
    return frame;
}

void makeTrim(Writer *writer, bool reserved = false, bool tail = false)
{
    writer->write(2U, reserved ? 2U : 1U);
    writer->write(2U, 0U);
    writer->write(2U, 2U);
    writer->write(1U, 0U);
    writer->write(1U, 0U);
    writer->write(5U, 0x1fU);
    writer->write(4U, 0U);
    writer->write(4U, 4U);
    writer->write(4U, 5U);
    writer->write(1U, 1U);
    writer->write(4U, 7U);
    writer->write(1U, 0U);
    writer->write(4U, 8U);
    for (unsigned cfg = 1U; cfg < eac3oamd::kNumTrimConfigs; ++cfg) writer->write(1U, 1U);
    writer->write(1U, 1U);
    writer->write(1U, 1U);
    writer->write(1U, 0U);
    if (tail) writer->write(1U, 1U);
}

void makeExtended(Writer *writer, bool tail = false)
{
    writer->write(1U, 1U); // divergence block
    writer->write(1U, 1U); writer->write(2U, 2U); writer->write(6U, 32U);
    writer->write(1U, 1U); writer->write(2U, 1U); // reuse prior object block
    // object 1 is the helper/bed object and emits no divergence fields
    writer->write(1U, 1U); // extended precision block
    writer->write(1U, 1U); writer->write(3U, 7U);
    writer->write(2U, 0U); writer->write(2U, 1U); writer->write(2U, 2U);
    writer->write(1U, 1U); writer->write(3U, 4U); writer->write(2U, 3U);
    if (tail) writer->write(1U, 1U);
}

void makeExtendedTableAndAbsent(Writer *writer)
{
    writer->write(1U, 1U); // divergence block
    writer->write(1U, 1U); writer->write(2U, 0U); writer->write(2U, 2U);
    writer->write(1U, 0U); // object 0, block 1: explicit reset to zero
    // object 1 is the helper/bed object and emits no divergence fields
    writer->write(1U, 0U); // no extended precision block
}

void makeExtendedInactive(Writer *writer)
{
    writer->write(1U, 1U); // divergence block
    writer->write(1U, 1U); writer->write(2U, 0U); writer->write(2U, 2U);
    // object 0, block 1 is inactive and emits no b_object_divergence bit;
    // object 1 is the helper/bed object and emits no fields either.
    writer->write(1U, 0U); // no extended precision block
}

void makeTrimGlobal(Writer *writer, unsigned globalMode)
{
    writer->write(2U, 1U);
    writer->write(2U, 0U);
    writer->write(2U, globalMode);
    writer->write(1U, 0U); // b_disable_trim_per_obj
}

void makeExtendedReserved(Writer *writer, unsigned mode, unsigned code)
{
    writer->write(1U, 1U); // divergence block
    writer->write(1U, 1U); writer->write(2U, mode); writer->write(6U, code);
    writer->write(1U, 0U); // object 0, block 1
    writer->write(1U, 0U); // no extended precision block
}

bool check(bool condition, const char *label, std::string *reason)
{
    if (!condition && reason && reason->empty()) *reason = label;
    return condition;
}

} // namespace

int main()
{
    eac3oamd::AdditionalSelfTestReport report;
    std::string reason;
    const eac3oamd::B2aFrame b2a = makeB2a();
    const std::vector<bool> helper {false, true};
    Writer trimWriter;
    makeTrim(&trimWriter);
    Writer extendedWriter;
    makeExtended(&extendedWriter);
    const auto b1 = makeB1(trimWriter.bytes(), trimWriter.bits(),
                           extendedWriter.bytes(), extendedWriter.bits(), true);
    const auto parsed = eac3oamd::parseAdditionalElements(
        b1, b2a, helper);
    ++report.cases;
    if (check(parsed.disposition == eac3oamd::AdditionalDisposition::Pass
                  && parsed.trimPresent && parsed.extendedPresent
                  && parsed.trim.configurations[0].centreCode == 0U
                  && parsed.trim.configurations[0].surroundCode == 4U
                  && parsed.trim.objectTrimDisabled == std::vector<bool> {true, false},
              "trim-marker-values", &reason)) {
        ++report.trimCases;
    }
    ++report.cases;
    if (check(parsed.extended.divergence[0].value > 0.7048F
                  && parsed.extended.divergence[1].reused
                  && parsed.extended.extendedPosition[0].values
                      == std::array<int, 3> {1, 2, -1}
                  && parsed.extended.extendedPosition[1].values[0] == -2,
              "extended-marker-values-and-reuse", &reason)) {
        ++report.divergenceCases;
        ++report.extendedPositionCases;
        ++report.reuseCases;
        ++report.multiObjectBlockCases;
    }
    eac3oamd::B2bState state;
    eac3oamd::B2bFrameOutput output;
    const auto applied = state.applyFrame(b2a, helper, &output, true, &b1);
    ++report.cases;
    if (!check(applied.disposition == eac3oamd::B2bDisposition::Pass
                   && output.objects[0].divergence > 0.7048F
                   && output.blockSnapshots[1].objects[0].divergenceReused
                   && output.blockSnapshots[0].objects[0].position.extendedPrecision[0] == 1,
               "b2b-effective-additional-state", &reason)) {
        // Keep the detailed parser checks below useful even if the integration
        // check exposes a regression.
    }
    Writer reservedTrim;
    makeTrim(&reservedTrim, true);
    auto reservedB1 = makeB1(reservedTrim.bytes(), reservedTrim.bits(), {}, 0U);
    const auto reservedResult = eac3oamd::parseAdditionalElements(
        reservedB1, b2a, helper);
    ++report.cases;
    if (check(reservedResult.disposition == eac3oamd::AdditionalDisposition::Unsupported,
              "reserved-warp-fail-closed", &reason)) ++report.reservedCases;
    Writer tailTrim;
    makeTrim(&tailTrim, false, true);
    auto tailB1 = makeB1(tailTrim.bytes(), tailTrim.bits(), {}, 0U);
    const auto tailResult = eac3oamd::parseAdditionalElements(tailB1, b2a, helper);
    ++report.cases;
    if (check(tailResult.disposition == eac3oamd::AdditionalDisposition::Malformed,
              "nonzero-tail-fail-closed", &reason)) ++report.tailCases;
    auto truncatedB1 = makeB1(trimWriter.bytes(), trimWriter.bits() - 1U, {}, 0U);
    const auto truncatedResult = eac3oamd::parseAdditionalElements(
        truncatedB1, b2a, helper);
    ++report.cases;
    if (check(truncatedResult.disposition == eac3oamd::AdditionalDisposition::Malformed,
              "truncated-fail-closed", &reason)) ++report.truncatedCases;
    eac3oamd::B1Frame emptyB1 = b1;
    emptyB1.elements[0].rawBody.clear();
    emptyB1.elements[0].rawBodyBits = 0U;
    const auto emptyResult = eac3oamd::parseAdditionalElements(emptyB1, b2a, helper);
    ++report.cases;
    if (check(emptyResult.disposition == eac3oamd::AdditionalDisposition::Malformed
                  && !emptyResult.trimPresent && !emptyResult.extendedPresent,
              "empty-on-failure", &reason)) ++report.emptyFailureCases;
    Writer reuseWriter;
    reuseWriter.write(1U, 1U); // divergence block
    reuseWriter.write(1U, 1U); reuseWriter.write(2U, 1U); // reuse block 0
    reuseWriter.write(1U, 0U); // no divergence in block 1
    reuseWriter.write(1U, 0U); // no extended position block
    const auto reuseBody = reuseWriter.bytes();
    auto reuseB1 = makeB1({}, 0U, reuseBody, reuseWriter.bits());
    const auto noPrior = eac3oamd::parseAdditionalElements(reuseB1, b2a, helper);
    const auto withPrior = eac3oamd::parseAdditionalElements(
        reuseB1, b2a, helper, std::vector<float> {0.25F, 0.0F},
        std::vector<bool> {true, false});
    ++report.cases;
    if (check(noPrior.disposition == eac3oamd::AdditionalDisposition::Malformed
                  && withPrior.disposition == eac3oamd::AdditionalDisposition::Pass
                  && withPrior.extended.divergence[0].reused,
              "reuse-reset-boundary", &reason)) ++report.resetCases;
    if (withPrior.disposition != eac3oamd::AdditionalDisposition::Pass) {
        std::cerr << "additionalReuseReason=" << withPrior.reason << '\n';
    }
    Writer tableAndAbsentWriter;
    makeExtendedTableAndAbsent(&tableAndAbsentWriter);
    const auto tableAndAbsentB1 = makeB1(
        {}, 0U, tableAndAbsentWriter.bytes(), tableAndAbsentWriter.bits());
    const auto tableAndAbsent = eac3oamd::parseAdditionalElements(
        tableAndAbsentB1, b2a, helper);
    ++report.cases;
    if (check(tableAndAbsent.disposition == eac3oamd::AdditionalDisposition::Pass
                  && tableAndAbsent.extended.divergence[0].mode == 0U
                  && tableAndAbsent.extended.divergence[0].table == 2U
                  && !tableAndAbsent.extended.divergence[1].present
                  && tableAndAbsent.extended.divergence[1].value == 0.0F
                  && !tableAndAbsent.extended.extendedPositionBlockPresent,
              "divergence-table-absent-and-block-reset", &reason)) {
        ++report.divergenceCases;
        ++report.resetCases;
    }
    Writer inactiveWriter;
    makeExtendedInactive(&inactiveWriter);
    auto inactiveB2a = b2a;
    inactiveB2a.objectInfo[1].objectNotActive = true;
    const auto inactiveB1 = makeB1({}, 0U, inactiveWriter.bytes(), inactiveWriter.bits());
    const auto inactiveResult = eac3oamd::parseAdditionalElements(
        inactiveB1, inactiveB2a, helper);
    ++report.cases;
    if (check(inactiveResult.disposition == eac3oamd::AdditionalDisposition::Pass
                  && inactiveResult.extended.divergence[1].present
                  && inactiveResult.extended.divergence[1].value == 0.0F,
              "inactive-divergence-reset", &reason)) {
        ++report.resetCases;
    }
    for (unsigned globalMode = 0U; globalMode <= 1U; ++globalMode) {
        Writer globalWriter;
        makeTrimGlobal(&globalWriter, globalMode);
        const auto globalB1 = makeB1(globalWriter.bytes(), globalWriter.bits(), {}, 0U);
        const auto globalResult = eac3oamd::parseAdditionalElements(
            globalB1, b2a, helper);
        ++report.cases;
        if (check(globalResult.disposition == eac3oamd::AdditionalDisposition::Pass
                      && globalResult.trim.configurations[0].defaultTrim,
                  "global-trim-mode-branch", &reason)) {
            ++report.trimCases;
        }
    }
    for (const auto reserved : {std::pair<unsigned, unsigned> {2U, 0U},
                                std::pair<unsigned, unsigned> {3U, 0U}}) {
        Writer reservedWriter;
        makeExtendedReserved(&reservedWriter, reserved.first, reserved.second);
        const auto reservedB1 = makeB1({}, 0U, reservedWriter.bytes(),
                                       reservedWriter.bits());
        const auto reservedResult = eac3oamd::parseAdditionalElements(
            reservedB1, b2a, helper);
        ++report.cases;
        if (reservedResult.disposition != eac3oamd::AdditionalDisposition::Unsupported)
            std::cerr << "reservedDivergenceMode=" << reserved.first
                      << " disposition="
                      << eac3oamd::additionalDispositionText(reservedResult.disposition)
                      << " reason=" << reservedResult.reason << '\n';
        if (check(reservedResult.disposition == eac3oamd::AdditionalDisposition::Unsupported,
                  "reserved-divergence-fail-closed", &reason)) {
            ++report.reservedCases;
        }
    }
    report.pass = reason.empty();
    report.reason = report.pass ? "normative-oamd-additional-selftest" : reason;
    std::cout << "oamdAdditionalSelfTest=" << (report.pass ? "PASS" : "FAIL")
              << " totalCases=" << report.cases
              << " trimCases=" << report.trimCases
              << " divergenceCases=" << report.divergenceCases
              << " extendedPositionCases=" << report.extendedPositionCases
              << " reuseCases=" << report.reuseCases
              << " resetCases=" << report.resetCases
              << " multiObjectBlockCases=" << report.multiObjectBlockCases
              << " reservedCases=" << report.reservedCases
              << " truncatedCases=" << report.truncatedCases
              << " tailCases=" << report.tailCases
              << " emptyFailureCases=" << report.emptyFailureCases
              << " reason=" << report.reason << '\n';
    return report.pass ? 0 : 1;
}
