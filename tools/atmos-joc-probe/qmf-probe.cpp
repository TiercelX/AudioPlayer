// Gate 4 diagnostic: ETSI TS 103 420 64-band complex QMF analysis and
// synthesis. The normative prot64/QWIN table is loaded from the user's local
// reference file at runtime and is intentionally not redistributed here.

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <string>
#include <iterator>
#include <vector>

#include "qmf-bank.h"

namespace {

using eac3qmf::Qmf;
using eac3qmf::Complex;
constexpr std::size_t kSubbands = eac3qmf::kSubbands;
constexpr std::size_t kFilterLength = eac3qmf::kFilterLength;
constexpr double kPi = 3.14159265358979323846;
constexpr double kMinimumReconstructionSnrDb = 72.0;
// TS 103 420 V1.2.1 Pseudocode 12: (j - 0.5), kept explicit for auditability.
constexpr double kAnalysisIndexOffset = -0.5;
// The matrix N equation immediately above Pseudocode 14 gives
// (j - 2*n + 0.5), or (2*j - 4*n + 1) in the expanded form. Pseudocode 14
// prints a conflicting (2*j - 2*n - 1); ETSI TS 103 190-1 Pseudocode 66
// independently uses the matrix-equation form for this 64-band/640-tap bank.
constexpr double kSynthesisIndexOffset = 1.0;
constexpr double kAnalysisImaginarySign = 1.0;
constexpr double kSynthesisImaginarySign = 1.0;

enum class SynthesisReferenceFormula {
    MatrixEquation,
    Pseudocode14,
};

std::array<float, kSubbands> referenceSynthesisBlock(
    const std::vector<Complex> &q,
    const std::vector<double> &qwin,
    std::vector<double> *state,
    SynthesisReferenceFormula formula)
{
    // Independent scalar implementation of TS 103 420 clause 7.3,
    // Pseudocode 13-17. The two published phase forms are intentionally kept
    // side by side so the internal specification conflict remains observable.
    std::array<float, kSubbands> output {};
    for (std::size_t j = 2 * kFilterLength; j-- > 2 * kSubbands;) {
        (*state)[j] = (*state)[j - 2 * kSubbands];
    }
    for (std::size_t j = 0; j < 2 * kSubbands; ++j) {
        double value = 0.0;
        for (std::size_t sb = 0; sb < kSubbands; ++sb) {
            const double indexTerm = formula == SynthesisReferenceFormula::MatrixEquation
                ? 2.0 * j - 4.0 * kSubbands + kSynthesisIndexOffset
                : 2.0 * j - 2.0 * kSubbands - 1.0;
            const double exponent = kPi / (4.0 * kSubbands)
                * (2.0 * sb + 1.0)
                * indexTerm;
            value += std::real(q[sb] / static_cast<double>(kSubbands)
                               * std::exp(Complex {0.0, exponent}));
        }
        (*state)[j] = value;
    }
    std::array<double, kFilterLength> g {};
    for (std::size_t block = 0; block < kFilterLength / (2 * kSubbands); ++block) {
        for (std::size_t sb = 0; sb < kSubbands; ++sb) {
            g[2 * kSubbands * block + sb] = (*state)[4 * kSubbands * block + sb];
            g[2 * kSubbands * block + kSubbands + sb]
                = (*state)[4 * kSubbands * block + 3 * kSubbands + sb];
        }
    }
    for (std::size_t sample = 0; sample < kSubbands; ++sample) {
        double value = 0.0;
        for (std::size_t block = 0; block < kFilterLength / kSubbands; ++block) {
            value += g[kSubbands * block + sample] * qwin[kSubbands * block + sample];
        }
        output[sample] = static_cast<float>(value);
    }
    return output;
}

float maxAbsDifference(const std::array<float, kSubbands> &left,
                       const std::array<float, kSubbands> &right,
                       std::size_t *firstMismatch)
{
    float maximum = 0.0f;
    if (firstMismatch) {
        *firstMismatch = kSubbands;
    }
    for (std::size_t index = 0; index < kSubbands; ++index) {
        const float difference = std::abs(left[index] - right[index]);
        maximum = std::max(maximum, difference);
        if (difference > 1.0e-6f && firstMismatch && *firstMismatch == kSubbands) {
            *firstMismatch = index;
        }
    }
    return maximum;
}

struct SynthesisReferenceComparison {
    float matrixMaximumError = 0.0f;
    std::size_t matrixFirstMismatch = kSubbands;
    float pseudocode14MaximumError = 0.0f;
    std::size_t pseudocode14FirstMismatch = kSubbands;
};

SynthesisReferenceComparison independentSynthesisReferenceTest(
    const std::vector<double> &qwin)
{
    const std::array<std::size_t, 6> subbands = {0, 1, 17, 32, 48, 63};
    const std::array<Complex, 6> values = {
        Complex {0.11, 0.07}, Complex {-0.23, 0.19}, Complex {0.31, -0.17},
        Complex {-0.41, -0.29}, Complex {0.17, 0.37}, Complex {-0.07, -0.43}};
    Qmf candidate(qwin);
    std::vector<double> matrixState(2 * kFilterLength, 0.0);
    std::vector<double> pseudocode14State(2 * kFilterLength, 0.0);
    SynthesisReferenceComparison comparison;
    for (std::size_t block = 0; block < subbands.size(); ++block) {
        std::vector<Complex> q(kSubbands, Complex {0.0, 0.0});
        q[subbands[block]] = values[block];
        const auto actual = candidate.synthesizeBlock(q);
        const auto matrixExpected = referenceSynthesisBlock(
            q, qwin, &matrixState, SynthesisReferenceFormula::MatrixEquation);
        const auto pseudocode14Expected = referenceSynthesisBlock(
            q, qwin, &pseudocode14State, SynthesisReferenceFormula::Pseudocode14);
        std::size_t matrixFirst = kSubbands;
        std::size_t pseudocode14First = kSubbands;
        comparison.matrixMaximumError = std::max(
            comparison.matrixMaximumError,
            maxAbsDifference(actual, matrixExpected, &matrixFirst));
        comparison.pseudocode14MaximumError = std::max(
            comparison.pseudocode14MaximumError,
            maxAbsDifference(actual, pseudocode14Expected, &pseudocode14First));
        if (comparison.matrixFirstMismatch == kSubbands && matrixFirst != kSubbands) {
            comparison.matrixFirstMismatch = block * kSubbands + matrixFirst;
        }
        if (comparison.pseudocode14FirstMismatch == kSubbands && pseudocode14First != kSubbands) {
            comparison.pseudocode14FirstMismatch = block * kSubbands + pseudocode14First;
        }
    }
    return comparison;
}

std::vector<float> process(const std::vector<float> &input, const std::vector<double> &qwin)
{
    Qmf qmf(qwin);
    std::vector<float> output;
    for (std::size_t offset = 0; offset + kSubbands <= input.size(); offset += kSubbands) {
        const std::vector<Complex> q = qmf.analyzeBlock(input.data() + offset);
        const auto pcm = qmf.synthesizeBlock(q);
        output.insert(output.end(), pcm.begin(), pcm.end());
    }
    return output;
}

std::vector<float> processSplit(const std::vector<float> &input,
                                const std::vector<double> &qwin)
{
    Qmf qmf(qwin);
    std::vector<float> pending;
    std::vector<float> output;
    const std::size_t splitPattern[] = {17, 103, 5, 257, 41, 89};
    std::size_t inputOffset = 0;
    std::size_t patternIndex = 0;
    while (inputOffset < input.size()) {
        const std::size_t count = std::min(splitPattern[patternIndex++ % std::size(splitPattern)],
                                            input.size() - inputOffset);
        pending.insert(pending.end(), input.begin() + static_cast<std::ptrdiff_t>(inputOffset),
                       input.begin() + static_cast<std::ptrdiff_t>(inputOffset + count));
        inputOffset += count;
        while (pending.size() >= kSubbands) {
            const std::vector<Complex> q = qmf.analyzeBlock(pending.data());
            const auto pcm = qmf.synthesizeBlock(q);
            output.insert(output.end(), pcm.begin(), pcm.end());
            pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(kSubbands));
        }
    }
    return output;
}

