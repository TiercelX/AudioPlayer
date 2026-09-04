#include "test_asioformats.h"
#include "windowsasioaudioplayer_formats.h"
#include "ffmpegpcmshared.h"

#include <QtTest>
#include <QAudioFormat>

void TestAsioFormats::initTestCase() {}
void TestAsioFormats::cleanupTestCase() {}

void TestAsioFormats::testAppendUniqueSampleRateValid()
{
    QList<int> rates;
    AsioFormats::appendUniqueSampleRate(&rates, 48000);
    QCOMPARE(rates.size(), 1);
    QCOMPARE(rates.first(), 48000);
}

void TestAsioFormats::testAppendUniqueSampleRateDuplicate()
{
    QList<int> rates;
    AsioFormats::appendUniqueSampleRate(&rates, 48000);
    AsioFormats::appendUniqueSampleRate(&rates, 48000);
    QCOMPARE(rates.size(), 1);
}

void TestAsioFormats::testAppendUniqueSampleRateZero()
{
    QList<int> rates;
    AsioFormats::appendUniqueSampleRate(&rates, 0);
    QVERIFY(rates.isEmpty());
}

void TestAsioFormats::testAppendUniqueSampleRateNegative()
{
    QList<int> rates;
    AsioFormats::appendUniqueSampleRate(&rates, -1);
    QVERIFY(rates.isEmpty());
}

void TestAsioFormats::testAppendUniqueSampleRateNull()
{
    AsioFormats::appendUniqueSampleRate(nullptr, 48000);
}

void TestAsioFormats::testSourcePreferredSampleRateCandidates44100()
{
    auto rates = AsioFormats::sourcePreferredSampleRateCandidates(44100);
    QVERIFY(!rates.isEmpty());
    QCOMPARE(rates.first(), 44100);
    QVERIFY(rates.contains(48000));
    QVERIFY(rates.contains(96000));
}

void TestAsioFormats::testSourcePreferredSampleRateCandidates48000()
{
    auto rates = AsioFormats::sourcePreferredSampleRateCandidates(48000);
    QVERIFY(!rates.isEmpty());
    QCOMPARE(rates.first(), 48000);
    QVERIFY(rates.contains(44100));
    QVERIFY(rates.contains(96000));
}

void TestAsioFormats::testSourcePreferredSampleRateCandidates96000()
{
    auto rates = AsioFormats::sourcePreferredSampleRateCandidates(96000);
    QVERIFY(!rates.isEmpty());
    QCOMPARE(rates.first(), 96000);
    QVERIFY(rates.contains(48000));
}

void TestAsioFormats::testSourcePreferredSampleRateCandidatesZero()
{
    auto rates = AsioFormats::sourcePreferredSampleRateCandidates(0);
    QVERIFY(!rates.isEmpty());
    QCOMPARE(rates.first(), 48000);
    QVERIFY(rates.contains(44100));
}

void TestAsioFormats::testSourcePreferredSampleRateCandidatesFallback()
{
    auto rates = AsioFormats::sourcePreferredSampleRateCandidates(0, 96000);
    QVERIFY(!rates.isEmpty());
    QCOMPARE(rates.first(), 96000);
}

void TestAsioFormats::testPcmStreamFormatFromQAudioFormatInt16()
{
    QAudioFormat fmt;
    fmt.setSampleRate(44100);
    fmt.setChannelCount(2);
    fmt.setSampleFormat(QAudioFormat::Int16);

    auto pcm = AsioFormats::pcmStreamFormatFromQAudioFormat(fmt);
    QCOMPARE(pcm.sampleRate, 44100);
    QCOMPARE(pcm.channelCount, 2);
    QCOMPARE(pcm.sampleEncoding, PcmSampleEncoding::Int16);
    QCOMPARE(pcm.validBitsPerSample, 16);
}

void TestAsioFormats::testPcmStreamFormatFromQAudioFormatInt32()
{
    QAudioFormat fmt;
    fmt.setSampleRate(96000);
    fmt.setChannelCount(6);
    fmt.setSampleFormat(QAudioFormat::Int32);

    auto pcm = AsioFormats::pcmStreamFormatFromQAudioFormat(fmt);
    QCOMPARE(pcm.sampleRate, 96000);
    QCOMPARE(pcm.channelCount, 6);
    QCOMPARE(pcm.sampleEncoding, PcmSampleEncoding::Int32);
    QCOMPARE(pcm.validBitsPerSample, 32);
}

void TestAsioFormats::testPcmStreamFormatFromQAudioFormatFloat()
{
    QAudioFormat fmt;
    fmt.setSampleRate(48000);
    fmt.setChannelCount(2);
    fmt.setSampleFormat(QAudioFormat::Float);

    auto pcm = AsioFormats::pcmStreamFormatFromQAudioFormat(fmt);
    QCOMPARE(pcm.sampleEncoding, PcmSampleEncoding::Float32);
    QCOMPARE(pcm.validBitsPerSample, 32);
}

void TestAsioFormats::testPcmStreamFormatFromQAudioFormatUInt8()
{
    QAudioFormat fmt;
    fmt.setSampleRate(22050);
    fmt.setChannelCount(1);
    fmt.setSampleFormat(QAudioFormat::UInt8);

    auto pcm = AsioFormats::pcmStreamFormatFromQAudioFormat(fmt);
    QCOMPARE(pcm.sampleEncoding, PcmSampleEncoding::UInt8);
    QCOMPARE(pcm.validBitsPerSample, 8);
}

void TestAsioFormats::testPcmStreamFormatFromQAudioFormatUnknown()
{
    QAudioFormat fmt;
    auto pcm = AsioFormats::pcmStreamFormatFromQAudioFormat(fmt);
    QCOMPARE(pcm.sampleEncoding, PcmSampleEncoding::Unknown);
    QCOMPARE(pcm.validBitsPerSample, 0);
}

void TestAsioFormats::testPcmCodecName()
{
    QCOMPARE(AsioFormats::pcmCodecName(QAudioFormat::Int16), QStringLiteral("pcm_s16le"));
    QCOMPARE(AsioFormats::pcmCodecName(QAudioFormat::Int32), QStringLiteral("pcm_s32le"));
    QCOMPARE(AsioFormats::pcmCodecName(QAudioFormat::Float), QStringLiteral("pcm_f32le"));
    QCOMPARE(AsioFormats::pcmCodecName(QAudioFormat::UInt8), QStringLiteral("pcm_u8"));
}

void TestAsioFormats::testPcmSampleFormatName()
{
    QCOMPARE(AsioFormats::pcmSampleFormatName(QAudioFormat::Int16), QStringLiteral("s16"));
    QCOMPARE(AsioFormats::pcmSampleFormatName(QAudioFormat::Int32), QStringLiteral("s32"));
    QCOMPARE(AsioFormats::pcmSampleFormatName(QAudioFormat::Float), QStringLiteral("flt"));
    QCOMPARE(AsioFormats::pcmSampleFormatName(QAudioFormat::UInt8), QStringLiteral("u8"));
}

void TestAsioFormats::testPcmMuxerName()
{
    QCOMPARE(AsioFormats::pcmMuxerName(QAudioFormat::Int16), QStringLiteral("s16le"));
    QCOMPARE(AsioFormats::pcmMuxerName(QAudioFormat::Int32), QStringLiteral("s32le"));
    QCOMPARE(AsioFormats::pcmMuxerName(QAudioFormat::Float), QStringLiteral("f32le"));
    QCOMPARE(AsioFormats::pcmMuxerName(QAudioFormat::UInt8), QStringLiteral("u8"));
}

#include "test_asioformats.moc"
