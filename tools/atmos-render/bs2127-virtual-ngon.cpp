#include "bs2127-virtual-ngon.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>

namespace eac3render {
namespace {

using Edge = std::pair<std::size_t, std::size_t>;

bool finite(double value)
{
    return std::isfinite(value);
}

bool finiteVector(const UnitVector3 &vector)
{
    return std::all_of(vector.begin(), vector.end(), [](double value) {
        return finite(value);
    });
}

bool finiteGains(const std::array<double, kSystemHSpeakerCount> &gains)
{
    return std::all_of(gains.begin(), gains.end(), [](double value) {
        return finite(value);
    });
}

double normSquared(const UnitVector3 &vector)
{
    return vector[0] * vector[0]
        + vector[1] * vector[1]
        + vector[2] * vector[2];
}

Edge makeEdge(std::size_t first, std::size_t second)
{
    return std::minmax(first, second);
}

bool contains(const std::vector<std::size_t> &values, std::size_t value)
{
    return std::find(values.begin(), values.end(), value) != values.end();
}

void invalidate(Bs2127LowerVirtualNgon *region, const char *reason)
{
    region->valid = false;
    region->reason = reason;
}

std::vector<std::size_t> walkCycle(
    std::size_t start, std::size_t firstNext,
    const std::array<std::vector<std::size_t>, kSystemHSpeakerCount> &adjacency)
{
    std::vector<std::size_t> cycle;
    cycle.push_back(start);
    std::size_t previous = start;
    std::size_t current = firstNext;
    for (std::size_t step = 0U; step <= kSystemHSpeakerCount; ++step) {
        if (current == start) {
            return cycle;
        }
        if (current >= adjacency.size() || contains(cycle, current)) {
            return {};
        }
        cycle.push_back(current);
        const auto &neighbors = adjacency[current];
        if (neighbors.size() != 2U) {
            return {};
        }
        const std::size_t next = neighbors[0] == previous
            ? neighbors[1] : neighbors[0];
        previous = current;
        current = next;
    }
    return {};
}

std::vector<std::size_t> standardSystemHLowerRing()
{
    // Project-local deterministic direction for this fixed System H lower
    // VirtualNgon. BS.2127 requires adjacent edge coverage and first-valid
    // handling, but this direction is not a normative ordering constant.
    // Labels avoid silently following a reordered implementation array.
    constexpr std::array<Bs2051Label, 8> labels {
        Bs2051Label::MPlus135,
        Bs2051Label::MPlus090,
        Bs2051Label::BPlus045,
        Bs2051Label::BPlus000,
        Bs2051Label::BMinus045,
        Bs2051Label::MMinus090,
        Bs2051Label::MMinus135,
        Bs2051Label::MPlus180,
    };
    std::vector<std::size_t> result;
    const auto &layout = Bs2051SystemHLayout::systemH();
    for (Bs2051Label label : labels) {
        const auto found = std::find_if(
            layout.begin(), layout.end(), [label](const auto &position) {
                return position.label == label;
            });
        if (found == layout.end()) {
            return {};
        }
        result.push_back(static_cast<std::size_t>(
            std::distance(layout.begin(), found)));
    }
    return result;
}

bool cyclicRingEquivalent(const std::vector<std::size_t> &candidate,
                          const std::vector<std::size_t> &standard)
{
    if (candidate.size() != standard.size() || standard.empty()) {
        return false;
    }
    for (std::size_t offset = 0U; offset < standard.size(); ++offset) {
        bool forward = true;
        bool reverse = true;
        for (std::size_t index = 0U; index < standard.size(); ++index) {
            forward = forward
                && candidate[index]
                    == standard[(offset + index) % standard.size()];
            const std::size_t reverseIndex =
                (offset + standard.size() - index) % standard.size();
            reverse = reverse
                && candidate[index] == standard[reverseIndex];
        }
        if (forward || reverse) {
            return true;
        }
    }
    return false;
}

bool validateRegionForRender(const Bs2127TopologyCatalog &catalog,
                             const Bs2127LowerVirtualNgon &region,
                             std::string *reason)
{
    const auto reject = [reason](const char *value) {
        if (reason) {
            *reason = value;
        }
        return false;
    };
    if (region.virtualPointIndex != kBs2127VirtualBottomIndex
        || region.virtualPointIndex >= catalog.pointVectors.size()
        || !catalog.virtualFlags[region.virtualPointIndex]) {
        return reject("invalid-virtual-point-index");
    }
    const std::vector<std::size_t> standardRing = standardSystemHLowerRing();
    // This is deliberately the fixed System H lower primitive, not a generic
    // VirtualNgon validator: the current contract is exactly eight vertices
    // and eight triangles. Rotation/reversal is accepted for callers that
    // preserve edge order; buildLowerVirtualNgon itself uses one fixed start
    // and direction for deterministic production calls.
    if (standardRing.size() != 8U
        || region.ringRealSpeakerIndices.size() != 8U
        || region.triangles.size() != 8U
        || !cyclicRingEquivalent(region.ringRealSpeakerIndices, standardRing)) {
        return reject("invalid-ring-or-triangle-count");
    }
    const double unitTolerance =
        catalog.options.tripletOptions.unitVectorTolerance;
    if (!finite(unitTolerance) || unitTolerance < 0.0
        || !finite(region.downmixCoefficient)
        || region.downmixCoefficient <= 0.0) {
        return reject("invalid-virtual-ngon-numeric-policy");
    }
    const double expectedDownmix = 1.0 / std::sqrt(
        static_cast<double>(region.ringRealSpeakerIndices.size()));
    if (!finite(expectedDownmix)
        || std::abs(region.downmixCoefficient - expectedDownmix)
            > std::max(1.0e-12, unitTolerance)) {
        return reject("virtual-downmix-coefficient-mismatch");
    }

    const auto validUnitVector = [unitTolerance](const UnitVector3 &vector) {
        if (!finiteVector(vector)) {
            return false;
        }
        const double squaredNorm = normSquared(vector);
        return finite(squaredNorm)
            && std::abs(squaredNorm - 1.0) <= unitTolerance;
    };
    if (!validUnitVector(catalog.pointVectors[region.virtualPointIndex])) {
        return reject("non-unit-virtual-point-vector");
    }

    std::vector<bool> seenSpeakers(kSystemHSpeakerCount, false);
    for (std::size_t speaker : region.ringRealSpeakerIndices) {
        if (speaker >= kSystemHSpeakerCount || seenSpeakers[speaker]) {
            return reject("duplicate-or-out-of-range-ring-speaker");
        }
        seenSpeakers[speaker] = true;
        if (!validUnitVector(catalog.pointVectors[speaker])) {
            return reject("non-unit-ring-speaker-vector");
        }
    }

    std::size_t lowerFacetCount = 0U;
    for (const Bs2127Facet &facet : catalog.facets) {
        if (!facet.containsVirtual
            || !contains(facet.vertexIndices, region.virtualPointIndex)) {
            continue;
        }
        ++lowerFacetCount;
        if (facet.kind != Bs2127FacetKind::VirtualHullFacet
            || facet.realSpeakerIndices.size() != 2U
            || facet.realSpeakerIndices[0] >= kSystemHSpeakerCount
            || facet.realSpeakerIndices[1] >= kSystemHSpeakerCount
            || facet.realSpeakerIndices[0] == facet.realSpeakerIndices[1]) {
            return reject("invalid-lower-virtual-facet");
        }
    }
    if (lowerFacetCount != 8U) {
        return reject("incomplete-lower-virtual-facet-set");
    }

    std::vector<bool> seenFacets(catalog.facets.size(), false);
    for (std::size_t index = 0U; index < region.triangles.size(); ++index) {
        const Bs2127VirtualTriangle &triangle = region.triangles[index];
        const std::size_t expectedFirst = region.ringRealSpeakerIndices[index];
        const std::size_t expectedSecond = region.ringRealSpeakerIndices[
            (index + 1U) % region.ringRealSpeakerIndices.size()];
        if (triangle.firstRealSpeakerIndex != expectedFirst
            || triangle.secondRealSpeakerIndex != expectedSecond
            || triangle.sourceFacetIndex >= catalog.facets.size()
            || seenFacets[triangle.sourceFacetIndex]) {
            return reject("virtual-triangle-order-or-facet-mismatch");
        }
        seenFacets[triangle.sourceFacetIndex] = true;
        const Bs2127Facet &facet = catalog.facets[triangle.sourceFacetIndex];
        const Edge expectedEdge = makeEdge(expectedFirst, expectedSecond);
        if (facet.kind != Bs2127FacetKind::VirtualHullFacet
            || !facet.containsVirtual
            || !contains(facet.vertexIndices, region.virtualPointIndex)
            || facet.realSpeakerIndices.size() != 2U
            || makeEdge(facet.realSpeakerIndices[0],
                        facet.realSpeakerIndices[1]) != expectedEdge) {
            return reject("virtual-triangle-topology-mismatch");
        }
    }
    std::size_t seenLowerFacetCount = 0U;
    for (std::size_t index = 0U; index < seenFacets.size(); ++index) {
        if (seenFacets[index]
            && catalog.facets[index].containsVirtual
            && contains(catalog.facets[index].vertexIndices,
                        region.virtualPointIndex)) {
            ++seenLowerFacetCount;
        }
    }
    if (seenLowerFacetCount != lowerFacetCount) {
        return reject("lower-virtual-facet-set-not-fully-covered");
    }
    return true;
}

} // namespace

Bs2127LowerVirtualNgon buildLowerVirtualNgon(
    const Bs2127TopologyCatalog &catalog)
{
    Bs2127LowerVirtualNgon region;
    if (!catalog.valid) {
        invalidate(&region, "invalid-topology-catalog");
        return region;
    }
    if (region.virtualPointIndex >= catalog.pointVectors.size()
        || !catalog.virtualFlags[region.virtualPointIndex]) {
        invalidate(&region, "lower-virtual-point-not-present");
        return region;
    }

    std::map<Edge, std::size_t> edgeFacets;
    std::array<std::vector<std::size_t>, kSystemHSpeakerCount> adjacency;
    for (std::size_t facetIndex = 0U; facetIndex < catalog.facets.size();
         ++facetIndex) {
        const Bs2127Facet &facet = catalog.facets[facetIndex];
        if (facet.kind != Bs2127FacetKind::VirtualHullFacet) {
            continue;
        }
        if (!facet.containsVirtual
            || !contains(facet.vertexIndices, region.virtualPointIndex)) {
            continue;
        }
        if (facet.realSpeakerIndices.size() != 2U
            || facet.realSpeakerIndices[0] >= kSystemHSpeakerCount
            || facet.realSpeakerIndices[1] >= kSystemHSpeakerCount
            || facet.realSpeakerIndices[0] == facet.realSpeakerIndices[1]) {
            invalidate(&region, "virtual-hull-facet-is-not-a-real-edge");
            return region;
        }
        const Edge edge = makeEdge(facet.realSpeakerIndices[0],
                                   facet.realSpeakerIndices[1]);
        if (edgeFacets.find(edge) != edgeFacets.end()) {
            invalidate(&region, "duplicate-lower-virtual-ring-edge");
            return region;
        }
        edgeFacets.emplace(edge, facetIndex);
        adjacency[edge.first].push_back(edge.second);
        adjacency[edge.second].push_back(edge.first);
    }

    if (edgeFacets.size() < 3U) {
        invalidate(&region, "no-lower-virtual-ngon-ring");
        return region;
    }
    for (auto &neighbors : adjacency) {
        std::sort(neighbors.begin(), neighbors.end());
        if (!neighbors.empty() && neighbors.size() != 2U) {
            invalidate(&region, "lower-virtual-ring-is-not-a-cycle");
            return region;
        }
    }

    const std::size_t start = edgeFacets.begin()->first.first;
    const auto &startNeighbors = adjacency[start];
    if (startNeighbors.size() != 2U) {
        invalidate(&region, "lower-virtual-ring-start-degree-mismatch");
        return region;
    }
    const std::vector<std::size_t> firstCycle =
        walkCycle(start, startNeighbors[0], adjacency);
    const std::vector<std::size_t> secondCycle =
        walkCycle(start, startNeighbors[1], adjacency);
    if (firstCycle.empty() || secondCycle.empty()
        || firstCycle.size() != edgeFacets.size()
        || secondCycle.size() != edgeFacets.size()) {
        invalidate(&region, "lower-virtual-ring-is-not-closed");
        return region;
    }
    const std::vector<std::size_t> recoveredCycle = firstCycle < secondCycle
        ? firstCycle : secondCycle;
    const std::vector<std::size_t> standardCycle = standardSystemHLowerRing();
    if (standardCycle.size() != edgeFacets.size()
        || recoveredCycle.size() != standardCycle.size()) {
        invalidate(&region, "lower-virtual-ring-standard-order-unavailable");
        return region;
    }
    for (std::size_t index = 0U; index < standardCycle.size(); ++index) {
        if (!contains(recoveredCycle, standardCycle[index])) {
            invalidate(&region, "lower-virtual-ring-standard-speaker-mismatch");
            return region;
        }
        const std::size_t first = standardCycle[index];
        const std::size_t second = standardCycle[
            (index + 1U) % standardCycle.size()];
        if (edgeFacets.find(makeEdge(first, second)) == edgeFacets.end()) {
            invalidate(&region, "lower-virtual-ring-standard-edge-mismatch");
            return region;
        }
    }
    // The adjacency walk proves closure/connectivity; the fixed labels above
    // provide this project's deterministic System H order. BS.2127 does not
    // define this sequence as a normative ordering constant.
    region.ringRealSpeakerIndices = standardCycle;

    for (std::size_t index = 0U; index < region.ringRealSpeakerIndices.size();
         ++index) {
        const std::size_t first = region.ringRealSpeakerIndices[index];
        const std::size_t second = region.ringRealSpeakerIndices[
            (index + 1U) % region.ringRealSpeakerIndices.size()];
        const auto found = edgeFacets.find(makeEdge(first, second));
        if (found == edgeFacets.end()) {
            invalidate(&region, "lower-virtual-ring-edge-not-backed-by-facet");
            return region;
        }
        region.triangles.push_back(
            Bs2127VirtualTriangle {first, second, found->second});
    }

    region.downmixCoefficient =
        1.0 / std::sqrt(static_cast<double>(region.ringRealSpeakerIndices.size()));
    if (!finite(region.downmixCoefficient)) {
        invalidate(&region, "nonfinite-virtual-downmix-coefficient");
        return region;
    }
    region.valid = true;
    region.reason = "none";
    return region;
}

const char *bs2127VirtualNgonStatusName(Bs2127VirtualNgonStatus status)
{
    switch (status) {
    case Bs2127VirtualNgonStatus::Selected: return "Selected";
    case Bs2127VirtualNgonStatus::Unsupported: return "Unsupported";
    case Bs2127VirtualNgonStatus::InvalidInput: return "InvalidInput";
    }
    return "Unknown";
}

Bs2127VirtualNgonRenderResult renderLowerVirtualNgon(
    const Bs2127TopologyCatalog &catalog,
    const Bs2127LowerVirtualNgon &region,
    const UnitVector3 &sourceVector)
{
    Bs2127VirtualNgonRenderResult result;
    if (!catalog.valid || !region.valid) {
        result.status = Bs2127VirtualNgonStatus::Unsupported;
        result.reason = region.reason.empty()
            ? "invalid-topology-or-virtual-ngon-region" : region.reason;
        return result;
    }
    std::string regionReason;
    if (!validateRegionForRender(catalog, region, &regionReason)) {
        result.status = Bs2127VirtualNgonStatus::InvalidInput;
        result.reason = regionReason.empty()
            ? "invalid-virtual-ngon-region" : regionReason;
        return result;
    }
    if (!finiteVector(sourceVector)) {
        result.status = Bs2127VirtualNgonStatus::InvalidInput;
        result.reason = "nonfinite-source-vector";
        return result;
    }
    const double sourcePower = normSquared(sourceVector);
    if (!finite(sourcePower)
        || std::abs(sourcePower - 1.0)
            > catalog.options.tripletOptions.unitVectorTolerance) {
        result.status = Bs2127VirtualNgonStatus::InvalidInput;
        result.reason = "source-vector-not-unit-within-tolerance";
        return result;
    }

    for (std::size_t triangleIndex = 0U;
         triangleIndex < region.triangles.size(); ++triangleIndex) {
        const Bs2127VirtualTriangle &triangle = region.triangles[triangleIndex];
        if (triangle.firstRealSpeakerIndex >= kSystemHSpeakerCount
            || triangle.secondRealSpeakerIndex >= kSystemHSpeakerCount) {
            result.status = Bs2127VirtualNgonStatus::Unsupported;
            result.reason = "virtual-triangle-speaker-index-out-of-range";
            return result;
        }
        const std::array<UnitVector3, 3> vectors {
            catalog.pointVectors[region.virtualPointIndex],
            catalog.pointVectors[triangle.firstRealSpeakerIndex],
            catalog.pointVectors[triangle.secondRealSpeakerIndex],
        };
        const TripletSolveResult solve = solveTriplet(
            vectors, sourceVector, catalog.options.tripletOptions);
        if (!solve.accepted) {
            continue;
        }

        result.selectedTriangleIndex = triangleIndex;
        result.triangleGains = solve.normalizedGains;
        result.virtualGain = solve.normalizedGains[0];
        for (std::size_t index = 0U; index < kSystemHSpeakerCount; ++index) {
            result.gains[index] = 0.0;
        }
        result.gains[triangle.firstRealSpeakerIndex] += solve.normalizedGains[1];
        result.gains[triangle.secondRealSpeakerIndex] += solve.normalizedGains[2];
        // BS.2127 §6.1.2.2 applies Wdmx to the virtual gain. For the direct
        // downmix in §6.1.3.1, each of the n adjacent real speakers receives
        // 1/sqrt(n); no private renderer weighting is introduced here.
        for (std::size_t speaker : region.ringRealSpeakerIndices) {
            result.gains[speaker] += solve.normalizedGains[0]
                * region.downmixCoefficient;
        }

        result.preNormalizationPower = 0.0;
        for (double gain : result.gains) {
            if (!finite(gain)) {
                result.status = Bs2127VirtualNgonStatus::Unsupported;
                result.reason = "nonfinite-virtual-ngon-gain";
                return result;
            }
            result.preNormalizationPower += gain * gain;
        }
        if (!finite(result.preNormalizationPower)
            || result.preNormalizationPower <= std::numeric_limits<double>::min()) {
            result.status = Bs2127VirtualNgonStatus::Unsupported;
            result.reason = "zero-or-nonfinite-post-downmix-power";
            return result;
        }
        const double inversePower = 1.0 / std::sqrt(result.preNormalizationPower);
        for (double &gain : result.gains) {
            gain *= inversePower;
        }
        if (!finiteGains(result.gains)) {
            result.status = Bs2127VirtualNgonStatus::Unsupported;
            result.reason = "nonfinite-normalized-virtual-ngon-gain";
            return result;
        }
        result.status = Bs2127VirtualNgonStatus::Selected;
        result.reason = "virtual-triangle-selected-first-valid-then-wdmx-normalized";
        return result;
    }

    result.status = Bs2127VirtualNgonStatus::Unsupported;
    result.reason = "no-valid-virtual-triangle-first-valid";
    return result;
}

Bs2127VirtualNgonRenderResult renderLowerVirtualNgon(
    const Bs2127TopologyCatalog &catalog,
    const Bs2127LowerVirtualNgon &region,
    const UnitVector3 &sourceVector,
    const std::array<UnitVector3, kBs2127TopologyStorageCount> &regionVectors)
{
    Bs2127TopologyCatalog adaptedCatalog = catalog;
    adaptedCatalog.pointVectors = regionVectors;
    return renderLowerVirtualNgon(adaptedCatalog, region, sourceVector);
}

Bs2127VirtualNgonRenderResult renderLowerVirtualNgon(
    const Bs2127TopologyCatalog &catalog,
    const Bs2127LowerVirtualNgon &region,
    const UnitVector3 &sourceVector,
    const std::array<UnitVector3, kBs2127TopologyPointCount> &regionVectors)
{
    std::array<UnitVector3, kBs2127TopologyStorageCount> expanded =
        catalog.pointVectors;
    for (std::size_t index = 0U; index < kBs2127TopologyPointCount; ++index) {
        expanded[index] = regionVectors[index];
    }
    return renderLowerVirtualNgon(catalog, region, sourceVector, expanded);
}

} // namespace eac3render
