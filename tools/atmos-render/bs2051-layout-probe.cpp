#include "bs2051-layout.h"

#include <cmath>
#include <iostream>
#include <string>
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

std::vector<eac3render::SofaEmitterRecord> cachedSystemHPositions()
{
    using Record = eac3render::SofaEmitterRecord;
    // Exported from the ignored local bbcrdlr_systemH.sofa.  The SOFA file's
    // global Comment supplies this order; it has no per-emitter descriptions.
    return {
        Record {0U, {}, 60.0, 0.0, 2.70687},
        Record {1U, {}, -60.0, 0.0, 2.70687},
        Record {2U, {}, 0.0, 0.0, 1.99400},
        Record {3U, {}, 135.0, 0.0, 3.00613},
        Record {4U, {}, -135.0, 0.0, 3.00613},
        Record {5U, {}, 30.0, 0.0, 2.37201},
        Record {6U, {}, -30.0, 0.0, 2.37201},
        Record {7U, {}, 180.0, 0.0, 1.99400},
        Record {8U, {}, 90.0, 0.0, 2.28400},
        Record {9U, {}, -90.0, 0.0, 2.28400},
        Record {10U, {}, 45.0, 40.0, 1.91029},
        Record {11U, {}, -45.0, 40.0, 1.91029},
        Record {12U, {}, 0.0, 40.0, 1.91029},
        Record {13U, {}, 0.0, 90.0, 1.29700},
        Record {14U, {}, 135.0, 40.0, 1.91029},
        Record {15U, {}, -135.0, 40.0, 1.91029},
        Record {16U, {}, 90.0, 40.0, 1.86906},
        Record {17U, {}, -90.0, 40.0, 1.86906},
        Record {18U, {}, 180.0, 40.0, 1.91029},
        Record {19U, {}, 0.0, -26.0, 2.20000},
        Record {20U, {}, 45.0, -22.0, 2.58975},
        Record {21U, {}, -45.0, -22.0, 2.58975},
    };
}

SelfTestReport runSelfTest()
{
    SelfTestReport report;
    std::string reason;
    const auto &catalog = eac3render::Bs2051SystemHLayout::systemH();
    expect(&report, catalog.size() == 22U, "system-h-count");
    expect(&report, eac3render::kSystemHLfePolicy
                       == eac3render::LfePolicy::SeparateFromPointLayout,
           "system-h-lfe-separate-policy");
    expect(&report, eac3render::Bs2051SystemHLayout::validateSystemH(&reason),
           "system-h-validation");

    eac3render::Bs2051SpeakerPosition position;
    expect(&report, eac3render::Bs2051SystemHLayout::find(
                       eac3render::Bs2051Label::MPlus060, &position, &reason)
                    && std::string(eac3render::bs2051LabelName(position.label)) == "M+060"
                    && position.azimuthDegrees == 60.0
                    && position.elevationDegrees == 0.0,
           "label-position-mapping");
    expect(&report, eac3render::Bs2051SystemHLayout::find(
                       eac3render::Bs2051Label::TPlus000, &position, &reason)
                    && position.layer == eac3render::Bs2051Layer::Upper
                    && position.elevationDegrees == 90.0,
           "top-position-mapping");

    const auto vector = catalog[0].unitVector();
    const double norm = vector[0] * vector[0] + vector[1] * vector[1]
        + vector[2] * vector[2];
    expect(&report, std::abs(norm - 1.0) < 1.0e-12,
           "unit-vector-normalization");
    expect(&report, std::abs(catalog[0].unitVector()[0]
                             - catalog[1].unitVector()[0]) < 1.0e-12
                    && std::abs(catalog[0].unitVector()[1]
                                + catalog[1].unitVector()[1]) < 1.0e-12,
           "unit-vector-mirror");

    std::vector<eac3render::SofaEmitterRecord> sofa = cachedSystemHPositions();
    eac3render::SofaLayoutComparison strictComparison =
        eac3render::compareSystemHToSofa(sofa, 1.0e-6, 1.0e-6);
    expect(&report, !strictComparison.pass && strictComparison.compared == 22U
                    && strictComparison.usedEmitterOrder
                    && strictComparison.perEmitterDescriptionMissing
                    && strictComparison.maxAzimuthErrorDegrees == 0.0
                    && strictComparison.maxElevationErrorDegrees == 10.0,
           "sofa-system-h-strict-angle-difference-reported");
    eac3render::SofaLayoutComparison comparison =
        eac3render::compareSystemHToSofa(sofa, 1.0e-6, 10.0);
    expect(&report, comparison.pass && comparison.usedEmitterOrder
                    && comparison.perEmitterDescriptionMissing,
           "sofa-system-h-angle-order-with-measured-tolerance");
    expect(&report, comparison.minimumDistanceMetres > 1.2
                    && comparison.maximumDistanceMetres > 3.0,
           "sofa-distance-reported-not-normalized");

    sofa[10].azimuthDegrees += 0.01;
    comparison = eac3render::compareSystemHToSofa(sofa, 1.0e-6, 10.0);
    expect(&report, !comparison.pass && comparison.mismatches == 1U,
           "sofa-angle-mismatch-rejected");

    sofa = cachedSystemHPositions();
    sofa[0].description = "wrong-label";
    comparison = eac3render::compareSystemHToSofa(sofa, 1.0e-6, 10.0);
    expect(&report, !comparison.pass && !comparison.usedEmitterOrder
                    && !comparison.perEmitterDescriptionMissing,
           "description-presence-reported");

    return report;
}

} // namespace

int main()
{
    const SelfTestReport report = runSelfTest();
    std::cout << "bs2051SystemHSelfTest=" << (report.pass ? "PASS" : "FAIL")
              << " cases=" << report.cases
              << " reason=" << (report.reason.empty() ? "none" : report.reason)
              << '\n';
    std::cout << "bs2051SystemHResult=" << (report.pass ? "PASS" : "FAIL")
              << " speakers=22 layers=9+10+3 lfePolicy=separate-excluded"
              << " evidenceLimit=offline-layout-contract-only\n";
    return report.pass ? 0 : 1;
}
