#include "spatial-bridge-core.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <thread>

namespace eac3bridge {
namespace {

bool finite(float value) { return std::isfinite(value); }

bool safeEnd(std::int64_t start, std::size_t count, std::int64_t *end)
{
    if (!end || count > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
        return false;
    }
    const std::int64_t length = static_cast<std::int64_t>(count);
    if (start > std::numeric_limits<std::int64_t>::max() - length) return false;
    *end = start + length;
    return true;
}

bool samePositionOrder(const std::vector<eac3gate6c::MetadataUpdate> &updates,
                       std::string *reason)
{
    for (std::size_t index = 1; index < updates.size(); ++index) {
        const auto &prior = updates[index - 1U];
        const auto &current = updates[index];
        if (current.sourcePosition < prior.sourcePosition) {
            if (reason) *reason = "metadata-source-position-not-monotonic";
            return false;
        }
        if (current.sourcePosition == prior.sourcePosition
            && current.objectIndex <= prior.objectIndex) {
            if (reason) *reason = "metadata-object-order-invalid";
            return false;
        }
    }
    return true;
}

} // namespace

const char *queueStatusText(QueueStatus status)
{
    switch (status) {
    case QueueStatus::Pass: return "PASS";
    case QueueStatus::Timeout: return "TIMEOUT";
    case QueueStatus::Closed: return "CLOSED";
    case QueueStatus::Canceled: return "CANCELED";
    case QueueStatus::Rejected: return "REJECTED";
    }
    return "REJECTED";
}

bool validateBatch(const eac3gate6c::Batch &batch, std::string *reason)
{
    if (!reason) return false;
    *reason = {};
    if (batch.objects.size() != eac3gate6c::kDynamicObjectCount) {
        *reason = "object-plane-count-not-15";
        return false;
    }
    const std::size_t samples = batch.objects.front().size();
    if (samples == 0U || samples > kMaxRenderFrames || batch.lfe.size() != samples) {
        *reason = "sample-count-zero-or-lfe-mismatch";
        return false;
    }
    if (batch.metadata.size() > kMaxMetadataUpdatesPerBatch) {
        *reason = "metadata-batch-too-large";
        return false;
    }
    for (const auto &object : batch.objects) {
        if (object.size() != samples) {
            *reason = "object-plane-sample-count-mismatch";
            return false;
        }
        for (float value : object) {
            if (!finite(value)) {
                *reason = "nonfinite-object-pcm";
                return false;
            }
        }
    }
    for (float value : batch.lfe) {
        if (!finite(value)) {
            *reason = "nonfinite-lfe-pcm";
            return false;
        }
    }
    std::int64_t expectedEnd = 0;
    if (batch.outputStart < 0 || !safeEnd(batch.outputStart, samples, &expectedEnd)
        || batch.outputEnd != expectedEnd) {
        *reason = "output-interval-invalid-or-overflowed";
        return false;
    }
    for (const auto &update : batch.metadata) {
        if (update.objectIndex < 1U || update.objectIndex > eac3gate7b::kDynamicObjectCount) {
            *reason = "metadata-object-index-out-of-range";
            return false;
        }
        if (update.sourcePosition < batch.outputStart
            || update.sourcePosition >= batch.outputEnd) {
            *reason = "metadata-outside-output-interval";
            return false;
        }
    }
    return samePositionOrder(batch.metadata, reason);
}

BoundedBatchQueue::BoundedBatchQueue(std::size_t capacity, std::uint64_t generation)
    : capacity_(std::max<std::size_t>(1U, capacity)), generation_(generation)
{}

QueueStatus BoundedBatchQueue::push(eac3gate6c::Batch batch,
                                    std::uint64_t generation,
                                    std::uint32_t timeoutMilliseconds)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation == 0U || generation != generation_) {
            ++metrics_.staleGenerationRejects;
            return QueueStatus::Rejected;
        }
        if (canceled_) return QueueStatus::Canceled;
        if (closed_) return QueueStatus::Closed;
    }
    std::string reason;
    if (!validateBatch(batch, &reason)) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++metrics_.validationRejects;
        return QueueStatus::Rejected;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    // Validation ran without the queue lock so producers can never commit a
    // batch against a generation that was reset during that validation.
    if (generation == 0U || generation != generation_) {
        ++metrics_.staleGenerationRejects;
        return QueueStatus::Rejected;
    }
    if (canceled_) return QueueStatus::Canceled;
    if (closed_) return QueueStatus::Closed;
    auto admissionCheck = [&]() {
        if (finalFlushAdmitted_) {
            ++metrics_.validationRejects;
            ++metrics_.flushRejects;
            return false;
        }
        if (!haveAdmissionInterval_ && batch.outputStart != 0) {
            ++metrics_.validationRejects;
            ++metrics_.admissionTimelineRejects;
            return false;
        }
        if (haveAdmissionInterval_ && batch.outputStart != admissionNextOutputStart_) {
            ++metrics_.validationRejects;
            ++metrics_.admissionTimelineRejects;
            return false;
        }
        bool nextHaveMetadata = haveAdmissionMetadata_;
        std::int64_t nextMetadataPosition = admissionLastMetadataPosition_;
        unsigned nextMetadataObject = admissionLastMetadataObject_;
        for (const auto &metadata : batch.metadata) {
            if (nextHaveMetadata
                && (metadata.sourcePosition < nextMetadataPosition
                    || (metadata.sourcePosition == nextMetadataPosition
                        && metadata.objectIndex <= nextMetadataObject))) {
                ++metrics_.validationRejects;
                ++metrics_.admissionTimelineRejects;
                return false;
            }
            nextHaveMetadata = true;
            nextMetadataPosition = metadata.sourcePosition;
            nextMetadataObject = metadata.objectIndex;
        }
        return true;
    };
    if (!admissionCheck()) return QueueStatus::Rejected;
    if (batches_.size() >= capacity_) {
        ++metrics_.fullWaits;
        if (timeoutMilliseconds == 0U) {
            ++metrics_.producerTimeouts;
            return QueueStatus::Timeout;
        }
        ++metrics_.producerWaits;
        const bool available = notFull_.wait_for(
            lock, std::chrono::milliseconds(timeoutMilliseconds),
            [&] { return batches_.size() < capacity_ || closed_ || canceled_; });
        if (!available) {
            ++metrics_.producerTimeouts;
            return QueueStatus::Timeout;
        }
        if (canceled_) return QueueStatus::Canceled;
        if (closed_) return QueueStatus::Closed;
        if (generation == 0U || generation != generation_) {
            ++metrics_.staleGenerationRejects;
            return QueueStatus::Rejected;
        }
        if (!admissionCheck()) return QueueStatus::Rejected;
    }
    bool nextHaveMetadata = haveAdmissionMetadata_;
    std::int64_t nextMetadataPosition = admissionLastMetadataPosition_;
    unsigned nextMetadataObject = admissionLastMetadataObject_;
    for (const auto &metadata : batch.metadata) {
        if (nextHaveMetadata
            && (metadata.sourcePosition < nextMetadataPosition
                || (metadata.sourcePosition == nextMetadataPosition
                    && metadata.objectIndex <= nextMetadataObject))) {
            ++metrics_.validationRejects;
            ++metrics_.admissionTimelineRejects;
            return QueueStatus::Rejected;
        }
        nextHaveMetadata = true;
        nextMetadataPosition = metadata.sourcePosition;
        nextMetadataObject = metadata.objectIndex;
    }
    batches_.push_back(BoundedBatchQueue::Item {generation, std::move(batch)});
    if (!haveAdmissionInterval_) haveAdmissionInterval_ = true;
    admissionNextOutputStart_ = batches_.back().batch.outputEnd;
    haveAdmissionMetadata_ = nextHaveMetadata;
    admissionLastMetadataPosition_ = nextMetadataPosition;
    admissionLastMetadataObject_ = nextMetadataObject;
    for (const auto &metadata : batches_.back().batch.metadata) {
        if (metadata.sourcePosition == 0 && metadata.objectIndex >= 1U
            && metadata.objectIndex <= eac3gate6c::kDynamicObjectCount) {
            initialPropertyMask_ |= 1U << (metadata.objectIndex - 1U);
        }
    }
    finalFlushAdmitted_ = batches_.back().batch.flush;
    ++metrics_.pushes;
    metrics_.maximumDepth = std::max(metrics_.maximumDepth, batches_.size());
    metrics_.itemsRemaining = batches_.size();
    lock.unlock();
    notEmpty_.notify_one();
    return QueueStatus::Pass;
}

