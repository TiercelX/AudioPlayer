#pragma once

#include "spatial-bridge-core.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace eac3renderer {

enum class SpatialRenderer {
    Standard,
    Hrtf,
};

enum class HrtfEnvironment {
    Small,
    Outdoors,
};

enum class Outcome {
    Pass,
    Fail,
    Inconclusive,
};

const char *outcomeText(Outcome outcome);
const char *spatialRendererText(SpatialRenderer renderer);
const char *hrtfEnvironmentText(HrtfEnvironment environment);

struct Options {
    std::size_t queueBatches = 8U;
    std::size_t prebufferBatches = 4U;
    std::uint32_t pushTimeoutMilliseconds = 2000U;
    bool disableLfe = false;
    eac3gate7b::PositionRadiusMode positionRadiusMode =
        eac3gate7b::PositionRadiusMode::Source;
    eac3gate7b::PositionDirectionMode positionDirectionMode =
        eac3gate7b::PositionDirectionMode::Metadata;
    float azimuthFocus = 0.0f;
    SpatialRenderer spatialRenderer = SpatialRenderer::Standard;
    HrtfEnvironment hrtfEnvironment = HrtfEnvironment::Small;
};

bool validateOptions(const Options &options, std::string *reason);
bool exactBufferLength(std::size_t frameCount, std::size_t byteLength);
bool exactObjectFrameLength(std::size_t frameCount, std::size_t sampleCount);
bool scaleHrtfSamples(const float *source, float *destination, std::size_t count,
                      float volume);

struct Metrics {
    Outcome outcome = Outcome::Inconclusive;
    std::string firstFailureStage;
    std::int64_t firstFailureHresult = 0;
    std::string endpointId;
    std::string renderer = "standard";
    std::string hrtfEnvironment = "small";
    bool interfaceAvailable = false;
    bool hrtfDistanceDecayNull = true;
    std::size_t preScaledSamples = 0;
    float preScaleMin = 0.0f;
    float preScaleMax = 0.0f;
    std::uint32_t nativeStaticMask = 0;
    std::uint32_t maximumDynamicObjects = 0;
    std::uint32_t activatedDynamicObjects = 0;
    std::uint32_t capacityNotifications = 0;
    std::uint32_t latestCapacity = 0;
    std::uint32_t objectFormatRate = 0;
    std::uint32_t objectFormatChannels = 0;
    std::uint32_t objectFormatBits = 0;
    std::size_t prebufferBatches = 0;
    std::size_t prebufferFrames = 0;
    bool initialPropertiesReady = false;
    std::size_t renderAttempts = 0;
    std::size_t renderCommits = 0;
    std::size_t beginCalls = 0;
    std::size_t endCalls = 0;
    std::size_t exactBufferCalls = 0;
    std::uint64_t exactBufferBytes = 0;
    std::size_t propertyCalls = 0;
    std::size_t lfeVolumeCalls = 0;
    float lfeVolume = 0.0f;
    std::size_t inactiveObjects = 0;
    std::size_t frameCountMin = 0;
    std::size_t frameCountMax = 0;
    std::uint64_t submittedFrames = 0;
    std::uint64_t sourceFrames = 0;
    std::uint64_t paddingFrames = 0;
    std::uint64_t underrunFrames = 0;
    std::uint64_t canceledStagedFrames = 0;
    std::size_t metadataApplied = 0;
    std::size_t metadataDeferred = 0;
    std::size_t eosPendingMetadata = 0;
    std::size_t terminalQuanta = 0;
    std::int64_t maximumPropertyLateness = 0;
    std::array<std::uint64_t, eac3gate7b::kDynamicObjectCount + 1U> finiteSamples {};
    std::array<float, eac3gate7b::kDynamicObjectCount + 1U> peaks {};
    std::uint64_t lfeFiniteSamples = 0;
    float lfePeak = 0.0f;
    bool terminalQuantum = false;
    bool initialized = false;
    bool streamStarted = false;
    bool streamStopped = false;
    bool streamReset = false;
    bool consumerJoined = false;
    bool comReleased = false;
    bool cleanupComplete = false;
    eac3bridge::QueueMetrics queue;
};

class SpatialBridgeRenderer {
public:
    explicit SpatialBridgeRenderer(Options options = {});
    ~SpatialBridgeRenderer();

    bool start();
    eac3bridge::QueueStatus submit(eac3gate6c::Batch batch);
    void finish();
    void cancel();
    Metrics result() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct SelfTestReport {
    bool pass = false;
    std::size_t cases = 0;
    std::string reason;
};

SelfTestReport runSelfTest();

} // namespace eac3renderer
