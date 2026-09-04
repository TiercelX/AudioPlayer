#pragma once

// N1A reusable state-only checks for the native audfrm()/audblk() parser.
// This module hashes and validates syntax snapshots; it never decodes
// coefficient values, produces PCM, applies DRC, or performs IMDCT work.

#include "native-eac3-audblk.h"

#include <cstdint>
#include <string>
#include <vector>

namespace eac3native {

struct AudblkStateValidationResult {
    bool valid = false;
    std::size_t frameOffset = 0U;
    unsigned blockIndex = static_cast<unsigned>(-1);
    unsigned channelIndex = static_cast<unsigned>(-1);
    bool channelIsLfe = false;
    std::size_t bitPosition = 0U;
    std::string reason;
};

// Validate only state invariants that are local to one frame snapshot.  A
// caller must start a new snapshot for every frame; reuse is never carried
// across this boundary.
AudblkStateValidationResult validateAudblkState(const AudblkFrameState &state);

// Deterministic FNV-1a identity over every state field, including complete
// BAP vectors and decoded exponent vectors.  The digest is not a coefficient
// value oracle and is intentionally independent of container addresses.
std::uint64_t digestAudblkState(const AudblkFrameState &state);
std::uint64_t digestBapVector(const std::vector<unsigned> &bap);
std::uint64_t digestCoefficientVector(const std::vector<double> &coefficients);
std::string formatAudblkStateDigest(std::uint64_t digest);

} // namespace eac3native
