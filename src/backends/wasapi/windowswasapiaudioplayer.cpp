#include "windowswasapiaudioplayer.h"

#include "audioartifactmonitor.h"
#include "audioutils.h"
#include "ffmpegpcmshared.h"
#include "libavseekdecoderworker.h"
#include "pcmseekcache.h"
#include "playbacksourceservice.h"
#include "playerlogger.h"

#include <QAudioDevice>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QIODevice>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMediaDevices>
#include <QProcess>
#include <QProcessEnvironment>
#include <QElapsedTimer>
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

#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

#include "windowswasapiaudioplayer_worker.h"

namespace {

constexpr qsizetype kMinBytesPerHalfSecond = 65536;
constexpr qsizetype kSeekResumeMinThresholdBytes = 32768;
constexpr int kBufferHalfSecondsNormal = 6;
constexpr int kBufferHalfSecondsStability = 8;

} // namespace

WindowsWasapiAudioPlayer::WindowsWasapiAudioPlayer(QObject *parent)
    : AudioPlayerBackend(parent)
    , m_audioThread(new QThread(this))
    , m_audioWorker(new WasapiOutputWorker)
    , m_decoderThread(new QThread(this))
    , m_decoderWorker(new FfmpegDecoderWorker)
    , m_libavSeekDecoderWorker(new LibavSeekDecoderWorker)
    , m_mediaDevices(new QMediaDevices(this))
    , m_outputDeviceChangeTimer(new QTimer(this))
{
    m_appStartTimeUtc = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    PlayerLogger::log(QStringLiteral("player"), QStringLiteral("WindowsWasapiAudioPlayer initializing"));
    m_audioWorker->moveToThread(m_audioThread);
    connect(m_audioThread, &QThread::finished, m_audioWorker, &QObject::deleteLater);
    connect(m_audioWorker, &WasapiOutputWorker::positionUpdated,
            this, &WindowsWasapiAudioPlayer::handleAudioPositionUpdated);
    connect(m_audioWorker, &WasapiOutputWorker::stateChanged,
            this, &WindowsWasapiAudioPlayer::handleAudioStateChanged);
    m_audioThread->start();

    m_decoderWorker->moveToThread(m_decoderThread);
    connect(m_decoderThread, &QThread::finished, m_decoderWorker, &QObject::deleteLater);
    connect(m_decoderWorker, &FfmpegDecoderWorker::dataAvailable,
            this, &WindowsWasapiAudioPlayer::handleDecoderDataAvailable);
    connect(m_decoderWorker, &FfmpegDecoderWorker::audioLevelsChanged, this,
            [this](int sessionId, qreal leftLevel, qreal rightLevel) {
                if (sessionId != m_activeDecoderSessionId || m_playbackState == PlaybackState::Stopping) {
                    return;
                }

                emitAudioLevels(leftLevel, rightLevel);
            });
    connect(m_decoderWorker, &FfmpegDecoderWorker::errorOccurred,
            this, &WindowsWasapiAudioPlayer::handleDecoderError);
    connect(m_decoderWorker, &FfmpegDecoderWorker::finished,
            this, &WindowsWasapiAudioPlayer::handleDecoderFinished);

    m_libavSeekDecoderWorker->moveToThread(m_decoderThread);
    connect(m_decoderThread, &QThread::finished, m_libavSeekDecoderWorker, &QObject::deleteLater);
    connect(m_libavSeekDecoderWorker, &LibavSeekDecoderWorker::dataAvailable,
            this, &WindowsWasapiAudioPlayer::handleDecoderDataAvailable);
    connect(m_libavSeekDecoderWorker, &LibavSeekDecoderWorker::audioLevelsChanged, this,
            [this](int sessionId, qreal leftLevel, qreal rightLevel) {
                if (sessionId != m_activeDecoderSessionId || m_playbackState == PlaybackState::Stopping) {
                    return;
                }

                emitAudioLevels(leftLevel, rightLevel);
            });
    connect(m_libavSeekDecoderWorker, &LibavSeekDecoderWorker::errorOccurred,
            this, &WindowsWasapiAudioPlayer::handleDecoderError);
    connect(m_libavSeekDecoderWorker, &LibavSeekDecoderWorker::finished,
            this, &WindowsWasapiAudioPlayer::handleDecoderFinished);
    m_decoderThread->start();

    m_outputDeviceChangeTimer->setSingleShot(true);
    m_outputDeviceChangeTimer->setInterval(kOutputDeviceChangeDebounceMs);
    connect(m_outputDeviceChangeTimer, &QTimer::timeout, this, [this] {
        if (!isActiveOutputSwitchInProgress()) {
            PlayerLogger::log(QStringLiteral("player"),
                              QStringLiteral("activeOutputSwitch debounce-skipped no-active-transaction"));
            return;
        }

        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("activeOutputSwitch debounce-fired intervalMs=%1 playbackState=%2 trigger=%3 phase=%4 force=%5")
                              .arg(m_outputDeviceChangeTimer->interval())
                              .arg(AudioUtils::playbackStateName(m_playbackState))
                              .arg(activeOutputSwitchTriggerName(m_activeOutputSwitch.trigger))
                              .arg(activeOutputSwitchPhaseName(m_activeOutputSwitch.phase))
                              .arg(m_activeOutputSwitch.forceReconfigure));
        applyActiveOutputSwitch();
    });

    connect(m_mediaDevices, &QMediaDevices::audioOutputsChanged,
            this, &WindowsWasapiAudioPlayer::handleAudioOutputsChanged);
}

WindowsWasapiAudioPlayer::~WindowsWasapiAudioPlayer()
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
    if (m_libavSeekDecoderWorker) {
        QMetaObject::invokeMethod(m_libavSeekDecoderWorker, [worker = m_libavSeekDecoderWorker] {
            worker->stopDecoding(true);
            worker->releaseSource();
        }, Qt::BlockingQueuedConnection);
    }
    delete m_pcmSeekCache;
    m_pcmSeekCache = nullptr;
    if (m_decoderThread) {
        m_decoderThread->quit();
        m_decoderThread->wait(1000);
    }
}
void WindowsWasapiAudioPlayer::setSource(const QString &filePath,
                                         int sourceChannelCount,
                                         int sourceSampleRate,
                                         int sourceBitDepth,
                                         const QString &sourceCodecName)
{
    const bool sourceSwitch = !m_sourcePath.isEmpty() && m_sourcePath != filePath;
    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("setSource path=%1 channels=%2 sampleRate=%3 bitDepth=%4 codec=%5 sourceSwitch=%6 previous=%7")
                          .arg(filePath)
                          .arg(sourceChannelCount)
                          .arg(sourceSampleRate)
                          .arg(sourceBitDepth)
                          .arg(sourceCodecName)
                          .arg(sourceSwitch)
                          .arg(m_sourcePath));
    if (sourceSwitch) {
        m_previousSourcePath = m_sourcePath;
    }
    resetOutputRecoveryState(QStringLiteral("setSource"));
    resetAnomalyTracking(QStringLiteral("setSource"));
    teardownPipeline();

    if (m_libavSeekDecoderWorker && sourceSwitch) {
        QMetaObject::invokeMethod(m_libavSeekDecoderWorker, [worker = m_libavSeekDecoderWorker] {
            worker->releaseSource();
        }, Qt::BlockingQueuedConnection);
    }

    if (sourceSwitch && m_pcmSeekCache) {
        m_pcmSeekCache->clear();
    }

    m_sourcePath = filePath;
    m_pendingSourceSwitchStartup = sourceSwitch;
    m_pendingSeekResumeStartup = false;
    m_lastSeekRequestTimeMs = -1;
    m_lastSeekRequestPositionMs = -1;
    m_seekResumeFirstDecodedPcmLogged = false;
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

QString WindowsWasapiAudioPlayer::source() const
{
    return m_sourcePath;
}

AudioPlayerBackend::BackendId WindowsWasapiAudioPlayer::backendId() const
{
    return BackendId::WindowsWasapi;
}

QString WindowsWasapiAudioPlayer::backendName() const
{
    if (m_stabilityModeEnabled) {
        return tr("WASAPI stable shared");
    }
    return m_exclusiveModeEnabled ? tr("WASAPI exclusive") : tr("WASAPI shared");
}

bool WindowsWasapiAudioPlayer::exclusiveModeEnabled() const
{
    return m_exclusiveModeEnabled;
}

