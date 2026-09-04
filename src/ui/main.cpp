#include "automationoptions.h"
#include "mainwindow.h"

#include <QApplication>
#include <QCommandLineParser>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    QCoreApplication::setApplicationName(QStringLiteral("AudioPlayer"));
    QCoreApplication::setApplicationVersion(QStringLiteral("1.0"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Audio player"));
    parser.addHelpOption();
    parser.addVersionOption();

    registerAutomationOptions(parser);
    parser.addPositionalArgument(QStringLiteral("source"), QStringLiteral("Optional audio file to load on startup."));
    parser.process(a);

    if (const auto probeExit = runAsioProbeIfRequested(parser)) {
        return *probeExit;
    }

    MainWindow w;
    w.show();

    setupAutomationFromCli(parser, w);

    return QCoreApplication::exec();
}
