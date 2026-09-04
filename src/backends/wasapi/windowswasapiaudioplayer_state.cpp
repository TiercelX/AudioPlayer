#include "windowswasapiaudioplayer.h"

#include "audioutils.h"
#include "playerlogger.h"
#include "toollocator.h"
#include "windowswasapiaudioplayer_worker_helpers.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTimer>

namespace {

constexpr qint64 kPositionRegressionToleranceMs = 5;
constexpr qint64 kPositionJumpToleranceMs = 240;
constexpr qint64 kPositionLagToleranceMs = 180;
constexpr int kPositionStallLogThreshold = 3;
constexpr int kActiveSwitchOutputStartErrorRetryLimit = 1;
constexpr int kDefaultStartupThresholdMs = 200;
constexpr int kStabilityModeStartupThresholdMs = 500;

QString audioErrorName(QtAudio::Error error)
{
    switch (error) {
    case QtAudio::NoError:
        return QStringLiteral("NoError");
    case QtAudio::OpenError:
        return QStringLiteral("OpenError");
    case QtAudio::IOError:
        return QStringLiteral("IOError");
    case QtAudio::UnderrunError:
        return QStringLiteral("UnderrunError");
    case QtAudio::FatalError:
        return QStringLiteral("FatalError");
    }

    return QStringLiteral("Unknown");
}

constexpr int kRecoveryDelayAttempt1Ms = 250;
constexpr int kRecoveryDelayAttempt2Ms = 500;
constexpr int kRecoveryDelayAttempt3Ms = 900;

int outputRecoveryDelayMsForAttempt(int attempt)
{
    switch (attempt) {
    case 1:
        return kRecoveryDelayAttempt1Ms;
    case 2:
        return kRecoveryDelayAttempt2Ms;
    case 3:
        return kRecoveryDelayAttempt3Ms;
    default:
        return -1;
    }
}

} // namespace

bool WindowsWasapiAudioPlayer::isSupportedForContext(const AudioPlayerSourceContext &context, QString *reason)
{
    Q_UNUSED(context);

#if defined(Q_OS_WINDOWS)
    if (reason) {
        *reason = QString();
    }
    return true;
#else
    if (reason) {
        *reason = tr("Current build is not targeting Windows.");
    }
    return false;
#endif
}

QString WindowsWasapiAudioPlayer::channelLayoutForCount(int channelCount) const
{
    return AudioUtils::channelLayoutForCount(channelCount);
}

QString WindowsWasapiAudioPlayer::locateFfmpegExecutable() const
{
    return AudioUtils::locateFfmpegExecutable();
}

QString WindowsWasapiAudioPlayer::pcmCodecName(const PcmStreamFormat &format) const
{
    return AudioUtils::pcmCodecName(format);
}

QString WindowsWasapiAudioPlayer::pcmSampleFormatName(const PcmStreamFormat &format) const
{
    return AudioUtils::pcmSampleFormatName(format);
}

QString WindowsWasapiAudioPlayer::pcmMuxerName(const PcmStreamFormat &format) const
{
    return AudioUtils::pcmMuxerName(format);
}

QString WindowsWasapiAudioPlayer::activeOutputSwitchTriggerName(ActiveOutputSwitchTrigger trigger) const
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
    case ActiveOutputSwitchTrigger::OutputFormatChange:
        return QStringLiteral("OutputFormatChange");
    }

    return QStringLiteral("Unknown");
}

QString WindowsWasapiAudioPlayer::activeOutputSwitchPhaseName(ActiveOutputSwitchPhase phase) const
{
    switch (phase) {
    case ActiveOutputSwitchPhase::Idle:
        return QStringLiteral("Idle");
    case ActiveOutputSwitchPhase::WaitingForInvalidation:
        return QStringLiteral("WaitingForInvalidation");
    case ActiveOutputSwitchPhase::Pending:
        return QStringLiteral("Pending");
    case ActiveOutputSwitchPhase::OutputSuspended:
        return QStringLiteral("OutputSuspended");
    case ActiveOutputSwitchPhase::Preflight:
        return QStringLiteral("Preflight");
    case ActiveOutputSwitchPhase::Applying:
        return QStringLiteral("Applying");
    case ActiveOutputSwitchPhase::WaitingForOutputStart:
        return QStringLiteral("WaitingForOutputStart");
    }

    return QStringLiteral("Unknown");
}

QString WindowsWasapiAudioPlayer::pipelineStartupProfileName(PipelineStartupProfile profile) const
{
    switch (profile) {
    case PipelineStartupProfile::NormalStart:
        return QStringLiteral("NormalStart");
    case PipelineStartupProfile::SeekRestart:
        return QStringLiteral("SeekRestart");
    case PipelineStartupProfile::SeekResume:
        return QStringLiteral("SeekResume");
    case PipelineStartupProfile::ActiveSwitchRebuild:
        return QStringLiteral("ActiveSwitchRebuild");
    case PipelineStartupProfile::ErrorRecovery:
        return QStringLiteral("ErrorRecovery");
    }

    return QStringLiteral("Unknown");
}

qsizetype WindowsWasapiAudioPlayer::startupThresholdBytes() const
{
    if (!m_decoderPcmFormat.isValid()) {
        return 32768;
    }

    const int startupThresholdMs =
        m_currentPipelineStartupProfile == PipelineStartupProfile::SeekResume
        ? (m_stabilityModeEnabled ? kStabilityModeSeekResumeStartupThresholdMs
                                  : kSeekResumeStartupThresholdMs)
        : (m_stabilityModeEnabled ? kStabilityModeStartupThresholdMs
                                  : kDefaultStartupThresholdMs);
    return qMax<qsizetype>(32768,
                           static_cast<qsizetype>(m_decoderPcmFormat.bytesPerFrame())
                               * m_decoderPcmFormat.sampleRate
                               * startupThresholdMs / 1000);
}

PcmStreamFormat WindowsWasapiAudioPlayer::decoderFormatForOutput(const PcmStreamFormat &outputFormat) const
{
    PcmStreamFormat decoderFormat = outputFormat;
    if (decoderFormat.sampleEncoding == PcmSampleEncoding::Int24
        || decoderFormat.sampleEncoding == PcmSampleEncoding::Int16) {
        decoderFormat.sampleEncoding = PcmSampleEncoding::Int32;
        decoderFormat.validBitsPerSample = 32;
    } else if (decoderFormat.sampleEncoding == PcmSampleEncoding::Int32
               && decoderFormat.effectiveValidBitsPerSample() < 32) {
        decoderFormat.validBitsPerSample = 32;
    }
    return decoderFormat;
}