void WindowsWasapiAudioPlayer::setExclusiveModeEnabled(bool enabled)
{
    if (enabled && m_stabilityModeEnabled) {
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("exclusiveMode ignored stabilityMode=1"));
        enabled = false;
    }
    if (m_exclusiveModeEnabled == enabled) {
        return;
    }

    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("exclusiveMode request previous=%1 enabled=%2")
                          .arg(m_exclusiveModeEnabled ? 1 : 0)
                          .arg(enabled ? 1 : 0));
    m_exclusiveModeEnabled = enabled;
    m_ignoreNextUnchangedOutputChangeAfterExclusiveFallback = false;
}

bool WindowsWasapiAudioPlayer::stabilityModeEnabled() const
{
    return m_stabilityModeEnabled;
}

void WindowsWasapiAudioPlayer::setStabilityModeEnabled(bool enabled)
{
    if (m_stabilityModeEnabled == enabled) {
        return;
    }

    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("stabilityMode request previous=%1 enabled=%2 outputBufferMs=%3")
                          .arg(m_stabilityModeEnabled ? 1 : 0)
                          .arg(enabled ? 1 : 0)
                          .arg(kStabilityModeOutputBufferMs));
    m_stabilityModeEnabled = enabled;
    if (m_stabilityModeEnabled && m_exclusiveModeEnabled) {
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("stabilityMode disabled-exclusive-mode"));
        m_exclusiveModeEnabled = false;
        m_ignoreNextUnchangedOutputChangeAfterExclusiveFallback = false;
    }
    if (m_audioThread && m_audioThread->isRunning()) {
        m_audioThread->setPriority(m_stabilityModeEnabled ? QThread::TimeCriticalPriority
                                                          : QThread::NormalPriority);
    }
}

int WindowsWasapiAudioPlayer::creativeChannelReorderMode() const
{
    return static_cast<int>(m_creativeChannelReorderMode);
}

void WindowsWasapiAudioPlayer::setCreativeChannelReorderMode(int mode)
{
    const auto newMode = static_cast<CreativeChannelReorderMode>(mode);
    if (m_creativeChannelReorderMode == newMode) {
        return;
    }
    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("creativeChannelReorderMode request previous=%1 mode=%2")
                          .arg(static_cast<int>(m_creativeChannelReorderMode))
                          .arg(static_cast<int>(newMode)));
    m_creativeChannelReorderMode = newMode;
}

bool WindowsWasapiAudioPlayer::exactPlaybackEnabled() const
{
    return m_exactPlaybackEnabled;
}

void WindowsWasapiAudioPlayer::setExactPlaybackEnabled(bool enabled)
{
    if (m_exactPlaybackEnabled == enabled) {
        return;
    }
    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("exactPlayback request previous=%1 enabled=%2")
                          .arg(m_exactPlaybackEnabled ? 1 : 0)
                          .arg(enabled ? 1 : 0));
    m_exactPlaybackEnabled = enabled;
}

QString WindowsWasapiAudioPlayer::decoderName() const
{
    if (m_sourceCodecName.isEmpty()) {
        return {};
    }

#ifdef AUDIOPLAYER_LIBAV_DECODER
    if (isPackagedLibavAudioCodec(m_sourceCodecName)
        && !wasapiLibavDecoderDisabled()
        && m_libavSeekDecoderWorker != nullptr) {
        return tr("libav (in-process)");
    }
#endif

    return tr("ffmpeg CLI");
}

void WindowsWasapiAudioPlayer::play()
{
    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("play state=%1 startPositionMs=%2 activeSession=%3 pendingSeekResume=%4")
                          .arg(AudioUtils::playbackStateName(m_playbackState))
                          .arg(m_startPositionMs)
                          .arg(m_activeDecoderSessionId)
                          .arg(m_pendingSeekResumeStartup));
    if (m_sourcePath.isEmpty()) {
        return;
    }

    if (m_playbackState == PlaybackState::Stopping) {
        return;
    }

    if (m_playbackState == PlaybackState::Paused && m_audioStarted && m_audioWorker) {
        resetAnomalyTracking(QStringLiteral("resume"));
        const int sessionId = m_activeDecoderSessionId;
        QMetaObject::invokeMethod(m_audioWorker, [worker = m_audioWorker, sessionId] {
            worker->resumeOutput(sessionId);
        }, Qt::QueuedConnection);
        setPlaybackState(PlaybackState::Playing);
        return;
    }

    if (m_playbackState == PlaybackState::Paused || m_playbackState == PlaybackState::Stopped) {
        resetOutputRecoveryState(QStringLiteral("play"));
        const PipelineStartupProfile startupProfile = m_pendingSeekResumeStartup
            ? PipelineStartupProfile::SeekResume
            : PipelineStartupProfile::NormalStart;
        m_pendingSeekResumeStartup = false;
        startPipeline(m_startPositionMs, startupProfile);
    }
}

void WindowsWasapiAudioPlayer::pause()
{
    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("pause state=%1 activeSession=%2 audioStarted=%3")
                          .arg(AudioUtils::playbackStateName(m_playbackState))
                          .arg(m_activeDecoderSessionId)
                          .arg(m_audioStarted));
    if (m_playbackState != PlaybackState::Playing || !m_audioStarted || !m_audioWorker) {
        return;
    }

    resetAnomalyTracking(QStringLiteral("pause"));
    const int sessionId = m_activeDecoderSessionId;
    QMetaObject::invokeMethod(m_audioWorker, [worker = m_audioWorker, sessionId] {
        worker->pauseOutput(sessionId);
    }, Qt::QueuedConnection);
    setPlaybackState(PlaybackState::Paused);
}

void WindowsWasapiAudioPlayer::stop()
{
    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("stop state=%1 activeSession=%2 stoppingSession=%3")
                          .arg(AudioUtils::playbackStateName(m_playbackState))
                          .arg(m_activeDecoderSessionId)
                          .arg(m_stoppingDecoderSessionId));
    resetOutputRecoveryState(QStringLiteral("stop"));
    resetActiveOutputSwitch(QStringLiteral("stop"));
    resetAnomalyTracking(QStringLiteral("stop"));
    if (m_outputDeviceChangeTimer) {
        m_outputDeviceChangeTimer->stop();
    }
    m_audioStarted = false;
    m_decoderFinished = false;
    m_pendingSeekResumeStartup = false;
    m_currentPositionMs = 0;
    m_startPositionMs = 0;
    m_audioState = QAudio::StoppedState;
    m_audioError = QtAudio::NoError;
    m_lastLoggedPositionBucket = -1;
    m_lastSeekRequestTimeMs = -1;
    m_lastSeekRequestPositionMs = -1;
    m_seekResumeFirstDecodedPcmLogged = false;
    emitAudioLevels(0.0, 0.0);
    emit positionChanged(0);
    releaseOutputResources(true);
    quarantineBufferDevice(QStringLiteral("stop"));

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

void WindowsWasapiAudioPlayer::seek(qint64 positionMs)
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

    if (m_playbackState == PlaybackState::Stopped) {
        const qint64 clampedPosition = qMax<qint64>(0, positionMs);
        m_currentPositionMs = clampedPosition;
        m_startPositionMs = clampedPosition;
        emit positionChanged(clampedPosition);
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("seek stored for stopped state positionMs=%1").arg(clampedPosition));
        return;
    }

    const qint64 clampedPosition = qMax<qint64>(0, positionMs);
    m_lastSeekRequestTimeMs = QDateTime::currentMSecsSinceEpoch();
    m_lastSeekRequestPositionMs = clampedPosition;
    m_seekResumeFirstDecodedPcmLogged = false;
    PlayerLogger::diagnostic(QStringLiteral("player"),
                             QStringLiteral("seek_resume_request"),
                             {
                                 {QStringLiteral("seekRequestTimeMs"), m_lastSeekRequestTimeMs},
                                 {QStringLiteral("targetPositionMs"), clampedPosition},
                                 {QStringLiteral("state"), AudioUtils::playbackStateName(m_playbackState)},
                                 {QStringLiteral("activeSessionId"), m_activeDecoderSessionId},
                             });
    resetOutputRecoveryState(QStringLiteral("seek"));
    if (m_playbackState == PlaybackState::Paused) {
        teardownPipeline();
        m_currentPositionMs = clampedPosition;
        m_startPositionMs = clampedPosition;
        m_pendingSeekResumeStartup = true;
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("seek paused-rearm target=%1 pendingSeekResume=1")
                              .arg(clampedPosition));
        emit positionChanged(clampedPosition);
        return;
    }

    startPipeline(clampedPosition, PipelineStartupProfile::SeekResume);
}

void WindowsWasapiAudioPlayer::setVolume(qreal volume)
{
    m_volume = volume;
    if (m_audioWorker) {
        QMetaObject::invokeMethod(m_audioWorker, [worker = m_audioWorker, volume] {
            worker->setVolume(volume);
        }, Qt::QueuedConnection);
    }
}

