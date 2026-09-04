#include "ffmpegpcmshared.h"

#include "pcmutils.h"
#include "playerlogger.h"

#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryFile>
#include <QTimer>

#include <cmath>
#include <cstring>

bool isPackagedLibavAudioCodec(const QString &codecName)
{
    const QString codec = codecName.trimmed().toLower();
    return codec == QStringLiteral("aac")
        || codec == QStringLiteral("ac3")
        || codec == QStringLiteral("alac")
        || codec == QStringLiteral("eac3")
        || codec == QStringLiteral("flac")
        || codec == QStringLiteral("mp3")
        || codec == QStringLiteral("mp3float")
        || codec == QStringLiteral("pcm_f32le")
        || codec == QStringLiteral("pcm_s16le")
        || codec == QStringLiteral("pcm_s24le")
        || codec == QStringLiteral("pcm_s32le")
        || codec == QStringLiteral("pcm_u8")
        || codec == QStringLiteral("truehd");
}

PcmStreamBuffer::PcmStreamBuffer(QObject *parent)
    : QIODevice(parent)
{
    open(QIODevice::ReadOnly);
}

void PcmStreamBuffer::clear()
{
    QMutexLocker locker(&m_mutex);
    m_readPos = 0;
    m_writePos = 0;
    m_bufferedBytes = 0;
}

qsizetype PcmStreamBuffer::discardPendingData()
{
    QMutexLocker locker(&m_mutex);
    const qsizetype pendingBytes = m_bufferedBytes;
    m_readPos = 0;
    m_writePos = 0;
    m_bufferedBytes = 0;
    m_discardWrites = true;
    return pendingBytes;
}

void PcmStreamBuffer::setDiscardWrites(bool discardWrites)
{
    QMutexLocker locker(&m_mutex);
    m_discardWrites = discardWrites;
    if (!m_discardWrites) {
        m_discardedWriteBytes = 0;
    }
}

bool PcmStreamBuffer::isDiscardingWrites() const
{
    QMutexLocker locker(&m_mutex);
    return m_discardWrites;
}

qint64 PcmStreamBuffer::discardedWriteBytes() const
{
    QMutexLocker locker(&m_mutex);
    return m_discardedWriteBytes;
}

bool PcmStreamBuffer::isSequential() const
{
    return true;
}

void PcmStreamBuffer::setEndOfStream(bool endOfStream)
{
    QMutexLocker locker(&m_mutex);
    m_endOfStream = endOfStream;
}

bool PcmStreamBuffer::endOfStream() const
{
    QMutexLocker locker(&m_mutex);
    return m_endOfStream;
}

void PcmStreamBuffer::setMaxSize(qsizetype maxSize)
{
    QMutexLocker locker(&m_mutex);
    m_maxSize = maxSize;
    if (maxSize > 0 && m_buffer.size() < maxSize) {
        m_buffer.resize(maxSize);
    }
}

qsizetype PcmStreamBuffer::maxSize() const
{
    QMutexLocker locker(&m_mutex);
    return m_maxSize;
}

void PcmStreamBuffer::setOwner(int sessionId, quint64 generation, const QString &source)
{
    QMutexLocker locker(&m_mutex);
    m_ownerSessionId = sessionId;
    m_bufferGeneration = generation;
    m_ownerSource = source;
}

int PcmStreamBuffer::ownerSessionId() const
{
    QMutexLocker locker(&m_mutex);
    return m_ownerSessionId;
}

quint64 PcmStreamBuffer::bufferGeneration() const
{
    QMutexLocker locker(&m_mutex);
    return m_bufferGeneration;
}

QString PcmStreamBuffer::ownerSource() const
{
    QMutexLocker locker(&m_mutex);
    return m_ownerSource;
}

bool PcmStreamBuffer::matchesOwner(int sessionId, quint64 generation) const
{
    QMutexLocker locker(&m_mutex);
    return (m_ownerSessionId == 0 || m_ownerSessionId == sessionId)
        && (m_bufferGeneration == 0 || m_bufferGeneration == generation);
}

bool PcmStreamBuffer::isEmpty() const
{
    QMutexLocker locker(&m_mutex);
    return m_bufferedBytes == 0;
}

qsizetype PcmStreamBuffer::bufferedBytes() const
{
    QMutexLocker locker(&m_mutex);
    return m_bufferedBytes;
}

qsizetype PcmStreamBuffer::writableBytes() const
{
    QMutexLocker locker(&m_mutex);
    return writableBytesUnlocked();
}

