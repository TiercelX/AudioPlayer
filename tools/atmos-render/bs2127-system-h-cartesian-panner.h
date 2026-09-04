#pragma once

#include "bs2051-layout.h"

#include <array>
#include <cstddef>
#include <string>

namespace eac3render {

struct Bs2127CartesianPannerResult {
    bool accepted = false;
    std::string reason;
    std::array<double, kSystemHSpeakerCount> gains {};
};

// Probe-local fixed System H Cartesian/allocentric point panner.  The input
// is an allocentric Cartesian point; it is deliberately not a UnitVector3.
class Bs2127SystemHCartesianPanner final {
public:
    Bs2127CartesianPannerResult render(const std::array<double, 3> &point) const;

    static bool mapEtsiRoomToAllocentric(
        const std::array<float, 3> &room, std::array<double, 3> *allocentric,
        std::string *reason = nullptr);
    static const std::array<std::array<double, 3>, kSystemHSpeakerCount> &
    speakerCoordinates();
};

} // namespace eac3render