void WindowsWasapiAudioPlayer::releaseOutputResources(bool submitStopPcmFade)
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
                                  [worker = m_audioWorker, sessionId, submitStopPcmFade] {
                                      worker->releaseOutput(sessionId, true, submitStopPcmFade);
                                  },
                                  Qt::BlockingQueuedConnection);
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("releaseOutputResources end session=%1").arg(sessionId));
    }
}

void WindowsWasapiAudioPlayer::prepareOutputDeviceChangeTransition(int sessionId)
{
    if (!m_audioWorker || sessionId == 0) {
        return;
    }

    QMetaObject::invokeMethod(m_audioWorker,
                              [worker = m_audioWorker, sessionId] {
                                  worker->prepareForOutputDeviceChange(sessionId);
                              },
                              Qt::BlockingQueuedConnection);
}

void WindowsWasapiAudioPlayer::restoreOutputDeviceChangeTransition(int sessionId)
{
    if (!m_audioWorker || sessionId == 0) {
        return;
    }

    QMetaObject::invokeMethod(m_audioWorker,
                              [worker = m_audioWorker, sessionId] {
                                   worker->restoreAfterCancelledOutputDeviceChange(sessionId);
                               },
                               Qt::BlockingQueuedConnection);
}

void WindowsWasapiAudioPlayer::prepareActiveOutputInvalidationTransition(int sessionId)
{
    if (!m_audioWorker || sessionId == 0) {
        return;
    }

    QMetaObject::invokeMethod(m_audioWorker,
                              [worker = m_audioWorker, sessionId] {
                                  worker->prepareForActiveOutputInvalidation(sessionId);
                              },
                              Qt::BlockingQueuedConnection);
}

void WindowsWasapiAudioPlayer::restoreActiveOutputInvalidationTransition(int sessionId)
{
    if (!m_audioWorker || sessionId == 0) {
        return;
    }

    QMetaObject::invokeMethod(m_audioWorker,
                              [worker = m_audioWorker, sessionId] {
                                  worker->restoreAfterCancelledActiveOutputInvalidation(sessionId);
                              },
                              Qt::BlockingQueuedConnection);
}

void WindowsWasapiAudioPlayer::prepareConfirmedActiveSwitchFade(const QString &timing)
{
    if (m_activeOutputSwitch.transitionPrepared
        || !m_audioWorker
        || !m_audioStarted
        || m_audioState != QAudio::ActiveState
        || m_playbackState != PlaybackState::Playing
        || m_activeDecoderSessionId == 0) {
        return;
    }

    const int sessionId = m_activeDecoderSessionId;
    m_activeOutputSwitch.sessionId = sessionId;
    const qint64 elapsedMs = m_activeOutputSwitch.transactionTimer.isValid()
        ? m_activeOutputSwitch.transactionTimer.elapsed()
        : -1;
    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("activeSwitchPreFade timing=%1 session=%2 trigger=%3 phase=%4 reason=%5 positionMs=%6 elapsedMs=%7")
                          .arg(timing)
                          .arg(sessionId)
                          .arg(activeOutputSwitchTriggerName(m_activeOutputSwitch.trigger))
                          .arg(activeOutputSwitchPhaseName(m_activeOutputSwitch.phase))
                          .arg(m_activeOutputSwitch.reason)
                          .arg(m_currentPositionMs)
                          .arg(elapsedMs));
    prepareOutputDeviceChangeTransition(sessionId);
    m_activeOutputSwitch.transitionPrepared = true;
}

bool WindowsWasapiAudioPlayer::spatialEndpointFlushEnabled() const
{
    const QString value = QProcessEnvironment::systemEnvironment()
                              .value(QStringLiteral("AUDIOPLAYER_SPATIAL_ENDPOINT_FLUSH"))
                              .trimmed();
    return value == QStringLiteral("1") || value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
}

int WindowsWasapiAudioPlayer::spatialEndpointFlushMs() const
{
    bool ok = false;
    const int configured = QProcessEnvironment::systemEnvironment()
                               .value(QStringLiteral("AUDIOPLAYER_SPATIAL_ENDPOINT_FLUSH_MS"))
                               .trimmed()
                               .toInt(&ok);
    return ok ? qBound(1, configured, 1000) : kDefaultSpatialEndpointFlushMs;
}

int WindowsWasapiAudioPlayer::spatialEndpointSettleMs() const
{
    bool ok = false;
    const int configured = QProcessEnvironment::systemEnvironment()
                               .value(QStringLiteral("AUDIOPLAYER_SPATIAL_ENDPOINT_SETTLE_MS"))
                               .trimmed()
                               .toInt(&ok);
    return ok ? qBound(0, configured, 1000) : kDefaultSpatialEndpointSettleMs;
}

QString WindowsWasapiAudioPlayer::startupObservationProfile(PipelineStartupProfile startupProfile) const
{
    if (!m_hasStartedPlaybackThisRun && startupProfile == PipelineStartupProfile::NormalStart) {
        return QStringLiteral("ColdStart");
    }
    if (m_pendingSourceSwitchStartup && startupProfile == PipelineStartupProfile::NormalStart) {
        return QStringLiteral("SourceSwitch");
    }
    if (startupProfile == PipelineStartupProfile::ActiveSwitchRebuild) {
        return QStringLiteral("AutomationSwitch");
    }
    return pipelineStartupProfileName(startupProfile);
}

void WindowsWasapiAudioPlayer::performSpatialEndpointFlush(const QAudioDevice &device,
                                                           const PcmStreamFormat &deviceFormat,
                                                           const QByteArray &waveFormatData,
                                                           const QString &reason,
                                                           const QString &startupProfile)
{
    if (!spatialEndpointFlushEnabled() || !m_audioWorker) {
        return;
    }

    const int flushMs = spatialEndpointFlushMs();
    const int settleMs = spatialEndpointSettleMs();
    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("spatialEndpointFlush dispatch reason=%1 enabled=1 flushMs=%2 settleMs=%3 source=%4 previousSource=%5 startupProfile=%6 spatialAudioMode=unknown")
                          .arg(reason)
                          .arg(flushMs)
                          .arg(settleMs)
                          .arg(m_sourcePath)
                          .arg(m_previousSourcePath)
                          .arg(startupProfile));
    QMetaObject::invokeMethod(m_audioWorker,
                              [worker = m_audioWorker,
                               device,
                               deviceFormat,
                               waveFormatData,
                               flushMs,
                               settleMs,
                               reason,
                               sourcePath = m_sourcePath,
                               previousSourcePath = m_previousSourcePath,
                               startupProfile,
                               appStartTimeUtc = m_appStartTimeUtc] {
                                  worker->flushSpatialEndpoint(device,
                                                               deviceFormat,
                                                               waveFormatData,
                                                               flushMs,
                                                               settleMs,
                                                               reason,
                                                               sourcePath,
                                                               previousSourcePath,
                                                               startupProfile,
                                                               appStartTimeUtc);
                              });
}

void WindowsWasapiAudioPlayer::syncExclusiveModeStateFromWorker(bool requestedExclusiveMode,
                                                                const QString &context)
{
    if (!m_audioWorker) {
        return;
    }

    bool activeExclusiveMode = false;
    QMetaObject::invokeMethod(m_audioWorker,
                              [worker = m_audioWorker, &activeExclusiveMode] {
                                  activeExclusiveMode = worker->exclusiveModeActive();
                              },
                              Qt::BlockingQueuedConnection);
    if (requestedExclusiveMode && !activeExclusiveMode) {
        m_ignoreNextUnchangedOutputChangeAfterExclusiveFallback = true;
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("exclusiveMode fallback-to-shared context=%1 suppressNextUnchangedOutputChange=1")
                              .arg(context));
    }
    m_exclusiveModeEnabled = activeExclusiveMode;
}

