#pragma once

#include "joc-gate5b.h"
#include "joc-gate6c.h"
#include "joc-qmf.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace eac3jocsession {

enum class Disposition {
    Joc,
    OrdinaryEac3,
    NotJoc,
    Unsupported,
    Malformed,
};

const char *dispositionText(Disposition disposition);

// Content disposition and delivery flow are orthogonal. FlowStatus reports
// why delivery stopped; it is not an E-AC-3/JOC capability result.
enum class FlowStatus {
    None,
    CallbackRejected,
    Canceled,
    AlreadyFlushed,
};

const char *flowStatusText(FlowStatus status);

// FailureStage identifies the Session pipeline stage that rejected content.
// Delivery flow (FlowStatus) remains orthogonal and is used for cancellation,
// callback rejection, and generation guards.
enum class FailureStage {
    None,
    Validation,
    Math,
    Sequence,
    Qmf,
    Gate6c,
};

const char *failureStageText(FailureStage stage);

// Content qualification is deliberately independent of the legacy
// Joc/NotJoc disposition.  Ordinary native E-AC-3 must never be reported as
// Joc merely because it decoded successfully.
enum class CoreContentKind {
    Unknown,
    JocQualified,
    OrdinaryEac3,
};

const char *coreContentKindText(CoreContentKind kind);

struct CoreDecoderConfig {
    unsigned sampleRate = 0;
    unsigned channels = 0;
    std::string channelLayout;
    bool candidate = false;
};

struct CoreFrame {
    unsigned sampleRate = 0;
    unsigned channels = 0;
    std::string channelLayout;
    std::size_t sampleCount = 0;
    // Timestamp in sample frames at the configured sample rate.
    std::int64_t timestamp = 0;
    bool reset = false;
    bool flush = false;
    std::vector<std::vector<float>> planar;
    CoreContentKind contentKind = CoreContentKind::Unknown;
    std::vector<std::string> channelIds;
    std::vector<bool> lfeChannels;
    bool drcApplied = false;
    // An EOS tail is not a normal access unit.  Ordinary native frames use
    // 1536 samples; the separate flush frame uses 256 samples.
    bool eosTail = false;
};

struct CoreDecodeResult {
    Disposition disposition = Disposition::Malformed;
    std::string reason;
    bool accepted = false;
    bool terminal = false;
    FlowStatus flow = FlowStatus::None;
    CoreContentKind contentKind = CoreContentKind::Unknown;
};

// The callback reference is borrowed for the duration of the call only; an
// adapter must not retain it after pushPacket/flush returns.
using CoreFrameCallback = std::function<bool(const CoreFrame &)>;

bool validateCoreFrame(const CoreFrame &frame, std::string *reason);

// Gate 8A seam. The first production implementation may adapt reviewed,
// pinned libavcodec output, but this interface does not claim a complete
// native TS 102 366 decoder.
class ICoreDecoder {
public:
    virtual ~ICoreDecoder() = default;

    virtual CoreDecodeResult open(const CoreDecoderConfig &config) = 0;
    virtual CoreDecodeResult pushPacket(const std::vector<std::uint8_t> &packet,
                                        std::int64_t timestamp,
                                        const CoreFrameCallback &callback) = 0;
    virtual CoreDecodeResult flush(const CoreFrameCallback &callback) = 0;
    virtual void reset() = 0;
    virtual void cancel() = 0;
};

struct Input {
    std::size_t unitIndex = 0;
    std::int64_t sourceStart = 0;
    eac3joc::FrameReport joc;
    std::vector<std::vector<float>> orderedPcm;
    std::vector<float> lfe;
    std::vector<eac3gate6c::MetadataUpdate> metadata;
};

struct ProcessResult {
    Disposition disposition = Disposition::Malformed;
    std::string reason;
    bool progressed = false;
    FlowStatus flow = FlowStatus::None;
    FailureStage stage = FailureStage::None;
};

struct Config {
    std::vector<double> qwin;
    std::int64_t decodedSourceSamples = 0;
    std::int64_t skipSamples = 0;
    std::int64_t discardPadding = 0;
    std::size_t callbackCapacity = 1;
    bool emitGate6c = false;
};

struct Report {
    std::size_t framesAccepted = 0;
    std::size_t batchesEmitted = 0;
    std::size_t flushes = 0;
    std::size_t resets = 0;
    std::size_t malformed = 0;
    std::size_t unsupported = 0;
    std::size_t callbackRejects = 0;
    std::string firstFailureReason;
};

class Session {
public:
    explicit Session(Config config = {});

    ProcessResult process(const Input &input,
                          const eac3gate6c::Callback &callback);
    ProcessResult flush(const eac3gate6c::Callback &callback);
    void reset();
    void cancel();

    const eac3joc::JocMathFrame &lastMathFrame() const { return lastMath_; }
    const eac3joc::JocQmfFrame &lastQmfFrame() const { return lastQmf_; }
    const Report &report() const { return report_; }

private:
    Config config_;
    eac3joc::JocMatrixState mathState_;
    eac3joc::JocQmfState qmfState_;
    eac3gate6c::Assembler gate6c_;
    eac3joc::JocMathFrame lastMath_;
    eac3joc::JocQmfFrame lastQmf_;
    bool canceled_ = false;
    bool blocked_ = false;
    bool flushed_ = false;
    Report report_;
};

struct SelfTestReport {
    bool pass = false;
    std::size_t cases = 0;
    std::size_t coreCases = 0;
    std::size_t sessionCases = 0;
    std::size_t gate6cCases = 0;
    std::string reason;
};

SelfTestReport runSelfTest(const std::vector<double> &qwin);

} // namespace eac3jocsession
