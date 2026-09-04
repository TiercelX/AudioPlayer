#include "bs2127-selector.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace eac3render {
namespace {

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

double dot(const UnitVector3 &left, const UnitVector3 &right)
{
    return left[0] * right[0]
        + left[1] * right[1]
        + left[2] * right[2];
}

UnitVector3 subtract(const UnitVector3 &left, const UnitVector3 &right)
{
    return {left[0] - right[0], left[1] - right[1], left[2] - right[2]};
}

UnitVector3 cross(const UnitVector3 &left, const UnitVector3 &right)
{
    return {
        left[1] * right[2] - left[2] * right[1],
        left[2] * right[0] - left[0] * right[2],
        left[0] * right[1] - left[1] * right[0],
    };
}

double norm(const UnitVector3 &vector)
{
    return std::sqrt(dot(vector, vector));
}

UnitVector3 scaled(const UnitVector3 &vector, double factor)
{
    return {vector[0] * factor, vector[1] * factor, vector[2] * factor};
}

struct TripletGeometryMetrics {
    bool finiteGeometry = false;
    double determinant = 0.0;
    double conditionEstimate = 0.0;
};

TripletGeometryMetrics measureTripletGeometry(
    const std::array<UnitVector3, 3> &vectors)
{
    TripletGeometryMetrics result;
    const UnitVector3 inverseRow0 = cross(vectors[1], vectors[2]);
    const UnitVector3 inverseRow1 = cross(vectors[2], vectors[0]);
    const UnitVector3 inverseRow2 = cross(vectors[0], vectors[1]);
    result.determinant = dot(vectors[0], inverseRow0);
    if (!finite(result.determinant)) {
        return result;
    }
    const double determinantMagnitude = std::abs(result.determinant);
    if (determinantMagnitude == 0.0) {
        return result;
    }
    const double inverseRowNorm0 =
        std::abs(inverseRow0[0]) + std::abs(inverseRow0[1])
        + std::abs(inverseRow0[2]);
    const double inverseRowNorm1 =
        std::abs(inverseRow1[0]) + std::abs(inverseRow1[1])
        + std::abs(inverseRow1[2]);
    const double inverseRowNorm2 =
        std::abs(inverseRow2[0]) + std::abs(inverseRow2[1])
        + std::abs(inverseRow2[2]);
    const double matrixRowNorm0 =
        std::abs(vectors[0][0]) + std::abs(vectors[1][0])
        + std::abs(vectors[2][0]);
    const double matrixRowNorm1 =
        std::abs(vectors[0][1]) + std::abs(vectors[1][1])
        + std::abs(vectors[2][1]);
    const double matrixRowNorm2 =
        std::abs(vectors[0][2]) + std::abs(vectors[1][2])
        + std::abs(vectors[2][2]);
    const double inverseNorm = std::max({inverseRowNorm0, inverseRowNorm1,
                                         inverseRowNorm2})
        / determinantMagnitude;
    const double matrixNorm = std::max({matrixRowNorm0, matrixRowNorm1,
                                         matrixRowNorm2});
    result.conditionEstimate = matrixNorm * inverseNorm;
    result.finiteGeometry = finite(result.conditionEstimate);
    return result;
}

bool validOptions(const Bs2127TopologyOptions &options)
{
    return finite(options.planeTolerance) && options.planeTolerance > 0.0
        && finite(options.planeMergeTolerance)
        && options.planeMergeTolerance >= options.planeTolerance
        && finite(options.maximumConditionEstimate)
        && options.maximumConditionEstimate > 0.0
        && finite(options.identityVectorTolerance)
        && options.identityVectorTolerance >= 0.0
        && finite(options.tripletOptions.determinantTolerance)
        && options.tripletOptions.determinantTolerance >= 0.0
        && finite(options.tripletOptions.negativeGainTolerance)
        && options.tripletOptions.negativeGainTolerance >= 0.0
        && finite(options.tripletOptions.unitVectorTolerance)
        && options.tripletOptions.unitVectorTolerance >= 0.0
        && finite(options.quadOptions.polynomialTolerance)
        && options.quadOptions.polynomialTolerance > 0.0
        && finite(options.quadOptions.boundaryTolerance)
        && options.quadOptions.boundaryTolerance >= 0.0
        && finite(options.quadOptions.directionTolerance)
        && options.quadOptions.directionTolerance >= 0.0
        && finite(options.quadOptions.unitVectorTolerance)
        && options.quadOptions.unitVectorTolerance >= 0.0;
}

bool samePlane(const Bs2127Facet &facet, const UnitVector3 &normal,
               double offset, double tolerance)
{
    return dot(facet.outwardNormal, normal) >= 1.0 - tolerance
        && std::abs(facet.planeOffset - offset) <= tolerance;
}

bool deriveSupportingPlane(
    const std::array<UnitVector3, kBs2127TopologyStorageCount> &points,
    std::size_t pointCount, std::size_t first, std::size_t second,
    std::size_t third,
    const Bs2127TopologyOptions &options, UnitVector3 *outwardNormal,
    double *planeOffset, std::vector<std::size_t> *vertices)
{
    const UnitVector3 edgeA = subtract(points[second], points[first]);
    const UnitVector3 edgeB = subtract(points[third], points[first]);
    const UnitVector3 rawNormal = cross(edgeA, edgeB);
    const double rawNorm = norm(rawNormal);
    if (!finite(rawNorm) || rawNorm <= options.planeTolerance) {
        return false;
    }

    UnitVector3 normal = scaled(rawNormal, 1.0 / rawNorm);
    double offset = dot(normal, points[first]);
    double minimum = dot(normal, points[0]) - offset;
    double maximum = minimum;
    for (std::size_t index = 1U; index < pointCount; ++index) {
        const double distance = dot(normal, points[index]) - offset;
        minimum = std::min(minimum, distance);
        maximum = std::max(maximum, distance);
    }

    // Orient the plane outward so every point is on or behind it.
    if (minimum >= -options.planeTolerance) {
        const double oldMinimum = minimum;
        const double oldMaximum = maximum;
        normal = scaled(normal, -1.0);
        offset = -offset;
        minimum = -oldMaximum;
        maximum = -oldMinimum;
    }
    if (maximum > options.planeTolerance) {
        return false;
    }

    vertices->clear();
    for (std::size_t index = 0U; index < pointCount; ++index) {
        if (std::abs(dot(normal, points[index]) - offset)
            <= options.planeTolerance) {
            vertices->push_back(index);
        }
    }
    if (vertices->size() < 3U) {
        return false;
    }
    *outwardNormal = normal;
    *planeOffset = offset;
    return true;
}

bool lexicographicFacetOrder(const Bs2127Facet &left,
                             const Bs2127Facet &right)
{
    if (left.vertexIndices != right.vertexIndices) {
        return left.vertexIndices < right.vertexIndices;
    }
    return static_cast<int>(left.kind) < static_cast<int>(right.kind);
}

void setInvalid(Bs2127PointSourceSelection *result, const char *reason)
{
    result->status = Bs2127SelectionStatus::InvalidInput;
    result->reason = reason;
}

std::vector<std::size_t> quadPerimeterOrder(
    const std::vector<std::size_t> &speakerIndices,
    const std::array<UnitVector3, kBs2127TopologyStorageCount> &points,
    const UnitVector3 &outwardNormal)
{
    if (speakerIndices.size() != 4U) {
        return {};
    }
    // The face normal points away from the listener for a convex hull face.
    // Looking from the listener therefore uses -outwardNormal as the viewing
    // axis; ascending angles in this basis are anticlockwise as required by
    // BS.2127 §6.1.2.3.1.
    const UnitVector3 viewAxis = scaled(outwardNormal, -1.0);
    UnitVector3 centre {0.0, 0.0, 0.0};
    for (std::size_t index : speakerIndices) {
        if (index >= points.size()) {
            return {};
        }
        for (std::size_t component = 0U; component < 3U; ++component) {
            centre[component] += points[index][component] * 0.25;
        }
    }
    UnitVector3 basisU = subtract(points[speakerIndices.front()], centre);
    const double basisNorm = norm(basisU);
    if (!finite(basisNorm) || basisNorm <= 1.0e-12) {
        return {};
    }
    basisU = scaled(basisU, 1.0 / basisNorm);
    UnitVector3 basisV = cross(viewAxis, basisU);
    const double basisVNorm = norm(basisV);
    if (!finite(basisVNorm) || basisVNorm <= 1.0e-12) {
        return {};
    }
    basisV = scaled(basisV, 1.0 / basisVNorm);

    std::vector<std::pair<double, std::size_t>> angles;
    angles.reserve(speakerIndices.size());
    for (std::size_t index : speakerIndices) {
        const UnitVector3 relative = subtract(points[index], centre);
        angles.emplace_back(std::atan2(dot(relative, basisV),
                                       dot(relative, basisU)), index);
    }
    std::sort(angles.begin(), angles.end(),
              [](const auto &left, const auto &right) {
                  if (left.first != right.first) {
                      return left.first < right.first;
                  }
                  return left.second < right.second;
              });
    std::vector<std::size_t> result;
    result.reserve(angles.size());
    for (const auto &entry : angles) {
        result.push_back(entry.second);
    }
    return result;
}

} // namespace

