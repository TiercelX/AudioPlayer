#include "ffmpegaudioplayer.h"
#include "audiooutputworker.h"
#include "audioutils.h"
#include "ffmpegpcmshared.h"
#include "playerlogger.h"

#include <QAudioDevice>
#include <QAudioSink>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QIODevice>
#include <QMediaDevices>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QtEndian>

#include <cmath>
#include <cstring>
#include <limits>

FfmpegAudioPlayer::FfmpegAudioPlayer(QObject *parent)
    : AudioPlayerBackend(parent)
    , m_audioThread(new QThread(this))
    , m_audioWorker(new AudioOutputWorker)
    , m_decoderThread(new QThread(this))
    , m_decoderWorker(new FfmpegDecoderWorker)
    , m_mediaDevices(new QMediaDevices(this))
{
    PlayerLogger::log(QStringLiteral("player"), QStringLiteral("FfmpegAudioPlayer initializing"));
    m_audioWorker->moveToThread(m_audioThread);
    connect(m_audioThread, &QThread::finished, m_audioWorker, &QObject::deleteLater);
    connect(m_audioWorker, &AudioOutputWorker::positionUpdated,
            this, &FfmpegAudioPlayer::handleAudioPositionUpdated);
    connect(m_audioWorker, &AudioOutputWorker::stateChanged,
            this, &FfmpegAudioPlayer::handleAudioStateChanged);
    m_audioThread->start();

    m_decoderWorker->moveToThread(m_decoderThread);
    connect(m_decoderThread, &QThread::finished, m_decoderWorker, &QObject::deleteLater);
    connect(m_decoderWorker, &FfmpegDecoderWorker::dataAvailable,
            this, &FfmpegAudioPlayer::handleDecoderDataAvailable);
    connect(m_decoderWorker, &FfmpegDecoderWorker::audioLevelsChanged, this,
            [this](int sessionId, qreal leftLevel, qreal rightLevel) {
                if (sessionId != m_activeDecoderSessionId || m_playbackState == PlaybackState::Stopping) {
                    return;
                }

                emitAudioLevels(leftLevel, rightLevel);
            });
    connect(m_decoderWorker, &FfmpegDecoderWorker::errorOccurred,
            this, &FfmpegAudioPlayer::handleDecoderError);
    connect(m_decoderWorker, &FfmpegDecoderWorker::finished,
            this, &FfmpegAudioPlayer::handleDecoderFinished);
    m_decoderThread->start();

    connect(m_mediaDevices, &QMediaDevices::audioOutputsChanged,
            this, &FfmpegAudioPlayer::handleAudioOutputsChanged);
}

FfmpegAudioPlayer::~FfmpegAudioPlayer()
{
    teardownPipeline();
    if (m_audioWorker) {
        QMetaObject::invokeMethod(m_audioWorker, [worker = m_audioWorker] {
            worker->releaseOutput(0, true);
        }, Qt::BlockingQueuedConnection);
    }
    if (m_audioThread) {
        m_audioThread->quit();
        m_audioThread->wait(1000);
    }
    if (m_decoderWorker) {
        QMetaObject::invokeMethod(m_decoderWorker, [worker = m_decoderWorker] {
            worker->stopDecoding(true);
        }, Qt::BlockingQueuedConnection);
    }
    if (m_decoderThread) {
        m_decoderThread->quit();
        m_decoderThread->wait(1000);
    }
}

void FfmpegAudioPlayer::setSource(const QString &filePath,
                                  int sourceChannelCount,
                                  int sourceSampleRate,
                                  int sourceBitDepth,
                                  const QString &sourceCodecName)
{
    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("setSource path=%1 channels=%2 sampleRate=%3 bitDepth=%4 codec=%5")
                          .arg(filePath)
                          .arg(sourceChannelCount)
                          .arg(sourceSampleRate)
                          .arg(sourceBitDepth)
                          .arg(sourceCodecName));
    resetOutputRecoveryState(QStringLiteral("setSource"));
    teardownPipeline();

    m_sourcePath = filePath;
    m_sourceChannelCount = sourceChannelCount;
    m_sourceSampleRate = sourceSampleRate;
    m_sourceBitDepth = sourceBitDepth;
    m_sourceCodecName = sourceCodecName.toLower();
    m_currentPositionMs = 0;
    m_startPositionMs = 0;
    m_activeOutputDeviceId.clear();

    emitAudioLevels(0.0, 0.0);
    emit positionChanged(0);
    setPlaybackState(PlaybackState::Stopped);
}

