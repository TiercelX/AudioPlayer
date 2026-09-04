#include "windowswasapiaudioplayer.h"

#include "playerlogger.h"

#include <QMediaDevices>
#include <QStringList>
#include <QTimer>

#include <algorithm>

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <audioclient.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <objbase.h>
#include <spatialaudioclient.h>

namespace {

constexpr int kSystemOutputDeviceChangeDebounceMs = 350;
constexpr int kOtherOutputDeviceChangeDebounceMs = 180;
constexpr auto kSameOutputInvalidationIoErrorTrigger = "SameOutputInvalidationIoError";

int outputDeviceChangeDebounceMsForReason(const QString &reason)
{
    if (reason == QStringLiteral("audioOutputsChanged")) {
        return kSystemOutputDeviceChangeDebounceMs;
    }

    return kOtherOutputDeviceChangeDebounceMs;
}

bool scriptedSmokeTestEnabled()
{
    const QString value = qEnvironmentVariable("AUDIOPLAYER_SCRIPTED_SMOKE_TEST");
    return value == QStringLiteral("1") || value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
}

bool scriptedSameOutputInvalidationIoErrorEnabled()
{
    return scriptedSmokeTestEnabled()
        && qEnvironmentVariable("AUDIOPLAYER_SCRIPTED_OUTPUT_SWITCH_TRIGGER")
               == QString::fromLatin1(kSameOutputInvalidationIoErrorTrigger);
}

template <typename T>
void safeRelease(T *&object)
{
    if (!object) {
        return;
    }

    object->Release();
    object = nullptr;
}

class ScopedComInitializer
{
public:
    ScopedComInitializer()
        : m_result(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))
    {
    }

    ~ScopedComInitializer()
    {
        if (SUCCEEDED(m_result)) {
            CoUninitialize();
        }
    }

    HRESULT result() const
    {
        return m_result;
    }

private:
    HRESULT m_result = E_FAIL;
};

QString endpointIdFromQtId(const QByteArray &deviceId)
{
    const QString utf8Id = QString::fromUtf8(deviceId);
    if (!utf8Id.contains(QChar::ReplacementCharacter)) {
        return utf8Id;
    }

    return QString::fromLatin1(deviceId);
}

DWORD channelMaskForCount(int channelCount)
{
    switch (channelCount) {
    case 1:
        return SPEAKER_FRONT_CENTER;
    case 2:
        return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    case 3:
        return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_LOW_FREQUENCY;
    case 4:
        return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT
            | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
    case 5:
        return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_LOW_FREQUENCY
            | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
    case 6:
        return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER
            | SPEAKER_LOW_FREQUENCY | SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT;
    case 7:
        return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER
            | SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_CENTER
            | SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT;
    case 8:
        return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER
            | SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT
            | SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT;
    default:
        return 0;
    }
}

DWORD channelMaskFromWaveFormat(const WAVEFORMATEX *waveFormat)
{
    if (!waveFormat
        || waveFormat->wFormatTag != WAVE_FORMAT_EXTENSIBLE
        || waveFormat->cbSize < 22) {
        return 0;
    }
    return reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(waveFormat)->dwChannelMask;
}

QString channelMaskText(DWORD channelMask)
{
    return QStringLiteral("0x%1").arg(static_cast<qulonglong>(channelMask),
                                      8,
                                      16,
                                      QLatin1Char('0'));
}

QString hresultText(HRESULT hr)
{
    return QStringLiteral("0x%1").arg(static_cast<qulonglong>(hr),
                                      8,
                                      16,
                                      QLatin1Char('0'));
}

QString audioObjectTypeText(AudioObjectType type)
{
    switch (type) {
    case AudioObjectType_Dynamic:
        return QStringLiteral("Dynamic");
    case AudioObjectType_FrontLeft:
        return QStringLiteral("FL");
    case AudioObjectType_FrontRight:
        return QStringLiteral("FR");
    case AudioObjectType_FrontCenter:
        return QStringLiteral("FC");
    case AudioObjectType_LowFrequency:
        return QStringLiteral("LFE");
    case AudioObjectType_SideLeft:
        return QStringLiteral("SL");
    case AudioObjectType_SideRight:
        return QStringLiteral("SR");
    case AudioObjectType_BackLeft:
        return QStringLiteral("BL");
    case AudioObjectType_BackRight:
        return QStringLiteral("BR");
    case AudioObjectType_TopFrontLeft:
        return QStringLiteral("TFL");
    case AudioObjectType_TopFrontRight:
        return QStringLiteral("TFR");
    case AudioObjectType_TopBackLeft:
        return QStringLiteral("TBL");
    case AudioObjectType_TopBackRight:
        return QStringLiteral("TBR");
    case AudioObjectType_BottomFrontLeft:
        return QStringLiteral("BFL");
    case AudioObjectType_BottomFrontRight:
        return QStringLiteral("BFR");
    case AudioObjectType_BottomBackLeft:
        return QStringLiteral("BBL");
    case AudioObjectType_BottomBackRight:
        return QStringLiteral("BBR");
    case AudioObjectType_BackCenter:
        return QStringLiteral("BC");
    case AudioObjectType_StereoLeft:
        return QStringLiteral("StereoL");
    case AudioObjectType_StereoRight:
        return QStringLiteral("StereoR");
    case AudioObjectType_None:
        break;
    }
    return {};
}

QString audioObjectMaskText(AudioObjectType mask)
{
    const AudioObjectType knownTypes[] = {
        AudioObjectType_Dynamic,
        AudioObjectType_FrontLeft,
        AudioObjectType_FrontRight,
        AudioObjectType_FrontCenter,
        AudioObjectType_LowFrequency,
        AudioObjectType_SideLeft,
        AudioObjectType_SideRight,
        AudioObjectType_BackLeft,
        AudioObjectType_BackRight,
        AudioObjectType_TopFrontLeft,
        AudioObjectType_TopFrontRight,
        AudioObjectType_TopBackLeft,
        AudioObjectType_TopBackRight,
        AudioObjectType_BottomFrontLeft,
        AudioObjectType_BottomFrontRight,
        AudioObjectType_BottomBackLeft,
        AudioObjectType_BottomBackRight,
        AudioObjectType_BackCenter,
        AudioObjectType_StereoLeft,
        AudioObjectType_StereoRight,
    };

    QStringList names;
    for (const AudioObjectType type : knownTypes) {
        if ((mask & type) == type) {
            const QString name = audioObjectTypeText(type);
            if (!name.isEmpty()) {
                names << name;
            }
        }
    }

    return QStringLiteral("0x%1[%2]")
        .arg(static_cast<qulonglong>(mask), 8, 16, QLatin1Char('0'))
        .arg(names.join(QLatin1Char(',')));
}

QString waveFormatSummary(const WAVEFORMATEX *format)
{
    if (!format) {
        return QStringLiteral("<null>");
    }

    QString subtype;
    DWORD channelMask = 0;
    int validBits = format->wBitsPerSample;
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE
        && format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto *extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(format);
        channelMask = extensible->dwChannelMask;
        validBits = extensible->Samples.wValidBitsPerSample;
        if (extensible->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
            subtype = QStringLiteral("float");
        } else if (extensible->SubFormat == KSDATAFORMAT_SUBTYPE_PCM) {
            subtype = QStringLiteral("pcm");
        } else {
            subtype = QStringLiteral("guid");
        }
    } else if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        subtype = QStringLiteral("float");
    } else if (format->wFormatTag == WAVE_FORMAT_PCM) {
        subtype = QStringLiteral("pcm");
    } else {
        subtype = QStringLiteral("tag-%1").arg(format->wFormatTag);
    }

    return QStringLiteral("rate=%1 channels=%2 bits=%3 validBits=%4 blockAlign=%5 tag=%6 subtype=%7 mask=%8")
        .arg(format->nSamplesPerSec)
        .arg(format->nChannels)
        .arg(format->wBitsPerSample)
        .arg(validBits)
        .arg(format->nBlockAlign)
        .arg(format->wFormatTag)
        .arg(subtype)
        .arg(channelMaskText(channelMask));
}

void logSpatialStaticObjectPosition(ISpatialAudioClient *spatialClient, AudioObjectType type)
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    const HRESULT hr = spatialClient->GetStaticObjectPosition(type, &x, &y, &z);
    if (FAILED(hr)) {
        return;
    }

    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("spatialAudioProbe staticObjectPosition type=%1 x=%2 y=%3 z=%4")
                          .arg(audioObjectTypeText(type))
                          .arg(QString::number(x, 'f', 3))
                          .arg(QString::number(y, 'f', 3))
                          .arg(QString::number(z, 'f', 3)));
}

