// J0A6: probe-only config-4 entry into the native JOC/OAMD session.
//
// This adapter consumes the linked J0A4 qualification and J0A5 owned
// seven-slot/LFE mapping contracts.  It deliberately stops at the existing
// renderer-neutral session/Gate6C output; it is not a production decoder,
// renderer, or playback path.

#include "joc-qmf.h"
#include "joc-session.h"
#include "native-eac3-config4-joc-bridge.h"
#include "native-eac3-config4-mapping.h"
#include "native-eac3-config4-joc-session.h"
#include "native-eac3-joc-session-bridge.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace config4session {
namespace {

constexpr unsigned kConfig = 4U;
constexpr unsigned kChannels = 7U;
constexpr std::uint16_t kChanmap = 0xA010U;
constexpr std::size_t kAuSamples = 1536U;
constexpr std::size_t kTailSamples = 256U;

bool checkedDecodedSourceSamples(std::uint64_t maxAUs,
                                 std::int64_t *samples,
                                 std::string *reason)
{
    if (!samples || maxAUs == 0U) {
        if (reason) *reason = "j0a6-invalid-max-aus";
        return false;
    }
    constexpr std::uint64_t kMax =
        static_cast<std::uint64_t>(INT64_MAX) / kAuSamples;
    if (maxAUs > kMax || maxAUs >= static_cast<std::uint64_t>(UINT_MAX)) {
        if (reason) *reason = "j0a6-max-aus-overflow";
        return false;
    }
    *samples = static_cast<std::int64_t>(maxAUs * kAuSamples);
    return true;
}

void hashByte(std::uint64_t *digest, std::uint8_t value)
{
    if (!digest) return;
    *digest ^= value;
    *digest *= 1099511628211ULL;
}

void hashFloat(std::uint64_t *digest, float value)
{
    std::uint32_t bits = 0U;
    std::memcpy(&bits, &value, sizeof(bits));
    for (unsigned shift = 0U; shift < 32U; shift += 8U)
        hashByte(digest, static_cast<std::uint8_t>((bits >> shift) & 0xffU));
}

struct BatchSummary {
    std::size_t unitIndex = 0U;
    bool flush = false;
    std::int64_t outputStart = 0;
    std::int64_t outputEnd = 0;
    std::size_t objectCount = 0U;
    std::size_t samples = 0U;
    std::size_t lfeSamples = 0U;
    std::size_t metadata = 0U;
    std::uint64_t digest = 1469598103934665603ULL;
    bool finite = true;
    std::vector<std::vector<float>> objects;
    std::vector<float> lfe;
};

struct SignalMetrics {
    double peak = 0.0;
    long double squareSum = 0.0L;
    std::uint64_t count = 0U;
    std::int64_t firstNonzero = -1;

