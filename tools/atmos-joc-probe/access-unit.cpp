// Gate 1 diagnostic: assemble bounded E-AC-3 access units from either a raw
// elementary stream or demuxed container packets. This is not production code.

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/samplefmt.h>
}

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "joc-gate5a.h"
#include "joc-gate5b.h"
#include "joc-gate6c.h"
#include "bear-export.h"
#include "joc-qmf.h"
#include "joc-matrix-trace.h"
#include "joc-session.h"
#include "annex-h-headphone.h"
#include "oamd-b1.h"
#include "oamd-b2a.h"
#include "oamd-b2b.h"
#include "qmf-bank.h"
#include "spatial-property-adapter.h"
#include "spatial-bridge-renderer.h"
#include "native-eac3-core.h"

namespace {

enum class FrameType : unsigned {
    Independent = 0,
    Dependent = 1,
    Ac3Convert = 2,
    Reserved = 3,
};

struct BitReader {
    const std::uint8_t *data = nullptr;
    std::size_t size = 0;
    std::size_t bit = 0;
    std::size_t limitBits = static_cast<std::size_t>(-1);

    bool canRead(unsigned count) const
    {
        const std::size_t totalBits = std::min(size * 8U, limitBits);
        return bit <= totalBits && count <= totalBits - bit;
    }

    bool read(unsigned count, unsigned *value)
    {
        if (!value || count > 32 || !canRead(count)) {
            return false;
        }
        unsigned result = 0;
        for (unsigned index = 0; index < count; ++index) {
            result = (result << 1U) | ((data[bit / 8U] >> (7U - (bit % 8U))) & 1U);
            ++bit;
        }
        *value = result;
        return true;
    }

    bool skip(unsigned count)
    {
        if (!canRead(count)) {
            return false;
        }
        bit += count;
        return true;
    }

    std::size_t position() const
    {
        return bit;
    }

    std::size_t remaining() const
    {
        const std::size_t totalBits = std::min(size * 8U, limitBits);
        return bit <= totalBits ? totalBits - bit : 0;
    }
};

struct Syncframe {
    std::size_t offset = 0;
    std::size_t unitOffset = 0;
    std::size_t sizeBytes = 0;
    std::size_t startBit = 0;
    std::size_t endBit = 0;
    FrameType frameType = FrameType::Reserved;
    unsigned substreamId = 0;
    unsigned sampleRate = 0;
    unsigned blocks = 0;
    unsigned channelCount = 0;
    unsigned acmod = 0;
    unsigned bsid = 0;
    bool lfe = false;
};

struct ParseResult {
    bool ok = false;
    std::string reason;
    Syncframe frame;
};

struct AccessUnit {
    std::vector<Syncframe> frames;
    std::vector<std::uint8_t> compressedBytes;
    std::map<std::string, unsigned> blocksByStream;
    unsigned sampleRate = 0;
    std::size_t ordinal = 0U;
    std::int64_t timestampSamples = 0;
};

struct EmdfPayload {
    unsigned id = 0;
    unsigned sizeBytes = 0;
    bool sampleOffsetExists = false;
    unsigned sampleOffset = 0;
    bool durationExists = false;
    unsigned duration = 0;
    bool groupIdExists = false;
    unsigned groupId = 0;
    bool discardUnknownPayload = false;
    bool frameAligned = false;
    unsigned priority = 0;
    unsigned procAllowed = 0;
    std::vector<std::uint8_t> bytes;
};

struct EmdfContainer {
    std::size_t startBit = 0;
    unsigned lengthBytes = 0;
    unsigned version = 0;
    unsigned keyId = 0;
    unsigned protectionPrimaryBits = 0;
    unsigned protectionSecondaryBits = 0;
    std::vector<EmdfPayload> payloads;
};

struct Chunk {
    std::string source;
    std::vector<std::uint8_t> bytes;
    std::int64_t skipSamples = 0;
    std::int64_t discardPadding = 0;
};

struct ProbeResult {
    bool ok = false;
    std::string reason;
    std::vector<AccessUnit> units;
    std::size_t frameCount = 0;
    std::size_t ac3ConvertCount = 0;
    std::size_t independentCount = 0;
    std::size_t dependentCount = 0;
    std::size_t reservedCount = 0;
};

constexpr std::array<unsigned, 3> kSampleRates = {48000, 44100, 32000};
constexpr std::array<unsigned, 4> kReducedSampleRates = {24000, 22050, 16000, 0};
constexpr std::array<unsigned, 4> kEac3Blocks = {1, 2, 3, 6};
constexpr std::array<std::array<unsigned, 3>, 38> kAc3FrameSizeWords = {{
    {{64, 69, 96}}, {{64, 70, 96}}, {{80, 87, 120}}, {{80, 88, 120}},
    {{96, 104, 144}}, {{96, 105, 144}}, {{112, 121, 168}}, {{112, 122, 168}},
    {{128, 139, 192}}, {{128, 140, 192}}, {{160, 174, 240}}, {{160, 175, 240}},
    {{192, 208, 288}}, {{192, 209, 288}}, {{224, 243, 336}}, {{224, 244, 336}},
    {{256, 278, 384}}, {{256, 279, 384}}, {{320, 348, 480}}, {{320, 349, 480}},
    {{384, 417, 576}}, {{384, 418, 576}}, {{448, 487, 672}}, {{448, 488, 672}},
    {{512, 557, 768}}, {{512, 558, 768}}, {{640, 696, 960}}, {{640, 697, 960}},
    {{768, 835, 1152}}, {{768, 836, 1152}}, {{896, 975, 1344}}, {{896, 976, 1344}},
    {{1024, 1114, 1536}}, {{1024, 1115, 1536}}, {{1152, 1253, 1728}}, {{1152, 1254, 1728}},
    {{1280, 1393, 1920}}, {{1280, 1394, 1920}},
}};
constexpr std::array<unsigned, 8> kAc3Channels = {2, 1, 2, 3, 3, 4, 4, 5};

unsigned channelCount(unsigned acmod, bool lfe)
{
    return kAc3Channels[acmod & 7U] + (lfe ? 1U : 0U);
}

std::string frameTypeText(FrameType type)
{
    switch (type) {
    case FrameType::Independent: return "independent";
    case FrameType::Dependent: return "dependent";
    case FrameType::Ac3Convert: return "ac3-convert";
    case FrameType::Reserved: return "reserved";
    }
    return "unknown";
}

std::string streamKey(const Syncframe &frame)
{
    return frameTypeText(frame.frameType) + "/sid" + std::to_string(frame.substreamId);
}

ParseResult parseSyncframe(const std::vector<std::uint8_t> &bytes, std::size_t offset)
{
    ParseResult result;
    result.frame.offset = offset;
    result.frame.startBit = offset * 8U;
    if (offset > bytes.size() || bytes.size() - offset < 7) {
        result.reason = "truncated-header";
        return result;
    }
    if (bytes[offset] != 0x0B || bytes[offset + 1] != 0x77) {
        result.reason = "missing-syncword";
        return result;
    }

    BitReader probe {bytes.data(), bytes.size(), offset * 8U + 16U};
    if (!probe.skip(24)) {
        result.reason = "truncated-bsid";
        return result;
    }
    unsigned bsid = 0;
    if (!probe.read(5, &bsid)) {
        result.reason = "truncated-bsid";
        return result;
    }
    result.frame.bsid = bsid;

    if (bsid <= 8) {
        // AC-3 core carried as an E-AC-3 AC3_CONVERT frame. The frame size is
        // the normative AC-3 table, not the E-AC-3 11-bit frmsiz field.
        BitReader reader {bytes.data(), bytes.size(), offset * 8U + 16U};
        unsigned crc1 = 0;
        unsigned fscod = 0;
        unsigned frmsizecod = 0;
        unsigned parsedBsid = 0;
        if (!reader.read(16, &crc1)
            || !reader.read(2, &fscod)
            || !reader.read(6, &frmsizecod)
            || !reader.read(5, &parsedBsid)) {
            result.reason = "truncated-ac3-header";
            return result;
        }
        (void)crc1;
        if (fscod >= kSampleRates.size()) {
            result.reason = "invalid-ac3-sample-rate-code";
            return result;
        }
        if (frmsizecod >= kAc3FrameSizeWords.size()) {
            result.reason = "invalid-ac3-frame-size-code";
            return result;
        }
        result.frame.sampleRate = kSampleRates[fscod] >> std::max(0, static_cast<int>(parsedBsid) - 8);
        result.frame.sizeBytes = static_cast<std::size_t>(kAc3FrameSizeWords[frmsizecod][fscod]) * 2U;
        result.frame.frameType = FrameType::Ac3Convert;
        result.frame.substreamId = 0;
        result.frame.blocks = 6;

        unsigned bsmod = 0;
        unsigned acmod = 0;
        if (!reader.read(3, &bsmod) || !reader.read(3, &acmod)) {
            result.reason = "truncated-ac3-channel-header";
            return result;
        }
        (void)bsmod;
        if (acmod == 1) {
            if (!reader.skip(2)) {
                result.reason = "truncated-ac3-stereo-header";
                return result;
            }
        } else {
            if ((acmod & 1U) && acmod != 1U && !reader.skip(2)) {
                result.reason = "truncated-ac3-center-header";
                return result;
            }
            if ((acmod & 4U) && !reader.skip(2)) {
                result.reason = "truncated-ac3-surround-header";
                return result;
            }
        }
        unsigned lfe = 0;
        if (!reader.read(1, &lfe)) {
            result.reason = "truncated-ac3-lfe-header";
            return result;
        }
        result.frame.acmod = acmod;
        result.frame.lfe = lfe != 0;
        result.frame.channelCount = channelCount(acmod, result.frame.lfe);
    } else if (bsid <= 10) {
        result.reason = "reserved-legacy-bsid";
        return result;
    } else {
        BitReader reader {bytes.data(), bytes.size(), offset * 8U + 16U};
        unsigned frameType = 0;
        unsigned substreamId = 0;
        unsigned frameSizeWords = 0;
        unsigned fscod = 0;
        unsigned numBlocksCode = 0;
        unsigned acmod = 0;
        unsigned lfe = 0;
        unsigned parsedBsid = 0;
        if (!reader.read(2, &frameType)
            || !reader.read(3, &substreamId)
            || !reader.read(11, &frameSizeWords)
            || !reader.read(2, &fscod)) {
            result.reason = "truncated-eac3-header";
            return result;
        }
        result.frame.frameType = static_cast<FrameType>(frameType);
        result.frame.substreamId = substreamId;
        if (result.frame.frameType == FrameType::Reserved) {
            result.reason = "reserved-stream-type";
            return result;
        }
        // This monolithic parser is the FFmpeg-side differential oracle. It
        // intentionally retains normative E-AC-3 fscod=3 reduced-sample-rate
        // syntax coverage, which is broader than the current Gate8N native
        // core acceptance and must not be treated as native acceptance proof.
        if (fscod == 3) {
            unsigned fscod2 = 0;
            if (!reader.read(2, &fscod2) || fscod2 >= 3) {
                result.reason = "invalid-reduced-sample-rate-code";
                return result;
            }
            result.frame.sampleRate = kReducedSampleRates[fscod2];
            result.frame.blocks = 6;
        } else {
            if (fscod >= kSampleRates.size()
                || !reader.read(2, &numBlocksCode)) {
                result.reason = "invalid-eac3-sample-rate-or-block-code";
                return result;
            }
            result.frame.sampleRate = kSampleRates[fscod];
            result.frame.blocks = kEac3Blocks[numBlocksCode];
        }
        if (!reader.read(3, &acmod)
            || !reader.read(1, &lfe)
            || !reader.read(5, &parsedBsid)) {
            result.reason = "truncated-eac3-channel-header";
            return result;
        }
        if (parsedBsid < 11 || parsedBsid > 16) {
            result.reason = "invalid-eac3-bsid";
            return result;
        }
        result.frame.bsid = parsedBsid;
        result.frame.sizeBytes = static_cast<std::size_t>(frameSizeWords + 1U) * 2U;
        result.frame.acmod = acmod;
        result.frame.lfe = lfe != 0;
        result.frame.channelCount = channelCount(acmod, result.frame.lfe);
    }

    if (result.frame.sizeBytes < 7) {
        result.reason = "invalid-frame-size";
        return result;
    }
    if (result.frame.offset > bytes.size()
        || result.frame.sizeBytes > bytes.size() - result.frame.offset) {
        result.reason = "truncated-frame-payload";
        return result;
    }
    result.frame.endBit = (result.frame.offset + result.frame.sizeBytes) * 8U;
    result.ok = true;
    return result;
}

bool readVariableBits(BitReader *reader, unsigned groupBits, unsigned *value)
{
    if (!reader || !value || groupBits == 0 || groupBits > 16) {
        return false;
    }
    std::uint64_t result = 0;
    for (unsigned group = 0; group < 8; ++group) {
        unsigned part = 0;
        unsigned more = 0;
        if (!reader->read(groupBits, &part) || !reader->read(1, &more)) {
            return false;
        }
        result += part;
        if (!more) {
            if (result > 0xffffffffULL) {
                return false;
            }
            *value = static_cast<unsigned>(result);
            return true;
        }
        result = (result << groupBits) + (1ULL << groupBits);
    }
    return false;
}

unsigned protectionBits(unsigned code, bool primary)
{
    if (primary && code == 0) {
        return 0;
    }
    switch (code) {
    case 0: return 0;
    case 1: return 8;
    case 2: return 32;
    case 3: return 128;
    }
    return 0;
}

bool allZero(BitReader *reader)
{
    if (!reader) {
        return false;
    }
    while (reader->remaining() > 0) {
        unsigned bit = 0;
        if (!reader->read(1, &bit) || bit != 0) {
            return false;
        }
    }
    return true;
}

std::optional<EmdfContainer> parseEmdfAt(const std::vector<std::uint8_t> &bytes,
                                         std::size_t startBit,
                                         std::size_t frameEndBit)
{
    if (startBit + 32 > frameEndBit) {
        return std::nullopt;
    }
    BitReader reader {bytes.data(), bytes.size(), startBit};
    unsigned sync = 0;
    unsigned length = 0;
    if (!reader.read(16, &sync) || sync != 0x5838 || !reader.read(16, &length)) {
        return std::nullopt;
    }
    const std::size_t containerEndBit = reader.position() + static_cast<std::size_t>(length) * 8U;
    if (length < 4 || containerEndBit > frameEndBit) {
        return std::nullopt;
    }
    reader.limitBits = containerEndBit;

    EmdfContainer container;
    container.startBit = startBit;
    container.lengthBytes = length;
    unsigned version = 0;
    unsigned keyId = 0;
    if (!reader.read(2, &version)) {
        return std::nullopt;
    }
    if (version == 3 && !readVariableBits(&reader, 2, &version)) {
        return std::nullopt;
    }
    if (!reader.read(3, &keyId)) {
        return std::nullopt;
    }
    if (keyId == 7 && !readVariableBits(&reader, 3, &keyId)) {
        return std::nullopt;
    }
    container.version = version;
    container.keyId = keyId;

    while (true) {
        unsigned payloadId = 0;
        if (!reader.read(5, &payloadId)) {
            return std::nullopt;
        }
        if (payloadId == 31 && !readVariableBits(&reader, 5, &payloadId)) {
            return std::nullopt;
        }
        if (payloadId == 0) {
            break;
        }

        EmdfPayload payload;
        payload.id = payloadId;
        unsigned flag = 0;
        if (!reader.read(1, &flag)) {
            return std::nullopt;
        }
        payload.sampleOffsetExists = flag != 0;
        if (payload.sampleOffsetExists) {
            if (!reader.read(11, &payload.sampleOffset) || !reader.skip(1)) {
                return std::nullopt;
            }
        }
        if (!reader.read(1, &flag)) {
            return std::nullopt;
        }
        payload.durationExists = flag != 0;
        if (payload.durationExists && !readVariableBits(&reader, 11, &payload.duration)) {
            return std::nullopt;
        }
        if (!reader.read(1, &flag)) {
            return std::nullopt;
        }
        payload.groupIdExists = flag != 0;
        if (payload.groupIdExists && !readVariableBits(&reader, 2, &payload.groupId)) {
            return std::nullopt;
        }
        if (!reader.read(1, &flag)) {
            return std::nullopt;
        }
        if (flag && !reader.skip(8)) {
            return std::nullopt;
        }
        if (!reader.read(1, &flag)) {
            return std::nullopt;
        }
        payload.discardUnknownPayload = flag != 0;
        if (!payload.discardUnknownPayload) {
            bool frameAligned = false;
            if (!payload.sampleOffsetExists) {
                if (!reader.read(1, &flag)) {
                    return std::nullopt;
                }
                frameAligned = flag != 0;
                if (frameAligned && (!reader.skip(1) || !reader.skip(1))) {
                    return std::nullopt;
                }
            }
            if (payload.sampleOffsetExists || frameAligned) {
                if (!reader.read(5, &payload.priority)
                    || !reader.read(2, &payload.procAllowed)) {
                    return std::nullopt;
                }
            }
            payload.frameAligned = frameAligned;
        }

        if (!readVariableBits(&reader, 8, &payload.sizeBytes)
            || static_cast<std::size_t>(payload.sizeBytes) * 8U > reader.remaining()) {
            return std::nullopt;
        }
        payload.bytes.reserve(payload.sizeBytes);
        for (unsigned index = 0; index < payload.sizeBytes; ++index) {
            unsigned value = 0;
            if (!reader.read(8, &value)) {
                return std::nullopt;
            }
            payload.bytes.push_back(static_cast<std::uint8_t>(value));
        }
        container.payloads.push_back(std::move(payload));
    }

    unsigned primaryCode = 0;
    unsigned secondaryCode = 0;
    if (!reader.read(2, &primaryCode) || !reader.read(2, &secondaryCode)) {
        return std::nullopt;
    }
    container.protectionPrimaryBits = protectionBits(primaryCode, true);
    container.protectionSecondaryBits = protectionBits(secondaryCode, false);
    if (primaryCode == 0
        || reader.remaining() < container.protectionPrimaryBits + container.protectionSecondaryBits
        || !reader.skip(container.protectionPrimaryBits + container.protectionSecondaryBits)
        || !allZero(&reader)) {
        return std::nullopt;
    }
    return container;
}

std::vector<EmdfContainer> scanEmdf(const AccessUnit &unit, const Syncframe &frame)
{
    std::vector<EmdfContainer> containers;
    const std::size_t begin = frame.unitOffset * 8U;
    const std::size_t end = begin + frame.sizeBytes * 8U;
    for (std::size_t bit = begin; bit + 32 <= end; ++bit) {
        BitReader probe {unit.compressedBytes.data(), unit.compressedBytes.size(), bit};
        unsigned sync = 0;
        if (!probe.read(16, &sync) || sync != 0x5838) {
            continue;
        }
        std::optional<EmdfContainer> parsed = parseEmdfAt(unit.compressedBytes, bit, end);
        if (parsed) {
            containers.push_back(std::move(*parsed));
            bit = containers.back().startBit + containers.back().lengthBytes * 8U + 31U;
        }
    }
    return containers;
}

class AccessUnitAssembler {
public:
    bool consume(const std::vector<std::uint8_t> &chunk,
                 const std::string &source,
                 std::size_t maxUnits,
                 ProbeResult *report)
    {
        if (!report) {
            return false;
        }
        std::size_t offset = 0;
        bool sawFrame = false;
        while (offset < chunk.size()) {
            if (chunk.size() - offset < 2 || chunk[offset] != 0x0B || chunk[offset + 1] != 0x77) {
                if (!sawFrame) {
                    ++offset;
                    continue;
                }
                report->reason = source + ":unexpected-bytes-at-" + std::to_string(offset);
                return false;
            }

            ParseResult parsed = parseSyncframe(chunk, offset);
            if (!parsed.ok) {
                report->reason = source + ":frame-" + std::to_string(offset) + ":" + parsed.reason;
                return false;
            }
            sawFrame = true;
            ++report->frameCount;
            if (parsed.frame.frameType == FrameType::Ac3Convert) {
                ++report->ac3ConvertCount;
            } else if (parsed.frame.frameType == FrameType::Independent) {
                ++report->independentCount;
            } else if (parsed.frame.frameType == FrameType::Dependent) {
                ++report->dependentCount;
            } else {
                ++report->reservedCount;
            }
            if (!consumeFrame(chunk, parsed.frame, source, report)) {
                return false;
            }
            offset += parsed.frame.sizeBytes;
            if (maxUnits != 0 && report->units.size() >= maxUnits) {
                stopRequested_ = true;
                return true;
            }
        }
        return true;
    }

    bool finish(ProbeResult *report)
    {
        if (!report) {
            return false;
        }
        if (stopRequested_) {
            pending_.reset();
        } else if (pending_) {
            if (!isComplete()) {
                report->reason = "incomplete-access-unit";
                return false;
            }
            flush(report);
        }
        report->ok = report->frameCount > 0 && !report->units.empty();
        if (!report->ok && report->reason.empty()) {
            report->reason = "no-access-units";
        }
        return report->ok;
    }

private:
    static bool sameKey(const Syncframe &left, const Syncframe &right)
    {
        return left.frameType == right.frameType && left.substreamId == right.substreamId;
    }

    bool isComplete() const
    {
        if (!pending_ || pending_->frames.empty()) {
            return false;
        }
        return std::all_of(pending_->blocksByStream.begin(),
                           pending_->blocksByStream.end(),
                           [](const auto &entry) { return entry.second == 6; });
    }

    void flush(ProbeResult *report)
    {
        if (!report || !pending_) return;
        pending_->ordinal = report->units.size();
        pending_->timestampSamples = nextTimestampSamples_;
        unsigned maxBlocks = 0U;
        for (const auto &[key, blocks] : pending_->blocksByStream) {
            (void)key;
            maxBlocks = std::max(maxBlocks, blocks);
        }
        nextTimestampSamples_ += static_cast<std::int64_t>(maxBlocks) * 256;
        report->units.push_back(std::move(*pending_));
        pending_.reset();
    }

    bool consumeFrame(const std::vector<std::uint8_t> &chunk,
                      const Syncframe &frame,
                      const std::string &source,
                      ProbeResult *report)
    {
        const std::string key = streamKey(frame);
        if (pending_ && isComplete()) {
            const bool keyRepeats = std::any_of(pending_->frames.begin(),
                                                pending_->frames.end(),
                                                [&](const Syncframe &candidate) {
                                                    return sameKey(candidate, frame);
                                                });
            if (keyRepeats) {
                flush(report);
                if (stopRequested_) {
                    return true;
                }
            }
        }
        if (!pending_) {
            pending_.emplace();
            pending_->sampleRate = frame.sampleRate;
        }
        if (frame.sampleRate != pending_->sampleRate) {
            report->reason = source + ":unsupported-sample-rate-change";
            return false;
        }
        const unsigned previousBlocks = pending_->blocksByStream[key];
        if (previousBlocks + frame.blocks > 6) {
            report->reason = source + ":block-count-mismatch:" + key;
            return false;
        }
        Syncframe frameInUnit = frame;
        frameInUnit.unitOffset = pending_->compressedBytes.size();
        pending_->frames.push_back(frameInUnit);
        pending_->blocksByStream[key] = previousBlocks + frame.blocks;
        pending_->compressedBytes.insert(pending_->compressedBytes.end(),
                                         chunk.begin() + static_cast<std::ptrdiff_t>(frame.offset),
                                         chunk.begin() + static_cast<std::ptrdiff_t>(frame.offset + frame.sizeBytes));
        return true;
    }

