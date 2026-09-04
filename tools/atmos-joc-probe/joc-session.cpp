#include "joc-session.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace eac3jocsession {
namespace {

CoreDecodeResult result(Disposition disposition, const char *reason,
                        bool accepted = false,
                        FlowStatus flow = FlowStatus::None)
{
    CoreDecodeResult output;
    output.disposition = disposition;
    output.reason = reason ? reason : "";
    output.accepted = accepted;
    output.flow = flow;
    if (disposition == Disposition::Joc) {
        output.contentKind = CoreContentKind::JocQualified;
    }
    return output;
}

class FakeCoreDecoder final : public ICoreDecoder {
public:
    CoreDecodeResult open(const CoreDecoderConfig &config) override
    {
        reset();
        if (!config.candidate) return result(Disposition::NotJoc, "candidate-not-joc");
        if (config.sampleRate != 48000U || config.channels == 0U
            || config.channelLayout.empty()) {
            return result(Disposition::Unsupported, "core-format-unsupported");
        }
        opened_ = true;
        return result(Disposition::Joc, "sample-qualified-fake-core-open", true);
    }

    CoreDecodeResult pushPacket(const std::vector<std::uint8_t> &packet,
                                std::int64_t timestamp,
                                const CoreFrameCallback &callback) override
    {
        if (canceled_) {
            return result(Disposition::Joc, "core-canceled", false,
                          FlowStatus::Canceled);
        }
        if (blocked_) {
            return result(Disposition::Joc, "core-callback-blocked", false,
                          FlowStatus::CallbackRejected);
        }
        if (!opened_) return result(Disposition::Malformed, "core-not-open");
        if (packet.empty() || !callback) {
            return result(Disposition::Malformed,
                          packet.empty() ? "core-empty-packet"
                                         : "core-callback-missing");
        }
        CoreFrame frame;
        frame.sampleRate = 48000U;
        frame.channels = 2U;
        frame.channelLayout = "FL,FR";
        frame.sampleCount = 1536U;
        frame.timestamp = timestamp;
        frame.planar.assign(2U, std::vector<float>(1536U, 0.0f));
        frame.contentKind = CoreContentKind::JocQualified;
        frame.planar[0][0] = 0.25f;
        frame.planar[1][0] = -0.25f;
        std::string frameReason;
        if (!validateCoreFrame(frame, &frameReason)) {
            return result(Disposition::Malformed, frameReason.c_str());
        }
        if (timestamp < 0
            || timestamp > std::numeric_limits<std::int64_t>::max() - 1536) {
            return result(Disposition::Malformed, "core-timestamp-overflow");
        }
        if (!callback(frame)) {
            blocked_ = true;
            return result(Disposition::Joc, "core-callback-rejected", false,
                          FlowStatus::CallbackRejected);
        }
        nextTimestamp_ = timestamp + 1536;
        return result(Disposition::Joc, "core-frame-accepted", true);
    }

    CoreDecodeResult flush(const CoreFrameCallback &callback) override
    {
        if (canceled_) {
            return result(Disposition::Joc, "core-canceled", false,
                          FlowStatus::Canceled);
        }
        if (!opened_) return result(Disposition::Malformed, "core-not-open");
        if (flushed_) {
            return result(Disposition::Joc, "core-flush-already-called", false,
                          FlowStatus::AlreadyFlushed);
        }
        if (blocked_) {
            return result(Disposition::Joc, "core-callback-blocked", false,
                          FlowStatus::CallbackRejected);
        }
        if (!callback) return result(Disposition::Malformed, "core-callback-missing");
        CoreFrame frame;
        frame.sampleRate = 48000U;
        frame.channels = 2U;
        frame.channelLayout = "FL,FR";
        frame.sampleCount = 0U;
        frame.timestamp = nextTimestamp_;
        frame.flush = true;
        frame.contentKind = CoreContentKind::JocQualified;
        frame.planar.assign(2U, {});
        std::string frameReason;
        if (!validateCoreFrame(frame, &frameReason)) {
            return result(Disposition::Malformed, frameReason.c_str());
        }
        if (!callback(frame)) {
            blocked_ = true;
            return result(Disposition::Joc, "core-callback-rejected-flush", false,
                          FlowStatus::CallbackRejected);
        }
        flushed_ = true;
        CoreDecodeResult output = result(Disposition::Joc, "core-flush-accepted", true);
        output.terminal = true;
        return output;
    }

