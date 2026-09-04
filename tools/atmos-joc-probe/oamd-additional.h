#pragma once

#include "oamd-b1.h"
#include "oamd-b2a.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace eac3oamd {

// ETSI TS 103 420 5.5.12 uses NUM_TRIM_CONFIGS as a helper variable.  The
// companion ETSI OAMD definition (TS 103 190-2 6.3.9.10.4) defines it as
// nine; keep the value explicit rather than deriving it from a sample/layout.
constexpr unsigned kNumTrimConfigs = 9U;

enum class AdditionalDisposition {
    Pass,
    Unsupported,
    Malformed,
};

const char *additionalDispositionText(AdditionalDisposition disposition);

struct TrimConfiguration {
    bool defaultTrim = true;
    bool disabled = false;
    std::array<bool, 5> balancePresence {};
    unsigned centreCode = 0U;
    unsigned surroundCode = 0U;
    unsigned heightCode = 0U;
    unsigned balanceTbSignCode = 0U;
    unsigned balanceTbAmountCode = 0U;
    unsigned balanceListenerSignCode = 0U;
    unsigned balanceListenerAmountCode = 0U;
};

struct TrimElement {
    unsigned warpMode = 0U;
    unsigned reserved = 0U;
    unsigned globalTrimMode = 0U;
    std::array<TrimConfiguration, kNumTrimConfigs> configurations {};
    bool disableTrimPerObject = false;
    std::vector<bool> objectTrimDisabled;
};

struct DivergenceState {
    bool present = false;
    bool reused = false;
    unsigned mode = 0U;
    unsigned table = 0U;
    unsigned code = 0U;
    float value = 0.0F;
};

struct ExtendedPositionState {
    bool present = false;
    std::array<bool, 3> presence {};
    std::array<int, 3> values {};
};

struct ExtendedObjectElement {
    bool divergenceBlockPresent = false;
    bool extendedPositionBlockPresent = false;
    unsigned objectCount = 0U;
    unsigned blockCount = 0U;
    // Object-major, block-minor.  Inactive/helper objects have zero/default
    // state because the syntax emits no fields for them.
    std::vector<DivergenceState> divergence;
    std::vector<ExtendedPositionState> extendedPosition;
};

struct AdditionalElements {
    AdditionalDisposition disposition = AdditionalDisposition::Pass;
    std::string reason;
    bool trimPresent = false;
    bool extendedPresent = false;
    TrimElement trim;
    ExtendedObjectElement extended;
};

// Parse the ID-2/ID-5 elements from one already accepted B1 frame.  The
// object element itself remains owned by parseObjectElement(); this function
// only parses the real additional oa_element containers and validates their
// boundaries/dispatch/version contract.
AdditionalElements parseAdditionalElements(
    const B1Frame &b1, const B2aFrame &b2a,
    const std::vector<bool> &objectInBedOrIsf,
    const std::vector<float> &priorDivergence = {},
    const std::vector<bool> &priorDivergenceValid = {});

struct AdditionalSelfTestReport {
    bool pass = false;
    std::size_t cases = 0U;
    std::size_t trimCases = 0U;
    std::size_t divergenceCases = 0U;
    std::size_t extendedPositionCases = 0U;
    std::size_t reuseCases = 0U;
    std::size_t resetCases = 0U;
    std::size_t multiObjectBlockCases = 0U;
    std::size_t reservedCases = 0U;
    std::size_t truncatedCases = 0U;
    std::size_t tailCases = 0U;
    std::size_t emptyFailureCases = 0U;
    std::string reason;
};

} // namespace eac3oamd
