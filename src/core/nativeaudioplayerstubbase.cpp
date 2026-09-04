#include "nativeaudioplayerstubbase.h"

#include "playerlogger.h"

#include <QMediaDevices>

#include <algorithm>

NativeAudioPlayerStubBase::NativeAudioPlayerStubBase(QObject *parent)
    : AudioPlayerBackend(parent)
    , m_mediaDevices(new QMediaDevices(this))
{
    connect(m_mediaDevices, &QMediaDevices::audioOutputsChanged,
            this, &NativeAudioPlayerStubBase::handleAudioOutputsChanged);
}

NativeAudioPlayerStubBase::~NativeAudioPlayerStubBase() = default;

void NativeAudioPlayerStubBase::setSource(const QString &filePath,
                                          int sourceChannelCount,
                                          int sourceSampleRate,
                                          int sourceBitDepth,
                                          const QString &sourceCodecName)
{
    m_sourcePath = filePath;
    m_sourceChannelCount = sourceChannelCount;
    m_sourceSampleRate = sourceSampleRate;
    m_sourceBitDepth = sourceBitDepth;
    m_sourceCodecName = sourceCodecName;
    emit positionChanged(0);
    setPlaybackState(PlaybackState::Stopped);
    emitCurrentOutputFormat();
}

QString NativeAudioPlayerStubBase::source() const
{
    return m_sourcePath;
}

QString NativeAudioPlayerStubBase::decoderName() const
{
    return tr("native decoder");
}

void NativeAudioPlayerStubBase::play()
{
    if (m_sourcePath.isEmpty()) {
        return;
    }

    reportUnavailableOperation(QStringLiteral("play"));
}

void NativeAudioPlayerStubBase::pause()
{
    if (m_sourcePath.isEmpty()) {
        return;
    }

    reportUnavailableOperation(QStringLiteral("pause"));
}

void NativeAudioPlayerStubBase::stop()
{
    if (m_playbackState == PlaybackState::Stopped) {
        return;
    }

    setPlaybackState(PlaybackState::Stopped);
    emit positionChanged(0);
}

void NativeAudioPlayerStubBase::seek(qint64 positionMs)
{
    if (m_sourcePath.isEmpty()) {
        return;
    }

    PlayerLogger::log(QStringLiteral("native"),
                      QStringLiteral("seek ignored backend=%1 positionMs=%2 source=%3")
                          .arg(backendName())
                          .arg(positionMs)
                          .arg(m_sourcePath));
    reportUnavailableOperation(QStringLiteral("seek"));
}

void NativeAudioPlayerStubBase::setVolume(qreal volume)
{
    m_volume = volume;
}

QList<QAudioDevice> NativeAudioPlayerStubBase::availableOutputDevices() const
{
    return QMediaDevices::audioOutputs();
}

QString NativeAudioPlayerStubBase::outputDeviceDescription() const
{
    return currentOutputDeviceDescription();
}

QAudioFormat NativeAudioPlayerStubBase::outputFormat() const
{
    const QAudioDevice device = resolveOutputDevice();
    return device.preferredFormat();
}

QAudioDevice NativeAudioPlayerStubBase::selectedOutputDevice() const
{
    return resolveOutputDevice();
}

QByteArray NativeAudioPlayerStubBase::selectedOutputDeviceId() const
{
    return m_selectedOutputDeviceId;
}

bool NativeAudioPlayerStubBase::usesDefaultOutputDevice() const
{
    return m_selectedOutputDeviceId.isEmpty();
}

