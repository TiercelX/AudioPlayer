// Gate 6B1 bounded OAMD framing/inventory. Element bodies remain opaque raw
// bits; object property and update/reuse semantics belong to Gate 6B2.

#include "oamd-b1.h"

#include <algorithm>
#include <limits>
#include <sstream>

namespace eac3oamd {
namespace {

constexpr std::size_t kMaxPayloadBytes = 1U << 20;
// Syntax maxima, not downstream renderer limits.  object_count_bits uses a
// five-bit escape (31) followed by a seven-bit extension, then code+1:
// 31 + 127 + 1 == 159.  oa_element_count uses 15 + 31 == 46.
constexpr unsigned kMaxElements = 46;
constexpr unsigned kMaxObjects = 159;
// variable_bits_max(4, 4) reaches this value when all four four-bit groups
// are 0xf and the first three groups continue.  The payload boundary below
// remains the authoritative allocation/availability limit.
constexpr unsigned variableBitsMaximum(unsigned groupBits, unsigned groups)
{
    std::uint64_t value = 0;
    const std::uint64_t partMaximum = (std::uint64_t {1} << groupBits) - 1U;
    for (unsigned group = 0; group < groups; ++group) {
        value += partMaximum;
        if (group + 1U < groups) {
            value = (value << groupBits) + (std::uint64_t {1} << groupBits);
        }
    }
    return static_cast<unsigned>(value);
}

constexpr unsigned kMaxElementSizeCode = variableBitsMaximum(4, 4);

struct Reader {
    const std::uint8_t *data = nullptr;
    std::size_t sizeBytes = 0;
    std::size_t bit = 0;
    std::size_t limitBits = 0;

    bool canRead(unsigned count) const
    {
        return bit <= limitBits && count <= limitBits - bit;
    }

    bool read(unsigned count, unsigned *value)
    {
        if (!value || count > 32 || !canRead(count)) {
            return false;
        }
        unsigned result = 0;
        for (unsigned index = 0; index < count; ++index) {
            result = (result << 1U)
                | ((data[bit / 8U] >> (7U - (bit % 8U))) & 1U);
            ++bit;
        }
        *value = result;
        return true;
    }

    bool skip(std::size_t count)
    {
        if (count > std::numeric_limits<unsigned>::max()) {
            return false;
        }
        if (!canRead(static_cast<unsigned>(count))) {
            return false;
        }
        bit += count;
        return true;
    }

