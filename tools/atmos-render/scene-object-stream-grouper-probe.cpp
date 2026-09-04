#include "scene-object-stream-grouper.h"

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

eac3gate6c::MetadataUpdate record(std::int64_t timestamp,
                                  unsigned duration, unsigned id)
{
    eac3gate6c::MetadataUpdate value;
    value.sourcePosition = timestamp;
    value.rampDuration = duration;
    value.objectIndex = id;
    value.state.objectIndex = id;
    value.state.active = true;
    value.state.basicValid = true;
    value.state.renderValid = true;
    value.state.position.valid = true;
    value.state.position.codedX = 1.0F;
    value.state.position.codedY = 0.5F;
    value.state.position.codedZ = 0.0F;
    value.state.position.x = 1.0F;
    value.state.position.y = 0.5F;
    value.state.position.z = 0.0F;
    value.state.position.standardX = 1.0F;
    value.state.position.standardY = 0.5F;
    value.state.position.standardZ = 0.0F;
    value.state.gainDb = 0.0F;
    value.state.gainMinusInfinity = false;
    value.state.raw.sizePresent = false;
    value.state.effectiveSizePresent = false;
    return value;
}

std::vector<eac3gate6c::MetadataUpdate> records(std::int64_t timestamp,
                                                unsigned duration,
                                                unsigned count = 15U)
{
    std::vector<eac3gate6c::MetadataUpdate> result;
    for (unsigned id = 1U; id <= count; ++id)
        result.push_back(record(timestamp, duration, id));
    return result;
}

SceneObjectBatchContext context(std::uint64_t generation = 1U)
{
    SceneObjectBatchContext value;
    value.property.generation = generation;
    value.property.coordinate = ObjectCoordinateRepresentation::OamdCartesian;
    value.coordinatePolicy = {10.0, 10.0, 10.0, 0.5, 0.5, 0.0};
    return value;
}

} // namespace

