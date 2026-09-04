#include "oamd-b2b.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace eac3oamd {
namespace {

constexpr unsigned kMaxObjects = 159U;
constexpr unsigned kMaxBlocks = 8U;
constexpr std::array<float, 16> kDistanceFactors {
    1.1f, 1.3f, 1.6f, 2.0f, 2.5f, 3.2f, 4.0f, 5.0f,
    6.3f, 7.9f, 10.0f, 12.6f, 15.8f, 20.0f, 25.1f, 50.1f,
};
constexpr std::array<float, 4> kDepthFactors {0.25f, 0.5f, 1.0f, 2.0f};

bool finite(float value)
{
    return std::isfinite(value);
}

void setError(B2bApplyResult *result, B2bDisposition disposition, const char *reason)
{
    result->disposition = disposition;
    result->reason = reason;
}

void setDefaults(B2bObjectState *object, unsigned index, bool helper)
{
    *object = B2bObjectState {};
    object->objectIndex = index;
    object->lfeHelper = helper;
    object->gainMinusInfinity = true;
    object->gainDb = 0.0f;
    object->priority = 0.0f;
    object->position.valid = true;
    object->position.x = object->position.codedX = object->position.standardX = 0.5f;
    object->position.y = object->position.codedY = object->position.standardY = 0.5f;
    object->position.z = object->position.codedZ = object->position.standardZ = 0.0f;
    object->position.screenAnchored = false;
    object->position.distanceSpecified = false;
    object->size = {0.0f, 0.0f, 0.0f};
    object->effectiveSizePresent = false;
    object->effectiveSizeIndex = 0U;
    object->zoneConstraints = {true, true, true, true, true, true};
    object->elevation = true;
    object->snap = false;
}

bool decodeGain(const B2aObjectInfo &raw, float prior, bool priorValid,
               bool priorMinusInfinity, float *gain, bool *minusInfinity)
{
    if (!gain || !minusInfinity || raw.gainIndex > 3U) {
        return false;
    }
    switch (raw.gainIndex) {
    case 0U:
        *gain = 0.0f;
        *minusInfinity = false;
        return true;
    case 1U:
        *gain = 0.0f;
        *minusInfinity = true;
        return true;
    case 2U:
        if (!raw.gainBitsPresent || raw.gainBits > 63U) {
            return false;
        }
        *gain = raw.gainBits < 15U
            ? static_cast<float>(15U - raw.gainBits)
            : static_cast<float>(14U - raw.gainBits);
        *minusInfinity = false;
        return finite(*gain);
    case 3U:
        // gain index 3 reuses the preceding object in this metadata block,
        // and is the defined 0 dB value for the first object in that block.
        *gain = priorValid ? prior : 0.0f;
        *minusInfinity = priorValid && priorMinusInfinity;
        return finite(*gain);
    }
    return false;
}

std::array<bool, 6> zoneConstraints(unsigned index)
{
    switch (index) {
    // Decoder-interface order: Screen, Side, Surround, Back,
    // Centre-and-back, Top-bottom.  The syntax carries elevation separately;
    // the last entry is synchronized by applyRender below.
    case 0U: return {true, true, true, true, true, true};
    case 1U: return {true, true, true, false, true, true};
    case 2U: return {true, false, true, true, true, true};
    case 3U: return {false, false, false, false, true, true};
    case 4U: return {true, false, false, false, false, true};
    case 5U: return {false, false, true, false, false, true};
    default: return {false, false, false, false, false, false};
    }
}

bool recomputePosition(B2bPosition *position, std::string *reason)
{
    if (!position || !reason) return false;
    if (position->screenAnchored) {
        // Screen-reference coordinates are not projected through the room
        // distance factor.  Keep coded coordinates and screen/depth factors
        // available to the downstream renderer.
        position->x = position->codedX;
        position->y = position->codedY;
        position->z = position->codedZ;
        return finite(position->x) && finite(position->y) && finite(position->z);
    }
    position->x = position->codedX;
    position->y = position->codedY;
    position->z = position->codedZ;
    if (position->distanceSpecified && !position->distanceInfinite) {
        const float dx = position->codedX - 0.5f;
        const float dy = position->codedY - 0.5f;
        const float dz = position->codedZ;
        float t = std::numeric_limits<float>::infinity();
        auto boundary = [&t](float origin, float direction, float low, float high) {
            if (direction > 0.0f) t = std::min(t, (high - origin) / direction);
            else if (direction < 0.0f) t = std::min(t, (low - origin) / direction);
        };
        boundary(0.5f, dx, 0.0f, 1.0f);
        boundary(0.5f, dy, 0.0f, 1.0f);
        boundary(0.0f, dz, -1.0f, 1.0f);
        if (!finite(t) || t <= 0.0f) {
            *reason = "distance-projection-has-no-finite-boundary";
            return false;
        }
        const float ix = 0.5f + dx * t;
        const float iy = 0.5f + dy * t;
        const float iz = dz * t;
        position->x = 0.5f + (ix - 0.5f) * position->distanceFactor;
        position->y = 0.5f + (iy - 0.5f) * position->distanceFactor;
        position->z = iz * position->distanceFactor;
    }
    return finite(position->x) && finite(position->y) && finite(position->z);
}

bool decodePosition(const B2aObjectInfo &raw, const B2bObjectState &prior,
                    B2bPosition *position, B2bApplyStats *stats,
                    std::string *reason)
{
    if (!position || !stats || !reason) {
        return false;
    }
    B2bPosition next = position->valid ? *position : B2bPosition {};
    float x = 0.5f;
    float y = 0.5f;
    float z = 0.0f;
    if (raw.differentialPosition) {
        if (!prior.renderValid || !prior.position.valid) {
            *reason = "differential-position-without-previous-state";
            return false;
        }
        auto signed3 = [](unsigned value) -> int {
            return value >= 4U ? static_cast<int>(value) - 8 : static_cast<int>(value);
        };
        x = std::clamp(prior.position.standardX
                           + static_cast<float>(signed3(raw.differentialPositionBits[0])) / 62.0f,
                       0.0f, 1.0f);
        y = std::clamp(prior.position.standardY
                           + static_cast<float>(signed3(raw.differentialPositionBits[1])) / 62.0f,
                       0.0f, 1.0f);
        z = std::clamp(prior.position.standardZ
                           + static_cast<float>(signed3(raw.differentialPositionBits[2])) / 15.0f,
                       -1.0f, 1.0f);
        ++stats->differentialPositionCount;
    } else {
        if (!raw.absolutePositionPresent || raw.absoluteXBits > 63U
            || raw.absoluteYBits > 63U || raw.absoluteZBits > 15U) {
            *reason = "absolute-position-codeword-out-of-range";
            return false;
        }
        x = std::min(1.0f, static_cast<float>(raw.absoluteXBits) / 62.0f);
        y = std::min(1.0f, static_cast<float>(raw.absoluteYBits) / 62.0f);
        z = (raw.absoluteZSignBits ? 1.0f : -1.0f)
            * std::min(1.0f, static_cast<float>(raw.absoluteZBits) / 15.0f);
        ++stats->absolutePositionCount;
    }
    if (!finite(x) || !finite(y) || !finite(z)) {
        *reason = "nonfinite-standard-position";
        return false;
    }
    next.standardX = x;
    next.standardY = y;
    next.standardZ = z;
    next.codedX = x;
    next.codedY = y;
    next.codedZ = z;
    next.x = x;
    next.y = y;
    next.z = z;
    next.valid = true;
    next.distanceSpecified = raw.distanceSpecified;
    next.distanceInfinite = raw.distanceSpecified && raw.objectAtInfinity;
    next.distanceFactor = 0.0f;
    if (raw.distanceSpecified && !raw.objectAtInfinity) {
        if (!raw.distanceFactorPresent || raw.distanceFactorIndex >= kDistanceFactors.size()) {
            *reason = "distance-factor-codeword-out-of-range";
            return false;
        }
        const float factor = kDistanceFactors[raw.distanceFactorIndex];
        next.distanceFactor = factor;
    }
    if (!recomputePosition(&next, reason)) {
        if (reason->empty()) *reason = "nonfinite-distance-position";
        return false;
    }
    *position = next;
    return true;
}

bool decodeSize(const B2aObjectInfo &raw, std::array<float, 3> *size,
                std::string *reason)
{
    if (!size || !reason || !raw.sizePresent) {
        return false;
    }
    if (raw.sizeIndex == 3U) {
        *reason = "reserved-size-index";
        return false;
    }
    if (raw.sizeIndex == 0U) {
        *size = {0.0f, 0.0f, 0.0f};
    } else if (raw.sizeIndex == 1U) {
        if (raw.sizeBits > 31U) { *reason = "scalar-size-out-of-range"; return false; }
        const float value = static_cast<float>(raw.sizeBits) / 31.0f;
        *size = {value, value, value};
    } else {
        if (raw.widthBits > 31U || raw.depthBits > 31U || raw.heightBits > 31U) {
            *reason = "three-dimensional-size-out-of-range";
            return false;
        }
        *size = {static_cast<float>(raw.widthBits) / 31.0f,
                 static_cast<float>(raw.depthBits) / 31.0f,
                 static_cast<float>(raw.heightBits) / 31.0f};
    }
    return finite((*size)[0]) && finite((*size)[1]) && finite((*size)[2]);
}

bool applyBasic(const B2aObjectInfo &raw, B2bObjectState *object,
                float *blockGain, bool *blockGainValid, bool *blockGainMinusInfinity,
                B2bApplyStats *stats,
                std::string *reason)
{
    const auto resolveBlockGain = [&]() {
        *blockGain = object->gainDb;
        *blockGainValid = true;
        *blockGainMinusInfinity = object->gainMinusInfinity;
    };
    if (raw.basicInfoStatus == 0U) {
        object->gainMinusInfinity = true;
        object->gainDb = 0.0f;
        object->priority = 0.0f;
        object->basicValid = true;
        ++stats->defaultBasicCount;
        ++stats->minusInfinityGainCount;
        resolveBlockGain();
        return true;
    }
    if (raw.basicInfoStatus == 2U) {
        if (!object->basicValid) { *reason = "basic-reuse-without-previous-state"; return false; }
        ++stats->reuseBasicCount;
        resolveBlockGain();
        return true;
    }
    if (raw.basicInfoStatus != 1U && raw.basicInfoStatus != 3U) {
        *reason = "reserved-basic-status";
        return false;
    }
    const bool gainPresent = raw.basicInfoStatus == 1U || raw.basicInfoPresence[1];
    const bool priorityPresent = raw.basicInfoStatus == 1U || raw.basicInfoPresence[0];
    if (gainPresent) {
        bool minusInfinity = false;
        if (!raw.gainIndexPresent || !decodeGain(raw, *blockGain, *blockGainValid,
                                                  *blockGainMinusInfinity,
                                                  &object->gainDb, &minusInfinity)) {
            *reason = "invalid-gain-update";
            return false;
        }
        object->gainMinusInfinity = minusInfinity;
        *blockGain = object->gainDb;
        *blockGainValid = true;
        *blockGainMinusInfinity = minusInfinity;
        if (minusInfinity) ++stats->minusInfinityGainCount;
    } else if (!object->basicValid) {
        *reason = "mixed-basic-gain-reuse-without-previous-state";
        return false;
    }
    if (priorityPresent) {
        if (raw.defaultPriorityPresent && raw.defaultPriority) {
            object->priority = 1.0f;
        } else if (raw.priorityBitsPresent && raw.priorityBits <= 31U) {
            object->priority = static_cast<float>(raw.priorityBits) / 32.0f;
        } else {
            *reason = "invalid-priority-update";
            return false;
        }
    } else if (!object->basicValid) {
        *reason = "mixed-basic-priority-reuse-without-previous-state";
        return false;
    }
    object->basicValid = true;
    if (raw.basicInfoStatus == 1U) ++stats->fullBasicCount;
    else ++stats->mixedBasicCount;
    // Even a mixed/reuse basic update resolves an object gain.  The next
    // object using gain_idx=3 consumes that resolved value, not only an
    // explicitly signalled gain codeword.
    resolveBlockGain();
    return true;
}

bool applyRender(const B2aObjectInfo &raw, B2bObjectState *object,
                 const B2bObjectState &prior, B2bApplyStats *stats,
                 std::string *reason)
{
    if (raw.renderInfoStatus == 0U) {
        object->position = B2bPosition {};
        object->position.valid = true;
        object->position.x = object->position.codedX = object->position.standardX = 0.5f;
        object->position.y = object->position.codedY = object->position.standardY = 0.5f;
        object->position.z = object->position.codedZ = object->position.standardZ = 0.0f;
        object->size = {0.0f, 0.0f, 0.0f};
        object->effectiveSizePresent = false;
        object->effectiveSizeIndex = 0U;
        object->zoneConstraints = {true, true, true, true, true, true};
        object->elevation = true;
        object->snap = false;
        object->renderValid = true;
        ++stats->defaultRenderCount;
        return true;
    }
    if (raw.renderInfoStatus == 2U) {
        if (!object->renderValid) { *reason = "render-reuse-without-previous-state"; return false; }
        ++stats->reuseRenderCount;
        return true;
    }
    if (raw.renderInfoStatus != 1U && raw.renderInfoStatus != 3U) {
        *reason = "reserved-render-status";
        return false;
    }
    const auto &presence = raw.renderInfoPresence;
    if (presence[0]) {
        if (!decodePosition(raw, prior, &object->position, stats, reason)) return false;
    } else if (!object->renderValid) {
        *reason = "mixed-render-position-reuse-without-previous-state";
        return false;
    }
    if (presence[1]) {
        if (!raw.zonePresent || raw.zoneConstraintsIndex >= 6U) {
            *reason = raw.zoneConstraintsIndex >= 6U
                ? "reserved-zone-constraints-index" : "invalid-zone-update";
            return false;
        }
        object->zoneConstraints = zoneConstraints(raw.zoneConstraintsIndex);
        object->elevation = raw.enableElevation;
    } else if (!object->renderValid) {
        *reason = "mixed-render-zone-reuse-without-previous-state";
        return false;
    }
    if (presence[2]) {
        if (!decodeSize(raw, &object->size, reason)) return false;
        object->effectiveSizePresent = true;
        object->effectiveSizeIndex = raw.sizeIndex;
    } else if (!object->renderValid) {
        *reason = "mixed-render-size-reuse-without-previous-state";
        return false;
    }
    if (presence[3]) {
        if (!raw.screenReferencePresent) { *reason = "invalid-screen-update"; return false; }
        object->position.screenAnchored = raw.useScreenReference;
        if (raw.useScreenReference) {
            if (raw.screenFactorBits > 7U || raw.depthFactorIndex >= kDepthFactors.size()) {
                *reason = "screen-factor-codeword-out-of-range";
                return false;
            }
            object->position.screenFactor = static_cast<float>(raw.screenFactorBits + 1U) / 8.0f;
            object->position.depthFactor = kDepthFactors[raw.depthFactorIndex];
        } else {
            object->position.screenFactor = 0.0f;
            object->position.depthFactor = 0.0f;
        }
    } else if (!object->renderValid) {
        *reason = "mixed-render-screen-reuse-without-previous-state";
        return false;
    }
    if (raw.snapPresent) object->snap = raw.snap;
    object->zoneConstraints[5] = object->elevation;
    if (!recomputePosition(&object->position, reason)) return false;
    object->renderValid = true;
    if (raw.renderInfoStatus == 1U) ++stats->fullRenderCount;
    else ++stats->mixedRenderCount;
    return true;
}

bool finiteObject(const B2bObjectState &object)
{
    const auto &p = object.position;
    return finite(object.gainDb) && finite(object.priority)
        && finite(p.x) && finite(p.y) && finite(p.z)
        && finite(p.codedX) && finite(p.codedY) && finite(p.codedZ)
        && finite(p.standardX) && finite(p.standardY) && finite(p.standardZ)
        && finite(p.distanceFactor) && finite(p.screenFactor) && finite(p.depthFactor)
        && finite(object.size[0]) && finite(object.size[1]) && finite(object.size[2]);
}

bool applyExtendedPosition(B2bPosition *position,
                            const ExtendedPositionState &extended,
                            std::string *reason)
{
    if (!position || !reason) return false;
    position->extendedPrecisionPresent = extended.presence;
    position->extendedPrecision = extended.values;
    // TS 103 420 5.6.1.1.8-14: extended precision is five units in the
    // corresponding standard codeword denominator.  standard* remains the
    // differential-code base; coded* is the effective value for projection.
    position->codedX = std::clamp(
        position->standardX + static_cast<float>(extended.values[0]) * 5.0F / 62.0F,
        0.0F, 1.0F);
    position->codedY = std::clamp(
        position->standardY + static_cast<float>(extended.values[1]) * 5.0F / 62.0F,
        0.0F, 1.0F);
    position->codedZ = std::clamp(
        position->standardZ + static_cast<float>(extended.values[2]) * 5.0F / 15.0F,
        -1.0F, 1.0F);
    return recomputePosition(position, reason);
}

} // namespace

