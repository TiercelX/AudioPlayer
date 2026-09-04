#include "alsaoutputworker.h"

#include "pcmutils.h"

#include <QThread>
#include <QDebug>

#include <cstring>

namespace {
constexpr int kLevelEmitIntervalMs = 50;
constexpr int kMinFrames = 64;
constexpr int kVolumeRampDurationMs = 20;
constexpr int kPcmFadeInDurationMs = 20;

PcmSampleEncoding alsaFormatToPcmEncoding(snd_pcm_format_t fmt)
{
    switch (fmt) {
    case SND_PCM_FORMAT_S16_LE:
        return PcmSampleEncoding::Int16;
    case SND_PCM_FORMAT_S24_3LE:
        return PcmSampleEncoding::Int24;
    case SND_PCM_FORMAT_S24_LE:
    case SND_PCM_FORMAT_S32_LE:
        return PcmSampleEncoding::Int32;
    case SND_PCM_FORMAT_FLOAT_LE:
        return PcmSampleEncoding::Float32;
    default:
        return PcmSampleEncoding::Unknown;
    }
}
}

AlsaOutputWorker::AlsaOutputWorker(QObject *parent)
    : QObject(parent)
{
}

AlsaOutputWorker::~AlsaOutputWorker()
{
    stop();
}

void AlsaOutputWorker::configure(snd_pcm_t *handle,
                                  PcmStreamBuffer *buffer,
                                  const PcmStreamFormat &bufferFormat,
                                  const PcmStreamFormat &deviceFormat,
                                  snd_pcm_format_t alsaFormat,
                                  int sessionId,
                                  quint64 bufferGeneration,
                                  bool startMutedForFadeIn,
                                  qsizetype startupThresholdBytes,
                                  qsizetype startupSilenceFrames,
                                  qsizetype warmupDiscardFrames)
{
    m_handle = handle;
    m_buffer = buffer;
    m_bufferFormat = bufferFormat;
    m_deviceFormat = deviceFormat;
    m_alsaFormat = alsaFormat;
    m_sessionId = sessionId;
    m_bufferGeneration = bufferGeneration;
    m_processedFrames = 0;
    m_xrunCount = 0;
    m_startupThresholdBytes = startupThresholdBytes;
    m_startupSilenceFrames = startupSilenceFrames;
    m_warmupDiscardFrames = warmupDiscardFrames;
    m_stopFadeOutRequested = false;
    m_pcmFadeOutTotalFrames = 0;
    m_pcmFadeOutFramesProcessed = 0;

    m_frameSize = deviceFormat.channelCount * AlsaFormatNegotiator::bytesPerSampleForFormat(alsaFormat);

    if (startMutedForFadeIn && deviceFormat.sampleRate > 0 && deviceFormat.bytesPerFrame() > 0) {
        m_pcmFadeTotalFrames = qMax<qsizetype>(1, deviceFormat.sampleRate * kPcmFadeInDurationMs / 1000);
        m_pcmFadeFramesProcessed = 0;
    } else {
        m_pcmFadeTotalFrames = 0;
        m_pcmFadeFramesProcessed = 0;
    }

    m_volumeRampActive = false;
    m_volumeRampTotalFrames = 0;
    m_volumeRampFramesProcessed = 0;
    m_currentVolume = startMutedForFadeIn ? 0.0 : static_cast<qreal>(m_targetVolume.load());
}

void AlsaOutputWorker::start()
{
    m_running = true;
    m_paused = false;
    m_lastLevelEmit.start();
    run();
}

void AlsaOutputWorker::stop()
{
    m_running = false;
}

void AlsaOutputWorker::setPaused(bool paused)
{
    m_paused = paused;
}

