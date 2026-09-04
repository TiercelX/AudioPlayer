#include "windowsasioaudioplayer_sessionprobe.h"

#include "windowsasioaudioplayer_discovery.h"
#include "playerlogger.h"

#include <QByteArray>
#include <QList>
#include <QString>

#include <algorithm>

#include <objbase.h>
#include <windows.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <propsys.h>

// PKEY_AudioEndpoint_FriendlyName — defined locally to avoid pulling in
// functiondiscoverykeys_devpkey.h which requires DEFINE_PROPERTYKEY.
static const PROPERTYKEY PKEY_AudioEndpoint_FriendlyName = {
    { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } },
    14
};

// PKEY_AudioEndpoint_FormFactor — endpoint form factor (EndpointFormFactor enum).
static const PROPERTYKEY PKEY_AudioEndpoint_FormFactor = {
    { 0x1da5d803, 0xd492, 0x4edd, { 0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x00 } },
    0
};

namespace AsioSessionProbe {

namespace {

QString audioSessionStateName(AudioSessionState state)
{
    switch (state) {
    case AudioSessionStateInactive:
        return QStringLiteral("Inactive");
    case AudioSessionStateActive:
        return QStringLiteral("Active");
    case AudioSessionStateExpired:
        return QStringLiteral("Expired");
    }

    return QStringLiteral("Unknown");
}

} // anonymous namespace

bool isCreativeAsioDriver(const QString &driverIdText)
{
    return driverIdText.contains(QStringLiteral("Creative"), Qt::CaseInsensitive)
        || driverIdText.contains(QStringLiteral("Sound Blaster"), Qt::CaseInsensitive)
        || driverIdText.contains(QStringLiteral("BlasterX"), Qt::CaseInsensitive);
}

QByteArray resolveWasapiEndpointForAsioDriver(const QByteArray &asioDriverId)
{
    const QString driverIdText = QString::fromUtf8(asioDriverId);
    // Check the ASIO registry name (not the CLSID) for known keywords.
    QString asioDriverName;
    bool isCreative = false;
    bool isRealtek = false;
    for (const AsioDiscovery::AsioDriverEntry &entry : AsioDiscovery::registeredAsioDrivers()) {
        if (entry.clsidText.compare(driverIdText, Qt::CaseInsensitive) == 0) {
            asioDriverName = entry.name;
            if (entry.name.contains(QStringLiteral("Creative"), Qt::CaseInsensitive)
                || entry.name.contains(QStringLiteral("Sound Blaster"), Qt::CaseInsensitive)
                || entry.name.contains(QStringLiteral("BlasterX"), Qt::CaseInsensitive)) {
                isCreative = true;
            }
            if (entry.name.contains(QStringLiteral("Realtek"), Qt::CaseInsensitive)) {
                isRealtek = true;
            }
            break;
        }
    }
    if (!isCreative && !isRealtek) {
        return {};
    }

    IMMDeviceEnumerator *enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void **>(&enumerator));
    if (FAILED(hr) || !enumerator) {
        return {};
    }

    IMMDeviceCollection *collection = nullptr;
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    enumerator->Release();
    if (FAILED(hr) || !collection) {
        return {};
    }

    struct Candidate {
        QByteArray endpointId;
        QString friendlyName;
        int formFactor;
    };
    QList<Candidate> candidates;

    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        IMMDevice *device = nullptr;
        if (FAILED(collection->Item(i, &device)) || !device) {
            continue;
        }

        IPropertyStore *props = nullptr;
        if (FAILED(device->OpenPropertyStore(STGM_READ, &props))) {
            device->Release();
            continue;
        }