    void reset() override
    {
        opened_ = false;
        flushed_ = false;
        blocked_ = false;
        canceled_ = false;
        nextTimestamp_ = 0;
    }

    void cancel() override { canceled_ = true; }

private:
    bool opened_ = false;
    bool flushed_ = false;
    bool blocked_ = false;
    bool canceled_ = false;
    std::int64_t nextTimestamp_ = 0;
};

ProcessResult processFailure(Disposition disposition, const char *reason,
                             FlowStatus flow = FlowStatus::None,
                             FailureStage stage = FailureStage::None)
{
    ProcessResult output;
    output.disposition = disposition;
    output.reason = reason ? reason : "";
    output.flow = flow;
    output.stage = stage;
    return output;
}

bool finiteInput(const Input &input, std::string *reason)
{
    if (!reason || input.sourceStart < 0
        || input.sourceStart % static_cast<std::int64_t>(eac3gate6c::kSamplesPerUnit) != 0) {
        if (reason) *reason = "session-source-position-invalid";
        return false;
    }
    if (input.orderedPcm.empty() || input.orderedPcm.size() > 7U) {
        *reason = "session-native-layout-empty-or-unsupported";
        return false;
    }
    for (const auto &channel : input.orderedPcm) {
        if (channel.size() != eac3gate6c::kSamplesPerUnit
            || !std::all_of(channel.begin(), channel.end(),
                            [](float value) { return std::isfinite(value); })) {
            *reason = "session-native-pcm-shape-or-finite-invalid";
            return false;
        }
    }
    if (input.lfe.size() != eac3gate6c::kSamplesPerUnit
        || !std::all_of(input.lfe.begin(), input.lfe.end(),
                        [](float value) { return std::isfinite(value); })) {
        *reason = "session-lfe-shape-or-finite-invalid";
        return false;
    }
    return true;
}

Input syntheticInput(unsigned config, std::size_t unitIndex = 0U,
                     std::int64_t sourceStart = 0)
{
    Input input;
    input.unitIndex = unitIndex;
    input.sourceStart = sourceStart;
    input.joc.disposition = eac3joc::ParseDisposition::Pass;
    input.joc.downmixConfigIndex = config;
    input.joc.numChannels = config == 3U ? 5U : 7U;
    input.joc.numObjects = eac3gate6c::kDynamicObjectCount;
    input.joc.sequenceCount = static_cast<unsigned>(unitIndex + 1U);
    input.joc.objects.resize(input.joc.numObjects);
    for (unsigned object = 0; object < input.joc.numObjects; ++object) {
        auto &summary = input.joc.objects[object];
        summary.present = true;
        summary.numBands = 1U;
        summary.quantIndex = 0U;
        summary.quantSteps = 96U;
        summary.slopeIndex = 0U;
        summary.numDataPoints = 1U;
        summary.dataPoints.resize(1U);
        summary.dataPoints[0].values.assign(input.joc.numChannels, 0U);
    }
    input.orderedPcm.assign(input.joc.numChannels,
                            std::vector<float>(eac3gate6c::kSamplesPerUnit, 0.0f));
    for (unsigned channel = 0; channel < input.joc.numChannels; ++channel) {
        std::fill(input.orderedPcm[channel].begin(), input.orderedPcm[channel].end(),
                  0.01f * static_cast<float>(channel + 1U));
    }
    input.lfe.assign(eac3gate6c::kSamplesPerUnit, 0.0f);
    input.metadata.reserve(eac3gate6c::kDynamicObjectCount);
    for (unsigned object = 1U; object <= eac3gate6c::kDynamicObjectCount; ++object) {
        eac3gate6c::MetadataUpdate update;
        update.sourcePosition = sourceStart;
        update.objectIndex = object;
        update.state.objectIndex = object;
        update.state.active = true;
        update.state.basicValid = true;
        update.state.renderValid = true;
        update.state.gainMinusInfinity = false;
        input.metadata.push_back(std::move(update));
    }
    return input;
}

bool sameQmfFrame(const eac3joc::JocQmfFrame &left,
                  const eac3joc::JocQmfFrame &right)
{
    return left.config == right.config && left.numChannels == right.numChannels
        && left.numObjects == right.numObjects && left.stateReset == right.stateReset
        && left.objects == right.objects;
}

bool sameFrame(const CoreFrame &left, const CoreFrame &right)
{
    return left.sampleRate == right.sampleRate && left.channels == right.channels
        && left.channelLayout == right.channelLayout
        && left.sampleCount == right.sampleCount
        && left.timestamp == right.timestamp && left.reset == right.reset
        && left.flush == right.flush && left.planar == right.planar
        && left.contentKind == right.contentKind
        && left.channelIds == right.channelIds
        && left.lfeChannels == right.lfeChannels
        && left.drcApplied == right.drcApplied
        && left.eosTail == right.eosTail;
}

} // namespace

