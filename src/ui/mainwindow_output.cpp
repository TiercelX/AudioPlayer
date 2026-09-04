#include "audioplayerbackend.h"
#include "mainwindow.h"
#include "mainwindow_helpers.h"
#include "playerlogger.h"
#include "ui_mainwindow.h"

#include <QAction>
#include <QActionGroup>
#include <QAudioDevice>
#include <QMediaDevices>
#include <QMenu>
#include <QMessageBox>
#include <QSettings>
#include <QThread>

#if defined(Q_OS_WINDOWS)
#include "windowsasioaudioplayer.h"
#include "windowsasioaudioplayer_sessionprobe.h"
#endif

#include <algorithm>

namespace {

using namespace MainWindowHelpers;

QString outputActionData(AudioPlayerBackend::BackendId backendId, const QByteArray &deviceId)
{
    return QStringLiteral("%1:%2")
        .arg(static_cast<int>(backendId))
        .arg(QString::fromLatin1(deviceId.toBase64()));
}

bool parseOutputActionData(const QVariant &data,
                           AudioPlayerBackend::BackendId *backendId,
                           QByteArray *deviceId)
{
    const QString token = data.toString();
    const qsizetype separatorIndex = token.indexOf(QChar(u':'));
    if (separatorIndex <= 0) {
        return false;
    }

    bool backendOk = false;
    const int backendValue = token.left(separatorIndex).toInt(&backendOk);
    if (!backendOk) {
        return false;
    }

    *backendId = static_cast<AudioPlayerBackend::BackendId>(backendValue);
    *deviceId = QByteArray::fromBase64(token.mid(separatorIndex + 1).toLatin1());
    return true;
}

} // namespace

void MainWindow::rebuildOutputDeviceMenu()
{
    if (!m_outputDeviceMenu || !m_outputDeviceActionGroup || !m_player) {
        return;
    }
#if defined(Q_OS_WINDOWS)
    WindowsAsioAudioPlayer::setHostWindowHandle(static_cast<quintptr>(winId()));
#endif

    const QList<QAction *> existingActions = m_outputDeviceActionGroup->actions();
    for (QAction *action : existingActions) {
        m_outputDeviceActionGroup->removeAction(action);
    }
    m_outputDeviceMenu->clear();

    const AudioPlayerBackend::BackendId currentBackendId = m_player->backendId();
    const AudioPlayerBackend::BackendId systemBackendId = systemOutputBackendId();

    auto *systemSectionAction = m_outputDeviceMenu->addAction(tr("系统输出"));
    systemSectionAction->setEnabled(false);

    auto *defaultAction = m_outputDeviceMenu->addAction(tr("默认输出设备"));
    defaultAction->setCheckable(true);
    defaultAction->setData(outputActionData(systemBackendId, QByteArray()));
    defaultAction->setActionGroup(m_outputDeviceActionGroup);
    defaultAction->setChecked(currentBackendId == systemBackendId && m_player->usesDefaultOutputDevice());

    const QList<QAudioDevice> outputDevices = QMediaDevices::audioOutputs();
    if (outputDevices.isEmpty()) {
        auto *unavailableAction = m_outputDeviceMenu->addAction(tr("未找到输出设备"));
        unavailableAction->setEnabled(false);
    } else {
        const QByteArray selectedDeviceId = m_player->selectedOutputDeviceId();
        for (const QAudioDevice &device : outputDevices) {
            const QAudioFormat preferredFormat = device.preferredFormat();
            QString actionText = device.description();
            if (preferredFormat.isValid()) {
                actionText = tr("%1 (%2ch, %3 Hz)")
                                 .arg(device.description())
                                 .arg(preferredFormat.channelCount())
                                 .arg(preferredFormat.sampleRate());
            }

            auto *deviceAction = m_outputDeviceMenu->addAction(actionText);
            deviceAction->setCheckable(true);
            deviceAction->setData(outputActionData(systemBackendId, device.id()));
            deviceAction->setActionGroup(m_outputDeviceActionGroup);
            deviceAction->setChecked(currentBackendId == systemBackendId
                                     && !m_player->usesDefaultOutputDevice()
                                     && device.id() == selectedDeviceId);
        }
    }

    m_outputDeviceMenu->addSeparator();
    auto *asioSectionAction = m_outputDeviceMenu->addAction(tr("ASIO"));
    asioSectionAction->setEnabled(false);

#if defined(Q_OS_WINDOWS)
    const QList<AudioOutputDeviceInfo> asioDevices = WindowsAsioAudioPlayer::availableAsioOutputDevices();
    if (asioDevices.isEmpty()) {
        auto *unavailableAsioAction = m_outputDeviceMenu->addAction(tr("未找到可用 ASIO 驱动"));
        unavailableAsioAction->setEnabled(false);
    } else {
        const QByteArray selectedDeviceId = m_player->selectedOutputDeviceId();
        QByteArray defaultAsioDeviceId = asioDevices.constFirst().id;
        const auto verifiedDefaultIt =
            std::find_if(asioDevices.cbegin(), asioDevices.cend(), [](const AudioOutputDeviceInfo &device) {
                return device.transport != QStringLiteral("ASIO-unverified");
            });
        if (verifiedDefaultIt != asioDevices.cend()) {
            defaultAsioDeviceId = verifiedDefaultIt->id;
        }
        for (const AudioOutputDeviceInfo &device : asioDevices) {
            QString actionText = device.description;
            if (device.preferredFormat.isValid()) {
                actionText = tr("%1 (%2ch, %3 Hz)")
                                 .arg(device.description)
                                 .arg(device.preferredFormat.channelCount())
                                 .arg(device.preferredFormat.sampleRate());
            }
            if (device.transport == QStringLiteral("ASIO-unverified")) {
                actionText = tr("%1 - 未验证").arg(actionText);
            }

            auto *deviceAction = m_outputDeviceMenu->addAction(actionText);
            deviceAction->setCheckable(true);
            deviceAction->setData(outputActionData(AudioPlayerBackend::BackendId::WindowsAsio, device.id));
            deviceAction->setActionGroup(m_outputDeviceActionGroup);
            deviceAction->setChecked(currentBackendId == AudioPlayerBackend::BackendId::WindowsAsio
                                     && (m_player->usesDefaultOutputDevice()
                                             ? device.id == defaultAsioDeviceId
                                             : device.id == selectedDeviceId));
        }
    }
#else
    auto *unsupportedAsioAction = m_outputDeviceMenu->addAction(tr("当前平台不支持 ASIO"));
    unsupportedAsioAction->setEnabled(false);
#endif
}

