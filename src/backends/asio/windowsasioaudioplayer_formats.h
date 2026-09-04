#ifndef WINDOWSASIOAUDIOPLAYER_FORMATS_H
#define WINDOWSASIOAUDIOPLAYER_FORMATS_H

#include <QAudioFormat>
#include <QList>
#include <QString>

struct PcmStreamFormat;

namespace AsioFormats {

QString pcmCodecName(QAudioFormat::SampleFormat sampleFormat);
QString pcmSampleFormatName(QAudioFormat::SampleFormat sampleFormat);
QString pcmMuxerName(QAudioFormat::SampleFormat sampleFormat);
PcmStreamFormat pcmStreamFormatFromQAudioFormat(const QAudioFormat &format);

void appendUniqueSampleRate(QList<int> *rates, int sampleRate);
QList<int> sourcePreferredSampleRateCandidates(int requestedSampleRate, int fallbackSampleRate = 48000);

qreal sampleMagnitude(const char *sampleData, QAudioFormat::SampleFormat sampleFormat);

} // namespace AsioFormats

#endif // WINDOWSASIOAUDIOPLAYER_FORMATS_H
