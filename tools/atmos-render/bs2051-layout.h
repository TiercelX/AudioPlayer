#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace eac3render {

constexpr std::size_t kSystemHSpeakerCount = 22U;

enum class LfePolicy {
    SeparateFromPointLayout,
};

constexpr LfePolicy kSystemHLfePolicy = LfePolicy::SeparateFromPointLayout;

enum class Bs2051Label {
    MPlus060,
    MMinus060,
    MPlus000,
    MPlus135,
    MMinus135,
    MPlus030,
    MMinus030,
    MPlus180,
    MPlus090,
    MMinus090,
    UPlus045,
    UMinus045,
    UPlus000,
    TPlus000,
    UPlus135,
    UMinus135,
    UPlus090,
    UMinus090,
    UPlus180,
    BPlus000,
    BPlus045,
    BMinus045,
};

enum class Bs2051Layer {
    Upper,
    Middle,
    Bottom,
};

const char *bs2051LabelName(Bs2051Label label);

struct Bs2051SpeakerPosition {
    Bs2051Label label = Bs2051Label::MPlus000;
    Bs2051Layer layer = Bs2051Layer::Middle;
    double azimuthDegrees = 0.0;
    double elevationDegrees = 0.0;

    // Unit vector convention for this contract is
    // [cos(elevation)cos(azimuth), cos(elevation)sin(azimuth),
    //  sin(elevation)].  This is a coordinate conversion only, not panning.
    std::array<double, 3> unitVector() const;
};

class Bs2051SystemHLayout {
public:
    static const std::array<Bs2051SpeakerPosition, kSystemHSpeakerCount> &systemH();

    static bool find(Bs2051Label label, Bs2051SpeakerPosition *position,
                     std::string *reason = nullptr);
    static bool validateSystemH(std::string *reason = nullptr);
};

// A compact export of SOFA EmitterPosition for offline comparison.  The
// production layout code does not open SOFA/HDF5 files.  An empty description
// means the comparison is using the documented emitter order because the
// cached System H file has one global EmitterDescription, not one per emitter.
struct SofaEmitterRecord {
    std::size_t emitterIndex = 0U;
    std::string description;
    double azimuthDegrees = 0.0;
    double elevationDegrees = 0.0;
    double distanceMetres = 0.0;
};

struct SofaLayoutComparison {
    bool pass = false;
    bool usedEmitterOrder = false;
    bool perEmitterDescriptionMissing = false;
    std::size_t compared = 0U;
    std::size_t mismatches = 0U;
    double maxAzimuthErrorDegrees = 0.0;
    double maxElevationErrorDegrees = 0.0;
    double minimumDistanceMetres = 0.0;
    double maximumDistanceMetres = 0.0;
    std::string reason;
};

SofaLayoutComparison compareSystemHToSofa(
    const std::vector<SofaEmitterRecord> &emitters,
    double azimuthToleranceDegrees = 1.0e-6,
    double elevationToleranceDegrees = 1.0e-6);

} // namespace eac3render
