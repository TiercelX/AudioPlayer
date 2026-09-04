#include "audioplayerbackend.h"
#include "audioplayerfactory.h"
#include "audioutils.h"
#include "diagnosticsdashboard.h"
#include "diagnosticsservice.h"
#include "mediainfodialog.h"
#include "mainwindow.h"
#include "mainwindow_helpers.h"
#include "playerlogger.h"
#include "playbacksourceservice.h"
#include "ui_mainwindow.h"

#include <QAction>
#include <QActionGroup>
#include <QCloseEvent>
#include <QDateTime>
#include <QDialog>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>

#if defined(Q_OS_WINDOWS)
#include "windowsasioaudioplayer.h"
#endif

namespace {
using namespace MainWindowHelpers;

using PlaybackError = AudioPlayerBackend::PlaybackError;

bool isAsioStartupFailure(PlaybackError errorCode)
{
    return errorCode == PlaybackError::DeviceOccupied
        || errorCode == PlaybackError::DriverInitFailure
        || errorCode == PlaybackError::DriverRecoveryFailure;
}

bool shouldShowErrorDialog(PlaybackError errorCode)
{
    // P0 errors - require explicit user action
    return errorCode == PlaybackError::DeviceOccupied
        || errorCode == PlaybackError::DriverInitFailure
        || errorCode == PlaybackError::OutputRecoveryFailure
        || errorCode == PlaybackError::DecoderNotFound
        || errorCode == PlaybackError::FormatNotSupported;
}

}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , m_mediaInfoDialog(new MediaInfoDialog(this))
    , m_probedDuration(0)
{
    ui->setupUi(this);
#if defined(Q_OS_WINDOWS)
    WindowsAsioAudioPlayer::setHostWindowHandle(static_cast<quintptr>(winId()));
#endif

    m_loadProgressFrame = new QFrame(this);
    m_loadProgressFrame->setFrameShape(QFrame::StyledPanel);
    m_loadProgressFrame->setVisible(false);
    auto *loadProgressLayout = new QVBoxLayout(m_loadProgressFrame);
    loadProgressLayout->setContentsMargins(12, 10, 12, 10);
    loadProgressLayout->setSpacing(6);

    m_loadProgressTitleLabel = new QLabel(m_loadProgressFrame);
    m_loadProgressTitleLabel->setWordWrap(true);
    m_loadProgressBar = new QProgressBar(m_loadProgressFrame);
    m_loadProgressBar->setRange(0, 0);
    m_loadProgressBar->setTextVisible(true);
    m_loadProgressStatsLabel = new QLabel(m_loadProgressFrame);
    m_loadProgressStatsLabel->setWordWrap(true);

    loadProgressLayout->addWidget(m_loadProgressTitleLabel);
    loadProgressLayout->addWidget(m_loadProgressBar);
    loadProgressLayout->addWidget(m_loadProgressStatsLabel);
    ui->verticalLayout->insertWidget(1, m_loadProgressFrame);

    m_loadProgressShowTimer = new QTimer(this);
    m_loadProgressShowTimer->setSingleShot(true);
    connect(m_loadProgressShowTimer, &QTimer::timeout, this, [this] {
        if (!m_isLoadingFile) {
            return;
        }

        updateLoadingProgress();
        m_loadProgressFrame->setVisible(true);
        m_loadProgressUpdateTimer->start();
    });

    m_loadProgressUpdateTimer = new QTimer(this);
    m_loadProgressUpdateTimer->setTimerType(Qt::PreciseTimer);
    m_loadProgressUpdateTimer->setInterval(250);
    connect(m_loadProgressUpdateTimer, &QTimer::timeout,
            this, &MainWindow::updateLoadingProgress);

    // ASIO retry status bar update timer
    m_asioRetryUpdateTimer = new QTimer(this);
    m_asioRetryUpdateTimer->setTimerType(Qt::PreciseTimer);
    m_asioRetryUpdateTimer->setInterval(250);
    connect(m_asioRetryUpdateTimer, &QTimer::timeout, this, [this] {
        if (!m_asioRetryActive) {
            return;
        }
        const qint64 elapsedMs = m_asioRetryElapsedOffsetMs
            + (m_asioRetryElapsedTimer.isValid() ? m_asioRetryElapsedTimer.elapsed() : 0);
        const int elapsedSec = qBound(0, static_cast<int>(elapsedMs / 1000), m_asioRetryTimeoutSec);
        const int remainingSec = m_asioRetryTimeoutSec - elapsedSec;
        if (remainingSec > 0) {
            if (elapsedSec != m_asioRetryLastShownSec) {
                m_asioRetryLastShownSec = elapsedSec;
                statusBar()->showMessage(
                    tr("%1（%2s/%3s）").arg(m_asioRetryStatusPrefix).arg(elapsedSec).arg(m_asioRetryTimeoutSec));
            }
        } else {
            const QString timeoutMessage = m_asioRetryTimeoutMessage.isEmpty()
                ? tr("%1（%2s/%3s）").arg(m_asioRetryStatusPrefix).arg(m_asioRetryTimeoutSec).arg(m_asioRetryTimeoutSec)
                : m_asioRetryTimeoutMessage;
            statusBar()->showMessage(timeoutMessage, 18000);
            m_asioRetryActive = false;
            m_asioRetryUpdateTimer->stop();
        }
    });

    // Playback rebuild status bar update timer
    m_rebuildStatusTimer = new QTimer(this);
    m_rebuildStatusTimer->setTimerType(Qt::PreciseTimer);
    m_rebuildStatusTimer->setInterval(250);
    connect(m_rebuildStatusTimer, &QTimer::timeout, this, [this] {
        if (!m_rebuildActive) {
            return;
        }
        const qint64 elapsedMs = QDateTime::currentMSecsSinceEpoch() - m_rebuildStartTimeMs;
        const int elapsedSec = static_cast<int>(elapsedMs / 1000);
        if (elapsedSec != m_rebuildLastShownSec) {
            m_rebuildLastShownSec = elapsedSec;
            statusBar()->showMessage(tr("正在重建播放管线（%1s）").arg(elapsedSec));
        }
    });

    replacePlayer(systemOutputBackendId());

    m_player->setVolume(ui->sliderVolume->value() / 100.0);
    ui->labelVolumeValue->setText(tr("%1%").arg(ui->sliderVolume->value()));
    resetMediaInfo();
    PlayerLogger::log(QStringLiteral("ui"),
                      QStringLiteral("MainWindow initialized, log=%1").arg(PlayerLogger::logFilePath()));

    ui->sliderProgress->setRange(0, 0);
    ui->sliderProgress->setEnabled(false);
    ui->buttonPlayPause->setEnabled(false);
    ui->buttonStop->setEnabled(false);

    auto *playbackMenu = menuBar()->addMenu(tr("播放"));
    m_outputDeviceMenu = playbackMenu->addMenu(tr("输出设备"));
    m_outputDeviceActionGroup = new QActionGroup(this);
    m_outputDeviceActionGroup->setExclusive(true);
    connect(m_outputDeviceActionGroup, &QActionGroup::triggered,
            this, &MainWindow::selectOutputDeviceAction);

    m_exclusiveModeAction = playbackMenu->addAction(tr("独占模式"));
    m_exclusiveModeAction->setCheckable(true);
    {
        QSignalBlocker blocker(m_exclusiveModeAction);
        m_exclusiveModeAction->setChecked(false);
    }
    connect(m_exclusiveModeAction, &QAction::toggled, this, [this](bool checked) {
        if (!m_player) {
            return;
        }
        if (checked && m_stabilityModeAction && m_stabilityModeAction->isChecked()) {
            QSignalBlocker blocker(m_stabilityModeAction);
            m_stabilityModeAction->setChecked(false);
        }
        if (checked) {
            switchToWasapiMode(true, false);
            if (m_exactPlaybackAction) {
                const bool stored = m_exactPlaybackAction->property("storedChecked").toBool();
                m_player->setExactPlaybackEnabled(stored);
                m_exactPlaybackAction->setEnabled(true);
                QSignalBlocker blocker(m_exactPlaybackAction);
                m_exactPlaybackAction->setChecked(stored);
            }
            return;
        }
        if (m_player->backendId() == AudioPlayerBackend::BackendId::WindowsWasapi
            && m_player->exclusiveModeEnabled()) {
            switchToWasapiMode(false, false);
            m_player->setExactPlaybackEnabled(false);
        } else {
            refreshOutputDeviceInfo();
        }
        if (m_exactPlaybackAction) {
            m_exactPlaybackAction->setEnabled(false);
            QSignalBlocker blocker(m_exactPlaybackAction);
            m_exactPlaybackAction->setChecked(false);
        }
    });

    m_stabilityModeAction = playbackMenu->addAction(tr("稳定模式（高缓冲）"));
    m_stabilityModeAction->setCheckable(true);
    {
        QSignalBlocker blocker(m_stabilityModeAction);
        m_stabilityModeAction->setChecked(false);
    }
    connect(m_stabilityModeAction, &QAction::toggled, this, [this](bool checked) {
        if (!m_player) {
            return;
        }
        if (checked && m_exclusiveModeAction && m_exclusiveModeAction->isChecked()) {
            QSignalBlocker blocker(m_exclusiveModeAction);
            m_exclusiveModeAction->setChecked(false);
        }
        if (checked) {
            switchToWasapiMode(false, true);
            if (m_exactPlaybackAction) {
                m_exactPlaybackAction->setEnabled(false);
                QSignalBlocker blocker(m_exactPlaybackAction);
                m_exactPlaybackAction->setChecked(false);
            }
            return;
        }
        if (m_player->backendId() == AudioPlayerBackend::BackendId::WindowsWasapi
            && m_player->stabilityModeEnabled()) {
            switchToWasapiMode(false, false);
        } else {
            refreshOutputDeviceInfo();
        }
        if (m_exactPlaybackAction) {
            m_exactPlaybackAction->setEnabled(false);
            QSignalBlocker blocker(m_exactPlaybackAction);
            m_exactPlaybackAction->setChecked(false);
        }
    });

    m_exactPlaybackAction = playbackMenu->addAction(tr("精确播放"));
    m_exactPlaybackAction->setCheckable(true);
    m_exactPlaybackAction->setEnabled(false);
    m_exactPlaybackAction->setStatusTip(tr("独占模式/ASIO 下优先匹配源采样率、位深和声道数。共享模式下无效"));
    {
        QSettings settings(MainWindowHelpers::kSettingsOrganization,
                           MainWindowHelpers::kSettingsApplication);
        const bool exactPlayback = settings.value(QStringLiteral("player/exactPlayback"), true).toBool();
        QSignalBlocker blocker(m_exactPlaybackAction);
        m_exactPlaybackAction->setChecked(false);
        m_exactPlaybackAction->setProperty("storedChecked", exactPlayback);
    }
    connect(m_exactPlaybackAction, &QAction::toggled, this, [this](bool checked) {
        if (m_exactPlaybackAction) {
            m_exactPlaybackAction->setProperty("storedChecked", checked);
        }
        if (m_player) {
            m_player->setExactPlaybackEnabled(checked);
            if (m_player->playbackState() != AudioPlayerBackend::PlaybackState::Stopped) {
                m_player->refreshOutputConfiguration(true);
            }
            refreshOutputDeviceInfo();
        }
        QSettings settings(MainWindowHelpers::kSettingsOrganization,
                           MainWindowHelpers::kSettingsApplication);
        settings.setValue(QStringLiteral("player/exactPlayback"), checked);
    });
    if (m_player) {
        m_player->setExactPlaybackEnabled(m_exactPlaybackAction->isChecked());
    }

    auto *channelReorderMenu = playbackMenu->addMenu(tr("声道重排"));
    m_creativeReorderActionGroup = new QActionGroup(this);
    m_creativeReorderActionGroup->setExclusive(true);

    m_creativeReorderAutoAction = channelReorderMenu->addAction(tr("自动检测"));
    m_creativeReorderAutoAction->setCheckable(true);
    m_creativeReorderAutoAction->setChecked(true);
    m_creativeReorderAutoAction->setStatusTip(tr("根据输出设备名称自动判断是否需要声道重排。Creative G5/G6 等设备会自动应用"));
    m_creativeReorderActionGroup->addAction(m_creativeReorderAutoAction);

    m_creativeReorderOffAction = channelReorderMenu->addAction(tr("关闭"));
    m_creativeReorderOffAction->setCheckable(true);
    m_creativeReorderOffAction->setStatusTip(tr("不对输出声道做任何重排，使用设备默认声道顺序"));
    m_creativeReorderActionGroup->addAction(m_creativeReorderOffAction);

    m_creativeReorderForceAction = channelReorderMenu->addAction(tr("强制重排"));
    m_creativeReorderForceAction->setCheckable(true);
    m_creativeReorderForceAction->setStatusTip(tr("强制应用声道重排策略，交换第3/4声道与第5/6声道。适用于声道错位的设备"));
    m_creativeReorderActionGroup->addAction(m_creativeReorderForceAction);

    connect(m_creativeReorderActionGroup, &QActionGroup::triggered,
            this, &MainWindow::applyCreativeReorderMode);

    playbackMenu->addSeparator();
    auto *actionCacheSettings = playbackMenu->addAction(tr("缓存设置..."));
    connect(actionCacheSettings, &QAction::triggered,
            this, &MainWindow::openCacheSettingsDialog);

    auto *viewMenu = menuBar()->addMenu(tr("查看"));
    auto *actionMediaInfo = viewMenu->addAction(tr("媒体信息"));
    viewMenu->addSeparator();
    m_diagnosticsAction = viewMenu->addAction(tr("性能监控"));
    m_diagnosticsAction->setCheckable(true);
    m_diagnosticsAction->setChecked(false);

    m_diagnosticsService = new DiagnosticsService(this);
    m_diagnosticsDashboard = new DiagnosticsDashboard(this);
    m_diagnosticsDashboard->setDiagnosticsService(m_diagnosticsService);
    m_diagnosticsDashboard->setVisible(false);
    addDockWidget(Qt::RightDockWidgetArea, m_diagnosticsDashboard);

    connect(m_diagnosticsAction, &QAction::toggled, this, [this](bool checked) {
        m_diagnosticsDashboard->setVisible(checked);
        if (checked && m_player) {
            m_diagnosticsService->attachToPlayer(m_player);
        } else if (!checked) {
            m_diagnosticsService->detachFromPlayer();
        }
    });

    connect(ui->buttonOpenFile, &QPushButton::clicked, this, &MainWindow::openAudioFile);
    connect(ui->buttonPlayPause, &QPushButton::clicked, this, &MainWindow::togglePlayback);
    connect(ui->buttonStop, &QPushButton::clicked, this, &MainWindow::stopPlayback);
    connect(actionMediaInfo, &QAction::triggered, this, &MainWindow::openMediaInfoDialog);
    ui->sliderProgress->installEventFilter(this);
    connect(ui->sliderProgress, &QSlider::sliderPressed, this, [this] {
        PlayerLogger::log(QStringLiteral("ui"),
                          QStringLiteral("sliderPressed state=%1 value=%2")
                              .arg(AudioUtils::playbackStateName(m_player->playbackState()))
                              .arg(ui->sliderProgress->value()));
        m_isSeeking = true;
        m_resumeAfterSeek = m_player->playbackState() == AudioPlayerBackend::PlaybackState::Playing;
        if (m_resumeAfterSeek) {
            m_player->pause();
        }
    });
    connect(ui->sliderProgress, &QSlider::sliderMoved, this, [this](int value) {
        ui->labelCurrentTime->setText(formatTime(value));
    });
    connect(ui->sliderProgress, &QSlider::sliderReleased, this, &MainWindow::seekToSliderPosition);
    connect(ui->sliderVolume, &QSlider::valueChanged, this, [this](int value) {
        m_player->setVolume(value / 100.0);
        ui->labelVolumeValue->setText(tr("%1%").arg(value));
    });

    rebuildOutputDeviceMenu();
    refreshOutputDeviceInfo();
    updatePlaybackState();
}