QueueStatus BoundedBatchQueue::pop(eac3gate6c::Batch *batch,
                                   std::uint32_t timeoutMilliseconds)
{
    if (!batch) return QueueStatus::Rejected;
    std::unique_lock<std::mutex> lock(mutex_);
    if (canceled_) return QueueStatus::Canceled;
    if (batches_.empty() && !closed_) {
        ++metrics_.emptyWaits;
        if (timeoutMilliseconds == 0U) {
            ++metrics_.consumerTimeouts;
            return QueueStatus::Timeout;
        }
        ++metrics_.consumerWaits;
        const bool available = notEmpty_.wait_for(
            lock, std::chrono::milliseconds(timeoutMilliseconds),
            [&] { return !batches_.empty() || closed_ || canceled_; });
        if (!available) {
            ++metrics_.consumerTimeouts;
            return QueueStatus::Timeout;
        }
    }
    if (canceled_) return QueueStatus::Canceled;
    if (batches_.empty() && closed_) return QueueStatus::Closed;
    if (batches_.front().generation == 0U
        || batches_.front().generation != generation_) {
        ++metrics_.staleGenerationRejects;
        batches_.pop_front();
        metrics_.itemsRemaining = batches_.size();
        notFull_.notify_one();
        return QueueStatus::Rejected;
    }
    *batch = std::move(batches_.front().batch);
    batches_.pop_front();
    ++metrics_.pops;
    metrics_.itemsRemaining = batches_.size();
    lock.unlock();
    notFull_.notify_one();
    return QueueStatus::Pass;
}

void BoundedBatchQueue::close()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_ || canceled_) return;
        closed_ = true;
        ++metrics_.closeWakes;
    }
    notEmpty_.notify_all();
    notFull_.notify_all();
}

void BoundedBatchQueue::cancel()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (canceled_) return;
        metrics_.canceledDiscarded += batches_.size();
        batches_.clear();
        metrics_.itemsRemaining = 0U;
        canceled_ = true;
        ++metrics_.cancelWakes;
    }
    notEmpty_.notify_all();
    notFull_.notify_all();
}

bool BoundedBatchQueue::resetGeneration(std::uint64_t generation)
{
    if (generation == 0U) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if ((!closed_ && !canceled_) || !batches_.empty()) return false;
    generation_ = generation;
    closed_ = false;
    canceled_ = false;
    haveAdmissionInterval_ = false;
    admissionNextOutputStart_ = 0;
    haveAdmissionMetadata_ = false;
    admissionLastMetadataPosition_ = 0;
    admissionLastMetadataObject_ = 0U;
    finalFlushAdmitted_ = false;
    initialPropertyMask_ = 0U;
    metrics_ = QueueMetrics {};
    return true;
}

std::uint64_t BoundedBatchQueue::generation() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return generation_;
}

QueueMetrics BoundedBatchQueue::metrics() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    QueueMetrics copy = metrics_;
    copy.itemsRemaining = batches_.size();
    copy.framesRemaining = 0U;
    for (const auto &item : batches_) copy.framesRemaining += item.batch.lfe.size();
    copy.initialPropertyMask = initialPropertyMask_;
    return copy;
}

bool BoundedBatchQueue::closedAndEmpty() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_ && batches_.empty();
}

const char *renderStatusText(RenderStatus status)
{
    switch (status) {
    case RenderStatus::Pass: return "PASS";
    case RenderStatus::NotReady: return "NOT_READY";
    case RenderStatus::EndOfStream: return "END_OF_STREAM";
    case RenderStatus::Timeout: return "TIMEOUT";
    case RenderStatus::Canceled: return "CANCELED";
    case RenderStatus::Fail: return "FAIL";
    }
    return "FAIL";
}

