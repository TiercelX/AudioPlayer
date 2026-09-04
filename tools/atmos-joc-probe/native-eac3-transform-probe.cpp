#include "native-eac3-transform.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

using eac3native::Eac3TransformChannel;
using eac3native::TransformFlushOutput;
using eac3native::TransformOutput;

bool finite(const std::vector<double>& values)
{
    return std::all_of(values.begin(), values.end(), [](double value) {
        return std::isfinite(value);
    });
}

bool zero(const std::vector<double>& values)
{
    return std::all_of(values.begin(), values.end(), [](double value) {
        return value == 0.0;
    });
}

bool nonZero(const std::vector<double>& values)
{
    return std::any_of(values.begin(), values.end(), [](double value) {
        return value != 0.0;
    });
}

bool equal(const std::vector<double>& left, const std::vector<double>& right)
{
    return left == right;
}

bool processOk(Eac3TransformChannel& channel, const std::vector<double>& coeffs,
               bool blockSwitch, std::vector<double>* samples)
{
    const TransformOutput result = channel.processBlock(coeffs, blockSwitch);
    if (!result.ok || result.samples.size() != eac3native::kEac3OutputSamples)
        return false;
    if (samples)
        *samples = result.samples;
    return finite(result.samples);
}

bool flushOk(Eac3TransformChannel& channel, bool expectedTail,
             std::vector<double>* samples)
{
    const TransformFlushOutput result = channel.flush();
    if (!result.ok || result.hadTail != expectedTail)
        return false;
    if (expectedTail && result.samples.size() != eac3native::kEac3OutputSamples)
        return false;
    if (!expectedTail && !result.samples.empty())
        return false;
    if (samples)
        *samples = result.samples;
    return finite(result.samples);
}