void MainWindow::refreshOutputDeviceInfo()
{
    if (!m_player) {
        return;
    }

    m_mediaInfoDialog->setBackendName(m_player->backendName());
    m_mediaInfoDialog->setDecoderName(m_player->decoderName());

    const AudioOutputDeviceInfo device = m_player->selectedOutputDeviceInfo();
    const bool asioUnverified = m_player->backendId() == AudioPlayerBackend::BackendId::WindowsAsio
        && device.transport == QStringLiteral("ASIO-unverified");
    const QString asioDescription = asioUnverified
        ? tr("%1（注册表枚举，能力未验证）").arg(device.description)
        : device.description;
    const QString deviceDescription = device.isNull()
        ? tr("未找到输出设备")
        : (m_player->backendId() == AudioPlayerBackend::BackendId::WindowsAsio
               ? (m_player->usesDefaultOutputDevice()
                      ? tr("默认 ASIO：%1").arg(asioDescription)
                      : tr("ASIO：%1").arg(asioDescription))
               : (m_player->usesDefaultOutputDevice()
                      ? tr("默认输出设备：%1").arg(device.description)
                      : device.description));
    const QAudioFormat format = m_player->outputFormat().isValid()
        ? m_player->outputFormat()
        : device.preferredFormat;
    if (!format.isValid()) {
        m_outputInfo = {};
        m_outputInfo.status = deviceDescription;
        m_mediaInfoDialog->setOutputInfo(m_outputInfo);
        return;
    }

    updateOutputInfoFromFormat(deviceDescription, format);
    const bool exclusiveModeEnabled = m_player->exclusiveModeEnabled();
    const bool stabilityModeEnabled =
        m_player->backendId() == AudioPlayerBackend::BackendId::WindowsWasapi
        && m_player->stabilityModeEnabled();
    m_mediaInfoDialog->setOutputAudioMode(
        m_player->backendId() == AudioPlayerBackend::BackendId::WindowsAsio
            ? (asioUnverified ? tr("ASIO（能力未验证）") : tr("ASIO"))
            : (stabilityModeEnabled ? tr("稳定模式（共享，高缓冲）")
                                     : (exclusiveModeEnabled ? tr("独占模式") : tr("共享模式"))));
    if (m_exclusiveModeAction) {
        QSignalBlocker blocker(m_exclusiveModeAction);
        m_exclusiveModeAction->setChecked(exclusiveModeEnabled);
        m_exclusiveModeAction->setEnabled(!stabilityModeEnabled);
    }
    if (m_stabilityModeAction) {
        QSignalBlocker blocker(m_stabilityModeAction);
        m_stabilityModeAction->setChecked(stabilityModeEnabled);
        m_stabilityModeAction->setEnabled(true);
    }
    if (m_creativeReorderActionGroup) {
        const int reorderMode = m_player->creativeChannelReorderMode();
        QSignalBlocker blocker(m_creativeReorderActionGroup);
        m_creativeReorderAutoAction->setChecked(reorderMode == 0);
        m_creativeReorderOffAction->setChecked(reorderMode == 1);
        m_creativeReorderForceAction->setChecked(reorderMode == 2);
    }
}