void probeSpatialAudioEndpoint(IMMDevice *endpoint, const QAudioDevice &qtDevice)
{
    if (!endpoint) {
        return;
    }

    ISpatialAudioClient *spatialClient = nullptr;
    HRESULT hr = endpoint->Activate(__uuidof(ISpatialAudioClient),
                                    CLSCTX_ALL,
                                    nullptr,
                                    reinterpret_cast<void **>(&spatialClient));
    if (FAILED(hr) || !spatialClient) {
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("spatialAudioProbe unavailable hr=%1 deviceId=%2 description=%3")
                              .arg(hresultText(hr))
                              .arg(QString::fromLatin1(qtDevice.id().toHex()))
                              .arg(qtDevice.description()));
        safeRelease(spatialClient);
        return;
    }

    const HRESULT streamAvailableHr =
        spatialClient->IsSpatialAudioStreamAvailable(__uuidof(ISpatialAudioObjectRenderStream), nullptr);

    AudioObjectType nativeMask = AudioObjectType_None;
    const HRESULT nativeMaskHr = spatialClient->GetNativeStaticObjectTypeMask(&nativeMask);

    UINT32 maxDynamicObjectCount = 0;
    const HRESULT maxDynamicHr = spatialClient->GetMaxDynamicObjectCount(&maxDynamicObjectCount);

    IAudioFormatEnumerator *formatEnumerator = nullptr;
    const HRESULT formatEnumHr = spatialClient->GetSupportedAudioObjectFormatEnumerator(&formatEnumerator);
    UINT32 formatCount = 0;
    if (SUCCEEDED(formatEnumHr) && formatEnumerator) {
        formatEnumerator->GetCount(&formatCount);
    }

    const AudioObjectType fiveOneTwoMask =
        static_cast<AudioObjectType>(AudioObjectType_FrontLeft
                                     | AudioObjectType_FrontRight
                                     | AudioObjectType_FrontCenter
                                     | AudioObjectType_LowFrequency
                                     | AudioObjectType_SideLeft
                                     | AudioObjectType_SideRight
                                     | AudioObjectType_TopFrontLeft
                                     | AudioObjectType_TopFrontRight);
    const bool supportsFiveOneTwo =
        SUCCEEDED(nativeMaskHr) && (nativeMask & fiveOneTwoMask) == fiveOneTwoMask;

    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("spatialAudioProbe ok deviceId=%1 description=%2 streamHr=%3 nativeMaskHr=%4 nativeMask=%5 supports5.1.2=%6 maxDynamicHr=%7 maxDynamicObjects=%8 formatEnumHr=%9 formatCount=%10")
                          .arg(QString::fromLatin1(qtDevice.id().toHex()))
                          .arg(qtDevice.description())
                          .arg(hresultText(streamAvailableHr))
                          .arg(hresultText(nativeMaskHr))
                          .arg(audioObjectMaskText(nativeMask))
                          .arg(supportsFiveOneTwo)
                          .arg(hresultText(maxDynamicHr))
                          .arg(maxDynamicObjectCount)
                          .arg(hresultText(formatEnumHr))
                          .arg(formatCount));

    if (SUCCEEDED(nativeMaskHr)) {
        const AudioObjectType positionTypes[] = {
            AudioObjectType_FrontLeft,
            AudioObjectType_FrontRight,
            AudioObjectType_FrontCenter,
            AudioObjectType_LowFrequency,
            AudioObjectType_SideLeft,
            AudioObjectType_SideRight,
            AudioObjectType_BackLeft,
            AudioObjectType_BackRight,
            AudioObjectType_TopFrontLeft,
            AudioObjectType_TopFrontRight,
        };
        for (const AudioObjectType type : positionTypes) {
            if ((nativeMask & type) == type) {
                logSpatialStaticObjectPosition(spatialClient, type);
            }
        }
    }

    if (formatEnumerator) {
        const UINT32 formatsToLog = qMin<UINT32>(formatCount, 8);
        for (UINT32 i = 0; i < formatsToLog; ++i) {
            WAVEFORMATEX *objectFormat = nullptr;
            const HRESULT getFormatHr = formatEnumerator->GetFormat(i, &objectFormat);
            UINT32 maxFrameCount = 0;
            HRESULT maxFrameHr = E_POINTER;
            HRESULT objectFormatSupportedHr = E_POINTER;
            if (SUCCEEDED(getFormatHr) && objectFormat) {
                maxFrameHr = spatialClient->GetMaxFrameCount(objectFormat, &maxFrameCount);
                objectFormatSupportedHr = spatialClient->IsAudioObjectFormatSupported(objectFormat);
            }
            PlayerLogger::log(QStringLiteral("player"),
                              QStringLiteral("spatialAudioProbe objectFormat index=%1 getHr=%2 supportedHr=%3 maxFrameHr=%4 maxFrameCount=%5 %6")
                                  .arg(i)
                                  .arg(hresultText(getFormatHr))
                                  .arg(hresultText(objectFormatSupportedHr))
                                  .arg(hresultText(maxFrameHr))
                                  .arg(maxFrameCount)
                                  .arg(waveFormatSummary(objectFormat)));
            if (objectFormat) {
                CoTaskMemFree(objectFormat);
            }
        }
    }

    safeRelease(formatEnumerator);
    safeRelease(spatialClient);
}

DWORD channelMaskFromWaveFormatData(const QByteArray &waveFormatData)
{
    if (waveFormatData.size() < static_cast<int>(sizeof(WAVEFORMATEX))) {
        return 0;
    }
    const auto *waveFormat = reinterpret_cast<const WAVEFORMATEX *>(waveFormatData.constData());
    if (waveFormatData.size() < static_cast<int>(sizeof(WAVEFORMATEX) + waveFormat->cbSize)) {
        return 0;
    }
    return channelMaskFromWaveFormat(waveFormat);
}

bool hasMatchingPcmLayoutAndRate(const PcmStreamFormat &lhs, const PcmStreamFormat &rhs)
{
    return lhs.isValid()
        && rhs.isValid()
        && lhs.sampleRate == rhs.sampleRate
        && lhs.sampleEncoding == rhs.sampleEncoding
        && lhs.channelCount == rhs.channelCount
        && lhs.bytesPerFrame() == rhs.bytesPerFrame()
        && lhs.effectiveValidBitsPerSample() == rhs.effectiveValidBitsPerSample();
}

bool canRenderBufferFormatToDeviceFormat(const PcmStreamFormat &bufferFormat, const PcmStreamFormat &deviceFormat)
{
    if (!bufferFormat.isValid() || !deviceFormat.isValid() || bufferFormat.sampleRate != deviceFormat.sampleRate) {
        return false;
    }

    if (hasMatchingPcmLayoutAndRate(bufferFormat, deviceFormat)) {
        return true;
    }

    return (bufferFormat.sampleEncoding == PcmSampleEncoding::Int32
            && deviceFormat.sampleEncoding == PcmSampleEncoding::Int24
            && bufferFormat.channelCount == deviceFormat.channelCount)
        || (bufferFormat.sampleEncoding == PcmSampleEncoding::Int32
            && deviceFormat.sampleEncoding == PcmSampleEncoding::Int32
            && bufferFormat.effectiveValidBitsPerSample() == 32
            && deviceFormat.effectiveValidBitsPerSample() == 24
            && bufferFormat.channelCount == deviceFormat.channelCount)
        || (bufferFormat.sampleEncoding == PcmSampleEncoding::Int32
            && deviceFormat.sampleEncoding == PcmSampleEncoding::Int16
            && bufferFormat.channelCount == deviceFormat.channelCount);
}

int waveFormatSize(const WAVEFORMATEX *waveFormat)
{
    if (!waveFormat) {
        return 0;
    }

    return sizeof(WAVEFORMATEX) + waveFormat->cbSize;
}

QByteArray copyWaveFormat(const WAVEFORMATEX *waveFormat)
{
    const int size = waveFormatSize(waveFormat);
    if (size <= 0) {
        return {};
    }

    return QByteArray(reinterpret_cast<const char *>(waveFormat), size);
}

PcmSampleEncoding sampleEncodingFromWaveFormat(const WAVEFORMATEX *waveFormat)
{
    if (!waveFormat) {
        return PcmSampleEncoding::Unknown;
    }

    if (waveFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        return waveFormat->wBitsPerSample == 32 ? PcmSampleEncoding::Float32
                                                : PcmSampleEncoding::Unknown;
    }

    if (waveFormat->wFormatTag == WAVE_FORMAT_PCM) {
        switch (waveFormat->wBitsPerSample) {
        case 8:
            return PcmSampleEncoding::UInt8;
        case 16:
            return PcmSampleEncoding::Int16;
        case 24:
            return PcmSampleEncoding::Int24;
        case 32:
            return PcmSampleEncoding::Int32;
        default:
            return PcmSampleEncoding::Unknown;
        }
    }

    if (waveFormat->wFormatTag != WAVE_FORMAT_EXTENSIBLE || waveFormat->cbSize < 22) {
        return PcmSampleEncoding::Unknown;
    }

    const auto *extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(waveFormat);
    if (extensible->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
        return waveFormat->wBitsPerSample == 32 ? PcmSampleEncoding::Float32
                                                : PcmSampleEncoding::Unknown;
    }
    if (extensible->SubFormat != KSDATAFORMAT_SUBTYPE_PCM) {
        return PcmSampleEncoding::Unknown;
    }

    switch (waveFormat->wBitsPerSample) {
    case 8:
        return PcmSampleEncoding::UInt8;
    case 16:
        return PcmSampleEncoding::Int16;
    case 24:
        return PcmSampleEncoding::Int24;
    case 32:
        return PcmSampleEncoding::Int32;
    default:
        return PcmSampleEncoding::Unknown;
    }
}

PcmStreamFormat pcmFormatFromWaveFormat(const WAVEFORMATEX *waveFormat)
{
    PcmStreamFormat format;
    if (!waveFormat) {
        return format;
    }

    format.sampleEncoding = sampleEncodingFromWaveFormat(waveFormat);
    format.sampleRate = static_cast<int>(waveFormat->nSamplesPerSec);
    format.channelCount = static_cast<int>(waveFormat->nChannels);
    format.validBitsPerSample = waveFormat->wBitsPerSample;
    if (waveFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE && waveFormat->cbSize >= 22) {
        format.validBitsPerSample =
            reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(waveFormat)->Samples.wValidBitsPerSample;
    }

    if (!format.isValid()) {
        return {};
    }
    return format;
}

PcmStreamFormat pcmFormatFromQAudioFormat(const QAudioFormat &format)
{
    PcmStreamFormat pcmFormat;
    if (!format.isValid()) {
        return pcmFormat;
    }

    pcmFormat.sampleRate = format.sampleRate();
    pcmFormat.channelCount = format.channelCount();
    switch (format.sampleFormat()) {
    case QAudioFormat::UInt8:
        pcmFormat.sampleEncoding = PcmSampleEncoding::UInt8;
        pcmFormat.validBitsPerSample = 8;
        break;
    case QAudioFormat::Int16:
        pcmFormat.sampleEncoding = PcmSampleEncoding::Int16;
        pcmFormat.validBitsPerSample = 16;
        break;
    case QAudioFormat::Int32:
        pcmFormat.sampleEncoding = PcmSampleEncoding::Int32;
        pcmFormat.validBitsPerSample = 32;
        break;
    case QAudioFormat::Float:
        pcmFormat.sampleEncoding = PcmSampleEncoding::Float32;
        pcmFormat.validBitsPerSample = 32;
        break;
    case QAudioFormat::Unknown:
    case QAudioFormat::NSampleFormats:
        break;
    }

    return pcmFormat;
}

QAudioFormat::SampleFormat sampleFormatFromWaveFormat(const WAVEFORMATEX *waveFormat)
{
    return pcmFormatFromWaveFormat(waveFormat).qAudioSampleFormat();
}

