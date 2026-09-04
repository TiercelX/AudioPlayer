#pragma once

#include "joc-gate5b.h"
#include "qmf-bank.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace eac3joc {

enum class ChannelIdentity {
    FrontLeft,
    FrontRight,
    FrontCenter,
    SideLeft,
    SideRight,
    BackLeft,
    BackRight,
    TopFrontLeft,
    TopFrontRight,
    LowFrequency,
};

const std::vector<ChannelIdentity> &inputIdentities(unsigned downmixConfigIndex);
const char *channelIdentityText(ChannelIdentity identity);

struct JocQmfFrame {
    unsigned config = 0;
    unsigned numChannels = 0;
    unsigned numObjects = 0;
    bool stateReset = false;
    // Diagnostic-only capture of the analyzed Qin coefficients for the
    // accepted unit: [inputChannel][timeslot * 64 + subband].
    std::vector<std::vector<eac3qmf::Complex>> inputQmf;
    std::vector<std::vector<eac3qmf::Complex>> objects;
};

struct JocQmfState {
    unsigned config = 0;
    unsigned numChannels = 0;
    unsigned numObjects = 0;
    bool initialized = false;
    std::vector<std::unique_ptr<eac3qmf::Qmf>> analysis;

    void reset();
};

struct JocQmfSelfTestReport {
    bool pass = false;
    std::size_t cases = 0;
    std::size_t zeroMatrixCases = 0;
    std::size_t singleChannelCopyCases = 0;
    std::size_t identityLikeCases = 0;
    std::size_t cancellationCases = 0;
    std::size_t resetEquivalenceCases = 0;
    std::size_t dimensionMismatchCases = 0;
    std::size_t nonFiniteRejectionCases = 0;
    std::size_t transactionalRejectionCases = 0;
    std::size_t reservedConfigRejects = 0;
    std::size_t configIdentityCases = 0;
    bool configIdentityOrderPass = false;
    std::string reason;
};

bool reconstructQmf(const JocMathFrame &math,
                    unsigned downmixConfigIndex,
                    const std::vector<std::vector<float>> &orderedPcm,
                    const std::vector<double> &qwin,
                    JocQmfState *state,
                    JocQmfFrame *output,
                    std::string *reason);

JocQmfSelfTestReport runQmfSelfTest(const std::vector<double> &qwin);

} // namespace eac3joc
