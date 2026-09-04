#include "scene-object-gain-frame.h"
#include "scene-object-stream-grouper.h"

#include <cmath>
#include <algorithm>
#include <iostream>

namespace {
using namespace eac3render;
struct Report { int cases = 0; int failures = 0; };
void expect(Report *r, bool ok, const char *name)
{
    ++r->cases;
    std::cout << (ok ? "PASS " : "FAIL ") << name << '\n';
    if (!ok) ++r->failures;
}
SceneObjectSnapshot object(std::uint64_t id, float gain, UnitVector3 position)
{ return {id, gain, position}; }
double power(const std::array<double, kSystemHSpeakerCount> &v)
{ double p=0.0; for(double x:v) p+=x*x; return p; }
eac3gate6c::MetadataUpdate metadata(std::int64_t t, unsigned d, unsigned id,
                                    float gainDb)
{
    eac3gate6c::MetadataUpdate m;
    m.sourcePosition=t; m.rampDuration=d; m.objectIndex=id;
    m.state.objectIndex=id; m.state.active=true; m.state.basicValid=true;
    m.state.renderValid=true; m.state.position.valid=true;
    m.state.position.codedX=1.0F; m.state.position.codedY=0.5F;
    m.state.position.codedZ=0.0F; m.state.position.x=1.0F;
    m.state.position.y=0.5F; m.state.position.z=0.0F;
    m.state.position.standardX=1.0F; m.state.position.standardY=0.5F;
    m.state.position.standardZ=0.0F; m.state.gainDb=gainDb;
    m.state.gainMinusInfinity=false; m.state.raw.sizePresent=false;
    m.state.effectiveSizePresent=false;
    return m;
}
SceneObjectBatchContext context(std::uint64_t generation=1U)
{
    SceneObjectBatchContext c; c.property.generation=generation;
    c.property.coordinate=ObjectCoordinateRepresentation::OamdCartesian;
    c.coordinatePolicy={10.0,10.0,10.0,0.5,0.5,0.0}; return c;
}
}

