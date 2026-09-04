#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "audioplayerbackend.h"
#include "audioplayerfactory.h"
#include "mediainfodialog.h"
#include "playbacksourceservice.h"

#include <QElapsedTimer>
#include <QMainWindow>
#include <QString>
#include <QtMultimedia/QAudioFormat>

class QAction;
class QActionGroup;
class DiagnosticsDashboard;
class DiagnosticsService;
class QFrame;
class QCloseEvent;
class QLabel;
class QMenu;
class QProgressBar;
class QTimer;

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    void loadFileFromPath(const QString &filePath);
    bool isPlaybackAutomationReady() const;
    void pausePlayback();
    void resumePlayback();
    void seekPlaybackTo(qint64 positionMs);
    void seekPlaybackToWithPauseResume(qint64 positionMs);
    void stopPlaybackNow();
    void refreshPlaybackOutput();
    void listOutputDevicesForAutomation() const;
    bool selectOutputDeviceByIndex(int index);
    bool selectAsioOutputDeviceByIndex(int index);
    void switchToWasapiMode(bool exclusiveMode, bool stabilityMode);

private:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *obj, QEvent *event) override;
    void connectPlayerSignals();
    QString formatBitRate(const QString &bitsPerSecond) const;
    QString formatBitDepth(int bitDepth) const;
    QString formatChannelCount(int channelCount) const;
    QString formatChannelDescription(int channelCount, const QString &layout) const;
    QString formatCodecDisplay(const QString &codecName, const QString &codecLongName) const;
    QString formatOutputSampleFormat(QAudioFormat::SampleFormat sampleFormat, int bitDepth) const;
    QString formatSampleRate(int sampleRate) const;
    void rebuildOutputDeviceMenu();
    void refreshOutputDeviceInfo();
    void selectOutputDeviceAction(QAction *action);
    void switchOutputBackendAndDevice(AudioPlayerBackend::BackendId backendId,
                                      const QByteArray &deviceId,
                                      bool applyWasapiMode = false,
                                      bool exclusiveMode = false,
                                      bool stabilityMode = false);
    void setCurrentWasapiMode(bool exclusiveMode, bool stabilityMode);
    void loadAudioFile(const QString &filePath);
    void openCacheSettingsDialog();
    void openMediaInfoDialog();
    void openAudioFile();
    void applyCreativeReorderMode(QAction *action);
    void replacePlayer(AudioPlayerBackend::BackendId backendId);
    void resetMediaInfo();
    void setLoadingState(bool loading, const QString &filePath = QString());
    void updateLoadingProgress();
    void updateOutputInfoFromFormat(const QString &deviceDescription, const QAudioFormat &format);
    void handlePlaybackFinished();
    void logAutomationEvent(const QString &message) const;
    void togglePlayback();
    void stopPlayback();
    void seekToSliderPosition();
    void updateDuration(qint64 duration);
    void updatePosition(qint64 position);
    void updatePlaybackState();
    QString formatTime(qint64 milliseconds) const;

    Ui::MainWindow *ui;
    AudioPlayerBackend *m_player = nullptr;
    MediaInfoDialog *m_mediaInfoDialog;
    PlaybackSourceService m_playbackSourceService;
    QMenu *m_outputDeviceMenu = nullptr;
    QActionGroup *m_outputDeviceActionGroup = nullptr;
    QAction *m_exclusiveModeAction = nullptr;
    QAction *m_stabilityModeAction = nullptr;
    QAction *m_exactPlaybackAction = nullptr;
    QActionGroup *m_creativeReorderActionGroup = nullptr;
    QAction *m_creativeReorderAutoAction = nullptr;
    QAction *m_creativeReorderOffAction = nullptr;
    QAction *m_creativeReorderForceAction = nullptr;
    QFrame *m_loadProgressFrame = nullptr;
    QLabel *m_loadProgressTitleLabel = nullptr;
    QLabel *m_loadProgressStatsLabel = nullptr;
    QProgressBar *m_loadProgressBar = nullptr;
    QTimer *m_loadProgressShowTimer = nullptr;
    QTimer *m_loadProgressUpdateTimer = nullptr;
    AudioInfo m_outputInfo;
    AudioInfo m_sourceInfo;
    PlaybackPreparationEstimate m_loadPreparationEstimate;
    QString m_loadingFilePath;
    qint64 m_probedDuration;
    qint64 m_lastAutomationPositionBucket = -1;
    qint64 m_lastAutomationAudioLevelLogMs = -1;
    qint64 m_pendingSeekPosition = -1;
    int m_loadRequestId = 0;
    QElapsedTimer m_loadElapsedTimer;
    bool m_isSeeking = false;
    bool m_isLoadingFile = false;
    bool m_resumeAfterSeek = false;
    QByteArray m_lastSelectedWasapiDeviceId;

    // ASIO retry status bar tracking
    QTimer *m_asioRetryUpdateTimer = nullptr;
    QElapsedTimer m_asioRetryElapsedTimer;
    qint64 m_asioRetryElapsedOffsetMs = 0;
    int m_asioRetryTimeoutSec = 0;
    int m_asioRetryLastShownSec = -1;
    QString m_asioRetryStatusPrefix;
    QString m_asioRetryTimeoutMessage;
    bool m_asioRetryActive = false;

    // Playback rebuild status bar tracking
    QTimer *m_rebuildStatusTimer = nullptr;
    qint64 m_rebuildStartTimeMs = 0;
    int m_rebuildLastShownSec = -1;
    bool m_rebuildActive = false;

    // Load progress display tracking
    int m_loadLastShownElapsedSec = -1;

    // Performance diagnostics
    DiagnosticsService *m_diagnosticsService = nullptr;
    DiagnosticsDashboard *m_diagnosticsDashboard = nullptr;
    QAction *m_diagnosticsAction = nullptr;
};
#endif // MAINWINDOW_H
