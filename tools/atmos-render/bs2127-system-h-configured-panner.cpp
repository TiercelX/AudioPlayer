#include "bs2127-system-h-configured-panner.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace eac3render {
namespace {

constexpr double kPi = 3.1415926535897932384626433832795;
constexpr std::array<std::size_t, kBs2127SystemHDirectVirtualCount>
    kDirectTargets {{3U, 4U, 7U, 8U, 9U}};

bool finite(double value)
{
    return std::isfinite(value);
}

bool finiteVector(const UnitVector3 &vector)
{
    return std::all_of(vector.begin(), vector.end(),
                       [](double value) { return finite(value); });
}

double dot(const UnitVector3 &left, const UnitVector3 &right)
{
    return left[0] * right[0] + left[1] * right[1]
        + left[2] * right[2];
}

double power(const std::array<double, kSystemHSpeakerCount> &gains)
{
    double result = 0.0;
    for (double gain : gains) {
        result += gain * gain;
    }
    return result;
}

UnitVector3 direction(double azimuthDegrees, double elevationDegrees)
{
    const double azimuth = azimuthDegrees * kPi / 180.0;
    const double elevation = elevationDegrees * kPi / 180.0;
    const double horizontal = std::cos(elevation);
    return {horizontal * std::cos(azimuth), horizontal * std::sin(azimuth),
            std::sin(elevation)};
}

double azimuthDegrees(const UnitVector3 &vector)
{
    return std::atan2(vector[1], vector[0]) * 180.0 / kPi;
}

double elevationDegrees(const UnitVector3 &vector)
{
    return std::atan2(vector[2], std::hypot(vector[0], vector[1]))
        * 180.0 / kPi;
}

bool samePhysicalVectors(
    const std::array<UnitVector3, kSystemHSpeakerCount> &left,
    const std::array<UnitVector3, kSystemHSpeakerCount> &right)
{
    return std::equal(left.begin(), left.end(), right.begin());
}

bool isVirtual(const Bs2127TopologyCatalog &catalog, std::size_t index)
{
    return index < catalog.pointCount && catalog.virtualFlags[index];
}

void mapGain(std::array<double, kSystemHSpeakerCount> *gains,
             std::size_t topologyIndex, double value,
             const std::array<Bs2127SystemHDirectVirtual,
                              kBs2127SystemHDirectVirtualCount> &directVirtuals)
{
    if (topologyIndex < kSystemHSpeakerCount) {
        (*gains)[topologyIndex] += value;
        return;
    }
    for (const auto &direct : directVirtuals) {
        if (direct.topologyIndex == topologyIndex) {
            (*gains)[direct.targetSpeakerIndex] += value;
            return;
        }
    }
}

bool finiteAndNormalized(
    const std::array<double, kSystemHSpeakerCount> &gains)
{
    for (double gain : gains) {
        if (!finite(gain)) {
            return false;
        }
    }
    return std::abs(power(gains) - 1.0) <= 1.0e-8;
}

bool normalizeGains(std::array<double, kSystemHSpeakerCount> *gains)
{
    const double value = power(*gains);
    if (!finite(value) || value <= std::numeric_limits<double>::min()) {
        return false;
    }
    const double inverse = 1.0 / std::sqrt(value);
    for (double &gain : *gains) {
        gain *= inverse;
    }
    return finiteAndNormalized(*gains);
}

} // namespace

Bs2127SystemHConfiguredPanner::Bs2127SystemHConfiguredPanner(
    const Bs2127TopologyOptions &options)
    : Bs2127SystemHConfiguredPanner(
          std::vector<Bs2127RealLoudspeaker> {}, options)
{
}