void AlsaOutputWorker::setVolume(qreal volume)
{
    const qreal clampedVolume = qBound<qreal>(0.0, volume, 1.0);
    if (qFuzzyCompare(static_cast<qreal>(m_targetVolume.load()), clampedVolume)) {
        return;
    }
    if (m_deviceFormat.sampleRate > 0 && m_frameSize > 0) {
        m_volumeRampStartVolume = m_currentVolume;
        m_targetVolume = clampedVolume;
        m_volumeRampTotalFrames = qMax<qsizetype>(1, m_deviceFormat.sampleRate * kVolumeRampDurationMs / 1000);
        m_volumeRampFramesProcessed = 0;
        m_volumeRampActive = true;
    } else {
        m_currentVolume = clampedVolume;
        m_targetVolume = clampedVolume;
        m_volumeRampActive = false;
        m_volumeRampTotalFrames = 0;
        m_volumeRampFramesProcessed = 0;
    }
}

void AlsaOutputWorker::run()
{
    while (m_running) {
        if (m_paused) {
            QThread::msleep(10);
            continue;
        }

        if (!m_buffer || m_frameSize <= 0) {
            QThread::msleep(10);
            continue;
        }

        qint64 available = m_buffer->bytesAvailable();
        const qsizetype minBytes = m_startupThresholdBytes > 0
            ? m_startupThresholdBytes
            : static_cast<qsizetype>(m_frameSize * kMinFrames);
        if (available < minBytes) {
            QThread::msleep(10);
            continue;
        }

        if (m_startupSilenceFrames > 0) {
            const int silenceBytes = static_cast<int>(m_startupSilenceFrames * m_frameSize);
            QByteArray silence(silenceBytes, 0);
            snd_pcm_sframes_t written = snd_pcm_writei(m_handle, silence.constData(), m_startupSilenceFrames);
            if (written < 0) {
                if (handleXrun(written) < 0) {
                    emit errorOccurred(m_sessionId, QStringLiteral("ALSA write failed during startup silence: %1").arg(snd_strerror(written)));
                    break;
                }
            }
            m_startupSilenceFrames = 0;
            continue;
        }

        if (m_warmupDiscardFrames > 0) {
            const int discardBufferBytes = static_cast<int>(m_warmupDiscardFrames * m_bufferFormat.bytesPerFrame());
            bool staleRead = false;
            QByteArray discarded = m_buffer->readForOwner(discardBufferBytes, m_sessionId, m_bufferGeneration, &staleRead);
            Q_UNUSED(discarded);
            if (staleRead) {
                continue;
            }
            QByteArray silence(static_cast<int>(m_warmupDiscardFrames * m_frameSize), 0);
            snd_pcm_sframes_t written = snd_pcm_writei(m_handle, silence.constData(), m_warmupDiscardFrames);
            if (written < 0) {
                if (handleXrun(written) < 0) {
                    emit errorOccurred(m_sessionId, QStringLiteral("ALSA write failed during warmup discard: %1").arg(snd_strerror(written)));
                    break;
                }
            }
            m_warmupDiscardFrames = 0;
            continue;
        }

        snd_pcm_sframes_t avail = snd_pcm_avail_update(m_handle);
        if (avail < 0) {
            if (handleXrun(avail) < 0) {
                emit errorOccurred(m_sessionId, QStringLiteral("ALSA XRUN recovery failed: %1").arg(snd_strerror(avail)));
                break;
            }
            continue;
        }

        if (avail == 0) {
            snd_pcm_wait(m_handle, 100);
            continue;
        }

        qint64 deviceBytesToRead = qMin(static_cast<qint64>(avail * m_frameSize), available);
        deviceBytesToRead = (deviceBytesToRead / m_frameSize) * m_frameSize;

        const int bufferFrameSize = m_bufferFormat.bytesPerFrame();
        qint64 bufferBytesToRead = bufferFrameSize > 0
            ? (deviceBytesToRead / m_frameSize) * bufferFrameSize
            : deviceBytesToRead;

        bool staleRead = false;
        QByteArray data = m_buffer->readForOwner(bufferBytesToRead, m_sessionId, m_bufferGeneration, &staleRead);
        if (staleRead || data.isEmpty()) {
            continue;
        }

        if (m_bufferFormat != m_deviceFormat) {
            convertFormat(data);
        }

        applyPcmFadeIn(data);
        if (m_stopFadeOutRequested && m_pcmFadeOutTotalFrames > 0) {
            applyPcmFadeOut(data);
        }
        applyVolume(data);

        int effectiveFrameSize = m_deviceFormat.channelCount
            * AlsaFormatNegotiator::bytesPerSampleForFormat(m_alsaFormat);
        snd_pcm_sframes_t frames = data.size() / effectiveFrameSize;
        snd_pcm_sframes_t written = snd_pcm_writei(m_handle, data.constData(), frames);

        if (written < 0) {
            if (handleXrun(written) < 0) {
                emit errorOccurred(m_sessionId, QStringLiteral("ALSA write failed: %1").arg(snd_strerror(written)));
                break;
            }
            continue;
        }

        m_processedFrames += written;

        emitAudioLevels(data);

        qint64 positionMs = m_processedFrames * 1000 / m_deviceFormat.sampleRate;
        emit positionUpdated(m_sessionId, positionMs);
    }

    emit finished();
}

