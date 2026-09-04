#include "bs2127-quad.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <vector>

namespace eac3render {
namespace {

bool finite(double value)
{
    return std::isfinite(value);
}

bool finiteVector(const UnitVector3 &vector)
{
    return std::all_of(vector.begin(), vector.end(),
                       [](double value) { return finite(value); });
}

double dot(const UnitVector3 &left, const UnitVector3 &right)
{
    return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
}

UnitVector3 cross(const UnitVector3 &left, const UnitVector3 &right)
{
    return {
        left[1] * right[2] - left[2] * right[1],
        left[2] * right[0] - left[0] * right[2],
        left[0] * right[1] - left[1] * right[0],
    };
}

double normSquared(const UnitVector3 &vector)
{
    return dot(vector, vector);
}

void reject(QuadSolveResult *result, const char *reason)
{
    result->accepted = false;
    result->reason = reason;
}

bool solvePolynomial(double a, double b, double c,
                     const QuadSolveOptions &options,
                     std::vector<double> *roots)
{
    roots->clear();
    const double scale = std::max({1.0, std::abs(a), std::abs(b), std::abs(c)});
    const double tolerance = options.polynomialTolerance * scale;
    if (std::abs(c) <= tolerance) {
        if (std::abs(b) <= tolerance) {
            return std::abs(a) <= tolerance;
        }
        roots->push_back(-a / b);
        return finite(roots->back());
    }

    double discriminant = b * b - 4.0 * c * a;
    if (!finite(discriminant)) {
        return false;
    }
    const double discriminantTolerance =
        options.polynomialTolerance * std::max({1.0, b * b, std::abs(4.0 * c * a)});
    if (discriminant < 0.0) {
        if (discriminant < -discriminantTolerance) {
            return false;
        }
        discriminant = 0.0;
    }
    const double squareRoot = std::sqrt(discriminant);
    if (!finite(squareRoot)) {
        return false;
    }
    if (squareRoot == 0.0) {
        roots->push_back(-b / (2.0 * c));
    } else {
        // This form avoids losing the smaller root when b and sqrt(D) nearly
        // cancel.  The two roots are sorted below for deterministic choice.
        const double q = -0.5 * (b + std::copysign(squareRoot, b));
        if (q == 0.0) {
            roots->push_back((-b + squareRoot) / (2.0 * c));
            roots->push_back((-b - squareRoot) / (2.0 * c));
        } else {
            roots->push_back(q / c);
            roots->push_back(a / q);
        }
    }
    if (!std::all_of(roots->begin(), roots->end(), finite)) {
        roots->clear();
        return false;
    }
    std::sort(roots->begin(), roots->end());
    roots->erase(std::unique(roots->begin(), roots->end(),
                             [options](double left, double right) {
                                 return std::abs(left - right)
                                     <= options.polynomialTolerance;
                             }),
                 roots->end());
    return !roots->empty();
}

bool inUnitInterval(double value, double tolerance)
{
    return finite(value) && value >= -tolerance && value <= 1.0 + tolerance;
}

double coefficient(const UnitVector3 &left, const UnitVector3 &right,
                   const UnitVector3 &direction)
{
    return dot(cross(left, right), direction);
}

void polynomialForX(const std::array<UnitVector3, 4> &p,
                    const UnitVector3 &direction,
                    double *a, double *b, double *c)
{
    const UnitVector3 p2MinusP1 {
        p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2]};
    const UnitVector3 p3MinusP4 {
        p[2][0] - p[3][0], p[2][1] - p[3][1], p[2][2] - p[3][2]};
    *a = coefficient(p[0], p[3], direction);
    *b = coefficient(p[0], p3MinusP4, direction)
        + coefficient(p2MinusP1, p[3], direction);
    *c = coefficient(p2MinusP1, p3MinusP4, direction);
}

} // namespace

