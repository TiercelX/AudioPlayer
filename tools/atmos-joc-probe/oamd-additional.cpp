#include "oamd-additional.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace eac3oamd {
namespace {

constexpr unsigned kMaxObjects = 159U;

struct Reader {
    const std::uint8_t *data = nullptr;
    std::size_t bit = 0U;
    std::size_t limit = 0U;

    bool canRead(unsigned count) const
    {
        return bit <= limit && static_cast<std::size_t>(count) <= limit - bit;
    }

    bool read(unsigned count, unsigned *value)
    {
        if (!value || count > 32U || !canRead(count)) return false;
        unsigned result = 0U;
        for (unsigned i = 0U; i < count; ++i) {
            result = (result << 1U)
                | ((data[bit / 8U] >> (7U - (bit % 8U))) & 1U);
            ++bit;
        }
        *value = result;
        return true;
    }

    std::size_t remaining() const { return bit <= limit ? limit - bit : 0U; }
};

bool finish(Reader *reader, std::string *reason)
{
    if (!reader || !reason) return false;
    unsigned value = 0U;
    while (reader->remaining() != 0U) {
        if (!reader->read(1U, &value) || value != 0U) {
            *reason = "nonzero-additional-element-padding";
            return false;
        }
    }
    return true;
}

bool readBit(Reader *reader, bool *value, const char *reason)
{
    unsigned raw = 0U;
    if (!reader || !value || !reader->read(1U, &raw)) return false;
    *value = raw != 0U;
    (void)reason;
    return true;
}

bool readCode(Reader *reader, unsigned bits, unsigned *value)
{
    return reader && value && reader->read(bits, value);
}

void malformed(AdditionalElements *result, const char *reason)
{
    result->disposition = AdditionalDisposition::Malformed;
    result->reason = reason;
}

void unsupported(AdditionalElements *result, const char *reason)
{
    result->disposition = AdditionalDisposition::Unsupported;
    result->reason = reason;
}

bool validTrimCode(unsigned code)
{
    return code >= 4U && code <= 15U;
}

bool parseTrim(const std::vector<std::uint8_t> &body, std::size_t bodyBits,
               unsigned objectCount, TrimElement *output, std::string *reason,
               AdditionalDisposition *disposition)
{
    if (!output || !reason || !disposition || body.empty() || bodyBits == 0U
        || bodyBits > body.size() * 8U || objectCount == 0U
        || objectCount > kMaxObjects) {
        if (reason) *reason = "trim-element-shape-out-of-range";
        return false;
    }
    Reader reader {body.data(), 0U, bodyBits};
    if (!readCode(&reader, 2U, &output->warpMode)
        || !readCode(&reader, 2U, &output->reserved)
        || !readCode(&reader, 2U, &output->globalTrimMode)) {
        *reason = "truncated-trim-header";
        return false;
    }
    if (output->warpMode >= 2U) {
        *reason = "reserved-trim-warp-mode";
        *disposition = AdditionalDisposition::Unsupported;
        return false;
    }
    if (output->reserved != 0U) {
        *reason = "reserved-trim-header-bits";
        *disposition = AdditionalDisposition::Unsupported;
        return false;
    }
    if (output->globalTrimMode == 3U) {
        *reason = "reserved-global-trim-mode";
        *disposition = AdditionalDisposition::Unsupported;
        return false;
    }
    if (output->globalTrimMode == 2U) {
        for (unsigned cfg = 0U; cfg < kNumTrimConfigs; ++cfg) {
            TrimConfiguration &configuration = output->configurations[cfg];
            if (!readBit(&reader, &configuration.defaultTrim, nullptr)) {
                *reason = "truncated-trim-default-flag";
                return false;
            }
            if (!configuration.defaultTrim) {
                if (!readBit(&reader, &configuration.disabled, nullptr)) {
                    *reason = "truncated-trim-disable-flag";
                    return false;
                }
                if (!configuration.disabled) {
                    unsigned presence = 0U;
                    if (!readCode(&reader, 5U, &presence)) {
                        *reason = "truncated-trim-presence";
                        return false;
                    }
                    for (unsigned index = 0U; index < 5U; ++index) {
                        configuration.balancePresence[index] =
                            (presence & (1U << (4U - index))) != 0U;
                    }
                    if (configuration.balancePresence[4]
                        && !readCode(&reader, 4U, &configuration.centreCode)) {
                        *reason = "truncated-trim-centre";
                        return false;
                    }
                    if (configuration.balancePresence[3]
                        && !readCode(&reader, 4U, &configuration.surroundCode)) {
                        *reason = "truncated-trim-surround";
                        return false;
                    }
                    if (configuration.balancePresence[2]
                        && !readCode(&reader, 4U, &configuration.heightCode)) {
                        *reason = "truncated-trim-height";
                        return false;
                    }
                    if (configuration.balancePresence[1]
                        && (!readCode(&reader, 1U, &configuration.balanceTbSignCode)
                            || !readCode(&reader, 4U,
                                         &configuration.balanceTbAmountCode))) {
                        *reason = "truncated-trim-top-bottom-balance";
                        return false;
                    }
                    if (configuration.balancePresence[0]
                        && (!readCode(&reader, 1U,
                                      &configuration.balanceListenerSignCode)
                            || !readCode(&reader, 4U,
                                         &configuration.balanceListenerAmountCode))) {
                        *reason = "truncated-trim-listener-balance";
                        return false;
                    }
                    if ((configuration.balancePresence[3]
                             && !validTrimCode(configuration.surroundCode))
                        || (configuration.balancePresence[2]
                            && !validTrimCode(configuration.heightCode))) {
                        *reason = "reserved-trim-level-code";
                        *disposition = AdditionalDisposition::Unsupported;
                        return false;
                    }
                }
            }
        }
    }
    if (!readBit(&reader, &output->disableTrimPerObject, nullptr)) {
        *reason = "truncated-per-object-trim-flag";
        return false;
    }
    if (output->disableTrimPerObject) {
        output->objectTrimDisabled.resize(objectCount);
        for (unsigned object = 0U; object < objectCount; ++object) {
            bool disabled = false;
            if (!readBit(&reader, &disabled, nullptr)) {
                *reason = "truncated-per-object-trim-disable";
                return false;
            }
            output->objectTrimDisabled[object] = disabled;
        }
    }
    return finish(&reader, reason);
}

float tableDivergence(unsigned index)
{
    constexpr std::array<float, 4> table {
        0.500755F, 0.608529F, 0.704833F, 1.0F,
    };
    return table[index];
}

float codeDivergence(unsigned code)
{
    constexpr std::array<float, 64> table {
        0.0F, 0.0F, 0.004026F, 0.007160F, 0.012731F, 0.020173F,
        0.028485F, 0.040210F, 0.050582F, 0.063601F, 0.079914F, 0.100299F,
        0.125666F, 0.140532F, 0.157027F, 0.175282F, 0.195417F, 0.217536F,
        0.241718F, 0.268002F, 0.296377F, 0.326766F, 0.359017F, 0.392895F,
        0.428081F, 0.464184F, 0.500755F, 0.537316F, 0.573389F, 0.608529F,
        0.642346F, 0.674524F, 0.704833F, 0.733123F, 0.759320F, 0.783416F,
        0.805451F, 0.825506F, 0.843686F, 0.860112F, 0.874914F, 0.888222F,
        0.900168F, 0.910875F, 0.920461F, 0.929035F, 0.936698F, 0.943544F,
        0.949656F, 0.955112F, 0.959980F, 0.964322F, 0.968195F, 0.974729F,
        0.979923F, 0.984050F, 0.987330F, 0.989935F, 0.992874F, 0.994955F,
        0.996817F, 0.998210F, 0.998993F, 1.0F,
    };
    return table[code];
}

bool parseExtended(const std::vector<std::uint8_t> &body, std::size_t bodyBits,
                   const B2aFrame &b2a, const std::vector<bool> &helper,
                   const std::vector<float> &prior,
                   const std::vector<bool> &priorValid,
                   ExtendedObjectElement *output, std::string *reason,
                   AdditionalDisposition *disposition)
{
    if (!output || !reason || !disposition || body.empty() || bodyBits == 0U
        || bodyBits > body.size() * 8U || b2a.objectInfoBlockCount == 0U
        || b2a.objectInfo.size() % b2a.objectInfoBlockCount != 0U
        || helper.size() != b2a.objectInfo.size() / b2a.objectInfoBlockCount) {
        if (reason) *reason = "extended-element-shape-out-of-range";
        return false;
    }
    const unsigned objectCount = static_cast<unsigned>(helper.size());
    const unsigned blockCount = b2a.objectInfoBlockCount;
    output->objectCount = objectCount;
    output->blockCount = blockCount;
    output->divergence.assign(static_cast<std::size_t>(objectCount) * blockCount,
                              DivergenceState {});
    output->extendedPosition.assign(
        static_cast<std::size_t>(objectCount) * blockCount,
        ExtendedPositionState {});
    Reader reader {body.data(), 0U, bodyBits};
    if (!readBit(&reader, &output->divergenceBlockPresent, nullptr)) {
        *reason = "truncated-divergence-block-flag";
        return false;
    }
    std::vector<float> previous = prior;
    std::vector<bool> previousValid = priorValid;
    if (previous.size() != objectCount) previous.assign(objectCount, 0.0F);
    if (previousValid.size() != objectCount) previousValid.assign(objectCount, false);
    if (output->divergenceBlockPresent) {
        for (unsigned object = 0U; object < objectCount; ++object) {
            for (unsigned block = 0U; block < blockCount; ++block) {
                DivergenceState &state = output->divergence[
                    static_cast<std::size_t>(object) * blockCount + block];
                const B2aObjectInfo &raw = b2a.objectInfo[
                    static_cast<std::size_t>(object) * blockCount + block];
                if (raw.objectNotActive) {
                    state.present = true;
                    state.value = 0.0F;
                    previous[object] = 0.0F;
                    previousValid[object] = true;
                    continue;
                }
                if (helper[object]) continue;
                bool present = false;
                if (!readBit(&reader, &present, nullptr)) {
                    *reason = "truncated-object-divergence-presence";
                    return false;
                }
                state.present = present;
                if (!present) {
                    previous[object] = 0.0F;
                    previousValid[object] = true;
                    continue;
                }
                if (!readCode(&reader, 2U, &state.mode)) {
                    *reason = "truncated-object-divergence-mode";
                    return false;
                }
                if (state.mode == 0U) {
                    if (!readCode(&reader, 2U, &state.table)) {
                        *reason = "truncated-object-divergence-table";
                        return false;
                    }
                    state.value = tableDivergence(state.table);
                } else if (state.mode == 1U) {
                    if (!previousValid[object]) {
                        *reason = "divergence-reuse-without-previous-state";
                        return false;
                    }
                    state.reused = true;
                    state.value = previous[object];
                } else if (state.mode == 2U) {
                    if (!readCode(&reader, 6U, &state.code)) {
                        *reason = "truncated-object-divergence-code";
                        return false;
                    }
                    if (state.code == 0U) {
                        *reason = "reserved-object-divergence-code";
                        *disposition = AdditionalDisposition::Unsupported;
                        return false;
                    }
                    state.value = codeDivergence(state.code);
                } else {
                    *reason = "reserved-object-divergence-mode";
                    *disposition = AdditionalDisposition::Unsupported;
                    return false;
                }
                if (!std::isfinite(state.value)) {
                    *reason = "nonfinite-object-divergence";
                    return false;
                }
                previous[object] = state.value;
                previousValid[object] = true;
            }
        }
    }
    if (!readBit(&reader, &output->extendedPositionBlockPresent, nullptr)) {
        *reason = "truncated-extended-position-block-flag";
        return false;
    }
    if (output->extendedPositionBlockPresent) {
        for (unsigned object = 0U; object < objectCount; ++object) {
            for (unsigned block = 0U; block < blockCount; ++block) {
                ExtendedPositionState &state = output->extendedPosition[
                    static_cast<std::size_t>(object) * blockCount + block];
                const B2aObjectInfo &raw = b2a.objectInfo[
                    static_cast<std::size_t>(object) * blockCount + block];
                if (raw.objectNotActive || helper[object]) continue;
                if (!readBit(&reader, &state.present, nullptr)) {
                    *reason = "truncated-extended-position-presence";
                    return false;
                }
                if (!state.present) continue;
                unsigned mask = 0U;
                if (!readCode(&reader, 3U, &mask)) {
                    *reason = "truncated-extended-position-presence-mask";
                    return false;
                }
                state.presence = {(mask & 0x4U) != 0U, (mask & 0x2U) != 0U,
                                  (mask & 0x1U) != 0U};
                for (unsigned axis = 0U; axis < 3U; ++axis) {
                    if (!state.presence[axis]) continue;
                    unsigned code = 0U;
                    if (!readCode(&reader, 2U, &code)) {
                        *reason = "truncated-extended-position-code";
                        return false;
                    }
                    state.values[axis] = code == 0U ? 1
                        : code == 1U ? 2 : code == 2U ? -1 : -2;
                }
            }
        }
    }
    return finish(&reader, reason);
}

} // namespace