void MainWindow::selectOutputDeviceAction(QAction *action)
{
    if (!action || !m_player) {
        return;
    }

    AudioPlayerBackend::BackendId backendId = m_player->backendId();
    QByteArray deviceId;
    if (!parseOutputActionData(action->data(), &backendId, &deviceId)) {
        deviceId = action->data().toByteArray();
    }
    PlayerLogger::log(QStringLiteral("ui"),
                      QStringLiteral("selectOutputDeviceAction backend=%1 id=%2 text=%3")
                          .arg(static_cast<int>(backendId))
                          .arg(QString::fromLatin1(deviceId.toHex()))
                          .arg(action->text()));

#if defined(Q_OS_WINDOWS)
    if (backendId == AudioPlayerBackend::BackendId::WindowsAsio) {
        const auto multiCheck = AsioSessionProbe::detectMultiplePhysicalDevicesForAsioDriver(deviceId);
        if (multiCheck.multipleDetected) {
            QString deviceList;
            for (const QString &name : multiCheck.deviceNames) {
                deviceList += QStringLiteral("  - %1\n").arg(name);
            }
            QMessageBox msgBox(this);
            msgBox.setWindowTitle(tr("检测到多个同类 ASIO 设备"));
            msgBox.setIcon(QMessageBox::Warning);
            msgBox.setText(tr("检测到以下多个 %1 设备同时连接：\n\n%2\n"
                              "%1 ASIO 驱动在多个设备同时连接时可能无法正常工作（音频回调不会触发）。\n\n"
                              "建议：断开其中一个设备，或使用 WASAPI 模式。")
                               .arg(multiCheck.vendorLabel, deviceList));
            msgBox.setStandardButtons(QMessageBox::Ok);
            msgBox.button(QMessageBox::Ok)->setText(tr("确认"));
            msgBox.exec();

            PlayerLogger::log(QStringLiteral("ui"),
                              QStringLiteral("selectOutputDeviceAction multi-device-warning confirmed vendor=%1 devices=%2")
                                  .arg(multiCheck.vendorLabel)
                                  .arg(multiCheck.deviceNames.join(QLatin1String(", "))));
            rebuildOutputDeviceMenu();
            return;
        }
    }
#endif

    switchOutputBackendAndDevice(backendId, deviceId);
    if (backendId == AudioPlayerBackend::BackendId::WindowsWasapi) {
        m_lastSelectedWasapiDeviceId = deviceId;
    }
}

void MainWindow::applyCreativeReorderMode(QAction *action)
{
    if (!action || !m_player) {
        return;
    }
    int mode = 0;
    if (action == m_creativeReorderOffAction) {
        mode = 1;
    } else if (action == m_creativeReorderForceAction) {
        mode = 2;
    }
    m_player->setCreativeChannelReorderMode(mode);

    if (m_player->playbackState() == AudioPlayerBackend::PlaybackState::Playing) {
        m_player->refreshOutputConfiguration(true);
    }

    QSettings settings(MainWindowHelpers::kSettingsOrganization,
                       MainWindowHelpers::kSettingsApplication);
    settings.setValue(QStringLiteral("player/creativeChannelReorder"), mode);
}

