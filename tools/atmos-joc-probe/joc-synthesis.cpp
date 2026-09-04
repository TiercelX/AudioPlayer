// Gate 6A diagnostic-only object-QMF synthesis. This module turns one
// successful Gate 5C complex-QMF frame into renderer-neutral per-object PCM.
// It does not parse OAMD or touch production playback/rendering.

#include "joc-synthesis.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace eac3joc {
namespace {

constexpr unsigned kMaxObjects = 16;

bool validQwin(const std::vector<double> &qwin, std::string *reason)
{
    if (qwin.size() != eac3qmf::kFilterLength) {
        *reason = "qmf-prototype-size-mismatch";
        return false;
    }
    for (const double value : qwin) {
        if (!std::isfinite(value)) {
            *reason = "qmf-prototype-not-finite";
            return false;
        }
    }
    return true;
}

bool validInput(const JocQmfFrame &input,
                const std::vector<double> &qwin,
                std::string *reason)
{
    if (!validQwin(qwin, reason)) {
        return false;
    }
    if (input.numChannels == 0 || input.numChannels > 7) {
        *reason = "joc-synthesis-channel-count-out-of-range";
        return false;
    }
    if (input.numObjects == 0 || input.numObjects > kMaxObjects
        || input.objects.size() != input.numObjects) {
        *reason = "joc-synthesis-object-count-or-dimension-mismatch";
        return false;
    }
    for (const auto &object : input.objects) {
        if (object.size() != kSynthesisSamplesPerObject) {
            *reason = "joc-synthesis-qmf-sample-count-mismatch";
            return false;
        }
        for (const eac3qmf::Complex value : object) {
            if (!std::isfinite(value.real()) || !std::isfinite(value.imag())) {
                *reason = "joc-synthesis-qmf-value-not-finite";
                return false;
            }
        }
    }
    return true;
}

bool samePcm(const std::vector<float> &left,
            const std::vector<float> &right,
            float tolerance = 0.0f)
{
    if (left.size() != right.size()) {
        return false;
    }
    return std::equal(left.begin(), left.end(), right.begin(),
                      [tolerance](float a, float b) {
                          return std::abs(a - b) <= tolerance;
                      });
}

bool allZero(const std::vector<float> &values)
{
    return std::all_of(values.begin(), values.end(), [](float value) {
        return value == 0.0f;
    });
}

bool allFinite(const std::vector<float> &values)
{
    return std::all_of(values.begin(), values.end(), [](float value) {
        return std::isfinite(value);
    });
}

JocQmfFrame makeZeroFrame(unsigned objects, unsigned channels)
{
    JocQmfFrame frame;
    frame.numChannels = channels;
    frame.numObjects = objects;
    frame.objects.assign(objects,
                         std::vector<eac3qmf::Complex>(kSynthesisSamplesPerObject,
                                                        eac3qmf::Complex {0.0, 0.0}));
    return frame;
}

JocQmfFrame makeAnalysisFrame(const std::vector<double> &qwin,
                              const std::vector<std::vector<float>> &pcm)
{
    JocQmfFrame frame;
    frame.numChannels = 5;
    frame.numObjects = static_cast<unsigned>(pcm.size());
    frame.objects.assign(pcm.size(),
                         std::vector<eac3qmf::Complex>(kSynthesisSamplesPerObject));
    for (std::size_t object = 0; object < pcm.size(); ++object) {
        eac3qmf::Qmf bank(qwin);
        for (std::size_t slot = 0; slot < kSynthesisTimeslots; ++slot) {
            const std::vector<eac3qmf::Complex> values =
                bank.analyzeBlock(pcm[object].data() + slot * eac3qmf::kSubbands);
            std::copy(values.begin(), values.end(),
                      frame.objects[object].begin() + slot * eac3qmf::kSubbands);
        }
    }
    return frame;
}

std::vector<std::vector<float>> referenceSynthesis(
    const std::vector<double> &qwin,
    const JocQmfFrame &input)
{
    std::vector<std::vector<float>> output(
        input.numObjects, std::vector<float>(kSynthesisSamplesPerObject));
    for (unsigned object = 0; object < input.numObjects; ++object) {
        eac3qmf::Qmf bank(qwin);
        for (std::size_t slot = 0; slot < kSynthesisTimeslots; ++slot) {
            const std::vector<eac3qmf::Complex> values(
                input.objects[object].begin() + slot * eac3qmf::kSubbands,
                input.objects[object].begin() + (slot + 1) * eac3qmf::kSubbands);
            const auto block = bank.synthesizeBlock(values);
            std::copy(block.begin(), block.end(),
                      output[object].begin() + slot * eac3qmf::kSubbands);
        }
    }
    return output;
}

bool sameFrame(const JocSynthesisFrame &left, const JocSynthesisFrame &right)
{
    if (left.stateReset != right.stateReset
        || left.algorithmicDelaySamples != right.algorithmicDelaySamples
        || left.objects.size() != right.objects.size()) {
        return false;
    }
    return std::equal(left.objects.begin(), left.objects.end(), right.objects.begin(),
                      [](const auto &a, const auto &b) { return samePcm(a, b); });
}

} // namespace

