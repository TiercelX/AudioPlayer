#ifndef WINDOWSASIOAUDIOPLAYER_SESSIONPROBE_H
#define WINDOWSASIOAUDIOPLAYER_SESSIONPROBE_H

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace AsioSessionProbe {

struct MultiDeviceCheckResult {
    bool multipleDetected = false;
    QString vendorLabel;
    QStringList deviceNames;
};

struct WasapiSessionCheckResult {
    bool hasActiveExternal = false;
    bool hasExternal = false;
    int activeExternalCount = 0;
    int externalCount = 0;
    int totalSessionCount = 0;
    QString endpointName;
    QString endpointId;
    QString sessionDetails;
};

bool isCreativeAsioDriver(const QString &driverIdText);
QByteArray resolveWasapiEndpointForAsioDriver(const QByteArray &asioDriverId);
bool isAudioEndpointBusy(const QByteArray &asioDriverId);
WasapiSessionCheckResult checkWasapiSessionsForEndpoint(const QByteArray &asioDriverId);
bool hasExternalWasapiRenderSessionsForAsioDriver(const QByteArray &driverId, bool includeInactive);
bool hasActiveExternalWasapiRenderSessionsForAsioDriver(const QByteArray &driverId);
bool hasAnyExternalWasapiRenderSessionsForAsioDriver(const QByteArray &driverId);
MultiDeviceCheckResult detectMultiplePhysicalDevicesForAsioDriver(const QByteArray &asioDriverId);

} // namespace AsioSessionProbe

#endif // WINDOWSASIOAUDIOPLAYER_SESSIONPROBE_H
