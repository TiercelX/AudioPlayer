#include "test_pcmutils.h"
#include "pcmutils.h"

#include <QtTest>
#include <QAudioFormat>
#include <QtEndian>
#include <cstring>

void TestPcmUtils::initTestCase() {}
void TestPcmUtils::cleanupTestCase() {}

void TestPcmUtils::testReadInt24SamplePositive()
{
    char data[3] = { 0x00, 0x00, static_cast<char>(0x01) }; // 0x010000 = 65536
    QCOMPARE(PcmUtils::readInt24Sample(data), 65536);
}

void TestPcmUtils::testReadInt24SampleNegative()
{
    char data[3] = { 0x00, 0x00, static_cast<char>(0x80) }; // sign bit set = -8388608
    QCOMPARE(PcmUtils::readInt24Sample(data), -8388608);
}

void TestPcmUtils::testReadInt24SampleZero()
{
    char data[3] = { 0x00, 0x00, 0x00 };
    QCOMPARE(PcmUtils::readInt24Sample(data), 0);
}

void TestPcmUtils::testReadInt24SampleMax()
{
    char data[3] = { static_cast<char>(0xFF), static_cast<char>(0xFF), 0x7F }; // 0x7FFFFF = 8388607
    QCOMPARE(PcmUtils::readInt24Sample(data), 8388607);
}

void TestPcmUtils::testReadInt24SampleMin()
{
    char data[3] = { 0x00, 0x00, static_cast<char>(0x80) }; // 0x800000 = -8388608
    QCOMPARE(PcmUtils::readInt24Sample(data), -8388608);
}

void TestPcmUtils::testFromQAudioSampleFormat()
{
    QCOMPARE(PcmUtils::fromQAudioSampleFormat(QAudioFormat::UInt8), PcmSampleEncoding::UInt8);
    QCOMPARE(PcmUtils::fromQAudioSampleFormat(QAudioFormat::Int16), PcmSampleEncoding::Int16);
    QCOMPARE(PcmUtils::fromQAudioSampleFormat(QAudioFormat::Int32), PcmSampleEncoding::Int32);
    QCOMPARE(PcmUtils::fromQAudioSampleFormat(QAudioFormat::Float), PcmSampleEncoding::Float32);
    QCOMPARE(PcmUtils::fromQAudioSampleFormat(QAudioFormat::Unknown), PcmSampleEncoding::Unknown);
}

void TestPcmUtils::testApplyGainToSampleInt16()
{
    char data[2];
    qint16 value = 16384;
    qToLittleEndian<qint16>(value, data);

    PcmUtils::applyGainToSample(PcmSampleEncoding::Int16, data, 0.5);
    qint16 result = qFromLittleEndian<qint16>(data);
    QCOMPARE(result, static_cast<qint16>(8192));
}

void TestPcmUtils::testApplyGainToSampleInt24()
{
    char data[3];
    qint32 value = 4194304; // half of max 24-bit
    data[0] = static_cast<char>(value & 0xFF);
    data[1] = static_cast<char>((value >> 8) & 0xFF);
    data[2] = static_cast<char>((value >> 16) & 0xFF);

    PcmUtils::applyGainToSample(PcmSampleEncoding::Int24, data, 0.5);
    qint32 result = PcmUtils::readInt24Sample(data);
    QCOMPARE(result, static_cast<qint32>(2097152));
}

void TestPcmUtils::testApplyGainToSampleInt32()
{
    char data[4];
    qint32 value = 1073741824; // quarter of max 32-bit
    qToLittleEndian<qint32>(value, data);

    PcmUtils::applyGainToSample(PcmSampleEncoding::Int32, data, 0.5);
    qint32 result = qFromLittleEndian<qint32>(data);
    QCOMPARE(result, static_cast<qint32>(536870912));
}

void TestPcmUtils::testApplyGainToSampleFloat32()
{
    char data[4];
    float value = 0.5f;
    std::memcpy(data, &value, sizeof(float));

    PcmUtils::applyGainToSample(PcmSampleEncoding::Float32, data, 0.5f);
    float result = 0.0f;
    std::memcpy(&result, data, sizeof(float));
    QVERIFY(qAbs(result - 0.25f) < 0.001f);
}

void TestPcmUtils::testApplyGainToSampleUInt8()
{
    char data[1];
    data[0] = 128 + 64; // centered = 64

    PcmUtils::applyGainToSample(PcmSampleEncoding::UInt8, data, 0.5);
    QCOMPARE(static_cast<quint8>(data[0]), static_cast<quint8>(128 + 32));
}

void TestPcmUtils::testApplyGainToSampleMute()
{
    char data[2];
    qint16 value = 16384;
    qToLittleEndian<qint16>(value, data);

    PcmUtils::applyGainToSample(PcmSampleEncoding::Int16, data, 0.0);
    qint16 result = qFromLittleEndian<qint16>(data);
    QCOMPARE(result, static_cast<qint16>(0));
}

