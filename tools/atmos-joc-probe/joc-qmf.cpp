// Gate 5C diagnostic: reconstruct bounded complex object QMF from native PCM
// and the already-reviewed Gate 5B interpolation output. This module has no
// OAMD, renderer, FFmpeg, or production playback dependency.

#include "joc-qmf.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace eac3joc {
namespace {

constexpr unsigned kSamplesPerUnit = 1536;
constexpr unsigned kQmfTimeslots = 24;
constexpr unsigned kMaxObjects = 16;
constexpr unsigned kMaxChannels = 7;
constexpr double kCoefficientBound = 64.0;

const std::vector<ChannelIdentity> kFiveChannels = {
    ChannelIdentity::FrontLeft, ChannelIdentity::FrontRight, ChannelIdentity::FrontCenter,
    ChannelIdentity::SideLeft, ChannelIdentity::SideRight,
};
const std::vector<ChannelIdentity> kSevenBackChannels = {
    ChannelIdentity::FrontLeft, ChannelIdentity::FrontRight, ChannelIdentity::FrontCenter,
    ChannelIdentity::SideLeft, ChannelIdentity::SideRight,
    ChannelIdentity::BackLeft, ChannelIdentity::BackRight,
};
const std::vector<ChannelIdentity> kSevenTopChannels = {
    ChannelIdentity::FrontLeft, ChannelIdentity::FrontRight, ChannelIdentity::FrontCenter,
    ChannelIdentity::SideLeft, ChannelIdentity::SideRight,
    ChannelIdentity::TopFrontLeft, ChannelIdentity::TopFrontRight,
};

bool finiteComplex(const eac3qmf::Complex &value)
{
    return std::isfinite(value.real()) && std::isfinite(value.imag());
}

bool validateMatrices(const JocMathFrame &math, std::string *reason)
{
    if (!reason) {
        return false;
    }
    for (unsigned object = 0; object < math.numObjects; ++object) {
        const JocInterpolatedObject &matrix = math.interpolated[object];
        if (!matrix.present) {
            continue;
        }
        if (matrix.numChannels != math.numChannels
            || matrix.values.size() != kQmfTimeslots) {
            *reason = "joc-qmf-matrix-timeslot-dimension-mismatch";
            return false;
        }
        for (unsigned timeslot = 0; timeslot < kQmfTimeslots; ++timeslot) {
            if (matrix.values[timeslot].size() != math.numChannels) {
                *reason = "joc-qmf-matrix-channel-dimension-mismatch";
                return false;
            }
            for (unsigned channel = 0; channel < math.numChannels; ++channel) {
                if (matrix.values[timeslot][channel].size() != eac3qmf::kSubbands) {
                    *reason = "joc-qmf-matrix-band-dimension-mismatch";
                    return false;
                }
                for (unsigned subband = 0; subband < eac3qmf::kSubbands; ++subband) {
                    const double coefficient = matrix.values[timeslot][channel][subband];
                    if (!std::isfinite(coefficient) || std::abs(coefficient) > kCoefficientBound) {
                        *reason = "joc-qmf-coefficient-not-finite-or-bounded";
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

JocMathFrame makeSynthetic(unsigned config, unsigned objects, unsigned channels)
{
    JocMathFrame frame;
    frame.numChannels = channels;
    frame.numObjects = objects;
    frame.interpolated.resize(objects);
    for (unsigned object = 0; object < objects; ++object) {
        JocInterpolatedObject &target = frame.interpolated[object];
        target.present = true;
        target.numChannels = channels;
        target.values.assign(kQmfTimeslots,
                             std::vector<std::vector<double>>(
                                 channels, std::vector<double>(eac3qmf::kSubbands, 0.0)));
    }
    static_cast<void>(config);
    return frame;
}

bool closeQmf(const std::vector<eac3qmf::Complex> &left,
              const std::vector<eac3qmf::Complex> &right,
              double tolerance = 1.0e-12)
{
    if (left.size() != right.size()) {
        return false;
    }
    return std::equal(left.begin(), left.end(), right.begin(),
                      [tolerance](const auto &a, const auto &b) {
                          return std::abs(a - b) <= tolerance;
                      });
}

bool allZero(const std::vector<eac3qmf::Complex> &values)
{
    return std::all_of(values.begin(), values.end(), [](const auto &value) {
        return std::abs(value) <= 1.0e-12;
    });
}

} // namespace

const std::vector<ChannelIdentity> &inputIdentities(unsigned downmixConfigIndex)
{
    switch (downmixConfigIndex) {
    case 0:
    case 3:
        return kFiveChannels;
    case 1:
        return kSevenBackChannels;
    case 2:
    case 4:
        return kSevenTopChannels;
    default: {
        static const std::vector<ChannelIdentity> empty;
        return empty;
    }
    }
}

const char *channelIdentityText(ChannelIdentity identity)
{
    switch (identity) {
    case ChannelIdentity::FrontLeft: return "FL";
    case ChannelIdentity::FrontRight: return "FR";
    case ChannelIdentity::FrontCenter: return "FC";
    case ChannelIdentity::SideLeft: return "SL";
    case ChannelIdentity::SideRight: return "SR";
    case ChannelIdentity::BackLeft: return "BL";
    case ChannelIdentity::BackRight: return "BR";
    case ChannelIdentity::TopFrontLeft: return "TFL";
    case ChannelIdentity::TopFrontRight: return "TFR";
    case ChannelIdentity::LowFrequency: return "LFE";
    }
    return "UNKNOWN";
}

void JocQmfState::reset()
{
    for (auto &bank : analysis) {
        if (bank) {
            bank->reset();
        }
    }
    analysis.clear();
    initialized = false;
    config = 0;
    numChannels = 0;
    numObjects = 0;
}

bool reconstructQmf(const JocMathFrame &math,
                    unsigned downmixConfigIndex,
                    const std::vector<std::vector<float>> &orderedPcm,
                    const std::vector<double> &qwin,
                    JocQmfState *state,
                    JocQmfFrame *output,
                    std::string *reason)
{
    if (!state || !output || !reason) {
        return false;
    }
    const std::vector<ChannelIdentity> &identities = inputIdentities(downmixConfigIndex);
    if (identities.empty()) {
        *reason = downmixConfigIndex >= 5 ? "reserved-downmix-config" : "unknown-downmix-config";
        return false;
    }
    if (qwin.size() != eac3qmf::kFilterLength) {
        *reason = "qmf-prototype-size-mismatch";
        return false;
    }
    if (math.numChannels != identities.size() || math.numChannels == 0
        || math.numChannels > kMaxChannels || math.numObjects == 0
        || math.numObjects > kMaxObjects || math.interpolated.size() != math.numObjects) {
        *reason = "joc-qmf-dimension-mismatch";
        return false;
    }
    if (orderedPcm.size() != identities.size()
        || std::any_of(orderedPcm.begin(), orderedPcm.end(), [](const auto &channel) {
               return channel.size() != kSamplesPerUnit;
           })) {
        *reason = "native-pcm-layout-or-sample-count-mismatch";
        return false;
    }
    // Validate the complete matrix before touching persistent QMF state. A
    // malformed coefficient late in a frame must not advance analysis history
    // before the caller selects its explicit fallback path.
    if (!validateMatrices(math, reason)) {
        return false;
    }
    bool stateReset = math.stateReset || !state->initialized
        || state->config != downmixConfigIndex
        || state->numChannels != math.numChannels || state->numObjects != math.numObjects;
    if (stateReset) {
        state->reset();
        state->config = downmixConfigIndex;
        state->numChannels = math.numChannels;
        state->numObjects = math.numObjects;
        state->analysis.reserve(math.numChannels);
        for (unsigned channel = 0; channel < math.numChannels; ++channel) {
            state->analysis.push_back(std::make_unique<eac3qmf::Qmf>(qwin));
        }
    }
    if (state->analysis.size() != math.numChannels) {
        *reason = "qmf-analysis-state-dimension-mismatch";
        return false;
    }

    output->config = downmixConfigIndex;
    output->numChannels = math.numChannels;
    output->numObjects = math.numObjects;
    output->stateReset = stateReset || math.stateReset;
    output->inputQmf.assign(math.numChannels,
                            std::vector<eac3qmf::Complex>(kSamplesPerUnit,
                                                           eac3qmf::Complex {0.0, 0.0}));
    output->objects.assign(math.numObjects,
                           std::vector<eac3qmf::Complex>(kSamplesPerUnit,
                                                           eac3qmf::Complex {0.0, 0.0}));
    for (unsigned timeslot = 0; timeslot < kQmfTimeslots; ++timeslot) {
        std::vector<std::vector<eac3qmf::Complex>> channels(math.numChannels);
        for (unsigned channel = 0; channel < math.numChannels; ++channel) {
            channels[channel] = state->analysis[channel]->analyzeBlock(
                orderedPcm[channel].data() + timeslot * eac3qmf::kSubbands);
            std::copy(channels[channel].begin(), channels[channel].end(),
                      output->inputQmf[channel].begin()
                          + timeslot * eac3qmf::kSubbands);
        }
        for (unsigned object = 0; object < math.numObjects; ++object) {
            if (object >= math.interpolated.size() || !math.interpolated[object].present) {
                continue;
            }
            const JocInterpolatedObject &matrix = math.interpolated[object];
            for (unsigned subband = 0; subband < eac3qmf::kSubbands; ++subband) {
                eac3qmf::Complex value {0.0, 0.0};
                for (unsigned channel = 0; channel < math.numChannels; ++channel) {
                    const double coefficient = matrix.values[timeslot][channel][subband];
                    value += coefficient * channels[channel][subband];
                }
                if (!finiteComplex(value)) {
                    *reason = "joc-qmf-output-not-finite";
                    return false;
                }
                output->objects[object][timeslot * eac3qmf::kSubbands + subband] = value;
            }
        }
    }
    state->initialized = true;
    return true;
}

JocQmfSelfTestReport runQmfSelfTest(const std::vector<double> &qwin)
{
    JocQmfSelfTestReport report;
    if (qwin.size() != eac3qmf::kFilterLength) {
        report.reason = "qmf-prototype-size-mismatch";
        return report;
    }
    std::vector<std::vector<float>> input(5, std::vector<float>(kSamplesPerUnit));
    for (unsigned channel = 0; channel < input.size(); ++channel) {
        for (unsigned sample = 0; sample < kSamplesPerUnit; ++sample) {
            input[channel][sample] = static_cast<float>(
                0.1 * (channel + 1) * std::sin((sample + 1) * (channel + 2) * 0.013));
        }
    }
    JocMathFrame zeroMatrix = makeSynthetic(0, 1, 5);
    JocQmfState state;
    JocQmfFrame output;
    std::string reason;
    if (!reconstructQmf(zeroMatrix, 0, input, qwin, &state, &output, &reason)
        || output.objects.size() != 1 || !allZero(output.objects[0])) {
        report.reason = "zero-matrix-nonzero-input-case-failed:" + reason;
        return report;
    }
    ++report.cases;
    report.zeroMatrixCases = 1;

    JocMathFrame copy = makeSynthetic(0, 1, 5);
    for (unsigned timeslot = 0; timeslot < kQmfTimeslots; ++timeslot) {
        for (unsigned subband = 0; subband < eac3qmf::kSubbands; ++subband) {
            copy.interpolated[0].values[timeslot][0][subband] = 1.0;
        }
    }
    state.reset();
    if (!reconstructQmf(copy, 0, input, qwin, &state, &output, &reason)) {
        report.reason = "single-channel-copy-case-failed:" + reason;
        return report;
    }
    eac3qmf::Qmf expectedBank(qwin);
    std::vector<eac3qmf::Complex> expected;
    for (unsigned timeslot = 0; timeslot < kQmfTimeslots; ++timeslot) {
        const auto block = expectedBank.analyzeBlock(input[0].data() + timeslot * eac3qmf::kSubbands);
        expected.insert(expected.end(), block.begin(), block.end());
    }
    if (!closeQmf(output.objects[0], expected)) {
        report.reason = "single-channel-copy-case-mismatch";
        return report;
    }
    ++report.cases;
    report.singleChannelCopyCases = 1;

    JocMathFrame resetTarget = copy;
    resetTarget.stateReset = true;
    JocQmfState historyState;
    JocQmfFrame historyOutput;
    if (!reconstructQmf(copy, 0, input, qwin, &historyState, &historyOutput, &reason)) {
        report.reason = "reset-equivalence-history-case-failed:" + reason;
        return report;
    }
    JocQmfFrame resetOutput;
    if (!reconstructQmf(resetTarget, 0, input, qwin, &historyState, &resetOutput, &reason)
        || !resetOutput.stateReset) {
        report.reason = "reset-equivalence-reset-case-failed:" + reason;
        return report;
    }
    JocQmfState freshState;
    JocQmfFrame freshOutput;
    if (!reconstructQmf(resetTarget, 0, input, qwin, &freshState, &freshOutput, &reason)
        || !freshOutput.stateReset
        || freshOutput.objects.size() != resetOutput.objects.size()
        || !std::equal(resetOutput.objects.begin(), resetOutput.objects.end(),
                       freshOutput.objects.begin(), [](const auto &left, const auto &right) {
                           return closeQmf(left, right);
                       })) {
        report.reason = "reset-equivalence-output-mismatch:" + reason;
        return report;
    }
    ++report.cases;
    report.resetEquivalenceCases = 1;

    JocMathFrame identity = makeSynthetic(0, 5, 5);
    for (unsigned object = 0; object < 5; ++object) {
        for (unsigned timeslot = 0; timeslot < kQmfTimeslots; ++timeslot) {
            for (unsigned subband = 0; subband < eac3qmf::kSubbands; ++subband) {
                identity.interpolated[object].values[timeslot][object][subband] = 1.0;
            }
        }
    }
    state.reset();
    if (!reconstructQmf(identity, 0, input, qwin, &state, &output, &reason)
        || output.objects.size() != 5) {
        report.reason = "identity-like-case-failed:" + reason;
        return report;
    }
    std::vector<eac3qmf::Qmf> identityBanks;
    identityBanks.reserve(5);
    for (unsigned channel = 0; channel < 5; ++channel) {
        identityBanks.emplace_back(qwin);
    }
    for (unsigned object = 0; object < 5; ++object) {
        std::vector<eac3qmf::Complex> identityExpected;
        for (unsigned timeslot = 0; timeslot < kQmfTimeslots; ++timeslot) {
            const auto block = identityBanks[object].analyzeBlock(
                input[object].data() + timeslot * eac3qmf::kSubbands);
            identityExpected.insert(identityExpected.end(), block.begin(), block.end());
        }
        if (!closeQmf(output.objects[object], identityExpected)) {
            report.reason = "identity-like-case-mismatch";
            return report;
        }
    }
    ++report.cases;
    report.identityLikeCases = 1;

    JocMathFrame cancel = makeSynthetic(0, 1, 5);
    for (unsigned timeslot = 0; timeslot < kQmfTimeslots; ++timeslot) {
        for (unsigned subband = 0; subband < eac3qmf::kSubbands; ++subband) {
            cancel.interpolated[0].values[timeslot][0][subband] = 1.0;
            cancel.interpolated[0].values[timeslot][1][subband] = -1.0;
        }
    }
    state.reset();
    auto identical = input;
    identical[1] = identical[0];
    if (!reconstructQmf(cancel, 0, identical, qwin, &state, &output, &reason)
        || !allZero(output.objects[0])) {
        report.reason = "cancellation-case-failed:" + reason;
        return report;
    }
    ++report.cases;
    report.cancellationCases = 1;

    JocMathFrame badDimensions = makeSynthetic(0, 1, 4);
    state.reset();
    if (reconstructQmf(badDimensions, 0, input, qwin, &state, &output, &reason)
        || reason != "joc-qmf-dimension-mismatch") {
        report.reason = "dimension-mismatch-case-not-rejected";
        return report;
    }
    ++report.cases;
    report.dimensionMismatchCases = 1;

    JocMathFrame nonFinite = makeSynthetic(0, 1, 5);
    nonFinite.interpolated[0].values[0][0][0] = std::numeric_limits<double>::quiet_NaN();
    state.reset();
    if (reconstructQmf(nonFinite, 0, input, qwin, &state, &output, &reason)
        || reason != "joc-qmf-coefficient-not-finite-or-bounded") {
        report.reason = "non-finite-coefficient-case-not-rejected";
        return report;
    }
    ++report.cases;
    report.nonFiniteRejectionCases = 1;

    JocMathFrame lateInvalid = copy;
    lateInvalid.interpolated[0].values[17][3][42]
        = std::numeric_limits<double>::quiet_NaN();
    JocQmfState transactionalState;
    JocQmfState referenceState;
    JocQmfFrame transactionalHistoryOutput;
    JocQmfFrame referenceHistoryOutput;
    if (!reconstructQmf(copy, 0, input, qwin, &transactionalState,
                        &transactionalHistoryOutput, &reason)
        || !reconstructQmf(copy, 0, input, qwin, &referenceState,
                           &referenceHistoryOutput, &reason)) {
        report.reason = "transactional-rejection-history-case-failed:" + reason;
        return report;
    }
    JocQmfFrame rejectedOutput;
    rejectedOutput.config = 99;
    rejectedOutput.objects.assign(1, std::vector<eac3qmf::Complex>(1));
    if (reconstructQmf(lateInvalid, 0, input, qwin, &transactionalState,
                       &rejectedOutput, &reason)
        || reason != "joc-qmf-coefficient-not-finite-or-bounded"
        || rejectedOutput.config != 99 || rejectedOutput.objects.size() != 1
        || rejectedOutput.objects[0].size() != 1) {
        report.reason = "transactional-rejection-was-not-atomic";
        return report;
    }
    JocQmfFrame transactionalTarget;
    JocQmfFrame referenceTarget;
    if (!reconstructQmf(copy, 0, input, qwin, &transactionalState,
                        &transactionalTarget, &reason)
        || !reconstructQmf(copy, 0, input, qwin, &referenceState,
                           &referenceTarget, &reason)
        || transactionalTarget.objects.size() != referenceTarget.objects.size()
        || !std::equal(transactionalTarget.objects.begin(), transactionalTarget.objects.end(),
                       referenceTarget.objects.begin(), [](const auto &left, const auto &right) {
                           return closeQmf(left, right);
                       })) {
        report.reason = "transactional-rejection-state-contaminated:" + reason;
        return report;
    }
    ++report.cases;
    report.transactionalRejectionCases = 1;

    unsigned reservedRejected = 0;
    for (unsigned config = 5; config <= 7; ++config) {
        JocMathFrame reserved = makeSynthetic(config, 1, 5);
        state.reset();
        if (!reconstructQmf(reserved, config, input, qwin, &state, &output, &reason)
            && reason == "reserved-downmix-config") {
            ++reservedRejected;
            ++report.reservedConfigRejects;
            ++report.cases;
        } else {
            report.reason = "reserved-config-rejection-failed-" + std::to_string(config);
            return report;
        }
    }
    if (reservedRejected != 3) {
        report.reason = "reserved-config-rejection-count-mismatch";
        return report;
    }
    for (unsigned config = 0; config <= 4; ++config) {
        const auto &identities = inputIdentities(config);
        const std::vector<ChannelIdentity> expected =
            (config == 0 || config == 3) ? kFiveChannels
            : (config == 1 ? kSevenBackChannels : kSevenTopChannels);
        if (identities != expected) {
            report.reason = "config-identity-order-or-coverage-failed";
            return report;
        }
        ++report.cases;
        ++report.configIdentityCases;
    }
    report.configIdentityOrderPass = true;
    report.pass = true;
    report.reason = "zero-matrix-copy-identity-cancellation-reset-and-reserved-config-cases";
    return report;
}

} // namespace eac3joc