bool isDirectMixWaveFormatSupported(const WAVEFORMATEX *waveFormat)
{
    return pcmFormatFromWaveFormat(waveFormat).isValid();
}

QAudioFormat qAudioFormatFromWaveFormat(const WAVEFORMATEX *waveFormat)
{
    QAudioFormat format;
    const PcmStreamFormat pcmFormat = pcmFormatFromWaveFormat(waveFormat);
    if (!pcmFormat.isValid()) {
        return format;
    }

    format.setSampleRate(pcmFormat.sampleRate);
    format.setChannelCount(pcmFormat.channelCount);
    format.setSampleFormat(pcmFormat.qAudioSampleFormat());
    return format.isValid() ? format : QAudioFormat {};
}

bool buildWaveFormat(const QAudioFormat &format, DWORD channelMask, WAVEFORMATEXTENSIBLE *waveFormat)
{
    if (!waveFormat || !format.isValid()) {
        return false;
    }

    WORD bitsPerSample = 0;
    GUID subFormat = KSDATAFORMAT_SUBTYPE_PCM;
    switch (format.sampleFormat()) {
    case QAudioFormat::UInt8:
        bitsPerSample = 8;
        break;
    case QAudioFormat::Int16:
        bitsPerSample = 16;
        break;
    case QAudioFormat::Int32:
        bitsPerSample = 32;
        break;
    case QAudioFormat::Float:
        bitsPerSample = 32;
        subFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        break;
    case QAudioFormat::Unknown:
    case QAudioFormat::NSampleFormats:
        return false;
    }

    WAVEFORMATEXTENSIBLE result = {};
    result.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    result.Format.nChannels = static_cast<WORD>(format.channelCount());
    result.Format.nSamplesPerSec = static_cast<DWORD>(format.sampleRate());
    result.Format.wBitsPerSample = bitsPerSample;
    result.Format.nBlockAlign = static_cast<WORD>(format.channelCount() * bitsPerSample / 8);
    result.Format.nAvgBytesPerSec = result.Format.nSamplesPerSec * result.Format.nBlockAlign;
    result.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    result.Samples.wValidBitsPerSample = bitsPerSample;
    result.dwChannelMask = channelMask;
    result.SubFormat = subFormat;

    *waveFormat = result;
    return true;
}

bool buildPcmWaveFormat(const PcmStreamFormat &format, DWORD channelMask, WAVEFORMATEXTENSIBLE *waveFormat)
{
    if (!waveFormat || !format.isValid()) {
        return false;
    }

    WORD bitsPerSample = 0;
    GUID subFormat = KSDATAFORMAT_SUBTYPE_PCM;
    switch (format.sampleEncoding) {
    case PcmSampleEncoding::UInt8:
        bitsPerSample = 8;
        break;
    case PcmSampleEncoding::Int16:
        bitsPerSample = 16;
        break;
    case PcmSampleEncoding::Int24:
        bitsPerSample = 24;
        break;
    case PcmSampleEncoding::Int32:
        bitsPerSample = 32;
        break;
    case PcmSampleEncoding::Float32:
        bitsPerSample = 32;
        subFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        break;
    case PcmSampleEncoding::Unknown:
        return false;
    }

    WAVEFORMATEXTENSIBLE result = {};
    result.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    result.Format.nChannels = static_cast<WORD>(format.channelCount);
    result.Format.nSamplesPerSec = static_cast<DWORD>(format.sampleRate);
    result.Format.wBitsPerSample = bitsPerSample;
    result.Format.nBlockAlign = static_cast<WORD>(format.channelCount * bitsPerSample / 8);
    result.Format.nAvgBytesPerSec = result.Format.nSamplesPerSec * result.Format.nBlockAlign;
    result.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    result.Samples.wValidBitsPerSample =
        static_cast<WORD>(qBound<int>(1, format.effectiveValidBitsPerSample(), bitsPerSample));
    result.dwChannelMask = channelMask;
    result.SubFormat = subFormat;

    *waveFormat = result;
    return true;
}

QList<QAudioFormat::SampleFormat> candidateSampleFormats(const WAVEFORMATEX *mixFormat,
                                                         int sourceBitDepth)
{
    QList<QAudioFormat::SampleFormat> formats;

    if (sourceBitDepth == 16) {
        formats.append(QAudioFormat::Int16);
    } else if (sourceBitDepth > 0 && sourceBitDepth <= 24) {
        formats.append(QAudioFormat::Int32);
    } else if (sourceBitDepth > 24) {
        formats.append(QAudioFormat::Int32);
        formats.append(QAudioFormat::Float);
    }

    const auto preferred = sampleFormatFromWaveFormat(mixFormat);
    if (preferred != QAudioFormat::Unknown && !formats.contains(preferred)) {
        formats.append(preferred);
    }

    const QList<QAudioFormat::SampleFormat> fallbacks {
        QAudioFormat::Float,
        QAudioFormat::Int16,
        QAudioFormat::Int32,
        QAudioFormat::UInt8,
    };
    for (const auto sampleFormat : fallbacks) {
        if (!formats.contains(sampleFormat)) {
            formats.append(sampleFormat);
        }
    }

    return formats;
}

void appendUniqueSampleRate(QList<int> *rates, int sampleRate)
{
    if (!rates || sampleRate <= 0 || rates->contains(sampleRate)) {
        return;
    }
    rates->append(sampleRate);
}

QList<int> exclusiveSampleRateCandidates(int mixSampleRate, int sourceSampleRate, bool exactMode)
{
    QList<int> rates;
    if (exactMode) {
        appendUniqueSampleRate(&rates, sourceSampleRate);
    } else {
        appendUniqueSampleRate(&rates, mixSampleRate);
    }

    const QList<int> commonRatesDescending {
        768000,
        705600,
        384000,
        352800,
        320000,
        282240,
        256000,
        192000,
        176400,
        128000,
        96000,
        88200,
        64000,
        48000,
        44100,
        32000,
        22050,
        16000,
        11025,
    };
    if (sourceSampleRate > 0) {
        for (const int rate : commonRatesDescending) {
            if (rate < sourceSampleRate) {
                appendUniqueSampleRate(&rates, rate);
            }
        }
        for (auto it = commonRatesDescending.crbegin(); it != commonRatesDescending.crend(); ++it) {
            if (*it > sourceSampleRate) {
                appendUniqueSampleRate(&rates, *it);
            }
        }
    } else {
        appendUniqueSampleRate(&rates, mixSampleRate);
        appendUniqueSampleRate(&rates, 48000);
        appendUniqueSampleRate(&rates, 44100);
        appendUniqueSampleRate(&rates, 96000);
        appendUniqueSampleRate(&rates, 192000);
    }

    appendUniqueSampleRate(&rates, mixSampleRate);
    return rates;
}

QList<PcmStreamFormat> exclusivePcmCandidates(const PcmStreamFormat &mixFormat,
                                              const QList<int> &sampleRates,
                                              const QList<int> &channelCounts,
                                              int sourceBitDepth,
                                              bool exactMode)
{
    QList<PcmStreamFormat> candidates;
    auto appendCandidate = [&candidates](int sampleRate,
                                         int channelCount,
                                         PcmSampleEncoding encoding,
                                         int validBits = 0) {
        PcmStreamFormat candidate;
        candidate.sampleRate = sampleRate;
        candidate.channelCount = channelCount;
        candidate.sampleEncoding = encoding;
        candidate.validBitsPerSample = validBits;
        if (!candidate.isValid()) {
            return;
        }

        const bool duplicate = std::any_of(candidates.cbegin(),
                                           candidates.cend(),
                                           [&candidate](const PcmStreamFormat &existing) {
                                               return existing.sampleRate == candidate.sampleRate
                                                   && existing.channelCount == candidate.channelCount
                                                   && existing.sampleEncoding == candidate.sampleEncoding
                                                   && existing.effectiveValidBitsPerSample()
                                                       == candidate.effectiveValidBitsPerSample();
                                           });
        if (!duplicate) {
            candidates.append(candidate);
        }
    };

    for (const int sampleRate : sampleRates) {
        for (const int channelCount : channelCounts) {
            if (exactMode) {
                if (sourceBitDepth == 16) {
                    appendCandidate(sampleRate, channelCount, PcmSampleEncoding::Int16, 16);
                } else if (sourceBitDepth == 24) {
                    appendCandidate(sampleRate, channelCount, PcmSampleEncoding::Int24, 24);
                    appendCandidate(sampleRate, channelCount, PcmSampleEncoding::Int32, 24);
                } else if (sourceBitDepth == 32) {
                    appendCandidate(sampleRate, channelCount, PcmSampleEncoding::Int32, 32);
                    appendCandidate(sampleRate, channelCount, PcmSampleEncoding::Float32, 32);
                } else if (sourceBitDepth == 8) {
                    appendCandidate(sampleRate, channelCount, PcmSampleEncoding::UInt8, 8);
                }
            }

            appendCandidate(sampleRate,
                            channelCount,
                            mixFormat.sampleEncoding,
                            mixFormat.effectiveValidBitsPerSample());

            if (!exactMode) {
                if (sourceBitDepth == 16) {
                    appendCandidate(sampleRate, channelCount, PcmSampleEncoding::Int16, 16);
                } else if (sourceBitDepth == 24) {
                    appendCandidate(sampleRate, channelCount, PcmSampleEncoding::Int24, 24);
                    appendCandidate(sampleRate, channelCount, PcmSampleEncoding::Int32, 24);
                } else if (sourceBitDepth == 32) {
                    appendCandidate(sampleRate, channelCount, PcmSampleEncoding::Int32, 32);
                    appendCandidate(sampleRate, channelCount, PcmSampleEncoding::Float32, 32);
                } else if (sourceBitDepth == 8) {
                    appendCandidate(sampleRate, channelCount, PcmSampleEncoding::UInt8, 8);
                }
            }

            appendCandidate(sampleRate, channelCount, PcmSampleEncoding::Int24, 24);
            appendCandidate(sampleRate, channelCount, PcmSampleEncoding::Int32, 24);
            appendCandidate(sampleRate, channelCount, PcmSampleEncoding::Int32, 32);
            appendCandidate(sampleRate, channelCount, PcmSampleEncoding::Int16, 16);
            appendCandidate(sampleRate, channelCount, PcmSampleEncoding::Float32, 32);
        }
    }

    return candidates;
}

