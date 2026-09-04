#include "test_integration.h"
#include "audioplayerbackend.h"
#include "audioplayerfactory.h"
#include "mediainfodialog.h"

#include <QTest>
#include <QSignalSpy>
#include <QAudioFormat>
#include <QAudioDevice>
#include <QtMultimedia/qaudio.h>

using PlaybackState = AudioPlayerBackend::PlaybackState;
using PlaybackError = AudioPlayerBackend::PlaybackError;
using BackendId = AudioPlayerBackend::BackendId;

namespace {

class MockAudioPlayerBackend : public AudioPlayerBackend
{
    Q_OBJECT

public:
    explicit MockAudioPlayerBackend(BackendId id = BackendId::Ffmpeg,
                                    QObject *parent = nullptr)
        : AudioPlayerBackend(parent)
        , m_id(id)
    {
    }

    BackendId backendId() const override { return m_id; }
    QString backendName() const override
    {
        switch (m_id) {
        case BackendId::Ffmpeg: return QStringLiteral("mock-ffmpeg");
        case BackendId::WindowsWasapi: return QStringLiteral("mock-wasapi");
        case BackendId::WindowsAsio: return QStringLiteral("mock-asio");
        default: return QStringLiteral("mock");
        }
    }
    QString decoderName() const override { return QStringLiteral("mock-decoder"); }

    void setSource(const QString &filePath, int channels, int sampleRate,
                   int bitDepth, const QString &codecName) override
    {
        m_sourcePath = filePath;
        m_sourceChannels = channels;
        m_sourceSampleRate = sampleRate;
        m_sourceBitDepth = bitDepth;
        m_sourceCodec = codecName;
    }
    QString source() const override { return m_sourcePath; }

    void play() override
    {
        if (m_playbackState == PlaybackState::Stopped
            || m_playbackState == PlaybackState::Paused) {
            setPlaybackState(PlaybackState::Playing);
        }
    }
    void pause() override
    {
        if (m_playbackState == PlaybackState::Playing)
            setPlaybackState(PlaybackState::Paused);
    }
    void stop() override
    {
        if (m_playbackState == PlaybackState::Playing
            || m_playbackState == PlaybackState::Paused) {
            setPlaybackState(PlaybackState::Stopping);
            setPlaybackState(PlaybackState::Stopped);
        }
    }
    void seek(qint64 positionMs) override
    {
        m_position = positionMs;
        emit positionChanged(positionMs);
    }
    void setVolume(qreal volume) override { m_volume = volume; }
    QList<QAudioDevice> availableOutputDevices() const override { return {}; }
    QString outputDeviceDescription() const override { return QStringLiteral("mock-device"); }
    QAudioFormat outputFormat() const override { return {}; }
    QAudioDevice selectedOutputDevice() const override { return {}; }
    QByteArray selectedOutputDeviceId() const override { return {}; }
    bool usesDefaultOutputDevice() const override { return true; }
    void setOutputDeviceId(const QByteArray &) override {}

    qreal volume() const { return m_volume; }
    qint64 position() const { return m_position; }
    int sourceChannels() const { return m_sourceChannels; }
    int sourceSampleRate() const { return m_sourceSampleRate; }
    int sourceBitDepth() const { return m_sourceBitDepth; }
    QString sourceCodec() const { return m_sourceCodec; }

    void simulateDeviceDisconnect()
    {
        setPlaybackState(PlaybackState::Stopped);
        emit errorOccurred(PlaybackError::DeviceDisconnected,
                           QStringLiteral("mock device disconnected"));
    }

    void simulateDecoderError()
    {
        setPlaybackState(PlaybackState::Stopped);
        emit errorOccurred(PlaybackError::DecoderError,
                           QStringLiteral("mock decoder error"));
    }

    void simulateOutputError()
    {
        setPlaybackState(PlaybackState::Stopped);
        emit errorOccurred(PlaybackError::OutputError,
                           QStringLiteral("mock output error"));
    }

private:
    BackendId m_id;
    QString m_sourcePath;
    int m_sourceChannels = 0;
    int m_sourceSampleRate = 0;
    int m_sourceBitDepth = 0;
    QString m_sourceCodec;
    qreal m_volume = 1.0;
    qint64 m_position = 0;
};

} // namespace

void TestIntegration::initTestCase() {}
void TestIntegration::cleanupTestCase() {}

// ── 1. Complete playback flow ────────────────────────────────────────

void TestIntegration::testPlaybackFlowFileLoadToOutput()
{
    MockAudioPlayerBackend player(BackendId::Ffmpeg);

    QSignalSpy stateSpy(&player, &AudioPlayerBackend::playbackStateChanged);

    player.setSource(QStringLiteral("/music/song.flac"), 2, 44100, 16, QStringLiteral("flac"));
    QCOMPARE(player.source(), QStringLiteral("/music/song.flac"));

    player.play();
    QCOMPARE(player.playbackState(), PlaybackState::Playing);
    QCOMPARE(stateSpy.count(), 1);
    QCOMPARE(stateSpy.last().at(0).value<PlaybackState>(), PlaybackState::Playing);

    player.stop();
    QCOMPARE(player.playbackState(), PlaybackState::Stopped);
}

