#include <windows.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mfmediaengine.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <AudioSessionTypes.h>
#include <propvarutil.h>
#include <shlwapi.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>

namespace {

class MediaEngineNotify final : public IMFMediaEngineNotify
{
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override
    {
        if (!object) {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == __uuidof(IMFMediaEngineNotify)) {
            *object = static_cast<IMFMediaEngineNotify *>(this);
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

    HRESULT STDMETHODCALLTYPE EventNotify(DWORD event, DWORD_PTR param1, DWORD param2) override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lastEvent = event;
        m_lastParam1 = param1;
        m_lastParam2 = param2;
        if (event == MF_MEDIA_ENGINE_EVENT_ERROR) {
            m_error = E_FAIL;
        }
        if (event == MF_MEDIA_ENGINE_EVENT_CANPLAY
            || event == MF_MEDIA_ENGINE_EVENT_CANPLAYTHROUGH
            || event == MF_MEDIA_ENGINE_EVENT_PLAYING
            || event == MF_MEDIA_ENGINE_EVENT_ERROR
            || event == MF_MEDIA_ENGINE_EVENT_ENDED) {
            m_condition.notify_all();
        }
        return S_OK;
    }

    bool waitFor(DWORD event, std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_condition.wait_for(lock, timeout, [this, event]() {
            return m_lastEvent == event || m_lastEvent == MF_MEDIA_ENGINE_EVENT_ERROR;
        }) && m_lastEvent == event;
    }

    bool waitForReady(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_condition.wait_for(lock, timeout, [this]() {
            return m_lastEvent == MF_MEDIA_ENGINE_EVENT_CANPLAY
                || m_lastEvent == MF_MEDIA_ENGINE_EVENT_CANPLAYTHROUGH
                || m_lastEvent == MF_MEDIA_ENGINE_EVENT_ERROR;
        }) && m_lastEvent != MF_MEDIA_ENGINE_EVENT_ERROR;
    }

    DWORD lastEvent() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_lastEvent;
    }

    DWORD_PTR lastParam1() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_lastParam1;
    }

    DWORD lastParam2() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_lastParam2;
    }

private:
    std::atomic<ULONG> m_references {1};
    mutable std::mutex m_mutex;
    std::condition_variable m_condition;
    DWORD m_lastEvent = 0;
    DWORD_PTR m_lastParam1 = 0;
    DWORD m_lastParam2 = 0;
    HRESULT m_error = S_OK;
};

std::wstring fileUrl(const std::wstring &path)
{
    wchar_t fullPath[MAX_PATH] = {};
    const DWORD length = GetFullPathNameW(path.c_str(), static_cast<DWORD>(std::size(fullPath)), fullPath, nullptr);
    if (length == 0 || length >= std::size(fullPath)) {
        return {};
    }

    wchar_t url[2048] = {};
    DWORD urlLength = static_cast<DWORD>(std::size(url));
    if (FAILED(UrlCreateFromPathW(fullPath, url, &urlLength, 0))) {
        return {};
    }
    return url;
}

const wchar_t *canPlayName(MF_MEDIA_ENGINE_CANPLAY value)
{
    switch (value) {
    case MF_MEDIA_ENGINE_CANPLAY_NOT_SUPPORTED:
        return L"NotSupported";
    case MF_MEDIA_ENGINE_CANPLAY_MAYBE:
        return L"Maybe";
    case MF_MEDIA_ENGINE_CANPLAY_PROBABLY:
        return L"Probably";
    default:
        return L"Unknown";
    }
}

void printDefaultRenderEndpoint()
{
    IMMDeviceEnumerator *enumerator = nullptr;
    IMMDevice *device = nullptr;
    LPWSTR endpointId = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&enumerator));
    if (SUCCEEDED(hr)) {
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    }
    if (SUCCEEDED(hr)) {
        hr = device->GetId(&endpointId);
    }
    if (SUCCEEDED(hr)) {
        std::wcout << L"engineDefaultRenderEndpointId=" << endpointId << std::endl;
    } else {
        std::wcerr << L"engineDefaultRenderEndpoint=INCONCLUSIVE hr=0x" << std::hex
                   << static_cast<unsigned long>(hr) << std::endl;
    }
    if (endpointId) {
        CoTaskMemFree(endpointId);
    }
    if (device) {
        device->Release();
    }
    if (enumerator) {
        enumerator->Release();
    }
}

