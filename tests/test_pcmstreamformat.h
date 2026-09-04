#ifndef TEST_PCMSTREAMFORMAT_H
#define TEST_PCMSTREAMFORMAT_H

#include <QObject>

class TestPcmStreamFormat : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void testDefaultConstructor();
    void testParameterizedConstructor();

    void testBitsPerSample();
    void testBytesPerSample();
    void testBytesPerFrame();
    void testEffectiveValidBitsPerSample();

    void testIsValid();
    void testEqualityOperator();

    void testQAudioSampleFormat();
};

#endif // TEST_PCMSTREAMFORMAT_H
