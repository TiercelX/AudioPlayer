#pragma once

#include <array>
#include <cstddef>
#include <string>

namespace eac3render {

using UnitVector3 = std::array<double, 3>;

struct TripletSolveOptions {
    double determinantTolerance = 1.0e-10;
    double negativeGainTolerance = 1.0e-10;
    double unitVectorTolerance = 1.0e-6;
};

struct TripletSolveResult {
    bool accepted = false;
    UnitVector3 rawGains {0.0, 0.0, 0.0};
    UnitVector3 normalizedGains {0.0, 0.0, 0.0};
    double determinant = 0.0;
    double conditionEstimate = 0.0;
    std::size_t toleranceZeroedNegativeGains = 0U;
    std::string reason;
};

// BS.2127 §6.1.1 triplet primitive only. Speaker vectors are the columns of
// a 3x3 matrix. The result does not select a triplet or perform any layout,
// extent, BRIR, or renderer scheduling operation.
TripletSolveResult solveTriplet(
    const std::array<UnitVector3, 3> &speakerVectors,
    const UnitVector3 &sourceVector,
    const TripletSolveOptions &options = {});

} // namespace eac3render
