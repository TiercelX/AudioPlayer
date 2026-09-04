#pragma once

// Gate J0A3: bounded config-3 bridge joining the native ordinary core seam
// with the existing renderer-neutral JOC/QMF/session diagnostics.  This is a
// diagnostic bridge only: it does not add a renderer, playback, DRC, phase
// compensation, or support for legacy/dependent/advanced E-AC-3.

#include "joc-session.h"
#include "native-eac3-core-decoder-seam.h"
#include "native-eac3-joc-qualifier.h"
#include "oamd-b2b.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace eac3native {

// Probe-local shared B1/B2A/B2B metadata conversion.  This is deliberately
// a diagnostic contract; it does not expose a production decoder API.
bool buildNativeJocMetadata(
    const NativeJocQualification &qualification,
    std::size_t unitIndex,
    eac3oamd::B2bState *metadataState,
    std::vector<eac3gate6c::MetadataUpdate> *metadata,
    std::string *reason);

struct JocSessionBridgeConfig {
    std::vector<double> qwin;
    // Gate6C needs the bounded source interval up front for trim accounting.
    // The probe supplies maxFrames * 1536; this is not a stream reset policy.
    std::int64_t decodedSourceSamples = 0;
    std::size_t callbackCapacity = 1;
};

enum class JocSessionBridgeDisposition {
    Accepted,
    Unsupported,
    Malformed,
};

struct JocSessionBridgeResult {
    JocSessionBridgeDisposition disposition = JocSessionBridgeDisposition::Malformed;
    std::string stage;
    std::string reason;
    std::size_t auIndex = 0;
    std::int64_t timestamp = 0;
    std::size_t metadataUpdates = 0;
    std::size_t emittedBatches = 0;
    std::size_t eosTailSamples = 0;
    bool progressed = false;
};

class NativeEac3JocSessionBridge final {
public:
    NativeEac3JocSessionBridge(std::string jocTablePath,
                               JocSessionBridgeConfig config);

    JocSessionBridgeResult open();
    JocSessionBridgeResult process(
        const std::vector<std::uint8_t> &packet,
        const eac3gate6c::Callback &callback);
    JocSessionBridgeResult flush(const eac3gate6c::Callback &callback);
    void reset();
    void cancel();

    bool opened() const { return opened_; }
    bool poisoned() const { return poisoned_; }
    std::size_t framesProcessed() const { return framesProcessed_; }
    const eac3jocsession::Report &sessionReport() const
    {
        return session_.report();
    }

private:
    JocSessionBridgeResult fail(JocSessionBridgeDisposition disposition,
                                const char *stage, const char *reason,
                                bool poison = true);
    bool buildMetadata(const NativeJocQualification &qualification,
                       std::size_t unitIndex,
                       std::vector<eac3gate6c::MetadataUpdate> *metadata,
                       std::string *reason);
    static bool mapCoreChannels(
        const eac3jocsession::CoreFrame &frame,
        std::vector<std::vector<float>> *orderedPcm,
        std::vector<float> *lfe,
        std::string *reason);

    std::string jocTablePath_;
    JocSessionBridgeConfig config_;
    NativeEac3CoreDecoderSeam core_;
    NativeEac3JocQualifier qualifier_;
    eac3jocsession::Session session_;
    eac3oamd::B2bState metadataState_;
    bool opened_ = false;
    bool poisoned_ = false;
    bool canceled_ = false;
    bool flushed_ = false;
    std::size_t framesProcessed_ = 0;
};

const char *toString(JocSessionBridgeDisposition value);

} // namespace eac3native
