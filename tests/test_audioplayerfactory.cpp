#include "test_audioplayerfactory.h"
#include "audioplayerfactory.h"

#include <QTest>

void TestAudioPlayerFactory::initTestCase() {}
void TestAudioPlayerFactory::cleanupTestCase() {}

void TestAudioPlayerFactory::testSourceContextDefaultConstruction()
{
    AudioPlayerSourceContext ctx;
    QVERIFY(ctx.filePath.isEmpty());
    QVERIFY(ctx.codecName.isEmpty());
    QCOMPARE(ctx.sourceChannelCount, 0);
}

void TestAudioPlayerFactory::testSourceContextFieldAssignment()
{
    AudioPlayerSourceContext ctx;
    ctx.filePath = QStringLiteral("/path/to/song.flac");
    ctx.codecName = QStringLiteral("flac");
    ctx.sourceChannelCount = 2;

    QCOMPARE(ctx.filePath, QStringLiteral("/path/to/song.flac"));
    QCOMPARE(ctx.codecName, QStringLiteral("flac"));
    QCOMPARE(ctx.sourceChannelCount, 2);
}

void TestAudioPlayerFactory::testPlaybackPlanDefaults()
{
    AudioPlaybackPlan plan;
    QCOMPARE(plan.backendId, AudioPlayerBackend::BackendId::Ffmpeg);
    QCOMPARE(plan.sourceMode, AudioPlaybackPlan::SourceMode::OriginalFile);
}

void TestAudioPlayerFactory::testPlaybackPlanSourceModeValues()
{
    AudioPlaybackPlan plan;

    plan.sourceMode = AudioPlaybackPlan::SourceMode::OriginalFile;
    QCOMPARE(plan.sourceMode, AudioPlaybackPlan::SourceMode::OriginalFile);

    plan.sourceMode = AudioPlaybackPlan::SourceMode::RemuxRawDolbySidecar;
    QCOMPARE(plan.sourceMode, AudioPlaybackPlan::SourceMode::RemuxRawDolbySidecar);
}

void TestAudioPlayerFactory::testBackendIdEnumValues()
{
    QCOMPARE(static_cast<int>(AudioPlayerBackend::BackendId::Ffmpeg), 0);
    QCOMPARE(static_cast<int>(AudioPlayerBackend::BackendId::WindowsWasapi), 1);
    QCOMPARE(static_cast<int>(AudioPlayerBackend::BackendId::WindowsAsio), 2);
    QCOMPARE(static_cast<int>(AudioPlayerBackend::BackendId::AppleNative), 3);
    QCOMPARE(static_cast<int>(AudioPlayerBackend::BackendId::AndroidNative), 4);
    QCOMPARE(static_cast<int>(AudioPlayerBackend::BackendId::LinuxAlsa), 5);
}


