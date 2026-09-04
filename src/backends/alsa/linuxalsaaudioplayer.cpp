#include "linuxalsaaudioplayer.h"

#include "alsaoutputworker.h"
#include "audioplayerfactory.h"
#include "pcmseekcache.h"
#include "playerlogger.h"

#ifdef AUDIOPLAYER_LIBAV_DECODER
#include "libavseekdecoderworker.h"
#endif

#include <QMediaDevices>
#include <QTimer>

LinuxAlsaAudioPlayer::LinuxAlsaAudioPlayer(QObject *parent)
    : AudioPlayerBackend(parent)
    , m_mediaDevices(new QMediaDevices(this))
    , m_outputDeviceChangeTimer(new QTimer(this))
#ifdef AUDIOPLAYER_LIBAV_DECODER
    , m_libavSeekDecoderWorker(new LibavSeekDecoderWorker)
#endif
{
    m_outputDeviceChangeTimer->setSingleShot(true);
    m_outputDeviceChangeTimer->setInterval(500);

    connect(m_mediaDevices, &QMediaDevices::audioOutputsChanged, this, [this]() {
        m_outputDeviceChangeTimer->start();
    });

    connect(m_outputDeviceChangeTimer, &QTimer::timeout, this, &LinuxAlsaAudioPlayer::handleAudioOutputsChanged);
}

LinuxAlsaAudioPlayer::~LinuxAlsaAudioPlayer()
{
    teardownPipeline();
    delete m_pcmSeekCache;
    closeAlsaDevice();
}

AudioPlayerBackend::BackendId LinuxAlsaAudioPlayer::backendId() const
{
    return BackendId::LinuxAlsa;
}

QString LinuxAlsaAudioPlayer::backendName() const
{
    return tr("Linux ALSA");
}

QString LinuxAlsaAudioPlayer::decoderName() const
{
    return m_libavSeekDecoderWorker ? tr("libav seek") : tr("ffmpeg CLI");
}

bool LinuxAlsaAudioPlayer::exclusiveModeEnabled() const
{
    return m_exclusiveModeEnabled;
}

void LinuxAlsaAudioPlayer::setExclusiveModeEnabled(bool enabled)
{
    m_exclusiveModeEnabled = enabled;
}

bool LinuxAlsaAudioPlayer::exactPlaybackEnabled() const
{
    return m_exactPlaybackEnabled;
}

void LinuxAlsaAudioPlayer::setExactPlaybackEnabled(bool enabled)
{
    m_exactPlaybackEnabled = enabled;
}

void LinuxAlsaAudioPlayer::setSource(const QString &filePath,
                                      int sourceChannelCount,
                                      int sourceSampleRate,
                                      int sourceBitDepth,
                                      const QString &sourceCodecName)
{
    if (m_sourcePath == filePath && m_sourceChannelCount == sourceChannelCount
        && m_sourceSampleRate == sourceSampleRate && m_sourceBitDepth == sourceBitDepth
        && m_sourceCodecName == sourceCodecName) {
        return;
    }

    m_sourcePath = filePath;
    m_sourceChannelCount = sourceChannelCount;
    m_sourceSampleRate = sourceSampleRate;
    m_sourceBitDepth = sourceBitDepth;
    m_sourceCodecName = sourceCodecName;

    PlayerLogger::log(QStringLiteral("alsa"),
                      QStringLiteral("setSource path=%1 codec=%2 channels=%3 rate=%4 bitDepth=%5")
                          .arg(filePath)
                          .arg(sourceCodecName)
                          .arg(sourceChannelCount)
                          .arg(sourceSampleRate)
                          .arg(sourceBitDepth));

    if (m_pcmSeekCache) {
        delete m_pcmSeekCache;
        m_pcmSeekCache = nullptr;
    }

    if (m_playbackState != PlaybackState::Stopped) {
        teardownPipeline();
    }
}

QString LinuxAlsaAudioPlayer::source() const
{
    return m_sourcePath;
}

