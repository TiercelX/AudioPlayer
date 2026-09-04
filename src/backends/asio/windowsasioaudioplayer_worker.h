#ifndef WINDOWSASIOAUDIOPLAYER_WORKER_H
#define WINDOWSASIOAUDIOPLAYER_WORKER_H

#include "asio_interface.h"
#include "audioartifactmonitor.h"
#include "ffmpegpcmshared.h"
#include "playerlogger.h"
#include "windowsasioaudioplayer_discovery.h"
#include "windowsasioaudioplayer_formats.h"
#include "windowsasioaudioplayer_sessionprobe.h"
#include "windowsasioaudioplayer_utils.h"

#include <QAudioFormat>
#include <QByteArray>
#include <QElapsedTimer>
#include <QMutex>
#include <QObject>
#include <QRandomGenerator>
#include <QString>
#include <QThread>
#include <QTimer>
#include <QVarLengthArray>
#include <QVector>
#include <QtEndian>
#include <QtMultimedia/qaudio.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <objbase.h>
#include <windows.h>
#include <audioclient.h>

class AsioOutputWorker;
class PcmStreamBuffer;

extern AsioOutputWorker *g_callbackWorker;
extern QMutex g_callbackWorkerMutex;

void asioBufferSwitch(long doubleBufferIndex, ASIOBool directProcess);
void asioSampleRateDidChange(ASIOSampleRate sampleRate);
long asioMessage(long selector, long value, void *message, double *opt);
ASIOTime *asioBufferSwitchTimeInfo(ASIOTime *params, long doubleBufferIndex, ASIOBool directProcess);

constexpr long kAsioFalse = 0;
constexpr long kAsioTrue = 1;
constexpr long kAsioSelectorSupported = 1;
constexpr long kAsioEngineVersion = 2;
constexpr long kAsioResetRequest = 3;
constexpr long kAsioBufferSizeChange = 4;
constexpr long kAsioResyncRequest = 5;
constexpr long kAsioLatenciesChanged = 6;
constexpr long kAsioSupportsTimeInfo = 7;
constexpr long kAsioSupportsTimeCode = 8;
constexpr int kAsioOpenRetryCount = 5;
constexpr int kAsioOpenRetryBaseDelayMs = 160;
constexpr int kAsioReleaseCooldownMs = 300;
constexpr int kAsioBusyRetryTimeoutMs = 15000;
constexpr int kAsioBusyRetryIntervalMs = 500;

namespace AsioWorker {

struct NoiseShaperState {
    double error[2] = {0.0, 0.0};
    uint32_t rng = 1;
};

inline int noiseShaperFastDither(NoiseShaperState &state)
{
    uint32_t x = state.rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    state.rng = x;
    return static_cast<int>(x & 1u) - static_cast<int>((x >> 1) & 1u);
}

inline qint32 noiseShapedQuantize32(qint32 sample, int shift, NoiseShaperState &state)
{
    const double lsb = static_cast<double>(1 << shift);
    const double dither = static_cast<double>(noiseShaperFastDither(state));
    const double shaped = sample + dither * lsb
        + 2.0 * state.error[0] - state.error[1];
    const double quantized = std::round(shaped / lsb) * lsb;
    state.error[1] = state.error[0];
    state.error[0] = shaped - quantized;
    return static_cast<qint32>(
        qBound<qint64>(static_cast<qint64>(std::numeric_limits<qint32>::min()),
                        static_cast<qint64>(std::round(quantized)),
                        static_cast<qint64>(std::numeric_limits<qint32>::max())));
}

} // namespace AsioWorker

class AsioOutputWorker : public QObject
{
    Q_OBJECT

public:
    enum class DriverOpenFailureReason { None, DeviceBusy, DriverError, RecoveryTimeout };
    Q_ENUM(DriverOpenFailureReason)

public:
    explicit AsioOutputWorker(QObject *parent = nullptr);
    ~AsioOutputWorker() override;

