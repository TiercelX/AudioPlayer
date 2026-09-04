#include "windowsasioaudioplayer_formats.h"

#include "audioutils.h"
#include "ffmpegpcmshared.h"

#include <QAudioFormat>
#include <QList>
#include <QString>

#include <cstring>

#include <QtEndian>

namespace AsioFormats {

namespace {

PcmSampleEncoding toPcmEncoding(QAudioFormat::SampleFormat fmt)
{
    switch (fmt) {
    case QAudioFormat::UInt8:
        return PcmSampleEncoding::UInt8;
    case QAudioFormat::Int16:
        return PcmSampleEncoding::Int16;
    case QAudioFormat::Int32:
        return PcmSampleEncoding::Int32;
    case QAudioFormat::Float:
        return PcmSampleEncoding::Float32;
    default:
        return PcmSampleEncoding::Unknown;
    }
}

} // namespace

QString pcmCodecName(QAudioFormat::SampleFormat sampleFormat)
{
    return AudioUtils::pcmCodecName(toPcmEncoding(sampleFormat));
}

QString pcmSampleFormatName(QAudioFormat::SampleFormat sampleFormat)
{
    return AudioUtils::pcmSampleFormatName(toPcmEncoding(sampleFormat));
}

QString pcmMuxerName(QAudioFormat::SampleFormat sampleFormat)
{
    return AudioUtils::pcmMuxerName(toPcmEncoding(sampleFormat));
}

PcmStreamFormat pcmStreamFormatFromQAudioFormat(const QAudioFormat &format)
{
    PcmStreamFormat pcmFormat;
    pcmFormat.sampleRate = format.sampleRate();
    pcmFormat.channelCount = format.channelCount();
    switch (format.sampleFormat()) {
    case QAudioFormat::UInt8:
        pcmFormat.sampleEncoding = PcmSampleEncoding::UInt8;
        pcmFormat.validBitsPerSample = 8;
        break;
    case QAudioFormat::Int16:
        pcmFormat.sampleEncoding = PcmSampleEncoding::Int16;
        pcmFormat.validBitsPerSample = 16;
        break;
    case QAudioFormat::Int32:
        pcmFormat.sampleEncoding = PcmSampleEncoding::Int32;
        pcmFormat.validBitsPerSample = 32;
        break;
    case QAudioFormat::Float:
        pcmFormat.sampleEncoding = PcmSampleEncoding::Float32;
        pcmFormat.validBitsPerSample = 32;
        break;
    case QAudioFormat::Unknown:
    case QAudioFormat::NSampleFormats:
        break;
    }
    return pcmFormat;
}

void appendUniqueSampleRate(QList<int> *rates, int sampleRate)
{
    if (!rates || sampleRate <= 0 || rates->contains(sampleRate)) {
        return;
    }
    rates->append(sampleRate);
}

QList<int> sourcePreferredSampleRateCandidates(int requestedSampleRate, int fallbackSampleRate)
{
    QList<int> rates;
    appendUniqueSampleRate(&rates, requestedSampleRate);

    const QList<int> commonRatesDescending {
        768000,
        705600,
        384000,
        352800,
        320000,
        282240,
        256000,
        192000,
        176400,
        128000,
        96000,
        88200,
        64000,
        48000,
        44100,
        32000,
        22050,
        16000,
        11025,
    };
    if (requestedSampleRate > 0) {
        for (const int rate : commonRatesDescending) {
            if (rate < requestedSampleRate) {
                appendUniqueSampleRate(&rates, rate);
            }
        }
        for (auto it = commonRatesDescending.crbegin(); it != commonRatesDescending.crend(); ++it) {
            if (*it > requestedSampleRate) {
                appendUniqueSampleRate(&rates, *it);
            }
        }
    } else {
        appendUniqueSampleRate(&rates, fallbackSampleRate);
        appendUniqueSampleRate(&rates, 44100);
        appendUniqueSampleRate(&rates, 96000);
        appendUniqueSampleRate(&rates, 192000);
    }

    appendUniqueSampleRate(&rates, fallbackSampleRate);
    return rates;
}

qreal sampleMagnitude(const char *sampleData, QAudioFormat::SampleFormat sampleFormat)
{
    switch (sampleFormat) {
    case QAudioFormat::Int16: {
        const qint16 value = qFromLittleEndian<qint16>(sampleData);
        return std::abs(static_cast<int>(value)) / 32768.0;
    }
    case QAudioFormat::Int32: {
        const qint32 value = qFromLittleEndian<qint32>(sampleData);
        return qMin<qreal>(1.0, std::abs(static_cast<double>(value)) / 2147483648.0);
    }
    case QAudioFormat::Float: {
        float value = 0.0f;
        std::memcpy(&value, sampleData, sizeof(value));
        return qMin<qreal>(1.0, std::abs(static_cast<double>(value)));
    }
    case QAudioFormat::UInt8: {
        quint8 value = 0;
        std::memcpy(&value, sampleData, sizeof(value));
        return std::abs(static_cast<int>(value) - 128) / 127.0;
    }
    case QAudioFormat::Unknown:
    case QAudioFormat::NSampleFormats:
        break;
    }
    return 0.0;
}

} // namespace AsioFormats
