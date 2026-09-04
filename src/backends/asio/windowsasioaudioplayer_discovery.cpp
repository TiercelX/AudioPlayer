#include "windowsasioaudioplayer_discovery.h"

#include "asio_interface.h"
#include "playerlogger.h"

#include <algorithm>

namespace AsioDiscovery {

static std::atomic<quintptr> g_asioHostWindowHandle = 0;

std::atomic<quintptr> &asioHostWindowHandle()
{
    return g_asioHostWindowHandle;
}

QString utf16StringFromRegistryValue(HKEY key, const wchar_t *valueName)
{
    DWORD type = 0;
    DWORD byteCount = 0;
    if (RegQueryValueExW(key, valueName, nullptr, &type, nullptr, &byteCount) != ERROR_SUCCESS
        || (type != REG_SZ && type != REG_EXPAND_SZ)
        || byteCount < sizeof(wchar_t)) {
        return {};
    }

    std::wstring buffer(byteCount / sizeof(wchar_t), L'\0');
    if (RegQueryValueExW(key,
                         valueName,
                         nullptr,
                         &type,
                         reinterpret_cast<LPBYTE>(buffer.data()),
                         &byteCount)
        != ERROR_SUCCESS) {
        return {};
    }

    while (!buffer.empty() && buffer.back() == L'\0') {
        buffer.pop_back();
    }
    return QString::fromWCharArray(buffer.c_str(), static_cast<int>(buffer.size())).trimmed();
}

void appendAsioRegistryEntries(HKEY root, QList<AsioDriverEntry> *entries)
{
    HKEY asioRoot = nullptr;
    if (RegOpenKeyExW(root, L"SOFTWARE\\ASIO", 0, KEY_READ, &asioRoot) != ERROR_SUCCESS) {
        return;
    }

    DWORD index = 0;
    wchar_t subkeyName[256] = {};
    DWORD subkeyNameLength = std::size(subkeyName);
    while (RegEnumKeyExW(asioRoot,
                         index,
                         subkeyName,
                         &subkeyNameLength,
                         nullptr,
                         nullptr,
                         nullptr,
                         nullptr)
           == ERROR_SUCCESS) {
        HKEY driverKey = nullptr;
        if (RegOpenKeyExW(asioRoot, subkeyName, 0, KEY_READ, &driverKey) == ERROR_SUCCESS) {
            const QString clsidText = utf16StringFromRegistryValue(driverKey, L"CLSID");
            if (!clsidText.isEmpty()) {
                entries->append({
                    QString::fromWCharArray(subkeyName).trimmed(),
                    clsidText,
                });
            }
            RegCloseKey(driverKey);
        }

        ++index;
        subkeyName[0] = L'\0';
        subkeyNameLength = std::size(subkeyName);
    }

    RegCloseKey(asioRoot);
}

QList<AsioDriverEntry> registeredAsioDrivers()
{
    QList<AsioDriverEntry> entries;
    appendAsioRegistryEntries(HKEY_CURRENT_USER, &entries);
    appendAsioRegistryEntries(HKEY_LOCAL_MACHINE, &entries);

    QList<AsioDriverEntry> uniqueEntries;
    for (const AsioDriverEntry &entry : entries) {
        const bool duplicate = std::any_of(uniqueEntries.cbegin(),
                                           uniqueEntries.cend(),
                                           [&entry](const AsioDriverEntry &existing) {
                                               return existing.clsidText.compare(entry.clsidText, Qt::CaseInsensitive) == 0;
                                           });
        if (!duplicate) {
            uniqueEntries.append(entry);
        }
    }
    return uniqueEntries;
}

BOOL CALLBACK enumAsioHostWindow(HWND window, LPARAM param)
{
    auto *search = reinterpret_cast<AsioHostWindowSearch *>(param);
    DWORD windowProcessId = 0;
    GetWindowThreadProcessId(window, &windowProcessId);
    if (windowProcessId == search->processId && IsWindowVisible(window)) {
        search->window = window;
        return FALSE;
    }
    return TRUE;
}

HWND asioHostWindow()
{
    const quintptr configuredHostWindow = asioHostWindowHandle().load(std::memory_order_relaxed);
    if (configuredHostWindow != 0) {
        HWND window = reinterpret_cast<HWND>(configuredHostWindow);
        if (IsWindow(window)) {
            return window;
        }
    }

    AsioHostWindowSearch search;
    search.processId = GetCurrentProcessId();
    EnumWindows(enumAsioHostWindow, reinterpret_cast<LPARAM>(&search));
    if (search.window) {
        return search.window;
    }

    HWND foregroundWindow = GetForegroundWindow();
    DWORD foregroundProcessId = 0;
    GetWindowThreadProcessId(foregroundWindow, &foregroundProcessId);
    if (foregroundWindow && foregroundProcessId == search.processId) {
        return foregroundWindow;
    }

    return GetDesktopWindow();
}

QList<AsioHostWindowCandidate> asioHostWindowCandidates()
{
    QList<AsioHostWindowCandidate> candidates;
    auto appendUnique = [&candidates](const QString &name, HWND window) {
        if (!window) {
            return;
        }
        const bool duplicate = std::any_of(candidates.cbegin(),
                                           candidates.cend(),
                                           [window](const AsioHostWindowCandidate &candidate) {
                                               return candidate.window == window;
                                           });
        if (!duplicate) {
            candidates.append({name, window});
        }
    };

    // Create a message window for ASIO drivers that need a dedicated window
    static HWND s_messageWindow = nullptr;
    if (!s_messageWindow) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.lpszClassName = L"AsioMessageWindow";
        RegisterClassW(&wc);
        s_messageWindow = CreateWindowExW(0, L"AsioMessageWindow", L"ASIO Host",
                                          0, 0, 0, 1, 1, HWND_MESSAGE, nullptr,
                                          GetModuleHandle(nullptr), nullptr);
    }

