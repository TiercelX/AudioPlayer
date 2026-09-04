#ifndef WINDOWSWASAPIAUDIOPLAYER_WORKER_HELPERS_H
#define WINDOWSWASAPIAUDIOPLAYER_WORKER_HELPERS_H

// Internal helper functions, constants, and structs for the WASAPI render worker.
// Extracted from windowswasiaudioplayer_worker.h to reduce header size.

#include "audioplayerbackend.h"
#include "audioartifactmonitor.h"
#include "audioutils.h"
#include "ffmpegpcmshared.h"
#include "playerlogger.h"

#include <QAudioDevice>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMediaDevices>
#include <QProcess>
#include <QProcessEnvironment>
#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QThread>
#include <QTimer>
#include <QVarLengthArray>
#include <QWinEventNotifier>
#include <QtEndian>

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <audioclient.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <objbase.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

constexpr UINT32 kRecoveryStartupSilenceMs = 48;
constexpr UINT32 kHotReconfigureStartupSilenceMs = 16;
constexpr UINT32 kActiveSwitchRebuildStartupSilenceMs = 8;
constexpr UINT32 kRecoveryWarmupSilenceMs = 32;
constexpr UINT32 kSeekResumeStartupSilenceMs = 8;
constexpr UINT32 kSeekResumeWarmupDiscardMs = 8;
constexpr int kDeferredFadeInDelayMs = 24;
constexpr int kSeekResumeStreamFadeInDelayMs = 0;
constexpr double kSeekResumeFfmpegInitialBurstSeconds = 1.5;
constexpr int kOutputDeviceChangeDebounceMs = 180;
constexpr qint64 kRecoveryStablePositionAdvanceMs = 120;
constexpr int kPcmFadeInDurationMs = 32;
constexpr int kSeekResumePcmFadeInDurationMs = 24;
constexpr int kPcmFadeOutDurationMs = 80;
// Exclusive event-driven render starts need a complete endpoint buffer.
constexpr int kSeekResumeStartupThresholdMs = 100;
constexpr int kStabilityModeSeekResumeStartupThresholdMs = 200;
constexpr int kActiveSwitchRebuildPcmFadeInDurationMs = kPcmFadeInDurationMs;
constexpr double kActiveSwitchFirstBlockMaxFadeGain = 0.50;
constexpr UINT32 kActiveSwitchPostInvalidationStartupSilenceMs = kActiveSwitchRebuildStartupSilenceMs;
constexpr int kActiveSwitchPostInvalidationPcmFadeInDurationMs = kActiveSwitchRebuildPcmFadeInDurationMs;
constexpr double kActiveSwitchPostInvalidationFirstBlockMaxFadeGain = kActiveSwitchFirstBlockMaxFadeGain;
constexpr float kActiveSwitchInvalidationTaperGain = 0.20f;
constexpr int kActiveSwitchInvalidationTaperHoldMs = 24;
constexpr float kActiveSwitchEntryBridgeStreamGain = 1.0f;
constexpr int kRenderMirrorWindowMs = 1000;
constexpr int kSubmittedTailWindowMs = 500;
constexpr int kDefaultSpatialEndpointFlushMs = 200;
constexpr int kDefaultSpatialEndpointSettleMs = 150;
constexpr auto kWasapiLibavDecoderEnv = "AUDIOPLAYER_WASAPI_LIBAV_DECODER";
constexpr auto kLegacyWasapiLibavSeekResumeEnv = "AUDIOPLAYER_WASAPI_LIBAV_SEEK_RESUME";
constexpr auto kWasapiRenderMirrorWindowMsEnv = "AUDIOPLAYER_WASAPI_RENDER_MIRROR_WINDOW_MS";
constexpr auto kWasapiExclusiveEnv = "AUDIOPLAYER_WASAPI_EXCLUSIVE";
constexpr auto kWasapiCreativeChannelReorderEnv = "AUDIOPLAYER_WASAPI_CREATIVE_CHANNEL_REORDER";
constexpr auto kWasapiSpatialStaticBedEnv = "AUDIOPLAYER_WASAPI_SPATIAL_STATIC_BED";
constexpr REFERENCE_TIME kExclusiveBufferDuration = 1000000; // 0.1s in 100-ns units
constexpr int kStabilityModeOutputBufferMs = 500;

