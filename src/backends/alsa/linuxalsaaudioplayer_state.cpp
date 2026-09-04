#include "linuxalsaaudioplayer.h"

#include "alsalogic.h"
#include "alsaformatnegotiator.h"
#include "alsaoutputworker.h"
#include "audioplayerfactory.h"
#include "ffmpegpcmshared.h"
#include "libavseekdecoderworker.h"
#include "pcmseekcache.h"
#include "playbacksourceservice.h"
#include "playerlogger.h"

#include <QAudioDevice>
#include <QFileInfo>
#include <QMediaDevices>
#include <QThread>
#include <QTimer>

namespace {
constexpr int kMaxOutputRecoveryAttempts = 3;
constexpr int kOutputRecoveryBaseDelayMs = 250;
constexpr int kDefaultStartupThresholdMs = 200;
constexpr int kSeekResumeStartupThresholdMs = 100;
}

qsizetype LinuxAlsaAudioPlayer::startupThresholdBytes(PipelineStartupProfile profile) const
{
    return AlsaLogic::startupThresholdBytes(m_decoderPcmFormat, static_cast<int>(profile));
}

QString LinuxAlsaAudioPlayer::pipelineStartupProfileName(PipelineStartupProfile profile) const
{
    switch (profile) {
    case PipelineStartupProfile::NormalStart:
        return QStringLiteral("NormalStart");
    case PipelineStartupProfile::SeekResume:
        return QStringLiteral("SeekResume");
    case PipelineStartupProfile::ActiveSwitchRebuild:
        return QStringLiteral("ActiveSwitchRebuild");
    case PipelineStartupProfile::ErrorRecovery:
        return QStringLiteral("ErrorRecovery");
    }
    return QStringLiteral("Unknown");
}

QString LinuxAlsaAudioPlayer::activeOutputSwitchTriggerName(ActiveOutputSwitchTrigger trigger) const
{
    switch (trigger) {
    case ActiveOutputSwitchTrigger::None:
        return QStringLiteral("None");
    case ActiveOutputSwitchTrigger::DeviceSelection:
        return QStringLiteral("DeviceSelection");
    case ActiveOutputSwitchTrigger::OutputRefresh:
        return QStringLiteral("OutputRefresh");
    case ActiveOutputSwitchTrigger::SystemDeviceChange:
        return QStringLiteral("SystemDeviceChange");
    }
    return QStringLiteral("Unknown");
}

void LinuxAlsaAudioPlayer::logPlaybackStateChange(PlaybackState from, PlaybackState to)
{
    Q_UNUSED(from);
    PlayerLogger::log(QStringLiteral("alsa"),
                      QStringLiteral("playbackStateChanged state=%1 source=%2")
                          .arg(static_cast<int>(to))
                          .arg(m_sourcePath));
}

QString LinuxAlsaAudioPlayer::rawInputFormatForSource() const
{
    return AlsaLogic::rawInputFormatForPath(m_sourcePath);
}

void LinuxAlsaAudioPlayer::finalizePlayback()
{
    PlayerLogger::log(QStringLiteral("alsa"),
                      QStringLiteral("finalizePlayback source=%1").arg(m_sourcePath));
    teardownPipeline();
    setPlaybackState(PlaybackState::Stopped);
    emit finished();
}

void LinuxAlsaAudioPlayer::scheduleOutputRecovery()
{
    if (m_outputRecoveryAttempt >= kMaxOutputRecoveryAttempts) {
        PlayerLogger::log(QStringLiteral("alsa"),
                          QStringLiteral("outputRecoveryExhausted attempts=%1").arg(m_outputRecoveryAttempt));
        teardownPipeline();
        setPlaybackState(PlaybackState::Stopped);
        emit errorOccurred(PlaybackError::OutputRecoveryFailure,
                           tr("ALSA output recovery failed after %1 attempts").arg(m_outputRecoveryAttempt));
        return;
    }

    m_outputRecoveryAttempt++;
    int delayMs = kOutputRecoveryBaseDelayMs * m_outputRecoveryAttempt;

    PlayerLogger::log(QStringLiteral("alsa"),
                      QStringLiteral("scheduleOutputRecovery attempt=%1 delay=%2ms")
                          .arg(m_outputRecoveryAttempt)
                          .arg(delayMs));

    PlayerLogger::diagnostic(QStringLiteral("alsa"),
                             QStringLiteral("output_recovery_scheduled"),
                             {
                                 {QStringLiteral("sessionId"), m_decoderSessionId},
                                 {QStringLiteral("positionMs"), m_outputRecoveryPositionMs},
                                 {QStringLiteral("attempt"), m_outputRecoveryAttempt},
                             });

    QTimer::singleShot(delayMs, this, [this]() {
        if (m_playbackState != PlaybackState::Playing) {
            return;
        }

        startPipeline(m_outputRecoveryPositionMs, PipelineStartupProfile::ErrorRecovery);
    });
}