        PROPVARIANT nameVar;
        PropVariantInit(&nameVar);
        if (SUCCEEDED(props->GetValue(PKEY_AudioEndpoint_FriendlyName, &nameVar)) && nameVar.vt == VT_LPWSTR) {
            const QString friendlyName = QString::fromWCharArray(nameVar.pwszVal);
            PropVariantClear(&nameVar);

            bool matched = false;
            if (isCreative
                && (friendlyName.contains(QStringLiteral("Creative"), Qt::CaseInsensitive)
                    || friendlyName.contains(QStringLiteral("Sound Blaster"), Qt::CaseInsensitive)
                    || friendlyName.contains(QStringLiteral("BlasterX"), Qt::CaseInsensitive)
                    || friendlyName.contains(QStringLiteral("G5"), Qt::CaseInsensitive))) {
                matched = true;
            }
            if (isRealtek
                && friendlyName.contains(QStringLiteral("Realtek"), Qt::CaseInsensitive)) {
                matched = true;
            }

            if (matched) {
                LPWSTR endpointId = nullptr;
                if (SUCCEEDED(device->GetId(&endpointId)) && endpointId) {
                    Candidate c;
                    c.endpointId = QString::fromWCharArray(endpointId).toUtf8();
                    c.friendlyName = friendlyName;
                    CoTaskMemFree(endpointId);

                    PROPVARIANT ffVar;
                    PropVariantInit(&ffVar);
                    if (SUCCEEDED(props->GetValue(PKEY_AudioEndpoint_FormFactor, &ffVar)) && ffVar.vt == VT_UI4) {
                        c.formFactor = static_cast<int>(ffVar.ulVal);
                    } else {
                        c.formFactor = -1;
                    }
                    PropVariantClear(&ffVar);

                    candidates.append(c);
                }
            }
        } else {
            PropVariantClear(&nameVar);
        }
        props->Release();
        device->Release();
    }
    collection->Release();

    if (candidates.isEmpty()) {
        return {};
    }

    // Prefer primary analog outputs over digital passthrough.
    // Speakers > Headphones > Headset > LineLevel > Unknown > SPDIF > DigitalAudioDisplayDevice
    std::sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) {
        auto priority = [](int ff) -> int {
            switch (ff) {
            case 1:  return 0; // Speakers
            case 3:  return 1; // Headphones
            case 5:  return 2; // Headset
            case 2:  return 3; // LineLevel
            case 8:  return 5; // SPDIF
            case 9:  return 6; // DigitalAudioDisplayDevice
            case -1: return 4; // Unknown (property missing)
            case 0:  return 4; // UnknownFormFactor
            default: return 4; // Other
            }
        };
        return priority(a.formFactor) < priority(b.formFactor);
    });

    for (const Candidate &c : candidates) {
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("asio resolveCandidate endpoint=%1 name=%2 formFactor=%3")
                              .arg(QString::fromUtf8(c.endpointId), c.friendlyName)
                              .arg(c.formFactor));
    }

    const QByteArray selected = candidates.first().endpointId;
    PlayerLogger::log(QStringLiteral("audio"),
                      QStringLiteral("asio resolvedWasapiEndpoint driverId=%1 endpoint=%2 name=%3 formFactor=%4")
                          .arg(driverIdText, QString::fromUtf8(selected),
                               candidates.first().friendlyName)
                          .arg(candidates.first().formFactor));
    return selected;
}

bool isAudioEndpointBusy(const QByteArray &asioDriverId)
{
    const QByteArray wasapiEndpointId = resolveWasapiEndpointForAsioDriver(asioDriverId);
    if (wasapiEndpointId.isEmpty()) {
        return false;
    }

    const QString endpointIdWide = QString::fromUtf8(wasapiEndpointId);
    IMMDeviceEnumerator *enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void **>(&enumerator));
    if (FAILED(hr) || !enumerator) {
        return false;
    }

    IMMDevice *device = nullptr;
    hr = enumerator->GetDevice(reinterpret_cast<LPCWSTR>(endpointIdWide.utf16()), &device);
    enumerator->Release();
    if (FAILED(hr) || !device) {
        return false;
    }

    IAudioClient *client = nullptr;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void **>(&client));
    device->Release();
    if (FAILED(hr) || !client) {
        return false;
    }

    WAVEFORMATEX *mixFormat = nullptr;
    hr = client->GetMixFormat(&mixFormat);
    if (FAILED(hr) || !mixFormat) {
        client->Release();
        return false;
    }

    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 0, 0, mixFormat, nullptr);
    CoTaskMemFree(mixFormat);
    client->Release();
    device->Release();

    if (hr == AUDCLNT_E_DEVICE_IN_USE || hr == AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED) {
        return true;
    }
    return false;
}