struct ActiveSwitchBoundaryPolicy
{
    QString name = QStringLiteral("shared-rebuild");
    UINT32 startupSilenceMs = kActiveSwitchRebuildStartupSilenceMs;
    int pcmFadeInDurationMs = kActiveSwitchRebuildPcmFadeInDurationMs;
    double firstBlockMaxFadeGain = kActiveSwitchFirstBlockMaxFadeGain;
    float entryBridgeStreamGain = kActiveSwitchEntryBridgeStreamGain;
    int currentSampleRate = 0;
    int targetSampleRate = 0;
};

struct WasapiArtifactTrackingConfig
{
    bool enabled = false;
    QString source;
    QString previousSource;
    QString playbackState;
    QString recentControlEvent;
    QString pipelineStartupProfile;
    QString startupObservationProfile;
    QString artifactPath;
    QString activeSwitchTrigger;
    QString activeSwitchPhase;
    QString activeSwitchReason;
    QString activeSwitchBoundaryPolicy;
    QString selectedOutputDeviceId;
    QString appStartTimeUtc;
    qint64 startPositionMs = 0;
    qint64 seekRequestTimeMs = -1;
    qint64 pipelineStartTimeMs = -1;
    quint64 bufferGeneration = 0;
    int recoveryAttempt = 0;
    int seekResumeStartupSilenceMs = 0;
    int seekResumeWarmupDiscardMs = 0;
    int seekResumeFadeInMs = 0;
    bool realtimeDecodeEnabled = false;
};

struct RenderedBlockMetrics
{
    bool valid = false;
    int sessionId = 0;
    qsizetype frameCount = 0;
    double peak = 0.0;
    double rms = 0.0;
    double jump = 0.0;
    double firstSample = 0.0;
    double lastSample = 0.0;
    double firstSamplePeak = 0.0;
    double lastSamplePeak = 0.0;
};

struct PcmFadeApplication
{
    bool applied = false;
    qsizetype frames = 0;
    qsizetype totalFrames = 0;
    qsizetype framesProcessedBefore = 0;
    qsizetype framesProcessedAfter = 0;
    double minGain = 1.0;
    double maxGain = 1.0;
};

struct NoiseShaperState {
    double error[2] = {0.0, 0.0};
    uint32_t rng = 1;
};

