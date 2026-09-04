#pragma once

#include "bs2127-selector.h"

#include <array>
#include <cstddef>
#include <limits>
#include <string>
#include <vector>

namespace eac3render {

constexpr std::size_t kNoBs2127VirtualTriangle =
    std::numeric_limits<std::size_t>::max();

struct Bs2127VirtualTriangle {
    std::size_t firstRealSpeakerIndex = kNoBs2127VirtualTriangle;
    std::size_t secondRealSpeakerIndex = kNoBs2127VirtualTriangle;
    std::size_t sourceFacetIndex = kNoBs2127VirtualTriangle;
};

// BS.2127 §6.1.2.2 lower-pole VirtualNgon contract. The ring is reconstructed
// from raw virtual hull facet adjacency. Its direction is a deterministic
// project-local System H policy for stable RegionHandler calls, not a
// normative BS.2127 ordering constant. Quad selection is provided by the
// sibling selector; this lower-pole primitive does not consume Quad facets.
struct Bs2127LowerVirtualNgon {
    std::size_t virtualPointIndex = kBs2127VirtualBottomIndex;
    std::vector<std::size_t> ringRealSpeakerIndices;
    std::vector<Bs2127VirtualTriangle> triangles;
    double downmixCoefficient = 0.0;
    bool valid = false;
    std::string reason;
};

Bs2127LowerVirtualNgon buildLowerVirtualNgon(
    const Bs2127TopologyCatalog &catalog);

enum class Bs2127VirtualNgonStatus {
    Selected,
    Unsupported,
    InvalidInput,
};

const char *bs2127VirtualNgonStatusName(Bs2127VirtualNgonStatus status);

struct Bs2127VirtualNgonRenderResult {
    Bs2127VirtualNgonStatus status = Bs2127VirtualNgonStatus::InvalidInput;
    std::string reason;
    std::size_t selectedTriangleIndex = kNoBs2127VirtualTriangle;
    // The three values are ordered [virtual, first real, second real].
    std::array<double, 3> triangleGains {0.0, 0.0, 0.0};
    double virtualGain = 0.0;
    double preNormalizationPower = 0.0;
    std::array<double, kSystemHSpeakerCount> gains {};
};

Bs2127VirtualNgonRenderResult renderLowerVirtualNgon(
    const Bs2127TopologyCatalog &catalog,
    const Bs2127LowerVirtualNgon &region,
    const UnitVector3 &sourceVector);

// Configuration-process variant: topology/adjacency stays nominal, while
// real speaker and virtual-point vectors are supplied for region math.
Bs2127VirtualNgonRenderResult renderLowerVirtualNgon(
    const Bs2127TopologyCatalog &catalog,
    const Bs2127LowerVirtualNgon &region,
    const UnitVector3 &sourceVector,
    const std::array<UnitVector3, kBs2127TopologyStorageCount> &regionVectors);

// Compatibility overload for callers that only provide the legacy 23-point
// nominal catalog view.
Bs2127VirtualNgonRenderResult renderLowerVirtualNgon(
    const Bs2127TopologyCatalog &catalog,
    const Bs2127LowerVirtualNgon &region,
    const UnitVector3 &sourceVector,
    const std::array<UnitVector3, kBs2127TopologyPointCount> &regionVectors);

} // namespace eac3render