void MainWindow::switchOutputBackendAndDevice(AudioPlayerBackend::BackendId backendId,
                                              const QByteArray &deviceId,
                                              bool applyWasapiMode,
                                              bool exclusiveMode,
                                              bool stabilityMode)
{
    if (!m_player) {
        return;
    }

    const bool backendChanged = m_player->backendId() != backendId;
    if (!backendChanged) {
        m_player->setOutputDeviceId(deviceId);
        if (applyWasapiMode) {
            setCurrentWasapiMode(exclusiveMode, stabilityMode);
            if (m_player->playbackState() != AudioPlayerBackend::PlaybackState::Stopped) {
                m_player->refreshOutputConfiguration(true);
            }
        }
        rebuildOutputDeviceMenu();
        refreshOutputDeviceInfo();
        return;
    }

    const QString sourcePath = m_player->source();
    const qint64 positionMs = ui->sliderProgress->value();
    const auto previousState = m_player->playbackState();
    const bool shouldResume = previousState == AudioPlayerBackend::PlaybackState::Playing;
    const bool shouldRearm = previousState == AudioPlayerBackend::PlaybackState::Playing
        || previousState == AudioPlayerBackend::PlaybackState::Paused;
    if (shouldRearm) {
        m_pendingSeekPosition = positionMs > 0 ? positionMs : -1;
        m_player->stop();
    }

#if defined(Q_OS_WINDOWS)
    constexpr int kExclusiveBackendSwitchCooldownMs = 300;
    const bool wasapiExclusiveToAsio = backendId == AudioPlayerBackend::BackendId::WindowsAsio
        && m_player->backendId() == AudioPlayerBackend::BackendId::WindowsWasapi
        && m_player->exclusiveModeEnabled();
    const bool asioToWasapiExclusive = m_player->backendId() == AudioPlayerBackend::BackendId::WindowsAsio
        && backendId == AudioPlayerBackend::BackendId::WindowsWasapi
        && applyWasapiMode && exclusiveMode;
    if (wasapiExclusiveToAsio || asioToWasapiExclusive) {
        PlayerLogger::log(QStringLiteral("ui"),
                          QStringLiteral("backendSwitchCooldown direction=%1 ms=%2")
                              .arg(wasapiExclusiveToAsio
                                       ? QStringLiteral("wasapi-exclusive-to-asio")
                                       : QStringLiteral("asio-to-wasapi-exclusive"))
                              .arg(kExclusiveBackendSwitchCooldownMs));
        QThread::msleep(kExclusiveBackendSwitchCooldownMs);
    }
#endif

    replacePlayer(backendId);
    m_player->setOutputDeviceId(deviceId);
    if (!sourcePath.isEmpty()) {
        m_player->setSource(sourcePath,
                            m_sourceInfo.channelCount,
                            m_sourceInfo.sampleRateValue,
                            m_sourceInfo.bitDepthValue,
                            m_sourceInfo.codecName);
    }
    if (applyWasapiMode) {
        setCurrentWasapiMode(exclusiveMode, stabilityMode);
    }
    if (!sourcePath.isEmpty()) {
        m_mediaInfoDialog->setDecoderName(m_player->decoderName());
        updatePlaybackState();
        m_pendingSeekPosition = shouldRearm ? positionMs : -1;
        if (shouldRearm && positionMs > 0) {
            m_player->play();
            m_player->seek(positionMs);
            if (!shouldResume) {
                m_player->pause();
            }
        } else if (shouldResume) {
            m_player->play();
        }
    }
    refreshOutputDeviceInfo();
}

