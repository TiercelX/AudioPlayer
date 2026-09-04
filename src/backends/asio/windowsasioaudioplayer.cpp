#include "windowsasioaudioplayer.h"

#include "audioutils.h"
#include "windowsasioaudioplayer_discovery.h"
#include "windowsasioaudioplayer_formats.h"
#include "windowsasioaudioplayer_sessionprobe.h"
#include "windowsasioaudioplayer_utils.h"
#include "windowsasioaudioplayer_worker.h"

#include "asio_interface.h"
#include "libavseekdecoderworker.h"
#include "playerlogger.h"
#include "toollocator.h"

#include <QCoreApplication>
#include <QDateTime>

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QMediaDevices>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QStringList>
#include <QThread>
#include <QTimer>
#include <QVarLengthArray>
#include <QVector>
#include <QtEndian>
#include <QtMultimedia/qaudio.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <objbase.h>
#include <windows.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <propidl.h>
#include <propsys.h>

// PKEY_AudioEndpoint_FriendlyName — defined locally to avoid pulling in
// functiondiscoverykeys_devpkey.h which requires DEFINE_PROPERTYKEY.
static const PROPERTYKEY PKEY_AudioEndpoint_FriendlyName = {
    { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } },
    14
};

namespace {

constexpr double kAsioFfmpegInitialBurstSeconds = 1.5;

QAudioFormat fallbackAsioProbeFormat(const AsioDiscovery::AsioDriverEntry &entry)
{
    QAudioFormat format;
    format.setSampleRate(48000);
    const bool likelySurroundCreative =
        entry.name.contains(QStringLiteral("Creative"), Qt::CaseInsensitive)
        || entry.name.contains(QStringLiteral("Sound Blaster"), Qt::CaseInsensitive);
    format.setChannelCount(likelySurroundCreative ? 8 : 2);
    format.setSampleFormat(likelySurroundCreative ? QAudioFormat::Int32 : QAudioFormat::Float);
    return format;
}

} // namespace

void asioBufferSwitch(long doubleBufferIndex, ASIOBool)
{
    QMutexLocker locker(&g_callbackWorkerMutex);
    if (g_callbackWorker) {
        g_callbackWorker->renderCallback(doubleBufferIndex);
    }
}

void asioSampleRateDidChange(ASIOSampleRate sampleRate)
{
    PlayerLogger::log(QStringLiteral("audio"),
                      QStringLiteral("asio sampleRateDidChange rate=%1").arg(sampleRate));
}

long asioMessage(long selector, long value, void *, double *)
{
    switch (selector) {
    case kAsioSelectorSupported:
        return value == kAsioEngineVersion
            || value == kAsioResetRequest
            || value == kAsioBufferSizeChange
            || value == kAsioResyncRequest
            || value == kAsioLatenciesChanged
            || value == kAsioSupportsTimeInfo;
    case kAsioEngineVersion:
        return 2;
    case kAsioResetRequest: {
        QMutexLocker locker(&g_callbackWorkerMutex);
        if (g_callbackWorker) {
            g_callbackWorker->notifyResetRequest();
            PlayerLogger::log(QStringLiteral("audio"),
                              QStringLiteral("asio kAsioResetRequest received — queued for worker reinit"));
        }
        return 1;
    }
    case kAsioBufferSizeChange:
    case kAsioResyncRequest:
    case kAsioLatenciesChanged:
        return 1;
    default:
        break;
    }
    return 0;
}

ASIOTime *asioBufferSwitchTimeInfo(ASIOTime *params, long doubleBufferIndex, ASIOBool directProcess)
{
    asioBufferSwitch(doubleBufferIndex, directProcess);
    return params;
}

void WindowsAsioAudioPlayer::setHostWindowHandle(quintptr handle)
{
    AsioDiscovery::asioHostWindowHandle().store(handle, std::memory_order_relaxed);
}

QList<AudioOutputDeviceInfo> WindowsAsioAudioPlayer::availableAsioOutputDevices()
{
    static QMutex cacheMutex;
    static QList<AudioOutputDeviceInfo> cachedDevices;
    static bool cacheInitialized = false;

    {
        QMutexLocker locker(&cacheMutex);
        if (cacheInitialized) {
            return cachedDevices;
        }
    }

    QList<AudioOutputDeviceInfo> devices;
    for (const AsioDiscovery::AsioDriverEntry &entry : AsioDiscovery::registeredAsioDrivers()) {
        const QAudioFormat format = fallbackAsioProbeFormat(entry);
        const QString transport =
            entry.name.contains(QStringLiteral("Creative"), Qt::CaseInsensitive)
                || entry.name.contains(QStringLiteral("Sound Blaster"), Qt::CaseInsensitive)
            ? QStringLiteral("ASIO-unverified")
            : QStringLiteral("ASIO");
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("asio registry available name=%1 clsid=%2 rate=%3 channels=%4 sampleFormat=%5 transport=%6")
                              .arg(entry.name, entry.clsidText)
                              .arg(format.sampleRate())
                              .arg(format.channelCount())
                              .arg(static_cast<int>(format.sampleFormat()))
                              .arg(transport));

        devices.append({
            entry.clsidText.toUtf8(),
            entry.name,
            format,
            transport,
        });
    }

    {
        QMutexLocker locker(&cacheMutex);
        if (!cacheInitialized) {
            cachedDevices = devices;
            cacheInitialized = true;
        }
        return cachedDevices;
    }
    return devices;
}

bool WindowsAsioAudioPlayer::hasAvailableAsioOutputDevices()
{
    return !availableAsioOutputDevices().isEmpty();
}

bool WindowsAsioAudioPlayer::isLikelyCreativeDriverId(const QString &driverIdText)
{
    for (const AsioDiscovery::AsioDriverEntry &entry : AsioDiscovery::registeredAsioDrivers()) {
        if (entry.clsidText.compare(driverIdText, Qt::CaseInsensitive) == 0) {
            return entry.name.contains(QStringLiteral("Creative"), Qt::CaseInsensitive)
                || entry.name.contains(QStringLiteral("Sound Blaster"), Qt::CaseInsensitive)
                || entry.name.contains(QStringLiteral("BlasterX"), Qt::CaseInsensitive);
        }
    }
    return false;
}

bool WindowsAsioAudioPlayer::runDriverInitProbe(const QString &driverIdText,
                                                 quintptr hostWindowValue,
                                                 const QString &hostKind,
                                                 int hostIndex)
{
    HWND hostWindow = nullptr;
    if (hostKind == QStringLiteral("app-window")) {
        hostWindow = reinterpret_cast<HWND>(hostWindowValue);
    } else if (hostKind == QStringLiteral("desktop")) {
        hostWindow = GetDesktopWindow();
    }
    // null-handle → nullptr (default)

    PlayerLogger::log(QStringLiteral("audio"),
                      QStringLiteral("asio probe driverId=%1 hostKind=%2 hostWindow=%3 hostIndex=%4")
                          .arg(driverIdText, hostKind)
                          .arg(hostWindowValue)
                          .arg(hostIndex));

    IASIO *driver = AsioDiscovery::createAsioDriver(driverIdText);
    if (!driver) {
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("asio probe create-failed driverId=%1").arg(driverIdText));
        return false;
    }

    bool crashed = false;
    const ASIOBool initResult = AsioDiscovery::safeAsioInit(driver, hostWindow, &crashed);
    const QString error = AsioUtils::asioDriverError(driver);
    PlayerLogger::log(QStringLiteral("audio"),
                      QStringLiteral("asio probe result=%1 crashed=%2 error=%3")
                          .arg(initResult)
                          .arg(crashed ? 1 : 0)
                          .arg(error));
    AsioDiscovery::safeAsioRelease(driver);
    return initResult == kAsioTrue && !crashed;
}

