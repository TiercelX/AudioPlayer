#include "native-eac3-exponents.h"

#include <limits>

namespace eac3native {
namespace {

ExponentDecodeResult failure(ExponentDisposition disposition,
                             const char *reason)
{
    ExponentDecodeResult result;
    result.disposition = disposition;
    result.reason = reason;
    return result;
}

unsigned groupSize(ExponentStrategy strategy)
{
    switch (strategy) {
    case ExponentStrategy::D15: return 1;
    case ExponentStrategy::D25: return 2;
    case ExponentStrategy::D45: return 4;
    case ExponentStrategy::Reuse: return 0;
    }
    return 0;
}

static ExponentDecodeResult decodeAbsoluteExponentSet(
    const ExponentDecodeRequest &request, unsigned transmittedAbsoluteMaximum)
{
    if (request.targetCoefficientCount == 0U) {
        return failure(ExponentDisposition::Malformed,
                       "zero-target-coefficient-count");
    }
    if (request.strategy == ExponentStrategy::Reuse) {
        if (request.block0) {
            return failure(ExponentDisposition::Malformed,
                           "reuse-on-block-zero");
        }
        if (!request.prior || !request.prior->valid) {
            return failure(ExponentDisposition::Malformed,
                           "reuse-without-prior-block");
        }
        if (request.prior->bandwidthCode != request.bandwidthCode) {
            return failure(ExponentDisposition::Malformed,
                           "reuse-bandwidth-change");
        }
        if (request.prior->exponents.size()
            != request.targetCoefficientCount) {
            return failure(ExponentDisposition::Malformed,
                           "reuse-coefficient-count-change");
        }
        if (!request.groupedCodes.empty()) {
            return failure(ExponentDisposition::Malformed,
                           "reuse-has-group-codes");
        }
        ExponentDecodeResult result;
        result.disposition = ExponentDisposition::Accepted;
        result.exponents = request.prior->exponents;
        return result;
    }

    const unsigned grpsize = groupSize(request.strategy);
    if (grpsize == 0U) {
        return failure(ExponentDisposition::Unsupported,
                       "unsupported-exponent-strategy");
    }
    if (request.absoluteExponent > transmittedAbsoluteMaximum) {
        return failure(ExponentDisposition::Malformed,
                       "absolute-exponent-out-of-range");
    }
    const std::size_t differentialCount = request.targetCoefficientCount - 1U;
    const std::size_t groupSpan = static_cast<std::size_t>(grpsize) * 3U;
    const std::size_t requiredGroups =
        (differentialCount + groupSpan - 1U) / groupSpan;
    if (request.groupedCodes.size() != requiredGroups) {
        return failure(ExponentDisposition::Malformed,
                       "group-code-count");
    }

    ExponentDecodeResult result;
    result.disposition = ExponentDisposition::Accepted;
    result.exponents.reserve(request.targetCoefficientCount);
    result.exponents.push_back(request.absoluteExponent);
    unsigned previous = request.absoluteExponent;
    for (unsigned grouped : request.groupedCodes) {
        if (grouped > 124U) {
            return failure(ExponentDisposition::Malformed,
                           "group-code-out-of-range");
        }
        const unsigned mapped[3] = {
            grouped / 25U,
            (grouped % 25U) / 5U,
            grouped % 5U};
        for (unsigned mappedValue : mapped) {
            // The final D25/D45 group can be only partially consumed.  Once
            // the requested bins are full, do not interpret unused tail
            // deltas as part of the stream (they may be out of range).
            if (result.exponents.size() == request.targetCoefficientCount) {
                break;
            }
            const int delta = static_cast<int>(mappedValue) - 2;
            const int next = static_cast<int>(previous) + delta;
            if (next < 0) {
                return failure(ExponentDisposition::Malformed,
                               "exponent-underflow");
            }
            if (next > 24) {
                return failure(ExponentDisposition::Malformed,
                               "exponent-overflow");
            }
            previous = static_cast<unsigned>(next);
            for (unsigned copy = 0; copy < grpsize; ++copy) {
                if (result.exponents.size()
                    == request.targetCoefficientCount) {
                    break;
                }
                result.exponents.push_back(previous);
            }
        }
    }
    if (result.exponents.size() != request.targetCoefficientCount) {
        return failure(ExponentDisposition::Malformed,
                       "exponent-expansion-short");
    }
    return result;
}

} // namespace

ExponentDecodeResult decodeExponentSet(const ExponentDecodeRequest &request)
{
    // Ordinary channel and LFE transmitted absolute exponents are 4-bit
    // values.  Expanded exponents may still range through 24.
    return decodeAbsoluteExponentSet(request, 15U);
}

ExponentDecodeResult decodeCouplingExponentSet(
    const CouplingExponentDecodeRequest &request)
{
    if (request.couplingEndMant <= request.couplingStartMant) {
        return failure(ExponentDisposition::Malformed,
                       "coupling-range-empty");
    }
    const std::size_t couplingBinCount =
        static_cast<std::size_t>(request.couplingEndMant
                                 - request.couplingStartMant);
    if (request.targetCoefficientCount != couplingBinCount) {
        return failure(ExponentDisposition::Malformed,
                       "coupling-bin-count-mismatch");
    }
    if (request.targetCoefficientCount == 0U) {
        return failure(ExponentDisposition::Malformed,
                       "zero-target-coefficient-count");
    }

    if (request.strategy == ExponentStrategy::Reuse) {
        if (request.block0) {
            return failure(ExponentDisposition::Malformed,
                           "coupling-reuse-on-block-zero");
        }
        if (!request.prior || !request.prior->valid) {
            return failure(ExponentDisposition::Malformed,
                           "coupling-reuse-without-prior-block");
        }
        if (request.prior->couplingStartMant != request.couplingStartMant
            || request.prior->couplingEndMant != request.couplingEndMant) {
            return failure(ExponentDisposition::Malformed,
                           "coupling-reuse-range-change");
        }
        if (request.prior->exponents.size() != couplingBinCount) {
            return failure(ExponentDisposition::Malformed,
                           "coupling-reuse-bin-count-change");
        }
        if (!request.groupedCodes.empty()) {
            return failure(ExponentDisposition::Malformed,
                           "coupling-reuse-has-group-codes");
        }
        ExponentDecodeResult result;
        result.disposition = ExponentDisposition::Accepted;
        result.exponents = request.prior->exponents;
        return result;
    }

    if (request.encodedAbsoluteExponent > 15U) {
        return failure(ExponentDisposition::Malformed,
                       "coupling-absolute-code-out-of-range");
    }
    const unsigned referenceExponent =
        request.encodedAbsoluteExponent << 1U;
    if (referenceExponent > 24U) {
        return failure(ExponentDisposition::Malformed,
                       "coupling-absolute-exponent-out-of-range");
    }
    ExponentDecodeRequest exponentRequest;
    exponentRequest.strategy = request.strategy;
    exponentRequest.absoluteExponent = referenceExponent;
    exponentRequest.groupedCodes = request.groupedCodes;
    // The first expanded value is the reference exponent.  The requested
    // coupling bins are the following values, so decode one extra value and
    // remove the reference before returning the public result.
    exponentRequest.targetCoefficientCount = couplingBinCount + 1U;
    exponentRequest.block0 = request.block0;
    const ExponentDecodeResult decoded =
        decodeAbsoluteExponentSet(exponentRequest, 24U);
    if (decoded.disposition != ExponentDisposition::Accepted) {
        return decoded;
    }
    if (decoded.exponents.size() != couplingBinCount + 1U
        || decoded.exponents.front() != referenceExponent) {
        return failure(ExponentDisposition::Malformed,
                       "coupling-reference-expansion-shape");
    }
    ExponentDecodeResult result = decoded;
    result.exponents.erase(result.exponents.begin());
    result.hasReferenceExponent = true;
    result.referenceExponent = referenceExponent;
    return result;
}

ExponentDecodeResult decodeLfeExponents(
    unsigned absoluteExponent,
    const std::vector<unsigned> &groupedCodes,
    ExponentStrategy strategy,
    bool block0,
    unsigned bandwidthCode,
    const ExponentReuseState *prior)
{
    if (strategy != ExponentStrategy::D15
        && strategy != ExponentStrategy::Reuse) {
        return failure(ExponentDisposition::Unsupported,
                       "lfe-strategy-not-d15-or-reuse");
    }
    ExponentDecodeRequest request;
    request.strategy = strategy;
    request.absoluteExponent = absoluteExponent;
    request.groupedCodes = groupedCodes;
    request.targetCoefficientCount = 7U;
    request.block0 = block0;
    request.bandwidthCode = bandwidthCode;
    request.prior = prior;
    return decodeExponentSet(request);
}

const char *toString(ExponentDisposition value)
{
    switch (value) {
    case ExponentDisposition::Accepted: return "accepted";
    case ExponentDisposition::Malformed: return "malformed";
    case ExponentDisposition::Unsupported: return "unsupported";
    }
    return "unknown";
}

const char *toString(ExponentStrategy value)
{
    switch (value) {
    case ExponentStrategy::Reuse: return "reuse";
    case ExponentStrategy::D15: return "d15";
    case ExponentStrategy::D25: return "d25";
    case ExponentStrategy::D45: return "d45";
    }
    return "unknown";
}

} // namespace eac3native