QString FfmpegAudioPlayer::source() const
{
    return m_sourcePath;
}

AudioPlayerBackend::BackendId FfmpegAudioPlayer::backendId() const
{
    return BackendId::Ffmpeg;
}

QString FfmpegAudioPlayer::backendName() const
{
    return tr("ffmpeg + QAudioSink");
}

QString FfmpegAudioPlayer::decoderName() const
{
    return tr("ffmpeg CLI");
}

void FfmpegAudioPlayer::play()
{
    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("play state=%1 startPositionMs=%2 activeSession=%3")
                          .arg(AudioUtils::playbackStateName(m_playbackState))
                          .arg(m_startPositionMs)
                          .arg(m_activeDecoderSessionId));
    if (m_sourcePath.isEmpty()) {
        return;
    }

    if (m_playbackState == PlaybackState::Stopping) {
        return;
    }

    if (m_playbackState == PlaybackState::Paused && m_audioStarted && m_audioWorker) {
        const int sessionId = m_activeDecoderSessionId;
        QMetaObject::invokeMethod(m_audioWorker, [worker = m_audioWorker, sessionId] {
            worker->resumeOutput(sessionId);
        }, Qt::QueuedConnection);
        setPlaybackState(PlaybackState::Playing);
        return;
    }

    if (m_playbackState == PlaybackState::Paused || m_playbackState == PlaybackState::Stopped) {
        resetOutputRecoveryState(QStringLiteral("play"));
        startPipeline(m_startPositionMs);
    }
}

void FfmpegAudioPlayer::pause()
{
    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("pause state=%1 activeSession=%2 audioStarted=%3")
                          .arg(AudioUtils::playbackStateName(m_playbackState))
                          .arg(m_activeDecoderSessionId)
                          .arg(m_audioStarted));
    if (m_playbackState != PlaybackState::Playing || !m_audioStarted || !m_audioWorker) {
        return;
    }

    const int sessionId = m_activeDecoderSessionId;
    QMetaObject::invokeMethod(m_audioWorker, [worker = m_audioWorker, sessionId] {
        worker->pauseOutput(sessionId);
    }, Qt::QueuedConnection);
    setPlaybackState(PlaybackState::Paused);
}

void FfmpegAudioPlayer::stop()
{
    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("stop state=%1 activeSession=%2 stoppingSession=%3")
                          .arg(AudioUtils::playbackStateName(m_playbackState))
                          .arg(m_activeDecoderSessionId)
                          .arg(m_stoppingDecoderSessionId));
    resetOutputRecoveryState(QStringLiteral("stop"));
    m_audioStarted = false;
    m_decoderFinished = false;
    m_currentPositionMs = 0;
    m_startPositionMs = 0;
    m_audioState = QAudio::StoppedState;
    m_audioError = QtAudio::NoError;
    m_lastLoggedPositionBucket = -1;
    emitAudioLevels(0.0, 0.0);
    emit positionChanged(0);
    releaseOutputResources();

    if (!m_activeDecoderSessionId) {
        clearBufferDevice();
        setPlaybackState(PlaybackState::Stopped);
        return;
    }

    m_stoppingDecoderSessionId = m_activeDecoderSessionId;
    m_activeDecoderSessionId = 0;
    setPlaybackState(PlaybackState::Stopping);
    stopDecoderWorker(false);
}

