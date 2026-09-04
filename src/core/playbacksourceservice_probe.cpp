#include "playbacksourceserviceinternal.h"

#include "playerlogger.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace {

qint64 durationMsFromFfmpegText(const QString &text)
{
    static const QRegularExpression durationExpression(
        QStringLiteral(R"(Duration:\s*(\d+):(\d+):(\d+(?:\.\d+)?))"));

    const QRegularExpressionMatch match = durationExpression.match(text);
    if (!match.hasMatch()) {
        return 0;
    }

    const int hours = match.captured(1).toInt();
    const int minutes = match.captured(2).toInt();
    const double seconds = match.captured(3).toDouble();
    if (hours < 0 || minutes < 0 || seconds <= 0.0) {
        return 0;
    }

    return qRound64(((hours * 3600.0) + (minutes * 60.0) + seconds) * 1000.0);
}

QString firstInputAudioStreamLine(const QString &text)
{
    const QStringList lines = text.split(QChar(u'\n'));
    for (const QString &line : lines) {
        const QString trimmed = line.trimmed();
        if (trimmed.startsWith(QStringLiteral("Stream mapping:"))) {
            break;
        }
        if (trimmed.contains(QStringLiteral("Audio:"))) {
            return trimmed;
        }
    }

    return {};
}

int channelCountFromLayoutToken(const QString &layoutToken)
{
    const QString normalized = layoutToken.trimmed().toLower();
    if (normalized.isEmpty()) {
        return 0;
    }
    if (normalized == QStringLiteral("mono")) {
        return 1;
    }
    if (normalized == QStringLiteral("stereo")) {
        return 2;
    }
    if (normalized.contains(QStringLiteral("5.1"))) {
        return 6;
    }
    if (normalized.contains(QStringLiteral("7.1"))) {
        return 8;
    }

    static const QRegularExpression channelsExpression(QStringLiteral(R"((\d+)\s*channels?)"));
    const QRegularExpressionMatch match = channelsExpression.match(normalized);
    return match.hasMatch() ? match.captured(1).toInt() : 0;
}

int bitDepthFromSampleFormatToken(const QString &sampleFormatToken, const QString &codecName)
{
    const QString normalizedSampleFormat = sampleFormatToken.trimmed().toLower();
    if (normalizedSampleFormat.startsWith(QStringLiteral("u8"))) {
        return 8;
    }
    if (normalizedSampleFormat.startsWith(QStringLiteral("s16"))) {
        return 16;
    }
    if (normalizedSampleFormat.startsWith(QStringLiteral("s24"))) {
        return 24;
    }
    if (normalizedSampleFormat.startsWith(QStringLiteral("s32"))) {
        return 32;
    }
    if (normalizedSampleFormat.startsWith(QStringLiteral("s64"))) {
        return 64;
    }

    const QString normalizedCodec = codecName.trimmed().toLower();
    if (normalizedCodec.startsWith(QStringLiteral("pcm_u8"))) {
        return 8;
    }
    if (normalizedCodec.startsWith(QStringLiteral("pcm_s16"))) {
        return 16;
    }
    if (normalizedCodec.startsWith(QStringLiteral("pcm_s24"))) {
        return 24;
    }
    if (normalizedCodec.startsWith(QStringLiteral("pcm_s32"))) {
        return 32;
    }
    if (normalizedCodec.startsWith(QStringLiteral("pcm_s64"))) {
        return 64;
    }

    return 0;
}