    void configureOutput(int sessionId,
                         const QByteArray &driverId,
                         const QAudioFormat &format,
                         qreal volume,
                         qint64 startPositionMs,
                         int sourceBitDepth = 0,
                         const QAudioFormat &decoderFormat = {});

    bool prepareOutput(int sessionId);
    QAudioFormat preparedOutputFormat() const;

    void startOutput(int sessionId, PcmStreamBuffer *buffer);
    void finishOutput(int sessionId);
    void pauseOutput(int sessionId, int generation);
    void resumeOutput(int sessionId, int generation);
    void forceReleaseDriver();

    void startRecoveryStatus();
    qint64 recoveryElapsedMs() const;
    void emitRecoveryStatus();
    void resetRecoveryStatus();

    void handleCallbackWatchdogRecovery();
    void handleDriverResetRequest();
    void releaseOutput(int sessionId, bool);

    DriverOpenFailureReason lastOpenFailureReason() const;
    QString lastOpenFailureDetail() const;
    void notifyResetRequest();
    void setVolume(qreal volume);

    void renderCallback(long doubleBufferIndex)
    {
        QMutexLocker locker(&m_renderMutex);
        const long callbackCount = ++m_callbackCount;
        if (callbackCount == 1) {
            m_callbackWatchdog->stop();
            const int sessionId = m_sessionId;
            PlayerLogger::log(QStringLiteral("audio"),
                              QStringLiteral("asio firstBufferSwitch session=%1 bufferIndex=%2")
                                  .arg(sessionId)
                                  .arg(doubleBufferIndex));
            if (m_recoveryPendingFirstBufferSwitch) {
                PlayerLogger::log(QStringLiteral("audio"),
                                  QStringLiteral("asio recovery-confirmed-first-buffer session=%1 elapsed=%2ms")
                                      .arg(sessionId)
                                      .arg(recoveryElapsedMs()));
                resetRecoveryStatus();
                emit statusMessage(tr("ASIO \346\222\255\346\224\276\345\267\262\346\201\242\345\244\215"));
            } else {
                resetRecoveryStatus();
            }
            QMetaObject::invokeMethod(this,
                                      [this, sessionId] {
                                          emit firstBufferSwitchReceived(sessionId);
                                      },
                                      Qt::QueuedConnection);
        }
        if (!m_started || !m_buffer || !m_outputFormat.isValid() || m_bufferInfos.isEmpty()) {
            clearAsioBuffers(doubleBufferIndex);
            return;
        }

        const int bytesPerFrame = m_outputFormat.bytesPerFrame();
        if (bytesPerFrame <= 0 || m_bufferSize <= 0) {
            clearAsioBuffers(doubleBufferIndex);
            return;
        }

        QByteArray chunk = m_buffer->read(static_cast<qint64>(m_bufferSize * bytesPerFrame));
        const qsizetype fullSize = static_cast<qsizetype>(m_bufferSize * bytesPerFrame);
        if (chunk.size() < fullSize) {
            chunk.append(fullSize - chunk.size(), '\0');
            if (m_buffer->endOfStream() && !m_completionPosted) {
                m_completionPosted = true;
                QMetaObject::invokeMethod(this, [this, sessionId = m_sessionId] {
                    emit stateChanged(sessionId, static_cast<int>(QAudio::IdleState), static_cast<int>(QtAudio::NoError));
                }, Qt::QueuedConnection);
            }
        }

        const int channelCount = m_outputFormat.channelCount();
        const int bytesPerSample = m_outputFormat.bytesPerSample();
        const qreal volume = m_volume;
        const char *input = chunk.constData();
        for (int channel = 0; channel < channelCount && channel < m_bufferInfos.size(); ++channel) {
            void *output = m_bufferInfos[channel].buffers[doubleBufferIndex];
            if (!output) {
                continue;
            }
            AsioWorker::NoiseShaperState shaperState;
            if (channel < m_noiseShaperState.size()) {
                shaperState = m_noiseShaperState[channel];
            }
            writeChannel(output,
                         input,
                         channel,
                         channelCount,
                         bytesPerSample,
                         static_cast<AsioUtils::AsioSampleType>(m_channelInfos[channel].type),
                         volume,
                         shaperState);
            if (channel < m_noiseShaperState.size()) {
                m_noiseShaperState[channel] = shaperState;
            }
        }

        if (m_artifactMonitorActive && !chunk.isEmpty() && m_outputFormat.isValid()) {
            const PcmStreamFormat pcmFormat = AsioFormats::pcmStreamFormatFromQAudioFormat(m_outputFormat);
            AudioArtifactMonitor::PlaybackContext playbackContext;
            playbackContext.sessionId = m_sessionId;
            playbackContext.playbackState = QStringLiteral("Active");
            AudioArtifactMonitor::RenderContext renderContext;
            renderContext.firstDataBlockAfterConfigure = (callbackCount == 1);
            m_artifactMonitor.analyzePcmBlock(chunk.constData(),
                                              chunk.size(),
                                              pcmFormat,
                                              playbackContext,
                                              QStringLiteral("asio-render"),
                                              renderContext);
            ++m_renderMirrorBlockCount;
            updateRenderMirrorWindow();
            if (m_renderMirrorWindowBlocks > 0
                && m_renderMirrorBlockCount >= m_renderMirrorWindowBlocks) {
                finishArtifactMonitoring();
            }
        }

        m_renderedFrames += m_bufferSize;
        if (m_driver) {
            bool asioCrashed = false;
            const ASIOError readyResult = AsioUtils::safeAsioOutputReady(m_driver, &asioCrashed);
            if (asioCrashed || (!AsioUtils::asioResultOk(readyResult) && readyResult != AsioUtils::kAsioNotPresent)) {
                PlayerLogger::log(QStringLiteral("audio"),
                                  QStringLiteral("asio outputReady failed session=%1 result=%2 crashed=%3")
                                      .arg(m_sessionId)
                                      .arg(readyResult)
                                      .arg(asioCrashed ? 1 : 0));
            }
        }
    }

signals:
    void firstBufferSwitchReceived(int sessionId);
    void positionUpdated(int sessionId, qint64 processedPositionMs);
    void released(int sessionId);
    void stateChanged(int sessionId, int state, int error);
    void statusMessage(const QString &message);

private:
    DriverOpenFailureReason classifyEndpointOpenFailure() const;
    bool openDriver();

