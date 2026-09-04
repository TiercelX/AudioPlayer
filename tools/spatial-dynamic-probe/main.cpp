#include <audioclient.h>
#include <mmdeviceapi.h>
#include <objbase.h>
#include <propkeydef.h>
#include <propvarutil.h>
#include <ksmedia.h>
#include <spatialaudioclient.h>
#include <SpatialAudioHrtf.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr UINT32 kDefaultDurationMs = 8000;
constexpr UINT32 kDefaultObjectCount = 15;
constexpr double kDefaultFrequency = 220.0;
constexpr UINT32 kRequiredSampleRate = 48000;
constexpr float kObjectPeak = 0.018f;
constexpr float kImpulsePeak = 0.1f;
constexpr float kLfePeak = 0.012f;

template <typename T>
void release(T *&value)
{
    if (value) {
        value->Release();
        value = nullptr;
    }
}

std::wstring hrText(HRESULT hr)
{
    wchar_t buffer[32] = {};
    swprintf_s(buffer, L"0x%08lX", static_cast<unsigned long>(hr));
    return buffer;
}

bool isMonoFloat48(const WAVEFORMATEX *format)
{
    if (!format || format->nChannels != 1 || format->nSamplesPerSec != kRequiredSampleRate
        || format->wBitsPerSample != 32) {
        return false;
    }
    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        return true;
    }
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE
        && format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto *extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(format);
        return extensible->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    }
    return false;
}

struct Options {
    UINT32 durationMs = kDefaultDurationMs;
    UINT32 impulseDelayMs = 0;
    UINT32 objectCount = kDefaultObjectCount;
    double frequency = kDefaultFrequency;
    enum class Signal { Sine, Impulse } signal = Signal::Sine;
    enum class PositionMode { Moving, Front, Left, Right, Upper } positionMode = PositionMode::Moving;
    enum class Renderer { Standard, Hrtf } renderer = Renderer::Standard;
    enum class HrtfEnvironment { Small, Outdoors } hrtfEnvironment = HrtfEnvironment::Small;
    bool selfTest = false;
};

double maximumGeneratedFrequency(const Options &options)
{
    return options.frequency + 37.0 * (static_cast<double>(options.objectCount) - 1.0);
}

bool validateOptions(const Options &options)
{
    const double maximumFrequency = maximumGeneratedFrequency(options);
    return options.durationMs >= 1000 && options.durationMs <= 600000
        && options.impulseDelayMs < options.durationMs
        && options.objectCount >= 1 && options.objectCount <= 15
        && std::isfinite(options.frequency) && options.frequency > 0.0
        && maximumFrequency <= 20000.0 && maximumFrequency < (kRequiredSampleRate / 2.0);
}

void printUsage()
{
    std::wcerr << L"Usage: SpatialDynamicProbe [--duration-ms N] [--objects N] [--frequency Hz]\n"
                  L"       [--impulse-delay-ms N]\n"
                  L"       [--signal sine|impulse] [--position moving|front|left|right|upper]\n"
                  L"       [--renderer standard|hrtf] [--hrtf-environment small|outdoors]\n"
                  L"       SpatialDynamicProbe --self-test"
               << std::endl;
}

bool parseOptions(int argc, wchar_t **argv, Options *options)
{
    if (!options) {
        return false;
    }
    *options = Options{};
    bool sawSelfTest = false;
    for (int index = 1; index < argc; ++index) {
        const std::wstring option = argv[index];
        try {
            if (option == L"--self-test") {
                if (sawSelfTest || index != 1 || index + 1 != argc) {
                    return false;
                }
                sawSelfTest = true;
                options->selfTest = true;
                continue;
            }
            if (sawSelfTest) {
                return false;
            }
            if (option == L"--duration-ms" && index + 1 < argc) {
                options->durationMs = static_cast<UINT32>(std::stoul(argv[++index]));
            } else if (option == L"--impulse-delay-ms" && index + 1 < argc) {
                options->impulseDelayMs = static_cast<UINT32>(std::stoul(argv[++index]));
            } else if (option == L"--objects" && index + 1 < argc) {
                options->objectCount = static_cast<UINT32>(std::stoul(argv[++index]));
            } else if (option == L"--frequency" && index + 1 < argc) {
                options->frequency = std::stod(argv[++index]);
            } else if (option == L"--signal" && index + 1 < argc) {
                const std::wstring value = argv[++index];
                if (value == L"sine") {
                    options->signal = Options::Signal::Sine;
                } else if (value == L"impulse") {
                    options->signal = Options::Signal::Impulse;
                } else {
                    return false;
                }
            } else if (option == L"--position" && index + 1 < argc) {
                const std::wstring value = argv[++index];
                if (value == L"moving") {
                    options->positionMode = Options::PositionMode::Moving;
                } else if (value == L"front") {
                    options->positionMode = Options::PositionMode::Front;
                } else if (value == L"left") {
                    options->positionMode = Options::PositionMode::Left;
                } else if (value == L"right") {
                    options->positionMode = Options::PositionMode::Right;
                } else if (value == L"upper") {
                    options->positionMode = Options::PositionMode::Upper;
                } else {
                    return false;
                }
            } else if (option == L"--renderer" && index + 1 < argc) {
                const std::wstring value = argv[++index];
                if (value == L"standard") {
                    options->renderer = Options::Renderer::Standard;
                } else if (value == L"hrtf") {
                    options->renderer = Options::Renderer::Hrtf;
                } else {
                    return false;
                }
            } else if (option == L"--hrtf-environment" && index + 1 < argc) {
                const std::wstring value = argv[++index];
                if (value == L"small") {
                    options->hrtfEnvironment = Options::HrtfEnvironment::Small;
                } else if (value == L"outdoors") {
                    options->hrtfEnvironment = Options::HrtfEnvironment::Outdoors;
                } else {
                    return false;
                }
            } else {
                return false;
            }
        } catch (...) {
            return false;
        }
    }
    return options->selfTest || (validateOptions(*options)
        && (options->renderer != Options::Renderer::Hrtf || options->objectCount == 1));
}

struct Position {
    float x;
    float y;
    float z;
};

Position syntheticPosition(size_t objectIndex, ULONGLONG frame, UINT32 sampleRate)
{
    const double t = static_cast<double>(frame) / static_cast<double>(sampleRate);
    const double phase = t * 0.65 + static_cast<double>(objectIndex) * 0.41;
    // Right-handed listener-relative metres: X right, Y up, Z behind.
    return {
        static_cast<float>(0.70 * std::sin(phase)),
        static_cast<float>(0.45 * std::cos(phase * 0.71 + 0.19)),
        static_cast<float>(0.90 * std::sin(phase * 0.53 + 1.17)),
    };
}

float syntheticSample(double frequency, size_t objectIndex, ULONGLONG frame, UINT32 sampleRate,
                      Options::Signal signal, UINT32 impulseDelayMs = 0)
{
    if (signal == Options::Signal::Impulse) {
        const ULONGLONG delayFrame = static_cast<ULONGLONG>(impulseDelayMs)
            * sampleRate / 1000;
        return (objectIndex == 0 && frame == delayFrame) ? kImpulsePeak : 0.0f;
    }
    const double objectFrequency = frequency + static_cast<double>(objectIndex) * 37.0;
    const double t = static_cast<double>(frame) / static_cast<double>(sampleRate);
    return static_cast<float>(kObjectPeak * std::sin(2.0 * kPi * objectFrequency * t));
}

float syntheticLfeSample(ULONGLONG frame, UINT32 sampleRate, Options::Signal signal)
{
    if (signal == Options::Signal::Impulse) {
        return 0.0f;
    }
    const double t = static_cast<double>(frame) / static_cast<double>(sampleRate);
    return static_cast<float>(kLfePeak * std::sin(2.0 * kPi * 55.0 * t));
}

