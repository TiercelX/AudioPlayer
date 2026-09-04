#pragma once

// Gate 8N-3b/N3B: offline, renderer-neutral connection of the already
// accepted ordinary uncoupled audblk coefficients to the standalone scalar
// transform.  This is a diagnostic session only: it does not apply DRC,
// rematrixing, JOC, IMDCT runtime policy, playback, or renderer behavior.

#include "native-eac3-audblk.h"
#include "native-eac3-transform.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace eac3native {

enum class PcmSessionDisposition {
    Accepted,
    Unsupported,
    Malformed,
};

// The zero mode is diagnostic-only: it suppresses generated dither while
// keeping the production/reference default unchanged. It is not a codec
// interoperability policy and must not be used as the normal session mode.
enum class PcmDitherMode {
    Reference,
    ZeroDiagnosticOnly,
};

struct PcmChannelFrame {
    unsigned channel = 0U;
    bool lfe = false;
    std::vector<double> samples;
    // One enclosing syntax-to-mantissa span per block. Channel syntax is
    // interleaved; these are not claims of contiguous channel ownership.
    std::vector<std::pair<std::size_t, std::size_t>> coefficientSpans;
    std::uint64_t ditherStateStart = 0U;
    std::uint64_t ditherStateEnd = 0U;
    std::size_t ditherSamplesStart = 0U;
    std::size_t ditherSamples = 0U;
};

struct PcmFrameResult {
    PcmSessionDisposition disposition = PcmSessionDisposition::Malformed;
    std::string reason;
    std::size_t frameOffset = 0U;
    std::size_t bitPosition = 0U;
    unsigned blockIndex = static_cast<unsigned>(-1);
    unsigned channelIndex = static_cast<unsigned>(-1);
    bool channelIsLfe = false;
    unsigned blocks = 0U;
    unsigned channels = 0U;
    unsigned sampleRate = 0U;
    std::size_t outputSamplesPerChannel = 0U;
    // TS §6.9.4.1 conceptual delay.  The transform stores this overlap
    // explicitly; it is not a renderer latency or a hidden sample pad.
    std::size_t conceptualDelaySamples = kEac3InherentDelaySamples;
    bool drcApplied = false;
    std::vector<PcmChannelFrame> channelsData;
};

struct PcmFlushResult {
    bool ok = false;
    bool hadTail = false;
    std::string reason;
    std::size_t tailSamplesPerChannel = 0U;
    std::vector<PcmChannelFrame> channelsData;
};

class Eac3PcmSession {
public:
    explicit Eac3PcmSession(PcmDitherMode ditherMode = PcmDitherMode::Reference)
        : ditherMode_(ditherMode)
    {
    }

    // Process one complete E-AC-3 syncframe. The audfrm/audblk parser is
    // invoked exactly once and its coefficient vectors are consumed in the
    // coded block/channel order. State is retained across adjacent frames;
    // callers reset only for a new stream, seek/discontinuity, or topology
    // rebuild.
    PcmFrameResult processFrame(const std::vector<std::uint8_t>& bytes,
                                const FrameHeader& frame,
                                const BsiInfo& bsi);

    // Emit one 256-sample EOS tail per channel when a block was processed.
    // A second flush is an empty EOS; processFrame after flush fails closed.
    PcmFlushResult flush();

    // Explicitly clear per-channel dither, transform overlap, topology, and
    // EOS/poison state. No frame call performs an implicit reset.
    void reset();

    bool initialized() const { return initialized_; }
    bool poisoned() const { return poisoned_; }
    bool hasPendingOverlap() const;
    unsigned channelCount() const { return channelCount_; }
    unsigned sampleRate() const { return sampleRate_; }
    std::size_t framesProcessed() const { return framesProcessed_; }

private:
    PcmFrameResult failure(PcmSessionDisposition disposition,
                           const FrameHeader& frame,
                           std::string reason,
                           unsigned block = static_cast<unsigned>(-1),
                           unsigned channel = static_cast<unsigned>(-1),
                           bool channelIsLfe = false);

    bool initialized_ = false;
    bool lfe_ = false;
    bool flushed_ = false;
    bool poisoned_ = false;
    PcmDitherMode ditherMode_ = PcmDitherMode::Reference;
    unsigned channelCount_ = 0U;
    unsigned sampleRate_ = 0U;
    std::size_t framesProcessed_ = 0U;
    std::vector<ReferenceDitherSource> ditherSources_;
    std::vector<Eac3TransformChannel> transforms_;
};

const char* toString(PcmSessionDisposition value);

} // namespace eac3native