    void clearAsioBuffers(long doubleBufferIndex)
    {
        if (doubleBufferIndex < 0 || doubleBufferIndex > 1) {
            return;
        }
        for (int channel = 0; channel < m_bufferInfos.size(); ++channel) {
            void *output = m_bufferInfos[channel].buffers[doubleBufferIndex];
            if (output) {
                std::memset(output, 0, static_cast<size_t>(AsioUtils::sampleTypeBytes(static_cast<AsioUtils::AsioSampleType>(m_channelInfos[channel].type)) * m_bufferSize));
            }
        }
    }

    qreal normalizedSample(const char *sampleData) const
    {
        const QAudioFormat &readFormat = m_decoderFormat.isValid() ? m_decoderFormat : m_outputFormat;
        switch (readFormat.sampleFormat()) {
        case QAudioFormat::Float: {
            float value = 0.0f;
            std::memcpy(&value, sampleData, sizeof(value));
            return qBound<qreal>(-1.0, value, 1.0);
        }
        case QAudioFormat::Int32: {
            const qint32 value = qFromLittleEndian<qint32>(sampleData);
            return qBound<qreal>(-1.0, static_cast<qreal>(value) / 2147483648.0, 1.0);
        }
        case QAudioFormat::Int16: {
            const qint16 value = qFromLittleEndian<qint16>(sampleData);
            return qBound<qreal>(-1.0, static_cast<qreal>(value) / 32768.0, 1.0);
        }
        case QAudioFormat::UInt8: {
            quint8 value = 0;
            std::memcpy(&value, sampleData, sizeof(value));
            return qBound<qreal>(-1.0, static_cast<qreal>(static_cast<int>(value) - 128) / 127.0, 1.0);
        }
        case QAudioFormat::Unknown:
        case QAudioFormat::NSampleFormats:
            break;
        }
        return 0.0;
    }

