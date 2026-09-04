#ifndef ASIO_INTERFACE_H
#define ASIO_INTERFACE_H

#include <objbase.h>
#include <windows.h>

using ASIOBool = long;
using ASIOError = long;
using ASIOSampleRate = double;
using ASIOSampleType = long;

struct ASIOSamples {
    unsigned long hi = 0;
    unsigned long lo = 0;
};

using ASIOTimeStamp = ASIOSamples;

struct ASIOClockSource {
    long index = 0;
    long associatedChannel = 0;
    long associatedGroup = 0;
    ASIOBool isCurrentSource = 0;
    char name[32] = {};
};

struct ASIOChannelInfo {
    long channel = 0;
    ASIOBool isInput = 0;
    ASIOBool isActive = 0;
    long channelGroup = 0;
    ASIOSampleType type = 0;
    char name[32] = {};
};

struct ASIOBufferInfo {
    ASIOBool isInput = 0;
    long channelNum = 0;
    void *buffers[2] = {};
};

struct ASIOTimeInfo {
    double speed = 0.0;
    ASIOTimeStamp systemTime;
    ASIOSamples samplePosition;
    ASIOSampleRate sampleRate = 0.0;
    long flags = 0;
    char reserved[12] = {};
};

struct ASIOTimeCode {
    double speed = 0.0;
    ASIOSamples timeCodeSamples;
    unsigned long flags = 0;
    char future[64] = {};
};

struct ASIOTime {
    long reserved[4] = {};
    ASIOTimeInfo timeInfo;
    ASIOTimeCode timeCode;
};

struct ASIOCallbacks {
    void (*bufferSwitch)(long doubleBufferIndex, ASIOBool directProcess) = nullptr;
    void (*sampleRateDidChange)(ASIOSampleRate sampleRate) = nullptr;
    long (*asioMessage)(long selector, long value, void *message, double *opt) = nullptr;
    ASIOTime *(*bufferSwitchTimeInfo)(ASIOTime *params, long doubleBufferIndex, ASIOBool directProcess) = nullptr;
};

class IASIO : public IUnknown
{
public:
    virtual ASIOBool init(void *sysHandle) = 0;
    virtual void getDriverName(char *name) = 0;
    virtual long getDriverVersion() = 0;
    virtual void getErrorMessage(char *string) = 0;
    virtual ASIOError start() = 0;
    virtual ASIOError stop() = 0;
    virtual ASIOError getChannels(long *numInputChannels, long *numOutputChannels) = 0;
    virtual ASIOError getLatencies(long *inputLatency, long *outputLatency) = 0;
    virtual ASIOError getBufferSize(long *minSize, long *maxSize, long *preferredSize, long *granularity) = 0;
    virtual ASIOError canSampleRate(ASIOSampleRate sampleRate) = 0;
    virtual ASIOError getSampleRate(ASIOSampleRate *sampleRate) = 0;
    virtual ASIOError setSampleRate(ASIOSampleRate sampleRate) = 0;
    virtual ASIOError getClockSources(ASIOClockSource *clocks, long *numSources) = 0;
    virtual ASIOError setClockSource(long reference) = 0;
    virtual ASIOError getSamplePosition(ASIOSamples *sPos, ASIOTimeStamp *tStamp) = 0;
    virtual ASIOError getChannelInfo(ASIOChannelInfo *info) = 0;
    virtual ASIOError createBuffers(ASIOBufferInfo *bufferInfos, long numChannels, long bufferSize, ASIOCallbacks *callbacks) = 0;
    virtual ASIOError disposeBuffers() = 0;
    virtual ASIOError controlPanel() = 0;
    virtual ASIOError future(long selector, void *opt) = 0;
    virtual ASIOError outputReady() = 0;
};

#endif // ASIO_INTERFACE_H
