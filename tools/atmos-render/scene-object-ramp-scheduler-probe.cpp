#include "scene-object-ramp-scheduler.h"

#include <cmath>
#include <iostream>
#include <limits>

namespace {
using namespace eac3render;

struct Report { int cases = 0; int failures = 0; };
void expect(Report *r, bool ok, const char *name)
{
    ++r->cases;
    std::cout << (ok ? "PASS " : "FAIL ") << name << '\n';
    if (!ok) ++r->failures;
}

SceneObjectCoordinatePolicy policy()
{
    return {10.0, 10.0, 10.0, 0.5, 0.5, 0.0};
}

SceneObjectTargetGroup group(std::uint64_t generation, std::int64_t timestamp,
                             std::int64_t duration, std::uint64_t id,
                             float gain, std::array<float, 3> room,
                             bool jump = false)
{
    SceneObjectTargetGroup result;
    result.generation = generation;
    result.timestampSamples = timestamp;
    result.durationSamples = duration;
    result.coordinatePolicy = policy();
    const double x = room[0] * 10.0 - 5.0;
    const double y = room[1] * 10.0 - 5.0;
    const double z = (room[2] + 1.0) * 5.0 - 5.0;
    std::array<double, 3> v {-y, x, z};
    const double n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    ObjectMetadataUpdate update;
    update.objectId = id;
    update.timestampSamples = timestamp;
    update.gain = gain;
    update.position = {v[0] / n, v[1] / n, v[2] / n};
    update.jumpPosition = jump;
    result.objects.push_back({update, room});
    return result;
}

bool accepted(const SceneObjectRampResult &r)
{ return r.disposition == SceneAdapterDisposition::Accepted; }

SceneObjectRampScheduler fresh(std::uint64_t generation)
{
    SceneObjectRampScheduler s;
    if (generation != kInitialGeneration) s.reset(generation);
    return s;
}

bool oneSnapshot(SceneObjectRampScheduler *s, std::uint64_t generation,
                 std::int64_t t, float gain, UnitVector3 position)
{
    std::vector<SceneObjectSnapshot> out;
    const auto r = s->evaluateSnapshot(generation, t, &out);
    return accepted(r) && out.size() == 1U
        && std::abs(out[0].gain - gain) < 1.0e-5F
        && std::abs(out[0].position[0] - position[0]) < 1.0e-5
        && std::abs(out[0].position[1] - position[1]) < 1.0e-5
        && std::abs(out[0].position[2] - position[2]) < 1.0e-5;
}

} // namespace