std::vector<float> processAfterReset(const std::vector<float> &input,
                                     const std::vector<double> &qwin)
{
    Qmf qmf(qwin);
    const std::size_t warmupSamples = std::min(input.size(), 7 * kSubbands);
    for (std::size_t offset = 0; offset + kSubbands <= warmupSamples; offset += kSubbands) {
        const std::vector<Complex> q = qmf.analyzeBlock(input.data() + offset);
        static_cast<void>(qmf.synthesizeBlock(q));
    }
    qmf.reset();

    std::vector<float> output;
    for (std::size_t offset = 0; offset + kSubbands <= input.size(); offset += kSubbands) {
        const std::vector<Complex> q = qmf.analyzeBlock(input.data() + offset);
        const auto pcm = qmf.synthesizeBlock(q);
        output.insert(output.end(), pcm.begin(), pcm.end());
    }
    return output;
}

struct Metric {
    double snrDb = -std::numeric_limits<double>::infinity();
    int delay = 0;
    double gain = 0.0;
};

Metric measure(const std::vector<float> &input, const std::vector<float> &output)
{
    Metric best;
    for (int delay = -1280; delay <= 1280; ++delay) {
        const std::size_t inputStart = delay < 0 ? static_cast<std::size_t>(-delay) : 0;
        const std::size_t outputStart = delay > 0 ? static_cast<std::size_t>(delay) : 0;
        const std::size_t overlap = std::min(input.size() - inputStart, output.size() - outputStart);
        if (overlap < 1024) {
            continue;
        }
        double energy = 0.0;
        double cross = 0.0;
        for (std::size_t index = 0; index < overlap; ++index) {
            const double source = input[inputStart + index];
            const double target = output[outputStart + index];
            energy += source * source;
            cross += source * target;
        }
        if (energy < 1.0e-20) {
            continue;
        }
        const double gain = cross / energy;
        double error = 0.0;
        for (std::size_t index = 0; index < overlap; ++index) {
            const double source = input[inputStart + index];
            const double target = output[outputStart + index];
            const double difference = target - gain * source;
            error += difference * difference;
        }
        const double snr = error < 1.0e-30
            ? std::numeric_limits<double>::infinity()
            : 10.0 * std::log10(energy * gain * gain / error);
        if (snr > best.snrDb) {
            best.snrDb = snr;
            best.delay = delay;
            best.gain = gain;
        }
    }
    return best;
}

