#include "brir-convolver.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {
constexpr std::size_t kChannels = 22U;
constexpr std::size_t kDefaultBlock = 1024U;

struct Args {
  bool selfTest = false;
  std::string cache;
  std::string input;
  std::string output;
  std::string expectedHash;
  std::size_t block = kDefaultBlock;
};

void usage() {
  std::cerr << "usage: Eac3SystemHBrirOffline --cache cache --input raw-f32-22ch "
               "--output raw-f32-stereo [--expected-cache-sha256 hex] "
               "[--block n]\n"
               "       Eac3SystemHBrirOffline --self-test --cache cache\n";
}

bool parseSize(const std::string &text, std::size_t *value) {
  if (!value || text.empty()) return false;
  try {
    std::size_t used = 0U;
    const auto parsed = std::stoull(text, &used);
    if (used != text.size() || parsed == 0U || parsed > (1ULL << 20U))
      return false;
    *value = static_cast<std::size_t>(parsed);
    return true;
  } catch (...) {
    return false;
  }
}

bool parseArgs(int argc, char **argv, Args *args) {
  if (!args) return false;
  for (int i = 1; i < argc; ++i) {
    const std::string key = argv[i];
    if (key == "--self-test") {
      args->selfTest = true;
    } else if (key == "--cache" || key == "--input" || key == "--output" ||
               key == "--expected-cache-sha256" || key == "--block") {
      if (++i >= argc) return false;
      const std::string value = argv[i];
      if (key == "--cache") args->cache = value;
      else if (key == "--input") args->input = value;
      else if (key == "--output") args->output = value;
      else if (key == "--expected-cache-sha256") args->expectedHash = value;
      else if (!parseSize(value, &args->block)) return false;
    } else {
      return false;
    }
  }
  if (args->cache.empty()) return false;
  if (args->selfTest)
    return args->input.empty() && args->output.empty();
  return !args->input.empty() && !args->output.empty();
}

bool validRawByteCount(std::uintmax_t bytes) {
  return bytes != 0U && bytes % (kChannels * sizeof(float)) == 0U;
}

bool finiteChannels(const std::array<std::vector<float>, kChannels> &channels) {
  for (const auto &channel : channels)
    for (float sample : channel)
      if (!std::isfinite(sample)) return false;
  return true;
}

int runSelfTest(const Args &args) {
  const auto loaded = eac3render::loadSofaBrirCache(args.cache, args.expectedHash);
  if (!loaded.accepted) {
    std::cerr << "cache-load=" << loaded.reason << "\n";
    return 1;
  }
  int failures = 0;
  auto check = [&failures](bool ok, const char *name) {
    std::cout << (ok ? "PASS " : "FAIL ") << name << "\n";
    if (!ok) ++failures;
  };

  check(!validRawByteCount(kChannels * sizeof(float) - 1U),
        "truncated-interleaved-input-rejected");

  eac3render::BrirConvolverStream stream;
  check(stream.init(loaded.cache, kDefaultBlock, 16384U).accepted,
        "cache-and-stream-init");
  std::array<std::vector<float>, kChannels> input;
  std::array<const float *, kChannels> pointers{};
  for (auto &channel : input) channel.assign(1U, 0.0F);
  input[2][0] = 1.0F; // M+000, the third documented System H channel.
  for (std::size_t channel = 0U; channel < kChannels; ++channel)
    pointers[channel] = input[channel].data();
  const auto appended = stream.append(pointers, 1U);
  const auto finished = stream.finish();
  bool channelOrder = appended.accepted && finished.accepted &&
                      stream.output()[0].size() == 16384U;
  if (channelOrder) {
    for (std::size_t i = 0U; i < 16384U; ++i) {
      channelOrder &= std::abs(stream.output()[0][i] -
                               loaded.cache.ir[0][2U * 16384U + i]) < 1.0e-5F;
      channelOrder &= std::abs(stream.output()[1][i] -
                               loaded.cache.ir[1][2U * 16384U + i]) < 1.0e-5F;
    }
  }
  check(channelOrder, "22ch-order-and-exact-tail-length");

  eac3render::BrirConvolver core;
  const auto init = core.init(loaded.cache, kDefaultBlock, 16384U);
  std::array<float, 1> nan{std::numeric_limits<float>::quiet_NaN()};
  std::array<const float *, kChannels> badPointers{};
  badPointers[0] = nan.data();
  std::array<float, 1> left{}, right{};
  const auto rejected = core.processFixedBlock(
      badPointers, {left.data(), right.data()});
  check(init.accepted && !rejected.accepted &&
            rejected.reason == "nonfinite-input",
        "nonfinite-input-rejected");

  std::cout << "SystemHBrirOfflineSelfTest="
            << (failures == 0 ? "PASS" : "FAIL") << " cases=4 failures="
            << failures << "\n";
  return failures == 0 ? 0 : 1;
}

