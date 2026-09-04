#include "audioartifactmonitor.h"

#include "playerlogger.h"

#include <QtEndian>

#include <QStringList>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace {

constexpr double kAdjacentJumpThreshold = 0.68;
constexpr double kBoundaryJumpThreshold = 0.45;
constexpr double kTransientPeakThreshold = 0.68;
constexpr double kTransientNeighborThreshold = 0.24;
constexpr double kSilentPeakThreshold = 0.003;
constexpr double kSilentRmsThreshold = 0.001;
constexpr double kHardSwitchOpeningPeakThreshold = 0.12;
constexpr double kHardSwitchJumpThreshold = 0.14;
constexpr int kHardSwitchOpeningWindowMs = 3;
constexpr int kShortBurstWindowMs = 2;
constexpr double kShortBurstPeakThreshold = 0.45;
constexpr double kShortBurstRmsThreshold = 0.10;
constexpr double kShortBurstRmsRatio = 2.0;
constexpr double kSensitiveShortBurstPeakThreshold = 0.10;
constexpr double kSensitiveShortBurstRmsThreshold = 0.018;
constexpr double kSensitiveShortBurstRmsRatio = 1.5;
constexpr int kCrackleWindowMs = 3;
constexpr double kCrackleJumpThreshold = 0.24;
constexpr double kCrackleAverageJumpThreshold = 0.30;
constexpr double kSensitiveCrackleJumpThreshold = 0.10;
constexpr double kSensitiveCrackleAverageJumpThreshold = 0.14;
constexpr qint64 kArtifactRateWindowMs = 60000;
constexpr qint64 kDuplicateArtifactLogIntervalMs = 1000;

qint32 readInt24Sample(const char *sampleData)
{
    const quint32 b0 = static_cast<quint8>(sampleData[0]);
    const quint32 b1 = static_cast<quint8>(sampleData[1]);
    const quint32 b2 = static_cast<quint8>(sampleData[2]);
    quint32 value = b0 | (b1 << 8) | (b2 << 16);
    if (value & 0x00800000u) {
        value |= 0xFF000000u;
    }
    return static_cast<qint32>(value);
}

bool sameMonitorFormat(const PcmStreamFormat &lhs, const PcmStreamFormat &rhs)
{
    return lhs.sampleRate == rhs.sampleRate
        && lhs.channelCount == rhs.channelCount
        && lhs.sampleEncoding == rhs.sampleEncoding
        && lhs.validBitsPerSample == rhs.validBitsPerSample
        && lhs.bytesPerFrame() == rhs.bytesPerFrame();
}

QString severityForJump(double jumpMagnitude)
{
    if (jumpMagnitude >= 1.50) {
        return QStringLiteral("critical");
    }
    if (jumpMagnitude >= 1.10) {
        return QStringLiteral("high");
    }
    return QStringLiteral("medium");
}

QString severityForHardSwitch(double openingPeak, double jumpMagnitude)
{
    if (openingPeak >= 0.65 || jumpMagnitude >= 0.85) {
        return QStringLiteral("high");
    }
    return QStringLiteral("medium");
}

QString severityForBurst(double peak, double rms)
{
    if (peak >= 0.60 || rms >= 0.16) {
        return QStringLiteral("high");
    }
    return QStringLiteral("medium");
}

QString severityForCrackle(double maxJump, double averageJump)
{
    if (maxJump >= 0.45 || averageJump >= 0.30) {
        return QStringLiteral("high");
    }
    return QStringLiteral("medium");
}

QString formatNumber(double value)
{
    if (!std::isfinite(value)) {
        return QStringLiteral("nan");
    }
    return QString::number(value, 'f', 4);
}

QString logValue(QString value)
{
    value = value.trimmed();
    value.replace(QChar('\r'), QChar(' '));
    value.replace(QChar('\n'), QChar(' '));
    return value.isEmpty() ? QStringLiteral("none") : value;
}

} // namespace