bool finiteAndBounded(const std::vector<float> &values, float bound, float *peak)
{
    float maximum = 0.0f;
    for (const float value : values) {
        if (!std::isfinite(value)) {
            return false;
        }
        maximum = std::max(maximum, std::abs(value));
    }
    if (peak) {
        *peak = maximum;
    }
    return maximum <= bound;
}

std::size_t subnormalCount(const std::vector<float> &values)
{
    return static_cast<std::size_t>(std::count_if(values.begin(), values.end(), [](float value) {
        return std::fpclassify(value) == FP_SUBNORMAL;
    }));
}

float maxAbsDifference(const std::vector<float> &left, const std::vector<float> &right)
{
    if (left.size() != right.size()) {
        return std::numeric_limits<float>::infinity();
    }
    float maximum = 0.0f;
    for (std::size_t index = 0; index < left.size(); ++index) {
        maximum = std::max(maximum, std::abs(left[index] - right[index]));
    }
    return maximum;
}

std::string metricText(const Metric &metric)
{
    std::ostringstream output;
    if (std::isinf(metric.snrDb)) {
        output << "INF";
    } else {
        output << std::fixed << std::setprecision(2) << metric.snrDb;
    }
    return output.str();
}

void usage()
{
    std::cerr << "Usage: Eac3QmfProbe [--table path] [--samples N]\n";
}

} // namespace

