#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace eac3oamd {

enum class B2aDisposition {
    Pass,
    Unsupported,
    Malformed,
};

enum class B2aBitOrder {
    Syntax5511Lsb,
    Table31Msb,
};

const char *b2aBitOrderText(B2aBitOrder order);

const char *b2aDispositionText(B2aDisposition disposition);

struct B2aBlockUpdate {
    unsigned blockOffsetFactor = 0;
    unsigned rampDurationCode = 0;
    bool rampDurationIndexPresent = false;
    unsigned rampDurationIndex = 0;
    bool rampDurationBitsPresent = false;
    unsigned rampDurationBits = 0;
};

struct B2aObjectInfo {
    unsigned objectIndex = 0;
    unsigned blockIndex = 0;
    bool objectNotActive = false;
    unsigned basicInfoStatus = 0;
    std::array<bool, 2> basicInfoPresence {};
    bool gainIndexPresent = false;
    unsigned gainIndex = 0;
    bool gainBitsPresent = false;
    unsigned gainBits = 0;
    bool defaultPriorityPresent = false;
    bool defaultPriority = false;
    bool priorityBitsPresent = false;
    unsigned priorityBits = 0;
    unsigned renderInfoStatus = 0;
    // Stable semantic order, independent of wire interpretation:
    // position, zone, size, screen.  B2aBitOrder controls how the four
    // transmitted bits populate this array.
    std::array<bool, 4> renderInfoPresence {};
    bool differentialPositionPresent = false;
    bool differentialPosition = false;
    std::array<unsigned, 3> differentialPositionBits {};
    bool absolutePositionPresent = false;
    unsigned absoluteXBits = 0;
    unsigned absoluteYBits = 0;
    unsigned absoluteZSignBits = 0;
    unsigned absoluteZBits = 0;
    bool distanceSpecified = false;
    bool objectAtInfinity = false;
    bool distanceFactorPresent = false;
    unsigned distanceFactorIndex = 0;
    bool zonePresent = false;
    unsigned zoneConstraintsIndex = 0;
    bool enableElevation = false;
    bool sizePresent = false;
    unsigned sizeIndex = 0;
    unsigned sizeBits = 0;
    unsigned widthBits = 0;
    unsigned depthBits = 0;
    unsigned heightBits = 0;
    bool screenReferencePresent = false;
    bool useScreenReference = false;
    unsigned screenFactorBits = 0;
    unsigned depthFactorIndex = 0;
    bool snapPresent = false;
    bool snap = false;
    bool additionalDataPresent = false;
    unsigned additionalDataSizeBytes = 0;
    std::size_t additionalDataBits = 0;
    std::vector<std::uint8_t> additionalData;
};

struct B2aFrame {
    B2aDisposition disposition = B2aDisposition::Malformed;
    std::string reason;
    unsigned sampleOffsetCode = 0;
    bool sampleOffsetIndexPresent = false;
    unsigned sampleOffsetIndex = 0;
    bool sampleOffsetBitsPresent = false;
    unsigned sampleOffsetBits = 0;
    unsigned objectInfoBlockCount = 0;
    bool reservedDataNotPresent = true;
    unsigned reservedData = 0;
    std::vector<B2aBlockUpdate> blocks;
    std::vector<B2aObjectInfo> objectInfo;
    std::size_t bitsConsumed = 0;
    std::size_t paddingBits = 0;
    std::size_t failureBitOffset = 0;
    unsigned failureObjectIndex = 0;
    unsigned failureBlockIndex = 0;
};

B2aFrame parseObjectElement(const std::vector<std::uint8_t> &body,
                            std::size_t bodyBits,
                            unsigned objectCount,
                            const std::vector<bool> &objectInBedOrIsf,
                            B2aBitOrder bitOrder = B2aBitOrder::Syntax5511Lsb);

struct B2aSelfTestReport {
    bool pass = false;
    std::size_t cases = 0;
    std::size_t branchCases = 0;
    std::size_t sampleOffsetCases = 0;
    std::size_t individualPresenceCases = 0;
    std::size_t table31PresenceCases = 0;
    std::size_t oneBlockCases = 0;
    std::size_t eightBlockCases = 0;
    std::size_t truncatedCases = 0;
    std::size_t arithmeticCases = 0;
    std::size_t reservedCases = 0;
    std::size_t paddingCases = 0;
    std::size_t additionalDataCases = 0;
    std::size_t lfeHelperCases = 0;
    std::size_t lfeWrongIndexCases = 0;
    std::string reason;
};

B2aSelfTestReport runB2aSelfTest();

} // namespace eac3oamd