void printEncodedEndpointSupport()
{
    IMMDeviceEnumerator *enumerator = nullptr;
    IMMDevice *device = nullptr;
    IAudioClient *audioClient = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&enumerator));
    if (SUCCEEDED(hr)) {
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    }
    if (SUCCEEDED(hr)) {
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void **>(&audioClient));
    }
    if (FAILED(hr)) {
        std::wcerr << L"encodedEndpoint=INCONCLUSIVE stage=activate hr=0x" << std::hex
                   << static_cast<unsigned long>(hr) << std::endl;
        if (audioClient) {
            audioClient->Release();
        }
        if (device) {
            device->Release();
        }
        if (enumerator) {
            enumerator->Release();
        }
        return;
    }

    struct EncodedFormat {
        const wchar_t *name;
        const GUID *subFormat;
    } formats[] = {
        {L"DD+", &KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_DIGITAL_PLUS},
        {L"DD+JOC", &KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_DIGITAL_PLUS_ATMOS},
    };
    for (const auto &encoded : formats) {
        WAVEFORMATEXTENSIBLE_IEC61937 format = {};
        format.FormatExt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        format.FormatExt.Format.nChannels = 2;
        format.FormatExt.Format.nSamplesPerSec = 192000;
        format.FormatExt.Format.nAvgBytesPerSec = 768000;
        format.FormatExt.Format.nBlockAlign = 4;
        format.FormatExt.Format.wBitsPerSample = 16;
        format.FormatExt.Format.cbSize = 34;
        format.FormatExt.Samples.wValidBitsPerSample = 16;
        format.FormatExt.dwChannelMask = KSAUDIO_SPEAKER_STEREO;
        format.FormatExt.SubFormat = *encoded.subFormat;
        format.dwEncodedSamplesPerSec = 48000;
        format.dwEncodedChannelCount = 6;

        for (const AUDCLNT_SHAREMODE mode : {AUDCLNT_SHAREMODE_SHARED, AUDCLNT_SHAREMODE_EXCLUSIVE}) {
            WAVEFORMATEX *closest = nullptr;
            const HRESULT supportHr = audioClient->IsFormatSupported(
                mode,
                reinterpret_cast<const WAVEFORMATEX *>(&format),
                mode == AUDCLNT_SHAREMODE_SHARED ? &closest : nullptr);
            std::wcout << L"encodedEndpointFormat=" << encoded.name
                       << L" mode=" << (mode == AUDCLNT_SHAREMODE_SHARED ? L"shared" : L"exclusive")
                       << L" hr=0x" << std::hex << static_cast<unsigned long>(supportHr)
                       << L" supported=" << (supportHr == S_OK ? 1 : 0) << std::endl;
            if (closest) {
                CoTaskMemFree(closest);
            }
        }
    }

    audioClient->Release();
    device->Release();
    enumerator->Release();
}

void printUsage()
{
    std::wcerr << L"Usage: MfMediaEngineProbe <sidecar.mka> [--render-ms <milliseconds>]" << std::endl;
}

} // namespace

