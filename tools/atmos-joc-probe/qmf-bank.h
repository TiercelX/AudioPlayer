#pragma once

#include <array>
#include <complex>
#include <cstddef>
#include <string>
#include <vector>

namespace eac3qmf {

constexpr std::size_t kSubbands = 64;
constexpr std::size_t kFilterLength = 640;
// Measured and fixed by the shared Gate 4 analysis/synthesis convention.
constexpr int kAnalysisSynthesisDelaySamples = 577;
using Complex = std::complex<double>;

bool loadQwin(const std::string &path, std::vector<double> *qwin, std::string *reason);

class Qmf
{
public:
    explicit Qmf(const std::vector<double> &qwin);

    void reset();
    std::vector<Complex> analyzeBlock(const float *pcm);
    std::array<float, kSubbands> synthesizeBlock(const std::vector<Complex> &q);

private:
    const std::vector<double> &qwin_;
    std::vector<double> analysisState_;
    std::vector<double> synthesisState_;
};

} // namespace eac3qmf