void MainWindow::switchToWasapiMode(bool exclusiveMode, bool stabilityMode)
{
    if (!m_player) {
        return;
    }

    QByteArray targetDeviceId;
    if (m_player->backendId() == AudioPlayerBackend::BackendId::WindowsWasapi
        && !m_player->usesDefaultOutputDevice()) {
        targetDeviceId = m_player->selectedOutputDeviceId();
    }
#if defined(Q_OS_WINDOWS)
    else if (m_player->backendId() == AudioPlayerBackend::BackendId::WindowsAsio) {
        const QByteArray asioDeviceId = m_player->selectedOutputDeviceId();
        if (!asioDeviceId.isEmpty()) {
            targetDeviceId = AsioSessionProbe::resolveWasapiEndpointForAsioDriver(asioDeviceId);
            if (!targetDeviceId.isEmpty()) {
                PlayerLogger::log(QStringLiteral("ui"),
                                  QStringLiteral("switchToWasapiMode resolvedAsioEndpoint asioId=%1 wasapiEndpoint=%2")
                                      .arg(QString::fromUtf8(asioDeviceId),
                                           QString::fromUtf8(targetDeviceId)));
            }
        }
    }
#endif
    if (targetDeviceId.isEmpty() && !m_lastSelectedWasapiDeviceId.isEmpty()) {
        targetDeviceId = m_lastSelectedWasapiDeviceId;
    }
    if (!targetDeviceId.isEmpty()) {
        m_lastSelectedWasapiDeviceId = targetDeviceId;
    }

    switchOutputBackendAndDevice(AudioPlayerBackend::BackendId::WindowsWasapi,
                                 targetDeviceId,
                                 true,
                                 exclusiveMode,
                                 stabilityMode);
}

void MainWindow::setCurrentWasapiMode(bool exclusiveMode, bool stabilityMode)
{
    if (!m_player || m_player->backendId() != AudioPlayerBackend::BackendId::WindowsWasapi) {
        return;
    }

    if (stabilityMode) {
        m_player->setExclusiveModeEnabled(false);
        m_player->setStabilityModeEnabled(true);
        return;
    }

    m_player->setStabilityModeEnabled(false);
    m_player->setExclusiveModeEnabled(exclusiveMode);
}

void MainWindow::updateOutputInfoFromFormat(const QString &deviceDescription, const QAudioFormat &format)
{
    if (!format.isValid()) {
        return;
    }

    const int deviceBitDepth = m_player ? m_player->outputDeviceBitDepth() : 0;
    const int effectiveBitDepth = deviceBitDepth > 0 ? deviceBitDepth : (format.bytesPerSample() * 8);

    m_outputInfo.audioFormat = formatOutputSampleFormat(format.sampleFormat(), effectiveBitDepth);
    m_outputInfo.channelCount = format.channelCount();
    m_outputInfo.channels = formatChannelDescription(m_outputInfo.channelCount, QString());
    m_outputInfo.sampleRateValue = format.sampleRate();
    m_outputInfo.sampleRate = formatSampleRate(m_outputInfo.sampleRateValue);
    m_outputInfo.bitDepthValue = effectiveBitDepth;
    m_outputInfo.bitDepth = formatBitDepth(m_outputInfo.bitDepthValue);
    const qint64 bitRate = static_cast<qint64>(format.sampleRate()) * format.channelCount()
        * format.bytesPerSample() * 8;
    m_outputInfo.bitRateValue = bitRate;
    m_outputInfo.bitRate = formatBitRate(QString::number(bitRate));
    m_outputInfo.status = deviceDescription;
    m_mediaInfoDialog->setOutputInfo(m_outputInfo);
}

void MainWindow::listOutputDevicesForAutomation() const
{
    if (!m_player) {
        PlayerLogger::log(QStringLiteral("automation"),
                          QStringLiteral("outputDeviceList unavailable hasPlayer=0"));
        return;
    }

    const QList<QAudioDevice> devices = QMediaDevices::audioOutputs();
    PlayerLogger::log(QStringLiteral("automation"),
                      QStringLiteral("outputDeviceList count=%1 defaultIndex=-1")
                          .arg(devices.size()));
    for (int index = 0; index < devices.size(); ++index) {
        const QAudioDevice &device = devices.at(index);
        const QAudioFormat preferredFormat = device.preferredFormat();
        PlayerLogger::log(QStringLiteral("automation"),
                          QStringLiteral("outputDevice index=%1 id=%2 description=%3 preferredRate=%4 preferredChannels=%5 preferredSampleFormat=%6")
                              .arg(index)
                              .arg(QString::fromLatin1(device.id().toHex()))
                              .arg(compactLogValue(device.description()))
                              .arg(preferredFormat.isValid() ? preferredFormat.sampleRate() : 0)
                              .arg(preferredFormat.isValid() ? preferredFormat.channelCount() : 0)
                              .arg(preferredFormat.isValid()
                                       ? static_cast<int>(preferredFormat.sampleFormat())
                                       : -1));
    }
#if defined(Q_OS_WINDOWS)
    const QList<AudioOutputDeviceInfo> asioDevices = WindowsAsioAudioPlayer::availableAsioOutputDevices();
    PlayerLogger::log(QStringLiteral("automation"),
                      QStringLiteral("asioOutputDeviceList count=%1")
                          .arg(asioDevices.size()));
    for (int index = 0; index < asioDevices.size(); ++index) {
        const AudioOutputDeviceInfo &device = asioDevices.at(index);
        const QAudioFormat preferredFormat = device.preferredFormat;
        const QString transport = device.transport.isEmpty() ? QStringLiteral("ASIO") : device.transport;
        PlayerLogger::log(QStringLiteral("automation"),
                          QStringLiteral("asioOutputDevice index=%1 id=%2 description=%3 preferredRate=%4 preferredChannels=%5 preferredSampleFormat=%6 transport=%7")
                              .arg(index)
                              .arg(QString::fromLatin1(device.id.toHex()))
                              .arg(compactLogValue(device.description))
                              .arg(preferredFormat.isValid() ? preferredFormat.sampleRate() : 0)
                              .arg(preferredFormat.isValid() ? preferredFormat.channelCount() : 0)
                              .arg(preferredFormat.isValid()
                                       ? static_cast<int>(preferredFormat.sampleFormat())
                                       : -1)
                              .arg(transport));
    }
#endif
}