void AudioArtifactMonitor::resetContinuity(const QString &reason)
{
    flushSuppressedArtifactLogs(reason.isEmpty() ? QStringLiteral("resetContinuity") : reason);

    if (m_hasPreviousBlock || !m_previousSamples.isEmpty()) {
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("audioArtifactMonitor resetContinuity reason=%1 artifactCountTotal=%2")
                              .arg(reason.isEmpty() ? QStringLiteral("unspecified") : reason)
                              .arg(m_artifactCountTotal));
    }

    m_previousFormat = {};
    m_previousSamples.clear();
    m_previousTailWindow = {};
    m_hasPreviousBlock = false;
    m_previousBlockSilent = true;
    m_artifactLogStates.clear();
}

void AudioArtifactMonitor::analyzePcmBlock(const char *data,
                                           qsizetype byteCount,
                                           const PcmStreamFormat &format,
                                           const PlaybackContext &context,
                                           const QString &renderSource,
                                           const RenderContext &renderContext)
{
    if (!data || byteCount <= 0 || !format.isValid()) {
        return;
    }

    const int bytesPerFrame = format.bytesPerFrame();
    const int bytesPerSample = format.bytesPerSample();
    const int channelCount = format.channelCount;
    if (bytesPerFrame <= 0 || bytesPerSample <= 0 || channelCount <= 0) {
        return;
    }

    const qsizetype frameCount = byteCount / bytesPerFrame;
    if (frameCount <= 0) {
        return;
    }

    ensureFormatState(format);

    BlockMetrics metrics;
    metrics.frameCount = frameCount;
    metrics.previousTailSamples = m_hasPreviousBlock ? m_previousSamples : QVector<double>();
    if (m_hasPreviousBlock) {
        metrics.previousWindowPeak = m_previousTailWindow.peak;
        metrics.previousWindowRms = m_previousTailWindow.rms;
    }
    metrics.currentFirstSamples = QVector<double>(channelCount, 0.0);
    metrics.currentLastSamples = QVector<double>(channelCount, 0.0);
    const qsizetype openingFrames = qMin<qsizetype>(
        frameCount,
        qMax<qsizetype>(1, static_cast<qsizetype>(format.sampleRate) * kHardSwitchOpeningWindowMs / 1000));
    QVector<double> previousInChannel = m_previousSamples;
    double sumSquares = 0.0;
    double openingSumSquares = 0.0;
    qsizetype openingSampleCount = 0;
    double tailSumSquares = 0.0;
    qsizetype tailSampleCount = 0;
    WindowMetrics currentTailWindow;

    for (qsizetype frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
        const char *frameData = data + frameIndex * bytesPerFrame;
        for (int channel = 0; channel < channelCount; ++channel) {
            const SampleRead sample = readSample(frameData + channel * bytesPerSample, format);
            if (!sample.finite) {
                metrics.hasInvalidSample = true;
                continue;
            }
            if (sample.outOfRange) {
                metrics.hasOutOfRangeSample = true;
            }

            const double magnitude = std::abs(sample.value);
            metrics.peak = qMax(metrics.peak, magnitude);
            sumSquares += sample.value * sample.value;
            ++metrics.sampleCount;

            if (frameIndex == 0) {
                metrics.currentFirstSamples[channel] = sample.value;
            }
            metrics.currentLastSamples[channel] = sample.value;

            if (frameIndex < openingFrames) {
                metrics.openingPeak = qMax(metrics.openingPeak, magnitude);
                metrics.currentWindowPeak = qMax(metrics.currentWindowPeak, magnitude);
                openingSumSquares += sample.value * sample.value;
                ++openingSampleCount;
            }

            if (frameIndex + openingFrames >= frameCount) {
                currentTailWindow.peak = qMax(currentTailWindow.peak, magnitude);
                tailSumSquares += sample.value * sample.value;
                ++tailSampleCount;
            }

            if (frameIndex == 0) {
                if (m_hasPreviousBlock && channel < m_previousSamples.size()) {
                    metrics.maxBoundaryJump =
                        qMax(metrics.maxBoundaryJump, std::abs(sample.value - m_previousSamples.at(channel)));
                }
            } else {
                metrics.maxJump =
                    qMax(metrics.maxJump, std::abs(sample.value - previousInChannel.at(channel)));
            }
            previousInChannel[channel] = sample.value;
        }
    }

    metrics.rms = metrics.sampleCount > 0 ? std::sqrt(sumSquares / metrics.sampleCount) : 0.0;
    metrics.currentWindowRms = openingSampleCount > 0 ? std::sqrt(openingSumSquares / openingSampleCount) : 0.0;
    currentTailWindow.sampleCount = tailSampleCount;
    currentTailWindow.rms = tailSampleCount > 0 ? std::sqrt(tailSumSquares / tailSampleCount) : 0.0;

    if (metrics.hasInvalidSample) {
        logArtifact(QStringLiteral("invalid-sample"),
                    QStringLiteral("critical"),
                    metrics.maxJump,
                    metrics,
                    format,
                    context,
                    renderSource,
                    renderContext);
    }
    if (metrics.hasOutOfRangeSample) {
        logArtifact(QStringLiteral("out-of-range-sample"),
                    QStringLiteral("high"),
                    metrics.maxJump,
                    metrics,
                    format,
                    context,
                    renderSource,
                    renderContext);
    }
    if (metrics.maxBoundaryJump >= kBoundaryJumpThreshold) {
        logArtifact(QStringLiteral("block-boundary-discontinuity"),
                    severityForJump(metrics.maxBoundaryJump),
                    metrics.maxBoundaryJump,
                    metrics,
                    format,
                    context,
                    renderSource,
                    renderContext);
    }
    if (metrics.maxJump >= kAdjacentJumpThreshold) {
        logArtifact(QStringLiteral("sample-jump"),
                    severityForJump(metrics.maxJump),
                    metrics.maxJump,
                    metrics,
                    format,
                    context,
                    renderSource,
                    renderContext);
    }

    const double transientSpike = detectTransientSpike(data, frameCount, format);
    if (transientSpike >= kTransientPeakThreshold) {
        logArtifact(QStringLiteral("transient-spike"),
                    QStringLiteral("high"),
                    transientSpike,
                    metrics,
                    format,
                    context,
                    renderSource,
                    renderContext);
    }

    const bool sensitiveContext = renderContext.recovery
        || context.recoveryPending
        || (renderContext.firstDataBlockAfterConfigure
            && context.pipelineStartupProfile != QStringLiteral("NormalStart"))
        || renderContext.firstDataBlockAfterDeviceRebuild
        || context.deviceRebuildPending;
    const BurstScanResult shortBurst = detectShortBurst(data, frameCount, format, metrics.rms);
    const double burstPeakThreshold = sensitiveContext ? kSensitiveShortBurstPeakThreshold
                                                       : kShortBurstPeakThreshold;
    const double burstRmsThreshold = sensitiveContext ? kSensitiveShortBurstRmsThreshold
                                                      : kShortBurstRmsThreshold;
    const double burstRatioThreshold = sensitiveContext ? kSensitiveShortBurstRmsRatio
                                                        : kShortBurstRmsRatio;
    if (shortBurst.peak >= burstPeakThreshold
        && shortBurst.rms >= burstRmsThreshold
        && shortBurst.ratioToBlockRms >= burstRatioThreshold) {
        logArtifact(QStringLiteral("short-burst"),
                    severityForBurst(shortBurst.peak, shortBurst.rms),
                    shortBurst.peak,
                    metrics,
                    format,
                    context,
                    renderSource,
                    renderContext);
    }

    const CrackleScanResult crackle = detectCrackleTexture(data, frameCount, format, sensitiveContext);
    if (crackle.jumpCount > 0) {
        logArtifact(QStringLiteral("crackle-texture"),
                    severityForCrackle(crackle.maxJump, crackle.averageJump),
                    crackle.score,
                    metrics,
                    format,
                    context,
                    renderSource,
                    renderContext);
    }

    if (m_hasPreviousBlock
        && m_previousBlockSilent
        && metrics.openingPeak >= kHardSwitchOpeningPeakThreshold
        && metrics.maxBoundaryJump >= kHardSwitchJumpThreshold) {
        logArtifact(QStringLiteral("silence-hard-switch"),
                    severityForHardSwitch(metrics.openingPeak, metrics.maxBoundaryJump),
                    metrics.maxBoundaryJump,
                    metrics,
                    format,
                    context,
                    renderSource,
                    renderContext);
    }

    m_previousSamples = previousInChannel;
    m_previousTailWindow = currentTailWindow;
    m_hasPreviousBlock = true;
    m_previousBlockSilent = isSilentBlock(metrics);
}