void TestPcmUtils::testApplyGainToSampleClamp()
{
    char data[2];
    qint16 value = 20000;
    qToLittleEndian<qint16>(value, data);

    PcmUtils::applyGainToSample(PcmSampleEncoding::Int16, data, 2.0);
    qint16 result = qFromLittleEndian<qint16>(data);
    QCOMPARE(result, static_cast<qint16>(32767)); // clamped to max
}

void TestPcmUtils::testSampleMagnitudeInt16()
{
    char data[2];
    qint16 value = 16384; // half of max
    qToLittleEndian<qint16>(value, data);

    qreal mag = PcmUtils::sampleMagnitude(PcmSampleEncoding::Int16, data);
    QVERIFY(qAbs(mag - 16384.0 / 32768.0) < 0.0001);
}

void TestPcmUtils::testSampleMagnitudeInt24()
{
    char data[3];
    qint32 value = 4194304;
    data[0] = static_cast<char>(value & 0xFF);
    data[1] = static_cast<char>((value >> 8) & 0xFF);
    data[2] = static_cast<char>((value >> 16) & 0xFF);

    qreal mag = PcmUtils::sampleMagnitude(PcmSampleEncoding::Int24, data);
    QVERIFY(qAbs(mag - 4194304.0 / 8388608.0) < 0.0001);
}

void TestPcmUtils::testSampleMagnitudeInt32()
{
    char data[4];
    qint32 value = 1073741824;
    qToLittleEndian<qint32>(value, data);

    qreal mag = PcmUtils::sampleMagnitude(PcmSampleEncoding::Int32, data);
    QVERIFY(qAbs(mag - 1073741824.0 / 2147483648.0) < 0.0001);
}

void TestPcmUtils::testSampleMagnitudeFloat32()
{
    char data[4];
    float value = 0.75f;
    std::memcpy(data, &value, sizeof(float));

    qreal mag = PcmUtils::sampleMagnitude(PcmSampleEncoding::Float32, data);
    QVERIFY(qAbs(mag - 0.75) < 0.001);
}

void TestPcmUtils::testSampleMagnitudeUInt8()
{
    char data[1];
    data[0] = 128 + 64; // centered = 64, magnitude = 64/127

    qreal mag = PcmUtils::sampleMagnitude(PcmSampleEncoding::UInt8, data);
    QVERIFY(qAbs(mag - 64.0 / 127.0) < 0.001);
}

void TestPcmUtils::testComputeLinearFadeGain()
{
    // 10 total frames, at frame 4 of first call
    qreal gain = PcmUtils::computeLinearFadeGain(0, 4, 10);
    QVERIFY(qAbs(gain - 5.0 / 10.0) < 0.001);

    // already processed 5 frames, at frame 0 of next chunk
    gain = PcmUtils::computeLinearFadeGain(5, 0, 10);
    QVERIFY(qAbs(gain - 6.0 / 10.0) < 0.001);

    // near end
    gain = PcmUtils::computeLinearFadeGain(8, 1, 10);
    QVERIFY(qAbs(gain - 1.0) < 0.001);
}

void TestPcmUtils::testComputeLinearFadeGainFromZero()
{
    // 10 total frames, at start
    qreal gain = PcmUtils::computeLinearFadeGainFromZero(0, 0, 10);
    QVERIFY(qAbs(gain - 0.0) < 0.001);

    // at frame 5
    gain = PcmUtils::computeLinearFadeGainFromZero(0, 5, 10);
    QVERIFY(qAbs(gain - 5.0 / 9.0) < 0.001);

    // near end
    gain = PcmUtils::computeLinearFadeGainFromZero(0, 9, 10);
    QVERIFY(qAbs(gain - 1.0) < 0.001);
}

void TestPcmUtils::testApplyGainRoundTripInt16()
{
    char data[2];
    qint16 original = 12345;
    qToLittleEndian<qint16>(original, data);

    PcmUtils::applyGainToSample(PcmSampleEncoding::Int16, data, 2.0);
    PcmUtils::applyGainToSample(PcmSampleEncoding::Int16, data, 0.5);
    qint16 result = qFromLittleEndian<qint16>(data);
    QCOMPARE(result, original);
}

void TestPcmUtils::testApplyGainRoundTripFloat32()
{
    char data[4];
    float original = 0.25f;
    std::memcpy(data, &original, sizeof(float));

    PcmUtils::applyGainToSample(PcmSampleEncoding::Float32, data, 4.0f);
    PcmUtils::applyGainToSample(PcmSampleEncoding::Float32, data, 0.25f);
    float result = 0.0f;
    std::memcpy(&result, data, sizeof(float));
    QVERIFY(qAbs(result - original) < 0.001f);
}

#include "test_pcmutils.moc"
