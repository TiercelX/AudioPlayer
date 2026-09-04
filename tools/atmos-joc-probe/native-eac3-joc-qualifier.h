#pragma once

// Gate J0A2: qualify only payloads accepted by the existing Gate 5A/OAMD
// parsers.  This layer is FFmpeg-free and never reconstructs JOC metadata,
// PCM, a renderer scene, or DRC state.

#include "joc-gate5a.h"
#include "native-eac3-emdf.h"
#include "oamd-b1.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace eac3native {

enum class JocQualificationDisposition {
    Qualified,
    Unsupported,
    Malformed,
};

struct NativeJocPayloadSource {
    unsigned id = 0U;
    std::size_t headerStartBit = 0U;
    std::size_t dataStartBit = 0U;
    std::size_t dataEndBit = 0U;
    std::size_t dataStartByte = 0U;
    std::size_t dataEndByte = 0U;
};

struct NativeJocQualification {
    JocQualificationDisposition disposition = JocQualificationDisposition::Malformed;
    std::string stage;
    std::string reason;
    std::size_t auIndex = 0U;
    std::int64_t timestamp = 0;
    FrameHeader frame;
    NativeEmdfResult emdf;
    std::vector<NativeJocPayloadSource> payloadSources;
    bool hasOamdReport = false;
    bool oamdAccepted = false;
    eac3oamd::B1Frame oamd;
    bool hasJocReport = false;
    bool jocAccepted = false;
    eac3joc::FrameReport joc;
};

// Qualify one already extracted J0A1 result.  Payload 11 and payload 14 must
// each occur exactly once.  Reports from the existing parsers are retained
// for diagnostics, but disposition is Qualified only when both parsers return
// their explicit accepted/pass disposition.
NativeJocQualification qualifyNativeEac3Emdf(
    const NativeEmdfResult &emdf, const std::string &jocTablePath,
    std::size_t auIndex, std::int64_t timestamp);

// Stateful stream wrapper.  A successful extraction advances by one 1536-
// sample timestamp step even when a recognized payload parser returns
// Unsupported.  Structural malformed input poisons the wrapper until reset.
class NativeEac3JocQualifier {
public:
    explicit NativeEac3JocQualifier(std::string jocTablePath);

    NativeJocQualification process(const std::vector<std::uint8_t> &bytes);
    void reset();

    bool poisoned() const { return poisoned_; }
    std::size_t framesProcessed() const { return framesProcessed_; }
    std::int64_t nextTimestamp() const { return nextTimestamp_; }

private:
    std::string jocTablePath_;
    std::size_t framesProcessed_ = 0U;
    std::int64_t nextTimestamp_ = 0;
    bool poisoned_ = false;
};

const char *toString(JocQualificationDisposition value);

} // namespace eac3native