SpatialBridgeCore::SpatialBridgeCore(std::size_t queueCapacity,
                                     std::uint64_t generation,
                                     eac3gate7b::Geometry geometry)
    : queue_(queueCapacity, generation), generation_(generation), geometry_(geometry), adapter_(geometry)
{}

QueueStatus SpatialBridgeCore::push(eac3gate6c::Batch batch,
                                    std::uint32_t timeoutMilliseconds)
{
    return queue_.push(std::move(batch), generation_, timeoutMilliseconds);
}

void SpatialBridgeCore::close() { queue_.close(); }
void SpatialBridgeCore::cancel()
{
    cancelRequested_.store(true, std::memory_order_release);
    queue_.cancel();
}

void SpatialBridgeCore::finalizeCancel()
{
    if (cancelFinalized_) return;
    metrics_.canceledStagedFrames += activeQuantumSourceFrames_;
    activeQuantumSourceFrames_ = 0U;
    if (currentBatch_) {
        metrics_.canceledCurrentFrames = currentBatch_->lfe.size() - currentOffset_;
    }
    metrics_.canceledPendingMetadata = pendingMetadata_.size();
    currentBatch_.reset();
    currentOffset_ = 0U;
    pendingMetadata_.clear();
    canceled_ = true;
    cancelFinalized_ = true;
}

bool SpatialBridgeCore::resetGeneration(std::uint64_t generation)
{
    if (currentBatch_ || !pendingMetadata_.empty()) return false;
    if (cancelRequested_.load(std::memory_order_acquire) && !cancelFinalized_) {
        return false;
    }
    if (!queue_.resetGeneration(generation)) return false;
    generation_ = generation;
    adapter_ = eac3gate7b::Adapter(geometry_);
    currentBatch_.reset();
    currentOffset_ = 0U;
    pendingMetadata_.clear();
    haveLastMetadata_ = false;
    lastMetadataPosition_ = 0;
    lastMetadataObject_ = 0U;
    endOfStream_ = false;
    canceled_ = false;
    cancelFinalized_ = false;
    cancelRequested_.store(false, std::memory_order_release);
    failed_ = false;
    nextSourcePosition_ = 0;
    activeQuantumSourceFrames_ = 0U;
    metrics_ = BridgeMetrics {};
    failureReason_.clear();
    return true;
}

bool SpatialBridgeCore::loadPendingMetadata(const eac3gate6c::Batch &batch)
{
    if (pendingMetadata_.size() + batch.metadata.size() > kMaxPendingMetadata) {
        ++metrics_.propertyFailures;
        fail("pending-metadata-bound-exceeded");
        return false;
    }
    bool nextHaveMetadata = haveLastMetadata_;
    std::int64_t nextMetadataPosition = lastMetadataPosition_;
    unsigned nextMetadataObject = lastMetadataObject_;
    for (const auto &metadata : batch.metadata) {
        if (nextHaveMetadata
            && (metadata.sourcePosition < nextMetadataPosition
                || (metadata.sourcePosition == nextMetadataPosition
                    && metadata.objectIndex <= nextMetadataObject))) {
            fail("metadata-order-across-batches-invalid");
            return false;
        }
        nextHaveMetadata = true;
        nextMetadataPosition = metadata.sourcePosition;
        nextMetadataObject = metadata.objectIndex;
    }
    pendingMetadata_.reserve(pendingMetadata_.size() + batch.metadata.size());
    for (const auto &metadata : batch.metadata) {
        pendingMetadata_.push_back(eac3gate7b::Update {
            metadata.sourcePosition,
            metadata.blockIndex,
            metadata.rampDuration,
            metadata.objectIndex,
            metadata.state});
    }
    haveLastMetadata_ = nextHaveMetadata;
    lastMetadataPosition_ = nextMetadataPosition;
    lastMetadataObject_ = nextMetadataObject;
    return true;
}

bool SpatialBridgeCore::acquireBatch(std::uint32_t timeoutMilliseconds,
                                     RenderStatus *status)
{
    if (!status) return false;
    if (cancelRequested_.load(std::memory_order_acquire)) {
        finalizeCancel();
        *status = RenderStatus::Canceled;
        return false;
    }
    if (currentBatch_) return true;
    if (endOfStream_) {
        *status = RenderStatus::EndOfStream;
        return false;
    }
    eac3gate6c::Batch batch;
    const QueueStatus queueStatus = queue_.pop(&batch, timeoutMilliseconds);
    if (queueStatus == QueueStatus::Timeout) {
        *status = RenderStatus::Timeout;
        return false;
    }
    if (queueStatus == QueueStatus::Closed) {
        if (!pendingMetadata_.empty()) {
            metrics_.eosPendingMetadata += pendingMetadata_.size();
            pendingMetadata_.clear();
        }
        endOfStream_ = true;
        *status = RenderStatus::EndOfStream;
        return false;
    }
    if (queueStatus == QueueStatus::Canceled) {
        finalizeCancel();
        *status = RenderStatus::Canceled;
        return false;
    }
    if (queueStatus != QueueStatus::Pass) {
        fail("queue-rejected-consumer-item");
        *status = RenderStatus::Fail;
        return false;
    }
    if (cancelRequested_.load(std::memory_order_acquire)) {
        currentBatch_ = std::move(batch);
        currentOffset_ = 0U;
        finalizeCancel();
        *status = RenderStatus::Canceled;
        return false;
    }
    if (batch.outputStart != nextSourcePosition_) {
        ++metrics_.timelineFailures;
        fail("source-timeline-gap-or-overlap");
        *status = RenderStatus::Fail;
        return false;
    }
    if (!loadPendingMetadata(batch)) {
        *status = RenderStatus::Fail;
        return false;
    }
    currentBatch_ = std::move(batch);
    currentOffset_ = 0U;
    return true;
}

