#include "alsaformatnegotiator.h"

#include <algorithm>
#include <cmath>

AlsaNegotiationResult AlsaFormatNegotiator::negotiate(snd_pcm_t *handle,
                                                      int sourceSampleRate,
                                                      int sourceBitDepth,
                                                      int sourceChannelCount,
                                                      bool exclusiveMode,
                                                      bool exactMode)
{
    AlsaNegotiationResult result;

    auto rates = sampleRateCandidates(sourceSampleRate, exactMode);
    auto formats = formatCandidates(sourceBitDepth, exactMode);
    auto channels = channelCandidates(sourceChannelCount);

    for (unsigned int rate : rates) {
        for (unsigned int ch : channels) {
            for (const auto &fmt : formats) {
                if (testFormat(handle, fmt.alsaFormat, rate, ch)) {
                    result.success = true;
                    result.alsaFormat = fmt.alsaFormat;
                    result.actualRate = rate;
                    result.actualChannels = ch;
                    result.exclusiveMode = exclusiveMode;

                    result.qtFormat.setSampleRate(rate);
                    result.qtFormat.setChannelCount(ch);
                    result.qtFormat.setSampleFormat(fmt.qtFormat);

                    return result;
                }
            }
        }
    }

    return result;
}

QList<unsigned int> AlsaFormatNegotiator::sampleRateCandidates(int sourceRate, bool exactMode)
{
    QList<unsigned int> candidates;

    if (exactMode && sourceRate > 0) {
        candidates << sourceRate;
    }

    const QList<unsigned int> commonRates = {
        44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000
    };

    for (unsigned int rate : commonRates) {
        if (!candidates.contains(rate)) {
            candidates << rate;
        }
    }

    std::sort(candidates.begin(), candidates.end(),
              [sourceRate](unsigned int a, unsigned int b) {
                  return std::abs(static_cast<int>(a) - sourceRate)
                       < std::abs(static_cast<int>(b) - sourceRate);
              });

    return candidates;
}

QList<AlsaFormatCandidate> AlsaFormatNegotiator::formatCandidates(int sourceBitDepth, bool exactMode)
{
    QList<AlsaFormatCandidate> candidates;

    switch (sourceBitDepth) {
    case 16:
        candidates << AlsaFormatCandidate{SND_PCM_FORMAT_S16_LE, QAudioFormat::Int16, 16, QStringLiteral("S16_LE")};
        break;
    case 24:
        candidates << AlsaFormatCandidate{SND_PCM_FORMAT_S24_3LE, QAudioFormat::Int32, 24, QStringLiteral("S24_3LE")};
        candidates << AlsaFormatCandidate{SND_PCM_FORMAT_S24_LE, QAudioFormat::Int32, 24, QStringLiteral("S24_LE")};
        candidates << AlsaFormatCandidate{SND_PCM_FORMAT_S32_LE, QAudioFormat::Int32, 32, QStringLiteral("S32_LE")};
        break;
    case 32:
        candidates << AlsaFormatCandidate{SND_PCM_FORMAT_S32_LE, QAudioFormat::Int32, 32, QStringLiteral("S32_LE")};
        candidates << AlsaFormatCandidate{SND_PCM_FORMAT_FLOAT_LE, QAudioFormat::Float, 32, QStringLiteral("FLOAT_LE")};
        break;
    default:
        candidates << AlsaFormatCandidate{SND_PCM_FORMAT_S32_LE, QAudioFormat::Int32, 32, QStringLiteral("S32_LE")};
        break;
    }

    if (!exactMode) {
        auto addIfMissing = [&](const AlsaFormatCandidate &c) {
            if (!candidates.contains(c)) {
                candidates << c;
            }
        };
        addIfMissing({SND_PCM_FORMAT_S24_3LE, QAudioFormat::Int32, 24, QStringLiteral("S24_3LE")});
        addIfMissing({SND_PCM_FORMAT_S24_LE, QAudioFormat::Int32, 24, QStringLiteral("S24_LE")});
        addIfMissing({SND_PCM_FORMAT_S32_LE, QAudioFormat::Int32, 32, QStringLiteral("S32_LE")});
        addIfMissing({SND_PCM_FORMAT_S16_LE, QAudioFormat::Int16, 16, QStringLiteral("S16_LE")});
        addIfMissing({SND_PCM_FORMAT_FLOAT_LE, QAudioFormat::Float, 32, QStringLiteral("FLOAT_LE")});
    }

    return candidates;
}

QList<unsigned int> AlsaFormatNegotiator::channelCandidates(int sourceChannels)
{
    QList<unsigned int> candidates;

    if (sourceChannels > 0) {
        candidates << sourceChannels;
    }

    const QList<unsigned int> commonChannels = {2, 1, 4, 6, 8};
    for (unsigned int ch : commonChannels) {
        if (!candidates.contains(ch)) {
            candidates << ch;
        }
    }

    return candidates;
}

bool AlsaFormatNegotiator::testFormat(snd_pcm_t *handle, snd_pcm_format_t format,
                                      unsigned int rate, unsigned int channels)
{
    snd_pcm_hw_params_t *params;
    snd_pcm_hw_params_alloca(&params);

    int err = snd_pcm_hw_params_any(handle, params);
    if (err < 0) {
        return false;
    }

    err = snd_pcm_hw_params_set_access(handle, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err < 0) {
        return false;
    }

    err = snd_pcm_hw_params_test_format(handle, params, format);
    if (err < 0) {
        return false;
    }

    err = snd_pcm_hw_params_test_rate(handle, params, rate, 0);
    if (err < 0) {
        return false;
    }

    err = snd_pcm_hw_params_test_channels(handle, params, channels);
    if (err < 0) {
        return false;
    }

    return true;
}

QAudioFormat::SampleFormat AlsaFormatNegotiator::toQtSampleFormat(snd_pcm_format_t format)
{
    switch (format) {
    case SND_PCM_FORMAT_S16_LE:
        return QAudioFormat::Int16;
    case SND_PCM_FORMAT_S24_3LE:
    case SND_PCM_FORMAT_S24_LE:
    case SND_PCM_FORMAT_S32_LE:
        return QAudioFormat::Int32;
    case SND_PCM_FORMAT_FLOAT_LE:
        return QAudioFormat::Float;
    case SND_PCM_FORMAT_U8:
        return QAudioFormat::UInt8;
    default:
        return QAudioFormat::Int16;
    }
}

int AlsaFormatNegotiator::validBitsForFormat(snd_pcm_format_t format)
{
    switch (format) {
    case SND_PCM_FORMAT_S16_LE:
        return 16;
    case SND_PCM_FORMAT_S24_3LE:
    case SND_PCM_FORMAT_S24_LE:
        return 24;
    case SND_PCM_FORMAT_S32_LE:
    case SND_PCM_FORMAT_FLOAT_LE:
        return 32;
    case SND_PCM_FORMAT_U8:
        return 8;
    default:
        return 16;
    }
}

int AlsaFormatNegotiator::bytesPerSampleForFormat(snd_pcm_format_t format)
{
    switch (format) {
    case SND_PCM_FORMAT_S16_LE:
        return 2;
    case SND_PCM_FORMAT_S24_3LE:
        return 3;
    case SND_PCM_FORMAT_S24_LE:
    case SND_PCM_FORMAT_S32_LE:
    case SND_PCM_FORMAT_FLOAT_LE:
        return 4;
    case SND_PCM_FORMAT_U8:
        return 1;
    default:
        return 2;
    }
}
