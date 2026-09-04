#ifndef TEST_PCMUTILS_H
#define TEST_PCMUTILS_H

#include <QObject>

class TestPcmUtils : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void testReadInt24SamplePositive();
    void testReadInt24SampleNegative();
    void testReadInt24SampleZero();
    void testReadInt24SampleMax();
    void testReadInt24SampleMin();

    void testFromQAudioSampleFormat();

    void testApplyGainToSampleInt16();
    void testApplyGainToSampleInt24();
    void testApplyGainToSampleInt32();
    void testApplyGainToSampleFloat32();
    void testApplyGainToSampleUInt8();
    void testApplyGainToSampleMute();
    void testApplyGainToSampleClamp();

    void testSampleMagnitudeInt16();
    void testSampleMagnitudeInt24();
    void testSampleMagnitudeInt32();
    void testSampleMagnitudeFloat32();
    void testSampleMagnitudeUInt8();

    void testComputeLinearFadeGain();
    void testComputeLinearFadeGainFromZero();

    void testApplyGainRoundTripInt16();
    void testApplyGainRoundTripFloat32();
};

#endif // TEST_PCMUTILS_H