void AudioArtifactMonitor::observeSilentFrames(qsizetype frameCount,
                                               const PcmStreamFormat &format,
                                               const PlaybackContext &context,
                                               const QString &renderSource,
                                               const RenderContext &renderContext)
{
    if (frameCount <= 0 || !format.isValid() || format.channelCount <= 0) {
        return;
    }

    ensureFormatState(format);

    BlockMetrics metrics;
    metrics.frameCount = frameCount;
    metrics.sampleCount = frameCount * format.channelCount;
    metrics.previousTailSamples = m_hasPreviousBlock ? m_previousSamples : QVector<double>();
    if (m_hasPreviousBlock) {
        metrics.previousWindowPeak = m_previousTailWindow.peak;
        metrics.previousWindowRms = m_previousTailWindow.rms;
    }
    metrics.currentFirstSamples = QVector<double>(format.channelCount, 0.0);
    metrics.currentLastSamples = QVector<double>(format.channelCount, 0.0);
    if (m_hasPreviousBlock) {
        for (double previousSample : std::as_const(m_previousSamples)) {
            metrics.maxBoundaryJump = qMax(metrics.maxBoundaryJump, std::abs(previousSample));
        }
    }

    if (metrics.maxBoundaryJump >= kBoundaryJumpThreshold) {
        logArtifact(QStringLiteral("block-boundary-discontinuity"),
                    severityForJump(metrics.maxBoundaryJump),
                    metrics.maxBoundaryJump,
                    metrics,
                    format,
                    context,
                    renderSource,
                    renderContext);
    }

    updatePreviousSamplesToSilence(format);
    m_previousTailWindow = { metrics.sampleCount, 0.0, 0.0 };
    m_hasPreviousBlock = true;
    m_previousBlockSilent = true;
}

