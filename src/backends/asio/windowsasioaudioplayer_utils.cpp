#include "windowsasioaudioplayer_utils.h"

#include "asio_interface.h"

#include <QProcessEnvironment>

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace AsioUtils {

QString hwndText(HWND window)
{
    return QStringLiteral("0x%1").arg(static_cast<qulonglong>(reinterpret_cast<quintptr>(window)),
                                      static_cast<int>(sizeof(void *) * 2),
                                      16,
                                      QLatin1Char('0'));
}

QString asioDriverError(IASIO *driver)
{
    if (!driver) {
        return {};
    }
    char message[128] = {};
    driver->getErrorMessage(message);
    return QString::fromLocal8Bit(message).trimmed();
}

bool asioResultOk(long result)
{
    return result == kAsioOk || result == kAsioSuccess;
}

long safeAsioStart(IASIO *driver, bool *crashed)
{
    if (crashed) {
        *crashed = false;
    }
    __try {
        return driver ? driver->start() : kAsioNotPresent;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (crashed) {
            *crashed = true;
        }
        return kAsioNotPresent;
    }
}

long safeAsioStop(IASIO *driver, bool *crashed)
{
    if (crashed) {
        *crashed = false;
    }
    __try {
        return driver ? driver->stop() : kAsioNotPresent;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (crashed) {
            *crashed = true;
        }
        return kAsioNotPresent;
    }
}

long safeAsioCanSampleRate(IASIO *driver, ASIOSampleRate sampleRate, bool *crashed)
{
    if (crashed) {
        *crashed = false;
    }
    __try {
        return driver ? driver->canSampleRate(sampleRate) : kAsioNotPresent;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (crashed) {
            *crashed = true;
        }
        return kAsioNotPresent;
    }
}

long safeAsioSetSampleRate(IASIO *driver, ASIOSampleRate sampleRate, bool *crashed)
{
    if (crashed) {
        *crashed = false;
    }
    __try {
        return driver ? driver->setSampleRate(sampleRate) : kAsioNotPresent;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (crashed) {
            *crashed = true;
        }
        return kAsioNotPresent;
    }
}

long safeAsioGetSampleRate(IASIO *driver, ASIOSampleRate *sampleRate, bool *crashed)
{
    if (crashed) {
        *crashed = false;
    }
    __try {
        return driver ? driver->getSampleRate(sampleRate) : kAsioNotPresent;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (crashed) {
            *crashed = true;
        }
        return kAsioNotPresent;
    }
}

long safeAsioGetChannels(IASIO *driver, long *inputChannels, long *outputChannels, bool *crashed)
{
    if (crashed) {
        *crashed = false;
    }
    __try {
        return driver ? driver->getChannels(inputChannels, outputChannels) : kAsioNotPresent;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (crashed) {
            *crashed = true;
        }
        return kAsioNotPresent;
    }
}

long safeAsioGetLatencies(IASIO *driver, long *inputLatency, long *outputLatency, bool *crashed)
{
    if (crashed) {
        *crashed = false;
    }
    __try {
        return driver ? driver->getLatencies(inputLatency, outputLatency) : kAsioNotPresent;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (crashed) {
            *crashed = true;
        }
        return kAsioNotPresent;
    }
}

long safeAsioGetBufferSize(IASIO *driver,
                           long *minSize,
                           long *maxSize,
                           long *preferredSize,
                           long *granularity,
                           bool *crashed)
{
    if (crashed) {
        *crashed = false;
    }
    __try {
        return driver ? driver->getBufferSize(minSize, maxSize, preferredSize, granularity)
                      : kAsioNotPresent;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (crashed) {
            *crashed = true;
        }
        return kAsioNotPresent;
    }
}

long safeAsioGetChannelInfo(IASIO *driver, ASIOChannelInfo *info, bool *crashed)
{
    if (crashed) {
        *crashed = false;
    }
    __try {
        return driver ? driver->getChannelInfo(info) : kAsioNotPresent;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (crashed) {
            *crashed = true;
        }
        return kAsioNotPresent;
    }
}

long safeAsioCreateBuffers(IASIO *driver,
                            ASIOBufferInfo *bufferInfos,
                            long channelCount,
                            long bufferSize,
                            ASIOCallbacks *callbacks,
                            bool *crashed)
{
    if (crashed) {
        *crashed = false;
    }
    __try {
        return driver ? driver->createBuffers(bufferInfos, channelCount, bufferSize, callbacks)
                      : kAsioNotPresent;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (crashed) {
            *crashed = true;
        }
        return kAsioNotPresent;
    }
}

long safeAsioDisposeBuffers(IASIO *driver, bool *crashed)
{
    if (crashed) {
        *crashed = false;
    }
    __try {
        return driver ? driver->disposeBuffers() : kAsioNotPresent;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (crashed) {
            *crashed = true;
        }
        return kAsioNotPresent;
    }
}

long safeAsioOutputReady(IASIO *driver, bool *crashed)
{
    if (crashed) {
        *crashed = false;
    }
    __try {
        return driver ? driver->outputReady() : kAsioNotPresent;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (crashed) {
            *crashed = true;
        }
        return kAsioNotPresent;
    }
}

int boundedEnvInt(const QString &name, int defaultValue, int minValue, int maxValue)
{
    const QString rawValue = QProcessEnvironment::systemEnvironment()
        .value(name)
        .trimmed();
    if (rawValue.isEmpty()) {
        return defaultValue;
    }

    bool ok = false;
    const int parsed = rawValue.toInt(&ok);
    if (!ok) {
        return defaultValue;
    }
    return qBound(minValue, parsed, maxValue);
}

int sampleTypeBytes(AsioSampleType sampleType)
{
    switch (sampleType) {
    case ASIOSTInt16LSB:
        return 2;
    case ASIOSTInt24LSB:
        return 3;
    case ASIOSTInt32LSB:
    case ASIOSTInt32LSB16:
    case ASIOSTInt32LSB18:
    case ASIOSTInt32LSB20:
    case ASIOSTInt32LSB24:
    case ASIOSTFloat32LSB:
        return 4;
    default:
        return 4;
    }
}

} // namespace AsioUtils
