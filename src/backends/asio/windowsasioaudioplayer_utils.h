#ifndef WINDOWSASIOAUDIOPLAYER_UTILS_H
#define WINDOWSASIOAUDIOPLAYER_UTILS_H

#include "asio_interface.h"
#include "audioplayerbackend.h"

#include <QString>

#include <windows.h>

namespace AsioUtils {

constexpr long kAsioOk = 0;
constexpr long kAsioSuccess = 0x3f4847a0;
constexpr long kAsioNotPresent = -1000;

enum AsioSampleType : long {
    ASIOSTInt16LSB = 16,
    ASIOSTInt24LSB = 17,
    ASIOSTInt32LSB = 18,
    ASIOSTFloat32LSB = 19,
    ASIOSTInt32LSB16 = 24,
    ASIOSTInt32LSB18 = 25,
    ASIOSTInt32LSB20 = 26,
    ASIOSTInt32LSB24 = 27,
};

int sampleTypeBytes(AsioSampleType sampleType);

int boundedEnvInt(const QString &name, int defaultValue, int minValue, int maxValue);

QString hwndText(HWND window);

QString asioDriverError(IASIO *driver);
bool asioResultOk(long result);

long safeAsioStart(IASIO *driver, bool *crashed);
long safeAsioStop(IASIO *driver, bool *crashed);
long safeAsioCanSampleRate(IASIO *driver, ASIOSampleRate sampleRate, bool *crashed);
long safeAsioSetSampleRate(IASIO *driver, ASIOSampleRate sampleRate, bool *crashed);
long safeAsioGetSampleRate(IASIO *driver, ASIOSampleRate *sampleRate, bool *crashed);
long safeAsioGetChannels(IASIO *driver, long *inputChannels, long *outputChannels, bool *crashed);
long safeAsioGetLatencies(IASIO *driver, long *inputLatency, long *outputLatency, bool *crashed);
long safeAsioGetBufferSize(IASIO *driver,
                          long *minSize,
                          long *maxSize,
                          long *preferredSize,
                          long *granularity,
                          bool *crashed);
long safeAsioGetChannelInfo(IASIO *driver, ASIOChannelInfo *info, bool *crashed);
long safeAsioCreateBuffers(IASIO *driver,
                           ASIOBufferInfo *bufferInfos,
                           long channelCount,
                           long bufferSize,
                           ASIOCallbacks *callbacks,
                           bool *crashed);
long safeAsioDisposeBuffers(IASIO *driver, bool *crashed);
long safeAsioOutputReady(IASIO *driver, bool *crashed);

} // namespace AsioUtils

#endif // WINDOWSASIOAUDIOPLAYER_UTILS_H
