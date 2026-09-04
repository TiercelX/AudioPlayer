#include "bs2127-virtual-ngon.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <string>

namespace {

struct SelfTestReport {
    bool pass = true;
    std::size_t cases = 0U;
    std::string reason;
};

void expect(SelfTestReport *report, bool condition, const char *name)
{
    ++report->cases;
    if (!condition && report->pass) {
        report->pass = false;
        report->reason = name;
    }
}

bool close(double left, double right, double tolerance = 1.0e-8)
{
    return std::abs(left - right) <= tolerance;
}

double power(const std::array<double, eac3render::kSystemHSpeakerCount> &gains)
{
    double result = 0.0;
    for (double gain : gains) {
        result += gain * gain;
    }
    return result;
}

eac3render::UnitVector3 normalizedSum(
    const eac3render::UnitVector3 &first,
    const eac3render::UnitVector3 &second,
    const eac3render::UnitVector3 &third)
{
    eac3render::UnitVector3 result {
        first[0] + second[0] + third[0],
        first[1] + second[1] + third[1],
        first[2] + second[2] + third[2],
    };
    const double length = std::sqrt(
        result[0] * result[0] + result[1] * result[1]
        + result[2] * result[2]);
    for (double &component : result) {
        component /= length;
    }
    return result;
}

eac3render::UnitVector3 normalizedPair(
    const eac3render::UnitVector3 &first,
    const eac3render::UnitVector3 &second)
{
    return normalizedSum(first, second, eac3render::UnitVector3 {0.0, 0.0, 0.0});
}

bool allFinite(const std::array<double, eac3render::kSystemHSpeakerCount> &gains)
{
    return std::all_of(gains.begin(), gains.end(), [](double gain) {
        return std::isfinite(gain);
    });
}

std::size_t indexForLabel(eac3render::Bs2051Label label)
{
    const auto &layout = eac3render::Bs2051SystemHLayout::systemH();
    for (std::size_t index = 0U; index < layout.size(); ++index) {
        if (layout[index].label == label) {
            return index;
        }
    }
    return eac3render::kSystemHSpeakerCount;
}

SelfTestReport runSelfTest()
{
    SelfTestReport report;
    const eac3render::Bs2127TopologyCatalog catalog =
        eac3render::buildSystemHBs2127Topology();
    const eac3render::Bs2127LowerVirtualNgon region =
        eac3render::buildLowerVirtualNgon(catalog);

    expect(&report, catalog.valid, "topology-catalog-valid");
    expect(&report, region.valid, "lower-virtual-ngon-built");
    expect(&report, region.virtualPointIndex
                        == eac3render::kBs2127VirtualBottomIndex,
           "lower-virtual-point-index");
    // This is a coordinate/tolerance regression for current System H, not a
    // normative count from BS.2127.
    expect(&report, region.ringRealSpeakerIndices.size() == 8U,
           "current-system-h-lower-ring-size");
    expect(&report, region.triangles.size() == region.ringRealSpeakerIndices.size()
                        && close(region.downmixCoefficient, 1.0 / std::sqrt(8.0)),
           "triangle-ring-and-wdmx");

    bool ringStartsAtMinimum = !region.ringRealSpeakerIndices.empty()
        && *std::min_element(region.ringRealSpeakerIndices.begin(),
                             region.ringRealSpeakerIndices.end())
            == region.ringRealSpeakerIndices.front();
    bool ringUnique = true;
    for (std::size_t first = 0U; first < region.ringRealSpeakerIndices.size();
         ++first) {
        for (std::size_t second = first + 1U;
             second < region.ringRealSpeakerIndices.size(); ++second) {
            ringUnique = ringUnique
                && region.ringRealSpeakerIndices[first]
                    != region.ringRealSpeakerIndices[second];
        }
    }
    expect(&report, ringStartsAtMinimum && ringUnique,
           "ring-canonical-start-and-no-duplicates");

    const std::array<eac3render::Bs2051Label, 8> expectedLabels {
        eac3render::Bs2051Label::MPlus135,
        eac3render::Bs2051Label::MPlus090,
        eac3render::Bs2051Label::BPlus045,
        eac3render::Bs2051Label::BPlus000,
        eac3render::Bs2051Label::BMinus045,
        eac3render::Bs2051Label::MMinus090,
        eac3render::Bs2051Label::MMinus135,
        eac3render::Bs2051Label::MPlus180,
    };
    bool standardRingOrder = region.ringRealSpeakerIndices.size()
        == expectedLabels.size();
    for (std::size_t index = 0U; index < expectedLabels.size(); ++index) {
        standardRingOrder = standardRingOrder
            && region.ringRealSpeakerIndices[index]
                == indexForLabel(expectedLabels[index]);
    }
    expect(&report, standardRingOrder, "standard-system-h-lower-ring-order");

    bool allEdgesBacked = true;
    for (std::size_t index = 0U; index < region.triangles.size(); ++index) {
        const auto &triangle = region.triangles[index];
        const auto &first = region.ringRealSpeakerIndices[index];
        const auto &second = region.ringRealSpeakerIndices[
            (index + 1U) % region.ringRealSpeakerIndices.size()];
        allEdgesBacked = allEdgesBacked
            && triangle.firstRealSpeakerIndex == first
            && triangle.secondRealSpeakerIndex == second
            && triangle.sourceFacetIndex < catalog.facets.size()
            && catalog.facets[triangle.sourceFacetIndex].kind
                == eac3render::Bs2127FacetKind::VirtualHullFacet;
    }
    expect(&report, allEdgesBacked, "closed-ring-raw-facet-adjacency");

    const auto lower = eac3render::renderLowerVirtualNgon(
        catalog, region,
        catalog.pointVectors[eac3render::kBs2127VirtualBottomIndex]);
    bool lowerEqual = lower.status == eac3render::Bs2127VirtualNgonStatus::Selected
        && close(lower.virtualGain, 1.0)
        && close(power(lower.gains), 1.0)
        && allFinite(lower.gains);
    for (std::size_t speaker = 0U; speaker < lower.gains.size(); ++speaker) {
        const bool shouldBeRing = std::find(region.ringRealSpeakerIndices.begin(),
                                            region.ringRealSpeakerIndices.end(),
                                            speaker)
            != region.ringRealSpeakerIndices.end();
        lowerEqual = lowerEqual
            && (shouldBeRing ? close(lower.gains[speaker],
                                     region.downmixCoefficient)
                             : close(lower.gains[speaker], 0.0));
    }
    expect(&report, lowerEqual, "lower-pole-equal-power-downmix");
    const std::size_t mPlus135 = indexForLabel(eac3render::Bs2051Label::MPlus135);
    const std::size_t mMinus135 = indexForLabel(eac3render::Bs2051Label::MMinus135);
    const std::size_t bPlus045 = indexForLabel(eac3render::Bs2051Label::BPlus045);
    const std::size_t bMinus045 = indexForLabel(eac3render::Bs2051Label::BMinus045);
    expect(&report, mPlus135 < lower.gains.size()
                        && mMinus135 < lower.gains.size()
                        && bPlus045 < lower.gains.size()
                        && bMinus045 < lower.gains.size()
                        && close(lower.gains[mPlus135], lower.gains[mMinus135])
                        && close(lower.gains[bPlus045], lower.gains[bMinus045]),
           "lower-pole-left-right-symmetry");

    bool everyEdgeSelected = true;
    for (const auto &triangle : region.triangles) {
        const auto source = normalizedSum(
            catalog.pointVectors[eac3render::kBs2127VirtualBottomIndex],
            catalog.pointVectors[triangle.firstRealSpeakerIndex],
            catalog.pointVectors[triangle.secondRealSpeakerIndex]);
        const auto edgeResult = eac3render::renderLowerVirtualNgon(
            catalog, region, source);
        everyEdgeSelected = everyEdgeSelected
            && edgeResult.status == eac3render::Bs2127VirtualNgonStatus::Selected
            && close(power(edgeResult.gains), 1.0)
            && allFinite(edgeResult.gains);
    }
    expect(&report, everyEdgeSelected,
           "every-lower-ring-edge-first-valid-triangle");

    const auto firstEdgeSource = normalizedPair(
        catalog.pointVectors[region.ringRealSpeakerIndices[0]],
        catalog.pointVectors[region.ringRealSpeakerIndices[1]]);
    const auto boundary = eac3render::renderLowerVirtualNgon(
        catalog, region, firstEdgeSource);
    const double edgeGain = 1.0 / std::sqrt(2.0);
    bool boundaryExact = boundary.status
        == eac3render::Bs2127VirtualNgonStatus::Selected
        && boundary.selectedTriangleIndex == 0U
        && close(boundary.virtualGain, 0.0, 1.0e-9)
        && close(boundary.triangleGains[1], edgeGain, 1.0e-9)
        && close(boundary.triangleGains[2], edgeGain, 1.0e-9)
        && close(power(boundary.gains), 1.0);
    for (std::size_t speaker = 0U; speaker < boundary.gains.size(); ++speaker) {
        const bool isFirst = speaker == region.ringRealSpeakerIndices[0];
        const bool isSecond = speaker == region.ringRealSpeakerIndices[1];
        boundaryExact = boundaryExact
            && (isFirst || isSecond ? close(boundary.gains[speaker], edgeGain,
                                            1.0e-9)
                                    : close(boundary.gains[speaker], 0.0,
                                            1.0e-9));
    }
    expect(&report, boundaryExact,
           "ring-edge-boundary-exact-two-speaker-gains");

    const auto &ring = region.ringRealSpeakerIndices;
    const auto reflectedSource = normalizedPair(
        catalog.pointVectors[ring[0]], catalog.pointVectors[ring[1]]);
    const auto reflected = eac3render::renderLowerVirtualNgon(
        catalog, region, reflectedSource);
    bool reflectedPower = reflected.status
        == eac3render::Bs2127VirtualNgonStatus::Selected
        && close(power(reflected.gains), 1.0);
    expect(&report, reflectedPower, "lateral-symmetry-power-preserved");

    const auto nan = eac3render::renderLowerVirtualNgon(
        catalog, region,
        eac3render::UnitVector3 {
            std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0});
    expect(&report, nan.status == eac3render::Bs2127VirtualNgonStatus::InvalidInput
                        && nan.reason == "nonfinite-source-vector",
           "nan-source-invalid");

    eac3render::Bs2127LowerVirtualNgon invalidRegion;
    const auto unsupported = eac3render::renderLowerVirtualNgon(
        catalog, invalidRegion, catalog.pointVectors[0]);
    expect(&report,
           unsupported.status == eac3render::Bs2127VirtualNgonStatus::Unsupported,
           "invalid-quad-or-region-structured-unsupported");

    auto tamperedRegion = region;
    tamperedRegion.valid = true;
    tamperedRegion.triangles[0].firstRealSpeakerIndex =
        eac3render::kSystemHSpeakerCount;
    const auto tampered = eac3render::renderLowerVirtualNgon(
        catalog, tamperedRegion, catalog.pointVectors[0]);
    expect(&report,
           tampered.status == eac3render::Bs2127VirtualNgonStatus::InvalidInput
               && tampered.reason == "virtual-triangle-order-or-facet-mismatch",
           "externally-marked-valid-region-revalidated");

    auto incompleteRegion = region;
    incompleteRegion.valid = true;
    incompleteRegion.ringRealSpeakerIndices.resize(3U);
    incompleteRegion.triangles.resize(3U);
    incompleteRegion.downmixCoefficient = 1.0 / std::sqrt(3.0);
    const auto incomplete = eac3render::renderLowerVirtualNgon(
        catalog, incompleteRegion, catalog.pointVectors[0]);
    expect(&report,
           incomplete.status == eac3render::Bs2127VirtualNgonStatus::InvalidInput
               && incomplete.reason == "invalid-ring-or-triangle-count",
           "three-edge-valid-region-rejected-as-incomplete-fixed-system-h");

    return report;
}

} // namespace

int main()
{
    const SelfTestReport report = runSelfTest();
    const auto catalog = eac3render::buildSystemHBs2127Topology();
    const auto region = eac3render::buildLowerVirtualNgon(catalog);
    std::cout << "bs2127VirtualNgon ring=" << region.ringRealSpeakerIndices.size()
              << " triangles=" << region.triangles.size()
              << " Wdmx=" << region.downmixCoefficient << '\n';
    std::cout << "bs2127VirtualNgonRingLabels=";
    const auto &layout = eac3render::Bs2051SystemHLayout::systemH();
    for (std::size_t index : region.ringRealSpeakerIndices) {
        std::cout << eac3render::bs2051LabelName(layout[index].label) << ' ';
    }
    std::cout << '\n';
    std::cout << "bs2127VirtualNgonSelfTest="
              << (report.pass ? "PASS" : "FAIL")
              << " cases=" << report.cases
              << " reason=" << (report.pass ? "none" : report.reason) << '\n';
    std::cout << "bs2127VirtualNgonResult="
              << (report.pass ? "PASS" : "FAIL")
              << " evidenceLimit=lower-virtual-ngon-primitive-only\n";
    return report.pass ? 0 : 1;
}
