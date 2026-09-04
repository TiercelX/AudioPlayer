#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <audioclient.h>
#include <avrt.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <objbase.h>
#include <propkeydef.h>
#include <propvarutil.h>
#include <functiondiscoverykeys_devpkey.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr int kSchemaVersion = 1;
constexpr double kClippingThreshold = 0.9990;
constexpr double kPeakSpikeThreshold = 0.8500;
constexpr double kSampleDeltaThreshold = 0.3000;
constexpr double kShortTransientPeakThreshold = 0.0800;
constexpr double kShortTransientPeakToRmsRatio = 8.0;
constexpr double kDropoutSilencePeakThreshold = 0.000001;
constexpr size_t kMaxReportedCandidates = 200;
constexpr size_t kMaxRecentAudiblePackets = 8;

template <typename T>
class ComPtr
{
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }

    ComPtr(const ComPtr &) = delete;
    ComPtr &operator=(const ComPtr &) = delete;

    T **put()
    {
        reset();
        return &m_ptr;
    }

    T *get() const { return m_ptr; }
    T *operator->() const { return m_ptr; }
    explicit operator bool() const { return m_ptr != nullptr; }

    void reset()
    {
        if (m_ptr) {
            m_ptr->Release();
            m_ptr = nullptr;
        }
    }

private:
    T *m_ptr = nullptr;
};

class CoTaskMemWaveFormat
{
public:
    ~CoTaskMemWaveFormat()
    {
        if (m_format) {
            CoTaskMemFree(m_format);
        }
    }

    WAVEFORMATEX **put()
    {
        if (m_format) {
            CoTaskMemFree(m_format);
            m_format = nullptr;
        }
        return &m_format;
    }

    WAVEFORMATEX *get() const { return m_format; }

private:
    WAVEFORMATEX *m_format = nullptr;
};

struct Options
{
    int durationMs = 5000;
    int simulateInvalidationAfterMs = -1;
    std::filesystem::path wavFile;
    std::filesystem::path reportFile;
    std::filesystem::path readyFile;
    std::filesystem::path stopFile;
};

struct MixFormatInfo
{
    uint16_t formatTag = 0;
    uint16_t channels = 0;
    uint32_t sampleRate = 0;
    uint32_t avgBytesPerSec = 0;
    uint16_t blockAlign = 0;
    uint16_t bitsPerSample = 0;
    uint16_t validBitsPerSample = 0;
    uint16_t cbSize = 0;
    std::string subFormat;
    bool metricsSupported = false;
    bool isFloat = false;
    bool isPcmInt = false;
};

struct TransientCandidate
{
    double timeMs = 0.0;
    double peak = 0.0;
    double sampleDelta = 0.0;
    std::string reason;
};

struct DropoutCandidate
{
    double timeMs = 0.0;
    double durationMs = 0.0;
    uint64_t silentPackets = 0;
};

struct AudiblePacketMetrics
{
    double timeMs = 0.0;
    double peak = 0.0;
    double rms = 0.0;
};

struct SegmentReport
{
    int segmentIndex = 0;
    std::filesystem::path wavFile;
    std::filesystem::path metadataFile;
    std::wstring deviceName;
    std::wstring deviceId;
    MixFormatInfo mixFormat;
    std::string startedUtc;
    std::string endedUtc;
    int64_t durationMs = 0;
    bool interrupted = false;
    std::string interruptionReason;
    std::string interruptionHresult;
    std::string interruptionStage;
    uint64_t framesCaptured = 0;
    uint64_t packetsCaptured = 0;
    uint64_t silentPackets = 0;
    double maxPeak = 0.0;
    double blockRmsMax = 0.0;
    double maxSampleDelta = 0.0;
    uint64_t transientCandidateCount = 0;
    uint64_t dropoutCandidateCount = 0;
    double trailingSilenceStartMs = -1.0;
    double trailingSilenceDurationMs = 0.0;
};

struct CaptureMetrics
{
    bool metricsSupported = false;
    bool captureInterrupted = false;
    std::string interruptionReason;
    std::string interruptionHresult;
    std::string interruptionStage;
    int segmentCount = 0;
    uint64_t framesCaptured = 0;
    uint64_t packetsCaptured = 0;
    uint64_t silentPackets = 0;
    double maxPeak = 0.0;
    double blockRmsMax = 0.0;
    double maxSampleDelta = 0.0;
    uint64_t transientCandidateCount = 0;
    bool transientCandidatesTruncated = false;
    std::vector<TransientCandidate> transientCandidates;
    uint64_t dropoutCandidateCount = 0;
    bool dropoutCandidatesTruncated = false;
    std::vector<DropoutCandidate> dropoutCandidates;
    double trailingSilenceStartMs = -1.0;
    double trailingSilenceDurationMs = 0.0;
    std::vector<AudiblePacketMetrics> recentAudiblePackets;
    std::vector<SegmentReport> segments;
};

struct BlockMetrics
{
    bool valid = false;
    double peak = 0.0;
    double rms = 0.0;
    double maxSampleDelta = 0.0;
};

int tailFadeDescendingStepCount(const CaptureMetrics &metrics)
{
    int count = 0;
    for (size_t i = 1; i < metrics.recentAudiblePackets.size(); ++i) {
        if (metrics.recentAudiblePackets[i].peak
            < metrics.recentAudiblePackets[i - 1].peak * 0.98) {
            ++count;
        }
    }
    return count;
}

bool tailFadeCandidateObserved(const CaptureMetrics &metrics)
{
    if (metrics.trailingSilenceDurationMs <= 0.0 || metrics.recentAudiblePackets.size() < 3) {
        return false;
    }

    const double firstPeak = metrics.recentAudiblePackets.front().peak;
    const double lastPeak = metrics.recentAudiblePackets.back().peak;
    return firstPeak > kDropoutSilencePeakThreshold
        && lastPeak <= firstPeak * 0.50
        && tailFadeDescendingStepCount(metrics) >= 2;
}

std::string narrowWide(const std::wstring &value)
{
    if (value.empty()) {
        return {};
    }
    const int required = WideCharToMultiByte(CP_UTF8,
                                            0,
                                            value.c_str(),
                                            static_cast<int>(value.size()),
                                            nullptr,
                                            0,
                                            nullptr,
                                            nullptr);
    if (required <= 0) {
        return {};
    }
    std::string result(static_cast<size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8,
                        0,
                        value.c_str(),
                        static_cast<int>(value.size()),
                        result.data(),
                        required,
                        nullptr,
                        nullptr);
    return result;
}

std::string pathUtf8(const std::filesystem::path &path)
{
    return narrowWide(std::filesystem::absolute(path).wstring());
}

std::string hresultText(HRESULT hr)
{
    std::ostringstream out;
    out << "0x" << std::hex << std::uppercase << static_cast<uint32_t>(hr);
    return out.str();
}

class HResultError : public std::runtime_error
{
public:
    HResultError(HRESULT hr, const std::string &stage)
        : std::runtime_error(stage + " failed: hr=" + hresultText(hr))
        , m_hr(hr)
        , m_stage(stage)
    {
    }