bool populateAudioInfoFromFfmpegText(const QString &text, AudioInfo *info)
{
    if (!info) {
        return false;
    }

    const QString audioLine = firstInputAudioStreamLine(text);
    if (audioLine.isEmpty()) {
        return false;
    }

    static const QRegularExpression codecExpression(QStringLiteral(R"(Audio:\s*([^\s,(]+))"));
    static const QRegularExpression sampleRateExpression(QStringLiteral(R"((\d+)\s*Hz)"));
    static const QRegularExpression bitRateExpression(QStringLiteral(R"((\d+)\s*kb/s)"));

    const QRegularExpressionMatch codecMatch = codecExpression.match(audioLine);
    if (!codecMatch.hasMatch()) {
        return false;
    }

    const QString codecName = codecMatch.captured(1).trimmed().toLower();
    const QRegularExpressionMatch sampleRateMatch = sampleRateExpression.match(audioLine);
    const int sampleRateValue = sampleRateMatch.hasMatch() ? sampleRateMatch.captured(1).toInt() : 0;

    const qsizetype sampleRateMarker = audioLine.indexOf(QStringLiteral("Hz,"));
    QString channelLayout;
    QString sampleFormatToken;
    if (sampleRateMarker >= 0) {
        const QStringList tokens = audioLine.mid(sampleRateMarker + 3).split(',', Qt::SkipEmptyParts);
        if (tokens.size() >= 1) {
            channelLayout = tokens.at(0).trimmed();
        }
        if (tokens.size() >= 2) {
            sampleFormatToken = tokens.at(1).trimmed();
        }
    }

    const QRegularExpressionMatch audioBitRateMatch = bitRateExpression.match(audioLine);
    const QRegularExpressionMatch fallbackBitRateMatch = bitRateExpression.match(text);
    const QString bitRateKbps = audioBitRateMatch.hasMatch()
        ? audioBitRateMatch.captured(1)
        : (fallbackBitRateMatch.hasMatch() ? fallbackBitRateMatch.captured(1) : QString());

    info->codecName = codecName;
    info->audioFormat = PlaybackSourceServiceInternal::formatCodecDisplay(codecName, {});
    info->channelCount = channelCountFromLayoutToken(channelLayout);
    info->channels = PlaybackSourceServiceInternal::formatChannelDescription(info->channelCount,
                                                                             channelLayout);
    info->sampleRateValue = sampleRateValue;
    info->sampleRate = PlaybackSourceServiceInternal::formatSampleRate(sampleRateValue);
    info->bitDepthValue = bitDepthFromSampleFormatToken(sampleFormatToken, codecName);
    info->bitDepth = PlaybackSourceServiceInternal::formatBitDepth(info->bitDepthValue);
    info->bitRateValue = bitRateKbps.isEmpty() ? 0 : bitRateKbps.toLongLong() * 1000;
    info->bitRate = bitRateKbps.isEmpty()
        ? QString()
        : PlaybackSourceServiceInternal::formatBitRate(QString::number(info->bitRateValue));
    info->durationMs = durationMsFromFfmpegText(text);
    info->atmosDetected = text.contains(QStringLiteral("Atmos"), Qt::CaseInsensitive);
    info->atmosKnown = info->atmosDetected;
    info->status = info->atmosDetected ? QObject::tr("是 Atmos") : QString();
    return PlaybackSourceServiceInternal::hasCoreSourceInfo(*info) || info->durationMs > 0;
}