quint64 AudioArtifactMonitor::artifactCountTotal() const
{
    return m_artifactCountTotal;
}

QHash<QString, quint64> AudioArtifactMonitor::artifactCountByType() const
{
    return m_artifactCountByType;
}

QHash<QString, quint64> AudioArtifactMonitor::artifactCountByRecentControlEvent() const
{
    return m_artifactCountByRecentControlEvent;
}

QHash<QString, quint64> AudioArtifactMonitor::artifactCountByPlaybackState() const
{
    return m_artifactCountByPlaybackState;
}

QHash<QString, quint64> AudioArtifactMonitor::artifactCountByDetectorAndSeverity() const
{
    return m_artifactCountByDetectorAndSeverity;
}

double AudioArtifactMonitor::artifactDensityPerMinute() const
{
    return static_cast<double>(m_recentArtifactTimesMs.size());
}

QDateTime AudioArtifactMonitor::lastArtifactTime() const
{
    return m_lastArtifactTime;
}

void AudioArtifactMonitor::ensureFormatState(const PcmStreamFormat &format)
{
    if (sameMonitorFormat(format, m_previousFormat) && m_previousSamples.size() == format.channelCount) {
        return;
    }

    m_previousFormat = format;
    m_previousSamples = QVector<double>(format.channelCount, 0.0);
    m_previousTailWindow = {};
    m_hasPreviousBlock = false;
    m_previousBlockSilent = true;
}

AudioArtifactMonitor::SampleRead AudioArtifactMonitor::readSample(const char *sampleData,
                                                                  const PcmStreamFormat &format) const
{
    SampleRead result;
    switch (format.sampleEncoding) {
    case PcmSampleEncoding::UInt8: {
        const auto sample = static_cast<quint8>(*reinterpret_cast<const unsigned char *>(sampleData));
        result.value = (static_cast<int>(sample) - 128) / 128.0;
        break;
    }
    case PcmSampleEncoding::Int16: {
        const qint16 sample = qFromLittleEndian<qint16>(sampleData);
        result.value = static_cast<double>(sample) / 32768.0;
        break;
    }
    case PcmSampleEncoding::Int24:
        result.value = static_cast<double>(readInt24Sample(sampleData)) / 8388608.0;
        break;
    case PcmSampleEncoding::Int32: {
        const qint32 sample = qFromLittleEndian<qint32>(sampleData);
        result.value = static_cast<double>(sample) / 2147483648.0;
        break;
    }
    case PcmSampleEncoding::Float32: {
        float sample = 0.0f;
        std::memcpy(&sample, sampleData, sizeof(sample));
        result.finite = std::isfinite(sample);
        result.outOfRange = result.finite && std::abs(static_cast<double>(sample)) > 1.0001;
        result.value = result.finite ? static_cast<double>(sample) : 0.0;
        break;
    }
    case PcmSampleEncoding::Unknown:
        result.finite = false;
        break;
    }

    return result;
}