WindowsAsioAudioPlayer::WindowsAsioAudioPlayer(QObject *parent)
    : AudioPlayerBackend(parent)
    , m_audioThread(new QThread(this))
    , m_audioWorker(new AsioOutputWorker)
    , m_decoderThread(new QThread(this))
    , m_decoderWorker(new FfmpegDecoderWorker)
    , m_libavSeekDecoderWorker(new LibavSeekDecoderWorker)
    , m_pauseReleaseTimer(new QTimer(this))
{
    m_pauseReleaseTimer->setSingleShot(true);
    m_pauseReleaseTimer->setInterval(3000);
    connect(m_pauseReleaseTimer, &QTimer::timeout, this, [this] {
        if (m_playbackState == PlaybackState::Paused && m_audioStarted) {
            PlayerLogger::log(QStringLiteral("player"),
                              QStringLiteral("asio pauseReleaseTimer fired (3s) — releasing driver"));
            m_audioStarted = false;
            releaseOutputResources();
        }
    });
    PlayerLogger::log(QStringLiteral("player"), QStringLiteral("WindowsAsioAudioPlayer initializing"));
    m_audioWorker->moveToThread(m_audioThread);
    connect(m_audioThread, &QThread::finished, m_audioWorker, &QObject::deleteLater);
    connect(m_audioWorker, &AsioOutputWorker::firstBufferSwitchReceived,
            this, &WindowsAsioAudioPlayer::handleAudioFirstBufferSwitch);
    connect(m_audioWorker, &AsioOutputWorker::positionUpdated,
            this, &WindowsAsioAudioPlayer::handleAudioPositionUpdated);
    connect(m_audioWorker, &AsioOutputWorker::stateChanged,
            this, &WindowsAsioAudioPlayer::handleAudioStateChanged);
    connect(m_audioWorker, &AsioOutputWorker::statusMessage, this,
            [this](const QString &message) {
                emit statusMessage(message);
            });
    m_audioThread->start();

    m_decoderWorker->moveToThread(m_decoderThread);
    connect(m_decoderThread, &QThread::finished, m_decoderWorker, &QObject::deleteLater);
    connect(m_decoderWorker, &FfmpegDecoderWorker::dataAvailable,
            this, &WindowsAsioAudioPlayer::handleDecoderDataAvailable);
    connect(m_decoderWorker, &FfmpegDecoderWorker::audioLevelsChanged, this,
            [this](int sessionId, qreal leftLevel, qreal rightLevel) {
                if (sessionId != m_activeDecoderSessionId || m_playbackState == PlaybackState::Stopping) {
                    return;
                }
                emitAudioLevels(leftLevel, rightLevel);
            });
    connect(m_decoderWorker, &FfmpegDecoderWorker::errorOccurred,
            this, &WindowsAsioAudioPlayer::handleDecoderError);
    connect(m_decoderWorker, &FfmpegDecoderWorker::finished,
            this, &WindowsAsioAudioPlayer::handleDecoderFinished);

    m_libavSeekDecoderWorker->moveToThread(m_decoderThread);
    connect(m_decoderThread, &QThread::finished, m_libavSeekDecoderWorker, &QObject::deleteLater);
    connect(m_libavSeekDecoderWorker, &LibavSeekDecoderWorker::dataAvailable,
            this, &WindowsAsioAudioPlayer::handleDecoderDataAvailable);
    connect(m_libavSeekDecoderWorker, &LibavSeekDecoderWorker::audioLevelsChanged, this,
            [this](int sessionId, qreal leftLevel, qreal rightLevel) {
                if (sessionId != m_activeDecoderSessionId || m_playbackState == PlaybackState::Stopping) {
                    return;
                }
                emitAudioLevels(leftLevel, rightLevel);
            });
    connect(m_libavSeekDecoderWorker, &LibavSeekDecoderWorker::errorOccurred,
            this, &WindowsAsioAudioPlayer::handleDecoderError);
    connect(m_libavSeekDecoderWorker, &LibavSeekDecoderWorker::finished,
            this, &WindowsAsioAudioPlayer::handleDecoderFinished);
    m_decoderThread->start();
}

WindowsAsioAudioPlayer::~WindowsAsioAudioPlayer()
{
    teardownPipeline();
    if (m_audioThread) {
        m_audioThread->quit();
        m_audioThread->wait();
    }
    if (m_decoderThread) {
        m_decoderThread->quit();
        m_decoderThread->wait();
    }
}

AudioPlayerBackend::BackendId WindowsAsioAudioPlayer::backendId() const
{
    return BackendId::WindowsAsio;
}

QString WindowsAsioAudioPlayer::backendName() const
{
    return tr("Windows ASIO");
}

QString WindowsAsioAudioPlayer::decoderName() const
{
    if (m_sourceCodecName.isEmpty()) {
        return {};
    }

    return shouldUseLibavDecoder() ? tr("libav (in-process)") : tr("ffmpeg CLI");
}

void WindowsAsioAudioPlayer::setSource(const QString &filePath,
                                       int sourceChannelCount,
                                       int sourceSampleRate,
                                       int sourceBitDepth,
                                       const QString &sourceCodecName)
{
    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("asio setSource path=%1 channels=%2 sampleRate=%3 bitDepth=%4 codec=%5")
                          .arg(filePath)
                          .arg(sourceChannelCount)
                          .arg(sourceSampleRate)
                          .arg(sourceBitDepth)
                          .arg(sourceCodecName));
    if (m_sourcePath == filePath
        && m_sourceChannelCount == sourceChannelCount
        && m_sourceSampleRate == sourceSampleRate
        && m_sourceBitDepth == sourceBitDepth
        && m_sourceCodecName == sourceCodecName) {
        return;
    }

    stop();
    m_sourcePath = filePath;
    m_sourceChannelCount = sourceChannelCount;
    m_sourceSampleRate = sourceSampleRate;
    m_sourceBitDepth = sourceBitDepth;
    m_sourceCodecName = sourceCodecName;
    m_currentPositionMs = 0;
    m_startPositionMs = 0;
}

QString WindowsAsioAudioPlayer::source() const
{
    return m_sourcePath;
}

void WindowsAsioAudioPlayer::play()
{
    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("asio play state=%1 source=%2")
                          .arg(AudioUtils::playbackStateName(m_playbackState))
                          .arg(m_sourcePath));
    if (m_sourcePath.isEmpty()) {
        return;
    }

    if (m_sessionRetryDeadline > 0 && m_sessionRetryAttempt > 0) {
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("asio play ignored reason=session-retry-active generation=%1 attempt=%2")
                              .arg(m_sessionRetryGeneration)
                              .arg(m_sessionRetryAttempt));
        emitAsioBusyRetryStatus();
        return;
    }

    if (m_playbackState == PlaybackState::Playing || m_playbackState == PlaybackState::Stopping) {
        return;
    }

    if (m_playbackState == PlaybackState::Paused && m_audioStarted && m_audioWorker) {
        m_pauseReleaseTimer->stop();
        if (m_decoderWorker) {
            QMetaObject::invokeMethod(m_decoderWorker, [w = m_decoderWorker] { w->setPaused(false); }, Qt::QueuedConnection);
        }
        const int sessionId = m_activeDecoderSessionId;
        const int generation = ++m_pauseResumeGeneration;
        QMetaObject::invokeMethod(m_audioWorker, [worker = m_audioWorker, sessionId, generation] {
            worker->resumeOutput(sessionId, generation);
        }, Qt::QueuedConnection);
        setPlaybackState(PlaybackState::Playing);
        return;
    }

    m_pauseReleaseTimer->stop();
    startPipeline(m_startPositionMs);
}

