#include "native-eac3-joc-session-bridge.h"

#include "oamd-b2a.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace eac3native {
namespace {

constexpr unsigned kSampleRate = 48000U;
constexpr unsigned kCoreChannels = 6U;
constexpr unsigned kJocChannels = 5U;
constexpr unsigned kObjectsWithLfeHelper = 16U;
constexpr unsigned kDynamicObjects = 15U;
constexpr std::size_t kSamplesPerUnit = 1536U;
constexpr std::size_t kTailSamples = 256U;
constexpr const char *kCoreLayout = "FL,FC,FR,SL,SR,LFE";

JocSessionBridgeResult result(JocSessionBridgeDisposition disposition,
                              const char *stage, const char *reason)
{
    JocSessionBridgeResult output;
    output.disposition = disposition;
    output.stage = stage ? stage : "";
    output.reason = reason ? reason : "";
    return output;
}

} // namespace

NativeEac3JocSessionBridge::NativeEac3JocSessionBridge(
    std::string jocTablePath, JocSessionBridgeConfig config)
    : jocTablePath_(std::move(jocTablePath))
    , config_(std::move(config))
    , qualifier_(jocTablePath_)
    , session_([this]() {
        eac3jocsession::Config value;
        value.qwin = config_.qwin;
        value.decodedSourceSamples = config_.decodedSourceSamples;
        value.callbackCapacity = config_.callbackCapacity;
        value.emitGate6c = true;
        return value;
    }())
{
}

JocSessionBridgeResult NativeEac3JocSessionBridge::fail(
    JocSessionBridgeDisposition disposition, const char *stage,
    const char *reason, bool poison)
{
    JocSessionBridgeResult output = result(disposition, stage, reason);
    output.auIndex = framesProcessed_;
    output.timestamp = static_cast<std::int64_t>(framesProcessed_ * kSamplesPerUnit);
    if (poison) poisoned_ = true;
    return output;
}

JocSessionBridgeResult NativeEac3JocSessionBridge::open()
{
    reset();
    if (config_.qwin.size() != eac3qmf::kFilterLength
        || config_.decodedSourceSamples <= 0
        || config_.callbackCapacity == 0U) {
        return fail(JocSessionBridgeDisposition::Unsupported, "open",
                    "joc-session-bridge-config-invalid", false);
    }
    const eac3jocsession::CoreDecoderConfig coreConfig {
        kSampleRate, kCoreChannels, kCoreLayout, true,
    };
    const eac3jocsession::CoreDecodeResult opened = core_.open(coreConfig);
    if (!opened.accepted) {
        return fail(JocSessionBridgeDisposition::Unsupported, "core-open",
                    opened.reason.c_str(), false);
    }
    opened_ = true;
    JocSessionBridgeResult output = result(
        JocSessionBridgeDisposition::Accepted, "open",
        "config3-native-core-and-joc-session-open");
    output.progressed = true;
    return output;
}

bool NativeEac3JocSessionBridge::mapCoreChannels(
    const eac3jocsession::CoreFrame &frame,
    std::vector<std::vector<float>> *orderedPcm,
    std::vector<float> *lfe, std::string *reason)
{
    if (!orderedPcm || !lfe || !reason
        || frame.sampleRate != kSampleRate
        || frame.channelLayout != kCoreLayout
        || frame.contentKind != eac3jocsession::CoreContentKind::OrdinaryEac3
        || frame.drcApplied || frame.channels != kCoreChannels
        || frame.sampleCount != kSamplesPerUnit
        || frame.channelIds != std::vector<std::string> {
            "FL", "FC", "FR", "SL", "SR", "LFE"}
        || frame.lfeChannels != std::vector<bool> {false, false, false,
                                                     false, false, true}
        || frame.planar.size() != kCoreChannels) {
        if (reason) *reason = "core-frame-config3-identity-invalid";
        return false;
    }
    // N5B emits FL,FC,FR,SL,SR,LFE. Gate5C's config-3 session order is
    // FL,FR,FC,SL,SR, while LFE is carried separately.
    const std::array<unsigned, kJocChannels> order = {0U, 2U, 1U, 3U, 4U};
    orderedPcm->clear();
    orderedPcm->reserve(kJocChannels);
    for (const unsigned source : order) {
        if (frame.planar[source].size() != kSamplesPerUnit
            || !std::all_of(frame.planar[source].begin(), frame.planar[source].end(),
                            [](float value) { return std::isfinite(value); })) {
            *reason = "core-frame-nonfinite-or-wrong-sized";
            return false;
        }
        orderedPcm->push_back(frame.planar[source]);
    }
    if (frame.planar[5].size() != kSamplesPerUnit
        || !std::all_of(frame.planar[5].begin(), frame.planar[5].end(),
                        [](float value) { return std::isfinite(value); })) {
        *reason = "core-frame-lfe-nonfinite-or-wrong-sized";
        return false;
    }
    *lfe = frame.planar[5];
    return true;
}