int main()
{
    Report report;

    eac3gate6c::MetadataUpdate decodedMetadata;
    decodedMetadata.sourcePosition = 50;
    decodedMetadata.rampDuration = 10;
    decodedMetadata.objectIndex = 1;
    decodedMetadata.state.objectIndex = 1;
    decodedMetadata.state.active = true;
    decodedMetadata.state.basicValid = true;
    decodedMetadata.state.renderValid = true;
    decodedMetadata.state.position.valid = true;
    decodedMetadata.state.position.codedX = 1.0F;
    decodedMetadata.state.position.codedY = 0.5F;
    decodedMetadata.state.position.codedZ = 0.0F;
    decodedMetadata.state.position.x = 1.0F;
    decodedMetadata.state.position.y = 0.5F;
    decodedMetadata.state.position.z = 0.0F;
    decodedMetadata.state.position.standardX = 1.0F;
    decodedMetadata.state.position.standardY = 0.5F;
    decodedMetadata.state.position.standardZ = 0.0F;
    decodedMetadata.state.gainDb = 0.0F;
    decodedMetadata.state.gainMinusInfinity = false;
    SceneObjectBatchContext decodedContext;
    decodedContext.property.generation = 1;
    decodedContext.property.coordinate = ObjectCoordinateRepresentation::OamdCartesian;
    decodedContext.coordinatePolicy = policy();
    const auto decodedBatch = adaptGate6cBatch({decodedMetadata}, decodedContext);
    expect(&report, decodedBatch.disposition == SceneAdapterDisposition::Accepted
                        && decodedBatch.targetGroup.objects.size() == 1U,
           "decoded-target-group-seam");
    auto decodedScheduler = fresh(1);
    expect(&report, accepted(decodedScheduler.applyTargetGroup(decodedBatch.targetGroup)),
           "decoded-target-group-applies");

    auto decoded = group(1, 100, 10, 1, 1.0F, {1.0F, 0.5F, 0.0F});
    auto scheduler = fresh(1);
    expect(&report, accepted(scheduler.applyTargetGroup(decoded))
                        && oneSnapshot(&scheduler, 1, 100, 1.0F, {0.0, 1.0, 0.0}),
           "first-snap");

    auto zero = group(1, 110, 0, 1, 0.25F, {0.5F, 0.0F, 0.0F});
    expect(&report, accepted(scheduler.applyTargetGroup(zero))
                        && oneSnapshot(&scheduler, 1, 110, 0.25F, {1.0, 0.0, 0.0}),
           "zero-step");

    auto ramp = group(1, 120, 10, 1, 1.0F, {1.0F, 0.5F, 0.0F});
    expect(&report, accepted(scheduler.applyTargetGroup(ramp))
                        && oneSnapshot(&scheduler, 1, 125, 0.625F,
                                       {0.7071067812, 0.7071067812, 0.0}),
           "nonzero-midpoint-exact-gain");
    expect(&report, oneSnapshot(&scheduler, 1, 130, 1.0F, {0.0, 1.0, 0.0}),
           "exact-endpoint-gain-direction");

    auto unequal = fresh(2);
    expect(&report, accepted(unequal.applyTargetGroup(
                        group(2, 200, 10, 1, 0.0F, {1.0F, 0.5F, 0.0F})))
                        && accepted(unequal.applyTargetGroup(
                            group(2, 210, 10, 1, 1.0F, {0.5F, 0.75F, 0.0F})))
                        && oneSnapshot(&unequal, 2, 215, 0.5F,
                                       {-1.0 / std::sqrt(5.0),
                                        2.0 / std::sqrt(5.0), 0.0}),
           "unequal-distance-cartesian-midpoint");

    auto overlap = fresh(3);
    overlap.applyTargetGroup(group(3, 300, 0, 1, 0.0F, {1.0F, 0.5F, 0.0F}));
    overlap.applyTargetGroup(group(3, 320, 100, 1, 1.0F, {0.5F, 0.0F, 0.0F}));
    overlap.applyTargetGroup(group(3, 350, 100, 1, 1.0F, {0.5F, 0.0F, 0.0F}));
    expect(&report, oneSnapshot(&overlap, 3, 350, 0.3F,
                                {0.3939192986, 0.9191450300, 0.0}), "overlap-continuity-at-S");
    expect(&report, oneSnapshot(&overlap, 3, 400, 0.65F,
                                {0.8804710999, 0.4740998230, 0.0}), "overlap-new-midpoint");

    auto jump = fresh(4);
    jump.applyTargetGroup(group(4, 400, 0, 1, 0.0F, {1.0F, 0.5F, 0.0F}));
    jump.applyTargetGroup(group(4, 420, 100, 1, 1.0F, {0.5F, 1.0F, 0.0F}));
    jump.applyTargetGroup(group(4, 450, 100, 1, 1.0F, {0.5F, 1.0F, 0.0F}, true));
    expect(&report, oneSnapshot(&jump, 4, 450, 0.3F,
                                {-1.0, 0.0, 0.0}),
           "jump-position-immediate-at-S");
    expect(&report, oneSnapshot(&jump, 4, 500, 0.65F,
                                {-1.0, 0.0, 0.0}),
           "jump-gain-still-ramped-at-S");

    auto stale = fresh(5);
    auto staleGroup = group(4, 1, 0, 1, 1.0F, {1.0F, 0.5F, 0.0F});
    expect(&report, stale.applyTargetGroup(staleGroup).disposition
                        == SceneAdapterDisposition::Stale, "stale-generation");
    expect(&report, accepted(stale.reset(6)) && stale.objectCount() == 0U,
           "reset-clears");
    expect(&report, stale.reset(6).disposition == SceneAdapterDisposition::Stale,
           "same-generation-reset-stale");
    auto empty = SceneObjectTargetGroup{};
    empty.generation = 6;
    expect(&report, stale.applyTargetGroup(empty).disposition
                        == SceneAdapterDisposition::InvalidInput,
           "empty-group");

    auto mismatch = fresh(7);
    auto timestampMismatch = group(7, 10, 0, 1, 1.0F, {1.0F, 0.5F, 0.0F});
    timestampMismatch.objects[0].update.timestampSamples = 11;
    expect(&report, mismatch.applyTargetGroup(timestampMismatch).disposition
                        == SceneAdapterDisposition::InvalidInput,
           "target-timestamp-mismatch");
    auto directionMismatch = group(7, 10, 0, 1, 1.0F, {1.0F, 0.5F, 0.0F});
    directionMismatch.objects[0].update.position = {1.0, 0.0, 0.0};
    expect(&report, mismatch.applyTargetGroup(directionMismatch).reason
                        == "scheduler-room-direction-mismatch",
           "room-direction-mismatch");

    auto invalidPolicy = group(8, 10, 0, 1, 1.0F, {1.0F, 0.5F, 0.0F});
    invalidPolicy.coordinatePolicy.listenerX = 2.0;
    auto policyScheduler = fresh(8);
    expect(&report, policyScheduler.applyTargetGroup(invalidPolicy).disposition
                        == SceneAdapterDisposition::InvalidInput,
           "invalid-out-of-range-policy");
    auto validPolicy = group(8, 11, 0, 1, 1.0F, {1.0F, 0.5F, 0.0F});
    policyScheduler.applyTargetGroup(validPolicy);
    validPolicy.timestampSamples = 12;
    validPolicy.objects[0].update.timestampSamples = 12;
    validPolicy.coordinatePolicy.roomWidthMetres = 11.0;
    expect(&report, policyScheduler.applyTargetGroup(validPolicy).reason
                        == "scheduler-coordinate-policy-changed",
           "changed-policy-rejected");

    auto crossingApply = fresh(9);
    crossingApply.applyTargetGroup(group(9, 100, 100, 1, 0.0F, {1.0F, 0.5F, 0.0F}));
    crossingApply.applyTargetGroup(group(9, 200, 100, 1, 1.0F, {0.0F, 0.5F, 0.0F}));
    auto replacement = group(9, 250, 0, 1, 1.0F, {1.0F, 0.5F, 0.0F});
    expect(&report, crossingApply.applyTargetGroup(replacement).reason
                        == "scheduler-overlap-position-invalid",
           "listener-crossing-at-apply");

    auto crossingEval = fresh(10);
    crossingEval.applyTargetGroup(group(10, 100, 100, 1, 0.0F, {1.0F, 0.5F, 0.0F}));
    crossingEval.applyTargetGroup(group(10, 200, 100, 1, 1.0F, {0.0F, 0.5F, 0.0F}));
    std::vector<SceneObjectSnapshot> crossingSnapshot;
    expect(&report, crossingEval.evaluateSnapshot(10, 250, &crossingSnapshot).disposition
                        == SceneAdapterDisposition::InvalidInput,
           "listener-crossing-during-evaluate");

    auto late = fresh(11);
    late.applyTargetGroup(group(11, 100, 0, 1, 1.0F, {1.0F, 0.5F, 0.0F}));
    auto lateGroup = group(11, 200, 0, 1, 1.0F, {1.0F, 0.5F, 0.0F});
    auto second = group(11, 200, 0, 2, 1.0F, {0.5F, 1.0F, 0.0F}).objects[0];
    second.update.position = {0.0, 0.0, 0.0};
    lateGroup.objects.push_back(second);
    expect(&report, late.applyTargetGroup(lateGroup).disposition
                        == SceneAdapterDisposition::InvalidInput
                        && late.objectCount() == 1U
                        && oneSnapshot(&late, 11, 100, 1.0F, {0.0, 1.0, 0.0}),
           "late-invalid-group-transactional");

    std::vector<SceneObjectSnapshot> prefilled(1);
    prefilled[0].objectId = 99;
    expect(&report, late.evaluateSnapshot(11, 99, &prefilled).disposition
                        == SceneAdapterDisposition::InvalidInput && prefilled.empty(),
           "prefilled-snapshot-cleared-on-time-failure");
    prefilled.resize(1);
    expect(&report, late.evaluateSnapshot(99, 200, &prefilled).disposition
                        == SceneAdapterDisposition::Stale && prefilled.empty(),
           "prefilled-snapshot-cleared-on-stale");

    auto laterFailure = fresh(12);
    auto two = group(12, 100, 0, 1, 1.0F, {1.0F, 0.5F, 0.0F});
    auto crossingObject = group(12, 100, 0, 2, 1.0F, {1.0F, 0.5F, 0.0F}).objects[0];
    two.objects.push_back(crossingObject);
    laterFailure.applyTargetGroup(two);
    auto crossingTarget = group(12, 200, 100, 1, 1.0F, {1.0F, 0.5F, 0.0F});
    auto crossingTarget2 = group(12, 200, 100, 2, 1.0F, {0.0F, 0.5F, 0.0F}).objects[0];
    crossingTarget.objects.push_back(crossingTarget2);
    laterFailure.applyTargetGroup(crossingTarget);
    prefilled.assign(1, SceneObjectSnapshot{});
    expect(&report, laterFailure.evaluateSnapshot(12, 250, &prefilled).disposition
                        == SceneAdapterDisposition::InvalidInput && prefilled.empty(),
           "prefilled-snapshot-cleared-on-later-object-failure");

    std::cout << "SchedulerSelfTest=" << (report.failures == 0 ? "PASS" : "FAIL")
              << " cases=" << report.cases << " failures=" << report.failures << '\n';
    return report.failures == 0 ? 0 : 1;
}
