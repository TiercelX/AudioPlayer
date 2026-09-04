#include "mainwindow_helpers.h"

#include "audioutils.h"

#include <QLocale>
#include <QObject>

namespace MainWindowHelpers {

AudioPlayerBackend::BackendId systemOutputBackendId()
{
#if defined(Q_OS_WINDOWS)
    return AudioPlayerBackend::BackendId::WindowsWasapi;
#else
    return AudioPlayerBackend::BackendId::Ffmpeg;
#endif
}

QString compactLogValue(QString value)
{
    value.replace(QChar(u'\n'), QChar(u' '));
    value.replace(QChar(u'\r'), QChar(u' '));
    return value.simplified();
}

QString formatDataSize(qint64 bytes)
{
    return QLocale().formattedDataSize(qMax<qint64>(0, bytes));
}

QString formatTransferRate(double bytesPerSecond)
{
    if (bytesPerSecond <= 0.0) {
        return QObject::tr("--");
    }

    return QObject::tr("%1/s")
        .arg(QLocale().formattedDataSize(static_cast<qint64>(bytesPerSecond)));
}

QString formatRemainingTime(qint64 milliseconds)
{
    if (milliseconds <= 0) {
        return QObject::tr("0 秒");
    }

    const qint64 totalSeconds = (milliseconds + 999) / 1000;
    const qint64 minutes = totalSeconds / 60;
    const qint64 seconds = totalSeconds % 60;
    if (minutes <= 0) {
        return QObject::tr("%1 秒").arg(totalSeconds);
    }

    return QObject::tr("%1 分 %2 秒").arg(minutes).arg(seconds);
}

} // namespace MainWindowHelpers