void MainWindow::connectPlayerSignals()
{
    Q_ASSERT(m_player);
    connect(m_player, &AudioPlayerBackend::outputFormatChanged, this, [this] {
        refreshOutputDeviceInfo();
    });
    connect(m_player, &AudioPlayerBackend::outputDevicesChanged, this,
            &MainWindow::rebuildOutputDeviceMenu);
    connect(m_player, &AudioPlayerBackend::outputDeviceSelectionChanged, this, [this] {
        rebuildOutputDeviceMenu();
        refreshOutputDeviceInfo();
    });
    connect(m_player, &AudioPlayerBackend::positionChanged, this, &MainWindow::updatePosition);
    connect(m_player, &AudioPlayerBackend::playbackStateChanged, this, [this](AudioPlayerBackend::PlaybackState state) {
        if (state == AudioPlayerBackend::PlaybackState::Stopped) {
            m_lastAutomationPositionBucket = -1;
            m_lastAutomationAudioLevelLogMs = -1;
            refreshOutputDeviceInfo();
        }
        logAutomationEvent(QStringLiteral("state=%1 ready=%2 hasSource=%3")
                               .arg(AudioUtils::playbackStateName(state))
                               .arg(isPlaybackAutomationReady())
                               .arg(m_player && !m_player->source().isEmpty()));
        updatePlaybackState();
    });
    connect(m_player, &AudioPlayerBackend::audioLevelsChanged, this, [this](qreal leftLevel, qreal rightLevel) {
        const qreal peakLevel = qMax(leftLevel, rightLevel);
        if (peakLevel <= 0.001) {
            return;
        }

        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (m_lastAutomationAudioLevelLogMs >= 0 && nowMs - m_lastAutomationAudioLevelLogMs < 250) {
            return;
        }

        m_lastAutomationAudioLevelLogMs = nowMs;
        logAutomationEvent(QStringLiteral("audioLevel peak=%1 left=%2 right=%3")
                               .arg(QString::number(peakLevel, 'f', 3))
                               .arg(QString::number(leftLevel, 'f', 3))
                               .arg(QString::number(rightLevel, 'f', 3)));
    });
    connect(m_player, &AudioPlayerBackend::finished, this, [this] {
        logAutomationEvent(QStringLiteral("finished"));
        handlePlaybackFinished();
    });
    connect(m_player, &AudioPlayerBackend::errorOccurred, this,
            [this](AudioPlayerBackend::PlaybackError errorCode, const QString &message) {
        PlayerLogger::log(QStringLiteral("ui"),
                          QStringLiteral("errorReceived code=%1 message=%2")
                              .arg(static_cast<int>(errorCode))
                              .arg(compactLogValue(message)));
        logAutomationEvent(QStringLiteral("error code=%1 message=%2")
                               .arg(static_cast<int>(errorCode))
                               .arg(compactLogValue(message)));
#if defined(Q_OS_WINDOWS)
        if (m_player
            && m_player->backendId() == AudioPlayerBackend::BackendId::WindowsAsio) {
            PlayerLogger::log(QStringLiteral("ui"),
                              QStringLiteral("asio fallback-to-wasapi-exclusive disabled reason=%1")
                                  .arg(compactLogValue(message)));
            if (isAsioStartupFailure(errorCode)) {
                logAutomationEvent(QStringLiteral("action=asio-fallback-disabled reason=%1")
                                       .arg(compactLogValue(message)));
            }
        }
#endif
        // Stop ASIO retry timer on error
        if (m_asioRetryActive) {
            m_asioRetryActive = false;
            m_asioRetryUpdateTimer->stop();
        }

        // Show dialog for critical errors, status bar for others
        if (shouldShowErrorDialog(errorCode)) {
            QMessageBox msgBox(this);
            msgBox.setWindowTitle(tr("播放错误"));
            msgBox.setIcon(QMessageBox::Warning);
            msgBox.setText(message);
            msgBox.setStandardButtons(QMessageBox::Ok);
            msgBox.exec();
        } else {
            statusBar()->showMessage(message, isAsioStartupFailure(errorCode) ? 18000 : 5000);
        }
        updatePlaybackState();
    });
    connect(m_player, &AudioPlayerBackend::statusMessage, this, [this](const QString &message) {
        // Detect ASIO retry/recovery messages and start update timer.
        static const QRegularExpression asioTimedStatusPattern(
            QStringLiteral("^(ASIO 设备被占用，正在重试|正在恢复 ASIO 播放).*（(\\d+)s/(\\d+)s）"));
        const auto match = asioTimedStatusPattern.match(message);
        if (match.hasMatch()) {
            const QString statusPrefix = match.captured(1);
            const int elapsedSec = match.captured(2).toInt();
            const int timeoutSec = match.captured(3).toInt();
            const bool statusChanged = !m_asioRetryActive
                || m_asioRetryTimeoutSec != timeoutSec
                || m_asioRetryStatusPrefix != statusPrefix;
            if (!m_asioRetryActive
                || m_asioRetryTimeoutSec != timeoutSec
                || m_asioRetryStatusPrefix != statusPrefix) {
                m_asioRetryActive = true;
                m_asioRetryTimeoutSec = timeoutSec;
                m_asioRetryStatusPrefix = statusPrefix;
                m_asioRetryTimeoutMessage = statusPrefix.contains(tr("恢复"))
                    ? tr("ASIO 播放恢复超时（%1 秒），请暂停或关闭其他音频应用后重试").arg(timeoutSec)
                    : tr("ASIO 设备仍被其他应用占用（已重试 %1 秒），请暂停或关闭其他音频应用后重试").arg(timeoutSec);
                m_asioRetryElapsedOffsetMs =
                    static_cast<qint64>(qBound(0, elapsedSec, timeoutSec)) * 1000;
                m_asioRetryElapsedTimer.restart();
                m_asioRetryLastShownSec = -1;
                m_asioRetryUpdateTimer->start();
            }
            if (statusChanged) {
                const int displayElapsedSec = qBound(0, elapsedSec, timeoutSec);
                m_asioRetryLastShownSec = displayElapsedSec;
                statusBar()->showMessage(
                    tr("%1（%2s/%3s）").arg(statusPrefix).arg(displayElapsedSec).arg(timeoutSec));
            }
            return;
        }

        // Detect ASIO device available message (transition from retry to starting)
        if (message.contains(tr("ASIO 设备已可用"))) {
            if (m_asioRetryActive) {
                m_asioRetryActive = false;
                m_asioRetryUpdateTimer->stop();
            }
            statusBar()->showMessage(message);
            return;
        }

        // Detect ASIO playback started message
        if (message.contains(tr("ASIO 播放已启动")) || message.contains(tr("ASIO 播放已恢复"))) {
            if (m_asioRetryActive) {
                m_asioRetryActive = false;
                m_asioRetryUpdateTimer->stop();
            }
            statusBar()->showMessage(message, 3000);
            return;
        }

        // Detect playback rebuild messages and start update timer
        if (message.contains(tr("正在重建播放管线"))) {
            if (!m_rebuildActive) {
                m_rebuildActive = true;
                m_rebuildStartTimeMs = QDateTime::currentMSecsSinceEpoch();
                m_rebuildLastShownSec = -1;
                m_rebuildStatusTimer->start();
            }
            statusBar()->showMessage(message);
            return;
        }

        if (message.contains(tr("播放管线已重建"))) {
            if (m_rebuildActive) {
                m_rebuildActive = false;
                m_rebuildStatusTimer->stop();
            }
            statusBar()->showMessage(message, 3000);
            return;
        }

        // Non-retry/non-rebuild message — stop timers if active
        if (m_asioRetryActive) {
            m_asioRetryActive = false;
            m_asioRetryUpdateTimer->stop();
        }
        if (m_rebuildActive) {
            m_rebuildActive = false;
            m_rebuildStatusTimer->stop();
        }
        statusBar()->showMessage(message, 18000);
    });

    if (m_diagnosticsService && m_diagnosticsAction && m_diagnosticsAction->isChecked()) {
        m_diagnosticsService->attachToPlayer(m_player);
    }
}

