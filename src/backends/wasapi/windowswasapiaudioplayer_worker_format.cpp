#include "windowswasapiaudioplayer_worker.h"

RenderedBlockMetrics WasapiOutputWorker::renderedBlockMetricsForChunk(const QByteArray &chunk) const
{
    return renderedBlockMetricsForChunk(chunk, m_bufferFormat);
}

RenderedBlockMetrics WasapiOutputWorker::renderedBlockMetricsForChunk(const QByteArray &chunk,
                                                  const PcmStreamFormat &format) const
{
    RenderedBlockMetrics metrics;
    const int bytesPerFrame = format.bytesPerFrame();
    const int channelCount = format.channelCount;
    const int bytesPerSample = format.bytesPerSample();
    if (chunk.isEmpty() || bytesPerFrame <= 0 || channelCount <= 0 || bytesPerSample <= 0) {
        return metrics;
    }

    QVarLengthArray<double, 8> previousSamples(channelCount);
    for (int channel = 0; channel < channelCount; ++channel) {
        previousSamples[channel] = 0.0;
    }
    double sumSquares = 0.0;
    qsizetype sampleCount = 0;
    const qsizetype frameCount = chunk.size() / bytesPerFrame;
    if (frameCount <= 0) {
        return metrics;
    }

    metrics.valid = true;
    metrics.sessionId = m_sessionId;
    metrics.frameCount = frameCount;
    const char *data = chunk.constData();
    for (qsizetype frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
        const char *frameData = data + frameIndex * bytesPerFrame;
        double framePeak = 0.0;
        for (int channel = 0; channel < channelCount; ++channel) {
            const double sample = readNormalizedSample(frameData + channel * bytesPerSample, format);
            const double magnitude = std::abs(sample);
            metrics.peak = qMax(metrics.peak, magnitude);
            metrics.jump = qMax(metrics.jump, std::abs(sample - previousSamples[channel]));
            framePeak = qMax(framePeak, magnitude);
            previousSamples[channel] = sample;
            sumSquares += sample * sample;
            ++sampleCount;
            if (channel == 0 && frameIndex == 0) {
                metrics.firstSample = sample;
            }
            if (channel == 0 && frameIndex + 1 == frameCount) {
                metrics.lastSample = sample;
            }
        }
        if (frameIndex == 0) {
            metrics.firstSamplePeak = framePeak;
        }
        if (frameIndex + 1 == frameCount) {
            metrics.lastSamplePeak = framePeak;
        }
    }

    metrics.rms = sampleCount > 0 ? std::sqrt(sumSquares / sampleCount) : 0.0;
    return metrics;
}

PcmFadeApplication WasapiOutputWorker::applyPcmFadeIn(QByteArray &chunk)
{
    PcmFadeApplication application;
    application.totalFrames = m_pcmFadeTotalFrames;
    application.framesProcessedBefore = m_pcmFadeFramesProcessed;
    application.framesProcessedAfter = m_pcmFadeFramesProcessed;
    if (m_pcmFadeTotalFrames <= 0 || !m_bufferFormat.isValid()) {
        return application;
    }

    const int bytesPerFrame = m_bufferFormat.bytesPerFrame();
    const int channelCount = m_bufferFormat.channelCount;
    const int bytesPerSample = m_bufferFormat.bytesPerSample();
    if (bytesPerFrame <= 0 || channelCount <= 0 || bytesPerSample <= 0) {
        m_pcmFadeTotalFrames = 0;
        m_pcmFadeFramesProcessed = 0;
        return application;
    }

    const qsizetype frameCount = chunk.size() / bytesPerFrame;
    if (frameCount <= 0) {
        return application;
    }

    const qsizetype framesToProcess = qMin(frameCount, m_pcmFadeTotalFrames - m_pcmFadeFramesProcessed);
    if (framesToProcess <= 0) {
        m_pcmFadeTotalFrames = 0;
        m_pcmFadeFramesProcessed = 0;
        return application;
    }

    application.applied = true;
    application.frames = framesToProcess;
    application.minGain = 1.0;
    application.maxGain = 0.0;
    char *data = chunk.data();
    const bool seekResumeZeroStartFade =
        m_artifactTracking.pipelineStartupProfile == QStringLiteral("SeekResume")
        && m_pcmFadeTotalFrames > 1;
    for (qsizetype frameIndex = 0; frameIndex < framesToProcess; ++frameIndex) {
        const qreal gain = seekResumeZeroStartFade
            ? PcmUtils::computeLinearFadeGainFromZero(m_pcmFadeFramesProcessed, frameIndex, m_pcmFadeTotalFrames)
            : PcmUtils::computeLinearFadeGain(m_pcmFadeFramesProcessed, frameIndex, m_pcmFadeTotalFrames);
        application.minGain = qMin(application.minGain, static_cast<double>(gain));
        application.maxGain = qMax(application.maxGain, static_cast<double>(gain));
        char *frameData = data + frameIndex * bytesPerFrame;
        for (int channel = 0; channel < channelCount; ++channel) {
            PcmUtils::applyGainToSample(m_bufferFormat.sampleEncoding, frameData + channel * bytesPerSample, gain);
        }
    }

    application.framesProcessedAfter = m_pcmFadeFramesProcessed + framesToProcess;
    m_pcmFadeFramesProcessed = application.framesProcessedAfter;
    if (m_pcmFadeFramesProcessed >= m_pcmFadeTotalFrames) {
        m_pcmFadeTotalFrames = 0;
        m_pcmFadeFramesProcessed = 0;
    }
    return application;
}