QString WindowsWasapiAudioPlayer::decoderChannelLayoutForSource(const QByteArray &waveFormatData,
                                                                int decoderChannelCount) const
{
    if (decoderChannelCount <= 0) {
        return {};
    }

    if (m_sourceChannelCount == 8 && decoderChannelCount == 8) {
        if (m_sourceCodecName == QStringLiteral("eac3")) {
            return QStringLiteral("5.1.2");
        }
        if (m_sourceCodecName == QStringLiteral("truehd")) {
            return QStringLiteral("7.1");
        }
    }

    return channelLayoutForWaveFormatData(waveFormatData, decoderChannelCount);
}

bool WindowsWasapiAudioPlayer::spatialStaticBedEnabledForSource() const
{
    const QString setting = QProcessEnvironment::systemEnvironment()
        .value(QString::fromLatin1(kWasapiSpatialStaticBedEnv))
        .trimmed();
    if (!setting.isEmpty() && envFlagDisabled(setting)) {
        return false;
    }

    return m_sourceCodecName == QStringLiteral("eac3")
        && m_sourceChannelCount == 8;
}

QString WindowsWasapiAudioPlayer::rawInputFormatForSource() const
{
    const QString suffix = QFileInfo(m_sourcePath).suffix().toLower();
    if (suffix == QStringLiteral("mlp")
        || suffix == QStringLiteral("thd")
        || suffix == QStringLiteral("truehd")) {
        return QStringLiteral("truehd");
    }
    if (suffix == QStringLiteral("eb3") || suffix == QStringLiteral("ec3")) {
        return QStringLiteral("eac3");
    }
    return {};
}

void WindowsWasapiAudioPlayer::resetAnomalyTracking(const QString &reason)
{
    if (m_lastObservedPositionMs >= 0 || m_flatPositionTickCount > 0) {
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("anomalyTracking reset reason=%1 lastPositionMs=%2 flatTicks=%3")
                              .arg(reason)
                              .arg(m_lastObservedPositionMs)
                              .arg(m_flatPositionTickCount));
    }

    m_positionObservationTimer.invalidate();
    m_lastPositionObservationElapsedMs = -1;
    m_lastObservedPositionMs = -1;
    m_flatPositionTickCount = 0;
}

void WindowsWasapiAudioPlayer::logPlaybackAnomaly(const QString &event, const QString &details) const
{
    const auto *buffer = static_cast<const PcmStreamBuffer *>(m_bufferDevice);
    const qint64 bufferedBytes = buffer ? buffer->bufferedBytes() : -1;
    const QString detailText = details.isEmpty() ? QString() : QStringLiteral(" details=%1").arg(details);
    PlayerLogger::log(QStringLiteral("anomaly"),
                      QStringLiteral("playback event=%1 playbackState=%2 audioState=%3 error=%4(%5) activeSession=%6 stoppingSession=%7 currentPositionMs=%8 startPositionMs=%9 recoveryPending=%10 recoveryAttempt=%11 bufferBytes=%12%13")
                          .arg(event)
                          .arg(AudioUtils::playbackStateName(m_playbackState))
                          .arg(AudioUtils::audioStateName(m_audioState))
                          .arg(audioErrorName(m_audioError))
                          .arg(static_cast<int>(m_audioError))
                          .arg(m_activeDecoderSessionId)
                          .arg(m_stoppingDecoderSessionId)
                          .arg(m_currentPositionMs)
                          .arg(m_startPositionMs)
                          .arg(m_outputRecoveryPending)
                          .arg(m_outputRecoveryAttempt)
                          .arg(bufferedBytes)
                          .arg(detailText));
}

void WindowsWasapiAudioPlayer::quarantineBufferDevice(const QString &reason, bool discardFutureWrites)
{
    auto *buffer = static_cast<PcmStreamBuffer *>(m_bufferDevice);
    if (!buffer) {
        return;
    }

    const qsizetype pendingBytes = buffer->bufferedBytes();
    buffer->clear();
    buffer->setDiscardWrites(discardFutureWrites);
    buffer->setEndOfStream(discardFutureWrites);
    logPlaybackAnomaly(QStringLiteral("buffer-quarantined"),
                       QStringLiteral("reason=%1 pendingBytes=%2 discardedWriteBytes=%3 discardFutureWrites=%4 ownerSession=%5 bufferGeneration=%6")
                           .arg(reason)
                           .arg(pendingBytes)
                           .arg(buffer->discardedWriteBytes())
                           .arg(discardFutureWrites)
                           .arg(buffer->ownerSessionId())
                           .arg(buffer->bufferGeneration()));
}

void WindowsWasapiAudioPlayer::resumeBufferDevice(const QString &reason)
{
    auto *buffer = static_cast<PcmStreamBuffer *>(m_bufferDevice);
    if (!buffer) {
        return;
    }

    const bool wasDiscarding = buffer->isDiscardingWrites();
    const qint64 discardedWriteBytes = buffer->discardedWriteBytes();
    buffer->setDiscardWrites(false);
    buffer->setEndOfStream(m_decoderFinished);
    if (wasDiscarding || discardedWriteBytes > 0) {
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("buffer-resumed reason=%1 decoderFinished=%2 discardedWriteBytes=%3")
                              .arg(reason)
                              .arg(m_decoderFinished)
                              .arg(discardedWriteBytes));
    }
}

void WindowsWasapiAudioPlayer::resetOutputRecoveryState(const QString &reason)
{
    if (!m_outputRecoveryPending && m_outputRecoveryAttempt == 0 && m_outputRecoveryExpectedSessionId == 0
        && m_outputRecoveryPositionMs == 0) {
        return;
    }

    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("outputRecovery reset reason=%1 pending=%2 attempt=%3 expectedSession=%4 positionMs=%5")
                          .arg(reason)
                          .arg(m_outputRecoveryPending)
                          .arg(m_outputRecoveryAttempt)
                          .arg(m_outputRecoveryExpectedSessionId)
                          .arg(m_outputRecoveryPositionMs));
    m_outputRecoveryPending = false;
    m_outputRecoveryAttempt = 0;
    m_outputRecoveryExpectedSessionId = 0;
    m_outputRecoveryPositionMs = 0;
    ++m_outputRecoveryGeneration;
}