void MainWindow::replacePlayer(AudioPlayerBackend::BackendId backendId)
{
    if (m_player && m_player->backendId() == backendId) {
        m_mediaInfoDialog->setBackendName(m_player->backendName());
        m_mediaInfoDialog->setDecoderName(m_player->decoderName());
        rebuildOutputDeviceMenu();
        refreshOutputDeviceInfo();
        return;
    }

    const QString previousBackendName = m_player ? m_player->backendName() : QStringLiteral("none");
    PlayerLogger::log(QStringLiteral("ui"),
                      QStringLiteral("replacePlayer previous=%1 target=%2")
                          .arg(previousBackendName)
                          .arg(static_cast<int>(backendId)));

    delete m_player;
    m_player = AudioPlayerFactory::create(backendId, this);
    connectPlayerSignals();
    m_player->setVolume(ui->sliderVolume->value() / 100.0);
    m_player->setStabilityModeEnabled(false);
    m_player->setExclusiveModeEnabled(false);
    if (m_exactPlaybackAction) {
        const bool stored = m_exactPlaybackAction->property("storedChecked").toBool();
        m_exactPlaybackAction->setEnabled(true);
        {
            QSignalBlocker blocker(m_exactPlaybackAction);
            m_exactPlaybackAction->setChecked(stored);
        }
        m_player->setExactPlaybackEnabled(stored);
    }
    m_mediaInfoDialog->setBackendName(m_player->backendName());
    m_mediaInfoDialog->setDecoderName(m_player->decoderName());
    rebuildOutputDeviceMenu();
    refreshOutputDeviceInfo();
    updatePlaybackState();
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    PlayerLogger::log(QStringLiteral("ui"), QStringLiteral("closeEvent stopPlayback"));
    stopPlayback();
    QMainWindow::closeEvent(event);
}