    std::size_t remaining() const
    {
        return bit <= limitBits ? limitBits - bit : 0;
    }
};

bool readVariableBits(Reader *reader,
                      unsigned groupBits,
                      unsigned maxGroups,
                      unsigned *value,
                      bool *extended)
{
    if (!reader || !value || groupBits == 0 || groupBits > 16 || maxGroups == 0) {
        return false;
    }
    std::uint64_t result = 0;
    if (extended) {
        *extended = false;
    }
    for (unsigned group = 0; group < maxGroups; ++group) {
        unsigned part = 0;
        unsigned more = 0;
        if (!reader->read(groupBits, &part) || !reader->read(1, &more)) {
            return false;
        }
        result += part;
        if (!more) {
            if (result > std::numeric_limits<unsigned>::max()) {
                return false;
            }
            *value = static_cast<unsigned>(result);
            return true;
        }
        if (group + 1U >= maxGroups) {
            return false;
        }
        if (result > (std::numeric_limits<std::uint64_t>::max() >> groupBits)) {
            return false;
        }
        result = (result << groupBits) + (std::uint64_t {1} << groupBits);
        if (extended) {
            *extended = true;
        }
    }
    return false;
}

bool copyBits(Reader *reader, std::vector<std::uint8_t> *bytes, std::size_t *bitCount)
{
    if (!reader || !bytes || !bitCount) {
        return false;
    }
    *bitCount = reader->remaining();
    bytes->assign((*bitCount + 7U) / 8U, 0);
    for (std::size_t index = 0; index < *bitCount; ++index) {
        unsigned value = 0;
        if (!reader->read(1, &value)) {
            return false;
        }
        if (value != 0) {
            (*bytes)[index / 8U]
                |= static_cast<std::uint8_t>(1U << (7U - (index % 8U)));
        }
    }
    return true;
}

bool isRecognized(unsigned id)
{
    return id == 1 || id == 2 || id == 5;
}

std::string programType(const ProgramAssignment &program)
{
    if (program.dynamicOnly) {
        return "dynamic-only";
    }
    std::string result;
    if (program.contentDescription[3]) {
        result += "bed";
    }
    if (program.contentDescription[2]) {
        if (!result.empty()) result += '+';
        result += "isf";
    }
    if (program.contentDescription[1]) {
        if (!result.empty()) result += '+';
        result += "dynamic";
    }
    return result.empty() ? "empty-or-reserved" : result;
}

bool parseProgramAssignment(Reader *reader,
                            ProgramAssignment *program,
                            std::string *reason,
                            bool *unsupported,
                            std::string *unsupportedReason)
{
    unsigned value = 0;
    if (!reader || !program || !reason || !reader->read(1, &value)) {
        if (reason) *reason = "truncated-program-dynamic-only-flag";
        return false;
    }
    program->dynamicOnly = value != 0;
    if (program->dynamicOnly) {
        if (!reader->read(1, &value)) {
            *reason = "truncated-program-lfe-flag";
            return false;
        }
        program->lfePresent = value != 0;
        program->programType = programType(*program);
        return true;
    }

    // The syntax lists content_description[3] first, followed by [2], [1],
    // and [0]. Keep the array indices aligned with the specification labels.
    for (int index = 3; index >= 0; --index) {
        if (!reader->read(1, &value)) {
            *reason = "truncated-content-description";
            return false;
        }
        program->contentDescription[index] = value != 0;
    }
    if (program->contentDescription[3]) {
        if (!reader->read(1, &value)) {
            *reason = "truncated-bed-distribute-flag";
            return false;
        }
        program->bedChannelDistribute = value != 0;
        if (!reader->read(1, &value)) {
            *reason = "truncated-multiple-bed-flag";
            return false;
        }
        program->multipleBedInstances = value != 0;
        program->bedInstances = 1;
        if (program->multipleBedInstances) {
            if (!reader->read(3, &value)) {
                *reason = "truncated-bed-count";
                return false;
            }
            program->bedInstances = value + 2U;
        }
        if (program->bedInstances > kMaxObjects) {
            *reason = "bed-count-out-of-range";
            return false;
        }
        for (unsigned bed = 0; bed < program->bedInstances; ++bed) {
            if (!reader->read(1, &value)) {
                *reason = "truncated-bed-lfe-only";
                return false;
            }
            program->bedLfeOnly.push_back(value != 0);
            if (value != 0) {
                continue;
            }
            if (!reader->read(1, &value)) {
                *reason = "truncated-bed-standard-flag";
                return false;
            }
            program->bedStandardChannelAssignment.push_back(value != 0);
            if (value != 0) {
                if (!reader->read(10, &value)) {
                    *reason = "truncated-standard-bed-assignment";
                    return false;
                }
                program->bedChannelAssignments.push_back(value);
            } else {
                if (!reader->read(17, &value)) {
                    *reason = "truncated-nonstandard-bed-assignment";
                    return false;
                }
                program->nonstandardBedAssignments.push_back(value);
            }
        }
    }
    if (program->contentDescription[2] && !reader->read(3, &program->intermediateSpatialFormat)) {
        *reason = "truncated-isf-index";
        return false;
    }
    if (program->contentDescription[2] && program->intermediateSpatialFormat >= 6U) {
        if (unsupported) {
            *unsupported = true;
        }
        if (unsupportedReason && unsupportedReason->empty()) {
            *unsupportedReason = "reserved-isf-index";
        }
    }
    if (program->contentDescription[1]) {
        if (!reader->read(5, &value)) {
            *reason = "truncated-dynamic-object-count";
            return false;
        }
        unsigned extension = 0;
        if (value == 0x1FU && !reader->read(7, &extension)) {
            *reason = "truncated-dynamic-object-count-extension";
            return false;
        }
        program->dynamicObjects = value + extension + 1U;
        if (program->dynamicObjects > kMaxObjects) {
            *reason = "dynamic-object-count-out-of-range";
            return false;
        }
    }
    if (program->contentDescription[0]) {
        if (!reader->read(4, &value)) {
            *reason = "truncated-reserved-data-size";
            return false;
        }
        const std::size_t reservedBytes = static_cast<std::size_t>(value) + 1U;
        if (reservedBytes > reader->remaining() / 8U || !reader->skip(reservedBytes * 8U)) {
            *reason = "reserved-data-size-overrun";
            return false;
        }
    }
    program->programType = programType(*program);
    return true;
}

} // namespace

const char *dispositionText(B1Disposition disposition)
{
    switch (disposition) {
    case B1Disposition::Pass: return "PASS";
    case B1Disposition::Unsupported: return "UNSUPPORTED";
    case B1Disposition::Malformed: return "MALFORMED";
    }
    return "MALFORMED";
}

B1Frame parseB1(const std::vector<std::uint8_t> &payload)
{
    B1Frame result;
    if (payload.empty() || payload.size() > kMaxPayloadBytes) {
        result.reason = "payload-size-out-of-range";
        return result;
    }
    Reader reader {payload.data(), payload.size(), 0, payload.size() * 8U};
    unsigned value = 0;
    if (!reader.read(2, &value)) {
        result.reason = "truncated-version";
        return result;
    }
    result.version = value;
    if (value == 3) {
        result.versionExtended = true;
        if (!reader.read(3, &value)) {
            result.reason = "truncated-version-extension";
            return result;
        }
        result.version += value;
    }
    if (!reader.read(5, &value)) {
        result.reason = "truncated-object-count";
        return result;
    }
    if (value == 0x1FU) {
        result.objectCountExtended = true;
        if (!reader.read(7, &value)) {
            result.reason = "truncated-object-count-extension";
            return result;
        }
        // The escape code 0x1f is part of object_count_bits before the
        // seven-bit extension is added; object_count is code + 1.
        result.objectCount = value + 32U;
    } else {
        result.objectCount = value + 1U;
    }
    if (result.objectCount == 0 || result.objectCount > kMaxObjects) {
        result.reason = "object-count-out-of-range";
        return result;
    }
    bool unsupported = result.version != 0 || result.versionExtended;
    std::string unsupportedReason = unsupported ? "unsupported-version" : std::string {};
    if (!parseProgramAssignment(&reader, &result.program, &result.reason,
                                &unsupported, &unsupportedReason)) {
        return result;
    }
    if (!reader.read(1, &value)) {
        result.reason = "truncated-alternate-object-data-flag";
        return result;
    }
    result.alternateObjectDataPresent = value != 0;
    if (!reader.read(4, &value)) {
        result.reason = "truncated-element-count";
        return result;
    }
    if (value == 0xFU) {
        unsigned extension = 0;
        if (!reader.read(5, &extension)) {
            result.reason = "truncated-element-count-extension";
            return result;
        }
        result.elementCount = value + extension;
    } else {
        result.elementCount = value;
    }
    if (result.elementCount > kMaxElements) {
        result.reason = "element-count-out-of-range";
        return result;
    }

    for (unsigned index = 0; index < result.elementCount; ++index) {
        ElementInventory element;
        if (!reader.read(4, &element.id)) {
            result.reason = "truncated-element-id";
            return result;
        }
        unsigned sizeCode = 0;
        if (!readVariableBits(&reader, 4, 4, &sizeCode, nullptr)) {
            result.reason = "truncated-or-invalid-element-size";
            return result;
        }
        if (sizeCode > kMaxElementSizeCode || sizeCode == std::numeric_limits<unsigned>::max()) {
            result.reason = "element-size-out-of-range";
            return result;
        }
        element.sizeBytes = sizeCode + 1U;
        const std::size_t elementBits = static_cast<std::size_t>(element.sizeBytes) * 8U;
        if (elementBits > reader.remaining()) {
            result.reason = "element-size-overrun";
            return result;
        }
        Reader elementReader {payload.data(), payload.size(), reader.bit,
                              reader.bit + elementBits};
        if (result.alternateObjectDataPresent) {
            if (!elementReader.read(4, &element.alternateId)) {
                result.reason = "element-alternate-id-overrun";
                return result;
            }
            element.alternateIdPresent = true;
            if (element.alternateId != 0U) {
                unsupported = true;
                if (unsupportedReason.empty()) {
                    unsupportedReason = "reserved-alternate-object-data-id";
                }
            }
        }
        if (!elementReader.read(1, &value)) {
            result.reason = "element-discard-flag-overrun";
            return result;
        }
        element.discardUnknown = value != 0;
        if (!copyBits(&elementReader, &element.rawBody, &element.rawBodyBits)) {
            result.reason = "element-body-copy-overrun";
            return result;
        }
        reader.bit += elementBits;
        element.recognized = isRecognized(element.id);
        // TS 103 420 5.6.4.5: the discard flag is meaningful only for an
        // unknown element.  Its already-validated byte span is skipped by
        // advancing the parent reader above; known-element syntax is checked
        // later and can never be masked by this flag.
        if (!element.recognized && !element.discardUnknown) {
            unsupported = true;
            if (unsupportedReason.empty()) {
                unsupportedReason = "unknown-non-discardable-element";
            }
        }
        result.elements.push_back(std::move(element));
    }

    result.finalPaddingBits = reader.remaining();
    while (reader.remaining() > 0) {
        if (!reader.read(1, &value) || value != 0) {
            result.reason = "nonzero-final-padding";
            return result;
        }
    }
    result.bitsConsumed = reader.bit;
    if (unsupported) {
        result.disposition = B1Disposition::Unsupported;
        result.reason = unsupportedReason;
    } else {
        result.disposition = B1Disposition::Pass;
        result.reason = "bounded-header-program-and-element-inventory";
    }
    return result;
}

} // namespace eac3oamd
