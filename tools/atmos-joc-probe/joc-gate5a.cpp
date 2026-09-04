// Gate 5A diagnostic: bounded ETSI TS 103 420 JOC syntax and Huffman tables.
// This file intentionally consumes EMDF payload 14 bytes only. It does not
// perform dequantization, QMF processing, or production playback routing.

#include "joc-gate5a.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>

namespace eac3joc {
namespace {

constexpr std::array<unsigned, 8> kNumBandsByIndex = {1, 3, 5, 7, 9, 12, 15, 23};
constexpr std::array<const char *, 6> kTableNames = {
    "joc_huff_code_coarse_generic",
    "joc_huff_code_fine_generic",
    "joc_huff_code_coarse_coeff_sparse",
    "joc_huff_code_fine_coeff_sparse",
    "joc_huff_code_5ch_pos_index_sparse",
    "joc_huff_code_7ch_pos_index_sparse",
};

struct BitReader {
    const std::vector<std::uint8_t> *data = nullptr;
    std::size_t bit = 0;

    explicit BitReader(const std::vector<std::uint8_t> *bytes)
        : data(bytes)
    {
    }

    bool canRead(unsigned count) const
    {
        if (!data || bit > data->size() * 8U) {
            return false;
        }
        return count <= data->size() * 8U - bit;
    }

    bool read(unsigned count, unsigned *value)
    {
        if (!value || count > 32 || !canRead(count)) {
            return false;
        }
        unsigned result = 0;
        for (unsigned index = 0; index < count; ++index) {
            result = (result << 1U) | (((*data)[bit / 8U] >> (7U - (bit % 8U))) & 1U);
            ++bit;
        }
        *value = result;
        return true;
    }

    std::size_t position() const
    {
        return bit;
    }