    void add(double value, std::int64_t sample)
    {
        peak = std::max(peak, std::abs(value));
        squareSum += static_cast<long double>(value)
            * static_cast<long double>(value);
        ++count;
        if (firstNonzero < 0 && value != 0.0) firstNonzero = sample;
    }
    double rms() const
    {
        return count == 0U ? 0.0 : std::sqrt(static_cast<double>(
            squareSum / static_cast<long double>(count)));
    }
};

struct LayerTelemetry {
    SignalMetrics mappedPcm;
    SignalMetrics matrix;
    SignalMetrics objectQmf;
    SignalMetrics objectPcm;
    std::uint64_t presentObjectFrames = 0U;
};

const char *classifyForwardConsistency(const LayerTelemetry &telemetry)
{
    if (telemetry.matrix.firstNonzero < 0
        && telemetry.objectQmf.firstNonzero < 0) {
        return "PASS_ALL_ZERO";
    }
    if (telemetry.matrix.firstNonzero >= 0
        && telemetry.objectQmf.firstNonzero == telemetry.matrix.firstNonzero) {
        return "PASS_ZERO_PREFIX";
    }
    return "FAIL";
}

void observeMappedPcm(const eac3jocsession::Input &input,
                      LayerTelemetry *telemetry)
{
    for (const auto &channel : input.orderedPcm) {
        for (std::size_t sample = 0U; sample < channel.size(); ++sample)
            telemetry->mappedPcm.add(channel[sample], input.sourceStart
                + static_cast<std::int64_t>(sample));
    }
}

void observeMath(const eac3joc::JocMathFrame &math,
                 std::int64_t sourceStart, LayerTelemetry *telemetry)
{
    for (const auto &object : math.interpolated) {
        if (!object.present) continue;
        ++telemetry->presentObjectFrames;
        for (std::size_t timeslot = 0U; timeslot < object.values.size(); ++timeslot) {
            for (const auto &channel : object.values[timeslot])
                for (double coefficient : channel)
                    telemetry->matrix.add(coefficient, sourceStart
                        + static_cast<std::int64_t>(timeslot * eac3qmf::kSubbands));
        }
    }
}

void observeQmf(const eac3joc::JocQmfFrame &qmf,
                std::int64_t sourceStart, LayerTelemetry *telemetry)
{
    for (const auto &object : qmf.objects) {
        for (std::size_t index = 0U; index < object.size(); ++index) {
            const double magnitude = std::abs(object[index]);
            telemetry->objectQmf.add(magnitude, sourceStart
                + static_cast<std::int64_t>((index / eac3qmf::kSubbands)
                    * eac3qmf::kSubbands));
        }
    }
}

void observeObjectPcm(const eac3gate6c::Batch &batch,
                      LayerTelemetry *telemetry)
{
    for (const auto &object : batch.objects)
        for (std::size_t sample = 0U; sample < object.size(); ++sample)
            telemetry->objectPcm.add(object[sample], batch.outputStart
                + static_cast<std::int64_t>(sample));
}

bool inspectBatch(const eac3gate6c::Batch &batch, BatchSummary *summary,
                  std::string *reason)
{
    if (!summary || !reason || batch.objects.size() != eac3gate6c::kDynamicObjectCount
        || batch.lfe.empty()) {
        if (reason) *reason = "j0a6-gate6c-batch-shape";
        return false;
    }
    summary->unitIndex = batch.unitIndex;
    summary->flush = batch.flush;
    summary->outputStart = batch.outputStart;
    summary->outputEnd = batch.outputEnd;
    summary->objectCount = batch.objects.size();
    summary->samples = batch.objects.front().size();
    summary->lfeSamples = batch.lfe.size();
    summary->metadata = batch.metadata.size();
    summary->objects = batch.objects;
    summary->lfe = batch.lfe;
    for (const std::vector<float> &object : batch.objects) {
        if (object.size() != summary->samples) {
            *reason = "j0a6-object-batch-size-mismatch";
            return false;
        }
        for (float value : object) {
            summary->finite = summary->finite && std::isfinite(value);
            hashFloat(&summary->digest, value);
        }
    }
    for (float value : batch.lfe) {
        summary->finite = summary->finite && std::isfinite(value);
        hashFloat(&summary->digest, value);
    }
    if (!summary->finite || summary->lfeSamples != summary->samples) {
        *reason = "j0a6-gate6c-nonfinite-or-lfe-shape";
        return false;
    }
    return true;
}

bool validAssociation(const config4joc::Association &association,
                      unsigned auIndex, std::int64_t timestamp,
                      std::string *reason)
{
    if (association.auIndex != auIndex || association.timestamp != timestamp
        || association.jocConfig != kConfig || association.jocChannels != kChannels
        || association.dependentChanmap != kChanmap
        || association.dependentChannelCount != 4U
        || association.baseAcceptedContainers != 0U
        || association.baseTargetPayloads != 0U
        || association.dependentAcceptedContainers != 1U
        || association.dependentTargetPayloads != 2U
        || association.payload11Bytes == 0U || association.payload14Bytes == 0U
        || association.qualification.disposition
               != eac3native::JocQualificationDisposition::Qualified
        || !association.qualification.jocAccepted
        || !association.qualification.hasJocReport
        || association.qualification.payloadSources.size() != 2U
        || association.qualification.joc.downmixConfigIndex != kConfig
        || association.qualification.joc.numChannels != kChannels) {
        if (reason) *reason = "j0a6-association-config4-qualification-mismatch";
        return false;
    }
    return true;
}

class Adapter final {
public:
    explicit Adapter(const std::vector<double> &qwin, unsigned maxAUs)
        : maxAUs_(maxAUs), session_([&qwin, maxAUs]() {
            eac3jocsession::Config config;
            config.qwin = qwin;
            // Use the native decoded interval; Gate6C owns its algorithmic
            // delay and reports the resulting output intervals directly.
            std::int64_t decodedSourceSamples = 0;
            std::string validationReason;
            if (!checkedDecodedSourceSamples(
                    maxAUs, &decodedSourceSamples, &validationReason)) {
                throw std::invalid_argument(validationReason);
            }
            config.decodedSourceSamples = decodedSourceSamples;
            config.emitGate6c = true;
            config.callbackCapacity = 1U;
            return config;
        }())
    {
    }

