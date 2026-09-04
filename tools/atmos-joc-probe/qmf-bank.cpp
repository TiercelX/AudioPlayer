// Shared Gate 4 QMF analysis/synthesis bank. The normative prot64/QWIN table
// is loaded from the user's local reference file at runtime and is not
// redistributed by this diagnostic.

#include "qmf-bank.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>

namespace eac3qmf {
namespace {

constexpr std::size_t kStateLength = 2 * kFilterLength;
constexpr double kPi = 3.14159265358979323846;
constexpr double kAnalysisIndexOffset = -0.5;
constexpr double kSynthesisIndexOffset = 1.0;
constexpr double kAnalysisImaginarySign = 1.0;
constexpr double kSynthesisImaginarySign = 1.0;

const std::array<std::array<Complex, kSubbands>, 2 * kSubbands> &synthesisKernel()
{
    static const auto kernel = [] {
        std::array<std::array<Complex, kSubbands>, 2 * kSubbands> values {};
        for (std::size_t index = 0; index < 2 * kSubbands; ++index) {
            for (std::size_t sb = 0; sb < kSubbands; ++sb) {
                const double angle =
                    kPi / (4.0 * kSubbands) * (2.0 * sb + 1.0)
                    * (2.0 * index - 4.0 * kSubbands + kSynthesisIndexOffset);
                values[index][sb] = Complex {
                    std::cos(angle), kSynthesisImaginarySign * std::sin(angle)};
            }
        }
        return values;
    }();
    return kernel;
}

const std::array<std::array<Complex, 2 * kSubbands>, kSubbands> &analysisKernel()
{
    static const auto kernel = [] {
        std::array<std::array<Complex, 2 * kSubbands>, kSubbands> values {};
        for (std::size_t sb = 0; sb < kSubbands; ++sb) {
            for (std::size_t index = 0; index < 2 * kSubbands; ++index) {
                const double angle =
                    kPi * (static_cast<double>(sb) + 0.5)
                    * (static_cast<double>(index) + kAnalysisIndexOffset)
                    / static_cast<double>(kSubbands);
                values[sb][index] = Complex {
                    std::cos(angle), kAnalysisImaginarySign * std::sin(angle)};
            }
        }
        return values;
    }();
    return kernel;
}

} // namespace

bool loadQwin(const std::string &path, std::vector<double> *qwin, std::string *reason)
{
    if (!qwin || !reason) {
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        *reason = "open-failed";
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const std::size_t marker = text.find("prot64[640]");
    if (marker == std::string::npos) {
        *reason = "prot64-640-marker-not-found";
        return false;
    }
    const std::size_t equals = text.find('=', marker);
    const std::size_t end = text.find("};", equals == std::string::npos ? marker : equals);
    if (equals == std::string::npos || end == std::string::npos) {
        *reason = "prot64-initializer-not-found";
        return false;
    }
    qwin->clear();
    std::size_t offset = equals + 1;
    while (offset < end && qwin->size() < kFilterLength) {
        const char character = text[offset];
        if (character == '+' || character == '-' || character == '.'
            || (character >= '0' && character <= '9')) {
            char *parsedEnd = nullptr;
            const double value = std::strtod(text.c_str() + offset, &parsedEnd);
            if (parsedEnd != text.c_str() + offset) {
                qwin->push_back(value);
                offset = static_cast<std::size_t>(parsedEnd - text.c_str());
                continue;
            }
        }
        ++offset;
    }
    if (qwin->size() != kFilterLength) {
        *reason = "prot64-coefficient-count=" + std::to_string(qwin->size());
        return false;
    }
    return true;
}

Qmf::Qmf(const std::vector<double> &qwin)
    : qwin_(qwin), analysisState_(kFilterLength, 0.0), synthesisState_(kStateLength, 0.0)
{
}

void Qmf::reset()
{
    std::fill(analysisState_.begin(), analysisState_.end(), 0.0);
    std::fill(synthesisState_.begin(), synthesisState_.end(), 0.0);
}

std::vector<Complex> Qmf::analyzeBlock(const float *pcm)
{
    const auto &kernel = analysisKernel();
    for (std::size_t index = kFilterLength; index-- > kSubbands;) {
        analysisState_[index] = analysisState_[index - kSubbands];
    }
    for (std::size_t index = 0; index < kSubbands; ++index) {
        analysisState_[index] = pcm[kSubbands - 1 - index];
    }

    double z[kFilterLength] = {};
    for (std::size_t index = 0; index < kFilterLength; ++index) {
        z[index] = analysisState_[index] * qwin_[index];
    }
    double u[2 * kSubbands] = {};
    for (std::size_t index = 0; index < 2 * kSubbands; ++index) {
        for (std::size_t k = 0; k < kFilterLength / (2 * kSubbands); ++k) {
            u[index] += z[index + k * 2 * kSubbands];
        }
    }

    std::vector<Complex> output(kSubbands, Complex {0.0, 0.0});
    for (std::size_t sb = 0; sb < kSubbands; ++sb) {
        for (std::size_t index = 0; index < 2 * kSubbands; ++index) {
            output[sb] += u[index] * kernel[sb][index];
        }
    }
    return output;
}

std::array<float, kSubbands> Qmf::synthesizeBlock(const std::vector<Complex> &q)
{
    const auto &kernel = synthesisKernel();
    for (std::size_t index = kStateLength; index-- > 2 * kSubbands;) {
        synthesisState_[index] = synthesisState_[index - 2 * kSubbands];
    }
    for (std::size_t index = 0; index < 2 * kSubbands; ++index) {
        double value = 0.0;
        for (std::size_t sb = 0; sb < kSubbands; ++sb) {
            value += std::real(q[sb] / static_cast<double>(kSubbands)
                               * kernel[index][sb]);
        }
        synthesisState_[index] = value;
    }

    double g[kFilterLength] = {};
    for (std::size_t block = 0; block < kFilterLength / (2 * kSubbands); ++block) {
        for (std::size_t sb = 0; sb < kSubbands; ++sb) {
            g[2 * kSubbands * block + sb] = synthesisState_[4 * kSubbands * block + sb];
            g[2 * kSubbands * block + kSubbands + sb]
                = synthesisState_[4 * kSubbands * block + 3 * kSubbands + sb];
        }
    }
    double w[kFilterLength] = {};
    for (std::size_t index = 0; index < kFilterLength; ++index) {
        w[index] = g[index] * qwin_[index];
    }
    std::array<double, kSubbands> accumulated {};
    for (std::size_t sample = 0; sample < kSubbands; ++sample) {
        for (std::size_t block = 0; block < kFilterLength / kSubbands; ++block) {
            accumulated[sample] += w[kSubbands * block + sample];
        }
    }
    std::array<float, kSubbands> output {};
    for (std::size_t sample = 0; sample < kSubbands; ++sample) {
        output[sample] = static_cast<float>(accumulated[sample]);
    }
    return output;
}

} // namespace eac3qmf