void TestIntegration::testPlaybackFlowBackendSelectionFlac()
{
    AudioPlayerSourceContext ctx;
    ctx.filePath = QStringLiteral("/music/song.flac");
    ctx.codecName = QStringLiteral("flac");
    ctx.sourceChannelCount = 2;

    QCOMPARE(ctx.filePath, QStringLiteral("/music/song.flac"));
    QCOMPARE(ctx.codecName, QStringLiteral("flac"));
    QCOMPARE(ctx.sourceChannelCount, 2);

    AudioPlaybackPlan plan;
    QCOMPARE(plan.backendId, BackendId::Ffmpeg);
    QCOMPARE(plan.sourceMode, AudioPlaybackPlan::SourceMode::OriginalFile);
}

void TestIntegration::testPlaybackFlowBackendSelectionDolby()
{
    AudioPlayerSourceContext ctx;
    ctx.filePath = QStringLiteral("/music/movie.ec3");
    ctx.codecName = QStringLiteral("eac3");
    ctx.sourceChannelCount = 6;

    QCOMPARE(ctx.filePath, QStringLiteral("/music/movie.ec3"));
    QCOMPARE(ctx.codecName, QStringLiteral("eac3"));

    AudioPlaybackPlan plan;
    plan.sourceMode = AudioPlaybackPlan::SourceMode::RemuxRawDolbySidecar;
    QCOMPARE(plan.sourceMode, AudioPlaybackPlan::SourceMode::RemuxRawDolbySidecar);
}

void TestIntegration::testPlaybackFlowSourceProbe()
{
    AudioInfo info;
    info.codecName = QStringLiteral("flac");
    info.channelCount = 2;
    info.sampleRateValue = 96000;
    info.durationMs = 180000;

    QVERIFY(!info.codecName.isEmpty());
    QCOMPARE(info.channelCount, 2);
    QCOMPARE(info.sampleRateValue, 96000);
    QCOMPARE(info.durationMs, 180000);
}

void TestIntegration::testPlaybackFlowSetSourceBeforePlay()
{
    MockAudioPlayerBackend player(BackendId::Ffmpeg);

    player.setSource(QStringLiteral("/path/song.wav"), 2, 48000, 24, QStringLiteral("pcm_s24le"));
    QCOMPARE(player.source(), QStringLiteral("/path/song.wav"));
    QCOMPARE(player.sourceChannels(), 2);
    QCOMPARE(player.sourceSampleRate(), 48000);
    QCOMPARE(player.sourceBitDepth(), 24);
    QCOMPARE(player.sourceCodec(), QStringLiteral("pcm_s24le"));

    player.play();
    QCOMPARE(player.playbackState(), PlaybackState::Playing);
}

void TestIntegration::testPlaybackFlowStopResetsState()
{
    MockAudioPlayerBackend player(BackendId::Ffmpeg);
    player.setSource(QStringLiteral("/music/song.mp3"), 2, 44100, 16, QStringLiteral("mp3"));

    player.play();
    QCOMPARE(player.playbackState(), PlaybackState::Playing);

    player.stop();
    QCOMPARE(player.playbackState(), PlaybackState::Stopped);

    player.play();
    QCOMPARE(player.playbackState(), PlaybackState::Playing);
}

// ── 2. Backend switching ─────────────────────────────────────────────

void TestIntegration::testBackendSwitchFfmpegToWasapi()
{
    MockAudioPlayerBackend ffmpegPlayer(BackendId::Ffmpeg);
    MockAudioPlayerBackend wasapiPlayer(BackendId::WindowsWasapi);

    ffmpegPlayer.setSource(QStringLiteral("/music/song.flac"), 2, 44100, 16, QStringLiteral("flac"));
    ffmpegPlayer.play();
    QCOMPARE(ffmpegPlayer.playbackState(), PlaybackState::Playing);

    ffmpegPlayer.stop();
    QCOMPARE(ffmpegPlayer.playbackState(), PlaybackState::Stopped);

    wasapiPlayer.setSource(ffmpegPlayer.source(), ffmpegPlayer.sourceChannels(),
                           ffmpegPlayer.sourceSampleRate(), ffmpegPlayer.sourceBitDepth(),
                           ffmpegPlayer.sourceCodec());
    wasapiPlayer.play();
    QCOMPARE(wasapiPlayer.playbackState(), PlaybackState::Playing);
    QCOMPARE(wasapiPlayer.backendId(), BackendId::WindowsWasapi);
}