int wmain(int argc, wchar_t **argv)
{
    if (argc < 2 || argc > 4) {
        printUsage();
        return 2;
    }

    DWORD renderMilliseconds = 0;
    for (int index = 2; index < argc; ++index) {
        const std::wstring option = argv[index];
        if (option == L"--render-ms" && index + 1 < argc) {
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

    const std::wstring url = fileUrl(argv[1]);
    if (url.empty()) {
        std::wcerr << L"engineResult=FAIL stage=file-url" << std::endl;
        return 1;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        std::wcerr << L"engineResult=FAIL stage=CoInitializeEx hr=0x" << std::hex
                   << static_cast<unsigned long>(hr) << std::endl;
        return 1;
    }

    bool mfStarted = false;
    IMFAttributes *attributes = nullptr;
    IMFMediaEngineClassFactory *factory = nullptr;
    IMFMediaEngine *engine = nullptr;
    MediaEngineNotify *notify = new MediaEngineNotify();
    int result = 1;

    hr = MFStartup(MF_VERSION);
    if (SUCCEEDED(hr)) {
        mfStarted = true;
        hr = MFCreateAttributes(&attributes, 4);
    }
    if (SUCCEEDED(hr)) {
        hr = attributes->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, notify);
    }
    if (SUCCEEDED(hr)) {
        hr = attributes->SetUINT32(MF_MEDIA_ENGINE_AUDIO_CATEGORY, AudioCategory_Media);
    }
    if (SUCCEEDED(hr)) {
        hr = attributes->SetUINT32(MF_MEDIA_ENGINE_AUDIO_ENDPOINT_ROLE, eConsole);
    }
    if (SUCCEEDED(hr)) {
        hr = CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&factory));
    }
    if (SUCCEEDED(hr)) {
        hr = factory->CreateInstance(MF_MEDIA_ENGINE_AUDIOONLY, attributes, &engine);
    }
    if (FAILED(hr)) {
        std::wcerr << L"engineResult=FAIL stage=create hr=0x" << std::hex
                   << static_cast<unsigned long>(hr) << std::endl;
        goto cleanup;
    }

    {
        const wchar_t *types[] = {
            L"video/mp4; codecs=\"avc1,ec-3\"",
            L"video/mp4; codecs=\"avc1,ec-3\"; features=\"audio-endpoint-codec=DD+JOC\"",
            L"video/mp4; codecs=\"avc1,ec-3\"; features=\"audio-endpoint-codec=DD+\"",
            L"video/mp4; features=\"audio-endpoint-codec=DD+JOC\"",
            L"video/mp4; features=\"audio-endpoint-codec=DD+\"",
            L"video/mp4; features=\"audio-endpoint-codec=PCM2.0\"",
            L"video/mp4; features=\"audio-endpoint-codec=PCM5.1\"",
            L"video/mp4; features=\"audio-endpoint-codec=PCM7.1\""
        };
        for (const wchar_t *type : types) {
            BSTR bstrType = SysAllocString(type);
            MF_MEDIA_ENGINE_CANPLAY answer = MF_MEDIA_ENGINE_CANPLAY_NOT_SUPPORTED;
            const HRESULT canPlayHr = engine->CanPlayType(bstrType, &answer);
            SysFreeString(bstrType);
            std::wcout << L"canPlayType=\"" << type << L"\" hr=0x" << std::hex
                       << static_cast<unsigned long>(canPlayHr) << L" answer="
                       << canPlayName(answer) << std::endl;
        }
    }

    printDefaultRenderEndpoint();
    printEncodedEndpointSupport();
    {
        BSTR bstrUrl = SysAllocString(url.c_str());
        hr = engine->SetSource(bstrUrl);
        SysFreeString(bstrUrl);
    }
    if (SUCCEEDED(hr)) {
        hr = engine->Load();
    }
    std::wcout << L"engineLoad=\"" << (SUCCEEDED(hr) ? L"PASS" : L"FAIL") << L"\" hr=0x"
               << std::hex << static_cast<unsigned long>(hr) << std::endl;
    if (FAILED(hr)) {
        goto cleanup;
    }

    if (!notify->waitForReady(std::chrono::seconds(20))) {
        std::wcerr << L"engineReady=FAIL lastEvent=" << std::dec << notify->lastEvent()
                   << L" errorCode=" << std::dec << notify->lastParam1()
                   << L" extendedHresult=0x" << std::hex << notify->lastParam2() << std::endl;
        goto cleanup;
    }
    std::wcout << L"engineReady=PASS event=" << std::dec << notify->lastEvent()
               << L" durationSeconds=" << engine->GetDuration()
               << L" hasAudio=" << (engine->HasAudio() ? 1 : 0) << std::endl;

    hr = engine->Play();
    std::wcout << L"enginePlay=\"" << (SUCCEEDED(hr) ? L"PASS" : L"FAIL") << L"\" hr=0x"
               << std::hex << static_cast<unsigned long>(hr) << std::endl;
    if (FAILED(hr)) {
        goto cleanup;
    }
    if (!notify->waitFor(MF_MEDIA_ENGINE_EVENT_PLAYING, std::chrono::seconds(10))) {
        std::wcerr << L"enginePlaying=FAIL lastEvent=" << std::dec << notify->lastEvent()
                   << L" errorCode=" << std::dec << notify->lastParam1()
                   << L" extendedHresult=0x" << std::hex << notify->lastParam2() << std::endl;
        goto cleanup;
    }
    std::wcout << L"enginePlaying=PASS" << std::endl;

    if (renderMilliseconds > 0) {
        Sleep(renderMilliseconds);
        std::wcout << L"engineRenderResult=PASS elapsedMs=" << std::dec << renderMilliseconds << std::endl;
    }
    result = 0;

cleanup:
    if (engine) {
        engine->Shutdown();
        engine->Release();
    }
    if (factory) {
        factory->Release();
    }
    if (attributes) {
        attributes->Release();
    }
    notify->Release();
    if (mfStarted) {
        MFShutdown();
    }
    CoUninitialize();
    return result;
}
