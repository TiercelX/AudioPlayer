#pragma once

// Gate J0A1: FFmpeg-free extraction of EMDF containers from one bounded,
// complete config-3 E-AC-3 syncframe.  This component delivers compressed
// payload bytes only; it does not qualify JOC, decode PCM, or apply DRC.

#include "native-eac3-core.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace eac3native {

enum class EmdfDisposition {
    Accepted,
    Unsupported,
    Malformed,
};

struct NativeEmdfPayload {
    // Header starts at the bit before payload_id. Data bounds cover only the
    // payload bytes, not the payload header or variable-size field.
    std::size_t headerStartBit = 0U;
    std::size_t dataStartBit = 0U;
    std::size_t dataEndBit = 0U;
    std::size_t dataStartByte = 0U;
    std::size_t dataEndByte = 0U;
    unsigned id = 0U;
    unsigned sizeBytes = 0U;
    bool sampleOffsetExists = false;
    unsigned sampleOffset = 0U;
    bool durationExists = false;
    unsigned duration = 0U;
    bool groupIdExists = false;
    unsigned groupId = 0U;
    bool discardUnknownPayload = false;
    bool frameAligned = false;
    unsigned priority = 0U;
    unsigned procAllowed = 0U;
    std::vector<std::uint8_t> bytes;
};

struct NativeEmdfContainer {
    std::size_t startBit = 0U;
    std::size_t endBit = 0U;
    unsigned lengthBytes = 0U;
    unsigned version = 0U;
    unsigned keyId = 0U;
    unsigned protectionPrimaryBits = 0U;
    unsigned protectionSecondaryBits = 0U;
    std::vector<NativeEmdfPayload> payloads;
};

struct NativeEmdfResult {
    EmdfDisposition disposition = EmdfDisposition::Malformed;
    std::string reason;
    std::size_t auIndex = 0U;
    std::size_t frameStartByte = 0U;
    std::size_t frameEndByte = 0U;
    std::size_t frameStartBit = 0U;
    std::size_t frameEndBit = 0U;
    FrameHeader frame;
    std::vector<NativeEmdfContainer> containers;
};

// Parse one candidate container at an exact bit address.  This public helper
// returns Malformed for truncation, invalid lengths, protection, or trailing
// non-zero bits; callers scanning a frame may skip non-container candidates.
NativeEmdfResult parseNativeEmdfContainer(
    const std::vector<std::uint8_t> &bytes, std::size_t startBit,
    std::size_t frameEndBit);

// Parse exactly one complete config-3 type-0 independent SID0 syncframe.
// EMDF containers are found only by strict parse+bounds checks and are
// non-overlapping in ascending bit order.
NativeEmdfResult extractNativeEac3Emdf(
    const std::vector<std::uint8_t> &bytes, std::size_t auIndex = 0U);

const char *toString(EmdfDisposition value);

} // namespace eac3native