const char *dispositionText(Disposition disposition)
{
    switch (disposition) {
    case Disposition::Joc: return "Joc";
    case Disposition::OrdinaryEac3: return "OrdinaryEac3";
    case Disposition::NotJoc: return "NotJoc";
    case Disposition::Unsupported: return "Unsupported";
    case Disposition::Malformed: return "Malformed";
    }
    return "Malformed";
}

const char *flowStatusText(FlowStatus status)
{
    switch (status) {
    case FlowStatus::None: return "None";
    case FlowStatus::CallbackRejected: return "CallbackRejected";
    case FlowStatus::Canceled: return "Canceled";
    case FlowStatus::AlreadyFlushed: return "AlreadyFlushed";
    }
    return "None";
}

const char *failureStageText(FailureStage stage)
{
    switch (stage) {
    case FailureStage::None: return "None";
    case FailureStage::Validation: return "Validation";
    case FailureStage::Math: return "Math";
    case FailureStage::Sequence: return "Sequence";
    case FailureStage::Qmf: return "Qmf";
    case FailureStage::Gate6c: return "Gate6c";
    }
    return "None";
}

const char *coreContentKindText(CoreContentKind kind)
{
    switch (kind) {
    case CoreContentKind::Unknown: return "Unknown";
    case CoreContentKind::JocQualified: return "JocQualified";
    case CoreContentKind::OrdinaryEac3: return "OrdinaryEac3";
    }
    return "Unknown";
}

bool validateCoreFrame(const CoreFrame &frame, std::string *reason)
{
    const auto fail = [reason](const char *value) {
        if (reason) *reason = value;
        return false;
    };
    if (frame.sampleRate == 0U || frame.channels == 0U
        || frame.channelLayout.empty()) {
        return fail("core-frame-format-invalid");
    }
    if (frame.timestamp < 0) return fail("core-frame-timestamp-invalid");
    if (frame.planar.size() != frame.channels) {
        return fail("core-frame-channel-shape-mismatch");
    }
    if (frame.contentKind == CoreContentKind::OrdinaryEac3) {
        if (frame.drcApplied) return fail("ordinary-core-drc-must-be-off");
        if (frame.channelIds.size() != frame.channels
            || frame.lfeChannels.size() != frame.channels) {
            return fail("ordinary-core-channel-identity-shape-mismatch");
        }
        std::vector<std::string> ids;
        ids.reserve(frame.channelIds.size());
        for (const std::string &id : frame.channelIds) {
            if (id.empty() || std::find(ids.begin(), ids.end(), id) != ids.end()) {
                return fail("ordinary-core-channel-identity-invalid");
            }
            ids.push_back(id);
        }
        if (std::count(frame.lfeChannels.begin(), frame.lfeChannels.end(), true) > 1) {
            return fail("ordinary-core-multiple-lfe");
        }
        if (frame.eosTail) {
            if (!frame.flush || frame.sampleCount != 256U) {
                return fail("ordinary-core-eos-tail-shape-invalid");
            }
        } else if (frame.flush) {
            if (frame.sampleCount != 0U) {
                return fail("ordinary-core-empty-flush-shape-invalid");
            }
        } else if (frame.sampleCount != 1536U) {
            return fail("ordinary-core-access-unit-shape-invalid");
        }
    }
    if (!frame.flush && frame.sampleCount == 0U) {
        return fail("core-frame-sample-count-invalid");
    }
    for (const auto &plane : frame.planar) {
        if (plane.size() != frame.sampleCount) {
            return fail("core-frame-sample-count-mismatch");
        }
        if (!std::all_of(plane.begin(), plane.end(),
                         [](float value) { return std::isfinite(value); })) {
            return fail("core-frame-nonfinite");
        }
    }
    return true;
}