qint64 PcmStreamBuffer::append(const QByteArray &chunk)
{
    qsizetype bytesToWrite = 0;
    {
        QMutexLocker locker(&m_mutex);
        if (m_discardWrites) {
            m_discardedWriteBytes += chunk.size();
            return 0;
        }

        bytesToWrite = qMin<qsizetype>(chunk.size(), writableBytesUnlocked());
        if (bytesToWrite <= 0) {
            return 0;
        }

        ensureCapacity(m_bufferedBytes + bytesToWrite);
        copyToRingBuffer(chunk.constData(), bytesToWrite);
    }

    Q_EMIT readyRead();
    return bytesToWrite;
}

qint64 PcmStreamBuffer::appendForOwner(const QByteArray &chunk, int sessionId, quint64 generation)
{
    qsizetype bytesToWrite = 0;
    int ownerSessionId = 0;
    quint64 ownerGeneration = 0;
    bool staleWrite = false;
    bool discarded = false;
    {
        QMutexLocker locker(&m_mutex);
        ownerSessionId = m_ownerSessionId;
        ownerGeneration = m_bufferGeneration;
        staleWrite = (ownerSessionId != 0 && ownerSessionId != sessionId)
            || (ownerGeneration != 0 && ownerGeneration != generation);
        if (staleWrite) {
            m_discardedWriteBytes += chunk.size();
        } else if (m_discardWrites) {
            m_discardedWriteBytes += chunk.size();
            discarded = true;
        } else {
            bytesToWrite = qMin<qsizetype>(chunk.size(), writableBytesUnlocked());
            if (bytesToWrite > 0) {
                ensureCapacity(m_bufferedBytes + bytesToWrite);
                copyToRingBuffer(chunk.constData(), bytesToWrite);
            }
        }
    }

    if (staleWrite) {
        PlayerLogger::log(QStringLiteral("anomaly"),
                          QStringLiteral("stale_session_write attemptedSession=%1 attemptedGeneration=%2 ownerSession=%3 ownerGeneration=%4 bytes=%5")
                              .arg(sessionId)
                              .arg(generation)
                              .arg(ownerSessionId)
                              .arg(ownerGeneration)
                              .arg(chunk.size()));
        PlayerLogger::diagnostic(QStringLiteral("decoder"),
                                 QStringLiteral("stale_session_write"),
                                 {
                                     {QStringLiteral("attemptedSessionId"), sessionId},
                                     {QStringLiteral("attemptedBufferGeneration"), static_cast<qint64>(generation)},
                                     {QStringLiteral("ownerSessionId"), ownerSessionId},
                                     {QStringLiteral("ownerBufferGeneration"), static_cast<qint64>(ownerGeneration)},
                                     {QStringLiteral("bytes"), chunk.size()},
                                 });
        return 0;
    }

    if (discarded || bytesToWrite <= 0) {
        return 0;
    }

    Q_EMIT readyRead();
    return bytesToWrite;
}

QByteArray PcmStreamBuffer::readForOwner(qint64 maxSize, int sessionId, quint64 generation, bool *staleRead)
{
    if (staleRead) {
        *staleRead = false;
    }

    int ownerSessionId = 0;
    quint64 ownerGeneration = 0;
    qint64 available = 0;
    QByteArray result;
    {
        QMutexLocker locker(&m_mutex);
        ownerSessionId = m_ownerSessionId;
        ownerGeneration = m_bufferGeneration;
        const bool stale = (ownerSessionId != 0 && ownerSessionId != sessionId)
            || (ownerGeneration != 0 && ownerGeneration != generation);
        if (stale) {
            if (staleRead) {
                *staleRead = true;
            }
            available = m_bufferedBytes;
        } else if (maxSize > 0) {
            available = m_bufferedBytes;
            const qint64 bytesToRead = qMin(maxSize, available);
            if (bytesToRead > 0) {
                result.resize(static_cast<qsizetype>(bytesToRead));
                copyFromRingBuffer(result.data(), static_cast<qsizetype>(bytesToRead));
            }
        }
    }

    if (staleRead && *staleRead) {
        PlayerLogger::log(QStringLiteral("anomaly"),
                          QStringLiteral("stale_buffer_read attemptedSession=%1 attemptedGeneration=%2 ownerSession=%3 ownerGeneration=%4 bufferedBytes=%5")
                              .arg(sessionId)
                              .arg(generation)
                              .arg(ownerSessionId)
                              .arg(ownerGeneration)
                              .arg(available));
        PlayerLogger::diagnostic(QStringLiteral("audio"),
                                 QStringLiteral("stale_buffer_read"),
                                 {
                                     {QStringLiteral("attemptedSessionId"), sessionId},
                                     {QStringLiteral("attemptedBufferGeneration"), static_cast<qint64>(generation)},
                                     {QStringLiteral("ownerSessionId"), ownerSessionId},
                                     {QStringLiteral("ownerBufferGeneration"), static_cast<qint64>(ownerGeneration)},
                                     {QStringLiteral("bufferedBytes"), available},
                                 });
    }

    return result;
}

