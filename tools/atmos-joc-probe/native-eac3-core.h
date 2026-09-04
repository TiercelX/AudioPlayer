#pragma once

// Gate 8N-1a diagnostic-only E-AC-3 syntax boundary.  This file deliberately
// has no FFmpeg/libav dependency.  It stops before exponents, bit allocation,
// mantissas, coupling, SPX, AHT, and IMDCT reconstruction.

#include <cstddef>
#include <cstdint>
#include <array>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace eac3native {

enum class Disposition {
    Accepted,
    NotEac3,
    Unsupported,
    Malformed,
};

enum class FailureStage {
    None,
    Validation,
    Header,
    Bounds,
    Sequence,
    Assembly,
};

// Flow is intentionally separate from content disposition.  The native
// parser has no callback, but the generation/flush/cancel states are retained
// so the seam has the same terminal semantics as the reusable decoder session.
enum class FlowStatus {
    None,
    Canceled,
    AlreadyFlushed,
};

enum class StreamType : std::uint8_t {
    Independent = 0,
    Dependent = 1,
    Ac3Convert = 2,
    Reserved = 3,
    // Legacy AC-3 inventory classified from bsid <= 8; this is deliberately
    // distinct from the E-AC-3 strmtyp=2 value above.
    LegacyAc3 = 4,
};

struct ParseCapabilities {
    bool frameBoundaryRangeChecked = false;
    bool crcRangeChecked = false;
    bool crcVerified = false;
};

class BoundedBitReader {
public:
    BoundedBitReader(const std::uint8_t *data,
                     std::size_t byteCount,
                     std::size_t startBit = 0,
                     std::size_t limitBit = static_cast<std::size_t>(-1));

    bool canRead(unsigned count) const;
    bool read(unsigned count, std::uint32_t *value);
    bool skip(unsigned count);
    std::size_t position() const;
    std::size_t remaining() const;
    bool overrun() const;

private:
    const std::uint8_t *data_ = nullptr;
    std::size_t byteCount_ = 0;
    std::size_t bit_ = 0;
    std::size_t limitBit_ = 0;
    bool overrun_ = false;
};

struct FrameHeader {
    std::size_t offset = 0;
    std::size_t sizeBytes = 0;
    std::size_t endBit = 0;
    StreamType streamType = StreamType::Reserved;
    unsigned substreamId = 0;
    unsigned sampleRate = 0;
    unsigned blocks = 0;
    unsigned sampleCount = 0;
    unsigned channelCount = 0;
    unsigned acmod = 0;
    unsigned bsid = 0;
    bool lfe = false;
    bool dependent = false;
    bool additional = false;
    ParseCapabilities capabilities;
};

struct ParseResult {
    Disposition disposition = Disposition::Malformed;
    FailureStage stage = FailureStage::Validation;
    FlowStatus flow = FlowStatus::None;
    std::string reason;
    std::optional<FrameHeader> frame;
};

struct AccessUnit {
    std::vector<FrameHeader> frames;
    std::map<std::string, unsigned> blocksByStream;
    unsigned sampleRate = 0;
    unsigned sampleCount = 0;
    std::size_t compressedBytes = 0;
};

struct ProcessResult {
    Disposition disposition = Disposition::Accepted;
    FailureStage stage = FailureStage::None;
    FlowStatus flow = FlowStatus::None;
    std::string reason;
    std::optional<AccessUnit> completed;
};

// Some Dolby Blu-ray E-AC-3 elementary streams carry a 16-byte per-AU
// carriage header immediately before the first syncframe in the AU.  The
// header is not part of the E-AC-3 syncframe syntax.  Keep recognition here
// so the native probes and the FFmpeg-backed access-unit probe use the same
// bounded framing rule instead of each scanning for a guessed magic sequence.
struct Eb3FramingResult {
    bool ok = false;
    bool wrapped = false;
    std::size_t wrapperCount = 0;
    FailureStage stage = FailureStage::Validation;
    std::string reason;
    std::vector<std::uint8_t> normalized;
};

inline bool eb3SyncAt(const std::vector<std::uint8_t> &bytes,
                      std::size_t offset)
{
    return offset <= bytes.size() && bytes.size() - offset >= 2U
        && bytes[offset] == 0x0b && bytes[offset + 1U] == 0x77;
}

inline bool eb3ReadBits(const std::vector<std::uint8_t> &bytes,
                        std::size_t bitOffset,
                        unsigned count,
                        unsigned *value)
{
    if (!value || count == 0U || count > 32U
        || bitOffset > bytes.size() * 8U
        || count > bytes.size() * 8U - bitOffset) {
        return false;
    }
    unsigned result = 0U;
    for (unsigned index = 0U; index < count; ++index) {
        result = (result << 1U)
            | ((bytes[(bitOffset + index) / 8U]
                >> (7U - ((bitOffset + index) % 8U))) & 1U);
    }
    *value = result;
    return true;
}

