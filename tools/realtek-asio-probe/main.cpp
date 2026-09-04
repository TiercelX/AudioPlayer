// Realtek ASIO driver diagnostic probe tool
// Tests various initialization strategies to find what the Realtek driver accepts.
//
// Usage: realtek-asio-probe.exe [driver-name]
//   driver-name defaults to "Realtek ASIO"
//
// Exit codes:
//   0 = at least one init() succeeded
//   1 = all init() attempts failed

#include "iasiodrv.h"

#include <windows.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <propsys.h>
#include <stdio.h>
#include <string.h>

// PKEY_AudioEndpoint_FriendlyName — defined locally
static const PROPERTYKEY PKEY_AudioEndpoint_FriendlyName = {
    { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } },
    14
};

// ---------------------------------------------------------------------------
// Registry enumeration (same as ASIO SDK asiolist.cpp)
// ---------------------------------------------------------------------------
struct DriverEntry {
    char name[128];
    char clsidStr[128];
    CLSID clsid;
};

static int enumerateDrivers(DriverEntry *entries, int maxEntries)
{
    HKEY hkEnum = 0;
    int count = 0;
    if (RegOpenKeyA(HKEY_LOCAL_MACHINE, "software\\asio", &hkEnum) != ERROR_SUCCESS)
        return 0;

    char keyname[256];
    DWORD index = 0;
    while (RegEnumKeyA(hkEnum, index++, keyname, sizeof(keyname)) == ERROR_SUCCESS) {
        if (count >= maxEntries) break;
        HKEY hksub = 0;
        if (RegOpenKeyExA(hkEnum, keyname, 0, KEY_READ, &hksub) == ERROR_SUCCESS) {
            char clsidBuf[256] = {};
            DWORD type = REG_SZ, size = sizeof(clsidBuf);
            if (RegQueryValueExA(hksub, "clsid", 0, &type, (LPBYTE)clsidBuf, &size) == ERROR_SUCCESS) {
                strncpy(entries[count].name, keyname, sizeof(entries[count].name) - 1);
                strncpy(entries[count].clsidStr, clsidBuf, sizeof(entries[count].clsidStr) - 1);
                // Parse CLSID
                wchar_t wclsid[256];
                MultiByteToWideChar(CP_ACP, 0, clsidBuf, -1, wclsid, 256);
                if (CLSIDFromString(wclsid, &entries[count].clsid) == S_OK) {
                    count++;
                }
            }
            RegCloseKey(hksub);
        }
    }
    RegCloseKey(hkEnum);
    return count;
}

// ---------------------------------------------------------------------------
// Helper: create a visible message window on the calling thread
// ---------------------------------------------------------------------------
static HWND createProbeWindow()
{
    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = L"AsioProbeWindow";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"ASIO Probe",
                                WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 100, 100,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (hwnd) {
        ShowWindow(hwnd, SW_SHOWNORMAL);
        UpdateWindow(hwnd);
        // Pump messages briefly to ensure the window is fully created
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
    return hwnd;
}

// ---------------------------------------------------------------------------
// Test result
// ---------------------------------------------------------------------------
struct TestResult {
    const char *comModel;
    const char *hostKind;
    HWND hostWindow;
    ASIOBool initResult;
    bool crashed;
    char driverName[32];
    char errorMessage[124];
};

// ---------------------------------------------------------------------------
// Run a single init test
// ---------------------------------------------------------------------------
static TestResult runTest(const CLSID &clsid, const char *comModelName,
                          DWORD comModel, const char *hostKind, HWND hostWindow)
{
    TestResult result = {};
    result.comModel = comModelName;
    result.hostKind = hostKind;
    result.hostWindow = hostWindow;
    result.initResult = ASIOFalse;
    result.crashed = false;

    // Initialize COM
    HRESULT hr = CoInitializeEx(nullptr, comModel);
    if (hr == RPC_E_CHANGED_MODE) {
        // Already initialized with different model - reinitialize
        CoUninitialize();
        hr = CoInitializeEx(nullptr, comModel);
    }
    if (FAILED(hr)) {
        snprintf(result.errorMessage, sizeof(result.errorMessage),
                 "CoInitializeEx failed: 0x%08lx", hr);
        return result;
    }

    // Create driver
    IASIO *driver = nullptr;
    hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, clsid,
                          reinterpret_cast<void **>(&driver));
    if (FAILED(hr) || !driver) {
        snprintf(result.errorMessage, sizeof(result.errorMessage),
                 "CoCreateInstance failed: 0x%08lx", hr);
        CoUninitialize();
        return result;
    }

    // Try init with SEH protection
    __try {
        result.initResult = driver->init(hostWindow);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result.crashed = true;
        result.initResult = ASIOFalse;
    }

    // Get driver info regardless of init result
    driver->getDriverName(result.driverName);
    driver->getErrorMessage(result.errorMessage);

    // If init succeeded, get more info
    if (result.initResult == ASIOTrue) {
        printf("  [SUCCESS] Driver: %s\n", result.driverName);
        long ver = driver->getDriverVersion();
        printf("  Driver version: %ld\n", ver);

        long inCh = 0, outCh = 0;
        if (driver->getChannels(&inCh, &outCh) == ASE_OK) {
            printf("  Channels: %ld in, %ld out\n", inCh, outCh);
        }

        double rate = 0;
        if (driver->getSampleRate(&rate) == ASE_OK) {
            printf("  Sample rate: %.0f\n", rate);
        }

        // Clean up properly
        driver->stop();
        driver->disposeBuffers();
    }

    driver->Release();
    CoUninitialize();
    return result;
}

