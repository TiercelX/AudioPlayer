#include "toollocator.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QStringList>

namespace {

constexpr auto kFfmpegPathOverrideEnv = "AUDIOPLAYER_FFMPEG_PATH";
constexpr auto kFfprobePathOverrideEnv = "AUDIOPLAYER_FFPROBE_PATH";

} // namespace

namespace AudioUtils {

QString toolExecutableOverride(const QString &environmentVariable)
{
    const QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    if (!environment.contains(environmentVariable)) {
        return {};
    }

    const QString overrideValue = environment.value(environmentVariable).trimmed();
    if (overrideValue.compare(QStringLiteral("disabled"), Qt::CaseInsensitive) == 0
        || overrideValue.compare(QStringLiteral("none"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("");
    }

    return overrideValue;
}

namespace {

QString resolveToolExecutable(const QString &environmentVariable,
                              const QString &baseName)
{
    const QString overrideValue = toolExecutableOverride(environmentVariable);
    if (!overrideValue.isNull() && overrideValue.isEmpty()) {
        return {};
    }

    const QStringList bundledNames = {
#ifdef Q_OS_WIN
        baseName + QStringLiteral(".exe"),
#endif
        baseName,
    };
    const QDir applicationDir(QCoreApplication::applicationDirPath());
    for (const QString &name : bundledNames) {
        const QString candidate = applicationDir.filePath(name);
        if (QFileInfo(candidate).isFile()) {
            return candidate;
        }
    }

    if (!overrideValue.isNull()) {
        const QFileInfo overrideInfo(overrideValue);
        if (overrideInfo.isDir()) {
            const QDir overrideDir(overrideInfo.absoluteFilePath());
            for (const QString &name : bundledNames) {
                const QString candidate = overrideDir.filePath(name);
                if (QFileInfo(candidate).isFile()) {
                    return candidate;
                }
            }
        } else if (overrideInfo.isFile()) {
            return overrideInfo.absoluteFilePath();
        }
    }

    return QStandardPaths::findExecutable(baseName);
}

} // namespace

QString locateFfmpegExecutable()
{
    return resolveToolExecutable(QString::fromLatin1(kFfmpegPathOverrideEnv),
                                 QStringLiteral("ffmpeg"));
}

QString locateFfprobeExecutable()
{
    return resolveToolExecutable(QString::fromLatin1(kFfprobePathOverrideEnv),
                                 QStringLiteral("ffprobe"));
}

} // namespace AudioUtils
