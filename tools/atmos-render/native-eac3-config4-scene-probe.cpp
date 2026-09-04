// J0A7: diagnostic prepared-scene entry for native config-4 Gate6C output.
// PCM stays owned by this probe; SceneAdapter produces metadata-derived gain
// frames, but no object PCM is mixed to a speaker bus.

#include "scene-adapter.h"
#include "scene-object-bridge.h"
#include "native-eac3-config4-scene.h"
#include "../atmos-joc-probe/native-eac3-config4-joc-session.h"
#include "../atmos-joc-probe/native-eac3-core.h"
#include "../atmos-joc-probe/native-eac3-joc-session-bridge.h"
#include "../atmos-joc-probe/joc-qmf.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <stdexcept>
#include <vector>

namespace config4scene {
namespace {
constexpr std::uint64_t kGeneration = eac3render::kInitialGeneration;
constexpr std::size_t kObjects = eac3gate6c::kDynamicObjectCount;

using Callback = PreparedCallback;

bool finiteVector(const std::vector<float> &values)
{
    return std::all_of(values.begin(), values.end(),
                       [](float value) { return std::isfinite(value); });
}

eac3render::SceneObjectBatchContext context(std::uint64_t generation)
{
    eac3render::SceneObjectBatchContext value;
    value.property.generation = generation;
    value.property.coordinate =
        eac3render::ObjectCoordinateRepresentation::OamdCartesian;
    value.coordinatePolicy = {10.0, 10.0, 7.0, 0.5, 0.5, 0.0};
    return value;
}

bool prepare(const eac3gate6c::Batch &batch, std::uint64_t generation,
             eac3render::SceneAdapter *adapter, PreparedBatch *prepared,
             std::string *reason)
{
    if (!adapter || !prepared || !reason || batch.objects.size() != kObjects
        || batch.lfe.empty() || batch.outputEnd <= batch.outputStart) {
        if (reason) *reason = "j0a7-batch-shape-or-time-range";
        return false;
    }
    const std::size_t samples = batch.objects.front().size();
    if (samples == 0U || batch.lfe.size() != samples
        || static_cast<std::size_t>(batch.outputEnd - batch.outputStart) != samples) {
        *reason = "j0a7-output-interval-sample-mismatch";
        return false;
    }
    std::vector<const float *> pointers;
    pointers.reserve(kObjects + 1U);
    for (const auto &channel : batch.objects) {
        if (channel.size() != samples || !finiteVector(channel)) {
            *reason = "j0a7-object-vector-invalid";
            return false;
        }
        pointers.push_back(channel.data());
    }
    if (!finiteVector(batch.lfe)) {
        *reason = "j0a7-lfe-vector-invalid";
        return false;
    }
    pointers.push_back(batch.lfe.data());
    for (std::size_t i = 0U; i < pointers.size(); ++i)
        for (std::size_t j = i + 1U; j < pointers.size(); ++j)
            if (pointers[i] == pointers[j]) {
                *reason = "j0a7-input-channel-alias";
                return false;
            }
    if ((!batch.flush && batch.metadata.size() != kObjects)
        || (batch.flush && !batch.metadata.empty())) {
        *reason = "j0a7-metadata-shape";
        return false;
    }
    PreparedBatch value;
    value.unitIndex = batch.unitIndex;
    value.flush = batch.flush;
    value.outputStart = batch.outputStart;
    value.outputEnd = batch.outputEnd;
    value.coordinatePolicy = context(generation).coordinatePolicy;
    value.objects.reserve(kObjects);
    for (std::size_t i = 0U; i < kObjects; ++i) {
        PreparedObject object;
        object.generation = generation;
        object.objectId = i + 1U;
        object.timestampSamples = batch.flush ? batch.outputStart
            : batch.metadata[i].sourcePosition;
        object.durationSamples = batch.flush ? static_cast<std::int64_t>(samples)
            : static_cast<std::int64_t>(batch.metadata[i].rampDuration);
        object.samples = batch.objects[i];
        if (!batch.flush && batch.metadata[i].objectIndex != i + 1U) {
            *reason = "j0a7-object-identity-order";
            return false;
        }
        value.objects.push_back(std::move(object));
    }
    value.lfe.generation = generation;
    value.lfe.timestampSamples = batch.outputStart;
    value.lfe.durationSamples = static_cast<std::int64_t>(samples);
    value.lfe.samples = batch.lfe;
    if (!batch.flush) {
        const auto converted = eac3render::adaptGate6cBatch(
            batch.metadata, context(generation));
        if (converted.disposition != eac3render::SceneAdapterDisposition::Accepted) {
            *reason = converted.reason.empty() ? "j0a7-scene-property-rejected"
                                                : converted.reason;
            return false;
        }
        eac3render::SceneAdapterUpdate update;
        update.generation = generation;
        update.timestampSamples = converted.targetGroup.timestampSamples;
        update.durationSamples = converted.targetGroup.durationSamples;
        for (std::size_t i = 0U; i < converted.targetGroup.objects.size(); ++i) {
            update.objects.push_back(converted.targetGroup.objects[i].update);
            value.objects[i].sceneUpdate = converted.targetGroup.objects[i].update;
            value.objects[i].sceneProperties = converted.properties[i];
            value.objects[i].roomPosition = converted.targetGroup.objects[i].roomPosition;
        }
        // SceneAdapter's LFE value is a gain, not decoded PCM amplitude.  The
        // actual LFE vector remains separate and this zero marker carries only
        // presence; no source gain is inferred.
        update.lfeEnabled = true;
        update.lfeGain = 0.0F;
        const auto result = adapter->pushUpdate(update);
        if (!result.accepted()) {
            *reason = result.reason.empty() ? "j0a7-scene-adapter-rejected"
                                             : result.reason;
            return false;
        }
        value.sceneFrame = result.frame;
    } else {
        const auto result = adapter->renderAt(generation, batch.outputStart);
        if (!result.accepted()) {
            *reason = result.reason.empty() ? "j0a7-flush-scene-frame-rejected"
                                             : result.reason;
            return false;
        }
        value.sceneFrame = result.frame;
    }
    *prepared = std::move(value);
    return true;
}

class Owner final {
public:
    Owner() : adapter_(eac3render::MetadataGapPolicy::HoldLast) {}
    bool open(std::string *reason)
    {
        if (!reason || opened_) {
            if (reason) *reason = "j0a7-open-state";
            return false;
        }
        if (adapter_.generation() != generation_
            && !adapter_.reset(generation_).accepted()) {
            *reason = "j0a7-scene-reset-failed";
            return false;
        }
        opened_ = true;
        return true;
    }
    bool push(const eac3gate6c::Batch &batch, const Callback &callback,
              std::string *reason)
    {
        if (!opened_ || canceled_ || poisoned_ || flushed_ || !callback) {
            if (reason) *reason = canceled_ ? "j0a7-canceled"
                : (poisoned_ ? "j0a7-poisoned-reset-required"
                             : "j0a7-owner-state-invalid");
            return false;
        }
        PreparedBatch value;
        if (!prepare(batch, generation_, &adapter_, &value, reason)) {
            poisoned_ = true;
            return false;
        }
        if (!callback(value)) {
            poisoned_ = true;
            if (reason && reason->empty()) *reason = "j0a7-callback-rejected";
            return false;
        }
        if (batch.flush) flushed_ = true;
        return true;
    }
    void cancel() { canceled_ = true; }
    void reset()
    {
        ++generation_;
        adapter_.reset(generation_);
        opened_ = false;
        canceled_ = false;
        poisoned_ = false;
        flushed_ = false;
    }
private:
    eac3render::SceneAdapter adapter_;
    bool opened_ = false;
    bool canceled_ = false;
    bool poisoned_ = false;
    bool flushed_ = false;
    std::uint64_t generation_ = kGeneration;
};

eac3gate6c::Batch syntheticBatch()
{
    eac3gate6c::Batch batch;
    batch.outputEnd = 8;
    batch.objects.assign(kObjects, std::vector<float>(8U, 0.0F));
    batch.lfe.assign(8U, 0.0F);
    for (std::size_t i = 0U; i < kObjects; ++i) {
        eac3gate6c::MetadataUpdate record;
        record.rampDuration = 8U;
        record.objectIndex = static_cast<unsigned>(i + 1U);
        record.state.objectIndex = record.objectIndex;
        record.state.basicValid = true;
        record.state.renderValid = true;
        record.state.active = true;
        record.state.gainDb = 0.0F;
        record.state.position.valid = true;
        record.state.position.codedX = 0.5F;
        record.state.position.codedY = 0.25F;
        record.state.position.codedZ = 0.0F;
        record.state.position.standardX = 0.5F;
        record.state.position.standardY = 0.25F;
        record.state.position.standardZ = 0.0F;
        record.state.position.x = 0.5F;
        record.state.position.y = 0.25F;
        record.state.position.z = 0.0F;
        record.state.effectiveSizePresent = false;
        record.state.size = {0.0F, 0.0F, 0.0F};
        batch.metadata.push_back(record);
    }
    return batch;
}

bool selfTest()
{
    std::size_t cases = 0U;
    bool pass = true;
    const Callback accept = [](const PreparedBatch &batch) {
        const std::int64_t sceneTimestamp = batch.flush
            ? batch.outputStart : batch.objects.front().timestampSamples;
        return batch.objects.size() == kObjects && batch.lfe.samples.size() == 8U
            && batch.sceneFrame.generation == batch.objects.front().generation
            && batch.sceneFrame.timestampSamples == sceneTimestamp
            && batch.sceneFrame.objects.size() == kObjects
            && batch.coordinatePolicy.roomWidthMetres == 10.0
            && batch.objects.front().roomPosition
                   == std::array<float, 3>({0.5F, 0.25F, 0.0F});
    };
    std::string reason;
    Owner owner;
    pass = owner.open(&reason) && owner.push(syntheticBatch(), accept, &reason);
    ++cases;
    Owner rejected;
    const bool callbackPoison = rejected.open(&reason)
        && !rejected.push(syntheticBatch(), [](const PreparedBatch &) { return false; }, &reason)
        && !rejected.push(syntheticBatch(), accept, &reason);
    ++cases;
    rejected.reset();
    const bool reset = rejected.open(&reason)
        && rejected.push(syntheticBatch(), accept, &reason);
    ++cases;
    Owner canceled;
    const bool cancelOpen = canceled.open(&reason);
    canceled.cancel();
    const bool cancel = cancelOpen
        && !canceled.push(syntheticBatch(), accept, &reason);
    ++cases;
    eac3render::SceneAdapter stale;
    stale.reset(2U);
    eac3render::SceneAdapterUpdate future;
    future.generation = 2U;
    future.timestampSamples = 10;
    future.durationSamples = 4;
    future.objects.push_back({1U, 15, 1.0F, {1.0, 0.0, 0.0}, false});
    const auto futureResult = stale.pushUpdate(future);
    eac3render::SceneAdapter oldGeneration;
    const auto oldResult = oldGeneration.pushUpdate(future);
    ++cases;
    const bool staleChecks = futureResult.disposition
            == eac3render::SceneAdapterDisposition::InvalidInput
        && oldResult.disposition == eac3render::SceneAdapterDisposition::Stale;
    pass = pass && callbackPoison && reset && cancel && staleChecks;
    std::cout << "j0a7SelfTest=" << (pass ? "PASS" : "FAIL")
              << " cases=" << cases << " objectIdentity=15 lfeSeparate=PASS"
              << " callbackPoison=" << (callbackPoison ? "PASS" : "FAIL")
              << " reset=" << (reset ? "PASS" : "FAIL")
              << " cancel=" << (cancel ? "PASS" : "FAIL")
              << " staleGeneration=" << (oldResult.disposition
                    == eac3render::SceneAdapterDisposition::Stale ? "PASS" : "FAIL")
              << " futureMetadata=" << (futureResult.disposition
                    == eac3render::SceneAdapterDisposition::InvalidInput ? "PASS" : "FAIL")
              << " reason=" << reason << '\n';
    return pass;
}

} // namespace

bool decodeFile(const std::string &path, unsigned maxAUs,
                const PreparedCallback &callback, std::string *reason)
{
    if (!reason || maxAUs == 0U || !callback) {
        if (reason) *reason = maxAUs == 0U ? "j0a7-invalid-max-aus"
                                           : "j0a7-null-prepared-callback";
        return false;
    }
    Owner owner;
    if (!owner.open(reason)) return false;
    const auto report = config4session::decodeFile(
        path, maxAUs, [&owner, &callback, reason](const eac3gate6c::Batch &batch) {
            return owner.push(batch, callback, reason);
        });
    if (!report.accepted) {
        *reason = report.reason.empty() ? "j0a7-session-rejected" : report.reason;
        return false;
    }
    return true;
}

bool decodeConfig3File(const std::string &path, unsigned maxAUs,
                       const PreparedCallback &callback, std::string *reason)
{
    if (!reason || maxAUs == 0U || !callback) {
        if (reason) *reason = maxAUs == 0U ? "j0a7-config3-invalid-max-aus"
                                           : "j0a7-config3-null-callback";
        return false;
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        *reason = "j0a7-config3-input-open-failed";
        return false;
    }
    const std::vector<std::uint8_t> bytes(
        std::istreambuf_iterator<char>(stream), {});
    std::vector<std::vector<std::uint8_t>> frames;
    frames.reserve(maxAUs);
    std::size_t offset = 0U;
    while (offset < bytes.size() && frames.size() < maxAUs) {
        const auto parsed = eac3native::parseSyncframe(bytes, offset);
        if (parsed.disposition != eac3native::Disposition::Accepted
            || !parsed.frame || parsed.frame->sizeBytes == 0U
            || parsed.frame->offset != offset
            || parsed.frame->sizeBytes > bytes.size() - offset) {
            *reason = parsed.reason.empty() ? "j0a7-config3-frame-parse-failed"
                                            : parsed.reason;
            return false;
        }
        frames.emplace_back(bytes.begin() + offset,
                            bytes.begin() + offset + parsed.frame->sizeBytes);
        offset += parsed.frame->sizeBytes;
    }
    if (frames.size() != maxAUs) {
        *reason = "j0a7-config3-fewer-than-requested-aus";
        return false;
    }
    std::vector<double> qwin;
    if (!eac3qmf::loadQwin("docs/dev/ts_103420_tables.c", &qwin, reason))
        return false;
    eac3native::JocSessionBridgeConfig config;
    config.qwin = std::move(qwin);
    config.decodedSourceSamples = static_cast<std::int64_t>(maxAUs) * 1536;
    eac3native::NativeEac3JocSessionBridge bridge(
        "docs/dev/ts_103420_tables.c", std::move(config));
    if (bridge.open().disposition
        != eac3native::JocSessionBridgeDisposition::Accepted) {
        *reason = "j0a7-config3-bridge-open-failed";
        return false;
    }
    Owner owner;
    if (!owner.open(reason)) return false;
    const auto gate6cCallback = [&owner, &callback, reason](
                                   const eac3gate6c::Batch &batch) {
        return owner.push(batch, callback, reason);
    };
    for (const auto &frame : frames) {
        const auto processed = bridge.process(frame, gate6cCallback);
        if (processed.disposition
            != eac3native::JocSessionBridgeDisposition::Accepted) {
            if (reason->empty()) {
                *reason = processed.reason.empty()
                    ? "j0a7-config3-bridge-process-failed" : processed.reason;
            }
            return false;
        }
    }
    const auto flushed = bridge.flush(gate6cCallback);
    if (flushed.disposition
        != eac3native::JocSessionBridgeDisposition::Accepted) {
        *reason = flushed.reason.empty()
            ? "j0a7-config3-bridge-flush-failed" : flushed.reason;
        return false;
    }
    return true;
}

bool checkedTotalSamples(unsigned maxAUs, std::int64_t *total)
{
    if (!total || maxAUs == 0U
        || maxAUs == std::numeric_limits<unsigned>::max()
        || static_cast<std::uint64_t>(maxAUs)
               > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
                   / 1536U) {
        return false;
    }
    *total = static_cast<std::int64_t>(maxAUs) * 1536;
    return true;
}

bool runFile(const std::string &path, unsigned maxAUs, unsigned jocConfig)
{
    std::int64_t totalSamples = 0;
    if (!checkedTotalSamples(maxAUs, &totalSamples)) {
        std::cerr << "j0a7=FAIL reason=j0a7-invalid-max-aus\n";
        return false;
    }
    Owner owner;
    std::string reason;
    if (!owner.open(&reason)) return false;
    std::size_t callbacks = 0U;
    std::vector<std::size_t> sizes;
    std::vector<std::int64_t> starts;
    std::vector<std::int64_t> sceneTimestamps;
    bool independent = true;
    struct PropertyWindow {
        const char *name;
        std::int64_t start;
        std::int64_t end;
        std::size_t updates = 0U;
        std::size_t listenerCentre = 0U;
        std::size_t nonzeroSize = 0U;
        std::size_t nonzeroPriority = 0U;
        std::size_t restrictedZone = 0U;
        std::size_t snap = 0U;
        std::size_t screen = 0U;
        std::size_t distance = 0U;
    };
    std::array<PropertyWindow, 3> windows {{
        {"A", 75 * 48000, 90 * 48000},
        {"B", 114 * 48000, 125 * 48000},
        {"C", 50 * 48000, 60 * 48000},
    }};
    const auto callback = [&callbacks, &sizes, &starts, &sceneTimestamps,
                           &independent, &windows](
                              const PreparedBatch &batch) {
        ++callbacks;
        sizes.push_back(static_cast<std::size_t>(batch.outputEnd - batch.outputStart));
        starts.push_back(batch.outputStart);
        sceneTimestamps.push_back(batch.sceneFrame.timestampSamples);
        std::vector<const float *> pointers;
        for (const auto &object : batch.objects) pointers.push_back(object.samples.data());
        pointers.push_back(batch.lfe.samples.data());
        for (std::size_t i = 0U; i < pointers.size(); ++i)
            for (std::size_t j = i + 1U; j < pointers.size(); ++j)
                independent = independent && pointers[i] != pointers[j];
        if (!batch.flush) {
            for (const auto &object : batch.objects) {
                const auto &property = object.sceneProperties;
                for (auto &window : windows) {
                    if (property.timestampSamples < window.start
                        || property.timestampSamples >= window.end) continue;
                    ++window.updates;
                    window.listenerCentre += property.roomPosition
                        == std::array<float, 3>({0.5F, 0.5F, 0.0F});
                    window.nonzeroSize += property.extentPresence
                        == eac3render::ObjectExtentPresence::NonZero;
                    window.nonzeroPriority += property.priority != 0.0F;
                    window.restrictedZone += std::any_of(
                        property.zoneConstraints.begin(), property.zoneConstraints.end(),
                        [](bool enabled) { return !enabled; });
                    window.snap += property.snap;
                    window.screen += property.screenAnchored;
                    window.distance += property.distanceSpecified;
                }
            }
        }
        const std::int64_t sceneTimestamp = batch.flush
            ? batch.outputStart : batch.objects.front().timestampSamples;
        const bool accepted = batch.objects.size() == kObjects
            && batch.lfe.samples.size() == sizes.back()
            && batch.sceneFrame.generation == kGeneration
            && batch.sceneFrame.timestampSamples == sceneTimestamp
            && batch.sceneFrame.objects.size() == kObjects;
        if (!accepted) {
            std::cerr << "j0a7-scene-frame-mismatch batchStart=" << batch.outputStart
                      << " frameGeneration=" << batch.sceneFrame.generation
                      << " frameTimestamp=" << batch.sceneFrame.timestampSamples
                      << " frameObjects=" << batch.sceneFrame.objects.size() << '\n';
        }
        return accepted;
    };
    bool decodeAccepted = false;
    std::size_t metadataBatches = 0U;
    if (jocConfig == 3U) {
        decodeAccepted = decodeConfig3File(path, maxAUs, callback, &reason);
        metadataBatches = decodeAccepted ? maxAUs : 0U;
    } else if (jocConfig == 4U) {
        const auto report = config4session::decodeFile(
            path, maxAUs, [&owner, &callback, &reason](const eac3gate6c::Batch &batch) {
                return owner.push(batch, callback, &reason);
            });
        decodeAccepted = report.accepted;
        metadataBatches = report.metadataBatches;
    } else {
        std::cerr << "j0a7=FAIL reason=j0a7-invalid-joc-config\n";
        return false;
    }
    std::vector<std::size_t> expected(maxAUs + 1U, 1536U);
    expected.front() = 959U;
    expected.back() = 577U;
    std::vector<std::int64_t> expectedStarts(maxAUs + 1U);
    std::vector<std::int64_t> expectedSceneTimestamps(maxAUs + 1U);
    expectedStarts[0] = 0;
    expectedSceneTimestamps[0] = 0;
    for (unsigned index = 1U; index < maxAUs; ++index) {
        expectedStarts[index] = 959 + static_cast<std::int64_t>(index - 1U) * 1536;
        expectedSceneTimestamps[index] = static_cast<std::int64_t>(index) * 1536;
    }
    expectedStarts.back() = totalSamples - 577;
    expectedSceneTimestamps.back() = expectedStarts.back();
    const bool timeline = decodeAccepted && sizes == expected
        && starts == expectedStarts && sceneTimestamps == expectedSceneTimestamps;
    const bool metadata = metadataBatches == maxAUs;
    std::cout << "j0a7=" << (timeline && metadata && independent ? "PASS" : "FAIL")
              << " stage=prepared-scene-entry jocConfig=" << jocConfig
              << " batches=" << callbacks << " sizes=";
    for (std::size_t value : sizes) std::cout << value << ',';
    std::cout << " starts=";
    for (std::int64_t value : starts) std::cout << value << ',';
    std::cout << " sceneTimestamps=";
    for (std::int64_t value : sceneTimestamps) std::cout << value << ',';
    std::cout << " objects=15 lfe=SEPARATE_PCM metadata=B1-B2A-B2B"
              << " ownedVectors=" << (independent ? "PASS" : "FAIL")
              << " sceneAdapter=GAIN_FRAME_ONLY pcmRendererApplied=NO r2c=NO brir=NO"
              << " playback=NO productionAcceptance=INCONCLUSIVE\n";
    for (const auto &window : windows) {
        std::cout << "propertyWindow=" << window.name
                  << " updates=" << window.updates
                  << " listenerCentre=" << window.listenerCentre
                  << " nonzeroSize=" << window.nonzeroSize
                  << " nonzeroPriority=" << window.nonzeroPriority
                  << " restrictedZone=" << window.restrictedZone
                  << " snap=" << window.snap
                  << " screen=" << window.screen
                  << " distance=" << window.distance << '\n';
    }
    return timeline && metadata && independent;
}

} // namespace config4scene

