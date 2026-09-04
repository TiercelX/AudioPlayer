// Gate 5B diagnostic: JOC coefficient reconstruction, dequantization, and
// temporal interpolation. This module has no QMF or playback dependency.

#include "joc-gate5b.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <utility>

namespace eac3joc {
namespace {

constexpr unsigned kQmfSubbands = 64;
constexpr unsigned kMaxObjects = 16;
constexpr unsigned kMaxChannels = 7;
constexpr unsigned kMaxBands = 23;
constexpr unsigned kMaxDataPoints = 2;
constexpr unsigned kMaxQmfTimeslots = 1024;

using Mapping = std::array<unsigned, kQmfSubbands>;

// ETSI TS 103 420 V1.2.1, clause 6.5, Table 54. These are independent
// expected run lengths for the eight legal band counts; the self-test expands
// them and compares every one of the 64 QMF subbands with production output.
constexpr std::array<unsigned, 8> kTable54BandCounts =
    {1U, 3U, 5U, 7U, 9U, 12U, 15U, 23U};
constexpr std::array<std::array<unsigned, kMaxBands>, 8> kTable54RunLengths = {{
    {64U},
    {3U, 11U, 50U},
    {1U, 2U, 6U, 14U, 41U},
    {1U, 1U, 2U, 4U, 6U, 9U, 41U},
    {1U, 1U, 1U, 2U, 2U, 2U, 5U, 9U, 41U},
    {1U, 1U, 1U, 1U, 2U, 2U, 3U, 3U, 4U, 5U, 12U, 29U},
    {1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 2U, 3U, 4U, 5U, 12U, 29U},
    {1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U, 1U,
     2U, 2U, 2U, 2U, 3U, 3U, 4U, 5U, 6U, 7U, 16U},
}};

Mapping makeMapping(std::initializer_list<unsigned> lengths)
{
    Mapping mapping {};
    unsigned subband = 0;
    unsigned parameterBand = 0;
    for (const unsigned length : lengths) {
        for (unsigned count = 0; count < length && subband < kQmfSubbands; ++count) {
            mapping[subband++] = parameterBand;
        }
        ++parameterBand;
    }
    return mapping;
}

const Mapping &mappingForBands(unsigned numBands)
{
    static const Mapping one = makeMapping({64});
    static const Mapping three = makeMapping({3, 11, 50});
    static const Mapping five = makeMapping({1, 2, 6, 14, 41});
    static const Mapping seven = makeMapping({1, 1, 2, 4, 6, 9, 41});
    static const Mapping nine = makeMapping({1, 1, 1, 2, 2, 2, 5, 9, 41});
    static const Mapping twelve = makeMapping({1, 1, 1, 1, 2, 2, 3, 3, 4, 5, 12, 29});
    static const Mapping fifteen = makeMapping({1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 3, 4, 5, 12, 29});
    static const Mapping twentyThree = makeMapping({1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                                                     2, 2, 2, 2, 3, 3, 4, 5, 6, 7, 16});
    switch (numBands) {
    case 1: return one;
    case 3: return three;
    case 5: return five;
    case 7: return seven;
    case 9: return nine;
    case 12: return twelve;
    case 15: return fifteen;
    case 23: return twentyThree;
    default: return one;
    }
}

bool validBandCount(unsigned bands)
{
    switch (bands) {
    case 1:
    case 3:
    case 5:
    case 7:
    case 9:
    case 12:
    case 15:
    case 23:
        return true;
    default:
        return false;
    }
}

bool validDimensions(const FrameReport &frame, std::string *reason)
{
    if (frame.numObjects == 0 || frame.numObjects > kMaxObjects) {
        *reason = "joc-math-object-count-out-of-range";
        return false;
    }
    if (frame.numChannels == 0 || frame.numChannels > kMaxChannels) {
        *reason = "joc-math-channel-count-out-of-range";
        return false;
    }
    if (frame.objects.size() != frame.numObjects) {
        *reason = "joc-math-object-array-size-mismatch";
        return false;
    }
    return true;
}

bool decodeQuantized(const FrameReport &frame,
                     std::vector<JocQuantizedObject> *output,
                     std::string *reason)
{
    if (!output || !reason || !validDimensions(frame, reason)) {
        return false;
    }
    output->clear();
    output->resize(frame.numObjects);
    for (unsigned objectIndex = 0; objectIndex < frame.numObjects; ++objectIndex) {
        const JocObjectSummary &source = frame.objects[objectIndex];
        JocQuantizedObject &target = (*output)[objectIndex];
        target.present = source.present;
        target.numChannels = frame.numChannels;
        target.numBands = source.numBands;
        target.quantIndex = source.quantIndex;
        target.slopeIndex = source.slopeIndex;
        if (!source.present) {
            continue;
        }
        if (!validBandCount(source.numBands)
            || source.quantSteps != (source.quantIndex == 0 ? 96U : 192U)
            || source.numDataPoints == 0 || source.numDataPoints > kMaxDataPoints
            || source.dataPoints.size() != source.numDataPoints) {
            *reason = "joc-math-object-info-out-of-range";
            return false;
        }
        target.values.resize(source.numDataPoints,
                             std::vector<std::vector<unsigned>>(
                                 frame.numChannels, std::vector<unsigned>(source.numBands, 0U)));
        for (unsigned dataPointIndex = 0; dataPointIndex < source.numDataPoints; ++dataPointIndex) {
            const JocDataPoint &dataPoint = source.dataPoints[dataPointIndex];
            if (source.sparse) {
                if (dataPoint.channelIndices.size() != source.numBands
                    || dataPoint.values.size() != source.numBands) {
                    *reason = "joc-math-sparse-data-size-mismatch";
                    return false;
                }
                unsigned channelIndex = dataPoint.channelIndices[0];
                if (channelIndex >= frame.numChannels) {
                    *reason = "joc-math-sparse-channel-index-out-of-range";
                    return false;
                }
                for (unsigned parameterBand = 0; parameterBand < source.numBands; ++parameterBand) {
                    if (parameterBand > 0) {
                        channelIndex = (dataPoint.channelIndices[parameterBand - 1]
                                        + dataPoint.channelIndices[parameterBand]) % frame.numChannels;
                    }
                if (dataPoint.values[parameterBand] >= source.quantSteps) {
                    *reason = "joc-math-sparse-coefficient-out-of-range";
                    return false;
                }
                for (unsigned channel = 0; channel < frame.numChannels; ++channel) {
                    target.values[dataPointIndex][channel][parameterBand]
                        = source.quantIndex == 0 ? 50U : 100U;
                }
                if (parameterBand == 0) {
                        target.values[dataPointIndex][channelIndex][parameterBand]
                            = (source.quantIndex == 0 ? 50U : 100U)
                            + dataPoint.values[parameterBand];
                        target.values[dataPointIndex][channelIndex][parameterBand]
                            %= source.quantSteps;
                    } else {
                        const unsigned previous = target.values[dataPointIndex][channelIndex][parameterBand - 1];
                        target.values[dataPointIndex][channelIndex][parameterBand]
                            = (previous + dataPoint.values[parameterBand]) % source.quantSteps;
                    }
                }
            } else {
                if (dataPoint.values.size() != frame.numChannels * source.numBands) {
                    *reason = "joc-math-dense-data-size-mismatch";
                    return false;
                }
                const unsigned offset = source.quantIndex == 0 ? 48U : 96U;
                for (unsigned channel = 0; channel < frame.numChannels; ++channel) {
                    for (unsigned parameterBand = 0; parameterBand < source.numBands; ++parameterBand) {
                        const unsigned coefficient = dataPoint.values[channel * source.numBands + parameterBand];
                        if (coefficient >= source.quantSteps) {
                            *reason = "joc-math-dense-coefficient-out-of-range";
                            return false;
                        }
                        if (parameterBand == 0) {
                            target.values[dataPointIndex][channel][parameterBand]
                                = (offset + coefficient) % source.quantSteps;
                        } else {
                            target.values[dataPointIndex][channel][parameterBand]
                                = (target.values[dataPointIndex][channel][parameterBand - 1]
                                   + coefficient) % source.quantSteps;
                        }
                    }
                }
            }
        }
    }
    return true;
}

bool dequantize(const std::vector<JocQuantizedObject> &quantized,
                std::vector<JocDequantizedObject> *output,
                std::string *reason)
{
    if (!output || !reason) {
        return false;
    }
    output->clear();
    output->resize(quantized.size());
    for (std::size_t objectIndex = 0; objectIndex < quantized.size(); ++objectIndex) {
        const JocQuantizedObject &source = quantized[objectIndex];
        JocDequantizedObject &target = (*output)[objectIndex];
        target.present = source.present;
        target.numChannels = source.numChannels;
        target.numBands = source.numBands;
        target.quantIndex = source.quantIndex;
        target.slopeIndex = source.slopeIndex;
        if (!source.present) {
            continue;
        }
        if (source.values.empty() || source.quantIndex > 1) {
            *reason = "joc-math-dequantization-input-invalid";
            return false;
        }
        const unsigned quantSteps = source.quantIndex == 0 ? 96U : 192U;
        const double denominator = 4096.0 * (1.0 + source.quantIndex);
        target.values.resize(source.values.size(),
                             std::vector<std::vector<double>>(
                                 source.numChannels, std::vector<double>(source.numBands, 0.0)));
        for (std::size_t dp = 0; dp < source.values.size(); ++dp) {
            for (unsigned channel = 0; channel < source.numChannels; ++channel) {
                for (unsigned parameterBand = 0; parameterBand < source.numBands; ++parameterBand) {
                    const unsigned value = source.values[dp][channel][parameterBand];
                    if (value >= quantSteps) {
                        *reason = "joc-math-quantized-value-out-of-range";
                        return false;
                    }
                    target.values[dp][channel][parameterBand]
                        = (static_cast<double>(value) - quantSteps / 2.0) * 820.0 / denominator;
                }
            }
        }
    }
    return true;
}

bool sequenceIsNext(unsigned previous, unsigned current)
{
    const unsigned expected = previous == 1023U ? 1U : (previous + 1U) & 0x3ffU;
    return current == expected;
}

bool dimensionsMatch(const JocMatrixState &state, const FrameReport &frame)
{
    return state.numChannels == frame.numChannels
        && state.numObjects == frame.numObjects
        && state.previous.size() == frame.numObjects;
}

void allocateState(JocMatrixState *state, const FrameReport &frame)
{
    state->numChannels = frame.numChannels;
    state->numObjects = frame.numObjects;
    state->previous.assign(frame.numObjects,
                           std::vector<std::vector<double>>(
                               frame.numChannels, std::vector<double>(kQmfSubbands, 0.0)));
}

bool interpolate(const FrameReport &frame,
                 const std::vector<JocDequantizedObject> &dequantized,
                 unsigned numQmfTimeslots,
                 JocMatrixState *state,
                 JocMathFrame *output,
                 std::string *reason)
{
    if (!state || !output || !reason || numQmfTimeslots == 0
        || numQmfTimeslots > kMaxQmfTimeslots) {
        if (reason) {
            *reason = "joc-math-qmf-timeslot-count-out-of-range";
        }
        return false;
    }
    const bool sequenceDiscontinuity = state->initialized
        && !sequenceIsNext(state->previousSequence, frame.sequenceCount)
        && frame.sequenceCount != 0;
    const bool stateReset = !state->initialized || !dimensionsMatch(*state, frame)
        || frame.sequenceCount == 0 || sequenceDiscontinuity;
    if (stateReset) {
        allocateState(state, frame);
    }

    output->interpolated.clear();
    output->interpolated.resize(frame.numObjects);
    for (unsigned objectIndex = 0; objectIndex < frame.numObjects; ++objectIndex) {
        const JocObjectSummary &source = frame.objects[objectIndex];
        JocInterpolatedObject &target = output->interpolated[objectIndex];
        target.present = source.present;
        target.numChannels = frame.numChannels;
        target.values.resize(numQmfTimeslots,
                             std::vector<std::vector<double>>(
                                 frame.numChannels, std::vector<double>(kQmfSubbands, 0.0)));
        if (!source.present) {
            for (auto &channel : state->previous[objectIndex]) {
                std::fill(channel.begin(), channel.end(), 0.0);
            }
            continue;
        }
        const JocDequantizedObject &object = dequantized[objectIndex];
        if (object.values.empty() || object.values.size() > kMaxDataPoints
            || source.slopeIndex > 1) {
            *reason = "joc-math-interpolation-input-invalid";
            return false;
        }
        if (source.slopeIndex == 1) {
            if (source.dataPoints.size() != object.values.size()
                || std::any_of(source.dataPoints.begin(), source.dataPoints.end(),
                               [](const JocDataPoint &point) {
                                   return point.offsetTs == 0 || point.offsetTs > kMaxQmfTimeslots;
                               })
                || (source.dataPoints.size() == 2
                    && source.dataPoints[1].offsetTs < source.dataPoints[0].offsetTs)) {
                *reason = "joc-math-steep-offsets-invalid";
                return false;
            }
        }
        if (object.values.size() == 2 && source.slopeIndex == 0 && numQmfTimeslots < 2) {
            *reason = "joc-math-two-point-timeslot-count-too-small";
            return false;
        }
        const Mapping &mapping = mappingForBands(source.numBands);
        const unsigned halfTimeslots = numQmfTimeslots / 2U;
        for (unsigned channel = 0; channel < frame.numChannels; ++channel) {
            for (unsigned subband = 0; subband < kQmfSubbands; ++subband) {
                const unsigned parameterBand = mapping[subband];
                for (unsigned timeslot = 0; timeslot < numQmfTimeslots; ++timeslot) {
                    double value = 0.0;
                    if (source.slopeIndex == 0) {
                        if (object.values.size() == 1) {
                            const double delta = object.values[0][channel][parameterBand]
                                - state->previous[objectIndex][channel][subband];
                            value = state->previous[objectIndex][channel][subband]
                                + (static_cast<double>(timeslot + 1U) * delta)
                                    / static_cast<double>(numQmfTimeslots);
                        } else if (timeslot < halfTimeslots) {
                            const double delta = object.values[0][channel][parameterBand]
                                - state->previous[objectIndex][channel][subband];
                            value = state->previous[objectIndex][channel][subband]
                                + (static_cast<double>(timeslot + 1U) * delta)
                                    / static_cast<double>(halfTimeslots);
                        } else {
                            const double delta = object.values[1][channel][parameterBand]
                                - object.values[0][channel][parameterBand];
                            value = object.values[0][channel][parameterBand]
                                + (static_cast<double>(timeslot - halfTimeslots + 1U) * delta)
                                    / static_cast<double>(numQmfTimeslots - halfTimeslots);
                        }
                    } else if (object.values.size() == 1) {
                        value = timeslot < source.dataPoints[0].offsetTs
                            ? state->previous[objectIndex][channel][subband]
                            : object.values[0][channel][parameterBand];
                    } else if (timeslot < source.dataPoints[0].offsetTs) {
                        value = state->previous[objectIndex][channel][subband];
                    } else if (timeslot < source.dataPoints[1].offsetTs) {
                        value = object.values[0][channel][parameterBand];
                    } else {
                        value = object.values[1][channel][parameterBand];
                    }
                    if (!std::isfinite(value)) {
                        *reason = "joc-math-non-finite-interpolation";
                        return false;
                    }
                    target.values[timeslot][channel][subband] = value;
                }
                state->previous[objectIndex][channel][subband]
                    = object.values.back()[channel][parameterBand];
            }
        }
    }
    state->initialized = true;
    state->previousSequence = frame.sequenceCount;
    output->stateReset = stateReset;
    output->sequenceDiscontinuity = sequenceDiscontinuity;
    return true;
}

FrameReport makeDenseFrame(unsigned sequence,
                           unsigned quantIndex,
                           unsigned slopeIndex,
                           unsigned numDataPoints,
                           unsigned numBands,
                           unsigned numChannels,
                           const std::vector<unsigned> &values,
                           const std::vector<unsigned> &offsets = {})
{
    FrameReport frame;
    frame.disposition = ParseDisposition::Pass;
    frame.numChannels = numChannels;
    frame.numObjects = 1;
    frame.sequenceCount = sequence;
    frame.objects.resize(1);
    JocObjectSummary &object = frame.objects[0];
    object.present = true;
    object.numBands = numBands;
    object.quantIndex = quantIndex;
    object.quantSteps = quantIndex == 0 ? 96U : 192U;
    object.slopeIndex = slopeIndex;
    object.numDataPoints = numDataPoints;
    object.dataPoints.resize(numDataPoints);
    for (unsigned dp = 0; dp < numDataPoints; ++dp) {
        object.dataPoints[dp].values = values;
        if (dp < offsets.size()) {
            object.dataPoints[dp].offsetTs = offsets[dp];
        }
    }
    return frame;
}

bool almostEqual(double left, double right, double tolerance = 1.0e-9)
{
    return std::abs(left - right) <= tolerance;
}

bool checkMapping(std::size_t *cases, std::string *reason)
{
    for (std::size_t tableIndex = 0; tableIndex < kTable54BandCounts.size(); ++tableIndex) {
        const unsigned bands = kTable54BandCounts[tableIndex];
        const Mapping &mapping = mappingForBands(bands);
        Mapping expected {};
        unsigned expectedSubband = 0U;
        for (unsigned parameterBand = 0; parameterBand < bands; ++parameterBand) {
            for (unsigned count = 0; count < kTable54RunLengths[tableIndex][parameterBand]; ++count) {
                if (expectedSubband >= kQmfSubbands) {
                    *reason = "table54-reference-overflow";
                    return false;
                }
                expected[expectedSubband++] = parameterBand;
            }
        }
        if (expectedSubband != kQmfSubbands) {
            *reason = "table54-reference-does-not-cover-64-subbands";
            return false;
        }
        for (unsigned subband = 0; subband < kQmfSubbands; ++subband) {
            if (mapping[subband] != expected[subband]) {
                *reason = "table54-exact-subband-mismatch:" + std::to_string(bands)
                    + ":" + std::to_string(subband) + ":expected="
                    + std::to_string(expected[subband]) + ":actual="
                    + std::to_string(mapping[subband]);
                return false;
            }
            ++(*cases);
        }
    }
    return true;
}

bool checkMathCases(std::size_t *interpolationCases, std::string *reason)
{
    // Dense MTX values are differential-coded with an offset of 48/96;
    // a raw zero therefore represents the quantizer center.
    const std::vector<unsigned> centerValues(5, 0U);
    const auto quant0 = [](unsigned value) {
        return (static_cast<double>(value) - 48.0) * 820.0 / 4096.0;
    };
    const auto checkSingleBandSeries = [&](const JocMathFrame &candidate,
                                           const std::vector<double> &expected,
                                           const char *label) {
        if (candidate.interpolated.empty()
            || candidate.interpolated[0].values.size() != expected.size()) {
            *reason = std::string(label) + "-shape-failed";
            return false;
        }
        for (std::size_t timeslot = 0; timeslot < expected.size(); ++timeslot) {
            if (!almostEqual(candidate.interpolated[0].values[timeslot][0][0],
                             expected[timeslot])) {
                *reason = std::string(label) + "-timeslot-" + std::to_string(timeslot)
                    + "-failed actual="
                    + std::to_string(candidate.interpolated[0].values[timeslot][0][0])
                    + " expected=" + std::to_string(expected[timeslot]);
                return false;
            }
        }
        return true;
    };
    JocMatrixState state;
    JocMathFrame output;
    FrameReport smoothOne = makeDenseFrame(1, 0, 0, 1, 1, 5, centerValues);
    if (!processMath(smoothOne, 4, &state, &output, reason)
        || !output.stateReset
        || !almostEqual(output.dequantized[0].values[0][0][0], 0.0)) {
        *reason = "quantizer-center-or-first-interpolation-failed";
        return false;
    }
    ++(*interpolationCases);

    resetMatrixState(&state);
    FrameReport endpoint = makeDenseFrame(1, 0, 0, 1, 1, 5, std::vector<unsigned>(5, 48U));
    if (!processMath(endpoint, 4, &state, &output, reason)
        || !almostEqual(output.dequantized[0].values[0][0][0], -48.0 * 820.0 / 4096.0)) {
        *reason = "quantizer-lower-endpoint-failed";
        return false;
    }
    ++(*interpolationCases);

    FrameReport sparse;
    sparse.disposition = ParseDisposition::Pass;
    sparse.numChannels = 2;
    sparse.numObjects = 1;
    sparse.sequenceCount = 1;
    sparse.objects.resize(1);
    sparse.objects[0].present = true;
    sparse.objects[0].sparse = true;
    sparse.objects[0].numBands = 3;
    sparse.objects[0].quantIndex = 0;
    sparse.objects[0].quantSteps = 96;
    sparse.objects[0].slopeIndex = 0;
    sparse.objects[0].numDataPoints = 1;
    sparse.objects[0].dataPoints.resize(1);
    sparse.objects[0].dataPoints[0].channelIndices = {1, 1, 0};
    sparse.objects[0].dataPoints[0].values = {5, 7, 11};
    resetMatrixState(&state);
    if (!processMath(sparse, 4, &state, &output, reason)
        || output.quantized[0].values[0][1][0] != 55U
        || output.quantized[0].values[0][0][1] != 57U
        || output.quantized[0].values[0][1][2] != 61U
        || output.quantized[0].values[0][0][0] != 50U
        || output.quantized[0].values[0][1][1] != 50U
        || output.quantized[0].values[0][0][2] != 50U) {
        *reason = "sparse-differential-modulo-failed:";
        *reason += std::to_string(output.quantized[0].values[0][1][0]);
        *reason += "," + std::to_string(output.quantized[0].values[0][0][1]);
        *reason += "," + std::to_string(output.quantized[0].values[0][1][2]);
        *reason += "," + std::to_string(output.quantized[0].values[0][0][0]);
        return false;
    }
    ++(*interpolationCases);

    // Independent dense 2-channel/3-band differential recurrence. The six
    // raw coefficients are intentionally asymmetric so [ch][parameterBand]
    // storage order cannot pass by symmetry.
    FrameReport dense = makeDenseFrame(0, 1, 0, 1, 3, 2,
                                       {1U, 2U, 3U, 10U, 20U, 30U});
    resetMatrixState(&state);
    if (!processMath(dense, 4, &state, &output, reason)
        || output.quantized[0].values[0][0][0] != 97U
        || output.quantized[0].values[0][0][1] != 99U
        || output.quantized[0].values[0][0][2] != 102U
        || output.quantized[0].values[0][1][0] != 106U
        || output.quantized[0].values[0][1][1] != 126U
        || output.quantized[0].values[0][1][2] != 156U) {
        *reason = "dense-channel-band-differential-recurrence-failed";
        return false;
    }
    ++(*interpolationCases);

    // Re-establish the original five-channel state before the legacy smooth
    // interpolation vector below; the dense/sparse storage vectors above
    // intentionally exercise different dimensions.
    resetMatrixState(&state);
    FrameReport smoothSeed = makeDenseFrame(1, 0, 0, 1, 1, 5, centerValues);
    if (!processMath(smoothSeed, 4, &state, &output, reason)) {
        *reason = "smooth-state-seed-failed:" + *reason;
        return false;
    }

    const std::vector<unsigned> endpointValues(5, 47U);
    FrameReport smoothTwo = makeDenseFrame(2, 0, 0, 2, 1, 5, centerValues);
    smoothTwo.objects[0].dataPoints[1].values = endpointValues;
    if (!processMath(smoothTwo, 4, &state, &output, reason)
        || output.stateReset
        || !almostEqual(output.interpolated[0].values[1][0][0],
                        output.dequantized[0].values[0][0][0])) {
        *reason = "two-point-smooth-interpolation-failed";
        return false;
    }
    ++(*interpolationCases);

    resetMatrixState(&state);
    FrameReport steepTwo = makeDenseFrame(3, 1, 1, 2, 1, 5, std::vector<unsigned>(5, 0U), {2, 4});
    steepTwo.objects[0].dataPoints[1].values.assign(5, 95U);
    if (!processMath(steepTwo, 6, &state, &output, reason)
        || !std::isfinite(output.interpolated[0].values[0][0][0])
        || !almostEqual(output.interpolated[0].values[2][0][0],
                        output.dequantized[0].values[0][0][0])
        || !almostEqual(output.interpolated[0].values[5][0][0],
                        output.dequantized[0].values[1][0][0])) {
        *reason = "two-point-steep-interpolation-failed";
        return false;
    }
    ++(*interpolationCases);

    JocMatrixState wrapState;
    FrameReport wrapA = makeDenseFrame(1023, 0, 0, 1, 1, 5, centerValues);
    FrameReport wrapB = makeDenseFrame(1, 0, 0, 1, 1, 5, centerValues);
    FrameReport reset = makeDenseFrame(0, 0, 0, 1, 1, 5, centerValues);
    if (!processMath(wrapA, 4, &wrapState, &output, reason)
        || !processMath(wrapB, 4, &wrapState, &output, reason)
        || output.stateReset
        || output.sequenceDiscontinuity
        || !processMath(reset, 4, &wrapState, &output, reason)
        || !output.stateReset) {
        *reason = "sequence-wrap-or-reset-failed";
        return false;
    }
    ++(*interpolationCases);

    // Exact 24-timeslot smooth interpolation vectors. These expected values
    // are scalar calculations from the dequantized endpoints, not production
    // interpolation output copied back into the test.
    constexpr unsigned kBoundaryTimeslots = 24U;
    FrameReport smoothOne24 = makeDenseFrame(0, 0, 0, 1, 1, 1, {2U});
    resetMatrixState(&state);
    if (!processMath(smoothOne24, kBoundaryTimeslots, &state, &output, reason)) {
        *reason = "smooth-one-24-process-failed:" + *reason;
        return false;
    }
    std::vector<double> expected(kBoundaryTimeslots, 0.0);
    const double smoothOneTarget = quant0(50U);
    for (unsigned timeslot = 0; timeslot < kBoundaryTimeslots; ++timeslot) {
        expected[timeslot] = smoothOneTarget * static_cast<double>(timeslot + 1U)
            / static_cast<double>(kBoundaryTimeslots);
    }
    if (!checkSingleBandSeries(output, expected, "smooth-one-24")) {
        return false;
    }
    ++(*interpolationCases);

    FrameReport smoothTwo24 = makeDenseFrame(1, 0, 0, 2, 1, 1, {4U});
    smoothTwo24.objects[0].dataPoints[1].values = {8U};
    if (!processMath(smoothTwo24, kBoundaryTimeslots, &state, &output, reason)
        || output.stateReset) {
        *reason = "smooth-two-24-process-or-reset-failed:" + *reason;
        return false;
    }
    expected.assign(kBoundaryTimeslots, 0.0);
    const double smoothTwoStart = quant0(50U);
    const double smoothTwoFirst = quant0(52U);
    const double smoothTwoSecond = quant0(56U);
    for (unsigned timeslot = 0; timeslot < kBoundaryTimeslots; ++timeslot) {
        if (timeslot < kBoundaryTimeslots / 2U) {
            expected[timeslot] = smoothTwoStart
                + static_cast<double>(timeslot + 1U)
                    * (smoothTwoFirst - smoothTwoStart) / 12.0;
        } else {
            expected[timeslot] = smoothTwoFirst
                + static_cast<double>(timeslot - 11U)
                    * (smoothTwoSecond - smoothTwoFirst) / 12.0;
        }
    }
    if (!checkSingleBandSeries(output, expected, "smooth-two-24")) {
        return false;
    }
    ++(*interpolationCases);

    // Steep one-point updates use literal boundary offsets 1, 12 and 24.
    for (const unsigned offset : {1U, 12U, 24U}) {
        FrameReport steepOne24 = makeDenseFrame(0, 0, 1, 1, 1, 1, {12U}, {offset});
        resetMatrixState(&state);
        if (!processMath(steepOne24, kBoundaryTimeslots, &state, &output, reason)) {
            *reason = "steep-one-24-process-failed:" + *reason;
            return false;
        }
        expected.assign(kBoundaryTimeslots, 0.0);
        for (unsigned timeslot = offset; timeslot < kBoundaryTimeslots; ++timeslot) {
            expected[timeslot] = quant0(60U);
        }
        if (!checkSingleBandSeries(output, expected, "steep-one-24")) {
            return false;
        }
        ++(*interpolationCases);
    }

    // Steep two-point updates cover the same offset boundaries and verify that
    // the last point becomes the previous matrix for the next sequential AU.
    for (const std::array<unsigned, 2> offsets : {
             std::array<unsigned, 2> {1U, 12U},
             std::array<unsigned, 2> {12U, 24U}}) {
        FrameReport steepTwo24 = makeDenseFrame(0, 0, 1, 2, 1, 1, {12U},
                                                {offsets[0], offsets[1]});
        steepTwo24.objects[0].dataPoints[1].values = {24U};
        resetMatrixState(&state);
        if (!processMath(steepTwo24, kBoundaryTimeslots, &state, &output, reason)) {
            *reason = "steep-two-24-process-failed:" + *reason;
            return false;
        }
        expected.assign(kBoundaryTimeslots, 0.0);
        for (unsigned timeslot = offsets[0]; timeslot < kBoundaryTimeslots; ++timeslot) {
            expected[timeslot] = timeslot < offsets[1] ? quant0(60U) : quant0(72U);
        }
        if (!checkSingleBandSeries(output, expected, "steep-two-24")) {
            return false;
        }
        ++(*interpolationCases);
        if (offsets[1] == 24U) {
            FrameReport nextSteep = makeDenseFrame(1, 0, 1, 1, 1, 1, {36U}, {1U});
            if (!processMath(nextSteep, kBoundaryTimeslots, &state, &output, reason)) {
                *reason = "steep-previous-update-process-failed:" + *reason;
                return false;
            }
            expected.assign(kBoundaryTimeslots, quant0(84U));
            expected[0] = quant0(72U);
            if (!checkSingleBandSeries(output, expected, "steep-previous-update")) {
                return false;
            }
            ++(*interpolationCases);
        }
    }

    // A non-sequential sequence after the previous update must reset rather
    // than interpolate from stale coefficients.
    FrameReport discontinuity = makeDenseFrame(3, 0, 1, 1, 1, 1, {40U}, {1U});
    if (!processMath(discontinuity, kBoundaryTimeslots, &state, &output, reason)
        || !output.stateReset || !output.sequenceDiscontinuity) {
        *reason = "24-timeslot-discontinuity-reset-failed:" + *reason;
        return false;
    }
    expected.assign(kBoundaryTimeslots, quant0(88U));
    expected[0] = 0.0;
    if (!checkSingleBandSeries(output, expected, "24-timeslot-discontinuity")) {
        return false;
    }
    ++(*interpolationCases);
    return true;
}

} // namespace

void resetMatrixState(JocMatrixState *state)
{
    if (!state) {
        return;
    }
    *state = JocMatrixState {};
}

bool processMath(const FrameReport &frame,
                 unsigned numQmfTimeslots,
                 JocMatrixState *state,
                 JocMathFrame *output,
                 std::string *reason)
{
    if (!state || !output || !reason) {
        if (reason) {
            *reason = "joc-math-null-output";
        }
        return false;
    }
    if (frame.disposition == ParseDisposition::Malformed) {
        *reason = "joc-math-received-malformed-frame";
        return false;
    }
    if (!validDimensions(frame, reason)) {
        return false;
    }
    std::vector<JocQuantizedObject> quantized;
    if (!decodeQuantized(frame, &quantized, reason)) {
        return false;
    }
    std::vector<JocDequantizedObject> dequantized;
    if (!dequantize(quantized, &dequantized, reason)) {
        return false;
    }
    output->numChannels = frame.numChannels;
    output->numObjects = frame.numObjects;
    output->sequenceCount = frame.sequenceCount;
    output->quantized = std::move(quantized);
    output->dequantized = std::move(dequantized);
    return interpolate(frame, output->dequantized, numQmfTimeslots, state, output, reason);
}

JocMathSelfTestReport runMathSelfTest()
{
    JocMathSelfTestReport result;
    if (!checkMapping(&result.mappingCases, &result.reason)) {
        return result;
    }
    if (!checkMathCases(&result.interpolationCases, &result.reason)) {
        return result;
    }
    result.pass = true;
    result.reason = "mapping-dequantization-interpolation-sequence-cases";
    return result;
}

} // namespace eac3joc
