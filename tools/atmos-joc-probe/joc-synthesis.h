#pragma once

#include "joc-qmf.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace eac3joc {

constexpr std::size_t kSynthesisSamplesPerObject = 1536;
constexpr std::size_t kSynthesisTimeslots = 24;
constexpr int kSynthesisAlgorithmicDelaySamples =
    eac3qmf::kAnalysisSynthesisDelaySamples;

struct JocSynthesisFrame {
    bool stateReset = false;
    int algorithmicDelaySamples = kSynthesisAlgorithmicDelaySamples;
    std::vector<std::vector<float>> objects;
};

struct JocSynthesisState {
    bool initialized = false;
    unsigned numObjects = 0;
    std::vector<std::unique_ptr<eac3qmf::Qmf>> synthesis;

    void reset();
};

struct JocSynthesisSelfTestReport {
    bool pass = false;
    std::size_t cases = 0;
    std::size_t zeroQmfCases = 0;
    std::size_t singleObjectCases = 0;
    std::size_t multipleObjectCases = 0;
    std::size_t splitCases = 0;
    std::size_t resetCases = 0;
    std::size_t objectCountResetCases = 0;
    std::size_t transactionalRejectionCases = 0;
    std::size_t exactSizeCases = 0;
    std::size_t sixteenObjectCases = 0;
    std::size_t boundaryRejectionCases = 0;
    std::size_t boundaryTransactionalCases = 0;
    std::string reason;
};

bool synthesizeQmf(const JocQmfFrame &input,
                  const std::vector<double> &qwin,
                  JocSynthesisState *state,
                  JocSynthesisFrame *output,
                  std::string *reason);

JocSynthesisSelfTestReport runSynthesisSelfTest(const std::vector<double> &qwin);

} // namespace eac3joc