void WindowsWasapiAudioPlayer::reconfigureActiveOutput(const QAudioDevice &device,
                                                       const QString &deviceDescription,
                                                       const QAudioFormat &format,
                                                       const QByteArray &waveFormatData,
                                                       const PcmStreamFormat &deviceFormat)
{
    auto *buffer = static_cast<PcmStreamBuffer *>(m_bufferDevice);
    if (!buffer || !m_audioWorker || m_activeDecoderSessionId == 0 || !format.isValid() || !deviceFormat.isValid()) {
        return;
    }

    const int sessionId = m_activeDecoderSessionId;
    const bool requireFreshBuffer = m_activeOutputSwitch.needsFreshBuffer;
    const qint64 reconfigurePositionMs = qMax<qint64>(0, m_currentPositionMs);
    const qsizetype bytesPerHalfSecond = qMax<qsizetype>(
        kMinBytesPerHalfSecond,
        static_cast<qsizetype>(m_decoderPcmFormat.bytesPerFrame()) * m_decoderPcmFormat.sampleRate / 2);
    const qsizetype stabilitySinkBufferSize = qMax<qsizetype>(
        startupThresholdBytes(),
        static_cast<qsizetype>(m_decoderPcmFormat.bytesPerFrame())
            * m_decoderPcmFormat.sampleRate
            * kStabilityModeOutputBufferMs / 1000);
    const qsizetype sinkBufferSize = m_stabilityModeEnabled
        ? stabilitySinkBufferSize
        : qMax<qsizetype>(startupThresholdBytes(), bytesPerHalfSecond / 2);

    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("activeOutputSwitch hot-reconfigure session=%1 positionMs=%2 previousDevice=%3 targetDevice=%4 bufferBytes=%5 decoderFinished=%6 targetRate=%7 targetChannels=%8 targetBits=%9 freshBufferRequired=%10")
                          .arg(sessionId)
                          .arg(reconfigurePositionMs)
                          .arg(QString::fromLatin1(m_activeOutputDeviceId.toHex()))
                          .arg(QString::fromLatin1(device.id().toHex()))
                          .arg(buffer->bufferedBytes())
                          .arg(m_decoderFinished)
                          .arg(deviceFormat.sampleRate)
                          .arg(deviceFormat.channelCount)
                          .arg(deviceFormat.bitsPerSample())
                          .arg(requireFreshBuffer));
    PlayerLogger::diagnostic(QStringLiteral("player"),
                             QStringLiteral("output_switch_hot_reconfigure"),
                             {
                                 {QStringLiteral("transactionKind"), QStringLiteral("active-switch")},
                                 {QStringLiteral("sessionId"), sessionId},
                                 {QStringLiteral("positionMs"), reconfigurePositionMs},
                                 {QStringLiteral("trigger"), activeOutputSwitchTriggerName(m_activeOutputSwitch.trigger)},
                                 {QStringLiteral("phase"), activeOutputSwitchPhaseName(m_activeOutputSwitch.phase)},
                                 {QStringLiteral("reason"), m_activeOutputSwitch.reason},
                                 {QStringLiteral("previousDeviceId"), QString::fromLatin1(m_activeOutputDeviceId.toHex())},
                                 {QStringLiteral("targetDeviceId"), QString::fromLatin1(device.id().toHex())},
                                 {QStringLiteral("targetDeviceName"), deviceDescription},
                                 {QStringLiteral("targetSampleRate"), deviceFormat.sampleRate},
                                 {QStringLiteral("targetChannels"), deviceFormat.channelCount},
                                 {QStringLiteral("targetBits"), deviceFormat.bitsPerSample()},
                                 {QStringLiteral("freshBufferRequired"), requireFreshBuffer},
                             });

    resetOutputRecoveryState(QStringLiteral("activeOutputSwitch-hot"));
    resetAnomalyTracking(QStringLiteral("activeOutputSwitch-hot"));
    m_audioStarted = false;
    m_audioState = QAudio::StoppedState;
    m_audioError = QtAudio::NoError;
    m_startPositionMs = reconfigurePositionMs;
    m_currentPositionMs = reconfigurePositionMs;
    m_lastLoggedPositionBucket = reconfigurePositionMs / 1000;
    m_outputFormat = format;
    m_outputPcmFormat = deviceFormat;
    m_outputWaveFormatData = waveFormatData;
    m_outputDeviceDescription = deviceDescription;
    m_activeOutputDeviceId = device.id();
    const bool requestedExclusiveMode = m_exclusiveModeEnabled && !m_stabilityModeEnabled;

    QMetaObject::invokeMethod(m_audioWorker,
                              [worker = m_audioWorker,
                               sessionId,
                               device,
                               deviceFormat,
                               bufferFormat = m_decoderPcmFormat,
                               waveFormatData,
                               sinkBufferSize,
                               volume = m_volume,
                               exclusiveMode = requestedExclusiveMode,
                               stabilityMode = m_stabilityModeEnabled] {
                                  worker->configureOutput(sessionId,
                                                          device,
                                                          deviceFormat,
                                                          bufferFormat,
                                                          waveFormatData,
                                                          sinkBufferSize,
                                                          volume,
                                                          true,
                                                          true,
                                                          false,
                                                          0,
                                                          kRecoveryWarmupSilenceMs,
                                                          0,
                                                          kDeferredFadeInDelayMs,
                                                          {},
                                                          {},
                                                          exclusiveMode,
                                                          stabilityMode,
                                                          false);
                              },
                              Qt::BlockingQueuedConnection);
    syncExclusiveModeStateFromWorker(requestedExclusiveMode, QStringLiteral("activeOutputSwitch-hot"));
    emit outputFormatChanged(m_outputDeviceDescription, m_outputFormat);
    if (requireFreshBuffer) {
        resumeBufferDevice(QStringLiteral("activeOutputSwitch-hot"));
        m_activeOutputSwitch.needsFreshBuffer = false;
        startAudioOutputIfReady();
        return;
    }

    QMetaObject::invokeMethod(m_audioWorker,
                              [worker = m_audioWorker, sessionId, buffer] {
                                  worker->startOutput(sessionId, buffer);
                              },
                              Qt::BlockingQueuedConnection);
    m_audioStarted = true;
}

void WindowsWasapiAudioPlayer::clearBufferDevice()
{
    auto *buffer = static_cast<PcmStreamBuffer *>(m_bufferDevice);
    if (buffer) {
        buffer->setDiscardWrites(true);
        buffer->setEndOfStream(true);
        const qsizetype clearedBytes = buffer->discardPendingData();
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("clearBufferDevice ownerSession=%1 bufferGeneration=%2 clearedBytes=%3 discardedWriteBytes=%4 discardingWrites=%5")
                              .arg(buffer->ownerSessionId())
                              .arg(buffer->bufferGeneration())
                              .arg(clearedBytes)
                              .arg(buffer->discardedWriteBytes())
                              .arg(buffer->isDiscardingWrites()));
    }
    if (buffer && (buffer->discardedWriteBytes() > 0 || buffer->isDiscardingWrites())) {
        logPlaybackAnomaly(QStringLiteral("buffer-clear"),
                           QStringLiteral("bufferedBytes=%1 discardedWriteBytes=%2 discardingWrites=%3 ownerSession=%4 bufferGeneration=%5")
                               .arg(buffer->bufferedBytes())
                               .arg(buffer->discardedWriteBytes())
                               .arg(buffer->isDiscardingWrites())
                               .arg(buffer->ownerSessionId())
                               .arg(buffer->bufferGeneration()));
    }
    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("clearBufferDevice hasBuffer=%1").arg(m_bufferDevice != nullptr));
    delete m_bufferDevice;
    m_bufferDevice = nullptr;
}

void WindowsWasapiAudioPlayer::stopDecoderWorker(bool waitForFinished)
{
    if ((!m_decoderWorker && !m_libavSeekDecoderWorker)
        || (!m_activeDecoderSessionId && !m_stoppingDecoderSessionId)) {
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
    if (m_decoderWorker) {
        QMetaObject::invokeMethod(m_decoderWorker,
                                  [worker = m_decoderWorker, waitForFinished] {
                                      worker->stopDecoding(waitForFinished);
                                  },
                                  waitForFinished ? Qt::BlockingQueuedConnection : Qt::QueuedConnection);
    }
    if (m_libavSeekDecoderWorker) {
        QMetaObject::invokeMethod(m_libavSeekDecoderWorker,
                                  [worker = m_libavSeekDecoderWorker, waitForFinished] {
                                      worker->stopDecoding(waitForFinished);
                                  },
                                  waitForFinished ? Qt::BlockingQueuedConnection : Qt::QueuedConnection);
    }
    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("stopDecoderWorker end waitForFinished=%1").arg(waitForFinished));
}

