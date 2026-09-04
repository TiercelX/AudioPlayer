#ifndef WINDOWSASIOAUDIOPLAYER_DISCOVERY_H
#define WINDOWSASIOAUDIOPLAYER_DISCOVERY_H

#include <QString>
#include <QList>

#include <objbase.h>
#include <windows.h>

// IASIO is defined in asio_interface.h
#include "asio_interface.h"

namespace AsioDiscovery {

constexpr long kAsioFalse = 0;

struct AsioDriverEntry {
    QString name;
    QString clsidText;
};

struct AsioHostWindowCandidate {
    QString name;
    HWND window = nullptr;
};

struct AsioHostWindowSearch {
    DWORD processId = 0;
    HWND window = nullptr;
};

std::atomic<quintptr> &asioHostWindowHandle();

QString utf16StringFromRegistryValue(HKEY key, const wchar_t *valueName);
void appendAsioRegistryEntries(HKEY root, QList<AsioDriverEntry> *entries);
QList<AsioDriverEntry> registeredAsioDrivers();
BOOL CALLBACK enumAsioHostWindow(HWND window, LPARAM param);
HWND asioHostWindow();
QList<AsioHostWindowCandidate> asioHostWindowCandidates();
bool parseClsid(const QString &clsidText, CLSID *clsid);
IASIO *createAsioDriver(const QString &clsidText);
ASIOBool safeAsioInit(IASIO *driver, HWND hostWindow, bool *crashed);
void safeAsioRelease(IASIO *driver);

} // namespace AsioDiscovery

#endif // WINDOWSASIOAUDIOPLAYER_DISCOVERY_H