Bs2127SystemHConfiguredPanner::Bs2127SystemHConfiguredPanner(
    const std::vector<Bs2127RealLoudspeaker> &speakers,
    const Bs2127TopologyOptions &options)
    : layout_(makeSystemHRealLayout(speakers))
{
    const auto &nominal = Bs2051SystemHLayout::systemH();
    for (std::size_t index = 0U; index < kSystemHSpeakerCount; ++index) {
        realVectors_[index] = layout_.realVectors[index];
    }
    std::array<UnitVector3, kBs2127TopologyStorageCount> points {};
    std::array<bool, kBs2127TopologyStorageCount> virtualFlags {};
    for (std::size_t index = 0U; index < kSystemHSpeakerCount; ++index) {
        points[index] = nominal[index].unitVector();
    }
    const double lowerElevation = [&]() {
        double sum = 0.0;
        for (Bs2051Label label : {Bs2051Label::BPlus000,
                                  Bs2051Label::BPlus045,
                                  Bs2051Label::BMinus045}) {
            Bs2051SpeakerPosition position;
            if (!Bs2051SystemHLayout::find(label, &position)) {
                return std::numeric_limits<double>::quiet_NaN();
            }
            const std::size_t index = static_cast<std::size_t>(
                std::distance(nominal.begin(), std::find_if(
                    nominal.begin(), nominal.end(), [label](const auto &item) {
                        return item.label == label;
                    })));
            sum += elevationDegrees(realVectors_[index]);
        }
        return sum / 3.0;
    }();
    for (std::size_t directIndex = 0U;
         directIndex < kBs2127SystemHDirectVirtualCount; ++directIndex) {
        const std::size_t target = kDirectTargets[directIndex];
        const std::size_t topologyIndex = kSystemHSpeakerCount + directIndex;
        directVirtuals_[directIndex].topologyIndex = topologyIndex;
        directVirtuals_[directIndex].targetSpeakerIndex = target;
        directVirtuals_[directIndex].sourceLabel = nominal[target].label;
        directVirtuals_[directIndex].nominalVector = direction(
            nominal[target].azimuthDegrees, -30.0);
        directVirtuals_[directIndex].realVector = direction(
            azimuthDegrees(realVectors_[target]), lowerElevation);
        points[topologyIndex] = directVirtuals_[directIndex].nominalVector;
        virtualFlags[topologyIndex] = true;
    }
    points[kBs2127SystemHConfiguredLowerIndex] = {0.0, 0.0, -1.0};
    virtualFlags[kBs2127SystemHConfiguredLowerIndex] = true;
    catalog_ = buildBs2127Topology(
        points, virtualFlags, kBs2127SystemHConfiguredPointCount, options);
    nominalIdentityGuardEnabled_ = samePhysicalVectors(
        realVectors_, [&]() {
            std::array<UnitVector3, kSystemHSpeakerCount> values {};
            for (std::size_t index = 0U; index < kSystemHSpeakerCount; ++index) {
                values[index] = nominal[index].unitVector();
            }
            return values;
        }());

    std::vector<std::size_t> ring;
    for (const auto &facet : catalog_.facets) {
        if (std::find(facet.vertexIndices.begin(), facet.vertexIndices.end(),
                      kBs2127SystemHConfiguredLowerIndex)
            == facet.vertexIndices.end()) {
            continue;
        }
        for (std::size_t point : facet.vertexIndices) {
            if (point != kBs2127SystemHConfiguredLowerIndex
                && std::find(ring.begin(), ring.end(), point) == ring.end()) {
                ring.push_back(point);
            }
        }
    }
    std::sort(ring.begin(), ring.end(), [this](std::size_t left,
                                               std::size_t right) {
        const double leftAngle = azimuthDegrees(catalog_.pointVectors[left]);
        const double rightAngle = azimuthDegrees(catalog_.pointVectors[right]);
        if (leftAngle != rightAngle) {
            return leftAngle < rightAngle;
        }
        return left < right;
    });
    lowerRingPointIndices_ = std::move(ring);
    if (!layout_.valid) {
        reason_ = layout_.reason;
    } else if (!catalog_.valid) {
        reason_ = catalog_.reason;
    } else if (lowerRingPointIndices_.size() != 8U) {
        reason_ = "configured-lower-virtual-ring-size-mismatch";
    } else if (!finite(lowerElevation)) {
        reason_ = "configured-lower-real-elevation-invalid";
    } else {
        reason_ = "none";
    }
}

bool Bs2127SystemHConfiguredPanner::valid() const
{
    return reason_ == "none";
}

