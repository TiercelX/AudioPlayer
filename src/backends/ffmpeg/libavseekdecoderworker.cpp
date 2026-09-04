#include "libavseekdecoderworker.h"

#include "pcmseekcache.h"
#include "pcmutils.h"
#include "playerlogger.h"

#include <QElapsedTimer>
#include <QFile>
#include <QTimer>
#include <QVarLengthArray>

#include <algorithm>
#include <cmath>
#include <cstring>

#ifdef AUDIOPLAYER_LIBAV_DECODER
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/downmix_info.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}
#endif

namespace {

QString libavErrorText(int errorCode)
{
#ifdef AUDIOPLAYER_LIBAV_DECODER
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(errorCode, buffer, sizeof(buffer));
    return QString::fromUtf8(buffer);
#else
    Q_UNUSED(errorCode);
    return QStringLiteral("libav decoder is not available");
#endif
}

#ifdef AUDIOPLAYER_LIBAV_DECODER
AVSampleFormat sampleFormatForOutput(const PcmStreamFormat &format)
{
    switch (format.sampleEncoding) {
    case PcmSampleEncoding::UInt8:
        return AV_SAMPLE_FMT_U8;
    case PcmSampleEncoding::Int16:
        return AV_SAMPLE_FMT_S16;
    case PcmSampleEncoding::Int24:
    case PcmSampleEncoding::Int32:
        return AV_SAMPLE_FMT_S32;
    case PcmSampleEncoding::Float32:
        return AV_SAMPLE_FMT_FLT;
    case PcmSampleEncoding::Unknown:
        break;
    }

    return AV_SAMPLE_FMT_NONE;
}

qint64 streamTimestampFromMs(const AVStream *stream, qint64 positionMs)
{
    if (!stream) {
        return 0;
    }

    return av_rescale_q(positionMs, AVRational {1, 1000}, stream->time_base);
}

DolbyDownmixParams extractDolbyDownmixParams(const AVFrame *frame)
{
    DolbyDownmixParams params;
    params.type = DolbyDownmixType::None;

    const AVFrameSideData *sd = av_frame_get_side_data(frame, AV_FRAME_DATA_DOWNMIX_INFO);
    if (!sd || !sd->data) {
        return params;
    }

    const auto *info = reinterpret_cast<const AVDownmixInfo *>(sd->data);

    switch (info->preferred_downmix_type) {
    case AV_DOWNMIX_TYPE_LTRT:
        params.type = DolbyDownmixType::LtRt;
        break;
    case AV_DOWNMIX_TYPE_DPLII:
        params.type = DolbyDownmixType::DplII;
        break;
    case AV_DOWNMIX_TYPE_LORO:
        params.type = DolbyDownmixType::LoRo;
        break;
    default:
        params.type = DolbyDownmixType::LtRt;
        break;
    }

    params.centerMixLevel = info->center_mix_level;
    params.centerMixLevelLtRt = info->center_mix_level_ltrt;
    params.surroundMixLevel = info->surround_mix_level;
    params.surroundMixLevelLtRt = info->surround_mix_level_ltrt;
    params.lfeMixLevel = info->lfe_mix_level;

    return params;
}

bool canApplyCreativeChannelReorder(const PcmStreamFormat &format)
{
    return (format.channelCount == 6 || format.channelCount == 8)
        && format.bytesPerSample() > 0
        && format.bytesPerFrame() > 0;
}

void applyCreativeChannelReorder(QByteArray &pcm, const PcmStreamFormat &format)
{
    if (!canApplyCreativeChannelReorder(format)) {
        return;
    }

    const int bytesPerSample = format.bytesPerSample();
    const int bytesPerFrame = format.bytesPerFrame();
    const qsizetype frameCount = pcm.size() / bytesPerFrame;
    QVarLengthArray<char, 8> scratch(bytesPerSample);
    char *data = pcm.data();

    for (qsizetype frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
        char *frame = data + frameIndex * bytesPerFrame;
        for (int pairOffset = 0; pairOffset < 2; ++pairOffset) {
            char *front = frame + (2 + pairOffset) * bytesPerSample;
            char *rear = frame + (4 + pairOffset) * bytesPerSample;
            std::memcpy(scratch.data(), front, static_cast<size_t>(bytesPerSample));
            std::memcpy(front, rear, static_cast<size_t>(bytesPerSample));
            std::memcpy(rear, scratch.data(), static_cast<size_t>(bytesPerSample));
        }
    }
}

bool initOutputLayout(AVChannelLayout *layout, const PcmStreamFormat &format, QString *usedLayout)
{
    if (!layout) {
        return false;
    }

    *layout = {};
    const QString requestedLayout = format.channelLayout.trimmed();
    if (!requestedLayout.isEmpty()) {
        const QByteArray requestedLayoutUtf8 = requestedLayout.toUtf8();
        if (av_channel_layout_from_string(layout, requestedLayoutUtf8.constData()) >= 0
            && layout->nb_channels == format.channelCount) {
            if (usedLayout) {
                *usedLayout = requestedLayout;
            }
            return true;
        }

        av_channel_layout_uninit(layout);
        PlayerLogger::log(QStringLiteral("decoder"),
                          QStringLiteral("libavSeek outputLayout fallback requested=%1 channels=%2")
                              .arg(requestedLayout)
                              .arg(format.channelCount));
    }

    av_channel_layout_default(layout, format.channelCount);
    if (usedLayout) {
        *usedLayout = QStringLiteral("default:%1").arg(format.channelCount);
    }
    return layout->nb_channels == format.channelCount;
}
#endif

} // namespace