bool WindowsWasapiAudioPlayer::shouldAttemptOutputRecovery(QAudio::State state, QtAudio::Error error) const
{
#ifdef Q_OS_WINDOWS
    if (state != QAudio::StoppedState || error == QtAudio::NoError) {
        return false;
    }

    if (m_playbackState != PlaybackState::Playing || m_sourcePath.isEmpty() || m_activeDecoderSessionId == 0) {
        return false;
    }

    switch (error) {
    case QtAudio::IOError:
    case QtAudio::FatalError:
        return true;
    case QtAudio::OpenError:
        return m_outputRecoveryPending || m_outputRecoveryAttempt > 0;
    case QtAudio::NoError:
    case QtAudio::UnderrunError:
        return false;
    }
#else
    Q_UNUSED(state);
    Q_UNUSED(error);
#endif

    return false;
}

bool WindowsWasapiAudioPlayer::shouldFinalizeAfterEndOfStreamOutputStop() const
{
    if (m_playbackState != PlaybackState::Playing || !m_decoderFinished) {
        return false;
    }

    const auto *buffer = static_cast<const PcmStreamBuffer *>(m_bufferDevice);
    if (!buffer || !buffer->endOfStream()) {
        return false;
    }

    if (buffer->isEmpty()) {
        return true;
    }

    if (!m_decoderPcmFormat.isValid()
        || m_decoderPcmFormat.bytesPerFrame() <= 0
        || m_decoderPcmFormat.sampleRate <= 0) {
        return false;
    }

    constexpr qint64 kCompletionTailToleranceMs = 250;
    const qint64 bufferedMs = static_cast<qint64>(buffer->bufferedBytes())
        / m_decoderPcmFormat.bytesPerFrame()
        * 1000
        / m_decoderPcmFormat.sampleRate;
    return bufferedMs <= kCompletionTailToleranceMs;
}

void WindowsWasapiAudioPlayer::scheduleOutputRecovery(int failedSessionId, QtAudio::Error error)
{
#ifndef Q_OS_WINDOWS
    Q_UNUSED(failedSessionId);
    Q_UNUSED(error);
    return;
#else
    if (isActiveOutputSwitchInProgress()) {
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("outputRecovery skipped active-output-switch session=%1 error=%2(%3) trigger=%4 phase=%5 reason=%6")
                              .arg(failedSessionId)
                              .arg(audioErrorName(error))
                              .arg(static_cast<int>(error))
                              .arg(activeOutputSwitchTriggerName(m_activeOutputSwitch.trigger))
                              .arg(activeOutputSwitchPhaseName(m_activeOutputSwitch.phase))
                              .arg(m_activeOutputSwitch.reason));
        return;
    }

    if (m_outputDeviceChangeTimer) {
        m_outputDeviceChangeTimer->stop();
    }

    if (m_outputRecoveryPending && m_outputRecoveryExpectedSessionId == failedSessionId) {
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("outputRecovery already-scheduled session=%1 attempt=%2 error=%3(%4)")
                              .arg(failedSessionId)
                              .arg(m_outputRecoveryAttempt)
                              .arg(audioErrorName(error))
                              .arg(static_cast<int>(error)));
        return;
    }

    const int nextAttempt = m_outputRecoveryAttempt + 1;
    const int delayMs = outputRecoveryDelayMsForAttempt(nextAttempt);
    m_outputRecoveryPositionMs = qMax<qint64>(0, m_currentPositionMs);
    if (delayMs < 0) {
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("outputRecovery exhausted session=%1 attempts=%2 error=%3(%4) positionMs=%5")
                              .arg(failedSessionId)
                              .arg(m_outputRecoveryAttempt)
                              .arg(audioErrorName(error))
                              .arg(static_cast<int>(error))
                              .arg(m_outputRecoveryPositionMs));
        resetOutputRecoveryState(QStringLiteral("exhausted"));
        teardownPipeline();
        setPlaybackState(PlaybackState::Stopped);
        // User-friendly message with recovery context
        emit errorOccurred(PlaybackError::OutputRecoveryFailure,
                           tr("音频输出错误，自动恢复失败\n\n"
                               "可能原因：设备被占用或驱动异常\n\n"
                               "建议操作：\n"
                               "1. 检查音频设备连接\n"
                               "2. 关闭其他音频应用\n"
                               "3. 重新播放"));
        return;
    }

    m_outputRecoveryPending = true;
    m_outputRecoveryAttempt = nextAttempt;
    m_outputRecoveryExpectedSessionId = failedSessionId;
    const quint64 generation = ++m_outputRecoveryGeneration;
    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("outputRecovery scheduled session=%1 attempt=%2 delayMs=%3 error=%4(%5) positionMs=%6 activeDevice=%7")
                          .arg(failedSessionId)
                          .arg(nextAttempt)
                          .arg(delayMs)
                          .arg(audioErrorName(error))
                          .arg(static_cast<int>(error))
                          .arg(m_outputRecoveryPositionMs)
                          .arg(QString::fromLatin1(m_activeOutputDeviceId.toHex())));
    PlayerLogger::diagnostic(QStringLiteral("player"),
                             QStringLiteral("output_recovery_scheduled"),
                             {
                                 {QStringLiteral("transactionKind"), QStringLiteral("recovery")},
                                 {QStringLiteral("sessionId"), failedSessionId},
                                 {QStringLiteral("positionMs"), m_outputRecoveryPositionMs},
                                 {QStringLiteral("attempt"), nextAttempt},
                                 {QStringLiteral("delayMs"), delayMs},
                                 {QStringLiteral("sinkError"), audioErrorName(error)},
                                 {QStringLiteral("sinkErrorCode"), static_cast<int>(error)},
                                 {QStringLiteral("activeDeviceId"), QString::fromLatin1(m_activeOutputDeviceId.toHex())},
                             });

    QTimer::singleShot(delayMs, this, [this, generation, failedSessionId] {
        if (!m_outputRecoveryPending || generation != m_outputRecoveryGeneration) {
            return;
        }

        if (m_playbackState != PlaybackState::Playing || m_sourcePath.isEmpty()) {
            resetOutputRecoveryState(QStringLiteral("cancelled-before-attempt state=%1")
                                         .arg(AudioUtils::playbackStateName(m_playbackState)));
            return;
        }

        if (m_activeDecoderSessionId != 0 && m_activeDecoderSessionId != failedSessionId) {
            PlayerLogger::log(QStringLiteral("player"),
                              QStringLiteral("outputRecovery skipped superseded expectedSession=%1 activeSession=%2")
                                  .arg(failedSessionId)
                                  .arg(m_activeDecoderSessionId));
            return;
        }

        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("outputRecovery attempt-begin attempt=%1 expectedSession=%2 restartPositionMs=%3")
                              .arg(m_outputRecoveryAttempt)
                              .arg(failedSessionId)
                              .arg(m_outputRecoveryPositionMs));
        PlayerLogger::diagnostic(QStringLiteral("player"),
                                 QStringLiteral("output_recovery_start"),
                                 {
                                     {QStringLiteral("transactionKind"), QStringLiteral("recovery")},
                                     {QStringLiteral("sessionId"), failedSessionId},
                                     {QStringLiteral("positionMs"), m_outputRecoveryPositionMs},
                                     {QStringLiteral("attempt"), m_outputRecoveryAttempt},
                                 });
        startPipeline(m_outputRecoveryPositionMs, PipelineStartupProfile::ErrorRecovery);
    });
