#ifndef TEST_AUDIOUTILS_H
#define TEST_AUDIOUTILS_H

#include <QObject>

class TestAudioUtils : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void testChannelLayoutForCount_data();
    void testChannelLayoutForCount();

    void testPcmCodecName_data();
    void testPcmCodecName();

    void testPcmSampleFormatName_data();
    void testPcmSampleFormatName();

    void testPcmMuxerName_data();
    void testPcmMuxerName();

    void testPlaybackStateName_data();
    void testPlaybackStateName();

    void testAudioStateName_data();
    void testAudioStateName();
};

#endif // TEST_AUDIOUTILS_H