inline bool eb3ValidateWrapper(const std::vector<std::uint8_t> &bytes,
                               std::size_t offset,
                               std::string *reason)
{
    if (offset > bytes.size() || bytes.size() - offset < 16U) {
        if (reason) {
            *reason = "eb3-wrapper-truncated";
        }
        return false;
    }
    static constexpr std::array<std::uint8_t, 4> kMagic =
        {{0x01, 0x10, 0x00, 0x01}};
    static constexpr std::array<std::uint8_t, 4> kTail =
        {{0x00, 0x08, 0x80, 0x00}};
    for (std::size_t index = 0U; index < kMagic.size(); ++index) {
        if (bytes[offset + index] != kMagic[index]) {
            if (reason) {
                *reason = "eb3-wrapper-bad-magic";
            }
            return false;
        }
    }
    // Bytes 4..11 are carriage metadata. They vary across the supplied DEE
    // 5.2.1 stream (including within one file), so keep them opaque here.
    for (std::size_t index = 0U; index < kTail.size(); ++index) {
        if (bytes[offset + 12U + index] != kTail[index]) {
            if (reason) {
                *reason = "eb3-wrapper-bad-tail";
            }
            return false;
        }
    }
    return true;
}

inline bool eb3FrameSizeAt(const std::vector<std::uint8_t> &bytes,
                           std::size_t offset,
                           std::size_t *size,
                           std::string *reason)
{
    static constexpr std::array<std::array<unsigned, 3>, 38> kAc3FrameSizeWords = {{
        {{64, 69, 96}}, {{64, 70, 96}}, {{80, 87, 120}}, {{80, 88, 120}},
        {{96, 104, 144}}, {{96, 105, 144}}, {{112, 121, 168}}, {{112, 122, 168}},
        {{128, 139, 192}}, {{128, 140, 192}}, {{160, 174, 240}}, {{160, 175, 240}},
        {{192, 208, 288}}, {{192, 209, 288}}, {{224, 243, 336}}, {{224, 244, 336}},
        {{256, 278, 384}}, {{256, 279, 384}}, {{320, 348, 480}}, {{320, 349, 480}},
        {{384, 417, 576}}, {{384, 418, 576}}, {{448, 487, 672}}, {{448, 488, 672}},
        {{512, 557, 768}}, {{512, 558, 768}}, {{640, 696, 960}}, {{640, 697, 960}},
        {{768, 835, 1152}}, {{768, 836, 1152}}, {{896, 975, 1344}}, {{896, 976, 1344}},
        {{1024, 1114, 1536}}, {{1024, 1115, 1536}}, {{1152, 1253, 1728}},
        {{1152, 1254, 1728}}, {{1280, 1393, 1920}}, {{1280, 1394, 1920}},
    }};
    if (!size || offset > bytes.size() || bytes.size() - offset < 7U
        || !eb3SyncAt(bytes, offset)) {
        if (reason) {
            *reason = "eb3-frame-header-truncated";
        }
        return false;
    }
    unsigned bsid = 0U;
    if (!eb3ReadBits(bytes, offset * 8U + 40U, 5U, &bsid)) {
        if (reason) {
            *reason = "eb3-frame-header-truncated";
        }
        return false;
    }
    std::size_t frameSize = 0U;
    if (bsid <= 8U) {
        unsigned fscod = 0U;
        unsigned frmsizecod = 0U;
        if (!eb3ReadBits(bytes, offset * 8U + 32U, 2U, &fscod)
            || !eb3ReadBits(bytes, offset * 8U + 34U, 6U, &frmsizecod)
            || fscod >= 3U || frmsizecod >= kAc3FrameSizeWords.size()) {
            if (reason) {
                *reason = "eb3-invalid-ac3-frame-header";
            }
            return false;
        }
        frameSize = static_cast<std::size_t>(
            kAc3FrameSizeWords[frmsizecod][fscod]) * 2U;
    } else if (bsid <= 10U) {
        if (reason) {
            *reason = "eb3-unsupported-bsid";
        }
        return false;
    } else if (bsid <= 16U) {
        unsigned streamType = 0U;
        unsigned frameSizeWords = 0U;
        unsigned fscod = 0U;
        if (!eb3ReadBits(bytes, offset * 8U + 16U, 2U, &streamType)
            || !eb3ReadBits(bytes, offset * 8U + 21U, 11U, &frameSizeWords)
            || !eb3ReadBits(bytes, offset * 8U + 32U, 2U, &fscod)) {
            if (reason) {
                *reason = "eb3-frame-header-truncated";
            }
            return false;
        }
        if (streamType >= 3U || fscod >= 3U) {
            if (reason) {
                *reason = "eb3-invalid-eac3-frame-header";
            }
            return false;
        }
        frameSize = static_cast<std::size_t>(frameSizeWords + 1U) * 2U;
    } else {
        if (reason) {
            *reason = "eb3-unsupported-bsid";
        }
        return false;
    }
    if (frameSize < 7U || frameSize > bytes.size() - offset) {
        if (reason) {
            *reason = "eb3-truncated-frame-payload";
        }
        return false;
    }
    *size = frameSize;
    return true;
}

