#ifndef NATIVEAUDIOPLAYERSTUBBASE_H
#define NATIVEAUDIOPLAYERSTUBBASE_H

#include "audioplayerbackend.h"

class QMediaDevices;

class NativeAudioPlayerStubBase : public AudioPlayerBackend
{
    Q_OBJECT

public:
    explicit NativeAudioPlayerStubBase(QObject *parent = nullptr);
    ~NativeAudioPlayerStubBase() override;

    void setSource(const QString &filePath,
                   int sourceChannelCount,
                   int sourceSampleRate,
                   int sourceBitDepth,
                   const QString &sourceCodecName) override;
    QString source() const override;
    QString decoderName() const override;

    void play() override;
    void pause() override;
    void stop() override;
    void seek(qint64 positionMs) override;
    void setVolume(qreal volume) override;

    QList<QAudioDevice> availableOutputDevices() const override;
    QString outputDeviceDescription() const override;
    QAudioFormat outputFormat() const override;
    QAudioDevice selectedOutputDevice() const override;
    QByteArray selectedOutputDeviceId() const override;
    bool usesDefaultOutputDevice() const override;
    void setOutputDeviceId(const QByteArray &deviceId) override;

private:
    QString currentOutputDeviceDescription() const;
    void emitCurrentOutputFormat();
    void handleAudioOutputsChanged();
    void reportUnavailableOperation(const QString &operation);
    QAudioDevice resolveOutputDevice(bool *usesDefault = nullptr) const;

    QString m_sourcePath;
    QString m_sourceCodecName;
    qreal m_volume = 1.0;
    int m_sourceChannelCount = 0;
    int m_sourceSampleRate = 0;
    int m_sourceBitDepth = 0;
    QByteArray m_selectedOutputDeviceId;
    QMediaDevices *m_mediaDevices = nullptr;
};

#endif // NATIVEAUDIOPLAYERSTUBBASE_H