qint64 PcmStreamBuffer::bytesAvailable() const
{
    return bufferedBytes() + QIODevice::bytesAvailable();
}

qint64 PcmStreamBuffer::readData(char *data, qint64 maxSize)
{
    QMutexLocker locker(&m_mutex);
    const qint64 available = m_bufferedBytes;
    if (maxSize <= 0 || available <= 0) {
        return 0;
    }

    const qint64 bytesToRead = qMin(maxSize, available);
    copyFromRingBuffer(data, static_cast<qsizetype>(bytesToRead));

    return bytesToRead;
}

qint64 PcmStreamBuffer::writeData(const char *, qint64)
{
    return -1;
}

qsizetype PcmStreamBuffer::bufferedBytesUnlocked() const
{
    return m_bufferedBytes;
}

qsizetype PcmStreamBuffer::writableBytesUnlocked() const
{
    return qMax<qsizetype>(0, m_maxSize - m_bufferedBytes);
}

void PcmStreamBuffer::ensureCapacity(qsizetype requiredSize)
{
    if (m_buffer.size() >= requiredSize) {
        return;
    }

    const qsizetype newSize = qMax(requiredSize, m_maxSize);
    if (newSize <= 0) {
        return;
    }

    QByteArray newBuffer;
    newBuffer.resize(newSize);

    if (m_bufferedBytes > 0 && !m_buffer.isEmpty()) {
        const qsizetype bufferCapacity = m_buffer.size();
        const qsizetype firstPart = qMin(m_bufferedBytes, bufferCapacity - m_readPos);
        if (firstPart > 0) {
            std::memcpy(newBuffer.data(), m_buffer.constData() + m_readPos, static_cast<size_t>(firstPart));
        }
        if (m_bufferedBytes > firstPart) {
            std::memcpy(newBuffer.data() + firstPart, m_buffer.constData(),
                        static_cast<size_t>(m_bufferedBytes - firstPart));
        }
    }

    m_buffer = newBuffer;
    m_readPos = 0;
    m_writePos = m_bufferedBytes;
}

void PcmStreamBuffer::copyToRingBuffer(const char *data, qsizetype size)
{
    const qsizetype bufferCapacity = m_buffer.size();
    const qsizetype firstPart = qMin(size, bufferCapacity - m_writePos);
    if (firstPart > 0) {
        std::memcpy(m_buffer.data() + m_writePos, data, static_cast<size_t>(firstPart));
    }
    if (size > firstPart) {
        std::memcpy(m_buffer.data(), data + firstPart, static_cast<size_t>(size - firstPart));
    }
    m_writePos = (m_writePos + size) % bufferCapacity;
    m_bufferedBytes += size;
}

void PcmStreamBuffer::copyFromRingBuffer(char *data, qsizetype size)
{
    const qsizetype bufferCapacity = m_buffer.size();
    const qsizetype firstPart = qMin(size, bufferCapacity - m_readPos);
    if (firstPart > 0) {
        std::memcpy(data, m_buffer.constData() + m_readPos, static_cast<size_t>(firstPart));
    }
    if (size > firstPart) {
        std::memcpy(data + firstPart, m_buffer.constData(), static_cast<size_t>(size - firstPart));
    }
    m_readPos = (m_readPos + size) % bufferCapacity;
    m_bufferedBytes -= size;
}

QString formatSeekSeconds(qint64 positionMs)
{
    return QString::number(positionMs / 1000.0, 'f', 3);
}

FfmpegDecoderWorker::FfmpegDecoderWorker(QObject *parent)
    : QObject(parent)
    , m_drainTimer(new QTimer(this))
{
    m_drainTimer->setInterval(10);
    m_drainTimer->setSingleShot(true);
    connect(m_drainTimer, &QTimer::timeout, this, &FfmpegDecoderWorker::drainOutput);
}

