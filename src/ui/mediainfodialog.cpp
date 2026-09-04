#include "mediainfodialog.h"

#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QVBoxLayout>

namespace {
QLabel *createValueLabel()
{
    auto *label = new QLabel(QObject::tr("未知"));
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}
}

MediaInfoDialog::MediaInfoDialog(QWidget *parent)
    : QDialog(parent)
    , m_backendValue(createValueLabel())
    , m_decoderValue(createValueLabel())
    , m_outputAudioModeValue(createValueLabel())
    , m_outputFormatValue(createValueLabel())
    , m_outputChannelsValue(createValueLabel())
    , m_outputSampleRateValue(createValueLabel())
    , m_outputBitDepthValue(createValueLabel())
    , m_outputBitRateValue(createValueLabel())
    , m_outputStatusValue(createValueLabel())
    , m_sourceFormatValue(createValueLabel())
    , m_sourceChannelsValue(createValueLabel())
    , m_sourceSampleRateValue(createValueLabel())
    , m_sourceBitDepthValue(createValueLabel())
    , m_sourceBitRateValue(createValueLabel())
    , m_sourceStatusValue(createValueLabel())
{
    setWindowTitle(tr("媒体信息"));
    resize(460, 320);

    auto *mainLayout = new QVBoxLayout(this);

    auto *backendGroup = new QGroupBox(tr("后端"), this);
    auto *backendLayout = new QFormLayout(backendGroup);
    backendLayout->addRow(tr("当前后端"), m_backendValue);
    backendLayout->addRow(tr("当前解码器"), m_decoderValue);
    mainLayout->addWidget(backendGroup);

    auto *outputGroup = new QGroupBox(tr("当前输出"), this);
    auto *outputLayout = new QFormLayout(outputGroup);
    outputLayout->addRow(tr("音频格式"), m_outputFormatValue);
    outputLayout->addRow(tr("声道"), m_outputChannelsValue);
    outputLayout->addRow(tr("采样率"), m_outputSampleRateValue);
    outputLayout->addRow(tr("位深"), m_outputBitDepthValue);
    outputLayout->addRow(tr("比特率"), m_outputBitRateValue);
    outputLayout->addRow(tr("输出模式"), m_outputAudioModeValue);
    outputLayout->addRow(tr("输出设备"), m_outputStatusValue);
    mainLayout->addWidget(outputGroup);

    auto *sourceGroup = new QGroupBox(tr("源文件"), this);
    auto *sourceLayout = new QFormLayout(sourceGroup);
    sourceLayout->addRow(tr("音频格式"), m_sourceFormatValue);
    sourceLayout->addRow(tr("声道"), m_sourceChannelsValue);
    sourceLayout->addRow(tr("采样率"), m_sourceSampleRateValue);
    sourceLayout->addRow(tr("位深"), m_sourceBitDepthValue);
    sourceLayout->addRow(tr("比特率"), m_sourceBitRateValue);
    sourceLayout->addRow(tr("Atmos 状态"), m_sourceStatusValue);
    mainLayout->addWidget(sourceGroup);
}

void MediaInfoDialog::setBackendName(const QString &backendName)
{
    m_backendValue->setText(backendName.isEmpty() ? tr("未知") : backendName);
}

void MediaInfoDialog::setDecoderName(const QString &decoderName)
{
    m_decoderValue->setText(decoderName.isEmpty() ? tr("未知") : decoderName);
}

void MediaInfoDialog::setOutputAudioMode(const QString &mode)
{
    m_outputAudioModeValue->setText(mode.isEmpty() ? tr("未知") : mode);
}

void MediaInfoDialog::setOutputInfo(const AudioInfo &info)
{
    updateSection(m_outputFormatValue,
                  m_outputChannelsValue,
                  m_outputSampleRateValue,
                  m_outputBitDepthValue,
                  m_outputBitRateValue,
                  m_outputStatusValue,
                  info);
}

void MediaInfoDialog::setSourceInfo(const AudioInfo &info)
{
    updateSection(m_sourceFormatValue,
                  m_sourceChannelsValue,
                  m_sourceSampleRateValue,
                  m_sourceBitDepthValue,
                  m_sourceBitRateValue,
                  m_sourceStatusValue,
                  info);
}

void MediaInfoDialog::updateSection(QLabel *formatValue,
                                    QLabel *channelsValue,
                                    QLabel *sampleRateValue,
                                    QLabel *bitDepthValue,
                                    QLabel *bitRateValue,
                                    QLabel *statusValue,
                                    const AudioInfo &info)
{
    formatValue->setText(info.audioFormat.isEmpty() ? tr("未知") : info.audioFormat);
    channelsValue->setText(info.channels.isEmpty() ? tr("未知") : info.channels);
    sampleRateValue->setText(info.sampleRate.isEmpty() ? tr("未知") : info.sampleRate);
    bitDepthValue->setText(info.bitDepth.isEmpty() ? tr("未知") : info.bitDepth);
    bitRateValue->setText(info.bitRate.isEmpty() ? tr("未知") : info.bitRate);
    statusValue->setText(info.status.isEmpty() ? tr("未知") : info.status);
}