void JocSynthesisState::reset()
{
    for (auto &bank : synthesis) {
        if (bank) {
            bank->reset();
        }
    }
    synthesis.clear();
    initialized = false;
    numObjects = 0;
}

bool synthesizeQmf(const JocQmfFrame &input,
                  const std::vector<double> &qwin,
                  JocSynthesisState *state,
                  JocSynthesisFrame *output,
                  std::string *reason)
{
    if (!state || !output || !reason) {
        return false;
    }
    if (!validInput(input, qwin, reason)) {
        return false;
    }

    const bool stateReset = input.stateReset || !state->initialized
        || state->numObjects != input.numObjects
        || state->synthesis.size() != input.numObjects;
    std::vector<std::unique_ptr<eac3qmf::Qmf>> working;
    working.reserve(input.numObjects);
    if (stateReset) {
        for (unsigned object = 0; object < input.numObjects; ++object) {
            working.push_back(std::make_unique<eac3qmf::Qmf>(qwin));
        }
    } else {
        for (const auto &bank : state->synthesis) {
            if (!bank) {
                *reason = "joc-synthesis-state-bank-missing";
                return false;
            }
            working.push_back(std::make_unique<eac3qmf::Qmf>(*bank));
        }
    }

    std::vector<std::vector<float>> synthesized(
        input.numObjects, std::vector<float>(kSynthesisSamplesPerObject));
    for (unsigned object = 0; object < input.numObjects; ++object) {
        for (std::size_t slot = 0; slot < kSynthesisTimeslots; ++slot) {
            const std::vector<eac3qmf::Complex> values(
                input.objects[object].begin() + slot * eac3qmf::kSubbands,
                input.objects[object].begin() + (slot + 1) * eac3qmf::kSubbands);
            const auto block = working[object]->synthesizeBlock(values);
            for (const float sample : block) {
                if (!std::isfinite(sample)) {
                    *reason = "joc-synthesis-output-not-finite";
                    return false;
                }
            }
            std::copy(block.begin(), block.end(),
                      synthesized[object].begin() + slot * eac3qmf::kSubbands);
        }
    }

    // Commit state and caller output only after every object and slot passed.
    state->synthesis = std::move(working);
    state->numObjects = input.numObjects;
    state->initialized = true;
    JocSynthesisFrame committed;
    committed.stateReset = stateReset;
    committed.algorithmicDelaySamples = kSynthesisAlgorithmicDelaySamples;
    committed.objects = std::move(synthesized);
    *output = std::move(committed);
    return true;
}