bool MainWindow::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == ui->sliderProgress) {
        QSlider *s = ui->sliderProgress;
        if (event->type() == QEvent::MouseButtonPress) {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() == Qt::LeftButton && s->isEnabled()) {
                const QRect groove = s->contentsRect();
                const int pixel = me->position().toPoint().x();
                const double ratio = qBound(0.0,
                    (pixel - groove.left()) / static_cast<double>(groove.width()), 1.0);
                const int pos = s->minimum() + qRound(ratio * (s->maximum() - s->minimum()));
                s->setSliderDown(true);
                s->setValue(pos);
                emit s->sliderMoved(pos);
                return true;
            }
        } else if (event->type() == QEvent::MouseMove && s->isSliderDown()) {
            auto *me = static_cast<QMouseEvent *>(event);
            const QRect groove = s->contentsRect();
            const int pixel = me->position().toPoint().x();
            const double ratio = qBound(0.0,
                (pixel - groove.left()) / static_cast<double>(groove.width()), 1.0);
            const int pos = s->minimum() + qRound(ratio * (s->maximum() - s->minimum()));
            s->setValue(pos);
            emit s->sliderMoved(pos);
            return true;
        } else if (event->type() == QEvent::MouseButtonRelease) {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() == Qt::LeftButton && s->isSliderDown()) {
                s->setSliderDown(false);
                return true;
            }
        }
    }
    return QMainWindow::eventFilter(obj, event);
}

