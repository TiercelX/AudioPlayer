#include "test_pcmstreamformat.h"
#include "ffmpegpcmshared.h"
#include <QTest>

void TestPcmStreamFormat::initTestCase() {}
void TestPcmStreamFormat::cleanupTestCase() {}

void TestPcmStreamFormat::testDefaultConstructor()
{
    PcmStreamFormat fmt;
    QCOMPARE(fmt.sampleRate, 0);
    QCOMPARE(fmt.channelCount, 0);
    QCOMPARE(fmt.sampleEncoding, PcmSampleEncoding::Unknown);
    QCOMPARE(fmt.validBitsPerSample, 0);
}

void TestPcmStreamFormat::testParameterizedConstructor()
{
    PcmStreamFormat fmt;
    fmt.sampleRate = 44100;
    fmt.channelCount = 2;
    fmt.sampleEncoding = PcmSampleEncoding::Int16;
    fmt.validBitsPerSample = 0;

    QCOMPARE(fmt.sampleRate, 44100);
    QCOMPARE(fmt.channelCount, 2);
    QCOMPARE(fmt.sampleEncoding, PcmSampleEncoding::Int16);
}

void TestPcmStreamFormat::testBitsPerSample()
{
    PcmStreamFormat fmt;

    fmt.sampleEncoding = PcmSampleEncoding::Unknown;
    QCOMPARE(fmt.bitsPerSample(), 0);

    fmt.sampleEncoding = PcmSampleEncoding::UInt8;
    QCOMPARE(fmt.bitsPerSample(), 8);

    fmt.sampleEncoding = PcmSampleEncoding::Int16;
    QCOMPARE(fmt.bitsPerSample(), 16);

    fmt.sampleEncoding = PcmSampleEncoding::Int24;
    QCOMPARE(fmt.bitsPerSample(), 24);

    fmt.sampleEncoding = PcmSampleEncoding::Int32;
    QCOMPARE(fmt.bitsPerSample(), 32);

    fmt.sampleEncoding = PcmSampleEncoding::Float32;
    QCOMPARE(fmt.bitsPerSample(), 32);
}

void TestPcmStreamFormat::testBytesPerSample()
{
    PcmStreamFormat fmt;

    fmt.sampleEncoding = PcmSampleEncoding::Unknown;
    QCOMPARE(fmt.bytesPerSample(), 0);

    fmt.sampleEncoding = PcmSampleEncoding::UInt8;
    QCOMPARE(fmt.bytesPerSample(), 1);

    fmt.sampleEncoding = PcmSampleEncoding::Int16;
    QCOMPARE(fmt.bytesPerSample(), 2);

    fmt.sampleEncoding = PcmSampleEncoding::Int24;
    QCOMPARE(fmt.bytesPerSample(), 3);

    fmt.sampleEncoding = PcmSampleEncoding::Int32;
    QCOMPARE(fmt.bytesPerSample(), 4);

    fmt.sampleEncoding = PcmSampleEncoding::Float32;
    QCOMPARE(fmt.bytesPerSample(), 4);
}

void TestPcmStreamFormat::testBytesPerFrame()
{
    PcmStreamFormat fmt;
    fmt.sampleEncoding = PcmSampleEncoding::Int16;

    fmt.channelCount = 0;
    QCOMPARE(fmt.bytesPerFrame(), 0);

    fmt.channelCount = 1;
    QCOMPARE(fmt.bytesPerFrame(), 2);

    fmt.channelCount = 2;
    QCOMPARE(fmt.bytesPerFrame(), 4);

    fmt.channelCount = 6;
    QCOMPARE(fmt.bytesPerFrame(), 12);

    fmt.sampleEncoding = PcmSampleEncoding::Unknown;
    QCOMPARE(fmt.bytesPerFrame(), 0);
}

void TestPcmStreamFormat::testEffectiveValidBitsPerSample()
{
    PcmStreamFormat fmt;
    fmt.sampleEncoding = PcmSampleEncoding::Int24;

    fmt.validBitsPerSample = 0;
    QCOMPARE(fmt.effectiveValidBitsPerSample(), 24);

    fmt.validBitsPerSample = 20;
    QCOMPARE(fmt.effectiveValidBitsPerSample(), 20);

    fmt.sampleEncoding = PcmSampleEncoding::Int16;
    fmt.validBitsPerSample = 0;
    QCOMPARE(fmt.effectiveValidBitsPerSample(), 16);
}

void TestPcmStreamFormat::testIsValid()
{
    PcmStreamFormat fmt;
    QVERIFY(!fmt.isValid());

    fmt.sampleRate = 44100;
    QVERIFY(!fmt.isValid());

    fmt.channelCount = 2;
    QVERIFY(!fmt.isValid());

    fmt.sampleEncoding = PcmSampleEncoding::Int16;
    QVERIFY(fmt.isValid());

    fmt.sampleRate = 0;
    QVERIFY(!fmt.isValid());

    fmt.sampleRate = 48000;
    fmt.channelCount = 0;
    QVERIFY(!fmt.isValid());

    fmt.channelCount = 1;
    fmt.sampleEncoding = PcmSampleEncoding::Unknown;
    QVERIFY(!fmt.isValid());
}

void TestPcmStreamFormat::testEqualityOperator()
{
    PcmStreamFormat a, b;
    QVERIFY(a == b);

    a.sampleRate = 44100;
    b.sampleRate = 44100;
    a.channelCount = 2;
    b.channelCount = 2;
    a.sampleEncoding = PcmSampleEncoding::Int16;
    b.sampleEncoding = PcmSampleEncoding::Int16;
    QVERIFY(a == b);

    b.sampleRate = 48000;
    QVERIFY(!(a == b));

    b.sampleRate = 44100;
    b.channelCount = 1;
    QVERIFY(!(a == b));

    b.channelCount = 2;
    b.sampleEncoding = PcmSampleEncoding::Float32;
    QVERIFY(!(a == b));

    b.sampleEncoding = PcmSampleEncoding::Int16;
    b.validBitsPerSample = 14;
    QVERIFY(!(a == b));
}

void TestPcmStreamFormat::testQAudioSampleFormat()
{
    PcmStreamFormat fmt;

    fmt.sampleEncoding = PcmSampleEncoding::Unknown;
    QCOMPARE(fmt.qAudioSampleFormat(), QAudioFormat::Unknown);

    fmt.sampleEncoding = PcmSampleEncoding::UInt8;
    QCOMPARE(fmt.qAudioSampleFormat(), QAudioFormat::UInt8);

    fmt.sampleEncoding = PcmSampleEncoding::Int16;
    QCOMPARE(fmt.qAudioSampleFormat(), QAudioFormat::Int16);

    fmt.sampleEncoding = PcmSampleEncoding::Int24;
    QCOMPARE(fmt.qAudioSampleFormat(), QAudioFormat::Int32);

    fmt.sampleEncoding = PcmSampleEncoding::Int32;
    QCOMPARE(fmt.qAudioSampleFormat(), QAudioFormat::Int32);

    fmt.sampleEncoding = PcmSampleEncoding::Float32;
    QCOMPARE(fmt.qAudioSampleFormat(), QAudioFormat::Float);
}


