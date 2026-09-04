#ifndef FFMPEGAUDIOPLAYER_H
#define FFMPEGAUDIOPLAYER_H

#include "audioplayerbackend.h"

#include <QString>
#include <QtMultimedia/qaudio.h>

class QMediaDevices;
class QAudioSink;
class QIODevice;
class QThread;
class FfmpegDecoderWorker;
class AudioOutputWorker;

class FfmpegAudioPlayer : public AudioPlayerBackend
{
    Q_OBJECT

public:
    explicit FfmpegAudioPlayer(QObject *parent = nullptr);
    ~FfmpegAudioPlayer() override;

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
    QString outputDeviceDescription() const override;
    QAudioFormat outputFormat() const override;
    QAudioDevice selectedOutputDevice() const override;
    QByteArray selectedOutputDeviceId() const override;
    bool usesDefaultOutputDevice() const override;
    void setOutputDeviceId(const QByteArray &deviceId) override;

private:
    QString channelLayoutForCount(int channelCount) const;
    QString locateFfmpegExecutable() const;
    QString pcmCodecName(QAudioFormat::SampleFormat sampleFormat) const;
    QString pcmSampleFormatName(QAudioFormat::SampleFormat sampleFormat) const;
    QString pcmMuxerName(QAudioFormat::SampleFormat sampleFormat) const;
    qsizetype startupThresholdBytes() const;
    QString rawInputFormatForSource() const;
    void finalizePlayback();
    void handleAudioPositionUpdated(int sessionId, qint64 processedPositionMs);
    void handleAudioStateChanged(int sessionId, int state, int error);
    void handleDecoderDataAvailable(int sessionId);
    void handleDecoderError(int sessionId, const QString &message);
    void handleDecoderFinished(int sessionId, int exitCode, int exitStatus, const QString &stderrText);
    void clearBufferDevice();
    void releaseOutputResources();
    void resetOutputRecoveryState(const QString &reason);
    void scheduleOutputRecovery(int failedSessionId, QtAudio::Error error);
    bool shouldAttemptOutputRecovery(QAudio::State state, QtAudio::Error error) const;
    void startAudioOutputIfReady();
    void startPipeline(qint64 startPositionMs);
    void stopDecoderWorker(bool waitForFinished);
    void teardownPipeline();
    void handleAudioOutputsChanged();
    void applyOutputDeviceChange();
    void emitOutputDeviceSelectionChanged();
    QAudioDevice resolveOutputDevice(bool *usesDefault = nullptr) const;
    QAudioFormat selectOutputFormat(QString *deviceDescription) const;

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
    QIODevice *m_bufferDevice = nullptr;
    QThread *m_audioThread = nullptr;
    AudioOutputWorker *m_audioWorker = nullptr;
    QThread *m_decoderThread = nullptr;
    FfmpegDecoderWorker *m_decoderWorker = nullptr;
    QMediaDevices *m_mediaDevices = nullptr;
    QString m_stderrBuffer;
    QAudio::State m_audioState = QAudio::StoppedState;
    QtAudio::Error m_audioError = QtAudio::NoError;
    int m_decoderSessionId = 0;
    int m_activeDecoderSessionId = 0;
    int m_stoppingDecoderSessionId = 0;
    QByteArray m_selectedOutputDeviceId;
    QByteArray m_activeOutputDeviceId;
    qint64 m_lastLoggedPositionBucket = -1;
    bool m_outputRecoveryPending = false;
    int m_outputRecoveryAttempt = 0;
    int m_outputRecoveryExpectedSessionId = 0;
    qint64 m_outputRecoveryPositionMs = 0;
    quint64 m_outputRecoveryGeneration = 0;
};

#endif // FFMPEGAUDIOPLAYER_H
