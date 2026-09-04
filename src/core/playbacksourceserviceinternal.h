#ifndef PLAYBACKSOURCESERVICEINTERNAL_H
#define PLAYBACKSOURCESERVICEINTERNAL_H

#include "playbacksourceservice.h"

#include <QProcess>
#include <QStringList>

namespace PlaybackSourceServiceInternal {

QString rawInputFormatForFile(const QString &filePath);

struct ToolProcessResult
{
    bool started = false;
    bool finished = false;
    bool timedOut = false;
    int exitCode = -1;
    QProcess::ExitStatus exitStatus = QProcess::NormalExit;
    QString standardOutput;
    QString standardError;
    QString errorText;
};

ToolProcessResult runToolProcess(const QString &program,
                                 const QStringList &arguments,
                                 int startTimeoutMs,
                                 int finishTimeoutMs);

bool hasCoreSourceInfo(const AudioInfo &info);
bool needsSupplementalSourceInfo(const AudioInfo &info);
void mergeMissingSourceInfo(AudioInfo *target, const AudioInfo &fallback);
QString formatBitRate(const QString &bitsPerSecond);
QString formatBitDepth(int bitDepth);
QString formatChannelDescription(int channelCount, const QString &layout);
QString formatCodecDisplay(const QString &codecName, const QString &codecLongName);
QString formatSampleRate(int sampleRate);

} // namespace PlaybackSourceServiceInternal

#endif // PLAYBACKSOURCESERVICEINTERNAL_H
