#ifndef TOOLLOCATOR_H
#define TOOLLOCATOR_H

#include <QString>

namespace AudioUtils {

QString locateFfmpegExecutable();
QString locateFfprobeExecutable();
QString toolExecutableOverride(const QString &envVarName);

} // namespace AudioUtils

#endif // TOOLLOCATOR_H