void FfmpegAudioPlayer::seek(qint64 positionMs)
{
    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("seek request positionMs=%1 state=%2 activeSession=%3")
                          .arg(positionMs)
                          .arg(AudioUtils::playbackStateName(m_playbackState))
                          .arg(m_activeDecoderSessionId));
    if (m_sourcePath.isEmpty()) {
        return;
    }

    if (m_playbackState == PlaybackState::Stopping) {
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("seek ignored in state=%1")
                              .arg(AudioUtils::playbackStateName(m_playbackState)));
        return;
    }

    const qint64 clampedPosition = qMax<qint64>(0, positionMs);

    if (m_playbackState == PlaybackState::Stopped) {
        m_currentPositionMs = clampedPosition;
        m_startPositionMs = clampedPosition;
        emit positionChanged(clampedPosition);
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("seek stored for stopped state positionMs=%1").arg(clampedPosition));
        return;
    }

    resetOutputRecoveryState(QStringLiteral("seek"));
    if (m_playbackState == PlaybackState::Paused) {
        teardownPipeline();
        m_currentPositionMs = clampedPosition;
        m_startPositionMs = clampedPosition;
        emit positionChanged(clampedPosition);
        return;
    }

    startPipeline(clampedPosition);
}

void FfmpegAudioPlayer::setVolume(qreal volume)
{
    m_volume = volume;
    if (m_audioWorker) {
        QMetaObject::invokeMethod(m_audioWorker, [worker = m_audioWorker, volume] {
            worker->setVolume(volume);
        }, Qt::QueuedConnection);
    }
}

void FfmpegAudioPlayer::releaseOutputResources()
{
    if (m_audioWorker) {
        const int sessionId = m_activeDecoderSessionId != 0
            ? m_activeDecoderSessionId
            : m_stoppingDecoderSessionId;
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("releaseOutputResources begin session=%1 buffer=%2")
                              .arg(sessionId)
                              .arg(m_bufferDevice != nullptr));
        QMetaObject::invokeMethod(m_audioWorker,
                                  [worker = m_audioWorker, sessionId] {
                                      worker->releaseOutput(sessionId, true);
                                  },
                                  Qt::BlockingQueuedConnection);
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("releaseOutputResources end session=%1").arg(sessionId));
    }
}

void FfmpegAudioPlayer::clearBufferDevice()
{
    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("clearBufferDevice hasBuffer=%1").arg(m_bufferDevice != nullptr));
    delete m_bufferDevice;
    m_bufferDevice = nullptr;
}

void FfmpegAudioPlayer::stopDecoderWorker(bool waitForFinished)
{
    if (!m_decoderWorker || (!m_activeDecoderSessionId && !m_stoppingDecoderSessionId)) {
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("stopDecoderWorker skipped waitForFinished=%1 active=%2 stopping=%3")
                              .arg(waitForFinished)
                              .arg(m_activeDecoderSessionId)
                              .arg(m_stoppingDecoderSessionId));
        return;
    }

    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("stopDecoderWorker begin waitForFinished=%1 active=%2 stopping=%3")
                          .arg(waitForFinished)
                          .arg(m_activeDecoderSessionId)
                          .arg(m_stoppingDecoderSessionId));
    QMetaObject::invokeMethod(m_decoderWorker,
                              [worker = m_decoderWorker, waitForFinished] {
                                  worker->stopDecoding(waitForFinished);
                              },
                              waitForFinished ? Qt::BlockingQueuedConnection : Qt::QueuedConnection);
    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("stopDecoderWorker end waitForFinished=%1").arg(waitForFinished));
}

void FfmpegAudioPlayer::startAudioOutputIfReady()
{
    auto *buffer = static_cast<PcmStreamBuffer *>(m_bufferDevice);
    if (!m_audioWorker || !buffer || m_audioStarted) {
        return;
    }

    if (!m_decoderFinished && buffer->bufferedBytes() < startupThresholdBytes()) {
        return;
    }

    const int sessionId = m_activeDecoderSessionId;
    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("startAudioOutputIfReady session=%1 bufferedBytes=%2 decoderFinished=%3")
                          .arg(sessionId)
                          .arg(buffer->bufferedBytes())
                          .arg(m_decoderFinished));
    QMetaObject::invokeMethod(m_audioWorker,
                              [worker = m_audioWorker, sessionId, buffer] {
                                  worker->startOutput(sessionId, buffer);
                              },
                              Qt::BlockingQueuedConnection);
    m_audioStarted = true;
}