    std::size_t remaining() const
    {
        return data && bit <= data->size() * 8U ? data->size() * 8U - bit : 0;
    }
};

struct HuffmanTable {
    std::string name;
    std::vector<std::array<int, 2>> nodes;
};

struct Codeword {
    int value = 0;
    std::vector<unsigned> bits;
};

bool readInitializer(const std::string &path,
                     const std::string &name,
                     HuffmanTable *table,
                     std::string *reason)
{
    if (!table || !reason) {
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        *reason = "table-open-failed";
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const std::string marker = name + "[";
    const std::size_t markerOffset = text.find(marker);
    if (markerOffset == std::string::npos) {
        *reason = "table-marker-missing:" + name;
        return false;
    }
    const std::size_t equals = text.find('=', markerOffset);
    const std::size_t end = text.find("};", equals == std::string::npos ? markerOffset : equals);
    if (equals == std::string::npos || end == std::string::npos) {
        *reason = "table-initializer-missing:" + name;
        return false;
    }

    std::vector<int> values;
    std::size_t offset = equals + 1;
    while (offset < end) {
        const char character = text[offset];
        if (character == '+' || character == '-' || (character >= '0' && character <= '9')) {
            char *parsedEnd = nullptr;
            const long value = std::strtol(text.c_str() + offset, &parsedEnd, 10);
            if (parsedEnd != text.c_str() + offset) {
                if (value < static_cast<long>(std::numeric_limits<int>::min())
                    || value > static_cast<long>(std::numeric_limits<int>::max())) {
                    *reason = "table-value-out-of-range:" + name;
                    return false;
                }
                values.push_back(static_cast<int>(value));
                offset = static_cast<std::size_t>(parsedEnd - text.c_str());
                continue;
            }
        }
        ++offset;
    }
    if (values.empty() || values.size() % 2U != 0) {
        *reason = "table-pair-count-invalid:" + name;
        return false;
    }

    table->name = name;
    table->nodes.clear();
    table->nodes.reserve(values.size() / 2U);
    for (std::size_t index = 0; index < values.size(); index += 2U) {
        table->nodes.push_back({values[index], values[index + 1]});
    }
    return true;
}

bool loadTables(const std::string &path,
                std::map<std::string, HuffmanTable> *tables,
                std::string *reason)
{
    if (!tables || !reason) {
        return false;
    }
    tables->clear();
    for (const char *name : kTableNames) {
        HuffmanTable table;
        if (!readInitializer(path, name, &table, reason)) {
            return false;
        }
        (*tables)[table.name] = std::move(table);
    }
    return true;
}

bool collectCodewords(const HuffmanTable &table,
                      std::size_t node,
                      std::vector<unsigned> *path,
                      std::vector<unsigned char> *state,
                      std::vector<Codeword> *codewords,
                      std::string *reason)
{
    if (!path || !state || !codewords || !reason || node >= table.nodes.size()) {
        if (reason) {
            *reason = "huffman-node-out-of-range:" + table.name;
        }
        return false;
    }
    if ((*state)[node] == 1U) {
        *reason = "huffman-cycle:" + table.name;
        return false;
    }
    if ((*state)[node] == 2U) {
        *reason = "huffman-shared-node:" + table.name;
        return false;
    }
    (*state)[node] = 1U;
    for (unsigned branch = 0; branch < 2; ++branch) {
        const int value = table.nodes[node][branch];
        path->push_back(branch);
        if (value == std::numeric_limits<int>::min()) {
            *reason = "huffman-leaf-out-of-range:" + table.name;
            return false;
        }
        if (value < 0) {
            codewords->push_back(Codeword {-value - 1, *path});
        } else if (!collectCodewords(table, static_cast<std::size_t>(value), path, state,
                                     codewords, reason)) {
            return false;
        }
        path->pop_back();
    }
    (*state)[node] = 2U;
    return true;
}

bool decodeHuffman(BitReader *reader,
                   const HuffmanTable &table,
                   int *value,
                   std::string *reason)
{
    if (!reader || !value || !reason || table.nodes.empty()) {
        if (reason) {
            *reason = "huffman-table-empty:" + table.name;
        }
        return false;
    }
    std::size_t node = 0;
    for (std::size_t step = 0; step < table.nodes.size(); ++step) {
        unsigned branch = 0;
        if (!reader->read(1, &branch)) {
            *reason = "huffman-codeword-truncated:" + table.name;
            return false;
        }
        const int next = table.nodes[node][branch];
        if (next < 0) {
            *value = -next - 1;
            return true;
        }
        if (static_cast<std::size_t>(next) >= table.nodes.size()) {
            *reason = "huffman-node-out-of-range:" + table.name;
            return false;
        }
        node = static_cast<std::size_t>(next);
    }
    *reason = "huffman-step-limit:" + table.name;
    return false;
}

std::vector<std::uint8_t> packBits(const std::vector<unsigned> &bits)
{
    std::vector<std::uint8_t> bytes((bits.size() + 7U) / 8U, 0);
    for (std::size_t index = 0; index < bits.size(); ++index) {
        if (bits[index] != 0U) {
            bytes[index / 8U] |= static_cast<std::uint8_t>(1U << (7U - index % 8U));
        }
    }
    return bytes;
}

std::string bitString(const std::vector<std::uint8_t> &payload,
                      std::size_t begin,
                      std::size_t end)
{
    if (begin > end || end > payload.size() * 8U) {
        return {};
    }
    std::string result;
    result.reserve(end - begin);
    for (std::size_t bit = begin; bit < end; ++bit) {
        result.push_back(((payload[bit / 8U] >> (7U - (bit % 8U))) & 1U) != 0U
                             ? '1' : '0');
    }
    return result;
}

std::string hexString(const std::vector<std::uint8_t> &payload)
{
    static constexpr char kHex[] = "0123456789abcdef";
    std::string result;
    result.reserve(payload.size() * 2U);
    for (const std::uint8_t byte : payload) {
        result.push_back(kHex[(byte >> 4U) & 0x0fU]);
        result.push_back(kHex[byte & 0x0fU]);
    }
    return result;
}

bool tableSelfTest(const HuffmanTable &table, std::size_t *leafCount, std::string *reason)
{
    if (!leafCount || !reason || table.nodes.empty()) {
        if (reason) {
            *reason = "huffman-table-empty:" + table.name;
        }
        return false;
    }
    std::vector<unsigned char> state(table.nodes.size(), 0U);
    std::vector<unsigned> path;
    std::vector<Codeword> codewords;
    if (!collectCodewords(table, 0, &path, &state, &codewords, reason)) {
        return false;
    }
    for (const Codeword &codeword : codewords) {
        const std::vector<std::uint8_t> bytes = packBits(codeword.bits);
        BitReader reader(&bytes);
        int decoded = 0;
        if (!decodeHuffman(&reader, table, &decoded, reason)
            || decoded != codeword.value || reader.position() != codeword.bits.size()) {
            *reason = "huffman-roundtrip-failed:" + table.name;
            return false;
        }
    }
    *leafCount += codewords.size();
    return true;
}

FrameReport malformed(const BitReader &reader, std::string reason)
{
    FrameReport result;
    result.disposition = ParseDisposition::Malformed;
    result.reason = std::move(reason);
    result.bitsConsumed = reader.position();
    return result;
}

bool readHuffmanValue(BitReader *reader,
                      const HuffmanTable &table,
                      unsigned quantSteps,
                      std::size_t *valueCount,
                      std::string *reason)
{
    int value = 0;
    if (!decodeHuffman(reader, table, &value, reason)) {
        return false;
    }
    if (value < 0 || static_cast<unsigned>(value) >= quantSteps) {
        *reason = "huffman-value-out-of-range:" + table.name;
        return false;
    }
    if (valueCount) {
        ++(*valueCount);
    }
    return true;
}

bool malformedSelfTests(std::string *reason)
{
    HuffmanTable cycle;
    cycle.name = "synthetic-cycle";
    cycle.nodes = {{{1, -1}, {0, -2}}};
    std::size_t leaves = 0;
    if (tableSelfTest(cycle, &leaves, reason)) {
        *reason = "malformed-cycle-was-accepted";
        return false;
    }

    HuffmanTable outOfRange;
    outOfRange.name = "synthetic-out-of-range";
    outOfRange.nodes = {{{4, -1}}};
    leaves = 0;
    if (tableSelfTest(outOfRange, &leaves, reason)) {
        *reason = "malformed-node-was-accepted";
        return false;
    }

    HuffmanTable truncated;
    truncated.name = "synthetic-truncated";
    truncated.nodes = {{{1, -1}, {-1, -2}}};
    const std::vector<std::uint8_t> noBits;
    BitReader reader(&noBits);
    int value = 0;
    if (decodeHuffman(&reader, truncated, &value, reason)) {
        *reason = "truncated-codeword-was-accepted";
        return false;
    }
    return true;
}

void appendBits(std::vector<unsigned> *bits, unsigned count, unsigned value)
{
    for (unsigned index = 0; index < count; ++index) {
        bits->push_back((value >> (count - index - 1U)) & 1U);
    }
}

std::vector<unsigned> literalBits(const char *text)
{
    std::vector<unsigned> bits;
    if (!text) {
        return bits;
    }
    for (const char *cursor = text; *cursor; ++cursor) {
        if (*cursor != '0' && *cursor != '1') {
            return {};
        }
        bits.push_back(static_cast<unsigned>(*cursor - '0'));
    }
    return bits;
}

bool annexACanarySelfTests(const std::map<std::string, HuffmanTable> &tables,
                           std::size_t *canaryCount,
                           std::string *reason)
{
    struct Canary {
        const char *table;
        const char *bits;
        int value;
    };
    // Fixed Annex A.1 codeword/symbol pairs. These are deliberately not
    // discovered by walking the loaded tree, so a self-consistent but wrong
    // tree cannot satisfy this check.
    constexpr std::array<Canary, 12> canaries = {{
        {"joc_huff_code_coarse_generic", "0", 0},
        {"joc_huff_code_coarse_generic", "11", 1},
        {"joc_huff_code_fine_generic", "0", 0},
        {"joc_huff_code_fine_generic", "100", 1},
        {"joc_huff_code_coarse_coeff_sparse", "0", 0},
        {"joc_huff_code_coarse_coeff_sparse", "100", 1},
        {"joc_huff_code_fine_coeff_sparse", "1", 0},
        {"joc_huff_code_fine_coeff_sparse", "001", 1},
        {"joc_huff_code_5ch_pos_index_sparse", "0", 0},
        {"joc_huff_code_5ch_pos_index_sparse", "100", 3},
        {"joc_huff_code_7ch_pos_index_sparse", "0", 0},
        {"joc_huff_code_7ch_pos_index_sparse", "1000", 1},
    }};
    for (const Canary &canary : canaries) {
        const auto table = tables.find(canary.table);
        const std::vector<unsigned> bits = literalBits(canary.bits);
        if (table == tables.end() || bits.empty()) {
            *reason = "annex-a-fixed-canary-definition-invalid";
            return false;
        }
        const std::vector<std::uint8_t> bytes = packBits(bits);
        BitReader reader(&bytes);
        int decoded = 0;
        std::string decodeReason;
        if (!decodeHuffman(&reader, table->second, &decoded, &decodeReason)
            || decoded != canary.value || reader.position() != bits.size()) {
            *reason = "annex-a-fixed-canary-failed:" + std::string(canary.table)
                + ":bits=" + canary.bits + ":expected="
                + std::to_string(canary.value) + ":actual="
                + std::to_string(decoded);
            if (!decodeReason.empty()) {
                *reason += ":" + decodeReason;
            }
            return false;
        }
        if (canaryCount) {
            ++(*canaryCount);
        }
    }
    return true;
}

bool syntaxMalformedSelfTests(const std::string &tablePath, std::string *reason)
{
    std::vector<unsigned> excessiveObjects;
    appendBits(&excessiveObjects, 3, 0);
    appendBits(&excessiveObjects, 6, 63);
    appendBits(&excessiveObjects, 3, 0);
    const FrameReport excessiveReport = parsePayload(packBits(excessiveObjects), tablePath);
    if (excessiveReport.disposition != ParseDisposition::Malformed) {
        *reason = "excessive-object-count-was-accepted";
        return false;
    }

    constexpr std::array<unsigned, 5> legalConfigChannels = {5, 7, 7, 5, 7};
    for (unsigned config = 0; config < legalConfigChannels.size(); ++config) {
        std::vector<unsigned> validEmptyConfig;
        appendBits(&validEmptyConfig, 3, config);
        appendBits(&validEmptyConfig, 6, 0);
        appendBits(&validEmptyConfig, 3, 0);
        appendBits(&validEmptyConfig, 3, 0);
        appendBits(&validEmptyConfig, 5, 0);
        appendBits(&validEmptyConfig, 10, 0);
        appendBits(&validEmptyConfig, 1, 0);
        const FrameReport validReport = parsePayload(packBits(validEmptyConfig), tablePath);
        if (validReport.disposition != ParseDisposition::Pass
            || validReport.numChannels != legalConfigChannels[config]
            || validReport.numObjects != 1
            || validReport.bitsConsumed != 31
            || validReport.paddingBits != 1) {
            *reason = "legal-config-zero-object-presence-was-rejected";
            return false;
        }
    }

    for (unsigned config = 5; config < 8; ++config) {
        std::vector<unsigned> reservedConfig;
        appendBits(&reservedConfig, 3, config);
        appendBits(&reservedConfig, 6, 0);
        appendBits(&reservedConfig, 3, 0);
        const FrameReport reservedReport = parsePayload(packBits(reservedConfig), tablePath);
        if (reservedReport.disposition != ParseDisposition::Unsupported
            || reservedReport.reason != "reserved-downmix-configuration") {
            *reason = "reserved-configuration-was-not-rejected";
            return false;
        }
    }

    std::vector<unsigned> excessivePadding;
    appendBits(&excessivePadding, 3, 0);
    appendBits(&excessivePadding, 6, 0);
    appendBits(&excessivePadding, 3, 0);
    appendBits(&excessivePadding, 3, 0);
    appendBits(&excessivePadding, 5, 0);
    appendBits(&excessivePadding, 10, 0);
    appendBits(&excessivePadding, 1, 0);
    appendBits(&excessivePadding, 8, 0);
    const FrameReport paddingReport = parsePayload(packBits(excessivePadding), tablePath);
    if (paddingReport.disposition != ParseDisposition::Malformed) {
        *reason = "excessive-padding-was-accepted";
        return false;
    }

    // Forensic syntax vector: config0, one present sparse object, three
    // parameter bands, fixed first channel 0, two channel-delta symbols, and
    // three coefficient symbols. The expected cursor ranges are independent
    // of the production trace writer.
    std::vector<unsigned> forensicBits;
    appendBits(&forensicBits, 3, 0); // config
    appendBits(&forensicBits, 6, 0); // one object
    appendBits(&forensicBits, 3, 0); // extension
    appendBits(&forensicBits, 3, 0); // clipgain X
    appendBits(&forensicBits, 5, 0); // clipgain Y
    appendBits(&forensicBits, 10, 0); // sequence
    appendBits(&forensicBits, 1, 1); // present
    appendBits(&forensicBits, 3, 1); // three bands
    appendBits(&forensicBits, 1, 1); // sparse
    appendBits(&forensicBits, 1, 0); // coarse quantizer
    appendBits(&forensicBits, 1, 0); // smooth
    appendBits(&forensicBits, 1, 0); // one data point
    appendBits(&forensicBits, 3, 0); // fixed first channel
    appendBits(&forensicBits, 1, 0); // position delta symbol 0
    appendBits(&forensicBits, 1, 0); // position delta symbol 0
    appendBits(&forensicBits, 1, 0); // coefficient symbol 0
    appendBits(&forensicBits, 1, 0);
    appendBits(&forensicBits, 1, 0);
    const std::vector<std::uint8_t> forensicPayload = packBits(forensicBits);
    const FrameReport forensicReport = parsePayload(forensicPayload, tablePath, true);
    if (forensicReport.disposition != ParseDisposition::Pass
        || !forensicReport.forensic
        || forensicReport.forensic->objects.size() != 1U
        || forensicReport.forensic->objects[0].headerBitOffset != 30U
        || forensicReport.forensic->objects[0].headerBitEnd != 38U
        || !forensicReport.forensic->objects[0].hasDataRange
        || forensicReport.forensic->objects[0].dataBitOffset != 38U
        || forensicReport.forensic->objects[0].dataBitEnd != 46U
        || forensicReport.forensic->objects[0].dataPoints.size() != 1U
        || forensicReport.forensic->objects[0].dataPoints[0].hasOffsetTsRange
        || !forensicReport.forensic->objects[0].dataPoints[0].hasSymbolDataRange
        || forensicReport.forensic->objects[0].dataPoints[0].symbolDataBitOffset != 38U
        || forensicReport.forensic->objects[0].dataPoints[0].symbolDataBitEnd != 46U
        || forensicReport.forensic->objects[0].dataPoints[0].symbols.size() != 6U) {
        *reason = "forensic-cursor-range-vector-failed:disposition="
            + std::to_string(static_cast<int>(forensicReport.disposition))
            + ":hasTrace=" + (forensicReport.forensic ? "1" : "0");
        if (forensicReport.forensic && !forensicReport.forensic->objects.empty()) {
            const auto &objectTrace = forensicReport.forensic->objects[0];
            *reason += ":header=" + std::to_string(objectTrace.headerBitOffset)
                + "-" + std::to_string(objectTrace.headerBitEnd)
                + ":data=" + std::to_string(objectTrace.dataBitOffset)
                + "-" + std::to_string(objectTrace.dataBitEnd)
                + ":dp=" + std::to_string(objectTrace.dataPoints.size());
            if (!objectTrace.dataPoints.empty()) {
                *reason += ":symbolDataRange="
                    + std::to_string(objectTrace.dataPoints[0].symbolDataBitOffset)
                    + "-" + std::to_string(objectTrace.dataPoints[0].symbolDataBitEnd)
                    + ":symbols="
                    + std::to_string(objectTrace.dataPoints[0].symbols.size());
            }
        }
        return false;
    }
    const auto &forensicSymbols = forensicReport.forensic->objects[0].dataPoints[0].symbols;
    if (forensicSymbols[0].kind != JocSymbolKind::SparseFixedChannel
        || forensicSymbols[0].bitOffset != 38U
        || forensicSymbols[0].bitLength != 3U
        || forensicSymbols[0].codeword != "000"
        || forensicSymbols[0].symbol != 0
        || forensicSymbols[0].parameterBand != 0
        || forensicSymbols[0].resolvedInputChannel != 0
        || forensicSymbols[1].kind != JocSymbolKind::SparseHuffmanChannelDelta
        || forensicSymbols[1].bitOffset != 41U
        || forensicSymbols[1].bitLength != 1U
        || forensicSymbols[1].codeword != "0"
        || forensicSymbols[1].parameterBand != 1
        || forensicSymbols[1].resolvedInputChannel != 0
        || forensicSymbols[2].kind != JocSymbolKind::SparseHuffmanChannelDelta
        || forensicSymbols[2].bitOffset != 42U
        || forensicSymbols[2].bitLength != 1U
        || forensicSymbols[2].codeword != "0"
        || forensicSymbols[2].parameterBand != 2
        || forensicSymbols[2].resolvedInputChannel != 0
        || forensicSymbols[3].kind != JocSymbolKind::HuffmanCoefficient
        || forensicSymbols[3].bitOffset != 43U
        || forensicSymbols[3].parameterBand != 0
        || forensicSymbols[3].resolvedInputChannel != 0
        || forensicSymbols[3].symbol != forensicReport.objects[0].dataPoints[0].values[0]
        || forensicSymbols[4].parameterBand != 1
        || forensicSymbols[4].resolvedInputChannel != 0
        || forensicSymbols[4].symbol != forensicReport.objects[0].dataPoints[0].values[1]
        || forensicSymbols[5].bitOffset != 45U
        || forensicSymbols[5].bitLength != 1U
        || forensicSymbols[5].parameterBand != 2
        || forensicSymbols[5].resolvedInputChannel != 0
        || forensicSymbols[5].symbol != forensicReport.objects[0].dataPoints[0].values[2]) {
        *reason = "forensic-symbol-codeword-vector-failed";
        return false;
    }
    // Dense companion vector: five input channels and one parameter band make
    // the channel/band coordinates independently observable for every raw
    // coefficient symbol.
    std::vector<unsigned> denseForensicBits;
    appendBits(&denseForensicBits, 3, 0); // config: five input channels
    appendBits(&denseForensicBits, 6, 0); // one object
    appendBits(&denseForensicBits, 3, 0); // extension
    appendBits(&denseForensicBits, 3, 0); // clipgain X
    appendBits(&denseForensicBits, 5, 0); // clipgain Y
    appendBits(&denseForensicBits, 10, 0); // sequence
    appendBits(&denseForensicBits, 1, 1); // present
    appendBits(&denseForensicBits, 3, 0); // one band
    appendBits(&denseForensicBits, 1, 0); // dense
    appendBits(&denseForensicBits, 1, 0); // coarse quantizer
    appendBits(&denseForensicBits, 1, 0); // smooth
    appendBits(&denseForensicBits, 1, 0); // one data point
    for (unsigned channel = 0; channel < 5; ++channel) {
        appendBits(&denseForensicBits, 1, 0); // coefficient symbol 0
    }
    const FrameReport denseForensicReport = parsePayload(
        packBits(denseForensicBits), tablePath, true);
    if (denseForensicReport.disposition != ParseDisposition::Pass
        || !denseForensicReport.forensic
        || denseForensicReport.forensic->objects.size() != 1U
        || denseForensicReport.forensic->objects[0].dataPoints.size() != 1U
        || denseForensicReport.forensic->objects[0].dataPoints[0].symbols.size() != 5U) {
        *reason = "forensic-dense-coordinate-vector-failed";
        return false;
    }
    const auto &denseSymbols = denseForensicReport.forensic->objects[0]
        .dataPoints[0].symbols;
    for (unsigned channel = 0; channel < 5; ++channel) {
        if (denseSymbols[channel].kind != JocSymbolKind::HuffmanCoefficient
            || denseSymbols[channel].parameterBand != 0
            || denseSymbols[channel].inputChannel != static_cast<int>(channel)
            || denseSymbols[channel].resolvedInputChannel != -1
            || denseSymbols[channel].symbol
                != denseForensicReport.objects[0].dataPoints[0].values[channel]) {
            *reason = "forensic-dense-symbol-coordinate-map-failed";
            return false;
        }
    }
    std::vector<std::uint8_t> truncatedForensic = forensicPayload;
    truncatedForensic.pop_back();
    const FrameReport truncatedForensicReport =
        parsePayload(truncatedForensic, tablePath, true);
    if (truncatedForensicReport.disposition != ParseDisposition::Malformed) {
        *reason = "forensic-truncated-payload-was-accepted";
        return false;
    }
    return true;
}

} // namespace

const char *dispositionText(ParseDisposition disposition)
{
    switch (disposition) {
    case ParseDisposition::Pass: return "PASS";
    case ParseDisposition::Unsupported: return "UNSUPPORTED";
    case ParseDisposition::Malformed: return "MALFORMED";
    }
    return "MALFORMED";
}

const char *symbolKindText(JocSymbolKind kind)
{
    switch (kind) {
    case JocSymbolKind::SparseFixedChannel: return "sparse-fixed-channel";
    case JocSymbolKind::SparseHuffmanChannelDelta: return "sparse-huffman-channel-delta";
    case JocSymbolKind::HuffmanCoefficient: return "huffman-coefficient";
    }
    return "huffman-coefficient";
}

HuffmanSelfTestReport runHuffmanSelfTest(const std::string &tablePath)
{
    HuffmanSelfTestReport result;
    std::map<std::string, HuffmanTable> tables;
    if (!loadTables(tablePath, &tables, &result.reason)) {
        return result;
    }
    result.tableCount = tables.size();
    for (const auto &[name, table] : tables) {
        if (!tableSelfTest(table, &result.leafCount, &result.reason)) {
            return result;
        }
    }
    if (!malformedSelfTests(&result.reason)) {
        return result;
    }
    if (!syntaxMalformedSelfTests(tablePath, &result.reason)) {
        return result;
    }
    std::size_t canaryCount = 0;
    if (!annexACanarySelfTests(tables, &canaryCount, &result.reason)) {
        return result;
    }
    result.pass = true;
    result.reason = "all-reachable-leaves-roundtrip-and-malformed-rejection;"
                    "config-policy-0-4-pass-5-7-reserved;annex-a-fixed-canaries="
                    + std::to_string(canaryCount);
    return result;
}

FrameReport parsePayload(const std::vector<std::uint8_t> &payload,
                         const std::string &tablePath,
                         bool captureForensics)
{
    BitReader reader(&payload);
    FrameReport result;
    if (captureForensics) {
        result.forensic = std::make_shared<JocForensicTrace>();
        result.forensic->payloadBitCount = payload.size() * 8U;
        result.forensic->payloadHex = hexString(payload);
    }
    std::map<std::string, HuffmanTable> tables;
    std::string reason;
    if (!loadTables(tablePath, &tables, &reason)) {
        return malformed(reader, reason);
    }

    unsigned value = 0;
    if (!reader.read(3, &result.downmixConfigIndex)
        || !reader.read(6, &value)) {
        return malformed(reader, "joc-header-truncated");
    }
    result.numObjects = value + 1U;
    if (result.numObjects == 0 || result.numObjects > 16) {
        return malformed(reader, "joc-object-count-out-of-range");
    }
    if (!reader.read(3, &result.extConfigIndex)) {
        return malformed(reader, "joc-header-truncated");
    }

    switch (result.downmixConfigIndex) {
    case 0: result.numChannels = 5; break;
    case 1: result.numChannels = 7; break;
    case 2: result.numChannels = 7; break;
    case 3: result.numChannels = 5; break;
    case 4: result.numChannels = 7; break;
    default:
        result.disposition = ParseDisposition::Unsupported;
        result.reason = "reserved-downmix-configuration";
        result.bitsConsumed = reader.position();
        return result;
    }
    if (result.extConfigIndex != 0) {
        result.disposition = ParseDisposition::Unsupported;
        result.reason = "reserved-extensional-configuration";
        result.bitsConsumed = reader.position();
        return result;
    }

    unsigned clipgainX = 0;
    unsigned clipgainY = 0;
    if (!reader.read(3, &clipgainX)
        || !reader.read(5, &clipgainY)
        || !reader.read(10, &result.sequenceCount)) {
        return malformed(reader, "joc-info-truncated");
    }

    result.objects.resize(result.numObjects);
    if (result.forensic) {
        result.forensic->objects.resize(result.numObjects);
    }
    for (unsigned objectIndex = 0; objectIndex < result.numObjects; ++objectIndex) {
        JocObjectSummary &object = result.objects[objectIndex];
        JocObjectTrace *objectTrace = result.forensic
            ? &result.forensic->objects[objectIndex] : nullptr;
        if (objectTrace) {
            objectTrace->headerBitOffset = reader.position();
        }
        unsigned present = 0;
        if (!reader.read(1, &present)) {
            return malformed(reader, "joc-object-presence-truncated");
        }
        object.present = present != 0;
        if (!object.present) {
            if (objectTrace) {
                objectTrace->headerBitEnd = reader.position();
            }
            continue;
        }
        unsigned bandsIndex = 0;
        if (!reader.read(3, &bandsIndex)
            || bandsIndex >= kNumBandsByIndex.size()
            || !reader.read(1, &present)
            || !reader.read(1, &object.quantIndex)
            || !reader.read(1, &object.slopeIndex)
            || !reader.read(1, &value)) {
            return malformed(reader, "joc-object-info-truncated");
        }
        object.numBands = kNumBandsByIndex[bandsIndex];
        object.sparse = present != 0;
        object.quantSteps = object.quantIndex == 0 ? 96 : 192;
        object.numDataPoints = value + 1U;
        if (object.numDataPoints > 2) {
            return malformed(reader, "joc-data-point-count-out-of-range");
        }
        if (object.slopeIndex == 1) {
            object.dataPoints.resize(object.numDataPoints);
            if (objectTrace) {
                objectTrace->dataPoints.resize(object.numDataPoints);
            }
            for (unsigned dp = 0; dp < object.numDataPoints; ++dp) {
                if (objectTrace) {
                    objectTrace->dataPoints[dp].hasOffsetTsRange = true;
                    objectTrace->dataPoints[dp].offsetTsBitOffset = reader.position();
                }
                if (!reader.read(5, &object.dataPoints[dp].offsetTs)) {
                    return malformed(reader, "joc-offset-truncated");
                }
                ++object.dataPoints[dp].offsetTs;
                if (objectTrace) {
                    objectTrace->dataPoints[dp].offsetTsBitEnd = reader.position();
                }
            }
        }
        if (object.slopeIndex == 0) {
            object.dataPoints.resize(object.numDataPoints);
            if (objectTrace) {
                objectTrace->dataPoints.resize(object.numDataPoints);
            }
        }
        if (objectTrace) {
            objectTrace->headerBitEnd = reader.position();
        }
    }

    for (unsigned objectIndex = 0; objectIndex < result.numObjects; ++objectIndex) {
        JocObjectSummary &object = result.objects[objectIndex];
        JocObjectTrace *objectTrace = result.forensic
            ? &result.forensic->objects[objectIndex] : nullptr;
        if (!object.present) {
            continue;
        }
        const char *positionName = result.numChannels == 5
            ? "joc_huff_code_5ch_pos_index_sparse"
            : "joc_huff_code_7ch_pos_index_sparse";
        const char *coefficientName = object.sparse
            ? (object.quantIndex == 0
                   ? "joc_huff_code_coarse_coeff_sparse"
                   : "joc_huff_code_fine_coeff_sparse")
            : (object.quantIndex == 0
                   ? "joc_huff_code_coarse_generic"
                   : "joc_huff_code_fine_generic");
        const HuffmanTable &positionTable = tables.at(positionName);
        const HuffmanTable &coefficientTable = tables.at(coefficientName);

        for (unsigned dp = 0; dp < object.numDataPoints; ++dp) {
            JocDataPointTrace *dataPointTrace = objectTrace
                ? &objectTrace->dataPoints[dp] : nullptr;
            if (dataPointTrace) {
                dataPointTrace->hasSymbolDataRange = true;
                dataPointTrace->symbolDataBitOffset = reader.position();
            }
            if (objectTrace && !objectTrace->hasDataRange) {
                objectTrace->hasDataRange = true;
                objectTrace->dataBitOffset = reader.position();
            }
            if (object.sparse) {
                JocDataPoint &dataPoint = object.dataPoints[dp];
                dataPoint.channelIndices.resize(object.numBands);
                dataPoint.values.resize(object.numBands);
                unsigned channelIndex = 0;
                std::vector<unsigned> resolvedChannels(object.numBands, 0U);
                const std::size_t channelBitOffset = reader.position();
                if (!reader.read(3, &channelIndex) || channelIndex >= result.numChannels) {
                    return malformed(reader, "joc-channel-index-out-of-range");
                }
                dataPoint.channelIndices[0] = channelIndex;
                resolvedChannels[0] = channelIndex;
                ++object.huffmanValueCount;
                if (dataPointTrace) {
                    dataPointTrace->symbols.push_back(JocHuffmanSymbolTrace {
                        JocSymbolKind::SparseFixedChannel, "fixed-3bit", channelBitOffset, 3,
                        bitString(payload, channelBitOffset, reader.position()),
                        static_cast<int>(channelIndex), 0, -1,
                        static_cast<int>(resolvedChannels[0])});
                }
                for (unsigned pb = 1; pb < object.numBands; ++pb) {
                    int decoded = 0;
                    const std::size_t bitOffset = reader.position();
                    if (!decodeHuffman(&reader, positionTable, &decoded, &reason)
                        || decoded < 0 || static_cast<unsigned>(decoded) >= result.numChannels) {
                        if (reason.empty()) {
                            reason = "joc-position-index-out-of-range";
                        }
                        return malformed(reader, reason);
                    }
                    dataPoint.channelIndices[pb] = static_cast<unsigned>(decoded);
                    resolvedChannels[pb] = (dataPoint.channelIndices[pb - 1U]
                                            + dataPoint.channelIndices[pb]) % result.numChannels;
                    ++object.huffmanValueCount;
                    if (dataPointTrace) {
                        dataPointTrace->symbols.push_back(JocHuffmanSymbolTrace {
                            JocSymbolKind::SparseHuffmanChannelDelta, positionTable.name,
                            bitOffset, reader.position() - bitOffset,
                            bitString(payload, bitOffset, reader.position()), decoded,
                            static_cast<int>(pb), -1,
                            static_cast<int>(resolvedChannels[pb])});
                    }
                }
                for (unsigned pb = 0; pb < object.numBands; ++pb) {
                    int decoded = 0;
                    const std::size_t bitOffset = reader.position();
                    if (!decodeHuffman(&reader, coefficientTable, &decoded, &reason)
                        || decoded < 0 || static_cast<unsigned>(decoded) >= object.quantSteps) {
                        if (reason.empty()) {
                            reason = "joc-coefficient-out-of-range";
                        }
                        return malformed(reader, reason);
                    }
                    dataPoint.values[pb] = static_cast<unsigned>(decoded);
                    ++object.huffmanValueCount;
                    if (dataPointTrace) {
                        dataPointTrace->symbols.push_back(JocHuffmanSymbolTrace {
                            JocSymbolKind::HuffmanCoefficient, coefficientTable.name,
                            bitOffset, reader.position() - bitOffset,
                            bitString(payload, bitOffset, reader.position()), decoded,
                            static_cast<int>(pb), -1,
                            static_cast<int>(resolvedChannels[pb])});
                    }
                }
            } else {
                JocDataPoint &dataPoint = object.dataPoints[dp];
                dataPoint.values.resize(result.numChannels * object.numBands);
                for (unsigned ch = 0; ch < result.numChannels; ++ch) {
                    for (unsigned pb = 0; pb < object.numBands; ++pb) {
                        int decoded = 0;
                        const std::size_t bitOffset = reader.position();
                        if (!decodeHuffman(&reader, coefficientTable, &decoded, &reason)
                            || decoded < 0 || static_cast<unsigned>(decoded) >= object.quantSteps) {
                            if (reason.empty()) {
                                reason = "joc-matrix-coefficient-out-of-range";
                            }
                            return malformed(reader, reason);
                        }
                        dataPoint.values[ch * object.numBands + pb] = static_cast<unsigned>(decoded);
                        ++object.huffmanValueCount;
                        if (dataPointTrace) {
                            dataPointTrace->symbols.push_back(JocHuffmanSymbolTrace {
                                JocSymbolKind::HuffmanCoefficient, coefficientTable.name,
                                bitOffset, reader.position() - bitOffset,
                                bitString(payload, bitOffset, reader.position()), decoded,
                                static_cast<int>(pb), static_cast<int>(ch), -1});
                        }
                    }
                }
            }
            if (dataPointTrace) {
                dataPointTrace->symbolDataBitEnd = reader.position();
            }
        }
        if (objectTrace) {
            objectTrace->dataBitEnd = reader.position();
        }
    }

    const std::size_t syntaxEnd = reader.position();
    result.paddingBits = reader.remaining();
    if (result.paddingBits > 7) {
        return malformed(reader, "joc-trailing-bits-exceed-padding");
    }
    while (reader.remaining() > 0) {
        unsigned bit = 0;
        if (!reader.read(1, &bit)) {
            return malformed(reader, "joc-padding-truncated");
        }
        result.paddingPattern = (result.paddingPattern << 1U) | bit;
    }
    result.bitsConsumed = syntaxEnd;
    if (result.forensic) {
        result.forensic->syntaxBitEnd = syntaxEnd;
    }
    // Configurations 0 through 4 are defined channel layouts.  The parser
    // reports them as Pass once bounded syntax and padding checks succeed;
    // any transform/renderer policy remains outside Gate 5A.
    result.disposition = ParseDisposition::Pass;
    result.reason = "ok";
    return result;
}

} // namespace eac3joc