    std::optional<AccessUnit> pending_;
    bool stopRequested_ = false;
    std::int64_t nextTimestampSamples_ = 0;
};

std::string errorText(int error)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(error, buffer, sizeof(buffer));
    return buffer;
}

bool isContainerPath(const std::string &path)
{
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) {
        return false;
    }
    const std::string extension = path.substr(dot + 1);
    return extension == "m4a" || extension == "mp4" || extension == "mka" || extension == "mkv";
}

bool loadChunks(const std::string &path,
                bool demuxRaw,
                int requestedAudioStream,
                std::vector<Chunk> *chunks,
                std::string *reason)
{
    if (!chunks || !reason) {
        return false;
    }
    if (!isContainerPath(path) && !demuxRaw) {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            *reason = "open-failed";
            return false;
        }
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)),
                                        std::istreambuf_iterator<char>());
        const std::size_t dot = path.find_last_of('.');
        bool isEb3 = dot != std::string::npos && dot + 1U < path.size();
        if (isEb3) {
            const std::string extension = path.substr(dot + 1U);
            isEb3 = extension.size() == 3U
                && std::tolower(static_cast<unsigned char>(extension[0])) == 'e'
                && std::tolower(static_cast<unsigned char>(extension[1])) == 'b'
                && std::tolower(static_cast<unsigned char>(extension[2])) == '3';
        }
        if (isEb3) {
            const eac3native::Eb3FramingResult framing =
                eac3native::normalizeEb3Framing(bytes);
            if (!framing.ok) {
                *reason = std::string("eb3-framing:") + framing.reason;
                return false;
            }
            bytes = framing.normalized;
        }
        chunks->push_back(Chunk {path, std::move(bytes)});
        return !chunks->back().bytes.empty();
    }

    AVFormatContext *format = nullptr;
    const AVInputFormat *inputFormat = demuxRaw && !isContainerPath(path)
        ? av_find_input_format("eac3")
        : nullptr;
    int result = avformat_open_input(&format, path.c_str(), inputFormat, nullptr);
    if (result < 0) {
        *reason = "avformat_open_input:" + errorText(result);
        return false;
    }
    result = avformat_find_stream_info(format, nullptr);
    if (result < 0) {
        *reason = "avformat_find_stream_info:" + errorText(result);
        avformat_close_input(&format);
        return false;
    }
    const AVCodec *codec = nullptr;
    int streamIndex = requestedAudioStream;
    if (streamIndex < 0) {
        streamIndex = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO,
                                          -1, -1, &codec, 0);
    } else if (streamIndex >= static_cast<int>(format->nb_streams)
               || format->streams[streamIndex]->codecpar->codec_type
                    != AVMEDIA_TYPE_AUDIO
               || format->streams[streamIndex]->codecpar->codec_id
                    != AV_CODEC_ID_EAC3) {
        streamIndex = AVERROR_STREAM_NOT_FOUND;
    } else {
        codec = avcodec_find_decoder(
            format->streams[streamIndex]->codecpar->codec_id);
    }
    if (streamIndex < 0 || !codec) {
        *reason = "no-audio-stream:" + errorText(streamIndex);
        avformat_close_input(&format);
        return false;
    }
    AVPacket *packet = av_packet_alloc();
    if (!packet) {
        *reason = "packet-allocation-failed";
        avformat_close_input(&format);
        return false;
    }
    while ((result = av_read_frame(format, packet)) >= 0) {
        if (packet->stream_index == streamIndex && packet->size > 0) {
            Chunk chunk;
            chunk.source = path + ":packet" + std::to_string(chunks->size());
            chunk.bytes.assign(packet->data, packet->data + packet->size);
            std::size_t skipDataSize = 0;
            const std::uint8_t *skipData = av_packet_get_side_data(packet,
                                                                    AV_PKT_DATA_SKIP_SAMPLES,
                                                                    &skipDataSize);
            if (skipData && skipDataSize >= 8) {
                chunk.skipSamples = static_cast<std::int64_t>(skipData[0])
                    | (static_cast<std::int64_t>(skipData[1]) << 8)
                    | (static_cast<std::int64_t>(skipData[2]) << 16)
                    | (static_cast<std::int64_t>(skipData[3]) << 24);
                chunk.discardPadding = static_cast<std::int64_t>(skipData[4])
                    | (static_cast<std::int64_t>(skipData[5]) << 8)
                    | (static_cast<std::int64_t>(skipData[6]) << 16)
                    | (static_cast<std::int64_t>(skipData[7]) << 24);
            }
            chunks->push_back(std::move(chunk));
        }
        av_packet_unref(packet);
    }
    av_packet_free(&packet);
    avformat_close_input(&format);
    if (result != AVERROR_EOF) {
        *reason = "av_read_frame:" + errorText(result);
        return false;
    }
    return !chunks->empty();
}

struct PcmReport {
    bool ok = false;
    std::string reason;
    std::size_t decodedFrames = 0;
    std::int64_t decodedSamples = 0;
    int firstFrameSamples = 0;
    int sampleRate = 0;
    int channels = 0;
    int format = AV_SAMPLE_FMT_NONE;
    std::string layout;
    bool nativeFormatPass = false;
    bool sampleCountPass = false;
    bool oneToOnePass = false;
};

struct Eac3DecodeOptions {
    double drcScale = 1.0;
    bool consistentNoiseGeneration = false;
};

struct OamdPayloadRecord {
    std::size_t unitIndex = 0;
    eac3oamd::B1Frame frame;
};

struct OamdB1Metrics {
    std::size_t payloadCount = 0;
    std::size_t passCount = 0;
    std::size_t unsupportedCount = 0;
    std::size_t malformedCount = 0;
    std::size_t unitAssociationCount = 0;
    std::size_t multiplePayloadUnits = 0;
    unsigned versionMin = std::numeric_limits<unsigned>::max();
    unsigned versionMax = 0;
    unsigned objectCountMin = std::numeric_limits<unsigned>::max();
    unsigned objectCountMax = 0;
    std::size_t lfeDeclaredCount = 0;
    std::size_t finalPaddingBits = 0;
    std::size_t recognizedElementCount = 0;
    std::size_t unknownDiscardableCount = 0;
    std::size_t unknownNondiscardableCount = 0;
    std::map<std::string, std::size_t> programTypes;
    std::map<unsigned, std::size_t> elementIdCounts;
    std::map<std::pair<unsigned, unsigned>, std::size_t> elementSizeCounts;
};

struct OamdB2aMetrics {
    eac3oamd::B2aBitOrder bitOrder = eac3oamd::B2aBitOrder::Syntax5511Lsb;
    std::size_t payloadCount = 0;
    std::size_t passCount = 0;
    std::size_t unsupportedCount = 0;
    std::size_t malformedCount = 0;
    std::size_t objectElementCount = 0;
    std::size_t unitAssociationCount = 0;
    std::size_t multipleObjectElements = 0;
    std::size_t objectInfoRecordCount = 0;
    std::size_t activeCount = 0;
    std::size_t inactiveCount = 0;
    std::size_t lfeHelperMapCount = 0;
    std::size_t additionalDataCount = 0;
    std::size_t additionalDataBytes = 0;
    std::size_t bitsConsumed = 0;
    std::size_t paddingBits = 0;
    std::map<unsigned, std::size_t> sampleOffsetCodes;
    std::map<unsigned, std::size_t> blockCounts;
    std::map<unsigned, std::size_t> blockOffsetFactors;
    std::map<unsigned, std::size_t> rampDurationCodes;
    std::map<unsigned, std::size_t> basicStatuses;
    std::map<unsigned, std::size_t> renderStatuses;
    std::map<unsigned, std::size_t> positionModes;
    std::map<unsigned, std::size_t> distanceModes;
    std::map<unsigned, std::size_t> sizeIndices;
    std::map<unsigned, std::size_t> screenModes;
    std::map<unsigned, std::size_t> snapValues;
    std::map<unsigned, std::size_t> activeByObject;
    std::map<unsigned, std::size_t> inactiveByObject;
    std::string firstFailureReason;
    std::size_t firstFailureBitOffset = 0;
    unsigned firstFailureObjectIndex = 0;
    unsigned firstFailureBlockIndex = 0;
};

struct OamdB2aHelperScanResult {
    unsigned helperObjectIndex = 0;
    eac3oamd::B2aBitOrder bitOrder = eac3oamd::B2aBitOrder::Syntax5511Lsb;
    eac3oamd::B2aDisposition disposition = eac3oamd::B2aDisposition::Malformed;
    std::string reason;
    std::size_t bitsConsumed = 0;
    std::size_t paddingBits = 0;
    std::size_t failureBitOffset = 0;
    unsigned failureObjectIndex = 0;
    unsigned failureBlockIndex = 0;
};

struct OamdB2bMetrics {
    std::size_t payloadCount = 0;
    std::size_t stateAppliedCount = 0;
    std::size_t fallbackCount = 0;
    std::size_t resetCount = 0;
    std::size_t defaultBasicCount = 0;
    std::size_t fullBasicCount = 0;
    std::size_t reuseBasicCount = 0;
    std::size_t mixedBasicCount = 0;
    std::size_t defaultRenderCount = 0;
    std::size_t fullRenderCount = 0;
    std::size_t reuseRenderCount = 0;
    std::size_t mixedRenderCount = 0;
    std::size_t absolutePositionCount = 0;
    std::size_t differentialPositionCount = 0;
    std::size_t activeCount = 0;
    std::size_t inactiveCount = 0;
    std::size_t finitePropertyCount = 0;
    std::size_t minusInfinityGainCount = 0;
    std::size_t lfeHelperCount = 0;
    std::size_t trimElementCount = 0;
    std::size_t extendedElementCount = 0;
    std::size_t additionalElementPassCount = 0;
    std::size_t additionalElementFailureCount = 0;
    std::size_t dynamicObjectCountMin = std::numeric_limits<std::size_t>::max();
    std::size_t dynamicObjectCountMax = 0;
    std::size_t dynamicObjectOrderPassCount = 0;
    std::string firstFailureReason;
};

struct Gate6cMetadataRecord {
    std::size_t unitIndex = 0;
    std::vector<eac3gate6c::MetadataUpdate> updates;
};

struct JocPayloadRecord {
    std::size_t unitIndex = 0;
    eac3joc::FrameReport report;
};

struct NativePcmCapture {
    bool ok = false;
    std::string reason;
    std::size_t decodedFrames = 0;
    std::int64_t decodedSamples = 0;
    int firstFrameSamples = 0;
    bool allFrames1536 = true;
    int sampleRate = 0;
    int channels = 0;
    int format = AV_SAMPLE_FMT_NONE;
    std::string layout;
    std::size_t layoutMismatchFrames = 0;
    std::vector<AVChannel> channelOrder;
    std::vector<std::vector<float>> planar;
};

struct PairingConfigMetrics {
    std::size_t found = 0;
    std::size_t paired = 0;
    std::size_t reconstructed = 0;
    std::size_t objectCountMin = 0;
    std::size_t objectCountMax = 0;
    std::size_t objectCountDrift = 0;
    std::size_t presentObjectFrames = 0;
    std::size_t objectQmfValueCount = 0;
    std::size_t objectQmfFiniteCount = 0;
    std::size_t lfeFiniteCount = 0;
    float lfePeak = 0.0f;
    std::size_t qmfResets = 0;
    std::size_t mathFailures = 0;
    std::size_t reconstructFailures = 0;
    std::size_t sequenceDiscontinuities = 0;
    std::size_t maxContinuousRun = 0;
    std::size_t layoutMismatch = 0;
    std::size_t primingSkipped = 0;
    std::size_t tailIncomplete = 0;
    std::size_t fallback = 0;
    std::int64_t pairedSamples = 0;
    std::int64_t lfeSamples = 0;
};

struct Gate6cRuntimeMetrics {
    std::size_t associatedUnits = 0;
    std::size_t batches = 0;
    std::size_t flushes = 0;
    std::size_t metadataApplied = 0;
    std::size_t metadataUpdates = 0;
    std::size_t objectSamples = 0;
    std::size_t lfeSamples = 0;
    std::size_t finiteObjectSamples = 0;
    std::size_t finiteLfeSamples = 0;
    std::size_t fallback = 0;
    std::size_t metadataOrderFailures = 0;
    std::size_t continuityFailures = 0;
    std::int64_t outputSamples = 0;
    std::int64_t firstOutputPosition = -1;
    std::int64_t lastOutputPosition = -1;
    std::int64_t lastOutputEnd = -1;
    float objectMin = std::numeric_limits<float>::infinity();
    float objectMax = -std::numeric_limits<float>::infinity();
    float objectPeak = 0.0f;
    float lfeMin = std::numeric_limits<float>::infinity();
    float lfeMax = -std::numeric_limits<float>::infinity();
    float lfePeak = 0.0f;
};

struct Gate7bRuntimeMetrics {
    std::size_t callbackBatches = 0;
    std::size_t flushBatches = 0;
    std::size_t metadataBatches = 0;
    std::size_t failedBatches = 0;
    std::string firstFailureReason;
};

std::string channelLayoutText(const AVChannelLayout &layout)
{
    char buffer[256] = {};
    if (av_channel_layout_describe(&layout, buffer, sizeof(buffer)) < 0) {
        return "unknown";
    }
    return buffer;
}

bool decodeNativePcmForPairing(const std::string &path,
                               const std::vector<Chunk> &chunks,
                               std::size_t chunkCount,
                               bool applyTrimMetadata,
                               const Eac3DecodeOptions &decodeOptions,
                               NativePcmCapture *capture)
{
    if (!capture || chunkCount == 0 || chunks.empty()) {
        return false;
    }
    AVFormatContext *format = nullptr;
    const AVInputFormat *inputFormat = isContainerPath(path) ? nullptr : av_find_input_format("eac3");
    int result = avformat_open_input(&format, path.c_str(), inputFormat, nullptr);
    if (result < 0) {
        capture->reason = "avformat_open_input:" + errorText(result);
        return false;
    }
    result = avformat_find_stream_info(format, nullptr);
    if (result < 0) {
        capture->reason = "avformat_find_stream_info:" + errorText(result);
        avformat_close_input(&format);
        return false;
    }
    const AVCodec *codec = nullptr;
    const int streamIndex = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    AVCodecContext *decoder = streamIndex >= 0 && codec ? avcodec_alloc_context3(codec) : nullptr;
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    if (streamIndex < 0 || !codec || !decoder || !packet || !frame) {
        capture->reason = "decoder-allocation-or-stream-failed";
        avcodec_free_context(&decoder);
        av_packet_free(&packet);
        av_frame_free(&frame);
        avformat_close_input(&format);
        return false;
    }
    result = avcodec_parameters_to_context(decoder, format->streams[streamIndex]->codecpar);
    if (result >= 0) {
        AVDictionary *decoderOptions = nullptr;
        const std::string drcScale = std::to_string(decodeOptions.drcScale);
        av_dict_set(&decoderOptions, "drc_scale", drcScale.c_str(), 0);
        av_dict_set(&decoderOptions, "cons_noisegen",
                    decodeOptions.consistentNoiseGeneration ? "1" : "0", 0);
        result = avcodec_open2(decoder, codec, &decoderOptions);
        av_dict_free(&decoderOptions);
    }
    if (result < 0) {
        capture->reason = "avcodec_open2:" + errorText(result);
        avcodec_free_context(&decoder);
        av_packet_free(&packet);
        av_frame_free(&frame);
        avformat_close_input(&format);
        return false;
    }
    const std::size_t inputChunkCount = std::min(chunks.size(), chunkCount);
    auto receive = [&]() -> bool {
        while (true) {
            result = avcodec_receive_frame(decoder, frame);
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
                return true;
            }
            if (result < 0) {
                capture->reason = "avcodec_receive_frame:" + errorText(result);
                return false;
            }
            const bool firstFrame = capture->decodedFrames == 0;
            if (frame->nb_samples != 1536) {
                capture->allFrames1536 = false;
            }
            if (firstFrame) {
                capture->firstFrameSamples = frame->nb_samples;
                capture->sampleRate = frame->sample_rate;
                capture->channels = frame->ch_layout.nb_channels;
                capture->format = frame->format;
                capture->layout = channelLayoutText(frame->ch_layout);
                capture->channelOrder.resize(frame->ch_layout.nb_channels);
                for (unsigned channel = 0; channel < frame->ch_layout.nb_channels; ++channel) {
                    capture->channelOrder[channel]
                        = av_channel_layout_channel_from_index(&frame->ch_layout, channel);
                }
                capture->planar.assign(frame->ch_layout.nb_channels, {});
            } else {
                bool sameLayout = frame->ch_layout.nb_channels == capture->channelOrder.size();
                if (sameLayout) {
                    for (unsigned channel = 0; channel < capture->channelOrder.size(); ++channel) {
                        if (av_channel_layout_channel_from_index(&frame->ch_layout, channel)
                            != capture->channelOrder[channel]) {
                            sameLayout = false;
                            break;
                        }
                    }
                }
                if (!sameLayout) {
                    ++capture->layoutMismatchFrames;
                    av_frame_unref(frame);
                    continue;
                }
            }
            if (capture->sampleRate != 48000 || capture->format != AV_SAMPLE_FMT_FLTP
                || frame->sample_rate != capture->sampleRate
                || frame->format != capture->format
                || frame->ch_layout.nb_channels != capture->channels) {
                capture->reason = "native-pcm-requires-48k-fltp-matching-layout";
                av_frame_unref(frame);
                return false;
            }
            for (unsigned channel = 0; channel < capture->planar.size(); ++channel) {
                const float *samples = reinterpret_cast<const float *>(frame->extended_data[channel]);
                capture->planar[channel].insert(capture->planar[channel].end(),
                                                samples, samples + frame->nb_samples);
            }
            ++capture->decodedFrames;
            capture->decodedSamples += frame->nb_samples;
            av_frame_unref(frame);
        }
    };

    for (std::size_t index = 0; index < inputChunkCount; ++index) {
        if (av_new_packet(packet, static_cast<int>(chunks[index].bytes.size())) < 0) {
            capture->reason = "packet-allocation-failed";
            break;
        }
        std::copy(chunks[index].bytes.begin(), chunks[index].bytes.end(), packet->data);
        if (applyTrimMetadata
            && (chunks[index].skipSamples != 0 || chunks[index].discardPadding != 0)) {
            std::uint8_t *skipData = av_packet_new_side_data(packet, AV_PKT_DATA_SKIP_SAMPLES, 10);
            if (!skipData) {
                capture->reason = "skip-sample-side-data-allocation-failed";
                av_packet_unref(packet);
                break;
            }
            const std::uint32_t skip = static_cast<std::uint32_t>(chunks[index].skipSamples);
            const std::uint32_t discard = static_cast<std::uint32_t>(chunks[index].discardPadding);
            for (unsigned byte = 0; byte < 4; ++byte) {
                skipData[byte] = static_cast<std::uint8_t>(skip >> (byte * 8U));
                skipData[4U + byte] = static_cast<std::uint8_t>(discard >> (byte * 8U));
            }
            skipData[8] = 0;
            skipData[9] = 0;
        }
        result = avcodec_send_packet(decoder, packet);
        av_packet_unref(packet);
        if (result < 0 && result != AVERROR(EAGAIN)) {
            capture->reason = "avcodec_send_packet:" + errorText(result);
            break;
        }
        if (!receive()) {
            break;
        }
    }
    if (capture->reason.empty()) {
        result = avcodec_send_packet(decoder, nullptr);
        if (result >= 0 || result == AVERROR_EOF) {
            receive();
        } else {
            capture->reason = "avcodec_flush:" + errorText(result);
        }
    }
    capture->ok = capture->reason.empty() && capture->sampleRate == 48000
        && capture->format == AV_SAMPLE_FMT_FLTP && !capture->planar.empty();
    avcodec_free_context(&decoder);
    av_packet_free(&packet);
    av_frame_free(&frame);
    avformat_close_input(&format);
    return capture->ok;
}

bool avChannelForIdentity(eac3joc::ChannelIdentity identity, AVChannel *channel)
{
    if (!channel) {
        return false;
    }
    switch (identity) {
    case eac3joc::ChannelIdentity::FrontLeft:
        *channel = AV_CHAN_FRONT_LEFT;
        return true;
    case eac3joc::ChannelIdentity::FrontRight:
        *channel = AV_CHAN_FRONT_RIGHT;
        return true;
    case eac3joc::ChannelIdentity::FrontCenter:
        *channel = AV_CHAN_FRONT_CENTER;
        return true;
    case eac3joc::ChannelIdentity::SideLeft:
        *channel = AV_CHAN_SIDE_LEFT;
        return true;
    case eac3joc::ChannelIdentity::SideRight:
        *channel = AV_CHAN_SIDE_RIGHT;
        return true;
    case eac3joc::ChannelIdentity::BackLeft:
        *channel = AV_CHAN_BACK_LEFT;
        return true;
    case eac3joc::ChannelIdentity::BackRight:
        *channel = AV_CHAN_BACK_RIGHT;
        return true;
    case eac3joc::ChannelIdentity::TopFrontLeft:
        *channel = AV_CHAN_TOP_FRONT_LEFT;
        return true;
    case eac3joc::ChannelIdentity::TopFrontRight:
        *channel = AV_CHAN_TOP_FRONT_RIGHT;
        return true;
    case eac3joc::ChannelIdentity::LowFrequency:
        *channel = AV_CHAN_LOW_FREQUENCY;
        return true;
    }
    return false;
}

bool expectedChannelsForConfig(unsigned config,
                               std::vector<AVChannel> *channels,
                               std::string *reason)
{
    if (!channels || !reason) {
        return false;
    }
    channels->clear();
    const auto &identities = eac3joc::inputIdentities(config);
    if (identities.empty()) {
        *reason = "unknown-or-reserved-downmix-config";
        return false;
    }
    for (const eac3joc::ChannelIdentity identity : identities) {
        AVChannel channel = AV_CHAN_NONE;
        if (!avChannelForIdentity(identity, &channel)) {
            *reason = "unmapped-joc-channel-identity";
            channels->clear();
            return false;
        }
        channels->push_back(channel);
    }
    AVChannel lfe = AV_CHAN_NONE;
    if (!avChannelForIdentity(eac3joc::ChannelIdentity::LowFrequency, &lfe)) {
        *reason = "unmapped-joc-lfe-identity";
        channels->clear();
        return false;
    }
    channels->push_back(lfe);
    return true;
}

