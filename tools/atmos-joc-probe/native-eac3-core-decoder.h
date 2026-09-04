#pragma once

// Gate 8N-5a/N5A: bounded native ordinary E-AC-3 core-decoder adapter.
// This is a renderer-neutral config-3 contract. It deliberately does not
// implement the older JOC ICoreDecoder seam directly, whose original
// Joc/NotJoc disposition and CoreFrame shape could not express ordinary
// E-AC-3 channel IDs plus a distinct EOS tail without mislabeling the content.
// NativeEac3CoreDecoderSeam provides the additive, explicitly ordinary
// adapter in native-eac3-core-decoder-seam.{h,cpp}.

#include "native-eac3-pcm-session.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace eac3native {

enum class NativeCoreDisposition {
    Accepted,
    Unsupported,
    Malformed,
};

enum class NativeCoreFlow {
    None,
    Canceled,
    AlreadyFlushed,
};

struct NativeCoreConfig {
    unsigned sampleRate = 48000U;
    unsigned channels = 6U;
    std::string channelLayout = "FL,FC,FR,SL,SR,LFE";
};

struct DecodedChannel {
    unsigned index = 0U;
    std::string id;
    bool lfe = false;
    std::vector<double> samples;
};

struct DecodedAccessUnit {
    std::size_t sampleStart = 0U;
    std::size_t sampleCount = 0U;
    unsigned sampleRate = 0U;
    std::vector<DecodedChannel> channels;
    bool drcApplied = false;
};

struct NativeCoreResult {
    NativeCoreDisposition disposition = NativeCoreDisposition::Malformed;
    NativeCoreFlow flow = NativeCoreFlow::None;
    std::string reason;
    bool accepted = false;
    bool poisoned = false;
    DecodedAccessUnit unit;
};

struct NativeCoreFlushResult {
    bool ok = false;
    bool hadTail = false;
    NativeCoreFlow flow = NativeCoreFlow::None;
    std::string reason;
    std::size_t sampleStart = 0U;
    std::size_t tailSamplesPerChannel = 0U;
    std::vector<DecodedChannel> channels;
};

class NativeEac3CoreDecoder {
public:
    NativeCoreResult open(const NativeCoreConfig& config = {});

    // packet is exactly one complete E-AC-3 syncframe/AU. The bounded slice
    // accepts only type-0 independent SID0 with the config-3 48 kHz, six
    // block, 5.1+LFE topology. sampleStart must be the next contiguous core
    // sample position; there is no implicit AU reset.
    NativeCoreResult processAccessUnit(const std::vector<std::uint8_t>& packet,
                                       std::size_t sampleStart);

    // EOS emits one separate 256-sample tail per channel after accepted AUs;
    // the tail is never reported as a normal access unit.
    NativeCoreFlushResult flush();

    // Reset starts a new stream/seek/discontinuity. It clears dither,
    // transform overlap, sequence, poison, and EOS state.
    void reset();
    void cancel();

    bool opened() const { return opened_; }
    bool poisoned() const { return poisoned_; }
    bool canceled() const { return canceled_; }
    std::size_t framesProcessed() const { return framesProcessed_; }
    bool hasPendingOverlap() const { return session_.hasPendingOverlap(); }
    const NativeCoreConfig& config() const { return config_; }

private:
    NativeCoreResult failure(NativeCoreDisposition disposition,
                             NativeCoreFlow flow, std::string reason,
                             bool poison = true);

    NativeCoreConfig config_;
    Eac3PcmSession session_;
    bool opened_ = false;
    bool poisoned_ = false;
    bool canceled_ = false;
    bool flushed_ = false;
    std::size_t framesProcessed_ = 0U;
};

const char* toString(NativeCoreDisposition value);
const char* toString(NativeCoreFlow value);

} // namespace eac3native