void LinuxAlsaAudioPlayer::resetOutputRecoveryState()
{
    m_outputRecoveryPending = false;
    m_outputRecoveryAttempt = 0;
    m_outputRecoveryPositionMs = 0;
    m_outputRecoveryGeneration = 0;
}

void LinuxAlsaAudioPlayer::handleDecoderDataAvailable(int sessionId)
{
    if (sessionId != m_decoderSessionId) {
        return;
    }
}

void LinuxAlsaAudioPlayer::handleDecoderError(int sessionId, const QString &message)
{
    if (sessionId != m_decoderSessionId) {
        return;
    }

    PlayerLogger::log(QStringLiteral("alsa"),
                      QStringLiteral("decoderError session=%1 message=%2")
                          .arg(sessionId)
                          .arg(message));
    PlayerLogger::diagnostic(QStringLiteral("alsa"),
                             QStringLiteral("decoder_error"),
                             {
                                 {QStringLiteral("sessionId"), sessionId},
                                 {QStringLiteral("message"), message},
                             });
    emit errorOccurred(PlaybackError::DecoderError, message);
}

void LinuxAlsaAudioPlayer::handleDecoderFinished(int sessionId, int exitCode, int exitStatus, const QString &stderrText)
{
    Q_UNUSED(exitStatus);
    Q_UNUSED(stderrText);

    if (sessionId != m_decoderSessionId) {
        return;
    }

    m_decoderFinished = true;

    PlayerLogger::log(QStringLiteral("alsa"),
                      QStringLiteral("decoderFinished session=%1 exitCode=%2")
                          .arg(sessionId)
                          .arg(exitCode));

    if (exitCode != 0) {
        if (m_buffer) {
            m_buffer->setEndOfStream(true);
        }
        emit errorOccurred(PlaybackError::DecoderError,
                           tr("FFmpeg decoder exited with code %1").arg(exitCode));
        return;
    }

    if (m_buffer) {
        m_buffer->setEndOfStream(true);
    }
}

void LinuxAlsaAudioPlayer::releaseOutputResources()
{
    PlayerLogger::diagnostic(QStringLiteral("alsa"),
                             QStringLiteral("output_release"),
                             {
                                 {QStringLiteral("sessionId"), m_decoderSessionId},
                                 {QStringLiteral("processedFrames"), m_outputWorker ? QStringLiteral("pending") : QStringLiteral("none")},
                             });

    if (m_outputWorker) {
        m_outputWorker->requestStopFadeOut();
        QThread::msleep(100);
        m_outputWorker->stop();
    }

    if (m_pcmHandle) {
        snd_pcm_drop(m_pcmHandle);
    }
}

void LinuxAlsaAudioPlayer::clearBufferDevice()
{
    if (m_buffer) {
        m_buffer->clear();
        m_buffer->setDiscardWrites(false);
    }
}

void LinuxAlsaAudioPlayer::stopDecoderWorker(bool waitForFinished)
{
    if (m_decoderWorker && m_decoderThread) {
        QMetaObject::invokeMethod(m_decoderWorker, [worker = m_decoderWorker, waitForFinished]() {
            worker->stopDecoding(waitForFinished);
        }, Qt::BlockingQueuedConnection);
    }
    if (m_libavSeekDecoderWorker && m_decoderThread) {
        QMetaObject::invokeMethod(m_libavSeekDecoderWorker, [worker = m_libavSeekDecoderWorker, waitForFinished]() {
            worker->stopDecoding(waitForFinished);
        }, Qt::BlockingQueuedConnection);
    }
}

void LinuxAlsaAudioPlayer::teardownPipeline()
{
    if (m_tearingDown) {
        return;
    }
    m_tearingDown = true;

    PlayerLogger::log(QStringLiteral("alsa"),
                      QStringLiteral("teardownPipeline source=%1").arg(m_sourcePath));

    PlayerLogger::diagnostic(QStringLiteral("alsa"),
                             QStringLiteral("pipeline_teardown"),
                             {
                                 {QStringLiteral("source"), m_sourcePath},
                                 {QStringLiteral("sessionId"), m_decoderSessionId},
                             });

    releaseOutputResources();

    if (m_outputThread) {
        m_outputThread->quit();
        m_outputThread->wait(1000);
        delete m_outputThread;
        m_outputThread = nullptr;
        m_outputWorker = nullptr;
    }

    stopDecoderWorker(false);

    if (m_decoderThread) {
        m_decoderThread->quit();
        m_decoderThread->wait(1000);
        delete m_decoderThread;
        m_decoderThread = nullptr;
        m_decoderWorker = nullptr;
        m_libavSeekDecoderWorker = nullptr;
    }

    clearBufferDevice();

    m_bufferGeneration++;
    m_audioStarted = false;
    m_decoderFinished = false;
    m_tearingDown = false;
}