int AlsaOutputWorker::handleXrun(int error)
{
    if (error == -EPIPE) {
        m_xrunCount++;
        qWarning() << "ALSA UNDERRUN #" << m_xrunCount;
        return snd_pcm_prepare(m_handle);
    } else if (error == -ESTRPIPE) {
        qWarning() << "ALSA device suspended";
        while ((error = snd_pcm_resume(m_handle)) == -EAGAIN) {
            QThread::msleep(100);
        }
        if (error < 0) {
            return snd_pcm_prepare(m_handle);
        }
        return 0;
    }
    return error;
}

void AlsaOutputWorker::convertFormat(QByteArray &data)
{
    if (m_bufferFormat.sampleEncoding == PcmSampleEncoding::Int32
        && m_alsaFormat == SND_PCM_FORMAT_S24_3LE) {
        int samples = data.size() / 4;
        const int32_t *src = reinterpret_cast<const int32_t*>(data.constData());
        QByteArray dst(samples * 3, 0);
        uint8_t *dstPtr = reinterpret_cast<uint8_t*>(dst.data());

        for (int i = 0; i < samples; ++i) {
            int32_t sample = src[i] >> 8;
            dstPtr[i * 3 + 0] = static_cast<uint8_t>(sample & 0xFF);
            dstPtr[i * 3 + 1] = static_cast<uint8_t>((sample >> 8) & 0xFF);
            dstPtr[i * 3 + 2] = static_cast<uint8_t>((sample >> 16) & 0xFF);
        }

        data = dst;
    } else if (m_bufferFormat.sampleEncoding == PcmSampleEncoding::Int32
               && m_alsaFormat == SND_PCM_FORMAT_S16_LE) {
        int samples = data.size() / 4;
        const int32_t *src = reinterpret_cast<const int32_t*>(data.constData());
        QByteArray dst(samples * 2, 0);
        uint8_t *dstPtr = reinterpret_cast<uint8_t*>(dst.data());

        for (int i = 0; i < samples; ++i) {
            int16_t sample = static_cast<int16_t>(src[i] >> 16);
            dstPtr[i * 2 + 0] = static_cast<uint8_t>(sample & 0xFF);
            dstPtr[i * 2 + 1] = static_cast<uint8_t>((sample >> 8) & 0xFF);
        }

        data = dst;
    }
}

