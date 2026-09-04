#include "playbacksourceserviceinternal.h"

#include "playerlogger.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QtConcurrent>

namespace {

constexpr qint64 kBytesPerMiB = 1024 * 1024;
constexpr int kSidecarRemuxStartTimeoutMs = 3000;
constexpr int kSidecarRemuxMinFinishTimeoutMs = 30000;
constexpr int kSidecarRemuxMaxFinishTimeoutMs = 180000;
constexpr int kSidecarRemuxPerMiBTimeoutMs = 150;

int sidecarRemuxFinishTimeoutMs(const QFileInfo &sourceInfo)
{
    const qint64 sizeInMiB =
        qMax<qint64>(1, (sourceInfo.size() + kBytesPerMiB - 1) / kBytesPerMiB);
    const qint64 scaledTimeout =
        static_cast<qint64>(kSidecarRemuxMinFinishTimeoutMs)
        + sizeInMiB * kSidecarRemuxPerMiBTimeoutMs;
    return static_cast<int>(qMin<qint64>(kSidecarRemuxMaxFinishTimeoutMs, scaledTimeout));
}

QString sidecarCachePath(const QString &filePath,
                         const QFileInfo &sourceInfo,
                         const QString &cacheRoot)
{
    const QByteArray cacheKey =
        filePath.toUtf8()
        + '|'
        + QByteArray::number(sourceInfo.lastModified().toMSecsSinceEpoch())
        + '|'
        + QByteArray::number(sourceInfo.size())
        + '|'
        + QByteArray("sidecar-remux-tight-cluster-v1");
    const QString cacheBaseName = QString::fromLatin1(
        QCryptographicHash::hash(cacheKey, QCryptographicHash::Sha1).toHex());
    return QDir(cacheRoot).filePath(QStringLiteral("%1.mka").arg(cacheBaseName));
}

void pruneCacheEntries(const QFileInfoList &entries,
                       int maxEntries,
                       int maxAgeDays,
                       qint64 maxTotalBytes,
                       const QString &logLabel)
{
    if (entries.isEmpty()) {
        return;
    }

    const QDateTime cutoff = QDateTime::currentDateTimeUtc().addDays(-maxAgeDays);
    int keptCount = 0;
    qint64 keptBytes = 0;
    for (const QFileInfo &entry : entries) {
        const bool keepNewest = keptCount < maxEntries;
        const bool expired = entry.lastModified().toUTC() < cutoff;
        const bool overBytes = maxTotalBytes > 0 && keptBytes + entry.size() > maxTotalBytes;
        if (keepNewest && !expired && !overBytes) {
            ++keptCount;
            keptBytes += entry.size();
            continue;
        }

        if (QFile::remove(entry.absoluteFilePath())) {
            PlayerLogger::log(QStringLiteral("source"),
                              QStringLiteral("prunePlaybackCache type=%1 removed=%2 expired=%3 overBytes=%4 size=%5 keptBytes=%6 maxBytes=%7")
                                  .arg(logLabel)
                                  .arg(entry.absoluteFilePath())
                                  .arg(expired)
                                  .arg(overBytes)
                                  .arg(entry.size())
                                  .arg(keptBytes)
                                  .arg(maxTotalBytes));
        }
    }
}

} // namespace

PlaybackCacheUsage PlaybackSourceService::cacheUsage() const
{
    PlaybackCacheUsage usage;
    QDir cacheDir(playbackCacheRoot());
    const auto accumulate = [&usage](const QFileInfoList &entries, qint64 *bytes, int *files) {
        for (const QFileInfo &entry : entries) {
            *bytes += entry.size();
            ++(*files);
            usage.totalBytes += entry.size();
            ++usage.totalFiles;
        }
    };

    accumulate(cacheDir.entryInfoList(QStringList() << QStringLiteral("*.mka"),
                                      QDir::Files | QDir::Readable,
                                      QDir::Time),
               &usage.sidecarBytes,
               &usage.sidecarFiles);
    accumulate(cacheDir.entryInfoList(QStringList() << QStringLiteral("*.wav"),
                                      QDir::Files | QDir::Readable,
                                      QDir::Time),
               &usage.diagnosticAudioBytes,
               &usage.diagnosticAudioFiles);

    QDir loopbackDir(cacheDir.filePath(QStringLiteral("loopback")));
    if (loopbackDir.exists()) {
        accumulate(loopbackDir.entryInfoList(QStringList() << QStringLiteral("*.wav"),
                                             QDir::Files | QDir::Readable,
                                             QDir::Time),
                   &usage.loopbackAudioBytes,
                   &usage.loopbackAudioFiles);
    }
    return usage;
}