void FfmpegAudioPlayer::startPipeline(qint64 startPositionMs)
{
    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("startPipeline startPositionMs=%1 source=%2")
                          .arg(startPositionMs)
                          .arg(m_sourcePath));
    const QString ffmpegExecutable = locateFfmpegExecutable();
    if (ffmpegExecutable.isEmpty()) {
        emit errorOccurred(PlaybackError::DecoderNotFound,
                           tr("未找到 FFmpeg 解码器\n\n"
                               "程序可能安装不完整\n\n"
                               "建议操作：重新安装程序或检查程序完整性"));
        return;
    }

    const QAudioDevice device = resolveOutputDevice();
    QString deviceDescription;
    const QAudioFormat selectedFormat = selectOutputFormat(&deviceDescription);
    const bool applyRecoveryFadeIn =
        m_outputRecoveryPending || (m_playbackState == PlaybackState::Playing && m_activeDecoderSessionId != 0);
    if (!selectedFormat.isValid()) {
        emit errorOccurred(PlaybackError::FormatNotSupported,
                           tr("未找到可用的音频输出格式\n\n"
                               "建议操作：\n"
                               "1. 检查音频设备连接\n"
                               "2. 尝试其他输出设备"));
        return;
    }

    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("startPipeline output deviceId=%1 description=%2 sampleRate=%3 channels=%4 sampleFormat=%5 usesDefault=%6 recoveryFadeIn=%7")
                          .arg(QString::fromLatin1(device.id().toHex()))
                          .arg(deviceDescription)
                          .arg(selectedFormat.sampleRate())
                          .arg(selectedFormat.channelCount())
                          .arg(static_cast<int>(selectedFormat.sampleFormat()))
                          .arg(m_selectedOutputDeviceId.isEmpty())
                          .arg(applyRecoveryFadeIn));

    teardownPipeline();

    m_outputFormat = selectedFormat;
    m_outputDeviceDescription = deviceDescription;
    m_activeOutputDeviceId = device.id();
    emit outputFormatChanged(m_outputDeviceDescription, m_outputFormat);

    auto *buffer = new PcmStreamBuffer(this);
    m_bufferDevice = buffer;
    buffer->clear();
    buffer->setEndOfStream(false);
    const qsizetype bytesPerHalfSecond = qMax<qsizetype>(
        65536,
        static_cast<qsizetype>(m_outputFormat.bytesPerFrame())
            * m_outputFormat.sampleRate() / 2);
    buffer->setMaxSize(bytesPerHalfSecond * 6);
    m_audioStarted = false;
    m_audioState = QAudio::StoppedState;
    m_audioError = QtAudio::NoError;

    const qsizetype sinkBufferSize = qMax<qsizetype>(startupThresholdBytes(), bytesPerHalfSecond / 2);
    if (m_audioWorker) {
        const int pendingSessionId = m_decoderSessionId + 1;
        QMetaObject::invokeMethod(m_audioWorker,
                                  [worker = m_audioWorker,
                                   pendingSessionId,
                                   device,
                                   format = m_outputFormat,
                                   sinkBufferSize,
                                   volume = m_volume,
                                   applyRecoveryFadeIn] {
                                      worker->configureOutput(pendingSessionId,
                                                              device,
                                                              format,
                                                              sinkBufferSize,
                                                              volume,
                                                              applyRecoveryFadeIn);
                                  },
                                  Qt::BlockingQueuedConnection);
    }

    QStringList arguments {
        QStringLiteral("-nostdin"),
        QStringLiteral("-v"),
        QStringLiteral("error"),
    };

    const QString downmixLayout = channelLayoutForCount(m_outputFormat.channelCount());
    if (m_sourceChannelCount > 0 && m_sourceChannelCount > m_outputFormat.channelCount() && !downmixLayout.isEmpty()
        && (m_sourceCodecName == QStringLiteral("truehd") || m_sourceCodecName == QStringLiteral("eac3")
            || m_sourceCodecName == QStringLiteral("ac3") || !rawInputFormatForSource().isEmpty())) {
        arguments << QStringLiteral("-downmix") << downmixLayout;
    }

    const QString rawInputFormat = rawInputFormatForSource();
    if (!rawInputFormat.isEmpty()) {
        arguments << QStringLiteral("-f") << rawInputFormat;
    }
    if (startPositionMs > 0) {
        arguments << QStringLiteral("-ss") << formatSeekSeconds(startPositionMs);
    }

    const QString targetChannelLayout = channelLayoutForCount(m_outputFormat.channelCount());
    QStringList filters {
        QStringLiteral("aresample=%1").arg(m_outputFormat.sampleRate()),
        QStringLiteral("aformat=sample_rates=%1:sample_fmts=%2")
            .arg(m_outputFormat.sampleRate())
            .arg(pcmSampleFormatName(m_outputFormat.sampleFormat())),
    };
    if (!targetChannelLayout.isEmpty()) {
        filters.last().append(QStringLiteral(":channel_layouts=%1").arg(targetChannelLayout));
    }

    arguments << QStringLiteral("-i") << m_sourcePath
              << QStringLiteral("-map") << QStringLiteral("0:a:0")
              << QStringLiteral("-vn")
              << QStringLiteral("-sn")
              << QStringLiteral("-dn")
              << QStringLiteral("-af") << filters.join(',')
              << QStringLiteral("-ac") << QString::number(m_outputFormat.channelCount())
              << QStringLiteral("-ar") << QString::number(m_outputFormat.sampleRate())
              << QStringLiteral("-acodec") << pcmCodecName(m_outputFormat.sampleFormat())
              << QStringLiteral("-f") << pcmMuxerName(m_outputFormat.sampleFormat())
              << QStringLiteral("-");

    m_stderrBuffer.clear();
    m_decoderFinished = false;
    m_activeDecoderSessionId = ++m_decoderSessionId;
    m_stoppingDecoderSessionId = 0;
    m_currentPositionMs = startPositionMs;
    m_startPositionMs = startPositionMs;
    m_lastLoggedPositionBucket = startPositionMs / 1000;
    emit positionChanged(startPositionMs);
    setPlaybackState(PlaybackState::Playing);
    if (m_decoderWorker) {
        const int sessionId = m_activeDecoderSessionId;
        QMetaObject::invokeMethod(m_decoderWorker,
                                  [worker = m_decoderWorker,
                                   sessionId,
                                   ffmpegExecutable,
                                   arguments,
                                   buffer,
                                   outputFormat = m_outputFormat]() {
                                      PcmStreamFormat decoderFormat;
                                      decoderFormat.sampleRate = outputFormat.sampleRate();
                                      decoderFormat.channelCount = outputFormat.channelCount();
                                      switch (outputFormat.sampleFormat()) {
                                      case QAudioFormat::UInt8:
                                          decoderFormat.sampleEncoding = PcmSampleEncoding::UInt8;
                                          decoderFormat.validBitsPerSample = 8;
                                          break;
                                      case QAudioFormat::Int16:
                                          decoderFormat.sampleEncoding = PcmSampleEncoding::Int16;
                                          decoderFormat.validBitsPerSample = 16;
                                          break;
                                      case QAudioFormat::Int32:
                                          decoderFormat.sampleEncoding = PcmSampleEncoding::Int32;
                                          decoderFormat.validBitsPerSample = 32;
                                          break;
                                      case QAudioFormat::Float:
                                          decoderFormat.sampleEncoding = PcmSampleEncoding::Float32;
                                          decoderFormat.validBitsPerSample = 32;
                                          break;
                                      case QAudioFormat::Unknown:
                                      case QAudioFormat::NSampleFormats:
                                          break;
                                      }
                                      worker->startDecoding(sessionId,
                                                           ffmpegExecutable,
                                                           arguments,
                                                           buffer,
                                                           decoderFormat);
                                  },
                                  Qt::QueuedConnection);
    }
}