struct LibavSeekDecoderWorker::State
{
    qint64 targetTimestamp = 0;
    qint64 targetPositionMs = 0;
    int streamIndex = -1;
    int swrOutputChannelCount = 0;
    bool discardUntilTarget = false;
    bool inputFinished = false;
    bool decoderFlushed = false;
    bool downmixInfoDetected = false;
    bool downmixConfigured = false;
    bool swrConfiguredForOutput = false;
    bool creativeChannelReorderLogged = false;
#ifdef AUDIOPLAYER_LIBAV_DECODER
    AVFormatContext *formatContext = nullptr;
    AVCodecContext *codecContext = nullptr;
    SwrContext *swrContext = nullptr;
    AVPacket *packet = nullptr;
    AVFrame *frame = nullptr;
    AVRational streamTimeBase = {0, 1};
#endif
};

LibavSeekDecoderWorker::LibavSeekDecoderWorker(QObject *parent)
    : QObject(parent)
    , m_decodeTimer(new QTimer(this))
{
    m_decodeTimer->setSingleShot(true);
    m_decodeTimer->setInterval(0);
    connect(m_decodeTimer, &QTimer::timeout, this, &LibavSeekDecoderWorker::decodeStep);
}

LibavSeekDecoderWorker::~LibavSeekDecoderWorker()
{
    cleanupState();
}

void LibavSeekDecoderWorker::startDecoding(int sessionId,
                                           const QString &sourcePath,
                                           qint64 startPositionMs,
                                           PcmStreamBuffer *buffer,
                                           const PcmStreamFormat &outputFormat)
{
    if (!buffer || !outputFormat.isValid()) {
        stopDecoding(true);
        failDecoding(QStringLiteral("invalid libav seek decoder buffer or output format"));
        return;
    }

    if (isSourcePrepared(sourcePath, outputFormat)) {
        PlayerLogger::log(QStringLiteral("decoder"),
                          QStringLiteral("libavSeek startDecoding reuse session=%1 source=%2 startPositionMs=%3")
                              .arg(sessionId)
                              .arg(sourcePath)
                              .arg(startPositionMs));
        const quint64 bufferGeneration = buffer ? buffer->bufferGeneration() : 0;
        seekTo(sessionId, startPositionMs, buffer, bufferGeneration);
        return;
    }

    stopDecoding(true);
    releaseSource();

    PlayerLogger::log(QStringLiteral("decoder"),
                      QStringLiteral("libavSeek startDecoding session=%1 source=%2 startPositionMs=%3 sampleRate=%4 channels=%5 bits=%6")
                          .arg(sessionId)
                          .arg(sourcePath)
                          .arg(startPositionMs)
                          .arg(outputFormat.sampleRate)
                          .arg(outputFormat.channelCount)
                          .arg(outputFormat.bitsPerSample()));

    if (!prepareSource(sourcePath, outputFormat)) {
        failDecoding(QStringLiteral("prepareSource failed for: %1").arg(sourcePath));
        return;
    }

    const quint64 bufferGeneration = buffer ? buffer->bufferGeneration() : 0;
    seekTo(sessionId, startPositionMs, buffer, bufferGeneration);
}

void LibavSeekDecoderWorker::resetDecodeState()
{
    m_pendingPcm.clear();
    m_pendingPcmOffset = 0;
    m_finished = false;
    m_decodedBytesWritten = 0;
    if (m_state) {
        m_state->targetTimestamp = 0;
        m_state->targetPositionMs = 0;
        m_state->discardUntilTarget = false;
        m_state->inputFinished = false;
        m_state->decoderFlushed = false;
        m_state->downmixInfoDetected = false;
        m_state->downmixConfigured = false;
        m_state->swrConfiguredForOutput = false;
        m_state->creativeChannelReorderLogged = false;
    }
}

void LibavSeekDecoderWorker::stopDecoding(bool waitForFinished)
{
    Q_UNUSED(waitForFinished);

    const int sessionId = m_sessionId;
    if (m_decodeTimer) {
        m_decodeTimer->stop();
    }
    resetDecodeState();
    m_buffer = nullptr;
    m_bufferGeneration = 0;
    const bool wasFinished = m_finished;
    m_finished = false;
    m_sessionId = 0;

    if (sessionId != 0 && !wasFinished) {
        PlayerLogger::log(QStringLiteral("decoder"),
                          QStringLiteral("libavSeek stopDecoding emitFinished session=%1").arg(sessionId));
        emit finished(sessionId, 0, 0, QString());
    }
}