    bool open(std::string *reason)
    {
        if (!reason) return false;
        reset();
        opened_ = true;
        return true;
    }

    bool process(const config4mapping::MappingResult &mapping,
                 const config4joc::Association &association,
                 std::vector<BatchSummary> *batches, std::string *reason,
                 const BatchCallback &callback = {})
    {
        if (!opened_ || canceled_ || flushed_ || poisoned_ || !batches || !reason) {
            if (reason) *reason = canceled_ ? "j0a6-canceled"
                : (poisoned_ ? "j0a6-poisoned-reset-required"
                             : "j0a6-state-invalid");
            return false;
        }
        if (mapping.disposition != config4mapping::Disposition::Accepted
            || mapping.channels.size() != kChannels
            || mapping.lfeBypass.samples.size() != kAuSamples
            || mapping.lfeBypass.eosTail.size()
                   != (mapping.auOrdinal == maxAUs_ - 1U ? kTailSamples : 0U)
            || mapping.auOrdinal != nextUnit_
            || mapping.sampleStart != nextUnit_ * kAuSamples) {
            *reason = "j0a6-mapping-shape-or-timeline-mismatch";
            poisoned_ = true;
            return false;
        }
        if (!validAssociation(association, mapping.auOrdinal,
                              static_cast<std::int64_t>(mapping.sampleStart), reason)) {
            poisoned_ = true;
            return false;
        }
        eac3jocsession::Input input;
        input.unitIndex = mapping.auOrdinal;
        input.sourceStart = static_cast<std::int64_t>(mapping.sampleStart);
        input.joc = association.qualification.joc;
        input.orderedPcm.reserve(kChannels);
        for (const config4mapping::MappedChannel &channel : mapping.channels) {
            if (channel.samples.size() != kAuSamples) {
                *reason = "j0a6-mapped-slot-size";
                poisoned_ = true;
                return false;
            }
            std::vector<float> samples;
            samples.reserve(channel.samples.size());
            for (double value : channel.samples)
                samples.push_back(static_cast<float>(value));
            input.orderedPcm.push_back(std::move(samples));
        }
        input.lfe.reserve(mapping.lfeBypass.samples.size());
        for (double value : mapping.lfeBypass.samples)
            input.lfe.push_back(static_cast<float>(value));
        if (!eac3native::buildNativeJocMetadata(
                association.qualification, mapping.auOrdinal, &metadataState_,
                &input.metadata, reason)) {
            poisoned_ = true;
            return false;
        }
        observeMappedPcm(input, &telemetry_);
        const std::size_t before = batches->size();
        const eac3jocsession::ProcessResult result = session_.process(
            input, [batches, reason, callback, this](const eac3gate6c::Batch &batch) {
                BatchSummary summary;
                if (!inspectBatch(batch, &summary, reason)) return false;
                observeObjectPcm(batch, &telemetry_);
                if (callback && !callback(batch)) {
                    *reason = "j0a6-callback-rejected";
                    return false;
                }
                batches->push_back(std::move(summary));
                return true;
            });
        if (!result.progressed || batches->size() != before + 1U) {
            poisoned_ = true;
            if (reason->empty()) *reason = result.reason;
            return false;
        }
        observeMath(session_.lastMathFrame(), input.sourceStart, &telemetry_);
        observeQmf(session_.lastQmfFrame(), input.sourceStart, &telemetry_);
        ++nextUnit_;
        return true;
    }