void WindowsAsioAudioPlayer::pause()
{
    if (m_playbackState != PlaybackState::Playing || !m_audioStarted || !m_audioWorker) {
        return;
    }

    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("asio pause position=%1")
                          .arg(m_currentPositionMs));

    // Stop ASIO streaming but keep the driver open. If the user
    // resumes within 3 seconds, the driver is still available for fast resume.
    // After 3 seconds, the timer releases the driver so other apps can use
    // the endpoint.
    const int sessionId = m_activeDecoderSessionId;
    const int generation = ++m_pauseResumeGeneration;
    QMetaObject::invokeMethod(m_audioWorker, [worker = m_audioWorker, sessionId, generation] {
        worker->pauseOutput(sessionId, generation);
    }, Qt::QueuedConnection);
    if (m_decoderWorker) {
        QMetaObject::invokeMethod(m_decoderWorker, [w = m_decoderWorker] { w->setPaused(true); }, Qt::QueuedConnection);
    }
    m_startPositionMs = m_currentPositionMs;
    emitAudioLevels(0.0, 0.0);
    setPlaybackState(PlaybackState::Paused);
    m_pauseReleaseTimer->start();
}

void WindowsAsioAudioPlayer::stop()
{
    m_pauseReleaseTimer->stop();
    m_pauseResumeGeneration = 0;
    cancelSessionRetry();
    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("asio stop state=%1 activeSession=%2")
                          .arg(AudioUtils::playbackStateName(m_playbackState))
                          .arg(m_activeDecoderSessionId));
    if (m_playbackState == PlaybackState::Stopped
        && !m_activeDecoderSessionId
        && !m_stoppingDecoderSessionId
        && m_outputRetainedAfterCompletion) {
        m_currentPositionMs = 0;
        m_startPositionMs = 0;
        m_lastLoggedPositionBucket = -1;
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("asio stop retained-completion-output"));
        emitAudioLevels(0.0, 0.0);
        emit positionChanged(0);
        setPlaybackState(PlaybackState::Stopped);
        return;
    }

    m_audioStarted = false;
    m_decoderFinished = false;
    m_currentPositionMs = 0;
    m_startPositionMs = 0;
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

void WindowsAsioAudioPlayer::seek(qint64 positionMs)
{
    if (m_sourcePath.isEmpty()
        || m_playbackState == PlaybackState::Stopping) {
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
    if (m_playbackState == PlaybackState::Paused) {
        teardownPipeline();
        m_currentPositionMs = clampedPosition;
        m_startPositionMs = clampedPosition;
        emit positionChanged(clampedPosition);
        return;
    }

    startPipeline(clampedPosition);
}

void WindowsAsioAudioPlayer::setVolume(qreal volume)
{
    m_volume = volume;
    if (m_audioWorker) {
        QMetaObject::invokeMethod(m_audioWorker, [worker = m_audioWorker, volume] {
            worker->setVolume(volume);
        }, Qt::QueuedConnection);
    }
}

QList<QAudioDevice> WindowsAsioAudioPlayer::availableOutputDevices() const
{
    return {};
}

QList<AudioOutputDeviceInfo> WindowsAsioAudioPlayer::availableOutputDeviceInfos() const
{
    return availableAsioOutputDevices();
}

QString WindowsAsioAudioPlayer::outputDeviceDescription() const
{
    return m_outputDeviceDescription;
}

QAudioFormat WindowsAsioAudioPlayer::outputFormat() const
{
    return m_outputFormat;
}

QAudioDevice WindowsAsioAudioPlayer::selectedOutputDevice() const
{
    return {};
}

AudioOutputDeviceInfo WindowsAsioAudioPlayer::selectedOutputDeviceInfo() const
{
    return resolveOutputDevice();
}

QByteArray WindowsAsioAudioPlayer::selectedOutputDeviceId() const
{
    return m_selectedOutputDeviceId;
}

bool WindowsAsioAudioPlayer::usesDefaultOutputDevice() const
{
    return m_selectedOutputDeviceId.isEmpty();
}

void WindowsAsioAudioPlayer::setOutputDeviceId(const QByteArray &deviceId)
{
    QByteArray normalizedDeviceId = deviceId;
    if (!normalizedDeviceId.isEmpty()) {
        const QList<AudioOutputDeviceInfo> devices = availableOutputDeviceInfos();
        const auto foundIt = std::find_if(devices.cbegin(), devices.cend(), [&normalizedDeviceId](const AudioOutputDeviceInfo &device) {
            return device.id == normalizedDeviceId;
        });
        if (foundIt == devices.cend()) {
            PlayerLogger::log(QStringLiteral("player"),
                              QStringLiteral("asio setOutputDeviceId fallback-default unknownId=%1")
                                  .arg(QString::fromUtf8(normalizedDeviceId)));
            normalizedDeviceId.clear();
        }
    }

    if (m_selectedOutputDeviceId == normalizedDeviceId) {
        return;
    }

    cancelSessionRetry();
    m_selectedOutputDeviceId = normalizedDeviceId;
    emitOutputDeviceSelectionChanged();
    if (m_sourcePath.isEmpty() || m_playbackState == PlaybackState::Stopping) {
        return;
    }
    if (m_playbackState == PlaybackState::Playing) {
        startPipeline(m_currentPositionMs);
    } else if (m_playbackState == PlaybackState::Paused) {
        teardownPipeline();
        m_startPositionMs = m_currentPositionMs;
        setPlaybackState(PlaybackState::Paused);
    }
}

void WindowsAsioAudioPlayer::refreshOutputConfiguration(bool force)
{
    Q_UNUSED(force);
    if (m_playbackState == PlaybackState::Playing) {
        startPipeline(m_currentPositionMs);
    } else if (m_playbackState == PlaybackState::Paused) {
        teardownPipeline();
        m_startPositionMs = m_currentPositionMs;
        setPlaybackState(PlaybackState::Paused);
    }
}



QString WindowsAsioAudioPlayer::channelLayoutForCount(int channelCount) const
{
    switch (channelCount) {
    case 1:
        return QStringLiteral("mono");
    case 2:
        return QStringLiteral("stereo");
    case 3:
        return QStringLiteral("2.1");
    case 4:
        return QStringLiteral("quad");
    case 5:
        return QStringLiteral("4.1");
    case 6:
        return QStringLiteral("5.1");
    case 7:
        return QStringLiteral("6.1");
    case 8:
        return QStringLiteral("7.1");
    default:
        return {};
    }
}

int WindowsAsioAudioPlayer::creativeChannelReorderMode() const
{
    return m_creativeChannelReorderMode;
}

void WindowsAsioAudioPlayer::setCreativeChannelReorderMode(int mode)
{
    if (m_creativeChannelReorderMode == mode) {
        return;
    }
    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("asio creativeChannelReorderMode previous=%1 mode=%2")
                          .arg(m_creativeChannelReorderMode)
                          .arg(mode));
    m_creativeChannelReorderMode = mode;
}

bool WindowsAsioAudioPlayer::exactPlaybackEnabled() const
{
    return m_exactPlaybackEnabled;
}