bool LibavSeekDecoderWorker::prepareSource(const QString &sourcePath, const PcmStreamFormat &outputFormat)
{
#ifdef AUDIOPLAYER_LIBAV_DECODER
    if (isSourcePrepared(sourcePath, outputFormat)) {
        PlayerLogger::log(QStringLiteral("decoder"),
                          QStringLiteral("libavSeek prepareSource reuse source=%1").arg(sourcePath));
        return true;
    }

    releaseSource();

    if (!outputFormat.isValid()) {
        PlayerLogger::log(QStringLiteral("decoder"),
                          QStringLiteral("libavSeek prepareSource failed: invalid output format"));
        return false;
    }

    if (!QFile::exists(sourcePath)) {
        PlayerLogger::log(QStringLiteral("decoder"),
                          QStringLiteral("libavSeek prepareSource failed: file not found: %1").arg(sourcePath));
        return false;
    }

    m_state = new State;
    int result = avformat_open_input(&m_state->formatContext, sourcePath.toUtf8().constData(), nullptr, nullptr);
    if (result < 0) {
        PlayerLogger::log(QStringLiteral("decoder"),
                          QStringLiteral("libavSeek prepareSource failed: avformat_open_input: %1").arg(libavErrorText(result)));
        cleanupState();
        return false;
    }

    result = avformat_find_stream_info(m_state->formatContext, nullptr);
    if (result < 0) {
        PlayerLogger::log(QStringLiteral("decoder"),
                          QStringLiteral("libavSeek prepareSource failed: avformat_find_stream_info: %1").arg(libavErrorText(result)));
        cleanupState();
        return false;
    }

    result = av_find_best_stream(m_state->formatContext, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (result < 0) {
        PlayerLogger::log(QStringLiteral("decoder"),
                          QStringLiteral("libavSeek prepareSource failed: audio stream not found: %1").arg(libavErrorText(result)));
        cleanupState();
        return false;
    }
    m_state->streamIndex = result;
    AVStream *stream = m_state->formatContext->streams[m_state->streamIndex];
    m_state->streamTimeBase = stream->time_base;

    const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        PlayerLogger::log(QStringLiteral("decoder"),
                          QStringLiteral("libavSeek prepareSource failed: audio decoder not found"));
        cleanupState();
        return false;
    }

    m_state->codecContext = avcodec_alloc_context3(codec);
    if (!m_state->codecContext) {
        PlayerLogger::log(QStringLiteral("decoder"),
                          QStringLiteral("libavSeek prepareSource failed: avcodec_alloc_context3"));
        cleanupState();
        return false;
    }

    result = avcodec_parameters_to_context(m_state->codecContext, stream->codecpar);
    if (result < 0) {
        PlayerLogger::log(QStringLiteral("decoder"),
                          QStringLiteral("libavSeek prepareSource failed: avcodec_parameters_to_context: %1").arg(libavErrorText(result)));
        cleanupState();
        return false;
    }

    result = avcodec_open2(m_state->codecContext, codec, nullptr);
    if (result < 0) {
        PlayerLogger::log(QStringLiteral("decoder"),
                          QStringLiteral("libavSeek prepareSource failed: avcodec_open2: %1").arg(libavErrorText(result)));
        cleanupState();
        return false;
    }

    AVChannelLayout outputLayout;
    QString usedOutputLayout;
    if (!initOutputLayout(&outputLayout, outputFormat, &usedOutputLayout)) {
        PlayerLogger::log(QStringLiteral("decoder"),
                          QStringLiteral("libavSeek prepareSource failed: invalid output layout channels=%1 layout=%2")
                              .arg(outputFormat.channelCount)
                              .arg(outputFormat.channelLayout));
        cleanupState();
        return false;
    }
    const AVSampleFormat outputSampleFormat = sampleFormatForOutput(outputFormat);
    if (outputSampleFormat == AV_SAMPLE_FMT_NONE) {
        av_channel_layout_uninit(&outputLayout);
        PlayerLogger::log(QStringLiteral("decoder"),
                          QStringLiteral("libavSeek prepareSource failed: unsupported output sample format"));
        cleanupState();
        return false;
    }

    m_state->swrOutputChannelCount = outputFormat.channelCount;

    result = swr_alloc_set_opts2(&m_state->swrContext,
                                 &outputLayout,
                                 outputSampleFormat,
                                 outputFormat.sampleRate,
                                 &m_state->codecContext->ch_layout,
                                 m_state->codecContext->sample_fmt,
                                 m_state->codecContext->sample_rate,
                                 0,
                                 nullptr);
    av_channel_layout_uninit(&outputLayout);
    if (result < 0 || !m_state->swrContext) {
        PlayerLogger::log(QStringLiteral("decoder"),
                          QStringLiteral("libavSeek prepareSource failed: swr_alloc_set_opts2: %1").arg(libavErrorText(result)));
        cleanupState();
        return false;
    }

    result = swr_init(m_state->swrContext);
    if (result < 0) {
        PlayerLogger::log(QStringLiteral("decoder"),
                          QStringLiteral("libavSeek prepareSource failed: swr_init: %1").arg(libavErrorText(result)));
        cleanupState();
        return false;
    }

    m_state->packet = av_packet_alloc();
    m_state->frame = av_frame_alloc();
    if (!m_state->packet || !m_state->frame) {
        PlayerLogger::log(QStringLiteral("decoder"),
                          QStringLiteral("libavSeek prepareSource failed: packet/frame allocation"));
        cleanupState();
        return false;
    }

    m_preparedSourcePath = sourcePath;
    m_preparedOutputFormat = outputFormat;
    m_outputFormat = outputFormat;

    PlayerLogger::log(QStringLiteral("decoder"),
                      QStringLiteral("libavSeek prepareSource ok source=%1 sampleRate=%2 channels=%3 bits=%4 layout=%5")
                          .arg(sourcePath)
                          .arg(outputFormat.sampleRate)
                          .arg(outputFormat.channelCount)
                          .arg(outputFormat.bitsPerSample())
                          .arg(usedOutputLayout));
    return true;
#else
    Q_UNUSED(sourcePath);
    Q_UNUSED(outputFormat);
    return false;
#endif
}

