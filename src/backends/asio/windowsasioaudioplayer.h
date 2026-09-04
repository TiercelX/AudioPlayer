#ifndef WINDOWSASIOAUDIOPLAYER_H
#define WINDOWSASIOAUDIOPLAYER_H

#include "audioplayerbackend.h"
#include "ffmpegpcmshared.h"

#include <QByteArray>
#include <QString>

class AsioOutputWorker;
class FfmpegDecoderWorker;
class LibavSeekDecoderWorker;
class QIODevice;
class QThread;

class WindowsAsioAudioPlayer : public AudioPlayerBackend
{
    Q_OBJECT

public:
    explicit WindowsAsioAudioPlayer(QObject *parent = nullptr);
    ~WindowsAsioAudioPlayer() override;

    BackendId backendId() const override;
    QString backendName() const override;
    QString decoderName() const override;
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
    QAudioDevice selectedOutputDevice() const override;
    AudioOutputDeviceInfo selectedOutputDeviceInfo() const override;
    QByteArray selectedOutputDeviceId() const override;
    bool usesDefaultOutputDevice() const override;
    void setOutputDeviceId(const QByteArray &deviceId) override;
    void refreshOutputConfiguration(bool force = false) override;

    static void setHostWindowHandle(quintptr handle);
    static QList<AudioOutputDeviceInfo> availableAsioOutputDevices();
    static bool hasAvailableAsioOutputDevices();
    static bool isLikelyCreativeDriverId(const QString &driverIdText);
    static bool runDriverInitProbe(const QString &driverIdText,
                                   quintptr hostWindowValue,
                                   const QString &hostKind,
                                   int hostIndex);

private:
    QString channelLayoutForCount(int channelCount) const;
    int creativeChannelReorderMode() const override;
    void setCreativeChannelReorderMode(int mode) override;
    bool exactPlaybackEnabled() const override;
    void setExactPlaybackEnabled(bool enabled) override;
    QString locateFfmpegExecutable() const;
    QString rawInputFormatForSource() const;
    void finalizePlayback();
    void handleAudioFirstBufferSwitch(int sessionId);
    void handleAudioPositionUpdated(int sessionId, qint64 processedPositionMs);
    void handleAudioStateChanged(int sessionId, int state, int error);
    void handleDecoderDataAvailable(int sessionId);
    void handleDecoderError(int sessionId, const QString &message);
    void handleDecoderFinished(int sessionId, int exitCode, int exitStatus, const QString &stderrText);
    void clearBufferDevice();
    void createAudioWorker();
    void createDecoderWorker();
    void finishOutputAfterCompletion();
    void releaseOutputResources();
    void resetAudioWorkerAfterFailure();
    bool shouldUseLibavDecoder() const;
    void cancelSessionRetry();
    void emitAsioBusyRetryStatus();
    void shutdownAudioWorker();
    void startAudioOutputIfReady();
    void startPipeline(qint64 startPositionMs);
    void startPipelineAttempt(qint64 startPositionMs, int retryGeneration);
    void continueStartPipeline(qint64 startPositionMs, int retryGeneration);
    void stopDecoderWorker(bool waitForFinished);
    void teardownPipeline();
    void emitOutputDeviceSelectionChanged();
    AudioOutputDeviceInfo resolveOutputDevice(bool *usesDefault = nullptr) const;
    QAudioFormat selectOutputFormat(const AudioOutputDeviceInfo &device, QString *deviceDescription) const;

    QString m_sourcePath;
    QString m_sourceCodecName;
    QString m_outputDeviceDescription;
    QAudioFormat m_outputFormat;
    qreal m_volume = 1.0;
    qint64 m_currentPositionMs = 0;
    qint64 m_startPositionMs = 0;
    int m_sourceChannelCount = 0;
    int m_sourceSampleRate = 0;
    int m_sourceBitDepth = 0;
    bool m_audioStarted = false;
    bool m_decoderFinished = false;
    bool m_tearingDown = false;
    bool m_outputRetainedAfterCompletion = false;
    QIODevice *m_bufferDevice = nullptr;
    QThread *m_audioThread = nullptr;
    AsioOutputWorker *m_audioWorker = nullptr;
    QThread *m_decoderThread = nullptr;
    FfmpegDecoderWorker *m_decoderWorker = nullptr;
    LibavSeekDecoderWorker *m_libavSeekDecoderWorker = nullptr;
    QString m_stderrBuffer;
    int m_decoderSessionId = 0;
    int m_activeDecoderSessionId = 0;
    int m_stoppingDecoderSessionId = 0;
    QByteArray m_selectedOutputDeviceId;
    QByteArray m_activeOutputDeviceId;
    QByteArray m_lastFailedDriverId;
    int m_sessionRetryAttempt = 0;
    qint64 m_sessionRetryDeadline = 0;
    int m_sessionRetryGeneration = 0;
    QTimer *m_pauseReleaseTimer = nullptr;
    int m_pauseResumeGeneration = 0;
    qint64 m_lastLoggedPositionBucket = -1;
    int m_creativeChannelReorderMode = 0;
    bool m_exactPlaybackEnabled = true;
};

#endif // WINDOWSASIOAUDIOPLAYER_H
