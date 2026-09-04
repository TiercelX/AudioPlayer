#include "windowsasioaudioplayer_worker.h"

#include <QDateTime>

AsioOutputWorker *g_callbackWorker = nullptr;
QMutex g_callbackWorkerMutex;

AsioOutputWorker::AsioOutputWorker(QObject *parent)
    : QObject(parent)
    , m_positionTimer(new QTimer(this))
    , m_callbackWatchdog(new QTimer(this))
    , m_deviceHealthTimer(new QTimer(this))
{
    m_positionTimer->setInterval(100);
    connect(m_positionTimer, &QTimer::timeout, this, [this] {
        if (m_resetRequestCount.load(std::memory_order_acquire) > 0) {
            handleDriverResetRequest();
            return;
        }
        if (!m_started || m_sessionId == 0 || !m_outputFormat.isValid()) {
            return;
        }
        emit positionUpdated(m_sessionId,
                             m_startedAtPositionMs
                                 + static_cast<qint64>(m_renderedFrames * 1000 / m_outputFormat.sampleRate()));
    });

    m_callbackWatchdog->setSingleShot(true);
    m_callbackWatchdog->setInterval(1500);
    connect(m_callbackWatchdog, &QTimer::timeout, this, [this] {
        if (!m_started || m_callbackCount > 0) {
            return;
        }
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("asio callbackWatchdog timeout session=%1 driverId=%2 — no buffer switch callback received")
                              .arg(m_sessionId)
                              .arg(QString::fromUtf8(m_driverId)));
        handleCallbackWatchdogRecovery();
    });

    // Device health check timer for detecting disconnection during pause
    m_deviceHealthTimer->setInterval(2000);
    connect(m_deviceHealthTimer, &QTimer::timeout, this, [this] {
        if (!m_driver || m_started) {
            return;
        }
        // Probe driver availability during pause state
        bool crashed = false;
        ASIOSampleRate sampleRate;
        AsioUtils::safeAsioGetSampleRate(m_driver, &sampleRate, &crashed);
        if (crashed) {
            PlayerLogger::log(QStringLiteral("audio"),
                              QStringLiteral("asio deviceHealthCheck failed session=%1 — device disconnected").arg(m_sessionId));
            m_deviceHealthTimer->stop();
            m_driver = nullptr;
            m_lastOpenFailureReason = DriverOpenFailureReason::DriverError;
            m_lastOpenFailureDetail = QStringLiteral("Device disconnected during pause (health check failed)");
            emit stateChanged(m_sessionId, static_cast<int>(QAudio::StoppedState), static_cast<int>(QtAudio::OpenError));
        }
    });
}

AsioOutputWorker::~AsioOutputWorker()
{
    releaseOutput(0, true);
    if (m_comInitialized) {
        CoUninitialize();
    }
}

void AsioOutputWorker::configureOutput(int sessionId,
                                       const QByteArray &driverId,
                                       const QAudioFormat &format,
                                       qreal volume,
                                       qint64 startPositionMs,
                                       int sourceBitDepth,
                                       const QAudioFormat &decoderFormat)
{
    PlayerLogger::log(QStringLiteral("audio"),
                      QStringLiteral("asio configureOutput session=%1 driverId=%2 rate=%3 channels=%4 sampleFormat=%5 startPositionMs=%6 sourceBitDepth=%7 decoderFormat=%8")
                          .arg(sessionId)
                          .arg(QString::fromUtf8(driverId))
                          .arg(format.sampleRate())
                          .arg(format.channelCount())
                          .arg(static_cast<int>(format.sampleFormat()))
                          .arg(startPositionMs)
                          .arg(sourceBitDepth)
                          .arg(decoderFormat.isValid() ? static_cast<int>(decoderFormat.sampleFormat()) : -1));
    const bool canReusePreparedOutput = m_driver
        && m_buffersCreated
        && !m_started
        && !m_forceDriverRelease
        && m_driverId == driverId
        && m_outputFormat == format;
    m_forceDriverRelease = false;
    m_callbackWatchdogRecoveryCount = 0;
    resetRecoveryStatus();
    if (canReusePreparedOutput) {
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("asio configureOutput reuse-prepared-driver previousSession=%1 nextSession=%2")
                              .arg(m_sessionId)
                              .arg(sessionId));
    } else {
        releaseOutput(0, true);
    }

    m_sessionId = sessionId;
    m_driverId = driverId;
    m_outputFormat = format;
    m_decoderFormat = decoderFormat.isValid() ? decoderFormat : format;
    m_volume = volume;
    m_sourceBitDepth = sourceBitDepth;
    m_startedAtPositionMs = startPositionMs;
    m_renderedFrames = 0;
    m_completionPosted = false;
    m_callbackCount = 0;
    startArtifactMonitoring();
}

bool AsioOutputWorker::prepareOutput(int sessionId)
{
    if (!m_outputFormat.isValid() || sessionId != m_sessionId) {
        return false;
    }
    if (m_driver && m_buffersCreated) {
        return true;
    }
    return openDriver();
}

QAudioFormat AsioOutputWorker::preparedOutputFormat() const
{
    return m_outputFormat;
}

void AsioOutputWorker::startOutput(int sessionId, PcmStreamBuffer *buffer)
{
    if (!buffer || !m_outputFormat.isValid() || m_started || sessionId != m_sessionId) {
        return;
    }

    if (!m_driver && !openDriver()) {
        emit stateChanged(m_sessionId, static_cast<int>(QAudio::StoppedState), static_cast<int>(QtAudio::OpenError));
        return;
    }

    m_buffer = buffer;
    {
        QMutexLocker locker(&g_callbackWorkerMutex);
        g_callbackWorker = this;
    }

    bool asioCrashed = false;
    const ASIOError startResult = AsioUtils::safeAsioStart(m_driver, &asioCrashed);
    if (!AsioUtils::asioResultOk(startResult)) {
        m_lastOpenFailureDetail = QStringLiteral("driver->start() failed (result=%1, error=%2)")
            .arg(startResult).arg(AsioUtils::asioDriverError(m_driver));
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("asio start failed session=%1 result=%2 crashed=%3 error=%4")
                              .arg(m_sessionId)
                              .arg(startResult)
                              .arg(asioCrashed ? 1 : 0)
                              .arg(AsioUtils::asioDriverError(m_driver)));
        m_lastOpenFailureReason = classifyEndpointOpenFailure();
        emit stateChanged(m_sessionId, static_cast<int>(QAudio::StoppedState), static_cast<int>(QtAudio::OpenError));
        return;
    }

    m_started = true;
    m_callbackWatchdog->start();
    m_positionTimer->start();
    emit stateChanged(m_sessionId, static_cast<int>(QAudio::ActiveState), static_cast<int>(QtAudio::NoError));
}