    void writeChannel(void *output,
                      const char *input,
                      int channel,
                      int channelCount,
                      int bytesPerSample,
                      AsioUtils::AsioSampleType sampleType,
                      qreal volume,
                      AsioWorker::NoiseShaperState &noiseShaperState)
    {
        const bool applyNoiseShaping = m_sourceBitDepth > 0 && m_sourceBitDepth < 32;
        char *outputBytes = static_cast<char *>(output);
        for (long frame = 0; frame < m_bufferSize; ++frame) {
            const char *sampleData = input + (frame * channelCount + channel) * bytesPerSample;
            const qreal value = qBound<qreal>(-1.0, normalizedSample(sampleData) * volume, 1.0);
            switch (sampleType) {
            case AsioUtils::ASIOSTFloat32LSB: {
                const float floatValue = static_cast<float>(value);
                std::memcpy(outputBytes + frame * 4, &floatValue, sizeof(floatValue));
                break;
            }
            case AsioUtils::ASIOSTInt16LSB: {
                qint32 raw = static_cast<qint32>(value * 32767.0);
                if (applyNoiseShaping) {
                    raw = AsioWorker::noiseShapedQuantize32(raw << 16, 16, noiseShaperState) >> 16;
                }
                const qint16 intValue = static_cast<qint16>(qBound<int>(-32768, raw, 32767));
                qToLittleEndian<qint16>(intValue, outputBytes + frame * 2);
                break;
            }
            case AsioUtils::ASIOSTInt24LSB: {
                qint32 raw = static_cast<qint32>(value * 8388607.0);
                if (applyNoiseShaping) {
                    raw = AsioWorker::noiseShapedQuantize32(raw << 8, 8, noiseShaperState) >> 8;
                }
                const qint32 intValue = qBound<qint32>(-8388608, raw, 8388607);
                char *target = outputBytes + frame * 3;
                target[0] = static_cast<char>(intValue & 0xff);
                target[1] = static_cast<char>((intValue >> 8) & 0xff);
                target[2] = static_cast<char>((intValue >> 16) & 0xff);
                break;
            }
            case AsioUtils::ASIOSTInt32LSB:
            case AsioUtils::ASIOSTInt32LSB16:
            case AsioUtils::ASIOSTInt32LSB18:
            case AsioUtils::ASIOSTInt32LSB20:
            case AsioUtils::ASIOSTInt32LSB24:
            default: {
                qint32 intValue = qBound<qint32>((std::numeric_limits<qint32>::min)(),
                                                  static_cast<qint32>(qRound64(value * 2147483647.0)),
                                                  (std::numeric_limits<qint32>::max)());
                if (applyNoiseShaping && sampleType == AsioUtils::ASIOSTInt32LSB24) {
                    intValue = AsioWorker::noiseShapedQuantize32(intValue, 8, noiseShaperState);
                }
                qToLittleEndian<qint32>(intValue, outputBytes + frame * 4);
                break;
            }
            }
        }
    }

    void startArtifactMonitoring()
    {
        m_artifactMonitor.resetContinuity(QStringLiteral("asio-render-start"));
        m_artifactMonitorActive = true;
        m_artifactCountAtSessionStart = m_artifactMonitor.artifactCountTotal();
        m_renderMirrorBlockCount = 0;
        m_renderMirrorWindowBlocks = 0;
    }

    void updateRenderMirrorWindow()
    {
        if (m_renderMirrorWindowBlocks > 0 || !m_artifactMonitorActive) {
            return;
        }
        if (m_bufferSize > 0 && m_outputFormat.isValid() && m_outputFormat.sampleRate() > 0) {
            const int windowMs = AsioUtils::boundedEnvInt(QStringLiteral("AUDIOPLAYER_ASIO_RENDER_MIRROR_WINDOW_MS"),
                                               1000, 100, 30000);
            const qint64 windowFrames = static_cast<qint64>(m_outputFormat.sampleRate()) * windowMs / 1000;
            m_renderMirrorWindowBlocks = static_cast<quint64>(
                qMax<qint64>(1, (windowFrames + m_bufferSize - 1) / m_bufferSize));
        }
    }