const char *bs2127FacetKindName(Bs2127FacetKind kind)
{
    switch (kind) {
    case Bs2127FacetKind::Triplet: return "Triplet";
    case Bs2127FacetKind::Quad: return "Quad";
    case Bs2127FacetKind::VirtualHullFacet: return "VirtualHullFacet";
    case Bs2127FacetKind::UnsupportedNgon: return "UnsupportedNgon";
    }
    return "Unknown";
}

Bs2127TopologyCatalog buildBs2127Topology(
    const std::array<UnitVector3, kBs2127TopologyStorageCount> &pointVectors,
    const std::array<bool, kBs2127TopologyStorageCount> &virtualFlags,
    std::size_t pointCount, const Bs2127TopologyOptions &options)
{
    Bs2127TopologyCatalog catalog;
    if (pointCount < 4U || pointCount > kBs2127TopologyStorageCount) {
        catalog.reason = "invalid-topology-point-count";
        return catalog;
    }
    catalog.pointCount = pointCount;
    catalog.pointVectors = pointVectors;
    catalog.virtualFlags = virtualFlags;
    catalog.options = options;
    if (!validOptions(options)) {
        catalog.reason = "invalid-topology-options";
        return catalog;
    }

    for (std::size_t first = 0U; first < pointCount; ++first) {
        for (std::size_t second = first + 1U;
             second < pointCount; ++second) {
            for (std::size_t third = second + 1U;
                 third < pointCount; ++third) {
                ++catalog.totalTriplesEnumerated;
                UnitVector3 normal;
                double offset = 0.0;
                std::vector<std::size_t> vertices;
                if (!deriveSupportingPlane(catalog.pointVectors, pointCount,
                                           first, second, third, options,
                                           &normal, &offset,
                                           &vertices)) {
                    ++catalog.nonSupportingTriplesRejected;
                    continue;
                }
                ++catalog.supportingPlanesFound;
                auto found = std::find_if(
                    catalog.facets.begin(), catalog.facets.end(),
                    [&normal, offset, &options](const Bs2127Facet &facet) {
                        return samePlane(facet, normal, offset,
                                          options.planeMergeTolerance);
                    });
                if (found != catalog.facets.end()) {
                    ++catalog.mergedPlaneDuplicates;
                    for (std::size_t vertex : vertices) {
                        if (std::find(found->vertexIndices.begin(),
                                      found->vertexIndices.end(), vertex)
                            == found->vertexIndices.end()) {
                            found->vertexIndices.push_back(vertex);
                        }
                    }
                    std::sort(found->vertexIndices.begin(),
                              found->vertexIndices.end());
                    continue;
                }

                Bs2127Facet facet;
                facet.vertexIndices = std::move(vertices);
                facet.outwardNormal = normal;
                facet.planeOffset = offset;
                facet.containsVirtual = std::any_of(
                    facet.vertexIndices.begin(), facet.vertexIndices.end(),
                    [&catalog](std::size_t index) {
                        return catalog.virtualFlags[index];
                    });
                for (std::size_t vertex : facet.vertexIndices) {
                    if (!catalog.virtualFlags[vertex]) {
                        facet.realSpeakerIndices.push_back(vertex);
                    }
                }
                if (facet.containsVirtual) {
                    facet.kind = facet.realSpeakerIndices.size() >= 2U
                        ? Bs2127FacetKind::VirtualHullFacet
                        : Bs2127FacetKind::UnsupportedNgon;
                } else if (facet.realSpeakerIndices.size() == 3U) {
                    facet.kind = Bs2127FacetKind::Triplet;
                } else if (facet.realSpeakerIndices.size() == 4U) {
                    facet.kind = Bs2127FacetKind::Quad;
                } else {
                    facet.kind = Bs2127FacetKind::UnsupportedNgon;
                }
                if (facet.vertexIndices.size() == 4U) {
                    facet.regionSpeakerIndices = quadPerimeterOrder(
                        facet.vertexIndices, catalog.pointVectors,
                        facet.outwardNormal);
                }
                if (facet.kind == Bs2127FacetKind::Triplet) {
                    const std::array<UnitVector3, 3> vectors {
                        catalog.pointVectors[facet.realSpeakerIndices[0]],
                        catalog.pointVectors[facet.realSpeakerIndices[1]],
                        catalog.pointVectors[facet.realSpeakerIndices[2]],
                    };
                    const TripletGeometryMetrics metrics =
                        measureTripletGeometry(vectors);
                    facet.determinant = metrics.determinant;
                    facet.conditionEstimate = metrics.conditionEstimate;
                    facet.tripletNumericallyUsable = metrics.finiteGeometry
                        && finite(metrics.determinant)
                        && std::abs(metrics.determinant)
                            > options.tripletOptions.determinantTolerance
                        && metrics.conditionEstimate
                            <= options.maximumConditionEstimate;
                    if (!facet.tripletNumericallyUsable) {
                        ++catalog.conditionRejected;
                    }
                }
                catalog.facets.push_back(std::move(facet));
            }
        }
    }

    std::sort(catalog.facets.begin(), catalog.facets.end(),
              lexicographicFacetOrder);
    catalog.valid = !catalog.facets.empty();
    catalog.reason = catalog.valid ? "none" : "no-supporting-system-h-facets";
    return catalog;
}