void FfmpegDecoderWorker::startDecoding(int sessionId,
                                        const QString &program,
                                        const QStringList &arguments,
                                        PcmStreamBuffer *buffer,
                                        const PcmStreamFormat &outputFormat)
{
    PlayerLogger::log(QStringLiteral("decoder"),
                      QStringLiteral("startDecoding session=%1 program=%2 argCount=%3")
                          .arg(sessionId)
                          .arg(program)
                          .arg(arguments.size()));
    stopDecoding(true);

    m_sessionId = sessionId;
    m_buffer = buffer;
    m_bufferGeneration = buffer ? buffer->bufferGeneration() : 0;
    m_outputFormat = outputFormat;
    m_stderrBuffer.clear();
    m_stdoutReadOffset = 0;
    m_processFinished = false;
    m_finishedExitCode = 0;
    m_finishedExitStatus = static_cast<int>(QProcess::NormalExit);
    m_useCaptureFallback =
        QProcessEnvironment::systemEnvironment()
            .value(QStringLiteral("AUDIOPLAYER_FFMPEG_STDOUT_CAPTURE"))
            .trimmed()
            == QStringLiteral("1");
    m_lastLevelEmit.invalidate();
    m_lastBackpressureLog.invalidate();
    m_lastBackpressurePendingBytes = -1;
    m_paused = false;

    QTemporaryFile standardErrorFile;
    QTemporaryFile standardOutputFile;
    if (!standardErrorFile.open() || (m_useCaptureFallback && !standardOutputFile.open())) {
        const QString errorText = tr("音频解码器初始化失败\n\n"
                                     "可能原因：系统临时文件夹不可用\n\n"
                                     "建议操作：\n"
                                     "1. 检查磁盘空间\n"
                                     "2. 重新启动程序");
        PlayerLogger::log(QStringLiteral("decoder"),
                          QStringLiteral("startDecoding capture-init-failed session=%1")
                              .arg(sessionId));
        emit errorOccurred(m_sessionId, errorText);
        cleanupProcess();
        return;
    }

    m_stdoutCapturePath = m_useCaptureFallback ? standardOutputFile.fileName() : QString();
    m_stderrCapturePath = standardErrorFile.fileName();
    if (m_useCaptureFallback) {
        standardOutputFile.close();
    }
    standardErrorFile.close();

    m_process = new QProcess(this);
    m_process->setStandardInputFile(QProcess::nullDevice());
    m_process->setStandardErrorFile(m_stderrCapturePath, QIODeviceBase::Truncate);
    if (m_useCaptureFallback) {
        m_process->setStandardOutputFile(m_stdoutCapturePath, QIODeviceBase::Truncate);
    } else {
        m_process->setProcessChannelMode(QProcess::SeparateChannels);
        connect(m_process, &QProcess::readyReadStandardOutput, this, [this, sessionId, process = m_process] {
            drainOutputForSession(sessionId, process);
        });
    }
    connect(m_process,
            static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
            this,
            [this, sessionId, process = m_process](int exitCode, QProcess::ExitStatus exitStatus) {
                if (process != m_process || sessionId != m_sessionId) {
                    discardPendingStandardOutput(QStringLiteral("stale-finished-signal"), process);
                    return;
                }
                m_processFinished = true;
                m_finishedExitCode = exitCode;
                m_finishedExitStatus = static_cast<int>(exitStatus);
                drainOutputForSession(sessionId, process);
                finishIfReady();
            });
    connect(m_process, &QProcess::errorOccurred, this, [this, sessionId, process = m_process](QProcess::ProcessError processError) {
        if (process != m_process || sessionId != m_sessionId) {
            discardPendingStandardOutput(QStringLiteral("stale-error-signal"), process);
            return;
        }
        refreshStandardErrorBuffer();
        // Log technical details
        const QString technicalError = m_process && !m_process->errorString().isEmpty()
            ? m_process->errorString()
            : m_stderrBuffer.trimmed();
        PlayerLogger::log(QStringLiteral("decoder"),
                          QStringLiteral("processError session=%1 type=%2 state=%3 error=%4")
                              .arg(m_sessionId)
                              .arg(static_cast<int>(processError))
                              .arg(m_process ? static_cast<int>(m_process->state()) : -1)
                              .arg(technicalError.isEmpty() ? QStringLiteral("none") : technicalError));

        // User-friendly message for non-start failures
        if (processError != QProcess::FailedToStart) {
            const QString errorText = tr("音频解码器运行异常\n\n"
                                         "建议操作：\n"
                                         "1. 尝试其他音频文件\n"
                                         "2. 重新播放");
            emit errorOccurred(m_sessionId, errorText);
        }
    });

    m_process->start(program, arguments);
    if (!m_process->waitForStarted(3000)) {
        refreshStandardErrorBuffer();
        // Log technical details
        PlayerLogger::log(QStringLiteral("decoder"),
                          QStringLiteral("startDecoding failed session=%1 error=%2")
                              .arg(sessionId)
                              .arg(m_process->errorString().isEmpty()
                                       ? (m_stderrBuffer.trimmed().isEmpty() ? QStringLiteral("unknown") : m_stderrBuffer.trimmed())
                                       : m_process->errorString()));

        // User-friendly message
        const QString errorText = tr("音频解码器启动失败\n\n"
                                     "可能原因：FFmpeg 程序不可用或损坏\n\n"
                                     "建议操作：\n"
                                     "1. 重新安装程序\n"
                                     "2. 检查程序完整性");
        emit errorOccurred(m_sessionId, errorText);
        cleanupProcess();
        return;
    }

    PlayerLogger::log(QStringLiteral("decoder"),
                      QStringLiteral("startDecoding started session=%1 pid=%2 stdoutMode=%3")
                          .arg(sessionId)
                          .arg(m_process->processId())
                          .arg(m_useCaptureFallback ? QStringLiteral("capture-file")
                                                     : QStringLiteral("readyRead")));
    if (m_useCaptureFallback) {
        m_drainTimer->start();
    }
    drainOutputForSession(sessionId, m_process);
}