double AudioArtifactMonitor::detectTransientSpike(const char *data,
                                                  qsizetype frameCount,
                                                  const PcmStreamFormat &format) const
{
    if (frameCount < 3 || !format.isValid()) {
        return 0.0;
    }

    const int bytesPerFrame = format.bytesPerFrame();
    const int bytesPerSample = format.bytesPerSample();
    double maxSpike = 0.0;
    for (qsizetype frameIndex = 1; frameIndex + 1 < frameCount; ++frameIndex) {
        const char *previousFrame = data + (frameIndex - 1) * bytesPerFrame;
        const char *currentFrame = data + frameIndex * bytesPerFrame;
        const char *nextFrame = data + (frameIndex + 1) * bytesPerFrame;
        for (int channel = 0; channel < format.channelCount; ++channel) {
            const int offset = channel * bytesPerSample;
            const SampleRead previous = readSample(previousFrame + offset, format);
            const SampleRead current = readSample(currentFrame + offset, format);
            const SampleRead next = readSample(nextFrame + offset, format);
            if (!previous.finite || !current.finite || !next.finite) {
                continue;
            }

            const double previousMagnitude = std::abs(previous.value);
            const double currentMagnitude = std::abs(current.value);
            const double nextMagnitude = std::abs(next.value);
            if (currentMagnitude >= kTransientPeakThreshold
                && previousMagnitude <= kTransientNeighborThreshold
                && nextMagnitude <= kTransientNeighborThreshold) {
                maxSpike = qMax(maxSpike, currentMagnitude);
            }
        }
    }

    return maxSpike;
}

AudioArtifactMonitor::BurstScanResult
AudioArtifactMonitor::detectShortBurst(const char *data,
                                       qsizetype frameCount,
                                       const PcmStreamFormat &format,
                                       double blockRms) const
{
    BurstScanResult result;
    if (!data || frameCount <= 0 || !format.isValid() || format.sampleRate <= 0) {
        return result;
    }

    const int bytesPerFrame = format.bytesPerFrame();
    const int bytesPerSample = format.bytesPerSample();
    if (bytesPerFrame <= 0 || bytesPerSample <= 0 || format.channelCount <= 0) {
        return result;
    }

    const qsizetype windowFrames = qMin<qsizetype>(
        frameCount,
        qMax<qsizetype>(3, static_cast<qsizetype>(format.sampleRate) * kShortBurstWindowMs / 1000));
    const qsizetype stepFrames = qMax<qsizetype>(1, windowFrames / 2);
    const double rmsFloor = 0.004;
    for (qsizetype startFrame = 0; startFrame < frameCount; startFrame += stepFrames) {
        const qsizetype endFrame = qMin(frameCount, startFrame + windowFrames);
        double peak = 0.0;
        double sumSquares = 0.0;
        qsizetype sampleCount = 0;
        for (qsizetype frameIndex = startFrame; frameIndex < endFrame; ++frameIndex) {
            const char *frameData = data + frameIndex * bytesPerFrame;
            for (int channel = 0; channel < format.channelCount; ++channel) {
                const SampleRead sample = readSample(frameData + channel * bytesPerSample, format);
                if (!sample.finite) {
                    continue;
                }

                peak = qMax(peak, std::abs(sample.value));
                sumSquares += sample.value * sample.value;
                ++sampleCount;
            }
        }

        if (sampleCount == 0) {
            continue;
        }

        const double rms = std::sqrt(sumSquares / sampleCount);
        const double ratioToBlockRms = rms / qMax(blockRms, rmsFloor);
        if (rms > result.rms || (qFuzzyCompare(rms, result.rms) && peak > result.peak)) {
            result.peak = peak;
            result.rms = rms;
            result.ratioToBlockRms = ratioToBlockRms;
        }
    }

    return result;
}