bool SpatialBridgeCore::applyDueMetadata(std::int64_t quantumStart,
                                         RenderQuantum *quantum)
{
    if (!quantum) return false;
    std::size_t dueCount = 0U;
    while (dueCount < pendingMetadata_.size()
           && pendingMetadata_[dueCount].sourcePosition <= quantumStart) ++dueCount;
    if (dueCount != 0U) {
        std::vector<eac3gate7b::Update> due(
            pendingMetadata_.begin(), pendingMetadata_.begin() + dueCount);
        const eac3gate7b::ApplyResult result = adapter_.applyBatch(due);
        if (result.disposition != eac3gate7b::Disposition::Pass) {
            ++metrics_.propertyFailures;
            fail("gate7b-metadata-apply-failed:" + result.reason);
            return false;
        }
        quantum->metadataApplied = dueCount;
        for (const auto &update : due) {
            quantum->maximumPropertyLateness = std::max(
                quantum->maximumPropertyLateness,
                quantumStart - update.sourcePosition);
        }
        pendingMetadata_.erase(pendingMetadata_.begin(),
                               pendingMetadata_.begin() + dueCount);
    }
    quantum->metadataDeferred = pendingMetadata_.size();
    return true;
}

bool SpatialBridgeCore::evaluateProperties(
    std::int64_t sourcePosition,
    std::vector<eac3gate7b::WindowsProperty> *properties)
{
    if (!properties) return false;
    properties->clear();
    properties->reserve(eac3gate7b::kDynamicObjectCount);
    for (unsigned object = 1U; object <= eac3gate7b::kDynamicObjectCount; ++object) {
        eac3gate7b::WindowsProperty property;
        std::string reason;
        if (!adapter_.evaluate(object, sourcePosition, &property, &reason)) {
            return false;
        }
        properties->push_back(property);
    }
    return true;
}

void SpatialBridgeCore::fail(const std::string &reason)
{
    failed_ = true;
    if (failureReason_.empty()) failureReason_ = reason;
}

RenderStatus SpatialBridgeCore::renderQuantum(std::size_t frameCount,
                                              std::uint32_t timeoutMilliseconds,
                                              RenderQuantum *quantum)
{
    if (cancelRequested_.load(std::memory_order_acquire)) {
        finalizeCancel();
        return RenderStatus::Canceled;
    }
    if (canceled_) return RenderStatus::Canceled;
    if (!quantum || frameCount == 0U || frameCount > kMaxRenderFrames) {
        return RenderStatus::Fail;
    }
    if (failed_) return RenderStatus::Fail;
    if (endOfStream_ && !currentBatch_) return RenderStatus::EndOfStream;
    RenderQuantum staged;
    activeQuantumSourceFrames_ = 0U;
    staged.sourceStart = nextSourcePosition_;
    staged.sourceEnd = nextSourcePosition_;
    staged.objects.assign(eac3gate7b::kDynamicObjectCount,
                          std::vector<float>(frameCount, 0.0f));
    staged.lfe.assign(frameCount, 0.0f);

    RenderStatus acquireStatus = RenderStatus::Timeout;
    if (!currentBatch_ && !endOfStream_
        && !acquireBatch(timeoutMilliseconds, &acquireStatus)) {
        if (acquireStatus == RenderStatus::Canceled || acquireStatus == RenderStatus::Fail) {
            return acquireStatus;
        }
        if (acquireStatus == RenderStatus::EndOfStream) return RenderStatus::EndOfStream;
    }
    if (cancelRequested_.load(std::memory_order_acquire)) {
        finalizeCancel();
        return RenderStatus::Canceled;
    }
    if (!applyDueMetadata(staged.sourceStart, &staged)) {
        if (cancelRequested_.load(std::memory_order_acquire)) {
            finalizeCancel();
            return RenderStatus::Canceled;
        }
        return RenderStatus::Fail;
    }
    if (!evaluateProperties(staged.sourceStart, &staged.properties)) {
        if (cancelRequested_.load(std::memory_order_acquire)) {
            finalizeCancel();
            return RenderStatus::Canceled;
        }
        return RenderStatus::NotReady;
    }
    if (cancelRequested_.load(std::memory_order_acquire)) {
        finalizeCancel();
        return RenderStatus::Canceled;
    }

    std::size_t sourceFrames = 0U;
    bool finalPadding = false;
    bool underrun = false;
    while (sourceFrames < frameCount) {
        if (cancelRequested_.load(std::memory_order_acquire)) {
            finalizeCancel();
            return RenderStatus::Canceled;
        }
        if (!currentBatch_) {
            RenderStatus nextStatus = RenderStatus::Timeout;
            if (!acquireBatch(timeoutMilliseconds, &nextStatus)) {
                if (nextStatus == RenderStatus::Canceled || nextStatus == RenderStatus::Fail) {
                    return nextStatus;
                }
                if (nextStatus == RenderStatus::EndOfStream) {
                    finalPadding = true;
                    break;
                }
                underrun = true;
                break;
            }
        }
        const std::size_t available = currentBatch_->lfe.size() - currentOffset_;
        const std::size_t count = std::min(frameCount - sourceFrames, available);
        for (unsigned object = 0U; object < eac3gate7b::kDynamicObjectCount; ++object) {
            std::copy_n(currentBatch_->objects[object].begin()
                            + static_cast<std::ptrdiff_t>(currentOffset_),
                        count, staged.objects[object].begin()
                            + static_cast<std::ptrdiff_t>(sourceFrames));
        }
        std::copy_n(currentBatch_->lfe.begin()
                        + static_cast<std::ptrdiff_t>(currentOffset_),
                    count, staged.lfe.begin()
                        + static_cast<std::ptrdiff_t>(sourceFrames));
        currentOffset_ += count;
        sourceFrames += count;
        activeQuantumSourceFrames_ = sourceFrames;
        if (cancelRequested_.load(std::memory_order_acquire)) {
            finalizeCancel();
            return RenderStatus::Canceled;
        }
        if (count > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())
            || nextSourcePosition_ > std::numeric_limits<std::int64_t>::max()
                - static_cast<std::int64_t>(count)) {
            if (cancelRequested_.load(std::memory_order_acquire)) {
                finalizeCancel();
                return RenderStatus::Canceled;
            }
            ++metrics_.timelineFailures;
            fail("source-timeline-overflow");
            return RenderStatus::Fail;
        }
        nextSourcePosition_ += static_cast<std::int64_t>(count);
        if (currentOffset_ == currentBatch_->lfe.size()) {
            currentBatch_.reset();
            currentOffset_ = 0U;
        }
    }
    if (sourceFrames < frameCount && !finalPadding && underrun) {
        staged.underrunFrames = frameCount - sourceFrames;
    } else if (sourceFrames < frameCount && finalPadding) {
        staged.finalPaddingFrames = frameCount - sourceFrames;
    }
    if (cancelRequested_.load(std::memory_order_acquire)) {
        finalizeCancel();
        return RenderStatus::Canceled;
    }
    if (!currentBatch_ && queue_.closedAndEmpty()) {
        staged.terminal = true;
        endOfStream_ = true;
        if (!pendingMetadata_.empty()) {
            metrics_.eosPendingMetadata += pendingMetadata_.size();
            pendingMetadata_.clear();
        }
    }
    staged.sourceFrames = sourceFrames;
    staged.sourceEnd = nextSourcePosition_;
    for (unsigned object = 0U; object < eac3gate7b::kDynamicObjectCount; ++object) {
        for (float value : staged.objects[object]) {
            ++metrics_.finiteSamples[object + 1U];
            metrics_.peaks[object + 1U] = std::max(metrics_.peaks[object + 1U],
                                                  std::abs(value));
        }
    }
    for (float value : staged.lfe) {
        ++metrics_.lfeFiniteSamples;
        metrics_.lfePeak = std::max(metrics_.lfePeak, std::abs(value));
    }
    metrics_.sourceFrames += staged.sourceFrames;
    metrics_.finalPaddingFrames += staged.finalPaddingFrames;
    metrics_.underrunFrames += staged.underrunFrames;
    if (staged.underrunFrames != 0U) ++metrics_.underrunPasses;
    metrics_.metadataApplied += staged.metadataApplied;
    metrics_.metadataDeferred += staged.metadataDeferred;
    metrics_.maximumPropertyLateness = std::max(
        metrics_.maximumPropertyLateness, staged.maximumPropertyLateness);
    ++metrics_.quanta;
    if (staged.terminal) ++metrics_.terminalQuanta;
    activeQuantumSourceFrames_ = 0U;
    *quantum = std::move(staged);
    return RenderStatus::Pass;
}