void AsioOutputWorker::finishOutput(int sessionId)
{
    if (sessionId != m_sessionId) {
        return;
    }

    PlayerLogger::log(QStringLiteral("audio"),
                      QStringLiteral("asio finishOutput begin session=%1 started=%2 buffersCreated=%3")
                          .arg(m_sessionId)
                          .arg(m_started ? 1 : 0)
                          .arg(m_buffersCreated ? 1 : 0));
    m_positionTimer->stop();
    m_callbackWatchdog->stop();
    {
        QMutexLocker locker(&g_callbackWorkerMutex);
        if (g_callbackWorker == this) {
            g_callbackWorker = nullptr;
        }
    }
    if (m_driver && m_started) {
        bool asioCrashed = false;
        AsioUtils::safeAsioStop(m_driver, &asioCrashed);
        if (asioCrashed) {
            PlayerLogger::log(QStringLiteral("audio"),
                              QStringLiteral("asio finishOutput stop crashed session=%1").arg(m_sessionId));
        }
    }
    finishArtifactMonitoring();
    m_started = false;
    m_buffer = nullptr;
    m_completionPosted = false;
    m_callbackCount = 0;
    PlayerLogger::log(QStringLiteral("audio"),
                      QStringLiteral("asio finishOutput done session=%1 driverRetained=%2 buffersRetained=%3")
                          .arg(m_sessionId)
                          .arg(m_driver ? 1 : 0)
                          .arg(m_buffersCreated ? 1 : 0));
}

void AsioOutputWorker::pauseOutput(int sessionId, int generation)
{
    if (!m_driver || !m_started || sessionId != m_sessionId) {
        return;
    }
    if (generation < m_pauseResumeGeneration) {
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("asio pauseOutput stale generation=%1 current=%2")
                              .arg(generation)
                              .arg(m_pauseResumeGeneration));
        return;
    }
    m_pauseResumeGeneration = generation;
    bool asioCrashed = false;
    AsioUtils::safeAsioStop(m_driver, &asioCrashed);
    if (asioCrashed) {
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("asio pause stop crashed session=%1 — releasing driver").arg(m_sessionId));
        // Driver crashed, release resources
        if (m_buffersCreated) {
            AsioUtils::safeAsioDisposeBuffers(m_driver, &asioCrashed);
        }
        m_buffersCreated = false;
        m_bufferInfos.clear();
        m_channelInfos.clear();
        if (m_driver) {
            AsioDiscovery::safeAsioRelease(m_driver);
            m_driver = nullptr;
        }
    }
    m_started = false;
    m_positionTimer->stop();
    emit positionUpdated(m_sessionId,
                         m_startedAtPositionMs
                             + static_cast<qint64>(m_renderedFrames * 1000 / m_outputFormat.sampleRate()));
    emit stateChanged(m_sessionId, static_cast<int>(QAudio::SuspendedState), static_cast<int>(QtAudio::NoError));
    // Start device health check timer during pause
    if (m_driver) {
        m_deviceHealthTimer->start();
    }
}

void AsioOutputWorker::resumeOutput(int sessionId, int generation)
{
    m_deviceHealthTimer->stop();
    if (!m_driver || m_started || sessionId != m_sessionId) {
        if (!m_driver && sessionId == m_sessionId) {
            // Driver was released (e.g., device disconnected during pause)
            m_lastOpenFailureReason = DriverOpenFailureReason::DriverError;
            m_lastOpenFailureDetail = QStringLiteral("Driver released during pause (device disconnected?)");
            emit stateChanged(sessionId, static_cast<int>(QAudio::StoppedState), static_cast<int>(QtAudio::OpenError));
        }
        return;
    }
    if (generation < m_pauseResumeGeneration) {
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("asio resumeOutput stale generation=%1 current=%2")
                              .arg(generation)
                              .arg(m_pauseResumeGeneration));
        return;
    }
    m_pauseResumeGeneration = generation;

    // Probe driver availability before attempting resume
    bool probeCrashed = false;
    ASIOSampleRate probeSampleRate;
    AsioUtils::safeAsioGetSampleRate(m_driver, &probeSampleRate, &probeCrashed);
    if (probeCrashed) {
        // Driver is no longer available (device disconnected during pause)
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("asio resumeOutput probe crashed session=%1 — device likely disconnected").arg(m_sessionId));
        m_driver = nullptr;
        m_lastOpenFailureReason = DriverOpenFailureReason::DriverError;
        m_lastOpenFailureDetail = QStringLiteral("Device disconnected during pause (probe failed)");
        emit stateChanged(sessionId, static_cast<int>(QAudio::StoppedState), static_cast<int>(QtAudio::OpenError));
        return;
    }

    // Dispose and recreate ASIO buffers to force the driver to flush
    // any internal DMA caches. Some drivers (e.g. Creative) retain
    // stale audio in internal buffers even after zeroing the
    // application-side pointers. Disposing buffers is a full reset
    // of the driver's audio state without releasing the COM object.
    bool asioCrashed = false;
    AsioUtils::safeAsioStop(m_driver, &asioCrashed);
    if (asioCrashed) {
        // Driver crashed during stop, release resources and report error
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("asio resumeOutput stop crashed session=%1").arg(m_sessionId));
        m_driver = nullptr;
        m_lastOpenFailureReason = DriverOpenFailureReason::DriverError;
        m_lastOpenFailureDetail = QStringLiteral("Driver crashed during resume stop");
        emit stateChanged(sessionId, static_cast<int>(QAudio::StoppedState), static_cast<int>(QtAudio::OpenError));
        return;
    }
    if (m_buffersCreated) {
        AsioUtils::safeAsioDisposeBuffers(m_driver, &asioCrashed);
        if (asioCrashed) {
            // Driver crashed during dispose, release resources and report error
            PlayerLogger::log(QStringLiteral("audio"),
                              QStringLiteral("asio resumeOutput dispose crashed session=%1").arg(m_sessionId));
            m_driver = nullptr;
            m_buffersCreated = false;
            m_lastOpenFailureReason = DriverOpenFailureReason::DriverError;
            m_lastOpenFailureDetail = QStringLiteral("Driver crashed during resume dispose");
            emit stateChanged(sessionId, static_cast<int>(QAudio::StoppedState), static_cast<int>(QtAudio::OpenError));
            return;
        }
        m_buffersCreated = false;
    }
    const long channelCount = m_outputFormat.channelCount();
    m_bufferInfos.resize(channelCount);
    m_channelInfos.resize(channelCount);
    for (int ch = 0; ch < channelCount; ++ch) {
        m_bufferInfos[ch].channelNum = ch;
        m_bufferInfos[ch].isInput = kAsioFalse;
        m_bufferInfos[ch].buffers[0] = nullptr;
        m_bufferInfos[ch].buffers[1] = nullptr;
    }
    const ASIOError createResult =
        AsioUtils::safeAsioCreateBuffers(m_driver, m_bufferInfos.data(), channelCount, m_bufferSize, &m_callbacks, &asioCrashed);
    if (!AsioUtils::asioResultOk(createResult)) {
        m_lastOpenFailureDetail = QStringLiteral("resumeOutput: createBuffers failed (result=%1)")
            .arg(createResult);
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("asio resumeOutput recreateBuffers failed session=%1 result=%2 crashed=%3")
                              .arg(m_sessionId)
                              .arg(createResult)
                              .arg(asioCrashed ? 1 : 0));
        m_lastOpenFailureReason = classifyEndpointOpenFailure();
        emit stateChanged(m_sessionId, static_cast<int>(QAudio::StoppedState), static_cast<int>(QtAudio::OpenError));
        return;
    }
    m_buffersCreated = true;
    for (int ch = 0; ch < channelCount; ++ch) {
        m_channelInfos[ch].channel = ch;
        m_channelInfos[ch].isInput = kAsioFalse;
        AsioUtils::safeAsioGetChannelInfo(m_driver, &m_channelInfos[ch], &asioCrashed);
    }
    PlayerLogger::log(QStringLiteral("audio"),
                      QStringLiteral("asio resumeOutput buffers recreated session=%1 channels=%2 bufferSize=%3")
                          .arg(m_sessionId)
                          .arg(channelCount)
                          .arg(m_bufferSize));
    const ASIOError startResult = AsioUtils::safeAsioStart(m_driver, &asioCrashed);
    if (!AsioUtils::asioResultOk(startResult)) {
        m_lastOpenFailureDetail = QStringLiteral("resumeOutput: driver->start() failed (result=%1, error=%2)")
            .arg(startResult).arg(AsioUtils::asioDriverError(m_driver));
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("asio resume failed session=%1 result=%2 crashed=%3 error=%4")
                              .arg(m_sessionId)
                              .arg(startResult)
                              .arg(asioCrashed ? 1 : 0)
                              .arg(AsioUtils::asioDriverError(m_driver)));
        m_lastOpenFailureReason = classifyEndpointOpenFailure();
        emit stateChanged(m_sessionId, static_cast<int>(QAudio::StoppedState), static_cast<int>(QtAudio::OpenError));
        return;
    }
    m_started = true;
    m_positionTimer->start();
    emit stateChanged(m_sessionId, static_cast<int>(QAudio::ActiveState), static_cast<int>(QtAudio::NoError));
}