std::size_t channelIndex(const std::vector<AVChannel> &channels, AVChannel identity)
{
    const auto found = std::find(channels.begin(), channels.end(), identity);
    return found == channels.end() ? channels.size()
                                   : static_cast<std::size_t>(found - channels.begin());
}

bool runNativePcmPairing(const std::string &path,
                         const std::vector<Chunk> &chunks,
                         std::size_t chunkCount,
                         const std::vector<AccessUnit> &units,
                         const std::vector<JocPayloadRecord> &records,
                         const std::vector<double> &qwin,
                         const std::vector<Gate6cMetadataRecord> &gate6cMetadata,
                         bool gate6c,
                         bool gate7b,
                         bool gate7c,
                         const eac3renderer::Options &rendererOptions,
                         const Eac3DecodeOptions &decodeOptions,
                         const std::string &bearExportPath,
                         const std::string &jocMatrixTracePath)
{
    std::cout << "eac3DecodeDrcScale=" << decodeOptions.drcScale
              << " eac3DecodeConsNoisegen="
              << (decodeOptions.consistentNoiseGeneration ? 1 : 0) << '\n';
    NativePcmCapture trimmedCapture;
    NativePcmCapture codedCapture;
    const bool trimmedDecodePass = decodeNativePcmForPairing(
        path, chunks, chunkCount, true, decodeOptions, &trimmedCapture);
    const bool codedDecodePass = decodeNativePcmForPairing(
        path, chunks, chunkCount, false, decodeOptions, &codedCapture);
    // Object-QMF pairing must use the untrimmed coded-domain stream. Container
    // trim metadata remains an output-policy record for a later synthesis or
    // renderer stage.
    NativePcmCapture &capture = codedCapture;
    const bool decodePass = codedDecodePass;
    std::map<unsigned, PairingConfigMetrics> metrics;
    std::map<unsigned, std::size_t> currentContinuousRun;
    std::vector<std::size_t> recordsPerUnit(units.size(), 0);
    bool multiplePayloadStop = false;
    for (const JocPayloadRecord &record : records) {
        if (record.unitIndex >= recordsPerUnit.size()) {
            continue;
        }
        ++recordsPerUnit[record.unitIndex];
        if (recordsPerUnit[record.unitIndex] > 1) {
            multiplePayloadStop = true;
        }
    }

    std::int64_t compressedSamples = static_cast<std::int64_t>(units.size()) * 1536;
    std::int64_t skipSamples = 0;
    std::int64_t discardPadding = 0;
    const std::int64_t gate6cDiscardPadding = 0;
    bool nonInitialSkip = false;
    bool nonTerminalDiscard = false;
    const std::size_t inputChunkCount = std::min(chunks.size(), chunkCount);
    for (std::size_t index = 0; index < inputChunkCount; ++index) {
        skipSamples += chunks[index].skipSamples;
        discardPadding += chunks[index].discardPadding;
        if (chunks[index].skipSamples != 0 && index != 0) {
            nonInitialSkip = true;
        }
        if (chunks[index].discardPadding != 0 && index + 1 != inputChunkCount) {
            nonTerminalDiscard = true;
        }
    }
    const std::int64_t gate6cExpectedOutputSamples = std::max<std::int64_t>(
        0, compressedSamples - skipSamples);
    const std::int64_t nativeTimelineStart = 0;
    const std::int64_t nativeTimelineEnd = compressedSamples;
    const std::int64_t nativeAvailableEnd = capture.decodedSamples;
    const bool timelinePass = codedDecodePass && !nonInitialSkip && !nonTerminalDiscard
        && nativeAvailableEnd >= nativeTimelineEnd && capture.allFrames1536;
    const std::int64_t trimmedTimelineStart = skipSamples;
    const std::int64_t trimmedTimelineEnd = std::max<std::int64_t>(
        trimmedTimelineStart, compressedSamples - discardPadding);
    const bool trimmedTimelinePass = trimmedDecodePass && !nonInitialSkip && !nonTerminalDiscard
        && trimmedCapture.decodedSamples + trimmedTimelineStart >= trimmedTimelineEnd;

    Gate6cRuntimeMetrics gate6cMetrics;
    std::vector<const Gate6cMetadataRecord *> metadataByUnit(units.size(), nullptr);
    std::size_t expectedMetadataEmitted = 0;
    std::array<bool, eac3gate6c::kDynamicObjectCount + 1U> boundaryMetadata {};
    std::map<unsigned, std::size_t> metadataRampDurations;
    for (const Gate6cMetadataRecord &record : gate6cMetadata) {
        if (record.unitIndex < metadataByUnit.size()) {
            metadataByUnit[record.unitIndex] = &record;
        }
        gate6cMetrics.metadataApplied += record.updates.size();
        for (const eac3gate6c::MetadataUpdate &update : record.updates) {
            ++metadataRampDurations[update.rampDuration];
            const std::int64_t sourcePosition = update.sourcePosition;
            if (sourcePosition >= skipSamples
                && sourcePosition < compressedSamples - gate6cDiscardPadding) {
                ++expectedMetadataEmitted;
            } else if (sourcePosition < skipSamples
                       && update.objectIndex <= eac3gate6c::kDynamicObjectCount) {
                boundaryMetadata[update.objectIndex] = true;
            }
        }
    }
    expectedMetadataEmitted += static_cast<std::size_t>(std::count(
        boundaryMetadata.begin() + 1, boundaryMetadata.end(), true));
    eac3jocsession::Config sessionConfig;
    sessionConfig.qwin = qwin;
    sessionConfig.decodedSourceSamples = compressedSamples;
    sessionConfig.skipSamples = skipSamples;
    // The max-unit diagnostic is a partial coded prefix; do not invent
    // terminal discard padding from decoder lookahead.
    sessionConfig.discardPadding = gate6cDiscardPadding;
    sessionConfig.callbackCapacity = 1U;
    sessionConfig.emitGate6c = gate6c;
    std::unique_ptr<eac3jocsession::Session> jocSession =
        std::make_unique<eac3jocsession::Session>(std::move(sessionConfig));
    std::unique_ptr<eac3joctrace::Writer> matrixTrace;
    if (!jocMatrixTracePath.empty()) {
        if (!gate6c || !codedDecodePass) {
            std::cout << "jocMatrixTrace=FAIL reason=requires-pcm-and-joc-gate6c-and-coded-decode\n";
            return false;
        }
        std::string traceReason;
        if (!eac3joctrace::runSelfTest(&traceReason)) {
            std::cout << "jocMatrixTrace=FAIL reason=self-test-" << traceReason << '\n';
            return false;
        }
        std::cout << "jocMatrixTraceSelfTest=PASS reason=" << traceReason << '\n';
        matrixTrace = std::make_unique<eac3joctrace::Writer>();
        if (!matrixTrace->open(jocMatrixTracePath, path, qwin, &traceReason)) {
            std::cout << "jocMatrixTrace=FAIL reason=" << traceReason << '\n';
            return false;
        }
    }
    eac3gate7b::Geometry gate7bGeometry = eac3gate7b::referenceGeometry();
    gate7bGeometry.positionRadiusMode = rendererOptions.positionRadiusMode;
    gate7bGeometry.positionDirectionMode = rendererOptions.positionDirectionMode;
    gate7bGeometry.azimuthFocus = rendererOptions.azimuthFocus;
    eac3gate7b::Adapter gate7bAdapter(gate7bGeometry);
    std::unique_ptr<eac3bear::Exporter> bearExporter;
    if (!bearExportPath.empty()) {
        bearExporter = std::make_unique<eac3bear::Exporter>(bearExportPath, path);
        std::string exportReason;
        if (!bearExporter->open(&exportReason)) {
            std::cout << "bearExport=FAIL reason=" << exportReason << '\n';
            return false;
        }
    }
    Gate7bRuntimeMetrics gate7bMetrics;
    std::unique_ptr<eac3renderer::SpatialBridgeRenderer> renderer;
    std::chrono::steady_clock::time_point gate7cProducerStart;
    if (gate7c) {
        renderer = std::make_unique<eac3renderer::SpatialBridgeRenderer>(rendererOptions);
        if (!renderer->start()) {
            const eac3renderer::Metrics result = renderer->result();
            std::cout << "gate7cResult=" << eac3renderer::outcomeText(result.outcome)
                      << " stage=" << (result.firstFailureStage.empty()
                                             ? "consumerInitialization"
                                             : result.firstFailureStage)
                      << " evidenceLimit=diagnostic-spatial-endpoint-submission;manual-listening-or-loopback-required\n";
            return false;
        }
        gate7cProducerStart = std::chrono::steady_clock::now();
    }
    const auto applyGate7bMetadata = [&](const eac3gate6c::Batch &batch) {
        if (!gate7b || batch.metadata.empty()) return true;
        ++gate7bMetrics.metadataBatches;
        std::vector<eac3gate7b::Update> updates;
        updates.reserve(batch.metadata.size());
        for (const eac3gate6c::MetadataUpdate &update : batch.metadata) {
            updates.push_back(eac3gate7b::Update {
                update.sourcePosition,
                update.blockIndex,
                update.rampDuration,
                update.objectIndex,
                update.state});
        }
        const eac3gate7b::ApplyResult propertyResult = gate7bAdapter.applyBatch(updates);
        if (propertyResult.disposition == eac3gate7b::Disposition::Pass) return true;
        ++gate7bMetrics.failedBatches;
        if (gate7bMetrics.firstFailureReason.empty()) {
            gate7bMetrics.firstFailureReason = propertyResult.reason;
        }
        return false;
    };

    auto resetStates = [&](unsigned config) {
        currentContinuousRun[config] = 0;
        jocSession->reset();
    };
    const auto accountGate6cOutput = [&gate6cMetrics](
        const eac3gate6c::Batch &batch, bool expectFlush) {
        if (batch.flush != expectFlush
            || batch.objects.size() != eac3gate6c::kDynamicObjectCount
            || batch.lfe.empty()) {
            return false;
        }
        const std::size_t samples = batch.lfe.size();
        if (batch.outputEnd < batch.outputStart
            || batch.outputEnd - batch.outputStart
                != static_cast<std::int64_t>(samples)) {
            ++gate6cMetrics.continuityFailures;
            return false;
        }
        Gate6cRuntimeMetrics staged = gate6cMetrics;
        if (staged.batches == 0U) {
            staged.firstOutputPosition = batch.outputStart;
            if (batch.outputStart != 0) {
                ++gate6cMetrics.continuityFailures;
                return false;
            }
        } else if (batch.outputStart != staged.lastOutputEnd) {
            ++gate6cMetrics.continuityFailures;
            return false;
        }
        for (const auto &object : batch.objects) {
            if (object.size() != samples) return false;
            staged.objectSamples += object.size();
            for (const float value : object) {
                if (!std::isfinite(value)) return false;
                staged.finiteObjectSamples++;
                staged.objectMin = std::min(staged.objectMin, value);
                staged.objectMax = std::max(staged.objectMax, value);
                staged.objectPeak = std::max(staged.objectPeak, std::abs(value));
            }
        }
        staged.lfeSamples += samples;
        for (const float value : batch.lfe) {
            if (!std::isfinite(value)) return false;
            staged.finiteLfeSamples++;
            staged.lfeMin = std::min(staged.lfeMin, value);
            staged.lfeMax = std::max(staged.lfeMax, value);
            staged.lfePeak = std::max(staged.lfePeak, std::abs(value));
        }
        staged.lastOutputPosition = batch.outputStart;
        staged.lastOutputEnd = batch.outputEnd;
        staged.outputSamples += static_cast<std::int64_t>(samples);
        ++staged.batches;
        if (expectFlush) ++staged.flushes;
        gate6cMetrics = staged;
        return true;
    };
    std::size_t previousUnitIndex = 0;
    bool havePreviousUnit = false;
    for (const JocPayloadRecord &record : records) {
        const eac3joc::FrameReport &jocReport = record.report;
        if (jocReport.disposition == eac3joc::ParseDisposition::Malformed) {
            ++metrics[jocReport.downmixConfigIndex].fallback;
            resetStates(jocReport.downmixConfigIndex);
            continue;
        }
        PairingConfigMetrics &configMetrics = metrics[jocReport.downmixConfigIndex];
        ++configMetrics.found;
        if (havePreviousUnit && record.unitIndex != previousUnitIndex + 1) {
            ++configMetrics.fallback;
            resetStates(jocReport.downmixConfigIndex);
            continue;
        }
        previousUnitIndex = record.unitIndex;
        havePreviousUnit = true;
        if (jocReport.downmixConfigIndex != 3 && jocReport.downmixConfigIndex != 4) {
            ++configMetrics.fallback;
            resetStates(jocReport.downmixConfigIndex);
            continue;
        }
        std::vector<AVChannel> expected;
        std::string expectedReason;
        if (!expectedChannelsForConfig(jocReport.downmixConfigIndex,
                                       &expected, &expectedReason)) {
            ++configMetrics.layoutMismatch;
            ++configMetrics.fallback;
            resetStates(jocReport.downmixConfigIndex);
            continue;
        }
        const bool layoutMatches = decodePass && capture.layoutMismatchFrames == 0
            && capture.channelOrder.size() == expected.size()
            && std::all_of(expected.begin(), expected.end(), [&](AVChannel identity) {
                   return channelIndex(capture.channelOrder, identity) < capture.channelOrder.size();
               });
        if (!layoutMatches) {
            ++configMetrics.layoutMismatch;
            ++configMetrics.fallback;
            resetStates(jocReport.downmixConfigIndex);
            continue;
        }
        const std::int64_t unitStart = static_cast<std::int64_t>(record.unitIndex) * 1536;
        const std::int64_t unitEnd = unitStart + 1536;
        if (unitStart < nativeTimelineStart || unitEnd <= nativeTimelineStart) {
            ++configMetrics.primingSkipped;
            ++configMetrics.fallback;
            resetStates(jocReport.downmixConfigIndex);
            continue;
        }
        if (unitEnd > nativeTimelineEnd || unitEnd > nativeAvailableEnd) {
            ++configMetrics.tailIncomplete;
            ++configMetrics.fallback;
            resetStates(jocReport.downmixConfigIndex);
            continue;
        }
        const std::int64_t outputOffset = unitStart - nativeTimelineStart;
        if (outputOffset < 0 || capture.planar.empty()
            || static_cast<std::size_t>(outputOffset + 1536) > capture.planar[0].size()) {
            ++configMetrics.tailIncomplete;
            ++configMetrics.fallback;
            resetStates(jocReport.downmixConfigIndex);
            continue;
        }
        ++configMetrics.paired;
        configMetrics.pairedSamples += 1536;
        const std::size_t lfeIndex = channelIndex(capture.channelOrder, expected.back());
        bool lfeFinite = lfeIndex < capture.planar.size();
        std::vector<float> lfe(1536, 0.0f);
        if (lfeFinite) {
            std::copy_n(capture.planar[lfeIndex].begin() + outputOffset, 1536, lfe.begin());
            for (const float sample : lfe) {
                if (std::isfinite(sample)) {
                    ++configMetrics.lfeFiniteCount;
                    configMetrics.lfePeak = std::max(configMetrics.lfePeak, std::abs(sample));
                } else {
                    lfeFinite = false;
                }
            }
            configMetrics.lfeSamples += 1536;
        }
        if (!lfeFinite) {
            ++configMetrics.fallback;
            resetStates(jocReport.downmixConfigIndex);
            continue;
        }

        std::vector<std::vector<float>> orderedPcm;
        orderedPcm.reserve(expected.size() - 1);
        for (std::size_t channel = 0; channel + 1 < expected.size(); ++channel) {
            const std::size_t sourceIndex = channelIndex(capture.channelOrder, expected[channel]);
            if (sourceIndex >= capture.planar.size()) {
                orderedPcm.clear();
                break;
            }
            orderedPcm.emplace_back(capture.planar[sourceIndex].begin() + outputOffset,
                                    capture.planar[sourceIndex].begin() + outputOffset + 1536);
        }
        if (orderedPcm.size() != expected.size() - 1) {
            ++configMetrics.fallback;
            resetStates(jocReport.downmixConfigIndex);
            continue;
        }

        eac3gate6c::Callback gate6cCallback;
        if (gate6c) {
            if (record.unitIndex >= metadataByUnit.size()
                || !metadataByUnit[record.unitIndex]) {
                ++gate6cMetrics.fallback;
                resetStates(jocReport.downmixConfigIndex);
                continue;
            }
            gate6cCallback =
                [&accountGate6cOutput, &gate6cMetrics, &gate7bAdapter,
                 &gate7bMetrics, &applyGate7bMetadata, gate7b, gate7c,
                 renderer = renderer.get(), bearExporter = bearExporter.get()](const eac3gate6c::Batch &batch) {
                    ++gate7bMetrics.callbackBatches;
                    if (batch.flush) {
                        ++gate7bMetrics.flushBatches;
                    }
                    if (gate7b && !applyGate7bMetadata(batch)) {
                        return false;
                    }
                    for (std::size_t index = 1; index < batch.metadata.size(); ++index) {
                        if (batch.metadata[index].sourcePosition
                            < batch.metadata[index - 1].sourcePosition) {
                            ++gate6cMetrics.metadataOrderFailures;
                            return false;
                        }
                    }
                    if (!accountGate6cOutput(batch, false)) return false;
                    if (bearExporter) {
                        std::string exportReason;
                        if (!bearExporter->append(batch, &exportReason)) {
                            std::cerr << "bearExport=FAIL reason=" << exportReason << '\n';
                            return false;
                        }
                    }
                    if (gate7c && renderer
                        && renderer->submit(batch) != eac3bridge::QueueStatus::Pass) {
                        return false;
                    }
                    gate6cMetrics.metadataUpdates += batch.metadata.size();
                    return true;
                };
        }
        eac3jocsession::Input sessionInput;
        sessionInput.unitIndex = record.unitIndex;
        sessionInput.sourceStart = unitStart;
        sessionInput.joc = jocReport;
        sessionInput.orderedPcm = std::move(orderedPcm);
        sessionInput.lfe = std::move(lfe);
        if (gate6c) {
            sessionInput.metadata = metadataByUnit[record.unitIndex]->updates;
        }
        const eac3jocsession::ProcessResult sessionResult =
            jocSession->process(sessionInput, gate6cCallback);
        if (sessionResult.flow != eac3jocsession::FlowStatus::None) {
            std::cout << "jocSessionFlow="
                      << eac3jocsession::flowStatusText(sessionResult.flow)
                      << " reason=" << sessionResult.reason
                      << " stage=gate8a-1-diagnostic-stop\n";
            return false;
        }
        if (sessionResult.disposition != eac3jocsession::Disposition::Joc) {
            switch (sessionResult.stage) {
            case eac3jocsession::FailureStage::Sequence:
                ++configMetrics.sequenceDiscontinuities;
                break;
            case eac3jocsession::FailureStage::Qmf:
                ++configMetrics.reconstructFailures;
                break;
            case eac3jocsession::FailureStage::Gate6c:
                break;
            case eac3jocsession::FailureStage::Validation:
            case eac3jocsession::FailureStage::Math:
            case eac3jocsession::FailureStage::None:
                ++configMetrics.mathFailures;
                break;
            }
            if (gate6c) ++gate6cMetrics.fallback;
            ++configMetrics.fallback;
            resetStates(jocReport.downmixConfigIndex);
            continue;
        }
        const eac3joc::JocMathFrame &mathFrame = jocSession->lastMathFrame();
        const eac3joc::JocQmfFrame &qmfFrame = jocSession->lastQmfFrame();
        if (qmfFrame.stateReset) {
            ++configMetrics.qmfResets;
        }
        const std::size_t objectCount = qmfFrame.objects.size();
        if (configMetrics.reconstructed == 0) {
            configMetrics.objectCountMin = objectCount;
            configMetrics.objectCountMax = objectCount;
        } else if (objectCount != configMetrics.objectCountMin) {
            ++configMetrics.objectCountDrift;
        }
        configMetrics.objectCountMin = std::min(configMetrics.objectCountMin, objectCount);
        configMetrics.objectCountMax = std::max(configMetrics.objectCountMax, objectCount);
        std::size_t presentObjects = 0;
        bool objectFinite = true;
        for (std::size_t object = 0; object < qmfFrame.objects.size(); ++object) {
            if (object < mathFrame.interpolated.size() && mathFrame.interpolated[object].present) {
                ++presentObjects;
                configMetrics.objectQmfValueCount += qmfFrame.objects[object].size();
                for (const auto &value : qmfFrame.objects[object]) {
                    if (std::isfinite(value.real()) && std::isfinite(value.imag())) {
                        ++configMetrics.objectQmfFiniteCount;
                    } else {
                        objectFinite = false;
                    }
                }
            }
        }
        configMetrics.presentObjectFrames += presentObjects;
        if (!objectFinite) {
            ++configMetrics.fallback;
            resetStates(jocReport.downmixConfigIndex);
            continue;
        }
        if (matrixTrace) {
            std::string traceReason;
            if (!matrixTrace->append(record.unitIndex, unitStart, jocReport,
                                     mathFrame, qmfFrame, sessionInput.lfe,
                                     &traceReason)) {
                std::cout << "jocMatrixTrace=FAIL reason=" << traceReason << '\n';
                return false;
            }
        }
        if (gate6c) {
            ++gate6cMetrics.associatedUnits;
        }
        ++configMetrics.reconstructed;
        const std::size_t run = ++currentContinuousRun[jocReport.downmixConfigIndex];
        configMetrics.maxContinuousRun = std::max(configMetrics.maxContinuousRun, run);
    }

    bool gate6cFlushPass = true;
    bool gate7bPass = true;
    if (gate6c) {
        const eac3gate6c::Callback flushCallback =
            [&accountGate6cOutput, &gate6cMetrics, &gate7bMetrics, &applyGate7bMetadata,
             gate7b, gate7c, renderer = renderer.get(), bearExporter = bearExporter.get()](const eac3gate6c::Batch &batch) {
                if (gate7b) {
                    ++gate7bMetrics.callbackBatches;
                    ++gate7bMetrics.flushBatches;
                }
                if (gate7b && !applyGate7bMetadata(batch)) {
                    return false;
                }
                for (std::size_t index = 1; index < batch.metadata.size(); ++index) {
                    if (batch.metadata[index].sourcePosition
                        < batch.metadata[index - 1].sourcePosition) {
                        ++gate6cMetrics.metadataOrderFailures;
                        return false;
                    }
                }
                if (!accountGate6cOutput(batch, true)) return false;
                if (bearExporter) {
                    std::string exportReason;
                    if (!bearExporter->append(batch, &exportReason)) return false;
                }
                return !gate7c || !renderer
                    || renderer->submit(batch) == eac3bridge::QueueStatus::Pass;
            };
        const eac3jocsession::ProcessResult flushResult = jocSession->flush(flushCallback);
        if (flushResult.flow != eac3jocsession::FlowStatus::None) {
            std::cout << "jocSessionFlow="
                      << eac3jocsession::flowStatusText(flushResult.flow)
                      << " reason=" << flushResult.reason
                      << " stage=gate8a-1-diagnostic-stop\n";
            return false;
        }
        gate6cFlushPass = flushResult.disposition
            == eac3jocsession::Disposition::Joc;
        if (!gate6cFlushPass) ++gate6cMetrics.fallback;
    }

    if (gate6c) {
        const std::int64_t expectedOutputSamples = gate6cExpectedOutputSamples;
        const bool gate6cPass = gate6cFlushPass
            && gate6cMetrics.associatedUnits == units.size()
            && gate6cMetrics.fallback == 0U
            && gate6cMetrics.batches > 0U
            && gate6cMetrics.metadataApplied == units.size() * 15U
            && gate6cMetrics.metadataUpdates == expectedMetadataEmitted
            && gate6cMetrics.objectSamples == gate6cMetrics.finiteObjectSamples
            && gate6cMetrics.lfeSamples == gate6cMetrics.finiteLfeSamples
            && std::isfinite(gate6cMetrics.objectMin)
            && std::isfinite(gate6cMetrics.objectMax)
            && std::isfinite(gate6cMetrics.objectPeak)
            && std::isfinite(gate6cMetrics.lfeMin)
            && std::isfinite(gate6cMetrics.lfeMax)
            && std::isfinite(gate6cMetrics.lfePeak)
            && gate6cMetrics.metadataOrderFailures == 0U
            && gate6cMetrics.continuityFailures == 0U
            && gate6cMetrics.firstOutputPosition == 0
            && gate6cMetrics.lastOutputEnd == expectedOutputSamples
            && gate6cMetrics.outputSamples == expectedOutputSamples;
        std::cout << "gate6cTrimPolicy=deferred-common-aligned-interval\n"
                  << "gate6cCommonDelaySamples=" << eac3gate6c::kCommonDelaySamples << '\n'
                  << "gate6cSelectedStart=" << skipSamples + eac3gate6c::kCommonDelaySamples << '\n'
                  << "gate6cDiscardPadding=" << gate6cDiscardPadding
                  << " policy=partial-prefix-terminal-discard-not-invented\n"
                  << "gate6cSelectedEnd=" << compressedSamples - gate6cDiscardPadding
                      + eac3gate6c::kCommonDelaySamples << '\n'
                  << "gate6cAssociatedUnits=" << gate6cMetrics.associatedUnits << '\n'
                  << "gate6cBatches=" << gate6cMetrics.batches << '\n'
                  << "gate6cFlushes=" << gate6cMetrics.flushes << '\n'
                  << "gate6cMetadataApplied=" << gate6cMetrics.metadataApplied << '\n'
                  << "gate6cMetadataUpdates=" << gate6cMetrics.metadataUpdates << '\n'
                  << "gate6cExpectedMetadataEmitted=" << expectedMetadataEmitted << '\n'
                  << "gate6cMetadataRampDurations=";
        for (const auto &entry : metadataRampDurations) {
            if (entry.first != metadataRampDurations.begin()->first) std::cout << ',';
            std::cout << entry.first << ':' << entry.second;
        }
        std::cout << '\n'
                  << "gate6cDynamicObjectCount=15\n"
                  << "gate6cObjectSamples=" << gate6cMetrics.objectSamples << '\n'
                  << "gate6cObjectFiniteSamples=" << gate6cMetrics.finiteObjectSamples << '\n'
                  << "gate6cLfeSamples=" << gate6cMetrics.lfeSamples << '\n'
                  << "gate6cLfeFiniteSamples=" << gate6cMetrics.finiteLfeSamples << '\n'
                  << "gate6cObjectRange="
                  << (std::isfinite(gate6cMetrics.objectMin) ? gate6cMetrics.objectMin : 0.0f)
                  << ':'
                  << (std::isfinite(gate6cMetrics.objectMax) ? gate6cMetrics.objectMax : 0.0f)
                  << " peak=" << gate6cMetrics.objectPeak << '\n'
                  << "gate6cLfeRange="
                  << (std::isfinite(gate6cMetrics.lfeMin) ? gate6cMetrics.lfeMin : 0.0f)
                  << ':'
                  << (std::isfinite(gate6cMetrics.lfeMax) ? gate6cMetrics.lfeMax : 0.0f)
                  << " peak=" << gate6cMetrics.lfePeak << '\n'
                  << "gate6cOutputSamples=" << gate6cMetrics.outputSamples << '\n'
                  << "gate6cExpectedOutputSamples=" << expectedOutputSamples << '\n'
                  << "gate6cFallback=" << gate6cMetrics.fallback << '\n'
                  << "gate6cMetadataOrderFailures=" << gate6cMetrics.metadataOrderFailures << '\n'
                  << "gate6cContinuityFailures=" << gate6cMetrics.continuityFailures << '\n'
                  << "gate6cOutputRange=" << gate6cMetrics.firstOutputPosition << '-'
                  << gate6cMetrics.lastOutputEnd << '\n'
                  << "gate6cResult=" << (gate6cPass ? "PASS" : "INCONCLUSIVE")
                  << " stage=gate6c-renderer-neutral-timeline\n";
        if (gate7b) {
            const eac3gate7b::Metrics &propertyMetrics = gate7bAdapter.metrics();
            bool stableIdentities = propertyMetrics.objectIndexMin == 1U
                && propertyMetrics.objectIndexMax == eac3gate7b::kDynamicObjectCount;
            for (unsigned object = 1U; object <= eac3gate7b::kDynamicObjectCount; ++object) {
                stableIdentities = stableIdentities
                    && propertyMetrics.updatesByObject[object] > 0U;
            }
            const bool finiteRanges = std::isfinite(propertyMetrics.positionMin[0])
                && std::isfinite(propertyMetrics.positionMax[0])
                && std::isfinite(propertyMetrics.positionMin[1])
                && std::isfinite(propertyMetrics.positionMax[1])
                && std::isfinite(propertyMetrics.positionMin[2])
                 && std::isfinite(propertyMetrics.positionMax[2])
                 && std::isfinite(propertyMetrics.volumeMin)
                 && std::isfinite(propertyMetrics.volumeMax)
                 && std::isfinite(propertyMetrics.sourceRadiusMin)
                 && std::isfinite(propertyMetrics.sourceRadiusMax)
                 && std::isfinite(propertyMetrics.outputRadiusMin)
                 && std::isfinite(propertyMetrics.outputRadiusMax)
                 && propertyMetrics.radiusSamples != 0U;
            gate7bPass = gate6cPass
                && gate7bMetrics.failedBatches == 0U
                && propertyMetrics.updatesAttempted == expectedMetadataEmitted
                && propertyMetrics.updatesCommitted == expectedMetadataEmitted
                && propertyMetrics.unsupportedCount == 0U
                && propertyMetrics.rejectedCount == 0U
                && stableIdentities && finiteRanges;
            const std::string failureReason = !propertyMetrics.firstFailureReason.empty()
                ? propertyMetrics.firstFailureReason : gate7bMetrics.firstFailureReason;
            const char *gate7bResult = gate7bPass ? "PASS"
                : (propertyMetrics.unsupportedCount != 0U ? "INCONCLUSIVE" : "FAIL");
            const eac3gate7b::Geometry &geometry = gate7bAdapter.geometry();
            const double sourceRadiusMean = propertyMetrics.radiusSamples != 0U
                ? propertyMetrics.sourceRadiusSum
                    / static_cast<double>(propertyMetrics.radiusSamples) : 0.0;
            const double outputRadiusMean = propertyMetrics.radiusSamples != 0U
                ? propertyMetrics.outputRadiusSum
                    / static_cast<double>(propertyMetrics.radiusSamples) : 0.0;
            std::cout << std::fixed << std::setprecision(6)
                      << "gate7bReferenceRoomMetres=" << geometry.roomWidthMetres << ','
                      << geometry.roomDepthMetres << ',' << geometry.roomHeightMetres << '\n'
                      << "gate7bReferenceListenerNormalized=" << geometry.listenerX << ','
                      << geometry.listenerY << ',' << geometry.listenerZ << '\n'
                      << "gate7bReferenceScreenBottomLeft=" << geometry.screenBottomLeftX << ','
                      << geometry.screenBottomLeftY << ',' << geometry.screenBottomLeftZ << '\n'
                      << "gate7bReferenceScreenNormalized=" << geometry.screenWidth << ','
                      << geometry.screenHeight << '\n'
                      << "gate7bGainHeadroomDb=" << geometry.gainHeadroomDb << '\n'
                      << "gate7bPositionRadiusMode="
                      << eac3gate7b::positionRadiusModeText(geometry.positionRadiusMode) << '\n'
                      << "gate7bPositionDirectionMode="
                      << eac3gate7b::positionDirectionModeText(geometry.positionDirectionMode) << '\n'
                      << "gate7bAzimuthFocus=" << geometry.azimuthFocus << '\n'
                      << "gate7bSourcePositionRadiusRange=" << propertyMetrics.sourceRadiusMin
                      << ':' << propertyMetrics.sourceRadiusMax << '\n'
                      << "gate7bSourcePositionRadiusMean=" << sourceRadiusMean << '\n'
                      << "gate7bOutputPositionRadiusRange=" << propertyMetrics.outputRadiusMin
                      << ':' << propertyMetrics.outputRadiusMax << '\n'
                      << "gate7bOutputPositionRadiusMean=" << outputRadiusMean << '\n'
                      << "gate7bPositionRadiusSamples=" << propertyMetrics.radiusSamples << '\n'
                      << "gate7bUpdatesAttempted=" << propertyMetrics.updatesAttempted << '\n'
                      << "gate7bUpdatesCommitted=" << propertyMetrics.updatesCommitted << '\n'
                      << "gate7bFirstStateSnaps=" << propertyMetrics.firstStateSnaps << '\n'
                      << "gate7bSteps=" << propertyMetrics.stepCount << '\n'
                      << "gate7bRamps=" << propertyMetrics.rampCount << '\n'
                      << "gate7bOverlaps=" << propertyMetrics.overlapCount << '\n'
                      << "gate7bScreenConversions=" << propertyMetrics.screenConversions << '\n'
                      << "gate7bRoomConversions=" << propertyMetrics.roomConversions << '\n'
                      << "gate7bSilentTargets=" << propertyMetrics.silentTargets << '\n'
                      << "gate7bUnsupported=" << propertyMetrics.unsupportedCount << '\n'
                      << "gate7bRejected=" << propertyMetrics.rejectedCount << '\n'
                      << "gate7bWindowsXRange=" << propertyMetrics.positionMin[0] << ':'
                      << propertyMetrics.positionMax[0] << '\n'
                      << "gate7bWindowsYRange=" << propertyMetrics.positionMin[1] << ':'
                      << propertyMetrics.positionMax[1] << '\n'
                      << "gate7bWindowsZRange=" << propertyMetrics.positionMin[2] << ':'
                      << propertyMetrics.positionMax[2] << '\n'
                      << "gate7bVolumeRange=" << propertyMetrics.volumeMin << ':'
                      << propertyMetrics.volumeMax << '\n'
                      << "gate7bStableObjectIndexRange=" << propertyMetrics.objectIndexMin
                      << '-' << propertyMetrics.objectIndexMax << '\n'
                      << "gate7bPerObjectUpdates=";
            for (unsigned object = 1U; object <= eac3gate7b::kDynamicObjectCount; ++object) {
                if (object != 1U) std::cout << ',';
                std::cout << object << ':' << propertyMetrics.updatesByObject[object];
            }
            std::cout << '\n'
                      << "gate7bResetCount=" << propertyMetrics.resetCount << '\n'
                      << "gate7bCallbackBatches=" << gate7bMetrics.callbackBatches << '\n'
                      << "gate7bMetadataBatches=" << gate7bMetrics.metadataBatches << '\n'
                      << "gate7bFlushBatches=" << gate7bMetrics.flushBatches << '\n'
                      << "gate7bFirstFailurePosition=" << propertyMetrics.firstFailurePosition << '\n'
                      << "gate7bFirstFailureObject=" << propertyMetrics.firstFailureObject << '\n'
                      << "gate7bFirstFailureReason=" << failureReason << '\n'
                      << "gate7bResult=" << gate7bResult
                      << " evidenceLimit=offline-property-adapter-only\n";
        }
        if (renderer) {
            const double producerWallSeconds = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - gate7cProducerStart).count();
            if (gate6cFlushPass && gate7bPass) {
                renderer->finish();
            } else {
                renderer->cancel();
            }
            const eac3renderer::Metrics renderMetrics = renderer->result();
            const bool hrtfRenderer = rendererOptions.spatialRenderer
                == eac3renderer::SpatialRenderer::Hrtf;
            const std::uint64_t expectedBufferBytes = renderMetrics.submittedFrames
                * (eac3gate7b::kDynamicObjectCount + (hrtfRenderer ? 0U : 1U)) * sizeof(float);
            const double sourceAudioSeconds = static_cast<double>(renderMetrics.sourceFrames)
                / 48000.0;
            bool finiteCountPass = hrtfRenderer
                ? renderMetrics.lfeFiniteSamples == 0U
                : renderMetrics.lfeFiniteSamples == renderMetrics.submittedFrames;
            for (unsigned object = 1U;
                 object <= eac3gate7b::kDynamicObjectCount; ++object) {
                finiteCountPass = finiteCountPass
                    && renderMetrics.finiteSamples[object] == renderMetrics.submittedFrames;
            }
            const bool renderPass = gate6cFlushPass && gate7bPass
                && renderMetrics.outcome == eac3renderer::Outcome::Pass
                && renderMetrics.queue.pushes == renderMetrics.queue.pops
                && renderMetrics.queue.producerTimeouts == 0U
                && renderMetrics.queue.staleGenerationRejects == 0U
                && renderMetrics.queue.validationRejects == 0U
                && renderMetrics.queue.admissionTimelineRejects == 0U
                && renderMetrics.queue.flushRejects == 0U
                && renderMetrics.queue.itemsRemaining == 0U
                && renderMetrics.queue.canceledDiscarded == 0U
                && renderMetrics.maximumDynamicObjects >= eac3gate7b::kDynamicObjectCount
                && renderMetrics.activatedDynamicObjects
                    == eac3gate7b::kDynamicObjectCount
                && renderMetrics.initialPropertiesReady
                && renderMetrics.underrunFrames == 0U
                && renderMetrics.canceledStagedFrames == 0U
                && renderMetrics.eosPendingMetadata == 0U
                && renderMetrics.sourceFrames
                    == static_cast<std::uint64_t>(gate6cExpectedOutputSamples)
                && renderMetrics.submittedFrames
                    == renderMetrics.sourceFrames + renderMetrics.paddingFrames
                && renderMetrics.renderCommits > 0U
                && renderMetrics.beginCalls == renderMetrics.renderCommits
                && renderMetrics.endCalls == renderMetrics.renderCommits
                && renderMetrics.terminalQuantum
                && renderMetrics.terminalQuanta == 1U
                && renderMetrics.maximumPropertyLateness
                    < static_cast<std::int64_t>(renderMetrics.frameCountMax)
                && renderMetrics.exactBufferCalls
                    == renderMetrics.renderCommits
                        * (eac3gate7b::kDynamicObjectCount + (hrtfRenderer ? 0U : 1U))
                && renderMetrics.propertyCalls
                    == renderMetrics.renderCommits
                        * eac3gate7b::kDynamicObjectCount * (hrtfRenderer ? 1U : 2U)
                && renderMetrics.lfeVolumeCalls == (hrtfRenderer ? 0U : 1U)
                && std::isfinite(renderMetrics.lfeVolume)
                && std::abs(renderMetrics.lfeVolume
                            - (rendererOptions.disableLfe ? 0.0f : 0.177827941f))
                    < 0.0000001f
                && renderMetrics.exactBufferBytes == expectedBufferBytes
                && finiteCountPass
                && renderMetrics.inactiveObjects == 0U
                && renderMetrics.streamStarted
                && renderMetrics.streamStopped
                && renderMetrics.streamReset
                && renderMetrics.comReleased
                && renderMetrics.cleanupComplete
                && renderMetrics.consumerJoined;
            std::cout << "gate7cQueueCapacity=" << rendererOptions.queueBatches << '\n'
                      << "gate7cPositionRadiusMode="
                      << eac3gate7b::positionRadiusModeText(rendererOptions.positionRadiusMode)
                      << '\n'
                      << "gate7cPositionDirectionMode="
                      << eac3gate7b::positionDirectionModeText(rendererOptions.positionDirectionMode)
                      << '\n'
                      << "gate7cAzimuthFocus=" << rendererOptions.azimuthFocus << '\n'
                      << "gate7cSpatialRenderer="
                      << eac3renderer::spatialRendererText(rendererOptions.spatialRenderer) << '\n'
                      << "gate7cHrtfEnvironment="
                      << eac3renderer::hrtfEnvironmentText(rendererOptions.hrtfEnvironment) << '\n'
                      << "gate7cRequestedStaticObjectMask="
                      << (hrtfRenderer ? "None" : "LowFrequency") << '\n'
                      << "gate7cPrebufferBatches=" << rendererOptions.prebufferBatches << '\n'
                      << "gate7cPushTimeoutMs=" << rendererOptions.pushTimeoutMilliseconds << '\n'
                      << "gate7cEndpointId=" << renderMetrics.endpointId << '\n'
                      << "gate7cInterfaceAvailable=" << (renderMetrics.interfaceAvailable ? 1 : 0) << '\n'
                      << "gate7cHrtfDistanceDecay="
                      << (renderMetrics.hrtfDistanceDecayNull ? "NULL" : "SET") << '\n'
                      << "gate7cPreScaleSamples=" << renderMetrics.preScaledSamples << '\n'
                      << "gate7cPreScaleRange=" << renderMetrics.preScaleMin << ':'
                      << renderMetrics.preScaleMax << '\n'
                      << "gate7cNativeStaticMask=" << renderMetrics.nativeStaticMask << '\n'
                      << "gate7cMaximumDynamicObjects=" << renderMetrics.maximumDynamicObjects << '\n'
                      << "gate7cActivatedDynamicObjects=" << renderMetrics.activatedDynamicObjects << '\n'
                      << "gate7cCapacityNotifications=" << renderMetrics.capacityNotifications << '\n'
                      << "gate7cLatestCapacity=" << renderMetrics.latestCapacity << '\n'
                      << "gate7cObjectFormat=" << renderMetrics.objectFormatRate << ','
                      << renderMetrics.objectFormatChannels << ','
                      << renderMetrics.objectFormatBits << '\n'
                      << "gate7cActualPrebufferBatches=" << renderMetrics.prebufferBatches << '\n'
                      << "gate7cActualPrebufferFrames=" << renderMetrics.prebufferFrames << '\n'
                      << "gate7cInitialPropertiesReady="
                      << (renderMetrics.initialPropertiesReady ? 1 : 0) << '\n'
                      << "gate7cActualPrebufferMs="
                      << (static_cast<double>(renderMetrics.prebufferFrames) * 1000.0 / 48000.0)
                      << '\n'
                      << "gate7cProducerWallSeconds=" << producerWallSeconds << '\n'
                      << "gate7cSourceAudioSeconds=" << sourceAudioSeconds << '\n'
                      << "gate7cProducerRealtimeRatio="
                      << (sourceAudioSeconds > 0.0
                              ? producerWallSeconds / sourceAudioSeconds : 0.0) << '\n'
                      << "gate7cPushes=" << renderMetrics.queue.pushes << '\n'
                      << "gate7cPops=" << renderMetrics.queue.pops << '\n'
                      << "gate7cMaximumDepth=" << renderMetrics.queue.maximumDepth << '\n'
                      << "gate7cProducerWaits=" << renderMetrics.queue.producerWaits << '\n'
                      << "gate7cProducerTimeouts=" << renderMetrics.queue.producerTimeouts << '\n'
                      << "gate7cValidationRejects=" << renderMetrics.queue.validationRejects << '\n'
                      << "gate7cFlushRejects=" << renderMetrics.queue.flushRejects << '\n'
                      << "gate7cRemaining=" << renderMetrics.queue.itemsRemaining << '\n'
                      << "gate7cDiscarded=" << renderMetrics.queue.canceledDiscarded << '\n'
                      << "gate7cRenderAttempts=" << renderMetrics.renderAttempts << '\n'
                      << "gate7cRenderCommits=" << renderMetrics.renderCommits << '\n'
                      << "gate7cFrameCountRange=" << renderMetrics.frameCountMin << ':'
                      << renderMetrics.frameCountMax << '\n'
                      << "gate7cSubmittedFrames=" << renderMetrics.submittedFrames << '\n'
                      << "gate7cSourceFrames=" << renderMetrics.sourceFrames << '\n'
                      << "gate7cPaddingFrames=" << renderMetrics.paddingFrames << '\n'
                      << "gate7cUnderrunFrames=" << renderMetrics.underrunFrames << '\n'
                      << "gate7cExactBufferCalls=" << renderMetrics.exactBufferCalls << '\n'
                      << "gate7cExactBufferBytes=" << renderMetrics.exactBufferBytes << '\n'
                      << "gate7cPropertyCalls=" << renderMetrics.propertyCalls << '\n'
                      << "gate7cLfePolicy="
                      << (rendererOptions.disableLfe ? "DISABLED_BY_USER" : "PROGRAM_HEADROOM")
                      << '\n'
                      << "gate7cDynamicGainHeadroomDb=15\n"
                      << "gate7cLfeVolumeCalls=" << renderMetrics.lfeVolumeCalls << '\n'
                      << "gate7cLfeVolume=" << renderMetrics.lfeVolume << '\n'
                      << "gate7cFiniteSamples=";
            for (unsigned object = 1U;
                 object <= eac3gate7b::kDynamicObjectCount; ++object) {
                if (object != 1U) std::cout << ',';
                std::cout << object << ':' << renderMetrics.finiteSamples[object];
            }
            std::cout << '\n' << "gate7cPeaks=";
            for (unsigned object = 1U;
                 object <= eac3gate7b::kDynamicObjectCount; ++object) {
                if (object != 1U) std::cout << ',';
                std::cout << object << ':' << renderMetrics.peaks[object];
            }
            std::cout << '\n'
                      << "gate7cLfeFiniteSamples=" << renderMetrics.lfeFiniteSamples << '\n'
                      << "gate7cLfePeak=" << renderMetrics.lfePeak << '\n'
                      << "gate7cMetadataApplied=" << renderMetrics.metadataApplied << '\n'
                      << "gate7cMetadataDeferred=" << renderMetrics.metadataDeferred << '\n'
                      << "gate7cEosPendingMetadata=" << renderMetrics.eosPendingMetadata << '\n'
                      << "gate7cMaximumLateness=" << renderMetrics.maximumPropertyLateness << '\n'
                      << "gate7cTerminalQuantum=" << (renderMetrics.terminalQuantum ? 1 : 0) << '\n'
                      << "gate7cTerminalQuanta=" << renderMetrics.terminalQuanta << '\n'
                      << "gate7cFirstFailureStage=" << renderMetrics.firstFailureStage << '\n'
                      << "gate7cFirstFailureHresult=" << renderMetrics.firstFailureHresult << '\n'
                      << "gate7cMetricConsistency=" << (renderPass ? "PASS" : "FAIL") << '\n'
                      << "gate7cStreamStarted=" << (renderMetrics.streamStarted ? 1 : 0)
                      << " stopped=" << (renderMetrics.streamStopped ? 1 : 0)
                      << " reset=" << (renderMetrics.streamReset ? 1 : 0)
                      << " joined=" << (renderMetrics.consumerJoined ? 1 : 0)
                      << " cleanup=" << (renderMetrics.cleanupComplete ? 1 : 0) << '\n'
                      << "gate7cResult=" << (renderPass ? "PASS" : "FAIL")
                      << " evidenceLimit=diagnostic-spatial-endpoint-submission;manual-listening-or-loopback-required\n";
            if (!renderPass) return false;
        }
        if (!gate6cPass || !gate7bPass) return false;
    }
    if (matrixTrace) {
        std::string traceReason;
        if (!matrixTrace->close(&traceReason)) {
            std::cout << "jocMatrixTrace=FAIL reason=" << traceReason << '\n';
            return false;
        }
        std::cout << "jocMatrixTrace=PASS path=" << jocMatrixTracePath << '\n';
    }
    if (bearExporter) {
        std::string exportReason;
        if (!bearExporter->finish(&exportReason)) {
            std::cout << "bearExport=FAIL reason=" << exportReason << '\n';
            return false;
        }
        std::cout << "bearExport=PASS batches=" << bearExporter->batches()
                  << " metadata=" << bearExporter->metadata() << '\n';
    }

    std::cout << "jocQmfPairingTrimPolicy=deferred-until-post-object-synthesis\n"
              << "jocQmfPairingContainerTrimMetadata=skipSamples:" << skipSamples
              << ",discardPadding:" << discardPadding << '\n'
              << "jocQmfPairingABTrimmedDecode=" << (trimmedDecodePass ? "PASS" : "INCONCLUSIVE") << '\n'
              << "jocQmfPairingABTrimmedDecodeReason=" << trimmedCapture.reason << '\n'
              << "jocQmfPairingABTrimmedDecodedFrames=" << trimmedCapture.decodedFrames << '\n'
              << "jocQmfPairingABTrimmedDecodedSamples=" << trimmedCapture.decodedSamples << '\n'
              << "jocQmfPairingABTrimmedFirstFrameSamples=" << trimmedCapture.firstFrameSamples << '\n'
              << "jocQmfPairingABTrimmedAllFrames1536="
              << (trimmedCapture.allFrames1536 ? "PASS" : "INCONCLUSIVE") << '\n'
              << "jocQmfPairingABTrimmedTimeline=" << trimmedTimelineStart << '-'
              << trimmedTimelineEnd << '\n'
              << "jocQmfPairingABCodedDecode=" << (codedDecodePass ? "PASS" : "INCONCLUSIVE") << '\n'
              << "jocQmfPairingABCodedDecodeReason=" << codedCapture.reason << '\n'
              << "jocQmfPairingABCodedDecodedFrames=" << codedCapture.decodedFrames << '\n'
              << "jocQmfPairingABCodedDecodedSamples=" << codedCapture.decodedSamples << '\n'
              << "jocQmfPairingABCodedFirstFrameSamples=" << codedCapture.firstFrameSamples << '\n'
              << "jocQmfPairingABCodedAllFrames1536="
              << (codedCapture.allFrames1536 ? "PASS" : "INCONCLUSIVE") << '\n'
              << "jocQmfPairingABCodedStrictAuPairing="
              << (timelinePass ? "PASS" : "STOP") << '\n'
              << "jocQmfPairingABTrimmedTimelineMapping="
              << (trimmedTimelinePass ? "PASS" : "INCONCLUSIVE") << '\n'
              << "jocQmfPairingChunkCount=" << inputChunkCount << '\n'
              << "jocQmfPairingAccessUnits=" << units.size() << '\n'
              << "jocQmfPairingAssemblerLookaheadChunks="
              << (inputChunkCount > units.size() ? inputChunkCount - units.size() : 0) << '\n'
              << "jocQmfPairingAssemblerLookaheadExplanation="
              << "next-chunk-flushes-final-pending-au;coded-prefix-pairing-excludes-lookahead" << '\n'
              << "jocQmfPairingNativeDecode=" << (decodePass ? "PASS" : "INCONCLUSIVE") << '\n'
              << "jocQmfPairingNativeDecodeReason=" << capture.reason << '\n'
              << "jocQmfPairingDecodedFrames=" << capture.decodedFrames << '\n'
              << "jocQmfPairingDecodedSamples=" << capture.decodedSamples << '\n'
              << "jocQmfPairingFirstFrameSamples=" << capture.firstFrameSamples << '\n'
              << "jocQmfPairingAllFrames1536=" << (capture.allFrames1536 ? "PASS" : "INCONCLUSIVE") << '\n'
              << "jocQmfPairingSampleRate=" << capture.sampleRate << '\n'
              << "jocQmfPairingChannels=" << capture.channels << '\n'
              << "jocQmfPairingLayout=" << capture.layout << '\n'
              << "jocQmfPairingFormat="
              << (av_get_sample_fmt_name(static_cast<AVSampleFormat>(capture.format))
                      ? av_get_sample_fmt_name(static_cast<AVSampleFormat>(capture.format))
                      : "unknown") << '\n'
              << "jocQmfPairingSkipSamples=" << skipSamples << '\n'
              << "jocQmfPairingDiscardPadding=" << discardPadding << '\n'
              << "jocQmfPairingNativeTimeline=" << nativeTimelineStart << '-'
              << nativeTimelineEnd << '\n'
              << "jocQmfPairingTimelineMapping=" << (timelinePass ? "PASS" : "STOP") << '\n'
              << "jocQmfPairingLayoutMismatchFrames=" << capture.layoutMismatchFrames << '\n'
              << "jocQmfPairingPcmFrameAuOneToOne="
              << (timelinePass ? "PASS" : "INCONCLUSIVE") << '\n';
    for (const auto &[config, configMetrics] : metrics) {
        std::cout << "jocQmfPairingConfig" << config << "Found=" << configMetrics.found << '\n'
                  << "jocQmfPairingConfig" << config << "Layout=" << capture.layout << '\n'
                  << "jocQmfPairingConfig" << config << "Paired=" << configMetrics.paired << '\n'
                  << "jocQmfPairingConfig" << config << "LayoutMismatch="
                  << configMetrics.layoutMismatch << '\n'
                  << "jocQmfPairingConfig" << config << "PrimingSkipped="
                  << configMetrics.primingSkipped << '\n'
                  << "jocQmfPairingConfig" << config << "TailIncomplete="
                  << configMetrics.tailIncomplete << '\n'
                  << "jocQmfPairingConfig" << config << "Fallback="
                  << configMetrics.fallback << '\n'
                  << "jocQmfPairingConfig" << config << "PairedSamples="
                  << configMetrics.pairedSamples << '\n'
                  << "jocQmfPairingConfig" << config << "LfeSamples="
                  << configMetrics.lfeSamples << '\n'
                  << "jocQmfPairingConfig" << config << "Reconstructed="
                  << configMetrics.reconstructed << '\n'
                  << "jocQmfPairingConfig" << config << "ObjectCountRange="
                  << configMetrics.objectCountMin << '-' << configMetrics.objectCountMax << '\n'
                  << "jocQmfPairingConfig" << config << "ObjectCountDrift="
                  << configMetrics.objectCountDrift << '\n'
                  << "jocQmfPairingConfig" << config << "PresentObjectFrames="
                  << configMetrics.presentObjectFrames << '\n'
                  << "jocQmfPairingConfig" << config << "ObjectQmfValueCount="
                  << configMetrics.objectQmfValueCount << '\n'
                  << "jocQmfPairingConfig" << config << "ObjectQmfFiniteCount="
                  << configMetrics.objectQmfFiniteCount << '\n'
                  << "jocQmfPairingConfig" << config << "ObjectQmfFinite="
                  << (configMetrics.objectQmfValueCount == configMetrics.objectQmfFiniteCount
                          ? "PASS" : "FAIL") << '\n'
                  << "jocQmfPairingConfig" << config << "LfeFiniteCount="
                  << configMetrics.lfeFiniteCount << '\n'
                  << "jocQmfPairingConfig" << config << "LfeFinite="
                  << (configMetrics.lfeFiniteCount == static_cast<std::size_t>(configMetrics.lfeSamples)
                          ? "PASS" : "FAIL") << '\n'
                  << "jocQmfPairingConfig" << config << "LfePeak="
                  << configMetrics.lfePeak << '\n'
                  << "jocQmfPairingConfig" << config << "QmfResets="
                  << configMetrics.qmfResets << '\n'
                  << "jocQmfPairingConfig" << config << "MathFailures="
                  << configMetrics.mathFailures << '\n'
                  << "jocQmfPairingConfig" << config << "ReconstructFailures="
                  << configMetrics.reconstructFailures << '\n'
                  << "jocQmfPairingConfig" << config << "SequenceDiscontinuities="
                  << configMetrics.sequenceDiscontinuities << '\n'
                  << "jocQmfPairingConfig" << config << "MaxContinuousRun="
                  << configMetrics.maxContinuousRun << '\n';
    }
    std::cout << "jocQmfPairingChannelMapConfig3=FL,FR,FC,SL,SR;LFE=LF\n"
              << "jocQmfPairingChannelMapConfig4=FL,FR,FC,SL,SR,TFL,TFR;LFE=LF\n";
    const PairingConfigMetrics *targetMetrics = nullptr;
    unsigned targetConfig = 0;
    for (const unsigned config : {3U, 4U}) {
        if (metrics[config].found == units.size()) {
            if (targetMetrics != nullptr) {
                targetMetrics = nullptr;
                targetConfig = 0;
                break;
            }
            targetConfig = config;
            targetMetrics = &metrics[config];
        }
    }
    const bool targetConfigPass = targetMetrics != nullptr;
    const bool objectMetricsPass = targetConfigPass
        && targetMetrics->objectCountMin > 0
        && targetMetrics->objectCountMin == targetMetrics->objectCountMax
        && targetMetrics->objectCountDrift == 0
        && targetMetrics->objectQmfValueCount == targetMetrics->objectQmfFiniteCount
        && targetMetrics->lfeFiniteCount == static_cast<std::size_t>(targetMetrics->lfeSamples);
    const bool reconstructionPass = targetConfigPass
        && targetMetrics->paired == units.size()
        && targetMetrics->reconstructed == units.size()
        && targetMetrics->maxContinuousRun >= units.size()
        && targetMetrics->fallback == 0
        && targetMetrics->mathFailures == 0
        && targetMetrics->reconstructFailures == 0
        && targetMetrics->sequenceDiscontinuities == 0
        && targetMetrics->layoutMismatch == 0
        && targetMetrics->primingSkipped == 0
        && targetMetrics->tailIncomplete == 0
        && objectMetricsPass;
    const bool coveragePass = reconstructionPass && units.size() >= 1000
        && targetMetrics->found >= 1000
        && targetMetrics->paired >= 1000
        && targetMetrics->reconstructed >= 1000
        && targetMetrics->maxContinuousRun >= 1000;
    const bool smokePass = reconstructionPass && units.size() < 1000;
    const bool stopCondition = multiplePayloadStop || !timelinePass
        || (metrics[3].layoutMismatch != 0) || (metrics[4].layoutMismatch != 0)
        || !targetConfigPass;
    const bool pass = !stopCondition && coveragePass;
    std::cout << "jocQmfPairingMultiplePayloadUnits="
              << (multiplePayloadStop ? "STOP" : "0") << '\n'
              << "jocQmfPairingStopCondition=" << (stopCondition ? "YES" : "NO") << '\n'
              << "jocQmfPairingTargetConfig=" << targetConfig << '\n'
              << "jocQmfSmokeResult="
              << (units.size() >= 1000 ? "NOT_APPLICABLE"
                                       : (smokePass ? "PASS" : "FAIL")) << '\n'
              << "jocQmfResult=" << (pass ? "PASS" : "INCONCLUSIVE")
              << " stage=gate5c-object-qmf-reconstruction reason="
              << (pass ? "coded-domain-pcm-object-qmf-reconstructed"
                       : (smokePass ? "coverage-below-1000-access-units"
                                    : (stopCondition ? "gate5c-stop-condition-or-config-pairing-failure"
                                                     : "object-qmf-metrics-or-fallback-failure"))) << '\n';
    return pass;
}