HRESULT openRenderEndpoint(const QAudioDevice &qtDevice,
                           bool usesDefault,
                           IMMDevice **device,
                           IAudioClient **audioClient)
{
    if (device) {
        *device = nullptr;
    }
    if (audioClient) {
        *audioClient = nullptr;
    }

    IMMDeviceEnumerator *enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator),
                                  nullptr,
                                  CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void **>(&enumerator));
    if (FAILED(hr)) {
        return hr;
    }

    IMMDevice *resolvedDevice = nullptr;
    if (usesDefault) {
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &resolvedDevice);
    } else {
        const QString endpointId = endpointIdFromQtId(qtDevice.id());
        hr = enumerator->GetDevice(reinterpret_cast<LPCWSTR>(endpointId.utf16()), &resolvedDevice);
    }
    safeRelease(enumerator);
    if (FAILED(hr)) {
        return hr;
    }

    IAudioClient *resolvedAudioClient = nullptr;
    hr = resolvedDevice->Activate(__uuidof(IAudioClient),
                                  CLSCTX_ALL,
                                  nullptr,
                                  reinterpret_cast<void **>(&resolvedAudioClient));
    if (FAILED(hr)) {
        safeRelease(resolvedDevice);
        return hr;
    }

    if (device) {
        *device = resolvedDevice;
    } else {
        safeRelease(resolvedDevice);
    }

    if (audioClient) {
        *audioClient = resolvedAudioClient;
    } else {
        safeRelease(resolvedAudioClient);
    }

    return S_OK;
}

} // namespace

QList<QAudioDevice> WindowsWasapiAudioPlayer::availableOutputDevices() const
{
    return QMediaDevices::audioOutputs();
}

QString WindowsWasapiAudioPlayer::outputDeviceDescription() const
{
    return m_outputDeviceDescription;
}

QAudioFormat WindowsWasapiAudioPlayer::outputFormat() const
{
    return m_outputFormat;
}

int WindowsWasapiAudioPlayer::outputDeviceBitDepth() const
{
    return m_outputPcmFormat.effectiveValidBitsPerSample();
}

QAudioDevice WindowsWasapiAudioPlayer::selectedOutputDevice() const
{
    return resolveOutputDevice();
}

QByteArray WindowsWasapiAudioPlayer::selectedOutputDeviceId() const
{
    return m_selectedOutputDeviceId;
}

bool WindowsWasapiAudioPlayer::usesDefaultOutputDevice() const
{
    return m_selectedOutputDeviceId.isEmpty();
}

void WindowsWasapiAudioPlayer::setOutputDeviceId(const QByteArray &deviceId)
{
    QByteArray normalizedDeviceId = deviceId;
    if (!normalizedDeviceId.isEmpty()) {
        const auto devices = availableOutputDevices();
        const auto foundIt = std::find_if(devices.cbegin(), devices.cend(), [&normalizedDeviceId](const QAudioDevice &device) {
            return device.id() == normalizedDeviceId;
        });
        if (foundIt == devices.cend()) {
            PlayerLogger::log(QStringLiteral("player"),
                              QStringLiteral("setOutputDeviceId fallback-default unknownId=%1")
                                  .arg(QString::fromLatin1(normalizedDeviceId.toHex())));
            normalizedDeviceId.clear();
        }
    }

    if (m_selectedOutputDeviceId == normalizedDeviceId) {
        return;
    }

    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("setOutputDeviceId previous=%1 target=%2")
                          .arg(QString::fromLatin1(m_selectedOutputDeviceId.toHex()))
                          .arg(QString::fromLatin1(normalizedDeviceId.toHex())));
    m_selectedOutputDeviceId = normalizedDeviceId;
    emitOutputDeviceSelectionChanged();
    beginActiveOutputSwitch(ActiveOutputSwitchTrigger::DeviceSelection,
                            QStringLiteral("selectionChanged"));
}

void WindowsWasapiAudioPlayer::handleAudioOutputsChanged()
{
    const auto devices = availableOutputDevices();
    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("handleAudioOutputsChanged count=%1 selectedDevice=%2 activeDevice=%3")
                          .arg(devices.size())
                          .arg(QString::fromLatin1(m_selectedOutputDeviceId.toHex()))
                          .arg(QString::fromLatin1(m_activeOutputDeviceId.toHex())));
    bool selectionChanged = false;
    if (!m_selectedOutputDeviceId.isEmpty()) {
        const auto foundIt = std::find_if(devices.cbegin(), devices.cend(), [this](const QAudioDevice &device) {
            return device.id() == m_selectedOutputDeviceId;
        });
        if (foundIt == devices.cend()) {
            PlayerLogger::log(QStringLiteral("player"),
                              QStringLiteral("handleAudioOutputsChanged selected-device-missing fallback-default id=%1")
                                  .arg(QString::fromLatin1(m_selectedOutputDeviceId.toHex())));
            m_selectedOutputDeviceId.clear();
            selectionChanged = true;
        }
    }

    emit outputDevicesChanged();
    if (selectionChanged) {
        emitOutputDeviceSelectionChanged();
    }

    const QAudioDevice targetDevice = resolveOutputDevice();
    const bool systemSwitchAlreadySuspended =
        m_activeOutputSwitch.trigger == ActiveOutputSwitchTrigger::SystemDeviceChange
        && m_activeOutputSwitch.phase == ActiveOutputSwitchPhase::OutputSuspended;
    const bool hasActiveSwitch = isActiveOutputSwitchInProgress();
    const bool activeSwitchWaitingForBoundary =
        m_activeOutputSwitch.phase == ActiveOutputSwitchPhase::WaitingForInvalidation
        || m_activeOutputSwitch.phase == ActiveOutputSwitchPhase::WaitingForOutputStart;
    if (hasActiveSwitch
        && activeSwitchWaitingForBoundary
        && m_playbackState == PlaybackState::Playing
        && m_audioWorker
        && m_activeDecoderSessionId != 0
        && !systemSwitchAlreadySuspended
        && targetDevice.id() == m_activeOutputDeviceId) {
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("handleAudioOutputsChanged deferred-active-output active-transaction trigger=%1 phase=%2 deviceId=%3")
                              .arg(activeOutputSwitchTriggerName(m_activeOutputSwitch.trigger))
                              .arg(activeOutputSwitchPhaseName(m_activeOutputSwitch.phase))
                              .arg(QString::fromLatin1(targetDevice.id().toHex())));
        return;
    }

    QString targetDeviceDescription;
    QByteArray targetWaveFormatData;
    PcmStreamFormat targetPcmFormat;
    const QAudioFormat targetFormat =
        selectOutputFormat(&targetDeviceDescription, &targetWaveFormatData, &targetPcmFormat);
    const bool effectiveOutputUnchanged =
        m_playbackState == PlaybackState::Playing
        && m_audioWorker
        && m_activeDecoderSessionId != 0
        && !systemSwitchAlreadySuspended
        && targetFormat.isValid()
        && targetDevice.id() == m_activeOutputDeviceId
        && targetFormat == m_outputFormat
        && targetWaveFormatData == m_outputWaveFormatData
        && targetDeviceDescription == m_outputDeviceDescription;

    if (effectiveOutputUnchanged && hasActiveSwitch) {
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("handleAudioOutputsChanged unchanged-active-output active-transaction trigger=%1 phase=%2 deviceId=%3")
                              .arg(activeOutputSwitchTriggerName(m_activeOutputSwitch.trigger))
                              .arg(activeOutputSwitchPhaseName(m_activeOutputSwitch.phase))
                              .arg(QString::fromLatin1(targetDevice.id().toHex())));
        return;
    }

    if (effectiveOutputUnchanged && m_ignoreNextUnchangedOutputChangeAfterExclusiveFallback) {
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("handleAudioOutputsChanged ignored-unchanged-output-after-exclusive-fallback deviceId=%1 rate=%2 channels=%3 bits=%4")
                              .arg(QString::fromLatin1(targetDevice.id().toHex()))
                              .arg(targetPcmFormat.sampleRate)
                              .arg(targetPcmFormat.channelCount)
                              .arg(targetPcmFormat.bitsPerSample()));
        m_ignoreNextUnchangedOutputChangeAfterExclusiveFallback = false;
        return;
    }

    if (effectiveOutputUnchanged) {
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("handleAudioOutputsChanged ignored-unchanged-active-output deviceId=%1 rate=%2 channels=%3 bits=%4")
                              .arg(QString::fromLatin1(targetDevice.id().toHex()))
                              .arg(targetPcmFormat.sampleRate)
                              .arg(targetPcmFormat.channelCount)
                              .arg(targetPcmFormat.bitsPerSample()));
        return;
    }

    beginActiveOutputSwitch(ActiveOutputSwitchTrigger::SystemDeviceChange,
                            QStringLiteral("audioOutputsChanged"),
                            true);
}

bool WindowsWasapiAudioPlayer::isActiveOutputSwitchInProgress() const
{
    return m_activeOutputSwitch.isActive();
}

bool WindowsWasapiAudioPlayer::activeOutputSwitchMatchesSession(int sessionId) const
{
    return isActiveOutputSwitchInProgress()
        && sessionId != 0
        && (m_activeOutputSwitch.sessionId == 0 || m_activeOutputSwitch.sessionId == sessionId);
}

