#pragma once

#include "brir-convolver.h"
#include "system-h-planar-mixer.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace eac3render {

struct I0pOfflinePaths {
  std::string stereoWav;
  std::string lfeWav;
  std::string reportJson;
};

struct I0pSignalMetrics {
  std::uint64_t frames = 0U;
  std::uint64_t samples = 0U;
  double peak = 0.0;
  double rms = 0.0;
  std::uint64_t digest = 0U;
};

struct I0pOfflineResult {
  bool accepted = false;
  std::string reason;
  std::uint64_t sourceFrames = 0U;
  std::uint64_t stereoFrames = 0U;
  std::uint64_t lfeFrames = 0U;
  std::uint64_t tailFrames = 0U;
  I0pSignalMetrics speakerBus;
  I0pSignalMetrics stereo;
  I0pSignalMetrics lfe;
  // E.2.9 is not active for this I0 contract: it emits a 22-speaker BRIR
  // bus plus a separate LFE sideband, with no proven 1/0 or 2/0 output mode
  // and no LFE-output-disabled selection. Keep the decision explicit.
  std::string lfeDownmixPolicy = "NO_MIX";
  std::string lfeDownmixReason =
      "output-mode-or-lfe-disabled-not-proven";
};

class I0pOfflineOutput final {
public:
  I0pOfflineResult init(const SofaBrirCache &cache, std::uint64_t generation,
                        std::size_t blockSize = 1024U,
                        std::size_t irLength = kR2aBrirIrLength);
  I0pOfflineResult append(const SystemHPlanarMixerBatch &batch);
  I0pOfflineResult finish(const I0pOfflinePaths &paths);
  I0pOfflineResult reset(std::uint64_t generation);

private:
  I0pOfflineResult fail(const char *reason) const;

  SofaBrirCache cache_;
  BrirConvolverStream stream_;
  std::vector<float> lfe_;
  std::uint64_t generation_ = 0U;
  std::int64_t nextSample_ = 0;
  std::size_t blockSize_ = 0U;
  std::size_t irLength_ = 0U;
  std::uint64_t speakerSamples_ = 0U;
  std::uint64_t speakerDigest_ = 0U;
  long double speakerSquareSum_ = 0.0L;
  double speakerPeak_ = 0.0;
  bool initialized_ = false;
  bool finished_ = false;
};

bool i0pOfflineSelfTest(std::string *reason);

} // namespace eac3render