void MainWindow::loadFileFromPath(const QString &filePath)
{
    const QFileInfo fileInfo(filePath);
    if (!fileInfo.exists() || !fileInfo.isFile()) {
        PlayerLogger::log(QStringLiteral("ui"),
                          QStringLiteral("loadFileFromPath ignored invalid=%1").arg(filePath));
        statusBar()->showMessage(tr("文件不存在：%1").arg(filePath), 5000);
        return;
    }

    QSettings settings(kSettingsOrganization, kSettingsApplication);
    settings.setValue(kLastDirectoryKey, fileInfo.absolutePath());
    loadAudioFile(fileInfo.absoluteFilePath());
}

void MainWindow::togglePlayback()
{
    if (m_player->source().isEmpty()) {
        return;
    }

    PlayerLogger::log(QStringLiteral("ui"),
                      QStringLiteral("togglePlayback state=%1")
                          .arg(AudioUtils::playbackStateName(m_player->playbackState())));
    if (m_player->playbackState() == AudioPlayerBackend::PlaybackState::Playing) {
        m_player->pause();
        return;
    }

    m_player->play();
}

void MainWindow::stopPlayback()
{
    PlayerLogger::log(QStringLiteral("ui"),
                      QStringLiteral("stopPlayback state=%1 pendingSeek=%2")
                          .arg(AudioUtils::playbackStateName(m_player->playbackState()))
                          .arg(m_pendingSeekPosition));
    m_isSeeking = false;
    m_pendingSeekPosition = -1;
    m_resumeAfterSeek = false;
    m_player->stop();
}

