#pragma once

// Gate 8N-5b: narrow adapter from the bounded native ordinary E-AC-3 core
// decoder to the existing callback seam.  This is not a JOC qualification
// path: successful frames carry OrdinaryEac3 content and are never labeled
// Joc.  No renderer, playback, or FFmpeg dependency is introduced.

#include "joc-session.h"
#include "native-eac3-core-decoder.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace eac3native {

class NativeEac3CoreDecoderSeam final : public eac3jocsession::ICoreDecoder {
public:
    NativeEac3CoreDecoderSeam() = default;

    eac3jocsession::CoreDecodeResult open(
        const eac3jocsession::CoreDecoderConfig &config) override;
    eac3jocsession::CoreDecodeResult pushPacket(
        const std::vector<std::uint8_t> &packet, std::int64_t timestamp,
        const eac3jocsession::CoreFrameCallback &callback) override;
    eac3jocsession::CoreDecodeResult flush(
        const eac3jocsession::CoreFrameCallback &callback) override;
    void reset() override;
    void cancel() override;

    bool opened() const { return opened_; }
    bool poisoned() const { return poisoned_; }
    std::size_t framesProcessed() const { return decoder_.framesProcessed(); }

private:
    eac3jocsession::CoreDecodeResult fail(
        eac3jocsession::Disposition disposition, const char *reason,
        eac3jocsession::FlowStatus flow = eac3jocsession::FlowStatus::None,
        bool poison = true);
    bool makeFrame(const DecodedAccessUnit &unit, bool flush, bool eosTail,
                   eac3jocsession::CoreFrame *frame,
                   std::string *reason) const;
    bool makeTailFrame(const NativeCoreFlushResult &tail,
                       eac3jocsession::CoreFrame *frame,
                       std::string *reason) const;
    static void setOrdinaryResult(eac3jocsession::CoreDecodeResult *result);

    NativeEac3CoreDecoder decoder_;
    NativeCoreConfig nativeConfig_;
    eac3jocsession::CoreDecoderConfig config_;
    bool opened_ = false;
    bool poisoned_ = false;
    bool canceled_ = false;
    bool blocked_ = false;
    // External callback timestamps may have a non-zero base.  The native
    // decoder receives its own zero-based contiguous cursor; this value is
    // only the next externally visible timestamp, including EOS.
    std::int64_t nextTimestamp_ = 0;
};

} // namespace eac3native