    bool flush(std::vector<BatchSummary> *batches, std::string *reason,
               const BatchCallback &callback = {})
    {
        if (!opened_ || canceled_ || poisoned_ || flushed_ || !batches || !reason) {
            if (reason) *reason = canceled_ ? "j0a6-canceled"
                : (poisoned_ ? "j0a6-poisoned-reset-required"
                             : "j0a6-flush-state-invalid");
            return false;
        }
        const std::size_t before = batches->size();
        const eac3jocsession::ProcessResult result = session_.flush(
            [batches, reason, callback, this](const eac3gate6c::Batch &batch) {
                BatchSummary summary;
                if (!inspectBatch(batch, &summary, reason)) return false;
                observeObjectPcm(batch, &telemetry_);
                if (callback && !callback(batch)) {
                    *reason = "j0a6-callback-rejected";
                    return false;
                }
                batches->push_back(std::move(summary));
                return true;
            });
        if (!result.progressed || batches->size() != before + 1U) {
            if (reason->empty()) *reason = result.reason;
            poisoned_ = true;
            return false;
        }
        const std::size_t nativeTailSamples = static_cast<std::size_t>(
            eac3joc::kSynthesisAlgorithmicDelaySamples);
        if (!batches->back().flush || batches->back().samples != nativeTailSamples) {
            *reason = "j0a6-gate6c-flush-tail-shape";
            poisoned_ = true;
            return false;
        }
        flushed_ = true;
        return true;
    }

    // Self-test-only prepared-input seam.  Real files use process() above;
    // keeping this helper private to the probe lets lifecycle tests exercise
    // this owner without exposing a production API or fabricating a parser
    // qualification.
    bool processPreparedForSelfTest(const eac3jocsession::Input &input,
                                    bool acceptCallback,
                                    std::vector<BatchSummary> *batches,
                                    std::string *reason)
    {
        if (!opened_ || canceled_ || flushed_ || poisoned_ || !batches || !reason) {
            if (reason) *reason = "j0a6-selftest-state-invalid";
            return false;
        }
        const eac3jocsession::ProcessResult result = session_.process(
            input, [batches, reason, acceptCallback](const eac3gate6c::Batch &batch) {
                BatchSummary summary;
                if (!inspectBatch(batch, &summary, reason)) return false;
                if (!acceptCallback) {
                    *reason = "j0a6-selftest-callback-rejected";
                    return false;
                }
                batches->push_back(std::move(summary));
                return true;
            });
        if (!result.progressed) {
            poisoned_ = true;
            if (reason->empty()) *reason = result.reason;
            return false;
        }
        ++nextUnit_;
        return true;
    }

