#include "bs2127-triplet.h"

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

bool close(double left, double right, double tolerance = 1.0e-10)
{
    return std::abs(left - right) <= tolerance;
}

double power(const eac3render::UnitVector3 &gains)
{
    return gains[0] * gains[0] + gains[1] * gains[1] + gains[2] * gains[2];
}

SelfTestReport runSelfTest()
{
    SelfTestReport report;
    using eac3render::UnitVector3;
    const std::array<UnitVector3, 3> axes {
        UnitVector3 {1.0, 0.0, 0.0},
        UnitVector3 {0.0, 1.0, 0.0},
        UnitVector3 {0.0, 0.0, 1.0},
    };

    eac3render::TripletSolveResult result = eac3render::solveTriplet(
        axes, UnitVector3 {1.0, 0.0, 0.0});
    expect(&report, result.accepted && close(result.rawGains[0], 1.0)
                    && close(result.rawGains[1], 0.0)
                    && close(result.rawGains[2], 0.0)
                    && close(result.determinant, 1.0)
                    && std::isfinite(result.conditionEstimate),
           "positive-axis-point");

    const double inverseSqrtThree = 1.0 / std::sqrt(3.0);
    result = eac3render::solveTriplet(
        axes, UnitVector3 {inverseSqrtThree, inverseSqrtThree, inverseSqrtThree});
    expect(&report, result.accepted && close(result.rawGains[0], inverseSqrtThree)
                    && close(result.rawGains[1], inverseSqrtThree)
                    && close(result.rawGains[2], inverseSqrtThree)
                    && close(power(result.normalizedGains), 1.0),
           "positive-interior-point-and-power");

    const double inverseSqrtTwo = 1.0 / std::sqrt(2.0);
    result = eac3render::solveTriplet(
        axes, UnitVector3 {inverseSqrtTwo, inverseSqrtTwo, 0.0});
    expect(&report, result.accepted && close(result.normalizedGains[2], 0.0)
                    && close(power(result.normalizedGains), 1.0),
           "positive-boundary-point");

    result = eac3render::solveTriplet(
        axes, UnitVector3 {-inverseSqrtTwo, inverseSqrtTwo, 0.0});
    expect(&report, !result.accepted
                    && result.reason == "source-outside-triplet-negative-gain",
           "external-negative-gain-rejected");

    const UnitVector3 roundoffNegative {
        -5.0e-12, std::sqrt(1.0 - 25.0e-24), 0.0,
    };
    result = eac3render::solveTriplet(axes, roundoffNegative);
    expect(&report, result.accepted
                    && result.toleranceZeroedNegativeGains == 1U
                    && close(result.normalizedGains[0], 0.0),
           "roundoff-negative-gain-zeroed-only-within-tolerance");

    const std::array<UnitVector3, 3> singular {
        axes[0], axes[1], axes[0],
    };
    result = eac3render::solveTriplet(singular, axes[0]);
    expect(&report, !result.accepted
                    && result.reason == "singular-or-ill-conditioned-triplet",
           "singular-triplet-rejected");

    const std::array<UnitVector3, 3> nonFiniteSpeaker {
        axes[0], axes[1], UnitVector3 {
            std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0},
    };
    result = eac3render::solveTriplet(nonFiniteSpeaker, axes[0]);
    expect(&report, !result.accepted && result.reason == "nonfinite-speaker-vector",
           "nan-speaker-rejected");
    result = eac3render::solveTriplet(
        axes, UnitVector3 {
            std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0});
    expect(&report, !result.accepted && result.reason == "nonfinite-source-vector",
           "nan-source-rejected");

    const std::array<UnitVector3, 3> permuted {
        axes[2], axes[0], axes[1],
    };
    const UnitVector3 interior {
        0.5, 0.5, std::sqrt(0.5),
    };
    const auto original = eac3render::solveTriplet(axes, interior);
    const auto reordered = eac3render::solveTriplet(permuted, interior);
    expect(&report, original.accepted && reordered.accepted
                    && close(original.normalizedGains[0], reordered.normalizedGains[1])
                    && close(original.normalizedGains[1], reordered.normalizedGains[2])
                    && close(original.normalizedGains[2], reordered.normalizedGains[0]),
           "speaker-order-permutation");

    const UnitVector3 almostUnit {1.0 + 2.0e-7, 0.0, 0.0};
    result = eac3render::solveTriplet(
        axes, almostUnit, eac3render::TripletSolveOptions {
            1.0e-10, 1.0e-10, 1.0e-6});
    expect(&report, result.accepted, "unit-vector-within-tolerance");
    const UnitVector3 outsideUnit {1.0 + 2.0e-5, 0.0, 0.0};
    result = eac3render::solveTriplet(
        axes, outsideUnit, eac3render::TripletSolveOptions {
            1.0e-10, 1.0e-10, 1.0e-6});
    expect(&report, !result.accepted
                    && result.reason == "source-vector-not-unit-within-tolerance",
           "unit-vector-outside-tolerance");

    const std::array<UnitVector3, 3> almostUnitSpeaker {
        UnitVector3 {1.0 + 2.0e-7, 0.0, 0.0}, axes[1], axes[2],
    };
    result = eac3render::solveTriplet(
        almostUnitSpeaker, axes[0], eac3render::TripletSolveOptions {
            1.0e-10, 1.0e-10, 1.0e-6});
    expect(&report, result.accepted, "speaker-unit-within-tolerance");

    const std::array<UnitVector3, 3> outsideUnitSpeaker {
        UnitVector3 {1.0 + 2.0e-5, 0.0, 0.0}, axes[1], axes[2],
    };
    result = eac3render::solveTriplet(
        outsideUnitSpeaker, axes[0], eac3render::TripletSolveOptions {
            1.0e-10, 1.0e-10, 1.0e-6});
    expect(&report, !result.accepted
                    && result.reason == "speaker-vector-not-unit-within-tolerance",
           "speaker-unit-outside-tolerance");

    return report;
}

} // namespace

int main()
{
    const SelfTestReport report = runSelfTest();
    std::cout << "bs2127TripletSelfTest=" << (report.pass ? "PASS" : "FAIL")
              << " cases=" << report.cases
              << " reason=" << (report.reason.empty() ? "none" : report.reason)
              << '\n';
    std::cout << "bs2127TripletResult=" << (report.pass ? "PASS" : "FAIL")
              << " evidenceLimit=single-triplet-primitive-only\n";
    return report.pass ? 0 : 1;
}