    HRESULT hresult() const { return m_hr; }
    const std::string &stage() const { return m_stage; }

private:
    HRESULT m_hr = S_OK;
    std::string m_stage;
};

bool isAudioClientInvalidatedHresult(HRESULT hr)
{
    return hr == AUDCLNT_E_DEVICE_INVALIDATED
        || hr == AUDCLNT_E_RESOURCES_INVALIDATED
        || hr == AUDCLNT_E_SERVICE_NOT_RUNNING;
}

std::string invalidationReasonForHresult(HRESULT hr)
{
    if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
        return "device-invalidated";
    }
    if (hr == AUDCLNT_E_RESOURCES_INVALIDATED) {
        return "resources-invalidated";
    }
    if (hr == AUDCLNT_E_SERVICE_NOT_RUNNING) {
        return "audio-service-not-running";
    }
    return "audio-client-invalidated";
}

void recordCaptureInterruption(CaptureMetrics &metrics,
                               SegmentReport *segment,
                               HRESULT hr,
                               const std::string &stage)
{
    if (!metrics.captureInterrupted) {
        metrics.captureInterrupted = true;
        metrics.interruptionReason = invalidationReasonForHresult(hr);
        metrics.interruptionHresult = hresultText(hr);
        metrics.interruptionStage = stage;
    }

    if (segment && !segment->interrupted) {
        segment->interrupted = true;
        segment->interruptionReason = invalidationReasonForHresult(hr);
        segment->interruptionHresult = hresultText(hr);
        segment->interruptionStage = stage;
    }
}

bool handleCaptureHresult(HRESULT hr,
                          const std::string &stage,
                          CaptureMetrics &metrics,
                          SegmentReport *segment)
{
    if (SUCCEEDED(hr)) {
        return true;
    }
    if (isAudioClientInvalidatedHresult(hr)) {
        recordCaptureInterruption(metrics, segment, hr, stage);
        return false;
    }
    throw HResultError(hr, stage);
}

void throwIfFailed(HRESULT hr, const std::string &stage)
{
    if (FAILED(hr)) {
        throw HResultError(hr, stage);
    }
}

std::string jsonEscape(const std::string &value)
{
    std::ostringstream out;
    for (const unsigned char ch : value) {
        switch (ch) {
        case '\\':
            out << "\\\\";
            break;
        case '"':
            out << "\\\"";
            break;
        case '\b':
            out << "\\b";
            break;
        case '\f':
            out << "\\f";
            break;
        case '\n':
            out << "\\n";
            break;
        case '\r':
            out << "\\r";
            break;
        case '\t':
            out << "\\t";
            break;
        default:
            if (ch < 0x20) {
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                    << static_cast<int>(ch);
            } else {
                out << ch;
            }
            break;
        }
    }
    return out.str();
}

std::string utcNowIso()
{
    SYSTEMTIME st {};
    GetSystemTime(&st);
    std::ostringstream out;
    out << std::setfill('0')
        << std::setw(4) << st.wYear << "-"
        << std::setw(2) << st.wMonth << "-"
        << std::setw(2) << st.wDay << "T"
        << std::setw(2) << st.wHour << ":"
        << std::setw(2) << st.wMinute << ":"
        << std::setw(2) << st.wSecond << "."
        << std::setw(3) << st.wMilliseconds << "Z";
    return out.str();
}

std::wstring argValue(const wchar_t *name, int &index, int argc, wchar_t **argv)
{
    if (index + 1 >= argc) {
        throw std::runtime_error("Missing value for argument " + narrowWide(name));
    }
    ++index;
    return argv[index];
}

Options parseOptions(int argc, wchar_t **argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--help" || arg == L"-h") {
            std::wcout << L"Usage: WasapiLoopbackCapture --duration-ms N --wav FILE --report FILE [--ready-file FILE] [--stop-file FILE]\n";
            std::exit(0);
        }
        if (arg == L"--duration-ms") {
            options.durationMs = std::stoi(argValue(L"--duration-ms", i, argc, argv));
        } else if (arg == L"--simulate-invalidation-after-ms") {
            options.simulateInvalidationAfterMs =
                std::stoi(argValue(L"--simulate-invalidation-after-ms", i, argc, argv));
        } else if (arg == L"--wav") {
            options.wavFile = argValue(L"--wav", i, argc, argv);
        } else if (arg == L"--report") {
            options.reportFile = argValue(L"--report", i, argc, argv);
        } else if (arg == L"--ready-file") {
            options.readyFile = argValue(L"--ready-file", i, argc, argv);
        } else if (arg == L"--stop-file") {
            options.stopFile = argValue(L"--stop-file", i, argc, argv);
        } else {
            throw std::runtime_error("Unknown argument: " + narrowWide(arg));
        }
    }

    if (options.durationMs <= 0) {
        throw std::runtime_error("--duration-ms must be greater than zero");
    }
    if (options.wavFile.empty()) {
        throw std::runtime_error("--wav is required");
    }
    if (options.reportFile.empty()) {
        throw std::runtime_error("--report is required");
    }
    return options;
}

bool stopRequested(const Options &options)
{
    if (options.stopFile.empty()) {
        return false;
    }
    std::error_code error;
    return std::filesystem::exists(options.stopFile, error);
}

std::filesystem::path segmentWavPath(const std::filesystem::path &basePath, int segmentIndex)
{
    if (segmentIndex <= 1) {
        return basePath;
    }

    const std::filesystem::path parent = basePath.parent_path();
    const std::wstring stem = basePath.stem().wstring();
    std::wstring extension = basePath.extension().wstring();
    if (extension.empty()) {
        extension = L".wav";
    }
    std::wostringstream name;
    name << stem << L"-segment" << segmentIndex << extension;
    return parent / name.str();
}

std::filesystem::path segmentMetadataPath(const std::filesystem::path &basePath, int segmentIndex)
{
    const std::filesystem::path parent = basePath.parent_path();
    const std::wstring stem = basePath.stem().wstring();
    std::wostringstream name;
    name << stem << L"-segment" << segmentIndex << L".report.json";
    return parent / name.str();
}

void ensureParentDirectory(const std::filesystem::path &path)
{
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }
}

void writeTextFile(const std::filesystem::path &path, const std::string &contents)
{
    ensureParentDirectory(path);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("Failed to open file for writing: " + pathUtf8(path));
    }
    out << contents;
}

void writeUInt16(std::ofstream &out, uint16_t value)
{
    out.write(reinterpret_cast<const char *>(&value), sizeof(value));
}

void writeUInt32(std::ofstream &out, uint32_t value)
{
    out.write(reinterpret_cast<const char *>(&value), sizeof(value));
}

size_t waveFormatByteSize(const WAVEFORMATEX *format)
{
    if (!format) {
        return 0;
    }
    return sizeof(WAVEFORMATEX) + format->cbSize;
}

std::string guidToString(const GUID &guid)
{
    wchar_t buffer[64] {};
    StringFromGUID2(guid, buffer, static_cast<int>(std::size(buffer)));
    return narrowWide(buffer);
}

