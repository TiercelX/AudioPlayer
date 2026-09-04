#ifndef WINDOWSWASAPIAUDIOPLAYER_H
#define WINDOWSWASAPIAUDIOPLAYER_H

#include "audioplayerbackend.h"
#include "ffmpegpcmshared.h"

#include <QElapsedTimer>
#include <QString>
#include <QtMultimedia/qaudio.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <audioclient.h>

class QMediaDevices;
class QIODevice;
class QThread;
class QTimer;
class FfmpegDecoderWorker;
class LibavSeekDecoderWorker;
class PcmSeekCache;
class PcmStreamBuffer;
class WasapiOutputWorker;
struct AudioPlayerSourceContext;
struct ActiveSwitchBoundaryPolicy;
struct WasapiArtifactTrackingConfig;

class WindowsWasapiAudioPlayer : public AudioPlayerBackend
{
    Q_OBJECT

public:
    enum class CreativeChannelReorderMode {
        Auto = 0,
        Off = 1,
        ForceCreative = 2,
    };

    explicit WindowsWasapiAudioPlayer(QObject *parent = nullptr);
    ~WindowsWasapiAudioPlayer() override;

    BackendId backendId() const override;
    QString backendName() const override;
    QString decoderName() const override;
    bool exclusiveModeEnabled() const override;
    void setExclusiveModeEnabled(bool enabled) override;
    bool exactPlaybackEnabled() const override;
    void setExactPlaybackEnabled(bool enabled) override;
    bool stabilityModeEnabled() const override;
    void setStabilityModeEnabled(bool enabled) override;
    int creativeChannelReorderMode() const override;
    void setCreativeChannelReorderMode(int mode) override;
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
    QString outputDeviceDescription() const override;
    QAudioFormat outputFormat() const override;
    int outputDeviceBitDepth() const override;
    QAudioDevice selectedOutputDevice() const override;
    QByteArray selectedOutputDeviceId() const override;
    bool usesDefaultOutputDevice() const override;
    void setOutputDeviceId(const QByteArray &deviceId) override;
    void refreshOutputConfiguration(bool force = false) override;

    static bool isSupportedForContext(const AudioPlayerSourceContext &context, QString *reason = nullptr);

private:
    enum class ActiveOutputSwitchTrigger {
        None,
        DeviceSelection,
        OutputRefresh,
        SystemDeviceChange,
        OutputFormatChange,
    };

    enum class ActiveOutputSwitchPhase {
        Idle,
        WaitingForInvalidation,
        Pending,
        OutputSuspended,
        Preflight,
        Applying,
        WaitingForOutputStart,
    };

    enum class PipelineStartupProfile {
        NormalStart,
        SeekRestart,
        SeekResume,
        ActiveSwitchRebuild,
        ErrorRecovery,
    };

    struct ActiveOutputSwitchTransaction {
        ActiveOutputSwitchTrigger trigger = ActiveOutputSwitchTrigger::None;
        ActiveOutputSwitchPhase phase = ActiveOutputSwitchPhase::Idle;
        QString reason;
        int sessionId = 0;
        bool forceReconfigure = false;
        bool transitionPrepared = false;
        bool needsFreshBuffer = false;
        int outputStartErrorRetryCount = 0;
        int absorbedOutputErrorCount = 0;
        int invalidationWatchdogRebuildCount = 0;
        int conservativeRebuildCount = 0;
        qint64 lastAbsorbedElapsedMs = -1;
        QElapsedTimer transactionTimer;
        QElapsedTimer rebuildTimer;

        bool isActive() const
        {
            return phase != ActiveOutputSwitchPhase::Idle;
        }
    };