void AsioOutputWorker::forceReleaseDriver()
{
    PlayerLogger::log(QStringLiteral("audio"),
                      QStringLiteral("asio forceReleaseDriver session=%1")
                          .arg(m_sessionId));
    m_positionTimer->stop();
    m_callbackWatchdog->stop();
    finishArtifactMonitoring();
    {
        QMutexLocker locker(&g_callbackWorkerMutex);
        if (g_callbackWorker == this) {
            g_callbackWorker = nullptr;
        }
    }
    if (m_driver && m_started) {
        bool asioCrashed = false;
        AsioUtils::safeAsioStop(m_driver, &asioCrashed);
    }
    m_started = false;
    if (m_driver && m_buffersCreated) {
        bool asioCrashed = false;
        AsioUtils::safeAsioDisposeBuffers(m_driver, &asioCrashed);
    }
    m_buffersCreated = false;
    m_bufferInfos.clear();
    m_channelInfos.clear();
    if (m_driver) {
        HMODULE driverModule = nullptr;
        GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(m_driver),
                          &driverModule);
        AsioDiscovery::safeAsioRelease(m_driver);
        m_driver = nullptr;
        if (driverModule) {
            FreeLibrary(driverModule);
        }
    }
    if (m_comInitialized) {
        CoFreeUnusedLibraries();
        CoUninitialize();
        m_comInitialized = false;
    }
    m_callbackCount = 0;
    m_callbackWatchdogRecoveryCount = 0;
    m_callbackWatchdogRecoveryStartMs = -1;
    m_recoveryPendingFirstBufferSwitch = false;
    m_lastReleaseTimer.restart();
    m_forceDriverRelease = false;
}

void AsioOutputWorker::startRecoveryStatus()
{
    if (m_callbackWatchdogRecoveryStartMs < 0) {
        m_callbackWatchdogRecoveryStartMs = QDateTime::currentMSecsSinceEpoch();
    }
    m_recoveryPendingFirstBufferSwitch = true;
}

qint64 AsioOutputWorker::recoveryElapsedMs() const
{
    if (m_callbackWatchdogRecoveryStartMs < 0) {
        return 0;
    }
    return qMax<qint64>(0, QDateTime::currentMSecsSinceEpoch() - m_callbackWatchdogRecoveryStartMs);
}

void AsioOutputWorker::emitRecoveryStatus()
{
    const qint64 elapsedMs = recoveryElapsedMs();
    emit statusMessage(tr("\346\255\243\345\234\250\346\201\242\345\244\215 ASIO \346\222\255\346\224\276\357\274\210%1s/%2s\357\274\211")
                           .arg(static_cast<int>(elapsedMs / 1000))
                           .arg(kAsioBusyRetryTimeoutMs / 1000));
}

void AsioOutputWorker::resetRecoveryStatus()
{
    m_callbackWatchdogRecoveryCount = 0;
    m_callbackWatchdogRecoveryStartMs = -1;
    m_recoveryPendingFirstBufferSwitch = false;
}

