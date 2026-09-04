#ifndef WINDOWSWASAPIAUDIOPLAYER_WORKER_H
#define WINDOWSWASAPIAUDIOPLAYER_WORKER_H

// Internal implementation header for the WASAPI render worker. It is split out
// to keep the high-level player source navigable while preserving the existing
// single-translation-unit helper visibility.

#include "audioplayerbackend.h"
#include "windowswasapiaudioplayer_worker_helpers.h"

#include "audioartifactmonitor.h"
#include "ffmpegpcmshared.h"
#include "pcmutils.h"
#include "playerlogger.h"

#include <QAudioDevice>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMediaDevices>
#include <QProcess>
#include <QProcessEnvironment>
#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QThread>
#include <QTimer>
#include <QVarLengthArray>
#include <QWinEventNotifier>
#include <QtEndian>

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <audioclient.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <objbase.h>
#include <spatialaudioclient.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <utility>


class WasapiOutputWorker : public QObject
{
    Q_OBJECT

public:
    explicit WasapiOutputWorker(QObject *parent = nullptr)
        : QObject(parent)
        , m_positionTimer(new QTimer(this))
        , m_fadeTimer(new QTimer(this))
    {
        m_positionTimer->setInterval(100);
        connect(m_positionTimer, &QTimer::timeout, this, [this] {
            if (m_sessionId == 0 || (!m_audioClient && !m_spatialStream)) {
                return;
            }

            emit positionUpdated(m_sessionId, processedPositionMs());
            emitIdleIfDrained();
        });
        m_fadeTimer->setSingleShot(false);
        m_fadeTimer->setInterval(4);
        connect(m_fadeTimer, &QTimer::timeout, this, [this] {
            if (m_fadeStep >= m_fadeTotalSteps) {
                setStreamGain(m_fadeTargetGain);
                m_fadeTimer->stop();
                return;
            }
            const float progress = static_cast<float>(m_fadeStep) / static_cast<float>(m_fadeTotalSteps);
            const float gain = m_fadeStartGain + (m_fadeTargetGain - m_fadeStartGain) * progress;
            setStreamGain(gain);
            ++m_fadeStep;
        });
    }

    ~WasapiOutputWorker() override
    {
        releaseOutput(0, true);
        if (m_shouldUninitializeCom) {
            CoUninitialize();
        }
    }

    void configureOutput(int sessionId,
                         const QAudioDevice &device,
                         const PcmStreamFormat &deviceFormat,
                         const PcmStreamFormat &bufferFormat,
                         const QByteArray &waveFormatData,
                         qsizetype bufferSizeBytes,
                         qreal volume,
                         bool startMutedForFadeIn,
                         bool injectStartupSilence = true,
                         bool discardWarmupFrames = true,
                         UINT32 startupSilenceMsOverride = 0,
                         UINT32 warmupDiscardMsOverride = kRecoveryWarmupSilenceMs,
                         int pcmFadeInDurationMsOverride = 0,
                         int streamFadeInDelayMs = kDeferredFadeInDelayMs,
                         ActiveSwitchBoundaryPolicy activeSwitchBoundaryPolicy = {},
                         WasapiArtifactTrackingConfig artifactTracking = {},
                         bool exclusiveMode = false,
                         bool stabilityMode = false,
                         bool spatialStaticBedRequested = false);


    bool flushSpatialEndpoint(const QAudioDevice &device,
                              const PcmStreamFormat &deviceFormat,
                              const QByteArray &waveFormatData,
                              int flushMs,
                              int settleMs,
                              const QString &reason,
                              const QString &sourcePath,
                              const QString &previousSourcePath,
                              const QString &startupProfile,
                              const QString &appStartTimeUtc);

    void startOutput(int sessionId, PcmStreamBuffer *buffer)
    {
        if (((!m_audioClient || !m_renderClient) && !m_spatialStream)
            || !buffer || m_started || sessionId != m_sessionId) {
            return;
        }

        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("startOutput session=%1 bufferOwnerSession=%2 bufferGeneration=%3")
                              .arg(sessionId)
                              .arg(buffer->ownerSessionId())
                              .arg(buffer->bufferGeneration()));
        m_buffer = buffer;
        m_outputBufferGeneration = buffer->bufferGeneration();
        if (!m_spatialStream) {
            connect(m_buffer, &QIODevice::readyRead, this, &WasapiOutputWorker::renderAvailableFrames, Qt::QueuedConnection);
        }