    QString channelLayoutForCount(int channelCount) const;
    QString locateFfmpegExecutable() const;
    QString pcmCodecName(const PcmStreamFormat &format) const;
    QString pcmSampleFormatName(const PcmStreamFormat &format) const;
    QString pcmMuxerName(const PcmStreamFormat &format) const;
    QString activeOutputSwitchTriggerName(ActiveOutputSwitchTrigger trigger) const;
    QString activeOutputSwitchPhaseName(ActiveOutputSwitchPhase phase) const;
    QString pipelineStartupProfileName(PipelineStartupProfile profile) const;
    qsizetype startupThresholdBytes() const;
    PcmStreamFormat decoderFormatForOutput(const PcmStreamFormat &outputFormat) const;
    QString decoderChannelLayoutForSource(const QByteArray &waveFormatData, int decoderChannelCount) const;
    bool spatialStaticBedEnabledForSource() const;
    QString rawInputFormatForSource() const;
    void finalizePlayback();
    void handleAudioPositionUpdated(int sessionId, qint64 processedPositionMs);
    void handleAudioStateChanged(int sessionId, int state, int error);
    bool handleActiveSwitchStateChange(int sessionId, bool activeSwitchSession);
    bool handleTerminalStateChange(int sessionId);
    void handleRecoveryStateChange(int sessionId);
    void handleDecoderDataAvailable(int sessionId);
    void handleDecoderError(int sessionId, const QString &message);
    void handleDecoderFinished(int sessionId, int exitCode, int exitStatus, const QString &stderrText);
    void clearBufferDevice();
    void releaseOutputResources(bool submitStopPcmFade = false);
    void quarantineBufferDevice(const QString &reason, bool discardFutureWrites = true);
    void resumeBufferDevice(const QString &reason);
    void resetOutputRecoveryState(const QString &reason);
    void resetAnomalyTracking(const QString &reason);
    void logPlaybackAnomaly(const QString &event, const QString &details = QString()) const;
    void scheduleOutputRecovery(int failedSessionId, QtAudio::Error error);
    bool shouldFinalizeAfterEndOfStreamOutputStop() const;
    bool shouldAttemptOutputRecovery(QAudio::State state, QtAudio::Error error) const;
    void startAudioOutputIfReady();
    void startPipeline(qint64 startPositionMs, PipelineStartupProfile startupProfile);
    QStringList prepareDecoderArgs(qint64 startPositionMs, int sessionId,
                                    const QAudioDevice &device,
                                    bool useLibavDecoder, bool realtimeDecode) const;
    void configureAudioWorker(int sessionId, const QAudioDevice &device,
                              qsizetype sinkBufferSize,
                              bool startMutedForFadeIn, bool injectStartupSilence,
                              bool discardWarmupFrames, quint32 startupSilenceMsOverride,
                              quint32 warmupDiscardMsOverride, int pcmFadeInDurationMsOverride,
                              int streamFadeInDelayMs,
                              const ActiveSwitchBoundaryPolicy &activeSwitchBoundaryPolicy,
                              const WasapiArtifactTrackingConfig &artifactTracking,
                              bool requestedExclusiveMode,
                              bool activeSwitchRebuildStartup,
                              PipelineStartupProfile startupProfile,
                              bool spatialStaticBedRequested);
    void startDecoderWorker(qint64 startPositionMs, int sessionId, bool useLibavDecoder,
                            const QStringList &arguments, const QString &ffmpegExecutable,
                            PcmStreamBuffer *buffer);
    void stopDecoderWorker(bool waitForFinished);
    void teardownPipeline();
    void handleAudioOutputsChanged();
    void beginActiveOutputSwitch(ActiveOutputSwitchTrigger trigger,
                                 const QString &reason,
                                 bool forceReconfigure = false);
    void scheduleActiveOutputSwitch();
    void applyActiveOutputSwitch();
    void triggerScriptedSameOutputInvalidationIoError();
    bool isActiveOutputSwitchInProgress() const;
    bool activeOutputSwitchMatchesSession(int sessionId) const;
    void setActiveOutputSwitchPhase(ActiveOutputSwitchPhase phase, const QString &reason);
    void resetActiveOutputSwitch(const QString &reason);
    void prepareOutputDeviceChangeTransition(int sessionId);
    void restoreOutputDeviceChangeTransition(int sessionId);
    void prepareActiveOutputInvalidationTransition(int sessionId);
    void restoreActiveOutputInvalidationTransition(int sessionId);
    void prepareConfirmedActiveSwitchFade(const QString &timing);
    bool spatialEndpointFlushEnabled() const;
    int spatialEndpointFlushMs() const;
    int spatialEndpointSettleMs() const;
    QString startupObservationProfile(PipelineStartupProfile startupProfile) const;
    void performSpatialEndpointFlush(const QAudioDevice &device,
                                     const PcmStreamFormat &deviceFormat,
                                     const QByteArray &waveFormatData,
                                     const QString &reason,
                                     const QString &startupProfile);
    void syncExclusiveModeStateFromWorker(bool requestedExclusiveMode, const QString &context);
    bool canHotReconfigureOutput(const PcmStreamFormat &deviceFormat) const;
    void reconfigureActiveOutput(const QAudioDevice &device,
                                 const QString &deviceDescription,
                                 const QAudioFormat &format,
                                 const QByteArray &waveFormatData,
                                 const PcmStreamFormat &deviceFormat);
    void emitOutputDeviceSelectionChanged();
    QAudioDevice resolveOutputDevice(bool *usesDefault = nullptr) const;
    QAudioFormat selectOutputFormat(QString *deviceDescription,
                                    QByteArray *waveFormatData = nullptr,
                                    PcmStreamFormat *pcmFormat = nullptr) const;
    bool selectExclusiveFormat(IAudioClient *audioClient,
                               const QAudioDevice &device,
                               const PcmStreamFormat &mixPcmFormat,
                               const QAudioFormat &mixQtFormat,
                               const QList<int> &channelCandidates,
                               int preferredChannelCount,
                               unsigned long preferredChannelMask,
                               QAudioFormat *outFormat,
                               QByteArray *outWaveFormatData,
                               PcmStreamFormat *outPcmFormat) const;
    bool selectSharedFormat(IAudioClient *audioClient,
                            const WAVEFORMATEX *mixFormat,
                            const PcmStreamFormat &mixPcmFormat,
                            const QAudioFormat &mixQtFormat,
                            bool canUseDirectMixFormat,
                            const QList<int> &channelCandidates,
                            int preferredChannelCount,
                            unsigned long preferredChannelMask,
                            QAudioFormat *outFormat,
                            QByteArray *outWaveFormatData,
                            PcmStreamFormat *outPcmFormat) const;

