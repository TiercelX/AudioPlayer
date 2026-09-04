#include "audiooutputworker.h"
#include "pcmutils.h"
#include "playerlogger.h"

#include <QAudioDevice>
#include <QAudioSink>
#include <QIODevice>
#include <QThread>
#include <QTimer>
#include <QtEndian>

#include <cstring>
#include <limits>

namespace {

constexpr int kPumpIntervalMs = 5;
constexpr int kPositionUpdateIntervalMs = 100;
constexpr int kVolumeRampIntervalMs = 5;
constexpr int kPcmFadeInDurationMs = 8;
constexpr qint64 kMaxReadChunkBytes = 32768;

} // namespace

AudioOutputWorker::AudioOutputWorker(QObject *parent)
    : QObject(parent)
    , m_pumpTimer(new QTimer(this))
    , m_positionTimer(new QTimer(this))
    , m_volumeRampTimer(new QTimer(this))
{
    m_pumpTimer->setInterval(kPumpIntervalMs);
    connect(m_pumpTimer, &QTimer::timeout, this, &AudioOutputWorker::pumpOutput);

    m_positionTimer->setInterval(kPositionUpdateIntervalMs);
    connect(m_positionTimer, &QTimer::timeout, this, [this] {
        if (!m_audioSink || !m_started || m_sessionId == 0) {
            return;
        }

        emit positionUpdated(m_sessionId, m_audioSink->processedUSecs() / 1000);
    });

    m_volumeRampTimer->setInterval(kVolumeRampIntervalMs);
    connect(m_volumeRampTimer, &QTimer::timeout, this, [this] {
        if (!m_audioSink || !m_volumeRampActive) {
            m_volumeRampTimer->stop();
            return;
        }

        ++m_volumeRampStep;
        const qreal progress = qMin<qreal>(1.0,
                                           static_cast<qreal>(m_volumeRampStep)
                                               / static_cast<qreal>(m_volumeRampTotalSteps));
        const qreal rampVolume = m_volumeRampStartVolume
            + (m_targetVolume - m_volumeRampStartVolume) * progress;
        m_audioSink->setVolume(rampVolume);
        if (progress >= 1.0) {
            m_volumeRampActive = false;
            m_volumeRampTimer->stop();
        }
    });
}

void AudioOutputWorker::configureOutput(int sessionId,
                                        const QAudioDevice &device,
                                        const QAudioFormat &format,
                                        qsizetype bufferSize,
                                        qreal volume,
                                        bool startMutedForFadeIn)
{
    PlayerLogger::log(QStringLiteral("audio"),
                      QStringLiteral("configureOutput session=%1 sampleRate=%2 channels=%3 bufferSize=%4 recoveryFadeIn=%5")
                          .arg(sessionId)
                          .arg(format.sampleRate())
                          .arg(format.channelCount())
                          .arg(bufferSize)
                          .arg(startMutedForFadeIn));
    releaseOutput(0, true);

    m_sessionId = sessionId;
    m_audioSink = new QAudioSink(device, format, this);
    m_audioSink->setBufferSize(bufferSize);
    m_outputFormat = format;
    m_targetVolume = volume;
    m_recoveryFadeInEnabled = startMutedForFadeIn;
    m_volumeRampActive = false;
    m_volumeRampStep = 0;
    resetPcmFadeIn(format, startMutedForFadeIn);
    m_audioSink->setVolume(startMutedForFadeIn ? 0.0 : volume);
    connect(m_audioSink, &QAudioSink::stateChanged, this, [this](QAudio::State state) {
        if (state == QAudio::ActiveState && m_started) {
            pumpOutput();
            m_pumpTimer->start();
            m_positionTimer->start();
            startVolumeRampIfNeeded();
        } else if (state == QAudio::IdleState && m_started) {
            pumpOutput();
        } else if (state == QAudio::SuspendedState || state == QAudio::StoppedState) {
            m_pumpTimer->stop();
            m_positionTimer->stop();
            stopVolumeRamp();
        }

        emit stateChanged(m_sessionId,
                          static_cast<int>(state),
                          static_cast<int>(m_audioSink ? m_audioSink->error() : QtAudio::NoError));
    });
    m_started = false;
}

void AudioOutputWorker::startOutput(int sessionId, QIODevice *buffer)
{
    if (!m_audioSink || !buffer || m_started || sessionId != m_sessionId) {
        return;
    }

    PlayerLogger::log(QStringLiteral("audio"),
                      QStringLiteral("startOutput session=%1").arg(sessionId));
    m_buffer = buffer;
    connect(m_buffer, &QIODevice::readyRead, this, &AudioOutputWorker::pumpOutput, Qt::QueuedConnection);

    m_outputDevice = m_audioSink->start();
    if (!m_outputDevice) {
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("startOutput failed session=%1 error=%2")
                              .arg(sessionId)
                              .arg(static_cast<int>(m_audioSink->error())));
        emit stateChanged(m_sessionId,
                          static_cast<int>(QAudio::StoppedState),
                          static_cast<int>(m_audioSink->error()));
        return;
    }

    m_started = true;
    startVolumeRampIfNeeded();
    m_pumpTimer->start();
    pumpOutput();
    emit positionUpdated(m_sessionId, m_audioSink->processedUSecs() / 1000);
}