#endif
}

void WindowsWasapiAudioPlayer::finalizePlayback()
{
    resetOutputRecoveryState(QStringLiteral("finalizePlayback"));
    resetAnomalyTracking(QStringLiteral("finalizePlayback"));
    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("finalizePlayback activeSession=%1")
                          .arg(m_activeDecoderSessionId));
    teardownPipeline();
    m_currentPositionMs = 0;
    m_startPositionMs = 0;
    emit positionChanged(0);
    setPlaybackState(PlaybackState::Stopped);
    emit finished();
}

void WindowsWasapiAudioPlayer::handleAudioPositionUpdated(int sessionId, qint64 processedPositionMs)
{
    if (sessionId != m_activeDecoderSessionId || m_playbackState == PlaybackState::Stopped
        || m_playbackState == PlaybackState::Stopping) {
        return;
    }

    const qint64 absolutePositionMs = m_startPositionMs + processedPositionMs;
    const bool activeSwitchTerminalPositionReset =
        activeOutputSwitchMatchesSession(sessionId)
        && (m_activeOutputSwitch.phase == ActiveOutputSwitchPhase::WaitingForInvalidation
            || m_activeOutputSwitch.phase == ActiveOutputSwitchPhase::Preflight
            || m_activeOutputSwitch.phase == ActiveOutputSwitchPhase::Applying)
        && m_audioState == QAudio::StoppedState
        && m_audioError != QtAudio::NoError
        && absolutePositionMs + kPositionRegressionToleranceMs < m_currentPositionMs;
    if (activeSwitchTerminalPositionReset) {
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("activeOutputSwitch ignore-terminal-position-reset session=%1 phase=%2 reason=%3 previousPositionMs=%4 reportedPositionMs=%5 error=%6(%7)")
                              .arg(sessionId)
                              .arg(activeOutputSwitchPhaseName(m_activeOutputSwitch.phase))
                              .arg(m_activeOutputSwitch.reason)
                              .arg(m_currentPositionMs)
                              .arg(absolutePositionMs)
                              .arg(audioErrorName(m_audioError))
                              .arg(static_cast<int>(m_audioError)));
        return;
    }

    if (!m_positionObservationTimer.isValid()) {
        m_positionObservationTimer.start();
        m_lastPositionObservationElapsedMs = 0;
    } else {
        const qint64 elapsedMs = m_positionObservationTimer.elapsed();
        if (m_lastObservedPositionMs >= 0 && m_lastPositionObservationElapsedMs >= 0) {
            const qint64 positionDeltaMs = absolutePositionMs - m_lastObservedPositionMs;
            const qint64 wallDeltaMs = elapsedMs - m_lastPositionObservationElapsedMs;
            if (positionDeltaMs < -kPositionRegressionToleranceMs) {
                logPlaybackAnomaly(QStringLiteral("position-regression"),
                                   QStringLiteral("session=%1 positionDeltaMs=%2 wallDeltaMs=%3 previousPositionMs=%4 absolutePositionMs=%5")
                                       .arg(sessionId)
                                       .arg(positionDeltaMs)
                                       .arg(wallDeltaMs)
                                       .arg(m_lastObservedPositionMs)
                                       .arg(absolutePositionMs));
                m_flatPositionTickCount = 0;
            } else if (m_playbackState == PlaybackState::Playing
                       && m_audioState == QAudio::ActiveState
                       && !m_outputRecoveryPending) {
                if (wallDeltaMs >= 80 && positionDeltaMs <= 0 && !m_decoderFinished) {
                    ++m_flatPositionTickCount;
                    if (m_flatPositionTickCount == kPositionStallLogThreshold) {
                        logPlaybackAnomaly(QStringLiteral("position-stall"),
                                           QStringLiteral("session=%1 positionDeltaMs=%2 wallDeltaMs=%3")
                                               .arg(sessionId)
                                               .arg(positionDeltaMs)
                                               .arg(wallDeltaMs));
                    }
                } else {
                    if (wallDeltaMs > 0 && positionDeltaMs > wallDeltaMs + kPositionJumpToleranceMs) {
                        logPlaybackAnomaly(QStringLiteral("position-jump"),
                                           QStringLiteral("session=%1 positionDeltaMs=%2 wallDeltaMs=%3 previousPositionMs=%4 absolutePositionMs=%5")
                                               .arg(sessionId)
                                               .arg(positionDeltaMs)
                                               .arg(wallDeltaMs)
                                               .arg(m_lastObservedPositionMs)
                                               .arg(absolutePositionMs));
                    } else if (wallDeltaMs >= 80 && positionDeltaMs + kPositionLagToleranceMs < wallDeltaMs
                               && !m_decoderFinished) {
                        logPlaybackAnomaly(QStringLiteral("position-lag"),
                                           QStringLiteral("session=%1 positionDeltaMs=%2 wallDeltaMs=%3")
                                               .arg(sessionId)
                                               .arg(positionDeltaMs)
                                               .arg(wallDeltaMs));
                    }
                    m_flatPositionTickCount = 0;
                }
            } else {
                m_flatPositionTickCount = 0;
            }
        }
        m_lastPositionObservationElapsedMs = elapsedMs;
    }

    m_currentPositionMs = absolutePositionMs;
    m_lastObservedPositionMs = absolutePositionMs;
    const qint64 currentBucket = absolutePositionMs / 1000;
    if (currentBucket != m_lastLoggedPositionBucket) {
        m_lastLoggedPositionBucket = currentBucket;
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("position session=%1 positionMs=%2")
                              .arg(sessionId)
                              .arg(absolutePositionMs));
    }

    if (m_outputRecoveryPending
        && absolutePositionMs >= m_outputRecoveryPositionMs + kRecoveryStablePositionAdvanceMs) {
        resetOutputRecoveryState(QStringLiteral("audioPositionStable deltaMs=%1")
                                     .arg(absolutePositionMs - m_outputRecoveryPositionMs));
    }

    emit positionChanged(absolutePositionMs);
}

