#include "automationoptions.h"

#include "diagnosticreportbuilder.h"
#include "mainwindow.h"
#include "playerlogger.h"
#if defined(Q_OS_WINDOWS)
#include "windowsasioaudioplayer.h"
#endif

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QPointer>
#include <QTimer>

#include <memory>

std::optional<int> runAsioProbeIfRequested(const QCommandLineParser &parser)
{
#if defined(Q_OS_WINDOWS)
    const QString probeDriver = parser.value(QStringLiteral("asio-probe-driver"));
    if (probeDriver.isEmpty()) {
        return std::nullopt;
    }

    bool hostWindowOk = false;
    const quintptr hostWindowValue =
        static_cast<quintptr>(parser.value(QStringLiteral("asio-probe-host-window")).toULongLong(&hostWindowOk, 0));
    bool hostIndexOk = false;
    const int hostIndex = parser.value(QStringLiteral("asio-probe-host-index")).toInt(&hostIndexOk);
    const bool probeOk =
        WindowsAsioAudioPlayer::runDriverInitProbe(probeDriver,
                                                   hostWindowOk ? hostWindowValue : 0,
                                                   parser.value(QStringLiteral("asio-probe-host-kind")),
                                                   hostIndexOk ? hostIndex : -1);
    return probeOk ? 0 : 2;
#else
    return std::nullopt;
#endif
}

