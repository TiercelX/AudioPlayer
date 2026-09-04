#include "spatial-bridge-renderer.h"

#include <mmdeviceapi.h>
#include <objbase.h>
#include <propkeydef.h>
#include <propvarutil.h>
#include <ks.h>
#include <ksmedia.h>
#include <spatialaudioclient.h>
#include <SpatialAudioHrtf.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

namespace eac3renderer {
namespace {

constexpr UINT32 kObjectCount = eac3gate7b::kDynamicObjectCount;
constexpr UINT32 kSampleRate = 48000U;

float programHeadroomVolume()
{
    const double headroomDb = eac3gate7b::referenceGeometry().gainHeadroomDb;
    return static_cast<float>(std::pow(10.0, -headroomDb / 20.0));
}

float lfeVolumeForOptions(const Options &options)
{
    return options.disableLfe ? 0.0f : programHeadroomVolume();
}

template <typename T>
void release(T *&value)
{
    if (value) {
        value->Release();
        value = nullptr;
    }
}

bool isMonoFloat48(const WAVEFORMATEX *format)
{
    if (!format || format->nChannels != 1 || format->nSamplesPerSec != kSampleRate
        || format->wBitsPerSample != 32) {
        return false;
    }
    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE
        && format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto *extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(format);
        return extensible->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    }
    return false;
}

std::string narrowId(LPWSTR value)
{
    if (!value) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (count <= 1) return {};
    std::string result(static_cast<std::size_t>(count), '\0');
    const int written = WideCharToMultiByte(
        CP_UTF8, 0, value, -1, result.data(), count, nullptr, nullptr);
    if (written <= 1) return {};
    result.resize(static_cast<std::size_t>(written - 1));
    return result;
}

std::int64_t hrValue(HRESULT hr) { return static_cast<std::int64_t>(hr); }

class CapacityNotify final : public ISpatialAudioObjectRenderStreamNotify {
public:
    explicit CapacityNotify(UINT32 requested) : requested_(requested) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override
    {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid == IID_IUnknown || iid == __uuidof(ISpatialAudioObjectRenderStreamNotify)) {
            *object = static_cast<ISpatialAudioObjectRenderStreamNotify *>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG remaining = --references_;
        if (remaining == 0U) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE OnAvailableDynamicObjectCountChange(
        ISpatialAudioObjectRenderStreamBase *, LONGLONG, UINT32 available) override
    {
        latest_.store(available, std::memory_order_release);
        notifications_.fetch_add(1U, std::memory_order_relaxed);
        if (available < requested_) belowContract_.store(true, std::memory_order_release);
        return S_OK;
    }
    UINT32 notifications() const { return notifications_.load(std::memory_order_acquire); }
    UINT32 latest() const { return latest_.load(std::memory_order_acquire); }
    bool belowContract() const { return belowContract_.load(std::memory_order_acquire); }

private:
    std::atomic<ULONG> references_ {1U};
    UINT32 requested_;
    std::atomic<UINT32> notifications_ {0U};
    std::atomic<UINT32> latest_ {UINT32_MAX};
    std::atomic<bool> belowContract_ {false};
};

bool enoughCapacity(UINT32 available) { return available >= kObjectCount; }

eac3gate7b::Geometry geometryForOptions(const Options &options)
{
    eac3gate7b::Geometry geometry = eac3gate7b::referenceGeometry();
    geometry.positionRadiusMode = options.positionRadiusMode;
    geometry.positionDirectionMode = options.positionDirectionMode;
    geometry.azimuthFocus = options.azimuthFocus;
    return geometry;
}

} // namespace

const char *outcomeText(Outcome outcome)
{
    switch (outcome) {
    case Outcome::Pass: return "PASS";
    case Outcome::Fail: return "FAIL";
    case Outcome::Inconclusive: return "INCONCLUSIVE";
    }
    return "INCONCLUSIVE";
}

const char *spatialRendererText(SpatialRenderer renderer)
{
    return renderer == SpatialRenderer::Hrtf ? "hrtf" : "standard";
}

const char *hrtfEnvironmentText(HrtfEnvironment environment)
{
    return environment == HrtfEnvironment::Outdoors ? "outdoors" : "small";
}

bool validateOptions(const Options &options, std::string *reason)
{
    if (!reason) return false;
    *reason = {};
    if (options.queueBatches < 2U || options.queueBatches > 64U) {
        *reason = "queue-capacity-out-of-range";
        return false;
    }
    if (options.prebufferBatches < 1U || options.prebufferBatches > options.queueBatches) {
        *reason = "prebuffer-out-of-range";
        return false;
    }
    if (options.pushTimeoutMilliseconds < 100U || options.pushTimeoutMilliseconds > 10000U) {
        *reason = "push-timeout-out-of-range";
        return false;
    }
    if (options.positionRadiusMode != eac3gate7b::PositionRadiusMode::Source
        && options.positionRadiusMode != eac3gate7b::PositionRadiusMode::Unit) {
        *reason = "position-radius-mode-invalid";
        return false;
    }
    if (options.positionDirectionMode != eac3gate7b::PositionDirectionMode::Metadata
        && options.positionDirectionMode != eac3gate7b::PositionDirectionMode::Front) {
        *reason = "position-direction-mode-invalid";
        return false;
    }
    if (!std::isfinite(options.azimuthFocus) || options.azimuthFocus < 0.0f
        || options.azimuthFocus > 1.0f) {
        *reason = "azimuth-focus-out-of-range-or-nonfinite";
        return false;
    }
    if (options.spatialRenderer != SpatialRenderer::Standard
        && options.spatialRenderer != SpatialRenderer::Hrtf) {
        *reason = "spatial-renderer-invalid";
        return false;
    }
    if (options.hrtfEnvironment != HrtfEnvironment::Small
        && options.hrtfEnvironment != HrtfEnvironment::Outdoors) {
        *reason = "hrtf-environment-invalid";
        return false;
    }
    if (options.spatialRenderer == SpatialRenderer::Hrtf && !options.disableLfe) {
        *reason = "hrtf-requires-disable-lfe";
        return false;
    }
    return true;
}

bool exactBufferLength(std::size_t frameCount, std::size_t byteLength)
{
    if (frameCount > (std::numeric_limits<std::size_t>::max)() / sizeof(float)) return false;
    return byteLength == frameCount * sizeof(float);
}

bool exactObjectFrameLength(std::size_t frameCount, std::size_t sampleCount)
{
    return sampleCount == frameCount;
}

bool scaleHrtfSamples(const float *source, float *destination, std::size_t count,
                      float volume)
{
    if (!source || !destination || !std::isfinite(volume) || volume < 0.0f) return false;
    for (std::size_t index = 0; index < count; ++index) {
        const double scaled = static_cast<double>(source[index]) * volume;
        if (!std::isfinite(source[index]) || !std::isfinite(scaled)
            || std::abs(scaled) > static_cast<double>((std::numeric_limits<float>::max)())) {
            return false;
        }
        destination[index] = static_cast<float>(scaled);
    }
    return true;
}

struct SpatialBridgeRenderer::Impl {
    explicit Impl(Options value)
        : options(value), core(value.queueBatches, 1U, geometryForOptions(value))
    {
    }