MixFormatInfo describeMixFormat(const WAVEFORMATEX *format)
{
    MixFormatInfo info;
    if (!format) {
        return info;
    }

    info.formatTag = format->wFormatTag;
    info.channels = format->nChannels;
    info.sampleRate = format->nSamplesPerSec;
    info.avgBytesPerSec = format->nAvgBytesPerSec;
    info.blockAlign = format->nBlockAlign;
    info.bitsPerSample = format->wBitsPerSample;
    info.validBitsPerSample = format->wBitsPerSample;
    info.cbSize = format->cbSize;

    GUID subFormat {};
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE
        && format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto *extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(format);
        info.validBitsPerSample = extensible->Samples.wValidBitsPerSample > 0
            ? extensible->Samples.wValidBitsPerSample
            : format->wBitsPerSample;
        subFormat = extensible->SubFormat;
        info.subFormat = guidToString(subFormat);
    }

    info.isFloat = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT
        || (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE
            && IsEqualGUID(subFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT));
    info.isPcmInt = format->wFormatTag == WAVE_FORMAT_PCM
        || (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE
            && IsEqualGUID(subFormat, KSDATAFORMAT_SUBTYPE_PCM));
    info.metricsSupported = info.channels > 0
        && info.blockAlign > 0
        && ((info.isFloat && (info.bitsPerSample == 32 || info.bitsPerSample == 64))
            || (info.isPcmInt
                && (info.bitsPerSample == 8 || info.bitsPerSample == 16
                    || info.bitsPerSample == 24 || info.bitsPerSample == 32)));
    return info;
}

double readInt24Sample(const uint8_t *sample)
{
    int32_t value = static_cast<int32_t>(sample[0])
        | (static_cast<int32_t>(sample[1]) << 8)
        | (static_cast<int32_t>(sample[2]) << 16);
    if (value & 0x00800000) {
        value |= 0xFF000000;
    }
    return static_cast<double>(value) / 8388608.0;
}

double readNormalizedSample(const uint8_t *sample, const MixFormatInfo &format)
{
    if (format.isFloat) {
        if (format.bitsPerSample == 32) {
            float value = 0.0f;
            std::memcpy(&value, sample, sizeof(value));
            return std::isfinite(value) ? std::clamp(static_cast<double>(value), -8.0, 8.0) : 0.0;
        }
        if (format.bitsPerSample == 64) {
            double value = 0.0;
            std::memcpy(&value, sample, sizeof(value));
            return std::isfinite(value) ? std::clamp(value, -8.0, 8.0) : 0.0;
        }
    }

    if (!format.isPcmInt) {
        return 0.0;
    }

    switch (format.bitsPerSample) {
    case 8:
        return (static_cast<int>(*sample) - 128) / 128.0;
    case 16: {
        int16_t value = 0;
        std::memcpy(&value, sample, sizeof(value));
        return static_cast<double>(value) / 32768.0;
    }
    case 24:
        return readInt24Sample(sample);
    case 32: {
        int32_t value = 0;
        std::memcpy(&value, sample, sizeof(value));
        const int validBits = std::clamp<int>(format.validBitsPerSample, 1, 32);
        const double denominator = std::ldexp(1.0, validBits - 1);
        return static_cast<double>(value) / denominator;
    }
    default:
        break;
    }
    return 0.0;
}

BlockMetrics analyzeBlock(const std::vector<uint8_t> &data,
                          uint32_t frames,
                          const MixFormatInfo &format,
                          std::vector<double> &previousSamples,
                          bool &hasPreviousSamples)
{
    BlockMetrics metrics;
    if (!format.metricsSupported || frames == 0 || data.empty()) {
        return metrics;
    }

    const uint16_t bytesPerSample = format.bitsPerSample / 8;
    if (bytesPerSample == 0 || format.channels == 0 || format.blockAlign == 0) {
        return metrics;
    }

    metrics.valid = true;
    double sumSquares = 0.0;
    uint64_t sampleCount = 0;
    if (previousSamples.size() != format.channels) {
        previousSamples.assign(format.channels, 0.0);
        hasPreviousSamples = false;
    }

    for (uint32_t frame = 0; frame < frames; ++frame) {
        const uint8_t *frameData = data.data() + static_cast<size_t>(frame) * format.blockAlign;
        for (uint16_t channel = 0; channel < format.channels; ++channel) {
            const uint8_t *sampleData = frameData + static_cast<size_t>(channel) * bytesPerSample;
            const double sample = readNormalizedSample(sampleData, format);
            const double magnitude = std::abs(sample);
            metrics.peak = std::max(metrics.peak, magnitude);
            sumSquares += sample * sample;
            ++sampleCount;
            if (hasPreviousSamples) {
                metrics.maxSampleDelta = std::max(metrics.maxSampleDelta,
                                                  std::abs(sample - previousSamples[channel]));
            }
            previousSamples[channel] = sample;
        }
        hasPreviousSamples = true;
    }

    metrics.rms = sampleCount > 0 ? std::sqrt(sumSquares / static_cast<double>(sampleCount)) : 0.0;
    return metrics;
}

std::string candidateReason(const BlockMetrics &metrics)
{
    std::vector<std::string> reasons;
    if (metrics.peak >= kClippingThreshold) {
        reasons.push_back("clipping");
    }
    if (metrics.peak >= kPeakSpikeThreshold) {
        reasons.push_back("peak-spike");
    }
    if (metrics.maxSampleDelta >= kSampleDeltaThreshold) {
        reasons.push_back("sudden-sample-delta");
    }
    if (metrics.peak >= kShortTransientPeakThreshold
        && metrics.rms > 0.0
        && metrics.peak / metrics.rms >= kShortTransientPeakToRmsRatio) {
        reasons.push_back("short-transient-peak-to-rms");
    }

    if (reasons.empty()) {
        return {};
    }

    std::ostringstream joined;
    for (size_t i = 0; i < reasons.size(); ++i) {
        if (i > 0) {
            joined << ",";
        }
        joined << reasons[i];
    }
    return joined.str();
}

class WavWriter
{
public:
    WavWriter(const std::filesystem::path &path, const WAVEFORMATEX *format)
        : m_path(path)
    {
        ensureParentDirectory(path);
        m_out.open(path, std::ios::binary | std::ios::trunc);
        if (!m_out) {
            throw std::runtime_error("Failed to open wav file: " + pathUtf8(path));
        }

        const size_t formatSize = waveFormatByteSize(format);
        if (formatSize < sizeof(WAVEFORMATEX) || formatSize > std::numeric_limits<uint32_t>::max()) {
            throw std::runtime_error("Unsupported wave format size");
        }

        m_out.write("RIFF", 4);
        writeUInt32(m_out, 0);
        m_out.write("WAVE", 4);
        m_out.write("fmt ", 4);
        writeUInt32(m_out, static_cast<uint32_t>(formatSize));
        m_out.write(reinterpret_cast<const char *>(format), static_cast<std::streamsize>(formatSize));
        m_out.write("data", 4);
        m_dataSizeOffset = m_out.tellp();
        writeUInt32(m_out, 0);
    }