void WindowsWasapiAudioPlayer::setActiveOutputSwitchPhase(ActiveOutputSwitchPhase phase, const QString &reason)
{
    if (m_activeOutputSwitch.phase == phase) {
        return;
    }

    if (m_activeOutputSwitch.phase == ActiveOutputSwitchPhase::Idle
        && phase != ActiveOutputSwitchPhase::Idle
        && !m_activeOutputSwitch.transactionTimer.isValid()) {
        m_activeOutputSwitch.transactionTimer.start();
    }

    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("activeOutputSwitch phase %1 -> %2 reason=%3 trigger=%4 session=%5 force=%6 freshBuffer=%7 transactionReason=%8")
                          .arg(activeOutputSwitchPhaseName(m_activeOutputSwitch.phase))
                          .arg(activeOutputSwitchPhaseName(phase))
                          .arg(reason)
                          .arg(activeOutputSwitchTriggerName(m_activeOutputSwitch.trigger))
                          .arg(m_activeOutputSwitch.sessionId)
                          .arg(m_activeOutputSwitch.forceReconfigure)
                          .arg(m_activeOutputSwitch.needsFreshBuffer)
                          .arg(m_activeOutputSwitch.reason));
    PlayerLogger::diagnostic(QStringLiteral("player"),
                             QStringLiteral("output_switch_phase"),
                             {
                                 {QStringLiteral("transactionKind"), QStringLiteral("active-switch")},
                                 {QStringLiteral("previousPhase"), activeOutputSwitchPhaseName(m_activeOutputSwitch.phase)},
                                 {QStringLiteral("phase"), activeOutputSwitchPhaseName(phase)},
                                 {QStringLiteral("reason"), reason},
                                 {QStringLiteral("transactionReason"), m_activeOutputSwitch.reason},
                                 {QStringLiteral("trigger"), activeOutputSwitchTriggerName(m_activeOutputSwitch.trigger)},
                                 {QStringLiteral("sessionId"), m_activeOutputSwitch.sessionId},
                                 {QStringLiteral("positionMs"), m_currentPositionMs},
                                 {QStringLiteral("forceReconfigure"), m_activeOutputSwitch.forceReconfigure},
                                 {QStringLiteral("freshBufferRequired"), m_activeOutputSwitch.needsFreshBuffer},
                             });
    m_activeOutputSwitch.phase = phase;
}

void WindowsWasapiAudioPlayer::resetActiveOutputSwitch(const QString &reason)
{
    if (!isActiveOutputSwitchInProgress()
        && m_activeOutputSwitch.trigger == ActiveOutputSwitchTrigger::None
        && m_activeOutputSwitch.sessionId == 0
        && !m_activeOutputSwitch.forceReconfigure
        && !m_activeOutputSwitch.transitionPrepared
        && !m_activeOutputSwitch.needsFreshBuffer) {
        return;
    }

    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("activeOutputSwitch reset reason=%1 trigger=%2 phase=%3 session=%4 force=%5 transitionPrepared=%6 freshBuffer=%7 transactionReason=%8")
                          .arg(reason)
                          .arg(activeOutputSwitchTriggerName(m_activeOutputSwitch.trigger))
                          .arg(activeOutputSwitchPhaseName(m_activeOutputSwitch.phase))
                          .arg(m_activeOutputSwitch.sessionId)
                          .arg(m_activeOutputSwitch.forceReconfigure)
                          .arg(m_activeOutputSwitch.transitionPrepared)
                          .arg(m_activeOutputSwitch.needsFreshBuffer)
                          .arg(m_activeOutputSwitch.reason));
    PlayerLogger::diagnostic(QStringLiteral("player"),
                             QStringLiteral("output_switch_done"),
                             {
                                 {QStringLiteral("transactionKind"), QStringLiteral("active-switch")},
                                 {QStringLiteral("reason"), reason},
                                 {QStringLiteral("trigger"), activeOutputSwitchTriggerName(m_activeOutputSwitch.trigger)},
                                 {QStringLiteral("phase"), activeOutputSwitchPhaseName(m_activeOutputSwitch.phase)},
                                 {QStringLiteral("sessionId"), m_activeOutputSwitch.sessionId},
                                 {QStringLiteral("positionMs"), m_currentPositionMs},
                                 {QStringLiteral("forceReconfigure"), m_activeOutputSwitch.forceReconfigure},
                                 {QStringLiteral("freshBufferRequired"), m_activeOutputSwitch.needsFreshBuffer},
                             });
    m_activeOutputSwitch = {};
}

void WindowsWasapiAudioPlayer::beginActiveOutputSwitch(ActiveOutputSwitchTrigger trigger,
                                                       const QString &reason,
                                                       bool forceReconfigure)
{
    if (trigger == ActiveOutputSwitchTrigger::None) {
        return;
    }

    if (m_outputRecoveryPending) {
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("activeOutputSwitch cancel-recovery reason=%1 trigger=%2 attempt=%3 expectedSession=%4")
                              .arg(reason)
                              .arg(activeOutputSwitchTriggerName(trigger))
                              .arg(m_outputRecoveryAttempt)
                              .arg(m_outputRecoveryExpectedSessionId));
        resetOutputRecoveryState(QStringLiteral("activeOutputSwitch:%1").arg(reason));
    }

    const bool alreadyActive = isActiveOutputSwitchInProgress();
    const ActiveOutputSwitchTrigger previousTrigger = m_activeOutputSwitch.trigger;
    const ActiveOutputSwitchPhase previousPhase = m_activeOutputSwitch.phase;
    const QString previousReason = m_activeOutputSwitch.reason;

    if (!alreadyActive) {
        m_activeOutputSwitch = {};
        m_activeOutputSwitch.trigger = trigger;
        m_activeOutputSwitch.reason = reason;
        m_activeOutputSwitch.sessionId = m_activeDecoderSessionId;
        m_activeOutputSwitch.forceReconfigure = forceReconfigure;
        setActiveOutputSwitchPhase(ActiveOutputSwitchPhase::Pending, reason);
        PlayerLogger::diagnostic(QStringLiteral("player"),
                                 QStringLiteral("output_switch_requested"),
                                 {
                                     {QStringLiteral("transactionKind"), QStringLiteral("active-switch")},
                                     {QStringLiteral("trigger"), activeOutputSwitchTriggerName(trigger)},
                                     {QStringLiteral("reason"), reason},
                                     {QStringLiteral("sessionId"), m_activeOutputSwitch.sessionId},
                                     {QStringLiteral("positionMs"), m_currentPositionMs},
                                     {QStringLiteral("previousDeviceId"), QString::fromLatin1(m_activeOutputDeviceId.toHex())},
                                     {QStringLiteral("targetDeviceId"), QString::fromLatin1(resolveOutputDevice().id().toHex())},
                                     {QStringLiteral("targetDeviceName"), resolveOutputDevice().description()},
                                     {QStringLiteral("forceReconfigure"), forceReconfigure},
                                 });
    } else {
        m_activeOutputSwitch.trigger = trigger;
        m_activeOutputSwitch.reason = reason;
        if (m_activeOutputSwitch.sessionId == 0) {
            m_activeOutputSwitch.sessionId = m_activeDecoderSessionId;
        }
        m_activeOutputSwitch.forceReconfigure = m_activeOutputSwitch.forceReconfigure || forceReconfigure;
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("activeOutputSwitch refresh reason=%1 previousReason=%2 trigger=%3 previousTrigger=%4 phase=%5 previousPhase=%6 force=%7")
                              .arg(reason)
                              .arg(previousReason)
                              .arg(activeOutputSwitchTriggerName(trigger))
                              .arg(activeOutputSwitchTriggerName(previousTrigger))
                              .arg(activeOutputSwitchPhaseName(m_activeOutputSwitch.phase))
                              .arg(activeOutputSwitchPhaseName(previousPhase))
                              .arg(m_activeOutputSwitch.forceReconfigure));
    }

    const bool isSystemDeviceChange = trigger == ActiveOutputSwitchTrigger::SystemDeviceChange;
    if (isSystemDeviceChange
        && m_playbackState == PlaybackState::Playing
        && m_audioStarted
        && m_audioWorker
        && m_activeDecoderSessionId != 0
        && m_activeOutputSwitch.phase != ActiveOutputSwitchPhase::OutputSuspended) {
        m_activeOutputSwitch.sessionId = m_activeDecoderSessionId;
        m_activeOutputSwitch.forceReconfigure = true;
        m_activeOutputSwitch.transitionPrepared = false;
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("activeOutputSwitch suspend-output reason=%1 session=%2 positionMs=%3 trigger=%4")
                              .arg(reason)
                              .arg(m_activeDecoderSessionId)
                              .arg(m_currentPositionMs)
                              .arg(activeOutputSwitchTriggerName(trigger)));
        prepareConfirmedActiveSwitchFade(QStringLiteral("before-output-suspend"));
        releaseOutputResources();
        m_audioStarted = false;
        m_audioState = QAudio::StoppedState;
        m_audioError = QtAudio::NoError;
        setActiveOutputSwitchPhase(ActiveOutputSwitchPhase::OutputSuspended, reason);
    }

    const bool shouldPrepareTransition = !isSystemDeviceChange
        && m_activeOutputSwitch.phase == ActiveOutputSwitchPhase::Pending;
    if (shouldPrepareTransition
        && m_playbackState == PlaybackState::Playing
        && m_audioWorker
        && m_activeDecoderSessionId != 0
        && !m_activeOutputSwitch.transitionPrepared) {
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("activeOutputSwitch prepare-transition reason=%1 trigger=%2 session=%3")
                              .arg(reason)
                              .arg(activeOutputSwitchTriggerName(trigger))
                              .arg(m_activeDecoderSessionId));
        prepareConfirmedActiveSwitchFade(QStringLiteral("pending-active-switch"));
    }

    scheduleActiveOutputSwitch();
}

void WindowsWasapiAudioPlayer::scheduleActiveOutputSwitch()
{
    if (!isActiveOutputSwitchInProgress()) {
        return;
    }

    if (!m_outputDeviceChangeTimer) {
        applyActiveOutputSwitch();
        return;
    }

    const bool shouldDebounce = m_playbackState == PlaybackState::Playing
        || m_playbackState == PlaybackState::Paused;
    if (!shouldDebounce) {
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("activeOutputSwitch immediate reason=%1 trigger=%2 state=%3")
                              .arg(m_activeOutputSwitch.reason)
                              .arg(activeOutputSwitchTriggerName(m_activeOutputSwitch.trigger))
                              .arg(static_cast<int>(m_playbackState)));
        applyActiveOutputSwitch();
        return;
    }

    const int debounceMs = outputDeviceChangeDebounceMsForReason(m_activeOutputSwitch.reason);
    if (m_outputDeviceChangeTimer->interval() != debounceMs) {
        m_outputDeviceChangeTimer->setInterval(debounceMs);
    }

    const bool debounceActive = m_outputDeviceChangeTimer->isActive();
    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("activeOutputSwitch %1 reason=%2 trigger=%3 phase=%4 intervalMs=%5 state=%6 force=%7 freshBuffer=%8")
                          .arg(debounceActive ? QStringLiteral("debounce-refresh")
                                              : QStringLiteral("debounce-start"))
                          .arg(m_activeOutputSwitch.reason)
                          .arg(activeOutputSwitchTriggerName(m_activeOutputSwitch.trigger))
                          .arg(activeOutputSwitchPhaseName(m_activeOutputSwitch.phase))
                          .arg(m_outputDeviceChangeTimer->interval())
                          .arg(static_cast<int>(m_playbackState))
                          .arg(m_activeOutputSwitch.forceReconfigure)
                          .arg(m_activeOutputSwitch.needsFreshBuffer));
    m_outputDeviceChangeTimer->start();
}