namespace {

bool envFlagDisabled(const QString &value)
{
    const QString trimmed = value.trimmed();
    return trimmed == QStringLiteral("0")
        || trimmed.compare(QStringLiteral("false"), Qt::CaseInsensitive) == 0
        || trimmed.compare(QStringLiteral("off"), Qt::CaseInsensitive) == 0;
}

bool isCreativeG5WasapiDeviceDescription(const QString &description)
{
    const bool isCreative = description.contains(QStringLiteral("Sound Blaster"), Qt::CaseInsensitive)
        || description.contains(QStringLiteral("Creative"), Qt::CaseInsensitive);
    return isCreative
        && (description.contains(QStringLiteral("G5"), Qt::CaseInsensitive)
            || description.contains(QStringLiteral("G6"), Qt::CaseInsensitive));
}

bool creativeWasapiChannelOrderWorkaroundEnabled()
{
    const QString value = qEnvironmentVariable(kWasapiCreativeChannelReorderEnv).trimmed();
    return value.isEmpty() || !envFlagDisabled(value);
}

enum class CreativeChannelReorderMode {
    Auto,
    Off,
    ForceCreative,
};

QString creativeWasapiChannelOrderFilter(const QAudioDevice &device,
                                         const QString &channelLayout,
                                         int channelCount,
                                         int reorderMode)
{
    const auto mode = static_cast<CreativeChannelReorderMode>(reorderMode);
    if (channelLayout.isEmpty()) {
        return {};
    }
    if (mode == CreativeChannelReorderMode::Off) {
        return {};
    }
    if (mode == CreativeChannelReorderMode::Auto) {
        return {};
    }

    if (channelCount == 8) {
        return QStringLiteral("pan=%1|c0=c0|c1=c1|c2=c4|c3=c5|c4=c2|c5=c3|c6=c6|c7=c7")
            .arg(channelLayout);
    }
    if (channelCount == 6) {
        return QStringLiteral("pan=%1|c0=c0|c1=c1|c2=c4|c3=c5|c4=c2|c5=c3")
            .arg(channelLayout);
    }
    return {};
}

DWORD channelMaskFromWaveFormatData(const QByteArray &waveFormatData)
{
    if (waveFormatData.size() < static_cast<int>(sizeof(WAVEFORMATEX))) {
        return 0;
    }
    const auto *waveFormat = reinterpret_cast<const WAVEFORMATEX *>(waveFormatData.constData());
    if (waveFormatData.size() < static_cast<int>(sizeof(WAVEFORMATEX) + waveFormat->cbSize)
        || waveFormat->wFormatTag != WAVE_FORMAT_EXTENSIBLE
        || waveFormat->cbSize < 22) {
        return 0;
    }
    return reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(waveFormat)->dwChannelMask;
}

QString channelLayoutForCountFallback(int channelCount)
{
    switch (channelCount) {
    case 1:
        return QStringLiteral("mono");
    case 2:
        return QStringLiteral("stereo");
    case 3:
        return QStringLiteral("2.1");
    case 4:
        return QStringLiteral("quad");
    case 5:
        return QStringLiteral("4.1");
    case 6:
        return QStringLiteral("5.1");
    case 7:
        return QStringLiteral("6.1");
    case 8:
        return QStringLiteral("7.1");
    default:
        return {};
    }
}

QString channelLayoutForMask(DWORD channelMask, int channelCount)
{
    constexpr DWORD mono = SPEAKER_FRONT_CENTER;
    constexpr DWORD stereo = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    constexpr DWORD twoOne = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_LOW_FREQUENCY;
    constexpr DWORD quad = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT
        | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
    constexpr DWORD fourOne = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_LOW_FREQUENCY
        | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
    constexpr DWORD fiveOneBack = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER
        | SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
    constexpr DWORD fiveOneSide = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER
        | SPEAKER_LOW_FREQUENCY | SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT;
    constexpr DWORD sixOne = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER
        | SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_CENTER
        | SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT;
    constexpr DWORD sevenOne = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER
        | SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT
        | SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT;
    constexpr DWORD sevenOneWide = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER
        | SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT
        | SPEAKER_FRONT_LEFT_OF_CENTER | SPEAKER_FRONT_RIGHT_OF_CENTER;
    constexpr DWORD sevenOneWideSide = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER
        | SPEAKER_LOW_FREQUENCY | SPEAKER_FRONT_LEFT_OF_CENTER | SPEAKER_FRONT_RIGHT_OF_CENTER
        | SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT;

    if (channelMask == 0) {
        return channelLayoutForCountFallback(channelCount);
    }
    if (channelMask == mono) {
        return QStringLiteral("mono");
    }
    if (channelMask == stereo) {
        return QStringLiteral("stereo");
    }
    if (channelMask == twoOne) {
        return QStringLiteral("2.1");
    }
    if (channelMask == quad) {
        return QStringLiteral("quad");
    }
    if (channelMask == fourOne) {
        return QStringLiteral("4.1");
    }
    if (channelMask == fiveOneBack) {
        return QStringLiteral("5.1");
    }
    if (channelMask == fiveOneSide) {
        return QStringLiteral("5.1(side)");
    }
    if (channelMask == sixOne) {
        return QStringLiteral("6.1");
    }
    if (channelMask == sevenOne) {
        return QStringLiteral("7.1");
    }
    if (channelMask == sevenOneWide) {
        return QStringLiteral("7.1(wide)");
    }
    if (channelMask == sevenOneWideSide) {
        return QStringLiteral("7.1(wide-side)");
    }
    return channelLayoutForCountFallback(channelCount);
}

QString channelLayoutForWaveFormatData(const QByteArray &waveFormatData, int channelCount)
{
    return channelLayoutForMask(channelMaskFromWaveFormatData(waveFormatData), channelCount);
}

QString channelMaskText(DWORD channelMask)
{
    return QStringLiteral("0x%1").arg(static_cast<qulonglong>(channelMask),
                                      8,
                                      16,
                                      QLatin1Char('0'));
}

int boundedEnvInt(const char *name, int defaultValue, int minValue, int maxValue)
{
    const QString rawValue = QProcessEnvironment::systemEnvironment()
        .value(QString::fromLatin1(name))
        .trimmed();
    if (rawValue.isEmpty()) {
        return defaultValue;
    }

    bool ok = false;
    const int parsed = rawValue.toInt(&ok);
    if (!ok) {
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("envInt invalid name=%1 value=%2 default=%3")
                              .arg(QString::fromLatin1(name), rawValue)
                              .arg(defaultValue));
        return defaultValue;
    }

