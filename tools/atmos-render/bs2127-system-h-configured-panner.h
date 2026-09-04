#pragma once

#include "bs2127-nominal-to-actual.h"

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace eac3render {

constexpr std::size_t kBs2127SystemHDirectVirtualCount = 5U;
constexpr std::size_t kBs2127SystemHConfiguredPointCount =
    kSystemHSpeakerCount + kBs2127SystemHDirectVirtualCount + 1U;
constexpr std::size_t kBs2127SystemHConfiguredLowerIndex =
    kBs2127SystemHConfiguredPointCount - 1U;

struct Bs2127SystemHDirectVirtual {
    std::size_t topologyIndex = 0U;
    std::size_t targetSpeakerIndex = 0U;
    Bs2051Label sourceLabel = Bs2051Label::MPlus000;
    UnitVector3 nominalVector {0.0, 0.0, 0.0};
    UnitVector3 realVector {0.0, 0.0, 0.0};
};

// R0D fixed System H configuration process.  This keeps the legacy 23-point
// diagnostic catalog intact while adding the five §6.1.3.1.1 direct-downmix
// virtual points to a separate 28-point nominal hull.
class Bs2127SystemHConfiguredPanner {
public:
    explicit Bs2127SystemHConfiguredPanner(
        const Bs2127TopologyOptions &options = {});
    explicit Bs2127SystemHConfiguredPanner(
        const std::vector<Bs2127RealLoudspeaker> &speakers,
        const Bs2127TopologyOptions &options = {});

    bool valid() const;
    const std::string &reason() const;
    const Bs2127SystemHRealLayout &layout() const;
    const Bs2127TopologyCatalog &catalog() const;
    const std::array<Bs2127SystemHDirectVirtual,
                     kBs2127SystemHDirectVirtualCount> &directVirtuals() const;
    const std::vector<std::size_t> &lowerRingPointIndices() const;

    Bs2127PointSourcePannerResult render(
        const UnitVector3 &sourceVector) const;

private:
    Bs2127SystemHRealLayout layout_;
    Bs2127TopologyCatalog catalog_;
    std::array<UnitVector3, kSystemHSpeakerCount> realVectors_ {};
    std::array<Bs2127SystemHDirectVirtual,
               kBs2127SystemHDirectVirtualCount> directVirtuals_ {};
    std::vector<std::size_t> lowerRingPointIndices_;
    bool nominalIdentityGuardEnabled_ = true;
    std::string reason_;
};

} // namespace eac3render