// ---------------------------------------------------------------------------
// Print test result
// ---------------------------------------------------------------------------
static void printResult(const TestResult &r)
{
    const char *status;
    if (r.initResult == ASIOTrue) {
        status = "PASS";
    } else if (r.crashed) {
        status = "CRASH";
    } else {
        status = "FAIL";
    }
    printf("  [%s] COM=%-3s host=%-16s init=%ld error=\"%s\"\n",
           status, r.comModel, r.hostKind,
           r.initResult,
           r.errorMessage[0] ? r.errorMessage : "(empty)");
}

// ---------------------------------------------------------------------------
// WASAPI endpoint check: is the Realtek audio device busy?
// ---------------------------------------------------------------------------
static void checkWasapiEndpoint(const char *driverName)
{
    printf("\n=== WASAPI Endpoint Check ===\n");

    // Find Realtek WASAPI endpoint
    IMMDeviceEnumerator *enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void **>(&enumerator));
    if (FAILED(hr) || !enumerator) {
        printf("  Cannot create MMDeviceEnumerator: 0x%08lx\n", hr);
        return;
    }

    IMMDeviceCollection *collection = nullptr;
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    enumerator->Release();
    if (FAILED(hr) || !collection) {
        printf("  Cannot enumerate endpoints\n");
        return;
    }

    UINT count = 0;
    collection->GetCount(&count);
    printf("  Active render endpoints: %u\n", count);

    bool foundRealtek = false;
    for (UINT i = 0; i < count; ++i) {
        IMMDevice *device = nullptr;
        if (FAILED(collection->Item(i, &device)) || !device) continue;

        IPropertyStore *props = nullptr;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
            PROPVARIANT nameVar;
            PropVariantInit(&nameVar);
            if (SUCCEEDED(props->GetValue(PKEY_AudioEndpoint_FriendlyName, &nameVar))
                && nameVar.vt == VT_LPWSTR) {
                wchar_t *name = nameVar.pwszVal;
                printf("  Endpoint[%u]: %ls\n", i, name);

                // Check if this is a Realtek endpoint
                if (wcsstr(name, L"Realtek") || wcsstr(name, L"realtek")) {
                    foundRealtek = true;
                    printf("    ^ This is a Realtek endpoint\n");

                    // Try to open exclusive mode to check if device is busy
                    IAudioClient *client = nullptr;
                    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                          reinterpret_cast<void **>(&client));
                    if (SUCCEEDED(hr) && client) {
                        WAVEFORMATEX *mixFmt = nullptr;
                        hr = client->GetMixFormat(&mixFmt);
                        if (SUCCEEDED(hr) && mixFmt) {
                            hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 0, 0, mixFmt, nullptr);
                            if (hr == AUDCLNT_E_DEVICE_IN_USE) {
                                printf("    Device is BUSY (AUDCLNT_E_DEVICE_IN_USE)\n");
                            } else if (SUCCEEDED(hr)) {
                                printf("    Device is AVAILABLE (shared mode OK)\n");
                                // Check for active sessions
                                IAudioSessionManager2 *sm = nullptr;
                                hr = device->Activate(__uuidof(IAudioSessionManager2),
                                                       CLSCTX_ALL, nullptr,
                                                       reinterpret_cast<void **>(&sm));
                                if (SUCCEEDED(hr) && sm) {
                                    IAudioSessionEnumerator *se = nullptr;
                                    hr = sm->GetSessionEnumerator(&se);
                                    if (SUCCEEDED(hr) && se) {
                                        int sc = 0;
                                        se->GetCount(&sc);
                                        printf("    Audio sessions: %d\n", sc);
                                        for (int s = 0; s < sc; ++s) {
                                            IAudioSessionControl *sess = nullptr;
                                            if (SUCCEEDED(se->GetSession(s, &sess)) && sess) {
                                                IAudioSessionControl2 *sess2 = nullptr;
                                                if (SUCCEEDED(sess->QueryInterface(
                                                        __uuidof(IAudioSessionControl2),
                                                        reinterpret_cast<void **>(&sess2)))) {
                                                    DWORD pid = 0;
                                                    sess2->GetProcessId(&pid);
                                                    AudioSessionState state;
                                                    sess->GetState(&state);
                                                    printf("      Session[%d]: pid=%lu state=%d\n",
                                                           s, pid, state);
                                                    sess2->Release();
                                                }
                                                sess->Release();
                                            }
                                        }
                                        se->Release();
                                    }
                                    sm->Release();
                                }
                            } else {
                                printf("    WASAPI shared init: 0x%08lx\n", hr);
                            }
                            CoTaskMemFree(mixFmt);
                        }
                        client->Release();
                    }
                }
                PropVariantClear(&nameVar);
            } else {
                PropVariantClear(&nameVar);
            }
            props->Release();
        }
        device->Release();
    }
    collection->Release();

    if (!foundRealtek) {
        printf("  No Realtek endpoint found in active render devices\n");
    }
}