void WindowsWasapiAudioPlayer::handleAudioStateChanged(int sessionId, int state, int error)
{
    if (sessionId != m_activeDecoderSessionId || m_tearingDown) {
        return;
    }

    m_audioState = static_cast<QAudio::State>(state);
    m_audioError = static_cast<QtAudio::Error>(error);
    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("audioState session=%1 playbackState=%2 state=%3 error=%4(%5) decoderFinished=%6 recoveryPending=%7 recoveryAttempt=%8")
                          .arg(sessionId)
                          .arg(AudioUtils::playbackStateName(m_playbackState))
                          .arg(AudioUtils::audioStateName(m_audioState))
                          .arg(audioErrorName(m_audioError))
                          .arg(static_cast<int>(m_audioError))
                          .arg(m_decoderFinished)
                          .arg(m_outputRecoveryPending)
                          .arg(m_outputRecoveryAttempt));
    PlayerLogger::diagnostic(QStringLiteral("audio"),
                             QStringLiteral("sink_state_changed"),
                             {
                                 {QStringLiteral("sessionId"), sessionId},
                                 {QStringLiteral("positionMs"), m_currentPositionMs},
                                 {QStringLiteral("playbackState"), AudioUtils::playbackStateName(m_playbackState)},
                                 {QStringLiteral("sinkState"), AudioUtils::audioStateName(m_audioState)},
                                 {QStringLiteral("sinkError"), audioErrorName(m_audioError)},
                                 {QStringLiteral("sinkErrorCode"), static_cast<int>(m_audioError)},
                                 {QStringLiteral("transactionKind"), isActiveOutputSwitchInProgress()
                                      ? QStringLiteral("active-switch")
                                      : (m_outputRecoveryPending ? QStringLiteral("recovery") : QStringLiteral("none"))},
                                 {QStringLiteral("activeSwitchTrigger"), activeOutputSwitchTriggerName(m_activeOutputSwitch.trigger)},
                                 {QStringLiteral("activeSwitchPhase"), activeOutputSwitchPhaseName(m_activeOutputSwitch.phase)},
                                 {QStringLiteral("activeSwitchReason"), m_activeOutputSwitch.reason},
                                 {QStringLiteral("recoveryPending"), m_outputRecoveryPending},
                                 {QStringLiteral("recoveryAttempt"), m_outputRecoveryAttempt},
                             });

    const bool activeSwitchSession = activeOutputSwitchMatchesSession(sessionId);
    const auto prepareObservedInvalidationTransition = [this, sessionId] {
        if (m_activeOutputSwitch.transitionPrepared || !m_audioWorker || sessionId == 0) {
            return;
        }

        const qint64 elapsedMs = m_activeOutputSwitch.transactionTimer.isValid()
            ? m_activeOutputSwitch.transactionTimer.elapsed()
            : -1;
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("activeOutputSwitch prepare-invalidation-transition reason=%1 session=%2 trigger=%3 phase=%4 elapsedMs=%5 timing=observed-invalidation")
                              .arg(m_activeOutputSwitch.reason)
                              .arg(sessionId)
                              .arg(activeOutputSwitchTriggerName(m_activeOutputSwitch.trigger))
                              .arg(activeOutputSwitchPhaseName(m_activeOutputSwitch.phase))
                              .arg(elapsedMs));
        prepareActiveOutputInvalidationTransition(sessionId);
        m_activeOutputSwitch.transitionPrepared = true;
    };

    if (handleActiveSwitchStateChange(sessionId, activeSwitchSession)) {
        return;
    }

    if (m_playbackState == PlaybackState::Playing && !m_outputRecoveryPending && !m_decoderFinished) {
        if (m_audioState == QAudio::IdleState) {
            logPlaybackAnomaly(QStringLiteral("unexpected-idle"),
                               QStringLiteral("session=%1").arg(sessionId));
        } else if (m_audioState == QAudio::StoppedState && m_audioError == QtAudio::NoError) {
            logPlaybackAnomaly(QStringLiteral("unexpected-stop"),
                               QStringLiteral("session=%1").arg(sessionId));
        }
    }

    if (handleTerminalStateChange(sessionId)) {
        return;
    }

    handleRecoveryStateChange(sessionId);
}

