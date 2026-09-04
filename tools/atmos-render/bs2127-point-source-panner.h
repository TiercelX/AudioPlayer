#pragma once

#include "bs2127-virtual-ngon.h"

#include <array>
#include <cstddef>
#include <limits>
#include <string>

namespace eac3render {

enum class Bs2127PointSourceRegionKind {
    Identity,
    Triplet,
    Quad,
    VirtualNgon,
};

const char *bs2127PointSourceRegionKindName(
    Bs2127PointSourceRegionKind kind);

enum class Bs2127PointSourcePannerStatus {
    Selected,
    Unsupported,
    InvalidInput,
};

const char *bs2127PointSourcePannerStatusName(
    Bs2127PointSourcePannerStatus status);

constexpr std::size_t kNoBs2127PointSourceTriangle =
    std::numeric_limits<std::size_t>::max();

struct Bs2127PointSourcePannerResult {
    Bs2127PointSourcePannerStatus status =
        Bs2127PointSourcePannerStatus::InvalidInput;
    std::string reason;
    Bs2127PointSourceRegionKind region =
        Bs2127PointSourceRegionKind::Identity;
    std::size_t selectedFacetIndex = kNoBs2127Facet;
    std::size_t selectedTriangleIndex = kNoBs2127PointSourceTriangle;
    std::size_t regionsExamined = 0U;
    std::array<double, kSystemHSpeakerCount> gains {};
};

// BS.2127-1 §6.1.1 first-region-valid dispatcher for the current nominal
// System H contract. The selector's stable Triplet/Quad facet order is tried
// first; the lower-pole VirtualNgon is the following region. The catalog and
// lower region are owned so callers cannot accidentally invalidate a panner's
// topology state between calls.
class Bs2127SystemHPointSourcePanner {
public:
    explicit Bs2127SystemHPointSourcePanner(
        const Bs2127TopologyOptions &options = {});

    // BS.2127-1 §6.1.3.1 configuration-process variant.  Topology and
    // first-region order remain the nominal System H catalog; supplied
    // vectors are the corresponding real loudspeaker positions used by the
    // region solvers.  Validation of the real layout belongs to the adapter.
    explicit Bs2127SystemHPointSourcePanner(
        const std::array<UnitVector3, kSystemHSpeakerCount> &regionVectors,
        const Bs2127TopologyOptions &options = {});

    bool valid() const;
    const std::string &reason() const;
    const Bs2127TopologyCatalog &catalog() const;
    const Bs2127LowerVirtualNgon &lowerVirtualNgon() const;

    Bs2127PointSourcePannerResult render(
        const UnitVector3 &sourceVector) const;

private:
    Bs2127TopologyCatalog catalog_;
    Bs2127LowerVirtualNgon lowerVirtualNgon_;
    std::array<UnitVector3, kSystemHSpeakerCount> regionVectors_ {};
    bool nominalIdentityGuardEnabled_ = true;
    std::string reason_;
};

} // namespace eac3render