QuadSolveResult solveQuad(
    const std::array<UnitVector3, 4> &speakerVectors,
    const UnitVector3 &sourceVector,
    const QuadSolveOptions &options)
{
    QuadSolveResult result;
    if (!finite(options.polynomialTolerance)
        || options.polynomialTolerance <= 0.0
        || !finite(options.boundaryTolerance)
        || options.boundaryTolerance < 0.0
        || !finite(options.directionTolerance)
        || options.directionTolerance < 0.0
        || !finite(options.unitVectorTolerance)
        || options.unitVectorTolerance < 0.0) {
        reject(&result, "invalid-quad-solver-tolerance");
        return result;
    }
    if (!finiteVector(sourceVector)
        || std::abs(normSquared(sourceVector) - 1.0)
            > options.unitVectorTolerance) {
        reject(&result, "source-vector-not-unit-within-tolerance");
        return result;
    }
    for (const UnitVector3 &speaker : speakerVectors) {
        if (!finiteVector(speaker)
            || std::abs(normSquared(speaker) - 1.0)
                > options.unitVectorTolerance) {
            reject(&result, "speaker-vector-not-unit-within-tolerance");
            return result;
        }
    }
    for (std::size_t first = 0U; first < speakerVectors.size(); ++first) {
        for (std::size_t second = first + 1U;
             second < speakerVectors.size(); ++second) {
            UnitVector3 difference {
                speakerVectors[first][0] - speakerVectors[second][0],
                speakerVectors[first][1] - speakerVectors[second][1],
                speakerVectors[first][2] - speakerVectors[second][2],
            };
            if (normSquared(difference)
                <= options.polynomialTolerance * options.polynomialTolerance) {
                reject(&result, "duplicate-quad-speaker-vector");
                return result;
            }
        }
    }

    double ax = 0.0;
    double bx = 0.0;
    double cx = 0.0;
    polynomialForX(speakerVectors, sourceVector, &ax, &bx, &cx);
    std::array<UnitVector3, 4> rotated {
        speakerVectors[1], speakerVectors[2], speakerVectors[3], speakerVectors[0]};
    double ay = 0.0;
    double by = 0.0;
    double cy = 0.0;
    polynomialForX(rotated, sourceVector, &ay, &by, &cy);
    std::vector<double> xRoots;
    std::vector<double> yRoots;
    if (!solvePolynomial(ax, bx, cx, options, &xRoots)
        || !solvePolynomial(ay, by, cy, options, &yRoots)) {
        reject(&result, "quad-position-polynomial-has-no-real-root");
        return result;
    }

    for (double x : xRoots) {
        if (!inUnitInterval(x, options.boundaryTolerance)) {
            continue;
        }
        x = std::clamp(x, 0.0, 1.0);
        for (double y : yRoots) {
            if (!inUnitInterval(y, options.boundaryTolerance)) {
                continue;
            }
            y = std::clamp(y, 0.0, 1.0);
            const std::array<double, 4> raw {
                (1.0 - x) * (1.0 - y), x * (1.0 - y), x * y,
                (1.0 - x) * y};
            const double power = std::inner_product(
                raw.begin(), raw.end(), raw.begin(), 0.0);
            if (!finite(power) || power <= std::numeric_limits<double>::min()) {
                continue;
            }
            const double inversePower = 1.0 / std::sqrt(power);
            std::array<double, 4> normalized {};
            for (std::size_t index = 0U; index < raw.size(); ++index) {
                normalized[index] = raw[index] * inversePower;
            }
            UnitVector3 velocity {0.0, 0.0, 0.0};
            for (std::size_t index = 0U; index < 4U; ++index) {
                for (std::size_t component = 0U; component < 3U; ++component) {
                    velocity[component] +=
                        normalized[index] * speakerVectors[index][component];
                }
            }
            const double scale = dot(velocity, sourceVector);
            const UnitVector3 crossVelocity = cross(velocity, sourceVector);
            const double residual = std::sqrt(normSquared(crossVelocity));
            if (!finite(scale) || !finite(residual) || scale <= 0.0
                || residual > options.directionTolerance) {
                continue;
            }
            result.accepted = true;
            result.rawGains = raw;
            result.normalizedGains = normalized;
            result.x = x;
            result.y = y;
            result.velocityScale = scale;
            result.reason = "none";
            return result;
        }
    }
    reject(&result, "quad-position-outside-region-or-opposite-direction");
    return result;
}

} // namespace eac3render