void NativeAudioPlayerStubBase::setOutputDeviceId(const QByteArray &deviceId)
{
    QByteArray normalizedDeviceId = deviceId;
    if (!normalizedDeviceId.isEmpty()) {
        const QList<QAudioDevice> devices = availableOutputDevices();
        const auto foundIt = std::find_if(devices.cbegin(), devices.cend(), [&normalizedDeviceId](const QAudioDevice &device) {
            return device.id() == normalizedDeviceId;
        });
        if (foundIt == devices.cend()) {
            PlayerLogger::log(QStringLiteral("native"),
                              QStringLiteral("setOutputDeviceId fallback-default backend=%1 unknownId=%2")
                                  .arg(backendName())
                                  .arg(QString::fromLatin1(normalizedDeviceId.toHex())));
            normalizedDeviceId.clear();
        }
    }

    if (m_selectedOutputDeviceId == normalizedDeviceId) {
        return;
    }

    PlayerLogger::log(QStringLiteral("native"),
                      QStringLiteral("setOutputDeviceId backend=%1 previous=%2 target=%3")
                          .arg(backendName())
                          .arg(QString::fromLatin1(m_selectedOutputDeviceId.toHex()))
                          .arg(QString::fromLatin1(normalizedDeviceId.toHex())));
    m_selectedOutputDeviceId = normalizedDeviceId;
    emit outputDeviceSelectionChanged();
    emitCurrentOutputFormat();
}

QString NativeAudioPlayerStubBase::currentOutputDeviceDescription() const
{
    bool usesDefault = false;
    const QAudioDevice device = resolveOutputDevice(&usesDefault);
    if (device.isNull()) {
        return tr("未找到输出设备");
    }

    return usesDefault ? tr("默认输出设备：%1").arg(device.description())
                       : device.description();
}

void NativeAudioPlayerStubBase::emitCurrentOutputFormat()
{
    const QAudioFormat format = outputFormat();
    if (!format.isValid()) {
        return;
    }

    emit outputFormatChanged(currentOutputDeviceDescription(), format);
}

void NativeAudioPlayerStubBase::handleAudioOutputsChanged()
{
    bool selectionChanged = false;
    if (!m_selectedOutputDeviceId.isEmpty()) {
        const QList<QAudioDevice> devices = availableOutputDevices();
        const auto foundIt = std::find_if(devices.cbegin(), devices.cend(), [this](const QAudioDevice &device) {
            return device.id() == m_selectedOutputDeviceId;
        });
        if (foundIt == devices.cend()) {
            PlayerLogger::log(QStringLiteral("native"),
                              QStringLiteral("handleAudioOutputsChanged selected-device-missing backend=%1 fallback-default id=%2")
                                  .arg(backendName())
                                  .arg(QString::fromLatin1(m_selectedOutputDeviceId.toHex())));
            m_selectedOutputDeviceId.clear();
            selectionChanged = true;
        }
    }

    emit outputDevicesChanged();
    if (selectionChanged) {
        emit outputDeviceSelectionChanged();
    }
    emitCurrentOutputFormat();
}

void NativeAudioPlayerStubBase::reportUnavailableOperation(const QString &operation)
{
    const QString message =
        tr("%1 backend skeleton is present but playback is not implemented yet (%2).")
            .arg(backendName(), operation);
    PlayerLogger::log(QStringLiteral("native"),
                      QStringLiteral("unsupported-operation backend=%1 operation=%2 source=%3 volume=%4 codec=%5 channels=%6")
                          .arg(backendName())
                          .arg(operation)
                          .arg(m_sourcePath)
                          .arg(QString::number(m_volume, 'f', 2))
                          .arg(m_sourceCodecName)
                          .arg(m_sourceChannelCount));
    emit errorOccurred(PlaybackError::UnsupportedOperation, message);
}

QAudioDevice NativeAudioPlayerStubBase::resolveOutputDevice(bool *usesDefault) const
{
    const QList<QAudioDevice> devices = availableOutputDevices();
    if (!m_selectedOutputDeviceId.isEmpty()) {
        const auto foundIt = std::find_if(devices.cbegin(), devices.cend(), [this](const QAudioDevice &device) {
            return device.id() == m_selectedOutputDeviceId;
        });
        if (foundIt != devices.cend()) {
            if (usesDefault) {
                *usesDefault = false;
            }
            return *foundIt;
        }
    }

    if (usesDefault) {
        *usesDefault = true;
    }
    return QMediaDevices::defaultAudioOutput();
}