void LinuxAlsaAudioPlayer::play()
{
    if (m_sourcePath.isEmpty()) {
        return;
    }

    if (m_playbackState == PlaybackState::Paused && m_audioStarted) {
        if (m_outputWorker) {
            m_outputWorker->setPaused(false);
        }
        if (m_decoderWorker) {
            m_decoderWorker->setPaused(false);
        }
        setPlaybackState(PlaybackState::Playing);
        return;
    }

    teardownPipeline();
    startPipeline(m_outputRecoveryPositionMs);
    m_outputRecoveryPositionMs = 0;
}

void LinuxAlsaAudioPlayer::pause()
{
    if (m_playbackState != PlaybackState::Playing) {
        return;
    }

    if (m_outputWorker) {
        m_outputWorker->setPaused(true);
    }
    if (m_decoderWorker) {
        m_decoderWorker->setPaused(true);
    }
    setPlaybackState(PlaybackState::Paused);
}

void LinuxAlsaAudioPlayer::stop()
{
    if (m_playbackState == PlaybackState::Stopped) {
        return;
    }

    teardownPipeline();
    setPlaybackState(PlaybackState::Stopped);
}

void LinuxAlsaAudioPlayer::seek(qint64 positionMs)
{
    if (m_sourcePath.isEmpty()) {
        return;
    }

    if (m_playbackState == PlaybackState::Stopped) {
        const qint64 clamped = qMax<qint64>(0, positionMs);
        m_outputRecoveryPositionMs = clamped;
        emit positionChanged(clamped);
        PlayerLogger::log(QStringLiteral("alsa"),
                          QStringLiteral("seek stored for stopped state positionMs=%1").arg(clamped));
        return;
    }

    if (m_playbackState == PlaybackState::Paused) {
        teardownPipeline();
        m_outputRecoveryPositionMs = qMax<qint64>(0, positionMs);
        startPipeline(m_outputRecoveryPositionMs, PipelineStartupProfile::SeekResume);
        return;
    }

    const qint64 clampedPosition = qMax<qint64>(0, positionMs);

    PlayerLogger::log(QStringLiteral("alsa"),
                      QStringLiteral("seek request positionMs=%1 state=%2 session=%3")
                          .arg(clampedPosition)
                          .arg(static_cast<int>(m_playbackState))
                          .arg(m_decoderSessionId));

    resetOutputRecoveryState();

    if (seekWhilePlaying(clampedPosition)) {
        PlayerLogger::log(QStringLiteral("alsa"),
                          QStringLiteral("seek fast-path succeeded positionMs=%1").arg(clampedPosition));
        return;
    }

    PlayerLogger::log(QStringLiteral("alsa"),
                      QStringLiteral("seek fast-path failed, using teardown-restart positionMs=%1")
                          .arg(clampedPosition));
    bool wasPlaying = (m_playbackState == PlaybackState::Playing);
    teardownPipeline();

    if (wasPlaying) {
        startPipeline(clampedPosition, PipelineStartupProfile::SeekResume);
    }
}

void LinuxAlsaAudioPlayer::setVolume(qreal volume)
{
    m_volume = qBound(0.0, volume, 1.0);
    if (m_outputWorker) {
        m_outputWorker->setVolume(m_volume);
    }
}

QList<QAudioDevice> LinuxAlsaAudioPlayer::availableOutputDevices() const
{
    return QMediaDevices::audioOutputs();
}

QString LinuxAlsaAudioPlayer::outputDeviceDescription() const
{
    return m_outputDeviceDescription;
}

QAudioFormat LinuxAlsaAudioPlayer::outputFormat() const
{
    return m_outputFormat;
}

int LinuxAlsaAudioPlayer::outputDeviceBitDepth() const
{
    return m_outputPcmFormat.effectiveValidBitsPerSample();
}

QAudioDevice LinuxAlsaAudioPlayer::selectedOutputDevice() const
{
    return resolveOutputDevice();
}

QByteArray LinuxAlsaAudioPlayer::selectedOutputDeviceId() const
{
    return m_selectedOutputDeviceId;
}

bool LinuxAlsaAudioPlayer::usesDefaultOutputDevice() const
{
    return m_selectedOutputDeviceId.isEmpty();
}