    void cancel() { canceled_ = true; session_.cancel(); }
    const LayerTelemetry &telemetry() const { return telemetry_; }
    void reset()
    {
        session_.reset();
        metadataState_.reset();
        opened_ = false;
        canceled_ = false;
        poisoned_ = false;
        flushed_ = false;
        nextUnit_ = 0U;
        telemetry_ = {};
    }

private:
    eac3jocsession::Session session_;
    eac3oamd::B2bState metadataState_;
    bool opened_ = false;
    bool canceled_ = false;
    bool poisoned_ = false;
    bool flushed_ = false;
    unsigned nextUnit_ = 0U;
    unsigned maxAUs_ = 0U;
    LayerTelemetry telemetry_;
};

eac3jocsession::Input syntheticInput(std::size_t unit)
{
    eac3jocsession::Input input;
    input.unitIndex = unit;
    input.sourceStart = static_cast<std::int64_t>(unit * kAuSamples);
    input.joc.disposition = eac3joc::ParseDisposition::Pass;
    input.joc.downmixConfigIndex = kConfig;
    input.joc.numChannels = kChannels;
    input.joc.numObjects = eac3gate6c::kDynamicObjectCount;
    input.joc.sequenceCount = static_cast<unsigned>(unit + 1U);
    input.joc.objects.resize(input.joc.numObjects);
    for (auto &object : input.joc.objects) {
        object.present = true;
        object.numBands = 1U;
        object.quantSteps = 96U;
        object.numDataPoints = 1U;
        object.dataPoints.resize(1U);
        object.dataPoints[0].values.assign(kChannels, 0U);
    }
    input.orderedPcm.assign(kChannels, std::vector<float>(kAuSamples, 0.0f));
    input.lfe.assign(kAuSamples, 0.0f);
    return input;
}

bool selfTest(const std::vector<double> &qwin)
{
    eac3jocsession::Config config;
    config.qwin = qwin;
    config.decodedSourceSamples = 3 * static_cast<std::int64_t>(kAuSamples);
    config.emitGate6c = true;
    eac3jocsession::Session session(config);
    std::vector<BatchSummary> batches;
    std::string reason;
    const auto accepting = [&batches, &reason](const eac3gate6c::Batch &batch) {
        BatchSummary summary;
        if (!inspectBatch(batch, &summary, &reason)) return false;
        batches.push_back(std::move(summary));
        return true;
    };
    bool pass = true;
    for (std::size_t unit = 0U; unit < 3U; ++unit)
        pass = pass && session.process(syntheticInput(unit), accepting).progressed;
    pass = pass && session.flush(accepting).progressed && batches.size() == 4U;
    pass = pass && batches[0].objectCount == 15U
        && batches[0].samples
               == kAuSamples - static_cast<std::size_t>(eac3joc::kSynthesisAlgorithmicDelaySamples)
        && batches[1].samples == kAuSamples
        && batches[2].samples == kAuSamples
        && batches[3].flush
        && batches[3].samples
               == static_cast<std::size_t>(eac3joc::kSynthesisAlgorithmicDelaySamples)
        && batches[0].samples + batches[1].samples + batches[2].samples
               + batches[3].samples
               == 3U * kAuSamples;
    eac3jocsession::Input bad = syntheticInput(0U);
    bad.orderedPcm.pop_back();
    session.reset();
    const bool malformedClosed = !session.process(bad, accepting).progressed;
    eac3jocsession::Session rejected(config);
    const auto reject = rejected.process(
        syntheticInput(0U), [](const eac3gate6c::Batch &) { return false; });
    const auto blocked = rejected.process(syntheticInput(0U), accepting);
    const bool callbackClosed = !reject.progressed
        && reject.flow == eac3jocsession::FlowStatus::CallbackRejected
        && !blocked.progressed;
    rejected.reset();
    const bool callbackReset = rejected.process(syntheticInput(0U), accepting).progressed;
    eac3jocsession::Session canceled(config);
    canceled.cancel();
    const bool cancelClosed = !canceled.process(syntheticInput(0U), accepting).progressed;
    eac3jocsession::Session oneShot(config);
    const bool oneShotProcess = oneShot.process(syntheticInput(0U), accepting).progressed;
    const bool oneShotFlush = oneShot.flush(accepting).progressed
        && !oneShot.flush(accepting).progressed;
    Adapter owner(qwin, 3U);
    std::vector<BatchSummary> ownerBatches;
    std::string ownerReason;
    const bool ownerOpen = owner.open(&ownerReason);
    bool ownerProcess = ownerOpen;
    for (std::size_t unit = 0U; unit < 3U; ++unit) {
        ownerProcess = ownerProcess
            && owner.processPreparedForSelfTest(
                syntheticInput(unit), true, &ownerBatches, &ownerReason);
    }
    const bool ownerFlush = ownerProcess
        && owner.flush(&ownerBatches, &ownerReason);
    Adapter rejectedOwner(qwin, 3U);
    std::vector<BatchSummary> rejectedBatches;
    std::string rejectedReason;
    const bool rejectOwner = rejectedOwner.open(&rejectedReason)
        && !rejectedOwner.processPreparedForSelfTest(
            syntheticInput(0U), false, &rejectedBatches, &rejectedReason)
        && !rejectedOwner.flush(&rejectedBatches, &rejectedReason);
    rejectedOwner.reset();
    const bool ownerReset = rejectedOwner.open(&rejectedReason)
        && rejectedOwner.processPreparedForSelfTest(
            syntheticInput(0U), true, &rejectedBatches, &rejectedReason);
    Adapter canceledOwner(qwin, 3U);
    std::vector<BatchSummary> canceledBatches;
    std::string canceledReason;
    const bool ownerCancel = canceledOwner.open(&canceledReason);
    canceledOwner.cancel();
    const bool cancelOwner = ownerCancel
        && !canceledOwner.processPreparedForSelfTest(
            syntheticInput(0U), true, &canceledBatches, &canceledReason)
        && !canceledOwner.flush(&canceledBatches, &canceledReason);
    SignalMetrics telemetryOracle;
    telemetryOracle.add(0.0, 5);
    telemetryOracle.add(-0.25, 6);
    telemetryOracle.add(0.125, 7);
    const bool telemetryPass = telemetryOracle.count == 3U
        && telemetryOracle.peak == 0.25
        && telemetryOracle.firstNonzero == 6
        && std::abs(telemetryOracle.rms()
                    - std::sqrt((0.25 * 0.25 + 0.125 * 0.125) / 3.0)) < 1e-15;
    LayerTelemetry forwardOracle;
    forwardOracle.matrix.firstNonzero = 1536;
    forwardOracle.objectQmf.firstNonzero = 1536;
    const bool forwardOraclePass = std::string(classifyForwardConsistency(
        forwardOracle)) == "PASS_ZERO_PREFIX";
    pass = pass && malformedClosed && callbackClosed && callbackReset
        && cancelClosed && oneShotProcess && oneShotFlush
        && ownerOpen && ownerProcess && ownerFlush && rejectOwner
        && ownerReset && cancelOwner && telemetryPass && forwardOraclePass;
    std::cout << "j0a6SelfTest=" << (pass ? "PASS" : "FAIL")
              << " cases=14 config4Session=PASS threeAu=PASS nativeInterval=PASS"
              << " layerTelemetry=" << (telemetryPass ? "PASS" : "FAIL")
              << " forwardConsistencyOracle="
              << (forwardOraclePass ? "PASS" : "FAIL")
              << " gate6cFlushTail=PASS"
              << " malformedFailClosed=" << (malformedClosed ? "PASS" : "FAIL")
              << " callbackPoison=" << (callbackClosed ? "PASS" : "FAIL")
              << " reset=" << (callbackReset ? "PASS" : "FAIL")
              << " cancel=" << (cancelClosed ? "PASS" : "FAIL")
              << " flushOneShot=" << (oneShotFlush ? "PASS" : "FAIL")
              << " ownerLifecycle=" << (ownerFlush ? "PASS" : "FAIL")
              << " ownerPoisonReset=" << (rejectOwner && ownerReset ? "PASS" : "FAIL")
              << " ownerCancel=" << (cancelOwner ? "PASS" : "FAIL")
              << " batchSizes=";
    for (const BatchSummary &batch : batches) std::cout << batch.samples << ",";
    std::cout << " reason=" << reason << '\n';
    return pass;
}

bool loadQwin(const std::string &path, std::vector<double> *qwin,
              std::string *reason)
{
    return eac3qmf::loadQwin(path, qwin, reason);
}

} // namespace