void FfmpegDecoderWorker::setPaused(bool paused)
{
    if (m_paused == paused) {
        return;
    }
    m_paused = paused;
    PlayerLogger::log(QStringLiteral("decoder"),
                      QStringLiteral("setPaused session=%1 paused=%2")
                          .arg(m_sessionId)
                          .arg(paused ? 1 : 0));
    if (!paused && m_process && !m_processFinished) {
        // Resume: kick the drain loop so buffered stdout gets consumed.
        drainOutput();
    }
}

void FfmpegDecoderWorker::stopDecoding(bool waitForFinished)
{
    const int sessionId = m_sessionId;
    refreshStandardErrorBuffer();
    QString stderrText = m_stderrBuffer;
    PlayerLogger::log(QStringLiteral("decoder"),
                      QStringLiteral("stopDecoding begin session=%1 waitForFinished=%2 hasProcess=%3")
                          .arg(sessionId)
                          .arg(waitForFinished)
                          .arg(m_process != nullptr));
    if (!m_process) {
        cleanupProcess();
        if (sessionId != 0) {
            PlayerLogger::log(QStringLiteral("decoder"),
                              QStringLiteral("stopDecoding no-process session=%1 emitFinished")
                                  .arg(sessionId));
            emit finished(sessionId,
                          0,
                          static_cast<int>(QProcess::NormalExit),
                          stderrText);
        }
        return;
    }

    disconnect(m_process, nullptr, this, nullptr);
    discardPendingStandardOutput(QStringLiteral("stopDecoding-disconnect"), m_process);
    if (m_process->state() != QProcess::NotRunning) {
        m_process->kill();
        if (waitForFinished) {
            const bool finished = m_process->waitForFinished(200);
            PlayerLogger::log(QStringLiteral("decoder"),
                              QStringLiteral("stopDecoding waited session=%1 finished=%2 state=%3")
                                  .arg(sessionId)
                                  .arg(finished)
                                  .arg(static_cast<int>(m_process->state())));
        }
    }
    discardPendingStandardOutput(QStringLiteral("stopDecoding-before-cleanup"), m_process);

    refreshStandardErrorBuffer();
    stderrText = m_stderrBuffer;
    cleanupProcess();
    if (sessionId != 0) {
        PlayerLogger::log(QStringLiteral("decoder"),
                          QStringLiteral("stopDecoding emitFinished session=%1").arg(sessionId));
        emit finished(sessionId,
                      0,
                      static_cast<int>(QProcess::NormalExit),
                      stderrText);
    }
}

qreal FfmpegDecoderWorker::sampleMagnitude(const char *sampleData) const
{
    return PcmUtils::sampleMagnitude(m_outputFormat.sampleEncoding, sampleData);
}