bool buildNativeJocMetadata(
    const NativeJocQualification &qualification, std::size_t unitIndex,
    eac3oamd::B2bState *metadataState,
    std::vector<eac3gate6c::MetadataUpdate> *metadata, std::string *reason)
{
    if (!metadataState || !metadata || !reason || !qualification.hasOamdReport
        || !qualification.oamdAccepted) {
        if (reason) *reason = "oamd-b1-not-qualified";
        return false;
    }
    const eac3oamd::B1Frame &b1 = qualification.oamd;
    if (!b1.program.dynamicOnly || !b1.program.lfePresent
        || b1.objectCount != kObjectsWithLfeHelper) {
        *reason = "oamd-config3-dynamic-lfe-topology-required";
        return false;
    }
    const eac3oamd::ElementInventory *element = nullptr;
    for (const eac3oamd::ElementInventory &candidate : b1.elements) {
        if (candidate.id != 1U) continue;
        if (element) {
            *reason = "oamd-object-element-duplicate";
            return false;
        }
        element = &candidate;
    }
    if (!element || !element->recognized) {
        *reason = "oamd-object-element-missing";
        return false;
    }
    std::vector<bool> helperMap(kObjectsWithLfeHelper, false);
    helperMap[0] = true;
    const eac3oamd::B2aFrame b2a = eac3oamd::parseObjectElement(
        element->rawBody, element->rawBodyBits, kObjectsWithLfeHelper,
        helperMap, eac3oamd::B2aBitOrder::Syntax5511Lsb);
    if (b2a.disposition != eac3oamd::B2aDisposition::Pass
        || b2a.objectInfoBlockCount == 0U) {
        *reason = b2a.reason.empty() ? "oamd-b2a-rejected" : b2a.reason;
        return false;
    }
    eac3oamd::B2bFrameOutput b2b;
    const eac3oamd::B2bApplyResult applied = metadataState->applyFrame(
        b2a, helperMap, &b2b, false, &b1);
    if (applied.disposition != eac3oamd::B2bDisposition::Pass
        || b2b.objects.size() != kObjectsWithLfeHelper
        || b2b.dynamicObjectIndices.size() != kDynamicObjects) {
        *reason = applied.reason.empty() ? "oamd-b2b-rejected" : applied.reason;
        return false;
    }
    std::int64_t sampleOffset = 0;
    if (!eac3gate6c::decodeSampleOffset(b2a, &sampleOffset, reason)) {
        return false;
    }
    std::vector<unsigned> rampDurations;
    rampDurations.reserve(b2a.blocks.size());
    for (const eac3oamd::B2aBlockUpdate &block : b2a.blocks) {
        unsigned duration = 0U;
        if (!eac3gate6c::decodeRampDuration(block, &duration, reason)) {
            return false;
        }
        rampDurations.push_back(duration);
    }
    const std::int64_t unitBase = static_cast<std::int64_t>(unitIndex)
        * static_cast<std::int64_t>(kSamplesPerUnit);
    metadata->clear();
    for (const eac3oamd::B2bFrameOutput::BlockSnapshot &snapshot
         : b2b.blockSnapshots) {
        if (snapshot.blockIndex >= b2a.blocks.size()
            || snapshot.objects.size() != kObjectsWithLfeHelper) {
            *reason = "oamd-b2b-block-snapshot-invalid";
            return false;
        }
        const eac3oamd::B2aBlockUpdate &block = b2a.blocks[snapshot.blockIndex];
        const std::int64_t position = unitBase + sampleOffset
            + static_cast<std::int64_t>(32) * block.blockOffsetFactor;
        if (position < unitBase
            || position >= unitBase + static_cast<std::int64_t>(kSamplesPerUnit)) {
            *reason = "oamd-metadata-position-out-of-unit";
            return false;
        }
        for (unsigned object = 1U; object <= kDynamicObjects; ++object) {
            const eac3oamd::B2bObjectState &state = snapshot.objects[object];
            if (state.objectIndex != object || !state.basicValid || !state.renderValid) {
                *reason = "oamd-b2b-dynamic-object-state-invalid";
                return false;
            }
            metadata->push_back(eac3gate6c::MetadataUpdate {
                position, snapshot.blockIndex, rampDurations[snapshot.blockIndex],
                object, state,
            });
        }
    }
    if (metadata->empty()) {
        *reason = "oamd-metadata-empty";
        return false;
    }
    return true;
}