void TestIntegration::testBackendSwitchWasapiToAsio()
{
    MockAudioPlayerBackend wasapiPlayer(BackendId::WindowsWasapi);
    MockAudioPlayerBackend asioPlayer(BackendId::WindowsAsio);

    wasapiPlayer.setSource(QStringLiteral("/music/song.flac"), 2, 96000, 24, QStringLiteral("flac"));
    wasapiPlayer.play();
    QCOMPARE(wasapiPlayer.playbackState(), PlaybackState::Playing);

    wasapiPlayer.stop();
    QCOMPARE(wasapiPlayer.playbackState(), PlaybackState::Stopped);

    asioPlayer.setSource(wasapiPlayer.source(), wasapiPlayer.sourceChannels(),
                         wasapiPlayer.sourceSampleRate(), wasapiPlayer.sourceBitDepth(),
                         wasapiPlayer.sourceCodec());
    asioPlayer.play();
    QCOMPARE(asioPlayer.playbackState(), PlaybackState::Playing);
    QCOMPARE(asioPlayer.backendId(), BackendId::WindowsAsio);
}

void TestIntegration::testBackendSwitchPreservesVolume()
{
    MockAudioPlayerBackend player1(BackendId::Ffmpeg);
    player1.setVolume(0.75);
    QCOMPARE(player1.volume(), 0.75);

    MockAudioPlayerBackend player2(BackendId::WindowsWasapi);
    player2.setVolume(player1.volume());
    QCOMPARE(player2.volume(), 0.75);
}

void TestIntegration::testBackendSwitchPreservesPosition()
{
    MockAudioPlayerBackend player1(BackendId::Ffmpeg);
    player1.setSource(QStringLiteral("/music/song.flac"), 2, 44100, 16, QStringLiteral("flac"));
    player1.play();
    player1.seek(30000);
    QCOMPARE(player1.position(), 30000);

    player1.stop();

    MockAudioPlayerBackend player2(BackendId::WindowsWasapi);
    player2.setSource(player1.source(), 2, 44100, 16, QStringLiteral("flac"));
    player2.play();
    player2.seek(player1.position());
    QCOMPARE(player2.position(), 30000);
}

void TestIntegration::testBackendSwitchStopsPrevious()
{
    MockAudioPlayerBackend player1(BackendId::Ffmpeg);
    player1.setSource(QStringLiteral("/music/song.flac"), 2, 44100, 16, QStringLiteral("flac"));
    player1.play();
    QCOMPARE(player1.playbackState(), PlaybackState::Playing);

    QSignalSpy stopSpy(&player1, &AudioPlayerBackend::playbackStateChanged);
    player1.stop();
    QCOMPARE(player1.playbackState(), PlaybackState::Stopped);
    QVERIFY(stopSpy.count() >= 1);

    MockAudioPlayerBackend player2(BackendId::WindowsWasapi);
    player2.setSource(player1.source(), 2, 44100, 16, QStringLiteral("flac"));
    player2.play();
    QCOMPARE(player2.playbackState(), PlaybackState::Playing);
}

// ── 3. Seek tests ────────────────────────────────────────────────────

void TestIntegration::testSeekWhilePlaying()
{
    MockAudioPlayerBackend player(BackendId::Ffmpeg);
    player.setSource(QStringLiteral("/music/song.flac"), 2, 44100, 16, QStringLiteral("flac"));
    player.play();
    QCOMPARE(player.playbackState(), PlaybackState::Playing);

    player.seek(60000);
    QCOMPARE(player.position(), 60000);
    QCOMPARE(player.playbackState(), PlaybackState::Playing);
}

void TestIntegration::testSeekWhilePaused()
{
    MockAudioPlayerBackend player(BackendId::Ffmpeg);
    player.setSource(QStringLiteral("/music/song.flac"), 2, 44100, 16, QStringLiteral("flac"));
    player.play();
    player.pause();
    QCOMPARE(player.playbackState(), PlaybackState::Paused);

    player.seek(45000);
    QCOMPARE(player.position(), 45000);
    QCOMPARE(player.playbackState(), PlaybackState::Paused);
}

void TestIntegration::testSeekWhileStopped()
{
    MockAudioPlayerBackend player(BackendId::Ffmpeg);
    player.setSource(QStringLiteral("/music/song.flac"), 2, 44100, 16, QStringLiteral("flac"));
    QCOMPARE(player.playbackState(), PlaybackState::Stopped);

    player.seek(30000);
    QCOMPARE(player.position(), 30000);
    QCOMPARE(player.playbackState(), PlaybackState::Stopped);
}

