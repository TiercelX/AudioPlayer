#ifndef FFMPEGPCMSHARED_H
#define FFMPEGPCMSHARED_H

#include "pcmutils.h"

#include <QAudioFormat>
#include <QElapsedTimer>
#include <QIODevice>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QStringList>

class QProcess;
class QTimer;

struct PcmStreamFormat
{
    int sampleRate = 0;
    int channelCount = 0;
    PcmSampleEncoding sampleEncoding = PcmSampleEncoding::Unknown;
    int validBitsPerSample = 0;
    QString channelLayout;

    int bitsPerSample() const
    {
        switch (sampleEncoding) {
        case PcmSampleEncoding::UInt8:
            return 8;
        case PcmSampleEncoding::Int16:
            return 16;
        case PcmSampleEncoding::Int24:
            return 24;
        case PcmSampleEncoding::Int32:
        case PcmSampleEncoding::Float32:
            return 32;
        case PcmSampleEncoding::Unknown:
            break;
        }

        return 0;
    }

    int bytesPerSample() const
    {
        const int bits = bitsPerSample();
        return bits > 0 ? bits / 8 : 0;
    }

    int bytesPerFrame() const
    {
        const int bytes = bytesPerSample();
        return channelCount > 0 && bytes > 0 ? channelCount * bytes : 0;
    }

    int effectiveValidBitsPerSample() const
    {
        return validBitsPerSample > 0 ? validBitsPerSample : bitsPerSample();
    }

    bool isValid() const
    {
        return sampleRate > 0 && channelCount > 0 && bytesPerSample() > 0;
    }

    bool operator==(const PcmStreamFormat &other) const
    {
        return sampleRate == other.sampleRate
            && channelCount == other.channelCount
            && sampleEncoding == other.sampleEncoding
            && validBitsPerSample == other.validBitsPerSample
            && channelLayout == other.channelLayout;
    }

    bool operator!=(const PcmStreamFormat &other) const
    {
        return !(*this == other);
    }

    QAudioFormat::SampleFormat qAudioSampleFormat() const
    {
        switch (sampleEncoding) {
        case PcmSampleEncoding::UInt8:
            return QAudioFormat::UInt8;
        case PcmSampleEncoding::Int16:
            return QAudioFormat::Int16;
        case PcmSampleEncoding::Int24:
        case PcmSampleEncoding::Int32:
            return QAudioFormat::Int32;
        case PcmSampleEncoding::Float32:
            return QAudioFormat::Float;
        case PcmSampleEncoding::Unknown:
            break;
        }

        return QAudioFormat::Unknown;
    }
};

bool isPackagedLibavAudioCodec(const QString &codecName);

class PcmStreamBuffer : public QIODevice
{
public:
    explicit PcmStreamBuffer(QObject *parent = nullptr);

    void clear();
    qsizetype discardPendingData();
    void setDiscardWrites(bool discardWrites);
    bool isDiscardingWrites() const;
    qint64 discardedWriteBytes() const;
    bool isSequential() const override;
    void setEndOfStream(bool endOfStream);
    bool endOfStream() const;
    void setMaxSize(qsizetype maxSize);
    qsizetype maxSize() const;
    void setOwner(int sessionId, quint64 generation, const QString &source = QString());
    int ownerSessionId() const;
    quint64 bufferGeneration() const;
    QString ownerSource() const;
    bool matchesOwner(int sessionId, quint64 generation) const;
    bool isEmpty() const;
    qsizetype bufferedBytes() const;
    qsizetype writableBytes() const;
    qint64 append(const QByteArray &chunk);
    qint64 appendForOwner(const QByteArray &chunk, int sessionId, quint64 generation);
    QByteArray readForOwner(qint64 maxSize, int sessionId, quint64 generation, bool *staleRead = nullptr);
    qint64 bytesAvailable() const override;

protected:
    qint64 readData(char *data, qint64 maxSize) override;
    qint64 writeData(const char *data, qint64 maxSize) override;

private:
    qsizetype bufferedBytesUnlocked() const;
    qsizetype writableBytesUnlocked() const;
    void ensureCapacity(qsizetype requiredSize);
    void copyToRingBuffer(const char *data, qsizetype size);
    void copyFromRingBuffer(char *data, qsizetype size);

    mutable QMutex m_mutex;
    QByteArray m_buffer;
    qsizetype m_readPos = 0;
    qsizetype m_writePos = 0;
    qsizetype m_bufferedBytes = 0;
    qsizetype m_maxSize = 0;
    int m_ownerSessionId = 0;
    quint64 m_bufferGeneration = 0;
    QString m_ownerSource;
    bool m_endOfStream = false;
    bool m_discardWrites = false;
    qint64 m_discardedWriteBytes = 0;
};

class FfmpegDecoderWorker : public QObject
{
    Q_OBJECT

public:
    explicit FfmpegDecoderWorker(QObject *parent = nullptr);

    void startDecoding(int sessionId,
                       const QString &program,
                       const QStringList &arguments,
                       PcmStreamBuffer *buffer,
                       const PcmStreamFormat &outputFormat);
    void stopDecoding(bool waitForFinished);
    void setPaused(bool paused);

signals:
    void audioLevelsChanged(int sessionId, qreal leftLevel, qreal rightLevel);
    void dataAvailable(int sessionId);
    void errorOccurred(int sessionId, const QString &message);
    void finished(int sessionId, int exitCode, int exitStatus, const QString &stderrText);

private:
    qreal sampleMagnitude(const char *sampleData) const;
    void emitAudioLevelsIfNeeded(const QByteArray &chunk);
    void drainOutput();
    void drainOutputForSession(int sessionId, QProcess *expectedProcess);
    void drainDirectStandardOutput(int sessionId, QProcess *expectedProcess);
    void drainCapturedStandardOutput(int sessionId, QProcess *expectedProcess);
    void discardPendingStandardOutput(const QString &reason, QProcess *process = nullptr);
    void refreshStandardErrorBuffer();
    bool hasPendingCapturedOutput() const;
    void finishIfReady();
    void cleanupProcess();

    PcmStreamBuffer *m_buffer = nullptr;
    QTimer *m_drainTimer = nullptr;
    QProcess *m_process = nullptr;
    int m_sessionId = 0;
    QString m_stderrBuffer;
    QString m_stdoutCapturePath;
    QString m_stderrCapturePath;
    qint64 m_stdoutReadOffset = 0;
    quint64 m_bufferGeneration = 0;
    bool m_useCaptureFallback = false;
    bool m_processFinished = false;
    bool m_paused = false;
    int m_finishedExitCode = 0;
    int m_finishedExitStatus = 0;
    PcmStreamFormat m_outputFormat;
    QElapsedTimer m_lastLevelEmit;
    QElapsedTimer m_lastBackpressureLog;
    qint64 m_lastBackpressurePendingBytes = -1;
};

QString formatSeekSeconds(qint64 positionMs);

#endif // FFMPEGPCMSHARED_H