void FfmpegAudioPlayer::teardownPipeline()
{
    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("teardownPipeline activeSession=%1 stoppingSession=%2")
                          .arg(m_activeDecoderSessionId)
                          .arg(m_stoppingDecoderSessionId));
    m_tearingDown = true;
    m_audioStarted = false;
    m_decoderFinished = false;
    m_audioState = QAudio::StoppedState;
    m_audioError = QtAudio::NoError;

    PlayerLogger::log(QStringLiteral("player"), QStringLiteral("teardownPipeline step=releaseOutputResources"));
    releaseOutputResources();
    PlayerLogger::log(QStringLiteral("player"), QStringLiteral("teardownPipeline step=stopDecoderWorker"));
    stopDecoderWorker(true);
    PlayerLogger::log(QStringLiteral("player"), QStringLiteral("teardownPipeline step=clearBufferDevice"));
    clearBufferDevice();

    m_activeDecoderSessionId = 0;
    m_stoppingDecoderSessionId = 0;
    m_activeOutputDeviceId.clear();
    m_outputFormat = {};
    m_outputDeviceDescription.clear();

    m_stderrBuffer.clear();
    m_tearingDown = false;
    emitAudioLevels(0.0, 0.0);
    PlayerLogger::log(QStringLiteral("player"), QStringLiteral("teardownPipeline done"));
}