    Options options;
    eac3bridge::SpatialBridgeCore core;
    mutable std::mutex mutex;
    std::condition_variable stateCv;
    std::condition_variable dataCv;
    std::thread consumer;
    bool ready = false;
    bool initFailed = false;
    bool producerClosed = false;
    bool cancelIssued = false;
    bool joined = false;
    Metrics metrics;

    IMMDeviceEnumerator *enumerator = nullptr;
    IMMDevice *device = nullptr;
    ISpatialAudioClient *spatialClient = nullptr;
    IAudioFormatEnumerator *formatEnumerator = nullptr;
    WAVEFORMATEX *selectedFormat = nullptr;
    ISpatialAudioObjectRenderStream *stream = nullptr;
    ISpatialAudioObjectRenderStreamForHrtf *hrtfStream = nullptr;
    CapacityNotify *notify = nullptr;
    HANDLE eventHandle = nullptr;
    ISpatialAudioObject *lfe = nullptr;
    std::vector<ISpatialAudioObject *> dynamicObjects;
    std::vector<ISpatialAudioObjectForHrtf *> hrtfObjects;
    SpatialAudioHrtfEnvironmentType hrtfEnvironmentValue = SpatialAudioHrtfEnvironment_Small;
    SpatialAudioHrtfDirectivityUnion hrtfDirectivity = {};
    SpatialAudioHrtfOrientation hrtfOrientation = {};

    void record(Outcome outcome, const char *stage, HRESULT hr)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (metrics.firstFailureStage.empty()) {
            metrics.firstFailureStage = stage;
            metrics.firstFailureHresult = hrValue(hr);
        }
        if (outcome == Outcome::Fail || metrics.outcome == Outcome::Pass) {
            metrics.outcome = outcome;
        } else if (metrics.outcome != Outcome::Fail) {
            metrics.outcome = outcome;
        }
    }

