#include "audioplayerbackend.h"
#include "audioplayerfactory.h"
#include "mainwindow.h"
#include "mainwindow_helpers.h"
#include "playerlogger.h"
#include "playbacksourceservice.h"
#include "ui_mainwindow.h"

#include <QDateTime>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QSettings>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QTimer>
#include <QtConcurrent>

#if defined(Q_OS_WINDOWS)
#include "windowsasioaudioplayer.h"
#endif

namespace {

using namespace MainWindowHelpers;

struct AudioFileLoadResult
{
    QString filePath;
    AudioInfo initialSourceInfo;
    AudioPlaybackPlan playbackPlan;
    PlaybackSourceResolution resolvedSource;
};

AudioFileLoadResult prepareAudioFileForPlayback(const QString &filePath)
{
    PlaybackSourceService playbackSourceService;
    AudioFileLoadResult result;
    result.filePath = filePath;
    result.initialSourceInfo = playbackSourceService.probeSourceInfo(filePath);
    const AudioPlayerSourceContext sourceContext {
        filePath,
        result.initialSourceInfo.codecName,
        result.initialSourceInfo.channelCount,
    };
    result.playbackPlan = AudioPlayerFactory::buildPlaybackPlan(sourceContext);
    result.resolvedSource = playbackSourceService.resolveForPlayback(
        filePath,
        result.initialSourceInfo,
        result.playbackPlan.sourceMode);
    return result;
}

} // namespace

QString MainWindow::formatBitRate(const QString &bitsPerSecond) const
{
    bool ok = false;
    const qint64 value = bitsPerSecond.toLongLong(&ok);
    if (!ok || value <= 0) {
        return {};
    }

    return tr("%1 kbps").arg(QString::number(value / 1000.0, 'f', value >= 100000 ? 0 : 1));
}

QString MainWindow::formatBitDepth(int bitDepth) const
{
    if (bitDepth <= 0) {
        return {};
    }

    return tr("%1-bit").arg(bitDepth);
}

QString MainWindow::formatChannelCount(int channelCount) const
{
    if (channelCount <= 0) {
        return {};
    }

    return tr("%1").arg(channelCount);
}

QString MainWindow::formatChannelDescription(int channelCount, const QString &layout) const
{
    if (channelCount <= 0) {
        return {};
    }

    if (layout.isEmpty()) {
        return formatChannelCount(channelCount);
    }

    return tr("%1 (%2)").arg(channelCount).arg(layout);
}

QString MainWindow::formatCodecDisplay(const QString &codecName, const QString &codecLongName) const
{
    const QString lowerName = codecName.toLower();
    if (lowerName == QStringLiteral("truehd")) {
        return tr("Dolby TrueHD");
    }
    if (lowerName == QStringLiteral("eac3")) {
        return tr("Dolby Digital Plus");
    }
    if (lowerName == QStringLiteral("ac3")) {
        return tr("Dolby Digital");
    }
    if (lowerName == QStringLiteral("aac")) {
        return tr("AAC");
    }
    if (lowerName == QStringLiteral("alac")) {
        return tr("ALAC");
    }
    if (lowerName == QStringLiteral("flac")) {
        return tr("FLAC");
    }
    if (lowerName == QStringLiteral("mp3")) {
        return tr("MP3");
    }
    if (lowerName.startsWith(QStringLiteral("pcm_"))) {
        return tr("PCM");
    }

    return !codecLongName.isEmpty() ? codecLongName : codecName;
}

QString MainWindow::formatOutputSampleFormat(QAudioFormat::SampleFormat sampleFormat, int bitDepth) const
{
    switch (sampleFormat) {
    case QAudioFormat::UInt8:
        return tr("PCM Unsigned Integer");
    case QAudioFormat::Int16:
        return tr("PCM Signed Integer");
    case QAudioFormat::Int32:
        return bitDepth > 0 && bitDepth < 32
            ? tr("PCM Signed Integer (%1-bit)").arg(bitDepth)
            : tr("PCM Signed Integer");
    case QAudioFormat::Float:
        return bitDepth > 0 ? tr("PCM Float") : tr("PCM");
    case QAudioFormat::Unknown:
    case QAudioFormat::NSampleFormats:
        break;
    }

    return tr("PCM");
}

QString MainWindow::formatSampleRate(int sampleRate) const
{
    if (sampleRate <= 0) {
        return {};
    }

    return tr("%1 Hz").arg(sampleRate);
}

void MainWindow::openAudioFile()
{
    QSettings settings(kSettingsOrganization, kSettingsApplication);
    const QString filePath = QFileDialog::getOpenFileName(
        this,
        tr("选择音频文件"),
        settings.value(kLastDirectoryKey).toString(),
        tr("音频文件 (*.mp3 *.wav *.flac *.m4a *.aac *.ac3 *.mlp *.eb3 *.ec3 *.mka);;所有文件 (*)"));

    if (filePath.isEmpty()) {
        return;
    }

    loadFileFromPath(filePath);
}