void AsioOutputWorker::handleCallbackWatchdogRecovery()
{
    startRecoveryStatus();
    const qint64 elapsedMs = QDateTime::currentMSecsSinceEpoch() - m_callbackWatchdogRecoveryStartMs;
    emitRecoveryStatus();
    if (elapsedMs >= kAsioBusyRetryTimeoutMs) {
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("asio callbackWatchdogRecovery timeout session=%1 count=%2 elapsed=%3ms — giving up")
                              .arg(m_sessionId)
                              .arg(m_callbackWatchdogRecoveryCount)
                              .arg(elapsedMs));
        m_lastOpenFailureReason = DriverOpenFailureReason::RecoveryTimeout;
        m_lastOpenFailureDetail = QStringLiteral("Callback watchdog recovery timeout (%1s)")
            .arg(kAsioBusyRetryTimeoutMs / 1000);
        resetRecoveryStatus();
        if (m_driver && m_started) {
            bool asioCrashed = false;
            AsioUtils::safeAsioStop(m_driver, &asioCrashed);
        }
        m_started = false;
        m_forceDriverRelease = true;
        QMetaObject::invokeMethod(this, [this, sessionId = m_sessionId] {
            emit stateChanged(sessionId, static_cast<int>(QAudio::StoppedState), static_cast<int>(QtAudio::OpenError));
        }, Qt::QueuedConnection);
        return;
    }

    ++m_callbackWatchdogRecoveryCount;
    const int sessionId = m_sessionId;
    const qint64 currentPositionMs = m_startedAtPositionMs
        + (m_outputFormat.sampleRate() > 0
               ? static_cast<qint64>(m_renderedFrames * 1000 / m_outputFormat.sampleRate())
               : 0);

    PlayerLogger::log(QStringLiteral("audio"),
                      QStringLiteral("asio callbackWatchdogRecovery session=%1 driverId=%2 attempt=%3 — attempting driver reinit")
                          .arg(sessionId)
                          .arg(QString::fromUtf8(m_driverId))
                          .arg(m_callbackWatchdogRecoveryCount));

    // Stop ASIO streaming
    m_positionTimer->stop();
    m_callbackWatchdog->stop();
    finishArtifactMonitoring();
    {
        QMutexLocker locker(&g_callbackWorkerMutex);
        if (g_callbackWorker == this) {
            g_callbackWorker = nullptr;
        }
    }
    if (m_driver && m_started) {
        bool asioCrashed = false;
        AsioUtils::safeAsioStop(m_driver, &asioCrashed);
    }
    m_started = false;

    // Dispose buffers
    if (m_driver && m_buffersCreated) {
        bool asioCrashed = false;
        AsioUtils::safeAsioDisposeBuffers(m_driver, &asioCrashed);
    }
    m_buffersCreated = false;
    m_bufferInfos.clear();
    m_channelInfos.clear();

    // Release driver COM object and unload DLL
    if (m_driver) {
        HMODULE driverModule = nullptr;
        GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(m_driver),
                          &driverModule);
        AsioDiscovery::safeAsioRelease(m_driver);
        m_driver = nullptr;
        if (driverModule) {
            FreeLibrary(driverModule);
        }
    }
    if (m_comInitialized) {
        CoFreeUnusedLibraries();
        CoUninitialize();
        m_comInitialized = false;
    }
    m_callbackCount = 0;
    m_lastReleaseTimer.restart();
    m_forceDriverRelease = false;

    // Cooldown before reopen
    QThread::msleep(kAsioReleaseCooldownMs);

    // Retry openDriver in a loop until the device becomes available or timeout
    const qint64 reopenDeadline = m_callbackWatchdogRecoveryStartMs + kAsioBusyRetryTimeoutMs;
    bool opened = false;
    for (int reopenAttempt = 1; ; ++reopenAttempt) {
        opened = openDriver();
        if (opened) {
            break;
        }
        emitRecoveryStatus();
        if (QDateTime::currentMSecsSinceEpoch() >= reopenDeadline) {
            PlayerLogger::log(QStringLiteral("audio"),
                              QStringLiteral("asio callbackWatchdogRecovery reinit-timeout session=%1 attempts=%2 — emitting error")
                                  .arg(sessionId)
                                  .arg(reopenAttempt));
            break;
        }
        PlayerLogger::log(QStringLiteral("audio"),
                           QStringLiteral("asio callbackWatchdogRecovery reinit-retry session=%1 attempt=%2 — waiting %3ms")
                              .arg(sessionId)
                              .arg(reopenAttempt + 1)
                              .arg(kAsioBusyRetryIntervalMs));
        QThread::msleep(kAsioBusyRetryIntervalMs);
    }
    if (!opened) {
        m_lastOpenFailureReason = DriverOpenFailureReason::RecoveryTimeout;
        m_lastOpenFailureDetail = QStringLiteral("Driver reinit failed during callback watchdog recovery");
        resetRecoveryStatus();
        m_forceDriverRelease = true;
        QMetaObject::invokeMethod(this, [this, sessionId] {
            emit stateChanged(sessionId, static_cast<int>(QAudio::StoppedState), static_cast<int>(QtAudio::OpenError));
        }, Qt::QueuedConnection);
        return;
    }

    // Restart ASIO streaming
    {
        QMutexLocker locker(&g_callbackWorkerMutex);
        g_callbackWorker = this;
    }
    m_renderedFrames = 0;
    m_startedAtPositionMs = currentPositionMs;
    m_completionPosted = false;
    m_callbackCount = 0;

    bool asioCrashed = false;
    const ASIOError startResult = AsioUtils::safeAsioStart(m_driver, &asioCrashed);
    if (!AsioUtils::asioResultOk(startResult)) {
        m_lastOpenFailureDetail = QStringLiteral("driver->start() failed during recovery (result=%1)")
            .arg(startResult);
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("asio callbackWatchdogRecovery restart-failed session=%1 result=%2")
                              .arg(sessionId)
                              .arg(startResult));
        m_lastOpenFailureReason = DriverOpenFailureReason::DeviceBusy;
        resetRecoveryStatus();
        m_forceDriverRelease = true;
        QMetaObject::invokeMethod(this, [this, sessionId] {
            emit stateChanged(sessionId, static_cast<int>(QAudio::StoppedState), static_cast<int>(QtAudio::OpenError));
        }, Qt::QueuedConnection);
        return;
    }

    m_started = true;
    m_callbackWatchdog->start();
    m_positionTimer->start();
    PlayerLogger::log(QStringLiteral("audio"),
                      QStringLiteral("asio callbackWatchdogRecovery restart-pending-callback session=%1")
                          .arg(sessionId));
    QMetaObject::invokeMethod(this, [this, sessionId] {
        emit stateChanged(sessionId, static_cast<int>(QAudio::ActiveState), static_cast<int>(QtAudio::NoError));
    }, Qt::QueuedConnection);
}