void WindowsWasapiAudioPlayer::startAudioOutputIfReady()
{
    auto *buffer = static_cast<PcmStreamBuffer *>(m_bufferDevice);
    if (!m_audioWorker || !buffer || m_audioStarted) {
        return;
    }

    const qsizetype bufferedBytes = buffer->bufferedBytes();
    const qsizetype thresholdBytes = startupThresholdBytes();
    const bool activeSwitchRebuildPipeline =
        m_currentPipelineStartupProfile == PipelineStartupProfile::ActiveSwitchRebuild;
    if (!m_decoderFinished && bufferedBytes < thresholdBytes) {
        if (activeSwitchRebuildPipeline) {
            PlayerLogger::log(QStringLiteral("player"),
                              QStringLiteral("activeSwitchRebuildTiming marker=buffer-wait session=%1 bufferedBytes=%2 thresholdBytes=%3 decoderFinished=%4")
                                  .arg(m_activeDecoderSessionId)
                                  .arg(bufferedBytes)
                                  .arg(thresholdBytes)
                                  .arg(m_decoderFinished));
        }
        return;
    }

    const int sessionId = m_activeDecoderSessionId;
    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("startAudioOutputIfReady session=%1 bufferedBytes=%2 decoderFinished=%3 bufferOwnerSession=%4 bufferGeneration=%5")
                          .arg(sessionId)
                          .arg(bufferedBytes)
                          .arg(m_decoderFinished)
                          .arg(buffer->ownerSessionId())
                          .arg(buffer->bufferGeneration()));
    if (activeSwitchRebuildPipeline) {
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("activeSwitchRebuildTiming marker=start-output-ready session=%1 bufferedBytes=%2 thresholdBytes=%3 decoderFinished=%4")
                              .arg(sessionId)
                              .arg(bufferedBytes)
                              .arg(thresholdBytes)
                              .arg(m_decoderFinished));
    }
    QMetaObject::invokeMethod(m_audioWorker,
                              [worker = m_audioWorker, sessionId, buffer] {
                                  worker->startOutput(sessionId, buffer);
                              },
                              Qt::BlockingQueuedConnection);
    m_audioStarted = true;
}

void WindowsWasapiAudioPlayer::startPipeline(qint64 startPositionMs, PipelineStartupProfile startupProfile)
{
    const qint64 pipelineStartTimeMs = QDateTime::currentMSecsSinceEpoch();
    const bool activeSwitchRebuildStartup =
        startupProfile == PipelineStartupProfile::ActiveSwitchRebuild;
    const bool seekResumeStartup =
        startupProfile == PipelineStartupProfile::SeekResume;
    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("startPipeline startPositionMs=%1 pipelineStartProfile=%2 source=%3")
                          .arg(startPositionMs)
                          .arg(pipelineStartupProfileName(startupProfile))
                          .arg(m_sourcePath));
    resetAnomalyTracking(QStringLiteral("startPipeline"));
    const QAudioDevice device = resolveOutputDevice();
    QString deviceDescription;
    QByteArray selectedWaveFormatData;
    PcmStreamFormat selectedPcmFormat;
    const QAudioFormat selectedFormat =
        selectOutputFormat(&deviceDescription, &selectedWaveFormatData, &selectedPcmFormat);
    const bool spatialStaticBedRequested = spatialStaticBedEnabledForSource();
    PcmStreamFormat selectedDecoderFormat = decoderFormatForOutput(selectedPcmFormat);
    if (spatialStaticBedRequested) {
        selectedDecoderFormat.sampleRate = 48000;
        selectedDecoderFormat.channelCount = 8;
        selectedDecoderFormat.sampleEncoding = PcmSampleEncoding::Float32;
        selectedDecoderFormat.validBitsPerSample = 32;
        selectedDecoderFormat.channelLayout = QStringLiteral("5.1.2");
    }
    selectedDecoderFormat.channelLayout =
        selectedDecoderFormat.channelLayout.isEmpty()
            ? decoderChannelLayoutForSource(selectedWaveFormatData, selectedDecoderFormat.channelCount)
            : selectedDecoderFormat.channelLayout;
    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("wasapi selectedDecoderFormat sampleRate=%1 channels=%2 bits=%3 layout=%4 sourceCodec=%5 sourceChannels=%6 spatialStaticBedRequested=%7")
                          .arg(selectedDecoderFormat.sampleRate)
                          .arg(selectedDecoderFormat.channelCount)
                          .arg(selectedDecoderFormat.bitsPerSample())
                          .arg(selectedDecoderFormat.channelLayout.isEmpty()
                                   ? QStringLiteral("none")
                                   : selectedDecoderFormat.channelLayout)
                          .arg(m_sourceCodecName)
                          .arg(m_sourceChannelCount)
                          .arg(spatialStaticBedRequested ? 1 : 0));
    const QString realtimeDecodeSetting =
        QProcessEnvironment::systemEnvironment()
            .value(QStringLiteral("AUDIOPLAYER_FFMPEG_REALTIME_DECODE"))
            .trimmed();
    const bool realtimeDecode =
        realtimeDecodeSetting.isEmpty()
        || !envFlagDisabled(realtimeDecodeSetting);
    bool useLibavDecoder = false;
#ifdef AUDIOPLAYER_LIBAV_DECODER
    useLibavDecoder = isPackagedLibavAudioCodec(m_sourceCodecName)
        && !wasapiLibavDecoderDisabled()
        && m_libavSeekDecoderWorker != nullptr;