void LibavSeekDecoderWorker::seekTo(int sessionId, qint64 startPositionMs, PcmStreamBuffer *buffer, quint64 bufferGeneration)
{
#ifdef AUDIOPLAYER_LIBAV_DECODER
    if (!m_state || !m_state->formatContext || !m_state->codecContext) {
        PlayerLogger::log(QStringLiteral("decoder"),
                          QStringLiteral("libavSeek seekTo failed: source not prepared session=%1").arg(sessionId));
        if (buffer) {
            emit errorOccurred(sessionId, QStringLiteral("source not prepared for seek"));
        }
        return;
    }

    if (m_decodeTimer) {
        m_decodeTimer->stop();
    }

    m_sessionId = sessionId;
    m_buffer = buffer;
    m_bufferGeneration = bufferGeneration;
    m_finished = false;
    m_pendingPcm.clear();
    m_pendingPcmOffset = 0;

    m_dolbyDownmix = DolbyDownmixProcessor();
    m_state->downmixInfoDetected = false;
    m_state->downmixConfigured = false;
    m_state->swrConfiguredForOutput = false;
    m_decodedBytesWritten = 0;

    const qint64 seekTimestamp = streamTimestampFromMs(
        m_state->formatContext->streams[m_state->streamIndex], startPositionMs);
    m_state->targetTimestamp = seekTimestamp;
    m_state->targetPositionMs = startPositionMs;
    m_state->discardUntilTarget = startPositionMs > 0;
    m_state->inputFinished = false;
    m_state->decoderFlushed = false;

    if (m_seekCache && startPositionMs > 0) {
        auto hit = m_seekCache->findHit(startPositionMs, 1000);
        if (hit.valid) {
            QByteArray cached = m_seekCache->readSegment(hit);
            if (!cached.isEmpty()) {
                const qint64 bytesPerSecond = static_cast<qint64>(m_outputFormat.bytesPerFrame()) * m_outputFormat.sampleRate;
                const qint64 cachedDurationMs = bytesPerSecond > 0 ? cached.size() * 1000 / bytesPerSecond : 0;
                PlayerLogger::log(QStringLiteral("decoder"),
                                  QStringLiteral("libavSeek seekTo cacheHit session=%1 targetMs=%2 cacheStartMs=%3 cacheDurationMs=%4 cacheBytes=%5")
                                      .arg(sessionId)
                                      .arg(startPositionMs)
                                      .arg(hit.positionMs)
                                      .arg(cachedDurationMs)
                                      .arg(cached.size()));
                if (buffer) {
                    buffer->appendForOwner(cached, sessionId, bufferGeneration);
                }
                m_decodedBytesWritten = cached.size();
                const qint64 resumeMs = hit.positionMs + cachedDurationMs;
                const qint64 resumeTimestamp = streamTimestampFromMs(
                    m_state->formatContext->streams[m_state->streamIndex], resumeMs);
                m_state->targetTimestamp = resumeTimestamp;
                m_state->targetPositionMs = resumeMs;
                m_state->discardUntilTarget = resumeMs > 0;

                int result = av_seek_frame(m_state->formatContext, m_state->streamIndex, resumeTimestamp, AVSEEK_FLAG_BACKWARD);
                if (result < 0) {
                    PlayerLogger::log(QStringLiteral("decoder"),
                                      QStringLiteral("libavSeek seekTo cacheResumeSeekFailed session=%1 fallbackToFullSeek").arg(sessionId));
                    m_state->targetTimestamp = seekTimestamp;
                    m_state->targetPositionMs = startPositionMs;
                    m_state->discardUntilTarget = startPositionMs > 0;
                    m_decodedBytesWritten = 0;
                } else {
                    avcodec_flush_buffers(m_state->codecContext);
                    m_decodeTimer->start(0);
                    emit dataAvailable(sessionId);
                    return;
                }
            }
        }
    }

    {
        const int seekResult = av_seek_frame(m_state->formatContext, m_state->streamIndex, seekTimestamp, AVSEEK_FLAG_BACKWARD);
        if (seekResult < 0) {
            if (startPositionMs > 0) {
                PlayerLogger::log(QStringLiteral("decoder"),
                                  QStringLiteral("libavSeek seekTo failed: av_seek_frame: %1 session=%2").arg(libavErrorText(seekResult)).arg(sessionId));
                emit errorOccurred(sessionId, QStringLiteral("av_seek_frame failed: %1").arg(libavErrorText(seekResult)));
                resetDecodeState();
                m_buffer = nullptr;
                m_bufferGeneration = 0;
                m_sessionId = 0;
                return;
            }
            PlayerLogger::log(QStringLiteral("decoder"),
                              QStringLiteral("libavSeek seekTo start-seek-failed session=%1 error=%2 continuing")
                                  .arg(sessionId)
                                  .arg(libavErrorText(seekResult)));
        } else {
            avcodec_flush_buffers(m_state->codecContext);
        }
    }

    PlayerLogger::log(QStringLiteral("decoder"),
                      QStringLiteral("libavSeek seekTo session=%1 startPositionMs=%2 sampleRate=%3 channels=%4 bits=%5")
                          .arg(sessionId)
                          .arg(startPositionMs)
                          .arg(m_outputFormat.sampleRate)
                          .arg(m_outputFormat.channelCount)
                          .arg(m_outputFormat.bitsPerSample()));

    m_decodeTimer->start(0);
#else
    Q_UNUSED(sessionId);
    Q_UNUSED(startPositionMs);
    Q_UNUSED(buffer);
    Q_UNUSED(bufferGeneration);
#endif
}

