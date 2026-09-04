#include "linuxalsaaudioplayer.h"

#include "alsaformatnegotiator.h"
#include "audioplayerfactory.h"
#include "audioutils.h"
#include "playerlogger.h"

#include <QAudioDevice>
#include <QMediaDevices>

#include <cstring>

snd_pcm_t *LinuxAlsaAudioPlayer::openAlsaDevice(const QString &deviceId, bool exclusive)
{
    snd_pcm_t *handle = nullptr;
    int err;

    QString deviceName = deviceId;
    if (deviceName.isEmpty()) {
        deviceName = exclusive ? QStringLiteral("hw:0") : QStringLiteral("default");
    }

    err = snd_pcm_open(&handle, deviceName.toUtf8().constData(), SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        PlayerLogger::log(QStringLiteral("alsa"),
                          QStringLiteral("openDevice failed device=%1 error=%2")
                              .arg(deviceName)
                              .arg(snd_strerror(err)));
        return nullptr;
    }

    PlayerLogger::log(QStringLiteral("alsa"),
                      QStringLiteral("openDevice success device=%1 exclusive=%2")
                          .arg(deviceName)
                          .arg(exclusive));

    return handle;
}

void LinuxAlsaAudioPlayer::closeAlsaDevice()
{
    if (m_pcmHandle) {
        snd_pcm_close(m_pcmHandle);
        m_pcmHandle = nullptr;
    }
}

QAudioFormat LinuxAlsaAudioPlayer::selectOutputFormat(PcmStreamFormat *pcmFormat) const
{
    QAudioDevice device = resolveOutputDevice();
    if (device.isNull()) {
        return QAudioFormat();
    }

    QAudioFormat preferred = device.preferredFormat();
    QAudioFormat requested = preferred;

    if (m_sourceSampleRate > 0 && m_exclusiveModeEnabled && m_exactPlaybackEnabled) {
        requested.setSampleRate(m_sourceSampleRate);
    }

    if (m_sourceChannelCount > 0 && m_sourceChannelCount < requested.channelCount()) {
        requested.setChannelCount(m_sourceChannelCount);
    }

    if (pcmFormat) {
        pcmFormat->sampleRate = requested.sampleRate();
        pcmFormat->channelCount = requested.channelCount();

        switch (requested.sampleFormat()) {
        case QAudioFormat::UInt8:
            pcmFormat->sampleEncoding = PcmSampleEncoding::UInt8;
            pcmFormat->validBitsPerSample = 8;
            break;
        case QAudioFormat::Int16:
            pcmFormat->sampleEncoding = PcmSampleEncoding::Int16;
            pcmFormat->validBitsPerSample = 16;
            break;
        case QAudioFormat::Int32:
            pcmFormat->sampleEncoding = PcmSampleEncoding::Int32;
            pcmFormat->validBitsPerSample = 32;
            break;
        case QAudioFormat::Float:
            pcmFormat->sampleEncoding = PcmSampleEncoding::Float32;
            pcmFormat->validBitsPerSample = 32;
            break;
        default:
            pcmFormat->sampleEncoding = PcmSampleEncoding::Int16;
            pcmFormat->validBitsPerSample = 16;
            break;
        }
    }

    return requested;
}

QString LinuxAlsaAudioPlayer::locateFfmpegExecutable() const
{
    QString ffmpegPath = qEnvironmentVariable("AUDIOPLAYER_FFMPEG_PATH");
    if (!ffmpegPath.isEmpty()) {
        return ffmpegPath;
    }

    return QStringLiteral("ffmpeg");
}

QString LinuxAlsaAudioPlayer::channelLayoutForCount(int channelCount) const
{
    return AudioUtils::channelLayoutForCount(channelCount);
}

PcmStreamFormat LinuxAlsaAudioPlayer::decoderFormatForOutput(const PcmStreamFormat &outputFormat) const
{
    PcmStreamFormat decoderFormat = outputFormat;

    if (outputFormat.sampleEncoding == PcmSampleEncoding::Int24
        || outputFormat.sampleEncoding == PcmSampleEncoding::Int16) {
        decoderFormat.sampleEncoding = PcmSampleEncoding::Int32;
        decoderFormat.validBitsPerSample = 32;
    }

    return decoderFormat;
}