AudioArtifactMonitor::CrackleScanResult
AudioArtifactMonitor::detectCrackleTexture(const char *data,
                                           qsizetype frameCount,
                                           const PcmStreamFormat &format,
                                           bool sensitiveContext) const
{
    CrackleScanResult result;
    if (!data || frameCount < 3 || !format.isValid() || format.sampleRate <= 0) {
        return result;
    }

    const int bytesPerFrame = format.bytesPerFrame();
    const int bytesPerSample = format.bytesPerSample();
    if (bytesPerFrame <= 0 || bytesPerSample <= 0 || format.channelCount <= 0) {
        return result;
    }

    const double jumpThreshold = sensitiveContext ? kSensitiveCrackleJumpThreshold : kCrackleJumpThreshold;
    const double averageJumpThreshold = sensitiveContext ? kSensitiveCrackleAverageJumpThreshold
                                                         : kCrackleAverageJumpThreshold;
    const qsizetype windowFrames = qMin<qsizetype>(
        frameCount,
        qMax<qsizetype>(3, static_cast<qsizetype>(format.sampleRate) * kCrackleWindowMs / 1000));
    const qsizetype stepFrames = qMax<qsizetype>(1, windowFrames / 2);
    const int minJumpCount = qMax(4, format.channelCount * 3);

    for (qsizetype startFrame = 0; startFrame < frameCount; startFrame += stepFrames) {
        const qsizetype endFrame = qMin(frameCount, startFrame + windowFrames);
        int jumpCount = 0;
        double jumpSum = 0.0;
        double maxJump = 0.0;
        for (qsizetype frameIndex = qMax<qsizetype>(1, startFrame); frameIndex < endFrame; ++frameIndex) {
            const char *previousFrame = data + (frameIndex - 1) * bytesPerFrame;
            const char *currentFrame = data + frameIndex * bytesPerFrame;
            for (int channel = 0; channel < format.channelCount; ++channel) {
                const int offset = channel * bytesPerSample;
                const SampleRead previous = readSample(previousFrame + offset, format);
                const SampleRead current = readSample(currentFrame + offset, format);
                if (!previous.finite || !current.finite) {
                    continue;
                }

                const double jump = std::abs(current.value - previous.value);
                if (jump < jumpThreshold) {
                    continue;
                }

                ++jumpCount;
                jumpSum += jump;
                maxJump = qMax(maxJump, jump);
            }
        }

        if (jumpCount < minJumpCount) {
            continue;
        }

        const double averageJump = jumpSum / jumpCount;
        if (averageJump < averageJumpThreshold) {
            continue;
        }

        const double score = averageJump * std::sqrt(static_cast<double>(jumpCount));
        if (score > result.score) {
            result.score = score;
            result.averageJump = averageJump;
            result.maxJump = maxJump;
            result.jumpCount = jumpCount;
        }
    }

    return result;
}

void AudioArtifactMonitor::updatePreviousSamplesToSilence(const PcmStreamFormat &format)
{
    if (m_previousSamples.size() != format.channelCount) {
        m_previousSamples = QVector<double>(format.channelCount, 0.0);
        return;
    }

    std::fill(m_previousSamples.begin(), m_previousSamples.end(), 0.0);
}

bool AudioArtifactMonitor::isSilentBlock(const BlockMetrics &metrics) const
{
    return metrics.peak <= kSilentPeakThreshold && metrics.rms <= kSilentRmsThreshold;
}