bool WindowsWasapiAudioPlayer::handleActiveSwitchStateChange(int sessionId, bool activeSwitchSession)
{
    const auto prepareObservedInvalidationTransition = [this, sessionId] {
        if (m_activeOutputSwitch.transitionPrepared || !m_audioWorker || sessionId == 0) {
            return;
        }

        const qint64 elapsedMs = m_activeOutputSwitch.transactionTimer.isValid()
            ? m_activeOutputSwitch.transactionTimer.elapsed()
            : -1;
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("activeOutputSwitch prepare-invalidation-transition reason=%1 session=%2 trigger=%3 phase=%4 elapsedMs=%5 timing=observed-invalidation")
                              .arg(m_activeOutputSwitch.reason)
                              .arg(sessionId)
                              .arg(activeOutputSwitchTriggerName(m_activeOutputSwitch.trigger))
                              .arg(activeOutputSwitchPhaseName(m_activeOutputSwitch.phase))
                              .arg(elapsedMs));
        prepareActiveOutputInvalidationTransition(sessionId);
        m_activeOutputSwitch.transitionPrepared = true;
    };

    if (activeSwitchSession && m_audioState == QAudio::ActiveState
        && m_activeOutputSwitch.phase == ActiveOutputSwitchPhase::WaitingForOutputStart
        && m_audioError == QtAudio::NoError) {
        const qint64 totalElapsedMs = m_activeOutputSwitch.transactionTimer.isValid()
            ? m_activeOutputSwitch.transactionTimer.elapsed()
            : -1;
        const qint64 rebuildElapsedMs = m_activeOutputSwitch.rebuildTimer.isValid()
            ? m_activeOutputSwitch.rebuildTimer.elapsed()
            : -1;
        const qint64 lastAbsorbedToActiveMs =
            totalElapsedMs >= 0 && m_activeOutputSwitch.lastAbsorbedElapsedMs >= 0
            ? totalElapsedMs - m_activeOutputSwitch.lastAbsorbedElapsedMs
            : -1;
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("activeOutputSwitch output-active-summary session=%1 trigger=%2 reason=%3 totalElapsedMs=%4 lastAbsorbedToActiveMs=%5 rebuildElapsedMs=%6 absorbedOutputErrors=%7 outputStartRetries=%8 watchdogRebuilds=%9 conservativeRebuilds=%10")
                              .arg(sessionId)
                              .arg(activeOutputSwitchTriggerName(m_activeOutputSwitch.trigger))
                              .arg(m_activeOutputSwitch.reason)
                              .arg(totalElapsedMs)
                              .arg(lastAbsorbedToActiveMs)
                              .arg(rebuildElapsedMs)
                              .arg(m_activeOutputSwitch.absorbedOutputErrorCount)
                              .arg(m_activeOutputSwitch.outputStartErrorRetryCount)
                              .arg(m_activeOutputSwitch.invalidationWatchdogRebuildCount)
                              .arg(m_activeOutputSwitch.conservativeRebuildCount));
        PlayerLogger::diagnostic(QStringLiteral("player"),
                                 QStringLiteral("output_switch_done"),
                                 {
                                     {QStringLiteral("transactionKind"), QStringLiteral("active-switch")},
                                     {QStringLiteral("reason"), QStringLiteral("output-active")},
                                     {QStringLiteral("sessionId"), sessionId},
                                     {QStringLiteral("positionMs"), m_currentPositionMs},
                                     {QStringLiteral("trigger"), activeOutputSwitchTriggerName(m_activeOutputSwitch.trigger)},
                                     {QStringLiteral("phase"), activeOutputSwitchPhaseName(m_activeOutputSwitch.phase)},
                                     {QStringLiteral("transactionReason"), m_activeOutputSwitch.reason},
                                     {QStringLiteral("totalElapsedMs"), totalElapsedMs},
                                     {QStringLiteral("lastAbsorbedToActiveMs"), lastAbsorbedToActiveMs},
                                     {QStringLiteral("rebuildElapsedMs"), rebuildElapsedMs},
                                     {QStringLiteral("absorbedOutputErrors"), m_activeOutputSwitch.absorbedOutputErrorCount},
                                     {QStringLiteral("outputStartRetries"), m_activeOutputSwitch.outputStartErrorRetryCount},
                                     {QStringLiteral("watchdogRebuilds"), m_activeOutputSwitch.invalidationWatchdogRebuildCount},
                                     {QStringLiteral("conservativeRebuilds"), m_activeOutputSwitch.conservativeRebuildCount},
                                 });
        if (m_activeOutputSwitch.conservativeRebuildCount > 0) {
            PlayerLogger::log(QStringLiteral("player"),
                              QStringLiteral("activeOutputSwitch rebuild-status-complete session=%1 trigger=%2 reason=%3")
                                  .arg(sessionId)
                                  .arg(activeOutputSwitchTriggerName(m_activeOutputSwitch.trigger))
                                  .arg(m_activeOutputSwitch.reason));
            emit statusMessage(tr("播放管线已重建"));
        }
        resetActiveOutputSwitch(QStringLiteral("output-active"));
    } else if (activeSwitchSession && m_audioState == QAudio::StoppedState) {
        if (m_audioError == QtAudio::NoError) {
            PlayerLogger::log(QStringLiteral("player"),
                              QStringLiteral("activeOutputSwitch observed-output-stop session=%1 trigger=%2 phase=%3 reason=%4 error=%5(%6)")
                                  .arg(sessionId)
                                  .arg(activeOutputSwitchTriggerName(m_activeOutputSwitch.trigger))
                                  .arg(activeOutputSwitchPhaseName(m_activeOutputSwitch.phase))
                                  .arg(m_activeOutputSwitch.reason)
                                  .arg(audioErrorName(m_audioError))
                                  .arg(static_cast<int>(m_audioError)));
            if (m_activeOutputSwitch.phase == ActiveOutputSwitchPhase::WaitingForInvalidation) {
                quarantineBufferDevice(QStringLiteral("activeOutputSwitch:%1").arg(m_activeOutputSwitch.reason), false);
                m_activeOutputSwitch.needsFreshBuffer = true;
                m_activeOutputSwitch.forceReconfigure = true;
                PlayerLogger::log(QStringLiteral("player"),
                                  QStringLiteral("activeOutputSwitch absorbed-output-stop session=%1 trigger=%2 phase=%3 reason=%4 timerActive=%5 freshBufferRequired=1")
                                      .arg(sessionId)
                                      .arg(activeOutputSwitchTriggerName(m_activeOutputSwitch.trigger))
                                      .arg(activeOutputSwitchPhaseName(m_activeOutputSwitch.phase))
                                      .arg(m_activeOutputSwitch.reason)
                                      .arg(m_outputDeviceChangeTimer && m_outputDeviceChangeTimer->isActive()));
                prepareObservedInvalidationTransition();
                const int expectedSessionId = sessionId;
                QTimer::singleShot(0, this, [this, expectedSessionId] {
                    if (!activeOutputSwitchMatchesSession(expectedSessionId)
                        || m_activeOutputSwitch.phase != ActiveOutputSwitchPhase::WaitingForInvalidation) {
                        return;
                    }

                    applyActiveOutputSwitch();
                });
            }
            return true;
        }

        if (m_activeOutputSwitch.phase == ActiveOutputSwitchPhase::WaitingForInvalidation
            || m_activeOutputSwitch.phase == ActiveOutputSwitchPhase::Pending
            || m_activeOutputSwitch.phase == ActiveOutputSwitchPhase::Preflight
            || m_activeOutputSwitch.phase == ActiveOutputSwitchPhase::OutputSuspended) {
            const bool shouldApplyImmediately =
                m_activeOutputSwitch.phase == ActiveOutputSwitchPhase::WaitingForInvalidation;
            quarantineBufferDevice(QStringLiteral("activeOutputSwitch:%1").arg(m_activeOutputSwitch.reason), false);
            m_activeOutputSwitch.needsFreshBuffer = true;
            m_activeOutputSwitch.forceReconfigure = true;
            ++m_activeOutputSwitch.absorbedOutputErrorCount;
            m_activeOutputSwitch.lastAbsorbedElapsedMs = m_activeOutputSwitch.transactionTimer.isValid()
                ? m_activeOutputSwitch.transactionTimer.elapsed()
                : -1;
            PlayerLogger::log(QStringLiteral("player"),
                              QStringLiteral("activeOutputSwitch absorbed-output-error session=%1 trigger=%2 phase=%3 reason=%4 error=%5(%6) timerActive=%7 absorbedCount=%8 elapsedMs=%9 freshBufferRequired=1")
                                  .arg(sessionId)
                                  .arg(activeOutputSwitchTriggerName(m_activeOutputSwitch.trigger))
                                  .arg(activeOutputSwitchPhaseName(m_activeOutputSwitch.phase))
                                  .arg(m_activeOutputSwitch.reason)
                                  .arg(audioErrorName(m_audioError))
                                  .arg(static_cast<int>(m_audioError))
                                  .arg(m_outputDeviceChangeTimer && m_outputDeviceChangeTimer->isActive())
                                  .arg(m_activeOutputSwitch.absorbedOutputErrorCount)
                                  .arg(m_activeOutputSwitch.lastAbsorbedElapsedMs));
            PlayerLogger::diagnostic(QStringLiteral("audio"),
                                     QStringLiteral("sink_backend_error"),
                                     {
                                         {QStringLiteral("transactionKind"), QStringLiteral("active-switch")},
                                         {QStringLiteral("eventScope"), QStringLiteral("absorbed-output-error")},
                                         {QStringLiteral("sessionId"), sessionId},
                                         {QStringLiteral("positionMs"), m_currentPositionMs},
                                         {QStringLiteral("trigger"), activeOutputSwitchTriggerName(m_activeOutputSwitch.trigger)},
                                         {QStringLiteral("phase"), activeOutputSwitchPhaseName(m_activeOutputSwitch.phase)},
                                         {QStringLiteral("reason"), m_activeOutputSwitch.reason},
                                         {QStringLiteral("sinkError"), audioErrorName(m_audioError)},
                                         {QStringLiteral("sinkErrorCode"), static_cast<int>(m_audioError)},
                                         {QStringLiteral("freshBufferRequired"), true},
                                     });
            if (shouldApplyImmediately) {
                prepareObservedInvalidationTransition();
                const int expectedSessionId = sessionId;
                QTimer::singleShot(0, this, [this, expectedSessionId] {
                    if (!activeOutputSwitchMatchesSession(expectedSessionId)
                        || m_activeOutputSwitch.phase != ActiveOutputSwitchPhase::WaitingForInvalidation) {
                        return;
                    }

                    applyActiveOutputSwitch();
                });
            }
            return true;
        }

        if (m_activeOutputSwitch.phase == ActiveOutputSwitchPhase::WaitingForOutputStart
            && m_currentPipelineStartupProfile == PipelineStartupProfile::ActiveSwitchRebuild
            && m_activeOutputSwitch.outputStartErrorRetryCount < kActiveSwitchOutputStartErrorRetryLimit) {
            ++m_activeOutputSwitch.outputStartErrorRetryCount;
            quarantineBufferDevice(QStringLiteral("activeOutputSwitch:%1").arg(m_activeOutputSwitch.reason), false);
            m_activeOutputSwitch.needsFreshBuffer = true;
            m_activeOutputSwitch.forceReconfigure = true;
            m_activeOutputSwitch.lastAbsorbedElapsedMs = m_activeOutputSwitch.transactionTimer.isValid()
                ? m_activeOutputSwitch.transactionTimer.elapsed()
                : -1;
            PlayerLogger::log(QStringLiteral("player"),
                              QStringLiteral("activeOutputSwitch absorbed-output-start-error session=%1 trigger=%2 phase=%3 reason=%4 error=%5(%6) retry=%7 retryLimit=%8 elapsedMs=%9 freshBufferRequired=1")
                                  .arg(sessionId)
                                  .arg(activeOutputSwitchTriggerName(m_activeOutputSwitch.trigger))
                                  .arg(activeOutputSwitchPhaseName(m_activeOutputSwitch.phase))
                                  .arg(m_activeOutputSwitch.reason)
                                  .arg(audioErrorName(m_audioError))
                                  .arg(static_cast<int>(m_audioError))
                                  .arg(m_activeOutputSwitch.outputStartErrorRetryCount)
                                  .arg(kActiveSwitchOutputStartErrorRetryLimit)
                                  .arg(m_activeOutputSwitch.lastAbsorbedElapsedMs));
            PlayerLogger::diagnostic(QStringLiteral("audio"),
                                     QStringLiteral("sink_backend_error"),
                                     {
                                         {QStringLiteral("transactionKind"), QStringLiteral("active-switch")},
                                         {QStringLiteral("eventScope"), QStringLiteral("absorbed-output-start-error")},
                                         {QStringLiteral("sessionId"), sessionId},
                                         {QStringLiteral("positionMs"), m_currentPositionMs},
                                         {QStringLiteral("trigger"), activeOutputSwitchTriggerName(m_activeOutputSwitch.trigger)},
                                         {QStringLiteral("phase"), activeOutputSwitchPhaseName(m_activeOutputSwitch.phase)},
                                         {QStringLiteral("reason"), m_activeOutputSwitch.reason},
                                         {QStringLiteral("sinkError"), audioErrorName(m_audioError)},
                                         {QStringLiteral("sinkErrorCode"), static_cast<int>(m_audioError)},
                                         {QStringLiteral("retry"), m_activeOutputSwitch.outputStartErrorRetryCount},
                                     });
            const int expectedSessionId = sessionId;
            QTimer::singleShot(0, this, [this, expectedSessionId] {
                if (!activeOutputSwitchMatchesSession(expectedSessionId)
                    || m_activeOutputSwitch.phase != ActiveOutputSwitchPhase::WaitingForOutputStart) {
                    return;
                }

                applyActiveOutputSwitch();
            });
            return true;
        }

        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("activeOutputSwitch output-error session=%1 trigger=%2 phase=%3 reason=%4 error=%5(%6) outputStartRetries=%7")
                              .arg(sessionId)
                              .arg(activeOutputSwitchTriggerName(m_activeOutputSwitch.trigger))
                              .arg(activeOutputSwitchPhaseName(m_activeOutputSwitch.phase))
                              .arg(m_activeOutputSwitch.reason)
                              .arg(audioErrorName(m_audioError))
                              .arg(static_cast<int>(m_audioError))
                              .arg(m_activeOutputSwitch.outputStartErrorRetryCount));
        resetActiveOutputSwitch(QStringLiteral("output-error"));
        resetOutputRecoveryState(QStringLiteral("active-output-switch-error"));
        teardownPipeline();
        setPlaybackState(PlaybackState::Stopped);
        emit errorOccurred(PlaybackError::OutputSwitchFailure,
                           tr("音频输出切换失败\n\n"
                               "可能原因：设备被占用或格式不支持\n\n"
                               "建议操作：\n"
                               "1. 检查音频设备连接\n"
                               "2. 尝试其他输出设备\n"
                               "3. 重新播放"));
        return true;
    }

    return false;
}