inline Eb3FramingResult normalizeEb3Framing(
    const std::vector<std::uint8_t> &bytes)
{
    Eb3FramingResult result;
    if (bytes.empty()) {
        result.stage = FailureStage::Bounds;
        result.reason = "empty-input";
        return result;
    }

    if (eb3SyncAt(bytes, 0U)) {
        result.ok = true;
        result.normalized = bytes;
        return result;
    }
    if (bytes.size() < 16U) {
        result.stage = FailureStage::Bounds;
        result.reason = "eb3-wrapper-truncated";
        return result;
    }
    std::string wrapperReason;
    if (!eb3ValidateWrapper(bytes, 0U, &wrapperReason)) {
        result.stage = FailureStage::Header;
        result.reason = wrapperReason;
        return result;
    }
    if (!eb3SyncAt(bytes, 16U)) {
        result.stage = FailureStage::Sequence;
        result.reason = "eb3-wrapper-missing-syncword";
        return result;
    }
    result.wrapped = true;
    result.wrapperCount = 1U;
    std::size_t frameOffset = 16U;
    while (true) {
        if (!eb3SyncAt(bytes, frameOffset)) {
            result.stage = FailureStage::Sequence;
            result.reason = "eb3-wrapper-missing-syncword";
            return result;
        }
        std::size_t firstFrameSize = 0U;
        std::string frameReason;
        if (!eb3FrameSizeAt(bytes, frameOffset, &firstFrameSize, &frameReason)) {
            result.stage = FailureStage::Header;
            result.reason = frameReason;
            return result;
        }
        const std::size_t auEnd = frameOffset + firstFrameSize;
        result.normalized.insert(result.normalized.end(),
                                 bytes.begin()
                                     + static_cast<std::ptrdiff_t>(frameOffset),
                                 bytes.begin()
                                     + static_cast<std::ptrdiff_t>(auEnd));
        if (auEnd == bytes.size()) {
            result.ok = true;
            return result;
        }
        if (eb3SyncAt(bytes, auEnd)) {
            frameOffset = auEnd;
            continue;
        }
        if (bytes.size() - auEnd < 16U) {
            result.stage = FailureStage::Bounds;
            result.reason = "eb3-wrapper-truncated";
            return result;
        }
        std::string wrapperReason;
        if (!eb3ValidateWrapper(bytes, auEnd, &wrapperReason)) {
            result.stage = FailureStage::Header;
            result.reason = wrapperReason;
            return result;
        }
        if (!eb3SyncAt(bytes, auEnd + 16U)) {
            result.stage = FailureStage::Sequence;
            result.reason = "eb3-wrapper-missing-syncword";
            return result;
        }
        ++result.wrapperCount;
        frameOffset = auEnd + 16U;
    }
}

ParseResult parseSyncframe(const std::vector<std::uint8_t> &bytes,
                           std::size_t offset);

const char *toString(Disposition value);
const char *toString(FailureStage value);
const char *toString(FlowStatus value);
const char *toString(StreamType value);

class AccessUnitAssembler {
public:
    ProcessResult process(const ParseResult &parsed);
    ProcessResult process(const FrameHeader &frame);
    ProcessResult flush();
    ProcessResult cancel();
    void reset();

private:
    static std::string streamKey(const FrameHeader &frame);
    static bool isBaseSid0(const FrameHeader &frame);
    static std::string baseKey(const FrameHeader &frame);
    std::string frameKey(const FrameHeader &frame) const;
    static ProcessResult failure(Disposition disposition,
                                 FailureStage stage,
                                 std::string reason);
    ProcessResult terminal(FlowStatus flow, std::string reason) const;
    ProcessResult processMutable(const FrameHeader &frame);
    void startPending(const FrameHeader &frame);
    bool topologyMatches(std::string *reason) const;

    std::optional<AccessUnit> pending_;
    std::optional<FrameHeader> activeParent_;
    std::optional<FrameHeader> currentBaseFrame_;
    unsigned sampleRate_ = 0;
    unsigned baseBlocks_ = 0;
    unsigned nextAdditionalSid_ = 1;
    unsigned nextDependentLocalSid_ = 0;
    std::set<unsigned> roundIndependentSids_;
    std::optional<std::set<std::string>> expectedTopology_;
    bool canceled_ = false;
    bool flushed_ = false;
};

} // namespace eac3native
