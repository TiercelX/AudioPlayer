#include "native-eac3-core.h"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace eac3native {
namespace {

constexpr std::array<unsigned, 3> kSampleRates = {48000, 44100, 32000};
constexpr std::array<unsigned, 4> kBlocks = {1, 2, 3, 6};
constexpr std::array<unsigned, 8> kAc3Channels = {2, 1, 2, 3, 3, 4, 4, 5};
constexpr std::array<std::array<unsigned, 3>, 38> kAc3FrameSizeWords = {{
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

unsigned channelCount(unsigned acmod, bool lfe)
{
    return kAc3Channels[acmod & 7U] + (lfe ? 1U : 0U);
}

ParseResult invalid(Disposition disposition, FailureStage stage, const char *reason)
{
    ParseResult result;
    result.disposition = disposition;
    result.stage = stage;
    result.reason = reason;
    return result;
}

bool readHeaderBits(BoundedBitReader *reader,
                    unsigned *streamType,
                    unsigned *substreamId,
                    unsigned *frameSizeWords,
                    unsigned *fscod)
{
    return reader && reader->read(2, streamType)
        && reader->read(3, substreamId)
        && reader->read(11, frameSizeWords)
        && reader->read(2, fscod);
}

} // namespace

BoundedBitReader::BoundedBitReader(const std::uint8_t *data,
                                   std::size_t byteCount,
                                   std::size_t startBit,
                                   std::size_t limitBit)
    : data_(data),
      byteCount_(byteCount),
      bit_(startBit),
      limitBit_(std::min(limitBit, byteCount > std::numeric_limits<std::size_t>::max() / 8U
                                      ? std::numeric_limits<std::size_t>::max()
                                      : byteCount * 8U))
{
    if (data_ == nullptr) {
        limitBit_ = 0;
    }
    if (bit_ > limitBit_) {
        overrun_ = true;
    }
}

bool BoundedBitReader::canRead(unsigned count) const
{
    return data_ != nullptr && bit_ <= limitBit_ && count <= limitBit_ - bit_;
}

bool BoundedBitReader::read(unsigned count, std::uint32_t *value)
{
    if (!value || count > 32 || !canRead(count)) {
        overrun_ = true;
        return false;
    }
    std::uint32_t result = 0;
    for (unsigned index = 0; index < count; ++index) {
        result = (result << 1U)
            | ((data_[bit_ / 8U] >> (7U - (bit_ % 8U))) & 1U);
        ++bit_;
    }
    *value = result;
    return true;
}

bool BoundedBitReader::skip(unsigned count)
{
    if (!canRead(count)) {
        overrun_ = true;
        return false;
    }
    bit_ += count;
    return true;
}

std::size_t BoundedBitReader::position() const
{
    return bit_;
}

std::size_t BoundedBitReader::remaining() const
{
    return bit_ <= limitBit_ ? limitBit_ - bit_ : 0;
}

bool BoundedBitReader::overrun() const
{
    return overrun_;
}

ParseResult parseSyncframe(const std::vector<std::uint8_t> &bytes, std::size_t offset)
{
    if (offset >= bytes.size() || bytes.size() - offset < 2U) {
        return invalid(Disposition::Malformed, FailureStage::Bounds, "truncated-syncword");
    }
    if (bytes[offset] != 0x0b || bytes[offset + 1U] != 0x77) {
        return invalid(Disposition::NotEac3, FailureStage::None, "missing-syncword");
    }

    BoundedBitReader probe(bytes.data(), bytes.size(), offset * 8U + 16U);
    std::uint32_t value = 0;
    if (!probe.skip(24U) || !probe.read(5U, &value)) {
        return invalid(Disposition::Malformed, FailureStage::Header, "truncated-bsid");
    }
    const unsigned bsid = value;

    FrameHeader frame;
    frame.offset = offset;
    frame.bsid = bsid;
    frame.capabilities.frameBoundaryRangeChecked = false;
    frame.capabilities.crcRangeChecked = false;
    frame.capabilities.crcVerified = false;

    if (bsid <= 8U) {
        // AC-3 core frames are represented as AC3_CONVERT.  This preserves
        // the current raw EB3 topology inventory; no AC-3 coefficient decode
        // is attempted here.
        BoundedBitReader reader(bytes.data(), bytes.size(), offset * 8U + 16U);
        std::uint32_t crc1 = 0;
        std::uint32_t fscod = 0;
        std::uint32_t frmsizecod = 0;
        std::uint32_t parsedBsid = 0;
        if (!reader.read(16U, &crc1)
            || !reader.read(2U, &fscod)
            || !reader.read(6U, &frmsizecod)
            || !reader.read(5U, &parsedBsid)) {
            return invalid(Disposition::Malformed, FailureStage::Header,
                           "truncated-ac3-header");
        }
        (void)crc1;
        if (fscod >= kSampleRates.size()) {
            return invalid(Disposition::Unsupported, FailureStage::Header,
                           "unsupported-ac3-rate-code");
        }
        if (frmsizecod >= kAc3FrameSizeWords.size()) {
            return invalid(Disposition::Unsupported, FailureStage::Header,
                           "reserved-ac3-frame-size-code");
        }
        frame.streamType = StreamType::LegacyAc3;
        frame.substreamId = 0;
        frame.sampleRate = kSampleRates[fscod] >> (parsedBsid > 8U ? parsedBsid - 8U : 0U);
        frame.blocks = 6;
        frame.sampleCount = 1536;
        frame.sizeBytes = static_cast<std::size_t>(
            kAc3FrameSizeWords[frmsizecod][fscod]) * 2U;
        std::uint32_t bsmod = 0;
        std::uint32_t acmod = 0;
        if (!reader.read(3U, &bsmod) || !reader.read(3U, &acmod)) {
            return invalid(Disposition::Malformed, FailureStage::Header,
                           "truncated-ac3-channel-header");
        }
        (void)bsmod;
        if ((acmod & 1U) && acmod != 1U && !reader.skip(2U)) {
            return invalid(Disposition::Malformed, FailureStage::Header,
                           "truncated-ac3-cmixlev");
        }
        if ((acmod & 4U) && !reader.skip(2U)) {
            return invalid(Disposition::Malformed, FailureStage::Header,
                           "truncated-ac3-surmixlev");
        }
        if (acmod == 2U && !reader.skip(2U)) {
            return invalid(Disposition::Malformed, FailureStage::Header,
                           "truncated-ac3-dsurmod");
        }
        std::uint32_t lfe = 0;
        if (!reader.read(1U, &lfe)) {
            return invalid(Disposition::Malformed, FailureStage::Header,
                           "truncated-ac3-lfe-header");
        }
        frame.acmod = acmod;
        frame.lfe = lfe != 0;
        frame.channelCount = channelCount(frame.acmod, frame.lfe);
        frame.dependent = false;
        frame.additional = false;
    } else if (bsid <= 10U) {
        return invalid(Disposition::Unsupported, FailureStage::Header,
                       "reserved-legacy-bsid");
    } else {
        BoundedBitReader reader(bytes.data(), bytes.size(), offset * 8U + 16U);
        std::uint32_t streamType = 0;
        std::uint32_t substreamId = 0;
        std::uint32_t frameSizeWords = 0;
        std::uint32_t fscod = 0;
        if (!readHeaderBits(&reader, &streamType, &substreamId,
                            &frameSizeWords, &fscod)) {
            return invalid(Disposition::Malformed, FailureStage::Header,
                           "truncated-eac3-header");
        }
        if (streamType >= static_cast<unsigned>(StreamType::Reserved)) {
            return invalid(Disposition::Unsupported, FailureStage::Header,
                           "reserved-stream-type");
        }
        frame.streamType = static_cast<StreamType>(streamType);
        frame.substreamId = substreamId;
        frame.dependent = frame.streamType == StreamType::Dependent;
        frame.additional = frame.streamType == StreamType::Independent
                           && frame.substreamId != 0U;
        frame.sizeBytes = static_cast<std::size_t>(frameSizeWords + 1U) * 2U;

        std::uint32_t numBlocksCode = 3;
        if (fscod == 3U) {
            return invalid(Disposition::Unsupported, FailureStage::Header,
                           "reserved-eac3-fscod3");
        }
        if (fscod >= kSampleRates.size()) {
            return invalid(Disposition::Unsupported, FailureStage::Header,
                           "reserved-sample-rate-code");
        }
        if (!reader.read(2U, &numBlocksCode)) {
            return invalid(Disposition::Malformed, FailureStage::Header,
                           "truncated-numblkscod");
        }
        frame.sampleRate = kSampleRates[fscod];
        frame.blocks = kBlocks[numBlocksCode];

        std::uint32_t acmod = 0;
        std::uint32_t lfe = 0;
        std::uint32_t parsedBsid = 0;
        if (!reader.read(3U, &acmod)
            || !reader.read(1U, &lfe)
            || !reader.read(5U, &parsedBsid)) {
            return invalid(Disposition::Malformed, FailureStage::Header,
                           "truncated-eac3-channel-header");
        }
        if (parsedBsid < 11U || parsedBsid > 16U) {
            return invalid(Disposition::Unsupported, FailureStage::Header,
                           "unsupported-eac3-bsid");
        }
        frame.bsid = parsedBsid;
        frame.acmod = acmod;
        frame.lfe = lfe != 0;
        frame.channelCount = channelCount(frame.acmod, frame.lfe);
        frame.sampleCount = frame.blocks * 256U;
    }

    if (frame.sizeBytes < 7U) {
        return invalid(Disposition::Malformed, FailureStage::Validation,
                       "frame-size-too-small");
    }
    if (offset > bytes.size() || frame.sizeBytes > bytes.size() - offset) {
        return invalid(Disposition::Malformed, FailureStage::Bounds,
                       "truncated-frame-payload");
    }
    frame.endBit = (offset + frame.sizeBytes) * 8U;
    frame.capabilities.frameBoundaryRangeChecked = true;
    // This gate only establishes bounds and field syntax.  It intentionally
    // does not calculate either CRC polynomial.
    frame.capabilities.crcRangeChecked = false;
    frame.capabilities.crcVerified = false;

    ParseResult result;
    result.disposition = Disposition::Accepted;
    result.stage = FailureStage::None;
    result.frame = frame;
    return result;
}

const char *toString(Disposition value)
{
    switch (value) {
    case Disposition::Accepted: return "accepted";
    case Disposition::NotEac3: return "not-eac3";
    case Disposition::Unsupported: return "unsupported";
    case Disposition::Malformed: return "malformed";
    }
    return "unknown";
}

const char *toString(FailureStage value)
{
    switch (value) {
    case FailureStage::None: return "none";
    case FailureStage::Validation: return "validation";
    case FailureStage::Header: return "header";
    case FailureStage::Bounds: return "bounds";
    case FailureStage::Sequence: return "sequence";
    case FailureStage::Assembly: return "assembly";
    }
    return "unknown";
}

const char *toString(FlowStatus value)
{
    switch (value) {
    case FlowStatus::None: return "none";
    case FlowStatus::Canceled: return "canceled";
    case FlowStatus::AlreadyFlushed: return "already-flushed";
    }
    return "unknown";
}

const char *toString(StreamType value)
{
    switch (value) {
    case StreamType::Independent: return "independent";
    case StreamType::Dependent: return "dependent";
    case StreamType::Ac3Convert: return "eac3-ac3-convert";
    case StreamType::Reserved: return "reserved";
    case StreamType::LegacyAc3: return "legacy-ac3";
    }
    return "unknown";
}

std::string AccessUnitAssembler::streamKey(const FrameHeader &frame)
{
    return baseKey(frame);
}

std::string AccessUnitAssembler::baseKey(const FrameHeader &frame)
{
    return "base/" + std::string(toString(frame.streamType))
        + "/sid" + std::to_string(frame.substreamId);
}

std::string AccessUnitAssembler::frameKey(const FrameHeader &frame) const
{
    if (frame.streamType == StreamType::Dependent && activeParent_) {
        return "dependent/p" + std::to_string(activeParent_->substreamId)
            + "/d" + std::to_string(frame.substreamId);
    }
    return baseKey(frame);
}

bool AccessUnitAssembler::isBaseSid0(const FrameHeader &frame)
{
    return frame.substreamId == 0U
        && (frame.streamType == StreamType::Independent
            || frame.streamType == StreamType::LegacyAc3
            || frame.streamType == StreamType::Ac3Convert);
}

void AccessUnitAssembler::startPending(const FrameHeader &frame)
{
    pending_.emplace();
    pending_->sampleRate = sampleRate_;
    pending_->frames.push_back(frame);
    pending_->blocksByStream[baseKey(frame)] = frame.blocks;
    pending_->compressedBytes = frame.sizeBytes;
    baseBlocks_ = frame.blocks;
    pending_->sampleCount = baseBlocks_ * 256U;
    activeParent_ = frame;
    currentBaseFrame_ = frame;
    nextAdditionalSid_ = 1U;
    nextDependentLocalSid_ = 0U;
    roundIndependentSids_.clear();
    roundIndependentSids_.insert(0U);
}

bool AccessUnitAssembler::topologyMatches(std::string *reason) const
{
    if (!pending_) {
        if (reason) {
            *reason = "missing-pending-access-unit";
        }
        return false;
    }
    std::set<std::string> actual;
    for (const auto &entry : pending_->blocksByStream) {
        if (entry.second != 6U) {
            if (reason) {
                *reason = "access-unit-stream-blocks-incomplete";
            }
            return false;
        }
        actual.insert(entry.first);
    }
    if (!expectedTopology_) {
        return true;
    }
    if (actual != *expectedTopology_) {
        if (reason) {
            *reason = "access-unit-topology-mismatch";
        }
        return false;
    }
    return true;
}

ProcessResult AccessUnitAssembler::failure(Disposition disposition,
                                            FailureStage stage,
                                            std::string reason)
{
    ProcessResult result;
    result.disposition = disposition;
    result.stage = stage;
    result.reason = std::move(reason);
    return result;
}

ProcessResult AccessUnitAssembler::terminal(FlowStatus flow, std::string reason) const
{
    ProcessResult result;
    result.flow = flow;
    result.reason = std::move(reason);
    return result;
}

ProcessResult AccessUnitAssembler::process(const ParseResult &parsed)
{
    if (parsed.disposition != Disposition::Accepted || !parsed.frame) {
        ProcessResult result;
        result.disposition = parsed.disposition;
        result.stage = parsed.stage;
        result.flow = parsed.flow;
        result.reason = parsed.reason;
        return result;
    }
    return process(*parsed.frame);
}

ProcessResult AccessUnitAssembler::process(const FrameHeader &frame)
{
    // Frame assembly has several coupled cursors (parent, local dependent
    // sid, additional sid and topology).  Run the mutating state machine on
    // a value copy and commit only an accepted result, so every rejection is
    // transactional and cannot poison the next frame.
    AccessUnitAssembler candidate = *this;
    ProcessResult result = candidate.processMutable(frame);
    if (result.disposition == Disposition::Accepted
        && result.flow == FlowStatus::None) {
        *this = std::move(candidate);
    }
    return result;
}

ProcessResult AccessUnitAssembler::processMutable(const FrameHeader &frame)
{
    if (canceled_) {
        return terminal(FlowStatus::Canceled, "session-canceled");
    }
    if (flushed_) {
        return terminal(FlowStatus::AlreadyFlushed, "session-already-flushed");
    }
    if (frame.sampleRate == 0U
        || (frame.blocks != 1U && frame.blocks != 2U
            && frame.blocks != 3U && frame.blocks != 6U)
        || frame.sampleCount != frame.blocks * 256U) {
        return failure(Disposition::Malformed, FailureStage::Validation,
                       "invalid-frame-shape");
    }

    if (!pending_) {
        if (!isBaseSid0(frame)) {
            return failure(Disposition::Malformed, FailureStage::Assembly,
                           "au-start-not-base-sid0");
        }
        sampleRate_ = frame.sampleRate;
        startPending(frame);
        return ProcessResult {};
    }

    if (frame.sampleRate != sampleRate_) {
        return failure(Disposition::Malformed, FailureStage::Sequence,
                       "sample-rate-change");
    }

    ProcessResult result;

    if (frame.streamType == StreamType::Dependent) {
        if (!activeParent_) {
            return failure(Disposition::Malformed, FailureStage::Sequence,
                           "orphan-dependent");
        }
        if (activeParent_->streamType == StreamType::Ac3Convert) {
            return failure(Disposition::Unsupported, FailureStage::Assembly,
                           "eac3-strmtyp2-dependent-forbidden");
        }
        if (frame.substreamId != nextDependentLocalSid_) {
            return failure(Disposition::Malformed, FailureStage::Sequence,
                           frame.substreamId > nextDependentLocalSid_
                               ? "dependent-local-id-gap"
                               : "dependent-local-id-repeat");
        }
        if (activeParent_->sampleRate != frame.sampleRate
            || activeParent_->blocks != frame.blocks) {
            return failure(Disposition::Malformed, FailureStage::Sequence,
                           "dependent-shape-mismatch");
        }
        ++nextDependentLocalSid_;
    } else if (frame.streamType == StreamType::Ac3Convert) {
        if (frame.substreamId != 0U) {
            return failure(Disposition::Unsupported, FailureStage::Assembly,
                           "eac3-strmtyp2-nonzero-sid");
        }
        if (currentBaseFrame_
            && currentBaseFrame_->streamType != frame.streamType) {
            return failure(Disposition::Malformed, FailureStage::Sequence,
                           "base-stream-type-change");
        }
        if (baseBlocks_ == 6U) {
            std::string topologyReason;
            if (!topologyMatches(&topologyReason)) {
                return failure(Disposition::Malformed, FailureStage::Assembly,
                               topologyReason);
            }
            if (!expectedTopology_) {
                std::set<std::string> actual;
                for (const auto &entry : pending_->blocksByStream) {
                    actual.insert(entry.first);
                }
                expectedTopology_ = std::move(actual);
            }
            result.completed = std::move(pending_);
            pending_.reset();
            startPending(frame);
            return result;
        }
        currentBaseFrame_ = frame;
        activeParent_ = frame;
        nextDependentLocalSid_ = 0U;
        roundIndependentSids_.clear();
        roundIndependentSids_.insert(0U);
    } else if (frame.streamType == StreamType::Independent
               || frame.streamType == StreamType::LegacyAc3) {
        if (frame.substreamId == 0U) {
            if (currentBaseFrame_
                && currentBaseFrame_->streamType != frame.streamType
                && baseBlocks_ != 6U) {
                return failure(Disposition::Malformed, FailureStage::Sequence,
                               "base-stream-type-change");
            }
            if (baseBlocks_ == 6U) {
                std::string topologyReason;
                if (!topologyMatches(&topologyReason)) {
                    return failure(Disposition::Malformed, FailureStage::Assembly,
                                   topologyReason);
                }
                if (!expectedTopology_) {
                    std::set<std::string> actual;
                    for (const auto &entry : pending_->blocksByStream) {
                        actual.insert(entry.first);
                    }
                    expectedTopology_ = std::move(actual);
                }
                result.completed = std::move(pending_);
                pending_.reset();
                startPending(frame);
                return result;
            }
            currentBaseFrame_ = frame;
            activeParent_ = frame;
            nextDependentLocalSid_ = 0U;
            roundIndependentSids_.clear();
            roundIndependentSids_.insert(0U);
        } else {
            if (frame.streamType != StreamType::Independent) {
                return failure(Disposition::Unsupported, FailureStage::Assembly,
                               "non-independent-additional-substream");
            }
            if (!currentBaseFrame_
                || frame.sampleRate != currentBaseFrame_->sampleRate
                || frame.blocks != currentBaseFrame_->blocks) {
                return failure(Disposition::Malformed, FailureStage::Sequence,
                               "additional-shape-mismatch");
            }
            if (frame.substreamId > nextAdditionalSid_) {
                return failure(Disposition::Malformed, FailureStage::Sequence,
                               "additional-substream-id-gap");
            }
            if (roundIndependentSids_.find(frame.substreamId)
                != roundIndependentSids_.end()) {
                return failure(Disposition::Malformed, FailureStage::Sequence,
                               "additional-repeat-in-round");
            }
            if (frame.substreamId == nextAdditionalSid_) {
                ++nextAdditionalSid_;
            }
            roundIndependentSids_.insert(frame.substreamId);
            activeParent_ = frame;
            nextDependentLocalSid_ = 0U;
        }
    } else {
        return failure(Disposition::Unsupported, FailureStage::Assembly,
                       "unsupported-stream-topology");
    }

    const std::string key = frameKey(frame);
    if (key.empty()) {
        return failure(Disposition::Malformed, FailureStage::Sequence,
                       "missing-topology-key");
    }
    const unsigned previousBlocks = pending_->blocksByStream[key];
    if (previousBlocks > 6U || frame.blocks > 6U
        || previousBlocks > 6U - frame.blocks) {
        return failure(Disposition::Malformed, FailureStage::Assembly,
                       "block-overflow");
    }
    pending_->frames.push_back(frame);
    pending_->blocksByStream[key] = previousBlocks + frame.blocks;
    pending_->compressedBytes += frame.sizeBytes;
    if (frame.streamType != StreamType::Dependent
        && frame.substreamId == 0U) {
        baseBlocks_ += frame.blocks;
    }
    pending_->sampleCount = baseBlocks_ * 256U;
    return result;
}

ProcessResult AccessUnitAssembler::flush()
{
    if (canceled_) {
        return terminal(FlowStatus::Canceled, "session-canceled");
    }
    if (flushed_) {
        return terminal(FlowStatus::AlreadyFlushed, "session-already-flushed");
    }
    if (!pending_ || baseBlocks_ != 6U) {
        return failure(Disposition::Malformed, FailureStage::Assembly,
                       "incomplete-access-unit");
    }
    std::string topologyReason;
    if (!topologyMatches(&topologyReason)) {
        return failure(Disposition::Malformed, FailureStage::Assembly,
                       topologyReason);
    }
    if (!expectedTopology_) {
        std::set<std::string> actual;
        for (const auto &entry : pending_->blocksByStream) {
            actual.insert(entry.first);
        }
        expectedTopology_ = std::move(actual);
    }
    ProcessResult result;
    result.completed = std::move(pending_);
    pending_.reset();
    activeParent_.reset();
    currentBaseFrame_.reset();
    baseBlocks_ = 0U;
    nextDependentLocalSid_ = 0U;
    roundIndependentSids_.clear();
    flushed_ = true;
    return result;
}

ProcessResult AccessUnitAssembler::cancel()
{
    if (canceled_) {
        return terminal(FlowStatus::Canceled, "session-canceled");
    }
    if (flushed_) {
        return terminal(FlowStatus::AlreadyFlushed, "session-already-flushed");
    }
    canceled_ = true;
    pending_.reset();
    activeParent_.reset();
    currentBaseFrame_.reset();
    return terminal(FlowStatus::Canceled, "session-canceled");
}

void AccessUnitAssembler::reset()
{
    pending_.reset();
    activeParent_.reset();
    currentBaseFrame_.reset();
    sampleRate_ = 0;
    baseBlocks_ = 0U;
    nextAdditionalSid_ = 1U;
    nextDependentLocalSid_ = 0U;
    roundIndependentSids_.clear();
    expectedTopology_.reset();
    canceled_ = false;
    flushed_ = false;
}

} // namespace eac3native