AudioInfo probeSourceInfoWithFfmpeg(const QString &filePath, const QString &ffmpegExecutable)
{
    AudioInfo info;
    if (ffmpegExecutable.isEmpty()) {
        return info;
    }

    QStringList arguments {
        QStringLiteral("-hide_banner"),
    };
    const QString rawInputFormat = PlaybackSourceServiceInternal::rawInputFormatForFile(filePath);
    if (!rawInputFormat.isEmpty()) {
        arguments << QStringLiteral("-f") << rawInputFormat;
    }
    arguments << QStringLiteral("-i")
              << filePath
              << QStringLiteral("-map")
              << QStringLiteral("0:a:0")
              << QStringLiteral("-frames:a")
              << QStringLiteral("1")
              << QStringLiteral("-f")
              << QStringLiteral("null")
              << QStringLiteral("-");

    const PlaybackSourceServiceInternal::ToolProcessResult probeResult =
        PlaybackSourceServiceInternal::runToolProcess(ffmpegExecutable, arguments, 3000, 8000);
    if (!probeResult.started) {
        PlayerLogger::log(QStringLiteral("source"),
                          QStringLiteral("probeSourceInfo ffmpeg-start-failed path=%1 ffmpeg=%2 error=%3 stderr=%4")
                              .arg(filePath)
                              .arg(ffmpegExecutable)
                              .arg(probeResult.errorText)
                              .arg(probeResult.standardError));
        return info;
    }

    if (!probeResult.finished || probeResult.exitStatus != QProcess::NormalExit
        || probeResult.exitCode != 0) {
        PlayerLogger::log(QStringLiteral("source"),
                          QStringLiteral("probeSourceInfo ffmpeg-run-failed path=%1 exitCode=%2 exitStatus=%3 error=%4 stderr=%5")
                              .arg(filePath)
                              .arg(probeResult.exitCode)
                              .arg(static_cast<int>(probeResult.exitStatus))
                              .arg(probeResult.errorText)
                              .arg(probeResult.standardError));
        return info;
    }

    if (!populateAudioInfoFromFfmpegText(probeResult.standardError, &info)) {
        PlayerLogger::log(QStringLiteral("source"),
                          QStringLiteral("probeSourceInfo ffmpeg-parse-failed path=%1 bytes=%2")
                              .arg(filePath)
                              .arg(probeResult.standardError.size()));
        return AudioInfo {};
    }

    PlayerLogger::log(QStringLiteral("source"),
                      QStringLiteral("probeSourceInfo ffmpeg-fallback-ok path=%1 codec=%2 channels=%3 durationMs=%4")
                          .arg(filePath)
                          .arg(info.codecName)
                          .arg(info.channelCount)
                          .arg(info.durationMs));
    return info;
}

} // namespace