WasapiSessionCheckResult checkWasapiSessionsForEndpoint(const QByteArray &asioDriverId)
{
    WasapiSessionCheckResult result;
    const QByteArray wasapiEndpointId = resolveWasapiEndpointForAsioDriver(asioDriverId);
    if (wasapiEndpointId.isEmpty()) {
        return result;
    }
    result.endpointId = QString::fromUtf8(wasapiEndpointId);
    result.endpointName = QStringLiteral("(resolved)");

    IMMDeviceEnumerator *enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void **>(&enumerator));
    if (FAILED(hr) || !enumerator) {
        return result;
    }

    IMMDevice *device = nullptr;
    hr = enumerator->GetDevice(reinterpret_cast<LPCWSTR>(result.endpointId.utf16()), &device);
    enumerator->Release();
    if (FAILED(hr) || !device) {
        return result;
    }

    IAudioSessionManager2 *sessionManager = nullptr;
    hr = device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void **>(&sessionManager));
    device->Release();
    if (FAILED(hr) || !sessionManager) {
        return result;
    }

    IAudioSessionEnumerator *sessionEnumerator = nullptr;
    hr = sessionManager->GetSessionEnumerator(&sessionEnumerator);
    sessionManager->Release();
    if (FAILED(hr) || !sessionEnumerator) {
        return result;
    }

    int sessionCount = 0;
    sessionEnumerator->GetCount(&sessionCount);
    result.totalSessionCount = sessionCount;

    const DWORD currentProcessId = GetCurrentProcessId();
    for (int i = 0; i < sessionCount; ++i) {
        IAudioSessionControl *session = nullptr;
        if (FAILED(sessionEnumerator->GetSession(i, &session)) || !session) {
            continue;
        }

        IAudioSessionControl2 *session2 = nullptr;
        if (FAILED(session->QueryInterface(__uuidof(IAudioSessionControl2),
                                           reinterpret_cast<void **>(&session2)))) {
            session->Release();
            continue;
        }

        DWORD processId = 0;
        session2->GetProcessId(&processId);

        AudioSessionState state = AudioSessionStateInactive;
        session->GetState(&state);

        // Ignore pid=0 (system sessions) and current process sessions
        if (processId != currentProcessId && processId != 0) {
            ++result.externalCount;
            result.hasExternal = true;
            if (!result.sessionDetails.isEmpty()) {
                result.sessionDetails.append(QLatin1Char(','));
            }
            result.sessionDetails.append(QStringLiteral("pid=%1 state=%2")
                                             .arg(processId)
                                             .arg(audioSessionStateName(state)));
            if (state == AudioSessionStateActive) {
                ++result.activeExternalCount;
                result.hasActiveExternal = true;
            }
        }

        session2->Release();
        session->Release();
    }
    sessionEnumerator->Release();

    return result;
}

bool hasExternalWasapiRenderSessionsForAsioDriver(const QByteArray &driverId, bool includeInactive)
{
    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool comInitializedHere = SUCCEEDED(comHr) && comHr != RPC_E_CHANGED_MODE;
    const WasapiSessionCheckResult sessionCheck = checkWasapiSessionsForEndpoint(driverId);
    if (!sessionCheck.endpointId.isEmpty()) {
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("asio wasapiSessionCheck %1 driverId=%2 endpoint=%3 name=%4 activeCount=%5 externalCount=%6 sessions=%7 includeInactive=%8")
                              .arg((includeInactive ? sessionCheck.hasExternal : sessionCheck.hasActiveExternal)
                                       ? QStringLiteral("external") : QStringLiteral("clear"))
                              .arg(QString::fromUtf8(driverId),
                                   sessionCheck.endpointId,
                                   sessionCheck.endpointName)
                              .arg(sessionCheck.activeExternalCount)
                              .arg(sessionCheck.externalCount)
                              .arg(sessionCheck.sessionDetails.isEmpty()
                                       ? QStringLiteral("none") : sessionCheck.sessionDetails)
                              .arg(includeInactive ? 1 : 0));
    }

    if (comInitializedHere) {
        CoUninitialize();
    }
    return includeInactive ? sessionCheck.hasExternal : sessionCheck.hasActiveExternal;
}