void PlaybackSourceService::prunePlaybackCacheNow() const
{
    const QString cacheRoot = playbackCacheRoot();
    QDir().mkpath(cacheRoot);
    prunePlaybackCache(cacheRoot);
}

PlaybackPreparationEstimate PlaybackSourceService::estimatePreparation(
    const QString &filePath,
    AudioPlaybackPlan::SourceMode sourceMode) const
{
    PlaybackPreparationEstimate estimate;
    if (sourceMode != AudioPlaybackPlan::SourceMode::RemuxRawDolbySidecar) {
        return estimate;
    }

    const QString rawInputFormat = PlaybackSourceServiceInternal::rawInputFormatForFile(filePath);
    if (rawInputFormat.isEmpty()) {
        return estimate;
    }

    const QFileInfo sourceInfo(filePath);
    estimate.usesSidecar = true;
    estimate.sourceSizeBytes = sourceInfo.size();
    estimate.timeoutMs = sidecarRemuxFinishTimeoutMs(sourceInfo);
    estimate.cachePath = sidecarCachePath(filePath, sourceInfo, playbackCacheRoot());
    estimate.cacheExists = QFileInfo::exists(estimate.cachePath)
        && QFileInfo(estimate.cachePath).size() > 0;
    return estimate;
}

PlaybackSourceService::PreparedPlaybackSource PlaybackSourceService::preparePlaybackSource(
    const QString &filePath,
    AudioPlaybackPlan::SourceMode sourceMode) const
{
    PreparedPlaybackSource result;
    result.path = filePath;

    const QString cacheRoot = playbackCacheRoot();
    QDir().mkpath(cacheRoot);

    static QMutex pruneMutex;
    static QElapsedTimer lastPruneTime;
    static bool pruneTimerInitialized = false;

    {
        QMutexLocker locker(&pruneMutex);
        if (!pruneTimerInitialized) {
            lastPruneTime.start();
            pruneTimerInitialized = true;
        } else if (lastPruneTime.elapsed() > 60000) {
            lastPruneTime.restart();
            QtConcurrent::run([this, cacheRoot]() {
                prunePlaybackCache(cacheRoot);
            });
        }
    }

    if (sourceMode != AudioPlaybackPlan::SourceMode::RemuxRawDolbySidecar) {
        return result;
    }

    const QString rawInputFormat = PlaybackSourceServiceInternal::rawInputFormatForFile(filePath);
    if (rawInputFormat.isEmpty()) {
        return result;
    }

    const QString ffmpegExecutable = locateFfmpegExecutable();
    if (ffmpegExecutable.isEmpty()) {
        result.warningMessage = QObject::tr("未找到 ffmpeg，原始 Dolby 码流将直接读取，时间轴可能不稳定");
        PlayerLogger::log(QStringLiteral("source"),
                          QStringLiteral("preparePlaybackSource ffmpeg-missing path=%1 format=%2")
                              .arg(filePath)
                              .arg(rawInputFormat));
        return result;
    }

    const QFileInfo sourceInfo(filePath);
    const QString cachedPath = sidecarCachePath(filePath, sourceInfo, cacheRoot);
    const int remuxFinishTimeoutMs = sidecarRemuxFinishTimeoutMs(sourceInfo);

    if (QFileInfo::exists(cachedPath) && QFileInfo(cachedPath).size() > 0) {
        result.path = cachedPath;
        result.usedSidecar = true;
        result.statusMessage = QObject::tr("已复用原始 Dolby 时间轴容器缓存");
        PlayerLogger::log(QStringLiteral("source"),
                          QStringLiteral("preparePlaybackSource reuse-sidecar source=%1 cache=%2")
                              .arg(filePath)
                              .arg(cachedPath));
        return result;
    }

    PlayerLogger::log(QStringLiteral("source"),
                      QStringLiteral("preparePlaybackSource build-sidecar source=%1 cache=%2 format=%3 timeoutMs=%4")
                          .arg(filePath)
                          .arg(cachedPath)
                          .arg(rawInputFormat)
                          .arg(remuxFinishTimeoutMs));

    QFile::remove(cachedPath);
    const PlaybackSourceServiceInternal::ToolProcessResult remuxResult =
        PlaybackSourceServiceInternal::runToolProcess(
            ffmpegExecutable,
            {QStringLiteral("-y"),
             QStringLiteral("-v"),
             QStringLiteral("error"),
             QStringLiteral("-f"),
             rawInputFormat,
             QStringLiteral("-i"),
             filePath,
             QStringLiteral("-map"),
             QStringLiteral("0:a:0"),
             QStringLiteral("-c"),
             QStringLiteral("copy"),
             QStringLiteral("-cluster_time_limit"),
             QStringLiteral("100"),
             QStringLiteral("-f"),
             QStringLiteral("matroska"),
             cachedPath},
            kSidecarRemuxStartTimeoutMs,
            remuxFinishTimeoutMs);
    if (!remuxResult.started || !remuxResult.finished
        || remuxResult.exitStatus != QProcess::NormalExit
        || remuxResult.exitCode != 0
        || !QFileInfo::exists(cachedPath)
        || QFileInfo(cachedPath).size() <= 0) {
        QFile::remove(cachedPath);
        result.warningMessage = remuxResult.timedOut
            ? QObject::tr("载入文件超时，已回退为直接读取，seek 可能不稳定")
            : QObject::tr("封装原始 Dolby 码流失败，回退为直接读取，seek 可能不稳定");
        PlayerLogger::log(QStringLiteral("source"),
                          QStringLiteral("preparePlaybackSource remux-failed source=%1 cache=%2 started=%3 finished=%4 exitCode=%5 exitStatus=%6 timeoutMs=%7 error=%8 stderr=%9")
                              .arg(filePath)
                              .arg(cachedPath)
                              .arg(remuxResult.started)
                              .arg(remuxResult.finished)
                              .arg(remuxResult.exitCode)
                              .arg(static_cast<int>(remuxResult.exitStatus))
                              .arg(remuxFinishTimeoutMs)
                              .arg(remuxResult.errorText)
                              .arg(remuxResult.standardError));
        return result;
    }

    result.path = cachedPath;
    result.usedSidecar = true;
    result.statusMessage = QObject::tr("已为原始 Dolby 码流建立时间轴容器");
    PlayerLogger::log(QStringLiteral("source"),
                      QStringLiteral("preparePlaybackSource remux-ok source=%1 cache=%2")
                          .arg(filePath)
                          .arg(cachedPath));
    return result;
}

