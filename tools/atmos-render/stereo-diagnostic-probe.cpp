#include "stereo-diagnostic.h"

#include <cmath>
#include <iostream>
#include <limits>
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

bool close(float left, float right, float tolerance = 1.0e-5F)
{
    return std::abs(left - right) <= tolerance;
}

bool equalVectors(const std::vector<float> &left,
                  const std::vector<float> &right,
                  float tolerance = 1.0e-5F)
{
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0U; index < left.size(); ++index) {
        if (!close(left[index], right[index], tolerance)) {
            return false;
        }
    }
    return true;
}

SelfTestReport runSelfTest()
{
    SelfTestReport report;
    const eac3render::SpeakerLayout layout =
        eac3render::SpeakerLayout::diagnosticStereo();
    std::string reason;
    eac3render::BedRoute route;
    expect(&report, layout.routeCount() == 8U, "diagnostic-layout-route-count");
    expect(&report, layout.routeForBed(eac3render::BedLabel::FL, &route, &reason)
                    && route.enabled && route.position.azimuthDegrees == -90.0F,
           "left-bed-route");
    expect(&report, layout.routeForBed(eac3render::BedLabel::FR, &route, &reason)
                    && route.enabled && route.position.azimuthDegrees == 90.0F,
           "right-bed-route");
    expect(&report, layout.routeForBed(eac3render::BedLabel::LFE, &route, &reason)
                    && !route.enabled,
           "lfe-disabled-route");

    eac3render::SpeakerPosition position;
    expect(&report, eac3render::SpeakerPosition::fromCartesian(
                       -1.0F, 1.0F, &position, &reason)
                    && close(position.azimuthDegrees, -45.0F),
           "cartesian-to-azimuth");
    expect(&report, !eac3render::SpeakerPosition::fromCartesian(
                        0.0F, 0.0F, &position, &reason)
                    && reason == "zero-cartesian-position",
           "zero-cartesian-rejected");

    eac3render::StereoDiagnosticPanner panner;
    const float mono[] {1.0F};
    std::vector<float> stereo;
    eac3render::RenderStats stats;
    expect(&report, panner.render(mono, 1U,
                                  eac3render::SpeakerPosition::fromAzimuthDegrees(-90.0F),
                                  1.0F, &stereo, &stats, nullptr, &reason)
                    && close(stereo[0], 1.0F) && close(stereo[1], 0.0F),
           "hard-left");
    expect(&report, panner.render(mono, 1U,
                                  eac3render::SpeakerPosition::fromAzimuthDegrees(0.0F),
                                  1.0F, &stereo, &stats, nullptr, &reason)
                    && close(stereo[0], stereo[1])
                    && close(stereo[0], 0.70710678F),
           "center-equal-power");
    expect(&report, panner.render(mono, 1U,
                                  eac3render::SpeakerPosition::fromAzimuthDegrees(90.0F),
                                  1.0F, &stereo, &stats, nullptr, &reason)
                    && close(stereo[0], 0.0F) && close(stereo[1], 1.0F),
           "hard-right");

    const float symmetricMono[] {0.5F, 0.5F, 0.5F, 0.5F};
    expect(&report, panner.render(symmetricMono, 4U,
                                  eac3render::SpeakerPosition::fromAzimuthDegrees(0.0F),
                                  1.0F, &stereo, &stats, nullptr, &reason)
                    && stats.clippedSamples == 0U
                    && close(stereo[0] * stereo[0] + stereo[1] * stereo[1], 0.25F),
           "symmetric-power");

    eac3render::GainInterpolator fullRamp;
    expect(&report, fullRamp.setRamp(0.0F, 1.0F, 8U, &reason), "ramp-created");
    std::vector<float> ramp;
    expect(&report, fullRamp.render(10U, &ramp, &reason)
                    && close(ramp[0], 0.0F) && close(ramp[4], 0.5F)
                    && close(ramp[8], 1.0F) && close(ramp[9], 1.0F),
           "sample-accurate-ramp");

    const float blockInput[16] {
        1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F,
        1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F};
    eac3render::GainInterpolator oneBlockRamp;
    eac3render::GainInterpolator fourBlockRamp;
    oneBlockRamp.setRamp(0.0F, 1.0F, 16U, &reason);
    fourBlockRamp.setRamp(0.0F, 1.0F, 16U, &reason);
    std::vector<float> oneBlock;
    std::vector<float> fourBlocks;
    expect(&report, panner.render(blockInput, 16U,
                                  eac3render::SpeakerPosition::fromAzimuthDegrees(0.0F),
                                  1.0F, &oneBlock, &stats, &oneBlockRamp, &reason),
           "one-block-ramp-render");
    for (std::size_t offset = 0U; offset < 16U; offset += 4U) {
        std::vector<float> block;
        expect(&report, panner.render(blockInput + offset, 4U,
                                      eac3render::SpeakerPosition::fromAzimuthDegrees(0.0F),
                                      1.0F, &block, &stats, &fourBlockRamp, &reason),
               "four-block-ramp-render");
        fourBlocks.insert(fourBlocks.end(), block.begin(), block.end());
    }
    expect(&report, equalVectors(oneBlock, fourBlocks), "block-size-invariance");

    eac3render::GainInterpolator resetRamp;
    resetRamp.setRamp(0.25F, 0.75F, 4U, &reason);
    resetRamp.render(2U, &ramp, &reason);
    resetRamp.reset();
    expect(&report, !resetRamp.render(1U, &ramp, &reason)
                    && reason == "interpolator-uninitialized",
           "reset-clears-old-ramp");
    expect(&report, resetRamp.setRamp(0.5F, 0.5F, 0U, &reason)
                    && resetRamp.render(1U, &ramp, &reason)
                    && close(ramp[0], 0.5F),
           "reset-new-ramp-starts-clean");

    const float nanMono[] {std::numeric_limits<float>::quiet_NaN()};
    expect(&report, !panner.render(nanMono, 1U,
                                   eac3render::SpeakerPosition::fromAzimuthDegrees(0.0F),
                                   1.0F, &stereo, &stats, nullptr, &reason)
                    && stats.nonFiniteInput == 1U
                    && reason == "nonfinite-mono-input",
           "nan-input-rejected");
    expect(&report, !panner.render(mono, 1U,
                                   eac3render::SpeakerPosition::fromAzimuthDegrees(0.0F),
                                   std::numeric_limits<float>::quiet_NaN(), &stereo,
                                   &stats, nullptr, &reason)
                    && reason == "invalid-panner-input",
           "nan-gain-rejected");

    const float loudMono[] {2.0F};
    expect(&report, panner.render(loudMono, 1U,
                                  eac3render::SpeakerPosition::fromAzimuthDegrees(0.0F),
                                  1.0F, &stereo, &stats, nullptr, &reason)
                    && stats.clippedSamples == 2U
                    && stereo[0] > 1.0F && stereo[1] > 1.0F,
           "finite-clip-count-without-limiter");
    const float overflowMono[] {std::numeric_limits<float>::max()};
    expect(&report, !panner.render(overflowMono, 1U,
                                   eac3render::SpeakerPosition::fromAzimuthDegrees(0.0F),
                                   2.0F, &stereo, &stats, nullptr, &reason)
                    && stats.overflowCount == 2U
                    && stats.nonFiniteOutput == 2U,
           "overflow-count-without-limiter");

    return report;
}

} // namespace

int main()
{
    const SelfTestReport report = runSelfTest();
    std::cout << "stereoDiagnosticSelfTest="
              << (report.pass ? "PASS" : "FAIL")
              << " cases=" << report.cases
              << " reason=" << (report.reason.empty() ? "none" : report.reason)
              << '\n';
    std::cout << "stereoDiagnosticResult="
              << (report.pass ? "PASS" : "FAIL")
              << " evidenceLimit=offline-diagnostic-panner-only\n";
    return report.pass ? 0 : 1;
}
