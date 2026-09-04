#include "playerlogger.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileInfoList>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QMutex>
#include <QMutexLocker>
#include <QProcessEnvironment>
#include <QTextStream>
#include <QThread>
#include <QVariant>

namespace PlayerLogger {

namespace {

constexpr int kMaxLogFiles = 20;
constexpr auto kCacheDirectoryEnv = "AUDIOPLAYER_CACHE_DIR";
constexpr auto kLogDirectoryEnv = "AUDIOPLAYER_LOG_DIR";
constexpr auto kHighVolumeJsonlDiagnosticsEnv = "AUDIOPLAYER_HIGH_VOLUME_JSONL_DIAGNOSTICS";

QMutex &logMutex()
{
    static QMutex mutex;
    return mutex;
}

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

QString resolveLogDirectory()
{
    const QString configuredLogDir =
        configuredDirectoryPath(QString::fromLatin1(kLogDirectoryEnv));
    if (!configuredLogDir.isEmpty()) {
        return configuredLogDir;
    }

    const QString configuredCacheDir =
        configuredDirectoryPath(QString::fromLatin1(kCacheDirectoryEnv));
    if (!configuredCacheDir.isEmpty()) {
        return QDir(configuredCacheDir).filePath(QStringLiteral("logs"));
    }

    if (shouldUseBuildTreeCache()) {
        return QDir(QStringLiteral(AUDIOPLAYER_BUILD_DIR)).filePath(QStringLiteral("cache/logs"));
    }

    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("cache/logs"));
}

QString threadToken()
{
    return QStringLiteral("0x%1").arg(reinterpret_cast<quintptr>(QThread::currentThreadId()), 0, 16);
}

QString configuredLogFilePath()
{
    const QString path =
        QProcessEnvironment::systemEnvironment().value(QStringLiteral("AUDIOPLAYER_LOG_FILE")).trimmed();
    if (path.isEmpty()) {
        return {};
    }

    return normalizedAbsolutePath(path);
}

bool envFlagEnabled(const QString &value)
{
    const QString trimmed = value.trimmed();
    return trimmed == QStringLiteral("1")
        || trimmed.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0
        || trimmed.compare(QStringLiteral("on"), Qt::CaseInsensitive) == 0
        || trimmed.compare(QStringLiteral("yes"), Qt::CaseInsensitive) == 0;
}

QString buildRunLogFilePath()
{
    const QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz"));
    return QDir(resolveLogDirectory()).filePath(
        QStringLiteral("player-%1-%2.log").arg(timestamp).arg(QCoreApplication::applicationPid()));
}

QString &currentLogFilePath()
{
    static QString path;
    return path;
}

QString &currentDiagnosticLogFilePath()
{
    static QString path;
    return path;
}

QString buildDiagnosticLogFilePath(const QString &textLogPath)
{
    QFileInfo info(textLogPath);
    const QString suffix = info.suffix();
    if (!suffix.isEmpty()) {
        QString baseName = info.completeBaseName();
        return QDir(info.absolutePath()).filePath(QStringLiteral("%1.jsonl").arg(baseName));
    }

    return textLogPath + QStringLiteral(".jsonl");
}

QString archiveDirectoryPathFor(const QFileInfo &fileInfo)
{
    QDateTime timestamp = fileInfo.lastModified();
    if (!timestamp.isValid()) {
        timestamp = QDateTime::currentDateTime();
    }

    return QDir(resolveLogDirectory()).filePath(
        QStringLiteral("archive/%1").arg(timestamp.toString(QStringLiteral("yyyyMM"))));
}

QString uniqueArchiveFilePath(const QDir &archiveDir, const QString &fileName)
{
    QString candidate = archiveDir.filePath(fileName);
    if (!QFileInfo::exists(candidate)) {
        return candidate;
    }

    const QFileInfo fileNameInfo(fileName);
    const QString suffix = fileNameInfo.suffix();
    const QString suffixText =
        suffix.isEmpty() ? QString() : QStringLiteral(".%1").arg(suffix);
    const QString archiveToken =
        QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMddHHmmsszzz"));
    for (int index = 1; ; ++index) {
        candidate = archiveDir.filePath(QStringLiteral("%1-archived-%2-%3%4")
                                            .arg(fileNameInfo.completeBaseName())
                                            .arg(archiveToken)
                                            .arg(index)
                                            .arg(suffixText));
        if (!QFileInfo::exists(candidate)) {
            return candidate;
        }
    }
}

void archiveFile(const QFileInfo &fileInfo, const QDir &archiveDir)
{
    if (!fileInfo.exists() || !fileInfo.isFile()) {
        return;
    }

    const QString sourcePath = fileInfo.absoluteFilePath();
    const QString destinationPath = uniqueArchiveFilePath(archiveDir, fileInfo.fileName());
    if (QFile::rename(sourcePath, destinationPath)) {
        return;
    }

    if (QFile::copy(sourcePath, destinationPath)) {
        QFile::remove(sourcePath);
    }
}

void archiveOldLog(const QFileInfo &logFileInfo)
{
    const QString archivePath = archiveDirectoryPathFor(logFileInfo);
    QDir().mkpath(archivePath);
    const QDir archiveDir(archivePath);

    archiveFile(QFileInfo(buildDiagnosticLogFilePath(logFileInfo.absoluteFilePath())), archiveDir);
    archiveFile(logFileInfo, archiveDir);
}

void archiveOldLogs()
{
    QDir dir(resolveLogDirectory());
    const QFileInfoList logFiles = dir.entryInfoList(
        {QStringLiteral("player-*.log")},
        QDir::Files | QDir::Readable,
        QDir::Time | QDir::Reversed);

    const int excessCount = logFiles.size() - kMaxLogFiles;
    for (int index = 0; index < excessCount; ++index) {
        archiveOldLog(logFiles.at(index));
    }
}

void ensureInitialized()
{
    if (!currentLogFilePath().isEmpty()) {
        return;
    }

    const QString configuredLogPath = configuredLogFilePath();
    if (!configuredLogPath.isEmpty()) {
        QDir().mkpath(QFileInfo(configuredLogPath).absolutePath());
        currentLogFilePath() = configuredLogPath;
        currentDiagnosticLogFilePath() = buildDiagnosticLogFilePath(currentLogFilePath());
        return;
    }

    QDir().mkpath(resolveLogDirectory());
    currentLogFilePath() = buildRunLogFilePath();
    currentDiagnosticLogFilePath() = buildDiagnosticLogFilePath(currentLogFilePath());
    archiveOldLogs();
}

} // namespace