    void writeFrames(const std::vector<uint8_t> &data)
    {
        if (data.empty()) {
            return;
        }
        if (m_dataBytes + data.size() > std::numeric_limits<uint32_t>::max()) {
            throw std::runtime_error("WAV data exceeded 4 GiB RIFF limit");
        }
        m_out.write(reinterpret_cast<const char *>(data.data()),
                    static_cast<std::streamsize>(data.size()));
        m_dataBytes += data.size();
    }

    uint64_t dataBytes() const { return m_dataBytes; }

    void finalize()
    {
        if (m_finalized) {
            return;
        }
        m_out.flush();
        const auto fileSize = static_cast<uint32_t>(m_out.tellp());
        m_out.seekp(4, std::ios::beg);
        writeUInt32(m_out, fileSize - 8);
        m_out.seekp(m_dataSizeOffset, std::ios::beg);
        writeUInt32(m_out, static_cast<uint32_t>(m_dataBytes));
        m_out.flush();
        m_finalized = true;
    }

    ~WavWriter()
    {
        try {
            finalize();
        } catch (...) {
        }
    }

private:
    std::filesystem::path m_path;
    std::ofstream m_out;
    std::streampos m_dataSizeOffset {};
    uint64_t m_dataBytes = 0;
    bool m_finalized = false;
};

std::string formatTagName(const MixFormatInfo &format)
{
    if (format.isFloat) {
        return "IEEE_FLOAT";
    }
    if (format.isPcmInt) {
        return "PCM";
    }
    if (format.formatTag == WAVE_FORMAT_EXTENSIBLE) {
        return "EXTENSIBLE";
    }
    std::ostringstream out;
    out << "0x" << std::hex << format.formatTag;
    return out.str();
}

std::wstring defaultDeviceId(IMMDevice *device)
{
    LPWSTR id = nullptr;
    throwIfFailed(device->GetId(&id), "IMMDevice::GetId");
    std::wstring result = id ? id : L"";
    CoTaskMemFree(id);
    return result;
}

std::wstring defaultDeviceName(IMMDevice *device)
{
    ComPtr<IPropertyStore> store;
    HRESULT hr = device->OpenPropertyStore(STGM_READ, store.put());
    if (FAILED(hr)) {
        return L"";
    }

    PROPVARIANT name;
    PropVariantInit(&name);
    hr = store->GetValue(PKEY_Device_FriendlyName, &name);
    std::wstring result;
    if (SUCCEEDED(hr) && name.vt == VT_LPWSTR && name.pwszVal) {
        result = name.pwszVal;
    }
    PropVariantClear(&name);
    return result;
}

std::string mixFormatJson(const MixFormatInfo &format)
{
    std::ostringstream out;
    out << "{"
        << "\"formatTag\":" << format.formatTag << ","
        << "\"formatTagName\":\"" << jsonEscape(formatTagName(format)) << "\","
        << "\"channels\":" << format.channels << ","
        << "\"sampleRate\":" << format.sampleRate << ","
        << "\"avgBytesPerSec\":" << format.avgBytesPerSec << ","
        << "\"blockAlign\":" << format.blockAlign << ","
        << "\"bitsPerSample\":" << format.bitsPerSample << ","
        << "\"validBitsPerSample\":" << format.validBitsPerSample << ","
        << "\"cbSize\":" << format.cbSize << ","
        << "\"subFormat\":\"" << jsonEscape(format.subFormat) << "\","
        << "\"metricsSupported\":" << (format.metricsSupported ? "true" : "false")
        << "}";
    return out.str();
}

void writeStringArray(std::ofstream &out,
                      const std::string &name,
                      const std::vector<std::string> &values)
{
    out << "  \"" << name << "\": [";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i > 0) {
            out << ", ";
        }
        out << "\"" << jsonEscape(values[i]) << "\"";
    }
    out << "],\n";
}

void writeSegmentMetadata(const SegmentReport &segment)
{
    ensureParentDirectory(segment.metadataFile);
    std::ofstream out(segment.metadataFile, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("Failed to open segment report file: " + pathUtf8(segment.metadataFile));
    }

    out << std::fixed << std::setprecision(6);
    out << "{\n";
    out << "  \"schemaVersion\": " << kSchemaVersion << ",\n";
    out << "  \"evidenceLayer\": \"wasapi-loopback-endpoint-capture-segment\",\n";
    out << "  \"verificationLayer\": \"endpoint-loopback-first-pass-transient-dropout-tail-detector\",\n";
    out << "  \"analysisLimitations\": \"Single segment metadata only; absence of detector candidates is not proof of pop-free endpoint output. Tail fade is a candidate signal, not a complete acoustic assertion.\",\n";
    out << "  \"segmentIndex\": " << segment.segmentIndex << ",\n";
    out << "  \"wavFile\": \"" << jsonEscape(pathUtf8(segment.wavFile)) << "\",\n";
    out << "  \"metadataFile\": \"" << jsonEscape(pathUtf8(segment.metadataFile)) << "\",\n";
    out << "  \"startedUtc\": \"" << jsonEscape(segment.startedUtc) << "\",\n";
    out << "  \"endedUtc\": \"" << jsonEscape(segment.endedUtc) << "\",\n";
    out << "  \"durationMs\": " << segment.durationMs << ",\n";
    out << "  \"deviceName\": \"" << jsonEscape(narrowWide(segment.deviceName)) << "\",\n";
    out << "  \"deviceId\": \"" << jsonEscape(narrowWide(segment.deviceId)) << "\",\n";
    out << "  \"mixFormat\": " << mixFormatJson(segment.mixFormat) << ",\n";
    out << "  \"interrupted\": " << (segment.interrupted ? "true" : "false") << ",\n";
    out << "  \"interruptionReason\": \"" << jsonEscape(segment.interruptionReason) << "\",\n";
    out << "  \"interruptionHresult\": \"" << jsonEscape(segment.interruptionHresult) << "\",\n";
    out << "  \"interruptionStage\": \"" << jsonEscape(segment.interruptionStage) << "\",\n";
    out << "  \"framesCaptured\": " << segment.framesCaptured << ",\n";
    out << "  \"packetsCaptured\": " << segment.packetsCaptured << ",\n";
    out << "  \"silentPackets\": " << segment.silentPackets << ",\n";
    out << "  \"maxPeak\": " << segment.maxPeak << ",\n";
    out << "  \"maxRms\": " << segment.blockRmsMax << ",\n";
    out << "  \"blockRmsMax\": " << segment.blockRmsMax << ",\n";
    out << "  \"maxSampleDelta\": " << segment.maxSampleDelta << ",\n";
    out << "  \"transientCandidateDetected\": "
        << (segment.transientCandidateCount > 0 ? "true" : "false") << ",\n";
    out << "  \"transientCandidateCount\": " << segment.transientCandidateCount << ",\n";
    out << "  \"dropoutCandidateDetected\": "
        << (segment.dropoutCandidateCount > 0 ? "true" : "false") << ",\n";
    out << "  \"dropoutCandidateCount\": " << segment.dropoutCandidateCount << ",\n";
    out << "  \"trailingSilenceStartMs\": " << segment.trailingSilenceStartMs << ",\n";
    out << "  \"trailingSilenceDurationMs\": " << segment.trailingSilenceDurationMs << "\n";
    out << "}\n";
}

