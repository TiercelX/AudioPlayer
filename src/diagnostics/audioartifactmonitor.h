#ifndef AUDIOARTIFACTMONITOR_H
#define AUDIOARTIFACTMONITOR_H

#include "ffmpegpcmshared.h"

#include <QDateTime>
#include <QHash>
#include <QString>
#include <QVector>

class AudioArtifactMonitor
{
public:
    struct PlaybackContext
    {
        int sessionId = 0;
        QString source;
        QString playbackState;
        QString recentControlEvent;
        quint64 outputGeneration = 0;
        quint64 bufferGeneration = 0;
        qint64 positionMs = -1;
        qreal audioLevelLeft = 0.0;
        qreal audioLevelRight = 0.0;
        bool recoveryPending = false;
        int recoveryAttempt = 0;
        bool deviceRebuildPending = false;
        QString pipelineStartupProfile;
        QString artifactPath;
        QString activeSwitchTrigger;
        QString activeSwitchPhase;
        QString activeSwitchReason;
    };

    struct RenderContext
    {
        bool warmup = false;
        bool silenceFill = false;
        bool recovery = false;
        bool firstDataBlockAfterConfigure = false;
        bool firstDataBlockAfterDeviceRebuild = false;
        qsizetype writeFrameCount = -1;
        qint64 wasapiPaddingFrames = -1;
        qint64 wasapiAvailableFrames = -1;
    };

    void resetContinuity(const QString &reason = QString());
    void analyzePcmBlock(const char *data,
                         qsizetype byteCount,
                         const PcmStreamFormat &format,
                         const PlaybackContext &context,
                         const QString &renderSource,
                         const RenderContext &renderContext);
    void observeSilentFrames(qsizetype frameCount,
                             const PcmStreamFormat &format,
                             const PlaybackContext &context,
                             const QString &renderSource,
                             const RenderContext &renderContext);

    quint64 artifactCountTotal() const;
    QHash<QString, quint64> artifactCountByType() const;
    QHash<QString, quint64> artifactCountByRecentControlEvent() const;
    QHash<QString, quint64> artifactCountByPlaybackState() const;
    QHash<QString, quint64> artifactCountByDetectorAndSeverity() const;
    double artifactDensityPerMinute() const;
    QDateTime lastArtifactTime() const;

private:
    struct SampleRead
    {
        double value = 0.0;
        bool finite = true;
        bool outOfRange = false;
    };

    struct BlockMetrics
    {
        qsizetype frameCount = 0;
        qsizetype sampleCount = 0;
        double peak = 0.0;
        double rms = 0.0;
        double maxJump = 0.0;
        double maxBoundaryJump = 0.0;
        double openingPeak = 0.0;
        double previousWindowPeak = 0.0;
        double previousWindowRms = 0.0;
        double currentWindowPeak = 0.0;
        double currentWindowRms = 0.0;
        QVector<double> previousTailSamples;
        QVector<double> currentFirstSamples;
        QVector<double> currentLastSamples;
        bool hasInvalidSample = false;
        bool hasOutOfRangeSample = false;
    };

    struct BurstScanResult
    {
        double peak = 0.0;
        double rms = 0.0;
        double ratioToBlockRms = 0.0;
    };

    struct CrackleScanResult
    {
        double score = 0.0;
        double averageJump = 0.0;
        double maxJump = 0.0;
        int jumpCount = 0;
    };

    struct WindowMetrics
    {
        qsizetype sampleCount = 0;
        double peak = 0.0;
        double rms = 0.0;
    };

    struct ArtifactLogState
    {
        qint64 lastLoggedMs = 0;
        quint64 suppressedCount = 0;
    };

    void ensureFormatState(const PcmStreamFormat &format);
    SampleRead readSample(const char *sampleData, const PcmStreamFormat &format) const;
    double detectTransientSpike(const char *data,
                                qsizetype frameCount,
                                const PcmStreamFormat &format) const;
    BurstScanResult detectShortBurst(const char *data,
                                     qsizetype frameCount,
                                     const PcmStreamFormat &format,
                                     double blockRms) const;
    CrackleScanResult detectCrackleTexture(const char *data,
                                           qsizetype frameCount,
                                           const PcmStreamFormat &format,
                                           bool sensitiveContext) const;
    void updatePreviousSamplesToSilence(const PcmStreamFormat &format);
    bool isSilentBlock(const BlockMetrics &metrics) const;
    void logArtifact(const QString &detectorType,
                     const QString &severity,
                     double jumpMagnitude,
                     const BlockMetrics &metrics,
                     const PcmStreamFormat &format,
                     const PlaybackContext &context,
                     const QString &renderSource,
                     const RenderContext &renderContext);
    QString artifactCountsText() const;
    QString countsText(const QHash<QString, quint64> &counts) const;
    QString controlEventKey(const QString &recentControlEvent) const;
    QString samplesText(const QVector<double> &samples) const;
    void pruneRecentArtifactTimes(qint64 nowMs);
    void flushSuppressedArtifactLogs(const QString &reason);

    PcmStreamFormat m_previousFormat;
    QVector<double> m_previousSamples;
    WindowMetrics m_previousTailWindow;
    bool m_hasPreviousBlock = false;
    bool m_previousBlockSilent = true;
    quint64 m_artifactCountTotal = 0;
    QHash<QString, quint64> m_artifactCountByType;
    QHash<QString, quint64> m_artifactCountByRecentControlEvent;
    QHash<QString, quint64> m_artifactCountByPlaybackState;
    QHash<QString, quint64> m_artifactCountByDetectorAndSeverity;
    QVector<qint64> m_recentArtifactTimesMs;
    QHash<QString, ArtifactLogState> m_artifactLogStates;
    QDateTime m_lastArtifactTime;
};

#endif // AUDIOARTIFACTMONITOR_H
