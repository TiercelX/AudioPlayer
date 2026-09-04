#include "bs2127-selector.h"

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

bool close(double left, double right, double tolerance = 1.0e-9)
{
    return std::abs(left - right) <= tolerance;
}

double dot(const eac3render::UnitVector3 &left,
           const eac3render::UnitVector3 &right)
{
    return left[0] * right[0]
        + left[1] * right[1]
        + left[2] * right[2];
}

double power(const std::array<double, eac3render::kSystemHSpeakerCount> &gains)
{
    double result = 0.0;
    for (double gain : gains) {
        result += gain * gain;
    }
    return result;
}

double power4(const std::array<double, 4> &gains)
{
    double result = 0.0;
    for (double gain : gains) {
        result += gain * gain;
    }
    return result;
}

eac3render::UnitVector3 normalizedSum(
    const eac3render::UnitVector3 &left,
    const eac3render::UnitVector3 &right)
{
    eac3render::UnitVector3 result {
        left[0] + right[0], left[1] + right[1], left[2] + right[2],
    };
    const double norm = std::sqrt(
        result[0] * result[0] + result[1] * result[1] + result[2] * result[2]);
    for (double &component : result) {
        component /= norm;
    }
    return result;
}

eac3render::UnitVector3 normalizedSum4(
    const std::array<eac3render::UnitVector3, 4> &vectors)
{
    eac3render::UnitVector3 result {0.0, 0.0, 0.0};
    for (const auto &vector : vectors) {
        for (std::size_t component = 0U; component < 3U; ++component) {
            result[component] += vector[component];
        }
    }
    const double norm = std::sqrt(dot(result, result));
    for (double &component : result) {
        component /= norm;
    }
    return result;
}