void WindowsWasapiAudioPlayer::refreshOutputConfiguration(bool force)
{
    if (force && scriptedSameOutputInvalidationIoErrorEnabled()) {
        triggerScriptedSameOutputInvalidationIoError();
        return;
    }

    const ActiveOutputSwitchTrigger trigger = force
        ? ActiveOutputSwitchTrigger::OutputFormatChange
        : ActiveOutputSwitchTrigger::OutputRefresh;
    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("refreshOutputConfiguration force=%1 state=%2 trigger=%3")
                          .arg(force)
                          .arg(static_cast<int>(m_playbackState))
                          .arg(activeOutputSwitchTriggerName(trigger)));
    beginActiveOutputSwitch(trigger, QStringLiteral("automationRefresh"), force);
}

void WindowsWasapiAudioPlayer::triggerScriptedSameOutputInvalidationIoError()
{
    const int sessionId = m_activeDecoderSessionId;
    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("scriptedOutputSwitch trigger=%1 requested state=%2 session=%3 active=%4")
                          .arg(QString::fromLatin1(kSameOutputInvalidationIoErrorTrigger))
                          .arg(static_cast<int>(m_playbackState))
                          .arg(sessionId)
                          .arg(isActiveOutputSwitchInProgress()));

    if (m_playbackState != PlaybackState::Playing || !m_audioWorker || sessionId == 0
        || m_sourcePath.isEmpty()) {
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("scriptedOutputSwitch ignored reason=not-ready state=%1 session=%2 hasWorker=%3 hasSource=%4")
                              .arg(static_cast<int>(m_playbackState))
                              .arg(sessionId)
                              .arg(m_audioWorker != nullptr)
                              .arg(!m_sourcePath.isEmpty()));
        return;
    }

    if (isActiveOutputSwitchInProgress()) {
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("scriptedOutputSwitch ignored reason=active-switch-in-progress trigger=%1 phase=%2 session=%3")
                              .arg(activeOutputSwitchTriggerName(m_activeOutputSwitch.trigger))
                              .arg(activeOutputSwitchPhaseName(m_activeOutputSwitch.phase))
                              .arg(m_activeOutputSwitch.sessionId));
        return;
    }

    QString targetDeviceDescription;
    QByteArray targetWaveFormatData;
    PcmStreamFormat targetPcmFormat;
    const QAudioDevice targetDevice = resolveOutputDevice();
    const QAudioFormat targetFormat =
        selectOutputFormat(&targetDeviceDescription, &targetWaveFormatData, &targetPcmFormat);
    Q_UNUSED(targetWaveFormatData);
    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("scriptedOutputSwitch same-output-invalidation trigger=%1 session=%2 deviceId=%3 formatValid=%4 rate=%5 channels=%6 bits=%7")
                          .arg(QString::fromLatin1(kSameOutputInvalidationIoErrorTrigger))
                          .arg(sessionId)
                          .arg(QString::fromLatin1(targetDevice.id().toHex()))
                          .arg(targetFormat.isValid())
                          .arg(targetPcmFormat.sampleRate)
                          .arg(targetPcmFormat.channelCount)
                          .arg(targetPcmFormat.bitsPerSample()));

    m_activeOutputSwitch = {};
    m_activeOutputSwitch.trigger = ActiveOutputSwitchTrigger::OutputFormatChange;
    m_activeOutputSwitch.reason = QStringLiteral("audioOutputsChanged");
    m_activeOutputSwitch.sessionId = sessionId;
    m_activeOutputSwitch.forceReconfigure = true;
    setActiveOutputSwitchPhase(ActiveOutputSwitchPhase::WaitingForInvalidation,
                               QStringLiteral("scripted-same-output-invalidation"));

    QTimer::singleShot(0, this, [this, sessionId] {
        if (!activeOutputSwitchMatchesSession(sessionId)
            || m_activeOutputSwitch.phase != ActiveOutputSwitchPhase::WaitingForInvalidation) {
            return;
        }

        handleAudioStateChanged(sessionId,
                                static_cast<int>(QAudio::StoppedState),
                                static_cast<int>(QtAudio::IOError));
    });
}

