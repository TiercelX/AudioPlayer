#include <QApplication>
#include <QString>
#include <QTest>
#include <cstdio>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include "test_pcmstreamformat.h"
#include "test_pcmutils.h"
#include "test_volumecontrol.h"
#include "test_audiobuffer.h"
#include "test_audioplayerfactory.h"
#include "test_audioutils.h"
#include "test_wasapi_states.h"
#include "test_alsa_logic.h"
#include "test_asioformats.h"
#include "test_integration.h"

static void silentMessageHandler(QtMsgType, const QMessageLogContext &, const QString &)
{
}

static FILE *s_trace = nullptr;

#define RUN_SUITE(Type) do { \
    if (s_trace) { fprintf(s_trace, "=== RUN  " #Type "\n"); fflush(s_trace); } \
    { Type _t; int _r = QTest::qExec(&_t, argc, argv); \
      if (s_trace) { fprintf(s_trace, "=== %s " #Type "\n", _r == 0 ? "PASS" : "FAIL"); fflush(s_trace); } \
      result += _r; } \
} while(0)

int main(int argc, char *argv[])
{
#ifdef Q_OS_WIN
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
#endif

    QString tracePath = QString("%1/test-trace.txt").arg(AUDIOPLAYER_BUILD_DIR);
    s_trace = fopen(tracePath.toLocal8Bit().constData(), "w");

    qInstallMessageHandler(silentMessageHandler);
    QApplication app(argc, argv);
    qInstallMessageHandler(nullptr);

    int result = 0;
    RUN_SUITE(TestPcmStreamFormat);
    RUN_SUITE(TestPcmUtils);
    RUN_SUITE(TestAudioUtils);
    RUN_SUITE(TestVolumeControl);
    RUN_SUITE(TestAudioBuffer);
    RUN_SUITE(TestAudioPlayerFactory);
    RUN_SUITE(TestWasapiStates);
    RUN_SUITE(TestAlsaLogic);
    RUN_SUITE(TestAsioFormats);
    RUN_SUITE(TestIntegration);

    if (s_trace) {
        fprintf(s_trace, "\n=== TOTAL: %s\n", result == 0 ? "ALL PASSED" : "FAILURES");
        fclose(s_trace);
    }

    return result;
}
