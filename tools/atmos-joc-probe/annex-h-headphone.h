#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace eac3annexh {

enum class Disposition { Pass, Unsupported, Malformed };

struct HeadphonePayload {
    unsigned channelCount = 0U;
    std::vector<unsigned> channelGain;
    bool lfeGainPresent = false;
    unsigned lfeGain = 0U;
    bool sequenceStart = false;
    bool sequenceEnd = false;
    bool brirDataPresent = false;
    // One 96-bit early-BRIR chunk per full-bandwidth channel and one
    // 1536-bit late-BRIR chunk.  Bytes are retained in wire order so the
    // 24-bit coefficient sign/scale convention is not guessed here.
    std::vector<std::vector<std::uint8_t>> earlyBrirChunks;
    std::vector<std::uint8_t> lateBrirChunk;
    bool parityPresent = false;
    std::uint8_t parityCheck = 0U;
    bool propagationDelayPresent = false;
    std::vector<unsigned> propagationDelay;
    bool rt60Present = false;
    unsigned rt60Bands = 0U;
    std::vector<unsigned> rt60;
};

struct ParseResult {
    Disposition disposition = Disposition::Malformed;
    std::string reason;
    HeadphonePayload payload;
};

ParseResult parse(const std::vector<std::uint8_t> &bytes);

struct SequenceResult {
    Disposition disposition = Disposition::Malformed;
    std::string reason;
    std::size_t payloads = 0U;
    std::size_t brirPayloads = 0U;
};

SequenceResult validateSequence(const std::vector<HeadphonePayload> &payloads);

// Renderer-neutral completed H.3.7 data.  The BRIR vectors contain raw
// unsigned 24-bit coefficient words (three bytes, MSB first, packed into the
// low 24 bits); no fixed-point scale or float conversion is claimed.
struct HeadphoneRenderingData {
    unsigned channelCount = 0U;
    std::vector<unsigned> channelGain;
    bool lfeGainPresent = false;
    unsigned lfeGain = 0U;
    std::vector<unsigned> propagationDelay;
    std::vector<unsigned> rt60;
    std::vector<std::vector<std::uint32_t>> earlyBrirRaw24;
    std::vector<std::uint32_t> lateBrirRaw24;
    std::size_t sequenceFrames = 0U;
    std::size_t earlyChunkCountPerChannel = 0U;
    std::size_t lateChunkCount = 0U;
    std::uint8_t parityCheck = 0U;
};

enum class ReassemblyDisposition { Pending, Pass, Unsupported, Malformed, Cancelled };

struct ReassemblyResult {
    ReassemblyDisposition disposition = ReassemblyDisposition::Malformed;
    std::string reason;
    std::size_t payloads = 0U;
    std::size_t brirPayloads = 0U;
    HeadphoneRenderingData data;
};

class HeadphoneRenderingCache final {
public:
    ReassemblyResult append(const HeadphonePayload &payload);
    ReassemblyResult cancel();
    void reset();
    bool active() const { return active_; }
    const HeadphoneRenderingData &committed() const { return committed_; }

private:
    void clearWorking();

    bool active_ = false;
    HeadphoneRenderingData working_;
    std::size_t payloads_ = 0U;
    std::size_t brirPayloads_ = 0U;
    std::size_t earlyChunkCount_ = 0U;
    std::size_t lateChunkCount_ = 0U;
    std::uint8_t parityXor_ = 0U;
    HeadphoneRenderingData committed_;
};

ReassemblyResult reassembleSequence(const std::vector<HeadphonePayload> &payloads);

struct PayloadContext {
    std::size_t accessUnitOrdinal = 0U;
    std::size_t frameOrdinal = 0U;
    std::int64_t timestampSamples = 0;
    unsigned frameType = 0U;
    unsigned substreamId = 0U;
    unsigned blocks = 0U;
    bool ac3Frame = false;
};

struct RoutedResult {
    ReassemblyDisposition disposition = ReassemblyDisposition::Malformed;
    std::string reason;
    PayloadContext context;
    HeadphoneRenderingData data;
};

// Bounded outer-carrier router.  It keeps AC-3 and E-AC-3 substreams
// independent, checks six-block E-AC-3/AC-3 carrier eligibility and sample
// continuity, then feeds the H.3.7 payload into the cache above.
class HeadphonePayloadRouter final {
public:
    RoutedResult ingest(const PayloadContext &context,
                        const std::vector<std::uint8_t> &bytes);
    std::size_t reset();
    std::size_t cancel();
    std::size_t activeStreams() const;
    std::size_t completedSequences() const { return completedSequences_; }

private:
    struct StreamState {
        HeadphoneRenderingCache cache;
        bool haveTimestamp = false;
        std::int64_t lastTimestampSamples = 0;
    };

    std::map<std::pair<unsigned, unsigned>, StreamState> streams_;
    std::size_t completedSequences_ = 0U;
};

} // namespace eac3annexh
