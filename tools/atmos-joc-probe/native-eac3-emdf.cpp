#include "native-eac3-emdf.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>

namespace eac3native {
namespace {

class BitReader {
public:
    BitReader(const std::vector<std::uint8_t> &bytes, std::size_t start,
              std::size_t limit)
        : bytes_(bytes), bit_(start), limit_(std::min(limit, bytes.size() * 8U))
    {
    }

    bool canRead(unsigned count) const
    {
        return bit_ <= limit_ && count <= limit_ - bit_;
    }

    bool read(unsigned count, unsigned *value)
    {
        if (!value || count == 0U || count > 32U || !canRead(count)) return false;
        unsigned result = 0U;
        for (unsigned index = 0U; index < count; ++index) {
            result = (result << 1U)
                | ((bytes_[bit_ / 8U] >> (7U - (bit_ % 8U))) & 1U);
            ++bit_;
        }
        *value = result;
        return true;
    }

    bool skip(std::size_t count)
    {
        if (count > static_cast<std::size_t>(std::numeric_limits<unsigned>::max())) {
            return false;
        }
        if (!canRead(static_cast<unsigned>(count))) return false;
        bit_ += count;
        return true;
    }

    std::size_t position() const { return bit_; }
    std::size_t remaining() const { return bit_ <= limit_ ? limit_ - bit_ : 0U; }
    void setLimit(std::size_t limit) { limit_ = std::min(limit, bytes_.size() * 8U); }

private:
    const std::vector<std::uint8_t> &bytes_;
    std::size_t bit_ = 0U;
    std::size_t limit_ = 0U;
};

unsigned protectionBits(unsigned code, bool primary)
{
    if (primary && code == 0U) return 0U;
    switch (code) {
    case 0U: return 0U;
    case 1U: return 8U;
    case 2U: return 32U;
    case 3U: return 128U;
    default: return 0U;
    }
}

bool readVariableBits(BitReader *reader, unsigned groupBits, unsigned *value)
{
    if (!reader || !value || groupBits == 0U || groupBits > 16U) return false;
    std::uint64_t result = 0U;
    for (unsigned group = 0U; group < 8U; ++group) {
        unsigned part = 0U;
        unsigned more = 0U;
        if (!reader->read(groupBits, &part) || !reader->read(1U, &more)) return false;
        result += part;
        if (!more) {
            if (result > std::numeric_limits<unsigned>::max()) return false;
            *value = static_cast<unsigned>(result);
            return true;
        }
        result = (result << groupBits) + (1ULL << groupBits);
    }
    return false;
}

bool allZero(BitReader *reader)
{
    if (!reader) return false;
    while (reader->remaining() > 0U) {
        unsigned bit = 0U;
        if (!reader->read(1U, &bit) || bit != 0U) return false;
    }
    return true;
}

NativeEmdfResult malformed(std::size_t startBit, std::size_t frameEndBit,
                           const char *reason)
{
    NativeEmdfResult result;
    result.disposition = EmdfDisposition::Malformed;
    result.reason = reason ? reason : "native-emdf-malformed";
    result.frameStartBit = startBit;
    result.frameEndBit = frameEndBit;
    return result;
}

} // namespace

NativeEmdfResult parseNativeEmdfContainer(
    const std::vector<std::uint8_t> &bytes, std::size_t startBit,
    std::size_t frameEndBit)
{
    if (startBit > frameEndBit || frameEndBit > bytes.size() * 8U
        || frameEndBit - startBit < 32U) {
        return malformed(startBit, frameEndBit, "native-emdf-container-header-truncated");
    }
    BitReader reader(bytes, startBit, frameEndBit);
    unsigned sync = 0U;
    unsigned length = 0U;
    if (!reader.read(16U, &sync) || sync != 0x5838U || !reader.read(16U, &length)) {
        return malformed(startBit, frameEndBit, "native-emdf-sync-or-length-invalid");
    }
    if (length < 4U || length > (frameEndBit - reader.position()) / 8U) {
        return malformed(startBit, frameEndBit, "native-emdf-container-bounds-invalid");
    }
    const std::size_t containerEndBit = reader.position() + static_cast<std::size_t>(length) * 8U;
    reader.setLimit(containerEndBit);

    NativeEmdfResult result;
    result.disposition = EmdfDisposition::Accepted;
    result.frameStartBit = startBit;
    result.frameEndBit = frameEndBit;
    NativeEmdfContainer container;
    container.startBit = startBit;
    container.endBit = containerEndBit;
    container.lengthBytes = length;
    unsigned version = 0U;
    unsigned keyId = 0U;
    if (!reader.read(2U, &version)) return malformed(startBit, frameEndBit, "native-emdf-version-truncated");
    if (version == 3U && !readVariableBits(&reader, 2U, &version)) {
        return malformed(startBit, frameEndBit, "native-emdf-version-extended-truncated");
    }
    if (!reader.read(3U, &keyId)) return malformed(startBit, frameEndBit, "native-emdf-key-truncated");
    if (keyId == 7U && !readVariableBits(&reader, 3U, &keyId)) {
        return malformed(startBit, frameEndBit, "native-emdf-key-extended-truncated");
    }
    container.version = version;
    container.keyId = keyId;

    while (true) {
        const std::size_t payloadHeaderStartBit = reader.position();
        unsigned payloadId = 0U;
        if (!reader.read(5U, &payloadId)) return malformed(startBit, frameEndBit, "native-emdf-payload-id-truncated");
        if (payloadId == 31U && !readVariableBits(&reader, 5U, &payloadId)) {
            return malformed(startBit, frameEndBit, "native-emdf-payload-id-extended-truncated");
        }
        if (payloadId == 0U) break;
        NativeEmdfPayload payload;
        payload.id = payloadId;
        unsigned flag = 0U;
        if (!reader.read(1U, &flag)) return malformed(startBit, frameEndBit, "native-emdf-sample-offset-flag-truncated");
        payload.sampleOffsetExists = flag != 0U;
        if (payload.sampleOffsetExists
            && (!reader.read(11U, &payload.sampleOffset) || !reader.skip(1U))) {
            return malformed(startBit, frameEndBit, "native-emdf-sample-offset-truncated");
        }
        if (!reader.read(1U, &flag)) return malformed(startBit, frameEndBit, "native-emdf-duration-flag-truncated");
        payload.durationExists = flag != 0U;
        if (payload.durationExists && !readVariableBits(&reader, 11U, &payload.duration)) {
            return malformed(startBit, frameEndBit, "native-emdf-duration-truncated");
        }
        if (!reader.read(1U, &flag)) return malformed(startBit, frameEndBit, "native-emdf-group-flag-truncated");
        payload.groupIdExists = flag != 0U;
        if (payload.groupIdExists && !readVariableBits(&reader, 2U, &payload.groupId)) {
            return malformed(startBit, frameEndBit, "native-emdf-group-truncated");
        }
        if (!reader.read(1U, &flag)) return malformed(startBit, frameEndBit, "native-emdf-language-flag-truncated");
        if (flag && !reader.skip(8U)) return malformed(startBit, frameEndBit, "native-emdf-language-truncated");
        if (!reader.read(1U, &flag)) return malformed(startBit, frameEndBit, "native-emdf-discard-flag-truncated");
        payload.discardUnknownPayload = flag != 0U;
        if (!payload.discardUnknownPayload) {
            bool frameAligned = false;
            if (!payload.sampleOffsetExists) {
                if (!reader.read(1U, &flag)) return malformed(startBit, frameEndBit, "native-emdf-frame-aligned-flag-truncated");
                frameAligned = flag != 0U;
                if (frameAligned && (!reader.skip(1U) || !reader.skip(1U))) {
                    return malformed(startBit, frameEndBit, "native-emdf-frame-aligned-truncated");
                }
            }
            if (payload.sampleOffsetExists || frameAligned) {
                if (!reader.read(5U, &payload.priority) || !reader.read(2U, &payload.procAllowed)) {
                    return malformed(startBit, frameEndBit, "native-emdf-priority-truncated");
                }
            }
            payload.frameAligned = frameAligned;
        }
        if (!readVariableBits(&reader, 8U, &payload.sizeBytes)
            || static_cast<std::size_t>(payload.sizeBytes) * 8U > reader.remaining()) {
            return malformed(startBit, frameEndBit, "native-emdf-payload-size-bounds-invalid");
        }
        payload.headerStartBit = payloadHeaderStartBit;
        payload.dataStartBit = reader.position();
        if (payload.dataStartBit > containerEndBit
            || static_cast<std::size_t>(payload.sizeBytes) * 8U
                > containerEndBit - payload.dataStartBit) {
            return malformed(startBit, frameEndBit, "native-emdf-payload-data-bounds-invalid");
        }
        payload.bytes.reserve(payload.sizeBytes);
        for (unsigned index = 0U; index < payload.sizeBytes; ++index) {
            unsigned value = 0U;
            if (!reader.read(8U, &value)) return malformed(startBit, frameEndBit, "native-emdf-payload-bytes-truncated");
            payload.bytes.push_back(static_cast<std::uint8_t>(value));
        }
        payload.dataEndBit = reader.position();
        payload.dataStartByte = payload.dataStartBit / 8U;
        payload.dataEndByte = (payload.dataEndBit + 7U) / 8U;
        if (payload.dataEndBit - payload.dataStartBit
                != static_cast<std::size_t>(payload.sizeBytes) * 8U
            || payload.dataEndBit > containerEndBit
            || payload.dataStartByte > payload.dataEndByte
            || payload.dataEndByte > (containerEndBit + 7U) / 8U) {
            return malformed(startBit, frameEndBit, "native-emdf-payload-data-bounds-invalid");
        }
        container.payloads.push_back(std::move(payload));
    }

    unsigned primaryCode = 0U;
    unsigned secondaryCode = 0U;
    if (!reader.read(2U, &primaryCode) || !reader.read(2U, &secondaryCode)) {
        return malformed(startBit, frameEndBit, "native-emdf-protection-code-truncated");
    }
    container.protectionPrimaryBits = protectionBits(primaryCode, true);
    container.protectionSecondaryBits = protectionBits(secondaryCode, false);
    const std::size_t protectionBitsTotal = static_cast<std::size_t>(container.protectionPrimaryBits)
        + container.protectionSecondaryBits;
    if (primaryCode == 0U || reader.remaining() < protectionBitsTotal
        || !reader.skip(protectionBitsTotal) || !allZero(&reader)) {
        return malformed(startBit, frameEndBit, "native-emdf-protection-or-padding-invalid");
    }
    result.containers.push_back(std::move(container));
    return result;
}

NativeEmdfResult extractNativeEac3Emdf(
    const std::vector<std::uint8_t> &bytes, std::size_t auIndex)
{
    NativeEmdfResult result;
    result.auIndex = auIndex;
    const ParseResult parsed = parseSyncframe(bytes, 0U);
    if (parsed.disposition != Disposition::Accepted || !parsed.frame) {
        result.disposition = parsed.disposition == Disposition::Unsupported
            ? EmdfDisposition::Unsupported : EmdfDisposition::Malformed;
        result.reason = "native-emdf-frame-" + parsed.reason;
        return result;
    }
    result.frame = *parsed.frame;
    result.frameStartByte = result.frame.offset;
    result.frameEndByte = result.frame.offset + result.frame.sizeBytes;
    result.frameStartBit = result.frame.offset * 8U;
    result.frameEndBit = result.frame.endBit;
    if (result.frame.sizeBytes != bytes.size()) {
        result.disposition = EmdfDisposition::Malformed;
        result.reason = "native-emdf-requires-exactly-one-syncframe";
        return result;
    }
    if (result.frame.streamType != StreamType::Independent
        || result.frame.substreamId != 0U) {
        result.disposition = EmdfDisposition::Unsupported;
        result.reason = "native-emdf-requires-type0-independent-sid0";
        return result;
    }
    if (result.frame.sampleRate != 48000U || result.frame.blocks != 6U
        || result.frame.sampleCount != 1536U || result.frame.channelCount != 6U
        || result.frame.acmod != 7U || !result.frame.lfe) {
        result.disposition = EmdfDisposition::Unsupported;
        result.reason = "native-emdf-config3-topology-required";
        return result;
    }

    result.disposition = EmdfDisposition::Accepted;
    const std::size_t end = result.frame.endBit;
    std::size_t bit = result.frameStartBit;
    std::string firstCandidateReason;
    while (bit + 32U <= end) {
        unsigned sync = 0U;
        BitReader probe(bytes, bit, end);
        if (!probe.read(16U, &sync) || sync != 0x5838U) {
            ++bit;
            continue;
        }
        NativeEmdfResult candidate = parseNativeEmdfContainer(bytes, bit, end);
        if (candidate.disposition == EmdfDisposition::Accepted
            && !candidate.containers.empty()) {
            result.containers.push_back(std::move(candidate.containers.front()));
            bit = result.containers.back().endBit;
        } else {
            if (firstCandidateReason.empty()) firstCandidateReason = candidate.reason;
            ++bit;
        }
    }
    if (result.containers.empty() && !firstCandidateReason.empty()) {
        result.reason = firstCandidateReason;
    }
    return result;
}

const char *toString(EmdfDisposition value)
{
    switch (value) {
    case EmdfDisposition::Accepted: return "accepted";
    case EmdfDisposition::Unsupported: return "unsupported";
    case EmdfDisposition::Malformed: return "malformed";
    }
    return "unknown";
}

} // namespace eac3native