void WindowsAsioAudioPlayer::setExactPlaybackEnabled(bool enabled)
{
    if (m_exactPlaybackEnabled == enabled) {
        return;
    }
    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("asio exactPlayback previous=%1 enabled=%2")
                          .arg(m_exactPlaybackEnabled ? 1 : 0)
                          .arg(enabled ? 1 : 0));
    m_exactPlaybackEnabled = enabled;
}

QString WindowsAsioAudioPlayer::locateFfmpegExecutable() const
{
    return AudioUtils::locateFfmpegExecutable();
}

QString WindowsAsioAudioPlayer::rawInputFormatForSource() const
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

bool WindowsAsioAudioPlayer::shouldUseLibavDecoder() const
{
#ifdef AUDIOPLAYER_LIBAV_DECODER
    return isPackagedLibavAudioCodec(m_sourceCodecName)
        && m_libavSeekDecoderWorker != nullptr;
#else
    return false;
#endif
}

void WindowsAsioAudioPlayer::finalizePlayback()
{
    finishOutputAfterCompletion();
    stopDecoderWorker(true);
    clearBufferDevice();
    m_activeDecoderSessionId = 0;
    m_stoppingDecoderSessionId = 0;
    m_activeOutputDeviceId.clear();
    m_outputFormat = {};
    m_outputDeviceDescription.clear();
    m_stderrBuffer.clear();
    m_audioStarted = false;
    m_decoderFinished = false;
    m_currentPositionMs = 0;
    m_startPositionMs = 0;
    cancelSessionRetry();
    setPlaybackState(PlaybackState::Stopped);
    emit finished();
}

void WindowsAsioAudioPlayer::handleAudioFirstBufferSwitch(int sessionId)
{
    if (sessionId != m_activeDecoderSessionId || m_playbackState == PlaybackState::Stopping) {
        return;
    }
    m_lastFailedDriverId.clear();
    m_sessionRetryAttempt = 0;
    m_sessionRetryDeadline = 0;
    emit statusMessage(tr("ASIO 播放已启动"));
}

void WindowsAsioAudioPlayer::handleAudioPositionUpdated(int sessionId, qint64 processedPositionMs)
{
    if (sessionId != m_activeDecoderSessionId || m_playbackState == PlaybackState::Stopping) {
        return;
    }
    m_currentPositionMs = processedPositionMs;
    emit positionChanged(processedPositionMs);
}

void WindowsAsioAudioPlayer::handleAudioStateChanged(int sessionId, int state, int error)
{
    const QAudio::State audioState = static_cast<QAudio::State>(state);
    const QtAudio::Error audioError = static_cast<QtAudio::Error>(error);
    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("asio audioStateChanged session=%1 state=%2 error=%3 activeSession=%4")
                          .arg(sessionId)
                          .arg(AudioUtils::audioStateName(audioState))
                          .arg(error)
                          .arg(m_activeDecoderSessionId));
    if (sessionId != m_activeDecoderSessionId || m_playbackState == PlaybackState::Stopping) {
        return;
    }
    if (audioState == QAudio::StoppedState && audioError != QtAudio::NoError) {
        const auto failureReason = m_audioWorker->lastOpenFailureReason();
        const QString failureDetail = m_audioWorker->lastOpenFailureDetail();
        // Log technical details
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("asio handleAudioStateChanged Stopped+Error reason=%1 detail=%2 tearing-down")
                              .arg(failureReason == AsioOutputWorker::DriverOpenFailureReason::DeviceBusy
                                       ? QStringLiteral("DeviceBusy")
                                       : (failureReason == AsioOutputWorker::DriverOpenFailureReason::RecoveryTimeout
                                              ? QStringLiteral("RecoveryTimeout")
                                              : QStringLiteral("DriverError")))
                              .arg(failureDetail.isEmpty() ? QStringLiteral("unknown") : failureDetail));

        // User-friendly error messages
        QString errorMsg;
        PlaybackError errorCode = PlaybackError::DriverRecoveryFailure;
        switch (failureReason) {
        case AsioOutputWorker::DriverOpenFailureReason::DeviceBusy:
            errorCode = PlaybackError::DeviceOccupied;
            errorMsg = tr("ASIO 设备被其他应用占用\n\n"
                          "建议操作：\n"
                          "1. 暂停或关闭其他音频应用（如 Apple Music、浏览器）\n"
                          "2. 等待几秒后重试\n"
                          "3. 如果问题持续，切换到 WASAPI 模式");
            break;
        case AsioOutputWorker::DriverOpenFailureReason::RecoveryTimeout:
            errorCode = PlaybackError::OutputRecoveryFailure;
            errorMsg = tr("ASIO 播放恢复超时（%1 秒）\n\n"
                          "设备可能仍被其他应用占用\n\n"
                          "建议操作：\n"
                          "1. 暂停或关闭其他音频应用\n"
                          "2. 重新播放\n"
                          "3. 如果问题持续，切换到 WASAPI 模式")
                           .arg(kAsioBusyRetryTimeoutMs / 1000);
            break;
        case AsioOutputWorker::DriverOpenFailureReason::None:
        case AsioOutputWorker::DriverOpenFailureReason::DriverError: {
            // Map technical details to user-friendly messages
            QString userDetail;
            if (failureDetail.contains(QStringLiteral("init() returned 0"))) {
                errorCode = PlaybackError::DriverInitFailure;
                userDetail = tr("驱动初始化被拒绝，可能被其他应用占用");
            } else if (failureDetail.contains(QStringLiteral("createBuffers failed"))) {
                userDetail = tr("音频缓冲区创建失败，请关闭其他音频应用");
            } else if (failureDetail.contains(QStringLiteral("No supported sample rate"))) {
                errorCode = PlaybackError::FormatNotSupported;
                userDetail = tr("设备不支持当前音频格式");
            } else if (failureDetail.contains(QStringLiteral("device disconnected"))
                       || failureDetail.contains(QStringLiteral("Driver released during pause"))) {
                // Device disconnected - show specific message
                emit errorOccurred(PlaybackError::DeviceDisconnected,
                                   tr("ASIO 设备已断开\n\n"
                                       "音频设备可能已被拔出或禁用\n\n"
                                       "建议操作：\n"
                                       "1. 重新连接音频设备\n"
                                       "2. 重新播放"));
                return;
            } else if (!failureDetail.isEmpty()) {
                userDetail = failureDetail;
            }

            if (userDetail.isEmpty()) {
                errorMsg = tr("ASIO 驱动恢复失败\n\n"
                              "建议操作：\n"
                              "1. 关闭其他音频应用\n"
                              "2. 重新播放\n"
                              "3. 如果问题持续，切换到 WASAPI 模式");
            } else {
                errorMsg = tr("ASIO 驱动恢复失败\n\n"
                              "技术详情：%1\n\n"
                              "建议操作：\n"
                              "1. 关闭其他音频应用\n"
                              "2. 重新播放\n"
                              "3. 如果问题持续，切换到 WASAPI 模式")
                               .arg(userDetail);
            }
            break;
        }
        }
        m_lastFailedDriverId = m_activeOutputDeviceId;
        teardownPipeline();
        cancelSessionRetry();
        setPlaybackState(PlaybackState::Stopped);
        emit errorOccurred(errorCode, errorMsg);
        return;
    }
    if (audioState == QAudio::IdleState && m_decoderFinished) {
        finalizePlayback();
    }
}

void WindowsAsioAudioPlayer::handleDecoderDataAvailable(int sessionId)
{
    if (sessionId != m_activeDecoderSessionId || m_playbackState == PlaybackState::Stopping) {
        return;
    }
    startAudioOutputIfReady();
}