    void signalReady(bool failed)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            ready = true;
            initFailed = failed;
            metrics.initialized = !failed;
        }
        stateCv.notify_all();
    }

    bool initializeEndpoint()
    {
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      __uuidof(IMMDeviceEnumerator),
                                      reinterpret_cast<void **>(&enumerator));
        if (SUCCEEDED(hr)) hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (FAILED(hr) || !device) {
            record(Outcome::Inconclusive, "defaultEndpoint", hr);
            return false;
        }
        LPWSTR id = nullptr;
        if (SUCCEEDED(device->GetId(&id)) && id) {
            metrics.endpointId = narrowId(id);
            CoTaskMemFree(id);
        }
        hr = device->Activate(__uuidof(ISpatialAudioClient), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void **>(&spatialClient));
        if (FAILED(hr) || !spatialClient) {
            record(Outcome::Inconclusive, "activateSpatialClient", hr);
            return false;
        }
        metrics.renderer = spatialRendererText(options.spatialRenderer);
        metrics.hrtfEnvironment = hrtfEnvironmentText(options.hrtfEnvironment);
        const IID streamIid = options.spatialRenderer == SpatialRenderer::Hrtf
            ? __uuidof(ISpatialAudioObjectRenderStreamForHrtf)
            : __uuidof(ISpatialAudioObjectRenderStream);
        hr = spatialClient->IsSpatialAudioStreamAvailable(streamIid, nullptr);
        if (hr != S_OK) {
            record(Outcome::Inconclusive, "streamAvailability", hr);
            return false;
        }
        metrics.interfaceAvailable = true;
        UINT32 maximum = 0;
        hr = spatialClient->GetMaxDynamicObjectCount(&maximum);
        if (FAILED(hr) || maximum < kObjectCount) {
            record(Outcome::Inconclusive, "dynamicCapacity", FAILED(hr) ? hr : S_FALSE);
            return false;
        }
        metrics.maximumDynamicObjects = maximum;
        AudioObjectType mask = AudioObjectType_None;
        hr = spatialClient->GetNativeStaticObjectTypeMask(&mask);
        if (options.spatialRenderer == SpatialRenderer::Standard
            && (FAILED(hr) || (mask & AudioObjectType_LowFrequency) == 0)) {
            record(Outcome::Inconclusive, "staticLfeCapability", FAILED(hr) ? hr : S_FALSE);
            return false;
        }
        metrics.nativeStaticMask = static_cast<std::uint32_t>(mask);
        hr = spatialClient->GetSupportedAudioObjectFormatEnumerator(&formatEnumerator);
        if (FAILED(hr) || !formatEnumerator) {
            record(Outcome::Inconclusive, "formatEnumerator", hr);
            return false;
        }
        UINT32 formatCount = 0;
        formatEnumerator->GetCount(&formatCount);
        for (UINT32 index = 0; index < formatCount; ++index) {
            WAVEFORMATEX *candidate = nullptr;
            if (SUCCEEDED(formatEnumerator->GetFormat(index, &candidate))
                && isMonoFloat48(candidate)) {
                selectedFormat = candidate;
                break;
            }
            if (candidate) CoTaskMemFree(candidate);
        }
        if (!selectedFormat) {
            record(Outcome::Inconclusive, "monoFloat48Format", S_FALSE);
            return false;
        }
        metrics.objectFormatRate = selectedFormat->nSamplesPerSec;
        metrics.objectFormatChannels = selectedFormat->nChannels;
        metrics.objectFormatBits = selectedFormat->wBitsPerSample;
        eventHandle = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!eventHandle) {
            record(Outcome::Inconclusive, "createEvent", HRESULT_FROM_WIN32(GetLastError()));
            return false;
        }
        notify = new CapacityNotify(kObjectCount);
        SpatialAudioObjectRenderStreamActivationParams activation = {};
        activation.ObjectFormat = selectedFormat;
        activation.StaticObjectTypeMask = AudioObjectType_LowFrequency;
        activation.MinDynamicObjectCount = kObjectCount;
        activation.MaxDynamicObjectCount = kObjectCount;
        activation.Category = AudioCategory_Media;
        activation.EventHandle = eventHandle;
        activation.NotifyObject = notify;
        SpatialAudioHrtfActivationParams hrtfActivation = {};
        if (options.spatialRenderer == SpatialRenderer::Hrtf) {
            hrtfEnvironmentValue = options.hrtfEnvironment == HrtfEnvironment::Outdoors
                ? SpatialAudioHrtfEnvironment_Outdoors : SpatialAudioHrtfEnvironment_Small;
            hrtfDirectivity.Omni.Type = SpatialAudioHrtfDirectivity_OmniDirectional;
            hrtfDirectivity.Omni.Scaling = 0.0f;
            const float identity[9] = {1.0f, 0.0f, 0.0f,
                                       0.0f, 1.0f, 0.0f,
                                       0.0f, 0.0f, 1.0f};
            std::copy(std::begin(identity), std::end(identity), std::begin(hrtfOrientation));
            hrtfActivation.ObjectFormat = selectedFormat;
            hrtfActivation.StaticObjectTypeMask = AudioObjectType_None;
            hrtfActivation.MinDynamicObjectCount = kObjectCount;
            hrtfActivation.MaxDynamicObjectCount = kObjectCount;
            hrtfActivation.Category = AudioCategory_Media;
            hrtfActivation.EventHandle = eventHandle;
            hrtfActivation.NotifyObject = notify;
            hrtfActivation.DistanceDecay = nullptr;
            hrtfActivation.Directivity = &hrtfDirectivity;
            hrtfActivation.Environment = &hrtfEnvironmentValue;
            hrtfActivation.Orientation = &hrtfOrientation;
        }
        PROPVARIANT property;
        PropVariantInit(&property);
        property.vt = VT_BLOB;
        if (options.spatialRenderer == SpatialRenderer::Hrtf) {
            property.blob.cbSize = sizeof(hrtfActivation);
            property.blob.pBlobData = reinterpret_cast<BYTE *>(&hrtfActivation);
            hr = spatialClient->ActivateSpatialAudioStream(
                &property, __uuidof(ISpatialAudioObjectRenderStreamForHrtf),
                reinterpret_cast<void **>(&hrtfStream));
        } else {
            property.blob.cbSize = sizeof(activation);
            property.blob.pBlobData = reinterpret_cast<BYTE *>(&activation);
            hr = spatialClient->ActivateSpatialAudioStream(
                &property, __uuidof(ISpatialAudioObjectRenderStream),
                reinterpret_cast<void **>(&stream));
        }
        property.vt = VT_EMPTY;
        if (FAILED(hr) || (options.spatialRenderer == SpatialRenderer::Hrtf ? !hrtfStream : !stream)) {
            record(Outcome::Inconclusive, "activateSpatialStream", hr);
            return false;
        }
        return true;
    }

    bool waitForPrebuffer()
    {
        constexpr std::uint32_t requiredInitialMask =
            (1U << eac3gate7b::kDynamicObjectCount) - 1U;
        std::unique_lock<std::mutex> lock(mutex);
        dataCv.wait(lock, [&] {
            const auto queue = core.queueMetrics();
            return producerClosed || cancelIssued
                || (queue.itemsRemaining >= options.prebufferBatches
                    && queue.initialPropertyMask == requiredInitialMask)
                || core.metrics().canceledCurrentFrames != 0U;
        });
        if (cancelIssued) return false;
        const auto queue = core.queueMetrics();
        metrics.prebufferBatches = queue.itemsRemaining;
        metrics.prebufferFrames = queue.framesRemaining;
        metrics.initialPropertiesReady = queue.initialPropertyMask == requiredInitialMask;
        return queue.itemsRemaining != 0U && metrics.initialPropertiesReady;
    }

    bool renderOne(UINT32 frameCount, bool *terminal)
    {
        ++metrics.renderAttempts;
        if (frameCount == 0U) {
            record(Outcome::Inconclusive, "zeroFrameEvent", E_INVALIDARG);
            return false;
        }
        eac3bridge::RenderQuantum quantum;
        const auto status = core.renderQuantum(frameCount, options.pushTimeoutMilliseconds, &quantum);
        if (status == eac3bridge::RenderStatus::Canceled) {
            metrics.canceledStagedFrames += quantum.sourceFrames;
            return false;
        }
        if (status == eac3bridge::RenderStatus::EndOfStream) return false;
        if (status != eac3bridge::RenderStatus::Pass) {
            record(Outcome::Fail, "coreRenderQuantum", E_FAIL);
            return false;
        }
        metrics.metadataApplied += quantum.metadataApplied;
        metrics.metadataDeferred += quantum.metadataDeferred;
        metrics.maximumPropertyLateness = (std::max)(
            metrics.maximumPropertyLateness, quantum.maximumPropertyLateness);
        metrics.sourceFrames += quantum.sourceFrames;
        metrics.paddingFrames += quantum.finalPaddingFrames;
        metrics.underrunFrames += quantum.underrunFrames;
        if (metrics.frameCountMin == 0U) metrics.frameCountMin = frameCount;
        metrics.frameCountMin = (std::min)(metrics.frameCountMin,
                                           static_cast<std::size_t>(frameCount));
        metrics.frameCountMax = (std::max)(metrics.frameCountMax,
                                           static_cast<std::size_t>(frameCount));
        if (quantum.objects.size() != kObjectCount || quantum.properties.size() != kObjectCount
            || quantum.lfe.size() != frameCount) {
            record(Outcome::Fail, "quantumShape", E_FAIL);
            return false;
        }
        bool pass = true;
        HRESULT hr = S_OK;
        if (options.spatialRenderer == SpatialRenderer::Standard) {
            BYTE *buffer = nullptr;
            UINT32 length = 0;
            hr = lfe->GetBuffer(&buffer, &length);
            if (FAILED(hr) || !buffer || !exactBufferLength(frameCount, length)) {
                record(Outcome::Inconclusive, "lfeBuffer", FAILED(hr) ? hr : S_FALSE);
                pass = false;
            } else {
                std::memset(buffer, 0, length);
                std::memcpy(buffer, quantum.lfe.data(), length);
                ++metrics.exactBufferCalls;
                metrics.exactBufferBytes += length;
                for (float value : quantum.lfe) {
                    if (!std::isfinite(value)) pass = false;
                    ++metrics.lfeFiniteSamples;
                    metrics.lfePeak = (std::max)(metrics.lfePeak, std::abs(value));
                }
            }
        }
        for (unsigned index = 0U; index < kObjectCount && pass; ++index) {
            if (!exactObjectFrameLength(frameCount, quantum.objects[index].size())) {
                record(Outcome::Fail, "objectFrameCount", E_INVALIDARG);
                pass = false;
                break;
            }
            BOOL active = FALSE;
            if (options.spatialRenderer == SpatialRenderer::Hrtf) {
                hr = hrtfObjects[index]->IsActive(&active);
            } else {
                hr = dynamicObjects[index]->IsActive(&active);
            }
            if (FAILED(hr) || !active) {
                ++metrics.inactiveObjects;
                record(Outcome::Inconclusive, "inactiveDynamicObject", FAILED(hr) ? hr : S_FALSE);
                pass = false;
                break;
            }
            BYTE *objectBuffer = nullptr;
            UINT32 objectLength = 0;
            if (options.spatialRenderer == SpatialRenderer::Hrtf) {
                hr = hrtfObjects[index]->GetBuffer(&objectBuffer, &objectLength);
            } else {
                hr = dynamicObjects[index]->GetBuffer(&objectBuffer, &objectLength);
            }
            if (FAILED(hr) || !objectBuffer || !exactBufferLength(frameCount, objectLength)) {
                record(Outcome::Inconclusive, "dynamicBuffer", FAILED(hr) ? hr : S_FALSE);
                pass = false;
                break;
            }
            std::memset(objectBuffer, 0, objectLength);
            const auto &property = quantum.properties[index];
            if (options.spatialRenderer == SpatialRenderer::Hrtf) {
                std::vector<float> scaled(frameCount);
                if (!scaleHrtfSamples(quantum.objects[index].data(), scaled.data(), frameCount,
                                      property.volume)) {
                    record(Outcome::Fail, "hrtfPreScale", E_INVALIDARG);
                    pass = false;
                    break;
                }
                std::memcpy(objectBuffer, scaled.data(), objectLength);
                if (metrics.preScaledSamples == 0U) {
                    metrics.preScaleMin = property.volume;
                    metrics.preScaleMax = property.volume;
                } else {
                    metrics.preScaleMin = (std::min)(metrics.preScaleMin, property.volume);
                    metrics.preScaleMax = (std::max)(metrics.preScaleMax, property.volume);
                }
                metrics.preScaledSamples += frameCount;
            } else {
                std::memcpy(objectBuffer, quantum.objects[index].data(), objectLength);
            }
            ++metrics.exactBufferCalls;
            metrics.exactBufferBytes += objectLength;
            if (options.spatialRenderer == SpatialRenderer::Hrtf) {
                hr = hrtfObjects[index]->SetPosition(property.position[0], property.position[1],
                                                      property.position[2]);
            } else {
                hr = dynamicObjects[index]->SetPosition(property.position[0], property.position[1],
                                                        property.position[2]);
            }
            if (FAILED(hr)) {
                record(Outcome::Inconclusive, "setPosition", hr);
                pass = false;
                break;
            }
            ++metrics.propertyCalls;
            if (options.spatialRenderer == SpatialRenderer::Standard) {
                hr = dynamicObjects[index]->SetVolume(property.volume);
                if (FAILED(hr)) {
                    record(Outcome::Inconclusive, "setVolume", hr);
                    pass = false;
                    break;
                }
                ++metrics.propertyCalls;
            }
            for (float value : quantum.objects[index]) {
                if (!std::isfinite(value)) pass = false;
                ++metrics.finiteSamples[index + 1U];
                const float measured = options.spatialRenderer == SpatialRenderer::Hrtf
                    ? value * property.volume : value;
                if (!std::isfinite(measured)) pass = false;
                metrics.peaks[index + 1U] = (std::max)(metrics.peaks[index + 1U],
                                                        std::abs(measured));
            }
        }
        if (!pass) return false;
        metrics.submittedFrames += frameCount;
        ++metrics.renderCommits;
        metrics.terminalQuantum = quantum.terminal;
        if (terminal) *terminal = quantum.terminal;
        return true;
    }

    void consumerMain()
    {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(com) && com != RPC_E_CHANGED_MODE) {
            record(Outcome::Inconclusive, "CoInitializeEx", com);
            signalReady(true);
            return;
        }
        const bool uninitialize = SUCCEEDED(com);
        if (!initializeEndpoint()) {
            signalReady(true);
            cleanup(uninitialize, false);
            return;
        }
        signalReady(false);
        if (!waitForPrebuffer()) {
            record(Outcome::Fail,
                   metrics.prebufferBatches == 0U
                       ? "prebufferEmpty" : "initialPropertiesNotReady",
                   E_FAIL);
            cleanup(uninitialize, false);
            return;
        }
        HRESULT hr = options.spatialRenderer == SpatialRenderer::Hrtf
            ? hrtfStream->Start() : stream->Start();
        if (FAILED(hr)) {
            record(Outcome::Inconclusive, "startSpatialStream", hr);
            cleanup(uninitialize, false);
            return;
        }
        metrics.streamStarted = true;
        metrics.outcome = Outcome::Pass;
        bool firstPass = true;
        bool terminal = false;
        while (!terminal) {
            const DWORD wait = WaitForSingleObject(eventHandle, 1000U);
            if (wait == WAIT_TIMEOUT) continue;
            if (wait == WAIT_FAILED) {
                record(Outcome::Inconclusive, "waitSpatialEvent",
                       HRESULT_FROM_WIN32(GetLastError()));
                break;
            }
            ++metrics.beginCalls;
            UINT32 available = 0U;
            UINT32 frameCount = 0U;
            hr = options.spatialRenderer == SpatialRenderer::Hrtf
                ? hrtfStream->BeginUpdatingAudioObjects(&available, &frameCount)
                : stream->BeginUpdatingAudioObjects(&available, &frameCount);
            if (FAILED(hr)) {
                record(Outcome::Inconclusive, "beginUpdate", hr);
                break;
            }
            bool updateOpen = true;
            if (available < kObjectCount || (notify && notify->belowContract())) {
                record(Outcome::Inconclusive, "capacityDuringUpdate", S_FALSE);
            }
            bool pass = available >= kObjectCount && (!notify || !notify->belowContract());
            if (pass && firstPass) {
                if (options.spatialRenderer == SpatialRenderer::Hrtf) {
                    hrtfObjects.reserve(kObjectCount);
                    for (unsigned index = 0U; pass && index < kObjectCount; ++index) {
                        ISpatialAudioObjectForHrtf *object = nullptr;
                        hr = hrtfStream->ActivateSpatialAudioObjectForHrtf(
                            AudioObjectType_Dynamic, &object);
                        if (FAILED(hr) || !object) {
                            record(Outcome::Inconclusive, "activateHrtfDynamicObject", hr);
                            release(object);
                            pass = false;
                            break;
                        }
                        hrtfObjects.push_back(object);
                    }
                    metrics.lfeVolume = 0.0f;
                    metrics.activatedDynamicObjects = static_cast<std::uint32_t>(hrtfObjects.size());
                } else {
                    hr = stream->ActivateSpatialAudioObject(AudioObjectType_LowFrequency, &lfe);
                    if (FAILED(hr) || !lfe) {
                        record(Outcome::Inconclusive, "activateLfe", hr);
                        pass = false;
                    } else {
                        const float volume = lfeVolumeForOptions(options);
                        hr = lfe->SetVolume(volume);
                        if (FAILED(hr)) {
                            record(Outcome::Inconclusive, "setLfeVolume", hr);
                            pass = false;
                        } else {
                            metrics.lfeVolume = volume;
                            ++metrics.lfeVolumeCalls;
                        }
                    }
                    dynamicObjects.reserve(kObjectCount);
                    for (unsigned index = 0U; pass && index < kObjectCount; ++index) {
                        ISpatialAudioObject *object = nullptr;
                        hr = stream->ActivateSpatialAudioObject(AudioObjectType_Dynamic, &object);
                        if (FAILED(hr) || !object) {
                            record(Outcome::Inconclusive, "activateDynamicObject", hr);
                            release(object);
                            pass = false;
                            break;
                        }
                        dynamicObjects.push_back(object);
                    }
                    metrics.activatedDynamicObjects = static_cast<std::uint32_t>(dynamicObjects.size());
                }
                firstPass = false;
            }
            if (pass) pass = renderOne(frameCount, &terminal);
            const HRESULT end = options.spatialRenderer == SpatialRenderer::Hrtf
                ? hrtfStream->EndUpdatingAudioObjects() : stream->EndUpdatingAudioObjects();
            updateOpen = false;
            ++metrics.endCalls;
            if (FAILED(end)) {
                record(Outcome::Inconclusive, "endUpdate", end);
                break;
            }
            if (!pass) break;
        }
        cleanup(uninitialize, true);
    }

    void cleanup(bool uninitialize, bool started)
    {
        if ((stream || hrtfStream) && started) {
            const HRESULT hr = options.spatialRenderer == SpatialRenderer::Hrtf
                ? hrtfStream->Stop() : stream->Stop();
            if (FAILED(hr)) record(Outcome::Inconclusive, "stopSpatialStream", hr);
            else metrics.streamStopped = true;
        }
        release(lfe);
        const auto activatedCount = options.spatialRenderer == SpatialRenderer::Hrtf
            ? hrtfObjects.size() : dynamicObjects.size();
        metrics.activatedDynamicObjects = static_cast<std::uint32_t>(activatedCount);
        for (auto *object : dynamicObjects) release(object);
        dynamicObjects.clear();
        for (auto *object : hrtfObjects) release(object);
        hrtfObjects.clear();
        if (stream || hrtfStream) {
            const HRESULT hr = options.spatialRenderer == SpatialRenderer::Hrtf
                ? hrtfStream->Reset() : stream->Reset();
            if (SUCCEEDED(hr)) metrics.streamReset = true;
            else record(Outcome::Inconclusive, "resetSpatialStream", hr);
        }
        release(stream);
        release(hrtfStream);
        if (eventHandle) CloseHandle(eventHandle);
        eventHandle = nullptr;
        if (notify) {
            metrics.capacityNotifications = notify->notifications();
            metrics.latestCapacity = notify->latest();
            notify->Release();
        }
        notify = nullptr;
        if (selectedFormat) CoTaskMemFree(selectedFormat);
        selectedFormat = nullptr;
        release(formatEnumerator);
        release(spatialClient);
        release(device);
        release(enumerator);
        metrics.queue = core.queueMetrics();
        const auto bridgeMetrics = core.metrics();
        metrics.eosPendingMetadata = bridgeMetrics.eosPendingMetadata;
        metrics.terminalQuanta = bridgeMetrics.terminalQuanta;
        metrics.metadataApplied = bridgeMetrics.metadataApplied;
        metrics.metadataDeferred = bridgeMetrics.metadataDeferred;
        metrics.maximumPropertyLateness = bridgeMetrics.maximumPropertyLateness;
        metrics.underrunFrames = bridgeMetrics.underrunFrames;
        metrics.cleanupComplete = !lfe && dynamicObjects.empty() && hrtfObjects.empty()
            && !stream && !hrtfStream && !eventHandle
            && !notify && metrics.streamReset;
        if (uninitialize) CoUninitialize();
        metrics.comReleased = true;
        if (metrics.outcome == Outcome::Inconclusive && metrics.firstFailureStage.empty()) {
            metrics.firstFailureStage = "cleanup";
        }
    }
};