void LibavSeekDecoderWorker::releaseSource()
{
    cleanupState();
    m_preparedSourcePath.clear();
    m_preparedOutputFormat = {};
}

bool LibavSeekDecoderWorker::isSourcePrepared(const QString &sourcePath, const PcmStreamFormat &outputFormat) const
{
    return m_state != nullptr
        && !m_preparedSourcePath.isEmpty()
        && m_preparedSourcePath == sourcePath
        && m_preparedOutputFormat == outputFormat;
}

void LibavSeekDecoderWorker::setSeekCache(PcmSeekCache *cache)
{
    m_seekCache = cache;
}

void LibavSeekDecoderWorker::setCreativeChannelReorderEnabled(bool enabled)
{
    if (m_creativeChannelReorderEnabled != enabled) {
        m_creativeChannelReorderEnabled = enabled;
        m_preparedSourcePath.clear();
    }
}

void LibavSeekDecoderWorker::decodeStep()
{
#ifndef AUDIOPLAYER_LIBAV_DECODER
    failDecoding(QStringLiteral("libav decoder is not compiled in"));
#else
    if (!m_state || !m_buffer || m_finished) {
        return;
    }

    QElapsedTimer budgetTimer;
    budgetTimer.start();
    bool appended = false;

    while (m_state && m_buffer && !m_finished && budgetTimer.elapsed() < 48) {
        if (!flushPendingPcm()) {
            m_decodeTimer->start(5);
            return;
        }

        int result = 0;
        if (!m_state->inputFinished) {
            result = av_read_frame(m_state->formatContext, m_state->packet);
            if (result == AVERROR_EOF) {
                m_state->inputFinished = true;
                result = avcodec_send_packet(m_state->codecContext, nullptr);
                if (result < 0 && result != AVERROR_EOF) {
                    failDecoding(QStringLiteral("avcodec_send_packet flush failed: %1").arg(libavErrorText(result)));
                    return;
                }
            } else if (result < 0) {
                failDecoding(QStringLiteral("av_read_frame failed: %1").arg(libavErrorText(result)));
                return;
            } else if (m_state->packet->stream_index == m_state->streamIndex) {
                result = avcodec_send_packet(m_state->codecContext, m_state->packet);
                av_packet_unref(m_state->packet);
                if (result < 0 && result != AVERROR(EAGAIN)) {
                    failDecoding(QStringLiteral("avcodec_send_packet failed: %1").arg(libavErrorText(result)));
                    return;
                }
            } else {
                av_packet_unref(m_state->packet);
                continue;
            }
        }

        while (true) {
            result = avcodec_receive_frame(m_state->codecContext, m_state->frame);
            if (result == AVERROR(EAGAIN)) {
                break;
            }
            if (result == AVERROR_EOF) {
                finishDecoding(0, 0);
                return;
            }
            if (result < 0) {
                failDecoding(QStringLiteral("avcodec_receive_frame failed: %1").arg(libavErrorText(result)));
                return;
            }

            int inputSamples = m_state->frame->nb_samples;
            const int inputChannelCount = m_state->codecContext->ch_layout.nb_channels;

            if (!m_state->downmixInfoDetected) {
                m_state->downmixInfoDetected = true;
                DolbyDownmixParams downmixParams = extractDolbyDownmixParams(m_state->frame);
                if (downmixParams.type != DolbyDownmixType::None
                    && inputChannelCount > m_outputFormat.channelCount
                    && m_outputFormat.channelCount == 2) {
                    if (m_dolbyDownmix.configure(downmixParams, inputChannelCount, 2)) {
                        m_state->downmixConfigured = true;
                        PlayerLogger::log(QStringLiteral("decoder"),
                                          QStringLiteral("dolbyDownmix activated type=%1 cmix=%2 smix=%3 lfeMix=%4 inputCh=%5 outputCh=%6")
                                              .arg(static_cast<int>(downmixParams.type))
                                              .arg(downmixParams.centerMixLevel, 0, 'f', 3)
                                              .arg(downmixParams.surroundMixLevel, 0, 'f', 3)
                                              .arg(downmixParams.lfeMixLevel, 0, 'f', 3)
                                              .arg(inputChannelCount)
                                              .arg(m_outputFormat.channelCount));
                    }
                }

                if (m_state->downmixConfigured) {
                    // Reconfigure SWR to output the full input channel count so
                    // DolbyDownmixProcessor can handle the multi-channel → stereo conversion.
                    // SWR only performs sample format/rate conversion; channel mixing is
                    // delegated entirely to DolbyDownmixProcessor with proper Dolby coefficients.
                    const AVSampleFormat outputSampleFormat = sampleFormatForOutput(m_outputFormat);
                    SwrContext *newSwr = nullptr;
                    int reResult = swr_alloc_set_opts2(&newSwr,
                                                       &m_state->codecContext->ch_layout,
                                                       outputSampleFormat,
                                                       m_outputFormat.sampleRate,
                                                       &m_state->codecContext->ch_layout,
                                                       m_state->codecContext->sample_fmt,
                                                       m_state->codecContext->sample_rate,
                                                       0, nullptr);
                    if (reResult >= 0 && newSwr) {
                        reResult = swr_init(newSwr);
                        if (reResult >= 0) {
                            swr_free(&m_state->swrContext);
                            m_state->swrContext = newSwr;
                            m_state->swrOutputChannelCount = inputChannelCount;
                        } else {
                            swr_free(&newSwr);
                        }
                    }
                }

                if (!m_state->downmixConfigured
                    && inputChannelCount != m_outputFormat.channelCount) {
                    AVChannelLayout targetLayout;
                    QString usedTargetLayout;
                    if (!initOutputLayout(&targetLayout, m_outputFormat, &usedTargetLayout)) {
                        av_frame_unref(m_state->frame);
                        failDecoding(QStringLiteral("invalid libav output layout"));
                        return;
                    }
                    const AVSampleFormat outputSampleFormat = sampleFormatForOutput(m_outputFormat);
                    SwrContext *newSwr = nullptr;
                    int reResult = swr_alloc_set_opts2(&newSwr,
                                                       &targetLayout,
                                                       outputSampleFormat,
                                                       m_outputFormat.sampleRate,
                                                       &m_state->codecContext->ch_layout,
                                                       m_state->codecContext->sample_fmt,
                                                       m_state->codecContext->sample_rate,
                                                       0,
                                                       nullptr);
                    av_channel_layout_uninit(&targetLayout);
                    if (reResult >= 0 && newSwr) {
                        reResult = swr_init(newSwr);
                        if (reResult >= 0) {
                            swr_free(&m_state->swrContext);
                            m_state->swrContext = newSwr;
                            m_state->swrOutputChannelCount = m_outputFormat.channelCount;
                            m_state->swrConfiguredForOutput = true;
                            PlayerLogger::log(QStringLiteral("decoder"),
                                              QStringLiteral("libavSeek reconfigured output layout=%1 channels=%2")
                                                  .arg(usedTargetLayout)
                                                  .arg(m_outputFormat.channelCount));
                        } else {
                            swr_free(&newSwr);
                        }
                    }
                }
            }

            int discardInputSamples = 0;
            if (m_state->discardUntilTarget && inputSamples > 0) {
                qint64 frameTimestamp = m_state->frame->best_effort_timestamp;
                if (frameTimestamp == AV_NOPTS_VALUE) {
                    frameTimestamp = m_state->frame->pts;
                }
                if (frameTimestamp != AV_NOPTS_VALUE && m_state->codecContext->sample_rate > 0) {
                    const qint64 frameDuration = av_rescale_q(inputSamples,
                                                              AVRational {1, m_state->codecContext->sample_rate},
                                                              m_state->streamTimeBase);
                    const qint64 frameEndTimestamp = frameTimestamp + frameDuration;
                    if (frameEndTimestamp <= m_state->targetTimestamp) {
                        PlayerLogger::diagnostic(QStringLiteral("decoder"),
                                                 QStringLiteral("libav_seek_target_discard"),
                                                 {
                                                     {QStringLiteral("sessionId"), m_sessionId},
                                                     {QStringLiteral("targetPositionMs"), m_state->targetPositionMs},
                                                     {QStringLiteral("discardedSamples"), inputSamples},
                                                     {QStringLiteral("frameTimestamp"), frameTimestamp},
                                                     {QStringLiteral("targetTimestamp"), m_state->targetTimestamp},
                                                 });
                        av_frame_unref(m_state->frame);
                        continue;
                    }

                    if (frameTimestamp < m_state->targetTimestamp) {
                        const qint64 discardSamples =
                            av_rescale_q(m_state->targetTimestamp - frameTimestamp,
                                         m_state->streamTimeBase,
                                         AVRational {m_state->codecContext->sample_rate, 1});
                        discardInputSamples =
                            static_cast<int>(std::clamp<qint64>(discardSamples, 0, inputSamples));
                    }
                    m_state->discardUntilTarget = false;
                } else {
                    PlayerLogger::diagnostic(QStringLiteral("decoder"),
                                             QStringLiteral("libav_seek_target_discard_unavailable"),
                                             {
                                                 {QStringLiteral("sessionId"), m_sessionId},
                                                 {QStringLiteral("targetPositionMs"), m_state->targetPositionMs},
                                                 {QStringLiteral("frameTimestamp"), frameTimestamp},
                                                 {QStringLiteral("sampleRate"), m_state->codecContext->sample_rate},
                                             });
                    m_state->discardUntilTarget = false;
                }
            }

            if (discardInputSamples >= inputSamples) {
                av_frame_unref(m_state->frame);
                continue;
            }

            const int inputBytesPerSample = av_get_bytes_per_sample(m_state->codecContext->sample_fmt);
            QVarLengthArray<const uint8_t *, 8> inputData;
            if (discardInputSamples > 0 && inputChannelCount > 0 && inputBytesPerSample > 0) {
                const bool planarInput =
                    av_sample_fmt_is_planar(m_state->codecContext->sample_fmt) != 0;
                const int inputPlaneCount = planarInput ? inputChannelCount : 1;
                inputData.resize(inputPlaneCount);
                for (int plane = 0; plane < inputPlaneCount; ++plane) {
                    const uint8_t *planeData = m_state->frame->extended_data[plane];
                    const qsizetype byteOffset = planarInput
                        ? static_cast<qsizetype>(discardInputSamples) * inputBytesPerSample
                        : static_cast<qsizetype>(discardInputSamples) * inputBytesPerSample * inputChannelCount;
                    inputData[plane] = planeData ? planeData + byteOffset : nullptr;
                }
                inputSamples -= discardInputSamples;
                PlayerLogger::diagnostic(QStringLiteral("decoder"),
                                         QStringLiteral("libav_seek_target_discard"),
                                         {
                                             {QStringLiteral("sessionId"), m_sessionId},
                                             {QStringLiteral("targetPositionMs"), m_state->targetPositionMs},
                                             {QStringLiteral("discardedSamples"), discardInputSamples},
                                             {QStringLiteral("remainingSamples"), inputSamples},
                                         });
            } else {
                const int inputPlaneCount =
                    av_sample_fmt_is_planar(m_state->codecContext->sample_fmt) != 0
                    ? qMax(1, inputChannelCount)
                    : 1;
                inputData.resize(inputPlaneCount);
                for (int plane = 0; plane < inputPlaneCount; ++plane) {
                    inputData[plane] = m_state->frame->extended_data[plane];
                }
            }

            const int swrOutputChannels = m_state->swrOutputChannelCount;
            const int finalOutputChannels = m_outputFormat.channelCount;
            const int swrBytesPerFrame = swrOutputChannels * m_outputFormat.bytesPerSample();
            const int finalBytesPerFrame = m_outputFormat.bytesPerFrame();
            const int outputSamples = swr_get_out_samples(m_state->swrContext, inputSamples) + 256;
            if (outputSamples <= 0 || swrBytesPerFrame <= 0) {
                av_frame_unref(m_state->frame);
                continue;
            }

            QByteArray swrOutput(outputSamples * swrBytesPerFrame, Qt::Uninitialized);
            auto *swrData = reinterpret_cast<uint8_t *>(swrOutput.data());
            const int convertedSamples = swr_convert(m_state->swrContext,
                                                     &swrData,
                                                     outputSamples,
                                                     inputData.data(),
                                                     inputSamples);
            av_frame_unref(m_state->frame);
            if (convertedSamples < 0) {
                failDecoding(QStringLiteral("swr_convert failed: %1").arg(libavErrorText(convertedSamples)));
                return;
            }

            QByteArray converted;
            if (m_dolbyDownmix.isActive() && swrOutputChannels != finalOutputChannels) {
                converted.resize(convertedSamples * finalBytesPerFrame);
                m_dolbyDownmix.processFloat32(
                    reinterpret_cast<const float *>(swrOutput.constData()),
                    reinterpret_cast<float *>(converted.data()),
                    convertedSamples);
            } else {
                converted = swrOutput;
                converted.resize(convertedSamples * finalBytesPerFrame);
            }
            if (m_creativeChannelReorderEnabled
                && swrOutputChannels == finalOutputChannels
                && canApplyCreativeChannelReorder(m_outputFormat)) {
                applyCreativeChannelReorder(converted, m_outputFormat);
                if (!m_state->creativeChannelReorderLogged) {
                    m_state->creativeChannelReorderLogged = true;
                    PlayerLogger::log(QStringLiteral("decoder"),
                                      QStringLiteral("libavSeek creativeChannelReorder postSWR channels=%1 sampleRate=%2")
                                          .arg(m_outputFormat.channelCount)
                                          .arg(m_outputFormat.sampleRate));
                }
            }
            if (!converted.isEmpty()) {
                appended = true;
                if (!appendPcm(converted)) {
                    m_decodeTimer->start(5);
                    return;
                }
            }
        }
    }

    if (appended) {
        emit dataAvailable(m_sessionId);
    }
    if (m_state && !m_finished) {
        m_decodeTimer->start(0);
    }
#endif
}

