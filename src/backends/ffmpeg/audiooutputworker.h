#ifndef AUDIOOUTPUTWORKER_H
#define AUDIOOUTPUTWORKER_H

#include <QAudio>
#include <QAudioFormat>
#include <QObject>

#include <QtGlobal>

class QAudioDevice;
class QAudioSink;
class QIODevice;
class QTimer;

class AudioOutputWorker : public QObject
{
    Q_OBJECT

public:
    explicit AudioOutputWorker(QObject *parent = nullptr);

    void configureOutput(int sessionId,
                         const QAudioDevice &device,
                         const QAudioFormat &format,
                         qsizetype bufferSize,
                         qreal volume,
                         bool startMutedForFadeIn);
    void startOutput(int sessionId, QIODevice *buffer);
    void pauseOutput(int sessionId);
    void resumeOutput(int sessionId);
    void releaseOutput(int sessionId, bool immediateReset);
    void setVolume(qreal volume);

signals:
    void positionUpdated(int sessionId, qint64 processedPositionMs);
    void released(int sessionId);
    void stateChanged(int sessionId, int state, int error);

private:
    void resetPcmFadeIn(const QAudioFormat &format, bool enabled);
    void applyPcmFadeIn(QByteArray &chunk);
    void fadeOutCurrentOutput();
    void startVolumeRampIfNeeded();
    void stopVolumeRamp();
    void pumpOutput();

    QAudioSink *m_audioSink = nullptr;
    QIODevice *m_buffer = nullptr;
    QIODevice *m_outputDevice = nullptr;
    QTimer *m_pumpTimer = nullptr;
    QTimer *m_positionTimer = nullptr;
    QTimer *m_volumeRampTimer = nullptr;
    QByteArray m_pendingOutput;
    int m_pendingOutputOffset = 0;
    int m_sessionId = 0;
    bool m_started = false;
    qreal m_targetVolume = 1.0;
    qreal m_volumeRampStartVolume = 0.0;
    bool m_recoveryFadeInEnabled = false;
    bool m_volumeRampActive = false;
    int m_volumeRampStep = 0;
    const int m_volumeRampTotalSteps = 4;
    QAudioFormat m_outputFormat;
    qsizetype m_pcmFadeTotalFrames = 0;
    qsizetype m_pcmFadeFramesProcessed = 0;
};

#endif // AUDIOOUTPUTWORKER_H