void WindowsAsioAudioPlayer::handleDecoderError(int sessionId, const QString &message)
{
    if (sessionId != m_activeDecoderSessionId || m_playbackState == PlaybackState::Stopping) {
        return;
    }
    teardownPipeline();
    cancelSessionRetry();
    setPlaybackState(PlaybackState::Stopped);
    emit errorOccurred(PlaybackError::DecoderError, message);
}

void WindowsAsioAudioPlayer::handleDecoderFinished(int sessionId,
                                                   int exitCode,
                                                   int exitStatus,
                                                   const QString &stderrText)
{
    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("asio decoderFinished session=%1 exitCode=%2 exitStatus=%3 stderr=%4")
                          .arg(sessionId)
                          .arg(exitCode)
                          .arg(exitStatus)
                          .arg(stderrText.trimmed()));
    if (sessionId == m_stoppingDecoderSessionId) {
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
                          QStringLiteral("asio decoderFailed exitCode=%1 stderr=%2")
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
        teardownPipeline();
        cancelSessionRetry();
        setPlaybackState(PlaybackState::Stopped);
        emit errorOccurred(PlaybackError::DecoderError, errorText);
        return;
    }

    startAudioOutputIfReady();
    if (auto *buffer = static_cast<PcmStreamBuffer *>(m_bufferDevice);
        buffer && buffer->isEmpty() && !m_audioStarted) {
        finalizePlayback();
    }
}

void WindowsAsioAudioPlayer::clearBufferDevice()
{
    delete m_bufferDevice;
    m_bufferDevice = nullptr;
}

void WindowsAsioAudioPlayer::finishOutputAfterCompletion()
{
    if (!m_audioWorker || !m_activeDecoderSessionId) {
        return;
    }
    const int sessionId = m_activeDecoderSessionId;
    QMetaObject::invokeMethod(m_audioWorker,
                              [worker = m_audioWorker, sessionId] {
                                  worker->finishOutput(sessionId);
                              },
                              Qt::BlockingQueuedConnection);
    m_outputRetainedAfterCompletion = true;
}

void WindowsAsioAudioPlayer::releaseOutputResources()
{
    if (!m_audioWorker) {
        return;
    }
    const int sessionId = m_activeDecoderSessionId != 0
        ? m_activeDecoderSessionId
        : m_stoppingDecoderSessionId;
    QMetaObject::invokeMethod(m_audioWorker,
                              [worker = m_audioWorker, sessionId] {
                                  worker->releaseOutput(sessionId, true);
                              },
                              Qt::BlockingQueuedConnection);
    m_outputRetainedAfterCompletion = false;
}

void WindowsAsioAudioPlayer::cancelSessionRetry()
{
    ++m_sessionRetryGeneration;
    m_sessionRetryAttempt = 0;
    m_sessionRetryDeadline = 0;
}

void WindowsAsioAudioPlayer::emitAsioBusyRetryStatus()
{
    if (m_sessionRetryDeadline <= 0) {
        emit statusMessage(tr("ASIO 设备被占用，正在重试（0s/%1s）").arg(kAsioBusyRetryTimeoutMs / 1000));
        return;
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 elapsedMs = nowMs - (m_sessionRetryDeadline - kAsioBusyRetryTimeoutMs);
    const int elapsedSec = static_cast<int>(
        qBound<qint64>(0, elapsedMs, static_cast<qint64>(kAsioBusyRetryTimeoutMs)) / 1000);
    emit statusMessage(tr("ASIO 设备被占用，正在重试（%1s/%2s）")
                           .arg(elapsedSec)
                           .arg(kAsioBusyRetryTimeoutMs / 1000));
}

void WindowsAsioAudioPlayer::startAudioOutputIfReady()
{
    auto *buffer = static_cast<PcmStreamBuffer *>(m_bufferDevice);
    if (!m_audioWorker || !buffer || m_audioStarted) {
        return;
    }
    const qsizetype threshold = qMax<qsizetype>(32768,
                                                static_cast<qsizetype>(m_outputFormat.bytesPerFrame())
                                                    * m_outputFormat.sampleRate() / 5);
    if (!m_decoderFinished && buffer->bufferedBytes() < threshold) {
        return;
    }

    const int sessionId = m_activeDecoderSessionId;
    QMetaObject::invokeMethod(m_audioWorker,
                              [worker = m_audioWorker, sessionId, buffer] {
                                  worker->startOutput(sessionId, buffer);
                              },
                              Qt::BlockingQueuedConnection);
    m_audioStarted = true;
}

void WindowsAsioAudioPlayer::startPipeline(qint64 startPositionMs)
{
    const int retryGeneration = ++m_sessionRetryGeneration;
    m_sessionRetryAttempt = 0;
    m_sessionRetryDeadline = 0;
    startPipelineAttempt(startPositionMs, retryGeneration);
}

void WindowsAsioAudioPlayer::startPipelineAttempt(qint64 startPositionMs, int retryGeneration)
{
    if (retryGeneration != m_sessionRetryGeneration
        || m_sourcePath.isEmpty()
        || m_playbackState == PlaybackState::Stopping) {
        return;
    }

    const bool useLibavDecoder = shouldUseLibavDecoder();
    if (!useLibavDecoder && locateFfmpegExecutable().isEmpty()) {
        cancelSessionRetry();
        emit errorOccurred(PlaybackError::DecoderNotFound,
                           tr("未找到 FFmpeg 解码器\n\n"
                               "程序可能安装不完整\n\n"
                               "建议操作：重新安装程序或检查程序完整性"));
        return;
    }

    const AudioOutputDeviceInfo device = resolveOutputDevice();
    QString deviceDescription;
    const QAudioFormat selectedFormat = selectOutputFormat(device, &deviceDescription);
    if (device.isNull() || !selectedFormat.isValid()) {
        cancelSessionRetry();
        emit errorOccurred(PlaybackError::DeviceNotFound,
                           tr("未找到可用的 ASIO 输出设备\n\n"
                               "建议操作：\n"
                               "1. 检查音频设备连接\n"
                               "2. 尝试切换到 WASAPI 模式"));
        return;
    }

    const QByteArray driverId = device.id;
    if (m_sessionRetryDeadline <= 0) {
        m_sessionRetryDeadline = QDateTime::currentMSecsSinceEpoch() + kAsioBusyRetryTimeoutMs;
    }

    const bool isBusy = AsioSessionProbe::hasActiveExternalWasapiRenderSessionsForAsioDriver(driverId);
    if (!isBusy) {
        if (m_sessionRetryAttempt > 0) {
            const qint64 elapsedMs =
                QDateTime::currentMSecsSinceEpoch() - (m_sessionRetryDeadline - kAsioBusyRetryTimeoutMs);
            const int timeoutSec = kAsioBusyRetryTimeoutMs / 1000;
            PlayerLogger::log(QStringLiteral("player"),
                              QStringLiteral("asio startPipeline session-cleared driverId=%1 attempts=%2 elapsed=%3ms")
                                  .arg(QString::fromUtf8(driverId))
                                  .arg(m_sessionRetryAttempt)
                                  .arg(elapsedMs));
            emit statusMessage(tr("ASIO 设备已可用（%1s/%2s），正在启动播放")
                                   .arg(static_cast<int>(qMax<qint64>(0, elapsedMs) / 1000))
                                   .arg(timeoutSec));
        }
        continueStartPipeline(startPositionMs, retryGeneration);
        return;
    }

    ++m_sessionRetryAttempt;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 elapsedMs = nowMs - (m_sessionRetryDeadline - kAsioBusyRetryTimeoutMs);
    const int timeoutSec = kAsioBusyRetryTimeoutMs / 1000;
    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("asio startPipeline session-retry driverId=%1 generation=%2 attempt=%3 elapsed=%4ms busy=1")
                          .arg(QString::fromUtf8(driverId))
                          .arg(retryGeneration)
                          .arg(m_sessionRetryAttempt)
                          .arg(elapsedMs));
    emitAsioBusyRetryStatus();

    if (nowMs >= m_sessionRetryDeadline) {
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("asio startPipeline session-check-timeout driverId=%1 generation=%2 attempts=%3")
                              .arg(QString::fromUtf8(driverId))
                              .arg(retryGeneration)
                              .arg(m_sessionRetryAttempt));
        cancelSessionRetry();
        setPlaybackState(PlaybackState::Stopped);
        emit errorOccurred(PlaybackError::DeviceOccupied,
                           tr("ASIO 设备仍被其他应用占用（已重试 %1 秒），请暂停或关闭其他音频应用后重试")
                               .arg(timeoutSec));
        return;
    }

    QTimer::singleShot(kAsioBusyRetryIntervalMs, this, [this, startPositionMs, retryGeneration] {
        startPipelineAttempt(startPositionMs, retryGeneration);
    });
}