void WindowsWasapiAudioPlayer::applyActiveOutputSwitch()
{
    if (!isActiveOutputSwitchInProgress()) {
        return;
    }

    const ActiveOutputSwitchTransaction transaction = m_activeOutputSwitch;
    const bool outputSuspended = transaction.phase == ActiveOutputSwitchPhase::OutputSuspended;
    const bool transitionPrepared = transaction.transitionPrepared;
    const bool forceReconfigure = transaction.forceReconfigure || outputSuspended;
    setActiveOutputSwitchPhase(ActiveOutputSwitchPhase::Preflight, QStringLiteral("preflight"));

    if (m_sourcePath.isEmpty() || m_playbackState == PlaybackState::Stopping) {
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("activeSwitchPreflight decision=inactive-source trigger=%1 reason=%2 entryPhase=%3 playbackState=%4")
                              .arg(activeOutputSwitchTriggerName(transaction.trigger))
                              .arg(transaction.reason)
                              .arg(activeOutputSwitchPhaseName(transaction.phase))
                              .arg(static_cast<int>(m_playbackState)));
        resetActiveOutputSwitch(QStringLiteral("inactive-source"));
        return;
    }

    const QAudioDevice device = resolveOutputDevice();
    const QByteArray targetDeviceId = device.id();
    QString targetDeviceDescription;
    QByteArray targetWaveFormatData;
    PcmStreamFormat targetPcmFormat;
    const QAudioFormat targetFormat =
        selectOutputFormat(&targetDeviceDescription, &targetWaveFormatData, &targetPcmFormat);
    const bool formatChanged = targetFormat.isValid()
        && (targetFormat != m_outputFormat
            || targetWaveFormatData != m_outputWaveFormatData
            || targetDeviceDescription != m_outputDeviceDescription);
    auto *buffer = static_cast<PcmStreamBuffer *>(m_bufferDevice);
    const qsizetype bufferedBytes = buffer ? buffer->bufferedBytes() : -1;
    const auto logPreflight = [&](const QString &decision, bool hotReconfigureEligible) {
        const qint64 elapsedMs = m_activeOutputSwitch.transactionTimer.isValid()
            ? m_activeOutputSwitch.transactionTimer.elapsed()
            : -1;
    PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("activeSwitchPreflight decision=%1 trigger=%2 reason=%3 entryPhase=%4 phase=%5 outputSuspended=%6 force=%7 freshBuffer=%8 transitionPrepared=%9 hotReconfigureEligible=%10 formatChanged=%11 targetFormatValid=%12 currentDevice=%13 targetDevice=%14 currentRate=%15 currentChannels=%16 currentBits=%17 targetRate=%18 targetChannels=%19 targetBits=%20 bufferedBytes=%21 decoderFinished=%22 audioStarted=%23 elapsedMs=%24")
                              .arg(decision)
                              .arg(activeOutputSwitchTriggerName(transaction.trigger))
                              .arg(transaction.reason)
                              .arg(activeOutputSwitchPhaseName(transaction.phase))
                              .arg(activeOutputSwitchPhaseName(m_activeOutputSwitch.phase))
                              .arg(outputSuspended)
                              .arg(forceReconfigure)
                              .arg(m_activeOutputSwitch.needsFreshBuffer)
                              .arg(m_activeOutputSwitch.transitionPrepared)
                              .arg(hotReconfigureEligible)
                              .arg(formatChanged)
                              .arg(targetFormat.isValid())
                              .arg(QString::fromLatin1(m_activeOutputDeviceId.toHex()))
                              .arg(QString::fromLatin1(targetDeviceId.toHex()))
                              .arg(m_decoderPcmFormat.sampleRate)
                              .arg(m_decoderPcmFormat.channelCount)
                              .arg(m_decoderPcmFormat.bitsPerSample())
                              .arg(targetPcmFormat.sampleRate)
                              .arg(targetPcmFormat.channelCount)
                              .arg(targetPcmFormat.bitsPerSample())
                              .arg(bufferedBytes)
                              .arg(m_decoderFinished)
                              .arg(m_audioStarted)
                              .arg(elapsedMs));
        PlayerLogger::diagnostic(QStringLiteral("player"),
                                 QStringLiteral("output_switch_start"),
                                 {
                                     {QStringLiteral("transactionKind"), QStringLiteral("active-switch")},
                                     {QStringLiteral("decision"), decision},
                                     {QStringLiteral("trigger"), activeOutputSwitchTriggerName(transaction.trigger)},
                                     {QStringLiteral("reason"), transaction.reason},
                                     {QStringLiteral("entryPhase"), activeOutputSwitchPhaseName(transaction.phase)},
                                     {QStringLiteral("phase"), activeOutputSwitchPhaseName(m_activeOutputSwitch.phase)},
                                     {QStringLiteral("sessionId"), m_activeOutputSwitch.sessionId},
                                     {QStringLiteral("positionMs"), m_currentPositionMs},
                                     {QStringLiteral("previousDeviceId"), QString::fromLatin1(m_activeOutputDeviceId.toHex())},
                                     {QStringLiteral("targetDeviceId"), QString::fromLatin1(targetDeviceId.toHex())},
                                     {QStringLiteral("targetDeviceName"), targetDeviceDescription},
                                     {QStringLiteral("currentSampleRate"), m_decoderPcmFormat.sampleRate},
                                     {QStringLiteral("targetSampleRate"), targetPcmFormat.sampleRate},
                                     {QStringLiteral("targetChannels"), targetPcmFormat.channelCount},
                                     {QStringLiteral("targetBits"), targetPcmFormat.bitsPerSample()},
                                     {QStringLiteral("hotReconfigureEligible"), hotReconfigureEligible},
                                     {QStringLiteral("formatChanged"), formatChanged},
                                     {QStringLiteral("elapsedMs"), elapsedMs},
                                 });
    };

    if (targetDeviceId == m_activeOutputDeviceId && !formatChanged && !forceReconfigure) {
        logPreflight(QStringLiteral("device-unchanged"), false);
        if (!outputSuspended && transitionPrepared && m_audioWorker && m_activeDecoderSessionId != 0) {
            const int sessionId = m_activeDecoderSessionId;
            PlayerLogger::log(QStringLiteral("player"),
                              QStringLiteral("activeOutputSwitch restore-transition session=%1 deviceUnchanged=1 trigger=%2")
                                  .arg(sessionId)
                                  .arg(activeOutputSwitchTriggerName(transaction.trigger)));
            restoreOutputDeviceChangeTransition(sessionId);
        }
        resetActiveOutputSwitch(QStringLiteral("device-unchanged"));
        return;
    }

    if (m_playbackState == PlaybackState::Playing) {
        const bool invalidatedActiveOutput =
            transaction.phase == ActiveOutputSwitchPhase::WaitingForInvalidation
            || transaction.needsFreshBuffer;
        setActiveOutputSwitchPhase(ActiveOutputSwitchPhase::Applying, QStringLiteral("apply"));
        const bool hotReconfigureEligible =
            !outputSuspended && !invalidatedActiveOutput && canHotReconfigureOutput(targetPcmFormat);
        if (hotReconfigureEligible) {
            logPreflight(QStringLiteral("hot-reconfigure"), hotReconfigureEligible);
            reconfigureActiveOutput(device,
                                    targetDeviceDescription,
                                    targetFormat,
                                    targetWaveFormatData,
                                    targetPcmFormat);
            setActiveOutputSwitchPhase(ActiveOutputSwitchPhase::WaitingForOutputStart,
                                       QStringLiteral("hot-reconfigure"));
            return;
        }

        prepareConfirmedActiveSwitchFade(QStringLiteral("pre-conservative-rebuild"));
        logPreflight(QStringLiteral("conservative-rebuild"), hotReconfigureEligible);
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("activeOutputSwitch conservative-rebuild positionMs=%1 previousDevice=%2 targetDevice=%3 formatChanged=%4 force=%5 trigger=%6 phase=%7 hotReconfigureEligible=%8 currentRate=%9 currentChannels=%10 currentBits=%11 targetRate=%12 targetChannels=%13 targetBits=%14")
                              .arg(m_currentPositionMs)
                              .arg(QString::fromLatin1(m_activeOutputDeviceId.toHex()))
                              .arg(QString::fromLatin1(targetDeviceId.toHex()))
                              .arg(formatChanged)
                              .arg(forceReconfigure)
                              .arg(activeOutputSwitchTriggerName(transaction.trigger))
                              .arg(activeOutputSwitchPhaseName(transaction.phase))
                              .arg(false)
                              .arg(m_decoderPcmFormat.sampleRate)
                              .arg(m_decoderPcmFormat.channelCount)
                              .arg(m_decoderPcmFormat.bitsPerSample())
                              .arg(targetPcmFormat.sampleRate)
                              .arg(targetPcmFormat.channelCount)
                              .arg(targetPcmFormat.bitsPerSample()));
        PlayerLogger::diagnostic(QStringLiteral("player"),
                                 QStringLiteral("output_switch_conservative_rebuild"),
                                 {
                                     {QStringLiteral("transactionKind"), QStringLiteral("active-switch")},
                                     {QStringLiteral("sessionId"), m_activeOutputSwitch.sessionId},
                                     {QStringLiteral("positionMs"), m_currentPositionMs},
                                     {QStringLiteral("trigger"), activeOutputSwitchTriggerName(transaction.trigger)},
                                     {QStringLiteral("phase"), activeOutputSwitchPhaseName(transaction.phase)},
                                     {QStringLiteral("reason"), transaction.reason},
                                     {QStringLiteral("previousDeviceId"), QString::fromLatin1(m_activeOutputDeviceId.toHex())},
                                     {QStringLiteral("targetDeviceId"), QString::fromLatin1(targetDeviceId.toHex())},
                                     {QStringLiteral("targetDeviceName"), targetDeviceDescription},
                                     {QStringLiteral("targetSampleRate"), targetPcmFormat.sampleRate},
                                     {QStringLiteral("targetChannels"), targetPcmFormat.channelCount},
                                     {QStringLiteral("targetBits"), targetPcmFormat.bitsPerSample()},
                                 });
        emit statusMessage(tr("正在重建播放管线…"));
        ++m_activeOutputSwitch.conservativeRebuildCount;
        m_activeOutputSwitch.rebuildTimer.restart();
        startPipeline(m_currentPositionMs, PipelineStartupProfile::ActiveSwitchRebuild);
        m_activeOutputSwitch.sessionId = m_activeDecoderSessionId;
        setActiveOutputSwitchPhase(ActiveOutputSwitchPhase::WaitingForOutputStart,
                                   QStringLiteral("conservative-rebuild"));
        return;
    }

    if (m_playbackState == PlaybackState::Paused) {
        logPreflight(QStringLiteral("paused-rearm"), false);
        setActiveOutputSwitchPhase(ActiveOutputSwitchPhase::Applying, QStringLiteral("apply"));
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("activeOutputSwitch rearm-paused positionMs=%1 previousDevice=%2 targetDevice=%3 formatChanged=%4 trigger=%5")
                              .arg(m_currentPositionMs)
                              .arg(QString::fromLatin1(m_activeOutputDeviceId.toHex()))
                              .arg(QString::fromLatin1(targetDeviceId.toHex()))
                              .arg(formatChanged)
                              .arg(activeOutputSwitchTriggerName(transaction.trigger)));
        emit statusMessage(tr("正在重建播放管线…"));
        teardownPipeline();
        m_startPositionMs = m_currentPositionMs;
        emit positionChanged(m_currentPositionMs);
        setPlaybackState(PlaybackState::Paused);
        resetActiveOutputSwitch(QStringLiteral("paused-rearmed"));
        emit statusMessage(tr("播放管线已重建"));
    }
}

bool WindowsWasapiAudioPlayer::canHotReconfigureOutput(const PcmStreamFormat &deviceFormat) const
{
    Q_UNUSED(deviceFormat);
    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("activeSwitch hot-reconfigure-disabled default=fresh-buffer-restart"));
    return false;
}

void WindowsWasapiAudioPlayer::emitOutputDeviceSelectionChanged()
{
    const QAudioDevice device = resolveOutputDevice();
    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("outputDeviceSelectionChanged usesDefault=%1 id=%2 description=%3")
                          .arg(m_selectedOutputDeviceId.isEmpty())
                          .arg(QString::fromLatin1(device.id().toHex()))
                          .arg(device.description()));
    emit outputDeviceSelectionChanged();
}

QAudioDevice WindowsWasapiAudioPlayer::resolveOutputDevice(bool *usesDefault) const
{
    const QList<QAudioDevice> devices = availableOutputDevices();
    if (!m_selectedOutputDeviceId.isEmpty()) {
        const auto foundIt = std::find_if(devices.cbegin(), devices.cend(), [this](const QAudioDevice &device) {
            return device.id() == m_selectedOutputDeviceId;
        });
        if (foundIt != devices.cend()) {
            if (usesDefault) {
                *usesDefault = false;
            }
            return *foundIt;
        }
    }

    if (usesDefault) {
        *usesDefault = true;
    }
    return QMediaDevices::defaultAudioOutput();
}

