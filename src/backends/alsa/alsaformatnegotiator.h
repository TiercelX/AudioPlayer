#ifndef ALSAFORMATNEGOTIATOR_H
#define ALSAFORMATNEGOTIATOR_H

#include <alsa/asoundlib.h>
#include <QAudioFormat>
#include <QList>
#include <QString>

struct AlsaFormatCandidate {
    snd_pcm_format_t alsaFormat = SND_PCM_FORMAT_S16_LE;
    QAudioFormat::SampleFormat qtFormat = QAudioFormat::Int16;
    int validBits = 16;
    QString name;

    bool operator==(const AlsaFormatCandidate &other) const
    {
        return alsaFormat == other.alsaFormat
            && qtFormat == other.qtFormat
            && validBits == other.validBits;
    }
};

struct AlsaNegotiationResult {
    bool success = false;
    snd_pcm_format_t alsaFormat = SND_PCM_FORMAT_S16_LE;
    QAudioFormat qtFormat;
    unsigned int actualRate = 0;
    unsigned int actualChannels = 0;
    bool exclusiveMode = false;
    QString deviceName;
};

class AlsaFormatNegotiator
{
public:
    static AlsaNegotiationResult negotiate(snd_pcm_t *handle,
                                           int sourceSampleRate,
                                           int sourceBitDepth,
                                           int sourceChannelCount,
                                           bool exclusiveMode,
                                           bool exactMode);

    static QList<unsigned int> sampleRateCandidates(int sourceRate, bool exactMode);
    static QList<AlsaFormatCandidate> formatCandidates(int sourceBitDepth, bool exactMode);
    static QList<unsigned int> channelCandidates(int sourceChannels);

    static bool testFormat(snd_pcm_t *handle, snd_pcm_format_t format,
                           unsigned int rate, unsigned int channels);

    static QAudioFormat::SampleFormat toQtSampleFormat(snd_pcm_format_t format);
    static int validBitsForFormat(snd_pcm_format_t format);
    static int bytesPerSampleForFormat(snd_pcm_format_t format);
};

#endif // ALSAFORMATNEGOTIATOR_H