void registerAutomationOptions(QCommandLineParser &parser)
{
    parser.addOption(QCommandLineOption(QStringLiteral("quit-after-ms"),
                                       QStringLiteral("Quit automatically after the specified number of milliseconds."),
                                       QStringLiteral("milliseconds")));
    parser.addOption(QCommandLineOption(QStringLiteral("source"),
                                       QStringLiteral("Load the specified audio file on startup."),
                                       QStringLiteral("file")));
    parser.addOption(QCommandLineOption(QStringLiteral("list-output-devices"),
                                       QStringLiteral("Log available output devices for automation.")));
    parser.addOption(QCommandLineOption(QStringLiteral("output-device-index"),
                                       QStringLiteral("Select an output device by zero-based index before playback; use -1 for default."),
                                       QStringLiteral("index")));
    parser.addOption(QCommandLineOption(QStringLiteral("asio-output-index"),
                                       QStringLiteral("Select an ASIO output driver by zero-based ASIO index before playback."),
                                       QStringLiteral("index")));
    parser.addOption(QCommandLineOption(QStringLiteral("exclusive-mode"),
                                       QStringLiteral("Enable WASAPI exclusive mode before playback.")));
    parser.addOption(QCommandLineOption(QStringLiteral("stability-mode"),
                                       QStringLiteral("Enable WASAPI stability mode before playback.")));
#if defined(Q_OS_WINDOWS)
    parser.addOption(QCommandLineOption(QStringLiteral("asio-probe-driver"),
                                       QStringLiteral("Probe an ASIO driver init and exit."),
                                       QStringLiteral("clsid")));
    parser.addOption(QCommandLineOption(QStringLiteral("asio-probe-host-window"),
                                       QStringLiteral("HWND value to pass to the ASIO init probe."),
                                       QStringLiteral("hwnd")));
    parser.addOption(QCommandLineOption(QStringLiteral("asio-probe-host-kind"),
                                       QStringLiteral("Host kind label for the ASIO init probe."),
                                       QStringLiteral("kind")));
    parser.addOption(QCommandLineOption(QStringLiteral("asio-probe-host-index"),
                                       QStringLiteral("Host index label for the ASIO init probe."),
                                       QStringLiteral("index")));
#endif
    parser.addOption(QCommandLineOption(QStringLiteral("switch-output-after-ms"),
                                       QStringLiteral("Switch output device after playback becomes ready."),
                                       QStringLiteral("milliseconds")));
    parser.addOption(QCommandLineOption(QStringLiteral("switch-output-to-index"),
                                       QStringLiteral("Target output device index for scripted switching; use -1 for default."),
                                       QStringLiteral("index")));
    parser.addOption(QCommandLineOption(QStringLiteral("repeat-output-switch"),
                                       QStringLiteral("Repeat scripted output switching the specified number of times."),
                                       QStringLiteral("count")));
    parser.addOption(QCommandLineOption(QStringLiteral("switch-interval-ms"),
                                       QStringLiteral("Interval between repeated scripted output switches."),
                                       QStringLiteral("milliseconds")));
    parser.addOption(QCommandLineOption(QStringLiteral("report-file"),
                                       QStringLiteral("Write a JSON diagnostic smoke-test report on exit."),
                                       QStringLiteral("file")));
    parser.addOption(QCommandLineOption(QStringLiteral("seek-after-ms"),
                                       QStringLiteral("Perform a scripted seek after the specified number of milliseconds."),
                                       QStringLiteral("milliseconds")));
    parser.addOption(QCommandLineOption(QStringLiteral("seek-to-ms"),
                                       QStringLiteral("Seek to the specified playback position in milliseconds."),
                                       QStringLiteral("milliseconds")));
    parser.addOption(QCommandLineOption(QStringLiteral("second-seek-after-ms"),
                                       QStringLiteral("Perform a second scripted seek after the specified number of milliseconds."),
                                       QStringLiteral("milliseconds")));
    parser.addOption(QCommandLineOption(QStringLiteral("second-seek-to-ms"),
                                       QStringLiteral("Seek to the second specified playback position in milliseconds."),
                                       QStringLiteral("milliseconds")));
    parser.addOption(QCommandLineOption(QStringLiteral("seek-pause-resume"),
                                       QStringLiteral("Route scripted seeks through pause, seek, and resume to match slider-release behavior.")));
    parser.addOption(QCommandLineOption(QStringLiteral("seek-sequence"),
                                       QStringLiteral("Comma-separated scripted seek list using afterMs:targetMs entries."),
                                       QStringLiteral("sequence")));
    parser.addOption(QCommandLineOption(QStringLiteral("pause-after-ms"),
                                       QStringLiteral("Pause playback after the specified number of milliseconds."),
                                       QStringLiteral("milliseconds")));
    parser.addOption(QCommandLineOption(QStringLiteral("resume-after-ms"),
                                       QStringLiteral("Resume playback after the specified number of milliseconds."),
                                       QStringLiteral("milliseconds")));
    parser.addOption(QCommandLineOption(QStringLiteral("stop-after-ms"),
                                       QStringLiteral("Stop playback after the specified number of milliseconds."),
                                       QStringLiteral("milliseconds")));
    parser.addOption(QCommandLineOption(QStringLiteral("refresh-output-after-ms"),
                                       QStringLiteral("Force scripted output reconfiguration after the specified number of milliseconds."),
                                       QStringLiteral("milliseconds")));
    parser.addOption(QCommandLineOption(QStringLiteral("refresh-output-count"),
                                       QStringLiteral("Repeat scripted output reconfiguration the specified number of times."),
                                       QStringLiteral("count")));
    parser.addOption(QCommandLineOption(QStringLiteral("refresh-output-interval-ms"),
                                       QStringLiteral("Interval between repeated scripted output reconfigurations."),
                                       QStringLiteral("milliseconds")));
    parser.addOption(QCommandLineOption(QStringLiteral("switch-source-after-ms"),
                                       QStringLiteral("Load another source after the specified number of milliseconds."),
                                       QStringLiteral("milliseconds")));
    parser.addOption(QCommandLineOption(QStringLiteral("switch-source"),
                                       QStringLiteral("Load the specified source during scripted automation."),
                                       QStringLiteral("file")));
}