#ifndef EAC3_CONFIG4_SCENE_NO_MAIN
int main(int argc, char **argv)
{
    if (argc == 2 && std::string(argv[1]) == "--self-test")
        return config4scene::selfTest() ? 0 : 1;
    unsigned maxAUs = 3U;
    unsigned jocConfig = 4U;
    const char *path = nullptr;
    if (argc == 2) path = argv[1];
    else if (argc == 4 && std::string(argv[1]) == "--max-aus") {
        try {
            const unsigned long long parsed = std::stoull(argv[2]);
            if (parsed == 0U || parsed > static_cast<unsigned long long>(
                    std::numeric_limits<unsigned>::max()))
                throw std::out_of_range("max-aus");
            maxAUs = static_cast<unsigned>(parsed);
            path = argv[3];
        } catch (...) {
            std::cerr << "j0a7=FAIL reason=j0a7-invalid-max-aus\n";
            return 2;
        }
    }
    else if (argc == 6 && std::string(argv[1]) == "--joc-config"
             && std::string(argv[3]) == "--max-aus") {
        try {
            jocConfig = static_cast<unsigned>(std::stoul(argv[2]));
            const unsigned long long parsed = std::stoull(argv[4]);
            if ((jocConfig != 3U && jocConfig != 4U) || parsed == 0U
                || parsed > static_cast<unsigned long long>(
                    std::numeric_limits<unsigned>::max()))
                throw std::out_of_range("joc-config-or-max-aus");
            maxAUs = static_cast<unsigned>(parsed);
            path = argv[5];
        } catch (...) {
            std::cerr << "j0a7=FAIL reason=j0a7-invalid-joc-config-or-max-aus\n";
            return 2;
        }
    }
    if (!path) {
        std::cerr << "Usage: Eac3NativeConfig4SceneProbe <raw.eac3|raw.eb3>\n"
                     "       Eac3NativeConfig4SceneProbe --max-aus N <raw.eac3|raw.eb3>\n"
                     "       Eac3NativeConfig4SceneProbe --joc-config 3 --max-aus N <raw.eac3>\n"
                     "       Eac3NativeConfig4SceneProbe --self-test\n";
        return 2;
    }
    return config4scene::runFile(path, maxAUs, jocConfig) ? 0 : 1;
}
#endif