int main(int argc, char **argv)
{
    std::string tablePath = "docs/dev/ts_103420_tables.c";
    std::size_t sampleCount = 64 * 400;
    for (int index = 1; index < argc; ++index) {
        const std::string option = argv[index];
        if (option == "--table" && index + 1 < argc) {
            tablePath = argv[++index];
        } else if (option == "--samples" && index + 1 < argc) {
            try {
                sampleCount = static_cast<std::size_t>(std::stoull(argv[++index]));
            } catch (...) {
                usage();
                return 2;
            }
        } else {
            usage();
            return 2;
        }
    }
    sampleCount = std::max(kSubbands * 32, sampleCount / kSubbands * kSubbands);
    std::vector<double> qwin;
    std::string reason;
    if (!eac3qmf::loadQwin(tablePath, &qwin, &reason)) {
        std::cerr << "probeResult=FAIL stage=load-qwin reason=" << reason << '\n';
        return 1;
    }

    const SynthesisReferenceComparison synthesisReference =
        independentSynthesisReferenceTest(qwin);
    const bool synthesisMatrixPass =
        synthesisReference.matrixMaximumError <= 1.0e-6f;
    const bool synthesisPseudocode14ConflictObserved =
        synthesisReference.pseudocode14MaximumError > 1.0e-6f;
    std::cout << "synthesisMatrixReferenceMaxAbsError="
              << synthesisReference.matrixMaximumError << '\n'
              << "synthesisMatrixReferenceFirstMismatch="
              << (synthesisReference.matrixFirstMismatch == kSubbands
                      ? -1LL : static_cast<long long>(synthesisReference.matrixFirstMismatch)) << '\n'
              << "synthesisMatrixReference="
              << (synthesisMatrixPass ? "PASS" : "FAIL") << '\n'
              << "synthesisPseudocode14DifferenceMaxAbsError="
              << synthesisReference.pseudocode14MaximumError << '\n'
              << "synthesisPseudocode14DifferenceFirstMismatch="
              << (synthesisReference.pseudocode14FirstMismatch == kSubbands
                      ? -1LL : static_cast<long long>(synthesisReference.pseudocode14FirstMismatch)) << '\n'
              << "synthesisPseudocode14ConflictObserved="
              << (synthesisPseudocode14ConflictObserved ? "PASS" : "FAIL") << '\n';
    if (!synthesisMatrixPass || !synthesisPseudocode14ConflictObserved) {
        std::cerr << "probeResult=FAIL stage=synthesis-reference-conflict-contract\n";
        return 1;
    }

    std::vector<float> silence(sampleCount, 0.0f);
    std::vector<float> impulse(sampleCount, 0.0f);
    impulse[sampleCount / 2] = 1.0f;
    std::vector<float> sine(sampleCount, 0.0f);
    std::vector<float> sweep(sampleCount, 0.0f);
    std::vector<float> random(sampleCount, 0.0f);
    std::uint32_t state = 0x12345678U;
    for (std::size_t index = 0; index < sampleCount; ++index) {
        const double phase = 2.0 * kPi * 997.0 * static_cast<double>(index) / 48000.0;
        sine[index] = static_cast<float>(0.5 * std::sin(phase));
        const double frequency = 80.0 * std::pow(6000.0 / 80.0, static_cast<double>(index) / sampleCount);
        sweep[index] = static_cast<float>(0.4 * std::sin(2.0 * kPi * frequency * index / 48000.0));
        state = state * 1664525U + 1013904223U;
        random[index] = (static_cast<float>(state >> 8) / static_cast<float>(0x00ffffffU)) * 2.0f - 1.0f;
    }

    const std::vector<float> silenceOutput = process(silence, qwin);
    const std::vector<float> impulseOutput = process(impulse, qwin);
    const std::vector<float> sineOutput = process(sine, qwin);
    const std::vector<float> sweepOutput = process(sweep, qwin);
    const std::vector<float> randomOutput = process(random, qwin);
    const std::vector<float> splitOutput = processSplit(random, qwin);
    const std::vector<float> resetOutput = processAfterReset(random, qwin);
    const Metric impulseMetric = measure(impulse, impulseOutput);
    const Metric sineMetric = measure(sine, sineOutput);
    const Metric sweepMetric = measure(sweep, sweepOutput);
    const Metric randomMetric = measure(random, randomOutput);

    float silencePeak = 0.0f;
    float randomPeak = 0.0f;
    bool finitePass = finiteAndBounded(silenceOutput, 4.0f, &silencePeak)
        && finiteAndBounded(impulseOutput, 4.0f, nullptr)
        && finiteAndBounded(sineOutput, 4.0f, nullptr)
        && finiteAndBounded(sweepOutput, 4.0f, nullptr)
        && finiteAndBounded(randomOutput, 4.0f, &randomPeak);
    const std::size_t subnormalTotal = subnormalCount(silenceOutput)
        + subnormalCount(impulseOutput) + subnormalCount(sineOutput)
        + subnormalCount(sweepOutput) + subnormalCount(randomOutput);
    const float splitDifference = maxAbsDifference(splitOutput, randomOutput);
    const float resetDifference = maxAbsDifference(resetOutput, randomOutput);
    const bool snrPass = impulseMetric.snrDb >= kMinimumReconstructionSnrDb
        && sineMetric.snrDb >= kMinimumReconstructionSnrDb
        && sweepMetric.snrDb >= kMinimumReconstructionSnrDb
        && randomMetric.snrDb >= kMinimumReconstructionSnrDb;
    const bool delayPass = impulseMetric.delay == eac3qmf::kAnalysisSynthesisDelaySamples
        && sineMetric.delay == eac3qmf::kAnalysisSynthesisDelaySamples
        && sweepMetric.delay == eac3qmf::kAnalysisSynthesisDelaySamples
        && randomMetric.delay == eac3qmf::kAnalysisSynthesisDelaySamples;
    const bool gainPass = std::abs(impulseMetric.gain - 1.0) <= 1.0e-3
        && std::abs(sineMetric.gain - 1.0) <= 1.0e-3
        && std::abs(sweepMetric.gain - 1.0) <= 1.0e-3
        && std::abs(randomMetric.gain - 1.0) <= 1.0e-3;
    const bool silencePass = silencePeak == 0.0f;
    const bool pass = snrPass && delayPass && gainPass && silencePass && finitePass
        && subnormalTotal == 0 && splitDifference <= 1.0e-6f && resetDifference <= 1.0e-6f;
    std::cout << "qmfSubbands=" << kSubbands << '\n'
              << "qmfFilterLength=" << kFilterLength << '\n'
              << "qwinCoefficientCount=" << qwin.size() << '\n'
              << "minimumReconstructionSNRdB=" << kMinimumReconstructionSnrDb << '\n'
              << "expectedDelaySamples=" << eac3qmf::kAnalysisSynthesisDelaySamples << '\n'
              << "impulseSNRdB=" << metricText(impulseMetric) << " delaySamples=" << impulseMetric.delay
              << " gain=" << impulseMetric.gain << '\n'
              << "sineSNRdB=" << metricText(sineMetric) << " delaySamples=" << sineMetric.delay
              << " gain=" << sineMetric.gain << '\n'
              << "sweepSNRdB=" << metricText(sweepMetric) << " delaySamples=" << sweepMetric.delay
              << " gain=" << sweepMetric.gain << '\n'
              << "randomSNRdB=" << metricText(randomMetric) << " delaySamples=" << randomMetric.delay
              << " gain=" << randomMetric.gain << '\n'
              << "silenceOutputPeak=" << silencePeak << '\n'
              << "randomOutputPeak=" << randomPeak << '\n'
              << "subnormalSampleCount=" << subnormalTotal << '\n'
              << "finiteNoClipping=" << (finitePass ? "PASS" : "FAIL") << '\n'
              << "splitBoundaryMaxAbsDiff=" << splitDifference << '\n'
              << "splitBoundaryInvariant=" << (splitDifference <= 1.0e-6f ? "PASS" : "FAIL") << '\n'
              << "stateResetMaxAbsDiff=" << resetDifference << '\n'
              << "stateResetInvariant=" << (resetDifference <= 1.0e-6f ? "PASS" : "FAIL") << '\n'
              << "probeResult=" << (pass ? "PASS" : "FAIL")
              << " stage=gate4-qmf-perfect-reconstruction"
              << " implementation=ts103420-matrix-n-runtime-qwin\n";
    return pass ? 0 : 1;
}