    return qBound(minValue, parsed, maxValue);
}

bool wasapiLibavDecoderDisabled()
{
    const QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    const QString generalSetting =
        environment.value(QString::fromLatin1(kWasapiLibavDecoderEnv)).trimmed();
    if (!generalSetting.isEmpty() && envFlagDisabled(generalSetting)) {
        return true;
    }

    const QString legacySeekSetting =
        environment.value(QString::fromLatin1(kLegacyWasapiLibavSeekResumeEnv)).trimmed();
    return !legacySeekSetting.isEmpty() && envFlagDisabled(legacySeekSetting);
}

REFERENCE_TIME bufferDurationForBytes(qsizetype bufferSizeBytes, const PcmStreamFormat &format)
{
    if (!format.isValid() || bufferSizeBytes <= 0 || format.bytesPerFrame() <= 0 || format.sampleRate <= 0) {
        return 0;
    }

    const qint64 frames = static_cast<qint64>(bufferSizeBytes) / format.bytesPerFrame();
    return static_cast<REFERENCE_TIME>(frames * 10000000LL / format.sampleRate);
}

QString pcmEncodingName(PcmSampleEncoding encoding)
{
    switch (encoding) {
    case PcmSampleEncoding::UInt8:
        return QStringLiteral("uint8");
    case PcmSampleEncoding::Int16:
        return QStringLiteral("int16");
    case PcmSampleEncoding::Int24:
        return QStringLiteral("int24");
    case PcmSampleEncoding::Int32:
        return QStringLiteral("int32");
    case PcmSampleEncoding::Float32:
        return QStringLiteral("float32");
    case PcmSampleEncoding::Unknown:
        break;
    }

    return QStringLiteral("unknown");
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

ActiveSwitchBoundaryPolicy activeSwitchBoundaryPolicyForOutputFormats(const PcmStreamFormat &currentFormat,
                                                                      const PcmStreamFormat &targetFormat,
                                                                      bool postInvalidationRebuild)
{
    ActiveSwitchBoundaryPolicy policy;
    policy.currentSampleRate = currentFormat.isValid() ? currentFormat.sampleRate : 0;
    policy.targetSampleRate = targetFormat.isValid() ? targetFormat.sampleRate : 0;

    if (postInvalidationRebuild) {
        policy.name = QStringLiteral("post-invalidation-rebuild");
        policy.startupSilenceMs = kActiveSwitchPostInvalidationStartupSilenceMs;
        policy.pcmFadeInDurationMs = kActiveSwitchPostInvalidationPcmFadeInDurationMs;
        policy.firstBlockMaxFadeGain = kActiveSwitchPostInvalidationFirstBlockMaxFadeGain;
    }

    return policy;
}

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

qint32 readInt24Sample(const char *sampleData)
{
    const quint32 b0 = static_cast<quint8>(sampleData[0]);
    const quint32 b1 = static_cast<quint8>(sampleData[1]);
    const quint32 b2 = static_cast<quint8>(sampleData[2]);
    quint32 value = b0 | (b1 << 8) | (b2 << 16);
    if (value & 0x00800000u) {
        value |= 0xFF000000u;
    }
    return static_cast<qint32>(value);
}

void writeInt24Sample(qint32 sample, char *sampleData)
{
    const qint32 clamped = qBound<qint32>(-8388608, sample, 8388607);
    sampleData[0] = static_cast<char>(clamped & 0xFF);
    sampleData[1] = static_cast<char>((clamped >> 8) & 0xFF);
    sampleData[2] = static_cast<char>((clamped >> 16) & 0xFF);
}

inline int noiseShaperFastDither(NoiseShaperState &state)
{
    uint32_t x = state.rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    state.rng = x;
    return static_cast<int>(x & 1u) - static_cast<int>((x >> 1) & 1u);
}

inline qint32 noiseShapedQuantize32To24(qint32 sample, NoiseShaperState &state)
{
    constexpr double lsb = 256.0;
    const double dither = static_cast<double>(noiseShaperFastDither(state));
    const double shaped = sample + dither * lsb
        + 2.0 * state.error[0] - state.error[1];
    const double quantized = std::round(shaped / lsb) * lsb;
    state.error[1] = state.error[0];
    state.error[0] = shaped - quantized;
    const qint64 quantized24 =
        qBound<qint64>(static_cast<qint64>(-8388608),
                       static_cast<qint64>(std::round(quantized / lsb)),
                       static_cast<qint64>(8388607));
    return static_cast<qint32>(quantized24 * 256);
}

inline qint32 roundedQuantize32To24(qint32 sample)
{
    const qint64 rounded = sample >= 0
        ? static_cast<qint64>(sample) + 128
        : static_cast<qint64>(sample) - 128;
    const qint64 quantized24 =
        qBound<qint64>(static_cast<qint64>(-8388608),
                       rounded / 256,
                       static_cast<qint64>(8388607));
    return static_cast<qint32>(quantized24 * 256);
}

inline qint16 noiseShapedQuantize32To16(qint32 sample, NoiseShaperState &state)
{
    constexpr double lsb = 65536.0;
    const double dither = static_cast<double>(noiseShaperFastDither(state));
    const double shaped = sample + dither * lsb
        + 2.0 * state.error[0] - state.error[1];
    const double quantized = std::round(shaped / lsb) * lsb;
    state.error[1] = state.error[0];
    state.error[0] = shaped - quantized;
    return static_cast<qint16>(
        qBound<qint64>(static_cast<qint64>(-32768),
                        static_cast<qint64>(std::round(quantized)) >> 16,
                        static_cast<qint64>(32767)));
}

bool hasSamePcmLayout(const PcmStreamFormat &lhs, const PcmStreamFormat &rhs)
{
    return lhs.isValid()
        && rhs.isValid()
        && lhs.sampleEncoding == rhs.sampleEncoding
        && lhs.channelCount == rhs.channelCount
        && lhs.bytesPerFrame() == rhs.bytesPerFrame()
        && lhs.effectiveValidBitsPerSample() == rhs.effectiveValidBitsPerSample();
}

bool buildWaveFormat(const QAudioFormat &format, WAVEFORMATEXTENSIBLE *waveFormat)
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
    result.dwChannelMask = channelMaskForCount(format.channelCount());
    result.SubFormat = subFormat;
    *waveFormat = result;
    return true;
}

QtAudio::Error mapWasapiError(HRESULT hr, bool openingStage)
{
    switch (hr) {
    case AUDCLNT_E_UNSUPPORTED_FORMAT:
    case AUDCLNT_E_DEVICE_IN_USE:
    case AUDCLNT_E_SERVICE_NOT_RUNNING:
        return QtAudio::OpenError;
    case AUDCLNT_E_DEVICE_INVALIDATED:
    case AUDCLNT_E_RESOURCES_INVALIDATED:
    case AUDCLNT_E_ENDPOINT_CREATE_FAILED:
        return QtAudio::IOError;
    default:
        return openingStage ? QtAudio::OpenError : QtAudio::FatalError;
    }
}

} // namespace

#endif // WINDOWSWASAPIAUDIOPLAYER_WORKER_HELPERS_H