QString logFilePath()
{
    QMutexLocker locker(&logMutex());
    ensureInitialized();
    return currentLogFilePath();
}

QString diagnosticLogFilePath()
{
    QMutexLocker locker(&logMutex());
    ensureInitialized();
    return currentDiagnosticLogFilePath();
}

bool highVolumeJsonlDiagnosticsEnabled()
{
    static const bool enabled = envFlagEnabled(
        QProcessEnvironment::systemEnvironment()
            .value(QString::fromLatin1(kHighVolumeJsonlDiagnosticsEnv)));
    return enabled;
}

void log(const QString &category, const QString &message)
{
    QMutexLocker locker(&logMutex());
    ensureInitialized();

    QFile file(currentLogFilePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return;
    }

    QTextStream stream(&file);
    stream << QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"))
           << " [" << category << "]"
           << " [tid=" << threadToken() << "] "
           << message << '\n';
    stream.flush();

    QFile diagnosticFile(currentDiagnosticLogFilePath());
    if (!diagnosticFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return;
    }

    QJsonObject object;
    object.insert(QStringLiteral("timestamp"),
                  QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    object.insert(QStringLiteral("category"), category);
    object.insert(QStringLiteral("event"), QStringLiteral("log"));
    object.insert(QStringLiteral("threadId"), threadToken());
    object.insert(QStringLiteral("message"), message);
    diagnosticFile.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
    diagnosticFile.write("\n");
}

void diagnostic(const QString &category,
                const QString &eventName,
                const QVariantMap &fields)
{
    QMutexLocker locker(&logMutex());
    ensureInitialized();

    QFile file(currentDiagnosticLogFilePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return;
    }

    QJsonObject object;
    object.insert(QStringLiteral("timestamp"),
                  QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    object.insert(QStringLiteral("category"), category);
    object.insert(QStringLiteral("event"), eventName);
    object.insert(QStringLiteral("threadId"), threadToken());

    for (auto it = fields.cbegin(); it != fields.cend(); ++it) {
        object.insert(it.key(), QJsonValue::fromVariant(it.value()));
    }

    file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
    file.write("\n");
}

} // namespace PlayerLogger