SpatialBridgeRenderer::SpatialBridgeRenderer(Options options)
    : impl_(std::make_unique<Impl>(options))
{
}

SpatialBridgeRenderer::~SpatialBridgeRenderer()
{
    if (impl_ && impl_->consumer.joinable()) cancel();
}

bool SpatialBridgeRenderer::start()
{
    std::string reason;
    if (!validateOptions(impl_->options, &reason)) {
        impl_->record(Outcome::Fail, reason.c_str(), E_INVALIDARG);
        return false;
    }
    impl_->consumer = std::thread([state = impl_.get()] { state->consumerMain(); });
    std::unique_lock<std::mutex> lock(impl_->mutex);
    if (!impl_->stateCv.wait_for(lock, std::chrono::seconds(5),
                                 [&] { return impl_->ready; })) {
        lock.unlock();
        cancel();
        impl_->metrics.outcome = Outcome::Inconclusive;
        impl_->metrics.firstFailureStage = "consumerInitializationTimeout";
        impl_->metrics.firstFailureHresult = WAIT_TIMEOUT;
        return false;
    }
    const bool failed = impl_->initFailed;
    lock.unlock();
    if (failed && impl_->consumer.joinable()) {
        impl_->consumer.join();
        impl_->joined = true;
        impl_->metrics.consumerJoined = true;
    }
    return !failed;
}