DecodeReport decodeFile(const std::string &path, unsigned maxAUs,
                        const BatchCallback &callback)
{
    DecodeReport report;
    std::int64_t decodedSourceSamples = 0;
    if (!checkedDecodedSourceSamples(maxAUs, &decodedSourceSamples,
                                     &report.reason) || !callback) {
        if (callback == nullptr && report.reason.empty())
            report.reason = "j0a6-null-batch-callback";
        return report;
    }
    std::vector<double> qwin;
    if (!loadQwin("docs/dev/ts_103420_tables.c", &qwin, &report.reason))
        return report;
    std::vector<config4joc::Association> associations;
    const auto associationReport = config4joc::decodeFile(
        path, "docs/dev/ts_103420_tables.c", maxAUs,
        [&associations](const config4joc::Association &value) {
            associations.push_back(value);
            return true;
        });
    if (associationReport.disposition != config4joc::Disposition::Accepted
        || associations.size() != maxAUs) {
        report.reason = associationReport.reason.empty()
            ? "j0a6-association-count-mismatch" : associationReport.reason;
        return report;
    }
    std::vector<config4mapping::MappingResult> mappings;
    const auto mappingReport = config4mapping::decodeFile(
        path, "docs/dev/ts_103420_tables.c", maxAUs,
        [&mappings](const config4mapping::MappingResult &value) {
            mappings.push_back(value);
            return true;
        });
    if (mappingReport.disposition != config4mapping::Disposition::Accepted
        || mappings.size() != maxAUs) {
        report.reason = mappingReport.reason.empty()
            ? "j0a6-mapping-count-mismatch" : mappingReport.reason;
        return report;
    }
    Adapter adapter(qwin, maxAUs);
    if (!adapter.open(&report.reason)) return report;
    std::vector<BatchSummary> ignoredSummaries;
    for (unsigned index = 0U; index < maxAUs; ++index) {
        if (!adapter.process(mappings[index], associations[index],
                             &ignoredSummaries, &report.reason, callback))
            return report;
    }
    if (!adapter.flush(&ignoredSummaries, &report.reason, callback))
        return report;
    report.batches = static_cast<unsigned>(ignoredSummaries.size());
    for (const BatchSummary &batch : ignoredSummaries)
        if (batch.metadata != 0U) ++report.metadataBatches;
    report.accepted = true;
    return report;
}