void PlaybackSourceService::prunePlaybackCache(const QString &cacheRoot) const
{
    const PlaybackCacheSettings settings = cacheSettings();
    QDir cacheDir(cacheRoot);
    const QFileInfoList cacheEntries =
        cacheDir.entryInfoList(QStringList() << QStringLiteral("*.mka"),
                               QDir::Files | QDir::Readable,
                               QDir::Time);
    pruneCacheEntries(cacheEntries,
                      settings.maxSidecars,
                      settings.maxSidecarAgeDays,
                      static_cast<qint64>(settings.maxSidecarMiB) * kBytesPerMiB,
                      QStringLiteral("sidecar"));

    const QFileInfoList rootWavEntries =
        cacheDir.entryInfoList(QStringList() << QStringLiteral("*.wav"),
                               QDir::Files | QDir::Readable,
                               QDir::Time);
    pruneCacheEntries(rootWavEntries,
                      settings.maxDiagnosticAudioFiles,
                      settings.maxDiagnosticAudioAgeDays,
                      static_cast<qint64>(settings.maxDiagnosticAudioMiB) * kBytesPerMiB,
                      QStringLiteral("diagnostic-wav"));

    QDir loopbackDir(cacheDir.filePath(QStringLiteral("loopback")));
    if (loopbackDir.exists()) {
        const QFileInfoList loopbackWavEntries =
            loopbackDir.entryInfoList(QStringList() << QStringLiteral("*.wav"),
                                      QDir::Files | QDir::Readable,
                                      QDir::Time);
        pruneCacheEntries(loopbackWavEntries,
                          settings.maxDiagnosticAudioFiles,
                          settings.maxDiagnosticAudioAgeDays,
                          static_cast<qint64>(settings.maxDiagnosticAudioMiB) * kBytesPerMiB,
                          QStringLiteral("loopback-wav"));
    }
}