bool LinuxAlsaAudioPlayer::reconfigureAlsaDevice()
{
    if (!m_pcmHandle) {
        return false;
    }

    snd_pcm_hw_params_t *params;
    snd_pcm_hw_params_alloca(&params);
    snd_pcm_hw_params_any(m_pcmHandle, params);
    snd_pcm_hw_params_set_access(m_pcmHandle, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(m_pcmHandle, params, m_alsaFormat);
    snd_pcm_hw_params_set_rate(m_pcmHandle, params, m_outputPcmFormat.sampleRate, 0);
    snd_pcm_hw_params_set_channels(m_pcmHandle, params, m_outputPcmFormat.channelCount);

    int err = snd_pcm_hw_params(m_pcmHandle, params);
    if (err < 0) {
        PlayerLogger::log(QStringLiteral("alsa"),
                          QStringLiteral("reconfigureAlsaDevice hw_params failed: %1")
                              .arg(snd_strerror(err)));
        return false;
    }

    err = snd_pcm_prepare(m_pcmHandle);
    if (err < 0) {
        PlayerLogger::log(QStringLiteral("alsa"),
                          QStringLiteral("reconfigureAlsaDevice prepare failed: %1")
                              .arg(snd_strerror(err)));
        return false;
    }

    return true;
}

bool LinuxAlsaAudioPlayer::seekWhilePlaying(qint64 positionMs)
{
    if (!m_pcmHandle || !m_outputWorker) {
        return false;
    }

    PlayerLogger::diagnostic(QStringLiteral("alsa"),
                             QStringLiteral("seek_started"),
                             {
                                 {QStringLiteral("targetMs"), positionMs},
                                 {QStringLiteral("sessionId"), m_decoderSessionId},
                             });

    PlayerLogger::log(QStringLiteral("alsa"),
                      QStringLiteral("seekWhilePlaying targetMs=%1 session=%2")
                          .arg(positionMs)
                          .arg(m_decoderSessionId));

    snd_pcm_drop(m_pcmHandle);

    if (!reconfigureAlsaDevice()) {
        PlayerLogger::log(QStringLiteral("alsa"),
                          QStringLiteral("seekWhilePlaying reconfigure failed, falling back to teardown"));
        return false;
    }

    if (m_outputWorker) {
        m_outputWorker->stop();
        QThread::msleep(20);
    }

    if (m_outputThread) {
        m_outputThread->quit();
        m_outputThread->wait(500);
        delete m_outputThread;
        m_outputThread = nullptr;
        m_outputWorker = nullptr;
    }

    stopDecoderWorker(false);
    if (m_decoderThread) {
        m_decoderThread->quit();
        m_decoderThread->wait(500);
        delete m_decoderThread;
        m_decoderThread = nullptr;
        m_decoderWorker = nullptr;
    }

    if (!m_libavSeekDecoderWorker) {
        m_decoderSessionId++;
    }
    m_bufferGeneration++;
    if (m_buffer) {
        m_buffer->clear();
        m_buffer->setDiscardWrites(false);
        m_buffer->setEndOfStream(false);
        m_buffer->setOwner(m_decoderSessionId, m_bufferGeneration, m_sourcePath);
    }

    m_outputThread = new QThread(this);
    m_outputWorker = new AlsaOutputWorker();
    m_outputWorker->moveToThread(m_outputThread);

    m_outputWorker->configure(
        m_pcmHandle,
        m_buffer,
        m_decoderPcmFormat,
        m_outputPcmFormat,
        m_alsaFormat,
        m_decoderSessionId,
        m_bufferGeneration,
        true,
        startupThresholdBytes(PipelineStartupProfile::SeekResume),
        qMax<qsizetype>(1, m_outputPcmFormat.sampleRate * 8 / 1000),
        qMax<qsizetype>(1, m_outputPcmFormat.sampleRate * 8 / 1000));

    connect(m_outputWorker, &AlsaOutputWorker::audioLevelsChanged,
            this, [this](int sessionId, qreal left, qreal right) {
                if (sessionId == m_decoderSessionId) {
                    emitAudioLevels(left, right);
                }
            });

    connect(m_outputWorker, &AlsaOutputWorker::positionUpdated,
            this, [this](int sessionId, qint64 posMs) {
                if (sessionId == m_decoderSessionId) {
                    emit positionChanged(posMs);
                }
            });

    connect(m_outputWorker, &AlsaOutputWorker::errorOccurred,
            this, [this](int sessionId, const QString &message) {
                if (sessionId == m_decoderSessionId) {
                    PlayerLogger::log(QStringLiteral("alsa"),
                                      QStringLiteral("outputError session=%1 message=%2")
                                          .arg(sessionId)
                                          .arg(message));
                    scheduleOutputRecovery();
                }
            });

    connect(m_outputWorker, &AlsaOutputWorker::finished,
            this, [this]() {
                if (m_decoderFinished) {
                    finalizePlayback();
                }
            });

    connect(m_outputThread, &QThread::started, m_outputWorker, &AlsaOutputWorker::start);
    m_outputThread->start();

    m_decoderFinished = false;

    if (m_libavSeekDecoderWorker) {
        m_libavSeekDecoderWorker->seekTo(
            m_decoderSessionId, positionMs, m_buffer, m_bufferGeneration);
    } else {
        m_decoderThread = new QThread(this);
        m_decoderWorker = new FfmpegDecoderWorker();
        m_decoderWorker->moveToThread(m_decoderThread);

        connect(m_decoderWorker, &FfmpegDecoderWorker::dataAvailable,
                this, &LinuxAlsaAudioPlayer::handleDecoderDataAvailable);
        connect(m_decoderWorker, &FfmpegDecoderWorker::errorOccurred,
                this, &LinuxAlsaAudioPlayer::handleDecoderError);
        connect(m_decoderWorker, &FfmpegDecoderWorker::finished,
                this, &LinuxAlsaAudioPlayer::handleDecoderFinished);
        connect(m_decoderWorker, &FfmpegDecoderWorker::audioLevelsChanged,
                this, [this](int sessionId, qreal left, qreal right) {
                    if (sessionId == m_decoderSessionId) {
                        emitAudioLevels(left, right);
                    }
                });

        int sessId = m_decoderSessionId;
        PcmStreamFormat decFormat = m_decoderPcmFormat;
        QString srcPath = m_sourcePath;
        QString srcCodecName = m_sourceCodecName;
        int srcChannelCount = m_sourceChannelCount;
        int outputChannelCount = m_outputPcmFormat.channelCount;
        connect(m_decoderThread, &QThread::started, this,
                [this, sessId, positionMs, decFormat, srcPath, srcCodecName, srcChannelCount, outputChannelCount]() {
                    QString ffmpegPath = locateFfmpegExecutable();
                    QString channelLayout = channelLayoutForCount(decFormat.channelCount);
                    const QString rawInputFormat = rawInputFormatForSource();
                    QStringList args;

                    const QString downmixLayout = channelLayout;
                    if (srcChannelCount > 0 && srcChannelCount > outputChannelCount
                        && !downmixLayout.isEmpty()
                        && (srcCodecName == QStringLiteral("truehd")
                            || srcCodecName == QStringLiteral("eac3")
                            || srcCodecName == QStringLiteral("ac3")
                            || !rawInputFormat.isEmpty())) {
                        args << QStringLiteral("-downmix") << downmixLayout;
                    }

                    if (!rawInputFormat.isEmpty()) {
                        args << QStringLiteral("-f") << rawInputFormat;
                    }

                    args << QStringLiteral("-ss")
                         << QString::number(positionMs / 1000.0, 'f', 3)
                         << QStringLiteral("-i") << srcPath
                         << QStringLiteral("-f")
                         << QStringLiteral("s%1le").arg(decFormat.validBitsPerSample)
                         << QStringLiteral("-acodec")
                         << QStringLiteral("pcm_s%1le").arg(decFormat.validBitsPerSample)
                         << QStringLiteral("-ar")
                         << QString::number(decFormat.sampleRate)
                         << QStringLiteral("-ac")
                         << QString::number(decFormat.channelCount);

                    if (!channelLayout.isEmpty()) {
                        args << QStringLiteral("-channel_layout") << channelLayout;
                    }

                    args << QStringLiteral("pipe:1");

                    m_decoderWorker->startDecoding(
                        sessId, ffmpegPath, args, m_buffer, decFormat);
                });

        m_decoderThread->start();
    }

    m_outputRecoveryPositionMs = positionMs;
    m_outputRecoveryGeneration = m_bufferGeneration;
    resetOutputRecoveryState();

    emit positionChanged(positionMs);

    return true;
}

void LinuxAlsaAudioPlayer::startPipeline(qint64 startPositionMs,
                                          PipelineStartupProfile profile)
{
    PlayerLogger::log(QStringLiteral("alsa"),
                      QStringLiteral("startPipeline source=%1 position=%2ms profile=%3")
                          .arg(m_sourcePath)
                          .arg(startPositionMs)
                          .arg(pipelineStartupProfileName(profile)));

    m_decoderSessionId++;
    m_bufferGeneration++;

    QAudioFormat format = selectOutputFormat(&m_outputPcmFormat);
    if (!format.isValid()) {
        emit errorOccurred(PlaybackError::FormatNotSupported, tr("Failed to select output format"));
        return;
    }

    m_outputFormat = format;
    m_decoderPcmFormat = decoderFormatForOutput(m_outputPcmFormat);

    bool usesDefault = false;
    QAudioDevice device = resolveOutputDevice(&usesDefault);
    m_outputDeviceDescription = device.description();

    bool exclusive = m_exclusiveModeEnabled;
    QString deviceId = usesDefault ? (exclusive ? QStringLiteral("hw:0") : QStringLiteral("default"))
                                   : QString::fromUtf8(m_selectedOutputDeviceId);

    if (profile == PipelineStartupProfile::SeekResume && m_pcmHandle) {
        snd_pcm_drop(m_pcmHandle);

        if (!reconfigureAlsaDevice()) {
            closeAlsaDevice();
            m_pcmHandle = openAlsaDevice(deviceId, exclusive);
            if (!m_pcmHandle && exclusive) {
                QString plugDevice = deviceId;
                if (plugDevice.startsWith(QStringLiteral("hw:"))) {
                    plugDevice = QStringLiteral("plughw:") + plugDevice.mid(3);
                } else {
                    plugDevice = QStringLiteral("plughw:0");
                }
                m_pcmHandle = openAlsaDevice(plugDevice, false);
                if (!m_pcmHandle) {
                    m_pcmHandle = openAlsaDevice(QStringLiteral("default"), false);
                }
            }
            if (!m_pcmHandle) {
                emit errorOccurred(PlaybackError::DeviceNotFound, tr("Failed to open ALSA device"));
                return;
            }
            exclusive = false;

            auto negotiationResult = AlsaFormatNegotiator::negotiate(
                m_pcmHandle, m_sourceSampleRate, m_sourceBitDepth,
                m_sourceChannelCount, exclusive, m_exactPlaybackEnabled);
            if (!negotiationResult.success) {
                closeAlsaDevice();
                emit errorOccurred(PlaybackError::FormatNotSupported, tr("ALSA format negotiation failed"));
                return;
            }

            m_alsaFormat = negotiationResult.alsaFormat;
            m_outputFormat = negotiationResult.qtFormat;
            m_outputPcmFormat.sampleRate = negotiationResult.actualRate;
            m_outputPcmFormat.channelCount = negotiationResult.actualChannels;
            m_decoderPcmFormat = decoderFormatForOutput(m_outputPcmFormat);

            snd_pcm_hw_params_t *params;
            snd_pcm_hw_params_alloca(&params);
            snd_pcm_hw_params_any(m_pcmHandle, params);
            snd_pcm_hw_params_set_access(m_pcmHandle, params, SND_PCM_ACCESS_RW_INTERLEAVED);
            snd_pcm_hw_params_set_format(m_pcmHandle, params, m_alsaFormat);
            snd_pcm_hw_params_set_rate(m_pcmHandle, params, negotiationResult.actualRate, 0);
            snd_pcm_hw_params_set_channels(m_pcmHandle, params, negotiationResult.actualChannels);
            snd_pcm_hw_params(m_pcmHandle, params);
            snd_pcm_prepare(m_pcmHandle);
        }
    } else {
        m_pcmHandle = openAlsaDevice(deviceId, exclusive);
        if (!m_pcmHandle && exclusive) {
            QString plugDevice = deviceId;
            if (plugDevice.startsWith(QStringLiteral("hw:"))) {
                plugDevice = QStringLiteral("plughw:") + plugDevice.mid(3);
            } else {
                plugDevice = QStringLiteral("plughw:0");
            }
            m_pcmHandle = openAlsaDevice(plugDevice, false);
            if (!m_pcmHandle) {
                m_pcmHandle = openAlsaDevice(QStringLiteral("default"), false);
            }
        }
        if (!m_pcmHandle) {
            emit errorOccurred(PlaybackError::DeviceNotFound, tr("Failed to open ALSA device"));
            return;
        }
        exclusive = false;

        auto negotiationResult = AlsaFormatNegotiator::negotiate(
            m_pcmHandle,
            m_sourceSampleRate,
            m_sourceBitDepth,
            m_sourceChannelCount,
            exclusive,
            m_exactPlaybackEnabled);

        if (!negotiationResult.success) {
            closeAlsaDevice();
            emit errorOccurred(PlaybackError::FormatNotSupported, tr("ALSA format negotiation failed"));
            return;
        }

        m_alsaFormat = negotiationResult.alsaFormat;
        m_outputFormat = negotiationResult.qtFormat;
        m_outputPcmFormat.sampleRate = negotiationResult.actualRate;
        m_outputPcmFormat.channelCount = negotiationResult.actualChannels;
        m_decoderPcmFormat = decoderFormatForOutput(m_outputPcmFormat);

        snd_pcm_hw_params_t *params;
        snd_pcm_hw_params_alloca(&params);
        snd_pcm_hw_params_any(m_pcmHandle, params);
        snd_pcm_hw_params_set_access(m_pcmHandle, params, SND_PCM_ACCESS_RW_INTERLEAVED);
        snd_pcm_hw_params_set_format(m_pcmHandle, params, m_alsaFormat);
        snd_pcm_hw_params_set_rate(m_pcmHandle, params, negotiationResult.actualRate, 0);
        snd_pcm_hw_params_set_channels(m_pcmHandle, params, negotiationResult.actualChannels);
        snd_pcm_hw_params(m_pcmHandle, params);
        snd_pcm_prepare(m_pcmHandle);
    }

    PlayerLogger::log(QStringLiteral("alsa"),
                      QStringLiteral("formatNegotiated rate=%1 channels=%2 format=%3 exclusive=%4 profile=%5")
                          .arg(m_outputPcmFormat.sampleRate)
                          .arg(m_outputPcmFormat.channelCount)
                          .arg(AlsaFormatNegotiator::bytesPerSampleForFormat(m_alsaFormat) * 8)
                          .arg(exclusive)
                          .arg(pipelineStartupProfileName(profile)));

    PlayerLogger::diagnostic(QStringLiteral("alsa"),
                             QStringLiteral("pipeline_started"),
                             {
                                 {QStringLiteral("profile"), pipelineStartupProfileName(profile)},
                                 {QStringLiteral("sourceRate"), m_sourceSampleRate},
                                 {QStringLiteral("sourceBitDepth"), m_sourceBitDepth},
                                 {QStringLiteral("sourceChannels"), m_sourceChannelCount},
                                 {QStringLiteral("outputRate"), m_outputPcmFormat.sampleRate},
                                 {QStringLiteral("outputChannels"), m_outputPcmFormat.channelCount},
                                 {QStringLiteral("alsaFormat"), AlsaFormatNegotiator::bytesPerSampleForFormat(m_alsaFormat) * 8},
                                 {QStringLiteral("exclusive"), m_exclusiveModeEnabled},
                                 {QStringLiteral("device"), m_outputDeviceDescription},
                             });

    if (!m_buffer) {
        m_buffer = new PcmStreamBuffer(this);
    }
    const qsizetype bytesPerHalfSecond = qMax<qsizetype>(
        65536,
        static_cast<qsizetype>(m_outputPcmFormat.bytesPerFrame())
            * m_outputPcmFormat.sampleRate / 2);
    m_buffer->setMaxSize(bytesPerHalfSecond * 6);
    m_buffer->setOwner(m_decoderSessionId, m_bufferGeneration, m_sourcePath);
    m_buffer->setDiscardWrites(false);
    m_buffer->setEndOfStream(false);
    m_buffer->clear();

    if (m_libavSeekDecoderWorker && !m_pcmSeekCache) {
        m_pcmSeekCache = new PcmSeekCache;
        const PlaybackCacheSettings cacheSettings = PlaybackSourceService().cacheSettings();
        const bool rawDolbySource =
            !rawInputFormatForSource().isEmpty()
            || m_sourceCodecName == QStringLiteral("truehd")
            || m_sourceCodecName == QStringLiteral("eac3")
            || m_sourceCodecName == QStringLiteral("ac3");
        const int maxPcmCacheMiB =
            (!cacheSettings.maxPcmCacheMiBConfigured && rawDolbySource)
                ? 64
                : cacheSettings.maxPcmCacheMiB;
        m_pcmSeekCache->initialize(m_sourcePath, m_decoderPcmFormat,
                                   cacheSettings.cacheDirectory,
                                   maxPcmCacheMiB,
                                   cacheSettings.maxPcmCacheAgeMinutes);
    }
    if (m_libavSeekDecoderWorker && m_pcmSeekCache) {
        m_libavSeekDecoderWorker->setSeekCache(m_pcmSeekCache);
    }

    if (m_outputWorker) {
        m_outputWorker->stop();
    }
    if (m_outputThread) {
        m_outputThread->quit();
        m_outputThread->wait(500);
        delete m_outputThread;
        m_outputThread = nullptr;
        m_outputWorker = nullptr;
    }

    m_outputThread = new QThread(this);
    m_outputWorker = new AlsaOutputWorker();
    m_outputWorker->moveToThread(m_outputThread);

    const bool isSeekResume = (profile == PipelineStartupProfile::SeekResume);
    const qsizetype silenceFrames = isSeekResume
        ? qMax<qsizetype>(1, m_outputPcmFormat.sampleRate * 8 / 1000)
        : 0;
    const qsizetype warmupFrames = isSeekResume
        ? qMax<qsizetype>(1, m_outputPcmFormat.sampleRate * 8 / 1000)
        : 0;
    m_outputWorker->configure(
        m_pcmHandle,
        m_buffer,
        m_decoderPcmFormat,
        m_outputPcmFormat,
        m_alsaFormat,
        m_decoderSessionId,
        m_bufferGeneration,
        isSeekResume,
        startupThresholdBytes(profile),
        silenceFrames,
        warmupFrames);

    connect(m_outputWorker, &AlsaOutputWorker::audioLevelsChanged,
            this, [this](int sessionId, qreal left, qreal right) {
                if (sessionId == m_decoderSessionId) {
                    emitAudioLevels(left, right);
                }
            });

    connect(m_outputWorker, &AlsaOutputWorker::positionUpdated,
            this, [this](int sessionId, qint64 positionMs) {
                if (sessionId == m_decoderSessionId) {
                    emit positionChanged(positionMs);
                }
            });

    connect(m_outputWorker, &AlsaOutputWorker::errorOccurred,
            this, [this](int sessionId, const QString &message) {
                if (sessionId == m_decoderSessionId) {
                    PlayerLogger::log(QStringLiteral("alsa"),
                                      QStringLiteral("outputError session=%1 message=%2")
                                          .arg(sessionId)
                                          .arg(message));
                    scheduleOutputRecovery();
                }
            });

    connect(m_outputWorker, &AlsaOutputWorker::finished,
            this, [this]() {
                if (m_decoderFinished) {
                    finalizePlayback();
                }
            });

    connect(m_outputThread, &QThread::started, m_outputWorker, &AlsaOutputWorker::start);
    m_outputThread->start();

    m_audioStarted = true;
    setPlaybackState(PlaybackState::Playing);

    emit outputFormatChanged(m_outputDeviceDescription, m_outputFormat);

    m_outputRecoveryPositionMs = startPositionMs;
    m_outputRecoveryGeneration = m_bufferGeneration;

    resetOutputRecoveryState();

    auto createFfmpegDecoder = [this](qint64 startMs) {
        stopDecoderWorker(false);
        if (m_decoderThread) {
            m_decoderThread->quit();
            m_decoderThread->wait(500);
            delete m_decoderThread;
            m_decoderThread = nullptr;
            m_decoderWorker = nullptr;
        }

        m_decoderThread = new QThread(this);
        m_decoderWorker = new FfmpegDecoderWorker();
        m_decoderWorker->moveToThread(m_decoderThread);

        connect(m_decoderWorker, &FfmpegDecoderWorker::dataAvailable,
                this, &LinuxAlsaAudioPlayer::handleDecoderDataAvailable);
        connect(m_decoderWorker, &FfmpegDecoderWorker::errorOccurred,
                this, &LinuxAlsaAudioPlayer::handleDecoderError);
        connect(m_decoderWorker, &FfmpegDecoderWorker::finished,
                this, &LinuxAlsaAudioPlayer::handleDecoderFinished);
        connect(m_decoderWorker, &FfmpegDecoderWorker::audioLevelsChanged,
                this, [this](int sessionId, qreal left, qreal right) {
                    if (sessionId == m_decoderSessionId) {
                        emitAudioLevels(left, right);
                    }
                });

        int sessId = m_decoderSessionId;
        PcmStreamFormat decFormat = m_decoderPcmFormat;
        QString srcPath = m_sourcePath;
        QString srcCodecName = m_sourceCodecName;
        int srcChannelCount = m_sourceChannelCount;
        int outputChannelCount = m_outputPcmFormat.channelCount;
        connect(m_decoderThread, &QThread::started, m_decoderWorker,
                [this, sessId, startMs, decFormat, srcPath, srcCodecName, srcChannelCount, outputChannelCount]() {
                    QString ffmpegPath = locateFfmpegExecutable();
                    QString channelLayout = channelLayoutForCount(decFormat.channelCount);
                    const QString rawInputFormat = rawInputFormatForSource();
                    QStringList args;

                    const QString downmixLayout = channelLayout;
                    if (srcChannelCount > 0 && srcChannelCount > outputChannelCount
                        && !downmixLayout.isEmpty()
                        && (srcCodecName == QStringLiteral("truehd")
                            || srcCodecName == QStringLiteral("eac3")
                            || srcCodecName == QStringLiteral("ac3")
                            || !rawInputFormat.isEmpty())) {
                        args << QStringLiteral("-downmix") << downmixLayout;
                    }

                    if (!rawInputFormat.isEmpty()) {
                        args << QStringLiteral("-f") << rawInputFormat;
                    }

                    if (startMs > 0) {
                        args << QStringLiteral("-ss")
                             << QString::number(startMs / 1000.0, 'f', 3);
                    }
                    args << QStringLiteral("-i") << srcPath
                         << QStringLiteral("-f")
                         << QStringLiteral("s%1le").arg(decFormat.validBitsPerSample)
                         << QStringLiteral("-acodec")
                         << QStringLiteral("pcm_s%1le").arg(decFormat.validBitsPerSample)
                         << QStringLiteral("-ar")
                         << QString::number(decFormat.sampleRate)
                         << QStringLiteral("-ac")
                         << QString::number(decFormat.channelCount);

                    if (!channelLayout.isEmpty()) {
                        args << QStringLiteral("-channel_layout") << channelLayout;
                    }

                    args << QStringLiteral("pipe:1");

                    m_decoderWorker->startDecoding(
                        sessId, ffmpegPath, args, m_buffer, decFormat);
                });

        m_decoderThread->start();
    };

    if (m_libavSeekDecoderWorker) {
        if (profile == PipelineStartupProfile::SeekResume) {
            m_libavSeekDecoderWorker->seekTo(
                m_decoderSessionId, startPositionMs, m_buffer, m_bufferGeneration);
        } else {
            m_libavSeekDecoderWorker->startDecoding(
                m_decoderSessionId, m_sourcePath, startPositionMs,
                m_buffer, m_decoderPcmFormat);
        }
    } else {
        createFfmpegDecoder(startPositionMs);
    }
}

void LinuxAlsaAudioPlayer::beginActiveOutputSwitch(ActiveOutputSwitchTrigger trigger,
                                                     const QString &reason)
{
    if (m_activeOutputSwitch.isActive()) {
        PlayerLogger::log(QStringLiteral("alsa"),
                          QStringLiteral("activeOutputSwitch already in progress, trigger=%1 reason=%2")
                              .arg(activeOutputSwitchTriggerName(m_activeOutputSwitch.trigger))
                              .arg(m_activeOutputSwitch.reason));
        return;
    }

    m_activeOutputSwitch = ActiveOutputSwitchTransaction();
    m_activeOutputSwitch.trigger = trigger;
    m_activeOutputSwitch.phase = ActiveOutputSwitchPhase::Pending;
    m_activeOutputSwitch.reason = reason;
    m_activeOutputSwitch.resumePositionMs = m_outputRecoveryPositionMs;
    m_activeOutputSwitch.transactionTimer.start();

    PlayerLogger::log(QStringLiteral("alsa"),
                      QStringLiteral("beginActiveOutputSwitch trigger=%1 reason=%2")
                          .arg(activeOutputSwitchTriggerName(trigger))
                          .arg(reason));

    PlayerLogger::diagnostic(QStringLiteral("alsa"),
                             QStringLiteral("active_output_switch_begin"),
                             {
                                 {QStringLiteral("trigger"), activeOutputSwitchTriggerName(trigger)},
                                 {QStringLiteral("reason"), reason},
                             });

    applyActiveOutputSwitch();
}

void LinuxAlsaAudioPlayer::applyActiveOutputSwitch()
{
    if (!m_activeOutputSwitch.isActive()) {
        return;
    }

    m_activeOutputSwitch.phase = ActiveOutputSwitchPhase::Applying;

    PlayerLogger::log(QStringLiteral("alsa"),
                      QStringLiteral("applyActiveOutputSwitch trigger=%1 elapsed=%2ms")
                          .arg(activeOutputSwitchTriggerName(m_activeOutputSwitch.trigger))
                          .arg(m_activeOutputSwitch.transactionTimer.elapsed()));

    PlayerLogger::diagnostic(QStringLiteral("alsa"),
                             QStringLiteral("active_output_switch_apply"),
                             {
                                 {QStringLiteral("trigger"), activeOutputSwitchTriggerName(m_activeOutputSwitch.trigger)},
                                 {QStringLiteral("reason"), m_activeOutputSwitch.reason},
                             });

    qint64 resumeMs = m_activeOutputSwitch.resumePositionMs;
    resetActiveOutputSwitch(QStringLiteral("applied"));

    startPipeline(resumeMs, PipelineStartupProfile::ActiveSwitchRebuild);
}

void LinuxAlsaAudioPlayer::resetActiveOutputSwitch(const QString &reason)
{
    if (!m_activeOutputSwitch.isActive()) {
        return;
    }

    PlayerLogger::log(QStringLiteral("alsa"),
                      QStringLiteral("resetActiveOutputSwitch trigger=%1 reason=%2")
                          .arg(activeOutputSwitchTriggerName(m_activeOutputSwitch.trigger))
                          .arg(reason));

    m_activeOutputSwitch = ActiveOutputSwitchTransaction();
}

bool LinuxAlsaAudioPlayer::isActiveOutputSwitchInProgress() const
{
    return m_activeOutputSwitch.isActive();
}
