#include "native-eac3-transform.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cfloat>
#include <limits>

namespace eac3native {
namespace {

constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr std::size_t kLongIfft = 128;
constexpr std::size_t kShortIfft = 64;

// ETSI TS 102 366 V1.4.1 Table 6.33, w[addr], addr = 10*A+B.
constexpr std::array<double, kEac3OutputSamples> kTransformWindow = {
    0.00014, 0.00024, 0.00037, 0.00051, 0.00067, 0.00086, 0.00107, 0.00130, 0.00157, 0.00187,
    0.00220, 0.00256, 0.00297, 0.00341, 0.00390, 0.00443, 0.00501, 0.00564, 0.00632, 0.00706,
    0.00785, 0.00871, 0.00962, 0.01061, 0.01166, 0.01279, 0.01399, 0.01526, 0.01662, 0.01806,
    0.01959, 0.02121, 0.02292, 0.02472, 0.02662, 0.02863, 0.03073, 0.03294, 0.03527, 0.03770,
    0.04025, 0.04292, 0.04571, 0.04862, 0.05165, 0.05481, 0.05810, 0.06153, 0.06508, 0.06878,
    0.07261, 0.07658, 0.08069, 0.08495, 0.08935, 0.09389, 0.09859, 0.10343, 0.10842, 0.11356,
    0.11885, 0.12429, 0.12988, 0.13563, 0.14152, 0.14757, 0.15376, 0.16011, 0.16661, 0.17325,
    0.18005, 0.18699, 0.19407, 0.20130, 0.20867, 0.21618, 0.22382, 0.23161, 0.23952, 0.24757,
    0.25574, 0.26404, 0.27246, 0.28100, 0.28965, 0.29841, 0.30729, 0.31626, 0.32533, 0.33450,
    0.34376, 0.35311, 0.36253, 0.37204, 0.38161, 0.39126, 0.40096, 0.41072, 0.42054, 0.43040,
    0.44030, 0.45023, 0.46020, 0.47019, 0.48020, 0.49022, 0.50025, 0.51028, 0.52031, 0.53033,
    0.54033, 0.55031, 0.56026, 0.57019, 0.58007, 0.58991, 0.59970, 0.60944, 0.61912, 0.62873,
    0.63827, 0.64774, 0.65713, 0.66643, 0.67564, 0.68476, 0.69377, 0.70269, 0.71150, 0.72019,
    0.72877, 0.73723, 0.74557, 0.75378, 0.76186, 0.76981, 0.77762, 0.78530, 0.79283, 0.80022,
    0.80747, 0.81457, 0.82151, 0.82831, 0.83496, 0.84145, 0.84779, 0.85398, 0.86001, 0.86588,
    0.87160, 0.87716, 0.88257, 0.88782, 0.89291, 0.89785, 0.90264, 0.90728, 0.91176, 0.91610,
    0.92028, 0.92432, 0.92822, 0.93197, 0.93558, 0.93906, 0.94240, 0.94560, 0.94867, 0.95162,
    0.95444, 0.95713, 0.95971, 0.96217, 0.96451, 0.96674, 0.96887, 0.97089, 0.97281, 0.97463,
    0.97635, 0.97799, 0.97953, 0.98099, 0.98236, 0.98366, 0.98488, 0.98602, 0.98710, 0.98811,
    0.98905, 0.98994, 0.99076, 0.99153, 0.99225, 0.99291, 0.99353, 0.99411, 0.99464, 0.99513,
    0.99558, 0.99600, 0.99639, 0.99674, 0.99706, 0.99736, 0.99763, 0.99788, 0.99811, 0.99831,
    0.99850, 0.99867, 0.99882, 0.99895, 0.99908, 0.99919, 0.99929, 0.99938, 0.99946, 0.99953,
    0.99959, 0.99965, 0.99969, 0.99974, 0.99978, 0.99981, 0.99984, 0.99986, 0.99988, 0.99990,
    0.99992, 0.99993, 0.99994, 0.99995, 0.99996, 0.99997, 0.99998, 0.99998, 0.99998, 0.99999,
    0.99999, 0.99999, 0.99999, 1.00000, 1.00000, 1.00000, 1.00000, 1.00000, 1.00000, 1.00000,
    1.00000, 1.00000, 1.00000, 1.00000, 1.00000, 1.00000,
};

double saturatingAdd(double a, double b)
{
    constexpr double kMax = std::numeric_limits<double>::max();
    if (b > 0.0 && a > kMax - b)
        return kMax;
    if (b < 0.0 && a < -kMax - b)
        return -kMax;
    return a + b;
}

double saturatingMultiply(double a, double b)
{
    constexpr double kMax = std::numeric_limits<double>::max();
    if (a == 0.0 || b == 0.0)
        return 0.0;
    const double magnitude = std::abs(a) * std::abs(b);
    if (!std::isfinite(magnitude))
        return std::signbit(a) == std::signbit(b) ? kMax : -kMax;
    return magnitude * (std::signbit(a) == std::signbit(b) ? 1.0 : -1.0);
}

double saturatingSumProduct(double a, double b, double c)
{
    return saturatingAdd(a, saturatingMultiply(b, c));
}

bool finiteVector(const std::vector<double>& values)
{
    return std::all_of(values.begin(), values.end(), [](double value) {
        return std::isfinite(value);
    });
}

} // namespace

Eac3TransformChannel::Eac3TransformChannel()
    : overlap_(kEac3OutputSamples, 0.0)
{
}

void Eac3TransformChannel::reset()
{
    std::fill(overlap_.begin(), overlap_.end(), 0.0);
    blocksProcessed_ = 0;
    hasOverlap_ = false;
    flushed_ = false;
}

std::vector<double> Eac3TransformChannel::inverseLong(
    const std::vector<double>& coefficients) const
{
    std::array<double, kLongIfft> zr{};
    std::array<double, kLongIfft> zi{};
    std::array<double, kLongIfft> yr{};
    std::array<double, kLongIfft> yi{};

    // TS §6.9.4.1, steps 2-4: 128-point complex IFFT with the prescribed
    // pre/post phase rotations. The loops are intentionally scalar and
    // unoptimized so the reference math remains directly auditable.
    for (std::size_t k = 0; k < kLongIfft; ++k) {
        const double c = -std::cos(2.0 * kPi * (8.0 * k + 1.0)
                                   / (8.0 * kEac3TransformSamples));
        const double s = -std::sin(2.0 * kPi * (8.0 * k + 1.0)
                                   / (8.0 * kEac3TransformSamples));
        const double odd = coefficients[kEac3TransformCoefficients - 2 * k - 1];
        const double even = coefficients[2 * k];
        zr[k] = saturatingAdd(saturatingMultiply(odd, c),
                              -saturatingMultiply(even, s));
        zi[k] = saturatingAdd(saturatingMultiply(even, c),
                              saturatingMultiply(odd, s));
    }

    std::array<double, kLongIfft> ifftR{};
    std::array<double, kLongIfft> ifftI{};
    for (std::size_t n = 0; n < kLongIfft; ++n) {
        for (std::size_t k = 0; k < kLongIfft; ++k) {
            const double angle = 8.0 * kPi * k * n
                                 / kEac3TransformSamples;
            const double c = std::cos(angle);
            const double s = std::sin(angle);
            ifftR[n] = saturatingSumProduct(ifftR[n], zr[k], c);
            ifftR[n] = saturatingSumProduct(ifftR[n], -zi[k], s);
            ifftI[n] = saturatingSumProduct(ifftI[n], zi[k], c);
            ifftI[n] = saturatingSumProduct(ifftI[n], zr[k], s);
        }
    }

    for (std::size_t n = 0; n < kLongIfft; ++n) {
        const double c = -std::cos(2.0 * kPi * (8.0 * n + 1.0)
                                   / (8.0 * kEac3TransformSamples));
        const double s = -std::sin(2.0 * kPi * (8.0 * n + 1.0)
                                   / (8.0 * kEac3TransformSamples));
        yr[n] = saturatingAdd(saturatingMultiply(ifftR[n], c),
                              -saturatingMultiply(ifftI[n], s));
        yi[n] = saturatingAdd(saturatingMultiply(ifftI[n], c),
                              saturatingMultiply(ifftR[n], s));
    }

    std::vector<double> windowed(kEac3TransformSamples, 0.0);
    for (std::size_t n = 0; n < kEac3OutputSamples / 4; ++n) {
        windowed[2 * n] = -saturatingMultiply(yi[kEac3OutputSamples / 4 + n],
                                               kTransformWindow[2 * n]);
        windowed[2 * n + 1] = saturatingMultiply(
            yr[kEac3OutputSamples / 4 - n - 1], kTransformWindow[2 * n + 1]);
        windowed[kEac3OutputSamples / 2 + 2 * n] = -saturatingMultiply(
            yr[n], kTransformWindow[kEac3OutputSamples / 2 + 2 * n]);
        windowed[kEac3OutputSamples / 2 + 2 * n + 1] = saturatingMultiply(
            yi[kEac3OutputSamples / 2 - n - 1],
            kTransformWindow[kEac3OutputSamples / 2 + 2 * n + 1]);
        windowed[kEac3OutputSamples + 2 * n] = -saturatingMultiply(
            yr[kEac3OutputSamples / 4 + n],
            kTransformWindow[kEac3OutputSamples - 2 * n - 1]);
        windowed[kEac3OutputSamples + 2 * n + 1] = saturatingMultiply(
            yi[kEac3OutputSamples / 4 - n - 1],
            kTransformWindow[kEac3OutputSamples - 2 * n - 2]);
        windowed[3 * kEac3OutputSamples / 2 + 2 * n] = saturatingMultiply(
            yi[n], kTransformWindow[kEac3OutputSamples / 2 - 2 * n - 1]);
        windowed[3 * kEac3OutputSamples / 2 + 2 * n + 1] = -saturatingMultiply(
            yr[kEac3OutputSamples / 2 - n - 1],
            kTransformWindow[kEac3OutputSamples / 2 - 2 * n - 2]);
    }
    return windowed;
}

std::vector<double> Eac3TransformChannel::inverseSwitched(
    const std::vector<double>& coefficients) const
{
    std::array<double, kShortIfft> zr1{};
    std::array<double, kShortIfft> zi1{};
    std::array<double, kShortIfft> zr2{};
    std::array<double, kShortIfft> zi2{};
    std::array<double, kShortIfft> yr1{};
    std::array<double, kShortIfft> yi1{};
    std::array<double, kShortIfft> yr2{};
    std::array<double, kShortIfft> yi2{};

    // TS §6.9.4.2, steps 1-4: split even/odd transmitted coefficients and
    // perform two 64-point scalar inverse transforms.
    for (std::size_t k = 0; k < kShortIfft; ++k) {
        const double c = -std::cos(2.0 * kPi * (8.0 * k + 1.0)
                                   / (4.0 * kEac3TransformSamples));
        const double s = -std::sin(2.0 * kPi * (8.0 * k + 1.0)
                                   / (4.0 * kEac3TransformSamples));
        const double x1 = coefficients[4 * k];
        const double x2 = coefficients[4 * k + 1];
        // x1/x2 are the even/odd de-interleaved 128-entry vectors from
        // §6.9.4.2 step 1, so their mirrored entries map back to 254-4*k
        // and 255-4*k in the transmitted 256-entry coefficient vector.
        zr1[k] = saturatingAdd(saturatingMultiply(coefficients[254 - 4 * k], c),
                               -saturatingMultiply(x1, s));
        zi1[k] = saturatingAdd(saturatingMultiply(x1, c),
                               saturatingMultiply(coefficients[254 - 4 * k], s));
        zr2[k] = saturatingAdd(saturatingMultiply(coefficients[255 - 4 * k], c),
                               -saturatingMultiply(x2, s));
        zi2[k] = saturatingAdd(saturatingMultiply(x2, c),
                               saturatingMultiply(coefficients[255 - 4 * k], s));
    }

    std::array<double, kShortIfft> ifftR1{};
    std::array<double, kShortIfft> ifftI1{};
    std::array<double, kShortIfft> ifftR2{};
    std::array<double, kShortIfft> ifftI2{};
    for (std::size_t n = 0; n < kShortIfft; ++n) {
        for (std::size_t k = 0; k < kShortIfft; ++k) {
            const double angle = 16.0 * kPi * k * n
                                 / kEac3TransformSamples;
            const double c = std::cos(angle);
            const double s = std::sin(angle);
            ifftR1[n] = saturatingSumProduct(ifftR1[n], zr1[k], c);
            ifftR1[n] = saturatingSumProduct(ifftR1[n], -zi1[k], s);
            ifftI1[n] = saturatingSumProduct(ifftI1[n], zi1[k], c);
            ifftI1[n] = saturatingSumProduct(ifftI1[n], zr1[k], s);
            ifftR2[n] = saturatingSumProduct(ifftR2[n], zr2[k], c);
            ifftR2[n] = saturatingSumProduct(ifftR2[n], -zi2[k], s);
            ifftI2[n] = saturatingSumProduct(ifftI2[n], zi2[k], c);
            ifftI2[n] = saturatingSumProduct(ifftI2[n], zr2[k], s);
        }
    }

    for (std::size_t n = 0; n < kShortIfft; ++n) {
        const double c = -std::cos(2.0 * kPi * (8.0 * n + 1.0)
                                   / (4.0 * kEac3TransformSamples));
        const double s = -std::sin(2.0 * kPi * (8.0 * n + 1.0)
                                   / (4.0 * kEac3TransformSamples));
        yr1[n] = saturatingAdd(saturatingMultiply(ifftR1[n], c),
                               -saturatingMultiply(ifftI1[n], s));
        yi1[n] = saturatingAdd(saturatingMultiply(ifftI1[n], c),
                               saturatingMultiply(ifftR1[n], s));
        yr2[n] = saturatingAdd(saturatingMultiply(ifftR2[n], c),
                               -saturatingMultiply(ifftI2[n], s));
        yi2[n] = saturatingAdd(saturatingMultiply(ifftI2[n], c),
                               saturatingMultiply(ifftR2[n], s));
    }

    std::vector<double> windowed(kEac3TransformSamples, 0.0);
    for (std::size_t n = 0; n < kShortIfft; ++n) {
        windowed[2 * n] = -saturatingMultiply(yi1[n], kTransformWindow[2 * n]);
        windowed[2 * n + 1] = saturatingMultiply(
            yr1[kShortIfft - n - 1], kTransformWindow[2 * n + 1]);
        windowed[kEac3OutputSamples / 2 + 2 * n] = -saturatingMultiply(
            yr1[n], kTransformWindow[kEac3OutputSamples / 2 + 2 * n]);
        windowed[kEac3OutputSamples / 2 + 2 * n + 1] = saturatingMultiply(
            yi1[kShortIfft - n - 1],
            kTransformWindow[kEac3OutputSamples / 2 + 2 * n + 1]);
        windowed[kEac3OutputSamples + 2 * n] = -saturatingMultiply(
            yr2[n], kTransformWindow[kEac3OutputSamples - 2 * n - 1]);
        windowed[kEac3OutputSamples + 2 * n + 1] = saturatingMultiply(
            yi2[kShortIfft - n - 1],
            kTransformWindow[kEac3OutputSamples - 2 * n - 2]);
        windowed[3 * kEac3OutputSamples / 2 + 2 * n] = saturatingMultiply(
            yi2[n], kTransformWindow[kEac3OutputSamples / 2 - 2 * n - 1]);
        windowed[3 * kEac3OutputSamples / 2 + 2 * n + 1] = -saturatingMultiply(
            yr2[kShortIfft - n - 1],
            kTransformWindow[kEac3OutputSamples / 2 - 2 * n - 2]);
    }
    return windowed;
}

TransformOutput Eac3TransformChannel::finishBlock(
    const std::vector<double>& windowed, bool blockSwitch)
{
    TransformOutput result;
    result.blockSwitch = blockSwitch;
    if (windowed.size() != kEac3TransformSamples || !finiteVector(windowed)) {
        result.error = "transform produced non-finite or wrong-sized windowed block";
        return result;
    }

    result.samples.resize(kEac3OutputSamples);
    for (std::size_t n = 0; n < kEac3OutputSamples; ++n)
        result.samples[n] = saturatingMultiply(
            saturatingAdd(windowed[n], overlap_[n]), 2.0);
    if (!finiteVector(result.samples)) {
        result.samples.clear();
        result.error = "overlap-add produced non-finite samples";
        return result;
    }

    std::copy(windowed.begin() + kEac3OutputSamples,
              windowed.end(), overlap_.begin());
    hasOverlap_ = true;
    ++blocksProcessed_;
    result.ok = true;
    return result;
}

TransformOutput Eac3TransformChannel::processBlock(
    const std::vector<double>& coefficients, bool blockSwitch)
{
    TransformOutput result;
    result.blockSwitch = blockSwitch;
    if (flushed_) {
        result.error = "block submitted after EOS; reset is required";
        return result;
    }
    if (coefficients.size() > kEac3TransformCoefficients) {
        result.error = "coefficient vector exceeds 256 entries";
        return result;
    }
    if (!finiteVector(coefficients)) {
        result.error = "coefficient vector contains non-finite value";
        return result;
    }

    std::vector<double> padded(kEac3TransformCoefficients, 0.0);
    std::copy(coefficients.begin(), coefficients.end(), padded.begin());
    return finishBlock(blockSwitch ? inverseSwitched(padded)
                                   : inverseLong(padded), blockSwitch);
}

TransformFlushOutput Eac3TransformChannel::flush()
{
    TransformFlushOutput result;
    result.ok = true;
    flushed_ = true;
    if (!hasOverlap_)
        return result;

    result.hadTail = true;
    result.samples.resize(kEac3OutputSamples);
    for (std::size_t n = 0; n < kEac3OutputSamples; ++n)
        result.samples[n] = saturatingMultiply(overlap_[n], 2.0);
    if (!finiteVector(result.samples)) {
        result.samples.clear();
        result.ok = false;
        result.error = "EOS overlap tail is non-finite";
        return result;
    }
    std::fill(overlap_.begin(), overlap_.end(), 0.0);
    hasOverlap_ = false;
    return result;
}

} // namespace eac3native