bool runPcmProbe(const std::string &path,
                 const std::vector<Chunk> &chunks,
                 const std::vector<AccessUnit> &units,
                 PcmReport *report)
{
    if (!report || units.empty()) {
        return false;
    }
    AVFormatContext *format = nullptr;
    const AVInputFormat *inputFormat = isContainerPath(path) ? nullptr : av_find_input_format("eac3");
    int result = avformat_open_input(&format, path.c_str(), inputFormat, nullptr);
    if (result < 0) {
        report->reason = "avformat_open_input:" + errorText(result);
        return false;
    }
    result = avformat_find_stream_info(format, nullptr);
    if (result < 0) {
        report->reason = "avformat_find_stream_info:" + errorText(result);
        avformat_close_input(&format);
        return false;
    }
    const AVCodec *codec = nullptr;
    const int streamIndex = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (streamIndex < 0 || !codec) {
        report->reason = "no-audio-stream:" + errorText(streamIndex);
        avformat_close_input(&format);
        return false;
    }
    AVCodecContext *decoder = avcodec_alloc_context3(codec);
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    if (!decoder || !packet || !frame) {
        report->reason = "allocation-failed";
        avcodec_free_context(&decoder);
        av_packet_free(&packet);
        av_frame_free(&frame);
        avformat_close_input(&format);
        return false;
    }
    result = avcodec_parameters_to_context(decoder, format->streams[streamIndex]->codecpar);
    if (result >= 0) {
        result = avcodec_open2(decoder, codec, nullptr);
    }
    if (result < 0) {
        report->reason = "avcodec_open2:" + errorText(result);
        avcodec_free_context(&decoder);
        av_packet_free(&packet);
        av_frame_free(&frame);
        avformat_close_input(&format);
        return false;
    }

    const std::size_t chunkCount = std::min(chunks.size(), units.size());
    auto receive = [&]() -> bool {
        while (true) {
            result = avcodec_receive_frame(decoder, frame);
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
                return true;
            }
            if (result < 0) {
                report->reason = "avcodec_receive_frame:" + errorText(result);
                return false;
            }
            if (report->decodedFrames == 0) {
                report->firstFrameSamples = frame->nb_samples;
            }
            ++report->decodedFrames;
            report->decodedSamples += frame->nb_samples;
            report->sampleRate = frame->sample_rate;
            report->channels = frame->ch_layout.nb_channels;
            report->format = frame->format;
            report->layout = channelLayoutText(frame->ch_layout);
            av_frame_unref(frame);
        }
    };

    for (std::size_t index = 0; index < chunkCount; ++index) {
        if (av_new_packet(packet, static_cast<int>(chunks[index].bytes.size())) < 0) {
            report->reason = "packet-allocation-failed";
            break;
        }
        std::copy(chunks[index].bytes.begin(), chunks[index].bytes.end(), packet->data);
        if (chunks[index].skipSamples != 0 || chunks[index].discardPadding != 0) {
            std::uint8_t *skipData = av_packet_new_side_data(packet, AV_PKT_DATA_SKIP_SAMPLES, 10);
            if (!skipData) {
                report->reason = "skip-sample-side-data-allocation-failed";
                av_packet_unref(packet);
                break;
            }
            const std::uint32_t skip = static_cast<std::uint32_t>(chunks[index].skipSamples);
            const std::uint32_t discard = static_cast<std::uint32_t>(chunks[index].discardPadding);
            for (unsigned byte = 0; byte < 4; ++byte) {
                skipData[byte] = static_cast<std::uint8_t>(skip >> (byte * 8U));
                skipData[4U + byte] = static_cast<std::uint8_t>(discard >> (byte * 8U));
            }
            skipData[8] = 0;
            skipData[9] = 0;
        }
        result = avcodec_send_packet(decoder, packet);
        av_packet_unref(packet);
        if (result < 0 && result != AVERROR(EAGAIN)) {
            report->reason = "avcodec_send_packet:" + errorText(result);
            break;
        }
        if (!receive()) {
            break;
        }
    }
    if (report->reason.empty()) {
        result = avcodec_send_packet(decoder, nullptr);
        if (result >= 0 || result == AVERROR_EOF) {
            receive();
        } else {
            report->reason = "avcodec_flush:" + errorText(result);
        }
    }

    std::int64_t expectedSamples = 0;
    std::int64_t skipSamples = 0;
    std::int64_t discardPadding = 0;
    std::size_t dependentUnits = 0;
    for (std::size_t index = 0; index < units.size(); ++index) {
        expectedSamples += 1536;
        if (index < chunks.size()) {
            skipSamples += chunks[index].skipSamples;
            discardPadding += chunks[index].discardPadding;
        }
        if (std::any_of(units[index].frames.begin(), units[index].frames.end(),
                       [](const Syncframe &candidate) {
                           return candidate.frameType == FrameType::Dependent;
                       })) {
            ++dependentUnits;
        }
    }
    expectedSamples = std::max<std::int64_t>(0, expectedSamples - skipSamples - discardPadding);
    report->nativeFormatPass = report->decodedFrames > 0
        && report->sampleRate == 48000
        && report->format == AV_SAMPLE_FMT_FLTP
        && (report->channels == 6 || report->channels == 8);
    report->sampleCountPass = report->nativeFormatPass && report->decodedSamples == expectedSamples;
    report->oneToOnePass = report->nativeFormatPass
        && report->decodedFrames == units.size()
        && report->firstFrameSamples == 1536
        && report->decodedSamples == expectedSamples;
    report->ok = report->reason.empty() && report->sampleCountPass;
    std::cout << "pcmDecodedFrames=" << report->decodedFrames << '\n'
              << "pcmDecodedSamples=" << report->decodedSamples << '\n'
              << "pcmExpectedSamplesAfterPacketTrim=" << expectedSamples << '\n'
              << "pcmPacketSkipSamples=" << skipSamples << '\n'
              << "pcmPacketDiscardPadding=" << discardPadding << '\n'
              << "pcmFirstFrameSamples=" << report->firstFrameSamples << '\n'
              << "pcmSampleRate=" << report->sampleRate << '\n'
              << "pcmChannels=" << report->channels << '\n'
              << "pcmLayout=" << report->layout << '\n'
              << "pcmFormat=" << (av_get_sample_fmt_name(static_cast<AVSampleFormat>(report->format))
                                      ? av_get_sample_fmt_name(static_cast<AVSampleFormat>(report->format))
                                      : "unknown") << '\n'
              << "pcmResampler=none\n"
              << "pcmNativeFormat=" << (report->nativeFormatPass ? "PASS" : "FAIL") << '\n'
              << "pcmSampleCount=" << (report->sampleCountPass ? "PASS" : "FAIL") << '\n'
              << "pcmAccessUnitPairing=" << (report->oneToOnePass
                                                   ? "PASS"
                                                   : (report->sampleCountPass ? "PASS_WITH_CODEC_PRIMING" : "INCONCLUSIVE")) << '\n'
              << "pcmDependentSubstreamHandling="
              << (dependentUnits == 0 ? "NOT_APPLICABLE" : (report->channels >= 8 ? "PASS" : "INCONCLUSIVE")) << '\n'
              << "pcmCoverageSamples=" << units.size() * 1536LL << '\n'
              << std::fixed << std::setprecision(3)
              << "pcmCoverageSeconds=" << (units.size() * 1536.0 / 48000.0) << '\n'
              << "pcmResult=" << (report->ok ? "PASS" : "INCONCLUSIVE")
              << " stage=gate3-native-ffmpeg-pcm-pairing\n";

    avcodec_free_context(&decoder);
    av_packet_free(&packet);
    av_frame_free(&frame);
    avformat_close_input(&format);
    return report->ok;
}