void writeSegments(std::ofstream &out, const std::vector<SegmentReport> &segments)
{
    out << "  \"segments\": [\n";
    for (size_t i = 0; i < segments.size(); ++i) {
        const auto &segment = segments[i];
        out << "    {\n";
        out << "      \"segmentIndex\": " << segment.segmentIndex << ",\n";
        out << "      \"wavFile\": \"" << jsonEscape(pathUtf8(segment.wavFile)) << "\",\n";
        out << "      \"metadataFile\": \"" << jsonEscape(pathUtf8(segment.metadataFile)) << "\",\n";
        out << "      \"startedUtc\": \"" << jsonEscape(segment.startedUtc) << "\",\n";
        out << "      \"endedUtc\": \"" << jsonEscape(segment.endedUtc) << "\",\n";
        out << "      \"deviceName\": \"" << jsonEscape(narrowWide(segment.deviceName)) << "\",\n";
        out << "      \"deviceId\": \"" << jsonEscape(narrowWide(segment.deviceId)) << "\",\n";
        out << "      \"mixFormat\": " << mixFormatJson(segment.mixFormat) << ",\n";
        out << "      \"framesCaptured\": " << segment.framesCaptured << ",\n";
        out << "      \"durationMs\": " << segment.durationMs << ",\n";
        out << "      \"interrupted\": " << (segment.interrupted ? "true" : "false") << ",\n";
        out << "      \"interruptionReason\": \"" << jsonEscape(segment.interruptionReason) << "\",\n";
        out << "      \"interruptionHresult\": \"" << jsonEscape(segment.interruptionHresult) << "\",\n";
        out << "      \"interruptionStage\": \"" << jsonEscape(segment.interruptionStage) << "\",\n";
        out << "      \"maxPeak\": " << segment.maxPeak << ",\n";
        out << "      \"maxRms\": " << segment.blockRmsMax << ",\n";
        out << "      \"blockRmsMax\": " << segment.blockRmsMax << ",\n";
        out << "      \"maxSampleDelta\": " << segment.maxSampleDelta << ",\n";
        out << "      \"transientCandidateCount\": " << segment.transientCandidateCount << ",\n";
        out << "      \"dropoutCandidateCount\": " << segment.dropoutCandidateCount << ",\n";
        out << "      \"trailingSilenceStartMs\": " << segment.trailingSilenceStartMs << ",\n";
        out << "      \"trailingSilenceDurationMs\": " << segment.trailingSilenceDurationMs << "\n";
        out << "    }";
        if (i + 1 < segments.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ],\n";
}

void writeReport(const std::filesystem::path &path,
                 const std::string &result,
                 const std::string &error,
                 const std::wstring &deviceName,
                 const std::wstring &deviceId,
                 const MixFormatInfo &format,
                 const std::string &captureStartedUtc,
                 const std::string &captureEndedUtc,
                 int64_t durationMs,
                 const std::filesystem::path &wavFile,
                 const CaptureMetrics &metrics)
{
    ensureParentDirectory(path);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("Failed to open report file: " + pathUtf8(path));
    }

    out << std::fixed << std::setprecision(6);
    out << "{\n";
    out << "  \"schemaVersion\": " << kSchemaVersion << ",\n";
    out << "  \"result\": \"" << jsonEscape(result) << "\",\n";
    out << "  \"evidenceLayer\": \"wasapi-loopback-endpoint-capture\",\n";
    out << "  \"verificationLayer\": \"endpoint-loopback-first-pass-transient-dropout-tail-detector\",\n";
    out << "  \"analysisLimitations\": \"First-pass detector flags obvious peak, clipping, sudden sample-delta, short peak-to-rms transient, and resumed near-silence dropout candidates. Tail fade is a candidate signal for tone fixtures. Absence of candidates is not proof of pop-free playback.\",\n";
    out << "  \"error\": \"" << jsonEscape(error) << "\",\n";
    out << "  \"deviceName\": \"" << jsonEscape(narrowWide(deviceName)) << "\",\n";
    out << "  \"deviceId\": \"" << jsonEscape(narrowWide(deviceId)) << "\",\n";
    out << "  \"mixFormat\": " << mixFormatJson(format) << ",\n";
    out << "  \"captureStartedUtc\": \"" << jsonEscape(captureStartedUtc) << "\",\n";
    out << "  \"captureEndedUtc\": \"" << jsonEscape(captureEndedUtc) << "\",\n";
    out << "  \"durationMs\": " << durationMs << ",\n";
    out << "  \"wavFile\": \"" << jsonEscape(pathUtf8(wavFile)) << "\",\n";
    std::vector<std::string> wavFiles;
    std::vector<std::string> metadataFiles;
    std::vector<std::string> interruptionHresults;
    wavFiles.reserve(metrics.segments.size());
    metadataFiles.reserve(metrics.segments.size());
    for (const auto &segment : metrics.segments) {
        wavFiles.push_back(pathUtf8(segment.wavFile));
        metadataFiles.push_back(pathUtf8(segment.metadataFile));
        if (!segment.interruptionHresult.empty()) {
            interruptionHresults.push_back(segment.interruptionHresult);
        }
    }
    if (wavFiles.empty()) {
        wavFiles.push_back(pathUtf8(wavFile));
    }
    if (interruptionHresults.empty() && !metrics.interruptionHresult.empty()) {
        interruptionHresults.push_back(metrics.interruptionHresult);
    }
    writeStringArray(out, "wavFiles", wavFiles);
    writeStringArray(out, "segmentMetadataFiles", metadataFiles);
    out << "  \"captureInterrupted\": " << (metrics.captureInterrupted ? "true" : "false") << ",\n";
    out << "  \"interruptionReason\": \"" << jsonEscape(metrics.interruptionReason) << "\",\n";
    out << "  \"interruptionHresult\": \"" << jsonEscape(metrics.interruptionHresult) << "\",\n";
    writeStringArray(out, "interruptionHresults", interruptionHresults);
    out << "  \"interruptionStage\": \"" << jsonEscape(metrics.interruptionStage) << "\",\n";
    out << "  \"segmentCount\": " << metrics.segmentCount << ",\n";
    out << "  \"framesCaptured\": " << metrics.framesCaptured << ",\n";
    out << "  \"capturedAudioMs\": "
        << (format.sampleRate > 0
                ? (static_cast<double>(metrics.framesCaptured) * 1000.0
                   / static_cast<double>(format.sampleRate))
                : 0.0)
        << ",\n";
    out << "  \"packetsCaptured\": " << metrics.packetsCaptured << ",\n";
    out << "  \"silentPackets\": " << metrics.silentPackets << ",\n";
    out << "  \"metricsSupported\": " << (metrics.metricsSupported ? "true" : "false") << ",\n";
    out << "  \"maxPeak\": " << metrics.maxPeak << ",\n";
    out << "  \"maxRms\": " << metrics.blockRmsMax << ",\n";
    out << "  \"blockRmsMax\": " << metrics.blockRmsMax << ",\n";
    out << "  \"maxSampleDelta\": " << metrics.maxSampleDelta << ",\n";
    out << "  \"transientCandidateDetected\": "
        << (metrics.transientCandidateCount > 0 ? "true" : "false") << ",\n";
    out << "  \"transientCandidateCount\": " << metrics.transientCandidateCount << ",\n";
    out << "  \"transientCandidatesTruncated\": "
        << (metrics.transientCandidatesTruncated ? "true" : "false") << ",\n";
    out << "  \"dropoutCandidateDetected\": "
        << (metrics.dropoutCandidateCount > 0 ? "true" : "false") << ",\n";
    out << "  \"dropoutCandidateCount\": " << metrics.dropoutCandidateCount << ",\n";
    out << "  \"dropoutCandidatesTruncated\": "
        << (metrics.dropoutCandidatesTruncated ? "true" : "false") << ",\n";
    out << "  \"trailingSilenceStartMs\": " << metrics.trailingSilenceStartMs << ",\n";
    out << "  \"trailingSilenceDurationMs\": " << metrics.trailingSilenceDurationMs << ",\n";
    out << "  \"tailFadeCandidateObserved\": "
        << (tailFadeCandidateObserved(metrics) ? "true" : "false") << ",\n";
    out << "  \"tailFadeDescendingStepCount\": " << tailFadeDescendingStepCount(metrics) << ",\n";
    writeSegments(out, metrics.segments);
    out << "  \"dropoutCandidates\": [\n";
    for (size_t i = 0; i < metrics.dropoutCandidates.size(); ++i) {
        const auto &candidate = metrics.dropoutCandidates[i];
        out << "    {"
            << "\"timeMs\": " << candidate.timeMs << ", "
            << "\"durationMs\": " << candidate.durationMs << ", "
            << "\"silentPackets\": " << candidate.silentPackets
            << "}";
        if (i + 1 < metrics.dropoutCandidates.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ],\n";
    out << "  \"recentAudiblePackets\": [\n";
    for (size_t i = 0; i < metrics.recentAudiblePackets.size(); ++i) {
        const auto &packet = metrics.recentAudiblePackets[i];
        out << "    {"
            << "\"timeMs\": " << packet.timeMs << ", "
            << "\"peak\": " << packet.peak << ", "
            << "\"rms\": " << packet.rms
            << "}";
        if (i + 1 < metrics.recentAudiblePackets.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ],\n";
    out << "  \"transientCandidates\": [\n";
    for (size_t i = 0; i < metrics.transientCandidates.size(); ++i) {
        const auto &candidate = metrics.transientCandidates[i];
        out << "    {"
            << "\"timeMs\": " << candidate.timeMs << ", "
            << "\"peak\": " << candidate.peak << ", "
            << "\"sampleDelta\": " << candidate.sampleDelta << ", "
            << "\"reason\": \"" << jsonEscape(candidate.reason) << "\""
            << "}";
        if (i + 1 < metrics.transientCandidates.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ]\n";
    out << "}\n";
}

void writeReadyFile(const std::filesystem::path &path,
                    const std::string &captureStartedUtc,
                    const std::wstring &deviceName,
                    const std::wstring &deviceId)
{
    if (path.empty()) {
        return;
    }
    std::ostringstream out;
    out << "{\n"
        << "  \"ready\": true,\n"
        << "  \"captureStartedUtc\": \"" << jsonEscape(captureStartedUtc) << "\",\n"
        << "  \"deviceName\": \"" << jsonEscape(narrowWide(deviceName)) << "\",\n"
        << "  \"deviceId\": \"" << jsonEscape(narrowWide(deviceId)) << "\"\n"
        << "}\n";
    writeTextFile(path, out.str());
}

CaptureMetrics captureLoopback(const Options &options,
                               std::wstring &deviceName,
                               std::wstring &deviceId,
                               MixFormatInfo &formatInfo,
                               std::string &captureStartedUtc,
                               std::string &captureEndedUtc,
                               int64_t &durationMs)
{
    CaptureMetrics metrics;
    constexpr int kMaxSegments = 8;
    constexpr REFERENCE_TIME bufferDuration = 10000000;
    const auto overallStarted = std::chrono::steady_clock::now();
    const auto deadline = overallStarted + std::chrono::milliseconds(options.durationMs);
    bool readyFileWritten = false;
    bool simulatedInvalidationUsed = false;

    while (std::chrono::steady_clock::now() < deadline
           && !stopRequested(options)
           && static_cast<int>(metrics.segments.size()) < kMaxSegments) {
        SegmentReport segment;
        segment.segmentIndex = static_cast<int>(metrics.segments.size()) + 1;
        segment.wavFile = segmentWavPath(options.wavFile, segment.segmentIndex);
        segment.metadataFile = segmentMetadataPath(options.wavFile, segment.segmentIndex);

        ComPtr<IMMDeviceEnumerator> enumerator;
        throwIfFailed(CoCreateInstance(__uuidof(MMDeviceEnumerator),
                                       nullptr,
                                       CLSCTX_ALL,
                                       __uuidof(IMMDeviceEnumerator),
                                       reinterpret_cast<void **>(enumerator.put())),
                      "CoCreateInstance(MMDeviceEnumerator)");

        ComPtr<IMMDevice> device;
        throwIfFailed(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.put()),
                      "GetDefaultAudioEndpoint(eRender,eConsole)");

        segment.deviceName = defaultDeviceName(device.get());
        segment.deviceId = defaultDeviceId(device.get());
        if (deviceName.empty()) {
            deviceName = segment.deviceName;
        }
        if (deviceId.empty()) {
            deviceId = segment.deviceId;
        }

        ComPtr<IAudioClient> audioClient;
        throwIfFailed(device->Activate(__uuidof(IAudioClient),
                                       CLSCTX_ALL,
                                       nullptr,
                                       reinterpret_cast<void **>(audioClient.put())),
                      "IMMDevice::Activate(IAudioClient)");

        CoTaskMemWaveFormat mixFormat;
        throwIfFailed(audioClient->GetMixFormat(mixFormat.put()), "IAudioClient::GetMixFormat");
        segment.mixFormat = describeMixFormat(mixFormat.get());
        if (formatInfo.sampleRate == 0) {
            formatInfo = segment.mixFormat;
        }
        metrics.metricsSupported = metrics.metricsSupported || segment.mixFormat.metricsSupported;
        if (segment.mixFormat.sampleRate == 0 || segment.mixFormat.blockAlign == 0) {
            throw std::runtime_error("Default endpoint returned an invalid mix format");
        }

        throwIfFailed(audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                              AUDCLNT_STREAMFLAGS_LOOPBACK
                                                  | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                              bufferDuration,
                                              0,
                                              mixFormat.get(),
                                              nullptr),
                      "IAudioClient::Initialize(loopback)");

        HANDLE refillEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!refillEvent) {
            throw std::runtime_error("CreateEvent failed: error=" + std::to_string(GetLastError()));
        }

        struct HandleGuard
        {
            HANDLE handle = nullptr;
            ~HandleGuard()
            {
                if (handle) {
                    CloseHandle(handle);
                }
            }
        } refillGuard {refillEvent};

        throwIfFailed(audioClient->SetEventHandle(refillEvent), "IAudioClient::SetEventHandle");

        ComPtr<IAudioCaptureClient> captureClient;
        throwIfFailed(audioClient->GetService(__uuidof(IAudioCaptureClient),
                                              reinterpret_cast<void **>(captureClient.put())),
                      "IAudioClient::GetService(IAudioCaptureClient)");

        WavWriter wav(segment.wavFile, mixFormat.get());

        DWORD avrtTaskIndex = 0;
        HANDLE avrtHandle = AvSetMmThreadCharacteristicsW(L"Audio", &avrtTaskIndex);

        const auto cleanupAvrt = [&] {
            if (avrtHandle) {
                AvRevertMmThreadCharacteristics(avrtHandle);
                avrtHandle = nullptr;
            }
        };

        bool audioStarted = false;
        throwIfFailed(audioClient->Start(), "IAudioClient::Start");
        audioStarted = true;
        segment.startedUtc = utcNowIso();
        if (captureStartedUtc.empty()) {
            captureStartedUtc = segment.startedUtc;
        }
        if (!readyFileWritten) {
            writeReadyFile(options.readyFile, captureStartedUtc, segment.deviceName, segment.deviceId);
            readyFileWritten = true;
        }

        const auto segmentStarted = std::chrono::steady_clock::now();
        std::vector<double> previousSamples;
        bool hasPreviousSamples = false;
        bool audiblePacketObserved = false;
        double pendingSilenceStartMs = -1.0;
        double pendingSilenceDurationMs = 0.0;
        uint64_t pendingSilentPackets = 0;
        metrics.recentAudiblePackets.clear();

        const auto commitPendingDropout = [&] {
            if (pendingSilentPackets == 0) {
                return;
            }

            ++metrics.dropoutCandidateCount;
            ++segment.dropoutCandidateCount;
            if (metrics.dropoutCandidates.size() < kMaxReportedCandidates) {
                metrics.dropoutCandidates.push_back(
                    DropoutCandidate {pendingSilenceStartMs,
                                      pendingSilenceDurationMs,
                                      pendingSilentPackets});
            } else {
                metrics.dropoutCandidatesTruncated = true;
            }
            pendingSilenceStartMs = -1.0;
            pendingSilenceDurationMs = 0.0;
            pendingSilentPackets = 0;
        };

        auto drainPackets = [&] {
            UINT32 packetFrames = 0;
            HRESULT hr = captureClient->GetNextPacketSize(&packetFrames);
            if (!handleCaptureHresult(hr,
                                      "IAudioCaptureClient::GetNextPacketSize",
                                      metrics,
                                      &segment)) {
                return false;
            }
            while (packetFrames > 0) {
                BYTE *data = nullptr;
                UINT32 framesAvailable = 0;
                DWORD flags = 0;
                UINT64 devicePosition = 0;
                UINT64 qpcPosition = 0;
                hr = captureClient->GetBuffer(&data,
                                              &framesAvailable,
                                              &flags,
                                              &devicePosition,
                                              &qpcPosition);
                if (!handleCaptureHresult(hr,
                                          "IAudioCaptureClient::GetBuffer",
                                          metrics,
                                          &segment)) {
                    return false;
                }

                const size_t byteCount =
                    static_cast<size_t>(framesAvailable) * segment.mixFormat.blockAlign;
                std::vector<uint8_t> packet(byteCount);
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    std::fill(packet.begin(), packet.end(), 0);
                    ++metrics.silentPackets;
                    ++segment.silentPackets;
                } else if (data && byteCount > 0) {
                    std::memcpy(packet.data(), data, byteCount);
                }

                const double packetStartMs = segment.mixFormat.sampleRate > 0
                    ? static_cast<double>(metrics.framesCaptured) * 1000.0
                          / static_cast<double>(segment.mixFormat.sampleRate)
                    : 0.0;
                const double packetDurationMs = segment.mixFormat.sampleRate > 0
                    ? static_cast<double>(framesAvailable) * 1000.0
                          / static_cast<double>(segment.mixFormat.sampleRate)
                    : 0.0;

                const BlockMetrics block = analyzeBlock(packet,
                                                        framesAvailable,
                                                        segment.mixFormat,
                                                        previousSamples,
                                                        hasPreviousSamples);
                if (block.valid) {
                    metrics.maxPeak = std::max(metrics.maxPeak, block.peak);
                    metrics.blockRmsMax = std::max(metrics.blockRmsMax, block.rms);
                    metrics.maxSampleDelta = std::max(metrics.maxSampleDelta, block.maxSampleDelta);
                    segment.maxPeak = std::max(segment.maxPeak, block.peak);
                    segment.blockRmsMax = std::max(segment.blockRmsMax, block.rms);
                    segment.maxSampleDelta = std::max(segment.maxSampleDelta, block.maxSampleDelta);

                    const std::string reason = candidateReason(block);
                    if (!reason.empty()) {
                        ++metrics.transientCandidateCount;
                        ++segment.transientCandidateCount;
                        if (metrics.transientCandidates.size() < kMaxReportedCandidates) {
                            metrics.transientCandidates.push_back(
                                TransientCandidate {packetStartMs,
                                                    block.peak,
                                                    block.maxSampleDelta,
                                                    reason});
                        } else {
                            metrics.transientCandidatesTruncated = true;
                        }
                    }
                }

                const bool effectivelySilent =
                    (flags & AUDCLNT_BUFFERFLAGS_SILENT)
                    || (block.valid && block.peak <= kDropoutSilencePeakThreshold);
                if (effectivelySilent) {
                    if (audiblePacketObserved) {
                        if (pendingSilentPackets == 0) {
                            pendingSilenceStartMs = packetStartMs;
                        }
                        pendingSilenceDurationMs += packetDurationMs;
                        ++pendingSilentPackets;
                    }
                } else {
                    commitPendingDropout();
                    audiblePacketObserved = true;
                    if (block.valid) {
                        metrics.recentAudiblePackets.push_back(
                            AudiblePacketMetrics {packetStartMs, block.peak, block.rms});
                        if (metrics.recentAudiblePackets.size() > kMaxRecentAudiblePackets) {
                            metrics.recentAudiblePackets.erase(metrics.recentAudiblePackets.begin());
                        }
                    }
                }

                wav.writeFrames(packet);
                metrics.framesCaptured += framesAvailable;
                segment.framesCaptured += framesAvailable;
                ++metrics.packetsCaptured;
                ++segment.packetsCaptured;
                hr = captureClient->ReleaseBuffer(framesAvailable);
                if (!handleCaptureHresult(hr,
                                          "IAudioCaptureClient::ReleaseBuffer",
                                          metrics,
                                          &segment)) {
                    return false;
                }
                hr = captureClient->GetNextPacketSize(&packetFrames);
                if (!handleCaptureHresult(hr,
                                          "IAudioCaptureClient::GetNextPacketSize",
                                          metrics,
                                          &segment)) {
                    return false;
                }
            }
            return true;
        };

        while (std::chrono::steady_clock::now() < deadline
               && !stopRequested(options)
               && !segment.interrupted) {
            const DWORD waitResult = WaitForSingleObject(refillEvent, 200);
            if (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_TIMEOUT) {
                if (!drainPackets()) {
                    break;
                }
                if (options.simulateInvalidationAfterMs >= 0
                    && !simulatedInvalidationUsed
                    && std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - segmentStarted)
                           .count()
                        >= options.simulateInvalidationAfterMs) {
                    simulatedInvalidationUsed = true;
                    recordCaptureInterruption(metrics,
                                              &segment,
                                              AUDCLNT_E_DEVICE_INVALIDATED,
                                              "simulated-device-invalidated");
                    break;
                }
                continue;
            }
            throw std::runtime_error("WaitForSingleObject failed: result=" + std::to_string(waitResult));
        }

        if (!segment.interrupted) {
            drainPackets();
        }
        if (audioStarted) {
            const HRESULT stopHr = audioClient->Stop();
            if (FAILED(stopHr)) {
                if (isAudioClientInvalidatedHresult(stopHr)) {
                    recordCaptureInterruption(metrics, &segment, stopHr, "IAudioClient::Stop");
                } else {
                    throw HResultError(stopHr, "IAudioClient::Stop");
                }
            }
        }
        cleanupAvrt();
        wav.finalize();

        segment.trailingSilenceStartMs = pendingSilenceStartMs;
        segment.trailingSilenceDurationMs = pendingSilenceDurationMs;
        metrics.trailingSilenceStartMs = segment.trailingSilenceStartMs;
        metrics.trailingSilenceDurationMs = segment.trailingSilenceDurationMs;
        segment.endedUtc = utcNowIso();
        segment.durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - segmentStarted)
                                 .count();
        writeSegmentMetadata(segment);
        captureEndedUtc = segment.endedUtc;
        metrics.segments.push_back(segment);
        metrics.segmentCount = static_cast<int>(metrics.segments.size());

        if (!segment.interrupted) {
            break;
        }
    }

    if (captureEndedUtc.empty()) {
        captureEndedUtc = utcNowIso();
    }
    durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - overallStarted)
                     .count();
    return metrics;
}

