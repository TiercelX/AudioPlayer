#include "test_volumecontrol.h"
#include "audioplayerbackend.h"

#include <QTest>

namespace {

class MockAudioPlayerBackend : public AudioPlayerBackend
{
    Q_OBJECT

public:
    explicit MockAudioPlayerBackend(QObject *parent = nullptr)
        : AudioPlayerBackend(parent)
    {
    }

    BackendId backendId() const override { return BackendId::Ffmpeg; }
    QString backendName() const override { return QStringLiteral("mock"); }
    QString decoderName() const override { return QStringLiteral("mock-decoder"); }
    void setSource(const QString &, int, int, int, const QString &) override {}
    QString source() const override { return {}; }
    void play() override {}
    void pause() override {}
    void stop() override {}
    void seek(qint64) override {}
    void setVolume(qreal volume) override { m_storedVolume = volume; }
    QList<QAudioDevice> availableOutputDevices() const override { return {}; }
    QString outputDeviceDescription() const override { return {}; }
    QAudioFormat outputFormat() const override { return {}; }
    QAudioDevice selectedOutputDevice() const override { return {}; }
    QByteArray selectedOutputDeviceId() const override { return {}; }
    bool usesDefaultOutputDevice() const override { return true; }
    void setOutputDeviceId(const QByteArray &) override {}

    qreal storedVolume() const { return m_storedVolume; }

private:
    qreal m_storedVolume = 1.0;
};

} // namespace

void TestVolumeControl::initTestCase() {}
void TestVolumeControl::cleanupTestCase() {}

void TestVolumeControl::testDefaultVolume()
{
    MockAudioPlayerBackend player;
    QCOMPARE(player.storedVolume(), 1.0);
}

void TestVolumeControl::testSetVolumeNormalRange()
{
    MockAudioPlayerBackend player;

    player.setVolume(0.0);
    QCOMPARE(player.storedVolume(), 0.0);

    player.setVolume(0.5);
    QCOMPARE(player.storedVolume(), 0.5);

    player.setVolume(1.0);
    QCOMPARE(player.storedVolume(), 1.0);

    player.setVolume(0.25);
    QCOMPARE(player.storedVolume(), 0.25);

    player.setVolume(0.75);
    QCOMPARE(player.storedVolume(), 0.75);
}

void TestVolumeControl::testSetVolumeBoundaryNegative()
{
    MockAudioPlayerBackend player;

    player.setVolume(-0.1);
    QCOMPARE(player.storedVolume(), -0.1);

    player.setVolume(-1.0);
    QCOMPARE(player.storedVolume(), -1.0);
}

void TestVolumeControl::testSetVolumeBoundaryExceedsOne()
{
    MockAudioPlayerBackend player;

    player.setVolume(1.1);
    QCOMPARE(player.storedVolume(), 1.1);

    player.setVolume(2.0);
    QCOMPARE(player.storedVolume(), 2.0);
}

void TestVolumeControl::testSetVolumeMute()
{
    MockAudioPlayerBackend player;

    player.setVolume(0.0);
    QCOMPARE(player.storedVolume(), 0.0);
    QVERIFY(qFuzzyIsNull(player.storedVolume()));
}

void TestVolumeControl::testSetVolumeMultipleUpdates()
{
    MockAudioPlayerBackend player;

    player.setVolume(0.0);
    QCOMPARE(player.storedVolume(), 0.0);

    player.setVolume(0.3);
    QCOMPARE(player.storedVolume(), 0.3);

    player.setVolume(0.7);
    QCOMPARE(player.storedVolume(), 0.7);

    player.setVolume(1.0);
    QCOMPARE(player.storedVolume(), 1.0);

    player.setVolume(0.0);
    QCOMPARE(player.storedVolume(), 0.0);
}

#include "test_volumecontrol.moc"