    void finishArtifactMonitoring()
    {
        if (!m_artifactMonitorActive) {
            return;
        }

        const quint64 artifactCount =
            m_artifactMonitor.artifactCountTotal() - m_artifactCountAtSessionStart;
        const bool artifactDetected = artifactCount > 0;
        const QString conclusion = artifactDetected
            ? QStringLiteral("submitted PCM artifact detected")
            : QStringLiteral("submitted PCM clean");

        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("asioRenderMirrorConclusion session=%1 result=\"%2\" artifactDetected=%3 artifactCount=%4 blocks=%5")
                              .arg(m_sessionId)
                              .arg(conclusion)
                              .arg(artifactDetected ? 1 : 0)
                              .arg(artifactCount)
                              .arg(m_renderMirrorBlockCount));
        PlayerLogger::diagnostic(QStringLiteral("audio"),
                                 QStringLiteral("asio_render_mirror_conclusion"),
                                 {
                                     {QStringLiteral("sessionId"), m_sessionId},
                                     {QStringLiteral("result"), conclusion},
                                     {QStringLiteral("artifactDetected"), artifactDetected},
                                     {QStringLiteral("artifactCount"), static_cast<qint64>(artifactCount)},
                                     {QStringLiteral("capturedBlocks"), static_cast<qint64>(m_renderMirrorBlockCount)},
                                     {QStringLiteral("observationLayer"), QStringLiteral("ASIO submitted PCM before endpoint output")},
                                 });

        m_artifactMonitorActive = false;
        m_renderMirrorBlockCount = 0;
        m_renderMirrorWindowBlocks = 0;
    }

    QTimer *m_positionTimer = nullptr;
    QTimer *m_callbackWatchdog = nullptr;
    QTimer *m_deviceHealthTimer = nullptr;
    IASIO *m_driver = nullptr;
    PcmStreamBuffer *m_buffer = nullptr;
    ASIOCallbacks m_callbacks;
    QVector<ASIOBufferInfo> m_bufferInfos;
    QVector<ASIOChannelInfo> m_channelInfos;
    QMutex m_renderMutex;
    QByteArray m_driverId;
    QAudioFormat m_outputFormat;
    QAudioFormat m_decoderFormat;
    qreal m_volume = 1.0;
    qint64 m_startedAtPositionMs = 0;
    qint64 m_renderedFrames = 0;
    int m_sessionId = 0;
    int m_sourceBitDepth = 0;
    long m_bufferSize = 0;
    bool m_started = false;
    bool m_buffersCreated = false;
    bool m_completionPosted = false;
    bool m_comInitialized = false;
    QElapsedTimer m_lastReleaseTimer;
    QVarLengthArray<AsioWorker::NoiseShaperState, 8> m_noiseShaperState;
    std::atomic<long> m_callbackCount = 0;
    std::atomic<int> m_resetRequestCount = 0;
    AudioArtifactMonitor m_artifactMonitor;
    quint64 m_artifactCountAtSessionStart = 0;
    quint64 m_renderMirrorBlockCount = 0;
    quint64 m_renderMirrorWindowBlocks = 0;
    bool m_artifactMonitorActive = false;
    bool m_forceDriverRelease = false;
    bool m_recoveryPendingFirstBufferSwitch = false;
    int m_callbackWatchdogRecoveryCount = 0;
    qint64 m_callbackWatchdogRecoveryStartMs = -1;
    int m_pauseResumeGeneration = 0;
    DriverOpenFailureReason m_lastOpenFailureReason = DriverOpenFailureReason::None;
    QString m_lastOpenFailureDetail;
};

#endif // WINDOWSASIOAUDIOPLAYER_WORKER_H
