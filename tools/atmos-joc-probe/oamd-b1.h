#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace eac3oamd {

enum class B1Disposition {
    Pass,
    Unsupported,
    Malformed,
};

const char *dispositionText(B1Disposition disposition);

struct ProgramAssignment {
    bool dynamicOnly = false;
    bool lfePresent = false;
    std::array<bool, 4> contentDescription {};
    bool bedChannelDistribute = false;
    bool multipleBedInstances = false;
    unsigned bedInstances = 0;
    std::vector<bool> bedLfeOnly;
    std::vector<bool> bedStandardChannelAssignment;
    std::vector<unsigned> bedChannelAssignments;
    std::vector<unsigned> nonstandardBedAssignments;
    unsigned intermediateSpatialFormat = 0;
    unsigned dynamicObjects = 0;
    std::string programType;
};

struct ElementInventory {
    unsigned id = 0;
    unsigned sizeBytes = 0;
    bool alternateIdPresent = false;
    unsigned alternateId = 0;
    bool discardUnknown = false;
    bool recognized = false;
    std::size_t rawBodyBits = 0;
    std::vector<std::uint8_t> rawBody;
};

struct B1Frame {
    B1Disposition disposition = B1Disposition::Malformed;
    std::string reason;
    unsigned version = 0;
    bool versionExtended = false;
    unsigned objectCount = 0;
    bool objectCountExtended = false;
    ProgramAssignment program;
    bool alternateObjectDataPresent = false;
    unsigned elementCount = 0;
    std::vector<ElementInventory> elements;
    std::size_t finalPaddingBits = 0;
    std::size_t bitsConsumed = 0;
};

B1Frame parseB1(const std::vector<std::uint8_t> &payload);

struct B1SelfTestReport {
    bool pass = false;
    std::size_t cases = 0;
    std::size_t normalCases = 0;
    std::size_t programAssignmentCases = 0;
    std::size_t extendedCases = 0;
    std::size_t unsupportedVersionCases = 0;
    std::size_t syntaxBoundaryCases = 0;
    std::size_t reservedValueCases = 0;
    std::size_t variableGroupCases = 0;
    std::size_t maxVariableSizeCases = 0;
    std::size_t truncatedCases = 0;
    std::size_t sizeOverrunCases = 0;
    std::size_t nonzeroPaddingCases = 0;
    std::size_t recognizedCases = 0;
    std::size_t discardableUnknownCases = 0;
    std::size_t nondiscardableUnknownCases = 0;
    std::string reason;
};

B1SelfTestReport runB1SelfTest();

} // namespace eac3oamd
