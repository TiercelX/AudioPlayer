#include "bs2127-triplet.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace eac3render {
namespace {

void reject(TripletSolveResult *result, const char *reason)
{
    result->accepted = false;
    result->reason = reason;
}

bool finite(double value)
{
    return std::isfinite(value);
}

bool finiteVector(const UnitVector3 &vector)
{
    return std::all_of(vector.begin(), vector.end(), [](double value) {
        return finite(value);
    });
}

double normSquared(const UnitVector3 &vector)
{
    return vector[0] * vector[0]
        + vector[1] * vector[1]
        + vector[2] * vector[2];
}

UnitVector3 cross(const UnitVector3 &left, const UnitVector3 &right)
{
    return {
        left[1] * right[2] - left[2] * right[1],
        left[2] * right[0] - left[0] * right[2],
        left[0] * right[1] - left[1] * right[0],
    };
}

double dot(const UnitVector3 &left, const UnitVector3 &right)
{
    return left[0] * right[0]
        + left[1] * right[1]
        + left[2] * right[2];
}

} // namespace

TripletSolveResult solveTriplet(
    const std::array<UnitVector3, 3> &speakerVectors,
    const UnitVector3 &sourceVector,
    const TripletSolveOptions &options)
{
    TripletSolveResult result;
    if (!finite(options.determinantTolerance)
        || options.determinantTolerance < 0.0
        || !finite(options.negativeGainTolerance)
        || options.negativeGainTolerance < 0.0
        || !finite(options.unitVectorTolerance)
        || options.unitVectorTolerance < 0.0) {
        reject(&result, "invalid-solver-tolerance");
        return result;
    }
    if (!finiteVector(sourceVector)) {
        reject(&result, "nonfinite-source-vector");
        return result;
    }

    const double sourceNormSquared = normSquared(sourceVector);
    if (!finite(sourceNormSquared)
        || std::abs(sourceNormSquared - 1.0) > options.unitVectorTolerance) {
        reject(&result, "source-vector-not-unit-within-tolerance");
        return result;
    }

    for (const UnitVector3 &speaker : speakerVectors) {
        if (!finiteVector(speaker)) {
            reject(&result, "nonfinite-speaker-vector");
            return result;
        }
        const double speakerNormSquared = normSquared(speaker);
        if (!finite(speakerNormSquared)
            || std::abs(speakerNormSquared - 1.0) > options.unitVectorTolerance) {
            reject(&result, "speaker-vector-not-unit-within-tolerance");
            return result;
        }
    }

    const UnitVector3 s0s1Cross = cross(speakerVectors[1], speakerVectors[2]);
    const UnitVector3 s2s0Cross = cross(speakerVectors[2], speakerVectors[0]);
    const UnitVector3 s0s1 = cross(speakerVectors[0], speakerVectors[1]);
    result.determinant = dot(speakerVectors[0], s0s1Cross);
    if (!finite(result.determinant)) {
        reject(&result, "nonfinite-determinant");
        return result;
    }
    if (std::abs(result.determinant) <= options.determinantTolerance) {
        reject(&result, "singular-or-ill-conditioned-triplet");
        return result;
    }

    // For a matrix with speaker vectors as columns, the rows of the inverse
    // are the cross products below divided by determinant.
    const double inverseRow0 =
        (std::abs(s0s1Cross[0]) + std::abs(s0s1Cross[1])
         + std::abs(s0s1Cross[2])) / std::abs(result.determinant);
    const double inverseRow1 =
        (std::abs(s2s0Cross[0]) + std::abs(s2s0Cross[1])
         + std::abs(s2s0Cross[2])) / std::abs(result.determinant);
    const double inverseRow2 =
        (std::abs(s0s1[0]) + std::abs(s0s1[1]) + std::abs(s0s1[2]))
        / std::abs(result.determinant);
    const double matrixRow0 = std::abs(speakerVectors[0][0])
        + std::abs(speakerVectors[1][0]) + std::abs(speakerVectors[2][0]);
    const double matrixRow1 = std::abs(speakerVectors[0][1])
        + std::abs(speakerVectors[1][1]) + std::abs(speakerVectors[2][1]);
    const double matrixRow2 = std::abs(speakerVectors[0][2])
        + std::abs(speakerVectors[1][2]) + std::abs(speakerVectors[2][2]);
    const double matrixNorm = std::max({matrixRow0, matrixRow1, matrixRow2});
    const double inverseNorm = std::max({inverseRow0, inverseRow1, inverseRow2});
    result.conditionEstimate = matrixNorm * inverseNorm;
    if (!finite(result.conditionEstimate)) {
        reject(&result, "nonfinite-condition-estimate");
        return result;
    }

    result.rawGains[0] = dot(sourceVector, s0s1Cross) / result.determinant;
    result.rawGains[1] = dot(sourceVector, s2s0Cross) / result.determinant;
    result.rawGains[2] = dot(sourceVector, s0s1) / result.determinant;
    if (!finiteVector(result.rawGains)) {
        reject(&result, "nonfinite-raw-gain");
        return result;
    }

    for (double &gain : result.rawGains) {
        if (gain < -options.negativeGainTolerance) {
            reject(&result, "source-outside-triplet-negative-gain");
            return result;
        }
        if (gain < 0.0) {
            // Only roundoff-sized negative zero is removed. A meaningful
            // negative gain never reaches this branch and is never clamped.
            gain = 0.0;
            ++result.toleranceZeroedNegativeGains;
        }
    }

    const double gainPower = normSquared(result.rawGains);
    if (!finite(gainPower) || gainPower <= std::numeric_limits<double>::min()) {
        reject(&result, "zero-or-nonfinite-gain-power");
        return result;
    }
    const double inversePower = 1.0 / std::sqrt(gainPower);
    for (std::size_t index = 0U; index < result.rawGains.size(); ++index) {
        result.normalizedGains[index] = result.rawGains[index] * inversePower;
    }
    if (!finiteVector(result.normalizedGains)) {
        reject(&result, "nonfinite-normalized-gain");
        return result;
    }

    result.accepted = true;
    return result;
}

} // namespace eac3render