bool NativeEac3JocSessionBridge::buildMetadata(
    const NativeJocQualification &qualification, std::size_t unitIndex,
    std::vector<eac3gate6c::MetadataUpdate> *metadata, std::string *reason)
{
    return buildNativeJocMetadata(qualification, unitIndex, &metadataState_,
                                  metadata, reason);
}

JocSessionBridgeResult NativeEac3JocSessionBridge::process(
    const std::vector<std::uint8_t> &packet,
    const eac3gate6c::Callback &callback)
{
    if (canceled_) return fail(JocSessionBridgeDisposition::Malformed, "state",
                               "joc-session-bridge-canceled", false);
    if (poisoned_) return fail(JocSessionBridgeDisposition::Malformed, "state",
                               "joc-session-bridge-poisoned-reset-required", false);
    if (!opened_) return fail(JocSessionBridgeDisposition::Malformed, "state",
                              "joc-session-bridge-not-open", false);
    if (flushed_) return fail(JocSessionBridgeDisposition::Malformed, "state",
                              "joc-session-bridge-flushed", false);
    if (!callback) return fail(JocSessionBridgeDisposition::Malformed, "session",
                               "joc-session-bridge-callback-missing");
    const std::size_t unitIndex = framesProcessed_;
    const std::int64_t timestamp = static_cast<std::int64_t>(unitIndex)
        * static_cast<std::int64_t>(kSamplesPerUnit);
    eac3jocsession::CoreFrame coreFrame;
    const eac3jocsession::CoreDecodeResult coreResult = core_.pushPacket(
        packet, timestamp, [&coreFrame](const eac3jocsession::CoreFrame &frame) {
            coreFrame = frame;
            return true;
        });
    if (!coreResult.accepted || coreResult.contentKind
            != eac3jocsession::CoreContentKind::OrdinaryEac3) {
        return fail(coreResult.disposition == eac3jocsession::Disposition::Unsupported
                        ? JocSessionBridgeDisposition::Unsupported
                        : JocSessionBridgeDisposition::Malformed,
                    "core", coreResult.reason.c_str());
    }
    if (coreFrame.timestamp != timestamp || coreFrame.sampleCount != kSamplesPerUnit
        || coreFrame.flush || coreFrame.eosTail) {
        return fail(JocSessionBridgeDisposition::Malformed, "alignment",
                    "core-au-index-timestamp-or-shape-mismatch");
    }
    const NativeJocQualification qualification = qualifier_.process(packet);
    if (qualification.disposition != JocQualificationDisposition::Qualified
        || qualification.auIndex != unitIndex
        || qualification.timestamp != timestamp
        || !qualification.hasJocReport || !qualification.jocAccepted
        || qualification.joc.downmixConfigIndex != 3U
        || qualification.joc.numChannels != kJocChannels) {
        return fail(qualification.disposition == JocQualificationDisposition::Unsupported
                        ? JocSessionBridgeDisposition::Unsupported
                        : JocSessionBridgeDisposition::Malformed,
                    "qualification", qualification.reason.c_str());
    }
    std::vector<std::vector<float>> orderedPcm;
    std::vector<float> lfe;
    std::string reason;
    if (!mapCoreChannels(coreFrame, &orderedPcm, &lfe, &reason)) {
        return fail(JocSessionBridgeDisposition::Malformed, "channel-map",
                    reason.c_str());
    }
    std::vector<eac3gate6c::MetadataUpdate> metadata;
    if (!buildMetadata(qualification, unitIndex, &metadata, &reason)) {
        return fail(JocSessionBridgeDisposition::Malformed, "metadata",
                    reason.c_str());
    }
    eac3jocsession::Input input;
    input.unitIndex = unitIndex;
    input.sourceStart = timestamp;
    input.joc = qualification.joc;
    input.orderedPcm = std::move(orderedPcm);
    input.lfe = std::move(lfe);
    input.metadata = std::move(metadata);
    const std::size_t metadataCount = input.metadata.size();
    const std::size_t batchesBefore = session_.report().batchesEmitted;
    const eac3jocsession::ProcessResult processed = session_.process(input, callback);
    if (!processed.progressed) {
        return fail(processed.disposition == eac3jocsession::Disposition::Unsupported
                        ? JocSessionBridgeDisposition::Unsupported
                        : JocSessionBridgeDisposition::Malformed,
                    "session", processed.reason.c_str());
    }
    ++framesProcessed_;
    JocSessionBridgeResult output = result(
        JocSessionBridgeDisposition::Accepted, "session",
        "native-core-qualified-joc-metadata-session-accepted");
    output.auIndex = unitIndex;
    output.timestamp = timestamp;
    output.metadataUpdates = metadataCount;
    output.emittedBatches = session_.report().batchesEmitted - batchesBefore;
    output.progressed = true;
    return output;
}