void MainWindow::seekToSliderPosition()
{
    const auto state = m_player->playbackState();
    if (state == AudioPlayerBackend::PlaybackState::Stopping) {
        m_isSeeking = false;
        m_pendingSeekPosition = -1;
        m_resumeAfterSeek = false;
        return;
    }

    m_isSeeking = false;
    const qint64 target = ui->sliderProgress->value();
    m_pendingSeekPosition = target;
    PlayerLogger::log(QStringLiteral("ui"),
                      QStringLiteral("seekToSliderPosition target=%1 state=%2 resumeAfterSeek=%3")
                          .arg(target)
                          .arg(AudioUtils::playbackStateName(state))
                          .arg(m_resumeAfterSeek));

    m_player->seek(target);
    {
        const QSignalBlocker blocker(ui->sliderProgress);
        ui->sliderProgress->setValue(static_cast<int>(target));
    }
    ui->labelCurrentTime->setText(formatTime(target));

    if (m_resumeAfterSeek) {
        m_player->play();
    }
    m_resumeAfterSeek = false;
}

void MainWindow::handlePlaybackFinished()
{
    PlayerLogger::log(QStringLiteral("ui"), QStringLiteral("handlePlaybackFinished"));
    m_isSeeking = false;
    m_pendingSeekPosition = -1;
    m_resumeAfterSeek = false;
    m_lastAutomationPositionBucket = -1;
    statusBar()->showMessage(tr("播放完成"), 3000);
}