void LibavSeekDecoderWorker::cleanupState()
{
#ifdef AUDIOPLAYER_LIBAV_DECODER
    if (!m_state) {
        return;
    }
    if (m_state->packet) {
        av_packet_free(&m_state->packet);
    }
    if (m_state->frame) {
        av_frame_free(&m_state->frame);
    }
    if (m_state->swrContext) {
        swr_free(&m_state->swrContext);
    }
    if (m_state->codecContext) {
        avcodec_free_context(&m_state->codecContext);
    }
    if (m_state->formatContext) {
        avformat_close_input(&m_state->formatContext);
    }
#endif
    delete m_state;
    m_state = nullptr;
}

void LibavSeekDecoderWorker::finishDecoding(int exitCode, int exitStatus, const QString &message)
{
    if (m_finished) {
        return;
    }

    const int sessionId = m_sessionId;
    m_finished = true;
    if (m_decodeTimer) {
        m_decodeTimer->stop();
    }
    if (m_buffer) {
        m_buffer->setEndOfStream(true);
    }
    PlayerLogger::log(QStringLiteral("decoder"),
                      QStringLiteral("libavSeek finished session=%1 exitCode=%2 exitStatus=%3 message=%4")
                          .arg(sessionId)
                          .arg(exitCode)
                          .arg(exitStatus)
                          .arg(message));
    emit finished(sessionId, exitCode, exitStatus, message);
    resetDecodeState();
    m_buffer = nullptr;
    m_bufferGeneration = 0;
    m_sessionId = 0;
}