#endif
    const QString decoderMode = useLibavDecoder
        ? QStringLiteral("libav-inprocess")
        : QStringLiteral("ffmpeg-cli");
    const QString ffmpegExecutable = useLibavDecoder ? QString() : locateFfmpegExecutable();
    if (!useLibavDecoder && ffmpegExecutable.isEmpty()) {
        emit errorOccurred(PlaybackError::DecoderNotFound,
                           tr("未找到 FFmpeg 解码器\n\n"
                               "程序可能安装不完整\n\n"
                               "建议操作：重新安装程序或检查程序完整性"));
        return;
    }
    const bool restartWhilePlaying =
        m_playbackState == PlaybackState::Playing && m_activeDecoderSessionId != 0;
    bool startMutedForFadeIn = false;
    bool injectStartupSilence = true;
    bool discardWarmupFrames = true;
    UINT32 startupSilenceMsOverride = 0;
    UINT32 warmupDiscardMsOverride = kRecoveryWarmupSilenceMs;
    int pcmFadeInDurationMsOverride = 0;
    int streamFadeInDelayMs = kDeferredFadeInDelayMs;
    ActiveSwitchBoundaryPolicy activeSwitchBoundaryPolicy;
    WasapiArtifactTrackingConfig artifactTracking;
    artifactTracking.pipelineStartupProfile = pipelineStartupProfileName(startupProfile);
    artifactTracking.startupObservationProfile = startupObservationProfile(startupProfile);
    artifactTracking.source = m_sourcePath;
    artifactTracking.previousSource = m_previousSourcePath;
    artifactTracking.playbackState = AudioUtils::playbackStateName(PlaybackState::Playing);
    artifactTracking.startPositionMs = startPositionMs;
    artifactTracking.selectedOutputDeviceId = QString::fromLatin1(device.id().toHex());
    artifactTracking.appStartTimeUtc = m_appStartTimeUtc;
    artifactTracking.pipelineStartTimeMs = pipelineStartTimeMs;
    artifactTracking.realtimeDecodeEnabled = !useLibavDecoder && realtimeDecode;
    if (seekResumeStartup) {
        artifactTracking.seekRequestTimeMs =
            m_lastSeekRequestTimeMs >= 0 && m_lastSeekRequestPositionMs == startPositionMs
            ? m_lastSeekRequestTimeMs
            : pipelineStartTimeMs;
    }
    switch (startupProfile) {
    case PipelineStartupProfile::NormalStart:
        startMutedForFadeIn = true;
        injectStartupSilence = false;
        discardWarmupFrames = false;
        artifactTracking.enabled = true;
        artifactTracking.artifactPath = m_pendingSourceSwitchStartup
            ? QStringLiteral("SourceSwitch")
            : QStringLiteral("NormalStart");
        artifactTracking.recentControlEvent = m_pendingSourceSwitchStartup
            ? QStringLiteral("SourceSwitch:play")
            : QStringLiteral("NormalStart:play");
        break;
    case PipelineStartupProfile::SeekRestart:
        startMutedForFadeIn = restartWhilePlaying;
        artifactTracking.enabled = true;
        artifactTracking.artifactPath = QStringLiteral("SeekRestart");
        artifactTracking.recentControlEvent = QStringLiteral("SeekRestart:seek");
        break;
    case PipelineStartupProfile::SeekResume:
        startMutedForFadeIn = true;
        injectStartupSilence = true;
        discardWarmupFrames = true;
        startupSilenceMsOverride = kSeekResumeStartupSilenceMs;
        warmupDiscardMsOverride = kSeekResumeWarmupDiscardMs;
        pcmFadeInDurationMsOverride = kSeekResumePcmFadeInDurationMs;
        streamFadeInDelayMs = kSeekResumeStreamFadeInDelayMs;
        artifactTracking.seekResumeStartupSilenceMs = static_cast<int>(kSeekResumeStartupSilenceMs);
        artifactTracking.seekResumeWarmupDiscardMs = static_cast<int>(kSeekResumeWarmupDiscardMs);
        artifactTracking.seekResumeFadeInMs = kSeekResumePcmFadeInDurationMs;
        artifactTracking.enabled = true;
        artifactTracking.artifactPath = QStringLiteral("SeekResume");
        artifactTracking.recentControlEvent = QStringLiteral("SeekResume:seek");
        break;
    case PipelineStartupProfile::ActiveSwitchRebuild: {
        startMutedForFadeIn = restartWhilePlaying;
        injectStartupSilence = true;
        discardWarmupFrames = false;
        const bool postInvalidationRebuild = m_activeOutputSwitch.needsFreshBuffer;
        activeSwitchBoundaryPolicy =
            activeSwitchBoundaryPolicyForOutputFormats(m_outputPcmFormat,
                                                       selectedPcmFormat,
                                                       postInvalidationRebuild);
        startupSilenceMsOverride = activeSwitchBoundaryPolicy.startupSilenceMs;
        artifactTracking.enabled = true;
        artifactTracking.artifactPath = QStringLiteral("ActiveSwitchRebuild");
        artifactTracking.activeSwitchTrigger = activeOutputSwitchTriggerName(m_activeOutputSwitch.trigger);
        artifactTracking.activeSwitchPhase = activeOutputSwitchPhaseName(m_activeOutputSwitch.phase);
        artifactTracking.activeSwitchReason = m_activeOutputSwitch.reason;
        artifactTracking.activeSwitchBoundaryPolicy = activeSwitchBoundaryPolicy.name;
        artifactTracking.recentControlEvent =
            QStringLiteral("ActiveSwitchRebuild:%1:%2")
                .arg(artifactTracking.activeSwitchTrigger,
                     artifactTracking.activeSwitchReason.isEmpty()
                         ? QStringLiteral("unspecified")
                         : artifactTracking.activeSwitchReason);
        break;
    }
    case PipelineStartupProfile::ErrorRecovery:
        startMutedForFadeIn = true;
        artifactTracking.enabled = true;
        artifactTracking.artifactPath = QStringLiteral("ErrorRecovery");
        artifactTracking.recoveryAttempt = m_outputRecoveryAttempt;
        artifactTracking.recentControlEvent =
            QStringLiteral("ErrorRecovery:attempt-%1").arg(m_outputRecoveryAttempt);
        break;
    }
    if (!selectedFormat.isValid() || !selectedPcmFormat.isValid() || !selectedDecoderFormat.isValid()) {
        emit errorOccurred(PlaybackError::FormatNotSupported,
                           tr("未找到可用的 WASAPI shared-mode 输出格式"));
        return;
    }

    const qsizetype seekResumeStartupThresholdBytes =
        qMax<qsizetype>(kSeekResumeMinThresholdBytes,
                        static_cast<qsizetype>(selectedDecoderFormat.bytesPerFrame())
                            * selectedDecoderFormat.sampleRate
                            * (m_stabilityModeEnabled ? kStabilityModeSeekResumeStartupThresholdMs
                                                      : kSeekResumeStartupThresholdMs) / 1000);
    const UINT32 startupSilenceLogMs = startMutedForFadeIn && injectStartupSilence
        ? (startupSilenceMsOverride > 0
               ? startupSilenceMsOverride
               : (discardWarmupFrames ? kRecoveryStartupSilenceMs
                                      : kHotReconfigureStartupSilenceMs))
        : 0;
    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("startPipeline output deviceId=%1 description=%2 sampleRate=%3 channels=%4 sampleFormat=%5 outputBits=%6 outputValidBits=%7 decoderBits=%8 decoderValidBits=%9 usesDefault=%10 startMutedForFadeIn=%11 startupSilence=%12 warmupDiscard=%13 startupSilenceMs=%14 pipelineStartProfile=%15")
                          .arg(QString::fromLatin1(device.id().toHex()))
                          .arg(deviceDescription)
                          .arg(selectedFormat.sampleRate())
                          .arg(selectedFormat.channelCount())
                          .arg(static_cast<int>(selectedFormat.sampleFormat()))
                          .arg(selectedPcmFormat.bitsPerSample())
                          .arg(selectedPcmFormat.effectiveValidBitsPerSample())
                          .arg(selectedDecoderFormat.bitsPerSample())
                          .arg(selectedDecoderFormat.effectiveValidBitsPerSample())
                          .arg(m_selectedOutputDeviceId.isEmpty())
                          .arg(startMutedForFadeIn)
                          .arg(injectStartupSilence)
                          .arg(discardWarmupFrames)
                          .arg(startupSilenceLogMs)
                          .arg(pipelineStartupProfileName(startupProfile)));
    if (seekResumeStartup) {
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("seekResumeTiming marker=pipeline-start seekRequestTimeMs=%1 pipelineStartTimeMs=%2 seekResumeStartupSilenceMs=%3 seekResumeWarmupDiscardMs=%4 seekResumeFadeInMs=%5 realtimeDecodeEnabled=%6 streamFadeInDelayMs=%7 startupThresholdBytes=%8")
                              .arg(artifactTracking.seekRequestTimeMs)
                              .arg(artifactTracking.pipelineStartTimeMs)
                              .arg(artifactTracking.seekResumeStartupSilenceMs)
                              .arg(artifactTracking.seekResumeWarmupDiscardMs)
                              .arg(artifactTracking.seekResumeFadeInMs)
                              .arg(artifactTracking.realtimeDecodeEnabled ? 1 : 0)
                              .arg(streamFadeInDelayMs)
                              .arg(seekResumeStartupThresholdBytes));
        PlayerLogger::diagnostic(QStringLiteral("player"),
                                 QStringLiteral("seek_resume_pipeline_start"),
                                 {
                                     {QStringLiteral("seekRequestTimeMs"), artifactTracking.seekRequestTimeMs},
                                     {QStringLiteral("pipelineStartTimeMs"), artifactTracking.pipelineStartTimeMs},
                                     {QStringLiteral("seekResumeStartupSilenceMs"), artifactTracking.seekResumeStartupSilenceMs},
                                     {QStringLiteral("seekResumeWarmupDiscardMs"), artifactTracking.seekResumeWarmupDiscardMs},
                                     {QStringLiteral("seekResumeFadeInMs"), artifactTracking.seekResumeFadeInMs},
                                     {QStringLiteral("realtimeDecodeEnabled"), artifactTracking.realtimeDecodeEnabled},
                                     {QStringLiteral("streamFadeInDelayMs"), streamFadeInDelayMs},
                                     {QStringLiteral("startupThresholdBytes"), static_cast<qint64>(seekResumeStartupThresholdBytes)},
                                 });
    }

    if (activeSwitchRebuildStartup) {
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("activeSwitchBoundaryPolicy name=%1 currentRate=%2 targetRate=%3 startupSilenceMs=%4 pcmFadeInMs=%5 firstBlockMaxFadeGain=%6 entryBridgeStreamGain=%7 scope=all-active-switches")
                              .arg(activeSwitchBoundaryPolicy.name)
                              .arg(activeSwitchBoundaryPolicy.currentSampleRate)
                              .arg(activeSwitchBoundaryPolicy.targetSampleRate)
                              .arg(activeSwitchBoundaryPolicy.startupSilenceMs)
                              .arg(activeSwitchBoundaryPolicy.pcmFadeInDurationMs)
                              .arg(QString::number(activeSwitchBoundaryPolicy.firstBlockMaxFadeGain, 'f', 2))
                              .arg(QString::number(activeSwitchBoundaryPolicy.entryBridgeStreamGain, 'f', 3)));
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("activeSwitchRebuildTiming marker=startPipeline-before-teardown startPositionMs=%1 activeSession=%2 audioStarted=%3")
                              .arg(startPositionMs)
                              .arg(m_activeDecoderSessionId)
                              .arg(m_audioStarted));
    }
    teardownPipeline();
    if (activeSwitchRebuildStartup) {
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("activeSwitchRebuildTiming marker=startPipeline-after-teardown startPositionMs=%1 activeSession=%2")
                              .arg(startPositionMs)
                              .arg(m_activeDecoderSessionId));
    }
    if (spatialEndpointFlushEnabled()
        && (startupProfile == PipelineStartupProfile::NormalStart
            || startupProfile == PipelineStartupProfile::ActiveSwitchRebuild)) {
        const QString flushReason = artifactTracking.startupObservationProfile == QStringLiteral("ColdStart")
            ? QStringLiteral("coldStartBeforePlayback")
            : (artifactTracking.startupObservationProfile == QStringLiteral("SourceSwitch")
                   ? QStringLiteral("sourceSwitchAfterTeardown")
                   : QStringLiteral("outputSwitchBeforePlayback"));
        performSpatialEndpointFlush(device,
                                    selectedPcmFormat,
                                    selectedWaveFormatData,
                                    flushReason,
                                    artifactTracking.startupObservationProfile);
    }

    m_outputFormat = selectedFormat;
    m_outputPcmFormat = selectedPcmFormat;
    m_decoderPcmFormat = selectedDecoderFormat;
    m_outputWaveFormatData = selectedWaveFormatData;
    m_outputDeviceDescription = deviceDescription;
    m_activeOutputDeviceId = device.id();

    if (useLibavDecoder && !m_pcmSeekCache) {
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

    const int newSessionId = m_decoderSessionId + 1;
    auto *buffer = new PcmStreamBuffer(this);
    m_bufferDevice = buffer;
    buffer->setOwner(newSessionId, ++m_bufferGeneration, m_sourcePath);
    artifactTracking.bufferGeneration = m_bufferGeneration;
    buffer->clear();
    buffer->setEndOfStream(false);
    const qsizetype bytesPerHalfSecond = qMax<qsizetype>(
        kMinBytesPerHalfSecond,
        static_cast<qsizetype>(m_decoderPcmFormat.bytesPerFrame())
            * m_decoderPcmFormat.sampleRate / 2);
    buffer->setMaxSize(bytesPerHalfSecond * (m_stabilityModeEnabled ? kBufferHalfSecondsStability : kBufferHalfSecondsNormal));
    m_audioStarted = false;
    m_audioState = QAudio::StoppedState;
    m_audioError = QtAudio::NoError;
    m_currentPipelineStartupProfile = startupProfile;
    const bool requestedExclusiveMode = m_exclusiveModeEnabled && !m_stabilityModeEnabled;

    const qsizetype stabilitySinkBufferSize = qMax<qsizetype>(
        startupThresholdBytes(),
        static_cast<qsizetype>(m_decoderPcmFormat.bytesPerFrame())
            * m_decoderPcmFormat.sampleRate
            * kStabilityModeOutputBufferMs / 1000);
    const qsizetype sinkBufferSize = m_stabilityModeEnabled
        ? stabilitySinkBufferSize
        : qMax<qsizetype>(startupThresholdBytes(), bytesPerHalfSecond / 2);
    configureAudioWorker(newSessionId, device, sinkBufferSize,
                         startMutedForFadeIn, injectStartupSilence,
                         discardWarmupFrames, startupSilenceMsOverride,
                         warmupDiscardMsOverride, pcmFadeInDurationMsOverride,
                         streamFadeInDelayMs, activeSwitchBoundaryPolicy,
                         artifactTracking, requestedExclusiveMode,
                         activeSwitchRebuildStartup, startupProfile,
                         spatialStaticBedRequested);


    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("startPipeline decoderMode=%1 realtime=%2 initialBurstSeconds=%3 realtimeSetting=%4 libavSetting=%5 pipelineStartProfile=%6")
                          .arg(decoderMode)
                          .arg(!useLibavDecoder && realtimeDecode)
                          .arg(!useLibavDecoder && realtimeDecode ? kSeekResumeFfmpegInitialBurstSeconds : 0.0,
                               0,
                               'f',
                               3)
                          .arg(realtimeDecodeSetting.isEmpty() ? QStringLiteral("default") : realtimeDecodeSetting)
                          .arg(wasapiLibavDecoderDisabled() ? QStringLiteral("disabled") : QStringLiteral("enabled"))
                          .arg(pipelineStartupProfileName(startupProfile)));

    const QStringList arguments = prepareDecoderArgs(startPositionMs, newSessionId,
                                                     device, useLibavDecoder, realtimeDecode);

    startDecoderWorker(startPositionMs, newSessionId, useLibavDecoder,
                       arguments, ffmpegExecutable, buffer);

    if (startupProfile == PipelineStartupProfile::NormalStart) {
        m_pendingSourceSwitchStartup = false;
    }
    m_hasStartedPlaybackThisRun = true;
}

