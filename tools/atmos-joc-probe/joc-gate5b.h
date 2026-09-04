#pragma once

#include "joc-gate5a.h"

#include <cstddef>
#include <string>
#include <vector>

namespace eac3joc {

struct JocQuantizedObject {
    bool present = false;
    unsigned numChannels = 0;
    unsigned numBands = 0;
    unsigned quantIndex = 0;
    unsigned slopeIndex = 0;
    std::vector<std::vector<std::vector<unsigned>>> values;
};

struct JocDequantizedObject {
    bool present = false;
    unsigned numChannels = 0;
    unsigned numBands = 0;
    unsigned quantIndex = 0;
    unsigned slopeIndex = 0;
    std::vector<std::vector<std::vector<double>>> values;
};

struct JocInterpolatedObject {
    bool present = false;
    unsigned numChannels = 0;
    std::vector<std::vector<std::vector<double>>> values;
};

struct JocMathFrame {
    unsigned numChannels = 0;
    unsigned numObjects = 0;
    unsigned sequenceCount = 0;
    bool stateReset = false;
    bool sequenceDiscontinuity = false;
    std::vector<JocQuantizedObject> quantized;
    std::vector<JocDequantizedObject> dequantized;
    std::vector<JocInterpolatedObject> interpolated;
};

struct JocMatrixState {
    bool initialized = false;
    unsigned previousSequence = 0;
    unsigned numChannels = 0;
    unsigned numObjects = 0;
    std::vector<std::vector<std::vector<double>>> previous;
};

struct JocMathSelfTestReport {
    bool pass = false;
    std::string reason;
    std::size_t mappingCases = 0;
    std::size_t interpolationCases = 0;
};

void resetMatrixState(JocMatrixState *state);

bool processMath(const FrameReport &frame,
                 unsigned numQmfTimeslots,
                 JocMatrixState *state,
                 JocMathFrame *output,
                 std::string *reason);

JocMathSelfTestReport runMathSelfTest();

} // namespace eac3joc