Position selectedPosition(const Options &options, size_t objectIndex, ULONGLONG frame,
                          UINT32 sampleRate)
{
    switch (options.positionMode) {
    case Options::PositionMode::Front:
        return {0.0f, 0.0f, -1.0f};
    case Options::PositionMode::Left:
        return {-1.0f, 0.0f, 0.0f};
    case Options::PositionMode::Right:
        return {1.0f, 0.0f, 0.0f};
    case Options::PositionMode::Upper:
        return {0.0f, 1.0f, 0.0f};
    case Options::PositionMode::Moving:
        return syntheticPosition(objectIndex, frame, sampleRate);
    }
    return syntheticPosition(objectIndex, frame, sampleRate);
}

bool hasSufficientCapacity(UINT32 requested, UINT32 available)
{
    return available >= requested;
}

bool isFinitePosition(const Position &position)
{
    return std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z);
}

bool bufferLengthMatchesFrameCount(UINT32 bufferLength, UINT32 frameCount)
{
    const std::uint64_t expectedBytes = static_cast<std::uint64_t>(frameCount) * sizeof(float);
    return static_cast<std::uint64_t>(bufferLength) == expectedBytes;
}

bool checkedMultiply(std::uint64_t left, std::uint64_t right, std::uint64_t *result)
{
    if (!result || (right != 0 && left > (std::numeric_limits<std::uint64_t>::max)() / right)) {
        return false;
    }
    *result = left * right;
    return true;
}

bool metricCountsAreConsistent(std::uint64_t generatedFiniteCount,
                               std::uint64_t submittedFrames,
                               UINT32 objectCount,
                               std::uint64_t lfeBufferCalls,
                               std::uint64_t dynamicBufferCalls,
                               std::uint64_t positionCalls,
                               std::uint64_t volumeCalls,
                               std::uint64_t isActiveCalls,
                               std::uint64_t updateSuccesses)
{
    std::uint64_t expectedFiniteCount = 0;
    std::uint64_t expectedDynamicCalls = 0;
    std::uint64_t expectedActiveCalls = 0;
    const std::uint64_t objectCountWithLfe = static_cast<std::uint64_t>(objectCount) + 1;
    if (!checkedMultiply(submittedFrames, objectCountWithLfe, &expectedFiniteCount)
        || !checkedMultiply(updateSuccesses, objectCount, &expectedDynamicCalls)
        || !checkedMultiply(updateSuccesses, objectCountWithLfe, &expectedActiveCalls)) {
        return false;
    }
    return generatedFiniteCount == expectedFiniteCount
        && lfeBufferCalls == updateSuccesses
        && dynamicBufferCalls == expectedDynamicCalls
        && positionCalls == expectedDynamicCalls
        && volumeCalls == expectedDynamicCalls
        && isActiveCalls == expectedActiveCalls;
}