eac3bridge::QueueStatus SpatialBridgeRenderer::submit(eac3gate6c::Batch batch)
{
    if (!impl_ || !impl_->ready || impl_->initFailed) return eac3bridge::QueueStatus::Rejected;
    const auto status = impl_->core.push(std::move(batch), impl_->options.pushTimeoutMilliseconds);
    impl_->dataCv.notify_one();
    return status;
}

void SpatialBridgeRenderer::finish()
{
    if (!impl_) return;
    impl_->core.close();
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->producerClosed = true;
    }
    impl_->dataCv.notify_all();
    if (impl_->consumer.joinable()) {
        impl_->consumer.join();
        impl_->joined = true;
        impl_->metrics.consumerJoined = true;
    }
}

void SpatialBridgeRenderer::cancel()
{
    if (!impl_) return;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->cancelIssued = true;
    }
    impl_->core.cancel();
    impl_->dataCv.notify_all();
    if (impl_->consumer.joinable()) {
        impl_->consumer.join();
        impl_->joined = true;
        impl_->metrics.consumerJoined = true;
    }
}

Metrics SpatialBridgeRenderer::result() const
{
    if (!impl_) return {};
    std::lock_guard<std::mutex> lock(impl_->mutex);
    Metrics result = impl_->metrics;
    result.queue = impl_->core.queueMetrics();
    return result;
}