void AudioArtifactMonitor::logArtifact(const QString &detectorType,
                                       const QString &severity,
                                       double jumpMagnitude,
                                       const BlockMetrics &metrics,
                                       const PcmStreamFormat &format,
                                       const PlaybackContext &context,
                                       const QString &renderSource,
                                       const RenderContext &renderContext)
{
    const QString recentControlEventKey = controlEventKey(context.recentControlEvent);
    const QString playbackStateKey = logValue(context.playbackState);
    const QString detectorSeverityKey = QStringLiteral("%1.%2").arg(detectorType, severity);

    ++m_artifactCountTotal;
    ++m_artifactCountByType[detectorType];
    ++m_artifactCountByRecentControlEvent[recentControlEventKey];
    ++m_artifactCountByPlaybackState[playbackStateKey];
    ++m_artifactCountByDetectorAndSeverity[detectorSeverityKey];
    m_lastArtifactTime = QDateTime::currentDateTime();
    const qint64 nowMs = m_lastArtifactTime.toMSecsSinceEpoch();
    m_recentArtifactTimesMs.append(nowMs);
    pruneRecentArtifactTimes(nowMs);

    const QString pipelineStartupProfile = logValue(context.pipelineStartupProfile);
    const QString artifactPath = logValue(context.artifactPath);
    const QString dedupeKey = QStringLiteral("%1|%2|%3|%4|%5|%6|%7|%8|%9|%10")
                                  .arg(detectorType)
                                  .arg(severity)
                                  .arg(playbackStateKey)
                                  .arg(recentControlEventKey)
                                  .arg(context.sessionId)
                                  .arg(context.outputGeneration)
                                  .arg(context.bufferGeneration)
                                  .arg(logValue(renderSource))
                                  .arg(pipelineStartupProfile)
                                  .arg(artifactPath);
    ArtifactLogState &logState = m_artifactLogStates[dedupeKey];
    if (logState.lastLoggedMs > 0 && nowMs - logState.lastLoggedMs < kDuplicateArtifactLogIntervalMs) {
        ++logState.suppressedCount;
        return;
    }

    const quint64 suppressedDuplicateArtifacts = logState.suppressedCount;
    logState.lastLoggedMs = nowMs;
    logState.suppressedCount = 0;

    const qreal audioLevelPeak = qMax(context.audioLevelLeft, context.audioLevelRight);
    const bool recovery = renderContext.recovery || context.recoveryPending;
    const bool firstDataBlockAfterDeviceRebuild =
        renderContext.firstDataBlockAfterDeviceRebuild
        || (renderContext.firstDataBlockAfterConfigure && context.deviceRebuildPending);
    PlayerLogger::log(QStringLiteral("anomaly"),
                      QStringLiteral("audioArtifact timestamp=%1 session=%2 source=%3 playbackState=%4 recentControlEvent=%5 sampleRate=%6 channels=%7 peak=%8 rms=%9 jump=%10 detector=%11 severity=%12 artifactCountTotal=%13 artifactCountByType=%14 artifactCountByRecentControlEvent=%15 artifactCountByPlaybackState=%16 artifactCountByDetectorAndSeverity=%17 artifactDensityPerMinute=%18 lastArtifactTime=%19 outputGeneration=%20 bufferGeneration=%21 positionMs=%22 audioLevelPeak=%23 audioLevelLeft=%24 audioLevelRight=%25 renderSource=%26 frames=%27 writeFrames=%28 wasapiPaddingFrames=%29 wasapiAvailableFrames=%30 previousBlockTailSample=%31 currentBlockFirstSample=%32 currentBlockLastSample=%33 previousWindowPeak=%34 previousWindowRms=%35 currentWindowPeak=%36 currentWindowRms=%37 warmup=%38 silenceFill=%39 recovery=%40 recoveryPending=%41 recoveryAttempt=%42 deviceRebuildPending=%43 firstDataBlockAfterConfigure=%44 firstDataBlockAfterDeviceRebuild=%45 suppressedDuplicateArtifacts=%46 pipelineStartProfile=%47 artifactPath=%48 activeSwitchTrigger=%49 activeSwitchPhase=%50 activeSwitchReason=%51")
                          .arg(m_lastArtifactTime.toString(Qt::ISODateWithMs))
                          .arg(context.sessionId)
                          .arg(logValue(context.source))
                          .arg(playbackStateKey)
                          .arg(logValue(context.recentControlEvent))
                          .arg(format.sampleRate)
                          .arg(format.channelCount)
                          .arg(formatNumber(metrics.peak))
                          .arg(formatNumber(metrics.rms))
                          .arg(formatNumber(jumpMagnitude))
                          .arg(detectorType)
                          .arg(severity)
                          .arg(m_artifactCountTotal)
                          .arg(artifactCountsText())
                          .arg(countsText(m_artifactCountByRecentControlEvent))
                          .arg(countsText(m_artifactCountByPlaybackState))
                          .arg(countsText(m_artifactCountByDetectorAndSeverity))
                          .arg(formatNumber(artifactDensityPerMinute()))
                          .arg(m_lastArtifactTime.toString(Qt::ISODateWithMs))
                          .arg(context.outputGeneration)
                          .arg(context.bufferGeneration)
                          .arg(context.positionMs)
                          .arg(formatNumber(audioLevelPeak))
                          .arg(formatNumber(context.audioLevelLeft))
                          .arg(formatNumber(context.audioLevelRight))
                          .arg(logValue(renderSource))
                          .arg(metrics.frameCount)
                          .arg(renderContext.writeFrameCount)
                          .arg(renderContext.wasapiPaddingFrames)
                          .arg(renderContext.wasapiAvailableFrames)
                          .arg(samplesText(metrics.previousTailSamples))
                          .arg(samplesText(metrics.currentFirstSamples))
                          .arg(samplesText(metrics.currentLastSamples))
                          .arg(formatNumber(metrics.previousWindowPeak))
                          .arg(formatNumber(metrics.previousWindowRms))
                          .arg(formatNumber(metrics.currentWindowPeak))
                          .arg(formatNumber(metrics.currentWindowRms))
                          .arg(renderContext.warmup)
                          .arg(renderContext.silenceFill)
                          .arg(recovery)
                          .arg(context.recoveryPending)
                          .arg(context.recoveryAttempt)
                          .arg(context.deviceRebuildPending)
                          .arg(renderContext.firstDataBlockAfterConfigure)
                          .arg(firstDataBlockAfterDeviceRebuild)
                          .arg(suppressedDuplicateArtifacts)
                          .arg(pipelineStartupProfile)
                          .arg(artifactPath)
                          .arg(logValue(context.activeSwitchTrigger))
                          .arg(logValue(context.activeSwitchPhase))
                          .arg(logValue(context.activeSwitchReason)));
}

