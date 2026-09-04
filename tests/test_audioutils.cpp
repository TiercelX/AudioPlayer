#include "test_audioutils.h"
#include "audioutils.h"

#include <QtTest>
#include <QAudio>

void TestAudioUtils::initTestCase()
{
}

void TestAudioUtils::cleanupTestCase()
{
}

void TestAudioUtils::testChannelLayoutForCount_data()
{
    QTest::addColumn<int>("channels");
    QTest::addColumn<QString>("expected");

    QTest::newRow("1ch-mono")   << 1 << QString("mono");
    QTest::newRow("2ch-stereo") << 2 << QString("stereo");
    QTest::newRow("3ch-2.1")    << 3 << QString("2.1");
    QTest::newRow("4ch-quad")   << 4 << QString("quad");
    QTest::newRow("5ch-4.1")    << 5 << QString("4.1");
    QTest::newRow("6ch-5.1")    << 6 << QString("5.1");
    QTest::newRow("7ch-6.1")    << 7 << QString("6.1");
    QTest::newRow("8ch-7.1")    << 8 << QString("7.1");
    QTest::newRow("0ch-empty")  << 0 << QString();
    QTest::newRow("9ch-empty")  << 9 << QString();
}

void TestAudioUtils::testChannelLayoutForCount()
{
    QFETCH(int, channels);
    QFETCH(QString, expected);
    QCOMPARE(AudioUtils::channelLayoutForCount(channels), expected);
}

void TestAudioUtils::testPcmCodecName_data()
{
    QTest::addColumn<PcmSampleEncoding>("encoding");
    QTest::addColumn<QString>("expected");

    QTest::newRow("UInt8")   << PcmSampleEncoding::UInt8   << QString("pcm_u8");
    QTest::newRow("Int16")   << PcmSampleEncoding::Int16   << QString("pcm_s16le");
    QTest::newRow("Int24")   << PcmSampleEncoding::Int24   << QString("pcm_s32le");
    QTest::newRow("Int32")   << PcmSampleEncoding::Int32   << QString("pcm_s32le");
    QTest::newRow("Float32") << PcmSampleEncoding::Float32 << QString("pcm_f32le");
    QTest::newRow("Unknown") << PcmSampleEncoding::Unknown << QString("pcm_s16le");
}

void TestAudioUtils::testPcmCodecName()
{
    QFETCH(PcmSampleEncoding, encoding);
    QFETCH(QString, expected);
    QCOMPARE(AudioUtils::pcmCodecName(encoding), expected);
}

void TestAudioUtils::testPcmSampleFormatName_data()
{
    QTest::addColumn<PcmSampleEncoding>("encoding");
    QTest::addColumn<QString>("expected");

    QTest::newRow("UInt8")   << PcmSampleEncoding::UInt8   << QString("u8");
    QTest::newRow("Int16")   << PcmSampleEncoding::Int16   << QString("s16");
    QTest::newRow("Int24")   << PcmSampleEncoding::Int24   << QString("s32");
    QTest::newRow("Int32")   << PcmSampleEncoding::Int32   << QString("s32");
    QTest::newRow("Float32") << PcmSampleEncoding::Float32 << QString("flt");
    QTest::newRow("Unknown") << PcmSampleEncoding::Unknown << QString("s16");
}

void TestAudioUtils::testPcmSampleFormatName()
{
    QFETCH(PcmSampleEncoding, encoding);
    QFETCH(QString, expected);
    QCOMPARE(AudioUtils::pcmSampleFormatName(encoding), expected);
}

void TestAudioUtils::testPcmMuxerName_data()
{
    QTest::addColumn<PcmSampleEncoding>("encoding");
    QTest::addColumn<QString>("expected");

    QTest::newRow("UInt8")   << PcmSampleEncoding::UInt8   << QString("u8");
    QTest::newRow("Int16")   << PcmSampleEncoding::Int16   << QString("s16le");
    QTest::newRow("Int24")   << PcmSampleEncoding::Int24   << QString("s32le");
    QTest::newRow("Int32")   << PcmSampleEncoding::Int32   << QString("s32le");
    QTest::newRow("Float32") << PcmSampleEncoding::Float32 << QString("f32le");
    QTest::newRow("Unknown") << PcmSampleEncoding::Unknown << QString("s16le");
}

void TestAudioUtils::testPcmMuxerName()
{
    QFETCH(PcmSampleEncoding, encoding);
    QFETCH(QString, expected);
    QCOMPARE(AudioUtils::pcmMuxerName(encoding), expected);
}

void TestAudioUtils::testPlaybackStateName_data()
{
    QTest::addColumn<AudioPlayerBackend::PlaybackState>("state");
    QTest::addColumn<QString>("expected");

    QTest::newRow("Stopped") << AudioPlayerBackend::PlaybackState::Stopped   << QString("Stopped");
    QTest::newRow("Playing") << AudioPlayerBackend::PlaybackState::Playing   << QString("Playing");
    QTest::newRow("Paused")  << AudioPlayerBackend::PlaybackState::Paused    << QString("Paused");
    QTest::newRow("Stopping")<< AudioPlayerBackend::PlaybackState::Stopping  << QString("Stopping");
}

void TestAudioUtils::testPlaybackStateName()
{
    QFETCH(AudioPlayerBackend::PlaybackState, state);
    QFETCH(QString, expected);
    QCOMPARE(AudioUtils::playbackStateName(state), expected);
}

void TestAudioUtils::testAudioStateName_data()
{
    QTest::addColumn<QAudio::State>("state");
    QTest::addColumn<QString>("expected");

    QTest::newRow("Active")    << QAudio::ActiveState    << QString("Active");
    QTest::newRow("Suspended") << QAudio::SuspendedState << QString("Suspended");
    QTest::newRow("Stopped")   << QAudio::StoppedState   << QString("Stopped");
    QTest::newRow("Idle")      << QAudio::IdleState      << QString("Idle");
}

void TestAudioUtils::testAudioStateName()
{
    QFETCH(QAudio::State, state);
    QFETCH(QString, expected);
    QCOMPARE(AudioUtils::audioStateName(state), expected);
}
