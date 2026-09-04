#include "scene-adapter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>

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

bool finiteGains(const std::array<double, eac3render::kSystemHSpeakerCount> &gains)
{
    return std::all_of(gains.begin(), gains.end(),
                       [](double value) { return std::isfinite(value); });
}

double gainPower(
    const std::array<double, eac3render::kSystemHSpeakerCount> &gains)
{
    double value = 0.0;
    for (double gain : gains) value += gain * gain;
    return value;
}

bool sameFrame(const eac3render::SceneAdapterFrame &left,
               const eac3render::SceneAdapterFrame &right)
{
    return left.generation == right.generation
        && left.timestampSamples == right.timestampSamples
        && left.durationSamples == right.durationSamples
        && left.bedGains == right.bedGains
        && left.speakerGains == right.speakerGains
        && left.lfeEnabled == right.lfeEnabled
        && left.lfeGain == right.lfeGain
        && left.metadataHeld == right.metadataHeld
        && left.metadataTerminated == right.metadataTerminated
        && left.objects.size() == right.objects.size();
}

eac3render::UnitVector3 direction(double x, double y, double z)
{
    const double norm = std::sqrt(x * x + y * y + z * z);
    return {x / norm, y / norm, z / norm};
}

Report runSelfTest()
{
    Report report;
    const std::array<eac3render::BedLabel, 7> bedLabels {
        eac3render::BedLabel::FL, eac3render::BedLabel::FR,
        eac3render::BedLabel::FC, eac3render::BedLabel::SL,
        eac3render::BedLabel::SR, eac3render::BedLabel::TFL,
        eac3render::BedLabel::TFR};
    const std::array<std::size_t, 7> expectedSpeakerIndices {{
        6U, 5U, 2U, 9U, 8U, 11U, 10U}};
    for (std::size_t index = 0U; index < bedLabels.size(); ++index) {
        std::size_t speaker = 0U;
        std::string reason;
        expect(&report,
               eac3render::SceneAdapter::bedSpeakerIndex(
                   bedLabels[index], &speaker, &reason)
                   && speaker == expectedSpeakerIndices[index],
               "explicit-bed-label-policy");
    }
    std::size_t lfeSpeaker = 0U;
    std::string lfeReason;
    expect(&report,
           !eac3render::SceneAdapter::bedSpeakerIndex(
               eac3render::BedLabel::LFE, &lfeSpeaker, &lfeReason)
               && lfeReason == "lfe-separate-from-point-layout",
           "lfe-separate-policy");

    eac3render::SceneAdapter adapter;
    expect(&report, adapter.valid() && adapter.generation() == 1U,
           "configured-adapter-valid");
    eac3render::SceneAdapterUpdate bedUpdate;
    bedUpdate.timestampSamples = 0;
    bedUpdate.durationSamples = 100;
    bedUpdate.lfeEnabled = true;
    bedUpdate.lfeGain = 0.75F;
    for (std::size_t index = 0U; index < bedLabels.size(); ++index) {
        bedUpdate.beds.push_back({bedLabels[index], 0.5F});
    }
    const auto bedFrame = adapter.pushUpdate(bedUpdate);
    bool allBedFinite = bedFrame.accepted() && finiteGains(bedFrame.frame.speakerGains);
    for (std::size_t index = 0U; index < expectedSpeakerIndices.size(); ++index) {
        allBedFinite = allBedFinite
            && std::abs(bedFrame.frame.bedGains[expectedSpeakerIndices[index]]
                        - 0.5) < 1.0e-8;
    }
    expect(&report, allBedFinite && bedFrame.frame.lfeEnabled
                        && bedFrame.frame.lfeGain == 0.75F
                        && !bedFrame.frame.extentApplied,
           "all-bed-labels-and-lfe-frame");

    expect(&report, adapter.reset(2U).accepted(), "reset-new-generation");
    const auto firstObject = adapter.pushUpdate(eac3render::SceneAdapterUpdate {
        2U, 0, 100, {}, {{7U, 0, 1.0F, {1.0, 0.0, 0.0}, false}}, false, 0.0F});
    expect(&report, firstObject.accepted() && firstObject.frame.objects.size() == 1U
                        && firstObject.frame.objects[0].objectId == 7U
                        && firstObject.frame.objects[0].speakerGains[2] == 1.0,
           "static-object-gain-and-identity");

    eac3render::SceneAdapter allocentric;
    eac3render::ObjectMetadataUpdate centre;
    centre.objectId = 70U;
    centre.timestampSamples = 0;
    centre.gain = 1.0F;
    centre.coordinateSpace = eac3render::ObjectMetadataUpdate::CoordinateSpace::EtsiRoomCartesian;
    centre.roomPosition = {0.5F, 0.5F, 0.0F};
    centre.priority = 0.75F;
    const auto centreFrame = allocentric.pushUpdate(
        eac3render::SceneAdapterUpdate {1U, 0, 100, {}, {centre}, false, 0.0F});
    expect(&report, centreFrame.accepted()
                        && centreFrame.frame.objects.size() == 1U
                        && centreFrame.frame.objects[0].coordinateSpace
                            == eac3render::ObjectMetadataUpdate::CoordinateSpace::EtsiRoomCartesian
                        && centreFrame.frame.objects[0].roomPosition
                            == std::array<float, 3>({0.5F, 0.5F, 0.0F})
                        && centreFrame.frame.objects[0].priority == 0.75F
                        && centreFrame.frame.objects[0].extentPresence
                               == eac3render::ObjectExtentPresence::Absent
                        && std::abs(gainPower(
                            centreFrame.frame.objects[0].speakerGains) - 1.0) < 1.0e-12,
           "allocentric-listener-centre-is-real-panned-point");
    auto rightRoom = centre;
    rightRoom.timestampSamples = 100;
    rightRoom.roomPosition = {1.0F, 0.5F, 0.0F};
    const auto rightRoomFrame = allocentric.pushUpdate(
        eac3render::SceneAdapterUpdate {1U, 100, 100, {}, {rightRoom}, false, 0.0F});
    const auto roomMidpoint = allocentric.renderAt(1U, 50);
    expect(&report, rightRoomFrame.accepted() && roomMidpoint.accepted()
                        && roomMidpoint.frame.objects.size() == 1U
                        && roomMidpoint.frame.objects[0].interpolated
                        && std::abs(roomMidpoint.frame.objects[0].roomPosition[0]
                                    - 0.75F) < 1.0e-6F
                        && std::abs(gainPower(
                            roomMidpoint.frame.objects[0].speakerGains) - 1.0) < 1.0e-12,
           "allocentric-room-position-interpolation");
    eac3render::SceneAdapter constrained;
    eac3render::ObjectMetadataUpdate snapped;
    snapped.objectId = 71U;
    snapped.timestampSamples = 0;
    snapped.position = eac3render::UnitVector3 {1.0, 0.0, 0.0};
    snapped.snap = true;
    const auto snappedFrame = constrained.pushUpdate(
        eac3render::SceneAdapterUpdate {1U, 0, 100, {}, {snapped}, false, 0.0F});
    expect(&report, snappedFrame.accepted()
                        && snappedFrame.frame.objects[0].snapApplied
                        && std::abs(gainPower(
                            snappedFrame.frame.objects[0].speakerGains) - 1.0) < 1.0e-12,
           "snap-nearest-eligible-preserves-energy");
    eac3render::SceneAdapter noZone;
    auto noZoneObject = snapped;
    noZoneObject.objectId = 72U;
    noZoneObject.snap = false;
    noZoneObject.zoneConstraints = {false, false, false, false, false, false};
    const auto noZoneFrame = noZone.pushUpdate(
        eac3render::SceneAdapterUpdate {1U, 0, 100, {}, {noZoneObject}, false, 0.0F});
    expect(&report, noZoneFrame.disposition
                        == eac3render::SceneAdapterDisposition::Unsupported
                        && noZoneFrame.reason
                               == "zone-constraints-remove-all-speakers",
           "all-zone-constraints-fail-closed");
    eac3render::SceneAdapter warped;
    auto warpObject = snapped;
    warpObject.objectId = 73U;
    warpObject.snap = false;
    warpObject.warpMode = eac3render::ObjectWarpMode::PositionY2;
    warpObject.position = eac3render::UnitVector3 {0.0, 0.6, 0.8};
    const auto warpFrame = warped.pushUpdate(
        eac3render::SceneAdapterUpdate {1U, 0, 100, {}, {warpObject}, false, 0.0F});
    expect(&report, warpFrame.accepted()
                        && std::abs(gainPower(
                            warpFrame.frame.objects[0].speakerGains) - 1.0) < 1.0e-12,
           "warp-position-y2-before-render");
    auto reservedWarp = warpObject;
    reservedWarp.objectId = 74U;
    reservedWarp.warpMode = eac3render::ObjectWarpMode::Reserved;
    eac3render::SceneAdapter reservedAdapter;
    const auto reservedFrame = reservedAdapter.pushUpdate(
        eac3render::SceneAdapterUpdate {1U, 0, 100, {}, {reservedWarp}, false, 0.0F});
    expect(&report, reservedFrame.disposition
                        == eac3render::SceneAdapterDisposition::Unsupported
                        && reservedFrame.reason == "reserved-warp-mode",
           "reserved-warp-fail-closed");
    const auto secondObject = adapter.pushUpdate(eac3render::SceneAdapterUpdate {
        2U, 100, 100, {}, {{7U, 100, 0.0F, {0.0, 1.0, 0.0}, false}}, false, 0.0F});
    const auto jumpTarget = adapter.pushUpdate(eac3render::SceneAdapterUpdate {
        2U, 200, 100, {}, {{7U, 200, 1.0F, {0.0, 0.0, 1.0}, false}}, false, 0.0F});
    const auto midpoint = adapter.renderAt(2U, 150);
    expect(&report, secondObject.accepted() && jumpTarget.accepted()
                        && midpoint.accepted()
                        && midpoint.frame.objects.size() == 1U
                        && midpoint.frame.objects[0].interpolated
                        && std::abs(midpoint.frame.objects[0].gain - 0.5F) < 1.0e-6F
                        && std::abs(midpoint.frame.objects[0].position[1]
                                     - std::sqrt(0.5)) < 1.0e-6
                        && std::abs(midpoint.frame.objects[0].position[2]
                                     - std::sqrt(0.5)) < 1.0e-6,
           "linear-object-position-and-gain-midpoint");

    const auto jumped = adapter.pushUpdate(eac3render::SceneAdapterUpdate {
        2U, 300, 100, {}, {{7U, 300, 0.0F, {-1.0, 0.0, 0.0}, true}}, false, 0.0F});
    const auto beforeJump = adapter.renderAt(2U, 250);
    expect(&report, jumpTarget.accepted() && jumped.accepted()
                        && beforeJump.accepted() && beforeJump.frame.objects.size() == 1U
                        && beforeJump.frame.objects[0].interpolated
                        && std::abs(beforeJump.frame.objects[0].position[2] - 1.0)
                               < 1.0e-8,
           "jump-position-keeps-preceding-position");

    const auto multi = adapter.pushUpdate(eac3render::SceneAdapterUpdate {
        2U, 400, 100, {}, {{7U, 400, 1.0F, direction(1.0, 1.0, 0.0), false},
                           {8U, 400, 0.25F, {0.0, 1.0, 0.0}, false}},
        false, 0.0F});
    expect(&report, multi.accepted() && multi.frame.objects.size() == 2U
                        && finiteGains(multi.frame.speakerGains),
           "multiple-object-per-object-frames-before-sum");

    const auto held = adapter.pushUpdate(eac3render::SceneAdapterUpdate {
        2U, 500, 100, {}, {}, false, 0.0F});
    expect(&report, held.accepted() && held.frame.metadataHeld
                        && held.frame.objects.size() == 2U,
           "metadata-gap-hold-last");
    expect(&report, held.frame.lfeGain == 0.0F && !held.frame.lfeEnabled,
           "lfe-disabled-is-zero");

    eac3render::SceneAdapter holdHistory;
    expect(&report, holdHistory.pushUpdate(eac3render::SceneAdapterUpdate {
                         1U, 0, 10, {},
                         {{31U, 0, 1.0F, {1.0, 0.0, 0.0}, false}}, false, 0.0F})
                        .accepted()
                    && holdHistory.pushUpdate(eac3render::SceneAdapterUpdate {
                         1U, 10, 10, {}, {}, false, 0.0F})
                        .accepted(),
           "historical-hold-gap-accepted");
    const auto holdAtGap = holdHistory.renderAt(1U, 15);
    expect(&report, holdAtGap.accepted() && holdAtGap.frame.objects.size() == 1U
                        && holdAtGap.frame.objects[0].held
                        && holdAtGap.frame.objects[0].position[0] == 1.0,
           "historical-hold-gap-state");
    expect(&report, holdHistory.pushUpdate(eac3render::SceneAdapterUpdate {
                         1U, 20, 10, {},
                         {{31U, 20, 0.25F, {0.0, 1.0, 0.0}, false}}, false, 0.0F})
                        .accepted(),
           "historical-hold-reappearance-accepted");
    const auto holdAfterReappearance = holdHistory.renderAt(1U, 15);
    const auto holdBeforeGap = holdHistory.renderAt(1U, 9);
    const auto holdBeforeReappearance = holdHistory.renderAt(1U, 19);
    const auto holdAtReappearance = holdHistory.renderAt(1U, 20);
    expect(&report, holdAfterReappearance.accepted()
                        && holdAfterReappearance.frame.objects.size() == 1U
                        && holdAfterReappearance.frame.objects[0].held
                        && holdBeforeGap.frame.objects.size() == 1U
                        && !holdBeforeGap.frame.objects[0].interpolated
                        && holdBeforeGap.frame.objects[0].gain == 1.0F
                        && holdBeforeGap.frame.objects[0].position[0] == 1.0
                        && holdBeforeReappearance.frame.objects.size() == 1U
                        && holdBeforeReappearance.frame.objects[0].held
                        && holdAtReappearance.frame.objects.size() == 1U
                        && !holdAtReappearance.frame.objects[0].held
                        && holdAtReappearance.frame.objects[0].gain == 0.25F,
           "historical-hold-gap-does-not-interpolate");

    eac3render::SceneAdapter terminating(eac3render::MetadataGapPolicy::Terminate);
    const auto termFirst = terminating.pushUpdate(eac3render::SceneAdapterUpdate {
        1U, 0, 100, {}, {{9U, 0, 1.0F, {1.0, 0.0, 0.0}, false}}, false, 0.0F});
    const auto termGap = terminating.pushUpdate(eac3render::SceneAdapterUpdate {
        1U, 100, 100, {}, {}, false, 0.0F});
    expect(&report, termFirst.accepted() && termGap.accepted()
                        && termGap.frame.objects.empty()
                        && termGap.frame.metadataTerminated,
           "metadata-gap-terminates");
    eac3render::SceneAdapter terminateHistory(eac3render::MetadataGapPolicy::Terminate);
    expect(&report, terminateHistory.pushUpdate(eac3render::SceneAdapterUpdate {
                         1U, 0, 10, {},
                         {{32U, 0, 1.0F, {1.0, 0.0, 0.0}, false}}, false, 0.0F})
                        .accepted()
                    && terminateHistory.pushUpdate(eac3render::SceneAdapterUpdate {
                         1U, 10, 10, {}, {}, false, 0.0F})
                        .accepted(),
           "historical-terminate-gap-accepted");
    const auto terminateAtGap = terminateHistory.renderAt(1U, 15);
    expect(&report, terminateAtGap.accepted() && terminateAtGap.frame.objects.empty()
                        && terminateAtGap.frame.metadataTerminated,
           "historical-terminate-gap-state");
    expect(&report, terminateHistory.pushUpdate(eac3render::SceneAdapterUpdate {
                         1U, 20, 10, {},
                         {{32U, 20, 0.5F, {0.0, 0.0, 1.0}, false}}, false, 0.0F})
                        .accepted(),
           "historical-terminate-reappearance-accepted");
    const auto terminateAfterReappearance = terminateHistory.renderAt(1U, 15);
    const auto terminateAtReappearance = terminateHistory.renderAt(1U, 20);
    expect(&report, terminateAfterReappearance.accepted()
                        && terminateAfterReappearance.frame.objects.empty()
                        && terminateAfterReappearance.frame.metadataTerminated
                        && terminateHistory.renderAt(1U, 9).frame.objects.size() == 1U
                        && !terminateHistory.renderAt(1U, 9)
                               .frame.objects[0].interpolated
                        && terminateHistory.renderAt(1U, 9)
                               .frame.objects[0].gain == 1.0F
                        && terminateAtReappearance.frame.objects.size() == 1U
                        && !terminateAtReappearance.frame.metadataTerminated
                        && terminateAtReappearance.frame.objects[0].gain == 0.5F,
           "historical-terminate-gap-does-not-interpolate");

    const auto staleUpdate = adapter.pushUpdate(eac3render::SceneAdapterUpdate {
        1U, 600, 100, {}, {}, false, 0.0F});
    expect(&report, staleUpdate.disposition
                        == eac3render::SceneAdapterDisposition::Stale,
           "stale-update-rejected");
    expect(&report, adapter.renderAt(1U, 0).disposition
                        == eac3render::SceneAdapterDisposition::Stale,
           "stale-render-rejected");
    expect(&report, adapter.reset(3U).accepted()
                        && adapter.renderAt(3U, 0).frame.objects.empty(),
           "reset-clears-object-state");

    eac3render::SceneAdapter invalid;
    expect(&report, invalid.pushUpdate(eac3render::SceneAdapterUpdate {
                         1U, 0, 10, {}, {{1U, 0, 1.0F, {2.0, 0.0, 0.0}, false}},
                         false, 0.0F})
                        .disposition
                    == eac3render::SceneAdapterDisposition::InvalidInput,
           "nonunit-object-position-rejected");
    eac3render::ObjectMetadataUpdate invalidPriority;
    invalidPriority.objectId = 1U;
    invalidPriority.timestampSamples = 0;
    invalidPriority.priority = 1.01F;
    expect(&report, invalid.pushUpdate(eac3render::SceneAdapterUpdate {
                         1U, 0, 10, {}, {invalidPriority}, false, 0.0F})
                        .reason == "scene-object-priority-invalid",
           "object-priority-range-rejected");
    expect(&report, invalid.pushUpdate(eac3render::SceneAdapterUpdate {
                         1U, 0, 10, {}, {{1U, 10, 1.0F, {1.0, 0.0, 0.0}, false}},
                         false, 0.0F})
                        .disposition
                    == eac3render::SceneAdapterDisposition::InvalidInput,
           "object-timestamp-out-of-block-rejected");
    expect(&report, invalid.pushUpdate(eac3render::SceneAdapterUpdate {
                         1U, 0, 10,
                         {{eac3render::BedLabel::FL, 1.0F},
                          {eac3render::BedLabel::FL, 1.0F}}, {}, false, 0.0F})
                        .disposition
                    == eac3render::SceneAdapterDisposition::InvalidInput,
           "duplicate-bed-rejected");
    expect(&report, invalid.pushUpdate(eac3render::SceneAdapterUpdate {
                         1U, 0, 10, {}, {{1U, 0, 1.0F, {1.0, 0.0, 0.0}, false},
                                        {1U, 0, 1.0F, {0.0, 1.0, 0.0}, false}},
                         false, 0.0F})
                        .disposition
                    == eac3render::SceneAdapterDisposition::InvalidInput,
           "duplicate-object-rejected");

    eac3render::SceneAdapter deterministicA;
    eac3render::SceneAdapter deterministicB;
    const eac3render::SceneAdapterUpdate deterministicUpdate {
        1U, 0, 100, {{eac3render::BedLabel::FC, 0.25F}},
        {{42U, 0, 0.75F, direction(0.3, 0.4, 0.5), false}}, true, 0.2F};
    const auto deterministicFrameA = deterministicA.pushUpdate(deterministicUpdate);
    const auto deterministicFrameB = deterministicB.pushUpdate(deterministicUpdate);
    expect(&report, deterministicFrameA.accepted() && deterministicFrameB.accepted()
                        && sameFrame(deterministicFrameA.frame,
                                     deterministicFrameB.frame),
           "deterministic-repeatable-output");

    std::cout << "sceneAdapter generation=" << adapter.generation()
              << " gapPolicy=" << eac3render::metadataGapPolicyName(adapter.gapPolicy())
              << " updates=" << adapter.updateCount() << '\n';
    std::cout << "sceneAdapterSelfTest=" << (report.pass ? "PASS" : "FAIL")
              << " cases=" << report.cases
              << " reason=" << (report.reason.empty() ? "none" : report.reason)
              << " extentApplied=NO lfePolicy=separate-not-point-source\n";
    return report;
}

} // namespace

int main()
{
    const Report report = runSelfTest();
    return report.pass ? 0 : 1;
}