AudioInfo PlaybackSourceService::probeSourceInfo(const QString &filePath) const
{
    AudioInfo info;
    const QString ffprobeExecutable = locateFfprobeExecutable();

    if (!ffprobeExecutable.isEmpty()) {
        QStringList arguments {
            QStringLiteral("-v"),
            QStringLiteral("error"),
            QStringLiteral("-show_streams"),
            QStringLiteral("-show_format"),
            QStringLiteral("-print_format"),
            QStringLiteral("json"),
        };
        const QString rawInputFormat = PlaybackSourceServiceInternal::rawInputFormatForFile(filePath);
        if (!rawInputFormat.isEmpty()) {
            arguments << QStringLiteral("-f") << rawInputFormat;
        }
        arguments << filePath;

        for (int attempt = 1; attempt <= 2; ++attempt) {
            const PlaybackSourceServiceInternal::ToolProcessResult probeResult =
                PlaybackSourceServiceInternal::runToolProcess(ffprobeExecutable, arguments, 3000, 5000);
            if (!probeResult.started) {
                PlayerLogger::log(QStringLiteral("source"),
                                  QStringLiteral("probeSourceInfo start-failed attempt=%1 path=%2 ffprobe=%3 error=%4 stderr=%5")
                                      .arg(attempt)
                                      .arg(filePath)
                                      .arg(ffprobeExecutable)
                                      .arg(probeResult.errorText)
                                      .arg(probeResult.standardError));
                continue;
            }

            if (!probeResult.finished || probeResult.exitStatus != QProcess::NormalExit
                || probeResult.exitCode != 0) {
                PlayerLogger::log(QStringLiteral("source"),
                                  QStringLiteral("probeSourceInfo run-failed attempt=%1 path=%2 exitCode=%3 exitStatus=%4 error=%5 stderr=%6")
                                      .arg(attempt)
                                      .arg(filePath)
                                      .arg(probeResult.exitCode)
                                      .arg(static_cast<int>(probeResult.exitStatus))
                                      .arg(probeResult.errorText)
                                      .arg(probeResult.standardError));
                continue;
            }

            const QJsonDocument json = QJsonDocument::fromJson(probeResult.standardOutput.toUtf8());
            if (!json.isObject()) {
                PlayerLogger::log(QStringLiteral("source"),
                                  QStringLiteral("probeSourceInfo invalid-json attempt=%1 path=%2 bytes=%3")
                                      .arg(attempt)
                                      .arg(filePath)
                                      .arg(probeResult.standardOutput.size()));
                continue;
            }

            const QJsonObject root = json.object();
            const QJsonArray streams = root.value(QStringLiteral("streams")).toArray();
            QJsonObject audioStream;
            for (const QJsonValue &streamValue : streams) {
                const QJsonObject stream = streamValue.toObject();
                if (stream.value(QStringLiteral("codec_type")).toString() == QStringLiteral("audio")) {
                    audioStream = stream;
                    break;
                }
            }

            if (audioStream.isEmpty()) {
                PlayerLogger::log(QStringLiteral("source"),
                                  QStringLiteral("probeSourceInfo no-audio-stream attempt=%1 path=%2")
                                      .arg(attempt)
                                      .arg(filePath));
                continue;
            }

            const QJsonObject format = root.value(QStringLiteral("format")).toObject();
            const QString codecName = audioStream.value(QStringLiteral("codec_name")).toString();
            const QString codecLongName = audioStream.value(QStringLiteral("codec_long_name")).toString();
            const QString profile = audioStream.value(QStringLiteral("profile")).toString();
            const QString channelLayout = audioStream.value(QStringLiteral("channel_layout")).toString();
            const QString streamBitRate = audioStream.value(QStringLiteral("bit_rate")).toString();
            const QString formatBitRateValue = format.value(QStringLiteral("bit_rate")).toString();
            const double formatDurationSeconds = format.value(QStringLiteral("duration")).toString().toDouble();

            info.codecName = codecName;
            info.audioFormat = PlaybackSourceServiceInternal::formatCodecDisplay(codecName,
                                                                                 codecLongName);
            info.channelCount = audioStream.value(QStringLiteral("channels")).toInt();
            info.channels = PlaybackSourceServiceInternal::formatChannelDescription(info.channelCount,
                                                                                    channelLayout);
            info.sampleRateValue = audioStream.value(QStringLiteral("sample_rate")).toString().toInt();
            info.sampleRate = PlaybackSourceServiceInternal::formatSampleRate(info.sampleRateValue);

            const int bitsPerRawSample =
                audioStream.value(QStringLiteral("bits_per_raw_sample")).toString().toInt();
            const int bitsPerSample = audioStream.value(QStringLiteral("bits_per_sample")).toInt();
            info.bitDepthValue = bitsPerRawSample > 0 ? bitsPerRawSample : bitsPerSample;
            info.bitDepth = PlaybackSourceServiceInternal::formatBitDepth(info.bitDepthValue);

            const QString effectiveBitRate = !streamBitRate.isEmpty() ? streamBitRate : formatBitRateValue;
            info.bitRate = PlaybackSourceServiceInternal::formatBitRate(effectiveBitRate);
            info.bitRateValue = effectiveBitRate.toLongLong();
            if (formatDurationSeconds > 0.0) {
                info.durationMs = static_cast<qint64>(formatDurationSeconds * 1000.0);
            }

            info.atmosKnown = true;
            info.atmosDetected = profile.contains(QStringLiteral("Atmos"), Qt::CaseInsensitive);
            info.status = info.atmosDetected ? QObject::tr("是 Atmos") : QObject::tr("不是 Atmos");
            PlayerLogger::log(QStringLiteral("source"),
                              QStringLiteral("probeSourceInfo ok attempt=%1 path=%2 codec=%3 channels=%4 durationMs=%5")
                                  .arg(attempt)
                                  .arg(filePath)
                                  .arg(info.codecName)
                                  .arg(info.channelCount)
                                  .arg(info.durationMs));
            return info;
        }
    } else {
        PlayerLogger::log(QStringLiteral("source"),
                          QStringLiteral("probeSourceInfo no-ffprobe fallback-ffmpeg path=%1").arg(filePath));
    }

    const AudioInfo ffmpegInfo = probeSourceInfoWithFfmpeg(filePath, locateFfmpegExecutable());
    if (PlaybackSourceServiceInternal::hasCoreSourceInfo(ffmpegInfo) || ffmpegInfo.durationMs > 0) {
        return ffmpegInfo;
    }

    PlayerLogger::log(QStringLiteral("source"),
                      QStringLiteral("probeSourceInfo failed path=%1 ffprobe=%2 ffmpeg=%3")
                          .arg(filePath)
                          .arg(ffprobeExecutable)
                          .arg(locateFfmpegExecutable()));
    return info;
}