Session::Session(Config config)
    : config_(std::move(config))
    , gate6c_([this]() {
        eac3gate6c::Config gateConfig;
        gateConfig.qwin = config_.qwin;
        gateConfig.decodedSourceSamples = config_.decodedSourceSamples;
        gateConfig.skipSamples = config_.skipSamples;
        gateConfig.discardPadding = config_.discardPadding;
        gateConfig.callbackCapacity = config_.callbackCapacity;
        return gateConfig;
    }())
{
}

ProcessResult Session::process(const Input &input,
                               const eac3gate6c::Callback &callback)
{
    if (canceled_) return processFailure(Disposition::Joc, "session-canceled",
                                         FlowStatus::Canceled);
    if (blocked_) return processFailure(Disposition::Joc,
                                        "session-callback-blocked",
                                        FlowStatus::CallbackRejected,
                                        FailureStage::Gate6c);
    if (flushed_) return processFailure(Disposition::Joc, "session-flushed",
                                        FlowStatus::AlreadyFlushed);
    if (config_.emitGate6c && !callback) {
        ++report_.malformed;
        return processFailure(Disposition::Malformed, "session-callback-missing",
                              FlowStatus::None, FailureStage::Validation);
    }
    // Gate 5A marks the currently paired config-3/4 phase as
    // "Unsupported" because its extensional phase is not implemented, while
    // Gate 5B/6C deliberately consume those recognized frames. Reserved
    // configurations remain structured Unsupported results.
    if (input.joc.disposition == eac3joc::ParseDisposition::Unsupported
        && input.joc.downmixConfigIndex != 3U
        && input.joc.downmixConfigIndex != 4U) {
        ++report_.unsupported;
        return processFailure(Disposition::Unsupported, "joc-frame-unsupported",
                              FlowStatus::None, FailureStage::Validation);
    }
    if (input.joc.disposition == eac3joc::ParseDisposition::Malformed) {
        ++report_.malformed;
        return processFailure(Disposition::Malformed, "joc-frame-malformed",
                              FlowStatus::None, FailureStage::Validation);
    }
    std::string reason;
    if (!finiteInput(input, &reason)) {
        ++report_.malformed;
        return processFailure(Disposition::Malformed, reason.c_str(),
                              FlowStatus::None, FailureStage::Validation);
    }
    if (config_.qwin.size() != eac3qmf::kFilterLength) {
        ++report_.malformed;
        return processFailure(Disposition::Malformed,
                              "session-qmf-prototype-size-mismatch",
                              FlowStatus::None, FailureStage::Validation);
    }

    eac3joc::JocMathFrame stagedMath;
    if (!eac3joc::processMath(input.joc, 24U, &mathState_, &stagedMath, &reason)) {
        reset();
        ++report_.malformed;
        return processFailure(Disposition::Malformed, reason.c_str(),
                              FlowStatus::None, FailureStage::Math);
    }
    if (stagedMath.sequenceDiscontinuity) {
        reset();
        ++report_.unsupported;
        return processFailure(Disposition::Unsupported,
                              "joc-sequence-discontinuity", FlowStatus::None,
                              FailureStage::Sequence);
    }
    eac3joc::JocQmfFrame stagedQmf;
    if (!eac3joc::reconstructQmf(stagedMath, input.joc.downmixConfigIndex,
                                 input.orderedPcm, config_.qwin, &qmfState_,
                                 &stagedQmf, &reason)) {
        reset();
        ++report_.malformed;
        return processFailure(Disposition::Malformed, reason.c_str(),
                              FlowStatus::None, FailureStage::Qmf);
    }

    if (config_.emitGate6c) {
        eac3gate6c::UnitInput unit;
        unit.unitIndex = input.unitIndex;
        unit.sourceStart = input.sourceStart;
        unit.qmf = stagedQmf;
        unit.lfe = input.lfe;
        unit.metadata = input.metadata;
        const eac3gate6c::Callback forwarding =
            [&callback, this](const eac3gate6c::Batch &batch) {
                if (!callback(batch)) return false;
                ++report_.batchesEmitted;
                return true;
        };
        const std::size_t callbackRejectsBefore = gate6c_.report().callbackRejects;
        if (!gate6c_.processUnit(unit, forwarding, &reason)) {
            if (report_.firstFailureReason.empty()) report_.firstFailureReason = reason;
            const bool callbackRejected = gate6c_.report().callbackRejects
                > callbackRejectsBefore;
            if (callbackRejected) {
                ++report_.callbackRejects;
                reset();
                blocked_ = true;
                return processFailure(Disposition::Joc, reason.c_str(),
                                      FlowStatus::CallbackRejected,
                                      FailureStage::Gate6c);
            }
            reset();
            return processFailure(Disposition::Malformed, reason.c_str(),
                                  FlowStatus::None, FailureStage::Gate6c);
        }
    }
    lastMath_ = std::move(stagedMath);
    lastQmf_ = std::move(stagedQmf);
    ++report_.framesAccepted;
    ProcessResult output;
    output.disposition = Disposition::Joc;
    output.reason = "joc-frame-accepted";
    output.progressed = true;
    return output;
}

