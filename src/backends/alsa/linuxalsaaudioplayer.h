#ifndef LINUXALSAAUDIOPLAYER_H
#define LINUXALSAAUDIOPLAYER_H

#include "audioplayerbackend.h"
#include "ffmpegpcmshared.h"

#include <QElapsedTimer>

#include <alsa/asoundlib.h>

class QThread;
class QMediaDevices;
class QTimer;
class FfmpegDecoderWorker;
class LibavSeekDecoderWorker;
class PcmSeekCache;
class AlsaOutputWorker;
class AlsaFormatNegotiator;
struct AudioPlayerSourceContext;

class LinuxAlsaAudioPlayer : public AudioPlayerBackend
{
    Q_OBJECT

public:
    explicit LinuxAlsaAudioPlayer(QObject *parent = nullptr);
    ~LinuxAlsaAudioPlayer() override;

    BackendId backendId() const override;
    QString backendName() const override;
    QString decoderName() const override;
    bool exclusiveModeEnabled() const override;
    void setExclusiveModeEnabled(bool enabled) override;
    bool exactPlaybackEnabled() const override;
    void setExactPlaybackEnabled(bool enabled) override;
    void setSource(const QString &filePath,
                   int sourceChannelCount,
                   int sourceSampleRate,
                   int sourceBitDepth,
                   const QString &sourceCodecName) override;
    QString source() const override;

    void play() override;
    void pause() override;
    void stop() override;
    void seek(qint64 positionMs) override;
    void setVolume(qreal volume) override;

    QList<QAudioDevice> availableOutputDevices() const override;
    QList<AudioOutputDeviceInfo> availableOutputDeviceInfos() const override;
    QString outputDeviceDescription() const override;
    QAudioFormat outputFormat() const override;
    int outputDeviceBitDepth() const override;
    QAudioDevice selectedOutputDevice() const override;
    AudioOutputDeviceInfo selectedOutputDeviceInfo() const override;
    QByteArray selectedOutputDeviceId() const override;
    bool usesDefaultOutputDevice() const override;
    void setOutputDeviceId(const QByteArray &deviceId) override;
    void refreshOutputConfiguration(bool force = false) override;

    static bool isSupportedForContext(const AudioPlayerSourceContext &context,
                                      QString *reason = nullptr);

private:
    enum class PipelineStartupProfile {
        NormalStart,
        SeekResume,
        ActiveSwitchRebuild,
        ErrorRecovery,
    };

    enum class ActiveOutputSwitchTrigger {
        None,
        DeviceSelection,
        OutputRefresh,
        SystemDeviceChange,
    };

    enum class ActiveOutputSwitchPhase {
        Idle,
        Pending,
        Applying,
    };

    struct ActiveOutputSwitchTransaction {
        ActiveOutputSwitchTrigger trigger = ActiveOutputSwitchTrigger::None;
        ActiveOutputSwitchPhase phase = ActiveOutputSwitchPhase::Idle;
        QString reason;
        qint64 resumePositionMs = 0;
        QElapsedTimer transactionTimer;

        bool isActive() const
        {
            return phase != ActiveOutputSwitchPhase::Idle;
        }
    };

    snd_pcm_t *openAlsaDevice(const QString &deviceId, bool exclusive);
    void closeAlsaDevice();
    QAudioFormat selectOutputFormat(PcmStreamFormat *pcmFormat = nullptr) const;
    QAudioDevice resolveOutputDevice(bool *usesDefault = nullptr) const;
    AudioOutputDeviceInfo resolveOutputDeviceInfo(bool *usesDefault = nullptr) const;
    QList<AudioOutputDeviceInfo> enumerateAlsaOutputDevices() const;
    QAudioFormat probeAlsaDevicePreferredFormat(const QString &deviceName) const;
    QString locateFfmpegExecutable() const;
    QString channelLayoutForCount(int channelCount) const;
    QString rawInputFormatForSource() const;
    PcmStreamFormat decoderFormatForOutput(const PcmStreamFormat &outputFormat) const;
    qsizetype startupThresholdBytes(PipelineStartupProfile profile) const;
    QString pipelineStartupProfileName(PipelineStartupProfile profile) const;
    QString activeOutputSwitchTriggerName(ActiveOutputSwitchTrigger trigger) const;

    void startPipeline(qint64 startPositionMs,
                       PipelineStartupProfile profile = PipelineStartupProfile::NormalStart);
    void teardownPipeline();
    bool seekWhilePlaying(qint64 positionMs);
    bool reconfigureAlsaDevice();
    void stopDecoderWorker(bool waitForFinished);
    void releaseOutputResources();
    void clearBufferDevice();

    void logPlaybackStateChange(PlaybackState from, PlaybackState to) override;
    void finalizePlayback();
    void scheduleOutputRecovery();
    void resetOutputRecoveryState();

    void beginActiveOutputSwitch(ActiveOutputSwitchTrigger trigger,
                                 const QString &reason);
    void applyActiveOutputSwitch();
    void resetActiveOutputSwitch(const QString &reason);
    bool isActiveOutputSwitchInProgress() const;

    void handleDecoderDataAvailable(int sessionId);
    void handleDecoderError(int sessionId, const QString &message);
    void handleDecoderFinished(int sessionId, int exitCode, int exitStatus, const QString &stderrText);
    void handleAudioOutputsChanged();

    QString m_sourcePath;
    QString m_sourceCodecName;
    int m_sourceChannelCount = 0;
    int m_sourceSampleRate = 0;
    int m_sourceBitDepth = 0;

    QString m_outputDeviceDescription;
    QAudioFormat m_outputFormat;
    PcmStreamFormat m_outputPcmFormat;
    PcmStreamFormat m_decoderPcmFormat;
    QByteArray m_selectedOutputDeviceId;

    snd_pcm_t *m_pcmHandle = nullptr;
    snd_pcm_format_t m_alsaFormat = SND_PCM_FORMAT_S16_LE;

    QThread *m_decoderThread = nullptr;
    QThread *m_outputThread = nullptr;
    FfmpegDecoderWorker *m_decoderWorker = nullptr;
    LibavSeekDecoderWorker *m_libavSeekDecoderWorker = nullptr;
    AlsaOutputWorker *m_outputWorker = nullptr;
    PcmSeekCache *m_pcmSeekCache = nullptr;
    PcmStreamBuffer *m_buffer = nullptr;

    QMediaDevices *m_mediaDevices = nullptr;
    QTimer *m_outputDeviceChangeTimer = nullptr;

    qreal m_volume = 1.0;
    int m_decoderSessionId = 0;
    quint64 m_bufferGeneration = 0;
    bool m_audioStarted = false;
    bool m_decoderFinished = false;
    bool m_tearingDown = false;

    bool m_outputRecoveryPending = false;
    int m_outputRecoveryAttempt = 0;
    int m_outputRecoveryExpectedSessionId = 0;
    qint64 m_outputRecoveryPositionMs = 0;
    quint64 m_outputRecoveryGeneration = 0;

    ActiveOutputSwitchTransaction m_activeOutputSwitch;

    bool m_exclusiveModeEnabled = true;
    bool m_exactPlaybackEnabled = true;

};

#endif // LINUXALSAAUDIOPLAYER_H
