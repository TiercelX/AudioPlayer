#ifndef TEST_ASIOFORMATS_H
#define TEST_ASIOFORMATS_H

#include <QObject>

class TestAsioFormats : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void testAppendUniqueSampleRateValid();
    void testAppendUniqueSampleRateDuplicate();
    void testAppendUniqueSampleRateZero();
    void testAppendUniqueSampleRateNegative();
    void testAppendUniqueSampleRateNull();

    void testSourcePreferredSampleRateCandidates44100();
    void testSourcePreferredSampleRateCandidates48000();
    void testSourcePreferredSampleRateCandidates96000();
    void testSourcePreferredSampleRateCandidatesZero();
    void testSourcePreferredSampleRateCandidatesFallback();

    void testPcmStreamFormatFromQAudioFormatInt16();
    void testPcmStreamFormatFromQAudioFormatInt32();
    void testPcmStreamFormatFromQAudioFormatFloat();
    void testPcmStreamFormatFromQAudioFormatUInt8();
    void testPcmStreamFormatFromQAudioFormatUnknown();

    void testPcmCodecName();
    void testPcmSampleFormatName();
    void testPcmMuxerName();
};

#endif // TEST_ASIOFORMATS_H
