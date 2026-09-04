#include "bs2127-nominal-to-actual.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct Report {
    bool pass = true;
    std::size_t cases = 0U;
    std::string reason;
};

void expect(Report *report, bool condition, const char *name)
{
    ++report->cases;
    if (!condition && report->pass) {
        report->pass = false;
        report->reason = name;
    }
}

double dot(const eac3render::UnitVector3 &left,
           const eac3render::UnitVector3 &right)
{
    return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
}

eac3render::UnitVector3 normalize(eac3render::UnitVector3 vector)
{
    const double length = std::sqrt(dot(vector, vector));
    for (double &component : vector) {
        component /= length;
    }
    return vector;
}

eac3render::UnitVector3 direction(double azimuth, double elevation)
{
    constexpr double kPi = 3.1415926535897932384626433832795;
    const double azimuthRadians = azimuth * kPi / 180.0;
    const double elevationRadians = elevation * kPi / 180.0;
    const double horizontal = std::cos(elevationRadians);
    return {horizontal * std::cos(azimuthRadians),
            horizontal * std::sin(azimuthRadians),
            std::sin(elevationRadians)};
}

std::vector<eac3render::Bs2127RealLoudspeaker> nominalRecords()
{
    std::vector<eac3render::Bs2127RealLoudspeaker> records;
    for (const auto &speaker : eac3render::Bs2051SystemHLayout::systemH()) {
        records.push_back({speaker.label, speaker.unitVector()});
    }
    return records;
}

double power(const std::array<double, eac3render::kSystemHSpeakerCount> &gains)
{
    double value = 0.0;
    for (double gain : gains) {
        value += gain * gain;
    }
    return value;
}

bool sameResult(const eac3render::Bs2127PointSourcePannerResult &left,
                const eac3render::Bs2127PointSourcePannerResult &right)
{
    return left.status == right.status && left.reason == right.reason
        && left.region == right.region
        && left.selectedFacetIndex == right.selectedFacetIndex
        && left.selectedTriangleIndex == right.selectedTriangleIndex
        && std::equal(left.gains.begin(), left.gains.end(), right.gains.begin(),
                      [](double a, double b) { return a == b; });
}