const std::string &Bs2127SystemHConfiguredPanner::reason() const
{
    return reason_;
}

const Bs2127SystemHRealLayout &
Bs2127SystemHConfiguredPanner::layout() const
{
    return layout_;
}

const Bs2127TopologyCatalog &Bs2127SystemHConfiguredPanner::catalog() const
{
    return catalog_;
}

const std::array<Bs2127SystemHDirectVirtual,
                 kBs2127SystemHDirectVirtualCount> &
Bs2127SystemHConfiguredPanner::directVirtuals() const
{
    return directVirtuals_;
}

const std::vector<std::size_t> &
Bs2127SystemHConfiguredPanner::lowerRingPointIndices() const
{
    return lowerRingPointIndices_;
}

Bs2127PointSourcePannerResult Bs2127SystemHConfiguredPanner::render(
    const UnitVector3 &sourceVector) const
{
    Bs2127PointSourcePannerResult result;
    if (!valid()) {
        result.status = Bs2127PointSourcePannerStatus::Unsupported;
        result.reason = reason_;
        return result;
    }
    if (!finiteVector(sourceVector)) {
        result.status = Bs2127PointSourcePannerStatus::InvalidInput;
        result.reason = "nonfinite-source-vector";
        return result;
    }
    const double sourcePower = dot(sourceVector, sourceVector);
    if (!finite(sourcePower)
        || std::abs(sourcePower - 1.0)
            > catalog_.options.tripletOptions.unitVectorTolerance) {
        result.status = Bs2127PointSourcePannerStatus::InvalidInput;
        result.reason = "source-vector-not-unit-within-tolerance";
        return result;
    }

    const UnitVector3 lowerVector {0.0, 0.0, -1.0};
    std::array<UnitVector3, kBs2127TopologyStorageCount> realPoints =
        catalog_.pointVectors;
    for (std::size_t index = 0U; index < kSystemHSpeakerCount; ++index) {
        realPoints[index] = realVectors_[index];
    }
    for (const auto &direct : directVirtuals_) {
        realPoints[direct.topologyIndex] = direct.realVector;
    }
    realPoints[kBs2127SystemHConfiguredLowerIndex] = lowerVector;

    // Keep the strict nominal physical identity fast path, but otherwise
    // dispatch every ordinary (non-lower-pole) facet through one stable
    // catalog-order loop.  Direct-downmix virtuals are ordinary topology
    // points here; their solved gains are mapped one-to-one below.
    if (catalog_.options.identityGuardEnabled && nominalIdentityGuardEnabled_) {
        const double tolerance = catalog_.options.identityVectorTolerance;
        const double toleranceSquared = tolerance * tolerance;
        for (std::size_t speaker = 0U; speaker < kSystemHSpeakerCount;
             ++speaker) {
            const UnitVector3 delta {
                sourceVector[0] - catalog_.pointVectors[speaker][0],
                sourceVector[1] - catalog_.pointVectors[speaker][1],
                sourceVector[2] - catalog_.pointVectors[speaker][2],
            };
            if (dot(delta, delta) <= toleranceSquared) {
                result.status = Bs2127PointSourcePannerStatus::Selected;
                result.reason = "speaker-identity";
                result.region = Bs2127PointSourceRegionKind::Identity;
                result.gains.fill(0.0);
                result.gains[speaker] = 1.0;
                return result;
            }
        }
    }

    for (std::size_t facetIndex = 0U; facetIndex < catalog_.facets.size();
         ++facetIndex) {
        const auto &facet = catalog_.facets[facetIndex];
        if (facet.kind != Bs2127FacetKind::Triplet
            && facet.kind != Bs2127FacetKind::Quad
            && facet.kind != Bs2127FacetKind::VirtualHullFacet) {
            continue;
        }
        if (std::find(facet.vertexIndices.begin(), facet.vertexIndices.end(),
                      kBs2127SystemHConfiguredLowerIndex)
            != facet.vertexIndices.end()
            || (facet.vertexIndices.size() != 3U
                && facet.vertexIndices.size() != 4U)) {
            continue;
        }
        ++result.regionsExamined;
        std::array<double, kSystemHSpeakerCount> gains {};
        bool accepted = false;
        Bs2127PointSourceRegionKind regionKind =
            Bs2127PointSourceRegionKind::Triplet;
        if (facet.vertexIndices.size() == 3U) {
            std::array<UnitVector3, 3> vectors {
                realPoints[facet.vertexIndices[0]],
                realPoints[facet.vertexIndices[1]],
                realPoints[facet.vertexIndices[2]],
            };
            const auto solve = solveTriplet(
                vectors, sourceVector, catalog_.options.tripletOptions);
            if (solve.accepted) {
                for (std::size_t index = 0U; index < vectors.size(); ++index) {
                    mapGain(&gains, facet.vertexIndices[index],
                            solve.normalizedGains[index], directVirtuals_);
                }
                accepted = true;
            }
        } else if (facet.vertexIndices.size() == 4U
                   && facet.regionSpeakerIndices.size() == 4U) {
            std::array<UnitVector3, 4> vectors {};
            for (std::size_t index = 0U; index < vectors.size(); ++index) {
                vectors[index] = realPoints[facet.regionSpeakerIndices[index]];
            }
            const auto solve = solveQuad(
                vectors, sourceVector, catalog_.options.quadOptions);
            if (solve.accepted) {
                for (std::size_t index = 0U; index < vectors.size(); ++index) {
                    mapGain(&gains, facet.regionSpeakerIndices[index],
                            solve.normalizedGains[index], directVirtuals_);
                }
                accepted = true;
                regionKind = Bs2127PointSourceRegionKind::Quad;
            }
        }
        if (!accepted || !normalizeGains(&gains)) {
            continue;
        }
        result.status = Bs2127PointSourcePannerStatus::Selected;
        result.reason = "configured-system-h-first-valid-ordinary-facet";
        result.region = regionKind;
        result.selectedFacetIndex = facetIndex;
        result.gains = gains;
        return result;
    }

    const double downmix = 1.0 / std::sqrt(
        static_cast<double>(lowerRingPointIndices_.size()));
    for (std::size_t edge = 0U; edge < lowerRingPointIndices_.size(); ++edge) {
        const std::size_t first = lowerRingPointIndices_[edge];
        const std::size_t second = lowerRingPointIndices_[
            (edge + 1U) % lowerRingPointIndices_.size()];
        const std::array<UnitVector3, 3> vectors {
            realPoints[kBs2127SystemHConfiguredLowerIndex], realPoints[first],
            realPoints[second]};
        const auto solve = solveTriplet(
            vectors, sourceVector, catalog_.options.tripletOptions);
        ++result.regionsExamined;
        if (!solve.accepted) {
            continue;
        }
        result.gains.fill(0.0);
        mapGain(&result.gains, first, solve.normalizedGains[1],
                directVirtuals_);
        mapGain(&result.gains, second, solve.normalizedGains[2],
                directVirtuals_);
        for (std::size_t point : lowerRingPointIndices_) {
            mapGain(&result.gains, point,
                    solve.normalizedGains[0] * downmix, directVirtuals_);
        }
        double preNormalizationPower = power(result.gains);
        if (!finite(preNormalizationPower)
            || preNormalizationPower <= std::numeric_limits<double>::min()) {
            continue;
        }
        const double inversePower = 1.0 / std::sqrt(preNormalizationPower);
        for (double &gain : result.gains) {
            gain *= inversePower;
        }
        if (!finiteAndNormalized(result.gains)) {
            result.status = Bs2127PointSourcePannerStatus::Unsupported;
            result.reason = "virtual-ngon-produced-invalid-gain-vector";
            result.gains.fill(0.0);
            return result;
        }
        result.status = Bs2127PointSourcePannerStatus::Selected;
        result.reason = "configured-lower-virtual-ngon-first-valid";
        result.region = Bs2127PointSourceRegionKind::VirtualNgon;
        result.selectedTriangleIndex = edge;
        return result;
    }
    result.status = Bs2127PointSourcePannerStatus::Unsupported;
    result.reason = "no-valid-configured-system-h-region";
    return result;
}

} // namespace eac3render