void printUsage()
{
    std::cerr << "Usage: Eac3AccessUnitProbe <raw.eac3|raw.ec3|raw.eb3|container.m4a> "
                 "[--max-units N] [--self-test] [--emdf] [--oamd] "
                 "[--oamd-b2a-order syntax5511-lsb|table31-msb] "
                 "[--oamd-b2a-control-15] [--oamd-b2a-helper-scan] "
                 "[--joc] [--joc-self-test] "
                 "[--joc-math-self-test] [--joc-table path] [--pcm] [--joc-qmf] "
                 "[--joc-gate6c] [--joc-gate7b] [--joc-gate7c] "
                 "[--eac3-drc-scale 0|1] [--eac3-cons-noisegen 0|1] "
                 "[--gate7c-queue-batches N] [--gate7c-prebuffer-batches N] "
                 "[--gate7c-push-timeout-ms N] [--spatial-disable-lfe] "
                 "[--spatial-position-radius source|unit] "
                 "[--spatial-position-direction metadata|front] "
                 "[--spatial-azimuth-focus 0..1] "
                 "[--spatial-renderer standard|hrtf] "
                 "[--spatial-hrtf-environment small|outdoors] [--summary] "
                 "[--dump-eac3 path] [--joc-bear-export dir] [--joc-matrix-trace path] "
                 "[--audio-stream-index N]\n";
}

std::vector<std::uint8_t> makeSyntheticFrame(unsigned frameType,
                                             unsigned blocksCode,
                                             unsigned sampleRateCode = 0,
                                             unsigned frameSizeWordsMinusOne = 4,
                                             unsigned bsid = 16)
{
    std::vector<std::uint8_t> bytes(10, 0);
    bytes[0] = 0x0B;
    bytes[1] = 0x77;
    auto setBits = [&](std::size_t bit, unsigned count, unsigned value) {
        for (unsigned index = 0; index < count; ++index) {
            const std::size_t position = bit + count - index - 1U;
            const std::uint8_t mask = static_cast<std::uint8_t>(1U << (7U - (position % 8U)));
            if ((value >> index) & 1U) {
                bytes[position / 8U] |= mask;
            } else {
                bytes[position / 8U] &= static_cast<std::uint8_t>(~mask);
            }
        }
    };
    setBits(16, 2, frameType);
    setBits(18, 3, 0);
    setBits(21, 11, frameSizeWordsMinusOne);
    setBits(32, 2, sampleRateCode);
    setBits(34, 2, blocksCode);
    setBits(36, 3, 2);  // stereo
    setBits(39, 1, 0);  // no LFE
    setBits(40, 5, bsid); // E-AC-3 bsid
    return bytes;
}