void MainWindow::updateDuration(qint64 duration)
{
    const qint64 effectiveDuration = duration > 0 ? duration : m_probedDuration;
    const QSignalBlocker blocker(ui->sliderProgress);
    ui->sliderProgress->setRange(0, static_cast<int>(effectiveDuration));
    ui->labelTotalTime->setText(formatTime(effectiveDuration));
}

void MainWindow::updatePosition(qint64 position)
{
    if (m_isSeeking) {
        ui->labelCurrentTime->setText(formatTime(ui->sliderProgress->value()));
        return;
    }

    if (m_pendingSeekPosition >= 0) {
        const qint64 pendingSeekTarget = m_pendingSeekPosition;
        if (m_player->playbackState() == AudioPlayerBackend::PlaybackState::Paused) {
            const QSignalBlocker blocker(ui->sliderProgress);
            ui->sliderProgress->setValue(static_cast<int>(pendingSeekTarget));
            ui->labelCurrentTime->setText(formatTime(pendingSeekTarget));
            return;
        }

        if (qAbs(position - pendingSeekTarget) > 1000) {
            const QSignalBlocker blocker(ui->sliderProgress);
            ui->sliderProgress->setValue(static_cast<int>(pendingSeekTarget));
            ui->labelCurrentTime->setText(formatTime(pendingSeekTarget));
            return;
        }

        logAutomationEvent(QStringLiteral("seekCompleted target=%1 actual=%2")
                               .arg(pendingSeekTarget)
                               .arg(position));
        m_pendingSeekPosition = -1;
    }

    if (!ui->sliderProgress->isSliderDown()) {
        const QSignalBlocker blocker(ui->sliderProgress);
        ui->sliderProgress->setValue(static_cast<int>(position));
    }

    const qint64 displayedPosition = ui->sliderProgress->isSliderDown()
        ? ui->sliderProgress->value()
        : position;
    ui->labelCurrentTime->setText(formatTime(displayedPosition));

    const qint64 currentBucket = position / 1000;
    if (currentBucket != m_lastAutomationPositionBucket) {
        m_lastAutomationPositionBucket = currentBucket;
        logAutomationEvent(QStringLiteral("positionMs=%1").arg(position));
    }
}

void MainWindow::updatePlaybackState()
{
    const bool hasSource = !m_player->source().isEmpty();
    const auto playbackState = m_player->playbackState();
    const bool isStopping = playbackState == AudioPlayerBackend::PlaybackState::Stopping;
    if (m_isLoadingFile) {
        ui->buttonPlayPause->setEnabled(false);
        ui->buttonStop->setEnabled(false);
        ui->sliderProgress->setEnabled(false);
        PlayerLogger::log(QStringLiteral("ui"),
                          QStringLiteral("updatePlaybackState loading=1 playEnabled=0 stopEnabled=0 seekEnabled=0"));
        ui->buttonPlayPause->setText(tr("加载中"));
        return;
    }

    const bool canSeek = hasSource && !isStopping;
    ui->buttonPlayPause->setEnabled(hasSource && !isStopping);
    ui->buttonStop->setEnabled(hasSource && !isStopping);
    ui->sliderProgress->setEnabled(canSeek);
    PlayerLogger::log(QStringLiteral("ui"),
                      QStringLiteral("updatePlaybackState state=%1 playEnabled=%2 stopEnabled=%3 seekEnabled=%4")
                          .arg(AudioUtils::playbackStateName(playbackState))
                          .arg(ui->buttonPlayPause->isEnabled())
                          .arg(ui->buttonStop->isEnabled())
                          .arg(ui->sliderProgress->isEnabled()));
    if (isStopping) {
        ui->buttonPlayPause->setText(tr("停止中"));
        return;
    }

    ui->buttonPlayPause->setText(playbackState == AudioPlayerBackend::PlaybackState::Playing ? tr("暂停")
                                                                                              : tr("播放"));
}

QString MainWindow::formatTime(qint64 milliseconds) const
{
    const qint64 totalSeconds = milliseconds / 1000;
    const qint64 hours = totalSeconds / 3600;
    const qint64 minutes = (totalSeconds % 3600) / 60;
    const qint64 seconds = totalSeconds % 60;

    if (hours > 0) {
        return tr("%1:%2:%3")
            .arg(hours, 2, 10, QChar(u'0'))
            .arg(minutes, 2, 10, QChar(u'0'))
            .arg(seconds, 2, 10, QChar(u'0'));
    }

    return tr("%1:%2")
        .arg(minutes, 2, 10, QChar(u'0'))
        .arg(seconds, 2, 10, QChar(u'0'));
}