void AsioOutputWorker::handleDriverResetRequest()
{
    m_resetRequestCount.store(0, std::memory_order_release);
    const int sessionId = m_sessionId;
    const qint64 currentPositionMs = m_startedAtPositionMs
        + (m_outputFormat.sampleRate() > 0
               ? static_cast<qint64>(m_renderedFrames * 1000 / m_outputFormat.sampleRate())
               : 0);

    PlayerLogger::log(QStringLiteral("audio"),
                      QStringLiteral("asio driverResetRequest session=%1 driverId=%2 — attempting reinit")
                          .arg(sessionId)
                          .arg(QString::fromUtf8(m_driverId)));
    startRecoveryStatus();
    emitRecoveryStatus();

    // Stop ASIO streaming
    m_positionTimer->stop();
    m_callbackWatchdog->stop();
    finishArtifactMonitoring();
    {
        QMutexLocker locker(&g_callbackWorkerMutex);
        if (g_callbackWorker == this) {
            g_callbackWorker = nullptr;
        }
    }
    if (m_driver && m_started) {
        bool asioCrashed = false;
        AsioUtils::safeAsioStop(m_driver, &asioCrashed);
    }
    m_started = false;

    // Dispose buffers
    if (m_driver && m_buffersCreated) {
        bool asioCrashed = false;
        AsioUtils::safeAsioDisposeBuffers(m_driver, &asioCrashed);
    }
    m_buffersCreated = false;
    m_bufferInfos.clear();
    m_channelInfos.clear();

    // Release driver COM object and unload DLL
    if (m_driver) {
        HMODULE driverModule = nullptr;
        GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(m_driver),
                          &driverModule);
        AsioDiscovery::safeAsioRelease(m_driver);
        m_driver = nullptr;
        if (driverModule) {
            FreeLibrary(driverModule);
        }
    }
    if (m_comInitialized) {
        CoFreeUnusedLibraries();
        CoUninitialize();
        m_comInitialized = false;
    }
    m_callbackCount = 0;
    m_lastReleaseTimer.restart();

    // Cooldown before reopen
    QThread::msleep(kAsioReleaseCooldownMs);

    // Retry openDriver in a loop until the device becomes available or timeout
    const qint64 reopenDeadline = m_callbackWatchdogRecoveryStartMs + kAsioBusyRetryTimeoutMs;
    bool opened = false;
    for (int reopenAttempt = 1; ; ++reopenAttempt) {
        opened = openDriver();
        if (opened) {
            break;
        }
        emitRecoveryStatus();
        if (QDateTime::currentMSecsSinceEpoch() >= reopenDeadline) {
            PlayerLogger::log(QStringLiteral("audio"),
                              QStringLiteral("asio driverResetRequest reinit-timeout session=%1 attempts=%2 — emitting error")
                                  .arg(sessionId)
                                  .arg(reopenAttempt));
            break;
        }
        PlayerLogger::log(QStringLiteral("audio"),
                           QStringLiteral("asio driverResetRequest reinit-retry session=%1 attempt=%2 — waiting %3ms")
                              .arg(sessionId)
                              .arg(reopenAttempt + 1)
                              .arg(kAsioBusyRetryIntervalMs));
        QThread::msleep(kAsioBusyRetryIntervalMs);
    }
    if (!opened) {
        m_lastOpenFailureReason = DriverOpenFailureReason::RecoveryTimeout;
        m_lastOpenFailureDetail = QStringLiteral("Driver reinit failed during reset request handling");
        resetRecoveryStatus();
        m_forceDriverRelease = true;
        QMetaObject::invokeMethod(this, [this, sessionId] {
            emit stateChanged(sessionId, static_cast<int>(QAudio::StoppedState), static_cast<int>(QtAudio::OpenError));
        }, Qt::QueuedConnection);
        return;
    }

    // Restart ASIO streaming
    {
        QMutexLocker locker(&g_callbackWorkerMutex);
        g_callbackWorker = this;
    }
    m_renderedFrames = 0;
    m_startedAtPositionMs = currentPositionMs;
    m_completionPosted = false;
    m_callbackCount = 0;

    bool asioCrashed = false;
    const ASIOError startResult = AsioUtils::safeAsioStart(m_driver, &asioCrashed);
    if (!AsioUtils::asioResultOk(startResult)) {
        m_lastOpenFailureDetail = QStringLiteral("driver->start() failed during reset request (result=%1)")
            .arg(startResult);
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("asio driverResetRequest restart-failed session=%1 result=%2")
                              .arg(sessionId)
                              .arg(startResult));
        resetRecoveryStatus();
        m_forceDriverRelease = true;
        QMetaObject::invokeMethod(this, [this, sessionId] {
            emit stateChanged(sessionId, static_cast<int>(QAudio::StoppedState), static_cast<int>(QtAudio::OpenError));
        }, Qt::QueuedConnection);
        return;
    }

    m_started = true;
    m_callbackWatchdog->start();
    m_positionTimer->start();
    PlayerLogger::log(QStringLiteral("audio"),
                      QStringLiteral("asio driverResetRequest restart-pending-callback session=%1")
                          .arg(sessionId));
    QMetaObject::invokeMethod(this, [this, sessionId] {
        emit stateChanged(sessionId, static_cast<int>(QAudio::ActiveState), static_cast<int>(QtAudio::NoError));
    }, Qt::QueuedConnection);
}

void AsioOutputWorker::releaseOutput(int sessionId, bool)
{
    if (sessionId != 0 && sessionId != m_sessionId) {
        return;
    }

    const int releasedSessionId = m_sessionId;
    const bool hadResources = m_driver || m_buffersCreated || m_sessionId != 0;
    PlayerLogger::log(QStringLiteral("audio"),
                      QStringLiteral("asio releaseOutput begin requestSession=%1 currentSession=%2")
                          .arg(sessionId)
                          .arg(releasedSessionId));
    m_positionTimer->stop();
    m_callbackWatchdog->stop();
    m_deviceHealthTimer->stop();
    finishArtifactMonitoring();
    {
        QMutexLocker locker(&g_callbackWorkerMutex);
        if (g_callbackWorker == this) {
            g_callbackWorker = nullptr;
        }
    }
    if (m_driver) {
        bool asioCrashed = false;
        AsioUtils::safeAsioStop(m_driver, &asioCrashed);
        if (asioCrashed) {
            PlayerLogger::log(QStringLiteral("audio"),
                              QStringLiteral("asio releaseOutput stop crashed session=%1").arg(m_sessionId));
        }
    }
    m_started = false;
    if (m_driver && m_buffersCreated) {
        bool asioCrashed = false;
        const ASIOError disposeResult = AsioUtils::safeAsioDisposeBuffers(m_driver, &asioCrashed);
        if (!AsioUtils::asioResultOk(disposeResult) || asioCrashed) {
            PlayerLogger::log(QStringLiteral("audio"),
                              QStringLiteral("asio disposeBuffers failed session=%1 result=%2 crashed=%3")
                                  .arg(m_sessionId)
                                  .arg(disposeResult)
                                  .arg(asioCrashed ? 1 : 0));
        }
    }
    m_buffersCreated = false;
    m_bufferInfos.clear();
    m_channelInfos.clear();
    if (m_driver) {
        HMODULE driverModule = nullptr;
        GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(m_driver),
                          &driverModule);
        AsioDiscovery::safeAsioRelease(m_driver);
        m_driver = nullptr;
        if (driverModule) {
            FreeLibrary(driverModule);
            PlayerLogger::log(QStringLiteral("audio"),
                              QStringLiteral("asio releaseOutput freed-driver-module"));
        }
    }
    if (m_comInitialized) {
        CoFreeUnusedLibraries();
        CoUninitialize();
        m_comInitialized = false;
    }
    m_buffer = nullptr;
    m_sessionId = 0;
    m_outputFormat = {};
    m_decoderFormat = {};
    m_driverId.clear();
    m_completionPosted = false;
    m_callbackCount = 0;
    resetRecoveryStatus();
    PlayerLogger::log(QStringLiteral("audio"),
                      QStringLiteral("asio releaseOutput done releasedSession=%1 comReset=1").arg(releasedSessionId));
    if (hadResources) {
        m_lastReleaseTimer.restart();
    }
    emit released(releasedSessionId);
}