void WindowsAsioAudioPlayer::continueStartPipeline(qint64 startPositionMs, int retryGeneration)
{
    if (retryGeneration != m_sessionRetryGeneration
        || m_sourcePath.isEmpty()
        || m_playbackState == PlaybackState::Stopping) {
        return;
    }

    const bool useLibavDecoder = shouldUseLibavDecoder();
    const QString ffmpegExecutable = useLibavDecoder ? QString() : locateFfmpegExecutable();
    if (!useLibavDecoder && ffmpegExecutable.isEmpty()) {
        cancelSessionRetry();
        emit errorOccurred(PlaybackError::DecoderNotFound,
                           tr("未找到 FFmpeg 解码器\n\n"
                               "程序可能安装不完整\n\n"
                               "建议操作：重新安装程序或检查程序完整性"));
        return;
    }

    const AudioOutputDeviceInfo device = resolveOutputDevice();
    QString deviceDescription;
    const QAudioFormat selectedFormat = selectOutputFormat(device, &deviceDescription);
    if (device.isNull() || !selectedFormat.isValid()) {
        cancelSessionRetry();
        emit errorOccurred(PlaybackError::DeviceNotFound,
                           tr("未找到可用的 ASIO 输出设备\n\n"
                               "建议操作：\n"
                               "1. 检查音频设备连接\n"
                               "2. 尝试切换到 WASAPI 模式"));
        return;
    }

    // If the same driver failed on the previous attempt, force a full driver
    // release so openDriver() starts from scratch.
    if (!m_lastFailedDriverId.isEmpty() && m_lastFailedDriverId == device.id) {
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("asio startPipeline retry-after-failure driverId=%1 — forcing driver release")
                              .arg(QString::fromUtf8(device.id)));
        if (m_audioWorker) {
            QMetaObject::invokeMethod(m_audioWorker, [worker = m_audioWorker] {
                worker->forceReleaseDriver();
            }, Qt::BlockingQueuedConnection);
        }
        m_lastFailedDriverId.clear();
    }

    teardownPipeline();

    m_outputFormat = selectedFormat;
    m_outputDeviceDescription = deviceDescription;
    m_activeOutputDeviceId = device.id;

    PcmStreamFormat decoderPcmFormat = AsioFormats::pcmStreamFormatFromQAudioFormat(m_outputFormat);
    if (m_sourceBitDepth > 0 && m_sourceBitDepth <= 16
        && decoderPcmFormat.sampleEncoding == PcmSampleEncoding::Int16) {
        decoderPcmFormat.sampleEncoding = PcmSampleEncoding::Int32;
        decoderPcmFormat.validBitsPerSample = 32;
    }
    QAudioFormat decoderQFormat;
    decoderQFormat.setSampleRate(decoderPcmFormat.sampleRate);
    decoderQFormat.setChannelCount(decoderPcmFormat.channelCount);
    decoderQFormat.setSampleFormat(decoderPcmFormat.qAudioSampleFormat());

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

    m_activeDecoderSessionId = ++m_decoderSessionId;
    m_stoppingDecoderSessionId = 0;
    buffer->setOwner(m_activeDecoderSessionId, 0, m_sourcePath);

    if (m_audioWorker) {
        const int sessionId = m_activeDecoderSessionId;
        m_outputRetainedAfterCompletion = false;
        QMetaObject::invokeMethod(m_audioWorker,
                                  [worker = m_audioWorker,
                                   sessionId,
                                   driverId = device.id,
                                   format = m_outputFormat,
                                   volume = m_volume,
                                   startPositionMs,
                                   sourceBitDepth = m_sourceBitDepth,
                                   decoderFormat = decoderQFormat] {
                                      worker->configureOutput(sessionId,
                                                              driverId,
                                                              format,
                                                              volume,
                                                              startPositionMs,
                                                              sourceBitDepth,
                                                              decoderFormat);
                                  },
                                  Qt::BlockingQueuedConnection);

        bool outputPrepared = false;
        QAudioFormat preparedOutputFormat;
        QMetaObject::invokeMethod(m_audioWorker,
                                  [worker = m_audioWorker, sessionId, &outputPrepared, &preparedOutputFormat] {
                                      outputPrepared = worker->prepareOutput(sessionId);
                                      if (outputPrepared) {
                                          preparedOutputFormat = worker->preparedOutputFormat();
                                      }
                                  },
                                  Qt::BlockingQueuedConnection);
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("asio prepareOutput session=%1 prepared=%2")
                              .arg(sessionId)
                              .arg(outputPrepared ? 1 : 0));
        if (!outputPrepared) {
            const auto failureReason = m_audioWorker->lastOpenFailureReason();
            PlayerLogger::log(QStringLiteral("player"),
                              QStringLiteral("asio prepareFailed reason=%1")
                                  .arg(failureReason == AsioOutputWorker::DriverOpenFailureReason::DeviceBusy
                                           ? QStringLiteral("DeviceBusy") : QStringLiteral("DriverError")));
            if (failureReason == AsioOutputWorker::DriverOpenFailureReason::DeviceBusy) {
                // Device still occupied after session check passed — check if
                // we've exceeded the overall timeout before retrying.
                if (m_sessionRetryDeadline > 0 && QDateTime::currentMSecsSinceEpoch() >= m_sessionRetryDeadline) {
                    m_lastFailedDriverId = device.id;
                    releaseOutputResources();
                    clearBufferDevice();
                    m_activeDecoderSessionId = 0;
                    m_stoppingDecoderSessionId = 0;
                    m_activeOutputDeviceId.clear();
                    m_outputFormat = {};
                    m_outputDeviceDescription.clear();
                    cancelSessionRetry();
                    setPlaybackState(PlaybackState::Stopped);
                    emit errorOccurred(PlaybackError::DeviceOccupied,
                                       tr("ASIO 设备被其他应用占用\n\n"
                                           "已重试 %1 秒仍无法获取设备\n\n"
                                           "建议操作：\n"
                                           "1. 暂停或关闭其他音频应用（如 Apple Music、浏览器）\n"
                                           "2. 等待几秒后重试\n"
                                           "3. 如果问题持续，切换到 WASAPI 模式")
                                            .arg(kAsioBusyRetryTimeoutMs / 1000));
                    return;
                }
                ++m_sessionRetryAttempt;
                emitAsioBusyRetryStatus();
                releaseOutputResources();
                clearBufferDevice();
                m_activeDecoderSessionId = 0;
                m_stoppingDecoderSessionId = 0;
                m_activeOutputDeviceId.clear();
                m_outputFormat = {};
                m_outputDeviceDescription.clear();
                QTimer::singleShot(kAsioBusyRetryIntervalMs, this, [this, startPositionMs, retryGeneration] {
                    startPipelineAttempt(startPositionMs, retryGeneration);
                });
                return;
            }
            m_lastFailedDriverId = device.id;
            const QString failureDetail = m_audioWorker->lastOpenFailureDetail();
            PlayerLogger::log(QStringLiteral("player"),
                              QStringLiteral("asio prepareFailed reason=DriverError detail=%1")
                                  .arg(failureDetail.isEmpty() ? QStringLiteral("unknown") : failureDetail));
            releaseOutputResources();
            clearBufferDevice();
            m_activeDecoderSessionId = 0;
            m_stoppingDecoderSessionId = 0;
            m_activeOutputDeviceId.clear();
            m_outputFormat = {};
            m_outputDeviceDescription.clear();
            cancelSessionRetry();
            setPlaybackState(PlaybackState::Stopped);
            // Log technical details
            PlayerLogger::log(QStringLiteral("player"),
                              QStringLiteral("asio initFailed detail=%1")
                                  .arg(failureDetail.isEmpty() ? QStringLiteral("unknown") : failureDetail));
            // User-friendly message
            {
                PlaybackError errorCode = PlaybackError::DriverInitFailure;
                QString userDetail;
                if (failureDetail.contains(QStringLiteral("init() returned 0"))) {
                    userDetail = tr("驱动初始化被拒绝，可能被其他应用占用");
                } else if (failureDetail.contains(QStringLiteral("createBuffers failed"))) {
                    userDetail = tr("音频缓冲区创建失败，请关闭其他音频应用");
                } else if (failureDetail.contains(QStringLiteral("No supported sample rate"))) {
                    errorCode = PlaybackError::FormatNotSupported;
                    userDetail = tr("设备不支持当前音频格式");
                } else if (!failureDetail.isEmpty()) {
                    userDetail = failureDetail;
                }

                QString errorMsg;
                if (userDetail.isEmpty()) {
                    errorMsg = tr("ASIO 驱动初始化失败\n\n"
                                  "建议操作：\n"
                                  "1. 关闭其他音频应用\n"
                                  "2. 重新播放\n"
                                  "3. 如果问题持续，切换到 WASAPI 模式");
                } else {
                    errorMsg = tr("ASIO 驱动初始化失败\n\n"
                                  "技术详情：%1\n\n"
                                  "建议操作：\n"
                                  "1. 关闭其他音频应用\n"
                                  "2. 重新播放\n"
                                  "3. 如果问题持续，切换到 WASAPI 模式")
                                   .arg(userDetail);
                }
                emit errorOccurred(errorCode, errorMsg);
            }
            return;
        }
        if (preparedOutputFormat.isValid() && preparedOutputFormat != m_outputFormat) {
            PlayerLogger::log(QStringLiteral("player"),
                              QStringLiteral("asio outputFormat resolved requestedRate=%1 actualRate=%2 channels=%3 sampleFormat=%4")
                                  .arg(selectedFormat.sampleRate())
                                  .arg(preparedOutputFormat.sampleRate())
                                  .arg(preparedOutputFormat.channelCount())
                                  .arg(static_cast<int>(preparedOutputFormat.sampleFormat())));
            m_outputFormat = preparedOutputFormat;
            const qsizetype resolvedBytesPerHalfSecond = qMax<qsizetype>(
                65536,
                static_cast<qsizetype>(m_outputFormat.bytesPerFrame())
                    * m_outputFormat.sampleRate() / 2);
            buffer->setMaxSize(resolvedBytesPerHalfSecond * 6);
        }
    }
    emit outputFormatChanged(m_outputDeviceDescription, m_outputFormat);

    const QString driverIdText = QString::fromLatin1(m_activeOutputDeviceId);
    const bool isCreativeDevice = isLikelyCreativeDriverId(driverIdText);
    const bool shouldApplyReorder =
        (m_creativeChannelReorderMode == 2)
        || (m_creativeChannelReorderMode == 0 && isCreativeDevice);

    QStringList arguments;
    if (!useLibavDecoder) {
        arguments = {
            QStringLiteral("-nostdin"),
            QStringLiteral("-v"),
            QStringLiteral("error"),
            QStringLiteral("-re"),
            QStringLiteral("-readrate_initial_burst"),
            QString::number(kAsioFfmpegInitialBurstSeconds, 'f', 3),
        };
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("asio decoderReadRate realtime=1 initialBurstSeconds=%1")
                              .arg(kAsioFfmpegInitialBurstSeconds, 0, 'f', 3));

        const QString rawInputFormat = rawInputFormatForSource();
        if (!rawInputFormat.isEmpty()) {
            arguments << QStringLiteral("-f") << rawInputFormat;
        }
        if (startPositionMs > 0) {
            arguments << QStringLiteral("-ss") << formatSeekSeconds(startPositionMs);
        }

        const QString targetChannelLayout = channelLayoutForCount(m_outputFormat.channelCount());
        QStringList filters {
            QStringLiteral("aresample=%1").arg(decoderQFormat.sampleRate()),
            QStringLiteral("aformat=sample_rates=%1:sample_fmts=%2")
                .arg(decoderQFormat.sampleRate())
                .arg(AsioFormats::pcmSampleFormatName(decoderQFormat.sampleFormat())),
        };
        if (!targetChannelLayout.isEmpty()) {
            filters.last().append(QStringLiteral(":channel_layouts=%1").arg(targetChannelLayout));
        }

        if (shouldApplyReorder && !targetChannelLayout.isEmpty()) {
            const int ch = m_outputFormat.channelCount();
            if (ch == 8) {
                const QString panFilter =
                    QStringLiteral("pan=%1|c0=c0|c1=c1|c2=c4|c3=c5|c4=c2|c5=c3|c6=c6|c7=c7")
                        .arg(targetChannelLayout);
                filters << panFilter;
                PlayerLogger::log(QStringLiteral("player"),
                                  QStringLiteral("asio creativeChannelOrderFilter session=%1 device=%2 filter=%3")
                                      .arg(m_activeDecoderSessionId)
                                      .arg(m_outputDeviceDescription)
                                      .arg(panFilter));
            } else if (ch == 6) {
                const QString panFilter =
                    QStringLiteral("pan=%1|c0=c0|c1=c1|c2=c4|c3=c5|c4=c2|c5=c3")
                        .arg(targetChannelLayout);
                filters << panFilter;
                PlayerLogger::log(QStringLiteral("player"),
                                  QStringLiteral("asio creativeChannelOrderFilter session=%1 device=%2 filter=%3")
                                      .arg(m_activeDecoderSessionId)
                                      .arg(m_outputDeviceDescription)
                                      .arg(panFilter));
            }
        }

        arguments << QStringLiteral("-i") << m_sourcePath
                  << QStringLiteral("-map") << QStringLiteral("0:a:0")
                  << QStringLiteral("-vn")
                  << QStringLiteral("-sn")
                  << QStringLiteral("-dn")
                  << QStringLiteral("-af") << filters.join(',')
                  << QStringLiteral("-ac") << QString::number(decoderQFormat.channelCount())
                  << QStringLiteral("-ar") << QString::number(decoderQFormat.sampleRate())
                  << QStringLiteral("-acodec") << AsioFormats::pcmCodecName(decoderQFormat.sampleFormat())
                  << QStringLiteral("-f") << AsioFormats::pcmMuxerName(decoderQFormat.sampleFormat())
                  << QStringLiteral("-");
    }

    m_stderrBuffer.clear();
    m_decoderFinished = false;
    m_currentPositionMs = startPositionMs;
    m_startPositionMs = startPositionMs;
    m_lastLoggedPositionBucket = startPositionMs / 1000;
    emit positionChanged(startPositionMs);
    setPlaybackState(PlaybackState::Playing);
    const int sessionId = m_activeDecoderSessionId;
    const PcmStreamFormat libavDecoderFormat = decoderPcmFormat;
    if (useLibavDecoder && m_libavSeekDecoderWorker) {
        m_libavSeekDecoderWorker->setCreativeChannelReorderEnabled(shouldApplyReorder);
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("asio startPipeline decoder-start mode=libav-inprocess session=%1 startPositionMs=%2 bufferMaxSize=%3 sourceCodec=%4 creativeReorder=%5")
                              .arg(sessionId)
                              .arg(startPositionMs)
                              .arg(buffer->maxSize())
                              .arg(m_sourceCodecName)
                              .arg(shouldApplyReorder ? 1 : 0));
        QMetaObject::invokeMethod(m_libavSeekDecoderWorker,
                                  [worker = m_libavSeekDecoderWorker,
                                   sessionId,
                                   sourcePath = m_sourcePath,
                                   startPositionMs,
                                   buffer,
                                   libavDecoderFormat] {
                                      worker->startDecoding(sessionId,
                                                           sourcePath,
                                                           startPositionMs,
                                                           buffer,
                                                           libavDecoderFormat);
                                  },
                                  Qt::QueuedConnection);
    } else if (m_decoderWorker) {
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("asio startPipeline decoder-start mode=ffmpeg-cli session=%1 startPositionMs=%2 bufferMaxSize=%3 sourceCodec=%4")
                              .arg(sessionId)
                              .arg(startPositionMs)
                              .arg(buffer->maxSize())
                              .arg(m_sourceCodecName));
        QMetaObject::invokeMethod(m_decoderWorker,
                                  [worker = m_decoderWorker,
                                   sessionId,
                                   ffmpegExecutable,
                                   arguments,
                                   buffer,
                                   libavDecoderFormat] {
                                      worker->startDecoding(sessionId,
                                                           ffmpegExecutable,
                                                           arguments,
                                                           buffer,
                                                           libavDecoderFormat);
                                  },
                                  Qt::QueuedConnection);
    }
    emit statusMessage(tr("ASIO 正在启动播放…"));
}

