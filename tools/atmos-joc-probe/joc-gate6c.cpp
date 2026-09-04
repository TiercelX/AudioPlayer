#include "joc-gate6c.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>

namespace eac3gate6c {
namespace {

bool finite(float value) { return std::isfinite(value); }

bool decodeSampleOffsetInternal(const eac3oamd::B2aFrame &frame,
                                std::int64_t *samples, std::string *reason)
{
    if (!samples || !reason) return false;
    *samples = 0;
    switch (frame.sampleOffsetCode) {
    case 0U:
        if (frame.sampleOffsetIndexPresent || frame.sampleOffsetBitsPresent) {
            *reason = "sample-offset-code0-with-payload";
            return false;
        }
        return true;
    case 1U: {
        static constexpr std::array<unsigned, 4> kIndex = {8U, 16U, 18U, 24U};
        if (!frame.sampleOffsetIndexPresent || frame.sampleOffsetBitsPresent
            || frame.sampleOffsetIndex >= kIndex.size()) {
            *reason = "sample-offset-index-inconsistent";
            return false;
        }
        *samples = kIndex[frame.sampleOffsetIndex];
        return true;
    }
    case 2U:
        if (!frame.sampleOffsetBitsPresent || frame.sampleOffsetIndexPresent
            || frame.sampleOffsetBits > 31U) {
            *reason = "sample-offset-bits-inconsistent";
            return false;
        }
        *samples = frame.sampleOffsetBits;
        return true;
    default:
        *reason = "reserved-sample-offset-code";
        return false;
    }
}

bool decodeRampDurationInternal(const eac3oamd::B2aBlockUpdate &block,
                                unsigned *samples, std::string *reason)
{
    if (!samples || !reason) return false;
    *samples = 0;
    switch (block.rampDurationCode) {
    case 0U:
        if (block.rampDurationIndexPresent || block.rampDurationBitsPresent) {
            *reason = "fixed-ramp-duration-with-payload";
            return false;
        }
        return true;
    case 1U:
        if (block.rampDurationIndexPresent || block.rampDurationBitsPresent) {
            *reason = "fixed-ramp-duration-with-payload";
            return false;
        }
        *samples = 512U;
        return true;
    case 2U:
        if (block.rampDurationIndexPresent || block.rampDurationBitsPresent) {
            *reason = "fixed-ramp-duration-with-payload";
            return false;
        }
        *samples = 1536U;
        return true;
    case 3U: {
        static constexpr std::array<unsigned, 16> kIndex = {
            32U, 64U, 128U, 256U, 320U, 480U, 1000U, 1001U,
            1024U, 1600U, 1601U, 1602U, 1920U, 2000U, 2002U, 2048U};
        if (block.rampDurationIndexPresent == block.rampDurationBitsPresent) {
            *reason = "ramp-duration-selector-inconsistent";
            return false;
        }
        if (block.rampDurationIndexPresent) {
            if (block.rampDurationIndex >= kIndex.size()) {
                *reason = "ramp-duration-index-out-of-range";
                return false;
            }
            *samples = kIndex[block.rampDurationIndex];
            return true;
        }
        if (block.rampDurationBits > 2047U) {
            *reason = "ramp-duration-bits-out-of-range";
            return false;
        }
        *samples = block.rampDurationBits;
        return true;
    }
    default:
        *reason = "reserved-ramp-duration-code";
        return false;
    }
}

bool finiteQmf(const eac3joc::JocQmfFrame &qmf, std::string *reason)
{
    if (qmf.numObjects != kDynamicObjectCount
        || qmf.objects.size() != kDynamicObjectCount
        || qmf.numChannels == 0U) {
        *reason = "gate6c-qmf-object-shape-mismatch";
        return false;
    }
    for (const auto &object : qmf.objects) {
        if (object.size() != kSamplesPerUnit) {
            *reason = "gate6c-qmf-unit-size-mismatch";
            return false;
        }
        for (const auto &value : object) {
            if (!std::isfinite(value.real()) || !std::isfinite(value.imag())) {
                *reason = "gate6c-qmf-nonfinite";
                return false;
            }
        }
    }
    return true;
}

bool validUnit(const UnitInput &input, std::string *reason)
{
    if (!reason || !finiteQmf(input.qmf, reason)
        || input.lfe.size() != kSamplesPerUnit) {
        if (reason && reason->empty()) *reason = "gate6c-lfe-unit-size-mismatch";
        return false;
    }
    for (const float value : input.lfe) {
        if (!finite(value)) {
            *reason = "gate6c-lfe-nonfinite";
            return false;
        }
    }
    if (input.sourceStart < 0
        || input.sourceStart % static_cast<std::int64_t>(kSamplesPerUnit) != 0) {
        *reason = "gate6c-source-position-not-access-unit-aligned";
        return false;
    }
    std::int64_t previousPosition = input.sourceStart;
    unsigned previousBlock = 0;
    for (std::size_t index = 0; index < input.metadata.size(); ++index) {
        const MetadataUpdate &update = input.metadata[index];
        if (update.objectIndex == 0U || update.objectIndex > kDynamicObjectCount
            || update.sourcePosition < input.sourceStart
            || update.sourcePosition >= input.sourceStart
                + static_cast<std::int64_t>(kSamplesPerUnit)
            || (index != 0U && update.sourcePosition < previousPosition)
            || (index != 0U && update.blockIndex < previousBlock)) {
            *reason = "gate6c-metadata-order-or-object-map-invalid";
            return false;
        }
        previousPosition = update.sourcePosition;
        previousBlock = update.blockIndex;
    }
    return true;
}

eac3joc::JocSynthesisState cloneSynthesis(
    const eac3joc::JocSynthesisState &source)
{
    eac3joc::JocSynthesisState copy;
    copy.initialized = source.initialized;
    copy.numObjects = source.numObjects;
    copy.synthesis.reserve(source.synthesis.size());
    for (const auto &bank : source.synthesis) {
        copy.synthesis.push_back(bank
            ? std::make_unique<eac3qmf::Qmf>(*bank) : nullptr);
    }
    return copy;
}

bool appendMetadata(const UnitInput &input, std::int64_t selectedStart,
                    std::int64_t selectedEnd, std::int64_t batchStart,
                    std::int64_t batchEnd, std::vector<MetadataUpdate> *carried,
                    std::vector<MetadataUpdate> *latestPreTrim,
                    std::vector<bool> *latestPreTrimValid,
                    bool *preTrimBoundaryEmitted,
                    std::vector<MetadataUpdate> *output, std::string *reason)
{
    if (!carried || !latestPreTrim || !latestPreTrimValid
        || !preTrimBoundaryEmitted || !output || !reason
        || latestPreTrim->size() != kDynamicObjectCount + 1U
        || latestPreTrimValid->size() != kDynamicObjectCount + 1U) return false;
    std::vector<MetadataUpdate> pending = *carried;
    for (const MetadataUpdate &source : input.metadata) {
        MetadataUpdate update = source;
        update.sourcePosition += kCommonDelaySamples;
        if (update.sourcePosition < selectedStart) {
            if (update.objectIndex <= kDynamicObjectCount) {
                (*latestPreTrim)[update.objectIndex] = std::move(update);
                (*latestPreTrimValid)[update.objectIndex] = true;
            }
        } else {
            pending.push_back(std::move(update));
        }
    }
    std::vector<MetadataUpdate> retained;
    for (MetadataUpdate &update : pending) {
        if (update.sourcePosition >= selectedEnd) {
            continue;
        } else if (update.sourcePosition >= batchStart
                   && update.sourcePosition < batchEnd) {
            update.sourcePosition -= selectedStart;
            output->push_back(std::move(update));
        } else {
            retained.push_back(std::move(update));
        }
    }
    if (batchEnd > batchStart && batchEnd > selectedStart
        && !*preTrimBoundaryEmitted) {
        std::vector<MetadataUpdate> boundary;
        for (unsigned object = 1U; object <= kDynamicObjectCount; ++object) {
            if (!(*latestPreTrimValid)[object]) continue;
            MetadataUpdate update = (*latestPreTrim)[object];
            update.sourcePosition = 0;
            boundary.push_back(std::move(update));
            (*latestPreTrimValid)[object] = false;
        }
        output->insert(output->begin(), boundary.begin(), boundary.end());
        *preTrimBoundaryEmitted = true;
    }
    *carried = std::move(retained);
    return true;
}

} // namespace

bool decodeSampleOffset(const eac3oamd::B2aFrame &frame,
                        std::int64_t *samples, std::string *reason)
{
    return decodeSampleOffsetInternal(frame, samples, reason);
}

bool decodeRampDuration(const eac3oamd::B2aBlockUpdate &block,
                        unsigned *samples, std::string *reason)
{
    return decodeRampDurationInternal(block, samples, reason);
}

Assembler::Assembler(Config config) : config_(config) {}

bool Assembler::processUnit(const UnitInput &input, const Callback &callback,
                            std::string *reason)
{
    if (!reason || !callback) return false;
    std::string localReason;
    if (!validUnit(input, &localReason)) {
        *reason = localReason;
        ++report_.transactionalRejects;
        return false;
    }
    if (flushed_ || (haveUnit_ && input.unitIndex != nextUnitIndex_)
        || (haveUnit_ && input.sourceStart != nextSourceStart_)
        || (!haveUnit_ && (input.unitIndex != 0U || input.sourceStart != 0))) {
        *reason = "gate6c-unit-association-or-reset-gap";
        ++report_.transactionalRejects;
        return false;
    }
    if (config_.decodedSourceSamples <= 0 || config_.skipSamples < 0
        || config_.discardPadding < 0
        || config_.skipSamples + kCommonDelaySamples
            > config_.decodedSourceSamples - config_.discardPadding) {
        *reason = "gate6c-trim-interval-invalid";
        ++report_.transactionalRejects;
        return false;
    }
    const std::int64_t selectedStart = config_.skipSamples + kCommonDelaySamples;
    const std::int64_t selectedEnd = config_.decodedSourceSamples
        - config_.discardPadding + kCommonDelaySamples;
    // Synthesis returns one block on the output timeline [sourceStart,
    // sourceStart+1536).  Its algorithmic delay is represented by the
    // selected interval, not by relabelling this block's origin.
    const std::int64_t outputStart = input.sourceStart;
    const std::int64_t outputEnd = outputStart + kSamplesPerUnit;
    const std::int64_t overlapStart = std::max(selectedStart, outputStart);
    const std::int64_t overlapEnd = std::min(selectedEnd, outputEnd);

    if (config_.qwin.size() != eac3qmf::kFilterLength) {
        *reason = "gate6c-qmf-prototype-size-mismatch";
        ++report_.transactionalRejects;
        return false;
    }
    if (config_.callbackCapacity == 0U && overlapEnd > overlapStart) {
        *reason = "gate6c-callback-capacity-zero";
        ++report_.callbackRejects;
        return false;
    }

    eac3joc::JocSynthesisState stagedSynthesis = cloneSynthesis(synthesis_);
    eac3joc::JocSynthesisFrame synthesized;
    if (!eac3joc::synthesizeQmf(input.qmf, config_.qwin, &stagedSynthesis,
                                &synthesized, &localReason)) {
        *reason = localReason;
        ++report_.transactionalRejects;
        return false;
    }
    std::vector<float> stagedLfeDelay = lfeDelay_;
    if (stagedLfeDelay.size() != static_cast<std::size_t>(kCommonDelaySamples)) {
        stagedLfeDelay.assign(kCommonDelaySamples, 0.0f);
    }
    std::vector<float> delayedLfe(kSamplesPerUnit, 0.0f);
    std::copy(stagedLfeDelay.begin(), stagedLfeDelay.end(), delayedLfe.begin());
    std::copy(input.lfe.begin(),
              input.lfe.begin() + (kSamplesPerUnit - kCommonDelaySamples),
              delayedLfe.begin() + kCommonDelaySamples);
    stagedLfeDelay.assign(input.lfe.end() - kCommonDelaySamples, input.lfe.end());
    std::vector<MetadataUpdate> stagedCarried = carriedMetadata_;
    std::vector<MetadataUpdate> stagedLatestPreTrim = latestPreTrim_;
    std::vector<bool> stagedLatestPreTrimValid = latestPreTrimValid_;
    if (stagedLatestPreTrim.size() != kDynamicObjectCount + 1U) {
        stagedLatestPreTrim.resize(kDynamicObjectCount + 1U);
    }
    if (stagedLatestPreTrimValid.size() != kDynamicObjectCount + 1U) {
        stagedLatestPreTrimValid.assign(kDynamicObjectCount + 1U, false);
    }
    bool stagedPreTrimBoundaryEmitted = preTrimBoundaryEmitted_;
    Batch batch;
    batch.unitIndex = input.unitIndex;
    batch.sourceStart = input.sourceStart;
    batch.sourceEnd = input.sourceStart + kSamplesPerUnit;
    if (overlapEnd > overlapStart) {
        const std::size_t offset = static_cast<std::size_t>(overlapStart - outputStart);
        const std::size_t count = static_cast<std::size_t>(overlapEnd - overlapStart);
        batch.outputStart = overlapStart - selectedStart;
        batch.outputEnd = overlapEnd - selectedStart;
        batch.objects.resize(kDynamicObjectCount);
        for (unsigned object = 0; object < kDynamicObjectCount; ++object) {
            batch.objects[object].assign(
                synthesized.objects[object].begin() + offset,
                synthesized.objects[object].begin() + offset + count);
        }
        batch.lfe.assign(delayedLfe.begin() + offset,
                         delayedLfe.begin() + offset + count);
        if (!appendMetadata(input, selectedStart, selectedEnd, overlapStart,
                            overlapEnd,
                            &stagedCarried, &stagedLatestPreTrim,
                            &stagedLatestPreTrimValid, &stagedPreTrimBoundaryEmitted,
                            &batch.metadata, &localReason)) {
            *reason = localReason;
            ++report_.transactionalRejects;
            return false;
        }
        if (!callback(batch)) {
            *reason = "gate6c-callback-rejected-batch";
            ++report_.callbackRejects;
            return false;
        }
        ++report_.batchesEmitted;
        report_.metadataEmitted += batch.metadata.size();
    } else {
        // Still consume metadata before the selected interval so its last
        // state can be carried to the first emitted batch.
        if (!appendMetadata(input, selectedStart, selectedEnd, outputStart,
                            outputEnd,
                            &stagedCarried, &stagedLatestPreTrim,
                            &stagedLatestPreTrimValid, &stagedPreTrimBoundaryEmitted,
                            &batch.metadata, &localReason)) {
            *reason = localReason;
            ++report_.transactionalRejects;
            return false;
        }
    }
    synthesis_ = std::move(stagedSynthesis);
    lfeDelay_ = std::move(stagedLfeDelay);
    carriedMetadata_ = std::move(stagedCarried);
    latestPreTrim_ = std::move(stagedLatestPreTrim);
    latestPreTrimValid_ = std::move(stagedLatestPreTrimValid);
    preTrimBoundaryEmitted_ = stagedPreTrimBoundaryEmitted;
    haveUnit_ = true;
    nextUnitIndex_ = input.unitIndex + 1U;
    nextSourceStart_ = input.sourceStart + kSamplesPerUnit;
    ++report_.unitsAccepted;
    return true;
}

bool Assembler::flush(const Callback &callback, std::string *reason)
{
    if (!reason || !callback || !haveUnit_ || flushed_) return false;
    eac3joc::JocSynthesisState staged = cloneSynthesis(synthesis_);
    eac3joc::JocQmfFrame zero;
    zero.numChannels = 1U;
    zero.numObjects = kDynamicObjectCount;
    zero.objects.assign(kDynamicObjectCount,
                        std::vector<eac3qmf::Complex>(kSamplesPerUnit,
                                                       eac3qmf::Complex {0.0, 0.0}));
    eac3joc::JocSynthesisFrame synthesized;
    std::string localReason;
    if (!eac3joc::synthesizeQmf(zero, config_.qwin, &staged,
                                &synthesized, &localReason)) {
        *reason = localReason;
        ++report_.transactionalRejects;
        return false;
    }
    const std::int64_t selectedStart = config_.skipSamples + kCommonDelaySamples;
    const std::int64_t selectedEnd = config_.decodedSourceSamples
        - config_.discardPadding + kCommonDelaySamples;
    const std::int64_t outputStart = nextSourceStart_;
    const std::int64_t outputEnd = outputStart + kSamplesPerUnit;
    const std::int64_t overlapStart = std::max(selectedStart, outputStart);
    const std::int64_t overlapEnd = std::min(selectedEnd, outputEnd);
    std::vector<MetadataUpdate> stagedCarried;
    stagedCarried.reserve(carriedMetadata_.size());
    Batch batch;
    batch.unitIndex = nextUnitIndex_;
    batch.flush = true;
    batch.sourceStart = nextSourceStart_;
    batch.sourceEnd = nextSourceStart_;
    if (overlapEnd > overlapStart) {
        if (config_.callbackCapacity == 0U) {
            *reason = "gate6c-callback-capacity-zero";
            ++report_.callbackRejects;
            return false;
        }
        const std::size_t offset = static_cast<std::size_t>(overlapStart - outputStart);
        const std::size_t count = static_cast<std::size_t>(overlapEnd - overlapStart);
        batch.outputStart = overlapStart - selectedStart;
        batch.outputEnd = overlapEnd - selectedStart;
        batch.objects.resize(kDynamicObjectCount);
        for (unsigned object = 0; object < kDynamicObjectCount; ++object) {
            batch.objects[object].assign(synthesized.objects[object].begin() + offset,
                                         synthesized.objects[object].begin() + offset + count);
        }
        std::vector<float> delayedLfe(kSamplesPerUnit, 0.0f);
        if (lfeDelay_.size() == static_cast<std::size_t>(kCommonDelaySamples)) {
            std::copy(lfeDelay_.begin(), lfeDelay_.end(), delayedLfe.begin());
        }
        batch.lfe.assign(delayedLfe.begin() + offset,
                         delayedLfe.begin() + offset + count);
        for (const MetadataUpdate &pending : carriedMetadata_) {
            if (pending.sourcePosition >= selectedEnd) {
                continue;
            }
            if (pending.sourcePosition >= overlapStart
                && pending.sourcePosition < overlapEnd) {
                MetadataUpdate update = pending;
                update.sourcePosition -= selectedStart;
                batch.metadata.push_back(std::move(update));
            } else {
                stagedCarried.push_back(pending);
            }
        }
        if (!std::is_sorted(batch.metadata.begin(), batch.metadata.end(),
                            [](const MetadataUpdate &left, const MetadataUpdate &right) {
                                return left.sourcePosition < right.sourcePosition;
                            })) {
            *reason = "gate6c-flush-metadata-order-invalid";
            ++report_.transactionalRejects;
            return false;
        }
        if (!callback(batch)) {
            *reason = "gate6c-callback-rejected-flush";
            ++report_.callbackRejects;
            return false;
        }
        ++report_.batchesEmitted;
        report_.metadataEmitted += batch.metadata.size();
    } else {
        for (const MetadataUpdate &pending : carriedMetadata_) {
            if (pending.sourcePosition < selectedEnd) stagedCarried.push_back(pending);
        }
    }
    synthesis_ = std::move(staged);
    lfeDelay_.assign(kCommonDelaySamples, 0.0f);
    carriedMetadata_ = std::move(stagedCarried);
    flushed_ = true;
    ++report_.flushes;
    return true;
}

void Assembler::reset()
{
    synthesis_.reset();
    haveUnit_ = false;
    flushed_ = false;
    nextUnitIndex_ = 0;
    nextSourceStart_ = 0;
    lfeDelay_.clear();
    carriedMetadata_.clear();
    latestPreTrim_.clear();
    latestPreTrimValid_.clear();
    preTrimBoundaryEmitted_ = false;
    report_ = Report {};
}

Report runSelfTest(const std::vector<double> &qwin)
{
    Report report;
    auto check = [&report](bool pass, const char *reason, std::size_t *counter) {
        ++report.cases;
        if (counter) ++*counter;
        if (!pass && report.reason.empty()) report.reason = reason;
    };
    auto makeUnit = [](std::size_t index, std::int64_t sourceStart,
                       unsigned blockCount = 1U) {
        UnitInput unit;
        unit.unitIndex = index;
        unit.sourceStart = sourceStart;
        unit.qmf.numChannels = 5U;
        unit.qmf.numObjects = kDynamicObjectCount;
        unit.qmf.objects.assign(kDynamicObjectCount,
                                std::vector<eac3qmf::Complex>(kSamplesPerUnit,
                                    eac3qmf::Complex {0.0, 0.0}));
        for (unsigned object = 0; object < kDynamicObjectCount; ++object) {
            unit.qmf.objects[object][object] =
                eac3qmf::Complex {static_cast<double>(object + 1U) * 0.01, 0.0};
        }
        unit.lfe.assign(kSamplesPerUnit, 0.125f);
        unit.metadata.reserve(kDynamicObjectCount);
        for (unsigned object = 0; object < kDynamicObjectCount; ++object) {
            MetadataUpdate update;
            update.sourcePosition = sourceStart
                + static_cast<std::int64_t>(std::min(object, blockCount - 1U) * 32U);
            update.blockIndex = std::min(object, blockCount - 1U);
            update.rampDuration = update.blockIndex + 1U;
            update.objectIndex = object + 1U;
            update.state.objectIndex = object + 1U;
            update.state.active = true;
            update.state.basicValid = true;
            update.state.renderValid = true;
            unit.metadata.push_back(update);
        }
        return unit;
    };
    auto finiteBatch = [](const Batch &batch) {
        if (batch.objects.size() != kDynamicObjectCount
            || batch.lfe.size() == 0U) return false;
        for (const auto &object : batch.objects) {
            if (object.size() != batch.lfe.size()
                || !std::all_of(object.begin(), object.end(), finite)) return false;
        }
        return std::all_of(batch.lfe.begin(), batch.lfe.end(), finite);
    };
    Config config;
    config.qwin = qwin;
    config.decodedSourceSamples = 3072;
    config.callbackCapacity = 1U;
    Assembler assembler(config);
    std::vector<Batch> batches;
    const Callback collect = [&batches](const Batch &batch) {
        batches.push_back(batch);
        return true;
    };
    const UnitInput unit0 = makeUnit(0U, 0);
    const UnitInput unit1 = makeUnit(1U, 1536);
    std::string reason;
    const bool accepted0 = assembler.processUnit(unit0, collect, &reason);
    const bool accepted1 = assembler.processUnit(unit1, collect, &reason);
    check(accepted0 && accepted1 && batches.size() == 2U
              && finiteBatch(batches[0]) && finiteBatch(batches[1])
              && batches[0].objects.size() == 15U
              && batches[0].lfe.size() == batches[0].objects[0].size(),
          "identity-15-plus-lfe-finite", &report.mappingCases);
    bool objectMap = true;
    std::size_t metadataCount = 0;
    for (const Batch &batch : batches) {
        metadataCount += batch.metadata.size();
        for (const MetadataUpdate &update : batch.metadata) {
            objectMap = objectMap && update.objectIndex >= 1U
                && update.objectIndex <= kDynamicObjectCount;
        }
    }
    check(objectMap && metadataCount == 30U, "metadata-object-map", &report.metadataCases);
    check(batches[0].outputStart == 0 && batches[0].outputEnd == 959
              && batches[1].outputStart == 959 && batches[1].outputEnd == 2495,
          "common-577-timeline", &report.timingCases);
    const bool flushOk = assembler.flush(collect, &reason);
    check(flushOk && assembler.report().flushes == 1U
              && batches.size() == 3U
              && batches.back().flush && batches.back().objects[0].size() == 577U
              && batches.back().lfe.size() == 577U
              && batches.back().outputStart == 2495
              && batches.back().outputEnd == 3072
              && batches.back().metadata.empty(),
          "zero-qmf-tail-flush-without-metadata", &report.flushCases);

    Config lateConfig = config;
    lateConfig.decodedSourceSamples = 1536;
    Assembler lateAssembler(lateConfig);
    UnitInput lateUnit = makeUnit(0U, 0);
    lateUnit.metadata.clear();
    MetadataUpdate lateUpdate;
    lateUpdate.sourcePosition = 1023;
    lateUpdate.objectIndex = 1U;
    lateUpdate.state.objectIndex = 1U;
    lateUpdate.state.active = true;
    lateUpdate.state.basicValid = true;
    lateUpdate.state.renderValid = true;
    lateUnit.metadata.push_back(lateUpdate);
    std::vector<Batch> lateBatches;
    const Callback collectLate = [&lateBatches](const Batch &batch) {
        lateBatches.push_back(batch);
        return true;
    };
    check(lateAssembler.processUnit(lateUnit, collectLate, &reason)
              && lateAssembler.flush(collectLate, &reason)
              && lateBatches.size() == 2U
              && lateBatches[0].metadata.empty()
              && lateBatches[1].metadata.size() == 1U
              && lateBatches[1].metadata[0].objectIndex == 1U
              && lateBatches[1].metadata[0].sourcePosition == 1023,
          "flush-carries-late-metadata", &report.metadataCases);

    std::size_t totalSamples = 0;
    bool impulsePresent = false;
    for (const Batch &batch : batches) {
        totalSamples += batch.lfe.size();
        impulsePresent = impulsePresent || std::any_of(
            batch.objects[0].begin(), batch.objects[0].end(),
            [](float value) { return std::abs(value) > 1.0e-7f; });
    }
    check(totalSamples == kSamplesPerUnit * 2U && impulsePresent,
          "delayed-object-and-lfe-impulse-accounting", &report.timingCases);

    UnitInput sourceImpulse = makeUnit(0U, 0);
    sourceImpulse.qmf.objects.assign(
        kDynamicObjectCount,
        std::vector<eac3qmf::Complex>(kSamplesPerUnit, eac3qmf::Complex {0.0, 0.0}));
    std::vector<float> impulsePcm(kSamplesPerUnit, 0.0f);
    impulsePcm[0] = 1.0f;
    eac3qmf::Qmf impulseBank(qwin);
    for (std::size_t slot = 0; slot < 24U; ++slot) {
        const auto values = impulseBank.analyzeBlock(
            impulsePcm.data() + slot * eac3qmf::kSubbands);
        std::copy(values.begin(), values.end(),
                  sourceImpulse.qmf.objects[0].begin()
                      + slot * eac3qmf::kSubbands);
    }
    sourceImpulse.lfe.assign(kSamplesPerUnit, 0.0f);
    sourceImpulse.lfe[0] = 1.0f;
    Config impulseConfig = config;
    impulseConfig.decodedSourceSamples = 1536;
    Assembler impulseAssembler(impulseConfig);
    std::vector<Batch> impulseBatches;
    const Callback collectImpulse = [&impulseBatches](const Batch &batch) {
        impulseBatches.push_back(batch);
        return true;
    };
    const bool impulseChain = impulseAssembler.processUnit(
        sourceImpulse, collectImpulse, &reason)
        && impulseAssembler.flush(collectImpulse, &reason);
    check(impulseChain && impulseBatches.size() == 2U
              && impulseBatches[0].objects[0].size() == 959U
              && impulseBatches[0].lfe.size() == 959U
              && std::abs(impulseBatches[0].objects[0][0]) > 1.0e-5f
              && std::abs(impulseBatches[0].lfe[0] - 1.0f) < 1.0e-6f
              && impulseBatches[1].objects[0].size() == 577U
              && impulseBatches[1].lfe.size() == 577U,
          "source-impulse-common-delay-alignment", &report.timingCases);

    Config eightConfig = config;
    eightConfig.decodedSourceSamples = 1536;
    Assembler eight(eightConfig);
    UnitInput eightUnit = makeUnit(0U, 0, 8U);
    eightUnit.metadata.clear();
    const std::array<unsigned, 8> syntheticRamps = {
        0U, 512U, 1536U, 32U, 64U, 128U, 256U, 320U};
    for (unsigned block = 0U; block < 8U; ++block) {
        for (unsigned object = 1U; object <= kDynamicObjectCount; ++object) {
            MetadataUpdate update;
            update.sourcePosition = static_cast<std::int64_t>(block) * 32;
            update.blockIndex = block;
            update.rampDuration = syntheticRamps[block];
            update.objectIndex = object;
            update.state.objectIndex = object;
            update.state.active = true;
            update.state.basicValid = true;
            update.state.renderValid = true;
            eightUnit.metadata.push_back(update);
        }
    }
    std::vector<Batch> eightBatches;
    const Callback collectEight = [&eightBatches](const Batch &batch) {
        eightBatches.push_back(batch);
        return true;
    };
    check(eight.processUnit(eightUnit, collectEight, &reason)
              && eightBatches.size() == 1U
              && eightBatches[0].metadata.size() == 120U
              && eightBatches[0].metadata.front().blockIndex == 0U
              && eightBatches[0].metadata.back().blockIndex == 7U
              && eightBatches[0].metadata.front().rampDuration == 0U
              && eightBatches[0].metadata[15].rampDuration == 512U
              && eightBatches[0].metadata[30].rampDuration == 1536U
              && eightBatches[0].metadata[105].rampDuration == 320U,
          "eight-metadata-blocks-and-ramp", &report.metadataCases);

    Config trimConfig = config;
    trimConfig.skipSamples = 10;
    Assembler trimmed(trimConfig);
    std::vector<Batch> trimmedBatches;
    const Callback collectTrim = [&trimmedBatches](const Batch &batch) {
        trimmedBatches.push_back(batch);
        return true;
    };
    check(trimmed.processUnit(unit0, collectTrim, &reason)
              && trimmed.processUnit(unit1, collectTrim, &reason)
              && trimmed.flush(collectTrim, &reason)
              && trimmedBatches.size() == 3U
              && trimmedBatches[0].outputStart == 0
              && trimmedBatches[0].objects[0].size() == 949U
              && !trimmedBatches[0].metadata.empty()
              && trimmedBatches[0].metadata.front().sourcePosition == 0,
          "leading-trim-carried-metadata", &report.timingCases);

    UnitInput sparseUnit1 = unit1;
    sparseUnit1.metadata.erase(
        std::remove_if(sparseUnit1.metadata.begin(), sparseUnit1.metadata.end(),
                       [](const MetadataUpdate &update) {
                           return update.objectIndex != 2U;
                       }),
        sparseUnit1.metadata.end());
    Config sparseConfig = config;
    sparseConfig.skipSamples = 1600;
    Assembler sparse(sparseConfig);
    std::vector<Batch> sparseBatches;
    const Callback collectSparse = [&sparseBatches](const Batch &batch) {
        sparseBatches.push_back(batch);
        return true;
    };
    bool sparseBoundary = false;
    check(sparse.processUnit(unit0, collectSparse, &reason)
              && sparse.processUnit(sparseUnit1, collectSparse, &reason)
              && !sparseBatches.empty(),
          "sparse-pretrim-state-staging", &report.metadataCases);
    if (!sparseBatches.empty()) {
        for (const MetadataUpdate &update : sparseBatches.front().metadata) {
            sparseBoundary = sparseBoundary
                || (update.objectIndex == 1U && update.sourcePosition == 0);
        }
    }
    check(sparseBoundary, "sparse-pretrim-boundary-state", &report.metadataCases);

    Assembler callbackState(config);
    bool rejectNext = true;
    const Callback rejectOnce = [&rejectNext](const Batch &) {
        if (rejectNext) { rejectNext = false; return false; }
        return true;
    };
    check(!callbackState.processUnit(unit0, rejectOnce, &reason)
              && callbackState.processUnit(unit0, rejectOnce, &reason)
              && callbackState.report().callbackRejects == 1U,
          "callback-rejection-transaction", &report.callbackCases);

    UnitInput nonfinite = unit0;
    nonfinite.qmf.objects[4][9] = eac3qmf::Complex {
        std::numeric_limits<double>::quiet_NaN(), 0.0};
    Assembler transactionState(config);
    std::vector<Batch> transactionBatches;
    const Callback collectTransaction = [&transactionBatches](const Batch &batch) {
        transactionBatches.push_back(batch);
        return true;
    };
    check(!transactionState.processUnit(nonfinite, collectTransaction, &reason)
              && transactionState.processUnit(unit0, collectTransaction, &reason)
              && transactionBatches.size() == 1U,
          "nonfinite-transactional-rejection", &report.transactionalRejects);

    UnitInput gap = makeUnit(2U, 3072);
    Assembler gapState(config);
    check(!gapState.processUnit(gap, collectTransaction, &reason)
              && gapState.processUnit(unit0, collectTransaction, &reason),
          "unit-association-rejection", &report.transactionalRejects);

    Assembler resetState(config);
    std::vector<Batch> resetBatches;
    const Callback collectReset = [&resetBatches](const Batch &batch) {
        resetBatches.push_back(batch);
        return true;
    };
    resetState.processUnit(unit0, collectReset, &reason);
    resetState.reset();
    resetBatches.clear();
    Assembler freshState(config);
    std::vector<Batch> freshBatches;
    const Callback collectFresh = [&freshBatches](const Batch &batch) {
        freshBatches.push_back(batch);
        return true;
    };
    const bool resetEquivalent = resetState.processUnit(unit0, collectReset, &reason)
        && freshState.processUnit(unit0, collectFresh, &reason)
        && resetBatches.size() == freshBatches.size()
        && resetBatches[0].objects == freshBatches[0].objects;
    check(resetEquivalent, "reset-fresh-equivalence", &report.resetCases);

    Config terminalConfig = config;
    terminalConfig.discardPadding = 100;
    Assembler terminal(terminalConfig);
    std::vector<Batch> terminalBatches;
    const Callback collectTerminal = [&terminalBatches](const Batch &batch) {
        terminalBatches.push_back(batch);
        return true;
    };
    std::size_t terminalSamples = 0;
    const bool terminalChain = terminal.processUnit(unit0, collectTerminal, &reason)
        && terminal.processUnit(unit1, collectTerminal, &reason)
        && terminal.flush(collectTerminal, &reason);
    for (const Batch &batch : terminalBatches) terminalSamples += batch.lfe.size();
    check(terminalChain && terminalSamples == 3072U - 100U
              && terminalBatches.back().lfe.size() == 477U,
          "terminal-discard-exact-length", &report.timingCases);

    UnitInput misaligned = unit0;
    misaligned.sourceStart = 1;
    Assembler timingState(config);
    check(!timingState.processUnit(misaligned, collectTransaction, &reason)
              && timingState.processUnit(unit0, collectTransaction, &reason),
          "source-timing-alignment-rejection", &report.transactionalRejects);

    bool sampleOffsets = true;
    eac3oamd::B2aFrame offsetFrame;
    std::int64_t offsetSamples = -1;
    offsetFrame.sampleOffsetCode = 0U;
    sampleOffsets = sampleOffsets && decodeSampleOffset(offsetFrame, &offsetSamples, &reason)
        && offsetSamples == 0;
    const std::array<unsigned, 4> expectedOffsets = {8U, 16U, 18U, 24U};
    for (unsigned index = 0U; index < expectedOffsets.size(); ++index) {
        offsetFrame = eac3oamd::B2aFrame {};
        offsetFrame.sampleOffsetCode = 1U;
        offsetFrame.sampleOffsetIndexPresent = true;
        offsetFrame.sampleOffsetIndex = index;
        sampleOffsets = sampleOffsets && decodeSampleOffset(
            offsetFrame, &offsetSamples, &reason)
            && offsetSamples == static_cast<std::int64_t>(expectedOffsets[index]);
    }
    offsetFrame = eac3oamd::B2aFrame {};
    offsetFrame.sampleOffsetCode = 2U;
    offsetFrame.sampleOffsetBitsPresent = true;
    offsetFrame.sampleOffsetBits = 31U;
    sampleOffsets = sampleOffsets && decodeSampleOffset(offsetFrame, &offsetSamples, &reason)
        && offsetSamples == 31;
    check(sampleOffsets, "sample-offset-table23-conversion", &report.conversionCases);

    bool rampValues = true;
    eac3oamd::B2aBlockUpdate ramp;
    unsigned rampSamples = 0;
    ramp.rampDurationCode = 0U;
    rampValues = rampValues && decodeRampDuration(ramp, &rampSamples, &reason)
        && rampSamples == 0U;
    ramp.rampDurationCode = 1U;
    rampValues = rampValues && decodeRampDuration(ramp, &rampSamples, &reason)
        && rampSamples == 512U;
    ramp.rampDurationCode = 2U;
    rampValues = rampValues && decodeRampDuration(ramp, &rampSamples, &reason)
        && rampSamples == 1536U;
    const std::array<unsigned, 16> expectedRamps = {
        32U, 64U, 128U, 256U, 320U, 480U, 1000U, 1001U,
        1024U, 1600U, 1601U, 1602U, 1920U, 2000U, 2002U, 2048U};
    for (unsigned index = 0U; index < expectedRamps.size(); ++index) {
        ramp = eac3oamd::B2aBlockUpdate {};
        ramp.rampDurationCode = 3U;
        ramp.rampDurationIndexPresent = true;
        ramp.rampDurationIndex = index;
        rampValues = rampValues && decodeRampDuration(ramp, &rampSamples, &reason)
            && rampSamples == expectedRamps[index];
    }
    ramp = eac3oamd::B2aBlockUpdate {};
    ramp.rampDurationCode = 3U;
    ramp.rampDurationBitsPresent = true;
    ramp.rampDurationBits = 2047U;
    rampValues = rampValues && decodeRampDuration(ramp, &rampSamples, &reason)
        && rampSamples == 2047U;
    check(rampValues, "ramp-duration-table24-25-conversion", &report.conversionCases);

    eac3oamd::B2aFrame badOffset = offsetFrame;
    badOffset.sampleOffsetCode = 3U;
    eac3oamd::B2aBlockUpdate badRamp;
    badRamp.rampDurationCode = 3U;
    badRamp.rampDurationIndexPresent = true;
    badRamp.rampDurationBitsPresent = true;
    eac3oamd::B2aBlockUpdate fixedRamp = badRamp;
    fixedRamp.rampDurationCode = 2U;
    const bool reservedRejected = !decodeSampleOffset(badOffset, &offsetSamples, &reason)
        && !decodeRampDuration(badRamp, &rampSamples, &reason)
        && !decodeRampDuration(fixedRamp, &rampSamples, &reason);
    check(reservedRejected, "reserved-and-inconsistent-timing-rejection",
          &report.conversionCases);

    report.pass = report.reason.empty() && report.cases == 20U;
    if (report.pass) report.reason = "bounded-gate6c-timeline-and-transaction-cases";
    return report;
}

} // namespace eac3gate6c