bool selfTest()
{
    constexpr std::size_t kN = eac3native::kEac3TransformCoefficients;
    const std::vector<double> zeroCoefficients(kN, 0.0);

    bool allZero = false;
    {
        Eac3TransformChannel channel;
        std::vector<double> output;
        std::vector<double> tail;
        allZero = processOk(channel, zeroCoefficients, false, &output)
            && zero(output)
            && flushOk(channel, true, &tail)
            && zero(tail);
    }

    bool impulse = false;
    {
        Eac3TransformChannel channel;
        const std::vector<double> coefficients{1.0};
        std::vector<double> output;
        std::vector<double> tail;
        impulse = processOk(channel, coefficients, false, &output)
            && nonZero(output)
            && flushOk(channel, true, &tail)
            && finite(tail);
    }

    bool singleBin = false;
    {
        singleBin = true;
        for (std::size_t index = 0; index < kN; ++index) {
            Eac3TransformChannel channel;
            std::vector<double> coefficients(kN, 0.0);
            coefficients[index] = 1.0;
            std::vector<double> output;
            std::vector<double> tail;
            if (!processOk(channel, coefficients, true, &output)
                || !flushOk(channel, true, &tail)
                || (!nonZero(output) && !nonZero(tail))) {
                std::cerr << "singleBinFailed=" << index << '\n';
                singleBin = false;
            }
        }
    }

    bool zeroFill = false;
    {
        Eac3TransformChannel shortInput;
        Eac3TransformChannel fullInput;
        const std::vector<double> shortCoefficients{0.25, -0.5, 0.125};
        std::vector<double> fullCoefficients(kN, 0.0);
        std::copy(shortCoefficients.begin(), shortCoefficients.end(),
                  fullCoefficients.begin());
        std::vector<double> shortOutput;
        std::vector<double> fullOutput;
        zeroFill = processOk(shortInput, shortCoefficients, false, &shortOutput)
            && processOk(fullInput, fullCoefficients, false, &fullOutput)
            && equal(shortOutput, fullOutput);
    }

    bool alternating = false;
    {
        Eac3TransformChannel channel;
        std::vector<double> coefficients(kN, 0.0);
        coefficients[3] = 0.5;
        std::size_t outputSamples = 0;
        bool ok = true;
        for (int block = 0; block < 4; ++block) {
            std::vector<double> output;
            ok = ok && processOk(channel, coefficients, (block % 2) != 0,
                                 &output);
            outputSamples += output.size();
        }
        std::vector<double> tail;
        ok = ok && flushOk(channel, true, &tail);
        outputSamples += tail.size();
        alternating = ok && outputSamples == 5 * eac3native::kEac3OutputSamples;
    }

    bool maximumFinite = false;
    {
        Eac3TransformChannel channel;
        const std::vector<double> coefficients(
            kN, std::numeric_limits<double>::max());
        std::vector<double> output;
        std::vector<double> tail;
        maximumFinite = processOk(channel, coefficients, false, &output)
            && processOk(channel, coefficients, true, &output)
            && flushOk(channel, true, &tail)
            && finite(output) && finite(tail);
    }

    bool channelIsolation = false;
    {
        Eac3TransformChannel left;
        Eac3TransformChannel right;
        std::vector<double> leftCoefficients(kN, 0.0);
        leftCoefficients[1] = 1.0;
        std::vector<double> leftOutput;
        std::vector<double> rightOutput;
        std::vector<double> leftTail;
        std::vector<double> rightTail;
        channelIsolation = processOk(left, leftCoefficients, false, &leftOutput)
            && processOk(right, zeroCoefficients, false, &rightOutput)
            && nonZero(leftOutput) && zero(rightOutput)
            && flushOk(left, true, &leftTail)
            && flushOk(right, true, &rightTail)
            && nonZero(leftTail) && zero(rightTail);
    }

    bool resetClearsOverlap = false;
    {
        Eac3TransformChannel channel;
        std::vector<double> coefficients(kN, 0.0);
        coefficients[5] = 1.0;
        std::vector<double> ignored;
        const bool first = processOk(channel, coefficients, false, &ignored);
        channel.reset();
        std::vector<double> tail;
        const bool noTail = flushOk(channel, false, &tail);
        channel.reset();
        std::vector<double> output;
        const bool afterReset = processOk(channel, zeroCoefficients, false, &output)
            && zero(output);
        resetClearsOverlap = first && noTail && afterReset;
    }

    bool eosPolicy = false;
    {
        Eac3TransformChannel channel;
        std::vector<double> output;
        std::vector<double> tail;
        const bool block = processOk(channel, zeroCoefficients, false, &output);
        const bool firstFlush = flushOk(channel, true, &tail)
            && tail.size() == eac3native::kEac3OutputSamples;
        const bool secondFlush = flushOk(channel, false, nullptr);
        const TransformOutput afterEos = channel.processBlock(zeroCoefficients, false);
        eosPolicy = block && firstFlush && secondFlush && !afterEos.ok;
    }

    bool invalidInput = false;
    {
        Eac3TransformChannel channel;
        std::vector<double> tooLong(kN + 1, 0.0);
        const TransformOutput tooLongResult = channel.processBlock(tooLong, false);
        std::vector<double> nonFinite(1, std::numeric_limits<double>::quiet_NaN());
        const TransformOutput nonFiniteResult = channel.processBlock(nonFinite, false);
        std::vector<double> output;
        invalidInput = !tooLongResult.ok && !nonFiniteResult.ok
            && processOk(channel, zeroCoefficients, false, &output);
    }

    bool shortLongDistinct = false;
    {
        Eac3TransformChannel longChannel;
        Eac3TransformChannel shortChannel;
        std::vector<double> coefficients(kN, 0.0);
        coefficients[0] = 1.0;
        std::vector<double> longOutput;
        std::vector<double> shortOutput;
        shortLongDistinct = processOk(longChannel, coefficients, false, &longOutput)
            && processOk(shortChannel, coefficients, true, &shortOutput)
            && finite(longOutput) && finite(shortOutput)
            && longOutput != shortOutput;
    }

    // Discriminator for TS §6.9.4.1 Step 5: x[2*n+1] uses yr[N/8-n-1]
    // (yr[63-n] for N=512), not the distinct yr[N/4-n-1] address used by
    // the final odd quadrant.  This expected value is from the independent
    // scalar equation oracle, and is intentionally sensitive to that index.
    bool longStep5Index = false;
    {
        Eac3TransformChannel channel;
        std::vector<double> coefficients(kN, 0.0);
        for (std::size_t index = 0; index < kN; ++index)
            coefficients[index] = (static_cast<double>(index % 11) - 5.0)
                * 0.03125;
        std::vector<double> output;
        longStep5Index = processOk(channel, coefficients, false, &output)
            && std::abs(output[1] - (-1.781777440736573e-05)) < 1e-14;
    }

    const bool pass = allZero && impulse && singleBin && zeroFill && alternating
        && maximumFinite && channelIsolation && resetClearsOverlap && eosPolicy
        && invalidInput && shortLongDistinct && longStep5Index;
    std::cout << "selfTest=" << (pass ? "PASS" : "FAIL")
              << " cases=12"
              << " allZero=" << (allZero ? "YES" : "NO")
              << " impulse=" << (impulse ? "YES" : "NO")
              << " singleBin=" << (singleBin ? "YES" : "NO")
              << " zeroFill=" << (zeroFill ? "YES" : "NO")
              << " alternating=" << (alternating ? "YES" : "NO")
              << " maximumFinite=" << (maximumFinite ? "YES" : "NO")
              << " channelIsolation=" << (channelIsolation ? "YES" : "NO")
              << " resetClearsOverlap=" << (resetClearsOverlap ? "YES" : "NO")
              << " eosPolicy=" << (eosPolicy ? "YES" : "NO")
              << " invalidInput=" << (invalidInput ? "YES" : "NO")
              << " shortLongDistinct=" << (shortLongDistinct ? "YES" : "NO")
              << " longStep5Index=" << (longStep5Index ? "YES" : "NO")
              << " drcApplied=NO ffmpegLinked=NO\n";
    return pass;
}

int dumpCase(const std::string& name)
{
    Eac3TransformChannel channel;
    std::vector<double> coefficients(eac3native::kEac3TransformCoefficients, 0.0);
    coefficients[0] = 1.0;
    const bool blockSwitch = name == "short";
    const TransformOutput output = channel.processBlock(coefficients, blockSwitch);
    if (!output.ok || (name != "long" && name != "short"))
        return 2;
    std::cout << std::setprecision(17);
    std::cout << "case=" << name << " samples=" << output.samples.size() << "\n";
    for (std::size_t i = 0; i < output.samples.size(); ++i)
        std::cout << i << " " << output.samples[i] << "\n";
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 2 && std::string(argv[1]) == "--self-test")
        return selfTest() ? 0 : 1;
    if (argc == 3 && std::string(argv[1]) == "--dump-case")
        return dumpCase(argv[2]);
    std::cerr << "Usage: Eac3NativeTransformProbe --self-test\n"
                 "       Eac3NativeTransformProbe --dump-case long|short\n";
    return 2;
}