JocSynthesisSelfTestReport runSynthesisSelfTest(const std::vector<double> &qwin)
{
    JocSynthesisSelfTestReport report;
    std::string reason;
    if (!validQwin(qwin, &reason)) {
        report.reason = reason;
        return report;
    }

    // Zero QMF must produce exact zero while still advancing synthesis state.
    const JocQmfFrame zero = makeZeroFrame(1, 5);
    JocSynthesisState zeroState;
    JocSynthesisFrame zeroOutput;
    if (!synthesizeQmf(zero, qwin, &zeroState, &zeroOutput, &reason)
        || zeroOutput.objects.size() != 1 || !allFinite(zeroOutput.objects[0])
        || !allZero(zeroOutput.objects[0])) {
        report.reason = "zero-qmf-synthesis-failed:" + reason;
        return report;
    }
    ++report.cases;
    ++report.zeroQmfCases;

    std::vector<std::vector<float>> onePcm(1, std::vector<float>(kSynthesisSamplesPerObject));
    for (std::size_t sample = 0; sample < onePcm[0].size(); ++sample) {
        onePcm[0][sample] = static_cast<float>(
            0.25 * std::sin((sample + 1) * 0.017)
            + 0.09 * std::cos((sample + 1) * 0.031));
    }
    const JocQmfFrame one = makeAnalysisFrame(qwin, onePcm);
    JocSynthesisState oneState;
    JocSynthesisFrame oneOutput;
    if (!synthesizeQmf(one, qwin, &oneState, &oneOutput, &reason)
        || oneOutput.objects.size() != 1
        || !samePcm(oneOutput.objects[0], referenceSynthesis(qwin, one)[0])) {
        report.reason = "single-object-reference-mismatch:" + reason;
        return report;
    }
    ++report.cases;
    ++report.singleObjectCases;
    ++report.exactSizeCases;

    std::vector<std::vector<float>> twoPcm = onePcm;
    twoPcm.push_back(std::vector<float>(kSynthesisSamplesPerObject));
    for (std::size_t sample = 0; sample < twoPcm[1].size(); ++sample) {
        twoPcm[1][sample] = static_cast<float>(0.18 * std::sin((sample + 3) * 0.023));
    }
    twoPcm.push_back(std::vector<float>(kSynthesisSamplesPerObject));
    for (std::size_t sample = 0; sample < twoPcm[2].size(); ++sample) {
        twoPcm[2][sample] = static_cast<float>(0.13 * std::cos((sample + 7) * 0.029));
    }
    const JocQmfFrame two = makeAnalysisFrame(qwin, twoPcm);
    JocSynthesisState twoState;
    JocSynthesisFrame twoOutput;
    const auto twoReference = referenceSynthesis(qwin, two);
    if (!synthesizeQmf(two, qwin, &twoState, &twoOutput, &reason)
        || twoOutput.objects.size() != 3
        || !samePcm(twoOutput.objects[0], twoReference[0])
        || !samePcm(twoOutput.objects[1], twoReference[1])
        || !samePcm(twoOutput.objects[2], twoReference[2])) {
        report.reason = "multiple-object-independence-failed:" + reason;
        return report;
    }
    ++report.cases;
    ++report.multipleObjectCases;

    const JocQmfFrame sixteen = makeZeroFrame(16, 5);
    JocSynthesisState sixteenState;
    JocSynthesisFrame sixteenOutput;
    if (!synthesizeQmf(sixteen, qwin, &sixteenState, &sixteenOutput, &reason)
        || sixteenOutput.objects.size() != 16
        || !std::all_of(sixteenOutput.objects.begin(), sixteenOutput.objects.end(),
                        [](const auto &object) {
                            return object.size() == kSynthesisSamplesPerObject
                                && allFinite(object);
                        })) {
        report.reason = "sixteen-object-boundary-failed:" + reason;
        return report;
    }
    ++report.cases;
    ++report.sixteenObjectCases;
    ++report.exactSizeCases;

    JocQmfFrame frameA = one;
    JocQmfFrame frameB = one;
    for (std::size_t sample = 0; sample < frameB.objects[0].size(); ++sample) {
        frameB.objects[0][sample] *= eac3qmf::Complex {0.7, -0.1};
    }

    // A zero frame still advances the persistent bank. Its effect is visible
    // after a non-zero history, even though a fresh all-zero frame is exact
    // zero.
    JocSynthesisState advancedZeroState;
    JocSynthesisState skippedZeroState;
    JocSynthesisFrame ignoredZero;
    JocSynthesisFrame advancedTarget;
    JocSynthesisFrame skippedTarget;
    if (!synthesizeQmf(frameA, qwin, &advancedZeroState, &ignoredZero, &reason)
        || !synthesizeQmf(zero, qwin, &advancedZeroState, &ignoredZero, &reason)
        || !synthesizeQmf(frameB, qwin, &advancedZeroState,
                          &advancedTarget, &reason)
        || !synthesizeQmf(frameA, qwin, &skippedZeroState, &ignoredZero, &reason)
        || !synthesizeQmf(frameB, qwin, &skippedZeroState,
                          &skippedTarget, &reason)
        || sameFrame(advancedTarget, skippedTarget)) {
        report.reason = "zero-qmf-state-advance-failed:" + reason;
        return report;
    }

    JocSynthesisState splitState;
    JocSynthesisFrame splitA;
    JocSynthesisFrame splitB;
    if (!synthesizeQmf(frameA, qwin, &splitState, &splitA, &reason)
        || !synthesizeQmf(frameB, qwin, &splitState, &splitB, &reason)) {
        report.reason = "split-synthesis-call-failed:" + reason;
        return report;
    }
    eac3qmf::Qmf continuousBank(qwin);
    std::vector<float> continuousA(kSynthesisSamplesPerObject);
    std::vector<float> continuousB(kSynthesisSamplesPerObject);
    for (unsigned frameIndex = 0; frameIndex < 2; ++frameIndex) {
        const JocQmfFrame &frame = frameIndex == 0 ? frameA : frameB;
        std::vector<float> &destination = frameIndex == 0 ? continuousA : continuousB;
        for (std::size_t slot = 0; slot < kSynthesisTimeslots; ++slot) {
            const std::vector<eac3qmf::Complex> values(
                frame.objects[0].begin() + slot * eac3qmf::kSubbands,
                frame.objects[0].begin() + (slot + 1) * eac3qmf::kSubbands);
            const auto block = continuousBank.synthesizeBlock(values);
            std::copy(block.begin(), block.end(), destination.begin()
                      + slot * eac3qmf::kSubbands);
        }
    }
    if (!samePcm(splitA.objects[0], continuousA)
        || !samePcm(splitB.objects[0], continuousB)) {
        report.reason = "split-boundary-mismatch:" + reason;
        return report;
    }
    ++report.cases;
    ++report.splitCases;

    JocSynthesisState historyState;
    JocSynthesisFrame ignored;
    if (!synthesizeQmf(frameA, qwin, &historyState, &ignored, &reason)) {
        report.reason = "reset-history-failed:" + reason;
        return report;
    }
    JocQmfFrame resetFrame = frameB;
    resetFrame.stateReset = true;
    JocSynthesisFrame resetOutput;
    JocSynthesisState freshState;
    JocSynthesisFrame freshOutput;
    if (!synthesizeQmf(resetFrame, qwin, &historyState, &resetOutput, &reason)
        || !synthesizeQmf(resetFrame, qwin, &freshState, &freshOutput, &reason)
        || !resetOutput.stateReset || !sameFrame(resetOutput, freshOutput)) {
        report.reason = "explicit-reset-mismatch:" + reason;
        return report;
    }
    ++report.cases;
    ++report.resetCases;

    JocSynthesisState countState;
    if (!synthesizeQmf(frameA, qwin, &countState, &ignored, &reason)) {
        report.reason = "object-count-history-failed:" + reason;
        return report;
    }
    JocSynthesisFrame countOutput;
    if (!synthesizeQmf(two, qwin, &countState, &countOutput, &reason)
        || !countOutput.stateReset || !sameFrame(countOutput,
                                                  [&]() {
                                                      JocSynthesisState fresh;
                                                      JocSynthesisFrame result;
                                                      synthesizeQmf(two, qwin, &fresh,
                                                                    &result, &reason);
                                                      return result;
                                                  }())) {
        report.reason = "object-count-reset-mismatch:" + reason;
        return report;
    }
    ++report.cases;
    ++report.objectCountResetCases;

    auto checkBoundaryRejection = [&](const JocQmfFrame &invalidFrame,
                                      const std::vector<double> &table,
                                      const char *label) {
        JocSynthesisState testState;
        JocSynthesisState referenceState;
        JocSynthesisFrame historyOutput;
        JocSynthesisFrame referenceHistory;
        if (!synthesizeQmf(frameA, qwin, &testState, &historyOutput, &reason)
            || !synthesizeQmf(frameA, qwin, &referenceState,
                              &referenceHistory, &reason)) {
            report.reason = std::string(label) + "-history-failed:" + reason;
            return false;
        }
        JocSynthesisFrame callerOutput = historyOutput;
        const JocSynthesisFrame sentinel = callerOutput;
        const bool rejected = !synthesizeQmf(invalidFrame, table, &testState,
                                             &callerOutput, &reason);
        ++report.boundaryRejectionCases;
        JocSynthesisFrame afterReject;
        JocSynthesisFrame referenceAfter;
        const bool followed = synthesizeQmf(frameB, qwin, &testState,
                                            &afterReject, &reason)
            && synthesizeQmf(frameB, qwin, &referenceState,
                             &referenceAfter, &reason);
        const bool preserved = rejected && sameFrame(callerOutput, sentinel)
            && followed && sameFrame(afterReject, referenceAfter);
        if (preserved) {
            ++report.boundaryTransactionalCases;
            ++report.cases;
        } else {
            report.reason = std::string(label) + "-transactional-rejection-failed:" + reason;
            return false;
        }
        return true;
    };

    const JocQmfFrame zeroObjects = makeZeroFrame(0, 5);
    const JocQmfFrame seventeenObjects = makeZeroFrame(17, 5);
    JocQmfFrame shortObject = frameB;
    shortObject.objects[0].pop_back();
    std::vector<double> qwinLateNaN = qwin;
    qwinLateNaN.back() = std::numeric_limits<double>::quiet_NaN();
    if (!checkBoundaryRejection(zeroObjects, qwin, "zero-object")
        || !checkBoundaryRejection(seventeenObjects, qwin, "seventeen-object")
        || !checkBoundaryRejection(shortObject, qwin, "short-object")
        || !checkBoundaryRejection(frameB, qwinLateNaN, "late-qwin-nan")) {
        return report;
    }

    JocSynthesisState transactionalState;
    JocSynthesisFrame transactionalHistory;
    JocSynthesisFrame transactionalOutput = resetOutput;
    if (!synthesizeQmf(frameA, qwin, &transactionalState,
                       &transactionalHistory, &reason)) {
        report.reason = "transactional-history-failed:" + reason;
        return report;
    }
    JocSynthesisState referenceState;
    JocSynthesisFrame referenceHistory;
    if (!synthesizeQmf(frameA, qwin, &referenceState, &referenceHistory, &reason)) {
        report.reason = "transactional-reference-history-failed:" + reason;
        return report;
    }
    JocQmfFrame invalid = frameB;
    invalid.objects[0].back() = eac3qmf::Complex {
        std::numeric_limits<double>::quiet_NaN(), 0.0};
    const JocSynthesisFrame sentinel = transactionalOutput;
    const bool rejected = !synthesizeQmf(invalid, qwin, &transactionalState,
                                         &transactionalOutput, &reason);
    JocSynthesisFrame afterReject;
    JocSynthesisFrame referenceAfter;
    const bool followed = synthesizeQmf(frameB, qwin, &transactionalState,
                                        &afterReject, &reason)
        && synthesizeQmf(frameB, qwin, &referenceState,
                         &referenceAfter, &reason);
    if (!rejected || !sameFrame(transactionalOutput, sentinel)
        || !followed || !sameFrame(afterReject, referenceAfter)) {
        report.reason = "transactional-late-nonfinite-rejection-failed:" + reason;
        return report;
    }
    ++report.cases;
    ++report.transactionalRejectionCases;

    report.pass = true;
    report.reason = "zero-chain-multi-object-split-reset-count-and-transactional-cases";
    return report;
}

} // namespace eac3joc