void FfmpegAudioPlayer::handleDecoderError(int sessionId, const QString &message)
{
    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("decoderError session=%1 message=%2")
                          .arg(sessionId)
                          .arg(message));
    if (sessionId != m_activeDecoderSessionId || m_playbackState == PlaybackState::Stopping) {
        return;
    }

    resetOutputRecoveryState(QStringLiteral("decoderError"));
    teardownPipeline();
    setPlaybackState(PlaybackState::Stopped);
    emit errorOccurred(PlaybackError::DecoderError, message);
}

void FfmpegAudioPlayer::handleDecoderFinished(int sessionId,
                                              int exitCode,
                                              int exitStatus,
                                              const QString &stderrText)
{
    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("decoderFinished session=%1 exitCode=%2 exitStatus=%3 stderr=%4")
                          .arg(sessionId)
                          .arg(exitCode)
                          .arg(exitStatus)
                          .arg(stderrText.trimmed()));
    if (sessionId == m_stoppingDecoderSessionId) {
        resetOutputRecoveryState(QStringLiteral("decoderStopped"));
        m_stoppingDecoderSessionId = 0;
        m_stderrBuffer.clear();
        clearBufferDevice();
        m_activeOutputDeviceId.clear();
        setPlaybackState(PlaybackState::Stopped);
        return;
    }

    if (sessionId != m_activeDecoderSessionId) {
        return;
    }

    m_stderrBuffer = stderrText;
    m_decoderFinished = true;
    if (auto *buffer = static_cast<PcmStreamBuffer *>(m_bufferDevice)) {
        buffer->setEndOfStream(true);
    }

    if (exitStatus != static_cast<int>(QProcess::NormalExit) || exitCode != 0) {
        // Log technical details
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("decoderFailed exitCode=%1 stderr=%2")
                              .arg(exitCode)
                              .arg(stderrText.isEmpty() ? QStringLiteral("none") : stderrText.trimmed()));

        // User-friendly message
        const QString errorText = stderrText.trimmed().isEmpty()
            ? tr("音频解码失败\n\n"
                 "可能原因：文件格式不支持或文件损坏\n\n"
                 "建议操作：\n"
                 "1. 尝试其他音频文件\n"
                 "2. 检查文件是否完整")
            : tr("音频解码失败\n\n"
                 "技术详情：%1\n\n"
                 "建议操作：\n"
                 "1. 尝试其他音频文件\n"
                 "2. 检查文件是否完整")
                  .arg(stderrText.trimmed());
        resetOutputRecoveryState(QStringLiteral("decoderFinished-error"));
        teardownPipeline();
        setPlaybackState(PlaybackState::Stopped);
        emit errorOccurred(PlaybackError::DecoderError, errorText);
        return;
    }

    startAudioOutputIfReady();
    if (auto *buffer = static_cast<PcmStreamBuffer *>(m_bufferDevice);
        buffer && buffer->isEmpty()
        && (!m_audioStarted || m_audioState == QAudio::IdleState
            || m_audioState == QAudio::StoppedState)) {
        finalizePlayback();
    }
}

