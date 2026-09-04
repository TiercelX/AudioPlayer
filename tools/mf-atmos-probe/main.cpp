#include <windows.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mfplay.h>
#include <mfreadwrite.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <iterator>
#include <mutex>
#include <string>

namespace {

// The SDK declares MEDIASUBTYPE_DOLBY_TRUEHD as an extern GUID in
// wmcodecdsp.h, but it is not exported by mfuuid.lib. Keep the documented
// value local so this diagnostic tool has no additional codec-DSP dependency.
constexpr GUID kMediaSubtypeDolbyTrueHd = {
    0xeb27cec4, 0x163e, 0x4ca3, {0x8b, 0x74, 0x8e, 0x25, 0xf9, 0x1b, 0x51, 0x7e}
};

std::wstring guidText(REFGUID guid)
{
    wchar_t buffer[64] = {};
    StringFromGUID2(guid, buffer, static_cast<int>(std::size(buffer)));
    return buffer;
}

const wchar_t *subtypeName(REFGUID subtype)
{
    if (subtype == MFAudioFormat_Dolby_DDPlus) {
        return L"DolbyDigitalPlus";
    }
    if (subtype == kMediaSubtypeDolbyTrueHd) {
        return L"DolbyTrueHD";
    }
    if (subtype == MFAudioFormat_Dolby_AC3) {
        return L"DolbyDigital";
    }
    return L"unknown";
}

bool matchesExpectedSubtype(REFGUID subtype, const std::wstring &expected)
{
    return expected.empty()
        || (expected == L"eac3" && subtype == MFAudioFormat_Dolby_DDPlus)
        || (expected == L"truehd" && subtype == kMediaSubtypeDolbyTrueHd);
}

void printDefaultRenderEndpoint()
{
    IMMDeviceEnumerator *enumerator = nullptr;
    IMMDevice *device = nullptr;
    IPropertyStore *properties = nullptr;
    LPWSTR endpointId = nullptr;
    PROPVARIANT friendlyName;
    PropVariantInit(&friendlyName);

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator),
                                  nullptr,
                                  CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void **>(&enumerator));
    if (SUCCEEDED(hr)) {
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    }
    if (SUCCEEDED(hr)) {
        hr = device->GetId(&endpointId);
    }
    if (SUCCEEDED(hr)) {
        hr = device->OpenPropertyStore(STGM_READ, &properties);
    }
    if (SUCCEEDED(hr)) {
        hr = properties->GetValue(PKEY_Device_FriendlyName, &friendlyName);
    }

    if (SUCCEEDED(hr)) {
        std::wcout << L"defaultRenderEndpointId=" << endpointId << std::endl;
        // Some endpoint names contain console-codepage characters. Keep this
        // command-line diagnostic machine-readable instead of allowing a
        // rendering failure to suppress later probe output.
        std::wcout << L"defaultRenderEndpointNameAvailable="
                   << (friendlyName.vt == VT_LPWSTR && friendlyName.pwszVal ? 1 : 0)
                   << std::endl;
    } else {
        std::wcerr << L"defaultRenderEndpoint=INCONCLUSIVE hr=0x" << std::hex
                   << static_cast<unsigned long>(hr) << std::endl;
    }

    PropVariantClear(&friendlyName);
    if (endpointId) {
        CoTaskMemFree(endpointId);
    }
    if (properties) {
        properties->Release();
    }
    if (device) {
        device->Release();
    }
    if (enumerator) {
        enumerator->Release();
    }
}

class MfPlayCallback final : public IMFPMediaPlayerCallback
{
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override
    {
        if (!object) {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == __uuidof(IMFPMediaPlayerCallback)) {
            *object = static_cast<IMFPMediaPlayerCallback *>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return ++m_references;
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG remaining = --m_references;
        if (remaining == 0) {
            delete this;
        }
        return remaining;
    }

    void STDMETHODCALLTYPE OnMediaPlayerEvent(MFP_EVENT_HEADER *eventHeader) override
    {
        if (!eventHeader) {
            return;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        if (eventHeader->eEventType == MFP_EVENT_TYPE_PLAY) {
            m_playStarted = true;
        } else if (eventHeader->eEventType == MFP_EVENT_TYPE_ERROR) {
            m_error = eventHeader->hrEvent;
        }
        m_condition.notify_all();
    }

    bool waitForPlay(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_condition.wait_for(lock, timeout, [this]() {
            return m_playStarted || FAILED(m_error);
        });
        return m_playStarted;
    }

    HRESULT error() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_error;
    }

private:
    std::atomic<ULONG> m_references {1};
    mutable std::mutex m_mutex;
    std::condition_variable m_condition;
    bool m_playStarted = false;
    HRESULT m_error = S_OK;
};

void printUsage()
{
    std::wcerr << L"Usage: MfAtmosProbe <sidecar.mka> [--expect eac3|truehd] [--render-ms <milliseconds>]" << std::endl;
}

} // namespace