int main()
{
    Report report;
    SceneObjectStreamGrouper grouper;
    const auto fifteen = records(100, 0);
    auto first = grouper.append(fifteen, context());
    expect(&report, first.disposition == SceneAdapterDisposition::Accepted
                        && first.groups.size() == 1U
                        && first.groups[0].objects.size() == 15U,
           "fifteen-object-style-ordering");

    auto twoGroups = records(200, 0);
    auto second = records(300, 10);
    twoGroups.insert(twoGroups.end(), second.begin(), second.end());
    SceneObjectStreamGrouper grouped;
    const auto groupedResult = grouped.append(twoGroups, context());
    expect(&report, groupedResult.disposition == SceneAdapterDisposition::Accepted
                        && groupedResult.groups.size() == 2U
                        && groupedResult.groups[0].durationSamples == 0
                        && groupedResult.groups[1].durationSamples == 10,
           "zero-and-nonzero-groups");

    auto mixedRamp = records(400, 0, 1);
    auto mixedRecord = record(400, 10, 2);
    mixedRamp.push_back(mixedRecord);
    SceneObjectStreamGrouper mixed;
    const auto mixedResult = mixed.append(mixedRamp, context());
    expect(&report, mixedResult.disposition == SceneAdapterDisposition::Unsupported
                        && mixedResult.failedRecordIndex == 1U
                        && mixedResult.groups.empty(),
           "same-timestamp-different-ramp-unsupported");

    auto duplicate = records(500, 0, 2);
    duplicate[1].objectIndex = 1U;
    duplicate[1].state.objectIndex = 1U;
    SceneObjectStreamGrouper duplicateGrouper;
    const auto duplicateResult = duplicateGrouper.append(duplicate, context());
    expect(&report, duplicateResult.disposition == SceneAdapterDisposition::InvalidInput
                        && duplicateResult.groups.empty()
                        && duplicateResult.failedGroupIndex == 0U,
           "duplicate-object-id-no-partial-output");

    auto descending = records(600, 0, 2);
    descending[1].objectIndex = 0U;
    descending[1].state.objectIndex = 0U;
    SceneObjectStreamGrouper descendingGrouper;
    const auto descendingResult = descendingGrouper.append(descending, context());
    expect(&report, descendingResult.disposition == SceneAdapterDisposition::InvalidInput
                        && descendingResult.groups.empty(),
           "descending-object-id-rejected");

    auto lateInvalid = records(700, 0, 1);
    auto extent = record(800, 0, 1);
    extent.state.raw.sizePresent = true;
    extent.state.raw.sizeIndex = 2U;
    extent.state.effectiveSizePresent = true;
    extent.state.effectiveSizeIndex = 2U;
    extent.state.size = {1.0F, 1.0F, 1.0F};
    lateInvalid.push_back(extent);
    SceneObjectStreamGrouper lateGrouper;
    const auto lateResult = lateGrouper.append(lateInvalid, context());
    expect(&report, lateResult.disposition == SceneAdapterDisposition::Unsupported
                        && lateResult.groups.empty()
                        && lateResult.failedGroupIndex == 1U,
           "late-invalid-group-transactional");

    auto regression = records(900, 0, 1);
    regression.push_back(record(899, 0, 2));
    SceneObjectStreamGrouper regressionGrouper;
    const auto regressionResult = regressionGrouper.append(regression, context());
    expect(&report, regressionResult.disposition == SceneAdapterDisposition::InvalidInput
                        && regressionResult.groups.empty(),
           "timestamp-regression-no-partial-output");

    SceneObjectStreamGrouper crossCall;
    expect(&report, crossCall.append(records(1000, 0, 1), context()).disposition
                        == SceneAdapterDisposition::Accepted
                        && crossCall.append(records(1000, 0, 1), context()).disposition
                            == SceneAdapterDisposition::InvalidInput,
           "timestamp-regression-across-calls");
    expect(&report, crossCall.append({}, context()).disposition
                        == SceneAdapterDisposition::InvalidInput,
           "empty-input-rejected");
    auto changedContext = context();
    changedContext.coordinatePolicy.roomWidthMetres = 11.0;
    expect(&report, crossCall.append(records(1001, 0, 1), changedContext).disposition
                        == SceneAdapterDisposition::InvalidInput,
           "coordinate-policy-change-across-calls");

    SceneObjectStreamGrouper generation;
    expect(&report, generation.append(records(1100, 0, 1), context(2)).disposition
                        == SceneAdapterDisposition::Stale
                        && generation.reset(2).disposition == SceneAdapterDisposition::Accepted
                        && generation.append(records(1100, 0, 1), context(2)).disposition
                            == SceneAdapterDisposition::Accepted
                        && generation.reset(2).disposition == SceneAdapterDisposition::Stale,
           "generation-reset-and-stale");

    SceneObjectStreamGrouper causalGrouper;
    const auto causalGroups = causalGrouper.append(twoGroups, context());
    SceneObjectRampScheduler scheduler;
    std::size_t next = 0U;
    auto notYet = applyNextCausalGroup(causalGroups.groups, &next, 199, &scheduler);
    const auto afterNotYet = next;
    auto firstReady = applyNextCausalGroup(causalGroups.groups, &next, 200, &scheduler);
    const auto afterFirst = next;
    auto secondReady = applyNextCausalGroup(causalGroups.groups, &next, 300, &scheduler);
    const auto afterSecond = next;
    expect(&report, notYet.disposition == SceneAdapterDisposition::Accepted
                        && !notYet.applied && afterNotYet == 0U
                        && firstReady.applied && afterFirst == 1U
                        && secondReady.applied && afterSecond == 2U,
           "causal-feed-no-future-apply");
    auto exhausted = applyNextCausalGroup(causalGroups.groups, &next, 400, &scheduler);
    expect(&report, exhausted.disposition == SceneAdapterDisposition::InvalidInput
                        && !exhausted.applied, "causal-feed-exhausted");
    std::size_t failedIndex = 0U;
    SceneObjectRampScheduler staleScheduler;
    staleScheduler.reset(2);
    const auto failedApply = applyNextCausalGroup(causalGroups.groups, &failedIndex,
                                                  200, &staleScheduler);
    expect(&report, failedApply.disposition == SceneAdapterDisposition::Stale
                        && !failedApply.applied && failedIndex == 0U,
           "causal-apply-failure-does-not-consume");
    const auto negativeSample = applyNextCausalGroup(causalGroups.groups, &failedIndex,
                                                     -1, &scheduler);
    expect(&report, negativeSample.disposition == SceneAdapterDisposition::InvalidInput
                        && !negativeSample.applied && failedIndex == 0U,
           "causal-negative-current-sample-rejected");

    std::cout << "StreamGrouperSelfTest="
              << (report.failures == 0 ? "PASS" : "FAIL")
              << " cases=" << report.cases
              << " failures=" << report.failures << '\n';
    return report.failures == 0 ? 0 : 1;
}