bool WindowsWasapiAudioPlayer::handleTerminalStateChange(int sessionId)
{
    if (m_audioState == QAudio::IdleState && m_decoderFinished) {
        finalizePlayback();
        return true;
    }
    return false;
}

void WindowsWasapiAudioPlayer::handleRecoveryStateChange(int sessionId)
{
    if (m_audioState != QAudio::StoppedState || m_audioError == QtAudio::NoError) {
        return;
    }

    if (shouldFinalizeAfterEndOfStreamOutputStop()) {
        const auto *buffer = static_cast<const PcmStreamBuffer *>(m_bufferDevice);
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("outputStopAfterEndOfStream finalize session=%1 error=%2(%3) bufferedBytes=%4 positionMs=%5")
                              .arg(sessionId)
                              .arg(audioErrorName(m_audioError))
                              .arg(static_cast<int>(m_audioError))
                              .arg(buffer ? buffer->bufferedBytes() : 0)
                              .arg(m_currentPositionMs));
        finalizePlayback();
        return;
    }

    if (shouldAttemptOutputRecovery(m_audioState, m_audioError)) {
        scheduleOutputRecovery(sessionId, m_audioError);
        return;
    }

    resetOutputRecoveryState(QStringLiteral("fatal-output-error"));
    teardownPipeline();
    setPlaybackState(PlaybackState::Stopped);
    emit errorOccurred(PlaybackError::OutputError,
                       tr("音频输出错误（代码 %1）\n\n"
                           "建议操作：\n"
                           "1. 检查音频设备连接\n"
                           "2. 关闭其他音频应用\n"
                           "3. 重新播放")
                           .arg(static_cast<int>(m_audioError)));
}