int wmain(int argc, wchar_t **argv)
{
    if (argc < 2) {
        printUsage();
        return 2;
    }

    std::wstring expected;
    DWORD renderMilliseconds = 0;
    for (int index = 2; index < argc; ++index) {
        const std::wstring option = argv[index];
        if (option == L"--expect" && index + 1 < argc) {
            expected = argv[++index];
            if (expected != L"eac3" && expected != L"truehd") {
                printUsage();
                return 2;
            }
        } else if (option == L"--render-ms" && index + 1 < argc) {
            try {
                const unsigned long value = std::stoul(argv[++index]);
                if (value < 1000 || value > 600000) {
                    printUsage();
                    return 2;
                }
                renderMilliseconds = value;
            } catch (...) {
                printUsage();
                return 2;
            }
        } else {
            printUsage();
            return 2;
        }
    }

    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool shouldUninitializeCom = SUCCEEDED(comHr);
    if (FAILED(comHr) && comHr != RPC_E_CHANGED_MODE) {
        std::wcerr << L"probeResult=FAIL stage=CoInitializeEx hr=0x" << std::hex
                   << static_cast<unsigned long>(comHr) << std::endl;
        return 1;
    }

    printDefaultRenderEndpoint();

    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        std::wcerr << L"probeResult=FAIL stage=MFStartup hr=0x" << std::hex
                   << static_cast<unsigned long>(hr) << std::endl;
        if (shouldUninitializeCom) {
            CoUninitialize();
        }
        return 1;
    }

    IMFSourceReader *reader = nullptr;
    hr = MFCreateSourceReaderFromURL(argv[1], nullptr, &reader);
    if (FAILED(hr)) {
        std::wcerr << L"probeResult=FAIL stage=MFCreateSourceReaderFromURL hr=0x" << std::hex
                   << static_cast<unsigned long>(hr) << std::endl;
        MFShutdown();
        if (shouldUninitializeCom) {
            CoUninitialize();
        }
        return 1;
    }

    DWORD audioStream = MF_SOURCE_READER_INVALID_STREAM_INDEX;
    IMFMediaType *audioType = nullptr;
    for (DWORD stream = 0; stream < 16; ++stream) {
        IMFMediaType *candidate = nullptr;
        if (FAILED(reader->GetNativeMediaType(stream, 0, &candidate))) {
            continue;
        }

        GUID majorType = GUID_NULL;
        const HRESULT majorTypeHr = candidate->GetGUID(MF_MT_MAJOR_TYPE, &majorType);
        if (SUCCEEDED(majorTypeHr) && majorType == MFMediaType_Audio) {
            audioStream = stream;
            audioType = candidate;
            break;
        }
        candidate->Release();
    }

    if (!audioType) {
        std::wcerr << L"probeResult=FAIL stage=GetNativeMediaType reason=no-audio-stream" << std::endl;
        reader->Release();
        MFShutdown();
        if (shouldUninitializeCom) {
            CoUninitialize();
        }
        return 1;
    }

    GUID subtype = GUID_NULL;
    hr = audioType->GetGUID(MF_MT_SUBTYPE, &subtype);
    if (FAILED(hr)) {
        std::wcerr << L"probeResult=FAIL stage=GetSubtype hr=0x" << std::hex
                   << static_cast<unsigned long>(hr) << std::endl;
        audioType->Release();
        reader->Release();
        MFShutdown();
        if (shouldUninitializeCom) {
            CoUninitialize();
        }
        return 1;
    }

    UINT32 channels = 0;
    UINT32 sampleRate = 0;
    audioType->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
    audioType->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sampleRate);
    std::wcout << L"audioStream=" << audioStream << std::endl;
    std::wcout << L"nativeSubtype=" << guidText(subtype) << std::endl;
    std::wcout << L"nativeSubtypeName=" << subtypeName(subtype) << std::endl;
    std::wcout << L"nativeChannels=" << channels << std::endl;
    std::wcout << L"nativeSampleRate=" << sampleRate << std::endl;

    if (!matchesExpectedSubtype(subtype, expected)) {
        std::wcerr << L"probeResult=FAIL stage=SubtypeCheck expected=" << expected
                   << L" actual=" << subtypeName(subtype) << std::endl;
        audioType->Release();
        reader->Release();
        MFShutdown();
        if (shouldUninitializeCom) {
            CoUninitialize();
        }
        return 1;
    }

    DWORD actualStream = MF_SOURCE_READER_INVALID_STREAM_INDEX;
    DWORD streamFlags = 0;
    LONGLONG timestamp = 0;
    IMFSample *sample = nullptr;
    hr = reader->ReadSample(audioStream, 0, &actualStream, &streamFlags, &timestamp, &sample);
    if (FAILED(hr) || !sample) {
        std::wcerr << L"probeResult=FAIL stage=ReadSample hr=0x" << std::hex
                   << static_cast<unsigned long>(hr) << L" flags=0x" << streamFlags << std::endl;
        if (sample) {
            sample->Release();
        }
        audioType->Release();
        reader->Release();
        MFShutdown();
        if (shouldUninitializeCom) {
            CoUninitialize();
        }
        return 1;
    }

    DWORD sampleBytes = 0;
    hr = sample->GetTotalLength(&sampleBytes);
    sample->Release();
    if (FAILED(hr) || sampleBytes == 0) {
        std::wcerr << L"probeResult=FAIL stage=SampleLength hr=0x" << std::hex
                   << static_cast<unsigned long>(hr) << L" bytes=" << sampleBytes << std::endl;
        audioType->Release();
        reader->Release();
        MFShutdown();
        return 1;
    }

    std::wcout << L"firstCompressedSampleBytes=" << sampleBytes << std::endl;
    std::wcout << L"probeResult=PASS" << std::endl;

    audioType->Release();
    reader->Release();

    if (renderMilliseconds > 0) {
        auto *callback = new MfPlayCallback;
        IMFPMediaPlayer *player = nullptr;
        hr = MFPCreateMediaPlayer(argv[1],
                                  TRUE,
                                  MFP_OPTION_FREE_THREADED_CALLBACK,
                                  callback,
                                  nullptr,
                                  &player);
        if (FAILED(hr) || !player) {
            std::wcerr << L"rendererResult=FAIL stage=MFPCreateMediaPlayer hr=0x" << std::hex
                       << static_cast<unsigned long>(hr) << std::endl;
            callback->Release();
            MFShutdown();
            if (shouldUninitializeCom) {
                CoUninitialize();
            }
            return 1;
        }

        float playerVolume = 0.0f;
        BOOL playerMuted = FALSE;
        const HRESULT volumeHr = player->GetVolume(&playerVolume);
        const HRESULT muteHr = player->GetMute(&playerMuted);
        if (SUCCEEDED(volumeHr) && SUCCEEDED(muteHr)) {
            std::wcout << L"rendererVolume=" << playerVolume << std::endl;
            std::wcout << L"rendererMuted=" << (playerMuted ? 1 : 0) << std::endl;
        } else {
            std::wcerr << L"rendererVolume=INCONCLUSIVE volumeHr=0x" << std::hex
                       << static_cast<unsigned long>(volumeHr) << L" muteHr=0x"
                       << static_cast<unsigned long>(muteHr) << std::endl;
        }

        if (!callback->waitForPlay(std::chrono::seconds(12))) {
            std::wcerr << L"rendererResult=FAIL stage=waitForPlay hr=0x" << std::hex
                       << static_cast<unsigned long>(callback->error()) << std::endl;
            player->Shutdown();
            player->Release();
            callback->Release();
            MFShutdown();
            if (shouldUninitializeCom) {
                CoUninitialize();
            }
            return 1;
        }

        std::wcout << L"rendererStarted=PASS" << std::endl;
        std::wcout << L"rendererDurationMs=" << renderMilliseconds << std::endl;
        Sleep(renderMilliseconds);
        player->Stop();
        player->Shutdown();
        player->Release();
        callback->Release();
        std::wcout << L"rendererResult=PASS" << std::endl;
    }

    MFShutdown();
    if (shouldUninitializeCom) {
        CoUninitialize();
    }
    return 0;
}