void WindowsAsioAudioPlayer::stopDecoderWorker(bool waitForFinished)
{
    if ((!m_decoderWorker && !m_libavSeekDecoderWorker)
        || (!m_activeDecoderSessionId && !m_stoppingDecoderSessionId)) {
        return;
    }
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
}

void WindowsAsioAudioPlayer::teardownPipeline()
{
    if (m_tearingDown) {
        return;
    }
    m_tearingDown = true;
    m_audioStarted = false;
    m_decoderFinished = false;
    releaseOutputResources();
    stopDecoderWorker(true);
    clearBufferDevice();
    m_activeDecoderSessionId = 0;
    m_stoppingDecoderSessionId = 0;
    m_activeOutputDeviceId.clear();
    m_outputFormat = {};
    m_outputDeviceDescription.clear();
    m_stderrBuffer.clear();
    m_tearingDown = false;
    m_outputRetainedAfterCompletion = false;
    emitAudioLevels(0.0, 0.0);
}

void WindowsAsioAudioPlayer::emitOutputDeviceSelectionChanged()
{
    const AudioOutputDeviceInfo device = resolveOutputDevice();
    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("asio outputDeviceSelectionChanged usesDefault=%1 id=%2 description=%3")
                          .arg(m_selectedOutputDeviceId.isEmpty())
                          .arg(QString::fromUtf8(device.id))
                          .arg(device.description));
    emit outputDeviceSelectionChanged();
}

