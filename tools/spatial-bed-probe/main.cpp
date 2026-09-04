#include <audioclient.h>
#include <mmdeviceapi.h>
#include <objbase.h>
#include <propkeydef.h>
#include <propvarutil.h>
#include <functiondiscoverykeys_devpkey.h>
#include <spatialaudioclient.h>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

template <typename T>
void release(T *&value)
{
    if (value) {
        value->Release();
        value = nullptr;
    }
}

std::wstring hrText(HRESULT hr)
{
    wchar_t buffer[32] = {};
    swprintf_s(buffer, L"0x%08lx", static_cast<unsigned long>(hr));
    return buffer;
}

bool isFloatMono(const WAVEFORMATEX *format)
{
    if (!format || format->nChannels != 1 || format->wBitsPerSample != 32) {
        return false;
    }
    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        return true;
    }
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE
        && format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto *extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(format);
        return extensible->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    }
    return false;
}

void printUsage()
{
    std::wcerr << L"Usage: SpatialBedProbe [--duration-ms <milliseconds>] [--frequency <hz>]" << std::endl;
}

} // namespace

int wmain(int argc, wchar_t **argv)
{
    DWORD durationMs = 8000;
    double baseFrequency = 220.0;
    for (int index = 1; index < argc; ++index) {
        const std::wstring option = argv[index];
        if (option == L"--duration-ms" && index + 1 < argc) {
            try {
                durationMs = static_cast<DWORD>(std::stoul(argv[++index]));
            } catch (...) {
                printUsage();
                return 2;
            }
        } else if (option == L"--frequency" && index + 1 < argc) {
            try {
                baseFrequency = std::stod(argv[++index]);
            } catch (...) {
                printUsage();
                return 2;
            }
        } else {
            printUsage();
            return 2;
        }
    }
    if (durationMs < 1000 || durationMs > 600000 || baseFrequency <= 0.0 || baseFrequency > 20000.0) {
        printUsage();
        return 2;
    }

    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool shouldUninitialize = SUCCEEDED(comHr);
    if (FAILED(comHr) && comHr != RPC_E_CHANGED_MODE) {
        std::wcerr << L"probeResult=FAIL stage=CoInitializeEx hr=" << hrText(comHr) << std::endl;
        return 1;
    }

    IMMDeviceEnumerator *enumerator = nullptr;
    IMMDevice *device = nullptr;
    ISpatialAudioClient *spatialClient = nullptr;
    IAudioFormatEnumerator *formatEnumerator = nullptr;
    WAVEFORMATEX *selectedFormat = nullptr;
    ISpatialAudioObjectRenderStream *stream = nullptr;
    HANDLE eventHandle = nullptr;
    std::vector<ISpatialAudioObject *> objects;
    SpatialAudioObjectRenderStreamActivationParams activation = {};
    PROPVARIANT activationProperty;
    PropVariantInit(&activationProperty);
    int result = 1;

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator),
                                  nullptr,
                                  CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void **>(&enumerator));
    if (SUCCEEDED(hr)) {
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    }
    if (FAILED(hr) || !device) {
        std::wcerr << L"probeResult=FAIL stage=defaultEndpoint hr=" << hrText(hr) << std::endl;
        goto cleanup;
    }
    std::wcout << L"endpointSelected=1" << std::endl;
    std::wcout << L"stage=beforeSpatialClientActivate" << std::endl;

    hr = device->Activate(__uuidof(ISpatialAudioClient),
                          CLSCTX_ALL,
                          nullptr,
                          reinterpret_cast<void **>(&spatialClient));
    if (FAILED(hr) || !spatialClient) {
        std::wcerr << L"probeResult=FAIL stage=activateSpatialClient hr=" << hrText(hr) << std::endl;
        goto cleanup;
    }

    hr = spatialClient->IsSpatialAudioStreamAvailable(__uuidof(ISpatialAudioObjectRenderStream), nullptr);
    std::wcout << L"spatialStreamAvailable=" << (SUCCEEDED(hr) ? 1 : 0)
               << L" hr=" << hrText(hr) << std::endl;
    if (FAILED(hr)) {
        std::wcerr << L"probeResult=FAIL stage=streamAvailability hr=" << hrText(hr) << std::endl;
        goto cleanup;
    }

    const AudioObjectType staticMask = static_cast<AudioObjectType>(
        AudioObjectType_FrontLeft
        | AudioObjectType_FrontRight
        | AudioObjectType_FrontCenter
        | AudioObjectType_LowFrequency
        | AudioObjectType_SideLeft
        | AudioObjectType_SideRight
        | AudioObjectType_TopFrontLeft
        | AudioObjectType_TopFrontRight);
    AudioObjectType nativeMask = AudioObjectType_None;
    hr = spatialClient->GetNativeStaticObjectTypeMask(&nativeMask);
    std::wcout << L"nativeStaticMask=0x" << std::hex << static_cast<unsigned int>(nativeMask)
               << std::dec << std::endl;
    if (FAILED(hr) || (nativeMask & staticMask) != staticMask) {
        std::wcerr << L"probeResult=FAIL stage=staticMask hr=" << hrText(hr)
                   << L" required=0x" << std::hex << static_cast<unsigned int>(staticMask)
                   << L" native=0x" << static_cast<unsigned int>(nativeMask) << std::dec << std::endl;
        goto cleanup;
    }

    hr = spatialClient->GetSupportedAudioObjectFormatEnumerator(&formatEnumerator);
    if (FAILED(hr) || !formatEnumerator) {
        std::wcerr << L"probeResult=FAIL stage=formatEnumerator hr=" << hrText(hr) << std::endl;
        goto cleanup;
    }
    UINT32 formatCount = 0;
    formatEnumerator->GetCount(&formatCount);
    for (UINT32 index = 0; index < formatCount; ++index) {
        WAVEFORMATEX *candidate = nullptr;
        if (SUCCEEDED(formatEnumerator->GetFormat(index, &candidate)) && isFloatMono(candidate)) {
            selectedFormat = candidate;
            break;
        }
        if (candidate) {
            CoTaskMemFree(candidate);
        }
    }
    if (!selectedFormat) {
        std::wcerr << L"probeResult=FAIL stage=monoFloatFormat formatCount=" << formatCount << std::endl;
        goto cleanup;
    }
    std::wcout << L"objectFormatRate=" << selectedFormat->nSamplesPerSec
               << L" objectFormatChannels=" << selectedFormat->nChannels
               << L" objectFormatBits=" << selectedFormat->wBitsPerSample << std::endl;

    eventHandle = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!eventHandle) {
        std::wcerr << L"probeResult=FAIL stage=createEvent error=" << GetLastError() << std::endl;
        goto cleanup;
    }

    activation.ObjectFormat = selectedFormat;
    activation.StaticObjectTypeMask = staticMask;
    activation.MinDynamicObjectCount = 0;
    activation.MaxDynamicObjectCount = 0;
    activation.Category = AudioCategory_Media;
    activation.EventHandle = eventHandle;
    activation.NotifyObject = nullptr;
    activationProperty.vt = VT_BLOB;
    activationProperty.blob.cbSize = sizeof(activation);
    activationProperty.blob.pBlobData = reinterpret_cast<BYTE *>(&activation);
    hr = spatialClient->ActivateSpatialAudioStream(&activationProperty,
                                                   __uuidof(ISpatialAudioObjectRenderStream),
                                                   reinterpret_cast<void **>(&stream));
    if (FAILED(hr) || !stream) {
        std::wcerr << L"probeResult=FAIL stage=activateSpatialStream hr=" << hrText(hr) << std::endl;
        goto cleanup;
    }

    const AudioObjectType objectTypes[] = {
        AudioObjectType_FrontLeft,
        AudioObjectType_FrontRight,
        AudioObjectType_FrontCenter,
        AudioObjectType_LowFrequency,
        AudioObjectType_SideLeft,
        AudioObjectType_SideRight,
        AudioObjectType_TopFrontLeft,
        AudioObjectType_TopFrontRight,
    };
    for (const AudioObjectType type : objectTypes) {
        ISpatialAudioObject *object = nullptr;
        hr = stream->ActivateSpatialAudioObject(type, &object);
        if (FAILED(hr) || !object) {
            std::wcerr << L"probeResult=FAIL stage=activateObject hr=" << hrText(hr) << std::endl;
            release(object);
            goto cleanup;
        }
        objects.push_back(object);
    }

    hr = stream->Start();
    if (FAILED(hr)) {
        std::wcerr << L"probeResult=FAIL stage=start hr=" << hrText(hr) << std::endl;
        goto cleanup;
    }

    const double sampleRate = static_cast<double>(selectedFormat->nSamplesPerSec);
    const ULONGLONG startTick = GetTickCount64();
    ULONGLONG totalFrames = 0;
    UINT32 updates = 0;
    bool firstUpdate = true;
    while (GetTickCount64() - startTick < durationMs) {
        WaitForSingleObject(eventHandle, 100);
        UINT32 availableDynamicObjects = 0;
        UINT32 frameCount = 0;
        hr = stream->BeginUpdatingAudioObjects(&availableDynamicObjects, &frameCount);
        if (FAILED(hr)) {
            std::wcerr << L"probeResult=FAIL stage=beginUpdate hr=" << hrText(hr) << std::endl;
            goto cleanup;
        }
        for (size_t channel = 0; channel < objects.size(); ++channel) {
            BYTE *buffer = nullptr;
            UINT32 bufferLength = 0;
            hr = objects[channel]->GetBuffer(&buffer, &bufferLength);
            if (FAILED(hr) || !buffer) {
                std::wcerr << L"probeResult=FAIL stage=getObjectBuffer channel=" << channel
                           << L" hr=" << hrText(hr) << std::endl;
                stream->EndUpdatingAudioObjects();
                goto cleanup;
            }
            const UINT32 bufferFrames = (std::min)(frameCount,
                                                   static_cast<UINT32>(bufferLength / sizeof(float)));
            auto *samples = reinterpret_cast<float *>(buffer);
            const double frequency = baseFrequency + static_cast<double>(channel) * 55.0;
            for (UINT32 frame = 0; frame < bufferFrames; ++frame) {
                const double t = static_cast<double>(totalFrames + frame) / sampleRate;
                samples[frame] = static_cast<float>(0.03 * std::sin(2.0 * kPi * frequency * t));
            }
            if (bufferFrames * sizeof(float) < bufferLength) {
                std::fill(reinterpret_cast<BYTE *>(samples + bufferFrames),
                          reinterpret_cast<BYTE *>(buffer) + bufferLength,
                          static_cast<BYTE>(0));
            }
        }
        hr = stream->EndUpdatingAudioObjects();
        if (FAILED(hr)) {
            std::wcerr << L"probeResult=FAIL stage=endUpdate hr=" << hrText(hr) << std::endl;
            goto cleanup;
        }
        totalFrames += frameCount;
        ++updates;
        if (firstUpdate) {
            std::wcout << L"firstUpdateFrames=" << frameCount << std::endl;
            firstUpdate = false;
        }
    }
    stream->Stop();
    std::wcout << L"updates=" << updates << L" submittedFrames=" << totalFrames << std::endl;
    std::wcout << L"probeResult=" << (updates > 0 ? L"PASS" : L"FAIL")
               << L" verification=PCM-submitted-only manual-listening-required=1" << std::endl;
    result = updates > 0 ? 0 : 1;

cleanup:
    if (stream) {
        stream->Stop();
    }
    for (ISpatialAudioObject *object : objects) {
        release(object);
    }
    release(stream);
    if (eventHandle) {
        CloseHandle(eventHandle);
    }
    PropVariantClear(&activationProperty);
    if (selectedFormat) {
        CoTaskMemFree(selectedFormat);
    }
    release(formatEnumerator);
    release(spatialClient);
    release(device);
    release(enumerator);
    if (shouldUninitialize) {
        CoUninitialize();
    }
    return result;
}