void WindowsWasapiAudioPlayer::handleDecoderDataAvailable(int sessionId)
{
    if (sessionId != m_activeDecoderSessionId) {
        return;
    }

    if (m_currentPipelineStartupProfile == PipelineStartupProfile::SeekResume
        && !m_seekResumeFirstDecodedPcmLogged) {
        m_seekResumeFirstDecodedPcmLogged = true;
        const qint64 firstDecodedPcmAfterSeekMs = QDateTime::currentMSecsSinceEpoch();
        const auto *buffer = static_cast<const PcmStreamBuffer *>(m_bufferDevice);
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("seekResumeTiming marker=first-decoded session=%1 seekRequestTimeMs=%2 firstDecodedPcmAfterSeekMs=%3 bufferedBytes=%4 thresholdBytes=%5")
                              .arg(sessionId)
                              .arg(m_lastSeekRequestTimeMs)
                              .arg(firstDecodedPcmAfterSeekMs)
                              .arg(buffer ? buffer->bufferedBytes() : -1)
                              .arg(startupThresholdBytes()));
        PlayerLogger::diagnostic(QStringLiteral("player"),
                                 QStringLiteral("seek_resume_first_decoded_pcm"),
                                 {
                                     {QStringLiteral("sessionId"), sessionId},
                                     {QStringLiteral("seekRequestTimeMs"), m_lastSeekRequestTimeMs},
                                     {QStringLiteral("firstDecodedPcmAfterSeekMs"), firstDecodedPcmAfterSeekMs},
                                     {QStringLiteral("bufferedBytes"), static_cast<qint64>(buffer ? buffer->bufferedBytes() : -1)},
                                     {QStringLiteral("startupThresholdBytes"), static_cast<qint64>(startupThresholdBytes())},
                                 });
    }

    if (m_currentPipelineStartupProfile == PipelineStartupProfile::ActiveSwitchRebuild
        && !m_audioStarted) {
        const auto *buffer = static_cast<const PcmStreamBuffer *>(m_bufferDevice);
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("activeSwitchRebuildTiming marker=decoder-data-available session=%1 bufferedBytes=%2 thresholdBytes=%3 decoderFinished=%4 audioStarted=%5")
                              .arg(sessionId)
                              .arg(buffer ? buffer->bufferedBytes() : -1)
                              .arg(startupThresholdBytes())
                              .arg(m_decoderFinished)
                              .arg(m_audioStarted));
    }
    startAudioOutputIfReady();
}