int main()
{
    Report report;
    Bs2127SystemHConfiguredPanner panner;
    expect(&report, panner.valid(), "panner-valid");
    const UnitVector3 front {1.0, 0.0, 0.0};
    const UnitVector3 right {0.0, 1.0, 0.0};
    auto empty = makeSystemHGainFrame({}, panner);
    expect(&report, empty.disposition == SceneAdapterDisposition::Accepted
                        && empty.objects.empty() && power(empty.summedGains) == 0.0,
           "empty-snapshot-silence");
    auto zero = makeSystemHGainFrame({object(1, 0.0F, front)}, panner);
    expect(&report, zero.disposition == SceneAdapterDisposition::Accepted
                        && zero.objects.size() == 1U
                        && zero.objects[0].objectId == 1U
                        && power(zero.objects[0].gains) == 0.0,
           "zero-gain-keeps-identity");
    auto one = makeSystemHGainFrame({object(1, 0.5F, front)}, panner);
    expect(&report, one.disposition == SceneAdapterDisposition::Accepted
                        && std::abs(power(one.objects[0].gains) - 0.25) < 1.0e-8,
           "speaker-power-equals-object-gain-squared");
    auto deterministic = makeSystemHGainFrame({object(1, 0.5F, front)}, panner);
    expect(&report, deterministic.objects.size() == one.objects.size()
                        && deterministic.objects[0].objectId == one.objects[0].objectId
                        && deterministic.objects[0].gains == one.objects[0].gains
                        && deterministic.summedGains == one.summedGains,
           "gain-frame-deterministic");
    auto two = makeSystemHGainFrame({object(1, 0.5F, front), object(2, 0.25F, right)}, panner);
    expect(&report, two.disposition == SceneAdapterDisposition::Accepted
                        && two.objects.size() == 2U
                        && std::all_of(two.summedGains.begin(), two.summedGains.end(),
                                       [](double x) { return std::isfinite(x); }),
           "two-object-summed-frame");
    auto duplicate = makeSystemHGainFrame({object(2, 0.5F, front), object(2, 0.25F, right)}, panner);
    expect(&report, duplicate.disposition == SceneAdapterDisposition::InvalidInput
                        && duplicate.objects.empty() && power(duplicate.summedGains) == 0.0,
           "duplicate-id-transactional");
    auto invalidGain = makeSystemHGainFrame({object(1, -1.0F, front)}, panner);
    expect(&report, invalidGain.disposition == SceneAdapterDisposition::InvalidInput,
           "negative-gain-rejected");
    auto invalidPosition = makeSystemHGainFrame({object(1, 1.0F, {2.0, 0.0, 0.0})}, panner);
    expect(&report, invalidPosition.disposition == SceneAdapterDisposition::InvalidInput,
           "nonunit-position-rejected");
    auto late = makeSystemHGainFrame({object(1, 0.5F, front), object(2, 0.5F, {0.0, 0.0, 0.0})}, panner);
    expect(&report, late.disposition == SceneAdapterDisposition::InvalidInput
                        && late.objects.empty(), "late-object-failure-no-partial");

    std::vector<eac3gate6c::MetadataUpdate> input {
        metadata(100, 0, 1, 0.0F), metadata(100, 0, 2, -6.0F),
        metadata(200, 10, 1, -3.0F), metadata(200, 10, 2, -9.0F)};
    SceneObjectStreamGrouper grouper;
    const auto grouped = grouper.append(input, context());
    expect(&report, grouped.disposition == SceneAdapterDisposition::Accepted
                        && grouped.groups.size() == 2U, "causal-decoder-grouping");
    SceneObjectRampScheduler scheduler;
    std::size_t next = 0U;
    const auto future = applyNextCausalGroup(grouped.groups, &next, 99, &scheduler);
    std::vector<SceneObjectSnapshot> snapshots;
    auto frameMatches = [](const SceneObjectGainFrameResult &frame,
                           double firstGain, double secondGain) {
        return frame.disposition == SceneAdapterDisposition::Accepted
            && frame.objects.size() == 2U
            && frame.objects[0].objectId == 1U && frame.objects[1].objectId == 2U
            && std::abs(std::sqrt(power(frame.objects[0].gains)) - firstGain) < 1.0e-7
            && std::abs(std::sqrt(power(frame.objects[1].gains)) - secondGain) < 1.0e-7
            && std::abs(power(frame.objects[0].gains) - firstGain * firstGain) < 1.0e-7
            && std::abs(power(frame.objects[1].gains) - secondGain * secondGain) < 1.0e-7;
    };
    const double oldFirst = 1.0;
    const double oldSecond = std::pow(10.0, -6.0 / 20.0);
    const double newFirst = std::pow(10.0, -3.0 / 20.0);
    const double newSecond = std::pow(10.0, -9.0 / 20.0);
    expect(&report, !future.applied && next == 0U,
           "causal-first-group-not-early");
    const auto firstReady = applyNextCausalGroup(grouped.groups, &next, 100, &scheduler);
    const auto start = scheduler.evaluateSnapshot(1, 100, &snapshots);
    const auto firstFrame = makeSystemHGainFrame(snapshots, panner);
    expect(&report, firstReady.applied && next == 1U
                        && start.disposition == SceneAdapterDisposition::Accepted
                        && frameMatches(firstFrame, oldFirst, oldSecond),
           "causal-first-group-frame");
    const auto secondFuture = applyNextCausalGroup(grouped.groups, &next, 199, &scheduler);
    expect(&report, !secondFuture.applied && next == 1U,
           "causal-second-group-not-early");
    const auto secondReady = applyNextCausalGroup(grouped.groups, &next, 200, &scheduler);
    const auto rampStart = scheduler.evaluateSnapshot(1, 200, &snapshots);
    const auto rampStartFrame = makeSystemHGainFrame(snapshots, panner);
    const auto rampMid = scheduler.evaluateSnapshot(1, 205, &snapshots);
    const auto rampMidFrame = makeSystemHGainFrame(snapshots, panner);
    const auto rampEnd = scheduler.evaluateSnapshot(1, 210, &snapshots);
    const auto rampEndFrame = makeSystemHGainFrame(snapshots, panner);
    expect(&report, secondReady.applied && next == 2U
                        && rampStart.disposition == SceneAdapterDisposition::Accepted
                        && frameMatches(rampStartFrame, oldFirst, oldSecond),
           "causal-ramp-start-exact");
    expect(&report, rampMid.disposition == SceneAdapterDisposition::Accepted
                        && frameMatches(rampMidFrame, (oldFirst + newFirst) / 2.0,
                                        (oldSecond + newSecond) / 2.0),
           "causal-ramp-mid-exact");
    expect(&report, rampEnd.disposition == SceneAdapterDisposition::Accepted
                        && frameMatches(rampEndFrame, newFirst, newSecond),
           "causal-ramp-end-exact");
    SceneObjectRampScheduler staleScheduler;
    staleScheduler.reset(2);
    std::size_t staleIndex = 0U;
    const auto stale = applyNextCausalGroup(grouped.groups, &staleIndex, 100,
                                            &staleScheduler);
    expect(&report, stale.disposition == SceneAdapterDisposition::Stale
                        && staleIndex == 0U && !stale.applied,
           "causal-stale-does-not-call-frame");

    std::cout << "GainFrameSelfTest=" << (report.failures == 0 ? "PASS" : "FAIL")
              << " cases=" << report.cases << " failures=" << report.failures << '\n';
    return report.failures == 0 ? 0 : 1;
}