SelfTestReport runSelfTest()
{
    SelfTestReport report;
    const auto catalog = eac3render::buildSystemHBs2127Topology();
    expect(&report, catalog.valid
                    && catalog.totalTriplesEnumerated == 1771U
                    && catalog.facets.size() > 0U,
           "topology-catalog-built-from-23-points");
    expect(&report, catalog.pointVectors[eac3render::kBs2127VirtualBottomIndex]
                        == eac3render::UnitVector3 {0.0, 0.0, -1.0}
                    && catalog.virtualFlags[eac3render::kBs2127VirtualBottomIndex]
                    && !catalog.virtualFlags[13U],
           "forced-bottom-only-virtual-rule");

    bool allSupporting = true;
    bool noDuplicateVertexSets = true;
    bool stableRealIndices = true;
    bool hasTriplet = false;
    bool hasQuad = false;
    bool hasVirtualHullFacet = false;
    bool tripletGeometryValid = true;
    for (std::size_t first = 0U; first < catalog.facets.size(); ++first) {
        const auto &facet = catalog.facets[first];
        for (std::size_t point = 0U; point < catalog.pointVectors.size(); ++point) {
            const double signedDistance =
                dot(catalog.pointVectors[point], facet.outwardNormal)
                - facet.planeOffset;
            allSupporting = allSupporting
                && signedDistance <= catalog.options.planeTolerance * 2.0;
        }
        stableRealIndices = stableRealIndices
            && std::is_sorted(facet.vertexIndices.begin(), facet.vertexIndices.end())
            && std::is_sorted(facet.realSpeakerIndices.begin(),
                              facet.realSpeakerIndices.end());
        for (std::size_t second = first + 1U; second < catalog.facets.size(); ++second) {
            noDuplicateVertexSets = noDuplicateVertexSets
                && facet.vertexIndices != catalog.facets[second].vertexIndices;
        }
        hasTriplet = hasTriplet || facet.kind == eac3render::Bs2127FacetKind::Triplet;
        hasQuad = hasQuad || facet.kind == eac3render::Bs2127FacetKind::Quad;
        hasVirtualHullFacet = hasVirtualHullFacet
            || facet.kind == eac3render::Bs2127FacetKind::VirtualHullFacet;
        if (facet.kind == eac3render::Bs2127FacetKind::Triplet) {
            tripletGeometryValid = tripletGeometryValid
                && std::isfinite(facet.determinant)
                && std::abs(facet.determinant)
                    > catalog.options.tripletOptions.determinantTolerance
                && std::isfinite(facet.conditionEstimate)
                && facet.conditionEstimate
                    <= catalog.options.maximumConditionEstimate;
        }
    }
    expect(&report, allSupporting, "all-facets-supporting-planes");
    expect(&report, noDuplicateVertexSets, "no-duplicate-facets-after-merge");
    expect(&report, stableRealIndices, "stable-sorted-facet-indices");
    expect(&report, hasTriplet && hasQuad && hasVirtualHullFacet,
           "facet-kinds-triplet-quad-virtual-hull-facet");
    expect(&report, tripletGeometryValid,
           "triplet-geometry-determinant-condition-explicit");

    std::size_t quadInteriorCount = 0U;
    bool quadOrderingValid = true;
    for (const auto &facet : catalog.facets) {
        if (facet.kind != eac3render::Bs2127FacetKind::Quad) {
            continue;
        }
        quadOrderingValid = quadOrderingValid
            && facet.regionSpeakerIndices.size() == 4U
            && std::is_permutation(facet.regionSpeakerIndices.begin(),
                                   facet.regionSpeakerIndices.end(),
                                   facet.realSpeakerIndices.begin());
        if (facet.regionSpeakerIndices.size() != 4U) {
            continue;
        }
        std::array<eac3render::UnitVector3, 4> vectors {};
        for (std::size_t index = 0U; index < 4U; ++index) {
            vectors[index] = catalog.pointVectors[facet.regionSpeakerIndices[index]];
        }
        const auto direct = eac3render::solveQuad(vectors,
                                                   normalizedSum4(vectors));
        quadInteriorCount += direct.accepted ? 1U : 0U;
        quadOrderingValid = quadOrderingValid && direct.accepted
            && close(power4(direct.normalizedGains), 1.0);
        const auto selected = eac3render::selectSystemHPointSource(
            catalog, normalizedSum4(vectors));
        quadOrderingValid = quadOrderingValid
            && selected.status == eac3render::Bs2127SelectionStatus::Selected
            && selected.selectedKind == eac3render::Bs2127FacetKind::Quad;
    }
    expect(&report, quadInteriorCount == 4U,
           "all-system-h-quad-interiors-solve");
    expect(&report, quadOrderingValid,
           "quad-perimeter-order-selection-and-power-normalization");

    const double elevation = 30.0 * 3.14159265358979323846 / 180.0;
    const double horizontal = std::cos(elevation);
    const auto syntheticQuad = std::array<eac3render::UnitVector3, 4> {
        eac3render::UnitVector3 {horizontal * std::cos(-45.0 * 3.14159265358979323846 / 180.0),
                                 horizontal * std::sin(-45.0 * 3.14159265358979323846 / 180.0),
                                 std::sin(elevation)},
        eac3render::UnitVector3 {horizontal * std::cos(45.0 * 3.14159265358979323846 / 180.0),
                                 horizontal * std::sin(45.0 * 3.14159265358979323846 / 180.0),
                                 std::sin(elevation)},
        eac3render::UnitVector3 {horizontal * std::cos(135.0 * 3.14159265358979323846 / 180.0),
                                 horizontal * std::sin(135.0 * 3.14159265358979323846 / 180.0),
                                 std::sin(elevation)},
        eac3render::UnitVector3 {horizontal * std::cos(225.0 * 3.14159265358979323846 / 180.0),
                                 horizontal * std::sin(225.0 * 3.14159265358979323846 / 180.0),
                                 std::sin(elevation)},
    };
    const auto syntheticCentre = normalizedSum4(syntheticQuad);
    const auto syntheticCentreSolve = eac3render::solveQuad(
        syntheticQuad, syntheticCentre);
    expect(&report, syntheticCentreSolve.accepted
                    && close(syntheticCentreSolve.x, 0.5, 1.0e-7)
                    && close(syntheticCentreSolve.y, 0.5, 1.0e-7),
           "quad-synthetic-centre-axes");
    const auto leftEdgeSource = normalizedSum(syntheticQuad[0], syntheticQuad[3]);
    const auto leftEdgeSolve = eac3render::solveQuad(syntheticQuad,
                                                      leftEdgeSource);
    expect(&report, leftEdgeSolve.accepted && close(leftEdgeSolve.x, 0.0, 1.0e-7),
           "quad-synthetic-boundary");
    const auto cornerSolve = eac3render::solveQuad(syntheticQuad,
                                                    syntheticQuad[0]);
    expect(&report, cornerSolve.accepted && close(cornerSolve.x, 0.0, 1.0e-7)
                    && close(cornerSolve.y, 0.0, 1.0e-7),
           "quad-synthetic-corner");
    const auto poleSolve = eac3render::solveQuad(
        syntheticQuad, eac3render::UnitVector3 {0.0, 0.0, -1.0});
    expect(&report, !poleSolve.accepted,
           "quad-outside-lower-pole-fail-closed");
    auto cyclicQuad = syntheticQuad;
    std::rotate(cyclicQuad.begin(), cyclicQuad.begin() + 1, cyclicQuad.end());
    const auto cyclicSolve = eac3render::solveQuad(cyclicQuad,
                                                    syntheticCentre);
    expect(&report, cyclicSolve.accepted && close(cyclicSolve.x, 0.5, 1.0e-7)
                    && close(cyclicSolve.y, 0.5, 1.0e-7),
           "quad-cyclic-layout-permutation");
    auto degenerateQuad = syntheticQuad;
    degenerateQuad[3] = degenerateQuad[0];
    const auto degenerateSolve = eac3render::solveQuad(
        degenerateQuad, syntheticCentre);
    expect(&report, !degenerateSolve.accepted,
           "quad-degenerate-facet-fail-closed");

    const auto sameCatalog = eac3render::buildSystemHBs2127Topology();
    bool sameOrder = catalog.facets.size() == sameCatalog.facets.size();
    if (sameOrder) {
        for (std::size_t index = 0U; index < catalog.facets.size(); ++index) {
            sameOrder = sameOrder
                && catalog.facets[index].kind == sameCatalog.facets[index].kind
                && catalog.facets[index].vertexIndices
                    == sameCatalog.facets[index].vertexIndices
                && catalog.facets[index].regionSpeakerIndices
                    == sameCatalog.facets[index].regionSpeakerIndices
                && close(catalog.facets[index].planeOffset,
                         sameCatalog.facets[index].planeOffset);
        }
    }
    expect(&report, sameOrder, "topology-order-deterministic");

    const auto &layout = eac3render::Bs2051SystemHLayout::systemH();
    for (std::size_t speaker = 0U; speaker < layout.size(); ++speaker) {
        const auto selected = eac3render::selectSystemHPointSource(
            catalog, layout[speaker].unitVector());
        bool identity = selected.status == eac3render::Bs2127SelectionStatus::Selected
            && selected.reason == "speaker-identity"
            && close(selected.gains[speaker], 1.0);
        for (std::size_t output = 0U; output < selected.gains.size(); ++output) {
            if (output != speaker) {
                identity = identity && close(selected.gains[output], 0.0);
            }
        }
        expect(&report, identity, "real-speaker-identity");
    }

    auto noIdentityOptions = eac3render::Bs2127TopologyOptions {};
    noIdentityOptions.identityGuardEnabled = false;
    const auto noIdentityCatalog =
        eac3render::buildSystemHBs2127Topology(noIdentityOptions);
    const auto identityWithoutGuard = eac3render::selectSystemHPointSource(
        noIdentityCatalog, layout[0].unitVector());
    expect(&report, identityWithoutGuard.reason != "speaker-identity",
           "identity-guard-can-be-disabled");

    for (const auto &facet : catalog.facets) {
        if (facet.kind != eac3render::Bs2127FacetKind::Triplet) {
            continue;
        }
        const eac3render::UnitVector3 internalSource = normalizedSum(
            catalog.pointVectors[facet.realSpeakerIndices[0]],
            normalizedSum(catalog.pointVectors[facet.realSpeakerIndices[1]],
                          catalog.pointVectors[facet.realSpeakerIndices[2]]));
        const std::array<eac3render::UnitVector3, 3> vectors {
            catalog.pointVectors[facet.realSpeakerIndices[0]],
            catalog.pointVectors[facet.realSpeakerIndices[1]],
            catalog.pointVectors[facet.realSpeakerIndices[2]],
        };
        const auto directSolve = eac3render::solveTriplet(vectors, internalSource);
        const auto selected = eac3render::selectSystemHPointSource(
            noIdentityCatalog, internalSource);
        expect(&report, directSolve.accepted
                        && selected.status
                            == eac3render::Bs2127SelectionStatus::Selected
                        && close(power(selected.gains), 1.0),
               "real-triplet-interior-selector-solve");
    }

    const auto overlapSource = normalizedSum(
        layout[0].unitVector(), layout[3].unitVector());
    const auto first = eac3render::selectSystemHPointSource(catalog, overlapSource);
    const auto second = eac3render::selectSystemHPointSource(catalog, overlapSource);
    expect(&report, first.status == second.status
                    && first.reason == second.reason
                    && first.selectedFacetIndex == second.selectedFacetIndex
                    && std::equal(first.gains.begin(), first.gains.end(),
                                  second.gains.begin(),
                                  [](double left, double right) {
                                      return close(left, right);
                                  }),
           "overlap-first-valid-output-stable");
    if (first.status == eac3render::Bs2127SelectionStatus::Selected) {
        expect(&report, close(power(first.gains), 1.0),
               "triplet-selection-power-normalized");
    } else {
        expect(&report, first.reason
                            == "no-valid-triplet-quad-or-virtual-ngon-unsupported",
               "non-triplet-region-structured-unsupported");
    }

    const auto &firstTriplet = *std::find_if(
        catalog.facets.begin(), catalog.facets.end(), [](const auto &facet) {
            return facet.kind == eac3render::Bs2127FacetKind::Triplet;
        });
    const auto boundarySource = normalizedSum(
        catalog.pointVectors[firstTriplet.realSpeakerIndices[0]],
        catalog.pointVectors[firstTriplet.realSpeakerIndices[1]]);
    const auto boundaryFirst = eac3render::selectSystemHPointSource(
        noIdentityCatalog, boundarySource);
    const auto boundarySecond = eac3render::selectSystemHPointSource(
        noIdentityCatalog, boundarySource);
    expect(&report, boundaryFirst.status == boundarySecond.status
                    && boundaryFirst.reason == boundarySecond.reason
                    && boundaryFirst.selectedFacetIndex
                        == boundarySecond.selectedFacetIndex,
           "triplet-boundary-first-valid-stable");

    const auto bottom = eac3render::selectSystemHPointSource(
        catalog, catalog.pointVectors[eac3render::kBs2127VirtualBottomIndex]);
    expect(&report, bottom.status == eac3render::Bs2127SelectionStatus::Unsupported
                    && bottom.reason
                        == "no-valid-triplet-quad-or-virtual-ngon-unsupported",
           "virtual-bottom-structured-unsupported");

    const auto nan = eac3render::selectSystemHPointSource(
        catalog, eac3render::UnitVector3 {
            std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0});
    expect(&report, nan.status == eac3render::Bs2127SelectionStatus::InvalidInput
                    && nan.reason == "nonfinite-source-vector",
           "nan-source-invalid");

    return report;
}

} // namespace