JocSessionBridgeResult NativeEac3JocSessionBridge::flush(
    const eac3gate6c::Callback &callback)
{
    if (canceled_) return fail(JocSessionBridgeDisposition::Malformed, "state",
                               "joc-session-bridge-canceled", false);
    if (poisoned_) return fail(JocSessionBridgeDisposition::Malformed, "state",
                               "joc-session-bridge-poisoned-reset-required", false);
    if (!opened_) return fail(JocSessionBridgeDisposition::Malformed, "state",
                              "joc-session-bridge-not-open", false);
    if (flushed_) return fail(JocSessionBridgeDisposition::Malformed, "state",
                              "joc-session-bridge-flushed", false);
    if (!callback) return fail(JocSessionBridgeDisposition::Malformed, "session",
                               "joc-session-bridge-callback-missing");
    eac3jocsession::CoreFrame tail;
    const eac3jocsession::CoreDecodeResult coreResult = core_.flush(
        [&tail](const eac3jocsession::CoreFrame &frame) {
            tail = frame;
            return true;
        });
    if (!coreResult.accepted || coreResult.contentKind
            != eac3jocsession::CoreContentKind::OrdinaryEac3) {
        return fail(JocSessionBridgeDisposition::Malformed, "core-flush",
                    coreResult.reason.c_str());
    }
    if (framesProcessed_ == 0U) {
        flushed_ = true;
        return JocSessionBridgeResult {
            JocSessionBridgeDisposition::Accepted, "core-flush",
            "empty-stream-flush", framesProcessed_, 0, 0, 0, 0, true};
    }
    if (!tail.flush || !tail.eosTail || tail.sampleCount != kTailSamples
        || tail.timestamp != static_cast<std::int64_t>(framesProcessed_)
            * static_cast<std::int64_t>(kSamplesPerUnit)) {
        return fail(JocSessionBridgeDisposition::Malformed, "core-flush",
                    "core-eos-tail-shape-or-timestamp-mismatch");
    }
    const std::size_t batchesBefore = session_.report().batchesEmitted;
    const eac3jocsession::ProcessResult flushed = session_.flush(callback);
    if (!flushed.progressed) {
        return fail(JocSessionBridgeDisposition::Malformed, "session-flush",
                    flushed.reason.c_str());
    }
    flushed_ = true;
    JocSessionBridgeResult output = result(
        JocSessionBridgeDisposition::Accepted, "session-flush",
        "native-core-eos-tail-and-session-flush-accepted");
    output.auIndex = framesProcessed_;
    output.timestamp = tail.timestamp;
    output.emittedBatches = session_.report().batchesEmitted - batchesBefore;
    output.eosTailSamples = kTailSamples;
    output.progressed = true;
    return output;
}

void NativeEac3JocSessionBridge::reset()
{
    core_.reset();
    qualifier_.reset();
    metadataState_.reset();
    session_.reset();
    opened_ = false;
    poisoned_ = false;
    canceled_ = false;
    flushed_ = false;
    framesProcessed_ = 0U;
}

void NativeEac3JocSessionBridge::cancel()
{
    canceled_ = true;
    core_.cancel();
    session_.cancel();
}

const char *toString(JocSessionBridgeDisposition value)
{
    switch (value) {
    case JocSessionBridgeDisposition::Accepted: return "accepted";
    case JocSessionBridgeDisposition::Unsupported: return "unsupported";
    case JocSessionBridgeDisposition::Malformed: return "malformed";
    }
    return "malformed";
}

} // namespace eac3native