void WasapiOutputWorker::guardActiveSwitchFirstDataBlockFade(const QByteArray &chunk)
{
    if (!shouldGuardActiveSwitchFirstDataBlock()) {
        return;
    }

    const int bytesPerFrame = m_bufferFormat.bytesPerFrame();
    if (bytesPerFrame <= 0) {
        return;
    }

    const qsizetype frameCount = chunk.size() / bytesPerFrame;
    if (frameCount <= 0) {
        return;
    }

    const qsizetype guardedFadeFrames = static_cast<qsizetype>(
        std::ceil(static_cast<double>(m_pcmFadeFramesProcessed + frameCount)
                  / m_activeSwitchFirstBlockMaxFadeGain));
    if (guardedFadeFrames <= m_pcmFadeTotalFrames) {
        return;
    }

    PlayerLogger::log(QStringLiteral("audio"),
                      QStringLiteral("activeSwitchFirstBlockEntryGuard session=%1 frames=%2 previousFadeFrames=%3 guardedFadeFrames=%4 maxFirstBlockGain=%5")
                          .arg(m_sessionId)
                          .arg(frameCount)
                          .arg(m_pcmFadeTotalFrames)
                          .arg(guardedFadeFrames)
                          .arg(QString::number(m_activeSwitchFirstBlockMaxFadeGain, 'f', 2)));
    m_pcmFadeTotalFrames = guardedFadeFrames;
}

void WasapiOutputWorker::applyStopPcmFadeOut(QByteArray &chunk, UINT32 processedFrames, UINT32 totalFrames)
{
    const int bytesPerFrame = m_bufferFormat.bytesPerFrame();
    const int channelCount = m_bufferFormat.channelCount;
    const int bytesPerSample = m_bufferFormat.bytesPerSample();
    if (bytesPerFrame <= 0 || channelCount <= 0 || bytesPerSample <= 0 || totalFrames == 0) {
        return;
    }

    const qreal outputVolume = m_volumeRampActive ? m_currentVolume : m_targetVolume;
    const qsizetype frameCount = chunk.size() / bytesPerFrame;
    char *data = chunk.data();
    for (qsizetype frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
        const qreal fadeGain = qMax<qreal>(
            0.0,
            1.0 - static_cast<qreal>(processedFrames + frameIndex + 1)
                / static_cast<qreal>(totalFrames));
        char *frameData = data + frameIndex * bytesPerFrame;
        for (int channel = 0; channel < channelCount; ++channel) {
            applyGainToSample(frameData + channel * bytesPerSample, fadeGain * outputVolume);
        }
    }
}

QString WasapiOutputWorker::metricText(double value) const
{
    return QString::number(value, 'f', 4);
}

QString WasapiOutputWorker::fineMetricText(double value) const
{
    return QString::number(value, 'f', 6);
}

bool WasapiOutputWorker::shouldGuardActiveSwitchFirstDataBlock() const
{
    return m_firstDataBlockAfterConfigure
        && m_artifactTracking.pipelineStartupProfile == QStringLiteral("ActiveSwitchRebuild")
        && m_bufferFormat.isValid()
        && m_activeSwitchFirstBlockMaxFadeGain > 0.0
        && m_activeSwitchFirstBlockMaxFadeGain < 1.0;
}

qint64 WasapiOutputWorker::processedPositionMsFromPadding(UINT32 paddingFrames) const
{
    if (m_deviceFormat.sampleRate <= 0) {
        return 0;
    }

    const quint64 processedFrames = m_submittedFrames > paddingFrames
        ? m_submittedFrames - paddingFrames
        : 0;
    return static_cast<qint64>(processedFrames * 1000 / static_cast<quint64>(m_deviceFormat.sampleRate));
}

void WasapiOutputWorker::resetPcmFadeIn(const PcmStreamFormat &format, bool enabled, int fadeInDurationMs)
{
    if (!enabled || !format.isValid() || format.sampleRate <= 0 || format.bytesPerFrame() <= 0) {
        m_pcmFadeTotalFrames = 0;
        m_pcmFadeFramesProcessed = 0;
        m_pcmFadeInDurationMs = 0;
        return;
    }

    m_pcmFadeInDurationMs = qMax(1, fadeInDurationMs);
    m_pcmFadeTotalFrames = qMax<qsizetype>(1, format.sampleRate * m_pcmFadeInDurationMs / 1000);
    m_pcmFadeFramesProcessed = 0;
    PlayerLogger::log(QStringLiteral("audio"),
                      QStringLiteral("resetPcmFadeIn session=%1 totalFrames=%2 sampleRate=%3 durationMs=%4")
                          .arg(m_sessionId)
                          .arg(m_pcmFadeTotalFrames)
                          .arg(format.sampleRate)
                          .arg(m_pcmFadeInDurationMs));
}

void WasapiOutputWorker::resetVolumeRamp(const PcmStreamFormat &format, qreal startVolume, qreal targetVolume)
{
    m_currentVolume = startVolume;
    m_targetVolume = targetVolume;
    m_volumeRampFramesProcessed = 0;
    if (!format.isValid() || format.sampleRate <= 0 || qFuzzyCompare(startVolume, targetVolume)) {
        m_volumeRampActive = false;
        m_volumeRampTotalFrames = 0;
        m_currentVolume = targetVolume;
        return;
    }

    m_volumeRampActive = true;
    m_volumeRampStartVolume = startVolume;
    m_volumeRampTotalFrames = qMax<qsizetype>(1, format.sampleRate * 20 / 1000);
}