class CapacityNotify final : public ISpatialAudioObjectRenderStreamNotify {
public:
    explicit CapacityNotify(UINT32 requested)
        : requested_(requested)
    {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override
    {
        if (!object) {
            return E_POINTER;
        }
        *object = nullptr;
        if (iid == IID_IUnknown || iid == __uuidof(ISpatialAudioObjectRenderStreamNotify)) {
            *object = static_cast<ISpatialAudioObjectRenderStreamNotify *>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return ++references_;
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG remaining = --references_;
        if (remaining == 0) {
            delete this;
        }
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE OnAvailableDynamicObjectCountChange(
        ISpatialAudioObjectRenderStreamBase *sender,
        LONGLONG hnsComplianceDeadlineTime,
        UINT32 availableDynamicObjectCountChange) override
    {
        (void)sender;
        (void)hnsComplianceDeadlineTime;
        latest_.store(availableDynamicObjectCountChange, std::memory_order_release);
        notifications_.fetch_add(1, std::memory_order_relaxed);
        if (availableDynamicObjectCountChange < requested_) {
            belowContract_.store(true, std::memory_order_release);
        }
        return S_OK;
    }

    UINT32 notifications() const { return notifications_.load(std::memory_order_acquire); }
    UINT32 latest() const { return latest_.load(std::memory_order_acquire); }
    bool belowContract() const { return belowContract_.load(std::memory_order_acquire); }

private:
    std::atomic<ULONG> references_{1};
    const UINT32 requested_;
    std::atomic<UINT32> notifications_{0};
    std::atomic<UINT32> latest_{UINT32_MAX};
    std::atomic<bool> belowContract_{false};
};

struct SelfTestMetrics {
    UINT32 attempts = 0;
    UINT32 successes = 0;
    ULONGLONG frames = 0;
};

class PassLedger {
public:
    bool begin()
    {
        if (open_) {
            return false;
        }
        open_ = true;
        ++metrics_.attempts;
        return true;
    }

    bool end(bool pass, UINT32 frames)
    {
        if (!open_) {
            return false;
        }
        open_ = false;
        if (pass) {
            ++metrics_.successes;
            metrics_.frames += frames;
        }
        return true;
    }

    const SelfTestMetrics &metrics() const { return metrics_; }
    bool open() const { return open_; }

private:
    SelfTestMetrics metrics_;
    bool open_ = false;
};

bool runSelfTest()
{
    bool ok = true;
    UINT32 checks = 0;
    auto check = [&](bool condition, const wchar_t *name) {
        ++checks;
        std::wcout << L"selfTestCheck=" << name << L" result=" << (condition ? L"PASS" : L"FAIL")
                   << std::endl;
        ok = ok && condition;
    };

    wchar_t selfTestName[] = L"SpatialDynamicProbe";
    wchar_t selfTestOption[] = L"--self-test";
    wchar_t *selfTestArgs[] = {selfTestName, selfTestOption};
    Options bounds;
    check(parseOptions(2, selfTestArgs, &bounds), L"argumentSelfTestMode");
    check(kDefaultObjectCount == 15, L"argumentBoundsExactly15");
    wchar_t invalidName[] = L"SpatialDynamicProbe";
    wchar_t invalidOption[] = L"--objects";
    wchar_t invalidValue[] = L"16";
    wchar_t *invalidArgs[] = {invalidName, invalidOption, invalidValue};
    Options invalidBounds;
    check(!parseOptions(3, invalidArgs, &invalidBounds), L"argumentBoundsRejectsOverCapacity");
    wchar_t combinedSelfTest[] = L"--self-test";
    wchar_t *combinedArgs[] = {invalidName, invalidOption, invalidValue, combinedSelfTest};
    Options combinedOptions;
    check(!parseOptions(4, combinedArgs, &combinedOptions), L"selfTestModeIsExclusive");
    wchar_t shortDurationOption[] = L"--duration-ms";
    wchar_t shortDurationValue[] = L"999";
    wchar_t *shortDurationArgs[] = {invalidName, shortDurationOption, shortDurationValue};
    Options shortDurationOptions;
    check(!parseOptions(3, shortDurationArgs, &shortDurationOptions), L"argumentBoundsRejectsShortDuration");
    wchar_t zeroFrequencyOption[] = L"--frequency";
    wchar_t zeroFrequencyValue[] = L"0";
    wchar_t *zeroFrequencyArgs[] = {invalidName, zeroFrequencyOption, zeroFrequencyValue};
    Options zeroFrequencyOptions;
    check(!parseOptions(3, zeroFrequencyArgs, &zeroFrequencyOptions), L"argumentBoundsRejectsFrequency");
    wchar_t highObjectsValue[] = L"15";
    wchar_t highFrequencyValue[] = L"19500";
    wchar_t *highFrequencyArgs[] = {invalidName, invalidOption, highObjectsValue,
                                    zeroFrequencyOption, highFrequencyValue};
    Options highFrequencyOptions;
    check(!parseOptions(5, highFrequencyArgs, &highFrequencyOptions), L"argumentBoundsRejectsGeneratedFrequency");

    wchar_t oneObjectOption[] = L"--objects";
    wchar_t oneObjectValue[] = L"1";
    wchar_t impulseOption[] = L"--signal";
    wchar_t impulseValue[] = L"impulse";
    wchar_t frontOption[] = L"--position";
    wchar_t frontValue[] = L"front";
    wchar_t *focusedArgs[] = {invalidName, oneObjectOption, oneObjectValue,
                              impulseOption, impulseValue, frontOption, frontValue};
    Options focusedOptions;
    check(parseOptions(7, focusedArgs, &focusedOptions), L"focusedImpulseFrontArguments");
    check(focusedOptions.objectCount == 1
              && focusedOptions.signal == Options::Signal::Impulse
              && focusedOptions.positionMode == Options::PositionMode::Front,
           L"focusedImpulseFrontSelection");
    wchar_t rendererOption[] = L"--renderer";
    wchar_t hrtfValue[] = L"hrtf";
    wchar_t environmentOption[] = L"--hrtf-environment";
    wchar_t outdoorsValue[] = L"outdoors";
    wchar_t *hrtfArgs[] = {invalidName, oneObjectOption, oneObjectValue,
                           rendererOption, hrtfValue, environmentOption, outdoorsValue};
    Options hrtfOptions;
    check(parseOptions(7, hrtfArgs, &hrtfOptions)
              && hrtfOptions.renderer == Options::Renderer::Hrtf
              && hrtfOptions.hrtfEnvironment == Options::HrtfEnvironment::Outdoors,
          L"hrtfRendererAndEnvironmentArguments");
    wchar_t *hrtfWrongObjectArgs[] = {invalidName, rendererOption, hrtfValue};
    Options hrtfWrongObjectOptions;
    check(!parseOptions(3, hrtfWrongObjectArgs, &hrtfWrongObjectOptions),
          L"hrtfRequiresOneObject");
    wchar_t delayedImpulseOption[] = L"--impulse-delay-ms";
    wchar_t delayedImpulseValue[] = L"300";
    wchar_t *delayedImpulseArgs[] = {invalidName, oneObjectOption, oneObjectValue,
                                     impulseOption, impulseValue, delayedImpulseOption,
                                     delayedImpulseValue};
    Options delayedImpulseOptions;
    check(parseOptions(7, delayedImpulseArgs, &delayedImpulseOptions)
              && delayedImpulseOptions.impulseDelayMs == 300,
          L"impulseDelayArgument");
    wchar_t equalDurationOption[] = L"--duration-ms";
    wchar_t equalDurationValue[] = L"1000";
    wchar_t *equalDurationArgs[] = {invalidName, equalDurationOption, equalDurationValue,
                                    delayedImpulseOption, equalDurationValue};
    Options equalDurationOptions;
    check(!parseOptions(5, equalDurationArgs, &equalDurationOptions),
          L"impulseDelayMustBeShorterThanDuration");
    check(syntheticSample(kDefaultFrequency, 0, 0, kRequiredSampleRate,
                          Options::Signal::Impulse) == kImpulsePeak
              && syntheticSample(kDefaultFrequency, 0, 1, kRequiredSampleRate,
                                 Options::Signal::Impulse) == 0.0f,
          L"impulseOneSampleContract");
    check(syntheticSample(kDefaultFrequency, 0, 14400, kRequiredSampleRate,
                          Options::Signal::Impulse, 300) == kImpulsePeak
              && syntheticSample(kDefaultFrequency, 0, 0, kRequiredSampleRate,
                                 Options::Signal::Impulse, 300) == 0.0f,
          L"delayedImpulseSampleContract");
    check(syntheticLfeSample(0, kRequiredSampleRate, Options::Signal::Impulse) == 0.0f
              && std::abs(syntheticLfeSample(1, kRequiredSampleRate,
                                             Options::Signal::Sine)) > 0.0f,
          L"impulseDisablesSyntheticLfe");
    const Position focusedPosition = selectedPosition(focusedOptions, 0, 0, kRequiredSampleRate);
    check(focusedPosition.x == 0.0f && focusedPosition.y == 0.0f && focusedPosition.z == -1.0f,
          L"frontPositionContract");

    std::vector<size_t> identities(kDefaultObjectCount);
    for (size_t index = 0; index < identities.size(); ++index) {
        identities[index] = index;
    }
    check(identities.size() == 15 && identities.front() == 0 && identities.back() == 14,
          L"objectIdentities");

    bool finite = true;
    float peak = 0.0f;
    for (size_t object = 0; object < identities.size(); ++object) {
        for (ULONGLONG frame = 0; frame < 480; frame += 17) {
            const float sample = syntheticSample(kDefaultFrequency, object, frame, kRequiredSampleRate,
                                                 Options::Signal::Sine);
            finite = finite && std::isfinite(sample);
            peak = (std::max)(peak, std::abs(sample));
        }
    }
    check(finite && peak <= 0.02f, L"finiteDeterministicPcmPeak");

    bool positiveX = false;
    bool negativeX = false;
    bool positiveY = false;
    bool negativeY = false;
    bool positiveZ = false;
    bool negativeZ = false;
    bool withinBounds = true;
    for (size_t object = 0; object < identities.size(); ++object) {
        for (ULONGLONG frame = 0; frame < 48000; frame += 997) {
            const Position position = syntheticPosition(object, frame, kRequiredSampleRate);
            positiveX = positiveX || position.x > 0.0f;
            negativeX = negativeX || position.x < 0.0f;
            positiveY = positiveY || position.y > 0.0f;
            negativeY = negativeY || position.y < 0.0f;
            positiveZ = positiveZ || position.z > 0.0f;
            negativeZ = negativeZ || position.z < 0.0f;
            const double radius = std::sqrt(static_cast<double>(position.x) * position.x
                                             + static_cast<double>(position.y) * position.y
                                             + static_cast<double>(position.z) * position.z);
            withinBounds = withinBounds && std::isfinite(radius) && radius < 2.0;
        }
    }
    check(positiveX && negativeX && positiveY && negativeY && positiveZ && negativeZ && withinBounds,
          L"rightHandedPositions");

    bool stableIdentity = true;
    const UINT32 updateSizes[] = {64, 480, 1024, 577};
    for (UINT32 size : updateSizes) {
        (void)size;
        for (size_t index = 0; index < identities.size(); ++index) {
            stableIdentity = stableIdentity && identities[index] == index;
        }
    }
    check(stableIdentity, L"stableObjectIdentityAcrossUpdateSizes");

    PassLedger ledger;
    const bool beganCommit = ledger.begin();
    const bool endedCommit = ledger.end(true, 480);
    const SelfTestMetrics committed = ledger.metrics();
    const bool beganReject = ledger.begin();
    const SelfTestMetrics beforeReject = ledger.metrics();
    const bool endedReject = ledger.end(false, 480);
    const SelfTestMetrics rejected = ledger.metrics();
    check(beganCommit && endedCommit && beganReject && endedReject && !ledger.open()
              && committed.attempts == 1 && committed.successes == 1 && committed.frames == 480
              && rejected.attempts == 2 && rejected.successes == beforeReject.successes
              && rejected.frames == beforeReject.frames,
          L"wholePassCommitRollback");

    const Position nonFinitePosition = {std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f};
    check(!hasSufficientCapacity(kDefaultObjectCount, 14)
              && !isFinitePosition(nonFinitePosition),
          L"capacityAndNonFiniteRejection");
    check(bufferLengthMatchesFrameCount(480U * sizeof(float), 480U)
              && !bufferLengthMatchesFrameCount(480U * sizeof(float) - sizeof(float), 480U)
              && metricCountsAreConsistent(480U * 16U,
                                            480U,
                                            15U,
                                            1U,
                                            15U,
                                            15U,
                                            15U,
                                            16U,
                                            1U),
          L"bufferLengthAndMetricFormula");

    struct Lifecycle {
        bool stream = false;
        bool lfe = false;
        bool dynamic = false;
        bool reset = false;
    } fresh;
    Lifecycle cleaned;
    cleaned.reset = true;
    check(!fresh.stream && !fresh.lfe && !fresh.dynamic && cleaned.reset,
          L"cleanupResetEquivalentToFresh");

    std::wcout << L"selfTestChecks=" << checks << L" generatedPeak=" << peak << std::endl;
    std::wcout << L"selfTest=" << (ok ? L"PASS" : L"FAIL")
               << L" probeResult=" << (ok ? L"PASS" : L"FAIL") << std::endl;
    return ok;
}

enum class Outcome {
    Pass,
    Fail,
    Inconclusive,
};

const wchar_t *outcomeText(Outcome outcome)
{
    switch (outcome) {
    case Outcome::Pass:
        return L"PASS";
    case Outcome::Fail:
        return L"FAIL";
    case Outcome::Inconclusive:
        return L"INCONCLUSIVE";
    }
    return L"FAIL";
}

const wchar_t *signalText(Options::Signal signal)
{
    return signal == Options::Signal::Impulse ? L"impulse" : L"sine";
}

const wchar_t *positionModeText(Options::PositionMode mode)
{
    switch (mode) {
    case Options::PositionMode::Front: return L"front";
    case Options::PositionMode::Left: return L"left";
    case Options::PositionMode::Right: return L"right";
    case Options::PositionMode::Upper: return L"upper";
    case Options::PositionMode::Moving: return L"moving";
    }
    return L"moving";
}

const wchar_t *rendererText(Options::Renderer renderer)
{
    return renderer == Options::Renderer::Hrtf ? L"hrtf" : L"standard";
}

const wchar_t *hrtfEnvironmentText(Options::HrtfEnvironment environment)
{
    return environment == Options::HrtfEnvironment::Outdoors ? L"outdoors" : L"small";
}

int runStandardLive(const Options &options)
{
    IMMDeviceEnumerator *enumerator = nullptr;
    IMMDevice *device = nullptr;
    ISpatialAudioClient *spatialClient = nullptr;
    IAudioFormatEnumerator *formatEnumerator = nullptr;
    WAVEFORMATEX *selectedFormat = nullptr;
    ISpatialAudioObjectRenderStream *stream = nullptr;
    CapacityNotify *notify = nullptr;
    HANDLE eventHandle = nullptr;
    ISpatialAudioObject *lfe = nullptr;
    std::vector<ISpatialAudioObject *> dynamicObjects;
    SpatialAudioObjectRenderStreamActivationParams activation = {};
    PROPVARIANT activationProperty;
    PropVariantInit(&activationProperty);

    Outcome outcome = Outcome::Inconclusive;
    std::wstring firstFailureStage;
    HRESULT firstFailureHr = S_OK;
    bool started = false;
    bool updateOpen = false;
    bool cleanupComplete = true;
    AudioObjectType nativeMask = AudioObjectType_None;
    UINT32 maximumDynamicObjects = 0;
    UINT32 formatCount = 0;
    UINT32 updateAttempts = 0;
    UINT32 updateSuccesses = 0;
    UINT32 timeoutCount = 0;
    ULONGLONG submittedFrames = 0;
    ULONGLONG bufferCalls = 0;
    ULONGLONG lfeBufferCalls = 0;
    ULONGLONG dynamicBufferCalls = 0;
    ULONGLONG positionCalls = 0;
    ULONGLONG volumeCalls = 0;
    ULONGLONG generatedFiniteCount = 0;
    float generatedPeak = 0.0f;
    UINT32 firstUpdateFrames = 0;
    ULONGLONG totalFrames = 0;
    UINT32 activeObjectsAtEnd = 0;
    UINT32 activeLfeAtEnd = 0;
    UINT32 activatedDynamicObjectsCount = 0;
    bool firstPass = true;
    UINT32 capacityChangeNotifications = 0;
    UINT32 latestCapacity = UINT32_MAX;
    ULONGLONG isActiveCalls = 0;
    UINT32 inactiveObjects = 0;
    const ULONGLONG expectedFrames = static_cast<ULONGLONG>(options.durationMs)
        * kRequiredSampleRate / 1000;
    double coverageRatio = 0.0;
    bool metricConsistency = false;
    bool resetAttempted = false;
    bool resetSucceeded = false;

    auto recordFailure = [&](Outcome requestedOutcome, const wchar_t *stage, HRESULT hr) {
        if (firstFailureStage.empty()) {
            firstFailureStage = stage;
            firstFailureHr = hr;
        }
        if (outcome == Outcome::Pass || requestedOutcome == Outcome::Fail) {
            outcome = requestedOutcome;
        } else if (outcome != Outcome::Fail) {
            outcome = requestedOutcome;
        }
    };

    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool shouldUninitialize = SUCCEEDED(comHr);
    if (FAILED(comHr) && comHr != RPC_E_CHANGED_MODE) {
        std::wcerr << L"probeResult=FAIL stage=CoInitializeEx hr=" << hrText(comHr) << std::endl;
        return 1;
    }

    do {
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator),
                                      nullptr,
                                      CLSCTX_ALL,
                                      __uuidof(IMMDeviceEnumerator),
                                      reinterpret_cast<void **>(&enumerator));
        if (SUCCEEDED(hr)) {
            hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        }
        if (FAILED(hr) || !device) {
            recordFailure(Outcome::Inconclusive, L"defaultEndpoint", hr);
            break;
        }
        LPWSTR endpointId = nullptr;
        if (SUCCEEDED(device->GetId(&endpointId)) && endpointId) {
            std::wcout << L"endpointSelected=1 endpointId=" << endpointId << std::endl;
            CoTaskMemFree(endpointId);
        } else {
            std::wcout << L"endpointSelected=1" << std::endl;
        }

        hr = device->Activate(__uuidof(ISpatialAudioClient),
                              CLSCTX_ALL,
                              nullptr,
                              reinterpret_cast<void **>(&spatialClient));
        if (FAILED(hr) || !spatialClient) {
            recordFailure(Outcome::Inconclusive, L"activateSpatialClient", hr);
            break;
        }

        hr = spatialClient->IsSpatialAudioStreamAvailable(__uuidof(ISpatialAudioObjectRenderStream), nullptr);
        std::wcout << L"spatialStreamAvailable=" << (hr == S_OK ? 1 : 0)
                   << L" hr=" << hrText(hr) << std::endl;
        if (hr != S_OK) {
            recordFailure(Outcome::Inconclusive, L"streamAvailability", hr);
            break;
        }

        hr = spatialClient->GetNativeStaticObjectTypeMask(&nativeMask);
        std::wcout << L"nativeStaticMask=0x" << std::hex << static_cast<unsigned int>(nativeMask)
                   << std::dec << L" hr=" << hrText(hr) << std::endl;
        if (FAILED(hr)) {
            recordFailure(Outcome::Inconclusive, L"nativeStaticMask", hr);
            break;
        }
        if ((nativeMask & AudioObjectType_LowFrequency) == 0) {
            recordFailure(Outcome::Inconclusive, L"staticLfeCapability", S_FALSE);
            break;
        }

        hr = spatialClient->GetMaxDynamicObjectCount(&maximumDynamicObjects);
        std::wcout << L"maxDynamicObjects=" << maximumDynamicObjects << L" hr=" << hrText(hr)
                   << std::endl;
        if (FAILED(hr)) {
            recordFailure(Outcome::Inconclusive, L"maxDynamicObjectCount", hr);
            break;
        }
        if (maximumDynamicObjects < options.objectCount) {
            recordFailure(Outcome::Inconclusive, L"dynamicCapacityBeforeStart", S_FALSE);
            break;
        }

        hr = spatialClient->GetSupportedAudioObjectFormatEnumerator(&formatEnumerator);
        if (FAILED(hr) || !formatEnumerator) {
            recordFailure(Outcome::Inconclusive, L"formatEnumerator", hr);
            break;
        }
        formatEnumerator->GetCount(&formatCount);
        for (UINT32 index = 0; index < formatCount; ++index) {
            WAVEFORMATEX *candidate = nullptr;
            if (SUCCEEDED(formatEnumerator->GetFormat(index, &candidate)) && isMonoFloat48(candidate)) {
                selectedFormat = candidate;
                break;
            }
            if (candidate) {
                CoTaskMemFree(candidate);
            }
        }
        if (!selectedFormat) {
            recordFailure(Outcome::Inconclusive, L"monoFloat48Format", S_FALSE);
            break;
        }
        std::wcout << L"objectFormatRate=" << selectedFormat->nSamplesPerSec
                   << L" objectFormatChannels=" << selectedFormat->nChannels
                   << L" objectFormatBits=" << selectedFormat->wBitsPerSample << std::endl;

        eventHandle = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!eventHandle) {
            recordFailure(Outcome::Fail, L"createEvent", HRESULT_FROM_WIN32(GetLastError()));
            break;
        }
        notify = new CapacityNotify(options.objectCount);
        activation.ObjectFormat = selectedFormat;
        activation.StaticObjectTypeMask = AudioObjectType_LowFrequency;
        activation.MinDynamicObjectCount = options.objectCount;
        activation.MaxDynamicObjectCount = options.objectCount;
        activation.Category = AudioCategory_Media;
        activation.EventHandle = eventHandle;
        activation.NotifyObject = notify;
        activationProperty.vt = VT_BLOB;
        activationProperty.blob.cbSize = sizeof(activation);
        activationProperty.blob.pBlobData = reinterpret_cast<BYTE *>(&activation);
        hr = spatialClient->ActivateSpatialAudioStream(&activationProperty,
                                                       __uuidof(ISpatialAudioObjectRenderStream),
                                                       reinterpret_cast<void **>(&stream));
        // The activation blob points at the stack-owned activation structure;
        // it is consumed by ActivateSpatialAudioStream and must not be freed
        // by PropVariantClear as if it were an allocated PROPVARIANT blob.
        activationProperty.vt = VT_EMPTY;
        if (FAILED(hr) || !stream) {
            recordFailure(Outcome::Inconclusive, L"activateSpatialStream", hr);
            break;
        }
        hr = stream->Start();
        if (FAILED(hr)) {
            recordFailure(Outcome::Inconclusive, L"startSpatialStream", hr);
            break;
        }
        started = true;

        outcome = Outcome::Pass;
        const ULONGLONG startTick = GetTickCount64();
        while (GetTickCount64() - startTick < options.durationMs) {
            const DWORD waitResult = WaitForSingleObject(eventHandle, 100);
            if (waitResult == WAIT_TIMEOUT) {
                ++timeoutCount;
                continue;
            }
            if (waitResult == WAIT_FAILED) {
                recordFailure(Outcome::Inconclusive, L"waitForSpatialEvent", HRESULT_FROM_WIN32(GetLastError()));
                break;
            }

            ++updateAttempts;
            UINT32 availableDynamicObjects = 0;
            UINT32 frameCount = 0;
            hr = stream->BeginUpdatingAudioObjects(&availableDynamicObjects, &frameCount);
            if (FAILED(hr)) {
                recordFailure(Outcome::Inconclusive, L"beginUpdate", hr);
                break;
            }
            updateOpen = true;
            bool passOk = hasSufficientCapacity(options.objectCount, availableDynamicObjects);
            if (!passOk) {
                recordFailure(Outcome::Inconclusive, L"dynamicCapacityDuringUpdate", S_FALSE);
            }
            if (notify->belowContract()) {
                passOk = false;
                recordFailure(Outcome::Inconclusive, L"capacityChangeBelowContract", S_FALSE);
            }

            if (passOk && firstPass) {
                hr = stream->ActivateSpatialAudioObject(AudioObjectType_LowFrequency, &lfe);
                if (FAILED(hr) || !lfe) {
                    passOk = false;
                    recordFailure(Outcome::Inconclusive, L"activateStaticLfe", hr);
                } else {
                    dynamicObjects.reserve(options.objectCount);
                    for (UINT32 index = 0; index < options.objectCount; ++index) {
                        ISpatialAudioObject *object = nullptr;
                        hr = stream->ActivateSpatialAudioObject(AudioObjectType_Dynamic, &object);
                        if (FAILED(hr) || !object) {
                            passOk = false;
                            recordFailure(Outcome::Inconclusive, L"activateDynamicObject", hr);
                            release(object);
                            break;
                        }
                        dynamicObjects.push_back(object);
                    }
                }
            }

            if (passOk) {
                BOOL lfeActive = FALSE;
                hr = lfe->IsActive(&lfeActive);
                ++isActiveCalls;
                if (FAILED(hr) || !lfeActive) {
                    passOk = false;
                    if (!lfeActive) {
                        ++inactiveObjects;
                    }
                    recordFailure(Outcome::Inconclusive, L"isActiveLfe", hr);
                }
            }
            for (size_t index = 0; passOk && index < dynamicObjects.size(); ++index) {
                BOOL objectActive = FALSE;
                hr = dynamicObjects[index]->IsActive(&objectActive);
                ++isActiveCalls;
                if (FAILED(hr) || !objectActive) {
                    passOk = false;
                    if (!objectActive) {
                        ++inactiveObjects;
                    }
                    recordFailure(Outcome::Inconclusive, L"isActiveDynamicObject", hr);
                    break;
                }
            }

            if (passOk) {
                BYTE *buffer = nullptr;
                UINT32 bufferLength = 0;
                hr = lfe->GetBuffer(&buffer, &bufferLength);
                if (FAILED(hr) || !buffer) {
                    passOk = false;
                    recordFailure(Outcome::Inconclusive, L"getLfeBuffer", hr);
                } else {
                    ++bufferCalls;
                    ++lfeBufferCalls;
                    if (!bufferLengthMatchesFrameCount(bufferLength, frameCount)) {
                        passOk = false;
                        recordFailure(Outcome::Inconclusive, L"lfeBufferSizeMismatch", S_FALSE);
                    } else {
                        std::memset(buffer, 0, bufferLength);
                        auto *samples = reinterpret_cast<float *>(buffer);
                        for (UINT32 frame = 0; frame < frameCount; ++frame) {
                            samples[frame] = syntheticLfeSample(totalFrames + frame,
                                                                 selectedFormat->nSamplesPerSec,
                                                                 options.signal);
                            if (!std::isfinite(samples[frame])) {
                                passOk = false;
                                recordFailure(Outcome::Inconclusive, L"nonFiniteLfePcm", E_FAIL);
                                break;
                            }
                            ++generatedFiniteCount;
                            generatedPeak = (std::max)(generatedPeak, std::abs(samples[frame]));
                        }
                    }
                }
            }

            for (size_t index = 0; passOk && index < dynamicObjects.size(); ++index) {
                BYTE *buffer = nullptr;
                UINT32 bufferLength = 0;
                hr = dynamicObjects[index]->GetBuffer(&buffer, &bufferLength);
                if (FAILED(hr) || !buffer) {
                    passOk = false;
                    recordFailure(Outcome::Inconclusive, L"getDynamicBuffer", hr);
                    break;
                }
                ++bufferCalls;
                ++dynamicBufferCalls;
                if (!bufferLengthMatchesFrameCount(bufferLength, frameCount)) {
                    passOk = false;
                    recordFailure(Outcome::Inconclusive, L"dynamicBufferSizeMismatch", S_FALSE);
                    break;
                }
                std::memset(buffer, 0, bufferLength);
                auto *samples = reinterpret_cast<float *>(buffer);
                for (UINT32 frame = 0; frame < frameCount; ++frame) {
                    samples[frame] = syntheticSample(options.frequency, index,
                                                     totalFrames + frame,
                                                     selectedFormat->nSamplesPerSec,
                                                     options.signal,
                                                     options.impulseDelayMs);
                    if (!std::isfinite(samples[frame])) {
                        passOk = false;
                        recordFailure(Outcome::Inconclusive, L"nonFiniteDynamicPcm", E_FAIL);
                        break;
                    }
                    ++generatedFiniteCount;
                    generatedPeak = (std::max)(generatedPeak, std::abs(samples[frame]));
                }
                if (!passOk) {
                    break;
                }
                const Position position = selectedPosition(options, index, totalFrames,
                                                           selectedFormat->nSamplesPerSec);
                if (!isFinitePosition(position)) {
                    passOk = false;
                    recordFailure(Outcome::Inconclusive, L"nonFinitePosition", E_FAIL);
                    break;
                }
                hr = dynamicObjects[index]->SetPosition(position.x, position.y, position.z);
                if (FAILED(hr)) {
                    passOk = false;
                    recordFailure(Outcome::Inconclusive, L"setPosition", hr);
                    break;
                }
                ++positionCalls;
                const float volume = static_cast<float>(0.55 + 0.10
                    * std::sin(static_cast<double>(totalFrames)
                               / selectedFormat->nSamplesPerSec + index * 0.17));
                if (!std::isfinite(volume) || volume < 0.0f || volume > 1.0f) {
                    passOk = false;
                    recordFailure(Outcome::Inconclusive, L"invalidVolume", E_FAIL);
                    break;
                }
                hr = dynamicObjects[index]->SetVolume(volume);
                if (FAILED(hr)) {
                    passOk = false;
                    recordFailure(Outcome::Inconclusive, L"setVolume", hr);
                    break;
                }
                ++volumeCalls;
            }

            if (notify->belowContract()) {
                passOk = false;
                recordFailure(Outcome::Inconclusive, L"capacityChangeBeforeEnd", S_FALSE);
            }
            hr = stream->EndUpdatingAudioObjects();
            updateOpen = false;
            if (FAILED(hr)) {
                recordFailure(Outcome::Inconclusive, L"endUpdate", hr);
                break;
            }
            if (!passOk) {
                break;
            }
            ++updateSuccesses;
            submittedFrames += frameCount;
            if (firstPass) {
                firstUpdateFrames = frameCount;
                firstPass = false;
            }
            totalFrames += frameCount;
        }
        coverageRatio = expectedFrames == 0
            ? 0.0
            : static_cast<double>(submittedFrames) / static_cast<double>(expectedFrames);
        if (outcome == Outcome::Pass && submittedFrames < expectedFrames) {
            recordFailure(Outcome::Inconclusive, L"submittedFrameCoverage", S_FALSE);
        }
        metricConsistency = metricCountsAreConsistent(generatedFiniteCount,
                                                       submittedFrames,
                                                       options.objectCount,
                                                       lfeBufferCalls,
                                                       dynamicBufferCalls,
                                                       positionCalls,
                                                       volumeCalls,
                                                       isActiveCalls,
                                                       updateSuccesses);
        if (outcome == Outcome::Pass && !metricConsistency) {
            recordFailure(Outcome::Inconclusive, L"metricConsistency", S_FALSE);
        }
        if (outcome == Outcome::Pass && inactiveObjects != 0) {
            recordFailure(Outcome::Inconclusive, L"inactiveObjects", S_FALSE);
        }
        activatedDynamicObjectsCount = static_cast<UINT32>(dynamicObjects.size());
        activeObjectsAtEnd = static_cast<UINT32>(dynamicObjects.size());
        activeLfeAtEnd = lfe ? 1U : 0U;
        if (outcome == Outcome::Pass && updateSuccesses == 0) {
            recordFailure(Outcome::Inconclusive, L"noSuccessfulUpdates", S_FALSE);
        }
    } while (false);