ProcessResult Session::flush(const eac3gate6c::Callback &callback)
{
    if (canceled_) return processFailure(Disposition::Joc, "session-canceled",
                                         FlowStatus::Canceled);
    if (blocked_) return processFailure(Disposition::Joc,
                                        "session-callback-blocked",
                                        FlowStatus::CallbackRejected,
                                        FailureStage::Gate6c);
    if (flushed_) return processFailure(Disposition::Joc,
                                        "session-flushed", FlowStatus::AlreadyFlushed);
    if (!config_.emitGate6c) {
        ++report_.flushes;
        flushed_ = true;
        return ProcessResult {Disposition::Joc, "session-flush-accepted", true,
                              FlowStatus::None, FailureStage::None};
    }
    if (!callback) {
        return processFailure(Disposition::Malformed, "session-callback-missing",
                              FlowStatus::None, FailureStage::Validation);
    }
    std::string reason;
    const eac3gate6c::Callback forwarding =
        [&callback, this](const eac3gate6c::Batch &batch) {
            if (!callback(batch)) return false;
            ++report_.batchesEmitted;
            return true;
        };
    const std::size_t callbackRejectsBefore = gate6c_.report().callbackRejects;
    if (!gate6c_.flush(forwarding, &reason)) {
        const bool callbackRejected = gate6c_.report().callbackRejects
            > callbackRejectsBefore;
        if (report_.firstFailureReason.empty()) report_.firstFailureReason = reason;
        if (callbackRejected) {
            ++report_.callbackRejects;
            blocked_ = true;
            return processFailure(Disposition::Joc, reason.c_str(),
                                  FlowStatus::CallbackRejected,
                                  FailureStage::Gate6c);
        }
        return processFailure(Disposition::Malformed, reason.c_str(),
                              FlowStatus::None, FailureStage::Gate6c);
    }
    ++report_.flushes;
    flushed_ = true;
    return ProcessResult {Disposition::Joc, "session-flush-accepted", true,
                          FlowStatus::None, FailureStage::None};
}

void Session::reset()
{
    mathState_ = eac3joc::JocMatrixState {};
    qmfState_.reset();
    gate6c_.reset();
    lastMath_ = eac3joc::JocMathFrame {};
    lastQmf_ = eac3joc::JocQmfFrame {};
    canceled_ = false;
    blocked_ = false;
    flushed_ = false;
    ++report_.resets;
}

void Session::cancel()
{
    canceled_ = true;
}

