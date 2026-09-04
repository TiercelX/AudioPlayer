#ifndef ALSAOUTPUTWORKER_H
#define ALSAOUTPUTWORKER_H

#include "alsaformatnegotiator.h"
#include "ffmpegpcmshared.h"

#include <QObject>
#include <QByteArray>
#include <QElapsedTimer>

#include <atomic>

class QThread;
class PcmStreamBuffer;

class AlsaOutputWorker : public QObject
{
    Q_OBJECT

public:
    explicit AlsaOutputWorker(QObject *parent = nullptr);
    ~AlsaOutputWorker() override;

    void configure(snd_pcm_t *handle,
                   PcmStreamBuffer *buffer,
                   const PcmStreamFormat &bufferFormat,
                   const PcmStreamFormat &deviceFormat,
                   snd_pcm_format_t alsaFormat,
                   int sessionId,
                   quint64 bufferGeneration,
                   bool startMutedForFadeIn = false,
                   qsizetype startupThresholdBytes = 0,
                   qsizetype startupSilenceFrames = 0,
                   qsizetype warmupDiscardFrames = 0);

    void start();
    void stop();
    void requestStopFadeOut();
    void setPaused(bool paused);
    void setVolume(qreal volume);

signals:
    void audioLevelsChanged(int sessionId, qreal leftLevel, qreal rightLevel);
    void positionUpdated(int sessionId, qint64 positionMs);
    void errorOccurred(int sessionId, const QString &message);
    void finished();

private:
    void run();
    int handleXrun(int error);
    void convertFormat(QByteArray &data);
    void applyPcmFadeIn(QByteArray &data);
    void applyPcmFadeOut(QByteArray &data);
    void applyVolume(QByteArray &data);
    void emitAudioLevels(const QByteArray &data);
    qreal sampleMagnitude(const char *sampleData, PcmSampleEncoding encoding) const;

    snd_pcm_t *m_handle = nullptr;
    PcmStreamBuffer *m_buffer = nullptr;
    PcmStreamFormat m_bufferFormat;
    PcmStreamFormat m_deviceFormat;
    snd_pcm_format_t m_alsaFormat = SND_PCM_FORMAT_S16_LE;
    int m_sessionId = 0;
    quint64 m_bufferGeneration = 0;

    bool m_running = false;
    std::atomic<bool> m_paused{false};
    std::atomic<float> m_targetVolume{1.0f};
    qreal m_currentVolume = 1.0;
    qreal m_volumeRampStartVolume = 1.0;
    bool m_volumeRampActive = false;
    qsizetype m_volumeRampTotalFrames = 0;
    qsizetype m_volumeRampFramesProcessed = 0;

    qsizetype m_pcmFadeTotalFrames = 0;
    qsizetype m_pcmFadeFramesProcessed = 0;

    qsizetype m_startupSilenceFrames = 0;
    qsizetype m_warmupDiscardFrames = 0;
    bool m_stopFadeOutRequested = false;
    qsizetype m_pcmFadeOutTotalFrames = 0;
    qsizetype m_pcmFadeOutFramesProcessed = 0;

    int m_xrunCount = 0;
    qint64 m_processedFrames = 0;
    int m_frameSize = 0;
    qsizetype m_startupThresholdBytes = 0;

    QElapsedTimer m_lastLevelEmit;
    qreal m_lastLeftLevel = 0.0;
    qreal m_lastRightLevel = 0.0;
};

#endif // ALSAOUTPUTWORKER_H
