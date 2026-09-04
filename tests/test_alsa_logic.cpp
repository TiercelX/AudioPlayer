#include "test_alsa_logic.h"
#include "alsalogic.h"
#include "ffmpegpcmshared.h"
#include <QTest>

void TestAlsaLogic::initTestCase() {}
void TestAlsaLogic::cleanupTestCase() {}

// --- rawInputFormatForPath tests ---

void TestAlsaLogic::testRawFormatTruehd()
{
    QCOMPARE(AlsaLogic::rawInputFormatForPath(QStringLiteral("/music/song.truehd")),
             QStringLiteral("truehd"));
}

void TestAlsaLogic::testRawFormatMlp()
{
    QCOMPARE(AlsaLogic::rawInputFormatForPath(QStringLiteral("song.mlp")),
             QStringLiteral("truehd"));
}

void TestAlsaLogic::testRawFormatThd()
{
    QCOMPARE(AlsaLogic::rawInputFormatForPath(QStringLiteral("song.thd")),
             QStringLiteral("truehd"));
}

void TestAlsaLogic::testRawFormatEac3()
{
    QCOMPARE(AlsaLogic::rawInputFormatForPath(QStringLiteral("song.ec3")),
             QStringLiteral("eac3"));
}

void TestAlsaLogic::testRawFormatEb3()
{
    QCOMPARE(AlsaLogic::rawInputFormatForPath(QStringLiteral("song.eb3")),
             QStringLiteral("eac3"));
}

void TestAlsaLogic::testRawFormatAc3()
{
    QCOMPARE(AlsaLogic::rawInputFormatForPath(QStringLiteral("song.ac3")),
             QString());
}

void TestAlsaLogic::testRawFormatFlac()
{
    QCOMPARE(AlsaLogic::rawInputFormatForPath(QStringLiteral("song.flac")),
             QString());
}

void TestAlsaLogic::testRawFormatEmpty()
{
    QCOMPARE(AlsaLogic::rawInputFormatForPath(QString()),
             QString());
}

void TestAlsaLogic::testCaseInsensitive()
{
    QCOMPARE(AlsaLogic::rawInputFormatForPath(QStringLiteral("song.TRUEHD")),
             QStringLiteral("truehd"));
    QCOMPARE(AlsaLogic::rawInputFormatForPath(QStringLiteral("song.EC3")),
             QStringLiteral("eac3"));
    QCOMPARE(AlsaLogic::rawInputFormatForPath(QStringLiteral("song.MLP")),
             QStringLiteral("truehd"));
}

// --- startupThresholdBytes tests ---

void TestAlsaLogic::testThresholdNormalStart()
{
    // 48000Hz stereo 16bit: bytesPerFrame=4, threshold = max(32768, 4*48000*200/1000) = max(32768, 38400) = 38400
    PcmStreamFormat fmt;
    fmt.sampleRate = 48000;
    fmt.channelCount = 2;
    fmt.sampleEncoding = PcmSampleEncoding::Int16;
    QCOMPARE(AlsaLogic::startupThresholdBytes(fmt, 0), static_cast<qsizetype>(38400));
}

void TestAlsaLogic::testThresholdSeekResume()
{
    // 48000Hz stereo 16bit: threshold = max(32768, 4*48000*100/1000) = max(32768, 19200) = 32768
    PcmStreamFormat fmt;
    fmt.sampleRate = 48000;
    fmt.channelCount = 2;
    fmt.sampleEncoding = PcmSampleEncoding::Int16;
    QCOMPARE(AlsaLogic::startupThresholdBytes(fmt, 1), static_cast<qsizetype>(32768));
}

void TestAlsaLogic::testThresholdInvalidFormat()
{
    // Invalid format -> 32768
    PcmStreamFormat fmt;
    QCOMPARE(AlsaLogic::startupThresholdBytes(fmt, 0), static_cast<qsizetype>(32768));
}

void TestAlsaLogic::testThresholdMinimumFloor()
{
    // Very low sample rate -> still at least 32768
    PcmStreamFormat fmt;
    fmt.sampleRate = 100;
    fmt.channelCount = 1;
    fmt.sampleEncoding = PcmSampleEncoding::Int16;
    // bytesPerFrame=2, threshold = max(32768, 2*100*200/1000) = max(32768, 40) = 32768
    QCOMPARE(AlsaLogic::startupThresholdBytes(fmt, 0), static_cast<qsizetype>(32768));
}
