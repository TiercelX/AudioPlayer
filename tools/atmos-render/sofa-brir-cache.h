#pragma once

#include "bs2051-layout.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace eac3render {

constexpr std::size_t kR2aBrirEmitterCount = kSystemHSpeakerCount;
constexpr std::size_t kR2aBrirReceiverCount = 2U;
constexpr std::size_t kR2aBrirIrLength = 16384U;

struct SofaBrirCache {
    std::uint32_t sampleRate = 0U;
    std::int32_t listenerViewIndex = -1;
    std::uint32_t leftReceiverIndex = 0U;
    std::uint32_t rightReceiverIndex = 0U;
    std::array<std::uint8_t, 32> sourceSha256 {};
    std::array<std::uint32_t, kSystemHSpeakerCount> systemToSofaEmitter {};
    std::array<std::array<double, kSystemHSpeakerCount>, 2U> delays {};
    std::array<std::vector<float>, 2U> ir;
};

struct SofaBrirCacheLoadResult {
    bool accepted = false;
    std::string reason;
    SofaBrirCache cache;
};

// expectedSha256Hex is optional; when supplied it must be 64 hex characters.
SofaBrirCacheLoadResult loadSofaBrirCache(
    const std::string &path, const std::string &expectedSha256Hex = {});

} // namespace eac3render
