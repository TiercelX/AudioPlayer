#pragma once

#include "bs2051-layout.h"
#include "bs2127-quad.h"
#include "bs2127-triplet.h"

#include <array>
#include <cstddef>
#include <limits>
#include <string>
#include <vector>

namespace eac3render {

constexpr std::size_t kBs2127VirtualBottomIndex = kSystemHSpeakerCount;
// Legacy nominal System H catalog point count retained for existing probes.
constexpr std::size_t kBs2127TopologyPointCount = kSystemHSpeakerCount + 1U;
// Storage capacity used by the configured System H R0D catalog.
constexpr std::size_t kBs2127TopologyStorageCount = kSystemHSpeakerCount + 6U;

enum class Bs2127FacetKind {
    Triplet,
    Quad,
    VirtualHullFacet,
    UnsupportedNgon,
};

const char *bs2127FacetKindName(Bs2127FacetKind kind);

struct Bs2127TopologyOptions {
    TripletSolveOptions tripletOptions {};
    QuadSolveOptions quadOptions {};

    // Numerical topology policies are explicit diagnostic choices, not
    // normative BS.2127 constants.
    double planeTolerance = 1.0e-9;
    double planeMergeTolerance = 1.0e-8;
    double maximumConditionEstimate = 1.0e3;
    double identityVectorTolerance = 1.0e-10;
    bool identityGuardEnabled = true;
};

struct Bs2127Facet {
    Bs2127FacetKind kind = Bs2127FacetKind::UnsupportedNgon;
    // Sorted topology identities, deliberately not a perimeter/winding order;
    // Lower-pole VirtualNgon adjacency ordering is deferred with that
    // renderer primitive.
    std::vector<std::size_t> vertexIndices;
    std::vector<std::size_t> realSpeakerIndices;
    // Perimeter order for a QuadRegion, anticlockwise as viewed from the
    // listener. realSpeakerIndices remains sorted topology identity order.
    std::vector<std::size_t> regionSpeakerIndices;
    bool containsVirtual = false;
    bool tripletNumericallyUsable = false;
    std::array<double, 3> outwardNormal {0.0, 0.0, 0.0};
    double planeOffset = 0.0;
    double determinant = 0.0;
    double conditionEstimate = 0.0;
};

struct Bs2127TopologyCatalog {
    std::array<UnitVector3, kBs2127TopologyStorageCount> pointVectors {};
    std::array<bool, kBs2127TopologyStorageCount> virtualFlags {};
    std::size_t pointCount = kSystemHSpeakerCount + 1U;
    std::vector<Bs2127Facet> facets;
    std::size_t totalTriplesEnumerated = 0U;
    std::size_t supportingPlanesFound = 0U;
    std::size_t mergedPlaneDuplicates = 0U;
    std::size_t nonSupportingTriplesRejected = 0U;
    std::size_t conditionRejected = 0U;
    Bs2127TopologyOptions options {};
    bool valid = false;
    std::string reason;
};

// The catalog contains the 22 fixed System H nominal points plus the
// BS.2127 generic-layout forced lower virtual point (0,0,-1). T+000 exists
// in System H, so no upper virtual point is added.
Bs2127TopologyCatalog buildSystemHBs2127Topology(
    const Bs2127TopologyOptions &options = {});

// Shared hull/topology builder used by the bounded configured-layout slices.
// The point storage is capacity-limited for compatibility; only pointCount
// entries participate in the convex hull. Direct-downmix metadata is kept
// separate from ordinary real-speaker identities by the caller.
Bs2127TopologyCatalog buildBs2127Topology(
    const std::array<UnitVector3, kBs2127TopologyStorageCount> &pointVectors,
    const std::array<bool, kBs2127TopologyStorageCount> &virtualFlags,
    std::size_t pointCount,
    const Bs2127TopologyOptions &options = {});

enum class Bs2127SelectionStatus {
    Selected,
    Unsupported,
    InvalidInput,
};

const char *bs2127SelectionStatusName(Bs2127SelectionStatus status);

constexpr std::size_t kNoBs2127Facet =
    std::numeric_limits<std::size_t>::max();

struct Bs2127PointSourceSelection {
    Bs2127SelectionStatus status = Bs2127SelectionStatus::InvalidInput;
    std::string reason;
    std::size_t selectedFacetIndex = kNoBs2127Facet;
    Bs2127FacetKind selectedKind = Bs2127FacetKind::UnsupportedNgon;
    std::size_t facetsExamined = 0U;
    std::array<double, kSystemHSpeakerCount> gains {};
    std::array<double, 3> tripletGains {0.0, 0.0, 0.0};
    std::array<double, 4> quadGains {0.0, 0.0, 0.0, 0.0};
    double conditionEstimate = 0.0;
};

// Real Triplet and Quad facets are queried in stable topology order; first
// valid wins, which mirrors the BS.2127 RegionHandler ordering semantics.
// VirtualHullFacet is intentionally left to the separate VirtualNgon region.
Bs2127PointSourceSelection selectSystemHPointSource(
    const Bs2127TopologyCatalog &catalog, const UnitVector3 &sourceVector);

// Configuration-process variant: topology and region order come from the
// nominal catalog, while these real vectors are used by the selected region
// solver, as required by BS.2127-1 §6.1.3.1 steps 3 and 6.
Bs2127PointSourceSelection selectSystemHPointSource(
    const Bs2127TopologyCatalog &catalog, const UnitVector3 &sourceVector,
    const std::array<UnitVector3, kSystemHSpeakerCount> &regionVectors);

} // namespace eac3render