qint64 PlaybackSourceService::probeDuration(const QString &filePath) const
{
    const QString ffprobeExecutable = locateFfprobeExecutable();
    if (!ffprobeExecutable.isEmpty()) {
        QStringList arguments {
            QStringLiteral("-v"),
            QStringLiteral("error"),
            QStringLiteral("-show_entries"),
            QStringLiteral("format=duration"),
            QStringLiteral("-of"),
            QStringLiteral("default=noprint_wrappers=1:nokey=1"),
        };
        const QString rawInputFormat = PlaybackSourceServiceInternal::rawInputFormatForFile(filePath);
        if (!rawInputFormat.isEmpty()) {
            arguments << QStringLiteral("-f") << rawInputFormat;
        }
        arguments << filePath;

        for (int attempt = 1; attempt <= 2; ++attempt) {
            const PlaybackSourceServiceInternal::ToolProcessResult probeResult =
                PlaybackSourceServiceInternal::runToolProcess(ffprobeExecutable, arguments, 3000, 5000);
            if (!probeResult.started) {
                PlayerLogger::log(QStringLiteral("source"),
                                  QStringLiteral("probeDuration start-failed attempt=%1 path=%2 error=%3 stderr=%4")
                                      .arg(attempt)
                                      .arg(filePath)
                                      .arg(probeResult.errorText)
                                      .arg(probeResult.standardError));
                continue;
            }

            if (!probeResult.finished || probeResult.exitStatus != QProcess::NormalExit
                || probeResult.exitCode != 0) {
                PlayerLogger::log(QStringLiteral("source"),
                                  QStringLiteral("probeDuration run-failed attempt=%1 path=%2 exitCode=%3 exitStatus=%4 error=%5 stderr=%6")
                                      .arg(attempt)
                                      .arg(filePath)
                                      .arg(probeResult.exitCode)
                                      .arg(static_cast<int>(probeResult.exitStatus))
                                      .arg(probeResult.errorText)
                                      .arg(probeResult.standardError));
                continue;
            }

            bool ok = false;
            const double seconds = probeResult.standardOutput.trimmed().toDouble(&ok);
            if (!ok || seconds <= 0.0) {
                PlayerLogger::log(QStringLiteral("source"),
                                  QStringLiteral("probeDuration invalid-output attempt=%1 path=%2 output=%3")
                                      .arg(attempt)
                                      .arg(filePath)
                                      .arg(probeResult.standardOutput.trimmed()));
                continue;
            }

            const qint64 durationMs = static_cast<qint64>(seconds * 1000.0);
            PlayerLogger::log(QStringLiteral("source"),
                              QStringLiteral("probeDuration ok attempt=%1 path=%2 durationMs=%3")
                                  .arg(attempt)
                                  .arg(filePath)
                                  .arg(durationMs));
            return durationMs;
        }
    } else {
        PlayerLogger::log(QStringLiteral("source"),
                          QStringLiteral("probeDuration no-ffprobe fallback-ffmpeg path=%1").arg(filePath));
    }

    const AudioInfo ffmpegInfo = probeSourceInfoWithFfmpeg(filePath, locateFfmpegExecutable());
    if (ffmpegInfo.durationMs > 0) {
        PlayerLogger::log(QStringLiteral("source"),
                          QStringLiteral("probeDuration ffmpeg-fallback-ok path=%1 durationMs=%2")
                              .arg(filePath)
                              .arg(ffmpegInfo.durationMs));
        return ffmpegInfo.durationMs;
    }

    PlayerLogger::log(QStringLiteral("source"),
                      QStringLiteral("probeDuration failed path=%1 ffprobe=%2 ffmpeg=%3")
                          .arg(filePath)
                          .arg(ffprobeExecutable)
                          .arg(locateFfmpegExecutable()));
    return 0;
}
