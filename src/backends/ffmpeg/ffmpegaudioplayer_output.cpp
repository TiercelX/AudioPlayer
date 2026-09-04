#include "ffmpegaudioplayer.h"

#include "playerlogger.h"

#include <QMediaDevices>

#include <algorithm>

QList<QAudioDevice> FfmpegAudioPlayer::availableOutputDevices() const
{
    return QMediaDevices::audioOutputs();
}

QString FfmpegAudioPlayer::outputDeviceDescription() const
{
    return m_outputDeviceDescription;
}

QAudioFormat FfmpegAudioPlayer::outputFormat() const
{
    return m_outputFormat;
}

QAudioDevice FfmpegAudioPlayer::selectedOutputDevice() const
{
    return resolveOutputDevice();
}

QByteArray FfmpegAudioPlayer::selectedOutputDeviceId() const
{
    return m_selectedOutputDeviceId;
}

bool FfmpegAudioPlayer::usesDefaultOutputDevice() const
{
    return m_selectedOutputDeviceId.isEmpty();
}

void FfmpegAudioPlayer::setOutputDeviceId(const QByteArray &deviceId)
{
    QByteArray normalizedDeviceId = deviceId;
    if (!normalizedDeviceId.isEmpty()) {
        const auto devices = availableOutputDevices();
        const auto foundIt = std::find_if(devices.cbegin(), devices.cend(), [&normalizedDeviceId](const QAudioDevice &device) {
            return device.id() == normalizedDeviceId;
        });
        if (foundIt == devices.cend()) {
            PlayerLogger::log(QStringLiteral("player"),
                              QStringLiteral("setOutputDeviceId fallback-default unknownId=%1")
                                  .arg(QString::fromLatin1(normalizedDeviceId.toHex())));
            normalizedDeviceId.clear();
        }
    }

    if (m_selectedOutputDeviceId == normalizedDeviceId) {
        return;
    }

    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("setOutputDeviceId previous=%1 target=%2")
                          .arg(QString::fromLatin1(m_selectedOutputDeviceId.toHex()))
                          .arg(QString::fromLatin1(normalizedDeviceId.toHex())));
    m_selectedOutputDeviceId = normalizedDeviceId;
    emitOutputDeviceSelectionChanged();
    applyOutputDeviceChange();
}

void FfmpegAudioPlayer::handleAudioOutputsChanged()
{
    const auto devices = availableOutputDevices();
    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("handleAudioOutputsChanged count=%1 selectedDevice=%2 activeDevice=%3")
                          .arg(devices.size())
                          .arg(QString::fromLatin1(m_selectedOutputDeviceId.toHex()))
                          .arg(QString::fromLatin1(m_activeOutputDeviceId.toHex())));
    bool selectionChanged = false;
    if (!m_selectedOutputDeviceId.isEmpty()) {
        const auto foundIt = std::find_if(devices.cbegin(), devices.cend(), [this](const QAudioDevice &device) {
            return device.id() == m_selectedOutputDeviceId;
        });
        if (foundIt == devices.cend()) {
            PlayerLogger::log(QStringLiteral("player"),
                              QStringLiteral("handleAudioOutputsChanged selected-device-missing fallback-default id=%1")
                                  .arg(QString::fromLatin1(m_selectedOutputDeviceId.toHex())));
            m_selectedOutputDeviceId.clear();
            selectionChanged = true;
        }
    }

    emit outputDevicesChanged();
    if (selectionChanged) {
        emitOutputDeviceSelectionChanged();
    }

    applyOutputDeviceChange();
}

void FfmpegAudioPlayer::applyOutputDeviceChange()
{
    if (m_sourcePath.isEmpty() || m_playbackState == PlaybackState::Stopping) {
        return;
    }

    const QAudioDevice device = resolveOutputDevice();
    const QByteArray targetDeviceId = device.id();
    if (targetDeviceId == m_activeOutputDeviceId) {
        return;
    }

    if (m_playbackState == PlaybackState::Playing) {
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("applyOutputDeviceChange restart-playing positionMs=%1 previousDevice=%2 targetDevice=%3")
                              .arg(m_currentPositionMs)
                              .arg(QString::fromLatin1(m_activeOutputDeviceId.toHex()))
                              .arg(QString::fromLatin1(targetDeviceId.toHex())));
        startPipeline(m_currentPositionMs);
        return;
    }

    if (m_playbackState == PlaybackState::Paused) {
        PlayerLogger::log(QStringLiteral("player"),
                          QStringLiteral("applyOutputDeviceChange rearm-paused positionMs=%1 previousDevice=%2 targetDevice=%3")
                              .arg(m_currentPositionMs)
                              .arg(QString::fromLatin1(m_activeOutputDeviceId.toHex()))
                              .arg(QString::fromLatin1(targetDeviceId.toHex())));
        teardownPipeline();
        m_startPositionMs = m_currentPositionMs;
        emit positionChanged(m_currentPositionMs);
        setPlaybackState(PlaybackState::Paused);
    }
}

void FfmpegAudioPlayer::emitOutputDeviceSelectionChanged()
{
    const QAudioDevice device = resolveOutputDevice();
    PlayerLogger::log(QStringLiteral("player"),
                      QStringLiteral("outputDeviceSelectionChanged usesDefault=%1 id=%2 description=%3")
                          .arg(m_selectedOutputDeviceId.isEmpty())
                          .arg(QString::fromLatin1(device.id().toHex()))
                          .arg(device.description()));
    emit outputDeviceSelectionChanged();
}

QAudioDevice FfmpegAudioPlayer::resolveOutputDevice(bool *usesDefault) const
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

QAudioFormat FfmpegAudioPlayer::selectOutputFormat(QString *deviceDescription) const
{
    const QAudioDevice device = resolveOutputDevice();
    if (deviceDescription) {
        *deviceDescription = device.isNull() ? tr("默认输出设备")
                                             : (m_selectedOutputDeviceId.isEmpty()
                                                    ? tr("默认输出设备：%1").arg(device.description())
                                                    : device.description());
    }

    QAudioFormat preferred = device.preferredFormat();
    if (!preferred.isValid()) {
        return {};
    }

    QAudioFormat requested = preferred;
    if (m_sourceChannelCount > 0 && m_sourceChannelCount < requested.channelCount()) {
        requested.setChannelCount(m_sourceChannelCount);
    }

    return device.isFormatSupported(requested) ? requested : preferred;
}
