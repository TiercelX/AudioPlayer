#include "diagnosticsservice.h"
#include "audioplayerbackend.h"
#include "playerlogger.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QThread>

#ifdef Q_OS_WIN
#include <windows.h>
#include <psapi.h>
#endif

DiagnosticsService::DiagnosticsService(QObject *parent)
    : QObject(parent)
    , m_collectTimer(new QTimer(this))
{
    m_peakHoldTimer.start();
    m_collectTimer->setInterval(m_updateIntervalMs);
    connect(m_collectTimer, &QTimer::timeout, this, &DiagnosticsService::collectMetrics);
}

DiagnosticsService::~DiagnosticsService()
{
    detachFromPlayer();
}

void DiagnosticsService::attachToPlayer(AudioPlayerBackend *player)
{
    if (m_player == player) {
        return;
    }

    detachFromPlayer();
    m_player = player;

    if (!m_player) {
        return;
    }

    connect(m_player, &AudioPlayerBackend::audioLevelsChanged,
            this, &DiagnosticsService::onAudioLevelsChanged);
    connect(m_player, &AudioPlayerBackend::playbackStateChanged,
            this, [this](AudioPlayerBackend::PlaybackState state) {
                onPlaybackStateChanged(static_cast<int>(state));
            });

    m_collectTimer->start();
    resetMetrics();
}

void DiagnosticsService::detachFromPlayer()
{
    m_collectTimer->stop();

    if (m_player) {
        disconnect(m_player, nullptr, this, nullptr);
        m_player = nullptr;
    }

    resetMetrics();
}

void DiagnosticsService::setUpdateIntervalMs(int intervalMs)
{
    m_updateIntervalMs = qMax(10, intervalMs);
    m_collectTimer->setInterval(m_updateIntervalMs);
}

int DiagnosticsService::updateIntervalMs() const
{
    return m_updateIntervalMs;
}

void DiagnosticsService::setPeakHoldTimeMs(int holdTimeMs)
{
    m_peakHoldTimeMs = qMax(100, holdTimeMs);
}

int DiagnosticsService::peakHoldTimeMs() const
{
    return m_peakHoldTimeMs;
}

DiagnosticsSnapshot DiagnosticsService::currentSnapshot() const
{
    DiagnosticsSnapshot snapshot;
    snapshot.audioLevels = m_audioLevels;
    snapshot.buffer = m_buffer;
    snapshot.latency = m_latency;
    snapshot.system = m_system;
    snapshot.timestampMs = QDateTime::currentMSecsSinceEpoch();
    return snapshot;
}

void DiagnosticsService::onAudioLevelsChanged(qreal leftLevel, qreal rightLevel)
{
    m_audioLevels.leftLevel = leftLevel;
    m_audioLevels.rightLevel = rightLevel;

    const qreal currentPeak = qMax(qAbs(leftLevel), qAbs(rightLevel));
    updatePeakLevel(currentPeak);

    m_audioLevels.peakLevel = m_peakLevel;

    const qreal rms = qSqrt((leftLevel * leftLevel + rightLevel * rightLevel) / 2.0);
    m_audioLevels.rmsLevel = rms;

    emit audioLevelsUpdated(m_audioLevels);
}

void DiagnosticsService::onPlaybackStateChanged(int state)
{
    const auto playbackState = static_cast<AudioPlayerBackend::PlaybackState>(state);
    const bool wasPlaying = m_isPlaying;
    m_isPlaying = (playbackState == AudioPlayerBackend::PlaybackState::Playing);

    if (!m_isPlaying && wasPlaying) {
        m_latency.decodeLatencyMs = 0.0;
        m_latency.outputLatencyMs = 0.0;
        m_latency.totalLatencyMs = 0.0;
    }
}

void DiagnosticsService::collectMetrics()
{
    updateSystemMetrics();

    if (m_isPlaying) {
        m_latency.decodeLatencyMs = 0.0;
        m_latency.outputLatencyMs = 0.0;
        m_latency.totalLatencyMs = 0.0;
    }

    const qreal peakAge = m_peakHoldTimer.elapsed();
    if (peakAge > m_peakHoldTimeMs && m_peakLevel > 0.0) {
        m_peakLevel = qMax(0.0, m_peakLevel - PEAK_DECAY_RATE * (peakAge - m_peakHoldTimeMs));
        m_audioLevels.peakLevel = m_peakLevel;
    }

    DiagnosticsSnapshot snapshot = currentSnapshot();
    emit diagnosticsUpdated(snapshot);
    emit bufferMetricsUpdated(snapshot.buffer);
    emit latencyMetricsUpdated(snapshot.latency);
    emit systemMetricsUpdated(snapshot.system);
}

void DiagnosticsService::updatePeakLevel(qreal level)
{
    if (level >= m_peakLevel) {
        m_peakLevel = level;
        m_peakHoldTimer.restart();
    }
}

void DiagnosticsService::updateSystemMetrics()
{
    m_system.threadCount = 0;

#ifdef Q_OS_WIN
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        m_system.memoryUsageBytes = static_cast<qint64>(pmc.WorkingSetSize);
    }

    static ULARGE_INTEGER lastCPU = {};
    static ULARGE_INTEGER lastSysCPU = {};
    static ULARGE_INTEGER lastUserCPU = {};
    static bool cpuInitialized = false;

    FILETIME ftime, fsys, fuser;
    ULARGE_INTEGER now, sys, user;

    GetSystemTimeAsFileTime(&ftime);
    now.LowPart = ftime.dwLowDateTime;
    now.HighPart = ftime.dwHighDateTime;

    GetProcessTimes(GetCurrentProcess(), &ftime, &ftime, &fsys, &fuser);
    sys.LowPart = fsys.dwLowDateTime;
    sys.HighPart = fsys.dwHighDateTime;
    user.LowPart = fuser.dwLowDateTime;
    user.HighPart = fuser.dwHighDateTime;

    if (cpuInitialized) {
        const auto systemTimeDiff = static_cast<double>(sys.QuadPart - lastSysCPU.QuadPart);
        const auto userTimeDiff = static_cast<double>(user.QuadPart - lastUserCPU.QuadPart);
        const auto totalDiff = static_cast<double>(now.QuadPart - lastCPU.QuadPart);

        if (totalDiff > 0) {
            m_system.cpuUsagePercent = (systemTimeDiff + userTimeDiff) / totalDiff * 100.0;
        }
    }

    lastCPU = now;
    lastSysCPU = sys;
    lastUserCPU = user;
    cpuInitialized = true;
#else
    // Linux/Unix: no platform-specific metrics implemented yet; report zeros.
    m_system.cpuUsagePercent = 0.0;
    m_system.memoryUsageBytes = 0;
#endif
}

void DiagnosticsService::resetMetrics()
{
    m_audioLevels = AudioLevelMetrics();
    m_buffer = BufferMetrics();
    m_latency = LatencyMetrics();
    m_system = SystemMetrics();
    m_peakLevel = 0.0;
    m_isPlaying = false;
}
