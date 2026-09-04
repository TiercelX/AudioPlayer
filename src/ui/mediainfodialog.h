#ifndef MEDIAINFODIALOG_H
#define MEDIAINFODIALOG_H

#include <QDialog>
#include <QString>

class QLabel;

struct AudioInfo
{
    QString codecName;
    QString audioFormat;
    QString channels;
    QString sampleRate;
    QString bitDepth;
    QString bitRate;
    QString status;
    int channelCount = 0;
    int sampleRateValue = 0;
    int bitDepthValue = 0;
    qint64 durationMs = 0;
    qint64 bitRateValue = 0;
    bool atmosKnown = false;
    bool atmosDetected = false;
};

class MediaInfoDialog : public QDialog
{
    Q_OBJECT

public:
    explicit MediaInfoDialog(QWidget *parent = nullptr);

    void setBackendName(const QString &backendName);
    void setDecoderName(const QString &decoderName);
    void setOutputAudioMode(const QString &mode);
    void setOutputInfo(const AudioInfo &info);
    void setSourceInfo(const AudioInfo &info);

private:
    void updateSection(QLabel *formatValue,
                       QLabel *channelsValue,
                       QLabel *sampleRateValue,
                       QLabel *bitDepthValue,
                       QLabel *bitRateValue,
                       QLabel *statusValue,
                       const AudioInfo &info);

    QLabel *m_backendValue;
    QLabel *m_decoderValue;
    QLabel *m_outputAudioModeValue;
    QLabel *m_outputFormatValue;
    QLabel *m_outputChannelsValue;
    QLabel *m_outputSampleRateValue;
    QLabel *m_outputBitDepthValue;
    QLabel *m_outputBitRateValue;
    QLabel *m_outputStatusValue;
    QLabel *m_sourceFormatValue;
    QLabel *m_sourceChannelsValue;
    QLabel *m_sourceSampleRateValue;
    QLabel *m_sourceBitDepthValue;
    QLabel *m_sourceBitRateValue;
    QLabel *m_sourceStatusValue;
};

#endif // MEDIAINFODIALOG_H
