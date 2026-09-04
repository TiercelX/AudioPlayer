// R2C: diagnostic sample-accurate System H planar bus mixer for J0A7.
// LFE is retained as a separate sideband. The default route has no BRIR;
// explicit --i0p-* options connect only the offline BRIR/WAV evidence sink.
// No normalization, DRC, limiter, WASAPI, production playback, or FFmpeg path
// is involved.

#include "native-eac3-config4-scene.h"
#include "bs2127-system-h-cartesian-panner.h"
#include "i0p-offline-output.h"
#include "system-h-planar-mixer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iomanip>
#include <iostream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <stdexcept>
#include <thread>
#include <vector>
#include <utility>

namespace config4r2c {
namespace {
constexpr std::size_t kObjects = eac3render::kSystemHObjectCount;
constexpr std::size_t kSpeakers = eac3render::kSystemHSpeakerCount;
constexpr std::uint64_t kGeneration = eac3render::kInitialGeneration;

void hashByte(std::uint64_t *digest, std::uint8_t value)
{
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

struct Report { std::size_t cases = 0U; bool pass = true; std::string reason; };
void expect(Report *report, bool value, const char *reason)
{
    ++report->cases;
    if (!value && report->pass) {
        report->pass = false;
        report->reason = reason;
    }
}

bool checkedTotalSamples(std::uint64_t maxAUs, std::int64_t *total)
{
    if (!total || maxAUs == 0U
        || maxAUs > static_cast<std::uint64_t>(
               std::numeric_limits<std::int64_t>::max()) / 1536U
        || maxAUs > static_cast<std::uint64_t>(
               std::numeric_limits<unsigned>::max())
        || maxAUs == static_cast<std::uint64_t>(
               std::numeric_limits<unsigned>::max())) {
        return false;
    }
    *total = static_cast<std::int64_t>(maxAUs * 1536U);
    return true;
}

bool expectedTimeline(unsigned maxAUs, std::vector<std::size_t> *sizes,
                      std::vector<std::int64_t> *starts)
{
    std::int64_t total = 0;
    if (!sizes || !starts || !checkedTotalSamples(maxAUs, &total)) return false;
    sizes->assign(maxAUs + 1U, 1536U);
    starts->assign(maxAUs + 1U, 0);
    (*sizes)[0] = 959U;
    sizes->back() = 577U;
    for (unsigned index = 1U; index < maxAUs; ++index)
        (*starts)[index] = 959 + static_cast<std::int64_t>(index - 1U) * 1536;
    starts->back() = total - 577;
    return true;
}

eac3render::SceneAdapterFrame gainFrame(std::uint64_t generation,
                                        std::int64_t timestamp,
                                        double objectGain = 0.0)
{
    eac3render::SceneAdapterFrame frame;
    frame.generation = generation;
    frame.timestampSamples = timestamp;
    frame.durationSamples = 1;
    frame.lfeEnabled = true;
    frame.lfeGain = 0.0F;
    frame.objects.resize(kObjects);
    for (std::size_t i = 0U; i < kObjects; ++i) {
        frame.objects[i].objectId = i + 1U;
        frame.objects[i].gain = static_cast<float>(objectGain);
        frame.objects[i].speakerGains.fill(0.0);
    }
    return frame;
}

eac3render::SystemHPlanarMixerInput input(
    std::int64_t start, std::size_t count,
    const eac3render::SceneAdapterFrame &frame)
{
    eac3render::SystemHPlanarMixerInput value;
    value.generation = kGeneration;
    value.sampleStart = start;
    value.objectPcm.assign(kObjects, std::vector<float>(count, 0.0F));
    value.lfePcm.assign(count, 0.0F);
    value.gainFrame = frame;
    return value;
}

bool same(const std::vector<float> &value, std::initializer_list<float> expected)
{
    return value == std::vector<float>(expected);
}

eac3render::SystemHPlanarMixerInput fromPrepared(
    const config4scene::PreparedBatch &batch,
    const std::vector<double> *evaluatedGains = nullptr)
{
    eac3render::SystemHPlanarMixerInput value;
    value.generation = batch.objects.front().generation;
    value.sampleStart = batch.outputStart;
    value.gainFrame = batch.sceneFrame;
    value.objectPcm.reserve(batch.objects.size());
    for (const auto &object : batch.objects)
        value.objectPcm.push_back(object.samples);
    value.lfePcm = batch.lfe.samples;
    if (evaluatedGains) value.evaluatedObjectSpeakerGains = *evaluatedGains;
    return value;
}

struct CartesianPoint {
    std::int64_t timestamp = 0;
    std::int64_t duration = 0;
    float startGain = 0.0F;
    float targetGain = 0.0F;
    std::array<float, 3> startRoom {0.5F, 0.5F, 0.0F};
    std::array<float, 3> targetRoom {0.5F, 0.5F, 0.0F};
    bool jumpPosition = false;
};

class CartesianSceneEvaluator final {
public:
    bool append(const config4scene::PreparedBatch &batch, std::string *reason)
    {
        if (!reason || batch.flush || batch.objects.size() != kObjects
            || batch.sceneFrame.objects.size() != kObjects) {
            if (reason) *reason = "r2c-cartesian-point-batch-shape";
            return false;
        }
        if (generation_ && batch.sceneFrame.generation != *generation_) {
            *reason = "r2c-cartesian-generation-changed";
            return false;
        }
        const auto &policy = batch.coordinatePolicy;
        if (policy_ && (policy.roomWidthMetres != policy_->roomWidthMetres
                        || policy.roomDepthMetres != policy_->roomDepthMetres
                        || policy.roomHeightMetres != policy_->roomHeightMetres
                        || policy.listenerX != policy_->listenerX
                        || policy.listenerY != policy_->listenerY
                        || policy.listenerZ != policy_->listenerZ)) {
            *reason = "r2c-cartesian-policy-changed";
            return false;
        }
        for (std::size_t object = 0U; object < kObjects; ++object) {
            const auto &value = batch.objects[object];
            if (value.generation != batch.sceneFrame.generation
                || value.objectId != object + 1U
                || batch.sceneFrame.objects[object].objectId != value.objectId
                || value.timestampSamples != batch.sceneFrame.timestampSamples
                || value.sceneUpdate.objectId != value.objectId
                || value.sceneUpdate.timestampSamples != value.timestampSamples
                || value.timestampSamples < 0
                || value.durationSamples < 0
                || value.durationSamples > std::numeric_limits<std::int64_t>::max()
                    - value.timestampSamples
                || !std::isfinite(value.sceneUpdate.gain)) {
                *reason = "r2c-cartesian-point-identity";
                return false;
            }
            if (!points_[object].empty()
                && value.timestampSamples <= points_[object].back().timestamp) {
                *reason = "r2c-cartesian-point-order";
                return false;
            }
        }
        if (!generation_) generation_ = batch.sceneFrame.generation;
        if (!policy_) policy_ = policy;
        for (std::size_t object = 0U; object < kObjects; ++object) {
            const auto &value = batch.objects[object];
            CartesianPoint point;
            point.timestamp = value.timestampSamples;
            point.duration = value.durationSamples;
            point.targetGain = value.sceneUpdate.gain;
            point.targetRoom = value.roomPosition;
            point.jumpPosition = value.sceneUpdate.jumpPosition;
            if (!points_[object].empty()) {
                const CartesianPoint &previous = points_[object].back();
                if (point.timestamp <= previous.timestamp) {
                    *reason = "r2c-cartesian-point-order";
                    return false;
                }
                const auto state = stateAt(previous, point.timestamp);
                point.startGain = state.first;
                point.startRoom = state.second;
            } else {
                point.startGain = point.targetGain;
                point.startRoom = point.targetRoom;
            }
            points_[object].push_back(point);
        }
        return true;
    }

    bool evaluate(std::int64_t timestamp, std::vector<double> *gains,
                  bool *interpolated, std::string *reason,
                  std::vector<std::array<double, 3>> *positions = nullptr) const
    {
        if (!gains || !interpolated || !reason || !policy_) {
            if (reason) *reason = "r2c-cartesian-evaluator-uninitialized";
            return false;
        }
        gains->clear();
        *interpolated = false;
        if (positions) positions->clear();
        gains->reserve(kObjects * kSpeakers);
        eac3render::Bs2127SystemHCartesianPanner panner;
        for (std::size_t object = 0U; object < kObjects; ++object) {
            const auto &timeline = points_[object];
            if (timeline.empty()) {
                *reason = "r2c-cartesian-point-missing";
                return false;
            }
            if (timestamp < timeline.front().timestamp) {
                *reason = "r2c-cartesian-query-before-first-point";
                return false;
            }
            auto right = std::upper_bound(
                timeline.begin(), timeline.end(), timestamp,
                [](std::int64_t value, const CartesianPoint &point) {
                    return value < point.timestamp;
                });
            const CartesianPoint &left = right == timeline.begin()
                ? timeline.front() : *std::prev(right);
            const auto state = stateAt(left, timestamp);
            *interpolated = *interpolated || (left.duration > 0
                && timestamp > left.timestamp
                && timestamp < left.timestamp + left.duration);
            const float gain = state.first;
            const auto &room = state.second;
            std::array<double, 3> point;
            if (!eac3render::Bs2127SystemHCartesianPanner::mapEtsiRoomToAllocentric(
                    room, &point, reason)) return false;
            const auto panned = panner.render(point);
            if (!panned.accepted) {
                *reason = (panned.reason.empty() ? "r2c-cartesian-pan-failed"
                                                  : panned.reason)
                    + " object=" + std::to_string(object + 1U)
                    + " point=" + std::to_string(point[0]) + ","
                    + std::to_string(point[1]) + "," + std::to_string(point[2]);
                return false;
            }
            if (!std::isfinite(gain) || gain < 0.0F) {
                *reason = "r2c-cartesian-gain-invalid";
                return false;
            }
            if (positions) positions->push_back(point);
            for (double gain : panned.gains) gains->push_back(
                static_cast<double>(state.first) * gain);
        }
        return gains->size() == kObjects * kSpeakers;
    }

private:
    static std::pair<float, std::array<float, 3>> stateAt(
        const CartesianPoint &point, std::int64_t timestamp)
    {
        const bool endOverflow = point.duration > 0
            && point.duration > std::numeric_limits<std::int64_t>::max()
                - point.timestamp;
        const std::int64_t end = endOverflow
            ? std::numeric_limits<std::int64_t>::max()
            : point.timestamp + point.duration;
        const double fraction = point.duration <= 0 || timestamp >= end
            ? 1.0 : timestamp <= point.timestamp ? 0.0
                : static_cast<double>(timestamp - point.timestamp)
                    / static_cast<double>(point.duration);
        const float gain = static_cast<float>(
            static_cast<double>(point.startGain)
            + (static_cast<double>(point.targetGain)
               - static_cast<double>(point.startGain)) * fraction);
        std::array<float, 3> room = point.targetRoom;
        if (!point.jumpPosition) {
            for (std::size_t axis = 0U; axis < 3U; ++axis)
                room[axis] = static_cast<float>(
                    static_cast<double>(point.startRoom[axis])
                    + (static_cast<double>(point.targetRoom[axis])
                       - static_cast<double>(point.startRoom[axis])) * fraction);
        }
        return {gain, room};
    }

    std::array<std::vector<CartesianPoint>, kObjects> points_;
    std::optional<std::uint64_t> generation_;
    std::optional<eac3render::SceneObjectCoordinatePolicy> policy_;
};

bool evaluatePreparedBatch(const config4scene::PreparedBatch &batch,
                           const CartesianSceneEvaluator &evaluator,
                           std::vector<double> *gains,
                           bool *interpolated,
                           std::string *reason,
                           unsigned jobs = 1U)
{
    if (!gains || !interpolated || !reason || jobs == 0U || batch.outputStart < 0
        || batch.outputEnd <= batch.outputStart) {
        if (reason) *reason = "r2c-evaluated-gain-range";
        return false;
    }
    gains->clear();
    *interpolated = false;
    const std::size_t samples = static_cast<std::size_t>(
        batch.outputEnd - batch.outputStart);
    std::size_t gainCount = 0U;
    if (!eac3render::checkedSystemHEvaluatedGainCount(samples, &gainCount)) {
        *reason = "r2c-evaluated-gain-size-overflow";
        return false;
    }
    gains->resize(gainCount);
    std::atomic<std::size_t> nextSample {0U};
    std::atomic<bool> failed {false};
    std::atomic<bool> anyInterpolated {false};
    std::string workerReason;
    const auto worker = [&]() {
        try {
            for (;;) {
                const std::size_t sample = nextSample.fetch_add(1U);
                if (sample >= samples || failed.load()) return;
                const std::int64_t timestamp = batch.outputStart
                    + static_cast<std::int64_t>(sample);
                std::vector<double> sampleGains;
                bool sampleInterpolated = false;
                std::string localReason;
                if (!evaluator.evaluate(timestamp, &sampleGains,
                                        &sampleInterpolated, &localReason)
                    || sampleGains.size() != kObjects * kSpeakers) {
                    if (!failed.exchange(true))
                        workerReason = localReason + "@sample="
                            + std::to_string(timestamp);
                    return;
                }
                std::copy(sampleGains.begin(), sampleGains.end(),
                          gains->begin() + sample * kObjects * kSpeakers);
                if (sampleInterpolated) anyInterpolated.store(true);
            }
        } catch (...) {
            if (!failed.exchange(true))
                workerReason = "r2c-evaluated-gain-worker-failed";
        }
    };
    const unsigned workerCount = std::min<unsigned>(
        jobs, static_cast<unsigned>(samples));
    std::vector<std::thread> threads;
    threads.reserve(workerCount > 0U ? workerCount - 1U : 0U);
    try {
        for (unsigned index = 1U; index < workerCount; ++index)
            threads.emplace_back(worker);
    } catch (...) {
        failed.store(true);
        for (auto &thread : threads) thread.join();
        *reason = "r2c-evaluated-gain-worker-start-failed";
        return false;
    }
    worker();
    for (auto &thread : threads) thread.join();
    if (failed.load()) {
        *reason = workerReason;
        return false;
    }
    *interpolated = anyInterpolated.load();
    return gains->size() == gainCount;
}

class Owner final {
public:
    bool open() { opened_ = true; return mixer_.reset(generation_).accepted; }
    bool push(const config4scene::PreparedBatch &prepared,
              const std::function<bool(const eac3render::SystemHPlanarMixerBatch &)> &callback,
              std::string *reason,
              const std::vector<double> *evaluatedGains = nullptr)
    {
        if (!opened_ || canceled_ || poisoned_ || !callback) {
            if (reason) *reason = canceled_ ? "r2c-canceled"
                : (poisoned_ ? "r2c-poisoned-reset-required" : "r2c-owner-state");
            return false;
        }
        const auto result = mixer_.process(fromPrepared(prepared, evaluatedGains));
        if (!result.accepted) {
            poisoned_ = true;
            if (reason) *reason = result.reason;
            return false;
        }
        if (!callback(result.batch)) {
            poisoned_ = true;
            if (reason) *reason = "r2c-callback-rejected";
            return false;
        }
        return true;
    }
    void cancel() { canceled_ = true; }
    void reset()
    {
        ++generation_;
        mixer_.reset(generation_);
        opened_ = false;
        canceled_ = false;
        poisoned_ = false;
    }
private:
    eac3render::SystemHPlanarMixer mixer_;
    std::uint64_t generation_ = kGeneration;
    bool opened_ = false;
    bool canceled_ = false;
    bool poisoned_ = false;
};

bool selfTest()
{
    Report report;
    eac3render::Bs2127SystemHCartesianPanner cartesian;
    const auto center = cartesian.render({0.0, 0.0, 0.0});
    double centerPower = 0.0;
    for (double gain : center.gains) centerPower += gain * gain;
    const auto &speakers = eac3render::Bs2127SystemHCartesianPanner::speakerCoordinates();
    const std::array<std::array<double, 3>, kSpeakers> expectedSpeakers {{
        {{-1.0, 0.414214, 0.0}}, {{1.0, 0.414214, 0.0}}, {{0.0, 1.0, 0.0}},
        {{-1.0, -1.0, 0.0}}, {{1.0, -1.0, 0.0}}, {{-1.0, 1.0, 0.0}},
        {{1.0, 1.0, 0.0}}, {{0.0, -1.0, 0.0}}, {{-1.0, 0.0, 0.0}},
        {{1.0, 0.0, 0.0}}, {{-1.0, 1.0, 1.0}}, {{1.0, 1.0, 1.0}},
        {{0.0, 1.0, 1.0}}, {{0.0, 0.0, 1.0}}, {{-1.0, -1.0, 1.0}},
        {{1.0, -1.0, 1.0}}, {{-1.0, 0.0, 1.0}}, {{1.0, 0.0, 1.0}},
        {{0.0, -1.0, 1.0}}, {{0.0, 1.0, -1.0}}, {{-1.0, 1.0, -1.0}},
        {{1.0, 1.0, -1.0}},
    }};
    std::size_t centerNonZero = 0U;
    for (double gain : center.gains) centerNonZero += gain > 0.0 ? 1U : 0U;
    expect(&report, center.accepted && std::abs(centerPower - 1.0) < 1.0e-9
                        && centerNonZero == 2U
                        && std::abs(center.gains[8] - std::sqrt(0.5)) < 1.0e-9
                        && std::abs(center.gains[9] - std::sqrt(0.5)) < 1.0e-9
                        && std::all_of(center.gains.begin(), center.gains.end(),
                            [](double gain) {
                                return std::abs(gain) < 1.0e-12
                                    || std::abs(gain - std::sqrt(0.5)) < 1.0e-9;
                            }),
           "cartesian-center-unit-power");
    expect(&report, speakers == expectedSpeakers, "system-h-cartesian-speaker-order");
    const auto nearRay = cartesian.render({0.1, 0.1, 0.0});
    const auto farRay = cartesian.render({0.4, 0.4, 0.0});
    const auto nearAxis = cartesian.render({0.1, 0.0, 0.0});
    const auto farAxis = cartesian.render({0.4, 0.0, 0.0});
    bool radiusChanged = nearRay.gains != farRay.gains
        || nearAxis.gains != farAxis.gains;
    const std::array<std::array<double, 3>, 4> directions {{
        {{1.0, 2.0, 0.5}}, {{2.0, -1.0, 1.0}},
        {{1.0, 0.5, -2.0}}, {{0.5, -2.0, -1.0}},
    }};
    for (const auto &direction : directions) {
        std::array<double, 3> first;
        std::array<double, 3> second;
        for (std::size_t axis = 0U; axis < 3U; ++axis) {
            first[axis] = direction[axis] * 0.1;
            second[axis] = direction[axis] * 0.4;
        }
        const auto firstResult = cartesian.render(first);
        const auto secondResult = cartesian.render(second);
        radiusChanged = radiusChanged || (firstResult.accepted && secondResult.accepted
                                          && firstResult.gains != secondResult.gains);
    }
    expect(&report, radiusChanged,
           "cartesian-radius-preserved");
    const auto activeSpeakers = [](const eac3render::Bs2127CartesianPannerResult &value) {
        std::vector<std::size_t> active;
        for (std::size_t index = 0U; index < value.gains.size(); ++index)
            if (value.gains[index] > 1.0e-12) active.push_back(index);
        return active;
    };
    expect(&report,
           activeSpeakers(cartesian.render({1.0, 0.0, 0.0}))
                   == std::vector<std::size_t>({9U})
               && activeSpeakers(cartesian.render({-1.0, 0.0, 0.0}))
                   == std::vector<std::size_t>({8U})
               && activeSpeakers(cartesian.render({0.0, 1.0, 0.0}))
                   == std::vector<std::size_t>({2U})
               && activeSpeakers(cartesian.render({0.0, -1.0, 0.0}))
                   == std::vector<std::size_t>({7U})
               && activeSpeakers(cartesian.render({0.0, 0.0, 1.0}))
                   == std::vector<std::size_t>({13U})
               && activeSpeakers(cartesian.render({0.0, 0.0, -1.0}))
                   == std::vector<std::size_t>({19U}),
           "cartesian-cardinal-speaker-identities");
    // Independent BS.2127-1 section 7.3.10 separable dual-balance oracle for
    // p=(0.2,-0.3,0.4). These constants were derived from the published x/y/z
    // cosine equations and fixed System H coordinates, not by calling the
    // panner (or any of its selection helpers) to form the expectation.
    const auto interior = cartesian.render({0.2, -0.3, 0.4});
    const std::array<std::size_t, 8> interiorIndices {
        4U, 7U, 8U, 9U, 13U, 15U, 17U, 18U};
    const std::array<double, 8> interiorExpected {
        0.11349762493488673, 0.34930977177059236,
        0.42369878044542130, 0.58317134113076310,
        0.49808778912025030, 0.08246085134279688,
        0.16183853313827165, 0.25378840463918180};
    bool interiorOracle = interior.accepted
        && activeSpeakers(interior)
            == std::vector<std::size_t>(interiorIndices.begin(),
                                         interiorIndices.end());
    for (std::size_t index = 0U; interiorOracle && index < interiorIndices.size();
         ++index) {
        interiorOracle = std::abs(interior.gains[interiorIndices[index]]
                                  - interiorExpected[index]) < 1.0e-12;
    }
    expect(&report, interiorOracle, "cartesian-interior-bs2127-oracle");
    const auto rampStart = cartesian.render({-0.6, 0.2, 0.1});
    const auto rampMid = cartesian.render({0.0, 0.2, 0.1});
    const auto rampEnd = cartesian.render({0.6, 0.2, 0.1});
    expect(&report, rampStart.accepted && rampMid.accepted && rampEnd.accepted,
           "cartesian-continuous-ramp");
    expect(&report, !cartesian.render({NAN, 0.0, 0.0}).accepted,
           "cartesian-nonfinite-rejected");
    std::array<double, 3> mapped;
    std::string mapReason;
    expect(&report,
           eac3render::Bs2127SystemHCartesianPanner::mapEtsiRoomToAllocentric(
               {0.5F, 0.5F, 0.0F}, &mapped, &mapReason)
               && mapped == std::array<double, 3>({0.0, 0.0, 0.0}),
           "etsi-center-mapping");
    expect(&report,
           !eac3render::Bs2127SystemHCartesianPanner::mapEtsiRoomToAllocentric(
               {NAN, 0.5F, 0.0F}, &mapped, &mapReason),
           "etsi-nonfinite-rejected");
    const auto makeCartesianBatch = [](std::int64_t timestamp,
                                       std::int64_t duration,
                                       std::array<float, 3> room,
                                       float gain, bool jump) {
        config4scene::PreparedBatch batch;
        batch.coordinatePolicy = {10.0, 10.0, 7.0, 0.5, 0.5, 0.0};
        batch.sceneFrame.generation = kGeneration;
        batch.sceneFrame.timestampSamples = timestamp;
        batch.sceneFrame.durationSamples = duration;
        batch.sceneFrame.objects.resize(kObjects);
        batch.objects.resize(kObjects);
        for (std::size_t object = 0U; object < kObjects; ++object) {
            auto &prepared = batch.objects[object];
            prepared.generation = kGeneration;
            prepared.objectId = object + 1U;
            prepared.timestampSamples = timestamp;
            prepared.durationSamples = duration;
            prepared.roomPosition = room;
            prepared.sceneUpdate.objectId = object + 1U;
            prepared.sceneUpdate.timestampSamples = timestamp;
            prepared.sceneUpdate.gain = gain;
            prepared.sceneUpdate.jumpPosition = jump;
            batch.sceneFrame.objects[object].objectId = object + 1U;
        }
        return batch;
    };
    CartesianSceneEvaluator gapEvaluator;
    auto gapStart = makeCartesianBatch(0, 2, {0.5F, 0.5F, 0.0F}, 1.0F, false);
    auto gapTarget = makeCartesianBatch(10, 4, {0.6F, 0.5F, 0.0F}, 0.5F, false);
    std::string evaluatorReason;
    std::vector<double> evaluatorGains;
    bool evaluatorInterpolated = false;
    const auto expectedGain = [&cartesian](std::array<float, 3> room,
                                           float gain, std::size_t speaker) {
        std::array<double, 3> point;
        std::string reason;
        if (!eac3render::Bs2127SystemHCartesianPanner::mapEtsiRoomToAllocentric(
                room, &point, &reason)) return -1.0;
        const auto result = cartesian.render(point);
        return result.accepted ? static_cast<double>(gain) * result.gains[speaker]
                               : -1.0;
    };
    std::vector<double> gapAt3;
    const bool gapLoaded = gapEvaluator.append(gapStart, &evaluatorReason)
        && gapEvaluator.append(gapTarget, &evaluatorReason)
        && gapEvaluator.evaluate(3, &gapAt3, &evaluatorInterpolated,
                                 &evaluatorReason)
        && gapEvaluator.evaluate(12, &evaluatorGains, &evaluatorInterpolated,
                                 &evaluatorReason);
    expect(&report, gapLoaded && std::abs(gapAt3[8] - expectedGain(
                        {0.5F, 0.5F, 0.0F}, 1.0F, 8U)) < 1.0e-9
                        && std::abs(evaluatorGains[8] - expectedGain(
                        {0.55F, 0.5F, 0.0F}, 0.75F, 8U)) < 1.0e-6,
           "cartesian-gap-duration-clamp");
    gapStart.outputStart = 0;
    gapStart.outputEnd = 16;
    std::vector<double> serialEvaluated;
    std::vector<double> parallelEvaluated;
    bool serialInterpolated = false;
    bool parallelInterpolated = false;
    const bool parallelEquivalent = evaluatePreparedBatch(
            gapStart, gapEvaluator, &serialEvaluated, &serialInterpolated,
            &evaluatorReason, 1U)
        && evaluatePreparedBatch(gapStart, gapEvaluator, &parallelEvaluated,
                                 &parallelInterpolated, &evaluatorReason, 8U)
        && serialEvaluated == parallelEvaluated
        && serialInterpolated == parallelInterpolated;
    expect(&report, parallelEquivalent,
           "cartesian-parallel-evaluation-byte-equivalent");
    CartesianSceneEvaluator overlapEvaluator;
    auto overlapStart = makeCartesianBatch(0, 0, {0.5F, 0.5F, 0.0F}, 1.0F, false);
    auto overlapTarget = makeCartesianBatch(1, 10, {0.6F, 0.5F, 0.0F}, 0.8F, false);
    auto overlapNext = makeCartesianBatch(4, 4, {0.4F, 0.5F, 0.0F}, 0.2F, false);
    std::vector<std::array<double, 3>> overlapPositions;
    std::vector<double> overlapAt0;
    std::vector<double> overlapAt1;
    std::vector<double> overlapAt4;
    const bool overlapLoaded = overlapEvaluator.append(overlapStart, &evaluatorReason)
        && overlapEvaluator.append(overlapTarget, &evaluatorReason)
        && overlapEvaluator.append(overlapNext, &evaluatorReason)
        && overlapEvaluator.evaluate(0, &overlapAt0, &evaluatorInterpolated,
                                     &evaluatorReason)
        && overlapEvaluator.evaluate(1, &overlapAt1, &evaluatorInterpolated,
                                     &evaluatorReason)
        && overlapEvaluator.evaluate(4, &overlapAt4, &evaluatorInterpolated,
                                     &evaluatorReason, &overlapPositions);
    expect(&report, overlapLoaded && overlapPositions.size() == kObjects
                        && std::abs(overlapPositions.front()[0] - 0.06) < 1.0e-6
                        && std::abs(overlapAt0[8] - expectedGain(
                            {0.5F, 0.5F, 0.0F}, 1.0F, 8U)) < 1.0e-9
                        && std::abs(overlapAt1[8] - expectedGain(
                            {0.5F, 0.5F, 0.0F}, 1.0F, 8U)) < 1.0e-9
                        && std::abs(overlapAt4[8] - expectedGain(
                            {0.53F, 0.5F, 0.0F}, 0.94F, 8U)) < 1.0e-6,
           "cartesian-overlap-start-state");
    CartesianSceneEvaluator stepEvaluator;
    auto stepStart = makeCartesianBatch(0, 0, {0.5F, 0.5F, 0.0F}, 1.0F, false);
    auto stepTarget = makeCartesianBatch(1, 0, {0.6F, 0.5F, 0.0F}, 0.5F, false);
    std::vector<std::array<double, 3>> stepPositions;
    const bool stepLoaded = stepEvaluator.append(stepStart, &evaluatorReason)
        && stepEvaluator.append(stepTarget, &evaluatorReason)
        && stepEvaluator.evaluate(1, &evaluatorGains, &evaluatorInterpolated,
                                  &evaluatorReason, &stepPositions);
    expect(&report, stepLoaded && stepPositions.size() == kObjects
                        && std::abs(stepPositions.front()[0] - 0.2) < 1.0e-6
                        && std::abs(evaluatorGains[8] - expectedGain(
                            {0.6F, 0.5F, 0.0F}, 0.5F, 8U)) < 1.0e-6,
           "cartesian-zero-duration-step");
    CartesianSceneEvaluator jumpEvaluator;
    auto jumpStart = makeCartesianBatch(0, 0, {0.5F, 0.5F, 0.0F}, 1.0F, false);
    auto jumpTarget = makeCartesianBatch(4, 4, {0.6F, 0.5F, 0.0F}, 0.5F, true);
    std::vector<std::array<double, 3>> jumpPositions;
    std::vector<double> jumpAtS;
    const bool jumpLoaded = jumpEvaluator.append(jumpStart, &evaluatorReason)
        && jumpEvaluator.append(jumpTarget, &evaluatorReason)
        && jumpEvaluator.evaluate(4, &jumpAtS, &evaluatorInterpolated,
                                  &evaluatorReason, &jumpPositions);
    std::vector<std::array<double, 3>> jumpMidPositions;
    std::vector<double> jumpMid;
    const bool jumpMidLoaded = jumpEvaluator.evaluate(
        6, &jumpMid, &evaluatorInterpolated, &evaluatorReason, &jumpMidPositions);
    expect(&report, jumpLoaded && jumpPositions.size() == kObjects
                        && jumpMidLoaded && jumpMidPositions.size() == kObjects
                        && std::abs(jumpPositions.front()[0] - 0.2) < 1.0e-6
                        && std::abs(jumpAtS[8] - expectedGain(
                            {0.6F, 0.5F, 0.0F}, 1.0F, 8U)) < 1.0e-6
                        && std::abs(jumpMid[8] - expectedGain(
                            {0.6F, 0.5F, 0.0F}, 0.75F, 8U)) < 1.0e-6,
           "cartesian-jump-position-only");
    CartesianSceneEvaluator generationEvaluator;
    auto generationStart = makeCartesianBatch(0, 0, {0.5F, 0.5F, 0.0F}, 1.0F, false);
    auto generationNext = makeCartesianBatch(1, 0, {0.5F, 0.5F, 0.0F}, 1.0F, false);
    auto mixedGeneration = generationNext;
    mixedGeneration.objects[3].generation = kGeneration + 1U;
    expect(&report, generationEvaluator.append(generationStart, &evaluatorReason)
                        && !generationEvaluator.append(mixedGeneration, &evaluatorReason),
           "cartesian-mixed-generation-rejected");
    auto staleGeneration = generationNext;
    staleGeneration.sceneFrame.generation = kGeneration + 1U;
    for (auto &object : staleGeneration.objects)
        object.generation = kGeneration + 1U;
    expect(&report, !generationEvaluator.append(staleGeneration, &evaluatorReason),
           "cartesian-stale-generation-rejected");
    CartesianSceneEvaluator sameTimestampEvaluator;
    expect(&report, sameTimestampEvaluator.append(generationStart, &evaluatorReason)
                        && !sameTimestampEvaluator.append(generationStart, &evaluatorReason),
           "cartesian-same-timestamp-rejected");
    CartesianSceneEvaluator reverseTimestampEvaluator;
    expect(&report, reverseTimestampEvaluator.append(generationNext, &evaluatorReason)
                        && !reverseTimestampEvaluator.append(generationStart, &evaluatorReason),
           "cartesian-reverse-timestamp-rejected");
    CartesianSceneEvaluator beforeFirstEvaluator;
    auto futureStart = makeCartesianBatch(10, 0, {0.5F, 0.5F, 0.0F}, 1.0F, false);
    std::vector<double> beforeFirstGains;
    expect(&report, beforeFirstEvaluator.append(futureStart, &evaluatorReason)
                        && !beforeFirstEvaluator.evaluate(
                               9, &beforeFirstGains, &evaluatorInterpolated,
                               &evaluatorReason),
           "cartesian-before-first-query-rejected");
    CartesianSceneEvaluator overflowEvaluator;
    auto overflowBatch = makeCartesianBatch(
        std::numeric_limits<std::int64_t>::max(), 1,
        {0.5F, 0.5F, 0.0F}, 1.0F, false);
    expect(&report, !overflowEvaluator.append(overflowBatch, &evaluatorReason),
           "cartesian-timestamp-duration-overflow-rejected");
    auto frame = gainFrame(kGeneration, 0);
    frame.objects[0].speakerGains[0] = 2.0;
    frame.objects[0].speakerGains[1] = 3.0;
    frame.objects[1].speakerGains[0] = 5.0;
    auto oracle = input(0, 2, frame);
    oracle.objectPcm[0] = {1.0F, 2.0F};
    oracle.objectPcm[1] = {4.0F, 5.0F};
    eac3render::SystemHPlanarMixer mixer;
    auto result = mixer.process(oracle);
    expect(&report, result.accepted && same(result.batch.speakers[0], {22.0F, 29.0F})
                        && same(result.batch.speakers[1], {3.0F, 6.0F}),
           "single-and-two-object-sum-oracle");
    expect(&report, result.accepted && result.batch.speakers[2][0] == 0.0F
                        && result.batch.lfeSideband == oracle.lfePcm,
           "silence-and-separate-lfe");
    auto object11Oracle = input(0, 2, gainFrame(kGeneration, 0));
    object11Oracle.objectPcm[10] = {3.0F, 4.0F};
    object11Oracle.gainFrame.objects[10].speakerGains[0] = 2.0;
    eac3render::SystemHPlanarMixer object11Mixer;
    const auto object11Result = object11Mixer.process(object11Oracle);
    expect(&report, object11Result.accepted
                        && same(object11Result.batch.speakers[0], {6.0F, 8.0F}),
           "object11-single-contribution-oracle");
    auto nonfinite = input(2, 1, gainFrame(kGeneration, 2));
    nonfinite.objectPcm[0][0] = NAN;
    expect(&report, !mixer.process(nonfinite).accepted,
           "nonfinite-pcm-rejected");
    eac3render::SystemHPlanarMixer malformed;
    auto bad = input(0, 1, gainFrame(kGeneration, 0));
    bad.objectPcm.pop_back();
    expect(&report, !malformed.process(bad).accepted,
           "missing-object-rejected");
    auto wrongOrder = input(0, 1, gainFrame(kGeneration, 0));
    std::swap(wrongOrder.gainFrame.objects[0], wrongOrder.gainFrame.objects[1]);
    expect(&report, !malformed.process(wrongOrder).accepted,
           "gain-object-order-rejected");
    eac3render::SystemHPlanarMixer shiftedIdentity;
    auto shifted = input(0, 1, gainFrame(kGeneration, 0));
    for (auto &object : shifted.gainFrame.objects) ++object.objectId;
    expect(&report, !shiftedIdentity.process(shifted).accepted,
           "gain-object-identity-rejected");
    eac3render::SystemHPlanarMixer bedWithoutPcm;
    auto nonzeroBed = input(0, 1, gainFrame(kGeneration, 0));
    nonzeroBed.gainFrame.bedGains[0] = 1.0;
    expect(&report, !bedWithoutPcm.process(nonzeroBed).accepted,
           "bed-gain-without-bed-pcm-rejected");
    eac3render::SystemHPlanarMixer lfeMismatch;
    auto missingLfeMarker = input(0, 1, gainFrame(kGeneration, 0));
    missingLfeMarker.gainFrame.lfeEnabled = false;
    expect(&report, !lfeMismatch.process(missingLfeMarker).accepted,
           "lfe-sideband-policy-mismatch-rejected");
    eac3render::SystemHPlanarMixer boundary;
    auto initial = gainFrame(kGeneration, 0);
    initial.objects[0].speakerGains[0] = 2.0;
    auto first = input(0, 6, initial);
    first.objectPcm[0].assign(6, 1.0F);
    const auto firstResult = boundary.process(first);
    auto future = gainFrame(kGeneration, 8);
    future.objects[0].speakerGains[0] = 4.0;
    auto second = input(6, 4, future);
    second.objectPcm[0].assign(4, 1.0F);
    const auto secondResult = boundary.process(second);
    expect(&report, firstResult.accepted && secondResult.accepted
                        && same(secondResult.batch.speakers[0], {2.0F, 2.0F, 4.0F, 4.0F}),
           "future-gain-starts-at-sample-time");
    auto stale = input(10, 1, future);
    stale.generation = 2U;
    expect(&report, !boundary.process(stale).accepted,
           "stale-generation-rejected");
    eac3render::SystemHPlanarMixer evaluatedMixer;
    auto evaluated = input(0, 4, initial);
    evaluated.objectPcm[0].assign(4, 1.0F);
    evaluated.evaluatedObjectSpeakerGains.assign(
        4U * kObjects * kSpeakers, 0.0);
    for (std::size_t sample = 0U; sample < 4U; ++sample)
        evaluated.evaluatedObjectSpeakerGains[
            (sample * kObjects) * kSpeakers] = sample < 2U ? 2.0 : 4.0;
    const auto evaluatedResult = evaluatedMixer.process(evaluated);
    expect(&report, evaluatedResult.accepted
                        && same(evaluatedResult.batch.speakers[0],
                                {2.0F, 2.0F, 4.0F, 4.0F}),
           "evaluated-per-sample-gain-oracle");
    Owner rejected;
    config4scene::PreparedBatch prepared;
    prepared.outputEnd = 2;
    prepared.objects.resize(kObjects);
    prepared.lfe.samples.assign(2, 0.0F);
    prepared.sceneFrame = gainFrame(kGeneration, 0);
    for (auto &object : prepared.objects) {
        object.generation = kGeneration;
        object.samples.assign(2, 0.0F);
    }
    const auto rejectCallback = [](const eac3render::SystemHPlanarMixerBatch &) { return false; };
    std::string reason;
    const bool poison = rejected.open()
        && !rejected.push(prepared, rejectCallback, &reason)
        && !rejected.push(prepared, [](const auto &) { return true; }, &reason);
    expect(&report, poison, "callback-poison");
    rejected.reset();
    auto resetPrepared = prepared;
    resetPrepared.sceneFrame = gainFrame(2U, 0);
    for (auto &object : resetPrepared.objects) object.generation = 2U;
    expect(&report, rejected.open()
                        && rejected.push(resetPrepared,
                                         [](const auto &) { return true; }, &reason),
           "reset-reopens-clean");
    Owner canceled;
    const bool cancelOpen = canceled.open();
    canceled.cancel();
    expect(&report, cancelOpen
                        && !canceled.push(prepared, [](const auto &) { return true; }, &reason),
           "cancel-closes-owner");
    std::int64_t totalSamples = 0;
    expect(&report, !checkedTotalSamples(0U, &totalSamples), "max-aus-zero-rejected");
    expect(&report, !checkedTotalSamples(std::numeric_limits<std::uint64_t>::max(),
                                         &totalSamples), "max-aus-overflow-rejected");
    std::size_t evaluatedGainCount = 0U;
    expect(&report, eac3render::checkedSystemHEvaluatedGainCount(
                               4U, &evaluatedGainCount)
                        && evaluatedGainCount == 4U * kObjects * kSpeakers
                        && !eac3render::checkedSystemHEvaluatedGainCount(
                               std::numeric_limits<std::size_t>::max(),
                               &evaluatedGainCount),
           "evaluated-gain-count-overflow-rejected");
    std::vector<std::size_t> timelineSizes;
    std::vector<std::int64_t> timelineStarts;
    expect(&report, expectedTimeline(1U, &timelineSizes, &timelineStarts)
                        && timelineSizes == std::vector<std::size_t>({959U, 577U})
                        && timelineStarts == std::vector<std::int64_t>({0, 959}),
           "one-au-final-tail-timeline");
    expect(&report, expectedTimeline(4U, &timelineSizes, &timelineStarts)
                        && timelineSizes == std::vector<std::size_t>(
                               {959U, 1536U, 1536U, 1536U, 577U})
                        && timelineStarts == std::vector<std::int64_t>(
                               {0, 959, 2495, 4031, 5567}),
           "four-au-final-tail-timeline");
    std::string i0pReason;
    expect(&report, eac3render::i0pOfflineSelfTest(&i0pReason),
           "i0p-offline-output-contract");
    std::cout << "r2cSelfTest=" << (report.pass ? "PASS" : "FAIL")
              << " cases=" << report.cases
              << " directOracle=" << (report.pass ? "PASS" : "FAIL")
              << " futureGainBoundary=PASS lfe=SEPARATE_SIDE_BAND"
              << " callbackPoison=PASS reset=PASS cancel=PASS reason="
              << report.reason << '\n';
    return report.pass;
}

} // namespace

bool runFile(const std::string &path, unsigned maxAUs,
             const std::string &i0pCache = {},
             const eac3render::I0pOfflinePaths &i0pPaths = {},
             unsigned jocConfig = 4U,
             unsigned jobs = 1U)
{
    using Clock = std::chrono::steady_clock;
    const auto runStarted = Clock::now();
    std::int64_t decodedSourceSamples = 0;
    if (!checkedTotalSamples(maxAUs, &decodedSourceSamples)) {
        std::cerr << "r2c=FAIL reason=r2c-invalid-max-aus\n";
        return false;
    }
    Owner owner;
    std::vector<std::int64_t> gainDurations;
    std::vector<eac3render::SceneAdapterFrame> gainFrames;
    std::vector<config4scene::PreparedBatch> prepared;
    bool interpolatedGainFrame = false;
    std::string reason;
    std::string callbackReason;
    std::vector<std::int64_t> starts;
    std::vector<std::size_t> sizes;
    bool finite = true;
    std::size_t outputCount = 0U;
    std::uint64_t digest = 1469598103934665603ULL;
    std::size_t total = 0U;
    double speakerGlobalPeak = 0.0;
    long double speakerGlobalSquareSum = 0.0L;
    std::uint64_t speakerGlobalSampleCount = 0U;
    double lfePeak = 0.0;
    long double lfeSquareSum = 0.0L;
    std::uint64_t lfeSampleCount = 0U;
    std::int64_t speakerFirstNonzero = -1;
    std::int64_t lfeFirstNonzero = -1;
    const bool i0pEnabled = !i0pCache.empty();
    eac3render::SofaBrirCacheLoadResult loaded;
    eac3render::I0pOfflineOutput i0p;
    eac3render::I0pOfflineResult i0pResult;
    Clock::duration i0pAppendTime {};
    if (i0pEnabled) {
        loaded = eac3render::loadSofaBrirCache(i0pCache);
        if (!loaded.accepted) {
            std::cerr << "i0p=FAIL reason=" << loaded.reason << '\n';
            return false;
        }
        i0pResult = i0p.init(loaded.cache, kGeneration);
        if (!i0pResult.accepted) {
            std::cerr << "i0p=FAIL reason=" << i0pResult.reason << '\n';
            return false;
        }
    }
    const auto callback = [&](const eac3render::SystemHPlanarMixerBatch &batch) {
        ++outputCount;
        starts.push_back(batch.sampleStart);
        const auto batchSize = static_cast<std::size_t>(
            batch.sampleEnd - batch.sampleStart);
        sizes.push_back(batchSize);
        total += batchSize;
        for (const auto &speaker : batch.speakers) {
            finite = finite && speaker.size() == batchSize;
            for (std::size_t sampleIndex = 0U;
                 sampleIndex < speaker.size(); ++sampleIndex) {
                const float value = speaker[sampleIndex];
                finite = finite && std::isfinite(value);
                if (std::isfinite(value)) {
                    const double sample = static_cast<double>(value);
                    speakerGlobalPeak = std::max(speakerGlobalPeak,
                                                 std::abs(sample));
                    speakerGlobalSquareSum += static_cast<long double>(sample)
                        * static_cast<long double>(sample);
                    ++speakerGlobalSampleCount;
                    if (speakerFirstNonzero < 0 && sample != 0.0)
                        speakerFirstNonzero = batch.sampleStart
                            + static_cast<std::int64_t>(sampleIndex);
                }
                hashFloat(&digest, value);
            }
        }
        finite = finite && batch.lfeSideband.size() == batchSize;
        for (std::size_t sampleIndex = 0U;
             sampleIndex < batch.lfeSideband.size(); ++sampleIndex) {
            const float value = batch.lfeSideband[sampleIndex];
            finite = finite && std::isfinite(value);
            if (std::isfinite(value)) {
                const double sample = static_cast<double>(value);
                lfePeak = std::max(lfePeak, std::abs(sample));
                lfeSquareSum += static_cast<long double>(sample)
                    * static_cast<long double>(sample);
                ++lfeSampleCount;
                if (lfeFirstNonzero < 0 && sample != 0.0)
                    lfeFirstNonzero = batch.sampleStart
                        + static_cast<std::int64_t>(sampleIndex);
            }
            hashFloat(&digest, value);
        }
        if (i0pEnabled) {
            const auto appendStarted = Clock::now();
            i0pResult = i0p.append(batch);
            i0pAppendTime += Clock::now() - appendStarted;
            if (!i0pResult.accepted) {
                callbackReason = i0pResult.reason;
                return false;
            }
        }
        return true;
    };
    const auto sceneDecoder = jocConfig == 3U
        ? config4scene::decodeConfig3File : config4scene::decodeFile;
    if (jobs == 0U || (jocConfig != 3U && jocConfig != 4U)
        || !sceneDecoder(path, maxAUs,
            [&prepared, &gainDurations, &gainFrames, &interpolatedGainFrame](
                const config4scene::PreparedBatch &batch) {
                prepared.push_back(batch);
                gainDurations.push_back(batch.sceneFrame.durationSamples);
                gainFrames.push_back(batch.sceneFrame);
                interpolatedGainFrame = interpolatedGainFrame
                    || std::any_of(batch.sceneFrame.objects.begin(),
                                   batch.sceneFrame.objects.end(),
                                   [](const eac3render::ObjectGainFrame &object) {
                                       return object.interpolated;
                                   });
                return true;
            }, &reason)) {
        std::cerr << "r2c=FAIL reason=" << reason << '\n';
        return false;
    }
    const auto decodeFinished = Clock::now();
    if (prepared.size() != maxAUs + 1U) {
        std::cerr << "r2c=FAIL reason=r2c-prepared-batch-count\n";
        return false;
    }
    CartesianSceneEvaluator evaluator;
    for (const auto &batch : prepared) {
        if (!batch.flush && !evaluator.append(batch, &reason)) {
            std::cerr << "r2c=FAIL reason=" << reason << '\n';
            return false;
        }
    }
    const auto evaluatorFinished = Clock::now();
    bool evaluatedGainFrames = true;
    bool evaluatedInterpolation = false;
    double evaluatedGainPeak = 0.0;
    if (!owner.open()) {
        std::cerr << "r2c=FAIL reason=r2c-owner-open\n";
        return false;
    }
    for (std::size_t index = 0U; index < prepared.size(); ++index) {
        std::vector<double> evaluatedGains;
        bool batchInterpolated = false;
        if (!evaluatePreparedBatch(prepared[index], evaluator,
                                   &evaluatedGains, &batchInterpolated,
                                   &reason, jobs)) {
            evaluatedGainFrames = false;
            break;
        }
        evaluatedInterpolation = evaluatedInterpolation || batchInterpolated;
        for (double gain : evaluatedGains)
            evaluatedGainPeak = std::max(evaluatedGainPeak, std::abs(gain));
        if (!owner.push(prepared[index], callback, &reason, &evaluatedGains)) {
            std::cerr << "r2c=FAIL reason="
                      << (callbackReason.empty() ? reason : callbackReason)
                      << '\n';
            return false;
        }
    }
    if (!evaluatedGainFrames) {
        std::cerr << "r2c=FAIL reason=" << reason << '\n';
        return false;
    }
    const auto mixFinished = Clock::now();
    std::vector<std::size_t> expected;
    std::vector<std::int64_t> expectedStarts;
    if (!expectedTimeline(maxAUs, &expected, &expectedStarts)) {
        std::cerr << "r2c=FAIL reason=r2c-invalid-timeline\n";
        return false;
    }
    finite = finite && outputCount == expected.size();
    double objectPcmPeak = 0.0;
    long double objectPcmSquareSum = 0.0L;
    std::uint64_t objectPcmSampleCount = 0U;
    std::int64_t objectPcmFirstNonzero = -1;
    for (const auto &batch : prepared) {
        for (const auto &object : batch.objects) {
            for (std::size_t sampleIndex = 0U;
                 sampleIndex < object.samples.size(); ++sampleIndex) {
                const double sample = static_cast<double>(object.samples[sampleIndex]);
                objectPcmPeak = std::max(objectPcmPeak, std::abs(sample));
                objectPcmSquareSum += static_cast<long double>(sample)
                    * static_cast<long double>(sample);
                ++objectPcmSampleCount;
                if (objectPcmFirstNonzero < 0 && sample != 0.0)
                    objectPcmFirstNonzero = batch.outputStart
                        + static_cast<std::int64_t>(sampleIndex);
            }
        }
    }
    const bool levelCountsExact = total != 0U
        && total <= std::numeric_limits<std::uint64_t>::max() / kSpeakers
        && speakerGlobalSampleCount
            == static_cast<std::uint64_t>(total) * kSpeakers
        && lfeSampleCount == static_cast<std::uint64_t>(total);
    finite = finite && levelCountsExact
        && std::isfinite(speakerGlobalSquareSum)
        && std::isfinite(lfeSquareSum);
    const double speakerGlobalRms = speakerGlobalSampleCount == 0U ? 0.0
        : std::sqrt(static_cast<double>(speakerGlobalSquareSum
            / static_cast<long double>(speakerGlobalSampleCount)));
    const double lfeRms = lfeSampleCount == 0U ? 0.0
        : std::sqrt(static_cast<double>(lfeSquareSum
            / static_cast<long double>(lfeSampleCount)));
    const double objectPcmRms = objectPcmSampleCount == 0U ? 0.0
        : std::sqrt(static_cast<double>(objectPcmSquareSum
            / static_cast<long double>(objectPcmSampleCount)));
    finite = finite && std::isfinite(speakerGlobalRms) && std::isfinite(lfeRms);
    const bool timeline = sizes == expected && starts == expectedStarts;
    bool gainFramesStable = gainFrames.size() == expected.size();
    std::int64_t gainChangeAt = -1;
    for (std::size_t frame = 1U; gainFramesStable && frame < gainFrames.size(); ++frame) {
        gainFramesStable = gainFrames[frame].objects.size()
                == gainFrames.front().objects.size()
            && gainFrames[frame].lfeEnabled == gainFrames.front().lfeEnabled
            && gainFrames[frame].lfeGain == gainFrames.front().lfeGain
            && gainFrames[frame].bedGains == gainFrames.front().bedGains;
        for (std::size_t object = 0U; gainFramesStable
             && object < gainFrames.front().objects.size(); ++object) {
            gainFramesStable = gainFrames[frame].objects[object].objectId
                    == gainFrames.front().objects[object].objectId
                && gainFrames[frame].objects[object].gain
                    == gainFrames.front().objects[object].gain
                && gainFrames[frame].objects[object].speakerGains
                    == gainFrames.front().objects[object].speakerGains;
        }
        if (!gainFramesStable) gainChangeAt = gainFrames[frame].timestampSamples;
    }
    const bool pass = timeline && finite && evaluatedGainFrames
        && static_cast<std::int64_t>(total) == decodedSourceSamples
        && outputCount == expected.size();
    std::cout << "r2c=" << (pass ? "PASS" : "FAIL")
              << " stage=system-h-planar-bus-mixer jocConfig=" << jocConfig
              << " jobs=" << jobs
              << " batches=" << outputCount
              << " sizes=";
    for (std::size_t value : sizes) std::cout << value << ',';
    std::cout << " starts=";
    for (std::int64_t value : starts) std::cout << value << ',';
    std::cout << " gainDurations=";
    for (std::int64_t value : gainDurations) std::cout << value << ',';
    std::cout << " speakers=22 bed=EMPTY lfe=SEPARATE_SIDE_BAND"
              << " totalSamples=" << total << " finite=" << (finite ? "PASS" : "FAIL")
              << std::setprecision(9)
              << " speakerGlobalPeak=" << speakerGlobalPeak
              << " speakerGlobalRms=" << speakerGlobalRms
              << " speakerFirstNonzero=" << speakerFirstNonzero
              << " objectPcmPeak=" << objectPcmPeak
              << " objectPcmRms=" << objectPcmRms
              << " objectPcmFirstNonzero=" << objectPcmFirstNonzero
              << " evaluatedGainPeak=" << evaluatedGainPeak
              << " lfePeak=" << lfePeak
              << " lfeRms=" << lfeRms
              << " lfeFirstNonzero=" << lfeFirstNonzero
              << " deterministicDigest=0x" << std::hex << digest << std::dec
              << " normalization=NO drc=NO limiter=NO postgain=NO brir=NO"
              << " interpolatedGainFrame=" << (interpolatedGainFrame ? "YES" : "NO")
              << " gainFramesStable=" << (gainFramesStable ? "YES" : "NO")
              << " evaluatedGainFrames=" << (evaluatedGainFrames ? "PASS" : "FAIL")
              << " evaluatedInterpolation=" << (evaluatedInterpolation ? "YES" : "NO");
    if (gainChangeAt >= 0) std::cout << " gainChangeAt=" << gainChangeAt;
    std::cout
              << " decodedSourceSamples=" << decodedSourceSamples
              << " rendererApplied=NO productionAcceptance=INCONCLUSIVE\n";
    const auto metricsFinished = Clock::now();
    const auto milliseconds = [](Clock::duration value) {
        return std::chrono::duration<double, std::milli>(value).count();
    };
    std::cout << std::setprecision(6)
              << "profile=PASS decodeMs="
              << milliseconds(decodeFinished - runStarted)
              << " evaluatorMs="
              << milliseconds(evaluatorFinished - decodeFinished)
              << " mixMs=" << milliseconds(mixFinished - evaluatorFinished)
              << " metricsMs=" << milliseconds(metricsFinished - mixFinished)
              << '\n';
    if (!pass || !i0pEnabled) return pass;
    prepared.clear();
    prepared.shrink_to_fit();
    gainFrames.clear();
    gainFrames.shrink_to_fit();
    i0pResult = i0p.finish(i0pPaths);
    if (!i0pResult.accepted) {
        std::cerr << "i0p=FAIL reason=" << i0pResult.reason << '\n';
        return false;
    }
    std::cout << std::setprecision(9)
              << "i0p=PASS stage=offline-r2c-brir-wav"
              << " sourceFrames=" << i0pResult.sourceFrames
              << " stereoFrames=" << i0pResult.stereoFrames
              << " lfeFrames=" << i0pResult.lfeFrames
              << " tailFrames=" << i0pResult.tailFrames
              << " sampleRate=" << loaded.cache.sampleRate
              << " speakerBusPeak=" << i0pResult.speakerBus.peak
              << " speakerBusRms=" << i0pResult.speakerBus.rms
              << " speakerBusDigest=0x" << std::hex
              << i0pResult.speakerBus.digest << std::dec
              << " stereoPeak=" << i0pResult.stereo.peak
              << " stereoRms=" << i0pResult.stereo.rms
              << " stereoDigest=0x" << std::hex << i0pResult.stereo.digest
              << std::dec
              << " lfePeak=" << i0pResult.lfe.peak
              << " lfeRms=" << i0pResult.lfe.rms
              << " lfeDigest=0x" << std::hex << i0pResult.lfe.digest
              << std::dec
              << " normalization=NO drc=NO limiter=NO postgain=NO"
              << " lfePolicy=ExcludedFromBinaural+SeparateStem"
              << " lfeDownmixPolicy=NO_MIX"
              << " lfeDownmixReason=output-mode-or-lfe-disabled-not-proven"
              << " amplitudeOracle=INCONCLUSIVE"
              << " productionAcceptance=INCONCLUSIVE\n";
    const auto i0pFinished = Clock::now();
    std::cout << "profileI0p=PASS i0pMs="
              << milliseconds(i0pFinished - metricsFinished)
              << " appendMs=" << milliseconds(i0pAppendTime)
              << " totalMs=" << milliseconds(i0pFinished - runStarted)
              << '\n';
    return true;
}

} // namespace config4r2c

int main(int argc, char **argv)
{
    if (argc == 2 && std::string(argv[1]) == "--self-test")
        return config4r2c::selfTest() ? 0 : 1;
    unsigned maxAUs = 3U;
    unsigned jocConfig = 4U;
    unsigned jobs = 1U;
    std::string path;
    std::string i0pCache;
    eac3render::I0pOfflinePaths i0pPaths;
    for (int index = 1; index < argc; ++index) {
        const std::string option = argv[index];
        const auto value = [&]() -> const char * {
            return index + 1 < argc ? argv[++index] : nullptr;
        };
        try {
            if (option == "--max-aus") {
                const char *text = value();
                if (!text) throw std::out_of_range("max-aus");
                const unsigned long long parsed = std::stoull(text);
                if (parsed == 0U || parsed > static_cast<unsigned long long>(
                        std::numeric_limits<unsigned>::max()))
                    throw std::out_of_range("max-aus");
                maxAUs = static_cast<unsigned>(parsed);
            } else if (option == "--joc-config") {
                const char *text = value();
                if (!text) throw std::out_of_range("joc-config");
                jocConfig = static_cast<unsigned>(std::stoul(text));
                if (jocConfig != 3U && jocConfig != 4U)
                    throw std::out_of_range("joc-config");
            } else if (option == "--jobs") {
                const char *text = value();
                if (!text) throw std::out_of_range("jobs");
                jobs = static_cast<unsigned>(std::stoul(text));
                if (jobs == 0U || jobs > 64U)
                    throw std::out_of_range("jobs");
            } else if (option == "--i0p-cache") {
                const char *text = value(); if (!text) throw std::out_of_range("cache");
                i0pCache = text;
            } else if (option == "--i0p-stereo-wav") {
                const char *text = value(); if (!text) throw std::out_of_range("stereo");
                i0pPaths.stereoWav = text;
            } else if (option == "--i0p-lfe-wav") {
                const char *text = value(); if (!text) throw std::out_of_range("lfe");
                i0pPaths.lfeWav = text;
            } else if (option == "--i0p-report") {
                const char *text = value(); if (!text) throw std::out_of_range("report");
                i0pPaths.reportJson = text;
            } else if (!option.empty() && option[0] == '-') {
                throw std::out_of_range("option");
            } else if (path.empty()) {
                path = option;
            } else {
                throw std::out_of_range("path");
            }
        } catch (...) {
            std::cerr << "r2c=FAIL reason=r2c-invalid-cli\n";
            return 2;
        }
    }
    const bool anyI0p = !i0pCache.empty() || !i0pPaths.stereoWav.empty()
        || !i0pPaths.lfeWav.empty() || !i0pPaths.reportJson.empty();
    const bool completeI0p = !i0pCache.empty() && !i0pPaths.stereoWav.empty()
        && !i0pPaths.lfeWav.empty() && !i0pPaths.reportJson.empty();
    if (anyI0p && !completeI0p) {
        std::cerr << "i0p=FAIL reason=i0p-cli-options-incomplete\n";
        return 2;
    }
    if (path.empty()) {
        std::cerr << "Usage: Eac3NativeConfig4R2CProbe <raw.eac3|raw.ec3|raw.eb3>\n"
                     "       Eac3NativeConfig4R2CProbe --joc-config 3|4"
                     " --max-aus N [--jobs 1..64] <raw.eac3|raw.ec3|raw.eb3>\n"
                     "       Eac3NativeConfig4R2CProbe --joc-config 3|4"
                     " --max-aus N --i0p-cache CACHE"
                     " --i0p-stereo-wav STEREO.wav --i0p-lfe-wav LFE.wav"
                     " --i0p-report REPORT.json <raw.eb3>\n"
                     "       Eac3NativeConfig4R2CProbe --self-test\n";
        return 2;
    }
    return config4r2c::runFile(path, maxAUs, i0pCache, i0pPaths, jocConfig,
                              jobs)
        ? 0 : 1;
}
