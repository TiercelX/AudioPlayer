#include "scene-model.h"

#include <cmath>
#include <cstdint>
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

eac3render::RenderScene makeScene()
{
    eac3render::RenderScene scene;
    std::string reason;
    scene.addBed(eac3render::RenderElement::bed(
                    eac3render::BedLabel::FL, 0U, 0, 1536), &reason);
    scene.addBed(eac3render::RenderElement::bed(
                    eac3render::BedLabel::FR, 1U, 0, 1536), &reason);
    scene.addObject(eac3render::RenderElement::object(42U, 2U, 0, 1536), &reason);
    scene.appendMetadata(eac3render::MetadataPoint {
                             1U, 42U, 0, 0.75F, {0.25F, -0.1F, 0.5F}},
                         &reason);
    return scene;
}

SelfTestReport runSelfTest()
{
    SelfTestReport report;
    std::string reason;

    const eac3render::RenderScene first = makeScene();
    const eac3render::RenderScene second = makeScene();
    expect(&report, first.listenerPose().isIdentity(), "identity-listener-pose");
    expect(&report, first.snapshot() == second.snapshot(), "deterministic-snapshot");
    expect(&report, first.snapshot().find("bed=FL") != std::string::npos,
           "fixed-bed-label");
    expect(&report, first.snapshot().find("object=42") != std::string::npos,
           "object-id-timestamp");

    eac3render::RenderScene duplicateObject;
    expect(&report, duplicateObject.addObject(
                       eac3render::RenderElement::object(7U, 0U, 0, 10), &reason),
           "first-object-accepted");
    expect(&report, !duplicateObject.addObject(
                        eac3render::RenderElement::object(7U, 0U, 10, 10), &reason)
                    && reason == "duplicate-object-id",
           "duplicate-object-rejected");

    eac3render::RenderScene duplicateBed;
    expect(&report, duplicateBed.addBed(
                       eac3render::RenderElement::bed(
                           eac3render::BedLabel::FC, 0U, 0, 10), &reason),
           "first-bed-accepted");
    expect(&report, !duplicateBed.addBed(
                        eac3render::RenderElement::bed(
                            eac3render::BedLabel::FC, 1U, 10, 10), &reason)
                    && reason == "duplicate-bed-label",
           "duplicate-bed-label-rejected");

    eac3render::RenderScene invalid;
    expect(&report, !invalid.addObject(
                        eac3render::RenderElement::object(1U, 0U, -1, 10), &reason)
                    && reason == "element-invalid-time-range",
           "negative-timestamp-rejected");
    expect(&report, !invalid.addObject(
                        eac3render::RenderElement::object(2U, 0U, 0, 0), &reason)
                    && reason == "element-invalid-time-range",
           "zero-duration-rejected");
    expect(&report, !invalid.addObject(
                        eac3render::RenderElement::object(
                            3U, 0U, std::numeric_limits<std::int64_t>::max(), 1),
                        &reason)
                    && reason == "element-invalid-time-range",
           "timestamp-overflow-rejected");

    eac3render::RenderScene nonFinite;
    expect(&report, nonFinite.addObject(
                       eac3render::RenderElement::object(9U, 0U, 0, 10), &reason),
           "metadata-object-accepted");
    eac3render::MetadataPoint nanPoint {
        1U, 9U, 0, std::numeric_limits<float>::quiet_NaN(), {0.0F, 0.0F, 0.0F}};
    expect(&report, !nonFinite.appendMetadata(nanPoint, &reason)
                    && reason == "metadata-non-finite-value",
           "nan-metadata-rejected");
    eac3render::MetadataPoint outsidePoint {1U, 10U, 0, 1.0F, {0.0F, 0.0F, 0.0F}};
    expect(&report, !nonFinite.appendMetadata(outsidePoint, &reason)
                    && reason == "metadata-object-not-in-scene",
           "unknown-object-metadata-rejected");

    eac3render::RenderScene metadataRange;
    expect(&report, metadataRange.addObject(
                       eac3render::RenderElement::object(11U, 0U, 100, 10), &reason),
           "metadata-range-object-accepted");
    expect(&report, !metadataRange.appendMetadata(
                        eac3render::MetadataPoint {
                            1U, 11U, 99, 1.0F, {0.0F, 0.0F, 0.0F}},
                        &reason)
                    && reason == "metadata-timestamp-out-of-range",
           "metadata-before-object-rejected");
    expect(&report, metadataRange.appendMetadata(
                       eac3render::MetadataPoint {
                           1U, 11U, 100, 1.0F, {0.0F, 0.0F, 0.0F}},
                       &reason),
           "metadata-at-object-start-accepted");
    expect(&report, !metadataRange.appendMetadata(
                        eac3render::MetadataPoint {
                            1U, 11U, 110, 1.0F, {0.0F, 0.0F, 0.0F}},
                        &reason)
                    && reason == "metadata-timestamp-out-of-range",
           "metadata-at-object-end-rejected");

    eac3render::RenderScene invalidGeneration(0U);
    expect(&report, !invalidGeneration.addObject(
                        eac3render::RenderElement::object(12U, 0U, 0, 10, 0U),
                        &reason)
                    && reason == "scene-invalid-generation",
           "zero-scene-generation-rejected");
    expect(&report, !invalidGeneration.appendMetadata(
                        eac3render::MetadataPoint {
                            0U, 12U, 0, 1.0F, {0.0F, 0.0F, 0.0F}},
                        &reason)
                    && reason == "scene-invalid-generation",
           "zero-scene-metadata-rejected");
    expect(&report, invalidGeneration.reset(1U, &reason),
           "zero-scene-generation-recovered-by-reset");
    expect(&report, invalidGeneration.addObject(
                       eac3render::RenderElement::object(12U, 0U, 0, 10, 1U),
                       &reason),
           "recovered-scene-generation-accepted");

    eac3render::MetadataTimeline invalidTimeline(0U);
    expect(&report, !invalidTimeline.append(
                        eac3render::MetadataPoint {
                            0U, 12U, 0, 1.0F, {0.0F, 0.0F, 0.0F}},
                        &reason)
                    && reason == "metadata-invalid-generation",
           "zero-timeline-generation-rejected");
    expect(&report, invalidTimeline.reset(1U, &reason),
           "zero-timeline-generation-recovered-by-reset");

    eac3render::RenderScene lifecycle = makeScene();
    const std::string oldSnapshot = lifecycle.snapshot();
    expect(&report, lifecycle.reset(2U, &reason), "reset-advances-generation");
    expect(&report, lifecycle.generation() == 2U
                    && lifecycle.elements().empty()
                    && lifecycle.metadata().size() == 0U
                    && lifecycle.listenerPose().isIdentity(),
           "reset-clears-old-generation");
    expect(&report, lifecycle.snapshot() != oldSnapshot, "reset-changes-snapshot");
    expect(&report, !lifecycle.addObject(
                        eac3render::RenderElement::object(42U, 2U, 0, 10, 1U), &reason)
                    && reason == "element-generation-mismatch",
           "stale-element-rejected");
    expect(&report, lifecycle.addObject(
                       eac3render::RenderElement::object(42U, 2U, 0, 10, 2U), &reason),
           "fresh-generation-accepted");
    expect(&report, !lifecycle.reset(2U, &reason)
                    && reason == "scene-generation-not-advanced",
           "same-generation-reset-rejected");

    return report;
}

} // namespace

int main()
{
    const SelfTestReport report = runSelfTest();
    std::cout << "sceneModelSelfTest=" << (report.pass ? "PASS" : "FAIL")
              << " cases=" << report.cases
              << " reason=" << (report.reason.empty() ? "none" : report.reason)
              << '\n';
    std::cout << "sceneModelResult=" << (report.pass ? "PASS" : "FAIL")
              << " evidenceLimit=offline-scene-contract-only\n";
    return report.pass ? 0 : 1;
}
