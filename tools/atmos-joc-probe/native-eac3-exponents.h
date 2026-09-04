#pragma once

// Gate 8N-2a: pure native exponent decoding primitives.  This file stops at
// validated absolute exponents; it does not parse bit allocation, mantissas,
// coupling coordinates, or IMDCT state.

#include <cstddef>
#include <string>
#include <vector>

namespace eac3native {

enum class ExponentDisposition {
    Accepted,
    Malformed,
    Unsupported,
};

enum class ExponentStrategy {
    Reuse,
    D15,
    D25,
    D45,
};

struct ExponentReuseState {
    bool valid = false;
    unsigned bandwidthCode = 0;
    std::vector<unsigned> exponents;
};

// Coupling reuse has its own coordinate state.  It must not be confused with
// the ordinary bandwidthCode used by channel/LFE exponent reuse.
struct CouplingReuseState {
    bool valid = false;
    unsigned couplingStartMant = 0;
    unsigned couplingEndMant = 0;
    std::vector<unsigned> exponents;
};

struct ExponentDecodeRequest {
    ExponentStrategy strategy = ExponentStrategy::D15;
    unsigned absoluteExponent = 0;
    std::vector<unsigned> groupedCodes;
    std::size_t targetCoefficientCount = 0;
    bool block0 = false;
    unsigned bandwidthCode = 0;
    const ExponentReuseState *prior = nullptr;
};

struct ExponentDecodeResult {
    ExponentDisposition disposition = ExponentDisposition::Malformed;
    std::string reason;
    std::vector<unsigned> exponents;
    bool hasReferenceExponent = false;
    unsigned referenceExponent = 0;
};

ExponentDecodeResult decodeExponentSet(const ExponentDecodeRequest &request);

struct CouplingExponentDecodeRequest {
    // cplabsexp is a reference exponent (encoded value << 1), not a
    // coefficient count or a replacement for the returned coupling bins.
    unsigned encodedAbsoluteExponent = 0;
    std::vector<unsigned> groupedCodes;
    ExponentStrategy strategy = ExponentStrategy::D15;
    unsigned couplingStartMant = 0;
    unsigned couplingEndMant = 0;
    // This is the actual number of coupling bins and must equal
    // couplingEndMant - couplingStartMant.
    std::size_t targetCoefficientCount = 0;
    bool block0 = false;
    const CouplingReuseState *prior = nullptr;
};

ExponentDecodeResult decodeCouplingExponentSet(
    const CouplingExponentDecodeRequest &request);

// LFE always has seven exponents and uses d15 for a new set.  Reuse is
// accepted only with an existing prior block and matching bandwidth state.
ExponentDecodeResult decodeLfeExponents(
    unsigned absoluteExponent,
    const std::vector<unsigned> &groupedCodes,
    ExponentStrategy strategy,
    bool block0,
    unsigned bandwidthCode,
    const ExponentReuseState *prior = nullptr);

const char *toString(ExponentDisposition value);
const char *toString(ExponentStrategy value);

} // namespace eac3native