void MainWindow::setLoadingState(bool loading, const QString &filePath)
{
    m_isLoadingFile = loading;
    if (loading) {
        m_loadingFilePath = filePath;
        const AudioPlayerSourceContext sourceContext { filePath, {}, 0 };
        const AudioPlaybackPlan previewPlan = AudioPlayerFactory::buildPlaybackPlan(sourceContext);
        m_loadPreparationEstimate = m_playbackSourceService.estimatePreparation(
            filePath,
            previewPlan.sourceMode);
        m_loadElapsedTimer.start();
        m_loadLastShownElapsedSec = -1;
        m_loadProgressFrame->setVisible(false);
        m_loadProgressBar->setRange(0, 0);
        m_loadProgressBar->setValue(0);
        m_loadProgressTitleLabel->setText(
            m_loadPreparationEstimate.usesSidecar
                ? (m_loadPreparationEstimate.cacheExists
                       ? tr("正在分析并复用本地播放缓存…")
                       : tr("正在为稳定 seek 建立本地播放缓存…"))
                : tr("正在分析并准备音频文件…"));
        m_loadProgressStatsLabel->setText(m_loadPreparationEstimate.sourceSizeBytes > 0
                                              ? tr("总大小 %1")
                                                    .arg(formatDataSize(
                                                        m_loadPreparationEstimate.sourceSizeBytes))
                                              : tr("正在获取文件信息…"));
        m_loadProgressUpdateTimer->stop();
        m_loadProgressShowTimer->start(5000);
        ui->labelNowPlaying->setText(tr("正在加载：%1").arg(QFileInfo(filePath).fileName()));
        ui->labelCurrentTime->setText(formatTime(0));
        ui->labelTotalTime->setText(formatTime(0));
        statusBar()->showMessage(tr("正在分析并准备：%1").arg(filePath));
    } else {
        m_loadProgressShowTimer->stop();
        m_loadProgressUpdateTimer->stop();
        m_loadProgressFrame->setVisible(false);
        m_loadElapsedTimer.invalidate();
        m_loadLastShownElapsedSec = -1;
        m_loadPreparationEstimate = {};
        m_loadingFilePath.clear();
    }
    updatePlaybackState();
}

void MainWindow::updateLoadingProgress()
{
    if (!m_isLoadingFile) {
        return;
    }

    const qint64 elapsedMs = m_loadElapsedTimer.isValid() ? m_loadElapsedTimer.elapsed() : 0;
    const int elapsedSec = static_cast<int>(elapsedMs / 1000);
    if (elapsedSec == m_loadLastShownElapsedSec) {
        return;
    }
    m_loadLastShownElapsedSec = elapsedSec;
    QString titleText = m_loadPreparationEstimate.usesSidecar
        ? (m_loadPreparationEstimate.cacheExists
               ? tr("正在分析并复用本地播放缓存…")
               : tr("正在为稳定 seek 建立本地播放缓存…"))
        : tr("正在分析并准备音频文件…");
    QStringList details;

    if (m_loadPreparationEstimate.sourceSizeBytes > 0) {
        details << tr("总大小 %1").arg(formatDataSize(m_loadPreparationEstimate.sourceSizeBytes));
    }
    details << tr("已用时 %1").arg(formatRemainingTime(elapsedMs));

    if (m_loadPreparationEstimate.usesSidecar && !m_loadPreparationEstimate.cacheExists) {
        const qint64 processedBytes = QFileInfo(m_loadPreparationEstimate.cachePath).size();
        const double bytesPerSecond = elapsedMs > 0
            ? (processedBytes * 1000.0) / elapsedMs
            : 0.0;
        const bool hasKnownTotal = m_loadPreparationEstimate.sourceSizeBytes > 0;

        if (hasKnownTotal) {
            const double progressRatio = qBound(
                0.0,
                processedBytes / static_cast<double>(m_loadPreparationEstimate.sourceSizeBytes),
                1.0);
            m_loadProgressBar->setRange(0, 1000);
            m_loadProgressBar->setValue(qRound(progressRatio * 1000.0));
            m_loadProgressBar->setFormat(tr("%1%").arg(qRound(progressRatio * 100.0)));
        } else {
            m_loadProgressBar->setRange(0, 0);
        }

        details << tr("已处理 %1").arg(formatDataSize(processedBytes));
        details << tr("速度 %1").arg(formatTransferRate(bytesPerSecond));
        if (hasKnownTotal && bytesPerSecond > 0.0
            && processedBytes < m_loadPreparationEstimate.sourceSizeBytes) {
            const qint64 remainingMs = qRound64(
                (m_loadPreparationEstimate.sourceSizeBytes - processedBytes) * 1000.0 / bytesPerSecond);
            details << tr("预计剩余 %1").arg(formatRemainingTime(remainingMs));
        } else {
            details << tr("预计剩余 --");
        }

        if (m_loadPreparationEstimate.timeoutMs > 0) {
            const qint64 timeoutRemainingMs = m_loadPreparationEstimate.timeoutMs - elapsedMs;
            details << tr("超时上限 %1").arg(formatRemainingTime(m_loadPreparationEstimate.timeoutMs));
            if (timeoutRemainingMs > 0) {
                details << tr("距超时 %1").arg(formatRemainingTime(timeoutRemainingMs));
            } else {
                titleText = tr("载入文件超时，正在回退为直接读取…");
            }
        }
    } else {
        m_loadProgressBar->setRange(0, 0);
    }

    m_loadProgressTitleLabel->setText(titleText);
    m_loadProgressStatsLabel->setText(details.join(tr(" | ")));
}
