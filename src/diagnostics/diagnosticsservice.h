#ifndef DIAGNOSTICSSERVICE_H
#define DIAGNOSTICSSERVICE_H

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>

class AudioPlayerBackend;

struct AudioLevelMetrics
{
    qreal leftLevel = 0.0;
    qreal rightLevel = 0.0;
    qreal peakLevel = 0.0;
    qreal rmsLevel = 0.0;
};

struct BufferMetrics
{
    qint64 bufferUsed = 0;
    qint64 bufferCapacity = 0;
    qreal usageRatio = 0.0;
};

struct LatencyMetrics
{
    qreal decodeLatencyMs = 0.0;
    qreal outputLatencyMs = 0.0;
    qreal totalLatencyMs = 0.0;
};

struct SystemMetrics
{
    qreal cpuUsagePercent = 0.0;
    qint64 memoryUsageBytes = 0;
    int threadCount = 0;
};

struct DiagnosticsSnapshot
{
    AudioLevelMetrics audioLevels;
    BufferMetrics buffer;
    LatencyMetrics latency;
    SystemMetrics system;
    qint64 timestampMs = 0;
};

class DiagnosticsService : public QObject
{
    Q_OBJECT

public:
    explicit DiagnosticsService(QObject *parent = nullptr);
    ~DiagnosticsService() override;

    void attachToPlayer(AudioPlayerBackend *player);
    void detachFromPlayer();

    void setUpdateIntervalMs(int intervalMs);
    int updateIntervalMs() const;

    void setPeakHoldTimeMs(int holdTimeMs);
    int peakHoldTimeMs() const;

    DiagnosticsSnapshot currentSnapshot() const;

signals:
    void diagnosticsUpdated(const DiagnosticsSnapshot &snapshot);
    void audioLevelsUpdated(const AudioLevelMetrics &levels);
    void bufferMetricsUpdated(const BufferMetrics &buffer);
    void latencyMetricsUpdated(const LatencyMetrics &latency);
    void systemMetricsUpdated(const SystemMetrics &system);

private slots:
    void onAudioLevelsChanged(qreal leftLevel, qreal rightLevel);
    void onPlaybackStateChanged(int state);
    void collectMetrics();

private:
    void updatePeakLevel(qreal level);
    void updateSystemMetrics();
    void resetMetrics();

    AudioPlayerBackend *m_player = nullptr;
    QTimer *m_collectTimer = nullptr;

    AudioLevelMetrics m_audioLevels;
    BufferMetrics m_buffer;
    LatencyMetrics m_latency;
    SystemMetrics m_system;

    qreal m_peakLevel = 0.0;
    QElapsedTimer m_peakHoldTimer;

    bool m_isPlaying = false;

    int m_updateIntervalMs = 50;
    int m_peakHoldTimeMs = 1000;

    static constexpr qreal PEAK_DECAY_RATE = 0.001;
};

#endif // DIAGNOSTICSSERVICE_H