bool hasActiveExternalWasapiRenderSessionsForAsioDriver(const QByteArray &driverId)
{
    return hasExternalWasapiRenderSessionsForAsioDriver(driverId, false);
}

bool hasAnyExternalWasapiRenderSessionsForAsioDriver(const QByteArray &driverId)
{
    return hasExternalWasapiRenderSessionsForAsioDriver(driverId, true);
}

MultiDeviceCheckResult detectMultiplePhysicalDevicesForAsioDriver(const QByteArray &asioDriverId)
{
    MultiDeviceCheckResult result;
    const QString driverIdText = QString::fromUtf8(asioDriverId);

    QString asioDriverName;
    bool isCreative = false;
    bool isRealtek = false;
    for (const AsioDiscovery::AsioDriverEntry &entry : AsioDiscovery::registeredAsioDrivers()) {
        if (entry.clsidText.compare(driverIdText, Qt::CaseInsensitive) == 0) {
            asioDriverName = entry.name;
            if (entry.name.contains(QStringLiteral("Creative"), Qt::CaseInsensitive)
                || entry.name.contains(QStringLiteral("Sound Blaster"), Qt::CaseInsensitive)
                || entry.name.contains(QStringLiteral("BlasterX"), Qt::CaseInsensitive)) {
                isCreative = true;
                result.vendorLabel = QStringLiteral("Creative");
            }
            if (entry.name.contains(QStringLiteral("Realtek"), Qt::CaseInsensitive)) {
                isRealtek = true;
                result.vendorLabel = QStringLiteral("Realtek");
            }
            break;
        }
    }
    if (!isCreative && !isRealtek) {
        return result;
    }

    IMMDeviceEnumerator *enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void **>(&enumerator));
    if (FAILED(hr) || !enumerator) {
        return result;
    }

    IMMDeviceCollection *collection = nullptr;
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    enumerator->Release();
    if (FAILED(hr) || !collection) {
        return result;
    }

    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        IMMDevice *device = nullptr;
        if (FAILED(collection->Item(i, &device)) || !device) {
            continue;
        }

        IPropertyStore *props = nullptr;
        if (FAILED(device->OpenPropertyStore(STGM_READ, &props))) {
            device->Release();
            continue;
        }

        PROPVARIANT nameVar;
        PropVariantInit(&nameVar);
        if (SUCCEEDED(props->GetValue(PKEY_AudioEndpoint_FriendlyName, &nameVar)) && nameVar.vt == VT_LPWSTR) {
            const QString friendlyName = QString::fromWCharArray(nameVar.pwszVal);
            PropVariantClear(&nameVar);

            bool matched = false;
            if (isCreative
                && (friendlyName.contains(QStringLiteral("Creative"), Qt::CaseInsensitive)
                    || friendlyName.contains(QStringLiteral("Sound Blaster"), Qt::CaseInsensitive)
                    || friendlyName.contains(QStringLiteral("BlasterX"), Qt::CaseInsensitive)
                    || friendlyName.contains(QStringLiteral("G5"), Qt::CaseInsensitive)
                    || friendlyName.contains(QStringLiteral("G6"), Qt::CaseInsensitive))) {
                matched = true;
            }
            if (isRealtek
                && friendlyName.contains(QStringLiteral("Realtek"), Qt::CaseInsensitive)) {
                matched = true;
            }

            if (matched) {
                result.deviceNames.append(friendlyName);
            }
        } else {
            PropVariantClear(&nameVar);
        }
        props->Release();
        device->Release();
    }
    collection->Release();

    result.multipleDetected = result.deviceNames.size() > 1;

    PlayerLogger::log(QStringLiteral("audio"),
                      QStringLiteral("asio multiDeviceCheck driverId=%1 vendor=%2 count=%3 multiple=%4 devices=%5")
                          .arg(driverIdText, result.vendorLabel)
                          .arg(result.deviceNames.size())
                          .arg(result.multipleDetected ? 1 : 0)
                          .arg(result.deviceNames.join(QLatin1String(", "))));

    return result;
}

} // namespace AsioSessionProbe