int main()
{
    const SelfTestReport report = runSelfTest();
    const auto catalog = eac3render::buildSystemHBs2127Topology();
    std::size_t tripletCount = 0U;
    std::size_t quadCount = 0U;
    std::size_t virtualHullFacetCount = 0U;
    std::size_t unsupportedNgonCount = 0U;
    for (const auto &facet : catalog.facets) {
        switch (facet.kind) {
        case eac3render::Bs2127FacetKind::Triplet: ++tripletCount; break;
        case eac3render::Bs2127FacetKind::Quad: ++quadCount; break;
        case eac3render::Bs2127FacetKind::VirtualHullFacet:
            ++virtualHullFacetCount;
            break;
        case eac3render::Bs2127FacetKind::UnsupportedNgon:
            ++unsupportedNgonCount;
            break;
        }
    }
    std::cout << "bs2127TopologyCatalog points="
              << eac3render::kBs2127TopologyPointCount
              << " facets=" << catalog.facets.size()
              << " triplet=" << tripletCount
              << " quad=" << quadCount
              << " virtualHullFacet=" << virtualHullFacetCount
              << " unsupportedNgon=" << unsupportedNgonCount
              << " rawSupportingPlanes=" << catalog.supportingPlanesFound
              << " mergedDuplicates=" << catalog.mergedPlaneDuplicates
              << '\n';
    std::cout << "bs2127TopologySelfTest=" << (report.pass ? "PASS" : "FAIL")
              << " cases=" << report.cases
              << " reason=" << (report.reason.empty() ? "none" : report.reason)
              << '\n';
    std::cout << "bs2127TopologyResult=" << (report.pass ? "PASS" : "FAIL")
              << " evidenceLimit=system-h-nominal-triplet-quad-selection\n";
    return report.pass ? 0 : 1;
}
