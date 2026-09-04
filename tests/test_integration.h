#ifndef TEST_INTEGRATION_H
#define TEST_INTEGRATION_H

#include <QObject>

class TestIntegration : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    // 1. Complete playback flow
    void testPlaybackFlowFileLoadToOutput();
    void testPlaybackFlowBackendSelectionFlac();
    void testPlaybackFlowBackendSelectionDolby();
    void testPlaybackFlowSourceProbe();
    void testPlaybackFlowSetSourceBeforePlay();
    void testPlaybackFlowStopResetsState();

    // 2. Backend switching
    void testBackendSwitchFfmpegToWasapi();
    void testBackendSwitchWasapiToAsio();
    void testBackendSwitchPreservesVolume();
    void testBackendSwitchPreservesPosition();
    void testBackendSwitchStopsPrevious();

    // 3. Seek tests
    void testSeekWhilePlaying();
    void testSeekWhilePaused();
    void testSeekWhileStopped();
    void testSeekUpdatesPosition();
    void testSeekToZero();
    void testSeekEmitsPositionChanged();

    // 4. Error recovery
    void testRecoveryAfterDeviceDisconnect();
    void testRecoveryAfterDecoderError();
    void testRecoveryExhaustionEmitsError();
    void testRecoveryResetsOnSuccessfulPlayback();
    void testErrorSignalEmitted();
};

#endif // TEST_INTEGRATION_H
