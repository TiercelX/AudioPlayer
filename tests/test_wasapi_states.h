#ifndef TEST_WASAPI_STATES_H
#define TEST_WASAPI_STATES_H

#include <QObject>

class TestWasapiStates : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    // PlaybackState transitions via MockAudioPlayerBackend
    void testStoppedToPlaying();
    void testPlayingToPaused();
    void testPlayingToStopping();
    void testPausedToPlaying();
    void testPausedToStopping();
    void testStoppingToStopped();
    void testPlayingToStoppedOnError();
};

#endif // TEST_WASAPI_STATES_H