QList<AudioOutputDeviceInfo> LinuxAlsaAudioPlayer::enumerateAlsaOutputDevices() const
{
    QList<AudioOutputDeviceInfo> devices;

    void **hints = nullptr;
    int err = snd_device_name_hint(-1, "pcm", &hints);
    if (err < 0) {
        PlayerLogger::log(QStringLiteral("alsa"),
                          QStringLiteral("enumerateDevices snd_device_name_hint failed: %1")
                              .arg(snd_strerror(err)));
        return devices;
    }

    for (void **n = hints; *n; n++) {
        char *name = snd_device_name_get_hint(*n, "NAME");
        char *desc = snd_device_name_get_hint(*n, "DESC");
        char *io = snd_device_name_get_hint(*n, "IOID");

        bool isOutput = !io || strcmp(io, "Output") == 0;
        bool isAlsaDevice = name
            && (strncmp(name, "hw:", 3) == 0 || strncmp(name, "plughw:", 7) == 0);

        if (isOutput && isAlsaDevice) {
            AudioOutputDeviceInfo info;
            info.id = QByteArray(name);
            info.description = desc ? QString::fromUtf8(desc).replace(QLatin1Char('\n'), QStringLiteral(", "))
                                    : QString::fromUtf8(name);

            bool isPlug = strncmp(name, "plughw:", 7) == 0;
            info.transport = isPlug ? QStringLiteral("ALSA-plughw") : QStringLiteral("ALSA-hw");

            info.preferredFormat = probeAlsaDevicePreferredFormat(QString::fromUtf8(name));

            PlayerLogger::log(QStringLiteral("alsa"),
                              QStringLiteral("enumerateDevice id=%1 transport=%2 rate=%3 channels=%4 sampleFormat=%5")
                                  .arg(info.id, info.transport)
                                  .arg(info.preferredFormat.sampleRate())
                                  .arg(info.preferredFormat.channelCount())
                                  .arg(static_cast<int>(info.preferredFormat.sampleFormat())));

            devices.append(info);
        }

        free(name);
        free(desc);
        free(io);
    }
    snd_device_name_free_hint(hints);

    return devices;
}

QAudioFormat LinuxAlsaAudioPlayer::probeAlsaDevicePreferredFormat(const QString &deviceName) const
{
    QAudioFormat format;

    snd_pcm_t *probeHandle = nullptr;
    int err = snd_pcm_open(&probeHandle, deviceName.toUtf8().constData(),
                           SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
    if (err < 0) {
        format.setSampleRate(44100);
        format.setChannelCount(2);
        format.setSampleFormat(QAudioFormat::Int16);
        return format;
    }

    snd_pcm_hw_params_t *params;
    snd_pcm_hw_params_alloca(&params);
    snd_pcm_hw_params_any(probeHandle, params);
    snd_pcm_hw_params_set_access(probeHandle, params, SND_PCM_ACCESS_RW_INTERLEAVED);

    unsigned int rate = 44100;
    snd_pcm_hw_params_get_rate_max(params, &rate, nullptr);
    if (rate > 384000) {
        rate = 384000;
    }

    unsigned int channels = 2;
    snd_pcm_hw_params_get_channels_max(params, &channels);
    if (channels > 8) {
        channels = 8;
    }

    snd_pcm_format_t bestFormat = SND_PCM_FORMAT_S16_LE;
    const snd_pcm_format_t probeFormats[] = {
        SND_PCM_FORMAT_S32_LE,
        SND_PCM_FORMAT_S24_LE,
        SND_PCM_FORMAT_S24_3LE,
        SND_PCM_FORMAT_S16_LE,
        SND_PCM_FORMAT_FLOAT_LE,
    };

    for (snd_pcm_format_t fmt : probeFormats) {
        if (snd_pcm_hw_params_test_format(probeHandle, params, fmt) == 0) {
            bestFormat = fmt;
            break;
        }
    }

    snd_pcm_close(probeHandle);

    format.setSampleRate(static_cast<int>(rate));
    format.setChannelCount(static_cast<int>(channels));
    format.setSampleFormat(AlsaFormatNegotiator::toQtSampleFormat(bestFormat));

    return format;
}

QList<AudioOutputDeviceInfo> LinuxAlsaAudioPlayer::availableOutputDeviceInfos() const
{
    return enumerateAlsaOutputDevices();
}

AudioOutputDeviceInfo LinuxAlsaAudioPlayer::selectedOutputDeviceInfo() const
{
    return resolveOutputDeviceInfo();
}

AudioOutputDeviceInfo LinuxAlsaAudioPlayer::resolveOutputDeviceInfo(bool *usesDefault) const
{
    const QList<AudioOutputDeviceInfo> devices = enumerateAlsaOutputDevices();

    if (!m_selectedOutputDeviceId.isEmpty()) {
        for (const AudioOutputDeviceInfo &device : devices) {
            if (device.id == m_selectedOutputDeviceId) {
                if (usesDefault) {
                    *usesDefault = false;
                }
                return device;
            }
        }
    }

    if (usesDefault) {
        *usesDefault = true;
    }
    return devices.isEmpty() ? AudioOutputDeviceInfo{} : devices.first();
}