SelfTestReport runSelfTest(const std::vector<double> &qwin)
{
    SelfTestReport report;
    auto check = [&report](bool pass, const char *reason, std::size_t *counter) {
        ++report.cases;
        if (counter) ++*counter;
        if (!pass && report.reason.empty()) report.reason = reason;
    };

    CoreDecoderConfig config;
    config.sampleRate = 48000U;
    config.channels = 2U;
    config.channelLayout = "FL,FR";
    config.candidate = true;
    FakeCoreDecoder decoder;
    check(decoder.open(config).disposition == Disposition::Joc,
          "synthetic-open", &report.coreCases);

    std::vector<CoreFrame> firstRun;
    const CoreFrameCallback collect = [&firstRun](const CoreFrame &frame) {
        firstRun.push_back(frame);
        return true;
    };
    const std::vector<std::uint8_t> packet = {0x0b, 0x77};
    CoreDecodeResult frameResult = decoder.pushPacket(packet, 0, collect);
    check(frameResult.accepted && frameResult.disposition == Disposition::Joc
              && firstRun.size() == 1U && firstRun[0].sampleCount == 1536U
              && firstRun[0].timestamp == 0 && firstRun[0].planar.size() == 2U,
          "synthetic-frame-contract", &report.coreCases);
    decoder.reset();
    check(decoder.open(config).disposition == Disposition::Joc,
          "reset-reopen", &report.coreCases);
    std::vector<CoreFrame> secondRun;
    decoder.pushPacket(packet, 0, [&secondRun](const CoreFrame &frame) {
        secondRun.push_back(frame);
        return true;
    });
    check(secondRun.size() == 1U && sameFrame(firstRun[0], secondRun[0]),
          "reset-fresh-equivalence", &report.coreCases);

    const CoreDecodeResult malformed = decoder.pushPacket({}, 1536, collect);
    check(malformed.disposition == Disposition::Malformed && !malformed.accepted
              && firstRun.size() == 1U,
          "malformed-no-state-commit", &report.coreCases);
    CoreDecoderConfig unsupported = config;
    unsupported.sampleRate = 44100U;
    decoder.reset();
    const CoreDecodeResult unsupportedResult = decoder.open(unsupported);
    check(unsupportedResult.disposition == Disposition::Unsupported
              && !unsupportedResult.reason.empty(),
          "unsupported-structured-reason", &report.coreCases);

    decoder.reset();
    decoder.open(config);
    decoder.pushPacket(packet, 0, collect);
    const CoreDecodeResult flushResult = decoder.flush([](const CoreFrame &) { return true; });
    const CoreDecodeResult secondFlush = decoder.flush([](const CoreFrame &) { return true; });
    check(flushResult.accepted && flushResult.terminal
              && secondFlush.disposition == Disposition::Joc
              && secondFlush.flow == FlowStatus::AlreadyFlushed
              && secondFlush.reason == "core-flush-already-called",
          "flush-only-once", &report.coreCases);

    decoder.reset();
    decoder.open(config);
    std::size_t rejectedCallbacks = 0;
    const CoreDecodeResult rejected = decoder.pushPacket(
        packet, 0, [&rejectedCallbacks](const CoreFrame &) {
            ++rejectedCallbacks;
            return false;
        });
    const CoreDecodeResult blocked = decoder.pushPacket(packet, 1536, collect);
    check(rejected.flow == FlowStatus::CallbackRejected && !rejected.accepted
              && blocked.flow == FlowStatus::CallbackRejected
              && rejectedCallbacks == 1U,
          "callback-backpressure-stops-progress", &report.coreCases);
    decoder.reset();
    decoder.open(config);
    decoder.cancel();
    const CoreDecodeResult canceled = decoder.pushPacket(packet, 0, collect);
    check(canceled.flow == FlowStatus::Canceled && canceled.disposition == Disposition::Joc,
          "cancel-stops-progress", &report.coreCases);

    CoreFrame validFrame;
    validFrame.sampleRate = 48000U;
    validFrame.channels = 2U;
    validFrame.channelLayout = "FL,FR";
    validFrame.sampleCount = 4U;
    validFrame.planar.assign(2U, std::vector<float>(4U, 0.0f));
    std::string frameReason;
    check(validateCoreFrame(validFrame, &frameReason),
          "core-frame-valid-shape", &report.coreCases);
    CoreFrame planarMismatch = validFrame;
    planarMismatch.planar.pop_back();
    check(!validateCoreFrame(planarMismatch, &frameReason)
              && frameReason == "core-frame-channel-shape-mismatch",
          "core-frame-planar-channel-mismatch", &report.coreCases);
    CoreFrame sampleMismatch = validFrame;
    sampleMismatch.planar[1].pop_back();
    check(!validateCoreFrame(sampleMismatch, &frameReason)
              && frameReason == "core-frame-sample-count-mismatch",
          "core-frame-sample-count-mismatch", &report.coreCases);
    CoreFrame overflowFrame = validFrame;
    overflowFrame.timestamp = std::numeric_limits<std::int64_t>::max() - 1535;
    decoder.reset();
    decoder.open(config);
    const CoreDecodeResult overflow = decoder.pushPacket(
        packet, overflowFrame.timestamp, [](const CoreFrame &) { return true; });
    check(overflow.disposition == Disposition::Malformed
              && overflow.reason == "core-timestamp-overflow",
          "core-timestamp-overflow-rejected", &report.coreCases);

    const auto makeSessionConfig = [&qwin](bool emitGate6c) {
        Config value;
        value.qwin = qwin;
        value.decodedSourceSamples = eac3gate6c::kSamplesPerUnit;
        value.emitGate6c = emitGate6c;
        return value;
    };
    const eac3gate6c::Callback acceptingCallback =
        [](const eac3gate6c::Batch &) { return true; };
    for (const unsigned configIndex : {3U, 4U}) {
        Session session(makeSessionConfig(true));
        std::vector<eac3gate6c::Batch> batches;
        const eac3gate6c::Callback collectBatches =
            [&batches](const eac3gate6c::Batch &batch) {
                batches.push_back(batch);
                return true;
            };
        const ProcessResult processed = session.process(
            syntheticInput(configIndex), collectBatches);
        const ProcessResult flushed = session.flush(collectBatches);
        const bool syntheticPass = processed.disposition == Disposition::Joc
                  && processed.flow == FlowStatus::None
                  && processed.stage == FailureStage::None && processed.progressed
                  && flushed.disposition == Disposition::Joc
                  && flushed.flow == FlowStatus::None
                  && flushed.stage == FailureStage::None && flushed.progressed
                  && !batches.empty() && session.lastQmfFrame().numObjects
                         == eac3gate6c::kDynamicObjectCount;
        if (!syntheticPass && report.reason.empty()) {
            report.reason = std::string(configIndex == 3U
                                            ? "session-config3-gate5b-qmf-gate6c:"
                                            : "session-config4-gate5b-qmf-gate6c:")
                + processed.reason + ":" + flushed.reason;
        }
        check(syntheticPass,
              configIndex == 3U ? "session-config3-gate5b-qmf-gate6c"
                                : "session-config4-gate5b-qmf-gate6c",
              &report.sessionCases);
    }

    Session session(makeSessionConfig(false));
    const Input valid = syntheticInput(3U);
    const ProcessResult first = session.process(valid, {});
    const eac3joc::JocQmfFrame firstQmf = session.lastQmfFrame();
    session.reset();
    const ProcessResult fresh = session.process(valid, {});
    check(first.disposition == Disposition::Joc
              && first.stage == FailureStage::None
              && fresh.disposition == Disposition::Joc
              && fresh.stage == FailureStage::None
              && sameQmfFrame(firstQmf, session.lastQmfFrame()),
          "session-reset-fresh-equivalence", &report.sessionCases);

    Input malformedInput;
    malformedInput.joc.disposition = eac3joc::ParseDisposition::Malformed;
    const eac3joc::JocQmfFrame beforeRejected = session.lastQmfFrame();
    const ProcessResult malformedSession = session.process(malformedInput, {});
    Input unsupportedInput = valid;
    unsupportedInput.joc.disposition = eac3joc::ParseDisposition::Unsupported;
    unsupportedInput.joc.downmixConfigIndex = 5U;
    const ProcessResult unsupportedSession = session.process(unsupportedInput, {});
    check(malformedSession.disposition == Disposition::Malformed
              && malformedSession.stage == FailureStage::Validation
              && unsupportedSession.disposition == Disposition::Unsupported
              && unsupportedSession.stage == FailureStage::Validation
              && sameQmfFrame(beforeRejected, session.lastQmfFrame()),
          "session-malformed-unsupported-no-state", &report.sessionCases);

    Session rejectedSession(makeSessionConfig(true));
    std::size_t rejectedCallbackCount = 0U;
    const eac3gate6c::Callback rejectingCallback =
        [&rejectedCallbackCount](const eac3gate6c::Batch &) {
            ++rejectedCallbackCount;
            return false;
        };
    const ProcessResult rejectedProcess = rejectedSession.process(
        valid, rejectingCallback);
    const ProcessResult blockedProcess = rejectedSession.process(
        valid, acceptingCallback);
    const bool callbackStopPass = rejectedProcess.flow == FlowStatus::CallbackRejected
              && blockedProcess.flow == FlowStatus::CallbackRejected
              && rejectedCallbackCount == 1U;
    if (!callbackStopPass && report.reason.empty()) {
        report.reason = "session-callback-reject-stops-progress:" + rejectedProcess.reason
            + ":" + flowStatusText(rejectedProcess.flow) + ":"
            + blockedProcess.reason + ":" + flowStatusText(blockedProcess.flow)
            + ":callbacks=" + std::to_string(rejectedCallbackCount);
    }
    check(callbackStopPass,
          "session-callback-reject-stops-progress", &report.sessionCases);

    Session canceledSession(makeSessionConfig(false));
    canceledSession.cancel();
    const ProcessResult canceledProcess = canceledSession.process(valid, {});
    canceledSession.reset();
    const ProcessResult afterCancelReset = canceledSession.process(valid, {});
    check(canceledProcess.flow == FlowStatus::Canceled
              && afterCancelReset.disposition == Disposition::Joc
              && afterCancelReset.flow == FlowStatus::None,
          "session-cancel-reset-restores", &report.sessionCases);

    Session flushedSession(makeSessionConfig(true));
    check(flushedSession.process(valid, acceptingCallback).disposition == Disposition::Joc,
          "session-flush-setup", &report.sessionCases);
    const ProcessResult flushAccepted = flushedSession.flush(acceptingCallback);
    const ProcessResult flushAgain = flushedSession.flush(acceptingCallback);
    const ProcessResult postFlush = flushedSession.process(valid, acceptingCallback);
    flushedSession.reset();
    const ProcessResult postResetProcess = flushedSession.process(valid, acceptingCallback);
    check(flushAccepted.flow == FlowStatus::None
              && flushAgain.flow == FlowStatus::AlreadyFlushed
              && postFlush.flow == FlowStatus::AlreadyFlushed
              && postResetProcess.disposition == Disposition::Joc,
          "session-flush-generation-guard", &report.sessionCases);

    Session noOutputFlushSession(makeSessionConfig(false));
    const ProcessResult noOutputProcess = noOutputFlushSession.process(valid, {});
    const ProcessResult noOutputFlush = noOutputFlushSession.flush({});
    const ProcessResult noOutputAgain = noOutputFlushSession.flush({});
    noOutputFlushSession.reset();
    const ProcessResult noOutputAfterReset = noOutputFlushSession.flush({});
    check(noOutputProcess.disposition == Disposition::Joc
              && noOutputFlush.flow == FlowStatus::None
              && noOutputAgain.flow == FlowStatus::AlreadyFlushed
              && noOutputAfterReset.flow == FlowStatus::None,
          "session-no-output-flush-generation-guard", &report.sessionCases);

    const eac3gate6c::Report gateReport = eac3gate6c::runSelfTest(qwin);
    report.gate6cCases = gateReport.cases;
    check(gateReport.pass, "gate6c-baseline", nullptr);
    report.pass = report.reason.empty();
    return report;
}

} // namespace eac3jocsession
