#pragma once

#include "bs2127-triplet.h"

#include <array>
#include <string>

namespace eac3render {

struct QuadSolveOptions {
    // These are numerical acceptance policies, not BS.2127 constants.
    double polynomialTolerance = 1.0e-12;
    double boundaryTolerance = 1.0e-9;
    double directionTolerance = 1.0e-8;
    double unitVectorTolerance = 1.0e-6;
};

struct QuadSolveResult {
    bool accepted = false;
    std::array<double, 4> rawGains {0.0, 0.0, 0.0, 0.0};
    std::array<double, 4> normalizedGains {0.0, 0.0, 0.0, 0.0};
    double x = 0.0;
    double y = 0.0;
    double velocityScale = 0.0;
    std::string reason;
};

// BS.2127-1 §6.1.2.3.1-.3 QuadRegion.  Speaker vectors must be supplied in
// the specification's anticlockwise order as viewed from the listener.
// This is a scalar primitive only: it performs no layout selection,
// downmixing, renderer scheduling, or room/BRIR processing.
QuadSolveResult solveQuad(
    const std::array<UnitVector3, 4> &speakerVectors,
    const UnitVector3 &sourceVector,
    const QuadSolveOptions &options = {});

} // namespace eac3render