QStringList WindowsWasapiAudioPlayer::prepareDecoderArgs(qint64 startPositionMs,
                                                          int sessionId,
                                                          const QAudioDevice &device,
                                                          bool useLibavDecoder,
                                                          bool realtimeDecode) const
{
    QStringList arguments;
    if (!useLibavDecoder) {
        arguments = {
            QStringLiteral("-nostdin"),
            QStringLiteral("-v"),
            QStringLiteral("error"),
        };
        if (realtimeDecode) {
            arguments << QStringLiteral("-re");
            arguments << QStringLiteral("-readrate_initial_burst")
                      << QString::number(kSeekResumeFfmpegInitialBurstSeconds, 'f', 3);
        }

        const QString outputChannelLayout =
            channelLayoutForWaveFormatData(m_outputWaveFormatData, m_outputPcmFormat.channelCount);
        const QString downmixLayout = outputChannelLayout;
        if (m_sourceChannelCount > 0 && m_sourceChannelCount > m_outputPcmFormat.channelCount
            && !downmixLayout.isEmpty()
            && (m_sourceCodecName == QStringLiteral("truehd")
                || m_sourceCodecName == QStringLiteral("eac3")
                || m_sourceCodecName == QStringLiteral("ac3")
                || !rawInputFormatForSource().isEmpty())) {
            arguments << QStringLiteral("-downmix") << downmixLayout;
        }

        const QString rawInputFormat = rawInputFormatForSource();
        if (!rawInputFormat.isEmpty()) {
            arguments << QStringLiteral("-f") << rawInputFormat;
        }
        if (startPositionMs > 0) {
            arguments << QStringLiteral("-ss") << formatSeekSeconds(startPositionMs);
        }

        const QString targetChannelLayout = !m_decoderPcmFormat.channelLayout.isEmpty()
            ? m_decoderPcmFormat.channelLayout
            : channelLayoutForWaveFormatData(m_outputWaveFormatData, m_decoderPcmFormat.channelCount);
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("wasapi decoderChannelLayout session=%1 layout=%2 outputChannels=%3 decoderChannels=%4 channelMask=%5")
                              .arg(sessionId)
                              .arg(targetChannelLayout)
                              .arg(m_outputPcmFormat.channelCount)
                              .arg(m_decoderPcmFormat.channelCount)
                              .arg(channelMaskText(channelMaskFromWaveFormatData(m_outputWaveFormatData))));
        QStringList filters {
            QStringLiteral("aresample=%1").arg(m_decoderPcmFormat.sampleRate),
            QStringLiteral("aformat=sample_rates=%1:sample_fmts=%2")
                .arg(m_decoderPcmFormat.sampleRate)
                .arg(pcmSampleFormatName(m_decoderPcmFormat)),
        };
        if (!targetChannelLayout.isEmpty()) {
            filters.last().append(QStringLiteral(":channel_layouts=%1").arg(targetChannelLayout));
        }
        const QString creativeChannelOrderFilter =
            creativeWasapiChannelOrderFilter(device, targetChannelLayout, m_decoderPcmFormat.channelCount,
                                              static_cast<int>(m_creativeChannelReorderMode));
        if (!creativeChannelOrderFilter.isEmpty()) {
            filters << creativeChannelOrderFilter;
            PlayerLogger::log(QStringLiteral("player"),
                              QStringLiteral("wasapi creativeChannelOrderFilter session=%1 device=%2 filter=%3")
                                  .arg(sessionId)
                                  .arg(device.description())
                                  .arg(creativeChannelOrderFilter));
        }

        arguments << QStringLiteral("-i") << m_sourcePath
                  << QStringLiteral("-map") << QStringLiteral("0:a:0")
                  << QStringLiteral("-vn")
                  << QStringLiteral("-sn")
                  << QStringLiteral("-dn")
                  << QStringLiteral("-af") << filters.join(',')
                  << QStringLiteral("-ac") << QString::number(m_decoderPcmFormat.channelCount)
                  << QStringLiteral("-ar") << QString::number(m_decoderPcmFormat.sampleRate)
                  << QStringLiteral("-acodec") << pcmCodecName(m_decoderPcmFormat)
                  << QStringLiteral("-f") << pcmMuxerName(m_decoderPcmFormat)
                  << QStringLiteral("-");
    }
    return arguments;
}

