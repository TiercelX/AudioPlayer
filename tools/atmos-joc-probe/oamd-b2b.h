#pragma once

#include "oamd-b2a.h"
#include "oamd-additional.h"

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace eac3oamd {

enum class B2bDisposition {
    Pass,
    Unsupported,
    Malformed,
};

const char *b2bDispositionText(B2bDisposition disposition);

struct B2bPosition {
    bool valid = false;
    bool screenAnchored = false;
    bool distanceSpecified = false;
    bool distanceInfinite = false;
    float x = 0.5f;
    float y = 0.5f;
    float z = 0.0f;
    float codedX = 0.5f;
    float codedY = 0.5f;
    float codedZ = 0.0f;
    float distanceFactor = 0.0f;
    float screenFactor = 0.0f;
    float depthFactor = 0.0f;
    float standardX = 0.5f;
    float standardY = 0.5f;
    float standardZ = 0.0f;
    // Extended precision codewords are kept separately from the standard
    // precision values used as the base for differential coding.
    std::array<bool, 3> extendedPrecisionPresent {};
    std::array<int, 3> extendedPrecision {};
};

struct B2bObjectState {
    unsigned objectIndex = 0;
    bool lfeHelper = false;
    bool active = false;
    bool basicValid = false;
    bool renderValid = false;
    bool gainMinusInfinity = true;
    float gainDb = 0.0f;
    float priority = 0.0f;
    B2bPosition position;
    std::array<float, 3> size {};
    // Effective render state after reuse/mixed presence resolution.  raw is
    // the current B2a codeword and may not describe retained size fields.
    bool effectiveSizePresent = false;
    unsigned effectiveSizeIndex = 0;
    std::array<bool, 6> zoneConstraints {true, true, true, true, true, true};
    bool elevation = true;
    bool snap = false;
    bool trimDisabled = false;
    bool divergencePresent = false;
    bool divergenceReused = false;
    unsigned divergenceMode = 0U;
    unsigned divergenceIndex = 0U;
    float divergence = 0.0F;
    B2aObjectInfo raw;
};

struct B2bFrameOutput {
    unsigned objectCount = 0;
    std::vector<B2bObjectState> objects;
    std::vector<unsigned> dynamicObjectIndices;
    bool trimPresent = false;
    TrimElement trim;
    struct BlockSnapshot {
        unsigned blockIndex = 0;
        std::vector<B2bObjectState> objects;
    };
    std::vector<BlockSnapshot> blockSnapshots;
};

struct B2bApplyStats {
    std::size_t resetCount = 0;
    std::size_t defaultBasicCount = 0;
    std::size_t fullBasicCount = 0;
    std::size_t reuseBasicCount = 0;
    std::size_t mixedBasicCount = 0;
    std::size_t defaultRenderCount = 0;
    std::size_t fullRenderCount = 0;
    std::size_t reuseRenderCount = 0;
    std::size_t mixedRenderCount = 0;
    std::size_t differentialPositionCount = 0;
    std::size_t absolutePositionCount = 0;
    std::size_t inactiveCount = 0;
    std::size_t activeCount = 0;
    std::size_t finitePropertyCount = 0;
    std::size_t minusInfinityGainCount = 0;
};

struct B2bApplyResult {
    B2bDisposition disposition = B2bDisposition::Malformed;
    std::string reason;
    B2bApplyStats stats;
};

class B2bState {
public:
    B2bApplyResult applyFrame(const B2aFrame &frame,
                              const std::vector<bool> &objectInBedOrIsf,
                              B2bFrameOutput *output,
                              bool resetBefore = false,
                              const B1Frame *metadata = nullptr);

    void reset();

private:
    unsigned objectCount_ = 0;
    std::vector<bool> helperMap_;
    std::vector<B2bObjectState> objects_;
    std::vector<float> divergence_;
    std::vector<bool> divergenceValid_;
    bool valid_ = false;
};

struct B2bSelfTestReport {
    bool pass = false;
    std::size_t cases = 0;
    std::size_t defaultCases = 0;
    std::size_t fullCases = 0;
    std::size_t reuseCases = 0;
    std::size_t mixedCases = 0;
    std::size_t differentialCases = 0;
    std::size_t conversionCases = 0;
    std::size_t inactiveCases = 0;
    std::size_t lfeHelperCases = 0;
    std::size_t resetCases = 0;
    std::size_t transactionalCases = 0;
    std::size_t boundaryCases = 0;
    std::size_t gainCodeCases = 0;
    std::size_t formulaBoundaryCases = 0;
    std::size_t zoneMappingCases = 0;
    std::size_t blockSnapshotCases = 0;
    std::size_t snapCases = 0;
    std::size_t maxShapeCases = 0;
    std::string reason;
};

B2bSelfTestReport runB2bSelfTest();

} // namespace eac3oamd