// ---------------------------------------------------------------------------
// Worker thread test: simulate the project's audio worker thread
// (separate thread, STA, no message pump, no visible window)
// ---------------------------------------------------------------------------
struct WorkerThreadParams {
    CLSID clsid;
    HWND hwndToUse;
    const char *hwndLabel;
    TestResult result;
};

static DWORD WINAPI workerThreadProc(LPVOID param)
{
    auto *p = static_cast<WorkerThreadParams *>(param);
    p->result = runTest(p->clsid, "STA", COINIT_APARTMENTTHREADED,
                        p->hwndLabel, p->hwndToUse);
    return 0;
}

static void runWorkerThreadTest(const CLSID &clsid, const char *label, HWND hwnd)
{
    printf("=== Worker Thread Test: %s ===\n", label);
    printf("  (Simulates project's AsioOutputWorker on QThread)\n");

    WorkerThreadParams params = {};
    params.clsid = clsid;
    params.hwndToUse = hwnd;
    params.hwndLabel = label;

    HANDLE hThread = CreateThread(nullptr, 0, workerThreadProc, &params, 0, nullptr);
    if (hThread) {
        WaitForSingleObject(hThread, 10000);
        CloseHandle(hThread);
    }

    printResult(params.result);
    printf("\n");
}

// ---------------------------------------------------------------------------
// MTA pre-init test: simulate Qt/another library pre-initializing MTA
// ---------------------------------------------------------------------------
static DWORD WINAPI workerThreadMtaPreinitProc(LPVOID param)
{
    auto *p = static_cast<WorkerThreadParams *>(param);

    // Step 1: Simulate Qt pre-initializing MTA on this thread
    HRESULT preHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    printf("  [thread] Pre-init MTA: hr=0x%08lx\n", preHr);

    // Step 2: openDriver tries STA — gets RPC_E_CHANGED_MODE
    HRESULT staHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool staBlocked = (staHr == RPC_E_CHANGED_MODE);
    printf("  [thread] Try STA after MTA: hr=0x%08lx blocked=%d\n",
           staHr, staBlocked ? 1 : 0);

    if (staBlocked) {
        // Step 3: openDriver skips STA, tries MTA
        // COM is already MTA, CoInitializeEx(COINIT_MULTITHREADED) returns S_FALSE
        HRESULT mtaHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        printf("  [thread] Try MTA (already init): hr=0x%08lx\n", mtaHr);

        // Step 4: CoCreateInstance with MTA
        IASIO *driver = nullptr;
        HRESULT hr = CoCreateInstance(p->clsid, nullptr, CLSCTX_INPROC_SERVER,
                                      p->clsid, reinterpret_cast<void **>(&driver));
        printf("  [thread] CoCreateInstance(MTA): hr=0x%08lx driver=%p\n",
               hr, (void *)driver);
        if (SUCCEEDED(hr) && driver) {
            ASIOBool initResult = ASIOFalse;
            __try {
                initResult = driver->init(p->hwndToUse);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                printf("  [thread] init() CRASHED\n");
            }
            char drvName[32] = {};
            char errMsg[124] = {};
            driver->getDriverName(drvName);
            driver->getErrorMessage(errMsg);
            printf("  [thread] init()=%ld driver=%s error=%s\n",
                   initResult, drvName, errMsg[0] ? errMsg : "(empty)");
            p->result.initResult = initResult;
            strncpy(p->result.driverName, drvName, sizeof(p->result.driverName));
            strncpy(p->result.errorMessage, errMsg, sizeof(p->result.errorMessage));
            p->result.comModel = "MTA-pre";
            p->result.hostKind = p->hwndLabel;
            driver->Release();
        } else {
            printf("  [thread] CoCreateInstance failed — THIS MATCHES PROJECT FAILURE!\n");
            snprintf(p->result.errorMessage, sizeof(p->result.errorMessage),
                     "CoCreateInstance failed: 0x%08lx", hr);
            p->result.comModel = "MTA-pre";
            p->result.hostKind = p->hwndLabel;
        }
    } else {
        // STA succeeded — unexpected if MTA was pre-initialized
        printf("  [thread] STA succeeded unexpectedly — no RPC_E_CHANGED_MODE\n");
        // Try init with STA
        IASIO *driver = nullptr;
        HRESULT hr = CoCreateInstance(p->clsid, nullptr, CLSCTX_INPROC_SERVER,
                                      p->clsid, reinterpret_cast<void **>(&driver));
        if (SUCCEEDED(hr) && driver) {
            ASIOBool initResult = ASIOFalse;
            __try {
                initResult = driver->init(p->hwndToUse);
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            char drvName[32] = {};
            driver->getDriverName(drvName);
            p->result.initResult = initResult;
            strncpy(p->result.driverName, drvName, sizeof(p->result.driverName));
            p->result.comModel = "STA-after-MTA";
            p->result.hostKind = p->hwndLabel;
            driver->Release();
        }
    }

    CoUninitialize();
    return 0;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(int argc, char *argv[])
{
    const char *targetDriver = (argc > 1) ? argv[1] : "Realtek ASIO";

    printf("=== Realtek ASIO Probe Tool ===\n");
    printf("Target driver: %s\n\n", targetDriver);

    // Enumerate ASIO drivers
    DriverEntry drivers[32];
    int driverCount = enumerateDrivers(drivers, 32);
    printf("Registered ASIO drivers: %d\n", driverCount);
    for (int i = 0; i < driverCount; ++i) {
        printf("  [%d] %s (CLSID: %s)\n", i, drivers[i].name, drivers[i].clsidStr);
    }
    printf("\n");

    // Find target driver
    int targetIndex = -1;
    for (int i = 0; i < driverCount; ++i) {
        if (_stricmp(drivers[i].name, targetDriver) == 0) {
            targetIndex = i;
            break;
        }
    }
    if (targetIndex < 0) {
        printf("ERROR: Driver '%s' not found in registry\n", targetDriver);
        return 1;
    }

    const CLSID clsid = drivers[targetIndex].clsid;
    printf("Using driver: %s\n", drivers[targetIndex].name);
    printf("CLSID: %s\n\n", drivers[targetIndex].clsidStr);

    // Check WASAPI endpoint first
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    checkWasapiEndpoint(drivers[targetIndex].name);
    CoUninitialize();
    printf("\n");

    // Create windows for testing
    HWND probeWindow = createProbeWindow();
    HWND desktopWindow = GetDesktopWindow();
    HWND foregroundWindow = GetForegroundWindow();

    // Create message-only window
    WNDCLASSW mwc = {};
    mwc.lpfnWndProc = DefWindowProcW;
    mwc.hInstance = GetModuleHandle(nullptr);
    mwc.lpszClassName = L"AsioProbeMessageWindow";
    RegisterClassW(&mwc);
    HWND messageWindow = CreateWindowExW(0, mwc.lpszClassName, L"ASIO Msg",
                                          0, 0, 0, 1, 1,
                                          HWND_MESSAGE, nullptr, mwc.hInstance, nullptr);

    printf("Window handles for testing:\n");
    printf("  probe-window:    %p\n", (void *)probeWindow);
    printf("  desktop:         %p\n", (void *)desktopWindow);
    printf("  foreground:      %p\n", (void *)foregroundWindow);
    printf("  message-window:  %p\n", (void *)messageWindow);
    printf("  null-handle:     %p\n\n", (void *)nullptr);

    // Test matrix: COM model x Window handle
    struct ComTest {
        const char *name;
        DWORD model;
    } comModels[] = {
        {"STA", COINIT_APARTMENTTHREADED},
        {"MTA", COINIT_MULTITHREADED},
    };

    struct WindowTest {
        const char *name;
        HWND hwnd;
    } windows[] = {
        {"probe-window",   probeWindow},
        {"foreground",     foregroundWindow},
        {"desktop",        desktopWindow},
        {"message-window", messageWindow},
        {"null-handle",    nullptr},
    };

    const int numComModels = 2;
    const int numWindows = 5;
    int passCount = 0;
    int totalTests = 0;

    printf("=== Init Test Matrix ===\n");
    for (int ci = 0; ci < numComModels; ++ci) {
        for (int wi = 0; wi < numWindows; ++wi) {
            totalTests++;
            TestResult r = runTest(clsid, comModels[ci].name, comModels[ci].model,
                                   windows[wi].name, windows[wi].hwnd);
            printResult(r);
            if (r.initResult == ASIOTrue) passCount++;
        }
    }

    printf("\n=== Summary ===\n");
    printf("Total tests: %d\n", totalTests);
    printf("Passed: %d\n", passCount);
    printf("Failed: %d\n", totalTests - passCount);

    if (passCount == 0) {
        printf("\nAll init() attempts failed. The Realtek ASIO driver may require:\n");
        printf("  1. Exclusive access to the audio device (close other audio apps)\n");
        printf("  2. A specific hardware state (try running Realtek Audio Console first)\n");
        printf("  3. A different driver version\n");
    }

    // Worker thread tests (simulate project's QThread audio worker)
    if (passCount > 0) {
        printf("\n");
        runWorkerThreadTest(clsid, "worker+null-handle", nullptr);
        runWorkerThreadTest(clsid, "worker+probe-window", probeWindow);
        runWorkerThreadTest(clsid, "worker+desktop", desktopWindow);
    }

    // Test: simulate Qt's MTA pre-init on worker thread, then try STA
    printf("=== MTA Pre-init Test ===\n");
    printf("  (Simulates Qt/another library pre-initializing MTA before openDriver)\n");
    {
        WorkerThreadParams params = {};
        params.clsid = clsid;
        params.hwndToUse = nullptr;
        params.hwndLabel = "mta-preinit+null";

        HANDLE hThread = CreateThread(nullptr, 0, workerThreadMtaPreinitProc, &params, 0, nullptr);
        if (hThread) {
            WaitForSingleObject(hThread, 10000);
            CloseHandle(hThread);
        }
        printResult(params.result);
    }
    printf("\n");

    // Cleanup
    if (probeWindow) DestroyWindow(probeWindow);
    if (messageWindow) DestroyWindow(messageWindow);

    return (passCount > 0) ? 0 : 1;
}