void MainWindow::loadAudioFile(const QString &filePath)
{
    PlayerLogger::log(QStringLiteral("ui"),
                      QStringLiteral("loadAudioFile path=%1").arg(filePath));
    const int requestId = ++m_loadRequestId;
    m_lastAutomationPositionBucket = -1;
    m_lastAutomationAudioLevelLogMs = -1;
    m_isSeeking = false;
    m_pendingSeekPosition = -1;
    m_resumeAfterSeek = false;
    if (m_player) {
        m_player->stop();
    }
    resetMediaInfo();
    m_probedDuration = 0;
    updateDuration(0);
    setLoadingState(true, filePath);

    auto *watcher = new QFutureWatcher<AudioFileLoadResult>(this);
    connect(watcher, &QFutureWatcher<AudioFileLoadResult>::finished, this, [this, watcher, requestId] {
        const AudioFileLoadResult result = watcher->result();
        watcher->deleteLater();

        if (requestId != m_loadRequestId) {
            PlayerLogger::log(QStringLiteral("ui"),
                              QStringLiteral("loadAudioFile stale-result request=%1 current=%2 path=%3")
                                  .arg(requestId)
                                  .arg(m_loadRequestId)
                                  .arg(result.filePath));
            return;
        }

        PlayerLogger::log(QStringLiteral("ui"),
                          QStringLiteral("loadAudioFile selectedBackend=%1 sourceMode=%2 codec=%3 channels=%4")
                              .arg(static_cast<int>(result.playbackPlan.backendId))
                              .arg(static_cast<int>(result.playbackPlan.sourceMode))
                              .arg(result.initialSourceInfo.codecName)
                              .arg(result.initialSourceInfo.channelCount));
        AudioPlayerBackend::BackendId targetBackendId = result.playbackPlan.backendId;
#if defined(Q_OS_WINDOWS)
        if (m_player
            && m_player->backendId() == AudioPlayerBackend::BackendId::WindowsAsio
            && WindowsAsioAudioPlayer::hasAvailableAsioOutputDevices()) {
            targetBackendId = AudioPlayerBackend::BackendId::WindowsAsio;
        }
#endif
        replacePlayer(targetBackendId);
        m_sourceInfo = result.resolvedSource.sourceInfo;
        refreshOutputDeviceInfo();
        m_mediaInfoDialog->setOutputInfo(m_outputInfo);
        m_mediaInfoDialog->setSourceInfo(m_sourceInfo);
        m_probedDuration = result.resolvedSource.durationMs;
        m_player->setSource(result.resolvedSource.playbackPath,
                            m_sourceInfo.channelCount,
                            m_sourceInfo.sampleRateValue,
                            m_sourceInfo.bitDepthValue,
                            m_sourceInfo.codecName);
        m_mediaInfoDialog->setDecoderName(m_player->decoderName());
        ui->labelNowPlaying->setText(tr("当前文件：%1").arg(QFileInfo(result.filePath).fileName()));
        if (!result.resolvedSource.warningMessage.isEmpty()) {
            statusBar()->showMessage(result.resolvedSource.warningMessage, 5000);
        } else if (!result.resolvedSource.statusMessage.isEmpty()) {
            statusBar()->showMessage(result.resolvedSource.statusMessage, 3000);
        } else {
            statusBar()->showMessage(tr("已加载：%1").arg(result.filePath), 3000);
        }
        updateDuration(m_probedDuration);
        logAutomationEvent(QStringLiteral("load backend=%1 source=%2 playbackSource=%3 durationMs=%4")
                               .arg(compactLogValue(m_player->backendName()))
                               .arg(compactLogValue(result.filePath))
                               .arg(compactLogValue(result.resolvedSource.playbackPath))
                               .arg(m_probedDuration));
        setLoadingState(false);
        m_player->play();
    });
    watcher->setFuture(QtConcurrent::run(&prepareAudioFileForPlayback, filePath));
}

void MainWindow::openMediaInfoDialog()
{
    PlayerLogger::log(QStringLiteral("ui"), QStringLiteral("openMediaInfoDialog"));
    if (m_player) {
        m_mediaInfoDialog->setBackendName(m_player->backendName());
        m_mediaInfoDialog->setDecoderName(m_player->decoderName());
    }
    m_mediaInfoDialog->show();
    m_mediaInfoDialog->raise();
    m_mediaInfoDialog->activateWindow();
}

void MainWindow::resetMediaInfo()
{
    m_outputInfo = {};
    m_sourceInfo = {};
    m_mediaInfoDialog->setDecoderName({});
    m_mediaInfoDialog->setOutputInfo(m_outputInfo);
    m_mediaInfoDialog->setSourceInfo(m_sourceInfo);
}
