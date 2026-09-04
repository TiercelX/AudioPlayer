// Steinberg ASIO SDK 2.3 interface definitions (minimal subset for probe)
// Based on the official Steinberg ASIO SDK headers

#ifndef __ASIO_H
#define __ASIO_H

#if defined(_MSC_VER) && !defined(__MWERKS__)
#pragma pack(push,4)
#endif

#if NATIVE_INT64
typedef long long int ASIOSamples;
#else
typedef struct ASIOSamples {
    unsigned long hi;
    unsigned long lo;
} ASIOSamples;
#endif

#if NATIVE_INT64
typedef long long int ASIOTimeStamp;
#else
typedef struct ASIOTimeStamp {
    unsigned long hi;
    unsigned long lo;
} ASIOTimeStamp;
#endif

// MSVC always uses IEEE 754 64-bit doubles
typedef double ASIOSampleRate;

typedef long ASIOBool;
enum { ASIOFalse = 0, ASIOTrue = 1 };

typedef long ASIOSampleType;
enum {
    ASIOSTInt16MSB   = 0,
    ASIOSTInt24MSB   = 1,
    ASIOSTInt32MSB   = 2,
    ASIOSTFloat32MSB = 3,
    ASIOSTFloat64MSB = 4,
    ASIOSTInt32MSB16 = 8,
    ASIOSTInt32MSB18 = 9,
    ASIOSTInt32MSB20 = 10,
    ASIOSTInt32MSB24 = 11,
    ASIOSTInt16LSB   = 16,
    ASIOSTInt24LSB   = 17,
    ASIOSTInt32LSB   = 18,
    ASIOSTFloat32LSB = 19,
    ASIOSTFloat64LSB = 20,
    ASIOSTInt32LSB16 = 24,
    ASIOSTInt32LSB18 = 25,
    ASIOSTInt32LSB20 = 26,
    ASIOSTInt32LSB24 = 27,
    ASIOSTDSDInt8LSB1 = 32,
    ASIOSTDSDInt8MSB1 = 33,
    ASIOSTDSDInt8NER8 = 40,
    ASIOSTLastEntry
};

typedef long ASIOError;
enum {
    ASE_OK = 0,
    ASE_SUCCESS = 0x3f4847a0,
    ASE_NotPresent = -1000,
    ASE_HWMalfunction,
    ASE_InvalidParameter,
    ASE_InvalidMode,
    ASE_SPNotAdvancing,
    ASE_NoClock,
    ASE_NoMemory
};

typedef struct ASIOTimeCode {
    double speed;
    ASIOSamples timeCodeSamples;
    unsigned long flags;
    char future[64];
} ASIOTimeCode;

typedef struct AsioTimeInfo {
    double speed;
    ASIOTimeStamp systemTime;
    ASIOSamples samplePosition;
    ASIOSampleRate sampleRate;
    unsigned long flags;
    char reserved[12];
} AsioTimeInfo;

typedef struct ASIOTime {
    long reserved[4];
    struct AsioTimeInfo timeInfo;
    struct ASIOTimeCode timeCode;
} ASIOTime;

typedef struct ASIOCallbacks {
    void (*bufferSwitch)(long doubleBufferIndex, ASIOBool directProcess);
    void (*sampleRateDidChange)(ASIOSampleRate sampleRate);
    long (*asioMessage)(long selector, long value, void* message, double* opt);
    ASIOTime* (*bufferSwitchTimeInfo)(ASIOTime* params, long doubleBufferIndex, ASIOBool directProcess);
} ASIOCallbacks;

enum {
    kAsioSelectorSupported = 1,
    kAsioEngineVersion,
    kAsioResetRequest,
    kAsioBufferSizeChange,
    kAsioResyncRequest,
    kAsioLatenciesChanged,
    kAsioSupportsTimeInfo,
    kAsioSupportsTimeCode,
    kAsioMMCCommand,
    kAsioSupportsInputMonitor,
    kAsioSupportsInputGain,
    kAsioSupportsInputMeter,
    kAsioSupportsOutputGain,
    kAsioSupportsOutputMeter,
    kAsioOverload,
    kAsioNumMessageSelectors
};

typedef struct ASIODriverInfo {
    long asioVersion;
    long driverVersion;
    char name[32];
    char errorMessage[124];
    void *sysRef;
} ASIODriverInfo;

typedef struct ASIOClockSource {
    long index;
    long associatedChannel;
    long associatedGroup;
    ASIOBool isCurrentSource;
    char name[32];
} ASIOClockSource;

typedef struct ASIOChannelInfo {
    long channel;
    ASIOBool isInput;
    ASIOBool isActive;
    long channelGroup;
    ASIOSampleType type;
    char name[32];
} ASIOChannelInfo;

typedef struct ASIOBufferInfo {
    ASIOBool isInput;
    long channelNum;
    void *buffers[2];
} ASIOBufferInfo;

#if defined(_MSC_VER) && !defined(__MWERKS__)
#pragma pack(pop)
#endif

#endif // __ASIO_H
