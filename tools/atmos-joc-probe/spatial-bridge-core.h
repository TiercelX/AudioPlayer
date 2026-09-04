#pragma once

#include "joc-gate6c.h"
#include "spatial-property-adapter.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace eac3bridge {

// The bridge is intentionally bounded to shapes accepted by Gate6C while
// leaving room for a small render quantum larger than a single unit.
constexpr std::size_t kMaxRenderFrames = eac3gate6c::kSamplesPerUnit * 8U;
constexpr std::size_t kMaxMetadataUpdatesPerBatch =
    eac3gate6c::kDynamicObjectCount /* boundary */
    + eac3gate6c::kDynamicObjectCount * 8U;
constexpr std::size_t kMaxPendingMetadata = kMaxMetadataUpdatesPerBatch * 8U;

enum class QueueStatus {
    Pass,
    Timeout,
    Closed,
    Canceled,
    Rejected,
};

const char *queueStatusText(QueueStatus status);

bool validateBatch(const eac3gate6c::Batch &batch, std::string *reason);

struct QueueMetrics {
    std::size_t pushes = 0;
    std::size_t pops = 0;
    std::size_t producerWaits = 0;
    std::size_t consumerWaits = 0;
    std::size_t producerTimeouts = 0;
    std::size_t consumerTimeouts = 0;
    std::size_t fullWaits = 0;
    std::size_t emptyWaits = 0;
    std::size_t maximumDepth = 0;
    std::size_t closeWakes = 0;
    std::size_t cancelWakes = 0;
    std::size_t staleGenerationRejects = 0;
    std::size_t validationRejects = 0;
    std::size_t admissionTimelineRejects = 0;
    std::size_t flushRejects = 0;
    std::size_t canceledDiscarded = 0;
    std::size_t itemsRemaining = 0;
    std::size_t framesRemaining = 0;
    std::uint32_t initialPropertyMask = 0;
};

class BoundedBatchQueue {
public:
    explicit BoundedBatchQueue(std::size_t capacity, std::uint64_t generation = 1U);

    QueueStatus push(eac3gate6c::Batch batch, std::uint64_t generation,
                     std::uint32_t timeoutMilliseconds);
    QueueStatus pop(eac3gate6c::Batch *batch, std::uint32_t timeoutMilliseconds);
    void close();
    void cancel();
    bool resetGeneration(std::uint64_t generation);
    bool closedAndEmpty() const;

    std::size_t capacity() const { return capacity_; }
    std::uint64_t generation() const;
    QueueMetrics metrics() const;

private:
    struct Item {
        std::uint64_t generation = 0U;
        eac3gate6c::Batch batch;
    };

    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable notEmpty_;
    std::condition_variable notFull_;
    std::deque<Item> batches_;
    std::uint64_t generation_ = 1U;
    bool closed_ = false;
    bool canceled_ = false;
    bool haveAdmissionInterval_ = false;
    std::int64_t admissionNextOutputStart_ = 0;
    bool haveAdmissionMetadata_ = false;
    std::int64_t admissionLastMetadataPosition_ = 0;
    unsigned admissionLastMetadataObject_ = 0U;
    bool finalFlushAdmitted_ = false;
    std::uint32_t initialPropertyMask_ = 0;
    QueueMetrics metrics_;
};

enum class RenderStatus {
    Pass,
    NotReady,
    EndOfStream,
    Timeout,
    Canceled,
    Fail,
};

const char *renderStatusText(RenderStatus status);

struct RenderQuantum {
    std::int64_t sourceStart = 0;
    std::int64_t sourceEnd = 0;
    std::size_t sourceFrames = 0;
    std::size_t finalPaddingFrames = 0;
    std::size_t underrunFrames = 0;
    bool terminal = false;
    std::size_t metadataApplied = 0;
    std::size_t metadataDeferred = 0;
    std::int64_t maximumPropertyLateness = 0;
    std::vector<std::vector<float>> objects;
    std::vector<float> lfe;
    std::vector<eac3gate7b::WindowsProperty> properties;
};

struct BridgeMetrics {
    std::size_t quanta = 0;
    std::size_t terminalQuanta = 0;
    std::size_t sourceFrames = 0;
    std::size_t finalPaddingFrames = 0;
    std::size_t underrunFrames = 0;
    std::size_t underrunPasses = 0;
    std::size_t metadataApplied = 0;
    std::size_t metadataDeferred = 0;
    std::int64_t maximumPropertyLateness = 0;
    std::array<std::size_t, eac3gate7b::kDynamicObjectCount + 1U> finiteSamples {};
    std::array<float, eac3gate7b::kDynamicObjectCount + 1U> peaks {};
    std::size_t lfeFiniteSamples = 0;
    float lfePeak = 0.0f;
    std::size_t propertyFailures = 0;
    std::size_t timelineFailures = 0;
    std::size_t generationFailures = 0;
    std::size_t canceledStagedFrames = 0;
    std::size_t canceledCurrentFrames = 0;
    std::size_t canceledPendingMetadata = 0;
    std::size_t eosPendingMetadata = 0;
};

class SpatialBridgeCore {
public:
    explicit SpatialBridgeCore(std::size_t queueCapacity = 8U,
                               std::uint64_t generation = 1U,
                               eac3gate7b::Geometry geometry
                                   = eac3gate7b::referenceGeometry());

    QueueStatus push(eac3gate6c::Batch batch, std::uint32_t timeoutMilliseconds = 0U);
    void close();
    void cancel();
    bool resetGeneration(std::uint64_t generation);

    RenderStatus renderQuantum(std::size_t frameCount,
                               std::uint32_t timeoutMilliseconds,
                               RenderQuantum *quantum);
    const BridgeMetrics &metrics() const { return metrics_; }
    const QueueMetrics queueMetrics() const { return queue_.metrics(); }
    std::uint64_t generation() const { return generation_; }
    std::int64_t sourceTimeline() const { return nextSourcePosition_; }
    std::string failureReason() const { return failureReason_; }

private:
    void finalizeCancel();
    bool acquireBatch(std::uint32_t timeoutMilliseconds, RenderStatus *status);
    bool loadPendingMetadata(const eac3gate6c::Batch &batch);
    bool applyDueMetadata(std::int64_t quantumStart, RenderQuantum *quantum);
    bool evaluateProperties(std::int64_t sourcePosition,
                            std::vector<eac3gate7b::WindowsProperty> *properties);
    void fail(const std::string &reason);

    BoundedBatchQueue queue_;
    std::uint64_t generation_ = 1U;
    eac3gate7b::Geometry geometry_;
    eac3gate7b::Adapter adapter_;
    std::optional<eac3gate6c::Batch> currentBatch_;
    std::size_t currentOffset_ = 0;
    std::vector<eac3gate7b::Update> pendingMetadata_;
    bool haveLastMetadata_ = false;
    std::int64_t lastMetadataPosition_ = 0;
    unsigned lastMetadataObject_ = 0U;
    bool endOfStream_ = false;
    bool canceled_ = false;
    bool cancelFinalized_ = false;
    std::atomic<bool> cancelRequested_ {false};
    bool failed_ = false;
    std::int64_t nextSourcePosition_ = 0;
    std::size_t activeQuantumSourceFrames_ = 0;
    BridgeMetrics metrics_;
    std::string failureReason_;
};

struct SelfTestReport {
    bool pass = false;
    std::size_t cases = 0;
    std::string reason;
};

SelfTestReport runSelfTest();

} // namespace eac3bridge