void setupAutomationFromCli(const QCommandLineParser &parser, MainWindow &w)
{
    if (parser.isSet(QStringLiteral("list-output-devices"))) {
        QTimer::singleShot(0, &w, [&w] {
            w.listOutputDevicesForAutomation();
        });
    }

    bool outputDeviceIndexOk = false;
    const int outputDeviceIndex = parser.value(QStringLiteral("output-device-index")).toInt(&outputDeviceIndexOk);
    if (outputDeviceIndexOk) {
        QTimer::singleShot(0, &w, [outputDeviceIndex, &w] {
            w.selectOutputDeviceByIndex(outputDeviceIndex);
        });
    }

    bool asioOutputIndexOk = false;
    const int asioOutputIndex = parser.value(QStringLiteral("asio-output-index")).toInt(&asioOutputIndexOk);
    if (asioOutputIndexOk) {
        QTimer::singleShot(0, &w, [asioOutputIndex, &w] {
            w.selectAsioOutputDeviceByIndex(asioOutputIndex);
        });
    }

    if (parser.isSet(QStringLiteral("exclusive-mode")) || parser.isSet(QStringLiteral("stability-mode"))) {
        const bool exclusive = parser.isSet(QStringLiteral("exclusive-mode"));
        const bool stability = parser.isSet(QStringLiteral("stability-mode"));
        QTimer::singleShot(0, &w, [exclusive, stability, &w] {
            w.switchToWasapiMode(exclusive, stability);
        });
    }

    QString startupSource = parser.value(QStringLiteral("source"));
    if (startupSource.isEmpty()) {
        const QStringList positionalArguments = parser.positionalArguments();
        if (!positionalArguments.isEmpty()) {
            startupSource = positionalArguments.constFirst();
        }
    }

    if (!startupSource.isEmpty()) {
        QTimer::singleShot(0, &w, [startupSource, &w] {
            w.loadFileFromPath(startupSource);
        });
    }

    bool switchOutputAfterOk = false;
    const int switchOutputAfterMs = parser.value(QStringLiteral("switch-output-after-ms")).toInt(&switchOutputAfterOk);
    bool switchOutputToIndexOk = false;
    const int switchOutputToIndex = parser.value(QStringLiteral("switch-output-to-index")).toInt(&switchOutputToIndexOk);
    const bool outputSwitchRequestedByCli = switchOutputAfterOk && switchOutputAfterMs >= 0 && switchOutputToIndexOk;
    const QString reportPath = parser.value(QStringLiteral("report-file"));
    QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, QCoreApplication::instance(),
                     [reportPath, outputSwitchRequestedByCli] {
                         PlayerLogger::log(QStringLiteral("automation"),
                                           QStringLiteral("aboutToQuit begin reportPath=%1")
                                               .arg(reportPath));
                         writeDiagnosticReport(reportPath, outputSwitchRequestedByCli);
                         PlayerLogger::log(QStringLiteral("automation"), QStringLiteral("aboutToQuit end"));
                     });

    auto scheduleAfterPlaybackReady = [&w](int delayMs, auto action) {
        if (delayMs < 0) {
            return;
        }

        auto *pollTimer = new QTimer(&w);
        pollTimer->setInterval(50);
        QPointer<QTimer> guard(pollTimer);
        QObject::connect(pollTimer, &QTimer::timeout, &w, [delayMs, action, &w, guard] {
            if (!guard || !w.isPlaybackAutomationReady()) {
                return;
            }

            guard->stop();
            guard->deleteLater();
            QTimer::singleShot(delayMs, &w, action);
        });
        pollTimer->start();
    };

    const bool seekPauseResume = parser.isSet(QStringLiteral("seek-pause-resume"));
    auto scheduleSeekAfterPlaybackReady = [&scheduleAfterPlaybackReady, seekPauseResume, &w](int delayMs,
                                                                                             qint64 targetMs) {
        scheduleAfterPlaybackReady(delayMs, [seekPauseResume, targetMs, &w] {
            if (seekPauseResume) {
                w.seekPlaybackToWithPauseResume(targetMs);
            } else {
                w.seekPlaybackTo(targetMs);
            }
        });
    };

    bool seekAfterOk = false;
    const int seekAfterMs = parser.value(QStringLiteral("seek-after-ms")).toInt(&seekAfterOk);
    bool seekToOk = false;
    const qint64 seekToMs = parser.value(QStringLiteral("seek-to-ms")).toLongLong(&seekToOk);
    if (seekAfterOk && seekAfterMs >= 0 && seekToOk && seekToMs >= 0) {
        scheduleSeekAfterPlaybackReady(seekAfterMs, seekToMs);
    }

    bool secondSeekAfterOk = false;
    const int secondSeekAfterMs = parser.value(QStringLiteral("second-seek-after-ms")).toInt(&secondSeekAfterOk);
    bool secondSeekToOk = false;
    const qint64 secondSeekToMs = parser.value(QStringLiteral("second-seek-to-ms")).toLongLong(&secondSeekToOk);
    if (secondSeekAfterOk && secondSeekAfterMs >= 0 && secondSeekToOk && secondSeekToMs >= 0) {
        scheduleSeekAfterPlaybackReady(secondSeekAfterMs, secondSeekToMs);
    }

    const QString seekSequence = parser.value(QStringLiteral("seek-sequence")).trimmed();
    if (!seekSequence.isEmpty()) {
        const QStringList entries = seekSequence.split(',', Qt::SkipEmptyParts);
        for (const QString &rawEntry : entries) {
            const QString entry = rawEntry.trimmed();
            const qsizetype separator = entry.indexOf(':');
            if (separator <= 0 || separator >= entry.size() - 1) {
                PlayerLogger::log(QStringLiteral("automation"),
                                  QStringLiteral("seek-sequence invalid entry=%1").arg(entry));
                continue;
            }

            bool entryAfterOk = false;
            bool entryTargetOk = false;
            const int entryAfterMs = entry.left(separator).toInt(&entryAfterOk);
            const qint64 entryTargetMs = entry.mid(separator + 1).toLongLong(&entryTargetOk);
            if (!entryAfterOk || entryAfterMs < 0 || !entryTargetOk || entryTargetMs < 0) {
                PlayerLogger::log(QStringLiteral("automation"),
                                  QStringLiteral("seek-sequence invalid entry=%1").arg(entry));
                continue;
            }

            scheduleSeekAfterPlaybackReady(entryAfterMs, entryTargetMs);
        }
    }

    bool pauseAfterOk = false;
    const int pauseAfterMs = parser.value(QStringLiteral("pause-after-ms")).toInt(&pauseAfterOk);
    if (pauseAfterOk && pauseAfterMs >= 0) {
        scheduleAfterPlaybackReady(pauseAfterMs, [&w] {
            w.pausePlayback();
        });
    }

    bool resumeAfterOk = false;
    const int resumeAfterMs = parser.value(QStringLiteral("resume-after-ms")).toInt(&resumeAfterOk);
    if (resumeAfterOk && resumeAfterMs >= 0) {
        scheduleAfterPlaybackReady(resumeAfterMs, [&w] {
            w.resumePlayback();
        });
    }

    bool stopAfterOk = false;
    const int stopAfterMs = parser.value(QStringLiteral("stop-after-ms")).toInt(&stopAfterOk);
    if (stopAfterOk && stopAfterMs >= 0) {
        scheduleAfterPlaybackReady(stopAfterMs, [&w] {
            w.stopPlaybackNow();
        });
    }

    bool refreshOutputAfterOk = false;
    const int refreshOutputAfterMs = parser.value(QStringLiteral("refresh-output-after-ms")).toInt(&refreshOutputAfterOk);
    if (refreshOutputAfterOk && refreshOutputAfterMs >= 0) {
        bool refreshOutputCountOk = false;
        const int parsedRefreshOutputCount =
            parser.value(QStringLiteral("refresh-output-count")).toInt(&refreshOutputCountOk);
        const int refreshOutputCount = refreshOutputCountOk ? qMax(1, parsedRefreshOutputCount) : 1;
        bool refreshOutputIntervalOk = false;
        const int parsedRefreshOutputIntervalMs =
            parser.value(QStringLiteral("refresh-output-interval-ms")).toInt(&refreshOutputIntervalOk);
        const int refreshOutputIntervalMs =
            refreshOutputIntervalOk ? qMax(0, parsedRefreshOutputIntervalMs) : 0;

        scheduleAfterPlaybackReady(refreshOutputAfterMs, [refreshOutputCount, refreshOutputIntervalMs, &w] {
            const auto firedCount = std::make_shared<int>(0);
            auto *refreshTimer = new QTimer(&w);
            refreshTimer->setInterval(qMax(1, refreshOutputIntervalMs));
            QPointer<QTimer> guard(refreshTimer);
            const auto runRefresh = [firedCount, refreshOutputCount, &w, guard] {
                if (!guard || *firedCount >= refreshOutputCount) {
                    return;
                }

                ++(*firedCount);
                w.refreshPlaybackOutput();
                if (*firedCount >= refreshOutputCount && guard) {
                    guard->stop();
                    guard->deleteLater();
                }
            };

            QObject::connect(refreshTimer, &QTimer::timeout, &w, runRefresh);
            runRefresh();
            if (refreshOutputCount > 1 && guard) {
                refreshTimer->start();
            }
        });
    }

    if (outputSwitchRequestedByCli) {
        bool repeatOutputSwitchOk = false;
        const int parsedRepeatOutputSwitch =
            parser.value(QStringLiteral("repeat-output-switch")).toInt(&repeatOutputSwitchOk);
        const int repeatOutputSwitch = repeatOutputSwitchOk ? qMax(1, parsedRepeatOutputSwitch) : 1;
        bool switchIntervalOk = false;
        const int parsedSwitchIntervalMs =
            parser.value(QStringLiteral("switch-interval-ms")).toInt(&switchIntervalOk);
        const int switchIntervalMs = switchIntervalOk ? qMax(1, parsedSwitchIntervalMs) : 1;

        scheduleAfterPlaybackReady(switchOutputAfterMs, [repeatOutputSwitch,
                                                         switchIntervalMs,
                                                         switchOutputToIndex,
                                                         &w] {
            const auto firedCount = std::make_shared<int>(0);
            auto *switchTimer = new QTimer(&w);
            switchTimer->setInterval(switchIntervalMs);
            QPointer<QTimer> guard(switchTimer);
            const auto runSwitch = [firedCount, repeatOutputSwitch, switchOutputToIndex, &w, guard] {
                if (!guard || *firedCount >= repeatOutputSwitch) {
                    return;
                }

                ++(*firedCount);
                w.selectOutputDeviceByIndex(switchOutputToIndex);
                if (*firedCount >= repeatOutputSwitch && guard) {
                    guard->stop();
                    guard->deleteLater();
                }
            };

            QObject::connect(switchTimer, &QTimer::timeout, &w, runSwitch);
            runSwitch();
            if (repeatOutputSwitch > 1 && guard) {
                switchTimer->start();
            }
        });
    }

    QString switchSource = parser.value(QStringLiteral("switch-source"));
    bool switchSourceAfterOk = false;
    const int switchSourceAfterMs = parser.value(QStringLiteral("switch-source-after-ms")).toInt(&switchSourceAfterOk);
    if (!switchSource.isEmpty() && switchSourceAfterOk && switchSourceAfterMs >= 0) {
        scheduleAfterPlaybackReady(switchSourceAfterMs, [switchSource, &w] {
            w.loadFileFromPath(switchSource);
        });
    }

    bool quitAfterOk = false;
    const int quitAfterMs = parser.value(QStringLiteral("quit-after-ms")).toInt(&quitAfterOk);
    if (quitAfterOk && quitAfterMs > 0) {
        PlayerLogger::log(QStringLiteral("automation"),
                          QStringLiteral("quit scheduled delayMs=%1").arg(quitAfterMs));
        QTimer::singleShot(quitAfterMs, QCoreApplication::instance(), [quitAfterMs, &w] {
            PlayerLogger::log(QStringLiteral("automation"),
                              QStringLiteral("quit fired delayMs=%1").arg(quitAfterMs));
            w.stopPlaybackNow();
            QCoreApplication::quit();
        });
    }
}