        m_submittedFrames = 0;
        m_idleSignaled = false;
        if (m_artifactTracking.pipelineStartupProfile == QStringLiteral("ActiveSwitchRebuild")) {
            PlayerLogger::log(QStringLiteral("audio"),
                              QStringLiteral("activeSwitchRebuildTiming marker=startOutput-before-prerender session=%1 bufferedBytes=%2 startupSilenceFrames=%3")
                                  .arg(sessionId)
                                  .arg(m_buffer->bufferedBytes())
                                  .arg(m_pendingStartupSilenceFrames));
        }
        if (!m_spatialStream) {
            renderAvailableFrames();
            if (m_terminalErrorLatched) {
                return;
            }
        }
        if (m_artifactTracking.pipelineStartupProfile == QStringLiteral("ActiveSwitchRebuild")) {
            PlayerLogger::log(QStringLiteral("audio"),
                              QStringLiteral("activeSwitchRebuildTiming marker=startOutput-after-prerender session=%1 submittedFrames=%2 startupSilenceFrames=%3")
                                  .arg(sessionId)
                                  .arg(m_submittedFrames)
                                  .arg(m_pendingStartupSilenceFrames));
        }

        const HRESULT hr = m_spatialStream ? m_spatialStream->Start() : m_audioClient->Start();
        if (FAILED(hr)) {
            handleFatalError(hr, false);
            return;
        }

        m_started = true;
        if (m_artifactTracking.pipelineStartupProfile == QStringLiteral("ActiveSwitchRebuild")) {
            PlayerLogger::log(QStringLiteral("audio"),
                              QStringLiteral("activeSwitchRebuildTiming marker=startOutput-started session=%1 submittedFrames=%2")
                                  .arg(sessionId)
                                  .arg(m_submittedFrames));
        }
        if (m_eventNotifier) {
            m_eventNotifier->setEnabled(true);
            PlayerLogger::log(QStringLiteral("audio"),
                              QStringLiteral("eventNotifier enabled session=%1 reason=startOutput")
                                  .arg(m_sessionId));
        }
        if (m_spatialEventNotifier) {
            m_spatialEventNotifier->setEnabled(true);
            PlayerLogger::log(QStringLiteral("audio"),
                              QStringLiteral("spatialEventNotifier enabled session=%1 reason=startOutput")
                                  .arg(m_sessionId));
        }
        m_positionTimer->start();
        emit stateChanged(m_sessionId,
                          static_cast<int>(QAudio::ActiveState),
                          static_cast<int>(QtAudio::NoError));
        emit positionUpdated(m_sessionId, processedPositionMs());
    }

    void pauseOutput(int sessionId);

    void resumeOutput(int sessionId);

    void prepareForOutputDeviceChange(int sessionId);

    void prepareForActiveOutputInvalidation(int sessionId);


    void restoreAfterCancelledOutputDeviceChange(int sessionId);

    void restoreAfterCancelledActiveOutputInvalidation(int sessionId);

    void restoreActiveOutputInvalidationTaper(int sessionId);

    void releaseOutput(int sessionId, bool immediateReset, bool submitStopPcmFade = false);


    void setVolume(qreal volume);


signals:
    void positionUpdated(int sessionId, qint64 processedPositionMs);
    void released(int sessionId);
    void stateChanged(int sessionId, int state, int error);