QAudioFormat WindowsWasapiAudioPlayer::selectOutputFormat(QString *deviceDescription,
                                                          QByteArray *waveFormatData,
                                                          PcmStreamFormat *pcmFormat) const
{
    bool usesDefault = false;
    const QAudioDevice device = resolveOutputDevice(&usesDefault);
    if (deviceDescription) {
        *deviceDescription = device.isNull() ? tr("默认输出设备")
                                             : (usesDefault
                                                    ? tr("默认输出设备：%1").arg(device.description())
                                                    : device.description());
    }
    if (device.isNull()) {
        return {};
    }

    const ScopedComInitializer comInitializer;
    if (FAILED(comInitializer.result()) && comInitializer.result() != RPC_E_CHANGED_MODE) {
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("selectOutputFormat com-init-failed hr=0x%1")
                              .arg(QString::number(static_cast<qulonglong>(comInitializer.result()), 16)));
        return {};
    }

    IMMDevice *endpoint = nullptr;
    IAudioClient *audioClient = nullptr;
    HRESULT hr = openRenderEndpoint(device, usesDefault, &endpoint, &audioClient);
    if (FAILED(hr)) {
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("selectOutputFormat open-endpoint-failed hr=0x%1 deviceId=%2")
                              .arg(QString::number(static_cast<qulonglong>(hr), 16))
                              .arg(QString::fromLatin1(device.id().toHex())));
        safeRelease(audioClient);
        safeRelease(endpoint);
        return {};
    }

    probeSpatialAudioEndpoint(endpoint, device);

    WAVEFORMATEX *mixFormat = nullptr;
    hr = audioClient->GetMixFormat(&mixFormat);
    if (FAILED(hr) || !mixFormat) {
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("selectOutputFormat get-mix-format-failed hr=0x%1 deviceId=%2")
                              .arg(QString::number(static_cast<qulonglong>(hr), 16))
                              .arg(QString::fromLatin1(device.id().toHex())));
        safeRelease(audioClient);
        safeRelease(endpoint);
        return {};
    }

    const PcmStreamFormat mixPcmFormat = pcmFormatFromWaveFormat(mixFormat);
    const QAudioFormat mixQtFormat = qAudioFormatFromWaveFormat(mixFormat);
    if (!mixPcmFormat.isValid() || !mixQtFormat.isValid()) {
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("selectOutputFormat unsupported-mix-format channels=%1 rate=%2 bits=%3 validBits=%4 tag=%5")
                              .arg(mixFormat->nChannels)
                              .arg(mixFormat->nSamplesPerSec)
                              .arg(mixFormat->wBitsPerSample)
                              .arg(mixPcmFormat.effectiveValidBitsPerSample())
                              .arg(mixFormat->wFormatTag));
        CoTaskMemFree(mixFormat);
        safeRelease(audioClient);
        safeRelease(endpoint);
        return {};
    }

    const bool canUseDirectMixFormat = isDirectMixWaveFormatSupported(mixFormat);
    QAudioFormat selectedFormat;
    QByteArray selectedWaveFormatData;
    PcmStreamFormat selectedPcmFormat;

    QList<int> channelCandidates;
    int preferredChannelCount = mixQtFormat.channelCount();
    const bool exclusiveRequested = m_exclusiveModeEnabled && !m_stabilityModeEnabled;
    if (exclusiveRequested && m_exactPlaybackEnabled && m_sourceChannelCount > 0
        && m_sourceChannelCount != preferredChannelCount) {
        preferredChannelCount = m_sourceChannelCount;
    }
    channelCandidates.append(preferredChannelCount);
    if (exclusiveRequested && m_sourceChannelCount > 0 && m_sourceChannelCount != preferredChannelCount
        && !channelCandidates.contains(m_sourceChannelCount)) {
        channelCandidates.append(m_sourceChannelCount);
    }
    if (m_sourceChannelCount > 0 && m_sourceChannelCount > preferredChannelCount) {
        const int kFallbackChain[] = {8, 6, 4, 2};
        for (const int ch : kFallbackChain) {
            if (ch < m_sourceChannelCount && !channelCandidates.contains(ch)) {
                channelCandidates.append(ch);
            }
        }
    } else {
        if (!channelCandidates.contains(mixQtFormat.channelCount())) {
            channelCandidates.append(mixQtFormat.channelCount());
        }
    }

    const DWORD mixChannelMask = channelMaskFromWaveFormat(mixFormat);
    DWORD preferredChannelMask = channelMaskForCount(preferredChannelCount);
    if (mixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE && preferredChannelCount == mixQtFormat.channelCount()) {
        preferredChannelMask = mixChannelMask;
    }

    const bool exclusiveFound = selectExclusiveFormat(audioClient, device, mixPcmFormat, mixQtFormat,
                                                      channelCandidates, preferredChannelCount,
                                                      preferredChannelMask,
                                                      &selectedFormat, &selectedWaveFormatData,
                                                      &selectedPcmFormat);

    bool sharedFound = false;
    if (!exclusiveFound) {
        sharedFound = selectSharedFormat(audioClient, mixFormat, mixPcmFormat, mixQtFormat,
                                         canUseDirectMixFormat, channelCandidates,
                                         preferredChannelCount, preferredChannelMask,
                                         &selectedFormat, &selectedWaveFormatData,
                                         &selectedPcmFormat);
    }

    if (!exclusiveFound && !sharedFound) {
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("selectOutputFormat no-supported-candidate deviceId=%1 rate=%2 channels=%3 bits=%4 tag=%5")
                              .arg(QString::fromLatin1(device.id().toHex()))
                              .arg(mixFormat->nSamplesPerSec)
                              .arg(mixFormat->nChannels)
                              .arg(mixFormat->wBitsPerSample)
                              .arg(mixFormat->wFormatTag));
        CoTaskMemFree(mixFormat);
        safeRelease(audioClient);
        safeRelease(endpoint);
        return {};
    }

    if (waveFormatData) {
        *waveFormatData = selectedWaveFormatData;
    }
    if (pcmFormat) {
        *pcmFormat = selectedPcmFormat;
    }

    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("selectOutputFormat deviceId=%1 description=%2 rate=%3 channels=%4 sampleFormat=%5 bits=%6 validBits=%7 waveBytes=%8 directMix=%9 exclusive=%10 mixMask=%11 selectedMask=%12 preferredMask=%13 sourceBitDepth=%14 bitDepthMatch=%15")
                          .arg(QString::fromLatin1(device.id().toHex()))
                          .arg(device.description())
                          .arg(selectedFormat.sampleRate())
                          .arg(selectedFormat.channelCount())
                          .arg(static_cast<int>(selectedFormat.sampleFormat()))
                          .arg(selectedPcmFormat.bitsPerSample())
                          .arg(selectedPcmFormat.effectiveValidBitsPerSample())
                          .arg(selectedWaveFormatData.size())
                          .arg(canUseDirectMixFormat)
                          .arg(exclusiveFound)
                          .arg(channelMaskText(mixChannelMask))
                          .arg(channelMaskText(channelMaskFromWaveFormatData(selectedWaveFormatData)))
                          .arg(channelMaskText(preferredChannelMask))
                          .arg(m_sourceBitDepth)
                          .arg(m_sourceBitDepth > 0 && selectedPcmFormat.bitsPerSample() == m_sourceBitDepth
                                   ? QStringLiteral("exact")
                                   : QStringLiteral("fallback")));

    CoTaskMemFree(mixFormat);
    safeRelease(audioClient);
    safeRelease(endpoint);
    return selectedFormat;
}

bool WindowsWasapiAudioPlayer::selectExclusiveFormat(
    IAudioClient *audioClient,
    const QAudioDevice &device,
    const PcmStreamFormat &mixPcmFormat,
    const QAudioFormat &mixQtFormat,
    const QList<int> &channelCandidates,
    int preferredChannelCount,
    unsigned long preferredChannelMask,
    QAudioFormat *outFormat,
    QByteArray *outWaveFormatData,
    PcmStreamFormat *outPcmFormat) const
{
    if (!m_exclusiveModeEnabled || m_stabilityModeEnabled) {
        return false;
    }

    HRESULT lastExclusiveHr = AUDCLNT_E_UNSUPPORTED_FORMAT;
    const QList<PcmStreamFormat> exclusiveCandidates =
        exclusivePcmCandidates(mixPcmFormat,
                               exclusiveSampleRateCandidates(mixQtFormat.sampleRate(), m_sourceSampleRate, m_exactPlaybackEnabled),
                               channelCandidates,
                               m_sourceBitDepth,
                               m_exactPlaybackEnabled);
    for (const PcmStreamFormat &candidate : exclusiveCandidates) {
        DWORD channelMask = candidate.channelCount == preferredChannelCount
            ? static_cast<DWORD>(preferredChannelMask)
            : channelMaskForCount(candidate.channelCount);
        WAVEFORMATEXTENSIBLE candidateWaveFormat = {};
        if (!buildPcmWaveFormat(candidate, channelMask, &candidateWaveFormat)) {
            continue;
        }

        const HRESULT supportHr = audioClient->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                                                 &candidateWaveFormat.Format,
                                                                 nullptr);
        if (supportHr != S_OK) {
            lastExclusiveHr = supportHr;
            continue;
        }

        *outPcmFormat = candidate;
        outFormat->setSampleRate(candidate.sampleRate);
        outFormat->setChannelCount(candidate.channelCount);
        outFormat->setSampleFormat(candidate.qAudioSampleFormat());
        *outWaveFormatData =
            QByteArray(reinterpret_cast<const char *>(&candidateWaveFormat),
                       static_cast<int>(sizeof(candidateWaveFormat)));
        const bool bitDepthExactMatch = m_sourceBitDepth > 0
            && candidate.bitsPerSample() == m_sourceBitDepth;
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("selectOutputFormat exclusive-candidate deviceId=%1 rate=%2 channels=%3 bits=%4 validBits=%5 encoding=%6 waveBytes=%7 sourceBitDepth=%8 bitDepthMatch=%9")
                              .arg(QString::fromLatin1(device.id().toHex()))
                              .arg(candidate.sampleRate)
                              .arg(candidate.channelCount)
                              .arg(candidate.bitsPerSample())
                              .arg(candidate.effectiveValidBitsPerSample())
                              .arg(static_cast<int>(candidate.sampleEncoding))
                              .arg(outWaveFormatData->size())
                              .arg(m_sourceBitDepth)
                              .arg(bitDepthExactMatch ? QStringLiteral("exact") : QStringLiteral("fallback")));
        return true;
    }

    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("selectOutputFormat exclusive-no-supported-candidate deviceId=%1 mixRate=%2 mixChannels=%3 lastHr=0x%4 fallbackShared=1")
                          .arg(QString::fromLatin1(device.id().toHex()))
                          .arg(mixPcmFormat.sampleRate)
                          .arg(mixPcmFormat.channelCount)
                          .arg(QString::number(static_cast<qulonglong>(lastExclusiveHr), 16)));
    return false;
}

bool WindowsWasapiAudioPlayer::selectSharedFormat(
    IAudioClient *audioClient,
    const WAVEFORMATEX *mixFormat,
    const PcmStreamFormat &mixPcmFormat,
    const QAudioFormat &mixQtFormat,
    bool canUseDirectMixFormat,
    const QList<int> &channelCandidates,
    int preferredChannelCount,
    unsigned long preferredChannelMask,
    QAudioFormat *outFormat,
    QByteArray *outWaveFormatData,
    PcmStreamFormat *outPcmFormat) const
{
    if (canUseDirectMixFormat) {
        *outFormat = mixQtFormat;
        *outWaveFormatData = copyWaveFormat(mixFormat);
        *outPcmFormat = mixPcmFormat;
        return true;
    }

    const auto sampleCandidates = candidateSampleFormats(mixFormat, m_sourceBitDepth);
    for (const int channelCount : channelCandidates) {
        DWORD channelMask = channelCount == preferredChannelCount
            ? static_cast<DWORD>(preferredChannelMask)
            : channelMaskForCount(channelCount);
        for (const auto sampleFormat : sampleCandidates) {
            QAudioFormat candidate;
            candidate.setSampleRate(mixQtFormat.sampleRate());
            candidate.setChannelCount(channelCount);
            candidate.setSampleFormat(sampleFormat);
            if (!candidate.isValid()) {
                continue;
            }

            WAVEFORMATEXTENSIBLE candidateWaveFormat = {};
            if (!buildWaveFormat(candidate, channelMask, &candidateWaveFormat)) {
                continue;
            }

            WAVEFORMATEX *closestMatch = nullptr;
            const HRESULT supportHr = audioClient->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED,
                                                                     &candidateWaveFormat.Format,
                                                                     &closestMatch);
            if (closestMatch) {
                CoTaskMemFree(closestMatch);
            }
            if (supportHr == S_OK) {
                *outFormat = candidate;
                *outWaveFormatData =
                    QByteArray(reinterpret_cast<const char *>(&candidateWaveFormat),
                               static_cast<int>(sizeof(candidateWaveFormat)));
                *outPcmFormat = pcmFormatFromQAudioFormat(candidate);
                return true;
            }
        }
    }

    return false;
}