Bs2127TopologyCatalog buildSystemHBs2127Topology(
    const Bs2127TopologyOptions &options)
{
    std::array<UnitVector3, kBs2127TopologyStorageCount> pointVectors {};
    std::array<bool, kBs2127TopologyStorageCount> virtualFlags {};
    const auto &layout = Bs2051SystemHLayout::systemH();
    for (std::size_t index = 0U; index < layout.size(); ++index) {
        pointVectors[index] = layout[index].unitVector();
    }
    pointVectors[kBs2127VirtualBottomIndex] = {0.0, 0.0, -1.0};
    virtualFlags[kBs2127VirtualBottomIndex] = true;
    return buildBs2127Topology(pointVectors, virtualFlags,
                                kSystemHSpeakerCount + 1U, options);
}

const char *bs2127SelectionStatusName(Bs2127SelectionStatus status)
{
    switch (status) {
    case Bs2127SelectionStatus::Selected: return "Selected";
    case Bs2127SelectionStatus::Unsupported: return "Unsupported";
    case Bs2127SelectionStatus::InvalidInput: return "InvalidInput";
    }
    return "Unknown";
}

Bs2127PointSourceSelection selectSystemHPointSource(
    const Bs2127TopologyCatalog &catalog, const UnitVector3 &sourceVector)
{
    std::array<UnitVector3, kSystemHSpeakerCount> regionVectors {};
    for (std::size_t index = 0U; index < kSystemHSpeakerCount; ++index) {
        regionVectors[index] = catalog.pointVectors[index];
    }
    return selectSystemHPointSource(catalog, sourceVector,
                                    regionVectors);
}