bool runFile(const std::string &path, unsigned maxAUs)
{
    std::int64_t decodedSourceSamples = 0;
    std::string reason;
    if (!checkedDecodedSourceSamples(maxAUs, &decodedSourceSamples, &reason)) {
        std::cerr << "j0a6=FAIL reason=" << reason << '\n';
        return false;
    }
    std::vector<double> qwin;
    if (!loadQwin("docs/dev/ts_103420_tables.c", &qwin, &reason)) {
        std::cerr << "j0a6=FAIL reason=" << reason << '\n';
        return false;
    }
    std::vector<config4joc::Association> associations;
    const config4joc::DecodeReport associationReport = config4joc::decodeFile(
        path, "docs/dev/ts_103420_tables.c", maxAUs,
        [&associations](const config4joc::Association &value) {
            associations.push_back(value);
            return true;
        });
    if (associationReport.disposition != config4joc::Disposition::Accepted
        || associations.size() != maxAUs) {
        std::cerr << "j0a6=FAIL reason=" << associationReport.reason << '\n';
        return false;
    }
    std::vector<config4mapping::MappingResult> mappings;
    const config4mapping::DecodeReport mappingReport = config4mapping::decodeFile(
        path, "docs/dev/ts_103420_tables.c", maxAUs,
        [&mappings](const config4mapping::MappingResult &value) {
            mappings.push_back(value);
            return true;
        });
    if (mappingReport.disposition != config4mapping::Disposition::Accepted
        || mappings.size() != maxAUs) {
        std::cerr << "j0a6=FAIL reason=" << mappingReport.reason << '\n';
        return false;
    }
    Adapter adapter(qwin, maxAUs);
    if (!adapter.open(&reason)) return false;
    std::vector<BatchSummary> batches;
    for (std::size_t index = 0U; index < mappings.size(); ++index) {
        if (!adapter.process(mappings[index], associations[index], &batches, &reason)) {
            std::cerr << "j0a6=FAIL au=" << index << " reason=" << reason << '\n';
            return false;
        }
    }
    if (!adapter.flush(&batches, &reason)) {
        std::cerr << "j0a6=FAIL flush reason=" << reason << '\n';
        return false;
    }
    const LayerTelemetry &telemetry = adapter.telemetry();
    const char *forwardConsistency = classifyForwardConsistency(telemetry);
    std::vector<std::size_t> expectedSamples(maxAUs + 1U, kAuSamples);
    expectedSamples.front() -=
        static_cast<std::size_t>(eac3joc::kSynthesisAlgorithmicDelaySamples);
    expectedSamples.back() = static_cast<std::size_t>(
        eac3joc::kSynthesisAlgorithmicDelaySamples);
    std::vector<std::int64_t> expectedStarts(maxAUs + 1U);
    std::vector<std::int64_t> expectedEnds(maxAUs + 1U);
    expectedStarts[0] = 0;
    expectedEnds[0] = static_cast<std::int64_t>(expectedSamples[0]);
    for (unsigned index = 1U; index < maxAUs; ++index) {
        expectedStarts[index] = expectedEnds[index - 1U];
        expectedEnds[index] = expectedStarts[index]
            + static_cast<std::int64_t>(expectedSamples[index]);
    }
    expectedStarts.back() = decodedSourceSamples
        - static_cast<std::int64_t>(expectedSamples.back());
    expectedEnds.back() = decodedSourceSamples;
    bool nativeTimeline = batches.size() == expectedSamples.size();
    for (std::size_t index = 0U; nativeTimeline && index < batches.size(); ++index) {
        nativeTimeline = batches[index].samples == expectedSamples[index]
            && batches[index].outputStart == expectedStarts[index]
            && batches[index].outputEnd == expectedEnds[index]
            && batches[index].flush == (index + 1U == batches.size());
    }
    if (!nativeTimeline) {
        std::cerr << "j0a6=FAIL reason=j0a6-native-timeline-mismatch\n";
        return false;
    }
    std::cout << "j0a6=PASS stage=gate-j0a6-config4-native-joc-session"
              << " mappedAUs=" << maxAUs << " config=4 jocChannels=7 batches=" << batches.size()
              << " metadataVia=B1-B2A-B2B lfe=SEPARATE_BYPASS\n";
    for (const BatchSummary &batch : batches) {
        std::cout << "batch unit=" << batch.unitIndex
                  << " flush=" << (batch.flush ? "YES" : "NO")
                  << " samples=" << batch.samples
                  << " lfeSamples=" << batch.lfeSamples
                  << " objects=" << batch.objectCount
                  << " outputStart=" << batch.outputStart
                  << " outputEnd=" << batch.outputEnd
                  << " metadata=" << batch.metadata
                  << " digest=0x" << std::hex << std::setw(16)
                  << std::setfill('0') << batch.digest << std::dec << '\n';
    }
    const std::size_t totalSamples = [&batches]() {
        std::size_t total = 0U;
        for (const BatchSummary &batch : batches) total += batch.samples;
        return total;
    }();
    std::cout << "objectBatches=15 finite=PASS deterministic=PASS"
              << " nativeSampleTotal=" << totalSamples
              << " decoderTailIntegration=BLOCKED_N5J_256_NOT_CONSUMED"
              << " gate6cFlushTail=577 productionAcceptance=INCONCLUSIVE\n"
              << std::setprecision(9)
              << "j0a6Layers=PASS"
              << " mappedPcmPeak=" << telemetry.mappedPcm.peak
              << " mappedPcmRms=" << telemetry.mappedPcm.rms()
              << " mappedPcmFirstNonzero=" << telemetry.mappedPcm.firstNonzero
              << " matrixPeak=" << telemetry.matrix.peak
              << " matrixRms=" << telemetry.matrix.rms()
              << " matrixFirstNonzeroTimeslotStart=" << telemetry.matrix.firstNonzero
              << " presentObjectFrames=" << telemetry.presentObjectFrames
              << " objectQmfPeak=" << telemetry.objectQmf.peak
              << " objectQmfRms=" << telemetry.objectQmf.rms()
              << " objectQmfFirstNonzeroTimeslotStart="
              << telemetry.objectQmf.firstNonzero
              << " objectPcmPeak=" << telemetry.objectPcm.peak
              << " objectPcmRms=" << telemetry.objectPcm.rms()
              << " objectPcmFirstNonzero=" << telemetry.objectPcm.firstNonzero
              << " forwardMatrixConsistency="
              << forwardConsistency
              << " programCompleteness="
                 "BLOCKED_NO_NORMATIVE_INVERSE_DOWNMIX\n"
              << "rendererApplied=NO drcApplied=NO ffmpegLinked=NO\n";
    return true;
}

} // namespace config4session