void FfmpegDecoderWorker::emitAudioLevelsIfNeeded(const QByteArray &chunk)
{
    const int channelCount = m_outputFormat.channelCount;
    const int bytesPerFrame = m_outputFormat.bytesPerFrame();
    const int bytesPerSample = m_outputFormat.bytesPerSample();
    if (channelCount <= 0 || bytesPerFrame <= 0 || bytesPerSample <= 0 || chunk.size() < bytesPerFrame) {
        return;
    }

    qreal leftPeak = 0.0;
    qreal rightPeak = 0.0;
    const int frameCount = chunk.size() / bytesPerFrame;
    const char *data = chunk.constData();
    for (int frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
        const char *frame = data + frameIndex * bytesPerFrame;
        leftPeak = qMax(leftPeak, sampleMagnitude(frame));
        const char *rightSample = frame + bytesPerSample * (channelCount > 1 ? 1 : 0);
        rightPeak = qMax(rightPeak, sampleMagnitude(rightSample));
    }

    if (m_lastLevelEmit.isValid() && m_lastLevelEmit.elapsed() < 16) {
        return;
    }

    if (!m_lastLevelEmit.isValid()) {
        m_lastLevelEmit.start();
    } else {
        m_lastLevelEmit.restart();
    }
    emit audioLevelsChanged(m_sessionId, leftPeak, rightPeak);
}

void FfmpegDecoderWorker::drainOutput()
{
    drainOutputForSession(m_sessionId, m_process);
}

void FfmpegDecoderWorker::drainOutputForSession(int sessionId, QProcess *expectedProcess)
{
    if (!expectedProcess) {
        return;
    }

    if (expectedProcess != m_process || sessionId != m_sessionId || !m_buffer) {
        PlayerLogger::log(QStringLiteral("anomaly"),
                          QStringLiteral("stale_decoder_drain requestedSession=%1 currentSession=%2 expectedProcess=%3 currentProcess=%4 hasBuffer=%5")
                              .arg(sessionId)
                              .arg(m_sessionId)
                              .arg(reinterpret_cast<quintptr>(expectedProcess), 0, 16)
                              .arg(reinterpret_cast<quintptr>(m_process), 0, 16)
                              .arg(m_buffer != nullptr));
        discardPendingStandardOutput(QStringLiteral("stale-decoder-drain"), expectedProcess);
        return;
    }

    if (m_useCaptureFallback) {
        drainCapturedStandardOutput(sessionId, expectedProcess);
    } else {
        drainDirectStandardOutput(sessionId, expectedProcess);
    }
}

void FfmpegDecoderWorker::drainDirectStandardOutput(int sessionId, QProcess *expectedProcess)
{
    if (m_paused) {
        // Don't drain stdout while paused. The OS pipe buffer fills up
        // and ffmpeg naturally blocks on write(), preserving all decoded data.
        return;
    }
    bool appended = false;
    qint64 discardedBytes = 0;
    qint64 burstBytes = 0;

    while (expectedProcess == m_process && sessionId == m_sessionId && m_buffer
           && expectedProcess->bytesAvailable() > 0) {
        const qsizetype writableBefore = m_buffer->writableBytes();
        const qsizetype bufferedBefore = m_buffer->bufferedBytes();
        if (writableBefore <= 0) {
            const qint64 pendingBytes = m_process->bytesAvailable();
            const bool shouldLogBackpressure =
                !m_lastBackpressureLog.isValid()
                || m_lastBackpressureLog.elapsed() >= 1000
                || pendingBytes >= m_lastBackpressurePendingBytes + (4 * 1024 * 1024);
            if (shouldLogBackpressure) {
                PlayerLogger::log(QStringLiteral("anomaly"),
                                  QStringLiteral("decoder-output-backpressure session=%1 pendingStdoutBytes=%2 bufferedBytes=%3 writableBytes=0")
                                      .arg(m_sessionId)
                                      .arg(pendingBytes)
                                      .arg(bufferedBefore));
                m_lastBackpressureLog.restart();
                m_lastBackpressurePendingBytes = pendingBytes;
            }
            if (!m_drainTimer->isActive()) {
                m_drainTimer->start();
            }
            break;
        }

        const qint64 chunkSize = qMin<qint64>(writableBefore, 32768);
        const QByteArray chunk = expectedProcess->read(chunkSize);
        if (chunk.isEmpty()) {
            break;
        }

        const qint64 appendedBytes = m_buffer->appendForOwner(chunk, sessionId, m_bufferGeneration);
        if (appendedBytes > 0) {
            burstBytes += appendedBytes;
            appended = true;
            emitAudioLevelsIfNeeded(appendedBytes == chunk.size() ? chunk : chunk.first(appendedBytes));
            if (PlayerLogger::highVolumeJsonlDiagnosticsEnabled()) {
                PlayerLogger::diagnostic(QStringLiteral("decoder"),
                                         QStringLiteral("decoder_read_burst"),
                                         {
                                             {QStringLiteral("sessionId"), m_sessionId},
                                             {QStringLiteral("bufferGeneration"), static_cast<qint64>(m_bufferGeneration)},
                                             {QStringLiteral("bytesRead"), chunk.size()},
                                             {QStringLiteral("bytesAppended"), appendedBytes},
                                             {QStringLiteral("bufferedBytesBefore"), static_cast<qint64>(bufferedBefore)},
                                             {QStringLiteral("bufferedBytesAfter"), static_cast<qint64>(m_buffer->bufferedBytes())},
                                             {QStringLiteral("writableBytesBefore"), static_cast<qint64>(writableBefore)},
                                             {QStringLiteral("writableBytesAfter"), static_cast<qint64>(m_buffer->writableBytes())},
                                         });
            }
        } else if (m_buffer->isDiscardingWrites()) {
            discardedBytes += chunk.size();
        } else {
            PlayerLogger::log(QStringLiteral("anomaly"),
                              QStringLiteral("decoder-output-append-failed session=%1 bytes=%2 bufferedBytes=%3 writableBytes=%4")
                                  .arg(m_sessionId)
                                  .arg(chunk.size())
                                  .arg(m_buffer->bufferedBytes())
                                  .arg(m_buffer->writableBytes()));
            if (!m_drainTimer->isActive()) {
                m_drainTimer->start();
            }
            break;
        }
    }

    refreshStandardErrorBuffer();
    if (discardedBytes > 0) {
        PlayerLogger::log(QStringLiteral("anomaly"),
                          QStringLiteral("decoder-output-discarded session=%1 bytes=%2")
                              .arg(m_sessionId)
                              .arg(discardedBytes));
    }
    if (appended) {
        PlayerLogger::log(QStringLiteral("decoder"),
                          QStringLiteral("decoderReadBurst session=%1 bytes=%2 bufferedBytes=%3 writableBytes=%4 mode=readyRead")
                              .arg(m_sessionId)
                              .arg(burstBytes)
                              .arg(m_buffer ? m_buffer->bufferedBytes() : -1)
                              .arg(m_buffer ? m_buffer->writableBytes() : -1));
        emit dataAvailable(m_sessionId);
    }

    finishIfReady();
}

