#ifndef AUDIOUTILS_H
#define AUDIOUTILS_H

// Shared utility functions for audio backends.
// Extracted from WASAPI, ASIO, and FFmpeg backends to eliminate code duplication.

#include "audioplayerbackend.h"
#include "ffmpegpcmshared.h"

#include <QAudio>
#include <QString>

namespace AudioUtils {

inline QString playbackStateName(AudioPlayerBackend::PlaybackState state)
{
    switch (state) {
    case AudioPlayerBackend::PlaybackState::Stopped:
        return QStringLiteral("Stopped");
    case AudioPlayerBackend::PlaybackState::Playing:
        return QStringLiteral("Playing");
    case AudioPlayerBackend::PlaybackState::Paused:
        return QStringLiteral("Paused");
    case AudioPlayerBackend::PlaybackState::Stopping:
        return QStringLiteral("Stopping");
    }

    return QStringLiteral("Unknown");
}

inline QString audioStateName(QAudio::State state)
{
    switch (state) {
    case QAudio::ActiveState:
        return QStringLiteral("Active");
    case QAudio::SuspendedState:
        return QStringLiteral("Suspended");
    case QAudio::StoppedState:
        return QStringLiteral("Stopped");
    case QAudio::IdleState:
        return QStringLiteral("Idle");
    }

    return QStringLiteral("Unknown");
}

inline QString channelLayoutForCount(int channelCount)
{
    switch (channelCount) {
    case 1:
        return QStringLiteral("mono");
    case 2:
        return QStringLiteral("stereo");
    case 3:
        return QStringLiteral("2.1");
    case 4:
        return QStringLiteral("quad");
    case 5:
        return QStringLiteral("4.1");
    case 6:
        return QStringLiteral("5.1");
    case 7:
        return QStringLiteral("6.1");
    case 8:
        return QStringLiteral("7.1");
    default:
        return {};
    }
}

inline QString pcmCodecName(PcmSampleEncoding encoding)
{
    switch (encoding) {
    case PcmSampleEncoding::UInt8:
        return QStringLiteral("pcm_u8");
    case PcmSampleEncoding::Int16:
        return QStringLiteral("pcm_s16le");
    case PcmSampleEncoding::Int24:
    case PcmSampleEncoding::Int32:
        return QStringLiteral("pcm_s32le");
    case PcmSampleEncoding::Float32:
        return QStringLiteral("pcm_f32le");
    case PcmSampleEncoding::Unknown:
        break;
    }

    return QStringLiteral("pcm_s16le");
}

inline QString pcmSampleFormatName(PcmSampleEncoding encoding)
{
    switch (encoding) {
    case PcmSampleEncoding::UInt8:
        return QStringLiteral("u8");
    case PcmSampleEncoding::Int16:
        return QStringLiteral("s16");
    case PcmSampleEncoding::Int24:
    case PcmSampleEncoding::Int32:
        return QStringLiteral("s32");
    case PcmSampleEncoding::Float32:
        return QStringLiteral("flt");
    case PcmSampleEncoding::Unknown:
        break;
    }

    return QStringLiteral("s16");
}

inline QString pcmMuxerName(PcmSampleEncoding encoding)
{
    switch (encoding) {
    case PcmSampleEncoding::UInt8:
        return QStringLiteral("u8");
    case PcmSampleEncoding::Int16:
        return QStringLiteral("s16le");
    case PcmSampleEncoding::Int24:
    case PcmSampleEncoding::Int32:
        return QStringLiteral("s32le");
    case PcmSampleEncoding::Float32:
        return QStringLiteral("f32le");
    case PcmSampleEncoding::Unknown:
        break;
    }

    return QStringLiteral("s16le");
}

inline QString pcmCodecName(const PcmStreamFormat &format)
{
    return pcmCodecName(format.sampleEncoding);
}

inline QString pcmSampleFormatName(const PcmStreamFormat &format)
{
    return pcmSampleFormatName(format.sampleEncoding);
}

inline QString pcmMuxerName(const PcmStreamFormat &format)
{
    return pcmMuxerName(format.sampleEncoding);
}

} // namespace AudioUtils

#endif // AUDIOUTILS_H
