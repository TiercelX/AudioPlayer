#include "bs2127-point-source-panner.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

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

double dot(const eac3render::UnitVector3 &left,
           const eac3render::UnitVector3 &right)
{
    return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
}

eac3render::UnitVector3 normalize(eac3render::UnitVector3 value)
{
    const double length = std::sqrt(dot(value, value));
    for (double &component : value) {
        component /= length;
    }
    return value;
}

eac3render::UnitVector3 normalizedSum(
    const std::vector<eac3render::UnitVector3> &vectors)
{
    eac3render::UnitVector3 result {0.0, 0.0, 0.0};
    for (const auto &vector : vectors) {
        for (std::size_t component = 0U; component < 3U; ++component) {
            result[component] += vector[component];
        }
    }
    return normalize(result);
}

double power(const std::array<double, eac3render::kSystemHSpeakerCount> &gains)
{
    double result = 0.0;
    for (double gain : gains) {
        result += gain * gain;
    }
    return result;
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

SelfTestReport runSelfTest()
{
    SelfTestReport report;
    const eac3render::Bs2127SystemHPointSourcePanner panner;
    expect(&report, panner.valid() && panner.reason() == "none",
           "system-h-panner-built");

    std::size_t identityCount = 0U;
    for (const auto &speaker : eac3render::Bs2051SystemHLayout::systemH()) {
        const auto result = panner.render(speaker.unitVector());
        ++identityCount;
        expect(&report, result.status
                            == eac3render::Bs2127PointSourcePannerStatus::Selected
                        && result.region
                            == eac3render::Bs2127PointSourceRegionKind::Identity
                        && result.selectedFacetIndex == eac3render::kNoBs2127Facet
                        && power(result.gains) == 1.0,
               "all-system-h-identities-dispatched");
    }
    expect(&report, identityCount == eac3render::kSystemHSpeakerCount,
           "system-h-identity-count");

    std::size_t tripletInteriorCount = 0U;
    std::size_t quadInteriorCount = 0U;
    for (const auto &facet : panner.catalog().facets) {
        if (facet.kind != eac3render::Bs2127FacetKind::Triplet
            && facet.kind != eac3render::Bs2127FacetKind::Quad) {
            continue;
        }
        const std::vector<std::size_t> &indices =
            facet.kind == eac3render::Bs2127FacetKind::Triplet
                ? facet.realSpeakerIndices : facet.regionSpeakerIndices;
        std::vector<eac3render::UnitVector3> vectors;
        for (std::size_t index : indices) {
            vectors.push_back(panner.catalog().pointVectors[index]);
        }
        const auto result = panner.render(normalizedSum(vectors));
        if (facet.kind == eac3render::Bs2127FacetKind::Triplet) {
            ++tripletInteriorCount;
            expect(&report, result.status
                                == eac3render::Bs2127PointSourcePannerStatus::Selected
                            && result.region
                                == eac3render::Bs2127PointSourceRegionKind::Triplet,
                   "triplet-interior-dispatch");
        } else {
            ++quadInteriorCount;
            expect(&report, result.status
                                == eac3render::Bs2127PointSourcePannerStatus::Selected
                            && result.region
                                == eac3render::Bs2127PointSourceRegionKind::Quad,
                   "quad-interior-dispatch");
        }
        expect(&report, result.status
                            == eac3render::Bs2127PointSourcePannerStatus::Selected
                        && std::abs(power(result.gains) - 1.0) < 1.0e-12,
               "facet-dispatch-power-normalized");
    }
    expect(&report, tripletInteriorCount == 26U && quadInteriorCount == 4U,
           "all-nominal-facet-interiors-covered");

    const auto lowerPole = panner.render(
        panner.catalog().pointVectors[eac3render::kBs2127VirtualBottomIndex]);
    expect(&report, lowerPole.status
                        == eac3render::Bs2127PointSourcePannerStatus::Selected
                    && lowerPole.region
                        == eac3render::Bs2127PointSourceRegionKind::VirtualNgon
                    && lowerPole.selectedTriangleIndex !=
                        eac3render::kNoBs2127PointSourceTriangle
                    && std::abs(power(lowerPole.gains) - 1.0) < 1.0e-12,
           "lower-pole-virtual-ngon-dispatch");

    const std::vector<eac3render::UnitVector3> axes {
        {1.0, 0.0, 0.0}, {-1.0, 0.0, 0.0}, {0.0, 1.0, 0.0},
        {0.0, -1.0, 0.0}, {0.0, 0.0, 1.0}, {0.0, 0.0, -1.0},
    };
    bool axisPass = true;
    for (const auto &axis : axes) {
        const auto result = panner.render(axis);
        axisPass = axisPass
            && result.status == eac3render::Bs2127PointSourcePannerStatus::Selected
            && std::abs(power(result.gains) - 1.0) < 1.0e-12;
    }
    expect(&report, axisPass, "axes-and-poles-dispatched");

    bool boundaryPass = true;
    std::size_t boundaryCount = 0U;
    for (const auto &facet : panner.catalog().facets) {
        if (facet.kind != eac3render::Bs2127FacetKind::Triplet
            && facet.kind != eac3render::Bs2127FacetKind::Quad) {
            continue;
        }
        const auto &indices = facet.kind == eac3render::Bs2127FacetKind::Triplet
            ? facet.realSpeakerIndices : facet.regionSpeakerIndices;
        if (indices.size() < 2U) {
            boundaryPass = false;
            continue;
        }
        const auto source = normalizedSum(std::vector<eac3render::UnitVector3> {
            panner.catalog().pointVectors[indices[0]],
            panner.catalog().pointVectors[indices[1]],
        });
        const auto result = panner.render(source);
        ++boundaryCount;
        boundaryPass = boundaryPass
            && result.status
                == eac3render::Bs2127PointSourcePannerStatus::Selected
            && std::abs(power(result.gains) - 1.0) < 1.0e-8;
    }
    expect(&report, boundaryCount == 30U && boundaryPass,
           "triplet-quad-boundaries-dispatched");

    constexpr std::size_t kSampleCount = 32768U;
    constexpr double kPi = 3.1415926535897932384626433832795;
    constexpr double kGoldenAngle = 2.39996322972865332223;
    std::size_t unsupportedCount = 0U;
    std::size_t invalidCount = 0U;
    std::size_t sampledTriplet = 0U;
    std::size_t sampledQuad = 0U;
    std::size_t sampledVirtual = 0U;
    bool samplesFiniteAndNormalized = true;
    bool repeatable = true;
    double minimumPower = std::numeric_limits<double>::max();
    double maximumPower = 0.0;
    for (std::size_t index = 0U; index < kSampleCount; ++index) {
        const double z = 1.0 - 2.0 * (static_cast<double>(index) + 0.5)
            / static_cast<double>(kSampleCount);
        const double radius = std::sqrt(std::max(0.0, 1.0 - z * z));
        const double angle = kGoldenAngle * static_cast<double>(index);
        const eac3render::UnitVector3 source {
            radius * std::cos(angle), radius * std::sin(angle), z};
        const auto first = panner.render(source);
        const auto second = panner.render(source);
        repeatable = repeatable && sameResult(first, second);
        if (first.status == eac3render::Bs2127PointSourcePannerStatus::Unsupported) {
            ++unsupportedCount;
            continue;
        }
        if (first.status != eac3render::Bs2127PointSourcePannerStatus::Selected) {
            ++invalidCount;
            continue;
        }
        switch (first.region) {
        case eac3render::Bs2127PointSourceRegionKind::Identity: break;
        case eac3render::Bs2127PointSourceRegionKind::Triplet: ++sampledTriplet; break;
        case eac3render::Bs2127PointSourceRegionKind::Quad: ++sampledQuad; break;
        case eac3render::Bs2127PointSourceRegionKind::VirtualNgon: ++sampledVirtual; break;
        }
        const double currentPower = power(first.gains);
        minimumPower = std::min(minimumPower, currentPower);
        maximumPower = std::max(maximumPower, currentPower);
        samplesFiniteAndNormalized = samplesFiniteAndNormalized
            && std::all_of(first.gains.begin(), first.gains.end(),
                           [](double gain) { return std::isfinite(gain); })
            && std::abs(currentPower - 1.0) < 1.0e-8;
    }
    expect(&report, unsupportedCount == 0U && invalidCount == 0U,
           "global-fibonacci-sample-no-unsupported");
    expect(&report, samplesFiniteAndNormalized,
           "global-sample-finite-power-normalized");
    expect(&report, repeatable, "global-sample-repeatable");

    const auto nan = panner.render(eac3render::UnitVector3 {
        std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0});
    const auto nonUnit = panner.render(eac3render::UnitVector3 {2.0, 0.0, 0.0});
    expect(&report, nan.status
                        == eac3render::Bs2127PointSourcePannerStatus::InvalidInput
                    && nonUnit.status
                        == eac3render::Bs2127PointSourcePannerStatus::InvalidInput,
           "nan-and-nonunit-fail-closed");

    std::cout << "bs2127PointSourcePannerSampleCount=" << kSampleCount
              << " unsupported=" << unsupportedCount
              << " invalid=" << invalidCount
              << " triplet=" << sampledTriplet
              << " quad=" << sampledQuad
              << " virtualNgon=" << sampledVirtual
              << " minPower=" << minimumPower
              << " maxPower=" << maximumPower << '\n';
    return report;
}

void emitVectors()
{
    const eac3render::Bs2127SystemHPointSourcePanner panner;
    const std::vector<std::pair<const char *, eac3render::UnitVector3>> cases {
        {"front", {1.0, 0.0, 0.0}},
        {"left", {0.0, 1.0, 0.0}},
        {"right", {0.0, -1.0, 0.0}},
        {"rear", {-1.0, 0.0, 0.0}},
        {"upper", {0.0, 0.0, 1.0}},
        {"lower", {0.0, 0.0, -1.0}},
        {"interior", {0.7071067811865475, 0.5, 0.5}},
    };
    for (const auto &entry : cases) {
        const auto result = panner.render(entry.second);
        std::cout << "VECTOR " << entry.first << " "
                  << eac3render::bs2127PointSourcePannerStatusName(result.status)
                  << " power=" << power(result.gains);
        for (double gain : result.gains) {
            std::cout << " " << gain;
        }
        std::cout << '\n';
    }
}

} // namespace

int main(int argc, char **argv)
{
    if (argc == 2 && std::string(argv[1]) == "--vectors") {
        emitVectors();
        return 0;
    }
    const SelfTestReport report = runSelfTest();
    std::cout << "bs2127PointSourcePannerSelfTest="
              << (report.pass ? "PASS" : "FAIL")
              << " cases=" << report.cases
              << " reason=" << (report.reason.empty() ? "none" : report.reason)
              << '\n';
    std::cout << "bs2127PointSourcePannerResult="
              << (report.pass ? "PASS" : "FAIL")
              << " evidenceLimit=system-h-first-region-dispatch-only\n";
    return report.pass ? 0 : 1;
}
