#include "oamd-b2a.h"

#include <algorithm>
#include <limits>

namespace eac3oamd {
namespace {

constexpr unsigned kMaxObjects = 159;
constexpr unsigned kMaxBlocks = 8;

struct Reader {
    const std::uint8_t *data = nullptr;
    std::size_t bytes = 0;
    std::size_t bit = 0;
    std::size_t limit = 0;

    bool canRead(unsigned count) const
    {
        return bit <= limit && static_cast<std::size_t>(count) <= limit - bit;
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
        if (count > limit - std::min(bit, limit)) {
            return false;
        }
        bit += count;
        return true;
    }

    std::size_t remaining() const
    {
        return bit <= limit ? limit - bit : 0;
    }
};

bool copyBits(Reader *reader, std::vector<std::uint8_t> *bytes, std::size_t *bitCount)
{
    if (!reader || !bytes || !bitCount) {
        return false;
    }
    *bitCount = reader->remaining();
    bytes->assign((*bitCount + 7U) / 8U, 0);
    for (std::size_t index = 0; index < *bitCount; ++index) {
        unsigned bit = 0;
        if (!reader->read(1, &bit)) {
            return false;
        }
        if (bit != 0) {
            (*bytes)[index / 8U]
                |= static_cast<std::uint8_t>(1U << (7U - (index % 8U)));
        }
    }
    return true;
}

bool readObjectBasicInfo(Reader *reader, unsigned status, B2aObjectInfo *object,
                         std::string *reason)
{
    if (status == 1U) {
        object->basicInfoPresence = {true, true};
    } else {
        unsigned value = 0;
        if (!reader->read(1, &value)) {
            *reason = "truncated-basic-info-presence";
            return false;
        }
        object->basicInfoPresence[1] = value != 0;
        if (!reader->read(1, &value)) {
            *reason = "truncated-basic-info-presence";
            return false;
        }
        object->basicInfoPresence[0] = value != 0;
    }
    // The array is transmitted from index 1 to index 0.  Keep those indices
    // explicit so the raw fields cannot be mistaken for normalized values.
    if (object->basicInfoPresence[1]) {
        if (!reader->read(2, &object->gainIndex)) {
            *reason = "truncated-gain-index";
            return false;
        }
        object->gainIndexPresent = true;
        if (object->gainIndex == 2U) {
            if (!reader->read(6, &object->gainBits)) {
                *reason = "truncated-gain-bits";
                return false;
            }
            object->gainBitsPresent = true;
        }
    }
    if (object->basicInfoPresence[0]) {
        unsigned value = 0;
        if (!reader->read(1, &value)) {
            *reason = "truncated-default-priority-flag";
            return false;
        }
        object->defaultPriorityPresent = true;
        object->defaultPriority = value != 0;
        if (!object->defaultPriority) {
            if (!reader->read(5, &object->priorityBits)) {
                *reason = "truncated-priority-bits";
                return false;
            }
            object->priorityBitsPresent = true;
        }
    }
    return true;
}

bool readObjectRenderInfo(Reader *reader, unsigned status, unsigned blockIndex,
                          bool objectInBedOrIsf, B2aObjectInfo *object,
                          B2aBitOrder bitOrder, std::string *reason)
{
    if (status == 1U) {
        object->renderInfoPresence = {true, true, true, true};
    } else {
        unsigned wireMask = 0;
        if (!reader->read(4, &wireMask)) {
            *reason = "truncated-render-info-presence";
            return false;
        }
        // Reader::read returns the four-bit wire codeword as an ordinary
        // integer.  The public array is semantic order: position, zone,
        // size, screen.  5.5.11 and Table 31 label that same codeword in
        // opposite directions; keep the two mappings explicit.
        if (bitOrder == B2aBitOrder::Syntax5511Lsb) {
            object->renderInfoPresence[0] = (wireMask & 0x1U) != 0;
            object->renderInfoPresence[1] = (wireMask & 0x2U) != 0;
            object->renderInfoPresence[2] = (wireMask & 0x4U) != 0;
            object->renderInfoPresence[3] = (wireMask & 0x8U) != 0;
        } else {
            object->renderInfoPresence[0] = (wireMask & 0x8U) != 0;
            object->renderInfoPresence[1] = (wireMask & 0x4U) != 0;
            object->renderInfoPresence[2] = (wireMask & 0x2U) != 0;
            object->renderInfoPresence[3] = (wireMask & 0x1U) != 0;
        }
    }
    if (object->renderInfoPresence[0]) {
        unsigned value = 0;
        if (blockIndex == 0U) {
            object->differentialPosition = false;
        } else if (!reader->read(1, &value)) {
            *reason = "truncated-differential-position-flag";
            return false;
        } else {
            object->differentialPosition = value != 0;
        }
        object->differentialPositionPresent = true;
        if (object->differentialPosition) {
            for (unsigned &part : object->differentialPositionBits) {
                if (!reader->read(3, &part)) {
                    *reason = "truncated-differential-position";
                    return false;
                }
            }
        } else {
            if (!reader->read(6, &object->absoluteXBits)
                || !reader->read(6, &object->absoluteYBits)
                || !reader->read(1, &object->absoluteZSignBits)
                || !reader->read(4, &object->absoluteZBits)) {
                *reason = "truncated-absolute-position";
                return false;
            }
            object->absolutePositionPresent = true;
        }
        if (!reader->read(1, &value)) {
            *reason = "truncated-distance-flag";
            return false;
        }
        object->distanceSpecified = value != 0;
        if (object->distanceSpecified) {
            if (!reader->read(1, &value)) {
                *reason = "truncated-infinity-flag";
                return false;
            }
            object->objectAtInfinity = value != 0;
            if (!object->objectAtInfinity) {
                if (!reader->read(4, &object->distanceFactorIndex)) {
                    *reason = "truncated-distance-index";
                    return false;
                }
                object->distanceFactorPresent = true;
            }
        }
    }
    if (object->renderInfoPresence[1]) {
        unsigned elevation = 0;
        if (!reader->read(3, &object->zoneConstraintsIndex)
            || !reader->read(1, &elevation)) {
            *reason = "truncated-zone-constraints";
            return false;
        }
        object->zonePresent = true;
        object->enableElevation = elevation != 0;
    }
    if (object->renderInfoPresence[2]) {
        if (!reader->read(2, &object->sizeIndex)) {
            *reason = "truncated-size-index";
            return false;
        }
        object->sizePresent = true;
        if (object->sizeIndex == 1U) {
            if (!reader->read(5, &object->sizeBits)) {
                *reason = "truncated-scalar-size";
                return false;
            }
        } else if (object->sizeIndex == 2U
                   && (!reader->read(5, &object->widthBits)
                       || !reader->read(5, &object->depthBits)
                       || !reader->read(5, &object->heightBits))) {
            *reason = "truncated-3d-size";
            return false;
        }
    }
    if (object->renderInfoPresence[3]) {
        unsigned value = 0;
        if (!reader->read(1, &value)) {
            *reason = "truncated-screen-reference-flag";
            return false;
        }
        object->screenReferencePresent = true;
        object->useScreenReference = value != 0;
        if (object->useScreenReference
            && (!reader->read(3, &object->screenFactorBits)
                || !reader->read(2, &object->depthFactorIndex))) {
            *reason = "truncated-screen-reference";
            return false;
        }
    }
    unsigned snap = 0;
    if (!reader->read(1, &snap)) {
        *reason = "truncated-snap";
        return false;
    }
    object->snapPresent = true;
    object->snap = snap != 0;
    (void)objectInBedOrIsf;
    return true;
}

} // namespace

const char *b2aDispositionText(B2aDisposition disposition)
{
    switch (disposition) {
    case B2aDisposition::Pass: return "PASS";
    case B2aDisposition::Unsupported: return "UNSUPPORTED";
    case B2aDisposition::Malformed: return "MALFORMED";
    }
    return "MALFORMED";
}

const char *b2aBitOrderText(B2aBitOrder order)
{
    return order == B2aBitOrder::Syntax5511Lsb ? "Syntax5511Lsb" : "Table31Msb";
}

B2aFrame parseObjectElementImpl(const std::vector<std::uint8_t> &body,
                                std::size_t bodyBits,
                                unsigned objectCount,
                                const std::vector<bool> &objectInBedOrIsf,
                                B2aBitOrder bitOrder)
{
    B2aFrame result;
    if (body.empty() || bodyBits == 0 || bodyBits > body.size() * 8U) {
        result.reason = "object-body-size-out-of-range";
        return result;
    }
    if (objectCount == 0 || objectCount > kMaxObjects
        || objectInBedOrIsf.size() != objectCount) {
        result.reason = "object-count-or-type-map-out-of-range";
        return result;
    }
    Reader reader {body.data(), body.size(), 0, bodyBits};
    unsigned value = 0;
    if (!reader.read(2, &result.sampleOffsetCode)) {
        result.reason = "truncated-sample-offset-code";
        return result;
    }
    if (result.sampleOffsetCode == 1U) {
        if (!reader.read(2, &result.sampleOffsetIndex)) {
            result.reason = "truncated-sample-offset-index";
            return result;
        }
        result.sampleOffsetIndexPresent = true;
    } else if (result.sampleOffsetCode == 2U) {
        if (!reader.read(5, &result.sampleOffsetBits)) {
            result.reason = "truncated-sample-offset-bits";
            return result;
        }
        result.sampleOffsetBitsPresent = true;
    } else if (result.sampleOffsetCode == 3U) {
        result.disposition = B2aDisposition::Unsupported;
        result.reason = "reserved-sample-offset-code";
    }
    unsigned blockCode = 0;
    if (!reader.read(3, &blockCode)) {
        result.reason = "truncated-object-info-block-count";
        result.disposition = B2aDisposition::Malformed;
        return result;
    }
    result.objectInfoBlockCount = blockCode + 1U;
    if (result.objectInfoBlockCount == 0 || result.objectInfoBlockCount > kMaxBlocks) {
        result.reason = "object-info-block-count-out-of-range";
        result.disposition = B2aDisposition::Malformed;
        return result;
    }
    result.blocks.reserve(result.objectInfoBlockCount);
    for (unsigned block = 0; block < result.objectInfoBlockCount; ++block) {
        B2aBlockUpdate update;
        if (!reader.read(6, &update.blockOffsetFactor)
            || !reader.read(2, &update.rampDurationCode)) {
            result.reason = "truncated-block-update-info";
            result.disposition = B2aDisposition::Malformed;
            return result;
        }
        if (update.rampDurationCode == 3U) {
            unsigned useIndex = 0;
            if (!reader.read(1, &useIndex)) {
                result.reason = "truncated-ramp-duration-selector";
                result.disposition = B2aDisposition::Malformed;
                return result;
            }
            if (useIndex) {
                if (!reader.read(4, &update.rampDurationIndex)) {
                    result.reason = "truncated-ramp-duration-index";
                    result.disposition = B2aDisposition::Malformed;
                    return result;
                }
                update.rampDurationIndexPresent = true;
            } else {
                if (!reader.read(11, &update.rampDurationBits)) {
                    result.reason = "truncated-ramp-duration-bits";
                    result.disposition = B2aDisposition::Malformed;
                    return result;
                }
                update.rampDurationBitsPresent = true;
            }
        }
        result.blocks.push_back(update);
    }
    unsigned reservedNotPresent = 0;
    if (!reader.read(1, &reservedNotPresent)) {
        result.reason = "truncated-reserved-data-flag";
        result.disposition = B2aDisposition::Malformed;
        return result;
    }
    result.reservedDataNotPresent = reservedNotPresent != 0;
    if (!result.reservedDataNotPresent && !reader.read(5, &result.reservedData)) {
        result.reason = "truncated-reserved-data";
        result.disposition = B2aDisposition::Malformed;
        return result;
    }
    result.objectInfo.reserve(static_cast<std::size_t>(objectCount)
                              * result.objectInfoBlockCount);
    for (unsigned objectIndex = 0; objectIndex < objectCount; ++objectIndex) {
        for (unsigned blockIndex = 0; blockIndex < result.objectInfoBlockCount; ++blockIndex) {
            B2aObjectInfo object;
            object.objectIndex = objectIndex;
            object.blockIndex = blockIndex;
            unsigned inactive = 0;
            if (!reader.read(1, &inactive)) {
                result.reason = "truncated-object-active-flag";
                result.disposition = B2aDisposition::Malformed;
                return result;
            }
            object.objectNotActive = inactive != 0;
            if (object.objectNotActive) {
                object.basicInfoStatus = 0;
            } else if (blockIndex == 0U) {
                object.basicInfoStatus = 1;
            } else if (!reader.read(2, &object.basicInfoStatus)) {
                result.reason = "truncated-basic-info-status";
                result.disposition = B2aDisposition::Malformed;
                return result;
            }
            if ((object.basicInfoStatus == 1U || object.basicInfoStatus == 3U)
                && !readObjectBasicInfo(&reader, object.basicInfoStatus, &object,
                                        &result.reason)) {
                result.failureObjectIndex = objectIndex;
                result.failureBlockIndex = blockIndex;
                result.failureBitOffset = reader.bit;
                result.disposition = B2aDisposition::Malformed;
                return result;
            }
            if (object.objectNotActive) {
                object.renderInfoStatus = 0;
            } else if (objectInBedOrIsf[objectIndex]) {
                object.renderInfoStatus = 0;
            } else if (blockIndex == 0U) {
                object.renderInfoStatus = 1;
            } else if (!reader.read(2, &object.renderInfoStatus)) {
                result.reason = "truncated-render-info-status";
                result.disposition = B2aDisposition::Malformed;
                return result;
            }
            if ((object.renderInfoStatus == 1U || object.renderInfoStatus == 3U)
                && !readObjectRenderInfo(&reader, object.renderInfoStatus, blockIndex,
                                         objectInBedOrIsf[objectIndex], &object, bitOrder,
                                         &result.reason)) {
                result.failureObjectIndex = objectIndex;
                result.failureBlockIndex = blockIndex;
                result.failureBitOffset = reader.bit;
                result.disposition = B2aDisposition::Malformed;
                return result;
            }
            unsigned additional = 0;
            if (!reader.read(1, &additional)) {
                result.reason = "truncated-additional-data-flag";
                result.disposition = B2aDisposition::Malformed;
                return result;
            }
            object.additionalDataPresent = additional != 0;
            if (object.additionalDataPresent) {
                unsigned sizeCode = 0;
                if (!reader.read(4, &sizeCode)) {
                    result.reason = "truncated-additional-data-size";
                    result.disposition = B2aDisposition::Malformed;
                    return result;
                }
                object.additionalDataSizeBytes = sizeCode + 1U;
                const std::size_t additionalBits =
                    static_cast<std::size_t>(object.additionalDataSizeBytes) * 8U;
                if (additionalBits > reader.remaining()) {
                    result.reason = "additional-data-size-overrun";
                    result.disposition = B2aDisposition::Malformed;
                    return result;
                }
                Reader additionalReader {body.data(), body.size(), reader.bit,
                                         reader.bit + additionalBits};
                if (!copyBits(&additionalReader, &object.additionalData,
                              &object.additionalDataBits)) {
                    result.reason = "additional-data-copy-overrun";
                    result.disposition = B2aDisposition::Malformed;
                    return result;
                }
                reader.bit += additionalBits;
            }
            result.objectInfo.push_back(std::move(object));
        }
    }
    result.paddingBits = reader.remaining();
    while (reader.remaining() > 0) {
        if (!reader.read(1, &value) || value != 0U) {
            result.reason = "nonzero-object-element-padding";
            result.disposition = B2aDisposition::Malformed;
            return result;
        }
    }
    result.bitsConsumed = reader.bit;
    if (result.disposition == B2aDisposition::Unsupported) {
        return result;
    }
    result.disposition = B2aDisposition::Pass;
    result.reason = "bounded-object-update-syntax";
    return result;
}

B2aFrame parseObjectElement(const std::vector<std::uint8_t> &body,
                            std::size_t bodyBits,
                            unsigned objectCount,
                            const std::vector<bool> &objectInBedOrIsf,
                            B2aBitOrder bitOrder)
{
    B2aFrame result = parseObjectElementImpl(body, bodyBits, objectCount,
                                             objectInBedOrIsf, bitOrder);
    if (result.disposition != B2aDisposition::Pass) {
        // The public failure result is atomic: no caller can accidentally
        // consume blocks or object records parsed before a late error.
        result.blocks.clear();
        result.objectInfo.clear();
        result.bitsConsumed = 0;
        result.paddingBits = 0;
    }
    return result;
}

} // namespace eac3oamd