void WindowsWasapiAudioPlayer::configureAudioWorker(
    int sessionId,
    const QAudioDevice &device,
    qsizetype sinkBufferSize,
    bool startMutedForFadeIn,
    bool injectStartupSilence,
    bool discardWarmupFrames,
    quint32 startupSilenceMsOverride,
    quint32 warmupDiscardMsOverride,
    int pcmFadeInDurationMsOverride,
    int streamFadeInDelayMs,
    const ActiveSwitchBoundaryPolicy &activeSwitchBoundaryPolicy,
    const WasapiArtifactTrackingConfig &artifactTracking,
    bool requestedExclusiveMode,
    bool activeSwitchRebuildStartup,
    PipelineStartupProfile startupProfile,
    bool spatialStaticBedRequested)
{
    if (!m_audioWorker) {
        return;
    }

    if (activeSwitchRebuildStartup) {
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("activeSwitchRebuildTiming marker=configureOutput-invoke-begin session=%1 sinkBufferSize=%2 startupThresholdBytes=%3")
                              .arg(sessionId)
                              .arg(sinkBufferSize)
                              .arg(startupThresholdBytes()));
    }
    QMetaObject::invokeMethod(m_audioWorker,
                              [worker = m_audioWorker,
                               pendingSessionId = sessionId,
                               device,
                               deviceFormat = m_outputPcmFormat,
                               bufferFormat = m_decoderPcmFormat,
                               waveFormatData = m_outputWaveFormatData,
                               sinkBufferSize,
                               volume = m_volume,
                               startMutedForFadeIn,
                               injectStartupSilence,
                               discardWarmupFrames,
                               startupSilenceMsOverride,
                               warmupDiscardMsOverride,
                               pcmFadeInDurationMsOverride,
                               streamFadeInDelayMs,
                               activeSwitchBoundaryPolicy,
                               artifactTracking,
                               exclusiveMode = requestedExclusiveMode,
                               stabilityMode = m_stabilityModeEnabled,
                               spatialStaticBedRequested] {
                                  worker->configureOutput(pendingSessionId,
                                                          device,
                                                          deviceFormat,
                                                          bufferFormat,
                                                          waveFormatData,
                                                          sinkBufferSize,
                                                          volume,
                                                          startMutedForFadeIn,
                                                          injectStartupSilence,
                                                          discardWarmupFrames,
                                                          startupSilenceMsOverride,
                                                          warmupDiscardMsOverride,
                                                          pcmFadeInDurationMsOverride,
                                                          streamFadeInDelayMs,
                                                          activeSwitchBoundaryPolicy,
                                                          artifactTracking,
                                                          exclusiveMode,
                                                          stabilityMode,
                                                          spatialStaticBedRequested);
                              },
                              Qt::BlockingQueuedConnection);
    syncExclusiveModeStateFromWorker(requestedExclusiveMode, pipelineStartupProfileName(startupProfile));
    emit outputFormatChanged(m_outputDeviceDescription, m_outputFormat);
    if (activeSwitchRebuildStartup) {
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("activeSwitchRebuildTiming marker=configureOutput-invoke-done session=%1")
                              .arg(sessionId));
    }
}

void WindowsWasapiAudioPlayer::startDecoderWorker(qint64 startPositionMs,
                                                    int sessionId,
                                                    bool useLibavDecoder,
                                                    const QStringList &arguments,
                                                    const QString &ffmpegExecutable,
                                                    PcmStreamBuffer *buffer)
{
    m_stderrBuffer.clear();
    m_decoderFinished = false;
    m_decoderSessionId = sessionId;
    m_activeDecoderSessionId = sessionId;
    m_stoppingDecoderSessionId = 0;
    m_seekResumeFirstDecodedPcmLogged =
        m_currentPipelineStartupProfile != PipelineStartupProfile::SeekResume;
    m_currentPositionMs = startPositionMs;
    m_startPositionMs = startPositionMs;
    m_lastLoggedPositionBucket = startPositionMs / 1000;
    emit positionChanged(startPositionMs);
    setPlaybackState(PlaybackState::Playing);

    const bool activeSwitchRebuildStartup =
        m_currentPipelineStartupProfile == PipelineStartupProfile::ActiveSwitchRebuild;
    const bool shouldApplyCreativeReorder =
        (m_creativeChannelReorderMode == CreativeChannelReorderMode::ForceCreative);

    if (useLibavDecoder && m_libavSeekDecoderWorker) {
        m_libavSeekDecoderWorker->setCreativeChannelReorderEnabled(shouldApplyCreativeReorder);
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("startPipeline decoder-start mode=libav-inprocess session=%1 startPositionMs=%2 startupThresholdBytes=%3 bufferMaxSize=%4 sourceCodec=%5 creativeReorder=%6")
                              .arg(sessionId)
                              .arg(startPositionMs)
                              .arg(startupThresholdBytes())
                              .arg(buffer->maxSize())
                              .arg(m_sourceCodecName)
                              .arg(shouldApplyCreativeReorder ? 1 : 0));
        QMetaObject::invokeMethod(m_libavSeekDecoderWorker,
                                  [worker = m_libavSeekDecoderWorker,
                                   seekCache = m_pcmSeekCache]() {
                                      worker->setSeekCache(seekCache);
                                  },
                                  Qt::BlockingQueuedConnection);
        QMetaObject::invokeMethod(m_libavSeekDecoderWorker,
                                  [worker = m_libavSeekDecoderWorker,
                                   sessionId,
                                   sourcePath = m_sourcePath,
                                   startPositionMs,
                                   buffer,
                                   outputFormat = m_decoderPcmFormat]() {
                                      worker->startDecoding(sessionId,
                                                           sourcePath,
                                                           startPositionMs,
                                                           buffer,
                                                           outputFormat);
                                  },
                                  Qt::QueuedConnection);
    } else if (m_decoderWorker) {
        if (activeSwitchRebuildStartup) {
            PlayerLogger::log(QStringLiteral("player"),
                              QStringLiteral("activeSwitchRebuildTiming marker=decoder-start-queue session=%1 startPositionMs=%2 startupThresholdBytes=%3 bufferMaxSize=%4")
                                  .arg(sessionId)
                                  .arg(startPositionMs)
                                  .arg(startupThresholdBytes())
                                  .arg(buffer->maxSize()));
        }
        QMetaObject::invokeMethod(m_decoderWorker,
                                  [worker = m_decoderWorker,
                                   sessionId,
                                   ffmpegExecutable,
                                   arguments,
                                   buffer,
                                   outputFormat = m_decoderPcmFormat]() {
                                      worker->startDecoding(sessionId,
                                                           ffmpegExecutable,
                                                           arguments,
                                                           buffer,
                                                           outputFormat);
                                  },
                                  Qt::QueuedConnection);
        if (activeSwitchRebuildStartup) {
            PlayerLogger::log(QStringLiteral("player"),
                              QStringLiteral("activeSwitchRebuildTiming marker=decoder-start-queued session=%1")
                                  .arg(sessionId));
        }
    }
}

void WindowsWasapiAudioPlayer::teardownPipeline()
{
    if (!m_activeDecoderSessionId && !m_stoppingDecoderSessionId && !m_bufferDevice) {
        return;
    }

    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("teardownPipeline activeSession=%1 stoppingSession=%2")
                          .arg(m_activeDecoderSessionId)
                          .arg(m_stoppingDecoderSessionId));
    resetAnomalyTracking(QStringLiteral("teardownPipeline"));
    m_tearingDown = true;
    if (m_outputDeviceChangeTimer) {
        m_outputDeviceChangeTimer->stop();
    }
    m_audioStarted = false;
    m_decoderFinished = false;
    m_audioState = QAudio::StoppedState;
    m_audioError = QtAudio::NoError;
    if (isActiveOutputSwitchInProgress()
        && m_activeOutputSwitch.phase != ActiveOutputSwitchPhase::Applying) {
        resetActiveOutputSwitch(QStringLiteral("teardownPipeline"));
    }

    PlayerLogger::log(QStringLiteral("player"), QStringLiteral("teardownPipeline step=quarantineBufferDevice"));
    quarantineBufferDevice(QStringLiteral("teardownPipeline"), true);
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
    m_outputPcmFormat = {};
    m_decoderPcmFormat = {};
    m_outputWaveFormatData.clear();
    m_outputDeviceDescription.clear();

    m_stderrBuffer.clear();
    m_tearingDown = false;
    m_currentPipelineStartupProfile = PipelineStartupProfile::NormalStart;
    emitAudioLevels(0.0, 0.0);
    PlayerLogger::log(QStringLiteral("player"), QStringLiteral("teardownPipeline done"));
}

void WindowsWasapiAudioPlayer::handleDecoderError(int sessionId, const QString &message)
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

void WindowsWasapiAudioPlayer::handleDecoderFinished(int sessionId,
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
