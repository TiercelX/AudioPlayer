#include "ffmpegaudioplayer.h"

#include "audioutils.h"
#include "pcmutils.h"
#include "playerlogger.h"
#include "toollocator.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTimer>

namespace {

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

int outputRecoveryDelayMsForAttempt(int attempt)
{
    switch (attempt) {
    case 1:
        return 150;
    case 2:
        return 300;
    case 3:
        return 600;
    default:
        return -1;
    }
}

} // namespace

QString FfmpegAudioPlayer::channelLayoutForCount(int channelCount) const
{
    return AudioUtils::channelLayoutForCount(channelCount);
}

QString FfmpegAudioPlayer::locateFfmpegExecutable() const
{
    return AudioUtils::locateFfmpegExecutable();
}

QString FfmpegAudioPlayer::pcmCodecName(QAudioFormat::SampleFormat sampleFormat) const
{
    return AudioUtils::pcmCodecName(PcmUtils::fromQAudioSampleFormat(sampleFormat));
}

QString FfmpegAudioPlayer::pcmSampleFormatName(QAudioFormat::SampleFormat sampleFormat) const
{
    return AudioUtils::pcmSampleFormatName(PcmUtils::fromQAudioSampleFormat(sampleFormat));
}

QString FfmpegAudioPlayer::pcmMuxerName(QAudioFormat::SampleFormat sampleFormat) const
{
    return AudioUtils::pcmMuxerName(PcmUtils::fromQAudioSampleFormat(sampleFormat));
}

qsizetype FfmpegAudioPlayer::startupThresholdBytes() const
{
    if (!m_outputFormat.isValid()) {
        return 32768;
    }

    const qsizetype bytesPerTenthSecond = static_cast<qsizetype>(m_outputFormat.bytesPerFrame())
        * m_outputFormat.sampleRate() / 10;
    const bool isLocalFile = !m_sourcePath.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive)
        && !m_sourcePath.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive)
        && !m_sourcePath.startsWith(QStringLiteral("rtsp://"), Qt::CaseInsensitive);
    const int multiplier = isLocalFile ? 1 : 2;
    return qMax<qsizetype>(32768, bytesPerTenthSecond * multiplier);
}

QString FfmpegAudioPlayer::rawInputFormatForSource() const
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

void FfmpegAudioPlayer::resetOutputRecoveryState(const QString &reason)
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

bool FfmpegAudioPlayer::shouldAttemptOutputRecovery(QAudio::State state, QtAudio::Error error) const
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

void FfmpegAudioPlayer::scheduleOutputRecovery(int failedSessionId, QtAudio::Error error)
{
#ifndef Q_OS_WINDOWS
    Q_UNUSED(failedSessionId);
    Q_UNUSED(error);
    return;
#else
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
        emit errorOccurred(PlaybackError::OutputError, tr("音频输出错误"));
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
        startPipeline(m_outputRecoveryPositionMs);
    });
#endif
}

void FfmpegAudioPlayer::finalizePlayback()
{
    resetOutputRecoveryState(QStringLiteral("finalizePlayback"));
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

void FfmpegAudioPlayer::handleAudioPositionUpdated(int sessionId, qint64 processedPositionMs)
{
    if (sessionId != m_activeDecoderSessionId || m_playbackState == PlaybackState::Stopped
        || m_playbackState == PlaybackState::Stopping) {
        return;
    }

    const qint64 absolutePositionMs = m_startPositionMs + processedPositionMs;
    m_currentPositionMs = absolutePositionMs;
    const qint64 currentBucket = absolutePositionMs / 1000;
    if (currentBucket != m_lastLoggedPositionBucket) {
        m_lastLoggedPositionBucket = currentBucket;
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("position session=%1 positionMs=%2")
                              .arg(sessionId)
                              .arg(absolutePositionMs));
    }

    emit positionChanged(absolutePositionMs);
}

void FfmpegAudioPlayer::handleAudioStateChanged(int sessionId, int state, int error)
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

    if (m_audioState == QAudio::ActiveState && m_outputRecoveryPending) {
        resetOutputRecoveryState(QStringLiteral("audioActive"));
    }

    if (m_audioState == QAudio::IdleState && m_decoderFinished) {
        finalizePlayback();
        return;
    }

    if (m_audioState == QAudio::StoppedState && m_audioError != QtAudio::NoError) {
        if (shouldAttemptOutputRecovery(m_audioState, m_audioError)) {
            scheduleOutputRecovery(sessionId, m_audioError);
            return;
        }

        resetOutputRecoveryState(QStringLiteral("fatal-output-error"));
        teardownPipeline();
        setPlaybackState(PlaybackState::Stopped);
        emit errorOccurred(PlaybackError::OutputError, tr("音频输出错误"));
    }
}

void FfmpegAudioPlayer::handleDecoderDataAvailable(int sessionId)
{
    if (sessionId != m_activeDecoderSessionId) {
        return;
    }

    startAudioOutputIfReady();
}