QString AudioArtifactMonitor::artifactCountsText() const
{
    return countsText(m_artifactCountByType);
}

QString AudioArtifactMonitor::countsText(const QHash<QString, quint64> &counts) const
{
    QStringList parts;
    parts.reserve(counts.size());
    for (auto it = counts.cbegin(); it != counts.cend(); ++it) {
        parts.append(QStringLiteral("%1:%2").arg(logValue(it.key())).arg(it.value()));
    }
    std::sort(parts.begin(), parts.end());
    return parts.isEmpty() ? QStringLiteral("none") : parts.join(QStringLiteral("|"));
}

QString AudioArtifactMonitor::controlEventKey(const QString &recentControlEvent) const
{
    const QString event = logValue(recentControlEvent);
    const int timestampSeparator = event.lastIndexOf(QChar('@'));
    if (timestampSeparator <= 0) {
        return event;
    }
    return logValue(event.left(timestampSeparator));
}

QString AudioArtifactMonitor::samplesText(const QVector<double> &samples) const
{
    if (samples.isEmpty()) {
        return QStringLiteral("none");
    }

    QStringList parts;
    parts.reserve(samples.size());
    for (double sample : samples) {
        parts.append(formatNumber(sample));
    }
    return parts.join(QChar(','));
}

void AudioArtifactMonitor::pruneRecentArtifactTimes(qint64 nowMs)
{
    const qint64 oldestAllowedMs = nowMs - kArtifactRateWindowMs;
    while (!m_recentArtifactTimesMs.isEmpty() && m_recentArtifactTimesMs.constFirst() < oldestAllowedMs) {
        m_recentArtifactTimesMs.removeFirst();
    }
}

void AudioArtifactMonitor::flushSuppressedArtifactLogs(const QString &reason)
{
    for (auto it = m_artifactLogStates.begin(); it != m_artifactLogStates.end(); ++it) {
        if (it.value().suppressedCount == 0) {
            continue;
        }

        PlayerLogger::log(QStringLiteral("anomaly"),
                          QStringLiteral("audioArtifactSuppressed reason=%1 key=%2 suppressedDuplicateArtifacts=%3")
                              .arg(logValue(reason))
                              .arg(logValue(it.key()))
                              .arg(it.value().suppressedCount));
        it.value().suppressedCount = 0;
    }
}
