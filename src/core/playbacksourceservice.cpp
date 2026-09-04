#include "playbacksourceservice.h"

#include "playbacksourceserviceinternal.h"
#include "toollocator.h"

#include "playerlogger.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QSettings>
#include <QTemporaryFile>

namespace {

constexpr auto kCacheDirectoryEnv = "AUDIOPLAYER_CACHE_DIR";
constexpr auto kSettingsOrganization = "AudioPlayer";
constexpr auto kSettingsApplication = "AudioPlayer";
constexpr auto kCacheDirectoryKey = "cache/directory";
constexpr auto kMaxSidecarsKey = "cache/maxSidecars";
constexpr auto kMaxSidecarAgeDaysKey = "cache/maxSidecarAgeDays";
constexpr auto kMaxSidecarMiBKey = "cache/maxSidecarMiB";
constexpr auto kMaxDiagnosticAudioFilesKey = "cache/maxDiagnosticAudioFiles";
constexpr auto kMaxDiagnosticAudioAgeDaysKey = "cache/maxDiagnosticAudioAgeDays";
constexpr auto kMaxDiagnosticAudioMiBKey = "cache/maxDiagnosticAudioMiB";
constexpr auto kMaxPcmCacheMiBKey = "cache/maxPcmCacheMiB";
constexpr auto kMaxPcmCacheAgeMinutesKey = "cache/maxPcmCacheAgeMinutes";

QString normalizedAbsolutePath(const QString &path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

QString configuredDirectoryPath(const QString &environmentVariable)
{
    const QString path =
        QProcessEnvironment::systemEnvironment().value(environmentVariable).trimmed();
    if (path.isEmpty()) {
        return {};
    }

    return normalizedAbsolutePath(path);
}

bool shouldUseBuildTreeCache()
{
#ifdef AUDIOPLAYER_BUILD_DIR
    const QString buildDirPath = normalizedAbsolutePath(QStringLiteral(AUDIOPLAYER_BUILD_DIR));
    const QString appDirPath = normalizedAbsolutePath(QCoreApplication::applicationDirPath());
    if (appDirPath == buildDirPath) {
        return true;
    }

    static const QStringList kBuildConfigDirectories {
        QStringLiteral("Debug"),
        QStringLiteral("Release"),
        QStringLiteral("RelWithDebInfo"),
        QStringLiteral("MinSizeRel"),
    };
    const QFileInfo appDirInfo(appDirPath);
    const QString parentPath =
        normalizedAbsolutePath(QDir(appDirPath).filePath(QStringLiteral("..")));
    return parentPath == buildDirPath && kBuildConfigDirectories.contains(appDirInfo.fileName());
#else
    return false;
#endif
}
} // namespace

namespace PlaybackSourceServiceInternal {

QString rawInputFormatForFile(const QString &filePath)
{
    const QString suffix = QFileInfo(filePath).suffix().toLower();
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

ToolProcessResult runToolProcess(const QString &program,
                                 const QStringList &arguments,
                                 int startTimeoutMs,
                                 int finishTimeoutMs)
{
    ToolProcessResult result;
    if (program.isEmpty()) {
        result.errorText = QStringLiteral("missing executable");
        return result;
    }

    QTemporaryFile standardOutputFile;
    QTemporaryFile standardErrorFile;
    if (!standardOutputFile.open() || !standardErrorFile.open()) {
        result.errorText = QStringLiteral("failed to create temporary capture files");
        return result;
    }

    const QString standardOutputPath = standardOutputFile.fileName();
    const QString standardErrorPath = standardErrorFile.fileName();
    standardOutputFile.close();
    standardErrorFile.close();

    QProcess process;
    process.setStandardInputFile(QProcess::nullDevice());
    process.setStandardOutputFile(standardOutputPath, QIODeviceBase::Truncate);
    process.setStandardErrorFile(standardErrorPath, QIODeviceBase::Truncate);
    process.start(program, arguments);
    result.started = process.waitForStarted(startTimeoutMs);
    if (!result.started) {
        result.errorText = process.errorString();
        QFile standardErrorCapture(standardErrorPath);
        if (standardErrorCapture.open(QIODevice::ReadOnly)) {
            result.standardError = QString::fromUtf8(standardErrorCapture.readAll()).trimmed();
        }
        return result;
    }

    result.finished = process.waitForFinished(finishTimeoutMs);
    if (!result.finished && process.state() != QProcess::NotRunning) {
        result.timedOut = true;
        process.kill();
        process.waitForFinished(500);
    }

    result.exitCode = process.exitCode();
    result.exitStatus = process.exitStatus();
    result.errorText = result.timedOut
        ? QStringLiteral("timed out after %1 ms").arg(finishTimeoutMs)
        : process.errorString();
    QFile standardOutputCapture(standardOutputPath);
    if (standardOutputCapture.open(QIODevice::ReadOnly)) {
        result.standardOutput = QString::fromUtf8(standardOutputCapture.readAll());
    }
    QFile standardErrorCapture(standardErrorPath);
    if (standardErrorCapture.open(QIODevice::ReadOnly)) {
        result.standardError = QString::fromUtf8(standardErrorCapture.readAll()).trimmed();
    }
    return result;
}

bool hasCoreSourceInfo(const AudioInfo &info)
{
    return !info.codecName.isEmpty() && info.channelCount > 0;
}

bool needsSupplementalSourceInfo(const AudioInfo &info)
{
    return !hasCoreSourceInfo(info) || info.durationMs <= 0;
}

void mergeMissingSourceInfo(AudioInfo *target, const AudioInfo &fallback)
{
    if (!target) {
        return;
    }

    if (target->codecName.isEmpty()) {
        target->codecName = fallback.codecName;
    }
    if (target->audioFormat.isEmpty()) {
        target->audioFormat = fallback.audioFormat;
    }
    if (target->channelCount <= 0) {
        target->channelCount = fallback.channelCount;
    }
    if (target->channels.isEmpty()) {
        target->channels = fallback.channels;
    }
    if (target->sampleRateValue <= 0) {
        target->sampleRateValue = fallback.sampleRateValue;
    }
    if (target->sampleRate.isEmpty()) {
        target->sampleRate = fallback.sampleRate;
    }
    if (target->bitDepthValue <= 0) {
        target->bitDepthValue = fallback.bitDepthValue;
    }
    if (target->bitDepth.isEmpty()) {
        target->bitDepth = fallback.bitDepth;
    }
    if (target->bitRateValue <= 0) {
        target->bitRateValue = fallback.bitRateValue;
    }
    if (target->bitRate.isEmpty()) {
        target->bitRate = fallback.bitRate;
    }
    if (target->durationMs <= 0) {
        target->durationMs = fallback.durationMs;
    }
    if (!target->atmosKnown) {
        target->atmosKnown = fallback.atmosKnown;
        target->atmosDetected = fallback.atmosDetected;
        target->status = fallback.status;
    }
}

QString formatBitRate(const QString &bitsPerSecond)
{
    bool ok = false;
    const qint64 value = bitsPerSecond.toLongLong(&ok);
    if (!ok || value <= 0) {
        return {};
    }

    return QObject::tr("%1 kbps").arg(QString::number(value / 1000.0, 'f', value >= 100000 ? 0 : 1));
}

QString formatBitDepth(int bitDepth)
{
    if (bitDepth <= 0) {
        return {};
    }

    return QObject::tr("%1-bit").arg(bitDepth);
}

QString formatChannelDescription(int channelCount, const QString &layout)
{
    if (channelCount <= 0) {
        return {};
    }

    if (layout.isEmpty()) {
        return QObject::tr("%1").arg(channelCount);
    }

    return QObject::tr("%1 (%2)").arg(channelCount).arg(layout);
}

QString formatCodecDisplay(const QString &codecName, const QString &codecLongName)
{
    const QString lowerName = codecName.toLower();
    if (lowerName == QStringLiteral("truehd")) {
        return QObject::tr("Dolby TrueHD");
    }
    if (lowerName == QStringLiteral("eac3")) {
        return QObject::tr("Dolby Digital Plus");
    }
    if (lowerName == QStringLiteral("ac3")) {
        return QObject::tr("Dolby Digital");
    }
    if (lowerName == QStringLiteral("aac")) {
        return QObject::tr("AAC");
    }
    if (lowerName == QStringLiteral("alac")) {
        return QObject::tr("ALAC");
    }
    if (lowerName == QStringLiteral("flac")) {
        return QObject::tr("FLAC");
    }
    if (lowerName == QStringLiteral("mp3")) {
        return QObject::tr("MP3");
    }
    if (lowerName.startsWith(QStringLiteral("pcm_"))) {
        return QObject::tr("PCM");
    }

    return !codecLongName.isEmpty() ? codecLongName : codecName;
}

QString formatSampleRate(int sampleRate)
{
    if (sampleRate <= 0) {
        return {};
    }

    return QObject::tr("%1 Hz").arg(sampleRate);
}

} // namespace PlaybackSourceServiceInternal

PlaybackSourceResolution PlaybackSourceService::resolveForPlayback(
    const QString &filePath,
    const AudioInfo &initialSourceInfo,
    AudioPlaybackPlan::SourceMode sourceMode) const
{
    PlaybackSourceResolution resolution;
    resolution.playbackPath = filePath;
    resolution.sourceInfo = initialSourceInfo;

    const PreparedPlaybackSource prepared = preparePlaybackSource(filePath, sourceMode);
    resolution.playbackPath = prepared.path;
    resolution.statusMessage = prepared.statusMessage;
    resolution.warningMessage = prepared.warningMessage;
    resolution.usedSidecar = prepared.usedSidecar;

    if (resolution.playbackPath != filePath
        && PlaybackSourceServiceInternal::needsSupplementalSourceInfo(resolution.sourceInfo)) {
        const AudioInfo playbackSourceInfo = probeSourceInfo(resolution.playbackPath);
        const bool beforeMergeHasCoreInfo =
            PlaybackSourceServiceInternal::hasCoreSourceInfo(resolution.sourceInfo);
        const qint64 beforeMergeDurationMs = resolution.sourceInfo.durationMs;
        PlaybackSourceServiceInternal::mergeMissingSourceInfo(&resolution.sourceInfo,
                                                              playbackSourceInfo);
        PlayerLogger::log(QStringLiteral("source"),
                          QStringLiteral("resolveForPlayback supplementalSourceInfo source=%1 playbackSource=%2 recoveredCore=%3 recoveredDuration=%4 codec=%5 channels=%6 durationMs=%7")
                              .arg(filePath)
                              .arg(resolution.playbackPath)
                              .arg(!beforeMergeHasCoreInfo
                                   && PlaybackSourceServiceInternal::hasCoreSourceInfo(
                                       resolution.sourceInfo))
                              .arg(beforeMergeDurationMs <= 0 && resolution.sourceInfo.durationMs > 0)
                              .arg(resolution.sourceInfo.codecName)
                              .arg(resolution.sourceInfo.channelCount)
                              .arg(resolution.sourceInfo.durationMs));
    }

    resolution.durationMs = resolution.sourceInfo.durationMs > 0
        ? resolution.sourceInfo.durationMs
        : probeDuration(resolution.playbackPath);
    return resolution;
}

QString PlaybackSourceService::locateFfmpegExecutable() const
{
    return AudioUtils::locateFfmpegExecutable();
}

QString PlaybackSourceService::locateFfprobeExecutable() const
{
    return AudioUtils::locateFfprobeExecutable();
}

QString PlaybackSourceService::playbackCacheRoot() const
{
    const QString configuredEnvironmentPath =
        configuredDirectoryPath(QString::fromLatin1(kCacheDirectoryEnv));
    if (!configuredEnvironmentPath.isEmpty()) {
        return configuredEnvironmentPath;
    }

    const QString configuredPath =
        QSettings(kSettingsOrganization, kSettingsApplication)
            .value(QString::fromLatin1(kCacheDirectoryKey))
            .toString()
            .trimmed();
    if (!configuredPath.isEmpty()) {
        return normalizedAbsolutePath(configuredPath);
    }

    if (shouldUseBuildTreeCache()) {
        return QDir(QStringLiteral(AUDIOPLAYER_BUILD_DIR)).filePath(QStringLiteral("cache"));
    }

    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("cache"));
}

PlaybackCacheSettings PlaybackSourceService::cacheSettings() const
{
    QSettings settings(kSettingsOrganization, kSettingsApplication);
    PlaybackCacheSettings result;
    result.cacheDirectory = playbackCacheRoot();
    result.maxSidecars = settings.value(QString::fromLatin1(kMaxSidecarsKey), result.maxSidecars).toInt();
    result.maxSidecarAgeDays =
        settings.value(QString::fromLatin1(kMaxSidecarAgeDaysKey), result.maxSidecarAgeDays).toInt();
    result.maxSidecarMiB = settings.value(QString::fromLatin1(kMaxSidecarMiBKey), result.maxSidecarMiB).toInt();
    result.maxDiagnosticAudioFiles =
        settings.value(QString::fromLatin1(kMaxDiagnosticAudioFilesKey), result.maxDiagnosticAudioFiles).toInt();
    result.maxDiagnosticAudioAgeDays =
        settings.value(QString::fromLatin1(kMaxDiagnosticAudioAgeDaysKey), result.maxDiagnosticAudioAgeDays).toInt();
    result.maxDiagnosticAudioMiB =
        settings.value(QString::fromLatin1(kMaxDiagnosticAudioMiBKey), result.maxDiagnosticAudioMiB).toInt();
    result.maxPcmCacheMiBConfigured = settings.contains(QString::fromLatin1(kMaxPcmCacheMiBKey));
    result.maxPcmCacheMiB =
        settings.value(QString::fromLatin1(kMaxPcmCacheMiBKey), result.maxPcmCacheMiB).toInt();
    result.maxPcmCacheAgeMinutes =
        settings.value(QString::fromLatin1(kMaxPcmCacheAgeMinutesKey), result.maxPcmCacheAgeMinutes).toInt();
    return result;
}

void PlaybackSourceService::saveCacheSettings(const PlaybackCacheSettings &settingsValue) const
{
    QSettings settings(kSettingsOrganization, kSettingsApplication);
    const QString cacheDirectory = settingsValue.cacheDirectory.trimmed();
    if (cacheDirectory.isEmpty()) {
        settings.remove(QString::fromLatin1(kCacheDirectoryKey));
    } else {
        settings.setValue(QString::fromLatin1(kCacheDirectoryKey),
                          normalizedAbsolutePath(cacheDirectory));
    }
    settings.setValue(QString::fromLatin1(kMaxSidecarsKey), qBound(1, settingsValue.maxSidecars, 1000));
    settings.setValue(QString::fromLatin1(kMaxSidecarAgeDaysKey),
                      qBound(1, settingsValue.maxSidecarAgeDays, 3650));
    settings.setValue(QString::fromLatin1(kMaxSidecarMiBKey), qBound(1, settingsValue.maxSidecarMiB, 102400));
    settings.setValue(QString::fromLatin1(kMaxDiagnosticAudioFilesKey),
                      qBound(1, settingsValue.maxDiagnosticAudioFiles, 1000));
    settings.setValue(QString::fromLatin1(kMaxDiagnosticAudioAgeDaysKey),
                      qBound(1, settingsValue.maxDiagnosticAudioAgeDays, 3650));
    settings.setValue(QString::fromLatin1(kMaxDiagnosticAudioMiBKey),
                      qBound(1, settingsValue.maxDiagnosticAudioMiB, 102400));
    settings.setValue(QString::fromLatin1(kMaxPcmCacheMiBKey),
                      qBound(0, settingsValue.maxPcmCacheMiB, 4096));
    settings.setValue(QString::fromLatin1(kMaxPcmCacheAgeMinutesKey),
                      qBound(1, settingsValue.maxPcmCacheAgeMinutes, 1440));
}