const char *additionalDispositionText(AdditionalDisposition disposition)
{
    switch (disposition) {
    case AdditionalDisposition::Pass: return "PASS";
    case AdditionalDisposition::Unsupported: return "UNSUPPORTED";
    case AdditionalDisposition::Malformed: return "MALFORMED";
    }
    return "MALFORMED";
}

AdditionalElements parseAdditionalElements(
    const B1Frame &b1, const B2aFrame &b2a,
    const std::vector<bool> &objectInBedOrIsf,
    const std::vector<float> &priorDivergence,
    const std::vector<bool> &priorDivergenceValid)
{
    AdditionalElements result;
    if (b1.disposition != B1Disposition::Pass || b1.version != 0U
        || b1.versionExtended || b1.objectCount != objectInBedOrIsf.size()
        || b2a.disposition != B2aDisposition::Pass
        || b2a.objectInfoBlockCount == 0U
        || b2a.objectInfo.size() != objectInBedOrIsf.size()
            * b2a.objectInfoBlockCount) {
        malformed(&result, "additional-context-not-qualified");
        return result;
    }
    const ElementInventory *trim = nullptr;
    const ElementInventory *extended = nullptr;
    for (const ElementInventory &element : b1.elements) {
        if (element.id != 2U && element.id != 5U) continue;
        const ElementInventory **slot = element.id == 2U ? &trim : &extended;
        if (*slot) {
            malformed(&result, element.id == 2U
                                   ? "duplicate-trim-element"
                                   : "duplicate-extended-element");
            return result;
        }
        *slot = &element;
    }
    if (trim) {
        TrimElement parsedTrim;
        AdditionalDisposition disposition = AdditionalDisposition::Malformed;
        if (!parseTrim(trim->rawBody, trim->rawBodyBits,
                       static_cast<unsigned>(objectInBedOrIsf.size()), &parsedTrim,
                       &result.reason, &disposition)) {
            result.disposition = disposition;
            return result;
        }
        result.trim = std::move(parsedTrim);
        result.trimPresent = true;
    }
    if (extended) {
        ExtendedObjectElement parsedExtended;
        AdditionalDisposition disposition = AdditionalDisposition::Malformed;
        if (!parseExtended(extended->rawBody, extended->rawBodyBits, b2a,
                           objectInBedOrIsf, priorDivergence,
                           priorDivergenceValid, &parsedExtended,
                           &result.reason,
                           &disposition)) {
            result.disposition = disposition;
            return result;
        }
        result.extended = std::move(parsedExtended);
        result.extendedPresent = true;
    }
    result.reason = "bounded-oamd-trim-and-extended-elements";
    return result;
}

} // namespace eac3oamd
