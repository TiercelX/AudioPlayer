#pragma once

#include "bs2127-point-source-panner.h"

#include <array>
#include <string>
#include <vector>

namespace eac3render {

// A real loudspeaker record is keyed by the stable BS.2051 label rather than
// by the implementation's array order.  This mirrors BS.2127-1 §6.1.3.1's
// separate nominal and real position lists while keeping the current seam
// deliberately limited to System H.
struct Bs2127RealLoudspeaker {
    Bs2051Label label = Bs2051Label::MPlus000;
    UnitVector3 unitVector {0.0, 0.0, 0.0};
};

struct Bs2127SystemHRealLayout {
    std::array<UnitVector3, kSystemHSpeakerCount> realVectors {};
    bool valid = false;
    std::string reason;
};

// Empty input selects the nominal System H positions.  Non-empty input must
// contain exactly one valid real position for each of the 22 labels.  The
// helper is intentionally strict: it does not infer missing speakers or
// accept a generic/non-System-H layout.
Bs2127SystemHRealLayout makeSystemHRealLayout(
    const std::vector<Bs2127RealLoudspeaker> &speakers);

class Bs2127SystemHNominalToActualPanner {
public:
    explicit Bs2127SystemHNominalToActualPanner(
        const Bs2127TopologyOptions &options = {});
    explicit Bs2127SystemHNominalToActualPanner(
        const std::vector<Bs2127RealLoudspeaker> &speakers,
        const Bs2127TopologyOptions &options = {});

    bool valid() const;
    const std::string &reason() const;
    const Bs2127SystemHRealLayout &layout() const;
    const Bs2127SystemHPointSourcePanner &nominalPanner() const;

    Bs2127PointSourcePannerResult render(
        const UnitVector3 &sourceVector) const;

private:
    Bs2127SystemHRealLayout layout_;
    Bs2127SystemHPointSourcePanner panner_;
    std::string reason_;
};

} // namespace eac3render