AsioOutputWorker::DriverOpenFailureReason AsioOutputWorker::lastOpenFailureReason() const
{
    return m_lastOpenFailureReason;
}

QString AsioOutputWorker::lastOpenFailureDetail() const
{
    return m_lastOpenFailureDetail;
}

void AsioOutputWorker::notifyResetRequest()
{
    m_resetRequestCount.fetch_add(1, std::memory_order_acq_rel);
}

void AsioOutputWorker::setVolume(qreal volume)
{
    QMutexLocker locker(&m_renderMutex);
    m_volume = volume;
}

AsioOutputWorker::DriverOpenFailureReason AsioOutputWorker::classifyEndpointOpenFailure() const
{
    return AsioSessionProbe::hasAnyExternalWasapiRenderSessionsForAsioDriver(m_driverId)
        ? DriverOpenFailureReason::DeviceBusy
        : DriverOpenFailureReason::DriverError;
}

bool AsioOutputWorker::openDriver()
{
    m_lastOpenFailureReason = DriverOpenFailureReason::None;
    m_lastOpenFailureDetail.clear();
    if (m_lastReleaseTimer.isValid()) {
        const qint64 elapsedMs = m_lastReleaseTimer.elapsed();
        if (elapsedMs >= 0 && elapsedMs < kAsioReleaseCooldownMs) {
            const unsigned long sleepMs = static_cast<unsigned long>(kAsioReleaseCooldownMs - elapsedMs);
            PlayerLogger::log(QStringLiteral("audio"),
                              QStringLiteral("asio openDriver cooldown sleepMs=%1 elapsedMs=%2 driverId=%3")
                                  .arg(sleepMs)
                                  .arg(elapsedMs)
                                  .arg(QString::fromUtf8(m_driverId)));
            QThread::msleep(sleepMs);
        }
    }

    const QString driverIdText = QString::fromUtf8(m_driverId);
    auto failOpenDriver = [this](DriverOpenFailureReason reason = DriverOpenFailureReason::DriverError,
                                 const QString &detail = QString()) {
        bool cleanupCrashed = false;
        if (m_driver && m_buffersCreated) {
            AsioUtils::safeAsioDisposeBuffers(m_driver, &cleanupCrashed);
        }
        m_buffersCreated = false;
        m_bufferInfos.clear();
        m_channelInfos.clear();
        if (m_driver) {
            AsioDiscovery::safeAsioRelease(m_driver);
            m_driver = nullptr;
        }
        m_lastOpenFailureReason = reason;
        if (!detail.isEmpty()) {
            m_lastOpenFailureDetail = detail;
        }
        return false;
    };

    // Try both COM threading models — some drivers (e.g. Realtek) need
    // COINIT_MULTITHREADED while others (e.g. Creative) need COINIT_APARTMENTTHREADED.
    // When STA gets RPC_E_CHANGED_MODE (another library pre-initialized MTA on this
    // thread), uninitialize the MTA and retry STA. This is safe because the audio
    // worker thread is dedicated to ASIO and we own its COM lifecycle.
    const DWORD comModels[] = { COINIT_APARTMENTTHREADED, COINIT_MULTITHREADED };
    const char *comModelNames[] = { "STA", "MTA" };
    constexpr int comModelCount = 2;

    bool asioCrashed = false;
    QString staFailureDetail;
    for (int comIndex = 0; comIndex < comModelCount; ++comIndex) {
        if (!m_comInitialized) {
            HRESULT hr = CoInitializeEx(nullptr, comModels[comIndex]);
            if (hr == RPC_E_CHANGED_MODE) {
                // COM already initialized with a different model on this thread.
                // For STA: try to uninitialize the existing MTA and retry STA.
                // For MTA: the existing STA is fine for MTA drivers, but
                // CoCreateInstance will handle it.
                if (comModels[comIndex] == COINIT_APARTMENTTHREADED) {
                    PlayerLogger::log(QStringLiteral("audio"),
                                      QStringLiteral("asio openDriver COM model=STA rpc-changed-mode — uninitializing existing MTA and retrying STA"));
                    CoUninitialize();
                    hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                    if (hr == RPC_E_CHANGED_MODE) {
                        PlayerLogger::log(QStringLiteral("audio"),
                                          QStringLiteral("asio openDriver COM model=STA retry still rpc-changed-mode — skipping"));
                        continue;
                    }
                } else {
                    // MTA requested but STA is active — STA works for MTA drivers too,
                    // but CoCreateInstance may fail. Log and continue.
                    PlayerLogger::log(QStringLiteral("audio"),
                                      QStringLiteral("asio openDriver COM model=%1 rpc-changed-mode — skipping")
                                          .arg(QString::fromLatin1(comModelNames[comIndex])));
                    continue;
                }
            }
            m_comInitialized = SUCCEEDED(hr);
            if (!m_comInitialized) {
                m_lastOpenFailureDetail = QStringLiteral("COM init failed (model=%1, hr=0x%2)")
                    .arg(QString::fromLatin1(comModelNames[comIndex]))
                    .arg(static_cast<qulonglong>(hr), 8, 16, QLatin1Char('0'));
                PlayerLogger::log(QStringLiteral("audio"),
                                  QStringLiteral("asio COM init failed model=%1 hr=%2")
                                      .arg(QString::fromLatin1(comModelNames[comIndex]))
                                      .arg(static_cast<qulonglong>(hr), 8, 16, QLatin1Char('0')));
                return false;
            }
            PlayerLogger::log(QStringLiteral("audio"),
                              QStringLiteral("asio openDriver COM model=%1 initialized")
                                  .arg(QString::fromLatin1(comModelNames[comIndex])));
        }

        const QList<AsioDiscovery::AsioHostWindowCandidate> hostCandidates = AsioDiscovery::asioHostWindowCandidates();
        for (int attempt = 1; attempt <= kAsioOpenRetryCount && !m_driver; ++attempt) {
            for (const AsioDiscovery::AsioHostWindowCandidate &candidate : hostCandidates) {
                m_driver = AsioDiscovery::createAsioDriver(driverIdText);
                if (!m_driver) {
                    m_lastOpenFailureDetail = QStringLiteral("CoCreateInstance failed for ASIO driver");
                    PlayerLogger::log(QStringLiteral("audio"),
                                      QStringLiteral("asio openDriver failed create driverId=%1 attempt=%2 comModel=%3")
                                          .arg(driverIdText)
                                          .arg(attempt)
                                          .arg(QString::fromLatin1(comModelNames[comIndex])));
                    // CoCreateInstance failed with this COM model — try next model
                    break;
                }

                const ASIOError initResult = AsioDiscovery::safeAsioInit(m_driver, candidate.window, &asioCrashed);
                if (initResult == kAsioTrue) {
                    PlayerLogger::log(QStringLiteral("audio"),
                                      QStringLiteral("asio openDriver init succeeded driverId=%1 hostKind=%2 hostWindow=%3 attempt=%4 comModel=%5")
                                          .arg(driverIdText,
                                               candidate.name,
                                               AsioUtils::hwndText(candidate.window))
                                          .arg(attempt)
                                          .arg(QString::fromLatin1(comModelNames[comIndex])));
                    break;
                }

                const QString driverError = AsioUtils::asioDriverError(m_driver);
                PlayerLogger::log(QStringLiteral("audio"),
                                  QStringLiteral("asio openDriver init attempt-failed driverId=%1 hostKind=%2 hostWindow=%3 attempt=%4 crashed=%5 initResult=%6 error=%7 comModel=%8")
                                      .arg(driverIdText,
                                           candidate.name,
                                           AsioUtils::hwndText(candidate.window))
                                      .arg(attempt)
                                      .arg(asioCrashed ? 1 : 0)
                                      .arg(initResult)
                                      .arg(driverError.isEmpty() ? QStringLiteral("(empty)") : driverError)
                                      .arg(QString::fromLatin1(comModelNames[comIndex])));
                AsioDiscovery::safeAsioRelease(m_driver);
                m_driver = nullptr;
            }

            if (!m_driver && attempt < kAsioOpenRetryCount) {
                const int delayMs = kAsioOpenRetryBaseDelayMs * attempt;
                PlayerLogger::log(QStringLiteral("audio"),
                                  QStringLiteral("asio openDriver retry-delay driverId=%1 attempt=%2 delayMs=%3")
                                      .arg(driverIdText)
                                      .arg(attempt)
                                      .arg(delayMs));
                QThread::msleep(static_cast<unsigned long>(delayMs));
            }
        }

        if (m_driver) {
            // Success with this COM model — stop trying other models.
            break;
        }

        // All host windows failed with this COM model. If we have another
        // model to try, release COM and loop. Preserve the first failure detail.
        if (staFailureDetail.isEmpty()) {
            staFailureDetail = m_lastOpenFailureDetail;
        }
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("asio openDriver init failed all-hosts comModel=%1 — trying next model")
                              .arg(QString::fromLatin1(comModelNames[comIndex])));
        if (m_comInitialized) {
            CoUninitialize();
            m_comInitialized = false;
        }
    }

    if (!m_driver) {
        const bool deviceBusy = (asioCrashed && AsioSessionProbe::isAudioEndpointBusy(m_driverId))
            || AsioSessionProbe::hasAnyExternalWasapiRenderSessionsForAsioDriver(m_driverId);
        m_lastOpenFailureReason = deviceBusy
            ? DriverOpenFailureReason::DeviceBusy
            : DriverOpenFailureReason::DriverError;
        // Prefer the STA failure detail (more informative) over MTA CoCreateInstance failure.
        if (!staFailureDetail.isEmpty()) {
            m_lastOpenFailureDetail = staFailureDetail;
        } else if (!m_lastOpenFailureDetail.isEmpty()) {
            // keep existing detail
        } else {
            m_lastOpenFailureDetail = deviceBusy
                ? QStringLiteral("Device occupied by another application")
                : QStringLiteral("driver->init() failed with all host window candidates");
        }
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("asio openDriver init failed driverId=%1 reason=%2")
                              .arg(driverIdText)
                              .arg(deviceBusy ? QStringLiteral("DeviceBusy") : QStringLiteral("DriverError")));
        return failOpenDriver(m_lastOpenFailureReason);
    }

    const int requestedSampleRate = m_outputFormat.sampleRate();
    int selectedSampleRate = 0;
    ASIOError selectedCanRateResult = AsioUtils::kAsioNotPresent;
    for (const int candidateSampleRate : AsioFormats::sourcePreferredSampleRateCandidates(requestedSampleRate)) {
        const ASIOError canRateResult =
            AsioUtils::safeAsioCanSampleRate(m_driver, candidateSampleRate, &asioCrashed);
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("asio sampleRate candidate driverId=%1 requested=%2 candidate=%3 result=%4 crashed=%5")
                              .arg(QString::fromUtf8(m_driverId))
                              .arg(requestedSampleRate)
                              .arg(candidateSampleRate)
                              .arg(canRateResult)
                              .arg(asioCrashed ? 1 : 0));
        if (asioCrashed) {
            m_lastOpenFailureDetail = QStringLiteral("canSampleRate() crashed");
            return failOpenDriver(classifyEndpointOpenFailure());
        }
        if (AsioUtils::asioResultOk(canRateResult)) {
            selectedSampleRate = candidateSampleRate;
            selectedCanRateResult = canRateResult;
            break;
        }
        selectedCanRateResult = canRateResult;
    }
    if (selectedSampleRate <= 0) {
        m_lastOpenFailureDetail = QStringLiteral("No supported sample rate (requested=%1)").arg(requestedSampleRate);
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("asio sampleRate unsupported-all driverId=%1 requested=%2 lastResult=%3")
                              .arg(QString::fromUtf8(m_driverId))
                              .arg(requestedSampleRate)
                              .arg(selectedCanRateResult));
        return failOpenDriver(classifyEndpointOpenFailure());
    }
    if (selectedSampleRate != requestedSampleRate) {
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("asio sampleRate fallback driverId=%1 requested=%2 selected=%3 reason=unsupported-requested")
                              .arg(QString::fromUtf8(m_driverId))
                              .arg(requestedSampleRate)
                              .arg(selectedSampleRate));
        m_outputFormat.setSampleRate(selectedSampleRate);
    }
    const ASIOError setRateResult =
        AsioUtils::safeAsioSetSampleRate(m_driver, m_outputFormat.sampleRate(), &asioCrashed);
    if (asioCrashed || !AsioUtils::asioResultOk(setRateResult)) {
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("asio setSampleRate failed driverId=%1 rate=%2 result=%3 crashed=%4")
                              .arg(QString::fromUtf8(m_driverId))
                              .arg(m_outputFormat.sampleRate())
                              .arg(setRateResult)
                              .arg(asioCrashed ? 1 : 0));
    }
    if (asioCrashed) {
        return failOpenDriver(classifyEndpointOpenFailure());
    }
    ASIOSampleRate actualSampleRate = 0.0;
    const ASIOError actualRateResult =
        AsioUtils::safeAsioGetSampleRate(m_driver, &actualSampleRate, &asioCrashed);
    const int roundedActualSampleRate = qRound(actualSampleRate);
    PlayerLogger::log(QStringLiteral("audio"),
                      QStringLiteral("asio sampleRate active driverId=%1 requested=%2 actual=%3 result=%4 crashed=%5")
                          .arg(QString::fromUtf8(m_driverId))
                          .arg(m_outputFormat.sampleRate())
                          .arg(actualSampleRate, 0, 'f', 1)
                          .arg(actualRateResult)
                          .arg(asioCrashed ? 1 : 0));
    if (asioCrashed) {
        return failOpenDriver(classifyEndpointOpenFailure());
    }
    if (AsioUtils::asioResultOk(actualRateResult)
        && roundedActualSampleRate > 0
        && roundedActualSampleRate != m_outputFormat.sampleRate()) {
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("asio sampleRate actual-format driverId=%1 configured=%2 actual=%3")
                              .arg(QString::fromUtf8(m_driverId))
                              .arg(m_outputFormat.sampleRate())
                              .arg(roundedActualSampleRate));
        m_outputFormat.setSampleRate(roundedActualSampleRate);
    }

    long inputChannels = 0;
    long outputChannels = 0;
    const ASIOError channelsResult =
        AsioUtils::safeAsioGetChannels(m_driver, &inputChannels, &outputChannels, &asioCrashed);
    if (!AsioUtils::asioResultOk(channelsResult)
        || outputChannels <= 0) {
        m_lastOpenFailureDetail = QStringLiteral("getChannels failed (outputs=%1, result=%2)")
            .arg(outputChannels).arg(channelsResult);
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("asio getChannels failed driverId=%1 result=%2 crashed=%3 outputs=%4")
                              .arg(QString::fromUtf8(m_driverId))
                              .arg(channelsResult)
                              .arg(asioCrashed ? 1 : 0)
                              .arg(outputChannels));
        return failOpenDriver(classifyEndpointOpenFailure());
    }
    PlayerLogger::log(QStringLiteral("audio"),
                      QStringLiteral("asio driverChannels driverId=%1 inputs=%2 outputs=%3 requestedOutputs=%4")
                          .arg(QString::fromUtf8(m_driverId))
                          .arg(inputChannels)
                          .arg(outputChannels)
                          .arg(m_outputFormat.channelCount()));

    long minSize = 0;
    long maxSize = 0;
    long preferredSize = 0;
    long granularity = 0;
    const ASIOError bufferSizeResult =
        AsioUtils::safeAsioGetBufferSize(m_driver, &minSize, &maxSize, &preferredSize, &granularity, &asioCrashed);
    if (!AsioUtils::asioResultOk(bufferSizeResult)
        || preferredSize <= 0) {
        m_lastOpenFailureDetail = QStringLiteral("getBufferSize failed (preferred=%1, result=%2)")
            .arg(preferredSize).arg(bufferSizeResult);
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("asio getBufferSize failed driverId=%1 result=%2 crashed=%3 preferred=%4")
                              .arg(QString::fromUtf8(m_driverId))
                              .arg(bufferSizeResult)
                              .arg(asioCrashed ? 1 : 0)
                              .arg(preferredSize));
        return failOpenDriver(classifyEndpointOpenFailure());
    }
    m_bufferSize = preferredSize;
    PlayerLogger::log(QStringLiteral("audio"),
                      QStringLiteral("asio bufferSize driverId=%1 min=%2 max=%3 preferred=%4 granularity=%5")
                          .arg(QString::fromUtf8(m_driverId))
                          .arg(minSize)
                          .arg(maxSize)
                          .arg(preferredSize)
                          .arg(granularity));

    const int channelCount = qMin<int>(m_outputFormat.channelCount(), static_cast<int>(outputChannels));
    m_bufferInfos.resize(channelCount);
    m_channelInfos.resize(channelCount);
    for (int channel = 0; channel < channelCount; ++channel) {
        m_bufferInfos[channel].isInput = kAsioFalse;
        m_bufferInfos[channel].channelNum = channel;
    }

    m_callbacks.bufferSwitch = asioBufferSwitch;
    m_callbacks.sampleRateDidChange = asioSampleRateDidChange;
    m_callbacks.asioMessage = asioMessage;
    m_callbacks.bufferSwitchTimeInfo = asioBufferSwitchTimeInfo;
    const ASIOError createResult =
        AsioUtils::safeAsioCreateBuffers(m_driver, m_bufferInfos.data(), channelCount, m_bufferSize, &m_callbacks, &asioCrashed);
    if (!AsioUtils::asioResultOk(createResult)) {
        m_lastOpenFailureDetail = QStringLiteral("createBuffers failed (size=%1, channels=%2, result=%3)")
            .arg(m_bufferSize).arg(channelCount).arg(createResult);
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("asio createBuffers failed driverId=%1 bufferSize=%2 channels=%3 result=%4 crashed=%5 error=%6")
                              .arg(QString::fromUtf8(m_driverId))
                              .arg(m_bufferSize)
                              .arg(channelCount)
                              .arg(createResult)
                              .arg(asioCrashed ? 1 : 0)
                              .arg(AsioUtils::asioDriverError(m_driver)));
        return failOpenDriver(classifyEndpointOpenFailure());
    }
    m_buffersCreated = true;

    for (int channel = 0; channel < channelCount; ++channel) {
        m_channelInfos[channel].channel = channel;
        m_channelInfos[channel].isInput = kAsioFalse;
        const ASIOError channelInfoResult =
            AsioUtils::safeAsioGetChannelInfo(m_driver, &m_channelInfos[channel], &asioCrashed);
        if (!AsioUtils::asioResultOk(channelInfoResult)) {
            m_lastOpenFailureDetail = QStringLiteral("getChannelInfo failed (channel=%1, result=%2)")
                .arg(channel).arg(channelInfoResult);
            PlayerLogger::log(QStringLiteral("audio"),
                              QStringLiteral("asio getChannelInfo failed driverId=%1 channel=%2 result=%3 crashed=%4")
                                  .arg(QString::fromUtf8(m_driverId))
                                  .arg(channel)
                                  .arg(channelInfoResult)
                                  .arg(asioCrashed ? 1 : 0));
            return failOpenDriver(classifyEndpointOpenFailure());
        }
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("asio channelInfo driverId=%1 channel=%2 active=%3 group=%4 type=%5 name=%6")
                              .arg(QString::fromUtf8(m_driverId))
                              .arg(channel)
                              .arg(m_channelInfos[channel].isActive)
                              .arg(m_channelInfos[channel].channelGroup)
                              .arg(m_channelInfos[channel].type)
                              .arg(QString::fromLocal8Bit(m_channelInfos[channel].name).trimmed()));
    }

    m_noiseShaperState.resize(channelCount);
    {
        uint32_t seed = QRandomGenerator::global()->generate();
        for (auto &state : m_noiseShaperState) {
            state = AsioWorker::NoiseShaperState{};
            state.rng = seed;
            seed = seed * 1103515245u + 12345u;
        }
    }
    if (m_sourceBitDepth > 0 && m_sourceBitDepth < 32) {
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("asio noiseShaping enabled driverId=%1 sourceBitDepth=%2 channels=%3")
                              .arg(QString::fromUtf8(m_driverId))
                              .arg(m_sourceBitDepth)
                              .arg(channelCount));
    }

    return true;
}

#include "windowsasioaudioplayer_worker.moc"