    QString m_sourcePath;
    QString m_previousSourcePath;
    QString m_sourceCodecName;
    QString m_outputDeviceDescription;
    QAudioFormat m_outputFormat;
    PcmStreamFormat m_outputPcmFormat;
    PcmStreamFormat m_decoderPcmFormat;
    QByteArray m_outputWaveFormatData;
    qreal m_volume = 1.0;
    qint64 m_currentPositionMs = 0;
    qint64 m_startPositionMs = 0;
    qint64 m_lastSeekRequestTimeMs = -1;
    qint64 m_lastSeekRequestPositionMs = -1;
    int m_sourceChannelCount = 0;
    int m_sourceSampleRate = 0;
    int m_sourceBitDepth = 0;
    bool m_audioStarted = false;
    bool m_hasStartedPlaybackThisRun = false;
    bool m_decoderFinished = false;
    bool m_tearingDown = false;
    bool m_pendingSourceSwitchStartup = false;
    bool m_pendingSeekResumeStartup = false;
    QIODevice *m_bufferDevice = nullptr;
    QThread *m_audioThread = nullptr;
    WasapiOutputWorker *m_audioWorker = nullptr;
    QThread *m_decoderThread = nullptr;
    FfmpegDecoderWorker *m_decoderWorker = nullptr;
    LibavSeekDecoderWorker *m_libavSeekDecoderWorker = nullptr;
    PcmSeekCache *m_pcmSeekCache = nullptr;
    QMediaDevices *m_mediaDevices = nullptr;
    QTimer *m_outputDeviceChangeTimer = nullptr;
    QString m_stderrBuffer;
    QAudio::State m_audioState = QAudio::StoppedState;
    QtAudio::Error m_audioError = QtAudio::NoError;
    int m_decoderSessionId = 0;
    quint64 m_bufferGeneration = 0;
    int m_activeDecoderSessionId = 0;
    int m_stoppingDecoderSessionId = 0;
    QByteArray m_selectedOutputDeviceId;
    QByteArray m_activeOutputDeviceId;
    QElapsedTimer m_positionObservationTimer;
    qint64 m_lastPositionObservationElapsedMs = -1;
    qint64 m_lastObservedPositionMs = -1;
    int m_flatPositionTickCount = 0;
    qint64 m_lastLoggedPositionBucket = -1;
    bool m_outputRecoveryPending = false;
    int m_outputRecoveryAttempt = 0;
    int m_outputRecoveryExpectedSessionId = 0;
    qint64 m_outputRecoveryPositionMs = 0;
    quint64 m_outputRecoveryGeneration = 0;
    ActiveOutputSwitchTransaction m_activeOutputSwitch;
    PipelineStartupProfile m_currentPipelineStartupProfile = PipelineStartupProfile::NormalStart;
    bool m_seekResumeFirstDecodedPcmLogged = false;
    QString m_appStartTimeUtc;
    bool m_exclusiveModeEnabled = false;
    bool m_stabilityModeEnabled = false;
    bool m_exactPlaybackEnabled = true;
    bool m_ignoreNextUnchangedOutputChangeAfterExclusiveFallback = false;
    CreativeChannelReorderMode m_creativeChannelReorderMode = CreativeChannelReorderMode::Auto;
};

#endif // WINDOWSWASAPIAUDIOPLAYER_H