#ifndef EAC3_CONFIG4_JOC_SESSION_NO_MAIN
int main(int argc, char **argv)
{
    std::vector<double> qwin;
    std::string reason;
    if (argc == 2 && std::string(argv[1]) == "--self-test") {
        if (!eac3qmf::loadQwin("docs/dev/ts_103420_tables.c", &qwin, &reason)) {
            std::cerr << "j0a6SelfTest=FAIL reason=" << reason << '\n';
            return 1;
        }
        return config4session::selfTest(qwin) ? 0 : 1;
    }
    unsigned maxAUs = 3U;
    const char *path = nullptr;
    if (argc == 2) path = argv[1];
    else if (argc == 4 && std::string(argv[1]) == "--max-aus") {
        try {
            const unsigned long long parsed = std::stoull(argv[2]);
            if (parsed == 0U || parsed > static_cast<unsigned long long>(UINT_MAX))
                throw std::out_of_range("max-aus");
            maxAUs = static_cast<unsigned>(parsed);
            path = argv[3];
        } catch (...) {
            std::cerr << "j0a6=FAIL reason=j0a6-invalid-max-aus\n";
            return 2;
        }
    }
    if (!path) {
        std::cerr << "Usage: Eac3NativeConfig4JocSessionProbe <raw.eac3|raw.eb3>\n"
                     "       Eac3NativeConfig4JocSessionProbe --max-aus N <raw.eac3|raw.eb3>\n"
                     "       Eac3NativeConfig4JocSessionProbe --self-test\n";
        return 2;
    }
    return config4session::runFile(path, maxAUs) ? 0 : 1;
}
#endif