void LibavSeekDecoderWorker::failDecoding(const QString &message)
{
    const int sessionId = m_sessionId;
    PlayerLogger::log(QStringLiteral("decoder"),
                      QStringLiteral("libavSeek error session=%1 message=%2").arg(sessionId).arg(message));
    m_finished = true;
    if (m_decodeTimer) {
        m_decodeTimer->stop();
    }
    emit errorOccurred(sessionId, message);
    resetDecodeState();
    m_buffer = nullptr;
    m_bufferGeneration = 0;
    m_sessionId = 0;
}

bool LibavSeekDecoderWorker::flushPendingPcm()
{
    if (m_pendingPcm.isEmpty()) {
        return true;
    }

    if (!m_buffer || m_pendingPcmOffset >= m_pendingPcm.size()) {
        m_pendingPcm.clear();
        m_pendingPcmOffset = 0;
        return true;
    }

    const QByteArray remaining =
        m_pendingPcm.mid(m_pendingPcmOffset, m_pendingPcm.size() - m_pendingPcmOffset);
    const qint64 appendedBytes =
        m_buffer->appendForOwner(remaining, m_sessionId, m_bufferGeneration);
    if (appendedBytes <= 0) {
        return false;
    }

    m_pendingPcmOffset += appendedBytes;
    emitAudioLevelsIfNeeded(remaining.first(appendedBytes));
    if (m_pendingPcmOffset >= m_pendingPcm.size()) {
        m_pendingPcm.clear();
        m_pendingPcmOffset = 0;
    } else {
        emit dataAvailable(m_sessionId);
        return false;
    }
    emit dataAvailable(m_sessionId);
    return true;
}

