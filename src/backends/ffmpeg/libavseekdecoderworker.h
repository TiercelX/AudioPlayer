#ifndef LIBAVSEEKDECODERWORKER_H
#define LIBAVSEEKDECODERWORKER_H

#include "dolbydownmixprocessor.h"
#include "ffmpegpcmshared.h"

#include <QObject>
#include <QString>

class QTimer;
class PcmSeekCache;

class LibavSeekDecoderWorker : public QObject
{
    Q_OBJECT

public:
    explicit LibavSeekDecoderWorker(QObject *parent = nullptr);
    ~LibavSeekDecoderWorker() override;

    void startDecoding(int sessionId,
                       const QString &sourcePath,
                       qint64 startPositionMs,
                       PcmStreamBuffer *buffer,
                       const PcmStreamFormat &outputFormat);
    void stopDecoding(bool waitForFinished);

    bool prepareSource(const QString &sourcePath, const PcmStreamFormat &outputFormat);
    void seekTo(int sessionId, qint64 startPositionMs, PcmStreamBuffer *buffer, quint64 bufferGeneration);
    void releaseSource();
    bool isSourcePrepared(const QString &sourcePath, const PcmStreamFormat &outputFormat) const;

    void setSeekCache(PcmSeekCache *cache);
    void setCreativeChannelReorderEnabled(bool enabled);

signals:
    void audioLevelsChanged(int sessionId, qreal leftLevel, qreal rightLevel);
    void dataAvailable(int sessionId);
    void errorOccurred(int sessionId, const QString &message);
    void finished(int sessionId, int exitCode, int exitStatus, const QString &stderrText);

private:
    struct State;

    void decodeStep();
    void cleanupState();
    void finishDecoding(int exitCode, int exitStatus, const QString &message = QString());
    void failDecoding(const QString &message);
    bool flushPendingPcm();
    bool appendPcm(const QByteArray &pcm);
    void emitAudioLevelsIfNeeded(const QByteArray &chunk);
    qreal sampleMagnitude(const char *sampleData) const;

    void resetDecodeState();

    QTimer *m_decodeTimer = nullptr;
    State *m_state = nullptr;
    PcmStreamBuffer *m_buffer = nullptr;
    PcmStreamFormat m_outputFormat;
    DolbyDownmixProcessor m_dolbyDownmix;
    QByteArray m_pendingPcm;
    qsizetype m_pendingPcmOffset = 0;
    QString m_preparedSourcePath;
    PcmStreamFormat m_preparedOutputFormat;
    PcmSeekCache *m_seekCache = nullptr;
    qint64 m_decodedBytesWritten = 0;
    int m_sessionId = 0;
    quint64 m_bufferGeneration = 0;
    bool m_creativeChannelReorderEnabled = false;
    bool m_finished = false;
};

#endif // LIBAVSEEKDECODERWORKER_H