namespace {

eac3gate6c::Batch testBatch(std::int64_t start, std::size_t samples,
                            bool flush = false)
{
    eac3gate6c::Batch batch;
    batch.flush = flush;
    batch.outputStart = start;
    batch.outputEnd = 0;
    if (!safeEnd(start, samples, &batch.outputEnd)) batch.outputEnd = start;
    batch.objects.assign(eac3gate6c::kDynamicObjectCount,
                         std::vector<float>(samples, 0.0f));
    batch.lfe.assign(samples, 0.0f);
    for (unsigned object = 0U; object < eac3gate6c::kDynamicObjectCount; ++object) {
        for (std::size_t frame = 0U; frame < samples; ++frame) {
            batch.objects[object][frame] = static_cast<float>(
                (object + 1U) * 1000U + static_cast<unsigned>(frame));
        }
    }
    for (std::size_t frame = 0U; frame < samples; ++frame) {
        batch.lfe[frame] = static_cast<float>(5000U + static_cast<unsigned>(frame));
    }
    return batch;
}

eac3gate6c::MetadataUpdate testMetadata(unsigned object, std::int64_t position)
{
    eac3gate6c::MetadataUpdate update;
    update.objectIndex = object;
    update.sourcePosition = position;
    update.state.objectIndex = object;
    update.state.active = true;
    update.state.basicValid = true;
    update.state.renderValid = true;
    update.state.gainMinusInfinity = false;
    update.state.gainDb = 0.0f;
    update.state.position.valid = true;
    update.state.position.x = 0.5f;
    update.state.position.y = 0.5f;
    update.state.position.z = 0.0f;
    update.state.zoneConstraints = {true, true, true, true, true, true};
    update.state.elevation = true;
    return update;
}

} // namespace