void AlsaOutputWorker::applyPcmFadeIn(QByteArray &data)
{
    if (m_pcmFadeTotalFrames <= 0) {
        return;
    }

    const int bytesPerSample = AlsaFormatNegotiator::bytesPerSampleForFormat(m_alsaFormat);
    const int channelCount = m_deviceFormat.channelCount;
    if (bytesPerSample <= 0 || channelCount <= 0) {
        return;
    }

    const int bytesPerFrame = channelCount * bytesPerSample;
    const qsizetype frameCount = data.size() / bytesPerFrame;
    if (frameCount <= 0) {
        return;
    }

    const PcmSampleEncoding encoding = alsaFormatToPcmEncoding(m_alsaFormat);
    if (encoding == PcmSampleEncoding::Unknown) {
        return;
    }

    const qsizetype framesToProcess = qMin(frameCount, m_pcmFadeTotalFrames - m_pcmFadeFramesProcessed);
    char *rawData = data.data();

    for (qsizetype frameIndex = 0; frameIndex < framesToProcess; ++frameIndex) {
        const qreal gain = PcmUtils::computeLinearFadeGain(
            m_pcmFadeFramesProcessed, frameIndex, m_pcmFadeTotalFrames);
        char *frameData = rawData + frameIndex * bytesPerFrame;
        for (int channel = 0; channel < channelCount; ++channel) {
            PcmUtils::applyGainToSample(encoding, frameData + channel * bytesPerSample, gain);
        }
    }

    m_pcmFadeFramesProcessed += framesToProcess;
    if (m_pcmFadeFramesProcessed >= m_pcmFadeTotalFrames) {
        m_pcmFadeTotalFrames = 0;
        m_pcmFadeFramesProcessed = 0;
    }
}

void AlsaOutputWorker::applyPcmFadeOut(QByteArray &data)
{
    if (m_pcmFadeOutTotalFrames <= 0) {
        return;
    }

    const int bytesPerSample = AlsaFormatNegotiator::bytesPerSampleForFormat(m_alsaFormat);
    const int channelCount = m_deviceFormat.channelCount;
    if (bytesPerSample <= 0 || channelCount <= 0) {
        return;
    }

    const int bytesPerFrame = channelCount * bytesPerSample;
    const qsizetype frameCount = data.size() / bytesPerFrame;
    if (frameCount <= 0) {
        return;
    }

    const PcmSampleEncoding encoding = alsaFormatToPcmEncoding(m_alsaFormat);
    if (encoding == PcmSampleEncoding::Unknown) {
        return;
    }

    const qsizetype framesToProcess = qMin(frameCount, m_pcmFadeOutTotalFrames - m_pcmFadeOutFramesProcessed);
    char *rawData = data.data();

    for (qsizetype frameIndex = 0; frameIndex < framesToProcess; ++frameIndex) {
        const qreal progress = static_cast<qreal>(m_pcmFadeOutFramesProcessed + frameIndex + 1)
            / static_cast<qreal>(m_pcmFadeOutTotalFrames);
        const qreal gain = qMax<qreal>(0.0, 1.0 - progress);
        char *frameData = rawData + frameIndex * bytesPerFrame;
        for (int channel = 0; channel < channelCount; ++channel) {
            PcmUtils::applyGainToSample(encoding, frameData + channel * bytesPerSample, gain);
        }
    }

    m_pcmFadeOutFramesProcessed += framesToProcess;
    if (m_pcmFadeOutFramesProcessed >= m_pcmFadeOutTotalFrames) {
        m_pcmFadeOutTotalFrames = 0;
        m_pcmFadeOutFramesProcessed = 0;
    }
}

void AlsaOutputWorker::requestStopFadeOut()
{
    m_stopFadeOutRequested = true;
    if (m_deviceFormat.sampleRate > 0) {
        m_pcmFadeOutTotalFrames = qMax<qsizetype>(1, m_deviceFormat.sampleRate * 80 / 1000);
        m_pcmFadeOutFramesProcessed = 0;
    }
}