Report runSelfTest()
{
    Report report;
    const auto records = nominalRecords();
    const eac3render::Bs2127SystemHNominalToActualPanner identity;
    expect(&report, identity.valid() && identity.reason() == "none",
           "nominal-layout-valid");
    const eac3render::Bs2127SystemHPointSourcePanner nominal;
    bool identityExact = true;
    for (const auto &speaker : eac3render::Bs2051SystemHLayout::systemH()) {
        identityExact = identityExact
            && sameResult(identity.render(speaker.unitVector()),
                          nominal.render(speaker.unitVector()));
    }
    expect(&report, identityExact, "nominal-equals-actual-strict-identity");

    std::vector<eac3render::Bs2127RealLoudspeaker> reversed = records;
    std::reverse(reversed.begin(), reversed.end());
    const eac3render::Bs2127SystemHNominalToActualPanner permutation(reversed);
    const auto source = normalize(eac3render::UnitVector3 {0.37, 0.51, 0.77});
    expect(&report, permutation.valid()
                        && sameResult(identity.render(source),
                                       permutation.render(source)),
           "label-keyed-layout-permutation");

    auto displacedRecords = records;
    displacedRecords[0].unitVector = direction(55.0, 2.0);
    const eac3render::Bs2127SystemHNominalToActualPanner displaced(
        displacedRecords);
    expect(&report, displaced.valid(), "allowed-single-speaker-displacement");
    const auto &catalog = nominal.catalog();
    std::vector<std::size_t> facetIndices;
    for (const auto &facet : catalog.facets) {
        if ((facet.kind == eac3render::Bs2127FacetKind::Triplet
             || facet.kind == eac3render::Bs2127FacetKind::Quad)
            && std::find(facet.realSpeakerIndices.begin(),
                         facet.realSpeakerIndices.end(), 0U)
                   != facet.realSpeakerIndices.end()) {
            const auto &indices = facet.kind == eac3render::Bs2127FacetKind::Triplet
                ? facet.realSpeakerIndices : facet.regionSpeakerIndices;
            if (indices.size() >= 2U) {
                facetIndices = indices;
                break;
            }
        }
    }
    expect(&report, !facetIndices.empty(), "displacement-test-facet-found");
    std::vector<eac3render::UnitVector3> boundaryVectors;
    for (std::size_t index : facetIndices) {
        boundaryVectors.push_back(catalog.pointVectors[index]);
    }
    eac3render::UnitVector3 facetSum {0.0, 0.0, 0.0};
    for (const auto &vector : boundaryVectors) {
        for (std::size_t component = 0U; component < 3U; ++component) {
            facetSum[component] += vector[component];
        }
    }
    const auto facetSource = normalize(facetSum);
    const auto nominalFacetResult = nominal.render(facetSource);
    const auto adaptedResult = displaced.render(facetSource);
    expect(&report, adaptedResult.status
                        == eac3render::Bs2127PointSourcePannerStatus::Selected
                    && std::isfinite(power(adaptedResult.gains))
                    && std::abs(power(adaptedResult.gains) - 1.0) < 1.0e-8
                    && nominalFacetResult.status
                        == eac3render::Bs2127PointSourcePannerStatus::Selected
                    && !sameResult(adaptedResult, nominalFacetResult),
           "displaced-layout-finite-power");
    expect(&report, sameResult(adaptedResult, displaced.render(facetSource)),
           "displaced-layout-repeatable");

    const auto displacedNominalDirection = records[0].unitVector;
    const auto displacedAtNominal = displaced.render(displacedNominalDirection);
    const auto displacedMinus = displaced.render(direction(60.0 - 1.0e-3, 0.0));
    const auto displacedPlus = displaced.render(direction(60.0 + 1.0e-3, 0.0));
    const auto validFinitePower = [](const auto &result) {
        return result.status
                == eac3render::Bs2127PointSourcePannerStatus::Selected
            && result.region != eac3render::Bs2127PointSourceRegionKind::Identity
            && std::all_of(result.gains.begin(), result.gains.end(),
                           [](double gain) { return std::isfinite(gain); })
            && std::abs(power(result.gains) - 1.0) < 1.0e-8;
    };
    expect(&report, validFinitePower(displacedAtNominal)
                        && validFinitePower(displacedMinus)
                        && validFinitePower(displacedPlus),
           "displaced-nominal-direction-neighborhood-no-identity-spike");
    expect(&report, sameResult(displacedAtNominal,
                               displaced.render(displacedNominalDirection)),
           "displaced-nominal-direction-repeatable");

    const auto boundarySource = normalize(eac3render::UnitVector3 {
        catalog.pointVectors[0][0] + catalog.pointVectors[1][0],
        catalog.pointVectors[0][1] + catalog.pointVectors[1][1],
        catalog.pointVectors[0][2] + catalog.pointVectors[1][2]});
    const auto boundaryResult = displaced.render(boundarySource);
    expect(&report, boundaryResult.status
                        == eac3render::Bs2127PointSourcePannerStatus::Selected
                    && std::abs(power(boundaryResult.gains) - 1.0) < 1.0e-8,
           "displaced-layout-boundary-continuity");

    const auto lowerResult = displaced.render(
        catalog.pointVectors[eac3render::kBs2127VirtualBottomIndex]);
    expect(&report, lowerResult.status
                        == eac3render::Bs2127PointSourcePannerStatus::Selected
                    && lowerResult.region
                        == eac3render::Bs2127PointSourceRegionKind::VirtualNgon
                    && std::abs(power(lowerResult.gains) - 1.0) < 1.0e-8,
           "displaced-lower-virtual-ngon");

    auto missing = records;
    missing.pop_back();
    expect(&report,
           !eac3render::Bs2127SystemHNominalToActualPanner(missing).valid(),
           "missing-layout-fails-closed");
    auto duplicateLabel = records;
    duplicateLabel[1].label = duplicateLabel[0].label;
    expect(&report, !eac3render::Bs2127SystemHNominalToActualPanner(
                                      duplicateLabel).valid(),
           "duplicate-label-fails-closed");
    auto invalidRange = records;
    invalidRange[0].unitVector = direction(40.0, 0.0);
    expect(&report, !eac3render::Bs2127SystemHNominalToActualPanner(
                                      invalidRange).valid(),
           "out-of-range-layout-fails-closed");
    auto nonUnit = records;
    nonUnit[0].unitVector = {2.0, 0.0, 0.0};
    expect(&report, !eac3render::Bs2127SystemHNominalToActualPanner(nonUnit).valid(),
           "non-unit-layout-fails-closed");

    std::cout << "bs2127NominalToActualSelfTest="
              << (report.pass ? "PASS" : "FAIL")
              << " cases=" << report.cases
              << " reason=" << (report.reason.empty() ? "none" : report.reason)
              << " topology=system-h-only nominalRealIdentity=STRICT\n";
    return report;
}

} // namespace

int main()
{
    const Report report = runSelfTest();
    return report.pass ? 0 : 1;
}
