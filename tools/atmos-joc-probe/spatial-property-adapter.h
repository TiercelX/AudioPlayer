#pragma once

#include "oamd-b2b.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace eac3gate7b {

constexpr unsigned kDynamicObjectCount = 15U;

enum class Disposition {
    Pass,
    Unsupported,
    Malformed,
};

const char *dispositionText(Disposition disposition);

enum class PositionRadiusMode {
    Source,
    Unit,
};

const char *positionRadiusModeText(PositionRadiusMode mode);

enum class PositionDirectionMode {
    Metadata,
    Front,
};

const char *positionDirectionModeText(PositionDirectionMode mode);

struct Geometry {
    double roomWidthMetres = 0.0;
    double roomDepthMetres = 0.0;
    double roomHeightMetres = 0.0;
    double listenerX = 0.0;
    double listenerY = 0.0;
    double listenerZ = 0.0;
    double screenBottomLeftX = 0.0;
    double screenBottomLeftY = 0.0;
    double screenBottomLeftZ = 0.0;
    double screenWidth = 0.0;
    double screenHeight = 0.0;
    double gainHeadroomDb = 15.0;
    PositionRadiusMode positionRadiusMode = PositionRadiusMode::Source;
    PositionDirectionMode positionDirectionMode = PositionDirectionMode::Metadata;
    float azimuthFocus = 0.0f;
};

Geometry referenceGeometry();
bool validateGeometry(const Geometry &geometry, std::string *reason);

struct Update {
    std::int64_t sourcePosition = 0;
    unsigned blockIndex = 0;
    unsigned rampDuration = 0;
    unsigned objectIndex = 0;
    eac3oamd::B2bObjectState state;
};

struct WindowsProperty {
    unsigned objectIndex = 0;
    bool active = false;
    bool minusInfinity = true;
    float gainDb = 0.0f;
    float volume = 0.0f;
    std::array<float, 3> position {0.0f, 0.0f, 0.0f};
};

struct RampRecord {
    Update update;
    WindowsProperty start;
    WindowsProperty target;
    bool firstStateSnap = false;
    bool immediateStep = false;
    bool overlap = false;
    bool screenConversion = false;
};

struct ApplyResult {
    Disposition disposition = Disposition::Malformed;
    std::string reason;
    std::vector<RampRecord> records;
};

struct Metrics {
    std::size_t resetCount = 0;
    std::size_t updatesAttempted = 0;
    std::size_t updatesCommitted = 0;
    std::size_t firstStateSnaps = 0;
    std::size_t stepCount = 0;
    std::size_t rampCount = 0;
    std::size_t overlapCount = 0;
    std::size_t screenConversions = 0;
    std::size_t roomConversions = 0;
    std::size_t silentTargets = 0;
    std::size_t unsupportedCount = 0;
    std::size_t rejectedCount = 0;
    std::array<std::size_t, kDynamicObjectCount + 1U> updatesByObject {};
    std::array<float, 3> positionMin {
        std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::infinity()};
    std::array<float, 3> positionMax {
        -std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity()};
    double sourceRadiusMin = std::numeric_limits<double>::infinity();
    double sourceRadiusMax = -std::numeric_limits<double>::infinity();
    double sourceRadiusSum = 0.0;
    double outputRadiusMin = std::numeric_limits<double>::infinity();
    double outputRadiusMax = -std::numeric_limits<double>::infinity();
    double outputRadiusSum = 0.0;
    std::size_t radiusSamples = 0;
    float volumeMin = std::numeric_limits<float>::infinity();
    float volumeMax = -std::numeric_limits<float>::infinity();
    unsigned objectIndexMin = kDynamicObjectCount + 1U;
    unsigned objectIndexMax = 0U;
    std::int64_t firstFailurePosition = -1;
    unsigned firstFailureObject = 0U;
    std::string firstFailureReason;
};

struct SelfTestReport {
    bool pass = false;
    std::size_t cases = 0;
    std::string reason;
};

class Adapter {
public:
    struct RampState {
        bool initialized = false;
        std::int64_t sourcePosition = 0;
        unsigned duration = 0;
        WindowsProperty start;
        WindowsProperty target;
    };

    explicit Adapter(Geometry geometry = referenceGeometry());

    ApplyResult applyBatch(const std::vector<Update> &updates);
    bool evaluate(unsigned objectIndex, std::int64_t samplePosition,
                  WindowsProperty *property, std::string *reason) const;
    void reset();

    const Geometry &geometry() const { return geometry_; }
    const Metrics &metrics() const { return metrics_; }

private:
    Geometry geometry_;
    std::array<RampState, kDynamicObjectCount + 1U> states_ {};
    Metrics metrics_;
};

SelfTestReport runSelfTest();

} // namespace eac3gate7b