Bs2127PointSourceSelection selectSystemHPointSource(
    const Bs2127TopologyCatalog &catalog, const UnitVector3 &sourceVector,
    const std::array<UnitVector3, kSystemHSpeakerCount> &regionVectors)
{
    Bs2127PointSourceSelection result;
    if (!catalog.valid) {
        setInvalid(&result, catalog.reason.empty()
                               ? "invalid-topology-catalog"
                               : catalog.reason.c_str());
        return result;
    }
    if (!finiteVector(sourceVector)) {
        setInvalid(&result, "nonfinite-source-vector");
        return result;
    }
    const double sourceNormSquared = dot(sourceVector, sourceVector);
    if (!finite(sourceNormSquared)
        || std::abs(sourceNormSquared - 1.0)
            > catalog.options.tripletOptions.unitVectorTolerance) {
        setInvalid(&result, "source-vector-not-unit-within-tolerance");
        return result;
    }

    if (catalog.options.identityGuardEnabled) {
        const double identityToleranceSquared =
            catalog.options.identityVectorTolerance
            * catalog.options.identityVectorTolerance;
        for (std::size_t speakerIndex = 0U;
             speakerIndex < kSystemHSpeakerCount; ++speakerIndex) {
            double distanceSquared = 0.0;
            for (std::size_t component = 0U; component < 3U; ++component) {
                const double delta = sourceVector[component]
                    - catalog.pointVectors[speakerIndex][component];
                distanceSquared += delta * delta;
            }
            if (distanceSquared <= identityToleranceSquared) {
                result.status = Bs2127SelectionStatus::Selected;
                result.reason = "speaker-identity";
                result.gains[speakerIndex] = 1.0;
                return result;
            }
        }
    }

    for (std::size_t facetIndex = 0U; facetIndex < catalog.facets.size();
         ++facetIndex) {
        const Bs2127Facet &facet = catalog.facets[facetIndex];
        if (facet.kind != Bs2127FacetKind::Triplet
            && facet.kind != Bs2127FacetKind::Quad) {
            continue;
        }
        ++result.facetsExamined;
        if (facet.kind == Bs2127FacetKind::Triplet) {
            if (!facet.tripletNumericallyUsable) {
                continue;
            }
            const std::array<UnitVector3, 3> vectors {
                regionVectors[facet.realSpeakerIndices[0]],
                regionVectors[facet.realSpeakerIndices[1]],
                regionVectors[facet.realSpeakerIndices[2]],
            };
            const TripletSolveResult solve = solveTriplet(
                vectors, sourceVector, catalog.options.tripletOptions);
            if (!solve.accepted) {
                continue;
            }
            result.status = Bs2127SelectionStatus::Selected;
            result.reason = "triplet-selected-first-valid-topology-order";
            result.selectedFacetIndex = facetIndex;
            result.selectedKind = facet.kind;
            result.conditionEstimate = facet.conditionEstimate;
            for (std::size_t index = 0U; index < 3U; ++index) {
                result.tripletGains[index] = solve.normalizedGains[index];
                result.gains[facet.realSpeakerIndices[index]] =
                    solve.normalizedGains[index];
            }
            return result;
        }

        if (facet.regionSpeakerIndices.size() != 4U) {
            continue;
        }
        std::array<UnitVector3, 4> vectors {};
        for (std::size_t index = 0U; index < 4U; ++index) {
            vectors[index] = regionVectors[facet.regionSpeakerIndices[index]];
        }
        const QuadSolveResult solve = solveQuad(
            vectors, sourceVector, catalog.options.quadOptions);
        if (!solve.accepted) {
            continue;
        }
        result.status = Bs2127SelectionStatus::Selected;
        result.reason = "quad-selected-first-valid-topology-order";
        result.selectedFacetIndex = facetIndex;
        result.selectedKind = facet.kind;
        for (std::size_t index = 0U; index < 4U; ++index) {
            result.quadGains[index] = solve.normalizedGains[index];
            result.gains[facet.regionSpeakerIndices[index]] =
                solve.normalizedGains[index];
        }
        return result;
    }

    result.status = Bs2127SelectionStatus::Unsupported;
    result.reason = "no-valid-triplet-quad-or-virtual-ngon-unsupported";
    return result;
}

} // namespace eac3render
