#include "alsalogic.h"
#include <QFileInfo>

namespace AlsaLogic {

QString rawInputFormatForPath(const QString &sourcePath)
{
    const QString suffix = QFileInfo(sourcePath).suffix().toLower();
    if (suffix == QStringLiteral("mlp")
        || suffix == QStringLiteral("thd")
        || suffix == QStringLiteral("truehd")) {
        return QStringLiteral("truehd");
    }
    if (suffix == QStringLiteral("eb3") || suffix == QStringLiteral("ec3")) {
        return QStringLiteral("eac3");
    }
    return {};
}

qsizetype startupThresholdBytes(const PcmStreamFormat &decoderPcmFormat, int profile)
{
    if (!decoderPcmFormat.isValid()) {
        return 32768;
    }
    const int thresholdMs = (profile == 1) // SeekResume
        ? 100
        : 200;
    return qMax<qsizetype>(32768,
                           static_cast<qsizetype>(decoderPcmFormat.bytesPerFrame())
                               * decoderPcmFormat.sampleRate
                               * thresholdMs / 1000);
}

} // namespace AlsaLogic