    if (updateOpen && stream) {
        const HRESULT endHr = stream->EndUpdatingAudioObjects();
        updateOpen = false;
        if (FAILED(endHr) && firstFailureStage.empty()) {
            firstFailureStage = L"endUpdateCleanup";
            firstFailureHr = endHr;
            outcome = Outcome::Inconclusive;
        }
    }
    if (stream && started) {
        const HRESULT stopHr = stream->Stop();
        if (FAILED(stopHr) && firstFailureStage.empty()) {
            firstFailureStage = L"stopSpatialStream";
            firstFailureHr = stopHr;
            outcome = Outcome::Inconclusive;
        }
    }
    release(lfe);
    for (ISpatialAudioObject *object : dynamicObjects) {
        release(object);
    }
    dynamicObjects.clear();
    if (stream) {
        resetAttempted = true;
        const HRESULT resetHr = stream->Reset();
        resetSucceeded = SUCCEEDED(resetHr);
        if (FAILED(resetHr)) {
            cleanupComplete = false;
            if (firstFailureStage.empty()) {
                firstFailureStage = L"resetSpatialStream";
                firstFailureHr = resetHr;
                outcome = Outcome::Inconclusive;
            }
        }
    }
    release(stream);
    if (eventHandle) {
        CloseHandle(eventHandle);
        eventHandle = nullptr;
    }
    if (notify) {
        capacityChangeNotifications = notify->notifications();
        latestCapacity = notify->latest();
        notify->Release();
        notify = nullptr;
    }
    PropVariantClear(&activationProperty);
    if (selectedFormat) {
        CoTaskMemFree(selectedFormat);
    }
    release(formatEnumerator);
    release(spatialClient);
    release(device);
    release(enumerator);
    if (shouldUninitialize) {
        CoUninitialize();
    }
    cleanupComplete = cleanupComplete && dynamicObjects.empty() && !lfe && !stream && !eventHandle && !notify;

