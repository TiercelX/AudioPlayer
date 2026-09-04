#ifndef AUDIOPLAYERBACKEND_H
#define AUDIOPLAYERBACKEND_H

#include <QObject>
#include <QByteArray>
#include <QList>
#include <QString>
#include <QtGlobal>
#include <QtMultimedia/qaudiodevice.h>
#include <QtMultimedia/qaudioformat.h>

struct AudioOutputDeviceInfo
{
    QByteArray id;
    QString description;
    QAudioFormat preferredFormat;
    QString transport;

    bool isNull() const
    {
        return id.isEmpty() && description.isEmpty() && !preferredFormat.isValid();
    }
};

class AudioPlayerBackend : public QObject
{
    Q_OBJECT

public:
    enum class BackendId {
        Ffmpeg,
        WindowsWasapi,
        WindowsAsio,
        AppleNative,
        AndroidNative,
        LinuxAlsa,
    };
    Q_ENUM(BackendId)

    enum class PlaybackState {
        Stopped,
        Playing,
        Paused,
        Stopping,
    };
    Q_ENUM(PlaybackState)

    enum class PlaybackError {
        None,
        DecoderNotFound,
        DeviceNotFound,
        DeviceOccupied,
        DeviceDisconnected,
        DriverInitFailure,
        DriverRecoveryFailure,
        FormatNotSupported,
        DecoderError,
        OutputError,
        OutputRecoveryFailure,
        OutputSwitchFailure,
        UnsupportedOperation,
    };
    Q_ENUM(PlaybackError)

    explicit AudioPlayerBackend(QObject *parent = nullptr)
        : QObject(parent)
    {
    }

    ~AudioPlayerBackend() override = default;

    virtual BackendId backendId() const = 0;
    virtual QString backendName() const = 0;
    virtual QString decoderName() const = 0;
    virtual bool exclusiveModeEnabled() const { return false; }
    virtual void setExclusiveModeEnabled(bool enabled) { Q_UNUSED(enabled); }
    virtual bool stabilityModeEnabled() const { return false; }
    virtual void setStabilityModeEnabled(bool enabled) { Q_UNUSED(enabled); }
    virtual bool exactPlaybackEnabled() const { return true; }
    virtual void setExactPlaybackEnabled(bool enabled) { Q_UNUSED(enabled); }
    virtual int creativeChannelReorderMode() const { return 0; }
    virtual void setCreativeChannelReorderMode(int mode) { Q_UNUSED(mode); }
    virtual void setSource(const QString &filePath,
                           int sourceChannelCount,
                           int sourceSampleRate,
                           int sourceBitDepth,
                           const QString &sourceCodecName) = 0;
    virtual QString source() const = 0;

    virtual void play() = 0;
    virtual void pause() = 0;
    virtual void stop() = 0;
    virtual void seek(qint64 positionMs) = 0;
    virtual void setVolume(qreal volume) = 0;

    virtual PlaybackState playbackState() const;
    virtual QList<QAudioDevice> availableOutputDevices() const = 0;
    virtual QList<AudioOutputDeviceInfo> availableOutputDeviceInfos() const
    {
        QList<AudioOutputDeviceInfo> result;
        const QList<QAudioDevice> devices = availableOutputDevices();
        result.reserve(devices.size());
        for (const QAudioDevice &device : devices) {
            result.append({
                device.id(),
                device.description(),
                device.preferredFormat(),
                QString(),
            });
        }
        return result;
    }
    virtual QString outputDeviceDescription() const = 0;
    virtual QAudioFormat outputFormat() const = 0;
    virtual int outputDeviceBitDepth() const { return 0; }
    virtual QAudioDevice selectedOutputDevice() const = 0;
    virtual AudioOutputDeviceInfo selectedOutputDeviceInfo() const
    {
        const QAudioDevice device = selectedOutputDevice();
        if (device.isNull()) {
            return {};
        }
        return {
            device.id(),
            device.description(),
            device.preferredFormat(),
            QString(),
        };
    }
    virtual QByteArray selectedOutputDeviceId() const = 0;
    virtual bool usesDefaultOutputDevice() const = 0;
    virtual void setOutputDeviceId(const QByteArray &deviceId) = 0;
    virtual void refreshOutputConfiguration(bool force = false)
    {
        Q_UNUSED(force);
    }

protected:
    void setPlaybackState(PlaybackState state);
    void emitAudioLevels(qreal leftLevel, qreal rightLevel);
    virtual void logPlaybackStateChange(PlaybackState from, PlaybackState to);
    PlaybackState m_playbackState = PlaybackState::Stopped;
    qreal m_lastLeftLevel = 0.0;
    qreal m_lastRightLevel = 0.0;

signals:
    void audioLevelsChanged(qreal leftLevel, qreal rightLevel);
    void errorOccurred(AudioPlayerBackend::PlaybackError errorCode, const QString &message);
    void finished();
    void outputFormatChanged(const QString &deviceDescription, const QAudioFormat &format);
    void outputDeviceSelectionChanged();
    void outputDevicesChanged();
    void playbackStateChanged(AudioPlayerBackend::PlaybackState state);
    void positionChanged(qint64 position);
    void statusMessage(const QString &message);
};

#endif // AUDIOPLAYERBACKEND_H
