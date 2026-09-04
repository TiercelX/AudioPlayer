#include "native-eac3-joc-qualifier.h"

#include <limits>
#include <utility>

namespace eac3native {
namespace {

JocQualificationDisposition mapOamd(eac3oamd::B1Disposition disposition)
{
    return disposition == eac3oamd::B1Disposition::Unsupported
        ? JocQualificationDisposition::Unsupported
        : JocQualificationDisposition::Malformed;
}

JocQualificationDisposition mapJoc(eac3joc::ParseDisposition disposition)
{
    return disposition == eac3joc::ParseDisposition::Unsupported
        ? JocQualificationDisposition::Unsupported
        : JocQualificationDisposition::Malformed;
}

NativeJocQualification failure(std::size_t auIndex, std::int64_t timestamp,
                              std::string stage, std::string reason,
                              JocQualificationDisposition disposition)
{
    NativeJocQualification result;
    result.auIndex = auIndex;
    result.timestamp = timestamp;
    result.stage = std::move(stage);
    result.reason = std::move(reason);
    result.disposition = disposition;
    return result;
}

} // namespace

NativeJocQualification qualifyNativeEac3Emdf(
    const NativeEmdfResult &emdf, const std::string &jocTablePath,
    std::size_t auIndex, std::int64_t timestamp)
{
    NativeJocQualification result;
    result.auIndex = auIndex;
    result.timestamp = timestamp;
    result.frame = emdf.frame;
    result.emdf = emdf;
    if (emdf.disposition != EmdfDisposition::Accepted) {
        result.disposition = emdf.disposition == EmdfDisposition::Unsupported
            ? JocQualificationDisposition::Unsupported
            : JocQualificationDisposition::Malformed;
        result.stage = "emdf";
        result.reason = emdf.reason;
        return result;
    }
    if (emdf.containers.size() != 1U) {
        result.disposition = JocQualificationDisposition::Malformed;
        result.stage = "emdf";
        result.reason = emdf.containers.empty()
            ? "joc-qualifier-emdf-container-missing"
            : "joc-qualifier-emdf-container-duplicate";
        return result;
    }

    const NativeEmdfContainer &container = emdf.containers.front();
    const NativeEmdfPayload *oamdPayload = nullptr;
    const NativeEmdfPayload *jocPayload = nullptr;
    std::size_t oamdCount = 0U;
    std::size_t jocCount = 0U;
    for (const NativeEmdfPayload &payload : container.payloads) {
        if (payload.id == 11U) {
            ++oamdCount;
            oamdPayload = &payload;
            result.payloadSources.push_back(NativeJocPayloadSource {
                payload.id, payload.headerStartBit, payload.dataStartBit,
                payload.dataEndBit, payload.dataStartByte, payload.dataEndByte,
            });
        } else if (payload.id == 14U) {
            ++jocCount;
            jocPayload = &payload;
            result.payloadSources.push_back(NativeJocPayloadSource {
                payload.id, payload.headerStartBit, payload.dataStartBit,
                payload.dataEndBit, payload.dataStartByte, payload.dataEndByte,
            });
        }
    }
    if (oamdCount != 1U) {
        result.disposition = JocQualificationDisposition::Malformed;
        result.stage = "payload11";
        result.reason = oamdCount == 0U
            ? "joc-qualifier-payload11-missing"
            : "joc-qualifier-payload11-duplicate";
        return result;
    }
    if (jocCount != 1U) {
        result.disposition = JocQualificationDisposition::Malformed;
        result.stage = "payload14";
        result.reason = jocCount == 0U
            ? "joc-qualifier-payload14-missing"
            : "joc-qualifier-payload14-duplicate";
        return result;
    }

    result.oamd = eac3oamd::parseB1(oamdPayload->bytes);
    result.hasOamdReport = true;
    result.oamdAccepted = result.oamd.disposition == eac3oamd::B1Disposition::Pass;
    if (!result.oamdAccepted) {
        result.disposition = mapOamd(result.oamd.disposition);
        result.stage = "payload11";
        result.reason = result.oamd.reason;
        return result;
    }

    result.joc = eac3joc::parsePayload(jocPayload->bytes, jocTablePath);
    result.hasJocReport = true;
    result.jocAccepted = result.joc.disposition == eac3joc::ParseDisposition::Pass;
    if (!result.jocAccepted) {
        result.disposition = mapJoc(result.joc.disposition);
        result.stage = "payload14";
        result.reason = result.joc.reason;
        return result;
    }

    result.disposition = JocQualificationDisposition::Qualified;
    result.stage = "qualification";
    result.reason = "payload11-and-payload14-accepted";
    return result;
}

NativeEac3JocQualifier::NativeEac3JocQualifier(std::string jocTablePath)
    : jocTablePath_(std::move(jocTablePath))
{
}

NativeJocQualification NativeEac3JocQualifier::process(
    const std::vector<std::uint8_t> &bytes)
{
    if (poisoned_) {
        return failure(framesProcessed_, nextTimestamp_, "state",
                       "joc-qualifier-poisoned-reset-required",
                       JocQualificationDisposition::Malformed);
    }
    const std::size_t auIndex = framesProcessed_;
    const std::int64_t timestamp = nextTimestamp_;
    const NativeEmdfResult emdf = extractNativeEac3Emdf(bytes, auIndex);
    NativeJocQualification result = qualifyNativeEac3Emdf(
        emdf, jocTablePath_, auIndex, timestamp);
    if (emdf.disposition == EmdfDisposition::Accepted
        && result.disposition != JocQualificationDisposition::Malformed) {
        if (nextTimestamp_ > std::numeric_limits<std::int64_t>::max() - 1536) {
            poisoned_ = true;
            return failure(auIndex, timestamp, "state",
                           "joc-qualifier-timestamp-overflow",
                           JocQualificationDisposition::Malformed);
        }
        ++framesProcessed_;
        nextTimestamp_ += 1536;
    } else if (result.disposition == JocQualificationDisposition::Malformed) {
        poisoned_ = true;
    }
    return result;
}

void NativeEac3JocQualifier::reset()
{
    framesProcessed_ = 0U;
    nextTimestamp_ = 0;
    poisoned_ = false;
}

const char *toString(JocQualificationDisposition value)
{
    switch (value) {
    case JocQualificationDisposition::Qualified: return "qualified";
    case JocQualificationDisposition::Unsupported: return "unsupported";
    case JocQualificationDisposition::Malformed: return "malformed";
    }
    return "malformed";
}

} // namespace eac3native
