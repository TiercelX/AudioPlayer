#include "bs2127-point-source-panner.h"

#include <algorithm>
#include <cmath>

namespace eac3render {
namespace {

bool finite(double value)
{
    return std::isfinite(value);
}

bool finiteGains(const std::array<double, kSystemHSpeakerCount> &gains)
{
    return std::all_of(gains.begin(), gains.end(),
                       [](double gain) { return finite(gain); });
}

double power(const std::array<double, kSystemHSpeakerCount> &gains)
{
    double result = 0.0;
    for (double gain : gains) {
        result += gain * gain;
    }
    return result;
}

} // namespace

const char *bs2127PointSourceRegionKindName(
    Bs2127PointSourceRegionKind kind)
{
    switch (kind) {
    case Bs2127PointSourceRegionKind::Identity: return "Identity";
    case Bs2127PointSourceRegionKind::Triplet: return "Triplet";
    case Bs2127PointSourceRegionKind::Quad: return "Quad";
    case Bs2127PointSourceRegionKind::VirtualNgon: return "VirtualNgon";
    }
    return "Unknown";
}

const char *bs2127PointSourcePannerStatusName(
    Bs2127PointSourcePannerStatus status)
{
    switch (status) {
    case Bs2127PointSourcePannerStatus::Selected: return "Selected";
    case Bs2127PointSourcePannerStatus::Unsupported: return "Unsupported";
    case Bs2127PointSourcePannerStatus::InvalidInput: return "InvalidInput";
    }
    return "Unknown";
}

Bs2127SystemHPointSourcePanner::Bs2127SystemHPointSourcePanner(
    const Bs2127TopologyOptions &options)
    : catalog_(buildSystemHBs2127Topology(options)),
      lowerVirtualNgon_(buildLowerVirtualNgon(catalog_))
{
    regionVectors_.fill({0.0, 0.0, 0.0});
    for (std::size_t index = 0U; index < kSystemHSpeakerCount; ++index) {
        regionVectors_[index] = catalog_.pointVectors[index];
    }
    nominalIdentityGuardEnabled_ = true;
    if (!catalog_.valid) {
        reason_ = catalog_.reason.empty()
            ? "invalid-topology-catalog" : catalog_.reason;
    } else if (!lowerVirtualNgon_.valid) {
        reason_ = lowerVirtualNgon_.reason.empty()
            ? "invalid-lower-virtual-ngon" : lowerVirtualNgon_.reason;
    } else {
        reason_ = "none";
    }
}

Bs2127SystemHPointSourcePanner::Bs2127SystemHPointSourcePanner(
    const std::array<UnitVector3, kSystemHSpeakerCount> &regionVectors,
    const Bs2127TopologyOptions &options)
    : catalog_(buildSystemHBs2127Topology(options)),
      lowerVirtualNgon_(buildLowerVirtualNgon(catalog_)),
      regionVectors_(regionVectors)
{
    nominalIdentityGuardEnabled_ = true;
    for (std::size_t index = 0U; index < kSystemHSpeakerCount; ++index) {
        nominalIdentityGuardEnabled_ = nominalIdentityGuardEnabled_
            && regionVectors_[index] == catalog_.pointVectors[index];
    }
    if (!catalog_.valid) {
        reason_ = catalog_.reason.empty()
            ? "invalid-topology-catalog" : catalog_.reason;
    } else if (!lowerVirtualNgon_.valid) {
        reason_ = lowerVirtualNgon_.reason.empty()
            ? "invalid-lower-virtual-ngon" : lowerVirtualNgon_.reason;
    } else {
        reason_ = "none";
    }
}

bool Bs2127SystemHPointSourcePanner::valid() const
{
    return catalog_.valid && lowerVirtualNgon_.valid;
}

const std::string &Bs2127SystemHPointSourcePanner::reason() const
{
    return reason_;
}

const Bs2127TopologyCatalog &Bs2127SystemHPointSourcePanner::catalog() const
{
    return catalog_;
}

const Bs2127LowerVirtualNgon &
Bs2127SystemHPointSourcePanner::lowerVirtualNgon() const
{
    return lowerVirtualNgon_;
}

Bs2127PointSourcePannerResult Bs2127SystemHPointSourcePanner::render(
    const UnitVector3 &sourceVector) const
{
    Bs2127PointSourcePannerResult result;
    if (!valid()) {
        result.status = Bs2127PointSourcePannerStatus::Unsupported;
        result.reason = reason_;
        return result;
    }

    // The selector's identity shortcut is valid only for strict
    // nominal==actual layouts.  A displaced real layout must solve even at a
    // nominal speaker direction, otherwise it creates an identity spike.
    Bs2127TopologyCatalog selectionCatalog = catalog_;
    selectionCatalog.options.identityGuardEnabled =
        selectionCatalog.options.identityGuardEnabled
        && nominalIdentityGuardEnabled_;
    const Bs2127PointSourceSelection selected = selectSystemHPointSource(
        selectionCatalog, sourceVector, regionVectors_);
    result.regionsExamined = selected.facetsExamined;
    if (selected.status == Bs2127SelectionStatus::InvalidInput) {
        result.status = Bs2127PointSourcePannerStatus::InvalidInput;
        result.reason = selected.reason;
        return result;
    }
    if (selected.status == Bs2127SelectionStatus::Selected) {
        result.status = Bs2127PointSourcePannerStatus::Selected;
        result.reason = selected.reason;
        result.selectedFacetIndex = selected.selectedFacetIndex;
        result.gains = selected.gains;
        result.region = selected.reason == "speaker-identity"
            ? Bs2127PointSourceRegionKind::Identity
            : selected.selectedKind == Bs2127FacetKind::Quad
                ? Bs2127PointSourceRegionKind::Quad
                : Bs2127PointSourceRegionKind::Triplet;
        if (!finiteGains(result.gains) || std::abs(power(result.gains) - 1.0)
                > 1.0e-8) {
            result.status = Bs2127PointSourcePannerStatus::Unsupported;
            result.reason = "selector-produced-invalid-gain-vector";
            result.gains.fill(0.0);
        }
        return result;
    }

    std::array<UnitVector3, kBs2127TopologyStorageCount> virtualRegionVectors =
        catalog_.pointVectors;
    for (std::size_t index = 0U; index < kSystemHSpeakerCount; ++index) {
        virtualRegionVectors[index] = regionVectors_[index];
    }
    const Bs2127VirtualNgonRenderResult virtualResult =
        renderLowerVirtualNgon(catalog_, lowerVirtualNgon_, sourceVector,
                               virtualRegionVectors);
    ++result.regionsExamined;
    if (virtualResult.status == Bs2127VirtualNgonStatus::InvalidInput) {
        result.status = Bs2127PointSourcePannerStatus::InvalidInput;
        result.reason = virtualResult.reason;
        return result;
    }
    if (virtualResult.status == Bs2127VirtualNgonStatus::Selected) {
        result.status = Bs2127PointSourcePannerStatus::Selected;
        result.reason = virtualResult.reason;
        result.region = Bs2127PointSourceRegionKind::VirtualNgon;
        result.selectedTriangleIndex = virtualResult.selectedTriangleIndex;
        result.gains = virtualResult.gains;
        if (!finiteGains(result.gains) || std::abs(power(result.gains) - 1.0)
                > 1.0e-8) {
            result.status = Bs2127PointSourcePannerStatus::Unsupported;
            result.reason = "virtual-ngon-produced-invalid-gain-vector";
            result.gains.fill(0.0);
        }
        return result;
    }

    result.status = Bs2127PointSourcePannerStatus::Unsupported;
    result.reason = "no-valid-triplet-quad-or-virtual-ngon-region";
    return result;
}

} // namespace eac3render