void LinuxAlsaAudioPlayer::setOutputDeviceId(const QByteArray &deviceId)
{
    QByteArray normalizedDeviceId = deviceId;
    if (!normalizedDeviceId.isEmpty()) {
        const QList<AudioOutputDeviceInfo> devices = enumerateAlsaOutputDevices();
        bool found = false;
        for (const AudioOutputDeviceInfo &device : devices) {
            if (device.id == normalizedDeviceId) {
                found = true;
                break;
            }
        }
        if (!found) {
            PlayerLogger::log(QStringLiteral("alsa"),
                              QStringLiteral("setOutputDeviceId fallback-default unknownId=%1")
                                  .arg(QString::fromUtf8(normalizedDeviceId)));
            normalizedDeviceId.clear();
        }
    }

    if (m_selectedOutputDeviceId == normalizedDeviceId) {
        return;
    }

    m_selectedOutputDeviceId = normalizedDeviceId;

    PlayerLogger::log(QStringLiteral("alsa"),
                      QStringLiteral("setOutputDeviceId deviceId=%1 state=%2")
                          .arg(QString::fromUtf8(normalizedDeviceId))
                          .arg(static_cast<int>(m_playbackState)));

    if (m_playbackState == PlaybackState::Playing && m_audioStarted) {
        beginActiveOutputSwitch(ActiveOutputSwitchTrigger::DeviceSelection,
                                QStringLiteral("device selection changed"));
    }

    emit outputDeviceSelectionChanged();
}

void LinuxAlsaAudioPlayer::refreshOutputConfiguration(bool force)
{
    Q_UNUSED(force);

    PlayerLogger::log(QStringLiteral("alsa"),
                      QStringLiteral("refreshOutputConfiguration force=%1 state=%2")
                          .arg(force)
                          .arg(static_cast<int>(m_playbackState)));

    if (m_playbackState == PlaybackState::Playing && m_audioStarted) {
        beginActiveOutputSwitch(ActiveOutputSwitchTrigger::OutputRefresh,
                                QStringLiteral("output configuration refresh"));
    }
}

bool LinuxAlsaAudioPlayer::isSupportedForContext(const AudioPlayerSourceContext &context, QString *reason)
{
#if defined(Q_OS_LINUX)
    Q_UNUSED(context);
    if (reason) {
        *reason = tr("ALSA backend available on Linux.");
    }
    return true;
#else
    Q_UNUSED(context);
    if (reason) {
        *reason = tr("ALSA backend is only available on Linux.");
    }
    return false;
#endif
}

QAudioDevice LinuxAlsaAudioPlayer::resolveOutputDevice(bool *usesDefault) const
{
    const QList<QAudioDevice> devices = QMediaDevices::audioOutputs();

    if (!m_selectedOutputDeviceId.isEmpty()) {
        for (const QAudioDevice &device : devices) {
            if (device.id() == m_selectedOutputDeviceId) {
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
    return devices.isEmpty() ? QAudioDevice() : devices.first();
}

void LinuxAlsaAudioPlayer::handleAudioOutputsChanged()
{
    PlayerLogger::diagnostic(QStringLiteral("alsa"),
                             QStringLiteral("audio_outputs_changed"),
                             {
                                 {QStringLiteral("deviceCount"), availableOutputDevices().size()},
                             });

    emit outputDevicesChanged();

    if (m_playbackState == PlaybackState::Playing && m_audioStarted) {
        bool deviceStillAvailable = false;
        const QList<AudioOutputDeviceInfo> devices = enumerateAlsaOutputDevices();

        if (m_selectedOutputDeviceId.isEmpty()) {
            deviceStillAvailable = !devices.isEmpty();
        } else {
            for (const AudioOutputDeviceInfo &d : devices) {
                if (d.id == m_selectedOutputDeviceId) {
                    deviceStillAvailable = true;
                    break;
                }
            }
        }

        if (!deviceStillAvailable) {
            PlayerLogger::log(QStringLiteral("alsa"),
                              QStringLiteral("audioOutputsChanged: selected device disappeared, stopping"));
            stop();
            return;
        }

        beginActiveOutputSwitch(ActiveOutputSwitchTrigger::SystemDeviceChange,
                                QStringLiteral("system audio outputs changed"));
    }
}
