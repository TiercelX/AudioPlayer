#include "audioplayerbackend.h"
#include "audioutils.h"
#include "mainwindow.h"
#include "mainwindow_helpers.h"
#include "playerlogger.h"
#include "ui_mainwindow.h"

#include <QDateTime>
#include <QSignalBlocker>
#include <QTimer>

namespace {

using namespace MainWindowHelpers;

} // namespace

bool MainWindow::isPlaybackAutomationReady() const
{
    if (!m_player || m_isLoadingFile || m_player->source().isEmpty()) {
        return false;
    }

    const auto state = m_player->playbackState();
    return state == AudioPlayerBackend::PlaybackState::Playing
        || state == AudioPlayerBackend::PlaybackState::Paused;
}

void MainWindow::pausePlayback()
{
    if (!m_player || m_player->playbackState() != AudioPlayerBackend::PlaybackState::Playing) {
        PlayerLogger::log(QStringLiteral("ui"),
                          QStringLiteral("pausePlayback ignored state=%1")
                              .arg(AudioUtils::playbackStateName(m_player ? m_player->playbackState()
                                                              : AudioPlayerBackend::PlaybackState::Stopped)));
        return;
    }

    PlayerLogger::log(QStringLiteral("ui"), QStringLiteral("pausePlayback scripted"));
    logAutomationEvent(QStringLiteral("action=pause"));
    m_player->pause();
}

void MainWindow::resumePlayback()
{
    if (!m_player || m_player->source().isEmpty()) {
        PlayerLogger::log(QStringLiteral("ui"), QStringLiteral("resumePlayback ignored no-source"));
        return;
    }

    const auto state = m_player->playbackState();
    if (state != AudioPlayerBackend::PlaybackState::Paused
        && state != AudioPlayerBackend::PlaybackState::Stopped) {
        PlayerLogger::log(QStringLiteral("ui"),
                          QStringLiteral("resumePlayback ignored state=%1")
                              .arg(AudioUtils::playbackStateName(state)));
        return;
    }

    PlayerLogger::log(QStringLiteral("ui"),
                      QStringLiteral("resumePlayback scripted state=%1")
                          .arg(AudioUtils::playbackStateName(state)));
    logAutomationEvent(QStringLiteral("action=resume"));
    m_player->play();
}

void MainWindow::seekPlaybackTo(qint64 positionMs)
{
    if (!m_player || m_player->source().isEmpty()) {
        PlayerLogger::log(QStringLiteral("ui"),
                          QStringLiteral("seekPlaybackTo ignored no-source target=%1").arg(positionMs));
        return;
    }

    const qint64 clampedPosition = qBound<qint64>(0, positionMs, m_probedDuration > 0 ? m_probedDuration : positionMs);
    PlayerLogger::log(QStringLiteral("ui"),
                      QStringLiteral("seekPlaybackTo target=%1 state=%2")
                          .arg(clampedPosition)
                          .arg(AudioUtils::playbackStateName(m_player->playbackState())));
    logAutomationEvent(QStringLiteral("action=seek target=%1").arg(clampedPosition));
    m_isSeeking = false;
    m_pendingSeekPosition = clampedPosition;
    m_resumeAfterSeek = m_player->playbackState() == AudioPlayerBackend::PlaybackState::Playing;
    m_player->seek(clampedPosition);
    {
        const QSignalBlocker blocker(ui->sliderProgress);
        ui->sliderProgress->setValue(static_cast<int>(clampedPosition));
    }
    ui->labelCurrentTime->setText(formatTime(clampedPosition));
}

void MainWindow::seekPlaybackToWithPauseResume(qint64 positionMs)
{
    if (!m_player || m_player->source().isEmpty()) {
        PlayerLogger::log(QStringLiteral("ui"),
                          QStringLiteral("seekPlaybackToWithPauseResume ignored no-source target=%1")
                              .arg(positionMs));
        return;
    }

    const qint64 clampedPosition = qBound<qint64>(0, positionMs, m_probedDuration > 0 ? m_probedDuration : positionMs);
    const auto initialState = m_player->playbackState();
    const bool resumeAfterSeek = initialState == AudioPlayerBackend::PlaybackState::Playing;
    PlayerLogger::log(QStringLiteral("ui"),
                      QStringLiteral("seekPlaybackToWithPauseResume target=%1 state=%2 resumeAfterSeek=%3")
                          .arg(clampedPosition)
                          .arg(AudioUtils::playbackStateName(initialState))
                          .arg(resumeAfterSeek));
    logAutomationEvent(QStringLiteral("action=seek-pause-resume target=%1").arg(clampedPosition));

    m_isSeeking = false;
    m_pendingSeekPosition = clampedPosition;
    m_resumeAfterSeek = resumeAfterSeek;
    if (resumeAfterSeek) {
        m_player->pause();
    }
    m_player->seek(clampedPosition);
    {
        const QSignalBlocker blocker(ui->sliderProgress);
        ui->sliderProgress->setValue(static_cast<int>(clampedPosition));
    }
    ui->labelCurrentTime->setText(formatTime(clampedPosition));
    if (resumeAfterSeek) {
        m_player->play();
    }
    m_resumeAfterSeek = false;
}

void MainWindow::stopPlaybackNow()
{
    PlayerLogger::log(QStringLiteral("ui"), QStringLiteral("stopPlaybackNow scripted"));
    logAutomationEvent(QStringLiteral("action=stop"));
    stopPlayback();
}

void MainWindow::refreshPlaybackOutput()
{
    if (!m_player || !isPlaybackAutomationReady()) {
        PlayerLogger::log(QStringLiteral("ui"),
                          QStringLiteral("refreshPlaybackOutput ignored ready=%1 hasPlayer=%2")
                              .arg(isPlaybackAutomationReady())
                              .arg(m_player != nullptr));
        return;
    }

    PlayerLogger::log(QStringLiteral("ui"),
                      QStringLiteral("refreshPlaybackOutput scripted state=%1")
                          .arg(AudioUtils::playbackStateName(m_player->playbackState())));
    logAutomationEvent(QStringLiteral("action=refresh-output"));
    m_player->refreshOutputConfiguration(true);
}

void MainWindow::logAutomationEvent(const QString &message) const
{
    PlayerLogger::log(QStringLiteral("automation"), compactLogValue(message));
}