SelfTestReport runSelfTest()
{
    SelfTestReport report;
    auto check = [&report](bool pass, const char *reason) {
        ++report.cases;
        if (!pass && report.reason.empty()) report.reason = reason;
    };
    auto waitFor = [](auto predicate) {
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(1000);
        while (!predicate() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        return predicate();
    };
    auto firstBatch = [](std::int64_t start, std::size_t samples) {
        eac3gate6c::Batch batch = testBatch(start, samples);
        for (unsigned object = 1U; object <= eac3gate7b::kDynamicObjectCount; ++object) {
            batch.metadata.push_back(testMetadata(object, start));
        }
        return batch;
    };
    auto makeMetadataBatch = [&](std::size_t updateCount) {
        eac3gate6c::Batch batch = testBatch(0, 10);
        for (std::size_t index = 0U; index < updateCount; ++index) {
            const unsigned object = static_cast<unsigned>(index % 15U) + 1U;
            const std::int64_t position = static_cast<std::int64_t>(index / 15U);
            batch.metadata.push_back(testMetadata(object, position));
        }
        return batch;
    };

    BoundedBatchQueue initialProperties(1U);
    constexpr std::uint32_t allInitialProperties =
        (1U << eac3gate7b::kDynamicObjectCount) - 1U;
    check(initialProperties.push(firstBatch(0, 2), 1U, 0U) == QueueStatus::Pass
              && initialProperties.metrics().initialPropertyMask == allInitialProperties,
          "initial-property-readiness-mask");

    BoundedBatchQueue fifo(2U);
    eac3gate6c::Batch one = testBatch(0, 2);
    eac3gate6c::Batch two = testBatch(2, 2);
    eac3gate6c::Batch three = testBatch(4, 2);
    check(fifo.push(one, 1U, 0U) == QueueStatus::Pass
              && fifo.push(two, 1U, 0U) == QueueStatus::Pass
              && fifo.metrics().maximumDepth == 2U
              && fifo.push(three, 1U, 0U) == QueueStatus::Timeout,
          "capacity-two-fifo-and-full-timeout");
    eac3gate6c::Batch popped;
    check(fifo.pop(&popped, 0U) == QueueStatus::Pass
              && popped.outputStart == 0
              && fifo.push(three, 1U, 0U) == QueueStatus::Pass
              && fifo.pop(&popped, 0U) == QueueStatus::Pass
              && popped.outputStart == 2,
          "fifo-order-and-recovery");
    BoundedBatchQueue empty(1U);
    check(empty.pop(&popped, 0U) == QueueStatus::Timeout, "empty-consumer-timeout");
    empty.push(testBatch(0, 1), 1U, 0U);
    empty.close();
    check(empty.pop(&popped, 0U) == QueueStatus::Pass
              && empty.pop(&popped, 0U) == QueueStatus::Closed,
          "close-and-drain");
    BoundedBatchQueue firstAdmission(1U);
    check(firstAdmission.push(testBatch(1, 1), 1U, 0U) == QueueStatus::Rejected
              && firstAdmission.push(testBatch(0, 1), 1U, 0U) == QueueStatus::Pass,
          "first-admission-must-start-at-zero");
    BoundedBatchQueue capacityOne(1U);
    check(capacityOne.push(testBatch(0, 1), 1U, 0U) == QueueStatus::Pass
              && capacityOne.pop(&popped, 0U) == QueueStatus::Pass
              && popped.outputStart == 0
              && capacityOne.push(testBatch(1, 1), 1U, 0U) == QueueStatus::Pass
              && capacityOne.pop(&popped, 0U) == QueueStatus::Pass
              && popped.outputStart == 1,
          "capacity-one-fifo");
    BoundedBatchQueue canceled(2U);
    canceled.push(testBatch(0, 1), 1U, 0U);
    canceled.push(testBatch(1, 1), 1U, 0U);
    canceled.cancel();
    check(canceled.pop(&popped, 0U) == QueueStatus::Canceled
              && canceled.metrics().canceledDiscarded == 2U,
          "cancel-discard-accounting");
    BoundedBatchQueue blocked(1U);
    QueueStatus blockedStatus = QueueStatus::Pass;
    std::thread waiter([&] {
        eac3gate6c::Batch ignored;
        blockedStatus = blocked.pop(&ignored, 2000U);
    });
    const bool consumerWaitObserved = waitFor([&] {
        return blocked.metrics().consumerWaits == 1U;
    });
    blocked.cancel();
    waiter.join();
    check(consumerWaitObserved && blockedStatus == QueueStatus::Canceled,
          "cancel-wakes-consumer");
    BoundedBatchQueue blockedProducer(1U);
    blockedProducer.push(testBatch(0, 1), 1U, 0U);
    QueueStatus producerStatus = QueueStatus::Pass;
    std::thread producerWaiter([&] {
        producerStatus = blockedProducer.push(testBatch(1, 1), 1U, 2000U);
    });
    const bool producerWaitObserved = waitFor([&] {
        return blockedProducer.metrics().producerWaits == 1U;
    });
    blockedProducer.cancel();
    producerWaiter.join();
    check(producerWaitObserved && producerStatus == QueueStatus::Canceled,
          "cancel-wakes-producer");
    BoundedBatchQueue closeWake(1U);
    QueueStatus closeStatus = QueueStatus::Pass;
    std::thread closeWaiter([&] {
        eac3gate6c::Batch ignored;
        closeStatus = closeWake.pop(&ignored, 2000U);
    });
    const bool closeConsumerWaitObserved = waitFor([&] {
        return closeWake.metrics().consumerWaits == 1U;
    });
    closeWake.close();
    closeWaiter.join();
    check(closeConsumerWaitObserved && closeStatus == QueueStatus::Closed,
          "close-wakes-consumer");
    BoundedBatchQueue closeProducer(1U);
    closeProducer.push(testBatch(0, 1), 1U, 0U);
    QueueStatus closeProducerStatus = QueueStatus::Pass;
    std::thread closeProducerWaiter([&] {
        closeProducerStatus = closeProducer.push(testBatch(1, 1), 1U, 2000U);
    });
    const bool closeProducerWaitObserved = waitFor([&] {
        return closeProducer.metrics().producerWaits == 1U;
    });
    closeProducer.close();
    closeProducerWaiter.join();
    check(closeProducerWaitObserved && closeProducerStatus == QueueStatus::Closed,
          "close-wakes-producer");
    SpatialBridgeCore concurrentCancelCore(1U);
    RenderStatus concurrentCancelStatus = RenderStatus::Fail;
    RenderQuantum concurrentCancelQuantum;
    std::thread blockedConsumer([&] {
        concurrentCancelStatus = concurrentCancelCore.renderQuantum(
            1U, 2000U, &concurrentCancelQuantum);
    });
    const bool bridgeConsumerWaitObserved = waitFor([&] {
        return concurrentCancelCore.queueMetrics().consumerWaits == 1U;
    });
    concurrentCancelCore.cancel();
    blockedConsumer.join();
    check(bridgeConsumerWaitObserved
              && concurrentCancelStatus == RenderStatus::Canceled
              && concurrentCancelCore.renderQuantum(1U, 0U,
                                                    &concurrentCancelQuantum)
                  == RenderStatus::Canceled,
          "concurrent-cancel-consumer-finalize");
    BoundedBatchQueue generations(1U);
    check(generations.push(testBatch(0, 1), 0U, 0U) == QueueStatus::Rejected
              && generations.push(testBatch(0, 1), 2U, 0U) == QueueStatus::Rejected,
          "zero-and-stale-generation");
    generations.close();
    check(generations.resetGeneration(2U)
              && generations.generation() == 2U,
          "reset-generation-fresh-boundary");

    std::string reason;
    eac3gate6c::Batch invalid = testBatch(0, 2);
    invalid.objects.pop_back();
    check(!validateBatch(invalid, &reason), "shape-rejection");
    invalid = testBatch(0, 2);
    invalid.objects[0][0] = std::numeric_limits<float>::quiet_NaN();
    check(!validateBatch(invalid, &reason), "finite-pcm-rejection");
    invalid = testBatch(std::numeric_limits<std::int64_t>::max(), 2);
    check(!validateBatch(invalid, &reason), "interval-overflow-rejection");
    invalid = testBatch(0, 2);
    invalid.metadata.push_back(testMetadata(1, 2));
    check(!validateBatch(invalid, &reason), "metadata-gap-rejection");
    invalid = testBatch(0, 2);
    invalid.metadata.push_back(testMetadata(2, 0));
    invalid.metadata.push_back(testMetadata(1, 0));
    check(!validateBatch(invalid, &reason), "metadata-overlap-order-rejection");
    BoundedBatchQueue transactionalQueue(2U);
    check(transactionalQueue.push(testBatch(0, 2), 1U, 0U) == QueueStatus::Pass,
          "transactional-queue-prime");
    const QueueMetrics beforeInvalidQueue = transactionalQueue.metrics();
    check(transactionalQueue.push(invalid, 1U, 0U) == QueueStatus::Rejected
              && transactionalQueue.metrics().pushes == beforeInvalidQueue.pushes
              && transactionalQueue.metrics().itemsRemaining
                  == beforeInvalidQueue.itemsRemaining,
          "invalid-item-whole-queue-rollback");
    eac3gate6c::Batch metadata135 = makeMetadataBatch(135U);
    eac3gate6c::Batch metadata136 = makeMetadataBatch(136U);
    check(validateBatch(metadata135, &reason)
              && !validateBatch(metadata136, &reason)
              && reason == "metadata-batch-too-large",
          "metadata-135-accepted-136-rejected");
    BoundedBatchQueue metadataQueue(1U);
    check(metadataQueue.push(std::move(metadata135), 1U, 0U) == QueueStatus::Pass,
          "metadata-135-queue-admission");
    BoundedBatchQueue flushQueue(2U);
    eac3gate6c::Batch flushBatch = testBatch(0, 1);
    flushBatch.flush = true;
    check(flushQueue.push(std::move(flushBatch), 1U, 0U) == QueueStatus::Pass
              && flushQueue.push(testBatch(1, 1), 1U, 0U) == QueueStatus::Rejected
              && flushQueue.metrics().flushRejects == 1U,
          "flush-final-admission");
    flushQueue.pop(&popped, 0U);
    flushQueue.close();
    check(flushQueue.resetGeneration(2U)
              && flushQueue.push(testBatch(0, 1), 2U, 0U) == QueueStatus::Pass,
          "flush-reset-clears-final-state");
    BoundedBatchQueue flushAttempt(1U);
    eac3gate6c::Batch timedFlush = testBatch(1, 1);
    timedFlush.flush = true;
    check(flushAttempt.push(testBatch(0, 1), 1U, 0U) == QueueStatus::Pass
              && flushAttempt.push(std::move(timedFlush), 1U, 0U)
                  == QueueStatus::Timeout
              && flushAttempt.pop(&popped, 0U) == QueueStatus::Pass
              && flushAttempt.push(testBatch(1, 1), 1U, 0U) == QueueStatus::Pass
              && flushAttempt.metrics().flushRejects == 0U,
          "flush-timeout-does-not-set-final-state");
    BoundedBatchQueue admissionQueue(3U);
    check(admissionQueue.push(testBatch(0, 2), 1U, 0U) == QueueStatus::Pass
              && admissionQueue.push(testBatch(3, 1), 1U, 0U) == QueueStatus::Rejected
              && admissionQueue.push(testBatch(2, 1), 1U, 0U) == QueueStatus::Pass
              && admissionQueue.push(testBatch(2, 1), 1U, 0U) == QueueStatus::Rejected
              && admissionQueue.metrics().admissionTimelineRejects == 2U,
          "admission-gap-overlap-rejection");
    BoundedBatchQueue timeoutAdmission(1U);
    check(timeoutAdmission.push(testBatch(0, 1), 1U, 0U) == QueueStatus::Pass
              && timeoutAdmission.push(testBatch(1, 1), 1U, 0U) == QueueStatus::Timeout
              && timeoutAdmission.pop(&popped, 0U) == QueueStatus::Pass
              && timeoutAdmission.push(testBatch(1, 1), 1U, 0U) == QueueStatus::Pass
              && timeoutAdmission.metrics().admissionTimelineRejects == 0U,
          "admission-timeout-does-not-advance");

    SpatialBridgeCore core(4U);
    eac3gate6c::Batch initial = firstBatch(0, 10);
    eac3gate6c::Batch next = testBatch(10, 10);
    check(core.push(initial) == QueueStatus::Pass
              && core.push(next) == QueueStatus::Pass,
          "core-admission");
    RenderQuantum quantum;
    check(core.renderQuantum(3U, 0U, &quantum) == RenderStatus::Pass
              && quantum.sourceFrames == 3U && quantum.objects.size() == 15U
              && quantum.objects[0][0] == 1000.0f
              && quantum.objects[0][2] == 1002.0f
              && quantum.objects[14][0] == 15000.0f
              && quantum.lfe[0] == 5000.0f,
          "split-quantum-identity");
    check(core.renderQuantum(17U, 0U, &quantum) == RenderStatus::Pass
              && quantum.sourceFrames == 17U
              && quantum.objects[0][0] == 1003.0f
              && quantum.objects[0][6] == 1009.0f
              && quantum.objects[0][7] == 1000.0f
              && quantum.objects[14][7] == 15000.0f
              && quantum.lfe[0] == 5003.0f,
          "coalesced-quantum");
    core.close();
    check(core.renderQuantum(1U, 0U, &quantum) == RenderStatus::EndOfStream,
          "clean-end-of-stream");

    SpatialBridgeCore notReadyCore(1U);
    check(notReadyCore.push(testBatch(0, 2)) == QueueStatus::Pass
              && notReadyCore.renderQuantum(2U, 0U, &quantum) == RenderStatus::NotReady,
          "initial-property-readiness");

    SpatialBridgeCore paddingCore(1U);
    check(paddingCore.push(firstBatch(0, 4)) == QueueStatus::Pass,
          "padding-admission");
    paddingCore.close();
    check(paddingCore.renderQuantum(6U, 0U, &quantum) == RenderStatus::Pass
              && quantum.sourceFrames == 4U
              && quantum.finalPaddingFrames == 2U
              && paddingCore.sourceTimeline() == 4,
          "final-partial-quantum-padding");

    SpatialBridgeCore exactCore(1U);
    check(exactCore.push(firstBatch(0, 4)) == QueueStatus::Pass,
          "exact-quantum-admission");
    exactCore.close();
    check(exactCore.renderQuantum(4U, 0U, &quantum) == RenderStatus::Pass
              && quantum.sourceFrames == 4U && quantum.finalPaddingFrames == 0U
              && exactCore.metrics().finalPaddingFrames == 0U
              && exactCore.renderQuantum(1U, 0U, &quantum) == RenderStatus::EndOfStream,
          "exact-quantum-end-of-stream");
    SpatialBridgeCore boundsCore(1U);
    RenderQuantum boundsSentinel;
    boundsSentinel.sourceStart = 123;
    boundsSentinel.sourceEnd = 456;
    boundsSentinel.sourceFrames = 7U;
    check(boundsCore.renderQuantum(kMaxRenderFrames + 1U, 0U, &boundsSentinel)
              == RenderStatus::Fail
              && boundsSentinel.sourceStart == 123
              && boundsSentinel.sourceEnd == 456
              && boundsSentinel.sourceFrames == 7U,
          "render-frame-bound-and-caller-rollback");

    SpatialBridgeCore cancelCore(1U);
    eac3gate6c::Batch cancelBatch = firstBatch(0, 4);
    cancelBatch.metadata.push_back(testMetadata(2, 3));
    check(cancelCore.push(cancelBatch) == QueueStatus::Pass,
          "cancel-admission");
    RenderQuantum cancelQuantum;
    check(cancelCore.renderQuantum(2U, 0U, &cancelQuantum) == RenderStatus::Pass
              && cancelQuantum.sourceFrames == 2U
              && cancelQuantum.metadataDeferred == 1U,
          "cancel-prime-current-and-pending");
    RenderQuantum sentinel;
    sentinel.sourceStart = 77;
    sentinel.sourceEnd = 88;
    sentinel.sourceFrames = 9U;
    cancelCore.cancel();
    const RenderStatus canceledStatus = cancelCore.renderQuantum(1U, 0U, &sentinel);
    const BridgeMetrics canceledMetrics = cancelCore.metrics();
    check(canceledMetrics.canceledCurrentFrames == 2U
              && canceledMetrics.canceledPendingMetadata == 1U
              && canceledStatus == RenderStatus::Canceled
              && sentinel.sourceStart == 77 && sentinel.sourceFrames == 9U,
          "cancel-clears-current-pending-and-preserves-caller");
    check(cancelCore.resetGeneration(2U)
              && cancelCore.metrics().quanta == 0U
              && cancelCore.sourceTimeline() == 0
              && cancelCore.push(firstBatch(0, 2)) == QueueStatus::Pass,
          "cancel-reset-fresh-equivalence");
    cancelCore.close();
    check(cancelCore.renderQuantum(2U, 0U, &sentinel) == RenderStatus::Pass
              && sentinel.sourceFrames == 2U,
          "reset-fresh-behavior");

    SpatialBridgeCore metadataCore(4U);
    eac3gate6c::Batch metadataBatch = firstBatch(0, 16);
    metadataBatch.metadata.push_back(testMetadata(1, 5));
    metadataBatch.metadata.back().rampDuration = 10U;
    metadataBatch.metadata.back().state.position.x = 1.0f;
    metadataBatch.metadata.push_back(testMetadata(2, 8));
    metadataBatch.flush = true;
    check(metadataCore.push(metadataBatch) == QueueStatus::Pass,
          "flush-carried-metadata-admission");
    check(metadataCore.renderQuantum(8U, 0U, &quantum) == RenderStatus::Pass
              && quantum.metadataApplied == 15U
              && quantum.metadataDeferred == 2U,
          "initial-and-in-quantum-metadata-causality");
    check(metadataCore.renderQuantum(8U, 0U, &quantum) == RenderStatus::Pass
              && quantum.metadataApplied == 2U
              && quantum.maximumPropertyLateness == 3
              && !quantum.properties.empty()
              && std::abs(quantum.properties[0].position[0] - 1.5f) < 0.0001f,
          "deferred-metadata-lateness-and-ramp-catchup");
    metadataCore.close();

    SpatialBridgeCore metadataTailCore(1U);
    eac3gate6c::Batch metadataTail = firstBatch(0, 4);
    metadataTail.metadata.push_back(testMetadata(1, 3));
    check(metadataTailCore.push(metadataTail) == QueueStatus::Pass,
          "eos-pending-metadata-admission");
    metadataTailCore.close();
    check(metadataTailCore.renderQuantum(6U, 0U, &quantum) == RenderStatus::Pass
              && quantum.finalPaddingFrames == 2U
              && quantum.metadataDeferred == 1U
              && metadataTailCore.renderQuantum(1U, 0U, &quantum)
                  == RenderStatus::EndOfStream
              && metadataTailCore.metrics().eosPendingMetadata == 1U,
          "eos-pending-metadata-accounted");

    SpatialBridgeCore underrun(2U);
    check(underrun.push(firstBatch(0, 4)) == QueueStatus::Pass,
          "underrun-admission");
    check(underrun.renderQuantum(4U, 0U, &quantum) == RenderStatus::Pass
              && underrun.sourceTimeline() == 4,
          "underrun-prime");
    check(underrun.renderQuantum(4U, 0U, &quantum) == RenderStatus::Pass
              && quantum.underrunFrames == 4U
              && underrun.sourceTimeline() == 4,
          "underrun-does-not-advance-source");
    underrun.close();
    check(underrun.resetGeneration(2U)
              && underrun.sourceTimeline() == 0,
          "core-reset-fresh-equivalence");
    report.pass = report.reason.empty();
    return report;
}

} // namespace eac3bridge