    std::wcout << L"signal=" << signalText(options.signal)
               << L" positionMode=" << positionModeText(options.positionMode)
               << L" requestedDynamicObjects=" << options.objectCount
               << L" activatedDynamicObjects=" << activatedDynamicObjectsCount
               << L" activeDynamicObjectsAtEnd=" << activeObjectsAtEnd
               << L" activeLfeAtEnd=" << activeLfeAtEnd << std::endl;
    std::wcout << L"capacityChangeNotifications=" << capacityChangeNotifications
               << L" latestCapacity=" << latestCapacity << std::endl;
    std::wcout << L"updateAttempts=" << updateAttempts << L" updateSuccesses=" << updateSuccesses
               << L" submittedFrames=" << submittedFrames << L" bufferCalls=" << bufferCalls
               << L" lfeBufferCalls=" << lfeBufferCalls
               << L" dynamicBufferCalls=" << dynamicBufferCalls
               << L" positionCalls=" << positionCalls << L" volumeCalls=" << volumeCalls
               << L" timeoutCount=" << timeoutCount << L" firstUpdateFrames=" << firstUpdateFrames
               << std::endl;
    std::wcout << L"isActiveCalls=" << isActiveCalls
               << L" inactiveObjects=" << inactiveObjects
               << L" expectedFrames=" << expectedFrames
               << L" coverageRatio=" << std::fixed << std::setprecision(6) << coverageRatio
               << L" metricConsistency=" << (metricConsistency ? L"PASS" : L"FAIL")
               << std::defaultfloat << std::endl;
    std::wcout << L"generatedFiniteCount=" << generatedFiniteCount << L" generatedPeak=" << generatedPeak
               << L" resetAttempted=" << (resetAttempted ? 1 : 0)
               << L" resetSucceeded=" << (resetSucceeded ? 1 : 0)
               << L" cleanupComplete=" << (cleanupComplete ? 1 : 0) << std::endl;
    if (!firstFailureStage.empty()) {
        std::wcout << L"firstFailureStage=" << firstFailureStage
                   << L" firstFailureHresult=" << hrText(firstFailureHr) << std::endl;
    }
    std::wcout << L"probeResult=" << outcomeText(outcome)
               << L" evidenceLimit=endpoint-submission-only;manual-listening-or-loopback-required"
               << std::endl;
    return outcome == Outcome::Pass ? 0 : 1;
}