namespace {
eac3gate6c::Batch selfTestBatch()
{
    eac3gate6c::Batch batch;
    batch.outputStart = 0;
    batch.outputEnd = 4;
    batch.objects.assign(kObjectCount, std::vector<float>(4, 0.25f));
    batch.lfe.assign(4, 0.125f);
    for (unsigned object = 1U; object <= kObjectCount; ++object) {
        eac3gate6c::MetadataUpdate update;
        update.objectIndex = object;
        update.sourcePosition = 0;
        update.state.objectIndex = object;
        update.state.active = true;
        update.state.basicValid = true;
        update.state.renderValid = true;
        update.state.position.valid = true;
        update.state.position.x = 0.5f;
        update.state.position.y = 0.5f;
        update.state.position.z = 0.0f;
        update.state.zoneConstraints = {true, true, true, true, true, true};
        update.state.elevation = true;
        batch.metadata.push_back(update);
    }
    return batch;
}
} // namespace

SelfTestReport runSelfTest()
{
    SelfTestReport report;
    auto check = [&](bool pass, const char *reason) {
        ++report.cases;
        if (!pass && report.reason.empty()) report.reason = reason;
    };
    std::string reason;
    check(validateOptions(Options{}, &reason), "default-options");
    check(std::string(outcomeText(Outcome::Pass)) == "PASS"
              && std::string(outcomeText(Outcome::Fail)) == "FAIL"
              && std::string(outcomeText(Outcome::Inconclusive)) == "INCONCLUSIVE",
          "outcome-texts");
    check(!validateOptions(Options {1U, 1U, 2000U}, &reason), "queue-lower-bound");
    check(!validateOptions(Options {8U, 9U, 2000U}, &reason), "prebuffer-bound");
    check(!validateOptions(Options {8U, 4U, 99U}, &reason), "timeout-lower-bound");
    check(validateOptions(Options {64U, 64U, 10000U}, &reason), "option-upper-bound");
    Options hrtf;
    hrtf.spatialRenderer = SpatialRenderer::Hrtf;
    hrtf.disableLfe = true;
    check(validateOptions(hrtf, &reason)
              && std::string(spatialRendererText(hrtf.spatialRenderer)) == "hrtf",
          "hrtf-disable-lfe-contract");
    Options hrtfWithoutLfe = hrtf;
    hrtfWithoutLfe.disableLfe = false;
    check(!validateOptions(hrtfWithoutLfe, &reason) && reason == "hrtf-requires-disable-lfe",
          "hrtf-requires-lfe-disable");
    Options hrtfOutdoors = hrtf;
    hrtfOutdoors.hrtfEnvironment = HrtfEnvironment::Outdoors;
    check(validateOptions(hrtfOutdoors, &reason)
              && std::string(hrtfEnvironmentText(hrtfOutdoors.hrtfEnvironment)) == "outdoors",
          "hrtf-environment-contract");
    Options unitRadius;
    unitRadius.positionRadiusMode = eac3gate7b::PositionRadiusMode::Unit;
    check(validateOptions(unitRadius, &reason)
              && geometryForOptions(unitRadius).positionRadiusMode
                  == eac3gate7b::PositionRadiusMode::Unit,
          "unit-radius-option-propagation");
    Options invalidRadius;
    invalidRadius.positionRadiusMode = static_cast<eac3gate7b::PositionRadiusMode>(99);
    check(!validateOptions(invalidRadius, &reason), "invalid-radius-option");
    Options frontDirection;
    frontDirection.positionDirectionMode = eac3gate7b::PositionDirectionMode::Front;
    check(validateOptions(frontDirection, &reason)
              && geometryForOptions(frontDirection).positionDirectionMode
                  == eac3gate7b::PositionDirectionMode::Front,
          "front-direction-option-propagation");
    Options invalidDirection;
    invalidDirection.positionDirectionMode =
        static_cast<eac3gate7b::PositionDirectionMode>(99);
    check(!validateOptions(invalidDirection, &reason), "invalid-direction-option");
    Options focus;
    focus.azimuthFocus = 0.5f;
    check(validateOptions(focus, &reason)
              && std::abs(geometryForOptions(focus).azimuthFocus - 0.5f) < 0.00001f,
          "azimuth-focus-option-propagation");
    Options invalidFocus;
    invalidFocus.azimuthFocus = 1.1f;
    check(!validateOptions(invalidFocus, &reason), "invalid-azimuth-focus-option");
    check(exactBufferLength(0U, 0U) && exactBufferLength(1536U, 6144U),
          "exact-buffer-length");
    check(!exactBufferLength(1536U, 6140U), "short-buffer-rejected");
    check(exactObjectFrameLength(480U, 480U)
              && !exactObjectFrameLength(480U, 479U),
          "object-frame-count-boundary");
    check(!exactBufferLength((std::numeric_limits<std::size_t>::max)(), 0U),
          "buffer-overflow-rejected");
    const float scaleInput[] = {1.0f, -0.5f, 0.25f};
    float scaleOutput[3] = {};
    check(scaleHrtfSamples(scaleInput, scaleOutput, 3U, 0.5f)
              && std::abs(scaleOutput[0] - 0.5f) < 0.000001f
              && std::abs(scaleOutput[1] + 0.25f) < 0.000001f,
          "hrtf-prescale-equivalence");
    const float nonfiniteInput[] = {std::numeric_limits<float>::quiet_NaN()};
    check(!scaleHrtfSamples(nonfiniteInput, scaleOutput, 1U, 1.0f),
          "hrtf-nonfinite-rejected");
    check(std::abs(programHeadroomVolume() - 0.177827941f) < 0.0000001f,
          "lfe-program-headroom-volume");
    check(!Options{}.disableLfe && std::abs(lfeVolumeForOptions(Options{})
                                                - 0.177827941f) < 0.0000001f,
          "lfe-default-policy");
    Options disabledLfe;
    disabledLfe.disableLfe = true;
    check(std::abs(lfeVolumeForOptions(disabledLfe)) < 0.0000001f,
          "lfe-disabled-policy");
    eac3bridge::SpatialBridgeCore core(2U);
    auto batch = selfTestBatch();
    check(core.push(batch) == eac3bridge::QueueStatus::Pass, "selftest-batch-admission");
    core.close();
    eac3bridge::RenderQuantum quantum;
    check(core.renderQuantum(4U, 0U, &quantum) == eac3bridge::RenderStatus::Pass
              && quantum.terminal && quantum.sourceFrames == 4U
              && quantum.objects.size() == kObjectCount
              && quantum.properties.size() == kObjectCount,
          "terminal-quantum-shape");
    check(core.metrics().terminalQuanta == 1U && core.renderQuantum(1U, 0U, &quantum)
              == eac3bridge::RenderStatus::EndOfStream,
          "terminal-no-extra-quantum");
    report.pass = report.reason.empty();
    return report;
}

} // namespace eac3renderer