bool LibavSeekDecoderWorker::appendPcm(const QByteArray &pcm)
{
    if (!m_buffer || pcm.isEmpty()) {
        return false;
    }

    if (!m_pendingPcm.isEmpty()) {
        m_pendingPcm.append(pcm);
        emit dataAvailable(m_sessionId);
        return false;
    }

    const qint64 appendedBytes = m_buffer->appendForOwner(pcm, m_sessionId, m_bufferGeneration);
    if (appendedBytes <= 0) {
        m_pendingPcm.append(pcm);
        return false;
    }

    const QByteArray appendedPcm = pcm.first(appendedBytes);
    emitAudioLevelsIfNeeded(appendedPcm);
    if (PlayerLogger::highVolumeJsonlDiagnosticsEnabled()) {
        PlayerLogger::diagnostic(QStringLiteral("decoder"),
                                 QStringLiteral("libav_decoder_read_burst"),
                                 {
                                     {QStringLiteral("sessionId"), m_sessionId},
                                     {QStringLiteral("bufferGeneration"), static_cast<qint64>(m_bufferGeneration)},
                                     {QStringLiteral("bytesAppended"), appendedBytes},
                                     {QStringLiteral("bufferedBytesAfter"), static_cast<qint64>(m_buffer->bufferedBytes())},
                                     {QStringLiteral("writableBytesAfter"), static_cast<qint64>(m_buffer->writableBytes())},
                                 });
    }

    if (m_seekCache && appendedBytes > 0) {
        const qint64 bytesPerSecond =
            static_cast<qint64>(m_outputFormat.bytesPerFrame()) * m_outputFormat.sampleRate;
        const qint64 segmentPositionMs = bytesPerSecond > 0
            ? m_state->targetPositionMs + m_decodedBytesWritten * 1000 / bytesPerSecond
            : m_state->targetPositionMs;
        m_seekCache->writeSegment(segmentPositionMs, appendedPcm, m_outputFormat);
        m_decodedBytesWritten += appendedBytes;
    }

    if (appendedBytes < pcm.size()) {
        m_pendingPcm.append(pcm.mid(appendedBytes));
        emit dataAvailable(m_sessionId);
        return false;
    }

    return true;
}

void LibavSeekDecoderWorker::emitAudioLevelsIfNeeded(const QByteArray &chunk)
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
        leftPeak = std::max(leftPeak, sampleMagnitude(frame));
        const char *rightSample = frame + bytesPerSample * (channelCount > 1 ? 1 : 0);
        rightPeak = std::max(rightPeak, sampleMagnitude(rightSample));
    }

    emit audioLevelsChanged(m_sessionId, leftPeak, rightPeak);
}

qreal LibavSeekDecoderWorker::sampleMagnitude(const char *sampleData) const
{
    return PcmUtils::sampleMagnitude(m_outputFormat.sampleEncoding, sampleData);
}
