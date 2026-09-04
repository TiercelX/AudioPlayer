#include "test_wasapi_states.h"

#include "audioplayerbackend.h"

#include <QTest>
#include <QSignalSpy>

using PlaybackState = AudioPlayerBackend::PlaybackState;
using PlaybackError = AudioPlayerBackend::PlaybackError;

namespace {

class MockAudioPlayerBackend : public AudioPlayerBackend
{
    Q_OBJECT

public:
    explicit MockAudioPlayerBackend(QObject *parent = nullptr)
        : AudioPlayerBackend(parent)
    {
    }

    BackendId backendId() const override { return BackendId::WindowsWasapi; }
    QString backendName() const override { return QStringLiteral("mock-wasapi"); }
    QString decoderName() const override { return QStringLiteral("mock-decoder"); }
    void setSource(const QString &, int, int, int, const QString &) override {}
    QString source() const override { return {}; }
    void play() override
    {
        if (m_playbackState == PlaybackState::Stopped)
            setPlaybackState(PlaybackState::Playing);
        else if (m_playbackState == PlaybackState::Paused)
            setPlaybackState(PlaybackState::Playing);
    }
    void pause() override
    {
        if (m_playbackState == PlaybackState::Playing)
            setPlaybackState(PlaybackState::Paused);
    }
    void stop() override
    {
        if (m_playbackState == PlaybackState::Playing)
            setPlaybackState(PlaybackState::Stopping);
        else if (m_playbackState == PlaybackState::Paused)
            setPlaybackState(PlaybackState::Stopping);
    }
    void seek(qint64) override {}
    void setVolume(qreal) override {}
    QList<QAudioDevice> availableOutputDevices() const override { return {}; }
    QString outputDeviceDescription() const override { return {}; }
    QAudioFormat outputFormat() const override { return {}; }
    QAudioDevice selectedOutputDevice() const override { return {}; }
    QByteArray selectedOutputDeviceId() const override { return {}; }
    bool usesDefaultOutputDevice() const override { return true; }
    void setOutputDeviceId(const QByteArray &) override {}

    void simulateDecoderFinished()
    {
        if (m_playbackState == PlaybackState::Stopping)
            setPlaybackState(PlaybackState::Stopped);
    }

    void simulateError()
    {
        setPlaybackState(PlaybackState::Stopped);
    }
};

} // namespace

void TestWasapiStates::initTestCase() {}

void TestWasapiStates::cleanupTestCase() {}

// ── PlaybackState transitions via MockAudioPlayerBackend ────────────

void TestWasapiStates::testStoppedToPlaying()
{
    MockAudioPlayerBackend player;
    QCOMPARE(player.playbackState(), PlaybackState::Stopped);

    QSignalSpy spy(&player, &AudioPlayerBackend::playbackStateChanged);
    player.play();
    QCOMPARE(player.playbackState(), PlaybackState::Playing);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).value<PlaybackState>(), PlaybackState::Playing);
}

void TestWasapiStates::testPlayingToPaused()
{
    MockAudioPlayerBackend player;
    player.play();
    QCOMPARE(player.playbackState(), PlaybackState::Playing);

    QSignalSpy spy(&player, &AudioPlayerBackend::playbackStateChanged);
    player.pause();
    QCOMPARE(player.playbackState(), PlaybackState::Paused);
    QCOMPARE(spy.count(), 1);
}

void TestWasapiStates::testPlayingToStopping()
{
    MockAudioPlayerBackend player;
    player.play();
    QCOMPARE(player.playbackState(), PlaybackState::Playing);

    QSignalSpy spy(&player, &AudioPlayerBackend::playbackStateChanged);
    player.stop();
    QCOMPARE(player.playbackState(), PlaybackState::Stopping);
    QCOMPARE(spy.count(), 1);
}

void TestWasapiStates::testPausedToPlaying()
{
    MockAudioPlayerBackend player;
    player.play();
    player.pause();
    QCOMPARE(player.playbackState(), PlaybackState::Paused);

    QSignalSpy spy(&player, &AudioPlayerBackend::playbackStateChanged);
    player.play();
    QCOMPARE(player.playbackState(), PlaybackState::Playing);
    QCOMPARE(spy.count(), 1);
}

void TestWasapiStates::testPausedToStopping()
{
    MockAudioPlayerBackend player;
    player.play();
    player.pause();
    QCOMPARE(player.playbackState(), PlaybackState::Paused);

    QSignalSpy spy(&player, &AudioPlayerBackend::playbackStateChanged);
    player.stop();
    QCOMPARE(player.playbackState(), PlaybackState::Stopping);
    QCOMPARE(spy.count(), 1);
}

void TestWasapiStates::testStoppingToStopped()
{
    MockAudioPlayerBackend player;
    player.play();
    player.stop();
    QCOMPARE(player.playbackState(), PlaybackState::Stopping);

    QSignalSpy spy(&player, &AudioPlayerBackend::playbackStateChanged);
    player.simulateDecoderFinished();
    QCOMPARE(player.playbackState(), PlaybackState::Stopped);
    QCOMPARE(spy.count(), 1);
}

void TestWasapiStates::testPlayingToStoppedOnError()
{
    MockAudioPlayerBackend player;
    player.play();
    QCOMPARE(player.playbackState(), PlaybackState::Playing);

    QSignalSpy spy(&player, &AudioPlayerBackend::playbackStateChanged);
    player.simulateError();
    QCOMPARE(player.playbackState(), PlaybackState::Stopped);
    QCOMPARE(spy.count(), 1);
}

#include "test_wasapi_states.moc"
