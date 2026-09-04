#ifndef PCMUTILS_H
#define PCMUTILS_H

#include <QAudioFormat>
#include <QtEndian>
#include <QtGlobal>

#include <cmath>
#include <cstring>
#include <limits>

enum class PcmSampleEncoding {
    Unknown,
    UInt8,
    Int16,
    Int24,
    Int32,
    Float32,
};

namespace PcmUtils {

inline qint32 readInt24Sample(const char *sampleData)
{
    const quint32 b0 = static_cast<quint8>(sampleData[0]);
    const quint32 b1 = static_cast<quint8>(sampleData[1]);
    const quint32 b2 = static_cast<quint8>(sampleData[2]);
    quint32 value = b0 | (b1 << 8) | (b2 << 16);
    if (value & 0x00800000u) {
        value |= 0xFF000000u;
    }
    return static_cast<qint32>(value);
}

inline PcmSampleEncoding fromQAudioSampleFormat(QAudioFormat::SampleFormat sf)
{
    switch (sf) {
    case QAudioFormat::UInt8:
        return PcmSampleEncoding::UInt8;
    case QAudioFormat::Int16:
        return PcmSampleEncoding::Int16;
    case QAudioFormat::Int32:
        return PcmSampleEncoding::Int32;
    case QAudioFormat::Float:
        return PcmSampleEncoding::Float32;
    case QAudioFormat::Unknown:
    case QAudioFormat::NSampleFormats:
        break;
    }
    return PcmSampleEncoding::Unknown;
}

inline void applyGainToSample(PcmSampleEncoding encoding, char *sampleData, qreal gain)
{
    switch (encoding) {
    case PcmSampleEncoding::UInt8: {
        quint8 value = static_cast<quint8>(*reinterpret_cast<unsigned char *>(sampleData));
        const int centered = static_cast<int>(value) - 128;
        const int scaled = qBound(-128,
                                  qRound(static_cast<qreal>(centered) * gain),
                                  127);
        *reinterpret_cast<unsigned char *>(sampleData) = static_cast<unsigned char>(scaled + 128);
        break;
    }
    case PcmSampleEncoding::Int16: {
        const qint16 value = qFromLittleEndian<qint16>(sampleData);
        const qint16 scaled = static_cast<qint16>(qBound(-32768,
                                                         qRound(static_cast<qreal>(value) * gain),
                                                         32767));
        qToLittleEndian<qint16>(scaled, sampleData);
        break;
    }
    case PcmSampleEncoding::Int24: {
        const qint32 value = readInt24Sample(sampleData);
        const qint32 scaled = static_cast<qint32>(qBound<qint64>(static_cast<qint64>(-8388608),
                                                                 qRound64(static_cast<qreal>(value) * gain),
                                                                 static_cast<qint64>(8388607)));
        const qint32 clamped = qBound<qint32>(-8388608, scaled, 8388607);
        sampleData[0] = static_cast<char>(clamped & 0xFF);
        sampleData[1] = static_cast<char>((clamped >> 8) & 0xFF);
        sampleData[2] = static_cast<char>((clamped >> 16) & 0xFF);
        break;
    }
    case PcmSampleEncoding::Int32: {
        const qint32 value = qFromLittleEndian<qint32>(sampleData);
        const qint64 scaledValue = qRound64(static_cast<qreal>(value) * gain);
        const qint32 scaled = static_cast<qint32>(qBound<qint64>(static_cast<qint64>(std::numeric_limits<qint32>::min()),
                                                                 scaledValue,
                                                                 static_cast<qint64>(std::numeric_limits<qint32>::max())));
        qToLittleEndian<qint32>(scaled, sampleData);
        break;
    }
    case PcmSampleEncoding::Float32: {
        float value = 0.0f;
        std::memcpy(&value, sampleData, sizeof(float));
        value = static_cast<float>(static_cast<qreal>(value) * gain);
        std::memcpy(sampleData, &value, sizeof(float));
        break;
    }
    case PcmSampleEncoding::Unknown:
        break;
    }
}

inline qreal computeLinearFadeGain(qsizetype processedFrames,
                                   qsizetype frameIndex,
                                   qsizetype totalFrames)
{
    return qMin<qreal>(1.0,
                       static_cast<qreal>(processedFrames + frameIndex + 1)
                           / static_cast<qreal>(totalFrames));
}

inline qreal computeLinearFadeGainFromZero(qsizetype processedFrames,
                                           qsizetype frameIndex,
                                           qsizetype totalFrames)
{
    const qsizetype denominator = totalFrames > 1 ? totalFrames - 1 : totalFrames;
    return qMin<qreal>(1.0,
                       static_cast<qreal>(processedFrames + frameIndex)
                           / static_cast<qreal>(denominator));
}

inline qreal sampleMagnitude(PcmSampleEncoding encoding, const char *sampleData)
{
    switch (encoding) {
    case PcmSampleEncoding::UInt8: {
        quint8 sample = 0;
        std::memcpy(&sample, sampleData, sizeof(sample));
        return std::abs(static_cast<int>(sample) - 128) / 127.0;
    }
    case PcmSampleEncoding::Int16: {
        qint16 sample = 0;
        std::memcpy(&sample, sampleData, sizeof(sample));
        return std::abs(static_cast<int>(sample)) / 32768.0;
    }
    case PcmSampleEncoding::Int24:
        return std::abs(static_cast<double>(readInt24Sample(sampleData))) / 8388608.0;
    case PcmSampleEncoding::Int32: {
        qint32 sample = 0;
        std::memcpy(&sample, sampleData, sizeof(sample));
        return std::abs(static_cast<double>(sample)) / 2147483648.0;
    }
    case PcmSampleEncoding::Float32: {
        float sample = 0.0f;
        std::memcpy(&sample, sampleData, sizeof(sample));
        return qMin<qreal>(1.0, std::abs(static_cast<double>(sample)));
    }
    case PcmSampleEncoding::Unknown:
        break;
    }

    return 0.0;
}

} // namespace PcmUtils

#endif // PCMUTILS_H