const char *b2bDispositionText(B2bDisposition disposition)
{
    switch (disposition) {
    case B2bDisposition::Pass: return "PASS";
    case B2bDisposition::Unsupported: return "UNSUPPORTED";
    case B2bDisposition::Malformed: return "MALFORMED";
    }
    return "MALFORMED";
}

B2bApplyResult B2bState::applyFrame(const B2aFrame &frame,
                                    const std::vector<bool> &objectInBedOrIsf,
                                    B2bFrameOutput *output, bool resetBefore,
                                    const B1Frame *metadata)
{
    B2bApplyResult result;
    if (!output) { setError(&result, B2bDisposition::Malformed, "null-output"); return result; }
    if (frame.disposition != B2aDisposition::Pass) {
        setError(&result, B2bDisposition::Malformed, "input-b2a-frame-not-pass");
        return result;
    }
    if (frame.objectInfoBlockCount == 0U || frame.objectInfoBlockCount > kMaxBlocks) {
        setError(&result, B2bDisposition::Malformed, "object-info-shape-mismatch");
        return result;
    }
    if (objectInBedOrIsf.empty() || objectInBedOrIsf.size() > kMaxObjects) {
        setError(&result, B2bDisposition::Malformed, "helper-map-size-out-of-range");
        return result;
    }
    const unsigned objectCount = static_cast<unsigned>(objectInBedOrIsf.size());
    if (frame.objectInfo.size() != static_cast<std::size_t>(objectCount)
        * frame.objectInfoBlockCount) {
        setError(&result, B2bDisposition::Malformed, "object-info-count-mismatch");
        return result;
    }

    B2bApplyStats stats;
    const bool shapeChanged = !valid_ || resetBefore || objectCount_ != objectCount
        || helperMap_ != objectInBedOrIsf || objects_.size() != objectCount;
    AdditionalElements additional;
    if (metadata) {
        const std::vector<float> priorDivergence = shapeChanged ? std::vector<float> {}
                                                                  : divergence_;
        const std::vector<bool> priorDivergenceValid = shapeChanged
            ? std::vector<bool> {} : divergenceValid_;
        additional = parseAdditionalElements(*metadata, frame, objectInBedOrIsf,
                                              priorDivergence,
                                              priorDivergenceValid);
        if (additional.disposition != AdditionalDisposition::Pass) {
            result.disposition = additional.disposition == AdditionalDisposition::Unsupported
                ? B2bDisposition::Unsupported : B2bDisposition::Malformed;
            result.reason = additional.reason;
            return result;
        }
    }
    std::vector<B2bObjectState> candidate;
    if (shapeChanged) {
        stats.resetCount = 1U;
        candidate.resize(objectCount);
        for (unsigned index = 0; index < objectCount; ++index) {
            setDefaults(&candidate[index], index, objectInBedOrIsf[index]);
        }
    } else {
        candidate = objects_;
    }

    std::vector<B2bFrameOutput::BlockSnapshot> blockSnapshots;
    blockSnapshots.reserve(frame.objectInfoBlockCount);
    for (unsigned block = 0; block < frame.objectInfoBlockCount; ++block) {
        float blockGain = 0.0f;
        bool blockGainValid = false;
        bool blockGainMinusInfinity = false;
        for (unsigned index = 0; index < objectCount; ++index) {
            const auto &raw = frame.objectInfo[static_cast<std::size_t>(index)
                                                * frame.objectInfoBlockCount + block];
            if (raw.objectIndex != index || raw.blockIndex != block) {
                setError(&result, B2bDisposition::Malformed, "object-info-order-mismatch");
                return result;
            }
            if (block == 0U && !raw.objectNotActive) {
                // B2A's bounded syntax establishes block zero as the full
                // baseline.  Later blocks are the only legal place for
                // reuse/mixed/differential updates.
                if (raw.basicInfoStatus != 1U
                    || (objectInBedOrIsf[index]
                        ? raw.renderInfoStatus != 0U
                        : raw.renderInfoStatus != 1U)
                    || raw.differentialPosition) {
                    setError(&result, B2bDisposition::Malformed,
                             "block-zero-is-not-b2a-full-baseline");
                    return result;
                }
            }
            if (raw.basicInfoStatus > 3U || raw.renderInfoStatus > 3U
                || raw.gainIndex > 3U || raw.sizeIndex > 3U
                || raw.zoneConstraintsIndex > 7U) {
                setError(&result, B2bDisposition::Unsupported, "reserved-property-codeword");
                return result;
            }
            B2bObjectState next = candidate[index];
            next.objectIndex = index;
            next.lfeHelper = objectInBedOrIsf[index];
            next.raw = raw;
            const B2bObjectState prior = candidate[index];
            if (raw.objectNotActive) {
                next.active = false;
                candidate[index] = next;
                ++stats.inactiveCount;
                continue;
            }
            next.active = true;
            ++stats.activeCount;
            if (!applyBasic(raw, &next, &blockGain, &blockGainValid,
                            &blockGainMinusInfinity, &stats, &result.reason)) {
                result.disposition = result.reason.find("reserved-") == 0
                    ? B2bDisposition::Unsupported : B2bDisposition::Malformed;
                return result;
            }
            if (!applyRender(raw, &next, prior, &stats, &result.reason)) {
                result.disposition = result.reason.find("reserved-") == 0
                    ? B2bDisposition::Unsupported : B2bDisposition::Malformed;
                return result;
            }
            if (!finiteObject(next)) {
                setError(&result, B2bDisposition::Malformed, "nonfinite-property-conversion");
                return result;
            }
            ++stats.finitePropertyCount;
            candidate[index] = next;
        }
        if (additional.extendedPresent) {
            if (additional.extended.objectCount != objectCount
                || additional.extended.blockCount != frame.objectInfoBlockCount) {
                setError(&result, B2bDisposition::Malformed,
                         "extended-element-shape-mismatch");
                return result;
            }
            for (unsigned index = 0U; index < objectCount; ++index) {
                B2bObjectState &next = candidate[index];
                const std::size_t offset = static_cast<std::size_t>(index)
                    * frame.objectInfoBlockCount + block;
                if (additional.extended.divergenceBlockPresent) {
                    const DivergenceState &divergence =
                        additional.extended.divergence[offset];
                    next.divergencePresent = divergence.present;
                    next.divergenceReused = divergence.reused;
                    next.divergenceMode = divergence.mode;
                    next.divergenceIndex = divergence.mode == 0U
                        ? divergence.table : divergence.code;
                    next.divergence = divergence.value;
                }
                if (additional.extended.extendedPositionBlockPresent
                    && !objectInBedOrIsf[index]
                    && !next.raw.objectNotActive && next.renderValid) {
                    const ExtendedPositionState &position =
                        additional.extended.extendedPosition[offset];
                    std::string positionReason;
                    // The syntax explicitly defines absent axis values as 0,
                    // so a present block also clears a prior extension.
                    if (!applyExtendedPosition(&next.position, position,
                                                &positionReason)) {
                        setError(&result, B2bDisposition::Malformed,
                                 positionReason.c_str());
                        return result;
                    }
                }
                if (additional.trimPresent && additional.trim.disableTrimPerObject
                    && index < additional.trim.objectTrimDisabled.size()) {
                    next.trimDisabled = additional.trim.objectTrimDisabled[index];
                } else if (additional.trimPresent) {
                    next.trimDisabled = false;
                }
                if (!finiteObject(next)) {
                    setError(&result, B2bDisposition::Malformed,
                             "nonfinite-extended-property-conversion");
                    return result;
                }
            }
        } else if (additional.trimPresent) {
            for (unsigned index = 0U; index < objectCount; ++index) {
                candidate[index].trimDisabled =
                    additional.trim.disableTrimPerObject
                    && index < additional.trim.objectTrimDisabled.size()
                    && additional.trim.objectTrimDisabled[index];
            }
        }
        blockSnapshots.push_back(B2bFrameOutput::BlockSnapshot {block, candidate});
    }

    B2bFrameOutput nextOutput;
    nextOutput.objectCount = objectCount;
    nextOutput.objects = candidate;
    nextOutput.trimPresent = additional.trimPresent;
    if (additional.trimPresent) nextOutput.trim = additional.trim;
    nextOutput.blockSnapshots = std::move(blockSnapshots);
    for (unsigned index = 0; index < objectCount; ++index) {
        if (!objectInBedOrIsf[index]) nextOutput.dynamicObjectIndices.push_back(index);
    }
    objects_ = std::move(candidate);
    divergence_.resize(objectCount);
    divergenceValid_.resize(objectCount);
    for (unsigned index = 0U; index < objectCount; ++index) {
        divergence_[index] = objects_[index].divergence;
        divergenceValid_[index] = objects_[index].divergencePresent;
    }
    helperMap_ = objectInBedOrIsf;
    objectCount_ = objectCount;
    valid_ = true;
    *output = std::move(nextOutput);
    result.stats = stats;
    result.disposition = B2bDisposition::Pass;
    result.reason = "transactional-object-property-state";
    return result;
}

