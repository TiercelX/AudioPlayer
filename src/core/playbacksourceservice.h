#ifndef PLAYBACKSOURCESERVICE_H
#define PLAYBACKSOURCESERVICE_H

#include "audioplayerfactory.h"
#include "mediainfodialog.h"

#include <QString>

struct PlaybackSourceResolution
{
    QString playbackPath;
    AudioInfo sourceInfo;
    qint64 durationMs = 0;
    QString statusMessage;
    QString warningMessage;
    bool usedSidecar = false;
};

struct PlaybackPreparationEstimate
{
    bool usesSidecar = false;
    bool cacheExists = false;
    QString cachePath;
    qint64 sourceSizeBytes = 0;
    int timeoutMs = 0;
};

struct PlaybackCacheSettings
{
    QString cacheDirectory;
    int maxSidecars = 12;
    int maxSidecarAgeDays = 14;
    int maxSidecarMiB = 512;
    int maxDiagnosticAudioFiles = 4;
    int maxDiagnosticAudioAgeDays = 2;
    int maxDiagnosticAudioMiB = 256;
    int maxPcmCacheMiB = 0;
    bool maxPcmCacheMiBConfigured = false;
    int maxPcmCacheAgeMinutes = 30;
};

struct PlaybackCacheUsage
{
    qint64 totalBytes = 0;
    qint64 sidecarBytes = 0;
    qint64 diagnosticAudioBytes = 0;
    qint64 loopbackAudioBytes = 0;
    int totalFiles = 0;
    int sidecarFiles = 0;
    int diagnosticAudioFiles = 0;
    int loopbackAudioFiles = 0;
};

class PlaybackSourceService
{
public:
    PlaybackPreparationEstimate estimatePreparation(const QString &filePath,
                                                   AudioPlaybackPlan::SourceMode sourceMode) const;
    AudioInfo probeSourceInfo(const QString &filePath) const;
    qint64 probeDuration(const QString &filePath) const;
    QString playbackCacheRoot() const;
    PlaybackCacheSettings cacheSettings() const;
    PlaybackCacheUsage cacheUsage() const;
    void saveCacheSettings(const PlaybackCacheSettings &settings) const;
    void prunePlaybackCacheNow() const;
    PlaybackSourceResolution resolveForPlayback(const QString &filePath,
                                                const AudioInfo &initialSourceInfo,
                                                AudioPlaybackPlan::SourceMode sourceMode) const;

private:
    struct PreparedPlaybackSource
    {
        QString path;
        QString statusMessage;
        QString warningMessage;
        bool usedSidecar = false;
    };

    QString locateFfmpegExecutable() const;
    QString locateFfprobeExecutable() const;
    PreparedPlaybackSource preparePlaybackSource(const QString &filePath,
                                                 AudioPlaybackPlan::SourceMode sourceMode) const;
    void prunePlaybackCache(const QString &cacheRoot) const;
};

#endif // PLAYBACKSOURCESERVICE_H