bool runSelfTest()
{
    const std::vector<std::uint8_t> valid = makeSyntheticFrame(0, 3);
    const ParseResult validResult = parseSyncframe(valid, 0);
    if (!validResult.ok || validResult.frame.sizeBytes != 10 || validResult.frame.blocks != 6) {
        std::cerr << "selfTest=FAIL case=valid-frame\n";
        return false;
    }
    std::vector<std::uint8_t> truncated = valid;
    truncated.pop_back();
    if (parseSyncframe(truncated, 0).ok) {
        std::cerr << "selfTest=FAIL case=truncated-frame\n";
        return false;
    }
    const std::vector<std::uint8_t> reserved = makeSyntheticFrame(3, 3);
    if (parseSyncframe(reserved, 0).ok || parseSyncframe(reserved, 0).reason != "reserved-stream-type") {
        std::cerr << "selfTest=FAIL case=reserved-stream-type\n";
        return false;
    }
    for (unsigned reservedBsid : {9U, 10U}) {
        const std::vector<std::uint8_t> reservedLegacy =
            makeSyntheticFrame(0, 3, 0, 4, reservedBsid);
        if (parseSyncframe(reservedLegacy, 0).ok
            || parseSyncframe(reservedLegacy, 0).reason != "reserved-legacy-bsid") {
            std::cerr << "selfTest=FAIL case=reserved-legacy-bsid-"
                      << reservedBsid << '\n';
            return false;
        }
    }
    std::vector<std::uint8_t> mismatch = makeSyntheticFrame(0, 1);
    const std::vector<std::uint8_t> mismatchTail = makeSyntheticFrame(0, 3);
    mismatch.insert(mismatch.end(), mismatchTail.begin(), mismatchTail.end());
    AccessUnitAssembler assembler;
    ProbeResult report;
    if (assembler.consume(mismatch, "self-test", 0, &report)
        || report.reason.find("block-count-mismatch") == std::string::npos) {
        std::cerr << "selfTest=FAIL case=block-count-mismatch\n";
        return false;
    }
    std::vector<std::uint8_t> rateChange = makeSyntheticFrame(0, 3);
    const std::vector<std::uint8_t> rateChangeTail = makeSyntheticFrame(1, 3, 1);
    rateChange.insert(rateChange.end(), rateChangeTail.begin(), rateChangeTail.end());
    AccessUnitAssembler rateAssembler;
    ProbeResult rateReport;
    if (rateAssembler.consume(rateChange, "self-test", 0, &rateReport)
        || rateReport.reason.find("sample-rate-change") == std::string::npos) {
        std::cerr << "selfTest=FAIL case=sample-rate-change\n";
        return false;
    }
    std::cout << "selfTest=PASS cases=valid,truncated,reserved,reserved-bsid-9-10,"
                 "block-count,sample-rate\n";
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        printUsage();
        return 2;
    }
    std::size_t maxUnits = 0;
    bool selfTest = false;
    bool emdf = false;
    bool oamd = false;
    bool oamdB2aControl15 = false;
    bool oamdB2aHelperScan = false;
    // Real samples are one-block/full-render and cannot distinguish the
    // contradictory labels; use the 5.5.11 semantic order as formal default.
    eac3oamd::B2aBitOrder oamdB2aBitOrder = eac3oamd::B2aBitOrder::Syntax5511Lsb;
    bool joc = false;
    bool jocSelfTest = false;
    bool jocMathSelfTest = false;
    bool pcm = false;
    bool jocQmf = false;
    bool jocGate6c = false;
    bool jocGate7b = false;
    bool jocGate7c = false;
    eac3renderer::Options rendererOptions;
    Eac3DecodeOptions decodeOptions;
    std::vector<double> jocQmfQwin;
    std::string jocTablePath = "docs/dev/ts_103420_tables.c";
    bool summaryOnly = false;
    std::string dumpEac3Path;
    std::string jocBearExportPath;
    std::string jocMatrixTracePath;
    int requestedAudioStream = -1;
    for (int index = 2; index < argc; ++index) {
        const std::string option = argv[index];
        if (option == "--max-units" && index + 1 < argc) {
            try {
                maxUnits = static_cast<std::size_t>(std::stoull(argv[++index]));
            } catch (...) {
                printUsage();
                return 2;
            }
        } else if (option == "--self-test") {
            selfTest = true;
        } else if (option == "--emdf") {
            emdf = true;
        } else if (option == "--oamd") {
            oamd = true;
            emdf = true;
        } else if (option == "--oamd-b2a-order" && index + 1 < argc) {
            const std::string order = argv[++index];
            if (order == "syntax5511-lsb") {
                oamdB2aBitOrder = eac3oamd::B2aBitOrder::Syntax5511Lsb;
            } else if (order == "table31-msb") {
                oamdB2aBitOrder = eac3oamd::B2aBitOrder::Table31Msb;
            } else {
                printUsage();
                return 2;
            }
        } else if (option == "--oamd-b2a-control-15") {
            oamdB2aControl15 = true;
        } else if (option == "--oamd-b2a-helper-scan") {
            oamdB2aHelperScan = true;
        } else if (option == "--joc") {
            joc = true;
            emdf = true;
        } else if (option == "--joc-self-test") {
            jocSelfTest = true;
        } else if (option == "--joc-math-self-test") {
            jocMathSelfTest = true;
        } else if (option == "--joc-table" && index + 1 < argc) {
            jocTablePath = argv[++index];
        } else if (option == "--pcm") {
            pcm = true;
        } else if (option == "--joc-qmf") {
            jocQmf = true;
            joc = true;
            emdf = true;
            pcm = true;
        } else if (option == "--joc-gate6c") {
            jocGate6c = true;
            jocQmf = true;
            joc = true;
            oamd = true;
            emdf = true;
            pcm = true;
        } else if (option == "--joc-gate7b") {
            jocGate7b = true;
            jocGate6c = true;
            jocQmf = true;
            joc = true;
            oamd = true;
            emdf = true;
            pcm = true;
        } else if (option == "--joc-gate7c") {
            jocGate7c = true;
            jocGate7b = true;
            jocGate6c = true;
            jocQmf = true;
            joc = true;
            oamd = true;
            emdf = true;
            pcm = true;
        } else if (option == "--spatial-disable-lfe") {
            rendererOptions.disableLfe = true;
        } else if (option == "--spatial-renderer" && index + 1 < argc) {
            const std::string value = argv[++index];
            if (value == "standard") {
                rendererOptions.spatialRenderer = eac3renderer::SpatialRenderer::Standard;
            } else if (value == "hrtf") {
                rendererOptions.spatialRenderer = eac3renderer::SpatialRenderer::Hrtf;
            } else {
                printUsage();
                return 2;
            }
        } else if (option == "--spatial-hrtf-environment" && index + 1 < argc) {
            const std::string value = argv[++index];
            if (value == "small") {
                rendererOptions.hrtfEnvironment = eac3renderer::HrtfEnvironment::Small;
            } else if (value == "outdoors") {
                rendererOptions.hrtfEnvironment = eac3renderer::HrtfEnvironment::Outdoors;
            } else {
                printUsage();
                return 2;
            }
        } else if (option == "--spatial-position-radius" && index + 1 < argc) {
            const std::string value = argv[++index];
            if (value == "source") {
                rendererOptions.positionRadiusMode =
                    eac3gate7b::PositionRadiusMode::Source;
            } else if (value == "unit") {
                rendererOptions.positionRadiusMode =
                    eac3gate7b::PositionRadiusMode::Unit;
            } else {
                printUsage();
                return 2;
            }
        } else if (option == "--spatial-position-direction" && index + 1 < argc) {
            const std::string value = argv[++index];
            if (value == "metadata") {
                rendererOptions.positionDirectionMode =
                    eac3gate7b::PositionDirectionMode::Metadata;
            } else if (value == "front") {
                rendererOptions.positionDirectionMode =
                    eac3gate7b::PositionDirectionMode::Front;
            } else {
                printUsage();
                return 2;
            }
        } else if (option == "--spatial-azimuth-focus" && index + 1 < argc) {
            try {
                const std::string value = argv[++index];
                std::size_t parsedLength = 0;
                const double parsed = std::stod(value, &parsedLength);
                if (parsedLength != value.size() || !std::isfinite(parsed)
                    || parsed < 0.0 || parsed > 1.0) {
                    printUsage();
                    return 2;
                }
                rendererOptions.azimuthFocus = static_cast<float>(parsed);
            } catch (...) {
                printUsage();
                return 2;
            }
        } else if (option == "--eac3-drc-scale" && index + 1 < argc) {
            try {
                const std::string value = argv[++index];
                std::size_t parsed = 0;
                const double drcScale = std::stod(value, &parsed);
                if (parsed != value.size() || !std::isfinite(drcScale)
                    || drcScale < 0.0 || drcScale > 6.0) {
                    printUsage();
                    return 2;
                }
                decodeOptions.drcScale = drcScale;
            } catch (...) {
                printUsage();
                return 2;
            }
        } else if (option == "--eac3-cons-noisegen" && index + 1 < argc) {
            const std::string value = argv[++index];
            if (value == "0") {
                decodeOptions.consistentNoiseGeneration = false;
            } else if (value == "1") {
                decodeOptions.consistentNoiseGeneration = true;
            } else {
                printUsage();
                return 2;
            }
        } else if (option == "--gate7c-queue-batches" && index + 1 < argc) {
            try {
                rendererOptions.queueBatches = static_cast<std::size_t>(
                    std::stoull(argv[++index]));
            } catch (...) {
                printUsage();
                return 2;
            }
        } else if (option == "--gate7c-prebuffer-batches" && index + 1 < argc) {
            try {
                rendererOptions.prebufferBatches = static_cast<std::size_t>(
                    std::stoull(argv[++index]));
            } catch (...) {
                printUsage();
                return 2;
            }
        } else if (option == "--gate7c-push-timeout-ms" && index + 1 < argc) {
            try {
                rendererOptions.pushTimeoutMilliseconds = static_cast<std::uint32_t>(
                    std::stoul(argv[++index]));
            } catch (...) {
                printUsage();
                return 2;
            }
        } else if (option == "--summary") {
            summaryOnly = true;
        } else if (option == "--dump-eac3" && index + 1 < argc) {
            dumpEac3Path = argv[++index];
        } else if (option == "--joc-bear-export" && index + 1 < argc) {
            jocBearExportPath = argv[++index];
        } else if (option == "--joc-matrix-trace" && index + 1 < argc) {
            jocMatrixTracePath = argv[++index];
        } else if (option == "--audio-stream-index" && index + 1 < argc) {
            try {
                requestedAudioStream = std::stoi(argv[++index]);
                if (requestedAudioStream < 0)
                    throw std::out_of_range("audio-stream-index");
            } catch (...) {
                printUsage();
                return 2;
            }
        } else {
            printUsage();
            return 2;
        }
    }
    if (joc) {
        jocSelfTest = true;
        jocMathSelfTest = true;
    }
    if (jocQmf) {
        std::string qwinReason;
        const bool qwinPass = eac3qmf::loadQwin(jocTablePath, &jocQmfQwin, &qwinReason);
        std::cout << "jocQmfPrototypeLoaded=" << (qwinPass ? "PASS" : "FAIL")
                  << " reason=" << qwinReason << '\n';
        if (!qwinPass) {
            return 1;
        }
    }
    if (!jocMatrixTracePath.empty() && (!pcm || !jocGate6c)) {
        std::cerr << "jocMatrixTrace=FAIL reason=requires-pcm-and-joc-gate6c\n";
        return 2;
    }
    if (jocGate7c) {
        std::string optionsReason;
        if (!eac3renderer::validateOptions(rendererOptions, &optionsReason)) {
            std::cerr << "gate7cResult=FAIL stage=options reason=" << optionsReason << '\n';
            return 2;
        }
    }
    if (rendererOptions.disableLfe && !jocGate7c) {
        std::cerr << "--spatial-disable-lfe requires --joc-gate7c\n";
        return 2;
    }
    if (selfTest && !runSelfTest()) {
        return 1;
    }
    eac3joc::HuffmanSelfTestReport jocSelfTestReport;
    eac3joc::JocMathSelfTestReport jocMathSelfTestReport;
    if (jocSelfTest) {
        jocSelfTestReport = eac3joc::runHuffmanSelfTest(jocTablePath);
        std::cout << "jocHuffmanSelfTest="
                  << (jocSelfTestReport.pass ? "PASS" : "FAIL")
                  << " tables=" << jocSelfTestReport.tableCount
                  << " leaves=" << jocSelfTestReport.leafCount
                  << " reason=" << jocSelfTestReport.reason << '\n';
    }
    if (jocMathSelfTest) {
        jocMathSelfTestReport = eac3joc::runMathSelfTest();
        std::cout << "jocMathSelfTest="
                  << (jocMathSelfTestReport.pass ? "PASS" : "FAIL")
                  << " mappingCases=" << jocMathSelfTestReport.mappingCases
                  << " interpolationCases=" << jocMathSelfTestReport.interpolationCases
                  << " reason=" << jocMathSelfTestReport.reason << '\n';
    }
    if (selfTest && !joc && !jocSelfTest && !jocMathSelfTest) {
        return 0;
    }

    std::vector<Chunk> chunks;
    std::string loadReason;
    if (!loadChunks(argv[1], pcm, requestedAudioStream,
                    &chunks, &loadReason)) {
        std::cerr << "probeResult=FAIL stage=load reason=" << loadReason << '\n';
        return 1;
    }
    AccessUnitAssembler assembler;
    ProbeResult report;
    std::size_t consumedChunkCount = 0;
    for (std::size_t chunkIndex = 0; chunkIndex < chunks.size(); ++chunkIndex) {
        const Chunk &chunk = chunks[chunkIndex];
        if (!assembler.consume(chunk.bytes, chunk.source, maxUnits, &report)) {
            std::cerr << "probeResult=FAIL stage=assemble reason=" << report.reason << '\n';
            return 1;
        }
        consumedChunkCount = chunkIndex + 1;
        if (maxUnits != 0 && report.units.size() >= maxUnits) {
            break;
        }
    }
    if (!assembler.finish(&report)) {
        std::cerr << "probeResult=FAIL stage=finish reason=" << report.reason << '\n';
        return 1;
    }

    if (!dumpEac3Path.empty()) {
        std::ofstream output(dumpEac3Path, std::ios::binary | std::ios::trunc);
        if (!output) {
            std::cerr << "dumpEac3=FAIL reason=open-failed path="
                      << dumpEac3Path << '\n';
            return 1;
        }
        std::uint64_t dumpedBytes = 0U;
        for (const AccessUnit &unit : report.units) {
            if (!unit.compressedBytes.empty()) {
                output.write(reinterpret_cast<const char *>(
                                 unit.compressedBytes.data()),
                             static_cast<std::streamsize>(
                                 unit.compressedBytes.size()));
                dumpedBytes += unit.compressedBytes.size();
            }
        }
        if (!output) {
            std::cerr << "dumpEac3=FAIL reason=write-failed path="
                      << dumpEac3Path << '\n';
            return 1;
        }
        std::cout << "dumpEac3=PASS path=" << dumpEac3Path
                  << " units=" << report.units.size()
                  << " bytes=" << dumpedBytes
                  << " codecCopy=YES transcode=NO\n";
    }

    for (std::size_t index = 0; !summaryOnly && index < report.units.size(); ++index) {
        const AccessUnit &unit = report.units[index];
        const bool complete = std::all_of(unit.blocksByStream.begin(), unit.blocksByStream.end(),
                                          [](const auto &entry) { return entry.second == 6; });
        std::cout << "accessUnit[" << index << "]"
                  << " frames=" << unit.frames.size()
                  << " bytes=" << unit.compressedBytes.size()
                  << " sampleRate=" << unit.sampleRate
                  << " samples=" << (complete ? 1536 : 0)
                  << " streams=";
        bool first = true;
        for (const auto &[key, blocks] : unit.blocksByStream) {
            if (!first) {
                std::cout << ',';
            }
            first = false;
            std::cout << key << ':' << blocks;
        }
        std::cout << '\n';
        for (const Syncframe &frame : unit.frames) {
            std::cout << "  frame offset=" << frame.offset
                      << " startBit=" << frame.startBit
                      << " endBit=" << frame.endBit
                      << " type=" << frameTypeText(frame.frameType)
                      << " sid=" << frame.substreamId
                      << " bytes=" << frame.sizeBytes
                      << " blocks=" << frame.blocks
                      << " bsid=" << frame.bsid << '\n';
        }
    }
    const bool everyUnitIs1536 = std::all_of(report.units.begin(), report.units.end(), [](const AccessUnit &unit) {
        return unit.sampleRate == 48000
            && !unit.frames.empty()
            && std::all_of(unit.blocksByStream.begin(), unit.blocksByStream.end(),
                           [](const auto &entry) { return entry.second == 6; });
    });
    std::size_t unitFrameCount = 0;
    std::size_t unitAc3ConvertCount = 0;
    std::size_t unitIndependentCount = 0;
    std::size_t unitDependentCount = 0;
    for (const AccessUnit &unit : report.units) {
        unitFrameCount += unit.frames.size();
        for (const Syncframe &frame : unit.frames) {
            if (frame.frameType == FrameType::Ac3Convert) {
                ++unitAc3ConvertCount;
            } else if (frame.frameType == FrameType::Independent) {
                ++unitIndependentCount;
            } else if (frame.frameType == FrameType::Dependent) {
                ++unitDependentCount;
            }
        }
    }
    std::cout << "inputChunks=" << chunks.size() << '\n'
              << "accessUnits=" << report.units.size() << '\n'
              << "syncframes=" << unitFrameCount << '\n'
              << "ac3ConvertFrames=" << unitAc3ConvertCount << '\n'
              << "independentFrames=" << unitIndependentCount << '\n'
              << "dependentFrames=" << unitDependentCount << '\n'
              << "reservedFrames=0\n"
              << "everyAccessUnit1536At48000=" << (everyUnitIs1536 ? "PASS" : "FAIL") << '\n'
              << "probeResult=" << (everyUnitIs1536 ? "PASS" : "FAIL")
              << " stage=gate1-access-unit-assembler implementation=self-written-ac3-eac3-header-parser\n";
    bool jocPass = true;
    bool jocMathPass = true;
    bool oamdPass = true;
    bool oamdB2aPass = true;
    bool oamdB2bPass = true;
    std::vector<JocPayloadRecord> jocPayloadRecords;
    std::vector<std::size_t> jocMathUnitIndices;
    std::vector<OamdPayloadRecord> oamdPayloadRecords;
    OamdB1Metrics oamdMetrics;
    OamdB2aMetrics oamdB2aMetrics;
    OamdB2bMetrics oamdB2bMetrics;
    eac3oamd::B2bState oamdB2bState;
    std::vector<Gate6cMetadataRecord> gate6cMetadata;
    oamdB2aMetrics.bitOrder = oamdB2aBitOrder;
    std::map<std::size_t, std::size_t> oamdB2aUnitObjectElements;
    std::vector<OamdB2aHelperScanResult> oamdB2aHelperScanResults;
    bool oamdB2aHelperScanDone = false;
    if (emdf) {
        std::size_t containerCount = 0;
        std::size_t oamdCount = 0;
        std::size_t jocCount = 0;
        std::size_t targetContainerCount = 0;
        std::size_t targetContainerOnExpectedStream = 0;
        std::size_t annexHPayloadCount = 0;
        std::size_t annexHPayloadBytes = 0;
        std::size_t annexHParsePass = 0;
        std::size_t annexHParseUnsupported = 0;
        std::size_t annexHParseMalformed = 0;
        std::size_t annexHPending = 0;
        std::size_t annexHCompleted = 0;
        std::size_t annexHActiveAtEnd = 0;
        std::size_t annexHPartialCanceled = 0;
        eac3annexh::HeadphonePayloadRouter annexHRouter;
        std::map<unsigned, std::size_t> payloadCounts;
        eac3joc::JocMatrixState jocMathState;
        std::size_t jocMathPassCount = 0;
        std::size_t jocMathFailCount = 0;
        std::size_t jocMathResetCount = 0;
        for (std::size_t unitIndex = 0; unitIndex < report.units.size(); ++unitIndex) {
            const AccessUnit &unit = report.units[unitIndex];
            const bool hasDependentStream = std::any_of(unit.frames.begin(), unit.frames.end(),
                                                        [](const Syncframe &frame) {
                                                            return frame.frameType == FrameType::Dependent;
                                                        });
            for (std::size_t frameOrdinal = 0U;
                 frameOrdinal < unit.frames.size(); ++frameOrdinal) {
                const Syncframe &frame = unit.frames[frameOrdinal];
                const std::vector<EmdfContainer> containers = scanEmdf(unit, frame);
                for (const EmdfContainer &container : containers) {
                    ++containerCount;
                    std::cout << "emdf unit=" << unitIndex
                              << " frameType=" << frameTypeText(frame.frameType)
                              << " sid=" << frame.substreamId
                              << " startBit=" << container.startBit
                              << " lengthBytes=" << container.lengthBytes
                              << " version=" << container.version
                              << " keyId=" << container.keyId
                              << " payloads=" << container.payloads.size() << '\n';
                    for (const EmdfPayload &payload : container.payloads) {
                        ++payloadCounts[payload.id];
                        if (payload.id == 11) {
                            ++oamdCount;
                        } else if (payload.id == 14) {
                            ++jocCount;
                        } else if (payload.id == 7) {
                            ++annexHPayloadCount;
                            annexHPayloadBytes += payload.bytes.size();
                            eac3annexh::PayloadContext context;
                            context.accessUnitOrdinal = unit.ordinal;
                            context.frameOrdinal = frameOrdinal;
                            context.timestampSamples = unit.timestampSamples;
                            context.frameType =
                                static_cast<unsigned>(frame.frameType);
                            context.substreamId = frame.substreamId;
                            context.blocks = frame.blocks;
                            context.ac3Frame = frame.frameType == FrameType::Ac3Convert;
                            const auto routed = annexHRouter.ingest(
                                context, payload.bytes);
                            std::cout << "  annexH accessUnitOrdinal="
                                      << context.accessUnitOrdinal
                                      << " frameOrdinal=" << context.frameOrdinal
                                      << " timestampSamples="
                                      << context.timestampSamples
                                      << " frameType=" << frameTypeText(frame.frameType)
                                      << " substreamId=" << context.substreamId
                                      << " disposition="
                                      << static_cast<int>(routed.disposition)
                                      << " reason=" << routed.reason << '\n';
                            if (routed.disposition
                                == eac3annexh::ReassemblyDisposition::Pending) {
                                ++annexHPending;
                            } else if (routed.disposition
                                       == eac3annexh::ReassemblyDisposition::Pass) {
                                ++annexHParsePass;
                                ++annexHCompleted;
                                std::cout << "  annexHComplete channelCount="
                                          << routed.data.channelCount
                                          << " sequenceFrames="
                                          << routed.data.sequenceFrames
                                          << " earlyChunksPerChannel="
                                          << routed.data.earlyChunkCountPerChannel
                                          << " lateChunks=" << routed.data.lateChunkCount
                                          << " raw24NoScale=YES\n";
                            } else if (routed.disposition
                                       == eac3annexh::ReassemblyDisposition::Unsupported) {
                                ++annexHParseUnsupported;
                            } else {
                                ++annexHParseMalformed;
                            }
                        }
                        std::cout << "  payloadId=" << payload.id
                                  << " sizeBytes=" << payload.sizeBytes
                                  << " sampleOffsetExists=" << (payload.sampleOffsetExists ? 1 : 0)
                                  << " sampleOffset=" << payload.sampleOffset
                                  << " durationExists=" << (payload.durationExists ? 1 : 0)
                                  << " duration=" << payload.duration
                                  << " groupIdExists=" << (payload.groupIdExists ? 1 : 0)
                                  << " groupId=" << payload.groupId
                                  << " discardUnknownPayload=" << (payload.discardUnknownPayload ? 1 : 0)
                                  << " frameAligned=" << (payload.frameAligned ? 1 : 0)
                                  << " priority=" << payload.priority
                                  << " procAllowed=" << payload.procAllowed << '\n';
                        if (oamd && payload.id == 11) {
                            OamdPayloadRecord record;
                            record.unitIndex = unitIndex;
                            record.frame = eac3oamd::parseB1(payload.bytes);
                            oamdPayloadRecords.push_back(record);
                            ++oamdMetrics.payloadCount;
                            ++oamdMetrics.unitAssociationCount;
                            if (record.frame.disposition == eac3oamd::B1Disposition::Pass) {
                                ++oamdMetrics.passCount;
                            } else if (record.frame.disposition
                                       == eac3oamd::B1Disposition::Unsupported) {
                                ++oamdMetrics.unsupportedCount;
                            } else {
                                ++oamdMetrics.malformedCount;
                            }
                            oamdMetrics.versionMin = std::min(oamdMetrics.versionMin,
                                                              record.frame.version);
                            oamdMetrics.versionMax = std::max(oamdMetrics.versionMax,
                                                              record.frame.version);
                            oamdMetrics.objectCountMin = std::min(oamdMetrics.objectCountMin,
                                                                   record.frame.objectCount);
                            oamdMetrics.objectCountMax = std::max(oamdMetrics.objectCountMax,
                                                                   record.frame.objectCount);
                            if (record.frame.program.lfePresent) {
                                ++oamdMetrics.lfeDeclaredCount;
                            }
                            oamdMetrics.finalPaddingBits += record.frame.finalPaddingBits;
                            ++oamdMetrics.programTypes[record.frame.program.programType];
                            for (const eac3oamd::ElementInventory &element
                                 : record.frame.elements) {
                                ++oamdMetrics.elementIdCounts[element.id];
                                ++oamdMetrics.elementSizeCounts[
                                    {element.id, element.sizeBytes}];
                                if (element.recognized) {
                                    ++oamdMetrics.recognizedElementCount;
                                } else if (element.discardUnknown) {
                                    ++oamdMetrics.unknownDiscardableCount;
                                } else {
                                    ++oamdMetrics.unknownNondiscardableCount;
                                }
                            }
                            ++oamdB2aMetrics.payloadCount;
                            std::size_t objectElementCount = 0;
                            for (const eac3oamd::ElementInventory &element
                                 : record.frame.elements) {
                                if (element.id != 1U) {
                                    continue;
                                }
                                ++objectElementCount;
                                ++oamdB2aMetrics.objectElementCount;
                                if (record.frame.disposition != eac3oamd::B1Disposition::Pass) {
                                    continue;
                                }
                                if (!record.frame.program.dynamicOnly) {
                                    ++oamdB2aMetrics.unsupportedCount;
                                    continue;
                                }
                                if (oamdB2aHelperScan && !oamdB2aHelperScanDone
                                    && record.frame.objectCount == 16U) {
                                    const eac3oamd::B2aBitOrder scanOrders[] = {
                                        eac3oamd::B2aBitOrder::Syntax5511Lsb,
                                        eac3oamd::B2aBitOrder::Table31Msb,
                                    };
                                    for (const eac3oamd::B2aBitOrder scanOrder : scanOrders) {
                                        for (unsigned helper = 0; helper < 16U; ++helper) {
                                            std::vector<bool> helperMap(16U, false);
                                            helperMap[helper] = true;
                                            const eac3oamd::B2aFrame candidate =
                                                eac3oamd::parseObjectElement(
                                                    element.rawBody, element.rawBodyBits,
                                                    16U, helperMap, scanOrder);
                                            oamdB2aHelperScanResults.push_back(
                                                OamdB2aHelperScanResult {
                                                    helper,
                                                    scanOrder,
                                                    candidate.disposition,
                                                    candidate.reason,
                                                    candidate.bitsConsumed,
                                                    candidate.paddingBits,
                                                    candidate.failureBitOffset,
                                                    candidate.failureObjectIndex,
                                                    candidate.failureBlockIndex,
                                                });
                                        }
                                    }
                                    oamdB2aHelperScanDone = true;
                                }
                                const unsigned parseObjectCount = oamdB2aControl15
                                    ? 15U : record.frame.objectCount;
                                std::vector<bool> objectInBedOrIsf(parseObjectCount, false);
                                if (!oamdB2aControl15
                                    && record.frame.program.dynamicOnly
                                    && record.frame.program.lfePresent
                                    && !objectInBedOrIsf.empty()) {
                                    // Dynamic-only + declared LFE creates one
                                    // LFE-only bed record at object index 0;
                                    // the object element still carries all
                                    // declared object records.
                                    objectInBedOrIsf[0] = true;
                                    ++oamdB2aMetrics.lfeHelperMapCount;
                                }
                                const eac3oamd::B2aFrame b2a =
                                    eac3oamd::parseObjectElement(
                                        element.rawBody, element.rawBodyBits,
                                        parseObjectCount, objectInBedOrIsf,
                                        oamdB2aBitOrder);
                                oamdB2aMetrics.sampleOffsetCodes[b2a.sampleOffsetCode]++;
                                if (b2a.disposition == eac3oamd::B2aDisposition::Pass) {
                                    ++oamdB2aMetrics.passCount;
                                    ++oamdB2aMetrics.unitAssociationCount;
                                    ++oamdB2aMetrics.blockCounts[b2a.objectInfoBlockCount];
                                    oamdB2aMetrics.bitsConsumed += b2a.bitsConsumed;
                                    oamdB2aMetrics.paddingBits += b2a.paddingBits;
                                    for (const eac3oamd::B2aBlockUpdate &block : b2a.blocks) {
                                        ++oamdB2aMetrics.blockOffsetFactors[
                                            block.blockOffsetFactor];
                                        ++oamdB2aMetrics.rampDurationCodes[
                                            block.rampDurationCode];
                                    }
                                    for (const eac3oamd::B2aObjectInfo &object
                                         : b2a.objectInfo) {
                                        ++oamdB2aMetrics.objectInfoRecordCount;
                                        ++oamdB2aMetrics.basicStatuses[object.basicInfoStatus];
                                        ++oamdB2aMetrics.renderStatuses[object.renderInfoStatus];
                                        if (object.objectNotActive) {
                                            ++oamdB2aMetrics.inactiveCount;
                                            ++oamdB2aMetrics.inactiveByObject[object.objectIndex];
                                        } else {
                                            ++oamdB2aMetrics.activeCount;
                                            ++oamdB2aMetrics.activeByObject[object.objectIndex];
                                        }
                                        unsigned positionMode = 0;
                                        if (object.renderInfoPresence[0]) {
                                            positionMode = object.differentialPosition ? 2U : 1U;
                                        }
                                        ++oamdB2aMetrics.positionModes[positionMode];
                                        unsigned distanceMode = object.distanceSpecified
                                            ? (object.objectAtInfinity ? 2U : 1U) : 0U;
                                        ++oamdB2aMetrics.distanceModes[distanceMode];
                                        ++oamdB2aMetrics.sizeIndices[
                                            object.sizePresent ? object.sizeIndex : 0U];
                                        unsigned screenMode = object.screenReferencePresent
                                            ? (object.useScreenReference ? 2U : 1U) : 0U;
                                        ++oamdB2aMetrics.screenModes[screenMode];
                                        ++oamdB2aMetrics.snapValues[object.snap ? 1U : 0U];
                                        if (object.additionalDataPresent) {
                                            ++oamdB2aMetrics.additionalDataCount;
                                            oamdB2aMetrics.additionalDataBytes +=
                                                object.additionalDataSizeBytes;
                                        }
                                    }
                                    ++oamdB2bMetrics.payloadCount;
                                    if (!oamdB2aControl15) {
                                        const bool hasAdditionalElements = std::any_of(
                                            record.frame.elements.begin(),
                                            record.frame.elements.end(),
                                            [](const eac3oamd::ElementInventory &element) {
                                                return element.id == 2U || element.id == 5U;
                                            });
                                        eac3oamd::B2bFrameOutput b2bOutput;
                                        // MONTERO and the bounded oracle fixtures carry
                                        // reserved trim warp_mode=3. Keep the standard
                                        // B2B object state for renderer diagnostics while
                                        // explicitly excluding unsupported additional
                                        // elements from their Gate6C metadata state.
                                        const eac3oamd::B1Frame *additionalPolicyFrame =
                                            hasAdditionalElements
                                            && (!jocBearExportPath.empty()
                                                || !jocMatrixTracePath.empty()
                                                || jocGate7c)
                                            ? nullptr : &record.frame;
                                        const eac3oamd::B2bApplyResult b2bResult =
                                            oamdB2bState.applyFrame(
                                                b2a, objectInBedOrIsf, &b2bOutput,
                                                false, additionalPolicyFrame);
                                if (b2bResult.disposition
                                            == eac3oamd::B2bDisposition::Pass) {
                                            ++oamdB2bMetrics.stateAppliedCount;
                                            if (b2bOutput.trimPresent) {
                                                ++oamdB2bMetrics.trimElementCount;
                                            }
                                            bool extendedPresent = false;
                                            for (const eac3oamd::B2bObjectState &state
                                                 : b2bOutput.objects) {
                                                extendedPresent = extendedPresent
                                                    || state.divergencePresent
                                                    || state.position.extendedPrecisionPresent
                                                        != std::array<bool, 3> {};
                                            }
                                            if (extendedPresent) {
                                                ++oamdB2bMetrics.extendedElementCount;
                                            }
                                            if (hasAdditionalElements) {
                                                ++oamdB2bMetrics.additionalElementFailureCount;
                                            }
                                            const eac3oamd::B2bApplyStats &stats = b2bResult.stats;
                                            oamdB2bMetrics.resetCount += stats.resetCount;
                                            oamdB2bMetrics.defaultBasicCount += stats.defaultBasicCount;
                                            oamdB2bMetrics.fullBasicCount += stats.fullBasicCount;
                                            oamdB2bMetrics.reuseBasicCount += stats.reuseBasicCount;
                                            oamdB2bMetrics.mixedBasicCount += stats.mixedBasicCount;
                                            oamdB2bMetrics.defaultRenderCount += stats.defaultRenderCount;
                                            oamdB2bMetrics.fullRenderCount += stats.fullRenderCount;
                                            oamdB2bMetrics.reuseRenderCount += stats.reuseRenderCount;
                                            oamdB2bMetrics.mixedRenderCount += stats.mixedRenderCount;
                                            oamdB2bMetrics.absolutePositionCount += stats.absolutePositionCount;
                                            oamdB2bMetrics.differentialPositionCount += stats.differentialPositionCount;
                                            oamdB2bMetrics.activeCount += stats.activeCount;
                                            oamdB2bMetrics.inactiveCount += stats.inactiveCount;
                                            oamdB2bMetrics.finitePropertyCount += stats.finitePropertyCount;
                                            oamdB2bMetrics.minusInfinityGainCount += stats.minusInfinityGainCount;
                                            if (!objectInBedOrIsf.empty() && objectInBedOrIsf[0]) {
                                                ++oamdB2bMetrics.lfeHelperCount;
                                            }
                                            const std::size_t dynamicCount =
                                                b2bOutput.dynamicObjectIndices.size();
                                            oamdB2bMetrics.dynamicObjectCountMin = std::min(
                                                oamdB2bMetrics.dynamicObjectCountMin, dynamicCount);
                                            oamdB2bMetrics.dynamicObjectCountMax = std::max(
                                                oamdB2bMetrics.dynamicObjectCountMax, dynamicCount);
                                            bool dynamicOrder = true;
                                            for (std::size_t index = 0; index < dynamicCount; ++index) {
                                                dynamicOrder = dynamicOrder
                                                    && b2bOutput.dynamicObjectIndices[index]
                                                        == index + (objectInBedOrIsf[0] ? 1U : 0U);
                                            }
                                            if (dynamicOrder) {
                                                ++oamdB2bMetrics.dynamicObjectOrderPassCount;
                                            }
                                            if (jocGate6c && !objectInBedOrIsf.empty()
                                                && objectInBedOrIsf[0]
                                                && b2bOutput.objects.size() == 16U) {
                                                Gate6cMetadataRecord metadataRecord;
                                                metadataRecord.unitIndex = unitIndex;
                                                std::int64_t sampleOffset = 0;
                                                std::string timingReason;
                                                bool timingValid = eac3gate6c::decodeSampleOffset(
                                                    b2a, &sampleOffset, &timingReason);
                                                std::vector<unsigned> rampDurations;
                                                for (const eac3oamd::B2aBlockUpdate &block : b2a.blocks) {
                                                    unsigned rampSamples = 0;
                                                    if (!eac3gate6c::decodeRampDuration(
                                                            block, &rampSamples, &timingReason)) {
                                                        timingValid = false;
                                                        break;
                                                    }
                                                    rampDurations.push_back(rampSamples);
                                                }
                                                if (!timingValid) {
                                                    ++oamdB2bMetrics.fallbackCount;
                                                    if (oamdB2bMetrics.firstFailureReason.empty()) {
                                                        oamdB2bMetrics.firstFailureReason = timingReason;
                                                    }
                                                    continue;
                                                }
                                                const std::int64_t unitBase =
                                                    static_cast<std::int64_t>(unitIndex) * 1536;
                                                for (const eac3oamd::B2bFrameOutput::BlockSnapshot &snapshot
                                                     : b2bOutput.blockSnapshots) {
                                                    if (snapshot.blockIndex >= b2a.blocks.size()) {
                                                        metadataRecord.updates.clear();
                                                        break;
                                                    }
                                                    const eac3oamd::B2aBlockUpdate &block =
                                                        b2a.blocks[snapshot.blockIndex];
                                                    const std::int64_t position = unitBase
                                                        + sampleOffset
                                                        + static_cast<std::int64_t>(32)
                                                            * block.blockOffsetFactor;
                                                    for (unsigned objectIndex = 1U;
                                                         objectIndex <= 15U; ++objectIndex) {
                                                        if (objectIndex >= snapshot.objects.size()) {
                                                            metadataRecord.updates.clear();
                                                            break;
                                                        }
                                                        metadataRecord.updates.push_back(
                                                            eac3gate6c::MetadataUpdate {
                                                        position,
                                                                snapshot.blockIndex,
                                                                rampDurations[snapshot.blockIndex],
                                                                objectIndex,
                                                                snapshot.objects[objectIndex]});
                                                    }
                                                    if (metadataRecord.updates.empty()) break;
                                                }
                                                if (!metadataRecord.updates.empty()) {
                                                    gate6cMetadata.push_back(std::move(metadataRecord));
                                                }
                                            }
                                        } else {
                                            ++oamdB2bMetrics.fallbackCount;
                                            if (hasAdditionalElements) {
                                                ++oamdB2bMetrics.additionalElementFailureCount;
                                            }
                                            if (oamdB2bMetrics.firstFailureReason.empty()) {
                                                oamdB2bMetrics.firstFailureReason = b2bResult.reason;
                                            }
                                        }
                                    } else {
                                        ++oamdB2bMetrics.fallbackCount;
                                        if (oamdB2bMetrics.firstFailureReason.empty()) {
                                            oamdB2bMetrics.firstFailureReason =
                                                "control-15-object-count-is-not-declared-state";
                                        }
                                    }
                                } else if (b2a.disposition
                                           == eac3oamd::B2aDisposition::Unsupported) {
                                    ++oamdB2aMetrics.unsupportedCount;
                                } else {
                                    ++oamdB2aMetrics.malformedCount;
                                    if (oamdB2aMetrics.firstFailureReason.empty()) {
                                        oamdB2aMetrics.firstFailureReason = b2a.reason;
                                        oamdB2aMetrics.firstFailureBitOffset = b2a.failureBitOffset;
                                        oamdB2aMetrics.firstFailureObjectIndex =
                                            b2a.failureObjectIndex;
                                        oamdB2aMetrics.firstFailureBlockIndex =
                                            b2a.failureBlockIndex;
                                    }
                                }
                            }
                            if (objectElementCount != 1U) {
                                if (objectElementCount > 1U) {
                                    ++oamdB2aMetrics.multipleObjectElements;
                                }
                                ++oamdB2aMetrics.malformedCount;
                            } else if (record.frame.disposition
                                       != eac3oamd::B1Disposition::Pass) {
                                ++oamdB2aMetrics.malformedCount;
                            }
                            oamdB2aUnitObjectElements[unitIndex] += objectElementCount;
                        }
                        if (joc && payload.id == 14) {
                            eac3joc::FrameReport jocReport =
                                eac3joc::parsePayload(payload.bytes, jocTablePath,
                                                       !jocMatrixTracePath.empty());
                            jocPayloadRecords.push_back(JocPayloadRecord {unitIndex, jocReport});
                            std::cout << "  joc disposition="
                                      << eac3joc::dispositionText(jocReport.disposition)
                                      << " config=" << jocReport.downmixConfigIndex
                                      << " channels=" << jocReport.numChannels
                                      << " objects=" << jocReport.numObjects
                                      << " sequence=" << jocReport.sequenceCount
                                      << " rawBytes=" << payload.bytes.size()
                                      << " bits=" << jocReport.bitsConsumed
                                      << " paddingBits=" << jocReport.paddingBits
                                      << " reason=" << jocReport.reason << '\n';
                            if (jocReport.disposition != eac3joc::ParseDisposition::Malformed) {
                                eac3joc::JocMathFrame mathFrame;
                                std::string mathReason;
                                if (eac3joc::processMath(jocReport, 24, &jocMathState,
                                                         &mathFrame, &mathReason)) {
                                    ++jocMathPassCount;
                                    jocMathUnitIndices.push_back(unitIndex);
                                    if (mathFrame.stateReset) {
                                        ++jocMathResetCount;
                                    }
                                } else {
                                    ++jocMathFailCount;
                                    std::cout << "  jocMath disposition=FAIL reason="
                                              << mathReason << '\n';
                                }
                            }
                        }
                    }
                    const bool hasTargetPayload = std::any_of(container.payloads.begin(),
                                                              container.payloads.end(),
                                                              [](const EmdfPayload &payload) {
                                                                  return payload.id == 11 || payload.id == 14;
                                                              });
                    if (hasTargetPayload) {
                        ++targetContainerCount;
                        if ((hasDependentStream && frame.frameType == FrameType::Dependent)
                            || (!hasDependentStream && frame.frameType == FrameType::Independent)) {
                            ++targetContainerOnExpectedStream;
                        }
                    }
                }
            }
        }
        annexHActiveAtEnd = annexHRouter.activeStreams();
        annexHPartialCanceled = annexHRouter.cancel();
        const bool emdfPass = oamdCount > 0 && jocCount > 0;
        const bool placementPass = targetContainerCount > 0
            && targetContainerCount == targetContainerOnExpectedStream;
        std::cout << "emdfContainerCount=" << containerCount << '\n'
                  << "oamdPayloadCount=" << oamdCount << '\n'
                  << "jocPayloadCount=" << jocCount << '\n'
                  << "targetPayloadContainerPlacement=" << (placementPass ? "PASS" : "INCONCLUSIVE") << '\n'
                  << "emdfPayloadCounts=";
        bool firstPayload = true;
        for (const auto &[payloadId, count] : payloadCounts) {
            if (!firstPayload) {
                std::cout << ',';
            }
            firstPayload = false;
            std::cout << payloadId << ':' << count;
        }
        std::cout << '\n'
                  << "emdfAnnexHPayloadCount=" << annexHPayloadCount << '\n'
                  << "emdfAnnexHPayloadBytes=" << annexHPayloadBytes << '\n'
                  << "emdfAnnexHPresence="
                  << (annexHPayloadCount == 0U ? "ABSENT" : "PRESENT") << '\n'
                  << "emdfAnnexHParsePass=" << annexHParsePass << '\n'
                  << "emdfAnnexHParseUnsupported=" << annexHParseUnsupported << '\n'
                  << "emdfAnnexHParseMalformed=" << annexHParseMalformed << '\n'
                  << "emdfAnnexHPending=" << annexHPending << '\n'
                  << "emdfAnnexHCompletedSequences=" << annexHCompleted << '\n'
                  << "emdfAnnexHActiveAtEnd=" << annexHActiveAtEnd << '\n'
                  << "emdfAnnexHPartialCanceled=" << annexHPartialCanceled << '\n'
                  << "emdfAnnexHResult="
                  << (annexHPayloadCount == 0U ? "NO_DATA"
                      : (annexHParseMalformed == 0U
                             && annexHParseUnsupported == 0U
                             && annexHCompleted > 0U
                             && annexHActiveAtEnd == 0U ? "PASS" : "INCONCLUSIVE"))
                  << '\n'
                  << "emdfResult=" << (emdfPass && placementPass ? "PASS" : "INCONCLUSIVE")
                  << " stage=gate2-emdf-payload-extraction reason="
                  << (emdfPass && placementPass
                          ? "payload11-and-payload14-found-on-last-applicable-stream"
                          : "no-complete-oamd-joc-pair-on-expected-stream")
                  << '\n';
        if (oamd) {
            std::map<std::size_t, std::size_t> unitsWithPayload;
            bool unitAssociationPass = oamdPayloadRecords.size() == report.units.size();
            for (const OamdPayloadRecord &record : oamdPayloadRecords) {
                ++unitsWithPayload[record.unitIndex];
            }
            for (std::size_t unit = 0; unit < report.units.size(); ++unit) {
                if (unitsWithPayload[unit] != 1) {
                    unitAssociationPass = false;
                }
            }
            for (const auto &[unit, count] : unitsWithPayload) {
                if (count > 1) {
                    oamdMetrics.multiplePayloadUnits += 1;
                }
            }
            oamdPass = oamdMetrics.payloadCount > 0
                && oamdMetrics.malformedCount == 0
                && oamdMetrics.unsupportedCount == 0
                && unitAssociationPass;
            const unsigned versionMin = oamdMetrics.payloadCount == 0
                ? 0 : oamdMetrics.versionMin;
            const unsigned objectCountMin = oamdMetrics.payloadCount == 0
                ? 0 : oamdMetrics.objectCountMin;
            std::cout << "oamdB1PayloadCount=" << oamdMetrics.payloadCount << '\n'
                      << "oamdB1DispositionPass=" << oamdMetrics.passCount << '\n'
                      << "oamdB1DispositionUnsupported=" << oamdMetrics.unsupportedCount << '\n'
                      << "oamdB1DispositionMalformed=" << oamdMetrics.malformedCount << '\n'
                      << "oamdB1VersionRange=" << versionMin << '-'
                      << oamdMetrics.versionMax << '\n'
                      << "oamdB1ObjectCountRange=" << objectCountMin << '-'
                      << oamdMetrics.objectCountMax << '\n'
                      << "oamdB1ProgramTypes=";
            bool firstMetric = true;
            for (const auto &[program, count] : oamdMetrics.programTypes) {
                if (!firstMetric) std::cout << ',';
                firstMetric = false;
                std::cout << program << ':' << count;
            }
            std::cout << '\n'
                      << "oamdB1LfeDeclaredPayloads=" << oamdMetrics.lfeDeclaredCount << '\n'
                      << "oamdB1ElementIdCounts=";
            firstMetric = true;
            for (const auto &[id, count] : oamdMetrics.elementIdCounts) {
                if (!firstMetric) std::cout << ',';
                firstMetric = false;
                std::cout << id << ':' << count;
            }
            std::cout << '\n'
                      << "oamdB1ElementIdSizeCounts=";
            firstMetric = true;
            for (const auto &[idSize, count] : oamdMetrics.elementSizeCounts) {
                if (!firstMetric) std::cout << ',';
                firstMetric = false;
                std::cout << idSize.first << ':' << idSize.second << ':' << count;
            }
            std::cout << '\n'
                      << "oamdB1RecognizedElementCount="
                      << oamdMetrics.recognizedElementCount << '\n'
                      << "oamdB1UnknownDiscardableCount="
                      << oamdMetrics.unknownDiscardableCount << '\n'
                      << "oamdB1UnknownNondiscardableCount="
                      << oamdMetrics.unknownNondiscardableCount << '\n'
                      << "oamdB1FinalPaddingBits=" << oamdMetrics.finalPaddingBits << '\n'
                      << "oamdB1ElementPaddingPolicy=opaque-body-includes-declared-boundary\n"
                      << "oamdB1PayloadUnitAssociationCount="
                      << oamdMetrics.unitAssociationCount << '\n'
                      << "oamdB1PayloadUnitAssociation="
                      << (unitAssociationPass ? "PASS" : "INCONCLUSIVE") << '\n'
                      << "oamdB1MultiplePayloadUnits="
                      << oamdMetrics.multiplePayloadUnits << '\n'
                      << "oamdB1Result=" << (oamdPass ? "PASS" : "INCONCLUSIVE")
                      << " stage=gate6b1-oamd-framing-inventory\n";
            bool b2aAssociationPass = oamdB2aMetrics.payloadCount > 0
                && oamdB2aMetrics.payloadCount == report.units.size();
            for (std::size_t unit = 0; unit < report.units.size(); ++unit) {
                if (oamdB2aUnitObjectElements[unit] != 1U) {
                    b2aAssociationPass = false;
                }
            }
            oamdB2aPass = !oamdB2aControl15 && b2aAssociationPass
                && oamdB2aMetrics.objectElementCount == oamdB2aMetrics.payloadCount
                && oamdB2aMetrics.passCount == oamdB2aMetrics.payloadCount
                && oamdB2aMetrics.unsupportedCount == 0
                && oamdB2aMetrics.malformedCount == 0;
            const auto printB2aMap = [](const std::map<unsigned, std::size_t> &values) {
                bool first = true;
                for (const auto &[key, count] : values) {
                    if (!first) std::cout << ',';
                    first = false;
                    std::cout << key << ':' << count;
                }
            };
            const auto printB2aObjectMap = [](const std::map<unsigned, std::size_t> &values,
                                              unsigned objectCount) {
                for (unsigned object = 0; object < objectCount; ++object) {
                    if (object != 0) std::cout << ',';
                    const auto found = values.find(object);
                    std::cout << object << ':'
                              << (found == values.end() ? 0U : found->second);
                }
            };
            const unsigned b2aObjectCount = oamdMetrics.objectCountMax;
            std::cout << "oamdB2aBitOrder="
                      << eac3oamd::b2aBitOrderText(oamdB2aMetrics.bitOrder) << '\n'
                      << "oamdB2aObjectCountMode="
                      << (oamdB2aControl15 ? "control-15" : "declared-16") << '\n'
                      << "oamdB2aHelperMap="
                      << (oamdB2aControl15 ? "control-all-false" :
                          "dynamic-only+lfePresent:index0") << '\n'
                      << "oamdB2aLfeHelperMapCount="
                      << oamdB2aMetrics.lfeHelperMapCount << '\n'
                      << "oamdB2aPayloadCount=" << oamdB2aMetrics.payloadCount << '\n'
                      << "oamdB2aDispositionPass=" << oamdB2aMetrics.passCount << '\n'
                      << "oamdB2aDispositionUnsupported="
                      << oamdB2aMetrics.unsupportedCount << '\n'
                      << "oamdB2aDispositionMalformed="
                      << oamdB2aMetrics.malformedCount << '\n'
                      << "oamdB2aFirstFailureReason="
                      << oamdB2aMetrics.firstFailureReason << '\n'
                      << "oamdB2aFirstFailureObject="
                      << oamdB2aMetrics.firstFailureObjectIndex << '\n'
                      << "oamdB2aFirstFailureBlock="
                      << oamdB2aMetrics.firstFailureBlockIndex << '\n'
                      << "oamdB2aFirstFailureBitOffset="
                      << oamdB2aMetrics.firstFailureBitOffset << '\n'
                      << "oamdB2aObjectElementCount="
                      << oamdB2aMetrics.objectElementCount << '\n'
                      << "oamdB2aBlockCounts=";
            printB2aMap(oamdB2aMetrics.blockCounts);
            std::cout << '\n' << "oamdB2aSampleOffsetCodes=";
            printB2aMap(oamdB2aMetrics.sampleOffsetCodes);
            std::cout << '\n' << "oamdB2aBlockOffsetFactors=";
            printB2aMap(oamdB2aMetrics.blockOffsetFactors);
            std::cout << '\n' << "oamdB2aRampDurationCodes=";
            printB2aMap(oamdB2aMetrics.rampDurationCodes);
            std::cout << '\n' << "oamdB2aBasicStatuses=";
            printB2aMap(oamdB2aMetrics.basicStatuses);
            std::cout << '\n' << "oamdB2aRenderStatuses=";
            printB2aMap(oamdB2aMetrics.renderStatuses);
            std::cout << '\n' << "oamdB2aPositionModes=";
            printB2aMap(oamdB2aMetrics.positionModes);
            std::cout << '\n' << "oamdB2aDistanceModes=";
            printB2aMap(oamdB2aMetrics.distanceModes);
            std::cout << '\n' << "oamdB2aSizeIndices=";
            printB2aMap(oamdB2aMetrics.sizeIndices);
            std::cout << '\n' << "oamdB2aScreenModes=";
            printB2aMap(oamdB2aMetrics.screenModes);
            std::cout << '\n' << "oamdB2aSnapValues=";
            printB2aMap(oamdB2aMetrics.snapValues);
            std::cout << '\n'
                      << "oamdB2aObjectInfoRecords="
                      << oamdB2aMetrics.objectInfoRecordCount << '\n'
                      << "oamdB2aActiveCount=" << oamdB2aMetrics.activeCount << '\n'
                      << "oamdB2aInactiveCount=" << oamdB2aMetrics.inactiveCount << '\n'
                      << "oamdB2aActiveByObject=";
            printB2aObjectMap(oamdB2aMetrics.activeByObject, b2aObjectCount);
            std::cout << '\n' << "oamdB2aInactiveByObject=";
            printB2aObjectMap(oamdB2aMetrics.inactiveByObject, b2aObjectCount);
            std::cout << '\n'
                      << "oamdB2aAdditionalDataCount="
                      << oamdB2aMetrics.additionalDataCount << '\n'
                      << "oamdB2aAdditionalDataBytes="
                      << oamdB2aMetrics.additionalDataBytes << '\n'
                      << "oamdB2aBitsConsumed=" << oamdB2aMetrics.bitsConsumed << '\n'
                      << "oamdB2aPaddingBits=" << oamdB2aMetrics.paddingBits << '\n'
                      << "oamdB2aPositionBitOrder="
                      << eac3oamd::b2aBitOrderText(oamdB2aBitOrder)
                      << ";Syntax5511 wireMask bit0/1/2/3=position/zone/size/screen;"
                         "Table31 bit3/2/1/0=position/zone/size/screen;labels conflict\n"
                      << "oamdB2aHelperScan="
                      << (oamdB2aHelperScan ? "enabled" : "disabled") << '\n';
            if (oamdB2aHelperScan) {
                std::map<eac3oamd::B2aBitOrder, std::vector<unsigned>> passingHelpers;
                for (const OamdB2aHelperScanResult &candidate : oamdB2aHelperScanResults) {
                    if (candidate.disposition == eac3oamd::B2aDisposition::Pass) {
                        passingHelpers[candidate.bitOrder].push_back(
                            candidate.helperObjectIndex);
                    }
                    std::cout << "oamdB2aHelperCandidate order="
                              << eac3oamd::b2aBitOrderText(candidate.bitOrder)
                              << " helperIndex=" << candidate.helperObjectIndex
                              << " disposition="
                              << eac3oamd::b2aDispositionText(candidate.disposition)
                              << " bitsConsumed=" << candidate.bitsConsumed
                              << " paddingBits=" << candidate.paddingBits
                              << " failureObject=" << candidate.failureObjectIndex
                              << " failureBlock=" << candidate.failureBlockIndex
                              << " failureBit=" << candidate.failureBitOffset
                              << " reason=" << candidate.reason << '\n';
                }
                for (const eac3oamd::B2aBitOrder order : {
                         eac3oamd::B2aBitOrder::Syntax5511Lsb,
                         eac3oamd::B2aBitOrder::Table31Msb}) {
                    std::cout << "oamdB2aHelperPassing order="
                              << eac3oamd::b2aBitOrderText(order) << " indices=";
                    const auto found = passingHelpers.find(order);
                    if (found != passingHelpers.end()) {
                        for (std::size_t index = 0; index < found->second.size(); ++index) {
                            if (index != 0) std::cout << ',';
                            std::cout << found->second[index];
                        }
                    }
                    std::cout << '\n';
                }
            }
            std::cout << "oamdB2aPayloadUnitAssociationCount="
                      << oamdB2aMetrics.unitAssociationCount << '\n'
                      << "oamdB2aPayloadUnitAssociation="
                      << (b2aAssociationPass ? "PASS" : "INCONCLUSIVE") << '\n'
                      << "oamdB2aMultipleObjectElements="
                      << oamdB2aMetrics.multipleObjectElements << '\n'
                      << "oamdB2aResult=" << (oamdB2aPass ? "PASS" : "INCONCLUSIVE")
                      << " stage=gate6b2a-raw-object-update-syntax\n";
            oamdB2bPass = !oamdB2aControl15
                && oamdB2bMetrics.payloadCount > 0U
                && oamdB2bMetrics.stateAppliedCount == oamdB2bMetrics.payloadCount
                && oamdB2bMetrics.fallbackCount == 0U
                && oamdB2bMetrics.dynamicObjectOrderPassCount
                    == oamdB2bMetrics.payloadCount;
            const std::size_t dynamicMin = oamdB2bMetrics.dynamicObjectCountMin
                == std::numeric_limits<std::size_t>::max()
                ? 0U : oamdB2bMetrics.dynamicObjectCountMin;
            std::cout << "oamdB2bPayloadCount=" << oamdB2bMetrics.payloadCount << '\n'
                      << "oamdB2bStateAppliedCount=" << oamdB2bMetrics.stateAppliedCount << '\n'
                      << "oamdB2bFallbackCount=" << oamdB2bMetrics.fallbackCount << '\n'
                      << "oamdB2bResetCount=" << oamdB2bMetrics.resetCount << '\n'
                      << "oamdB2bBasicStatuses=default:" << oamdB2bMetrics.defaultBasicCount
                      << ",full:" << oamdB2bMetrics.fullBasicCount
                      << ",reuse:" << oamdB2bMetrics.reuseBasicCount
                      << ",mixed:" << oamdB2bMetrics.mixedBasicCount << '\n'
                      << "oamdB2bRenderStatuses=default:" << oamdB2bMetrics.defaultRenderCount
                      << ",full:" << oamdB2bMetrics.fullRenderCount
                      << ",reuse:" << oamdB2bMetrics.reuseRenderCount
                      << ",mixed:" << oamdB2bMetrics.mixedRenderCount << '\n'
                      << "oamdB2bPositionModes=absolute:" << oamdB2bMetrics.absolutePositionCount
                      << ",differential:" << oamdB2bMetrics.differentialPositionCount << '\n'
                      << "oamdB2bActiveCount=" << oamdB2bMetrics.activeCount << '\n'
                      << "oamdB2bInactiveCount=" << oamdB2bMetrics.inactiveCount << '\n'
                      << "oamdB2bFinitePropertyCount=" << oamdB2bMetrics.finitePropertyCount << '\n'
                      << "oamdB2bMinusInfinityGainCount=" << oamdB2bMetrics.minusInfinityGainCount << '\n'
                      << "oamdB2bLfeHelperCount=" << oamdB2bMetrics.lfeHelperCount << '\n'
                      << "oamdB2bTrimElementCount=" << oamdB2bMetrics.trimElementCount << '\n'
                      << "oamdB2bExtendedElementCount=" << oamdB2bMetrics.extendedElementCount << '\n'
                      << "oamdB2bAdditionalElementPassCount="
                      << oamdB2bMetrics.additionalElementPassCount << '\n'
                      << "oamdB2bAdditionalElementFailureCount="
                      << oamdB2bMetrics.additionalElementFailureCount << '\n'
                      << "oamdB2bDynamicObjectCountRange=" << dynamicMin << '-'
                      << oamdB2bMetrics.dynamicObjectCountMax << '\n'
                      << "oamdB2bDynamicObjectOrderPassCount="
                      << oamdB2bMetrics.dynamicObjectOrderPassCount << '\n'
                      << "oamdB2bFirstFailureReason=" << oamdB2bMetrics.firstFailureReason << '\n'
                      << "oamdB2bResult=" << (oamdB2bPass ? "PASS" : "INCONCLUSIVE")
                      << " stage=gate6b2b-object-property-state\n";
        }
        if (joc) {
            std::size_t parsedPassCount = 0;
            std::size_t unsupportedCount = 0;
            std::size_t malformedCount = 0;
            std::size_t paddingBitsTotal = 0;
            std::size_t huffmanValues = 0;
            std::size_t syntaxBitsMin = 0;
            std::size_t syntaxBitsMax = 0;
            unsigned minObjects = 0;
            unsigned maxObjects = 0;
            bool haveObjects = false;
            std::map<unsigned, std::size_t> configCounts;
            std::map<unsigned, std::size_t> objectPresentCounts;
            std::map<unsigned, std::size_t> objectAbsentCounts;
            std::map<unsigned, std::size_t> bandsCounts;
            std::map<unsigned, std::size_t> sparseCounts;
            std::map<unsigned, std::size_t> quantCounts;
            std::map<unsigned, std::size_t> slopeCounts;
            std::map<unsigned, std::size_t> dataPointCounts;
            bool haveSequence = false;
            unsigned expectedSequence = 0;
            unsigned previousSequence = 0;
            std::size_t sequenceContinuity = 0;
            std::size_t sequenceReset = 0;
            std::size_t sequenceSpliceReset = 0;
            std::size_t sequenceWrap = 0;
            std::size_t sequenceDiscontinuity = 0;
            const auto nextSequence = [](unsigned sequence) {
                return sequence == 1023U ? 1U : (sequence + 1U) & 0x3ffU;
            };
            for (const JocPayloadRecord &jocRecord : jocPayloadRecords) {
                const eac3joc::FrameReport &jocReport = jocRecord.report;
                if (jocReport.disposition == eac3joc::ParseDisposition::Pass) {
                    ++parsedPassCount;
                } else if (jocReport.disposition == eac3joc::ParseDisposition::Unsupported) {
                    ++unsupportedCount;
                } else {
                    ++malformedCount;
                }
                if (jocReport.disposition == eac3joc::ParseDisposition::Malformed) {
                    continue;
                }
                ++configCounts[jocReport.downmixConfigIndex];
                paddingBitsTotal += jocReport.paddingBits;
                syntaxBitsMin = syntaxBitsMin == 0
                    ? jocReport.bitsConsumed
                    : std::min(syntaxBitsMin, jocReport.bitsConsumed);
                syntaxBitsMax = std::max(syntaxBitsMax, jocReport.bitsConsumed);
                minObjects = haveObjects ? std::min(minObjects, jocReport.numObjects)
                                         : jocReport.numObjects;
                maxObjects = haveObjects ? std::max(maxObjects, jocReport.numObjects)
                                         : jocReport.numObjects;
                haveObjects = true;
                for (std::size_t objectIndex = 0; objectIndex < jocReport.objects.size(); ++objectIndex) {
                    const eac3joc::JocObjectSummary &object = jocReport.objects[objectIndex];
                    if (!object.present) {
                        ++objectAbsentCounts[static_cast<unsigned>(objectIndex)];
                        continue;
                    }
                    ++objectPresentCounts[static_cast<unsigned>(objectIndex)];
                    huffmanValues += object.huffmanValueCount;
                    ++bandsCounts[object.numBands];
                    ++sparseCounts[object.sparse ? 1U : 0U];
                    ++quantCounts[object.quantIndex];
                    ++slopeCounts[object.slopeIndex];
                    ++dataPointCounts[object.numDataPoints];
                }
                if (!haveSequence) {
                    haveSequence = true;
                    previousSequence = jocReport.sequenceCount;
                    expectedSequence = nextSequence(jocReport.sequenceCount);
                    if (jocReport.sequenceCount == 0) {
                        ++sequenceReset;
                        ++sequenceSpliceReset;
                    }
                } else if (jocReport.sequenceCount == expectedSequence) {
                    ++sequenceContinuity;
                    if (previousSequence == 1023U && jocReport.sequenceCount == 1U) {
                        // A 1023 -> 1 transition is the only legal wrap.
                        ++sequenceWrap;
                    }
                    previousSequence = jocReport.sequenceCount;
                    expectedSequence = nextSequence(jocReport.sequenceCount);
                } else {
                    if (jocReport.sequenceCount == 0) {
                        ++sequenceReset;
                        ++sequenceSpliceReset;
                    } else {
                        ++sequenceDiscontinuity;
                    }
                    previousSequence = jocReport.sequenceCount;
                    expectedSequence = nextSequence(jocReport.sequenceCount);
                }
            }
            const auto printCounts = [](const std::map<unsigned, std::size_t> &counts) {
                if (counts.empty()) {
                    std::cout << "none";
                    return;
                }
                bool first = true;
                for (const auto &[key, count] : counts) {
                    if (!first) {
                        std::cout << ',';
                    }
                    first = false;
                    std::cout << key << ':' << count;
                }
            };
            const bool selfTestPass = !jocSelfTest || jocSelfTestReport.pass;
            const bool mathSelfTestPass = !jocMathSelfTest || jocMathSelfTestReport.pass;
            jocPass = !jocPayloadRecords.empty() && malformedCount == 0 && selfTestPass;
            jocMathPass = joc && !jocPayloadRecords.empty() && malformedCount == 0
                && jocMathFailCount == 0 && mathSelfTestPass;
            std::cout << "jocParsedPassCount=" << parsedPassCount << '\n'
                      << "jocUnsupportedCount=" << unsupportedCount << '\n'
                      << "jocMalformedCount=" << malformedCount << '\n'
                      << "jocHuffmanValueCount=" << huffmanValues << '\n'
                      << "jocPaddingBitsTotal=" << paddingBitsTotal << '\n'
                      << "jocSyntaxBitsRange=" << syntaxBitsMin << '-' << syntaxBitsMax << '\n'
                      << "jocDownmixConfigs=";
            printCounts(configCounts);
            std::cout << '\n'
                      << "jocObjectCountRange=";
            if (haveObjects) {
                std::cout << minObjects << '-' << maxObjects;
            } else {
                std::cout << "none";
            }
            std::cout << '\n'
                      << "jocObjectPresencePresent=";
            printCounts(objectPresentCounts);
            std::cout << '\n'
                      << "jocObjectPresenceAbsent=";
            printCounts(objectAbsentCounts);
            std::cout << '\n'
                      << "jocBands=";
            printCounts(bandsCounts);
            std::cout << '\n'
                      << "jocSparseCounts=";
            printCounts(sparseCounts);
            std::cout << '\n'
                      << "jocQuantIndexCounts=";
            printCounts(quantCounts);
            std::cout << '\n'
                      << "jocSlopeCounts=";
            printCounts(slopeCounts);
            std::cout << '\n'
                      << "jocDataPointCounts=";
            printCounts(dataPointCounts);
            std::cout << '\n'
                      << "jocSequenceContinuity="
                      << (sequenceDiscontinuity == 0 ? "PASS" : "FAIL") << '\n'
                      << "jocSequenceContinuityCount=" << sequenceContinuity << '\n'
                      << "jocSequenceWraps=" << sequenceWrap << '\n'
                      << "jocSequenceResets=" << sequenceReset << '\n'
                      << "jocSequenceSpliceResets=" << sequenceSpliceReset << '\n'
                      << "jocSequenceDiscontinuities=" << sequenceDiscontinuity << '\n'
                      << "jocResult=" << (jocPass ? "PASS" : "INCONCLUSIVE")
                      << " stage=gate5a-joc-syntax reason="
                      << (jocPass ? "bounded-header-info-data-and-huffman-parse"
                                   : "missing-payload-malformed-payload-or-huffman-self-test-failure")
                      << '\n';
            std::cout << "jocMathPassCount=" << jocMathPassCount << '\n'
                      << "jocMathFailCount=" << jocMathFailCount << '\n'
                      << "jocMathStateResetCount=" << jocMathResetCount << '\n'
                      << "jocPayloadUnitAssociationCount=" << jocPayloadRecords.size() << '\n'
                      << "jocMathUnitAssociationCount=" << jocMathUnitIndices.size() << '\n'
                      << "jocMathResult=" << (jocMathPass ? "PASS" : "INCONCLUSIVE")
                      << " stage=gate5b-coefficient-math reason="
                      << (jocMathPass ? "differential-dequantization-interpolation"
                                       : "math-self-test-or-real-frame-math-failure")
                      << '\n';
        }
    }
    bool pcmPass = true;
    if (!jocBearExportPath.empty() && !jocGate6c) {
        std::cerr << "bearExport=FAIL reason=requires-joc-gate6c\n";
        return 1;
    }
    if (pcm) {
        if (jocQmf) {
            pcmPass = runNativePcmPairing(argv[1], chunks, consumedChunkCount,
                                          report.units, jocPayloadRecords, jocQmfQwin,
                                          gate6cMetadata, jocGate6c, jocGate7b,
                                          jocGate7c, rendererOptions, decodeOptions,
                                          jocBearExportPath, jocMatrixTracePath);
        } else {
            PcmReport pcmReport;
            pcmPass = runPcmProbe(argv[1], chunks, report.units, &pcmReport);
        }
    }
    return everyUnitIs1536 && pcmPass && jocPass && jocMathPass && oamdPass
        && oamdB2aPass && oamdB2bPass ? 0 : 1;
}
