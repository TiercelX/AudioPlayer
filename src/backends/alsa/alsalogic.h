#ifndef ALSALOGIC_H
#define ALSALOGIC_H

#include "ffmpegpcmshared.h"
#include <QString>

namespace AlsaLogic {

// Maps file extension to raw Dolby input format (truehd/eac3/empty)
QString rawInputFormatForPath(const QString &sourcePath);

// Calculates minimum buffer bytes before output starts
// profile: 0=NormalStart, 1=SeekResume
qsizetype startupThresholdBytes(const PcmStreamFormat &decoderPcmFormat, int profile);

} // namespace AlsaLogic

#endif // ALSALOGIC_H