void AlsaOutputWorker::applyVolume(QByteArray &data)
{
    if (!m_volumeRampActive && qFuzzyCompare(static_cast<qreal>(m_targetVolume.load()), 1.0)) {
        return;
    }

    const int bytesPerSample = AlsaFormatNegotiator::bytesPerSampleForFormat(m_alsaFormat);
    const int channelCount = m_deviceFormat.channelCount;
    if (bytesPerSample <= 0 || channelCount <= 0) {
        return;
    }

    const int bytesPerFrame = channelCount * bytesPerSample;
    const qsizetype frameCount = data.size() / bytesPerFrame;
    if (frameCount <= 0) {
        return;
    }

    const PcmSampleEncoding encoding = alsaFormatToPcmEncoding(m_alsaFormat);
    if (encoding == PcmSampleEncoding::Unknown) {
        return;
    }

    char *rawData = data.data();
    for (qsizetype frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
        qreal gain = static_cast<qreal>(m_targetVolume.load());
        if (m_volumeRampActive && m_volumeRampTotalFrames > 0) {
            const qreal progress = qMin<qreal>(
                1.0,
                static_cast<qreal>(m_volumeRampFramesProcessed + 1)
                    / static_cast<qreal>(m_volumeRampTotalFrames));
            gain = m_volumeRampStartVolume
                + (static_cast<qreal>(m_targetVolume.load()) - m_volumeRampStartVolume) * progress;
            ++m_volumeRampFramesProcessed;
            if (m_volumeRampFramesProcessed >= m_volumeRampTotalFrames) {
                m_volumeRampActive = false;
                m_volumeRampTotalFrames = 0;
                m_volumeRampFramesProcessed = 0;
            }
            m_currentVolume = gain;
        } else {
            m_currentVolume = m_targetVolume.load();
            gain = m_currentVolume;
        }

        if (qFuzzyCompare(gain, 1.0)) {
            continue;
        }

        char *frameData = rawData + frameIndex * bytesPerFrame;
        for (int channel = 0; channel < channelCount; ++channel) {
            PcmUtils::applyGainToSample(encoding, frameData + channel * bytesPerSample, gain);
        }
    }
}

void AlsaOutputWorker::emitAudioLevels(const QByteArray &data)
{
    if (m_lastLevelEmit.elapsed() < kLevelEmitIntervalMs) {
        return;
    }
    m_lastLevelEmit.restart();

    int channels = m_deviceFormat.channelCount;
    if (channels <= 0) {
        return;
    }

    int bytesPerSample = AlsaFormatNegotiator::bytesPerSampleForFormat(m_alsaFormat);
    int frames = data.size() / (channels * bytesPerSample);
    if (frames <= 0) {
        return;
    }

    qreal leftMax = 0.0;
    qreal rightMax = 0.0;

    const PcmSampleEncoding levelEncoding = alsaFormatToPcmEncoding(m_alsaFormat);

    const char *ptr = data.constData();
    for (int f = 0; f < frames; ++f) {
        for (int ch = 0; ch < channels; ++ch) {
            qreal mag = sampleMagnitude(ptr, levelEncoding);
            if (ch == 0) {
                leftMax = qMax(leftMax, mag);
            } else if (ch == 1) {
                rightMax = qMax(rightMax, mag);
            }
            ptr += bytesPerSample;
        }
    }

    m_lastLeftLevel = leftMax;
    m_lastRightLevel = rightMax;
    emit audioLevelsChanged(m_sessionId, leftMax, rightMax);
}

qreal AlsaOutputWorker::sampleMagnitude(const char *sampleData, PcmSampleEncoding encoding) const
{
    switch (encoding) {
    case PcmSampleEncoding::Int16: {
        int16_t val;
        memcpy(&val, sampleData, sizeof(val));
        return qAbs(static_cast<qreal>(val)) / 32768.0;
    }
    case PcmSampleEncoding::Int24:
    case PcmSampleEncoding::Int32: {
        int32_t val;
        memcpy(&val, sampleData, sizeof(val));
        if (encoding == PcmSampleEncoding::Int24) {
            val >>= 8;
        }
        return qAbs(static_cast<qreal>(val)) / 2147483648.0;
    }
    case PcmSampleEncoding::Float32: {
        float val;
        memcpy(&val, sampleData, sizeof(val));
        return qAbs(static_cast<qreal>(val));
    }
    case PcmSampleEncoding::UInt8: {
        uint8_t val;
        memcpy(&val, sampleData, sizeof(val));
        return qAbs(static_cast<qreal>(val) - 128.0) / 128.0;
    }
    default:
        return 0.0;
    }
}