AudioOutputDeviceInfo WindowsAsioAudioPlayer::resolveOutputDevice(bool *usesDefault) const
{
    const QList<AudioOutputDeviceInfo> devices = availableOutputDeviceInfos();
    if (!m_selectedOutputDeviceId.isEmpty()) {
        const auto foundIt = std::find_if(devices.cbegin(), devices.cend(), [this](const AudioOutputDeviceInfo &device) {
            return device.id == m_selectedOutputDeviceId;
        });
        if (foundIt != devices.cend()) {
            if (usesDefault) {
                *usesDefault = false;
            }
            return *foundIt;
        }
    }

    if (usesDefault) {
        *usesDefault = true;
    }
    const auto verifiedIt = std::find_if(devices.cbegin(), devices.cend(), [](const AudioOutputDeviceInfo &device) {
        return device.transport != QStringLiteral("ASIO-unverified");
    });
    if (verifiedIt != devices.cend()) {
        return *verifiedIt;
    }
    return devices.isEmpty() ? AudioOutputDeviceInfo {} : devices.constFirst();
}

QAudioFormat WindowsAsioAudioPlayer::selectOutputFormat(const AudioOutputDeviceInfo &device, QString *deviceDescription) const
{
    if (deviceDescription) {
        *deviceDescription = device.isNull()
            ? tr("未找到 ASIO 输出设备")
            : (m_selectedOutputDeviceId.isEmpty()
                   ? tr("默认 ASIO：%1").arg(device.description)
                   : tr("ASIO：%1").arg(device.description));
    }

    QAudioFormat format = device.preferredFormat;
    if (!format.isValid()) {
        format.setSampleRate(48000);
        format.setChannelCount(2);
        format.setSampleFormat(QAudioFormat::Float);
    }
    if (m_sourceChannelCount > 0 && m_sourceChannelCount < format.channelCount()) {
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("asio preserve-driver-channel-count sourceChannels=%1 outputChannels=%2 device=%3")
                              .arg(m_sourceChannelCount)
                              .arg(format.channelCount())
                              .arg(device.description));
    }
    if (format.channelCount() <= 0) {
        format.setChannelCount(2);
    }
    if (m_exactPlaybackEnabled && m_sourceSampleRate > 0) {
        if (format.sampleRate() != m_sourceSampleRate) {
            PlayerLogger::log(QStringLiteral("player"),
                              QStringLiteral("asio source-sample-rate-request sourceRate=%1 previousRate=%2 device=%3")
                                  .arg(m_sourceSampleRate)
                                  .arg(format.sampleRate())
                                  .arg(device.description));
        }
        format.setSampleRate(m_sourceSampleRate);
    } else if (format.sampleRate() <= 0) {
        format.setSampleRate(48000);
    }
    if (format.sampleFormat() == QAudioFormat::Unknown) {
        if (m_sourceBitDepth > 0 && m_sourceBitDepth <= 16) {
            format.setSampleFormat(QAudioFormat::Int16);
            PlayerLogger::log(QStringLiteral("player"),
                              QStringLiteral("asio source-bit-depth-preference sourceBitDepth=%1 format=Int16 device=%2")
                                  .arg(m_sourceBitDepth)
                                  .arg(device.description));
        } else {
            format.setSampleFormat(QAudioFormat::Float);
        }
    }
    return format;
}

#include "windowsasioaudioplayer.moc"