void FfmpegDecoderWorker::drainCapturedStandardOutput(int sessionId, QProcess *expectedProcess)
{
    if (m_paused) {
        return;
    }
    if (expectedProcess != m_process || sessionId != m_sessionId || !m_buffer) {
        discardPendingStandardOutput(QStringLiteral("stale-captured-drain"), expectedProcess);
        return;
    }

    bool appended = false;
    qint64 discardedBytes = 0;
    qint64 burstBytes = 0;
    QFile outputCapture(m_stdoutCapturePath);
    if (outputCapture.open(QIODevice::ReadOnly) && outputCapture.seek(m_stdoutReadOffset)) {
        while (expectedProcess == m_process && sessionId == m_sessionId && m_buffer
               && m_buffer->writableBytes() > 0) {
            const qint64 chunkSize = qMin<qint64>(m_buffer->writableBytes(), 32768);
            const QByteArray chunk = outputCapture.read(chunkSize);
            if (chunk.isEmpty()) {
                break;
            }

            m_stdoutReadOffset += chunk.size();
            const qint64 appendedBytes = m_buffer->appendForOwner(chunk, sessionId, m_bufferGeneration);
            if (appendedBytes > 0) {
                burstBytes += appendedBytes;
                emitAudioLevelsIfNeeded(appendedBytes == chunk.size() ? chunk : chunk.first(appendedBytes));
                appended = true;
            } else if (m_buffer->isDiscardingWrites()) {
                discardedBytes += chunk.size();
            }
        }
    }

    refreshStandardErrorBuffer();
    if (discardedBytes > 0) {
        PlayerLogger::log(QStringLiteral("anomaly"),
                          QStringLiteral("decoder-output-discarded session=%1 bytes=%2")
                              .arg(m_sessionId)
                              .arg(discardedBytes));
    }
    if (appended) {
        PlayerLogger::log(QStringLiteral("decoder"),
                          QStringLiteral("decoderReadBurst session=%1 bytes=%2 bufferedBytes=%3 writableBytes=%4 mode=capture-file")
                              .arg(m_sessionId)
                              .arg(burstBytes)
                              .arg(m_buffer ? m_buffer->bufferedBytes() : -1)
                              .arg(m_buffer ? m_buffer->writableBytes() : -1));
        emit dataAvailable(m_sessionId);
    }

    finishIfReady();
}