void B2bState::reset()
{
    objectCount_ = 0;
    helperMap_.clear();
    objects_.clear();
    divergence_.clear();
    divergenceValid_.clear();
    valid_ = false;
}

B2bSelfTestReport runB2bSelfTest()
{
    B2bSelfTestReport report;
    auto check = [&report](bool pass, const char *name, std::size_t *counter) {
        ++report.cases;
        if (counter) ++*counter;
        if (!pass && report.reason.empty()) report.reason = name;
    };
    auto frame = [](unsigned count, unsigned blocks = 1U) {
        B2aFrame value;
        value.disposition = B2aDisposition::Pass;
        value.objectInfoBlockCount = blocks;
        value.blocks.resize(blocks);
        value.objectInfo.reserve(static_cast<std::size_t>(count) * blocks);
        for (unsigned index = 0; index < count; ++index) {
            for (unsigned block = 0; block < blocks; ++block) {
                B2aObjectInfo object;
                object.objectIndex = index;
                object.blockIndex = block;
                object.basicInfoStatus = 1U;
                object.basicInfoPresence = {true, true};
                object.gainIndexPresent = true;
                object.gainIndex = 2U;
                object.gainBitsPresent = true;
                object.gainBits = 10U;
                object.defaultPriorityPresent = true;
                object.defaultPriority = false;
                object.priorityBitsPresent = true;
                object.priorityBits = 16U;
                object.renderInfoStatus = 1U;
                object.renderInfoPresence = {true, true, true, true};
                object.absolutePositionPresent = true;
                object.absoluteXBits = 32U;
                object.absoluteYBits = 32U;
                object.absoluteZSignBits = 1U;
                object.absoluteZBits = 8U;
                object.differentialPositionPresent = true;
                object.distanceSpecified = false;
                object.zonePresent = true;
                object.zoneConstraintsIndex = 0U;
                object.enableElevation = true;
                object.sizePresent = true;
                object.sizeIndex = 1U;
                object.sizeBits = 8U;
                object.screenReferencePresent = true;
                object.useScreenReference = false;
                object.snapPresent = true;
                object.snap = false;
                value.objectInfo.push_back(object);
            }
        }
        return value;
    };
    const auto near = [](float left, float right) {
        return std::fabs(left - right) < 0.00001f;
    };
    const auto outputEquivalent = [](const B2bFrameOutput &left,
                                     const B2bFrameOutput &right) {
        if (left.objectCount != right.objectCount
            || left.dynamicObjectIndices != right.dynamicObjectIndices
            || left.objects.size() != right.objects.size()
            || left.blockSnapshots.size() != right.blockSnapshots.size()) {
            return false;
        }
        for (std::size_t index = 0; index < left.objects.size(); ++index) {
            if (left.objects[index].objectIndex != right.objects[index].objectIndex
                || left.objects[index].active != right.objects[index].active
                || left.objects[index].gainDb != right.objects[index].gainDb
                || left.objects[index].raw.gainIndex != right.objects[index].raw.gainIndex) {
                return false;
            }
        }
        for (std::size_t index = 0; index < left.blockSnapshots.size(); ++index) {
            if (left.blockSnapshots[index].blockIndex
                    != right.blockSnapshots[index].blockIndex
                || left.blockSnapshots[index].objects.size()
                    != right.blockSnapshots[index].objects.size()) {
                return false;
            }
        }
        return true;
    };
    const std::vector<bool> one {false};
    B2bFrameOutput output;
    B2bState state;
    B2aFrame defaults = frame(1U, 2U);
    defaults.objectInfo[1].basicInfoStatus = 0U;
    defaults.objectInfo[1].renderInfoStatus = 0U;
    auto defaultsResult = state.applyFrame(defaults, one, &output, true);
    check(defaultsResult.disposition == B2bDisposition::Pass
              && output.objects.size() == 1U && output.objects[0].gainMinusInfinity
              && output.objects[0].position.x == 0.5f,
          "default", &report.defaultCases);

    B2aFrame full = frame(1U);
    full.objectInfo[0].distanceSpecified = true;
    full.objectInfo[0].distanceFactorPresent = true;
    full.objectInfo[0].distanceFactorIndex = 1U;
    full.objectInfo[0].screenReferencePresent = true;
    full.objectInfo[0].useScreenReference = true;
    full.objectInfo[0].screenFactorBits = 3U;
    full.objectInfo[0].depthFactorIndex = 2U;
    auto fullResult = state.applyFrame(full, one, &output, true);
    check(fullResult.disposition == B2bDisposition::Pass && output.objects[0].active
              && output.objects[0].position.distanceFactor == 1.3f
              && output.objects[0].size[0] == 8.0f / 31.0f,
          "full", &report.fullCases);
    check(fullResult.stats.finitePropertyCount == 1U, "conversion", &report.conversionCases);

    B2aFrame gainChain = frame(2U);
    gainChain.objectInfo[0].gainIndex = 2U;
    gainChain.objectInfo[0].gainBits = 10U;
    gainChain.objectInfo[1].gainIndex = 3U;
    B2bState gainState;
    B2bFrameOutput gainOutput;
    auto gainResult = gainState.applyFrame(gainChain, std::vector<bool> {false, false},
                                           &gainOutput, true);
    check(gainResult.disposition == B2bDisposition::Pass
              && near(gainOutput.objects[0].gainDb, 5.0f)
              && near(gainOutput.objects[1].gainDb, 5.0f),
          "gain-prior-object", &report.gainCodeCases);
    B2aFrame gainDefault = frame(2U, 2U);
    gainDefault.objectInfo[1].basicInfoStatus = 0U;
    gainDefault.objectInfo[1].renderInfoStatus = 2U;
    gainDefault.objectInfo[3].gainIndex = 3U;
    gainDefault.objectInfo[3].renderInfoStatus = 2U;
    B2bFrameOutput gainDefaultOutput;
    auto gainDefaultResult = gainState.applyFrame(
        gainDefault, std::vector<bool> {false, false}, &gainDefaultOutput, true);
    check(gainDefaultResult.disposition == B2bDisposition::Pass
              && gainDefaultOutput.objects[0].gainMinusInfinity
              && gainDefaultOutput.objects[1].gainMinusInfinity,
          "gain-default-chain", &report.gainCodeCases);
    B2aFrame gainMinusInfinity = frame(1U);
    gainMinusInfinity.objectInfo[0].gainIndex = 1U;
    B2bFrameOutput gainMinusOutput;
    auto gainMinusResult = gainState.applyFrame(
        gainMinusInfinity, one, &gainMinusOutput, true);
    check(gainMinusResult.disposition == B2bDisposition::Pass
              && gainMinusOutput.objects[0].gainMinusInfinity,
          "gain-minus-infinity", &report.gainCodeCases);
    B2aFrame priority = frame(1U);
    priority.objectInfo[0].defaultPriority = true;
    B2bFrameOutput priorityOutput;
    auto priorityResult = gainState.applyFrame(priority, one, &priorityOutput, true);
    check(priorityResult.disposition == B2bDisposition::Pass
              && near(priorityOutput.objects[0].priority, 1.0f),
          "priority-default", &report.conversionCases);
    priority.objectInfo[0].defaultPriority = false;
    priority.objectInfo[0].priorityBits = 31U;
    auto priorityMaxResult = gainState.applyFrame(priority, one, &priorityOutput, true);
    check(priorityMaxResult.disposition == B2bDisposition::Pass
              && near(priorityOutput.objects[0].priority, 31.0f / 32.0f),
          "priority-coded-31", &report.conversionCases);
    priority.objectInfo[0].priorityBits = 0U;
    auto priorityZeroResult = gainState.applyFrame(priority, one, &priorityOutput, true);
    const B2bFrameOutput priorityZeroOutput = priorityOutput;
    priority.objectInfo[0].priorityBits = 16U;
    auto priorityMidResult = gainState.applyFrame(priority, one, &priorityOutput, true);
    check(priorityZeroResult.disposition == B2bDisposition::Pass
              && near(priorityZeroOutput.objects[0].priority, 0.0f)
              && priorityMidResult.disposition == B2bDisposition::Pass
              && near(priorityOutput.objects[0].priority, 16.0f / 32.0f),
          "priority-coded-0-16", &report.conversionCases);

    B2aFrame reuse = frame(1U, 2U);
    reuse.objectInfo[1].basicInfoStatus = 2U;
    reuse.objectInfo[1].renderInfoStatus = 2U;
    auto reuseResult = state.applyFrame(reuse, one, &output);
    check(reuseResult.disposition == B2bDisposition::Pass
              && output.objects[0].gainDb == 5.0f
              && output.objects[0].effectiveSizePresent
              && output.objects[0].effectiveSizeIndex == 1U
              && near(output.objects[0].size[0], 8.0f / 31.0f),
          "reuse", &report.reuseCases);

    B2aFrame mixed = frame(1U, 2U);
    mixed.objectInfo[1].basicInfoStatus = 3U;
    mixed.objectInfo[1].basicInfoPresence = {false, true};
    mixed.objectInfo[1].renderInfoStatus = 3U;
    mixed.objectInfo[1].renderInfoPresence = {true, false, false, false};
    mixed.objectInfo[1].differentialPosition = true;
    mixed.objectInfo[1].differentialPositionBits = {1U, 0U, 7U};
    mixed.objectInfo[1].distanceSpecified = false;
    mixed.objectInfo[1].snapPresent = true;
    auto mixedResult = state.applyFrame(mixed, one, &output);
    check(mixedResult.disposition == B2bDisposition::Pass
              && output.objects[0].effectiveSizePresent
              && output.objects[0].effectiveSizeIndex == 1U
              && near(output.objects[0].size[0], 8.0f / 31.0f),
          "mixed", &report.mixedCases);
    check(mixedResult.stats.differentialPositionCount == 1U, "differential", &report.differentialCases);

    B2aFrame absoluteBoundary = frame(1U, 2U);
    absoluteBoundary.objectInfo[0].absoluteXBits = 63U;
    absoluteBoundary.objectInfo[0].absoluteYBits = 0U;
    absoluteBoundary.objectInfo[0].absoluteZSignBits = 1U;
    absoluteBoundary.objectInfo[0].absoluteZBits = 15U;
    absoluteBoundary.objectInfo[1] = absoluteBoundary.objectInfo[0];
    absoluteBoundary.objectInfo[1].blockIndex = 1U;
    B2bState formulaState;
    B2bFrameOutput formulaOutput;
    auto absoluteBoundaryResult = formulaState.applyFrame(
        absoluteBoundary, one, &formulaOutput, true);
    check(absoluteBoundaryResult.disposition == B2bDisposition::Pass
              && near(formulaOutput.objects[0].position.x, 1.0f)
              && near(formulaOutput.objects[0].position.y, 0.0f)
              && near(formulaOutput.objects[0].position.z, 1.0f)
              && near(formulaOutput.objects[0].position.standardX, 1.0f),
          "absolute-62-15-boundary", &report.formulaBoundaryCases);
    B2aFrame differentialBoundary = frame(1U, 2U);
    differentialBoundary.objectInfo[0].absoluteXBits = 63U;
    differentialBoundary.objectInfo[0].absoluteYBits = 0U;
    differentialBoundary.objectInfo[0].absoluteZSignBits = 1U;
    differentialBoundary.objectInfo[0].absoluteZBits = 15U;
    differentialBoundary.objectInfo[1].basicInfoStatus = 2U;
    differentialBoundary.objectInfo[1].renderInfoStatus = 3U;
    differentialBoundary.objectInfo[1].renderInfoPresence = {true, false, false, false};
    differentialBoundary.objectInfo[1].differentialPosition = true;
    differentialBoundary.objectInfo[1].differentialPositionBits = {7U, 0U, 7U};
    differentialBoundary.objectInfo[1].snapPresent = true;
    auto differentialBoundaryResult = formulaState.applyFrame(
        differentialBoundary, one, &formulaOutput);
    check(differentialBoundaryResult.disposition == B2bDisposition::Pass,
          differentialBoundaryResult.disposition == B2bDisposition::Pass
              ? "differential-raw-history-result" : differentialBoundaryResult.reason.c_str(),
          &report.formulaBoundaryCases);
    check(near(formulaOutput.objects[0].position.x, 1.0f - 1.0f / 62.0f),
          "differential-normalized-x",
          &report.formulaBoundaryCases);
    check(near(formulaOutput.objects[0].position.z, 1.0f - 1.0f / 15.0f),
          "differential-normalized-z",
          &report.formulaBoundaryCases);

    const std::array<std::array<bool, 6>, 6> expectedZones {
        std::array<bool, 6> {true, true, true, true, true, true},
        std::array<bool, 6> {true, true, true, false, true, true},
        std::array<bool, 6> {true, false, true, true, true, true},
        std::array<bool, 6> {false, false, false, false, true, true},
        std::array<bool, 6> {true, false, false, false, false, true},
        std::array<bool, 6> {false, false, true, false, false, true},
    };
    bool zonesPass = true;
    for (unsigned zone = 0; zone < expectedZones.size(); ++zone) {
        B2aFrame zoneFrame = frame(1U);
        zoneFrame.objectInfo[0].zoneConstraintsIndex = zone;
        B2bFrameOutput zoneOutput;
        B2bState zoneState;
        const auto zoneResult = zoneState.applyFrame(zoneFrame, one, &zoneOutput, true);
        zonesPass = zonesPass && zoneResult.disposition == B2bDisposition::Pass
            && zoneOutput.objects[0].zoneConstraints == expectedZones[zone]
            && zoneOutput.objects[0].zoneConstraints[5]
                == zoneOutput.objects[0].elevation;
    }
    check(zonesPass, "zone-table20-mapping", &report.zoneMappingCases);

    B2aFrame inactive = frame(1U);
    inactive.objectInfo[0].objectNotActive = true;
    auto inactiveResult = state.applyFrame(inactive, one, &output);
    check(inactiveResult.disposition == B2bDisposition::Pass && !output.objects[0].active,
          "inactive", &report.inactiveCases);
    B2aFrame reactivate = frame(1U, 2U);
    reactivate.objectInfo[1].basicInfoStatus = 2U;
    reactivate.objectInfo[1].renderInfoStatus = 2U;
    auto reactivateResult = state.applyFrame(reactivate, one, &output);
    check(reactivateResult.disposition == B2bDisposition::Pass
              && output.objects[0].active && near(output.objects[0].gainDb, 5.0f),
          "inactive-reactivation-reuse", &report.inactiveCases);

    B2aFrame invalidReuse = frame(1U);
    invalidReuse.objectInfo[0].basicInfoStatus = 2U;
    invalidReuse.objectInfo[0].renderInfoStatus = 2U;
    B2bState fresh;
    B2bFrameOutput untouched;
    auto invalidResult = fresh.applyFrame(invalidReuse, one, &untouched, true);
    check(invalidResult.disposition != B2bDisposition::Pass, "reuse-without-state", &report.transactionalCases);
    B2aFrame invalidDifferential = frame(1U, 2U);
    invalidDifferential.objectInfo[0].objectNotActive = true;
    invalidDifferential.objectInfo[1].basicInfoStatus = 1U;
    invalidDifferential.objectInfo[1].basicInfoPresence = {true, true};
    invalidDifferential.objectInfo[1].gainIndexPresent = true;
    invalidDifferential.objectInfo[1].gainIndex = 0U;
    invalidDifferential.objectInfo[1].defaultPriorityPresent = true;
    invalidDifferential.objectInfo[1].defaultPriority = true;
    invalidDifferential.objectInfo[1].renderInfoStatus = 3U;
    invalidDifferential.objectInfo[1].renderInfoPresence = {true, false, false, false};
    invalidDifferential.objectInfo[1].differentialPosition = true;
    invalidDifferential.objectInfo[1].differentialPositionBits = {1U, 1U, 1U};
    auto invalidDifferentialResult = fresh.applyFrame(
        invalidDifferential, one, &untouched, true);
    check(invalidDifferentialResult.disposition != B2bDisposition::Pass,
          "differential-without-history", &report.transactionalCases);

    B2aFrame bad = frame(2U);
    bad.objectInfo[1].sizeIndex = 3U;
    std::vector<bool> two {false, false};
    B2bState transactional;
    B2bFrameOutput before;
    auto beforeResult = transactional.applyFrame(frame(2U), two, &before, true);
    B2bFrameOutput after = before;
    auto badResult = transactional.applyFrame(bad, two, &after);
    check(beforeResult.disposition == B2bDisposition::Pass
              && badResult.disposition != B2bDisposition::Pass
              && outputEquivalent(after, before),
          "transactional-late-failure", &report.transactionalCases);

    B2bState resetState;
    B2bFrameOutput resetOutput;
    resetState.applyFrame(full, one, &resetOutput, true);
    resetState.applyFrame(mixed, one, &resetOutput);
    B2bFrameOutput resetTarget;
    auto resetResult = resetState.applyFrame(full, one, &resetTarget, true);
    B2bState freshTarget;
    B2bFrameOutput freshOutput;
    auto freshResult = freshTarget.applyFrame(full, one, &freshOutput, true);
    check(resetResult.disposition == B2bDisposition::Pass
              && freshResult.disposition == B2bDisposition::Pass
              && resetTarget.objects[0].position.x == freshOutput.objects[0].position.x,
          "reset-equivalence", &report.resetCases);

    B2aFrame sizeScalar = frame(1U);
    sizeScalar.objectInfo[0].sizeIndex = 1U;
    sizeScalar.objectInfo[0].sizeBits = 31U;
    B2bState sizeState;
    B2bFrameOutput sizeOutput;
    auto sizeScalarResult = sizeState.applyFrame(sizeScalar, one, &sizeOutput, true);
    check(sizeScalarResult.disposition == B2bDisposition::Pass
              && near(sizeOutput.objects[0].size[0], 1.0f)
              && near(sizeOutput.objects[0].size[1], 1.0f)
              && near(sizeOutput.objects[0].size[2], 1.0f),
          "scalar-size-31-boundary", &report.conversionCases);
    B2aFrame size3d = frame(1U);
    size3d.objectInfo[0].sizeIndex = 2U;
    size3d.objectInfo[0].widthBits = 31U;
    size3d.objectInfo[0].depthBits = 0U;
    size3d.objectInfo[0].heightBits = 15U;
    auto size3dResult = sizeState.applyFrame(size3d, one, &sizeOutput, true);
    check(size3dResult.disposition == B2bDisposition::Pass
              && near(sizeOutput.objects[0].size[0], 1.0f)
              && near(sizeOutput.objects[0].size[1], 0.0f)
              && near(sizeOutput.objects[0].size[2], 15.0f / 31.0f),
          "three-dimensional-size-31-boundary", &report.conversionCases);

    B2aFrame distance = frame(1U);
    distance.objectInfo[0].distanceSpecified = true;
    distance.objectInfo[0].distanceFactorPresent = true;
    distance.objectInfo[0].distanceFactorIndex = 0U;
    B2bState distanceState;
    B2bFrameOutput distanceOutput;
    auto distanceResult = distanceState.applyFrame(distance, one, &distanceOutput, true);
    const B2bFrameOutput distanceFiniteOutput = distanceOutput;
    B2aFrame infinity = distance;
    infinity.objectInfo[0].objectAtInfinity = true;
    infinity.objectInfo[0].distanceFactorPresent = false;
    auto infinityResult = distanceState.applyFrame(infinity, one, &distanceOutput, true);
    check(distanceResult.disposition == B2bDisposition::Pass
              && near(distanceFiniteOutput.objects[0].position.distanceFactor, 1.1f)
              && distanceFiniteOutput.objects[0].position.x
                    > distanceFiniteOutput.objects[0].position.codedX
              && infinityResult.disposition == B2bDisposition::Pass
              && distanceOutput.objects[0].position.distanceInfinite,
          "distance-factor-and-infinity", &report.conversionCases);

    B2aFrame screen = frame(1U, 3U);
    screen.objectInfo[0].distanceSpecified = true;
    screen.objectInfo[0].distanceFactorPresent = true;
    screen.objectInfo[0].distanceFactorIndex = 0U;
    screen.objectInfo[0].useScreenReference = true;
    screen.objectInfo[0].screenFactorBits = 7U;
    screen.objectInfo[0].depthFactorIndex = 3U;
    screen.objectInfo[1].basicInfoStatus = 2U;
    screen.objectInfo[1].renderInfoStatus = 3U;
    screen.objectInfo[1].renderInfoPresence = {false, false, false, true};
    screen.objectInfo[1].useScreenReference = false;
    screen.objectInfo[1].snapPresent = true;
    screen.objectInfo[2].basicInfoStatus = 2U;
    screen.objectInfo[2].renderInfoStatus = 3U;
    screen.objectInfo[2].renderInfoPresence = {false, false, false, true};
    screen.objectInfo[2].useScreenReference = true;
    screen.objectInfo[2].screenFactorBits = 7U;
    screen.objectInfo[2].depthFactorIndex = 3U;
    screen.objectInfo[2].snapPresent = true;
    B2bState screenState;
    B2bFrameOutput screenOutput;
    auto screenResult = screenState.applyFrame(screen, one, &screenOutput, true);
    const float screenCodedX = screenOutput.objects[0].position.codedX;
    const float screenX = screenOutput.objects[0].position.x;
    check(screenResult.disposition == B2bDisposition::Pass
              && near(screenX, screenCodedX)
              && screenOutput.blockSnapshots.size() == 3U
              && screenOutput.blockSnapshots[0].objects[0].position.screenAnchored
              && !screenOutput.blockSnapshots[1].objects[0].position.screenAnchored
              && screenOutput.blockSnapshots[2].objects[0].position.screenAnchored
              && near(screenOutput.blockSnapshots[0].objects[0].position.x, screenCodedX)
              && !near(screenOutput.blockSnapshots[1].objects[0].position.x, screenCodedX)
              && near(screenOutput.blockSnapshots[2].objects[0].position.x, screenCodedX)
              && near(screenOutput.objects[0].position.screenFactor, 1.0f)
              && near(screenOutput.objects[0].position.depthFactor, 2.0f),
          "screen-distance-toggle", &report.snapCases);
    B2aFrame snapOff = frame(1U, 2U);
    snapOff.objectInfo[1].snap = false;
    B2bFrameOutput snapOutput;
    B2bState snapState;
    auto snapFull = snapState.applyFrame(full, one, &snapOutput, true);
    snapOff.objectInfo[1].renderInfoStatus = 3U;
    snapOff.objectInfo[1].renderInfoPresence = {false, false, false, false};
    snapOff.objectInfo[1].basicInfoStatus = 2U;
    auto snapMixed = snapState.applyFrame(snapOff, one, &snapOutput);
    check(snapFull.disposition == B2bDisposition::Pass
              && snapMixed.disposition == B2bDisposition::Pass
              && !snapOutput.objects[0].snap,
          "unconditional-snap-update", &report.snapCases);

    B2aFrame multiBlock = frame(1U, 2U);
    multiBlock.objectInfo[0].absoluteXBits = 10U;
    multiBlock.objectInfo[1].absoluteXBits = 50U;
    B2bState multiBlockState;
    B2bFrameOutput multiBlockOutput;
    auto multiBlockResult = multiBlockState.applyFrame(
        multiBlock, one, &multiBlockOutput, true);
    check(multiBlockResult.disposition == B2bDisposition::Pass
              && multiBlockOutput.blockSnapshots.size() == 2U
              && multiBlockOutput.blockSnapshots[0].blockIndex == 0U
              && multiBlockOutput.blockSnapshots[1].blockIndex == 1U
              && multiBlockOutput.blockSnapshots[0].objects[0].raw.absoluteXBits == 10U
              && multiBlockOutput.blockSnapshots[1].objects[0].raw.absoluteXBits == 50U
              && multiBlockOutput.objects[0].raw.absoluteXBits == 50U,
          "ordered-metadata-block-snapshots", &report.blockSnapshotCases);
    B2bFrameOutput eightBlockOutput;
    auto eightBlockResult = multiBlockState.applyFrame(
        frame(1U, 8U), one, &eightBlockOutput, true);
    check(eightBlockResult.disposition == B2bDisposition::Pass
              && eightBlockOutput.blockSnapshots.size() == 8U,
          "eight-block-snapshots", &report.blockSnapshotCases);

    std::vector<bool> helper(16U, false);
    helper[0] = true;
    B2aFrame lfe = frame(16U);
    lfe.objectInfo[0].renderInfoStatus = 0U;
    B2bState lfeState;
    B2bFrameOutput lfeOutput;
    auto lfeResult = lfeState.applyFrame(lfe, helper, &lfeOutput, true);
    check(lfeResult.disposition == B2bDisposition::Pass
              && lfeOutput.objects.size() == 16U
              && lfeOutput.objects[0].lfeHelper
              && lfeOutput.dynamicObjectIndices.size() == 15U
              && lfeOutput.dynamicObjectIndices.front() == 1U,
          "lfe-helper", &report.lfeHelperCases);

    B2bState shapeState;
    B2bFrameOutput shapeOutput;
    shapeState.applyFrame(frame(1U), one, &shapeOutput, true);
    const std::vector<bool> shapeMap(2U, false);
    B2bFrameOutput changedShapeOutput;
    const auto changedShapeResult = shapeState.applyFrame(
        frame(2U), shapeMap, &changedShapeOutput);
    B2bState freshShapeState;
    B2bFrameOutput freshShapeOutput;
    const auto freshShapeResult = freshShapeState.applyFrame(
        frame(2U), shapeMap, &freshShapeOutput, true);
    check(changedShapeResult.disposition == B2bDisposition::Pass
              && changedShapeResult.stats.resetCount == 1U
              && freshShapeResult.disposition == B2bDisposition::Pass
              && outputEquivalent(changedShapeOutput, freshShapeOutput),
          "object-count-shape-reset", &report.resetCases);
    B2bState helperShapeState;
    B2bFrameOutput helperShapeOutput;
    helperShapeState.applyFrame(frame(1U), one, &helperShapeOutput, true);
    B2aFrame helperShapeFrame = frame(1U);
    helperShapeFrame.objectInfo[0].renderInfoStatus = 0U;
    const std::vector<bool> helperShapeMap {true};
    const auto helperShapeResult = helperShapeState.applyFrame(
        helperShapeFrame, helperShapeMap, &helperShapeOutput);
    B2bState freshHelperShapeState;
    B2bFrameOutput freshHelperShapeOutput;
    const auto freshHelperShapeResult = freshHelperShapeState.applyFrame(
        helperShapeFrame, helperShapeMap, &freshHelperShapeOutput, true);
    check(helperShapeResult.disposition == B2bDisposition::Pass
              && helperShapeResult.stats.resetCount == 1U
              && freshHelperShapeResult.disposition == B2bDisposition::Pass
              && outputEquivalent(helperShapeOutput, freshHelperShapeOutput),
          "helper-map-shape-reset", &report.resetCases);

    const std::vector<bool> maxHelper(159U, false);
    B2bState maxState;
    B2bFrameOutput maxOutput;
    auto maxResult = maxState.applyFrame(frame(159U), maxHelper, &maxOutput, true);
    check(maxResult.disposition == B2bDisposition::Pass
              && maxOutput.objectCount == 159U
              && maxOutput.objects.size() == 159U,
          "maximum-object-shape", &report.maxShapeCases);

    B2aFrame badCount = frame(1U);
    B2bFrameOutput boundaryOutput;
    auto badMapResult = state.applyFrame(badCount, {}, &boundaryOutput, true);
    check(badMapResult.disposition != B2bDisposition::Pass, "boundary-map", &report.boundaryCases);
    std::vector<bool> tooMany(160U, false);
    auto badCountResult = state.applyFrame(badCount, tooMany, &boundaryOutput, true);
    check(badCountResult.disposition != B2bDisposition::Pass,
          "boundary-object-count", &report.boundaryCases);
    B2aFrame badZone = frame(1U);
    badZone.objectInfo[0].zoneConstraintsIndex = 6U;
    auto badZoneResult = state.applyFrame(badZone, one, &boundaryOutput);
    check(badZoneResult.disposition == B2bDisposition::Unsupported, "reserved-zone", &report.boundaryCases);

    report.pass = report.reason.empty() && report.cases == 37U;
    if (report.pass) report.reason = "all bounded property-state cases passed; conversion finite proof is arithmetic, not injected NaN";
    return report;
}

} // namespace eac3oamd