bool MainWindow::selectOutputDeviceByIndex(int index)
{
    if (!m_player) {
        PlayerLogger::log(QStringLiteral("ui"),
                          QStringLiteral("selectOutputDeviceByIndex ignored no-player index=%1")
                              .arg(index));
        return false;
    }

    if (index < 0) {
        PlayerLogger::log(QStringLiteral("ui"),
                          QStringLiteral("selectOutputDeviceByIndex default index=%1")
                              .arg(index));
        logAutomationEvent(QStringLiteral("action=select-output-device index=%1 default=1")
                               .arg(index));
        m_player->setOutputDeviceId(QByteArray());
        return true;
    }

    const QList<QAudioDevice> devices = m_player->availableOutputDevices();
    if (index >= devices.size()) {
        PlayerLogger::log(QStringLiteral("ui"),
                          QStringLiteral("selectOutputDeviceByIndex ignored out-of-range index=%1 count=%2")
                              .arg(index)
                              .arg(devices.size()));
        return false;
    }

    const QAudioDevice &device = devices.at(index);
    PlayerLogger::log(QStringLiteral("ui"),
                      QStringLiteral("selectOutputDeviceByIndex index=%1 id=%2 description=%3")
                          .arg(index)
                          .arg(QString::fromLatin1(device.id().toHex()))
                          .arg(compactLogValue(device.description())));
    logAutomationEvent(QStringLiteral("action=select-output-device index=%1 id=%2")
                           .arg(index)
                           .arg(QString::fromLatin1(device.id().toHex())));
    m_player->setOutputDeviceId(device.id());
    return true;
}

bool MainWindow::selectAsioOutputDeviceByIndex(int index)
{
#if defined(Q_OS_WINDOWS)
    const QList<AudioOutputDeviceInfo> devices = WindowsAsioAudioPlayer::availableAsioOutputDevices();
    if (index < 0 || index >= devices.size()) {
        PlayerLogger::log(QStringLiteral("ui"),
                          QStringLiteral("selectAsioOutputDeviceByIndex ignored out-of-range index=%1 count=%2")
                              .arg(index)
                              .arg(devices.size()));
        return false;
    }

    const AudioOutputDeviceInfo &device = devices.at(index);
    PlayerLogger::log(QStringLiteral("ui"),
                      QStringLiteral("selectAsioOutputDeviceByIndex index=%1 id=%2 description=%3")
                          .arg(index)
                          .arg(QString::fromLatin1(device.id.toHex()))
                          .arg(compactLogValue(device.description)));
    logAutomationEvent(QStringLiteral("action=select-asio-output-device index=%1 id=%2")
                           .arg(index)
                           .arg(QString::fromLatin1(device.id.toHex())));
    switchOutputBackendAndDevice(AudioPlayerBackend::BackendId::WindowsAsio, device.id);
    return true;
#else
    Q_UNUSED(index);
    PlayerLogger::log(QStringLiteral("ui"),
                      QStringLiteral("selectAsioOutputDeviceByIndex ignored unsupported-platform"));
    return false;
#endif
}