    appendUnique(QStringLiteral("app-window"), asioHostWindow());
    appendUnique(QStringLiteral("desktop"), GetDesktopWindow());
    if (s_messageWindow) {
        appendUnique(QStringLiteral("message-window"), s_messageWindow);
    }
    candidates.append({QStringLiteral("null-handle"), nullptr});
    return candidates;
}

bool parseClsid(const QString &clsidText, CLSID *clsid)
{
    const std::wstring wideText = clsidText.toStdWString();
    return CLSIDFromString(wideText.c_str(), clsid) == S_OK;
}

IASIO *createAsioDriver(const QString &clsidText)
{
    CLSID clsid;
    if (!parseClsid(clsidText, &clsid)) {
        return nullptr;
    }

    IASIO *driver = nullptr;
    const HRESULT hr = CoCreateInstance(clsid,
                                        nullptr,
                                        CLSCTX_INPROC_SERVER,
                                        clsid,
                                        reinterpret_cast<void **>(&driver));
    if (SUCCEEDED(hr)) {
        return driver;
    }

    PlayerLogger::log(QStringLiteral("audio"),
                      QStringLiteral("asio CoCreateInstance failed clsid=%1 hr=0x%2")
                          .arg(clsidText)
                          .arg(static_cast<qulonglong>(hr), 8, 16, QLatin1Char('0')));
    return nullptr;
}

ASIOBool safeAsioInit(IASIO *driver, HWND hostWindow, bool *crashed)
{
    if (crashed) {
        *crashed = false;
    }
    __try {
        return driver ? driver->init(hostWindow) : kAsioFalse;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (crashed) {
            *crashed = true;
        }
        return kAsioFalse;
    }
}

void safeAsioRelease(IASIO *driver)
{
    if (!driver) {
        return;
    }
    __try {
        driver->Release();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

} // namespace AsioDiscovery