void AudioOutputWorker::pauseOutput(int sessionId)
{
    if (!m_audioSink || !m_started || sessionId != m_sessionId) {
        return;
    }

    PlayerLogger::log(QStringLiteral("audio"),
                      QStringLiteral("pauseOutput session=%1").arg(sessionId));
    m_audioSink->suspend();
    m_pumpTimer->stop();
    m_positionTimer->stop();
    emit positionUpdated(m_sessionId, m_audioSink->processedUSecs() / 1000);
}

void AudioOutputWorker::resumeOutput(int sessionId)
{
    if (!m_audioSink || !m_started || sessionId != m_sessionId) {
        return;
    }

    PlayerLogger::log(QStringLiteral("audio"),
                      QStringLiteral("resumeOutput session=%1").arg(sessionId));
    m_audioSink->resume();
    m_pumpTimer->start();
    pumpOutput();
    m_positionTimer->start();
}

void AudioOutputWorker::releaseOutput(int sessionId, bool immediateReset)
{
    if (sessionId != 0 && sessionId != m_sessionId) {
        return;
    }

    const int releasedSessionId = m_sessionId;
    PlayerLogger::log(QStringLiteral("audio"),
                      QStringLiteral("releaseOutput begin requestSession=%1 currentSession=%2 immediateReset=%3")
                          .arg(sessionId)
                          .arg(releasedSessionId)
                          .arg(immediateReset));
    m_pumpTimer->stop();
    m_positionTimer->stop();
    fadeOutCurrentOutput();
    stopVolumeRamp();
    if (m_buffer) {
        disconnect(m_buffer, nullptr, this, nullptr);
        m_buffer = nullptr;
    }
    m_outputDevice = nullptr;
    m_pendingOutput.clear();
    m_pendingOutputOffset = 0;
    if (m_audioSink) {
        disconnect(m_audioSink, nullptr, this, nullptr);
        if (immediateReset) {
            m_audioSink->reset();
        } else {
            m_audioSink->stop();
        }
        m_audioSink->deleteLater();
        m_audioSink = nullptr;
    }

    m_started = false;
    m_sessionId = 0;
    m_outputFormat = {};
    m_pcmFadeTotalFrames = 0;
    m_pcmFadeFramesProcessed = 0;
    PlayerLogger::log(QStringLiteral("audio"),
                      QStringLiteral("releaseOutput done releasedSession=%1").arg(releasedSessionId));
    emit released(releasedSessionId);
}

void AudioOutputWorker::setVolume(qreal volume)
{
    const qreal clampedVolume = qBound<qreal>(0.0, volume, 1.0);
    if (qFuzzyCompare(m_targetVolume, clampedVolume)) {
        return;
    }

    if (m_audioSink && m_started) {
        const qreal startVolume = m_audioSink->volume();
        if (!qFuzzyCompare(startVolume, clampedVolume)) {
            m_volumeRampStep = 0;
            m_volumeRampStartVolume = startVolume;
            m_targetVolume = clampedVolume;
            m_volumeRampActive = true;
            m_volumeRampTimer->start();
            PlayerLogger::log(QStringLiteral("audio"),
                              QStringLiteral("setVolume ramp session=%1 start=%2 target=%3")
                                  .arg(m_sessionId)
                                  .arg(QString::number(startVolume, 'f', 3))
                                  .arg(QString::number(clampedVolume, 'f', 3)));
            return;
        }
    }

    m_targetVolume = clampedVolume;
    if (m_audioSink) {
        m_audioSink->setVolume(clampedVolume);
    }
}

void AudioOutputWorker::resetPcmFadeIn(const QAudioFormat &format, bool enabled)
{
    if (!enabled || !format.isValid() || format.sampleRate() <= 0 || format.bytesPerFrame() <= 0) {
        m_pcmFadeTotalFrames = 0;
        m_pcmFadeFramesProcessed = 0;
        return;
    }

    m_pcmFadeTotalFrames = qMax<qsizetype>(1, format.sampleRate() * kPcmFadeInDurationMs / 1000);
    m_pcmFadeFramesProcessed = 0;
    PlayerLogger::log(QStringLiteral("audio"),
                      QStringLiteral("resetPcmFadeIn session=%1 totalFrames=%2 sampleRate=%3")
                          .arg(m_sessionId)
                          .arg(m_pcmFadeTotalFrames)
                          .arg(format.sampleRate()));
}