private:
    HRESULT ensureComInitialized();

    void setStreamGain(float gain)
    {
        if (!m_streamVolume || !m_deviceFormat.isValid() || m_deviceFormat.channelCount <= 0) {
            return;
        }

        QVarLengthArray<float, 8> gains(m_deviceFormat.channelCount);
        for (int channel = 0; channel < gains.size(); ++channel) {
            gains[channel] = gain;
        }
        m_streamVolume->SetAllVolumes(static_cast<UINT32>(gains.size()), gains.data());
        m_streamGain = gain;
    }

    void fadeStreamGainTo(float targetGain, bool blocking = false)
    {
        if (!m_streamVolume) {
            m_fadeTimer->stop();
            m_streamGain = targetGain;
            return;
        }

        const float startGain = m_streamGain;
        if (qAbs(startGain - targetGain) < 0.001f) {
            m_fadeTimer->stop();
            setStreamGain(targetGain);
            return;
        }

        if (blocking) {
            m_fadeTimer->stop();
            constexpr int kFadeSteps = 8;
            constexpr unsigned long kFadeDelayMs = 4;
            for (int step = 1; step <= kFadeSteps; ++step) {
                const float progress = static_cast<float>(step) / static_cast<float>(kFadeSteps);
                const float gain = startGain + (targetGain - startGain) * progress;
                setStreamGain(gain);
                QThread::msleep(kFadeDelayMs);
            }
            setStreamGain(targetGain);
            return;
        }

        m_fadeStartGain = startGain;
        m_fadeTargetGain = targetGain;
        m_fadeStep = 1;
        m_fadeTotalSteps = 4;
        m_fadeTimer->start();
    }

    void fadeOutStreamGainBeforeStop();


    bool submitPcmFadeOutBeforeStop();

    void drainMutedPaddingBeforeReset(UINT32 initialPaddingFrames);

    void fadeInStreamGainIfNeeded();


    void scheduleFadeInStreamGain();


    void noteFirstSubmittedPcmAfterSeek(UINT32 writtenFrames,
                                        UINT32 paddingFrames,
                                        UINT32 availableFrames,
                                        const RenderedBlockMetrics &metrics,
                                        const PcmFadeApplication &fade);

    void logSeekResumeLatencyIfNeeded(qint64 firstAudibleOrFadeOpenMs);

    void handleFatalError(HRESULT hr, bool openingStage);


    void resetPcmFadeIn(const PcmStreamFormat &format, bool enabled, int fadeInDurationMs = kPcmFadeInDurationMs);


    void resetVolumeRamp(const PcmStreamFormat &format, qreal startVolume, qreal targetVolume);


    void applyGainToSample(char *sampleData, qreal gain)
    {
        PcmUtils::applyGainToSample(m_bufferFormat.sampleEncoding, sampleData, gain);
    }

    double readNormalizedSample(const char *sampleData, const PcmStreamFormat &format) const
    {
        switch (format.sampleEncoding) {
        case PcmSampleEncoding::UInt8: {
            const auto sample = static_cast<quint8>(*reinterpret_cast<const unsigned char *>(sampleData));
            return (static_cast<int>(sample) - 128) / 128.0;
        }
        case PcmSampleEncoding::Int16: {
            const qint16 sample = qFromLittleEndian<qint16>(sampleData);
            return static_cast<double>(sample) / 32768.0;
        }
        case PcmSampleEncoding::Int24:
            return static_cast<double>(readInt24Sample(sampleData)) / 8388608.0;
        case PcmSampleEncoding::Int32: {
            const qint32 sample = qFromLittleEndian<qint32>(sampleData);
            return static_cast<double>(sample) / 2147483648.0;
        }
        case PcmSampleEncoding::Float32: {
            float sample = 0.0f;
            std::memcpy(&sample, sampleData, sizeof(sample));
            return std::isfinite(sample) ? static_cast<double>(sample) : 0.0;
        }
        case PcmSampleEncoding::Unknown:
            break;
        }

        return 0.0;
    }

    QString metricText(double value) const;

    QString fineMetricText(double value) const;

    RenderedBlockMetrics renderedBlockMetricsForChunk(const QByteArray &chunk) const;


    RenderedBlockMetrics renderedBlockMetricsForChunk(const QByteArray &chunk,
                                                      const PcmStreamFormat &format) const;


    double currentFadeEndpointGain() const
    {
        if (m_pcmFadeTotalFrames <= 0) {
            return 1.0;
        }
        return qBound(0.0,
                      static_cast<double>(m_pcmFadeFramesProcessed)
                          / static_cast<double>(m_pcmFadeTotalFrames),
                      1.0);
    }


    PcmFadeApplication applyPcmFadeIn(QByteArray &chunk);


    bool shouldGuardActiveSwitchFirstDataBlock() const;

    void guardActiveSwitchFirstDataBlockFade(const QByteArray &chunk);


    void applyOutputVolume(QByteArray &chunk)
    {
        if (!m_bufferFormat.isValid()) {
            return;
        }
        if (!m_volumeRampActive && qAbs(m_targetVolume - 1.0) < 0.0001) {
            return;
        }

        const int bytesPerFrame = m_bufferFormat.bytesPerFrame();
        const int channelCount = m_bufferFormat.channelCount;
        const int bytesPerSample = m_bufferFormat.bytesPerSample();
        if (bytesPerFrame <= 0 || channelCount <= 0 || bytesPerSample <= 0) {
            return;
        }

        const qsizetype frameCount = chunk.size() / bytesPerFrame;
        char *data = chunk.data();
        for (qsizetype frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
            qreal gain = m_targetVolume;
            if (m_volumeRampActive && m_volumeRampTotalFrames > 0) {
                const qreal progress = qMin<qreal>(1.0,
                                                   static_cast<qreal>(m_volumeRampFramesProcessed + 1)
                                                       / static_cast<qreal>(m_volumeRampTotalFrames));
                gain = m_volumeRampStartVolume + (m_targetVolume - m_volumeRampStartVolume) * progress;
                ++m_volumeRampFramesProcessed;
                if (m_volumeRampFramesProcessed >= m_volumeRampTotalFrames) {
                    m_volumeRampActive = false;
                    m_volumeRampTotalFrames = 0;
                    m_volumeRampFramesProcessed = 0;
                }
                m_currentVolume = gain;
            } else {
                m_currentVolume = m_targetVolume;
                gain = m_currentVolume;
            }

            char *frameData = data + frameIndex * bytesPerFrame;
            for (int channel = 0; channel < channelCount; ++channel) {
                applyGainToSample(frameData + channel * bytesPerSample, gain);
            }
        }
    }

    void applyStopPcmFadeOut(QByteArray &chunk, UINT32 processedFrames, UINT32 totalFrames);


    bool applyFadeVolumeAndConvert(const QByteArray &sourceChunk, BYTE *renderData,
                                   PcmFadeApplication &fadeOut)
    {
        if (!renderData || !m_bufferFormat.isValid() || !m_deviceFormat.isValid()) {
            return false;
        }

        fadeOut.totalFrames = m_pcmFadeTotalFrames;
        fadeOut.framesProcessedBefore = m_pcmFadeFramesProcessed;
        fadeOut.framesProcessedAfter = m_pcmFadeFramesProcessed;
        fadeOut.applied = false;

        const int bufferBytesPerFrame = m_bufferFormat.bytesPerFrame();
        const int renderBytesPerFrame = m_deviceFormat.bytesPerFrame();
        const int channelCount = m_bufferFormat.channelCount;
        const int bytesPerSample = m_bufferFormat.bytesPerSample();

        if (bufferBytesPerFrame <= 0 || renderBytesPerFrame <= 0 || channelCount <= 0 || bytesPerSample <= 0) {
            return false;
        }

        const qsizetype frameCount = sourceChunk.size() / bufferBytesPerFrame;
        if (frameCount <= 0) {
            return false;
        }

        const bool hasFade = m_pcmFadeTotalFrames > 0;
        const bool hasVolume = m_volumeRampActive || qAbs(m_targetVolume - 1.0) >= 0.0001;
        const bool needConversion = !hasSamePcmLayout(m_bufferFormat, m_deviceFormat);

        if (!hasFade && !hasVolume && !needConversion) {
            std::memcpy(renderData, sourceChunk.constData(), static_cast<size_t>(sourceChunk.size()));
            return true;
        }

        const bool seekResumeZeroStartFade =
            m_artifactTracking.pipelineStartupProfile == QStringLiteral("SeekResume")
            && m_pcmFadeTotalFrames > 1;
        const qsizetype fadeDenominator =
            seekResumeZeroStartFade ? m_pcmFadeTotalFrames - 1 : m_pcmFadeTotalFrames;

        const char *source = sourceChunk.constData();
        char *target = reinterpret_cast<char *>(renderData);

        const bool useNoiseShaping = m_noiseShaperState.size() == channelCount;
        const bool isInt32ToInt24 = m_bufferFormat.sampleEncoding == PcmSampleEncoding::Int32
            && m_deviceFormat.sampleEncoding == PcmSampleEncoding::Int24
            && m_bufferFormat.channelCount == m_deviceFormat.channelCount;
        const bool isInt32ToInt16 = m_bufferFormat.sampleEncoding == PcmSampleEncoding::Int32
            && m_deviceFormat.sampleEncoding == PcmSampleEncoding::Int16
            && m_bufferFormat.channelCount == m_deviceFormat.channelCount;
        const bool isInt32ToInt32_24 = m_bufferFormat.sampleEncoding == PcmSampleEncoding::Int32
            && m_deviceFormat.sampleEncoding == PcmSampleEncoding::Int32
            && m_bufferFormat.effectiveValidBitsPerSample() == 32
            && m_deviceFormat.effectiveValidBitsPerSample() == 24
            && m_bufferFormat.channelCount == m_deviceFormat.channelCount;

        fadeOut.minGain = 1.0;
        fadeOut.maxGain = 0.0;

        for (qsizetype frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
            qreal fadeGain = 1.0;
            if (hasFade) {
                const qsizetype fadeNumerator =
                    m_pcmFadeFramesProcessed + frameIndex + (seekResumeZeroStartFade ? 0 : 1);
                fadeGain = qMin<qreal>(1.0,
                                       static_cast<qreal>(fadeNumerator)
                                           / static_cast<qreal>(fadeDenominator));
                fadeOut.minGain = qMin(fadeOut.minGain, static_cast<double>(fadeGain));
                fadeOut.maxGain = qMax(fadeOut.maxGain, static_cast<double>(fadeGain));
                fadeOut.applied = true;
            }

            qreal volumeGain = m_targetVolume;
            if (m_volumeRampActive && m_volumeRampTotalFrames > 0) {
                const qreal progress = qMin<qreal>(1.0,
                                                   static_cast<qreal>(m_volumeRampFramesProcessed + 1)
                                                       / static_cast<qreal>(m_volumeRampTotalFrames));
                volumeGain = m_volumeRampStartVolume + (m_targetVolume - m_volumeRampStartVolume) * progress;
                ++m_volumeRampFramesProcessed;
                if (m_volumeRampFramesProcessed >= m_volumeRampTotalFrames) {
                    m_volumeRampActive = false;
                    m_volumeRampTotalFrames = 0;
                    m_volumeRampFramesProcessed = 0;
                }
                m_currentVolume = volumeGain;
            } else {
                m_currentVolume = m_targetVolume;
                volumeGain = m_currentVolume;
            }

            const qreal combinedGain = fadeGain * volumeGain;

            const char *sourceFrame = source + frameIndex * bufferBytesPerFrame;
            char *targetFrame = target + frameIndex * renderBytesPerFrame;

            for (int channel = 0; channel < channelCount; ++channel) {
                if (m_bufferFormat.sampleEncoding == PcmSampleEncoding::Float32
                    && m_deviceFormat.sampleEncoding == PcmSampleEncoding::Float32) {
                    float sample = 0.0f;
                    std::memcpy(&sample, sourceFrame + channel * bytesPerSample, sizeof(float));
                    sample = static_cast<float>(static_cast<qreal>(sample) * combinedGain);
                    std::memcpy(targetFrame + channel * m_deviceFormat.bytesPerSample(),
                                &sample, sizeof(float));
                    continue;
                }

                const qint32 sample32 =
                    qFromLittleEndian<qint32>(sourceFrame + channel * bytesPerSample);

                const qint64 scaledValue = qRound64(static_cast<qreal>(sample32) * combinedGain);
                const qint32 scaled = static_cast<qint32>(
                    qBound<qint64>(static_cast<qint64>(std::numeric_limits<qint32>::min()),
                                   scaledValue,
                                   static_cast<qint64>(std::numeric_limits<qint32>::max())));

                if (isInt32ToInt24) {
                    const qint32 quantized = useNoiseShaping
                        ? noiseShapedQuantize32To24(scaled, m_noiseShaperState[channel])
                        : roundedQuantize32To24(scaled);
                    writeInt24Sample(quantized / 256,
                                     targetFrame + channel * m_deviceFormat.bytesPerSample());
                } else if (isInt32ToInt16) {
                    qint16 output16;
                    if (useNoiseShaping) {
                        output16 = noiseShapedQuantize32To16(scaled, m_noiseShaperState[channel]);
                    } else {
                        const qint64 rounded = scaled >= 0
                            ? static_cast<qint64>(scaled) + 32768
                            : static_cast<qint64>(scaled) - 32768;
                        output16 = static_cast<qint16>(qBound<qint64>(static_cast<qint64>(-32768),
                                                                       rounded / 65536,
                                                                       static_cast<qint64>(32767)));
                    }
                    qToLittleEndian<qint16>(output16, targetFrame + channel * 2);
                } else if (isInt32ToInt32_24) {
                    const qint32 quantized = useNoiseShaping
                        ? noiseShapedQuantize32To24(scaled, m_noiseShaperState[channel])
                        : roundedQuantize32To24(scaled);
                    qToLittleEndian<qint32>(quantized,
                                           targetFrame + channel * m_deviceFormat.bytesPerSample());
                } else if (m_deviceFormat.sampleEncoding == PcmSampleEncoding::Float32) {
                    const float output = static_cast<float>(
                        static_cast<double>(scaled)
                        / static_cast<double>(std::numeric_limits<qint32>::max()));
                    std::memcpy(targetFrame + channel * m_deviceFormat.bytesPerSample(),
                                &output, sizeof(float));
                } else {
                    std::memcpy(targetFrame + channel * m_deviceFormat.bytesPerSample(),
                                sourceFrame + channel * bytesPerSample,
                                qMin(bytesPerSample, m_deviceFormat.bytesPerSample()));
                }
            }
        }

        fadeOut.framesProcessedAfter = m_pcmFadeFramesProcessed + frameCount;
        m_pcmFadeFramesProcessed = fadeOut.framesProcessedAfter;
        if (m_pcmFadeFramesProcessed >= m_pcmFadeTotalFrames) {
            m_pcmFadeTotalFrames = 0;
            m_pcmFadeFramesProcessed = 0;
        }

        return true;
    }

    bool copyConvertedFramesToRenderBuffer(const QByteArray &chunk, BYTE *renderData)
    {
        if (!renderData) {
            return false;
        }

        if (hasSamePcmLayout(m_bufferFormat, m_deviceFormat)) {
            std::memcpy(renderData, chunk.constData(), static_cast<size_t>(chunk.size()));
            return true;
        }

        if (m_bufferFormat.sampleEncoding == PcmSampleEncoding::Int32
            && m_deviceFormat.sampleEncoding == PcmSampleEncoding::Int24
            && m_bufferFormat.channelCount == m_deviceFormat.channelCount) {
            const int channelCount = m_bufferFormat.channelCount;
            const qsizetype frameCount = chunk.size() / m_bufferFormat.bytesPerFrame();
            const char *source = chunk.constData();
            char *target = reinterpret_cast<char *>(renderData);
            const bool useNoiseShaping = m_noiseShaperState.size() == channelCount;
            for (qsizetype frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
                const char *sourceFrame = source + frameIndex * m_bufferFormat.bytesPerFrame();
                char *targetFrame = target + frameIndex * m_deviceFormat.bytesPerFrame();
                for (int channel = 0; channel < channelCount; ++channel) {
                    const qint32 sample32 =
                        qFromLittleEndian<qint32>(sourceFrame + channel * m_bufferFormat.bytesPerSample());
                    const qint32 quantized = useNoiseShaping
                        ? noiseShapedQuantize32To24(sample32, m_noiseShaperState[channel])
                        : roundedQuantize32To24(sample32);
                    writeInt24Sample(quantized / 256,
                                     targetFrame + channel * m_deviceFormat.bytesPerSample());
                }
            }
            return true;
        }

        if (m_bufferFormat.sampleEncoding == PcmSampleEncoding::Int32
            && m_deviceFormat.sampleEncoding == PcmSampleEncoding::Int32
            && m_bufferFormat.effectiveValidBitsPerSample() == 32
            && m_deviceFormat.effectiveValidBitsPerSample() == 24
            && m_bufferFormat.channelCount == m_deviceFormat.channelCount) {
            const int channelCount = m_bufferFormat.channelCount;
            const qsizetype frameCount = chunk.size() / m_bufferFormat.bytesPerFrame();
            const char *source = chunk.constData();
            char *target = reinterpret_cast<char *>(renderData);
            const bool useNoiseShaping = m_noiseShaperState.size() == channelCount;
            for (qsizetype frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
                const char *sourceFrame = source + frameIndex * m_bufferFormat.bytesPerFrame();
                char *targetFrame = target + frameIndex * m_deviceFormat.bytesPerFrame();
                for (int channel = 0; channel < channelCount; ++channel) {
                    const qint32 sample32 =
                        qFromLittleEndian<qint32>(sourceFrame + channel * m_bufferFormat.bytesPerSample());
                    const qint32 quantized = useNoiseShaping
                        ? noiseShapedQuantize32To24(sample32, m_noiseShaperState[channel])
                        : roundedQuantize32To24(sample32);
                    qToLittleEndian<qint32>(quantized,
                                           targetFrame + channel * m_deviceFormat.bytesPerSample());
                }
            }
            return true;
        }

        if (m_bufferFormat.sampleEncoding == PcmSampleEncoding::Int32
            && m_deviceFormat.sampleEncoding == PcmSampleEncoding::Int16
            && m_bufferFormat.channelCount == m_deviceFormat.channelCount) {
            const int channelCount = m_bufferFormat.channelCount;
            const qsizetype frameCount = chunk.size() / m_bufferFormat.bytesPerFrame();
            const char *source = chunk.constData();
            char *target = reinterpret_cast<char *>(renderData);
            const bool useNoiseShaping = m_noiseShaperState.size() == channelCount;
            for (qsizetype frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
                const char *sourceFrame = source + frameIndex * m_bufferFormat.bytesPerFrame();
                char *targetFrame = target + frameIndex * m_deviceFormat.bytesPerFrame();
                for (int channel = 0; channel < channelCount; ++channel) {
                    const qint32 sample32 =
                        qFromLittleEndian<qint32>(sourceFrame + channel * m_bufferFormat.bytesPerSample());
                    qint16 output16;
                    if (useNoiseShaping) {
                        output16 = noiseShapedQuantize32To16(sample32, m_noiseShaperState[channel]);
                    } else {
                        const qint64 rounded = sample32 >= 0
                            ? static_cast<qint64>(sample32) + 32768
                            : static_cast<qint64>(sample32) - 32768;
                        output16 = static_cast<qint16>(qBound<qint64>(static_cast<qint64>(-32768),
                                                                       rounded / 65536,
                                                                       static_cast<qint64>(32767)));
                    }
                    qToLittleEndian<qint16>(output16, targetFrame + channel * 2);
                }
            }
            return true;
        }

        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("unsupportedRenderConversion session=%1 bufferBits=%2 deviceBits=%3 bufferEncoding=%4 deviceEncoding=%5")
                              .arg(m_sessionId)
                              .arg(m_bufferFormat.bitsPerSample())
                              .arg(m_deviceFormat.bitsPerSample())
                              .arg(static_cast<int>(m_bufferFormat.sampleEncoding))
                              .arg(static_cast<int>(m_deviceFormat.sampleEncoding)));
        return false;
    }

    qint64 processedPositionMs();


    qint64 processedPositionMsFromPadding(UINT32 paddingFrames) const;

    bool artifactTrackingEnabled() const
    {
        return m_artifactTracking.enabled && m_bufferFormat.isValid();
    }

    AudioArtifactMonitor::PlaybackContext artifactPlaybackContext(UINT32 paddingFrames) const;


    AudioArtifactMonitor::RenderContext artifactRenderContext(UINT32 writeFrameCount,
                                                              UINT32 paddingFrames,
                                                              UINT32 availableFrames,
                                                              bool warmup,
                                                              bool silenceFill,
                                                              bool firstDataBlock) const;


    QJsonObject formatJson(const PcmStreamFormat &format) const;

    QString renderMirrorBasePath(int sessionId) const;

    QString previousRunTailFingerprintPath() const;

    void saveSubmittedTailFingerprint(const QString &reason);

    void startRenderMirrorCapture(const QByteArray &previousTail,
                                  const PcmStreamFormat &previousTailFormat);

    void finishRenderMirrorCapture(const QString &reason);

    void appendSubmittedPcmTail(const QByteArray &submittedChunk, const PcmStreamFormat &format);

    void captureSeekResumeFirst50msSubmittedPcm(const QByteArray &submittedChunk,
                                                UINT32 writtenFrames,
                                                UINT32 paddingFrames,
                                                UINT32 availableFrames,
                                                bool warmup,
                                                bool silenceFill);

    void mirrorSubmittedBlock(const QByteArray &submittedChunk,
                              UINT32 writtenFrames,
                              UINT32 paddingFrames,
                              UINT32 availableFrames,
                              const QString &renderSource,
                              bool warmup,
                              bool silenceFill,
                              bool firstDataBlock);

    void observeArtifactSilence(UINT32 frameCount,
                                UINT32 paddingFrames,
                                UINT32 availableFrames,
                                bool warmup,
                                const QString &renderSource);

    void analyzeArtifactBlock(const QByteArray &chunk,
                              UINT32 writtenFrames,
                              UINT32 paddingFrames,
                              UINT32 availableFrames,
                              const QString &renderSource);

    void emitIdleIfDrained()
    {
        if (m_terminalErrorLatched || (!m_audioClient && !m_spatialStream)
            || !m_buffer || !m_buffer->endOfStream() || m_idleSignaled) {
            return;
        }

        if (m_spatialStream) {
            if (m_buffer->bufferedBytes() == 0) {
                m_idleSignaled = true;
                emit stateChanged(m_sessionId,
                                  static_cast<int>(QAudio::IdleState),
                                  static_cast<int>(QtAudio::NoError));
            }
            return;
        }

        UINT32 paddingFrames = 0;
        const HRESULT hr = m_audioClient->GetCurrentPadding(&paddingFrames);
        if (FAILED(hr)) {
            handleFatalError(hr, false);
            return;
        }

        if (paddingFrames == 0 && m_buffer->bufferedBytes() == 0) {
            m_idleSignaled = true;
            emit stateChanged(m_sessionId,
                              static_cast<int>(QAudio::IdleState),
                              static_cast<int>(QtAudio::NoError));
        }
    }

    void renderAvailableFrames();
    bool configureSpatialStaticBed(const QAudioDevice &device);
    void releaseSpatialStaticBed();
    void renderSpatialFrames();
    bool ensureSpatialStaticObjectsActivated();
    bool writeSpatialSilence(UINT32 frameCount, const QString &reason);
    bool writeSpatialPcm(const QByteArray &chunk, UINT32 frameCount);


    PcmStreamBuffer *m_buffer = nullptr;
    IMMDevice *m_device = nullptr;
    IAudioClient *m_audioClient = nullptr;
    IAudioRenderClient *m_renderClient = nullptr;
    IAudioStreamVolume *m_streamVolume = nullptr;
    ISpatialAudioClient *m_spatialClient = nullptr;
    ISpatialAudioObjectRenderStream *m_spatialStream = nullptr;
    QVarLengthArray<ISpatialAudioObject *, 8> m_spatialObjects;
    QVarLengthArray<AudioObjectType, 8> m_spatialObjectTypes;
    HANDLE m_spatialEvent = nullptr;
    QWinEventNotifier *m_spatialEventNotifier = nullptr;
    QByteArray m_spatialObjectFormatData;
    bool m_spatialStaticBedActive = false;
    HANDLE m_refillEvent = nullptr;
    QWinEventNotifier *m_eventNotifier = nullptr;
    QTimer *m_positionTimer = nullptr;
    QTimer *m_fadeTimer = nullptr;
    int m_fadeStep = 0;
    int m_fadeTotalSteps = 4;
    float m_fadeStartGain = 1.0f;
    float m_fadeTargetGain = 1.0f;
    QByteArray m_waveFormatData;
    UINT32 m_bufferFrameCount = 0;
    UINT32 m_pendingStartupSilenceFrames = 0;
    UINT32 m_configuredStartupSilenceFrames = 0;
    UINT32 m_configuredStartupSilenceMs = 0;
    UINT32 m_pendingRecoveryWarmupFrames = 0;
    UINT32 m_configuredWarmupDiscardMs = 0;
    quint64 m_submittedFrames = 0;
    int m_sessionId = 0;
    bool m_started = false;
    bool m_comInitialized = false;
    bool m_shouldUninitializeCom = false;
    bool m_idleSignaled = false;
    bool m_streamFadeInPending = false;
    bool m_streamFadeInScheduled = false;
    float m_streamGain = 1.0f;
    bool m_stopFadeInProgress = false;
    int m_renderCallbacksDuringStopFade = 0;
    qreal m_targetVolume = 1.0;
    qreal m_currentVolume = 1.0;
    qreal m_volumeRampStartVolume = 1.0;
    bool m_volumeRampActive = false;
    qsizetype m_volumeRampTotalFrames = 0;
    qsizetype m_volumeRampFramesProcessed = 0;
    AudioArtifactMonitor m_artifactMonitor;
    WasapiArtifactTrackingConfig m_artifactTracking;
    bool m_firstDataBlockAfterConfigure = false;
    RenderedBlockMetrics m_lastRenderedBlock;
    RenderedBlockMetrics m_previousRenderedBlock;
    RenderedBlockMetrics m_activeSwitchEntryBridgeBlock;
    bool m_activeSwitchEntryBridgeFallback = false;
    QString m_activeSwitchBoundaryPolicyName = QStringLiteral("none");
    double m_activeSwitchFirstBlockMaxFadeGain = kActiveSwitchFirstBlockMaxFadeGain;
    float m_activeSwitchEntryBridgeStreamGain = kActiveSwitchEntryBridgeStreamGain;
    PcmStreamFormat m_deviceFormat;
    PcmStreamFormat m_bufferFormat;
    QVarLengthArray<NoiseShaperState, 8> m_noiseShaperState;
    int m_pcmFadeInDurationMs = kPcmFadeInDurationMs;
    qsizetype m_pcmFadeTotalFrames = 0;
    qsizetype m_pcmFadeFramesProcessed = 0;
    int m_streamFadeInDelayMs = kDeferredFadeInDelayMs;
    qint64 m_seekResumeFirstDecodedPcmAfterSeekMs = -1;
    qint64 m_seekResumeFirstSubmittedPcmAfterSeekMs = -1;
    bool m_seekResumeLatencyLogged = false;
    QByteArray m_seekResumeFirst50msSubmittedPcm;
    qint64 m_seekResumeFirst50msTargetFrames = 0;
    qint64 m_seekResumeFirst50msCapturedFrames = 0;
    qint64 m_seekResumeFirst50msStartupSilenceFrames = 0;
    qint64 m_seekResumeFirst50msWarmupFrames = 0;
    qint64 m_seekResumeFirst50msRealPcmFrames = 0;
    bool m_seekResumeFirst50msLogged = false;
    int m_waitForDataStreak = 0;
    bool m_terminalErrorLatched = false;
    qint64 m_rateWindowStartTimeMs = 0;
    quint64 m_rateWindowStartFrames = 0;
    qint64 m_rateLastLogTimeMs = 0;
    qint64 m_lastRenderCallbackTime = 0;
    bool m_exclusiveModeActive = false;
public:
    bool exclusiveModeActive() const { return m_exclusiveModeActive; }
private:
    quint64 m_outputBufferGeneration = 0;
    QByteArray m_submittedRenderTail;
    PcmStreamFormat m_submittedRenderTailFormat;
    QFile m_renderMirrorFile;
    QJsonObject m_renderMirrorMetadata;
    AudioArtifactMonitor m_renderMirrorMonitor;
    bool m_renderMirrorActive = false;
    qint64 m_renderMirrorCapturedFrames = 0;
    qint64 m_renderMirrorMaxFrames = 0;
    QString m_renderMirrorRawPath;
    QString m_renderMirrorMetadataPath;
};

#endif // WINDOWSWASAPIAUDIOPLAYER_WORKER_H
