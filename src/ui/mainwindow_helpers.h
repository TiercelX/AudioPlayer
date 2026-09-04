#ifndef MAINWINDOW_HELPERS_H
#define MAINWINDOW_HELPERS_H

#include "audioplayerbackend.h"

#include <QString>
#include <QtGlobal>

namespace MainWindowHelpers {

constexpr auto kSettingsOrganization = "AudioPlayer";
constexpr auto kSettingsApplication = "AudioPlayer";
constexpr auto kLastDirectoryKey = "player/lastDirectory";

AudioPlayerBackend::BackendId systemOutputBackendId();
QString compactLogValue(QString value);
QString formatDataSize(qint64 bytes);
QString formatTransferRate(double bytesPerSecond);
QString formatRemainingTime(qint64 milliseconds);

} // namespace MainWindowHelpers

#endif // MAINWINDOW_HELPERS_H