std::string resultForMetrics(const CaptureMetrics &metrics)
{
    if (metrics.transientCandidateCount > 0 || metrics.dropoutCandidateCount > 0) {
        return "FAIL";
    }
    return "INCONCLUSIVE";
}

} // namespace

int wmain(int argc, wchar_t **argv)
{
    Options options;
    try {
        options = parseOptions(argc, argv);
    } catch (const std::exception &error) {
        std::cerr << "Argument error: " << error.what() << "\n";
        return 2;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool shouldUninitialize = SUCCEEDED(hr);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        CaptureMetrics metrics;
        MixFormatInfo format;
        const std::string now = utcNowIso();
        try {
            const bool invalidated = isAudioClientInvalidatedHresult(hr);
            if (invalidated) {
                metrics.captureInterrupted = true;
                metrics.interruptionReason = invalidationReasonForHresult(hr);
                metrics.interruptionHresult = hresultText(hr);
                metrics.interruptionStage = "CoInitializeEx";
            }
            writeReport(options.reportFile,
                        invalidated ? "INCONCLUSIVE" : "FAIL",
                        "CoInitializeEx failed: hr=" + hresultText(hr),
                        L"",
                        L"",
                        format,
                        now,
                        now,
                        0,
                        options.wavFile,
                        metrics);
        } catch (...) {
        }
        return isAudioClientInvalidatedHresult(hr) ? 0 : 1;
    }

    std::wstring deviceName;
    std::wstring deviceId;
    MixFormatInfo format;
    std::string captureStartedUtc;
    std::string captureEndedUtc;
    int64_t durationMs = 0;

    try {
        CaptureMetrics metrics = captureLoopback(options,
                                                 deviceName,
                                                 deviceId,
                                                 format,
                                                 captureStartedUtc,
                                                 captureEndedUtc,
                                                 durationMs);
        const std::string result = resultForMetrics(metrics);
        writeReport(options.reportFile,
                    result,
                    "",
                    deviceName,
                    deviceId,
                    format,
                    captureStartedUtc,
                    captureEndedUtc,
                    durationMs,
                    options.wavFile,
                    metrics);

        std::cout << "result:" << result << "\n";
        std::cout << "wav:" << pathUtf8(options.wavFile) << "\n";
        std::cout << "report:" << pathUtf8(options.reportFile) << "\n";
        std::cout << "captureInterrupted:" << (metrics.captureInterrupted ? "true" : "false") << "\n";
        if (metrics.captureInterrupted) {
            std::cout << "interruptionReason:" << metrics.interruptionReason << "\n";
            std::cout << "interruptionHresult:" << metrics.interruptionHresult << "\n";
        }
        std::cout << "transientCandidateCount:" << metrics.transientCandidateCount << "\n";
        std::cout << "dropoutCandidateCount:" << metrics.dropoutCandidateCount << "\n";
        std::cout << "tailFadeCandidateObserved:"
                  << (tailFadeCandidateObserved(metrics) ? "true" : "false") << "\n";
    } catch (const HResultError &error) {
        CaptureMetrics metrics;
        const bool invalidated = isAudioClientInvalidatedHresult(error.hresult());
        if (invalidated) {
            metrics.captureInterrupted = true;
            metrics.interruptionReason = invalidationReasonForHresult(error.hresult());
            metrics.interruptionHresult = hresultText(error.hresult());
            metrics.interruptionStage = error.stage();
        }
        const std::string ended = utcNowIso();
        if (captureStartedUtc.empty()) {
            captureStartedUtc = ended;
        }
        captureEndedUtc = ended;
        try {
            writeReport(options.reportFile,
                        invalidated ? "INCONCLUSIVE" : "FAIL",
                        error.what(),
                        deviceName,
                        deviceId,
                        format,
                        captureStartedUtc,
                        captureEndedUtc,
                        durationMs,
                        options.wavFile,
                        metrics);
        } catch (...) {
        }
        std::cerr << "capture failed: " << error.what() << "\n";
        if (shouldUninitialize) {
            CoUninitialize();
        }
        return invalidated ? 0 : 1;
    } catch (const std::exception &error) {
        CaptureMetrics metrics;
        const std::string ended = utcNowIso();
        if (captureStartedUtc.empty()) {
            captureStartedUtc = ended;
        }
        captureEndedUtc = ended;
        try {
            writeReport(options.reportFile,
                        "FAIL",
                        error.what(),
                        deviceName,
                        deviceId,
                        format,
                        captureStartedUtc,
                        captureEndedUtc,
                        durationMs,
                        options.wavFile,
                        metrics);
        } catch (...) {
        }
        std::cerr << "capture failed: " << error.what() << "\n";
        if (shouldUninitialize) {
            CoUninitialize();
        }
        return 1;
    }

    if (shouldUninitialize) {
        CoUninitialize();
    }
    return 0;
}