void AudioOutputWorker::applyPcmFadeIn(QByteArray &chunk)
{
    if (m_pcmFadeTotalFrames <= 0 || !m_outputFormat.isValid()) {
        return;
    }

    const int bytesPerFrame = m_outputFormat.bytesPerFrame();
    const int channelCount = m_outputFormat.channelCount();
    const int bytesPerSample = m_outputFormat.bytesPerSample();
    if (bytesPerFrame <= 0 || channelCount <= 0 || bytesPerSample <= 0) {
        m_pcmFadeTotalFrames = 0;
        m_pcmFadeFramesProcessed = 0;
        return;
    }

    const PcmSampleEncoding encoding = PcmUtils::fromQAudioSampleFormat(m_outputFormat.sampleFormat());
    if (encoding == PcmSampleEncoding::Unknown) {
        m_pcmFadeTotalFrames = 0;
        m_pcmFadeFramesProcessed = 0;
        return;
    }

    const qsizetype frameCount = chunk.size() / bytesPerFrame;
    if (frameCount <= 0) {
        return;
    }

    const qsizetype framesToProcess = qMin(frameCount, m_pcmFadeTotalFrames - m_pcmFadeFramesProcessed);
    if (framesToProcess <= 0) {
        m_pcmFadeTotalFrames = 0;
        m_pcmFadeFramesProcessed = 0;
        return;
    }

    char *data = chunk.data();
    for (qsizetype frameIndex = 0; frameIndex < framesToProcess; ++frameIndex) {
        const qreal gain = PcmUtils::computeLinearFadeGain(m_pcmFadeFramesProcessed, frameIndex, m_pcmFadeTotalFrames);
        char *frameData = data + frameIndex * bytesPerFrame;
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

void AudioOutputWorker::fadeOutCurrentOutput()
{
    if (!m_audioSink || !m_started) {
        return;
    }

    stopVolumeRamp();
    const qreal startVolume = m_audioSink->volume();
    if (startVolume <= 0.0) {
        return;
    }

    constexpr int kFadeOutSteps = 4;
    constexpr unsigned long kFadeOutStepDelayMs = 3;
    PlayerLogger::log(QStringLiteral("audio"),
                      QStringLiteral("fadeOutCurrentOutput session=%1 startVolume=%2")
                          .arg(m_sessionId)
                          .arg(QString::number(startVolume, 'f', 3)));
    for (int step = kFadeOutSteps - 1; step >= 0; --step) {
        const qreal progress = static_cast<qreal>(step) / static_cast<qreal>(kFadeOutSteps);
        m_audioSink->setVolume(startVolume * progress);
        QThread::msleep(kFadeOutStepDelayMs);
    }
    m_audioSink->setVolume(0.0);
}

void AudioOutputWorker::startVolumeRampIfNeeded()
{
    if (!m_audioSink || !m_recoveryFadeInEnabled) {
        return;
    }

    m_recoveryFadeInEnabled = false;
    m_volumeRampStep = 0;
    if (m_targetVolume <= 0.0) {
        m_audioSink->setVolume(m_targetVolume);
        return;
    }

    m_volumeRampActive = true;
    m_volumeRampStartVolume = 0.0;
    m_audioSink->setVolume(0.0);
    m_volumeRampTimer->start();
    PlayerLogger::log(QStringLiteral("audio"),
                      QStringLiteral("startVolumeRamp session=%1 targetVolume=%2 stepCount=%3")
                          .arg(m_sessionId)
                          .arg(QString::number(m_targetVolume, 'f', 3))
                          .arg(m_volumeRampTotalSteps));
}

void AudioOutputWorker::stopVolumeRamp()
{
    m_volumeRampTimer->stop();
    m_volumeRampActive = false;
    m_volumeRampStep = 0;
    m_volumeRampStartVolume = 0.0;
    m_recoveryFadeInEnabled = false;
}

void AudioOutputWorker::pumpOutput()
{
    if (!m_audioSink || !m_outputDevice || !m_buffer || !m_started) {
        return;
    }

    while (true) {
        if (!m_pendingOutput.isEmpty() && m_pendingOutputOffset < m_pendingOutput.size()) {
            const qint64 bytesFree = m_audioSink->bytesFree();
            if (bytesFree <= 0) {
                return;
            }

            const qint64 remaining = m_pendingOutput.size() - m_pendingOutputOffset;
            const qint64 bytesToWrite = qMin(bytesFree, remaining);
            const qint64 written = m_outputDevice->write(m_pendingOutput.constData() + m_pendingOutputOffset,
                                                         bytesToWrite);
            if (written <= 0) {
                return;
            }

            m_pendingOutputOffset += static_cast<int>(written);
            if (m_pendingOutputOffset < m_pendingOutput.size()) {
                return;
            }

            m_pendingOutput.clear();
            m_pendingOutputOffset = 0;
            continue;
        }

        const qint64 bytesFree = m_audioSink->bytesFree();
        if (bytesFree <= 0) {
            return;
        }

        QByteArray chunk = m_buffer->read(static_cast<qint64>(qMin<qint64>(bytesFree, kMaxReadChunkBytes)));
        if (chunk.isEmpty()) {
            return;
        }

        applyPcmFadeIn(chunk);

        const qint64 written = m_outputDevice->write(chunk);
        if (written < 0) {
            return;
        }

        if (written < chunk.size()) {
            m_pendingOutput = chunk;
            m_pendingOutputOffset = static_cast<int>(written);
            return;
        }
    }
}
