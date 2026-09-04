#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace eac3joc {

enum class JocSymbolKind {
    SparseFixedChannel,
    SparseHuffmanChannelDelta,
    HuffmanCoefficient,
};

const char *symbolKindText(JocSymbolKind kind);

struct JocHuffmanSymbolTrace {
    JocSymbolKind kind = JocSymbolKind::HuffmanCoefficient;
    std::string table;
    std::size_t bitOffset = 0;
    std::size_t bitLength = 0;
    std::string codeword;
    int symbol = 0;
    int parameterBand = -1;
    int inputChannel = -1;
    int resolvedInputChannel = -1;
};

struct JocDataPointTrace {
    bool hasOffsetTsRange = false;
    std::size_t offsetTsBitOffset = 0;
    std::size_t offsetTsBitEnd = 0;
    bool hasSymbolDataRange = false;
    std::size_t symbolDataBitOffset = 0;
    std::size_t symbolDataBitEnd = 0;
    std::vector<JocHuffmanSymbolTrace> symbols;
};

struct JocObjectTrace {
    std::size_t headerBitOffset = 0;
    std::size_t headerBitEnd = 0;
    bool hasDataRange = false;
    std::size_t dataBitOffset = 0;
    std::size_t dataBitEnd = 0;
    std::vector<JocDataPointTrace> dataPoints;
};

struct JocForensicTrace {
    std::size_t syntaxBitEnd = 0;
    std::size_t payloadBitCount = 0;
    std::string payloadHex;
    std::vector<JocObjectTrace> objects;
};

struct JocDataPoint {
    unsigned offsetTs = 0;
    std::vector<unsigned> channelIndices;
    std::vector<unsigned> values;
};

enum class ParseDisposition {
    Pass,
    Unsupported,
    Malformed,
};

struct JocObjectSummary {
    bool present = false;
    unsigned numBands = 0;
    bool sparse = false;
    unsigned quantIndex = 0;
    unsigned quantSteps = 0;
    unsigned slopeIndex = 0;
    unsigned numDataPoints = 0;
    std::size_t huffmanValueCount = 0;
    std::vector<JocDataPoint> dataPoints;
};

struct FrameReport {
    ParseDisposition disposition = ParseDisposition::Malformed;
    std::string reason;
    unsigned downmixConfigIndex = 0;
    unsigned numChannels = 0;
    unsigned numObjects = 0;
    unsigned extConfigIndex = 0;
    unsigned sequenceCount = 0;
    std::size_t bitsConsumed = 0;
    std::size_t paddingBits = 0;
    std::uint32_t paddingPattern = 0;
    std::vector<JocObjectSummary> objects;
    // Allocated only when parsePayload(..., captureForensics=true) is used.
    std::shared_ptr<JocForensicTrace> forensic;
};

struct HuffmanSelfTestReport {
    bool pass = false;
    std::string reason;
    std::size_t tableCount = 0;
    std::size_t leafCount = 0;
};

FrameReport parsePayload(const std::vector<std::uint8_t> &payload,
                         const std::string &tablePath,
                         bool captureForensics = false);

HuffmanSelfTestReport runHuffmanSelfTest(const std::string &tablePath);

const char *dispositionText(ParseDisposition disposition);

} // namespace eac3joc