int runHrtfLive(const Options &options)
{
    IMMDeviceEnumerator *enumerator = nullptr;
    IMMDevice *device = nullptr;
    ISpatialAudioClient *spatialClient = nullptr;
    IAudioFormatEnumerator *formatEnumerator = nullptr;
    WAVEFORMATEX *selectedFormat = nullptr;
    ISpatialAudioObjectRenderStreamForHrtf *stream = nullptr;
    ISpatialAudioObjectForHrtf *object = nullptr;
    CapacityNotify *notify = nullptr;
    HANDLE eventHandle = nullptr;
    SpatialAudioHrtfActivationParams activation = {};
    SpatialAudioHrtfDirectivityUnion directivity = {};
    SpatialAudioHrtfEnvironmentType environment = options.hrtfEnvironment
        == Options::HrtfEnvironment::Outdoors
        ? SpatialAudioHrtfEnvironment_Outdoors
        : SpatialAudioHrtfEnvironment_Small;
    SpatialAudioHrtfOrientation orientation = {
        1.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 1.0f};
    directivity.Omni.Type = SpatialAudioHrtfDirectivity_OmniDirectional;
    directivity.Omni.Scaling = 0.0f;
    PROPVARIANT activationProperty;
    PropVariantInit(&activationProperty);

    Outcome outcome = Outcome::Inconclusive;
    std::wstring firstFailureStage;
    HRESULT firstFailureHr = S_OK;
    bool started = false;
    bool updateOpen = false;
    bool cleanupComplete = true;
    UINT32 maximumDynamicObjects = 0;
    UINT32 formatCount = 0;
    UINT32 updateAttempts = 0;
    UINT32 updateSuccesses = 0;
    UINT32 timeoutCount = 0;
    ULONGLONG submittedFrames = 0;
    ULONGLONG bufferCalls = 0;
    ULONGLONG positionCalls = 0;
    ULONGLONG generatedFiniteCount = 0;
    float generatedPeak = 0.0f;
    UINT32 firstUpdateFrames = 0;
    ULONGLONG totalFrames = 0;
    UINT32 activatedDynamicObjectsCount = 0;
    bool firstPass = true;
    UINT32 capacityChangeNotifications = 0;
    UINT32 latestCapacity = UINT32_MAX;
    bool resetAttempted = false;
    bool resetSucceeded = false;
    const ULONGLONG expectedFrames = static_cast<ULONGLONG>(options.durationMs)
        * kRequiredSampleRate / 1000;

    auto recordFailure = [&](Outcome requestedOutcome, const wchar_t *stage, HRESULT hr) {
        if (firstFailureStage.empty()) {
            firstFailureStage = stage;
            firstFailureHr = hr;
        }
        if (outcome == Outcome::Pass || requestedOutcome == Outcome::Fail) {
            outcome = requestedOutcome;
        } else if (outcome != Outcome::Fail) {
            outcome = requestedOutcome;
        }
    };

    std::wcout << L"renderer=hrtf environment=" << hrtfEnvironmentText(options.hrtfEnvironment)
               << L" interface=ISpatialAudioObjectRenderStreamForHrtf"
               << L" distanceDecay=activation-null-default"
               << L" directivity=omnidirectional scaling=0 orientation=identity" << std::endl;

    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool shouldUninitialize = SUCCEEDED(comHr);
    if (FAILED(comHr) && comHr != RPC_E_CHANGED_MODE) {
        std::wcerr << L"probeResult=FAIL stage=CoInitializeEx hr=" << hrText(comHr) << std::endl;
        return 1;
    }

    do {
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      __uuidof(IMMDeviceEnumerator),
                                      reinterpret_cast<void **>(&enumerator));
        if (SUCCEEDED(hr)) {
            hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        }
        if (FAILED(hr) || !device) {
            recordFailure(Outcome::Inconclusive, L"defaultEndpoint", hr);
            break;
        }
        LPWSTR endpointId = nullptr;
        if (SUCCEEDED(device->GetId(&endpointId)) && endpointId) {
            std::wcout << L"endpointId=" << endpointId << std::endl;
            CoTaskMemFree(endpointId);
        } else {
            std::wcout << L"endpointId=UNAVAILABLE" << std::endl;
        }

        hr = device->Activate(__uuidof(ISpatialAudioClient), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void **>(&spatialClient));
        if (FAILED(hr) || !spatialClient) {
            recordFailure(Outcome::Inconclusive, L"activateSpatialClient", hr);
            break;
        }
        hr = spatialClient->IsSpatialAudioStreamAvailable(
            __uuidof(ISpatialAudioObjectRenderStreamForHrtf), nullptr);
        std::wcout << L"interfaceAvailability=" << (hr == S_OK ? L"PASS" : L"INCONCLUSIVE")
                   << L" hr=" << hrText(hr) << std::endl;
        if (hr != S_OK) {
            recordFailure(Outcome::Inconclusive, L"hrtfStreamAvailability", hr);
            break;
        }
        hr = spatialClient->GetMaxDynamicObjectCount(&maximumDynamicObjects);
        std::wcout << L"maxDynamicObjects=" << maximumDynamicObjects
                   << L" hr=" << hrText(hr) << std::endl;
        if (FAILED(hr) || maximumDynamicObjects < 1) {
            recordFailure(Outcome::Inconclusive, L"hrtfDynamicCapacity", hr);
            break;
        }

        hr = spatialClient->GetSupportedAudioObjectFormatEnumerator(&formatEnumerator);
        if (FAILED(hr) || !formatEnumerator) {
            recordFailure(Outcome::Inconclusive, L"formatEnumerator", hr);
            break;
        }
        formatEnumerator->GetCount(&formatCount);
        for (UINT32 index = 0; index < formatCount; ++index) {
            WAVEFORMATEX *candidate = nullptr;
            if (SUCCEEDED(formatEnumerator->GetFormat(index, &candidate)) && isMonoFloat48(candidate)) {
                selectedFormat = candidate;
                break;
            }
            if (candidate) {
                CoTaskMemFree(candidate);
            }
        }
        if (!selectedFormat) {
            recordFailure(Outcome::Inconclusive, L"monoFloat48Format", S_FALSE);
            break;
        }
        std::wcout << L"objectFormatRate=" << selectedFormat->nSamplesPerSec
                   << L" objectFormatChannels=" << selectedFormat->nChannels
                   << L" objectFormatBits=" << selectedFormat->wBitsPerSample << std::endl;

        eventHandle = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!eventHandle) {
            recordFailure(Outcome::Fail, L"createEvent", HRESULT_FROM_WIN32(GetLastError()));
            break;
        }
        notify = new CapacityNotify(1);
        activation.ObjectFormat = selectedFormat;
        activation.StaticObjectTypeMask = AudioObjectType_None;
        activation.MinDynamicObjectCount = 1;
        activation.MaxDynamicObjectCount = 1;
        activation.Category = AudioCategory_Media;
        activation.EventHandle = eventHandle;
        activation.NotifyObject = notify;
        activation.DistanceDecay = nullptr;
        activation.Directivity = &directivity;
        activation.Environment = &environment;
        activation.Orientation = &orientation;
        activationProperty.vt = VT_BLOB;
        activationProperty.blob.cbSize = sizeof(activation);
        activationProperty.blob.pBlobData = reinterpret_cast<BYTE *>(&activation);
        hr = spatialClient->ActivateSpatialAudioStream(
            &activationProperty,
            __uuidof(ISpatialAudioObjectRenderStreamForHrtf),
            reinterpret_cast<void **>(&stream));
        activationProperty.vt = VT_EMPTY;
        if (FAILED(hr) || !stream) {
            recordFailure(Outcome::Inconclusive, L"activateHrtfSpatialStream", hr);
            break;
        }
        hr = stream->Start();
        if (FAILED(hr)) {
            recordFailure(Outcome::Inconclusive, L"startHrtfSpatialStream", hr);
            break;
        }
        started = true;
        outcome = Outcome::Pass;
        const ULONGLONG startTick = GetTickCount64();
        while (GetTickCount64() - startTick < options.durationMs) {
            const DWORD waitResult = WaitForSingleObject(eventHandle, 100);
            if (waitResult == WAIT_TIMEOUT) {
                ++timeoutCount;
                continue;
            }
            if (waitResult == WAIT_FAILED) {
                recordFailure(Outcome::Inconclusive, L"waitForHrtfEvent",
                              HRESULT_FROM_WIN32(GetLastError()));
                break;
            }
            ++updateAttempts;
            UINT32 availableDynamicObjects = 0;
            UINT32 frameCount = 0;
            hr = stream->BeginUpdatingAudioObjects(&availableDynamicObjects, &frameCount);
            if (FAILED(hr)) {
                recordFailure(Outcome::Inconclusive, L"beginHrtfUpdate", hr);
                break;
            }
            updateOpen = true;
            bool passOk = hasSufficientCapacity(1, availableDynamicObjects);
            if (!passOk) {
                recordFailure(Outcome::Inconclusive, L"hrtfDynamicCapacityDuringUpdate", S_FALSE);
            }
            if (notify->belowContract()) {
                passOk = false;
                recordFailure(Outcome::Inconclusive, L"hrtfCapacityChangeBelowContract", S_FALSE);
            }
            if (passOk && firstPass) {
                hr = stream->ActivateSpatialAudioObjectForHrtf(AudioObjectType_Dynamic, &object);
                if (FAILED(hr) || !object) {
                    passOk = false;
                    recordFailure(Outcome::Inconclusive, L"activateHrtfDynamicObject", hr);
                } else {
                    activatedDynamicObjectsCount = 1;
                    hr = object->SetEnvironment(environment);
                    if (FAILED(hr)) {
                        passOk = false;
                        recordFailure(Outcome::Inconclusive, L"setHrtfEnvironment", hr);
                    }
                    if (passOk) {
                        hr = object->SetDirectivity(&directivity);
                        if (FAILED(hr)) {
                            passOk = false;
                            recordFailure(Outcome::Inconclusive, L"setHrtfDirectivity", hr);
                        }
                    }
                    if (passOk) {
                        hr = object->SetOrientation(&orientation);
                        if (FAILED(hr)) {
                            passOk = false;
                            recordFailure(Outcome::Inconclusive, L"setHrtfOrientation", hr);
                        }
                    }
                }
            }
            if (passOk && object) {
                BOOL active = FALSE;
                hr = object->IsActive(&active);
                if (FAILED(hr) || !active) {
                    passOk = false;
                    recordFailure(Outcome::Inconclusive, L"isActiveHrtfObject", hr);
                }
            }
            BYTE *buffer = nullptr;
            UINT32 bufferLength = 0;
            if (passOk) {
                hr = object->GetBuffer(&buffer, &bufferLength);
                if (FAILED(hr) || !buffer) {
                    passOk = false;
                    recordFailure(Outcome::Inconclusive, L"getHrtfBuffer", hr);
                } else if (!bufferLengthMatchesFrameCount(bufferLength, frameCount)) {
                    passOk = false;
                    recordFailure(Outcome::Inconclusive, L"hrtfBufferSizeMismatch", S_FALSE);
                }
            }
            if (passOk) {
                ++bufferCalls;
                std::memset(buffer, 0, bufferLength);
                auto *samples = reinterpret_cast<float *>(buffer);
                for (UINT32 frame = 0; frame < frameCount; ++frame) {
                    samples[frame] = syntheticSample(options.frequency, 0,
                                                     totalFrames + frame,
                                                     selectedFormat->nSamplesPerSec,
                                                     options.signal, options.impulseDelayMs);
                    if (!std::isfinite(samples[frame])) {
                        passOk = false;
                        recordFailure(Outcome::Inconclusive, L"nonFiniteHrtfPcm", E_FAIL);
                        break;
                    }
                    ++generatedFiniteCount;
                    generatedPeak = (std::max)(generatedPeak, std::abs(samples[frame]));
                }
            }
            if (passOk) {
                const Position position = selectedPosition(options, 0, totalFrames,
                                                           selectedFormat->nSamplesPerSec);
                if (!isFinitePosition(position)) {
                    passOk = false;
                    recordFailure(Outcome::Inconclusive, L"nonFiniteHrtfPosition", E_FAIL);
                } else {
                    hr = object->SetPosition(position.x, position.y, position.z);
                    if (FAILED(hr)) {
                        passOk = false;
                        recordFailure(Outcome::Inconclusive, L"setHrtfPosition", hr);
                    } else {
                        ++positionCalls;
                    }
                }
            }
            hr = stream->EndUpdatingAudioObjects();
            updateOpen = false;
            if (FAILED(hr)) {
                recordFailure(Outcome::Inconclusive, L"endHrtfUpdate", hr);
                break;
            }
            if (!passOk) {
                break;
            }
            ++updateSuccesses;
            submittedFrames += frameCount;
            if (firstPass) {
                firstUpdateFrames = frameCount;
                firstPass = false;
            }
            totalFrames += frameCount;
        }
        if (outcome == Outcome::Pass && submittedFrames < expectedFrames) {
            recordFailure(Outcome::Inconclusive, L"hrtfSubmittedFrameCoverage", S_FALSE);
        }
        if (outcome == Outcome::Pass && updateSuccesses == 0) {
            recordFailure(Outcome::Inconclusive, L"noSuccessfulHrtfUpdates", S_FALSE);
        }
    } while (false);

    if (updateOpen && stream) {
        const HRESULT endHr = stream->EndUpdatingAudioObjects();
        updateOpen = false;
        if (FAILED(endHr) && firstFailureStage.empty()) {
            firstFailureStage = L"endHrtfUpdateCleanup";
            firstFailureHr = endHr;
            outcome = Outcome::Inconclusive;
        }
    }
    if (stream && started) {
        const HRESULT stopHr = stream->Stop();
        if (FAILED(stopHr) && firstFailureStage.empty()) {
            firstFailureStage = L"stopHrtfSpatialStream";
            firstFailureHr = stopHr;
            outcome = Outcome::Inconclusive;
        }
    }
    release(object);
    if (stream) {
        resetAttempted = true;
        const HRESULT resetHr = stream->Reset();
        resetSucceeded = SUCCEEDED(resetHr);
        if (FAILED(resetHr)) {
            cleanupComplete = false;
            if (firstFailureStage.empty()) {
                firstFailureStage = L"resetHrtfSpatialStream";
                firstFailureHr = resetHr;
                outcome = Outcome::Inconclusive;
            }
        }
    }
    release(stream);
    if (eventHandle) {
        CloseHandle(eventHandle);
        eventHandle = nullptr;
    }
    if (notify) {
        capacityChangeNotifications = notify->notifications();
        latestCapacity = notify->latest();
        notify->Release();
        notify = nullptr;
    }
    PropVariantClear(&activationProperty);
    if (selectedFormat) {
        CoTaskMemFree(selectedFormat);
    }
    release(formatEnumerator);
    release(spatialClient);
    release(device);
    release(enumerator);
    if (shouldUninitialize) {
        CoUninitialize();
    }
    cleanupComplete = cleanupComplete && !object && !stream && !eventHandle && !notify;

    const bool metricConsistency = generatedFiniteCount == submittedFrames
        && bufferCalls == updateSuccesses && positionCalls == updateSuccesses;
    std::wcout << L"hrtfEnvironment=" << hrtfEnvironmentText(options.hrtfEnvironment)
               << L" staticObjectMask=none distanceDecay=activation-null-default"
               << L" directivity=omnidirectional orientation=identity" << std::endl;
    std::wcout << L"updateAttempts=" << updateAttempts
               << L" updateSuccesses=" << updateSuccesses
               << L" submittedFrames=" << submittedFrames
               << L" bufferCalls=" << bufferCalls
               << L" positionCalls=" << positionCalls
               << L" timeoutCount=" << timeoutCount
               << L" firstUpdateFrames=" << firstUpdateFrames << std::endl;
    std::wcout << L"requestedDynamicObjects=1 activatedDynamicObjects=" << activatedDynamicObjectsCount
               << L" maxDynamicObjects=" << maximumDynamicObjects
               << L" capacityChangeNotifications=" << capacityChangeNotifications
               << L" latestCapacity=" << latestCapacity << std::endl;
    std::wcout << L"expectedFrames=" << expectedFrames
               << L" generatedFiniteCount=" << generatedFiniteCount
               << L" generatedPeak=" << generatedPeak
               << L" metricConsistency=" << (metricConsistency ? L"PASS" : L"FAIL")
               << L" resetAttempted=" << (resetAttempted ? 1 : 0)
               << L" resetSucceeded=" << (resetSucceeded ? 1 : 0)
               << L" cleanupComplete=" << (cleanupComplete ? 1 : 0) << std::endl;
    if (!firstFailureStage.empty()) {
        std::wcout << L"firstFailureStage=" << firstFailureStage
                   << L" firstFailureHresult=" << hrText(firstFailureHr) << std::endl;
    }
    std::wcout << L"probeResult=" << outcomeText(outcome)
               << L" evidenceLimit=hrtf-endpoint-submission-only;loopback-required"
               << std::endl;
    return outcome == Outcome::Pass ? 0 : 1;
}

int runLive(const Options &options)
{
    return options.renderer == Options::Renderer::Hrtf
        ? runHrtfLive(options)
        : runStandardLive(options);
}

} // namespace

int wmain(int argc, wchar_t **argv)
{
    Options options;
    if (!parseOptions(argc, argv, &options)) {
        printUsage();
        return 2;
    }
    if (options.selfTest) {
        return runSelfTest() ? 0 : 1;
    }
    return runLive(options);
}
