#include "bs2127-system-h-configured-panner.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

struct Report { bool pass = true; std::size_t cases = 0U; std::string reason; };
void expect(Report *report, bool condition, const char *name)
{
    ++report->cases;
    if (!condition && report->pass) { report->pass = false; report->reason = name; }
}

double power(const std::array<double, eac3render::kSystemHSpeakerCount> &gains)
{
    double value = 0.0;
    for (double gain : gains) value += gain * gain;
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

bool selectedFinitePower(const eac3render::Bs2127PointSourcePannerResult &result)
{
    return result.status == eac3render::Bs2127PointSourcePannerStatus::Selected
        && std::all_of(result.gains.begin(), result.gains.end(),
                       [](double value) { return std::isfinite(value); })
        && std::abs(power(result.gains) - 1.0) < 1.0e-8;
}

Report runSelfTest()
{
    Report report;
    const eac3render::Bs2127SystemHConfiguredPanner panner;
    expect(&report, panner.valid() && panner.reason() == "none",
           "configured-system-h-valid");
    expect(&report, panner.catalog().pointCount
                        == eac3render::kBs2127SystemHConfiguredPointCount,
           "configured-point-count-28");
    std::vector<eac3render::Bs2127RealLoudspeaker> nominalRecords;
    for (const auto &speaker : eac3render::Bs2051SystemHLayout::systemH()) {
        nominalRecords.push_back({speaker.label, speaker.unitVector()});
    }
    auto reversedRecords = nominalRecords;
    std::reverse(reversedRecords.begin(), reversedRecords.end());
    const eac3render::Bs2127SystemHConfiguredPanner reversedPanner(
        reversedRecords);
    expect(&report, reversedPanner.valid()
                        && sameResult(panner.render({1.0, 0.0, 0.0}),
                                      reversedPanner.render({1.0, 0.0, 0.0})),
           "label-keyed-configured-permutation");
    const std::array<std::size_t, eac3render::kBs2127SystemHDirectVirtualCount>
        targets {{3U, 4U, 7U, 8U, 9U}};
    bool directSet = true;
    for (std::size_t index = 0U; index < targets.size(); ++index) {
        const auto &direct = panner.directVirtuals()[index];
        directSet = directSet && direct.topologyIndex
                == eac3render::kSystemHSpeakerCount + index
            && direct.targetSpeakerIndex == targets[index]
            && direct.sourceLabel
                == eac3render::Bs2051SystemHLayout::systemH()[targets[index]].label
            && panner.catalog().virtualFlags[direct.topologyIndex];
    }
    expect(&report, directSet, "exact-five-direct-virtual-set");
    expect(&report, panner.lowerRingPointIndices().size() == 8U,
           "configured-lower-ring-size");
    const std::array<std::size_t, 3> physicalLower {{19U, 20U, 21U}};
    bool ringHasDirect = true;
    for (const auto &direct : panner.directVirtuals()) {
        ringHasDirect = ringHasDirect
            && std::find(panner.lowerRingPointIndices().begin(),
                         panner.lowerRingPointIndices().end(),
                         direct.topologyIndex)
                != panner.lowerRingPointIndices().end();
    }
    for (std::size_t index : physicalLower) {
        ringHasDirect = ringHasDirect
            && std::find(panner.lowerRingPointIndices().begin(),
                         panner.lowerRingPointIndices().end(), index)
                != panner.lowerRingPointIndices().end();
    }
    expect(&report, ringHasDirect, "direct-virtuals-in-lower-ring");
    std::size_t maximumFacetSize = 0U;
    for (const auto &facet : panner.catalog().facets) {
        maximumFacetSize = std::max(maximumFacetSize, facet.vertexIndices.size());
    }
    expect(&report, maximumFacetSize <= 4U, "configured-facets-at-most-four-edges");

    bool physicalIdentity = true;
    for (const auto &speaker : eac3render::Bs2051SystemHLayout::systemH()) {
        const auto result = panner.render(speaker.unitVector());
        physicalIdentity = physicalIdentity
            && result.status == eac3render::Bs2127PointSourcePannerStatus::Selected
            && result.region == eac3render::Bs2127PointSourceRegionKind::Identity
            && result.gains[static_cast<std::size_t>(
                &speaker - eac3render::Bs2051SystemHLayout::systemH().data())]
                == 1.0;
    }
    expect(&report, physicalIdentity, "physical-speaker-identities");

    bool directIdentity = true;
    for (std::size_t index = 0U; index < targets.size(); ++index) {
        const auto result = panner.render(panner.directVirtuals()[index].nominalVector);
        directIdentity = directIdentity && selectedFinitePower(result)
            && std::abs(result.gains[targets[index]] - 1.0) < 1.0e-8;
    }
    expect(&report, directIdentity, "direct-virtual-one-to-one-downmix");

    // Independently reconstruct the first-valid ordinary-facet decision from
    // the catalog and solve primitives.  This deliberately does not call
    // render(), so the dispatcher cannot prove its own ordering assertion.
    const auto earliestOrdinaryFacet =
        [&panner](const eac3render::UnitVector3 &source) {
            const auto noFacet = std::numeric_limits<std::size_t>::max();
            for (std::size_t facetIndex = 0U;
                 facetIndex < panner.catalog().facets.size(); ++facetIndex) {
                const auto &facet = panner.catalog().facets[facetIndex];
                if (facet.kind != eac3render::Bs2127FacetKind::Triplet
                    && facet.kind != eac3render::Bs2127FacetKind::Quad
                    && facet.kind
                        != eac3render::Bs2127FacetKind::VirtualHullFacet) {
                    continue;
                }
                if (std::find(facet.vertexIndices.begin(),
                              facet.vertexIndices.end(),
                              eac3render::kBs2127SystemHConfiguredLowerIndex)
                        != facet.vertexIndices.end()) {
                    continue;
                }
                if (facet.vertexIndices.size() == 3U) {
                    const std::array<eac3render::UnitVector3, 3> vectors {
                        panner.catalog().pointVectors[facet.vertexIndices[0]],
                        panner.catalog().pointVectors[facet.vertexIndices[1]],
                        panner.catalog().pointVectors[facet.vertexIndices[2]],
                    };
                    if (eac3render::solveTriplet(
                            vectors, source,
                            panner.catalog().options.tripletOptions)
                            .accepted) {
                        return facetIndex;
                    }
                } else if (facet.vertexIndices.size() == 4U
                           && facet.regionSpeakerIndices.size() == 4U) {
                    std::array<eac3render::UnitVector3, 4> vectors {};
                    for (std::size_t index = 0U; index < 4U; ++index) {
                        vectors[index] = panner.catalog().pointVectors[
                            facet.regionSpeakerIndices[index]];
                    }
                    if (eac3render::solveQuad(
                            vectors, source,
                            panner.catalog().options.quadOptions)
                            .accepted) {
                        return facetIndex;
                    }
                }
            }
            return noFacet;
        };
    const auto normalize = [](eac3render::UnitVector3 vector) {
        const double norm = std::sqrt(vector[0] * vector[0]
                                      + vector[1] * vector[1]
                                      + vector[2] * vector[2]);
        return eac3render::UnitVector3 {vector[0] / norm, vector[1] / norm,
                                        vector[2] / norm};
    };
    const auto directSource = panner.directVirtuals()[0].nominalVector;
    const auto directExpected = earliestOrdinaryFacet(directSource);
    const auto directSelected = panner.render(directSource);
    expect(&report, directExpected != std::numeric_limits<std::size_t>::max()
                        && directSelected.selectedFacetIndex == directExpected,
           "catalog-order-direct-first-valid-independent");
    std::size_t firstDirectFacet = std::numeric_limits<std::size_t>::max();
    std::size_t laterRealFacet = std::numeric_limits<std::size_t>::max();
    for (std::size_t facetIndex = 0U;
         facetIndex < panner.catalog().facets.size(); ++facetIndex) {
        const auto &facet = panner.catalog().facets[facetIndex];
        const bool hasLower = std::find(
            facet.vertexIndices.begin(), facet.vertexIndices.end(),
            eac3render::kBs2127SystemHConfiguredLowerIndex)
            != facet.vertexIndices.end();
        const bool hasDirect = std::any_of(
            facet.vertexIndices.begin(), facet.vertexIndices.end(),
            [](std::size_t index) {
                return index >= eac3render::kSystemHSpeakerCount
                    && index < eac3render::kBs2127SystemHConfiguredLowerIndex;
            });
        const bool pureReal = facet.kind == eac3render::Bs2127FacetKind::Triplet
            || facet.kind == eac3render::Bs2127FacetKind::Quad;
        if (!hasLower && hasDirect
            && firstDirectFacet == std::numeric_limits<std::size_t>::max()) {
            firstDirectFacet = facetIndex;
        } else if (!hasLower && pureReal && firstDirectFacet !=
                       std::numeric_limits<std::size_t>::max()
                   && facetIndex > firstDirectFacet) {
            laterRealFacet = facetIndex;
            break;
        }
    }
    expect(&report, firstDirectFacet != std::numeric_limits<std::size_t>::max()
                        && laterRealFacet != std::numeric_limits<std::size_t>::max()
                        && firstDirectFacet < laterRealFacet,
           "direct-facet-precedes-later-real-facet-observed");
    if (firstDirectFacet != std::numeric_limits<std::size_t>::max()) {
        eac3render::UnitVector3 directFacetSource {0.0, 0.0, 0.0};
        for (std::size_t index : panner.catalog().facets[firstDirectFacet]
                                     .vertexIndices) {
            for (std::size_t axis = 0U; axis < 3U; ++axis) {
                directFacetSource[axis]
                    += panner.catalog().pointVectors[index][axis];
            }
        }
        directFacetSource = normalize(directFacetSource);
        const auto expected = earliestOrdinaryFacet(directFacetSource);
        const auto selected = panner.render(directFacetSource);
        expect(&report, expected == firstDirectFacet
                            && selected.selectedFacetIndex == expected,
               "catalog-order-direct-facet-interior-independent");
    }
    std::size_t firstPureFacet = std::numeric_limits<std::size_t>::max();
    eac3render::UnitVector3 pureSource {0.0, 0.0, 0.0};
    for (std::size_t facetIndex = 0U;
         facetIndex < panner.catalog().facets.size(); ++facetIndex) {
        const auto &facet = panner.catalog().facets[facetIndex];
        if (facet.kind == eac3render::Bs2127FacetKind::Triplet
            && facet.vertexIndices.size() == 3U) {
            for (std::size_t index : facet.vertexIndices) {
                for (std::size_t axis = 0U; axis < 3U; ++axis) {
                    pureSource[axis] += panner.catalog().pointVectors[index][axis];
                }
            }
            firstPureFacet = facetIndex;
            break;
        }
    }
    expect(&report, firstPureFacet != std::numeric_limits<std::size_t>::max(),
           "pure-real-representative-found");
    if (firstPureFacet != std::numeric_limits<std::size_t>::max()) {
        pureSource = normalize(pureSource);
        const auto expected = earliestOrdinaryFacet(pureSource);
        const auto selected = panner.render(pureSource);
        expect(&report, expected != std::numeric_limits<std::size_t>::max()
                            && selected.selectedFacetIndex == expected,
               "catalog-order-real-first-valid-independent");
    }
    std::cout << "ordinaryFirstValidChecks directExpected=" << directExpected
              << " directSelected=" << directSelected.selectedFacetIndex
              << " firstDirectFacet=" << firstDirectFacet
              << " laterRealFacet=" << laterRealFacet << '\n';

    const auto lower = panner.render({0.0, 0.0, -1.0});
    expect(&report, selectedFinitePower(lower)
                        && lower.region
                            == eac3render::Bs2127PointSourceRegionKind::VirtualNgon,
           "generic-lower-pole-virtual-ngon");
    const auto source = eac3render::UnitVector3 {0.31, 0.47, 0.825};
    const auto sourceNorm = std::sqrt(source[0] * source[0] + source[1] * source[1]
                                      + source[2] * source[2]);
    const eac3render::UnitVector3 unit {source[0] / sourceNorm,
                                        source[1] / sourceNorm,
                                        source[2] / sourceNorm};
    const auto first = panner.render(unit);
    const auto second = panner.render(unit);
    expect(&report, selectedFinitePower(first) && sameResult(first, second),
           "deterministic-finite-normalized-render");

    auto displaced = std::vector<eac3render::Bs2127RealLoudspeaker> {};
    for (const auto &speaker : eac3render::Bs2051SystemHLayout::systemH()) {
        displaced.push_back({speaker.label, speaker.unitVector()});
    }
    auto direction = [](double azimuth, double elevation) {
        constexpr double pi = 3.14159265358979323846;
        const double a = azimuth * pi / 180.0;
        const double e = elevation * pi / 180.0;
        return eac3render::UnitVector3 {std::cos(e) * std::cos(a),
                                        std::cos(e) * std::sin(a), std::sin(e)};
    };
    displaced[3].unitVector = direction(120.0, 5.0);
    displaced[19].unitVector = direction(0.0, -20.0);
    displaced[20].unitVector = direction(50.0, -20.0);
    displaced[21].unitVector = direction(-50.0, -20.0);
    const eac3render::Bs2127SystemHConfiguredPanner displacedPanner(displaced);
    expect(&report, displacedPanner.valid(), "displaced-layout-valid");
    expect(&report, std::abs(std::atan2(
        displacedPanner.directVirtuals()[0].realVector[1],
        displacedPanner.directVirtuals()[0].realVector[0])
        - 120.0 * 3.14159265358979323846 / 180.0) < 1.0e-8
        && std::abs(std::atan2(displacedPanner.directVirtuals()[0].realVector[2],
                               std::hypot(displacedPanner.directVirtuals()[0].realVector[0],
                                          displacedPanner.directVirtuals()[0].realVector[1]))
                    + 20.0 * 3.14159265358979323846 / 180.0) < 1.0e-8,
           "direct-real-azimuth-and-mean-elevation");

    auto missing = displaced;
    missing.pop_back();
    expect(&report, !eac3render::Bs2127SystemHConfiguredPanner(missing).valid(),
           "invalid-layout-fails-closed");
    const auto nan = panner.render({std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0});
    expect(&report, nan.status == eac3render::Bs2127PointSourcePannerStatus::InvalidInput,
           "nonfinite-source-fails-closed");

    std::size_t unsupported = 0U;
    std::size_t invalid = 0U;
    bool allFinite = true;
    bool repeatable = true;
    std::size_t firstUnsupportedIndex = std::numeric_limits<std::size_t>::max();
    std::string firstUnsupportedReason;
    constexpr std::size_t sampleCount = 32768U;
    constexpr double golden = 2.39996322972865332223;
    constexpr double pi = 3.14159265358979323846;
    for (std::size_t index = 0U; index < sampleCount; ++index) {
        const double z = 1.0 - 2.0 * (static_cast<double>(index) + 0.5)
            / static_cast<double>(sampleCount);
        const double radius = std::sqrt(std::max(0.0, 1.0 - z * z));
        const eac3render::UnitVector3 sample {
            radius * std::cos(golden * static_cast<double>(index)),
            radius * std::sin(golden * static_cast<double>(index)), z};
        const auto one = panner.render(sample);
        const auto two = panner.render(sample);
        repeatable = repeatable && sameResult(one, two);
        if (one.status == eac3render::Bs2127PointSourcePannerStatus::Unsupported) {
            ++unsupported;
            if (firstUnsupportedIndex == std::numeric_limits<std::size_t>::max()) {
                firstUnsupportedIndex = index;
                firstUnsupportedReason = one.reason;
            }
        } else if (one.status != eac3render::Bs2127PointSourcePannerStatus::Selected) {
            ++invalid;
        } else {
            allFinite = allFinite && selectedFinitePower(one);
        }
    }
    expect(&report, unsupported == 0U && invalid == 0U && allFinite && repeatable,
           "global-32768-sample-coverage");
    std::cout << "bs2127ConfiguredCatalog pointCount=" << panner.catalog().pointCount
              << " facets=" << panner.catalog().facets.size()
              << " lowerRing=" << panner.lowerRingPointIndices().size()
              << " directVirtuals=" << panner.directVirtuals().size()
              << " maxFacetVertices=" << maximumFacetSize
              << " samples=" << sampleCount << " unsupported=" << unsupported
              << " invalid=" << invalid
              << " firstUnsupported=" << (firstUnsupportedIndex
                                             == std::numeric_limits<std::size_t>::max()
                                             ? "none"
                                             : std::to_string(firstUnsupportedIndex))
              << " firstReason=" << (firstUnsupportedReason.empty()
                                      ? "none" : firstUnsupportedReason)
              << '\n';
    std::cout << "bs2127SystemHConfiguredPannerSelfTest="
              << (report.pass ? "PASS" : "FAIL") << " cases=" << report.cases
              << " reason=" << (report.reason.empty() ? "none" : report.reason)
              << " directVirtualPolicy=R0D\n";
    return report;
}

} // namespace

int main()
{
    const Report report = runSelfTest();
    return report.pass ? 0 : 1;
}