int runRender(const Args &args) {
  const auto loaded = eac3render::loadSofaBrirCache(args.cache, args.expectedHash);
  if (!loaded.accepted) {
    std::cerr << "cache-load=" << loaded.reason << "\n";
    return 1;
  }
  if (loaded.cache.sampleRate != 48000U) {
    std::cerr << "cache-sample-rate=" << loaded.cache.sampleRate
              << " (expected 48000)\n";
    return 1;
  }
  std::ifstream inputFile(args.input, std::ios::binary | std::ios::ate);
  if (!inputFile) {
    std::cerr << "input-open-failed\n";
    return 1;
  }
  const auto inputSize = inputFile.tellg();
  if (inputSize < 0 || !validRawByteCount(static_cast<std::uintmax_t>(inputSize))) {
    std::cerr << "input-size-invalid (expected non-empty interleaved 22ch f32)\n";
    return 1;
  }
  const std::uintmax_t frames = static_cast<std::uintmax_t>(inputSize) /
                                (kChannels * sizeof(float));
  inputFile.seekg(0);
  eac3render::BrirConvolverStream stream;
  const auto initialized = stream.init(loaded.cache, args.block, 16384U);
  if (!initialized.accepted) {
    std::cerr << "stream-init=" << initialized.reason << "\n";
    return 1;
  }

  const std::filesystem::path partial =
      std::filesystem::path(args.output).concat(".partial");
  std::error_code ec;
  std::filesystem::remove(partial, ec);
  std::array<std::vector<float>, kChannels> channels;
  std::vector<float> interleaved(args.block * kChannels);
  std::uintmax_t consumed = 0U;
  while (consumed < frames) {
    const std::size_t count = static_cast<std::size_t>(
        std::min<std::uintmax_t>(args.block, frames - consumed));
    inputFile.read(reinterpret_cast<char *>(interleaved.data()),
                   static_cast<std::streamsize>(count * kChannels * sizeof(float)));
    if (inputFile.gcount() !=
        static_cast<std::streamsize>(count * kChannels * sizeof(float))) {
      std::cerr << "input-read-truncated\n";
      return 1;
    }
    for (auto &channel : channels) channel.resize(count);
    for (std::size_t frame = 0U; frame < count; ++frame)
      for (std::size_t channel = 0U; channel < kChannels; ++channel)
        channels[channel][frame] = interleaved[frame * kChannels + channel];
    if (!finiteChannels(channels)) {
      std::cerr << "nonfinite-input\n";
      return 1;
    }
    std::array<const float *, kChannels> pointers{};
    for (std::size_t channel = 0U; channel < kChannels; ++channel)
      pointers[channel] = channels[channel].data();
    const auto appended = stream.append(pointers, count);
    if (!appended.accepted) {
      std::cerr << "stream-append=" << appended.reason << "\n";
      return 1;
    }
    consumed += count;
  }
  const auto finished = stream.finish();
  if (!finished.accepted) {
    std::cerr << "stream-finish=" << finished.reason << "\n";
    return 1;
  }
  const auto &output = stream.output();
  if (output[0].size() != frames + 16384U - 1U ||
      output[1].size() != output[0].size()) {
    std::cerr << "output-size-invalid\n";
    return 1;
  }
  std::ofstream outputFile(partial, std::ios::binary | std::ios::trunc);
  if (!outputFile) {
    std::cerr << "output-open-failed\n";
    return 1;
  }
  for (std::size_t frame = 0U; frame < output[0].size(); ++frame) {
    const float pair[2] = {output[0][frame], output[1][frame]};
    if (!std::isfinite(pair[0]) || !std::isfinite(pair[1])) {
      outputFile.close();
      std::filesystem::remove(partial, ec);
      std::cerr << "nonfinite-output\n";
      return 1;
    }
    outputFile.write(reinterpret_cast<const char *>(pair), sizeof(pair));
    if (!outputFile) {
      outputFile.close();
      std::filesystem::remove(partial, ec);
      std::cerr << "output-write-failed\n";
      return 1;
    }
  }
  outputFile.close();
  if (std::filesystem::exists(args.output, ec)) {
    std::filesystem::remove(partial, ec);
    std::cerr << "output-exists (refusing overwrite)\n";
    return 1;
  }
  std::filesystem::rename(partial, args.output, ec);
  if (ec) {
    std::filesystem::remove(partial, ec);
    std::cerr << "output-rename-failed\n";
    return 1;
  }
  std::cout << "accepted=true inputFrames=" << frames
            << " outputFrames=" << output[0].size()
            << " tailFrames=16383 block=" << args.block << "\n";
  return 0;
}
} // namespace

int main(int argc, char **argv) {
  Args args;
  if (!parseArgs(argc, argv, &args)) {
    usage();
    return 2;
  }
  return args.selfTest ? runSelfTest(args) : runRender(args);
}