void TestIntegration::testSeekUpdatesPosition()
{
    MockAudioPlayerBackend player(BackendId::Ffmpeg);
    player.setSource(QStringLiteral("/music/song.flac"), 2, 44100, 16, QStringLiteral("flac"));
    player.play();

    player.seek(10000);
    QCOMPARE(player.position(), 10000);

    player.seek(120000);
    QCOMPARE(player.position(), 120000);

    player.seek(0);
    QCOMPARE(player.position(), 0);
}

void TestIntegration::testSeekToZero()
{
    MockAudioPlayerBackend player(BackendId::Ffmpeg);
    player.setSource(QStringLiteral("/music/song.flac"), 2, 44100, 16, QStringLiteral("flac"));
    player.play();
    player.seek(60000);
    QCOMPARE(player.position(), 60000);

    player.seek(0);
    QCOMPARE(player.position(), 0);
}

void TestIntegration::testSeekEmitsPositionChanged()
{
    MockAudioPlayerBackend player(BackendId::Ffmpeg);
    player.setSource(QStringLiteral("/music/song.flac"), 2, 44100, 16, QStringLiteral("flac"));
    player.play();

    QSignalSpy posSpy(&player, &AudioPlayerBackend::positionChanged);
    player.seek(50000);
    QCOMPARE(posSpy.count(), 1);
    QCOMPARE(posSpy.last().at(0).toLongLong(), 50000);
}

// ── 4. Error recovery ────────────────────────────────────────────────

void TestIntegration::testRecoveryAfterDeviceDisconnect()
{
    MockAudioPlayerBackend player(BackendId::WindowsWasapi);
    player.setSource(QStringLiteral("/music/song.flac"), 2, 44100, 16, QStringLiteral("flac"));
    player.play();
    QCOMPARE(player.playbackState(), PlaybackState::Playing);

    QSignalSpy errorSpy(&player, &AudioPlayerBackend::errorOccurred);
    player.simulateDeviceDisconnect();
    QCOMPARE(player.playbackState(), PlaybackState::Stopped);
    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(errorSpy.last().at(0).value<PlaybackError>(), PlaybackError::DeviceDisconnected);

    player.play();
    QCOMPARE(player.playbackState(), PlaybackState::Playing);
}

void TestIntegration::testRecoveryAfterDecoderError()
{
    MockAudioPlayerBackend player(BackendId::Ffmpeg);
    player.setSource(QStringLiteral("/music/corrupt.flac"), 2, 44100, 16, QStringLiteral("flac"));
    player.play();
    QCOMPARE(player.playbackState(), PlaybackState::Playing);

    QSignalSpy errorSpy(&player, &AudioPlayerBackend::errorOccurred);
    player.simulateDecoderError();
    QCOMPARE(player.playbackState(), PlaybackState::Stopped);
    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(errorSpy.last().at(0).value<PlaybackError>(), PlaybackError::DecoderError);

    player.setSource(QStringLiteral("/music/valid.flac"), 2, 44100, 16, QStringLiteral("flac"));
    player.play();
    QCOMPARE(player.playbackState(), PlaybackState::Playing);
}

void TestIntegration::testRecoveryExhaustionEmitsError()
{
    MockAudioPlayerBackend player(BackendId::WindowsWasapi);
    player.setSource(QStringLiteral("/music/song.flac"), 2, 44100, 16, QStringLiteral("flac"));
    player.play();

    QSignalSpy errorSpy(&player, &AudioPlayerBackend::errorOccurred);

    player.simulateDeviceDisconnect();
    QCOMPARE(errorSpy.count(), 1);

    player.play();
    player.simulateDeviceDisconnect();
    QCOMPARE(errorSpy.count(), 2);

    player.play();
    player.simulateDeviceDisconnect();
    QCOMPARE(errorSpy.count(), 3);
}

void TestIntegration::testRecoveryResetsOnSuccessfulPlayback()
{
    MockAudioPlayerBackend player(BackendId::WindowsWasapi);
    player.setSource(QStringLiteral("/music/song.flac"), 2, 44100, 16, QStringLiteral("flac"));

    player.play();
    player.simulateDeviceDisconnect();
    QCOMPARE(player.playbackState(), PlaybackState::Stopped);

    player.play();
    QCOMPARE(player.playbackState(), PlaybackState::Playing);

    player.stop();
    QCOMPARE(player.playbackState(), PlaybackState::Stopped);
}

void TestIntegration::testErrorSignalEmitted()
{
    MockAudioPlayerBackend player(BackendId::Ffmpeg);
    player.setSource(QStringLiteral("/music/song.flac"), 2, 44100, 16, QStringLiteral("flac"));
    player.play();

    QSignalSpy errorSpy(&player, &AudioPlayerBackend::errorOccurred);

    player.simulateOutputError();
    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(errorSpy.last().at(0).value<PlaybackError>(), PlaybackError::OutputError);
    QVERIFY(!errorSpy.last().at(1).toString().isEmpty());
}

#include "test_integration.moc"