void FfmpegDecoderWorker::discardPendingStandardOutput(const QString &reason, QProcess *process)
{
    auto *targetProcess = process ? process : m_process;
    if (!targetProcess || m_useCaptureFallback) {
        return;
    }

    if (reason.startsWith(QStringLiteral("stopDecoding-")) || reason == QStringLiteral("cleanupProcess")) {
        const qint64 pendingBytes = targetProcess->bytesAvailable();
        if (pendingBytes <= 0) {
            return;
        }
        PlayerLogger::log(QStringLiteral("anomaly"),
                          QStringLiteral("decoder-drop-pending-stdout reason=%1 session=%2 bytes=%3")
                              .arg(reason)
                              .arg(m_sessionId)
                              .arg(pendingBytes));
        PlayerLogger::diagnostic(QStringLiteral("decoder"),
                                 QStringLiteral("decoder_drop_pending_stdout"),
                                 {
                                     {QStringLiteral("reason"), reason},
                                     {QStringLiteral("sessionId"), m_sessionId},
                                     {QStringLiteral("bytes"), pendingBytes},
                                 });
        return;
    }

    const QByteArray discarded = targetProcess->readAllStandardOutput();
    if (discarded.isEmpty()) {
        return;
    }

    PlayerLogger::log(QStringLiteral("anomaly"),
                      QStringLiteral("decoder-discard-pending-stdout reason=%1 session=%2 bytes=%3")
                          .arg(reason)
                          .arg(m_sessionId)
                          .arg(discarded.size()));
    PlayerLogger::diagnostic(QStringLiteral("decoder"),
                             QStringLiteral("decoder_discard_pending_stdout"),
                             {
                                 {QStringLiteral("reason"), reason},
                                 {QStringLiteral("sessionId"), m_sessionId},
                                 {QStringLiteral("bytes"), discarded.size()},
                             });
}

void FfmpegDecoderWorker::refreshStandardErrorBuffer()
{
    if (m_stderrCapturePath.isEmpty()) {
        return;
    }

    QFile standardErrorCapture(m_stderrCapturePath);
    if (!standardErrorCapture.open(QIODevice::ReadOnly)) {
        return;
    }

    const QString capturedText = QString::fromUtf8(standardErrorCapture.readAll()).trimmed();
    if (capturedText.isEmpty()) {
        return;
    }

    m_stderrBuffer = capturedText;
    if (m_stderrBuffer.size() > 4096) {
        m_stderrBuffer = m_stderrBuffer.right(4096);
    }
}

bool FfmpegDecoderWorker::hasPendingCapturedOutput() const
{
    if (!m_useCaptureFallback && m_process) {
        return m_process->bytesAvailable() > 0;
    }

    if (m_stdoutCapturePath.isEmpty()) {
        return false;
    }

    const QFileInfo outputInfo(m_stdoutCapturePath);
    return outputInfo.exists() && outputInfo.size() > m_stdoutReadOffset;
}

void FfmpegDecoderWorker::finishIfReady()
{
    if (!m_process || !m_processFinished) {
        return;
    }

    if (hasPendingCapturedOutput()) {
        return;
    }

    refreshStandardErrorBuffer();
    emit finished(m_sessionId, m_finishedExitCode, m_finishedExitStatus, m_stderrBuffer);
    cleanupProcess();
}

void FfmpegDecoderWorker::cleanupProcess()
{
    PlayerLogger::log(QStringLiteral("decoder"),
                      QStringLiteral("cleanupProcess session=%1").arg(m_sessionId));
    m_drainTimer->stop();
    if (m_process) {
        disconnect(m_process, nullptr, this, nullptr);
        discardPendingStandardOutput(QStringLiteral("cleanupProcess"), m_process);
        m_process->deleteLater();
        m_process = nullptr;
    }

    QFile::remove(m_stdoutCapturePath);
    QFile::remove(m_stderrCapturePath);
    m_stdoutCapturePath.clear();
    m_stderrCapturePath.clear();
    m_stdoutReadOffset = 0;
    m_useCaptureFallback = false;
    m_bufferGeneration = 0;
    m_buffer = nullptr;
    m_sessionId = 0;
    m_stderrBuffer.clear();
    m_processFinished = false;
    m_finishedExitCode = 0;
    m_finishedExitStatus = static_cast<int>(QProcess::NormalExit);
    m_lastBackpressureLog.invalidate();
    m_lastBackpressurePendingBytes = -1;
}
