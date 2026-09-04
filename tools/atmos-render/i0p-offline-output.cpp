#include "i0p-offline-output.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>

namespace eac3render {
namespace {

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

void hashFloat(std::uint64_t *digest, float value) {
  std::uint32_t bits = 0U;
  std::memcpy(&bits, &value, sizeof(bits));
  for (unsigned shift = 0U; shift < 32U; shift += 8U) {
    *digest ^= static_cast<std::uint8_t>((bits >> shift) & 0xffU);
    *digest *= kFnvPrime;
  }
}

I0pSignalMetrics metrics(const std::vector<float> &samples,
                         std::uint64_t frames) {
  I0pSignalMetrics result;
  result.frames = frames;
  result.samples = static_cast<std::uint64_t>(samples.size());
  result.digest = kFnvOffset;
  long double squareSum = 0.0L;
  for (float value : samples) {
    const double sample = static_cast<double>(value);
    result.peak = std::max(result.peak, std::abs(sample));
    squareSum += static_cast<long double>(sample) * sample;
    hashFloat(&result.digest, value);
  }
  if (!samples.empty())
    result.rms = std::sqrt(static_cast<double>(squareSum / samples.size()));
  return result;
}

void putU16(std::vector<std::uint8_t> *bytes, std::uint16_t value) {
  bytes->push_back(static_cast<std::uint8_t>(value & 0xffU));
  bytes->push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
}
void putU32(std::vector<std::uint8_t> *bytes, std::uint32_t value) {
  for (unsigned shift = 0U; shift < 32U; shift += 8U)
    bytes->push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
}

bool writeAtomic(const std::filesystem::path &path,
                 const std::vector<std::uint8_t> &bytes,
                 std::string *reason) {
  std::error_code ec;
  if (!path.has_filename()) {
    *reason = "i0p-output-path-invalid";
    return false;
  }
  if (path.has_parent_path())
    std::filesystem::create_directories(path.parent_path(), ec);
  if (ec) {
    *reason = "i0p-output-directory-failed";
    return false;
  }
  const auto temporary = path.string() + ".i0p-tmp";
  {
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file) {
      *reason = "i0p-output-open-failed";
      return false;
    }
    if (!bytes.empty())
      file.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    if (!file) {
      *reason = "i0p-output-write-failed";
      file.close();
      std::filesystem::remove(temporary, ec);
      return false;
    }
  }
  std::filesystem::remove(path, ec);
  ec.clear();
  std::filesystem::rename(temporary, path, ec);
  if (ec) {
    std::filesystem::remove(temporary, ec);
    *reason = "i0p-output-rename-failed";
    return false;
  }
  return true;
}

bool encodeFloatWav(const std::vector<float> &interleaved,
                    std::uint16_t channels, std::uint32_t sampleRate,
                    std::vector<std::uint8_t> *bytes, std::string *reason) {
  if (!bytes || !reason || channels == 0U || sampleRate == 0U ||
      interleaved.size() % channels != 0U ||
      interleaved.size() > (std::numeric_limits<std::uint32_t>::max() - 36U) /
                               sizeof(float)) {
    if (reason)
      *reason = "i0p-wav-shape-invalid";
    return false;
  }
  const auto dataBytes = static_cast<std::uint32_t>(interleaved.size() * 4U);
  bytes->clear();
  bytes->reserve(44U + dataBytes);
  bytes->insert(bytes->end(), {'R', 'I', 'F', 'F'});
  putU32(bytes, 36U + dataBytes);
  bytes->insert(bytes->end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  putU32(bytes, 16U);
  putU16(bytes, 3U); // WAVE_FORMAT_IEEE_FLOAT
  putU16(bytes, channels);
  putU32(bytes, sampleRate);
  putU32(bytes, sampleRate * channels * 4U);
  putU16(bytes, static_cast<std::uint16_t>(channels * 4U));
  putU16(bytes, 32U);
  bytes->insert(bytes->end(), {'d', 'a', 't', 'a'});
  putU32(bytes, dataBytes);
  for (float value : interleaved) {
    std::uint32_t bits = 0U;
    std::memcpy(&bits, &value, sizeof(bits));
    putU32(bytes, bits);
  }
  return true;
}

std::string hexBytes(const std::array<std::uint8_t, 32> &bytes) {
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (auto value : bytes)
    out << std::setw(2) << static_cast<unsigned>(value);
  return out.str();
}

std::string hexDigest(std::uint64_t value) {
  std::ostringstream out;
  out << "0x" << std::hex << std::setfill('0') << std::setw(16) << value;
  return out.str();
}

std::uint32_t readU32(const std::vector<std::uint8_t> &bytes,
                      std::size_t offset) {
  if (offset + 4U > bytes.size())
    return 0U;
  std::uint32_t value = 0U;
  for (unsigned i = 0; i < 4U; ++i)
    value |= static_cast<std::uint32_t>(bytes[offset + i]) << (8U * i);
  return value;
}

std::uint16_t readU16(const std::vector<std::uint8_t> &bytes,
                      std::size_t offset) {
  if (offset + 2U > bytes.size())
    return 0U;
  return static_cast<std::uint16_t>(bytes[offset]) |
         static_cast<std::uint16_t>(bytes[offset + 1U] << 8U);
}

bool tagAt(const std::vector<std::uint8_t> &bytes, std::size_t offset,
           const std::array<std::uint8_t, 4> &tag) {
  return offset + tag.size() <= bytes.size() &&
         std::equal(tag.begin(), tag.end(), bytes.begin() + offset);
}

std::vector<std::uint8_t> readAll(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file)
    return {};
  const auto size = file.tellg();
  if (size < 0)
    return {};
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  file.seekg(0);
  if (!bytes.empty())
    file.read(reinterpret_cast<char *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
  return file ? bytes : std::vector<std::uint8_t>{};
}

SofaBrirCache syntheticCache(std::size_t irLength) {
  SofaBrirCache cache;
  cache.sampleRate = 48000U;
  cache.listenerViewIndex = 90;
  cache.leftReceiverIndex = 0U;
  cache.rightReceiverIndex = 1U;
  for (std::size_t speaker = 0U; speaker < kSystemHSpeakerCount; ++speaker)
    cache.systemToSofaEmitter[speaker] = static_cast<std::uint32_t>(speaker);
  for (auto &ear : cache.ir)
    ear.assign(kSystemHSpeakerCount * irLength, 0.0F);
  cache.ir[0][0] = 1.0F;
  cache.ir[0][irLength - 1U] = 0.25F;
  cache.ir[1][0] = 0.5F;
  cache.ir[1][irLength - 1U] = -0.25F;
  return cache;
}

SystemHPlanarMixerBatch syntheticBatch(std::uint64_t generation,
                                       std::int64_t start,
                                       std::size_t frames) {
  SystemHPlanarMixerBatch batch;
  batch.generation = generation;
  batch.sampleStart = start;
  batch.sampleEnd = start + static_cast<std::int64_t>(frames);
  for (auto &speaker : batch.speakers)
    speaker.assign(frames, 0.0F);
  batch.lfeSideband.assign(frames, 0.0F);
  if (frames) {
    batch.speakers[0][0] = 1.0F;
    batch.lfeSideband[0] = -0.75F;
  }
  return batch;
}

} // namespace

I0pOfflineResult I0pOfflineOutput::fail(const char *reason) const {
  I0pOfflineResult result;
  result.reason = reason;
  return result;
}

I0pOfflineResult I0pOfflineOutput::init(const SofaBrirCache &cache,
                                        std::uint64_t generation,
                                        std::size_t blockSize,
                                        std::size_t irLength) {
  initialized_ = finished_ = false;
  lfe_.clear();
  nextSample_ = 0;
  speakerSamples_ = 0U;
  speakerDigest_ = kFnvOffset;
  speakerSquareSum_ = 0.0L;
  speakerPeak_ = 0.0;
  const auto initResult = stream_.init(cache, blockSize, irLength);
  if (!initResult.accepted) {
    I0pOfflineResult result;
    result.reason = "i0p-" + initResult.reason;
    return result;
  }
  cache_ = cache;
  generation_ = generation;
  blockSize_ = blockSize;
  irLength_ = irLength;
  initialized_ = true;
  I0pOfflineResult result;
  result.accepted = true;
  result.reason = "i0p-initialized";
  return result;
}

I0pOfflineResult I0pOfflineOutput::append(
    const SystemHPlanarMixerBatch &batch) {
  if (!initialized_)
    return fail("i0p-not-initialized");
  if (finished_)
    return fail("i0p-append-after-finish");
  if (batch.generation != generation_)
    return fail("i0p-generation-mismatch");
  if (batch.sampleStart != nextSample_ || batch.sampleEnd <= batch.sampleStart)
    return fail("i0p-timeline-discontinuity");
  const auto delta = static_cast<std::uint64_t>(batch.sampleEnd - batch.sampleStart);
  if (delta > std::numeric_limits<std::size_t>::max())
    return fail("i0p-frame-count-overflow");
  const auto frames = static_cast<std::size_t>(delta);
  for (const auto &speaker : batch.speakers) {
    if (speaker.size() != frames)
      return fail("i0p-speaker-shape-invalid");
    if (!std::all_of(speaker.begin(), speaker.end(),
                     [](float value) { return std::isfinite(value); }))
      return fail("i0p-nonfinite-speaker");
  }
  if (batch.lfePolicy != LfePolicy::SeparateFromPointLayout ||
      batch.lfeSideband.size() != frames)
    return fail("i0p-lfe-shape-or-policy-invalid");
  if (!std::all_of(batch.lfeSideband.begin(), batch.lfeSideband.end(),
                   [](float value) { return std::isfinite(value); }))
    return fail("i0p-nonfinite-lfe");
  if (lfe_.size() > std::numeric_limits<std::size_t>::max() - frames)
    return fail("i0p-frame-count-overflow");
  std::array<const float *, kSystemHSpeakerCount> pointers{};
  for (std::size_t speaker = 0U; speaker < pointers.size(); ++speaker)
    pointers[speaker] = batch.speakers[speaker].data();
  const auto appended = stream_.append(pointers, frames);
  if (!appended.accepted) {
    I0pOfflineResult result;
    result.reason = "i0p-" + appended.reason;
    return result;
  }
  for (const auto &speaker : batch.speakers) {
    for (float value : speaker) {
      const double sample = static_cast<double>(value);
      speakerPeak_ = std::max(speakerPeak_, std::abs(sample));
      speakerSquareSum_ += static_cast<long double>(sample) * sample;
      hashFloat(&speakerDigest_, value);
      ++speakerSamples_;
    }
  }
  lfe_.insert(lfe_.end(), batch.lfeSideband.begin(), batch.lfeSideband.end());
  nextSample_ = batch.sampleEnd;
  I0pOfflineResult result;
  result.accepted = true;
  result.reason = "i0p-batch-appended";
  result.sourceFrames = static_cast<std::uint64_t>(lfe_.size());
  return result;
}

I0pOfflineResult I0pOfflineOutput::finish(const I0pOfflinePaths &paths) {
  if (!initialized_)
    return fail("i0p-not-initialized");
  if (finished_)
    return fail("i0p-finish-after-finish");
  if (lfe_.empty())
    return fail("i0p-empty-input");
  if (paths.stereoWav.empty() || paths.lfeWav.empty() ||
      paths.reportJson.empty())
    return fail("i0p-output-path-invalid");
  const auto finishResult = stream_.finish();
  if (!finishResult.accepted) {
    I0pOfflineResult result;
    result.reason = "i0p-" + finishResult.reason;
    return result;
  }
  const auto &deinterleaved = stream_.output();
  if (deinterleaved[0].size() != deinterleaved[1].size() ||
      deinterleaved[0].size() != lfe_.size() + irLength_ - 1U)
    return fail("i0p-output-frame-count-invalid");
  std::vector<float> stereo;
  if (deinterleaved[0].size() > std::numeric_limits<std::size_t>::max() / 2U)
    return fail("i0p-output-frame-count-overflow");
  stereo.reserve(deinterleaved[0].size() * 2U);
  for (std::size_t frame = 0U; frame < deinterleaved[0].size(); ++frame) {
    const float left = deinterleaved[0][frame];
    const float right = deinterleaved[1][frame];
    if (!std::isfinite(left) || !std::isfinite(right))
      return fail("i0p-nonfinite-stereo");
    stereo.push_back(left);
    stereo.push_back(right);
  }
  I0pOfflineResult result;
  result.sourceFrames = static_cast<std::uint64_t>(lfe_.size());
  result.stereoFrames = static_cast<std::uint64_t>(deinterleaved[0].size());
  result.lfeFrames = result.sourceFrames;
  result.tailFrames = static_cast<std::uint64_t>(irLength_ - 1U);
  result.speakerBus.frames = result.sourceFrames;
  result.speakerBus.samples = speakerSamples_;
  result.speakerBus.peak = speakerPeak_;
  result.speakerBus.rms = speakerSamples_ == 0U ? 0.0
      : std::sqrt(static_cast<double>(speakerSquareSum_ / speakerSamples_));
  result.speakerBus.digest = speakerDigest_;
  result.stereo = metrics(stereo, result.stereoFrames);
  result.lfe = metrics(lfe_, result.lfeFrames);
  std::vector<std::uint8_t> stereoWav;
  std::vector<std::uint8_t> lfeWav;
  std::string reason;
  if (!encodeFloatWav(stereo, 2U, cache_.sampleRate, &stereoWav, &reason) ||
      !encodeFloatWav(lfe_, 1U, cache_.sampleRate, &lfeWav, &reason)) {
    result.reason = reason;
    return result;
  }
  std::ostringstream report;
  report << std::setprecision(17)
         << "{\n"
         << "  \"schema\": \"audioplayer.i0p-offline.v1\",\n"
         << "  \"i0p\": \"PASS\",\n"
         << "  \"productionAcceptance\": \"INCONCLUSIVE\",\n"
         << "  \"amplitudeOracle\": \"INCONCLUSIVE\",\n"
         << "  \"generation\": " << generation_ << ",\n"
         << "  \"sampleRate\": " << cache_.sampleRate << ",\n"
         << "  \"sourceFrames\": " << result.sourceFrames << ",\n"
         << "  \"stereoFrames\": " << result.stereoFrames << ",\n"
         << "  \"lfeFrames\": " << result.lfeFrames << ",\n"
         << "  \"tailFrames\": " << result.tailFrames << ",\n"
         << "  \"normalization\": \"NO\",\n"
         << "  \"drc\": \"NO\",\n"
         << "  \"limiter\": \"NO\",\n"
         << "  \"postgain\": \"NO\",\n"
         << "  \"lfePolicy\": \"ExcludedFromBinaural+SeparateStem\",\n"
         << "  \"lfeDownmixPolicy\": \"NO_MIX\",\n"
         << "  \"lfeDownmixReason\": \"output-mode-or-lfe-disabled-not-proven\",\n"
         << "  \"stereoFormat\": \"IEEE_FLOAT32_LE_INTERLEAVED\",\n"
         << "  \"lfeFormat\": \"IEEE_FLOAT32_LE_MONO\",\n"
         << "  \"speakerBus\": {\"channels\": 22, \"peak\": "
         << result.speakerBus.peak << ", \"rms\": " << result.speakerBus.rms
         << ", \"digest\": \"" << hexDigest(result.speakerBus.digest)
         << "\"},\n"
         << "  \"stereo\": {\"peak\": " << result.stereo.peak
         << ", \"rms\": " << result.stereo.rms
         << ", \"digest\": \"" << hexDigest(result.stereo.digest) << "\"},\n"
         << "  \"lfe\": {\"peak\": " << result.lfe.peak
         << ", \"rms\": " << result.lfe.rms
         << ", \"digest\": \"" << hexDigest(result.lfe.digest) << "\"},\n"
         << "  \"cache\": {\"sampleRate\": " << cache_.sampleRate
         << ", \"emitterCount\": 22, \"receiverCount\": 2"
         << ", \"irLength\": " << irLength_
         << ", \"listenerViewIndex\": " << cache_.listenerViewIndex
         << ", \"leftReceiverIndex\": " << cache_.leftReceiverIndex
         << ", \"rightReceiverIndex\": " << cache_.rightReceiverIndex
         << ", \"delays\": \"ALL_ZERO\""
         << ", \"systemToSofaMapping\": \"PERMUTATION_VALIDATED\""
         << ", \"sourceSha256\": \"" << hexBytes(cache_.sourceSha256)
         << "\"}\n}"
         << '\n';
  const auto json = report.str();
  const std::vector<std::uint8_t> jsonBytes(json.begin(), json.end());
  if (!writeAtomic(paths.stereoWav, stereoWav, &reason) ||
      !writeAtomic(paths.lfeWav, lfeWav, &reason) ||
      !writeAtomic(paths.reportJson, jsonBytes, &reason)) {
    result.reason = reason;
    return result;
  }
  finished_ = true;
  result.accepted = true;
  result.reason = "i0p-offline-artifacts-written";
  return result;
}

I0pOfflineResult I0pOfflineOutput::reset(std::uint64_t generation) {
  if (!initialized_)
    return fail("i0p-not-initialized");
  stream_.reset();
  lfe_.clear();
  speakerSamples_ = 0U;
  speakerDigest_ = kFnvOffset;
  speakerSquareSum_ = 0.0L;
  speakerPeak_ = 0.0;
  generation_ = generation;
  nextSample_ = 0;
  finished_ = false;
  I0pOfflineResult result;
  result.accepted = true;
  result.reason = "i0p-reset";
  return result;
}

bool i0pOfflineSelfTest(std::string *reason) {
  if (!reason)
    return false;
  const std::uint64_t generation = 7U;
  const auto cache = syntheticCache(8U);
  const auto nonce = std::chrono::high_resolution_clock::now()
                         .time_since_epoch().count();
  const auto root = std::filesystem::temp_directory_path() /
                    ("audioplayer-i0p-offline-selftest-" +
                     std::to_string(nonce));
  std::error_code ec;
  I0pOfflinePaths paths{(root / "stereo.wav").string(),
                        (root / "lfe.wav").string(),
                        (root / "report.json").string()};
  I0pOfflineOutput output;
  auto init = output.init(cache, generation, 4U, 8U);
  auto batch = syntheticBatch(generation, 0, 4U);
  auto appended = output.append(batch);
  auto finished = output.finish(paths);
  const auto stereoBytes = readAll(paths.stereoWav);
  const auto lfeBytes = readAll(paths.lfeWav);
  const auto reportBytes = readAll(paths.reportJson);
  const std::string report(reportBytes.begin(), reportBytes.end());
  bool pass = init.accepted && appended.accepted && finished.accepted &&
              finished.sourceFrames == 4U && finished.stereoFrames == 11U &&
              finished.lfeFrames == 4U && finished.tailFrames == 7U &&
              stereoBytes.size() == 44U + 11U * 2U * 4U &&
              lfeBytes.size() == 44U + 4U * 4U &&
              tagAt(stereoBytes, 0U, {'R', 'I', 'F', 'F'}) &&
              tagAt(stereoBytes, 8U, {'W', 'A', 'V', 'E'}) &&
              tagAt(stereoBytes, 12U, {'f', 'm', 't', ' '}) &&
              tagAt(stereoBytes, 36U, {'d', 'a', 't', 'a'}) &&
              readU32(stereoBytes, 4U) == stereoBytes.size() - 8U &&
              readU32(stereoBytes, 16U) == 16U &&
              readU16(stereoBytes, 20U) == 3U &&
              readU16(stereoBytes, 22U) == 2U &&
              readU32(stereoBytes, 24U) == 48000U &&
              readU32(stereoBytes, 28U) == 48000U * 2U * 4U &&
              readU16(stereoBytes, 32U) == 8U &&
              readU16(stereoBytes, 34U) == 32U &&
              readU32(stereoBytes, 40U) == 11U * 2U * 4U &&
              readU16(lfeBytes, 20U) == 3U &&
              readU16(lfeBytes, 22U) == 1U &&
              readU32(lfeBytes, 40U) == 4U * 4U &&
              report.find("\"schema\": \"audioplayer.i0p-offline.v1\"") != std::string::npos &&
              report.find("\"i0p\": \"PASS\"") != std::string::npos &&
              report.find("\"productionAcceptance\": \"INCONCLUSIVE\"") != std::string::npos &&
              report.find("\"normalization\": \"NO\"") != std::string::npos &&
              report.find("\"stereoFormat\": \"IEEE_FLOAT32_LE_INTERLEAVED\"") != std::string::npos &&
              report.find("\"speakerBus\": {\"channels\": 22") != std::string::npos &&
              report.find("\"lfeDownmixPolicy\": \"NO_MIX\"") != std::string::npos &&
              report.find("\"lfeDownmixReason\": \"output-mode-or-lfe-disabled-not-proven\"")
                  != std::string::npos &&
              report.find("\"lfePolicy\": \"ExcludedFromBinaural+SeparateStem\"") != std::string::npos;

  I0pOfflineOutput rejection;
  pass = pass && rejection.init(cache, generation, 4U, 8U).accepted;
  auto wrongGeneration = syntheticBatch(generation + 1U, 0, 1U);
  pass = pass && !rejection.append(wrongGeneration).accepted;
  auto wrongTimeline = syntheticBatch(generation, 1, 1U);
  pass = pass && !rejection.append(wrongTimeline).accepted;
  auto wrongShape = syntheticBatch(generation, 0, 1U);
  wrongShape.speakers[4].clear();
  pass = pass && !rejection.append(wrongShape).accepted;
  auto nonfinite = syntheticBatch(generation, 0, 1U);
  nonfinite.speakers[0][0] = std::numeric_limits<float>::quiet_NaN();
  pass = pass && !rejection.append(nonfinite).accepted;
  pass = pass && !rejection.finish(paths).accepted;

  I0pOfflineOutput resetOutput;
  const I0pOfflinePaths resetPaths{(root / "reset-stereo.wav").string(),
                                   (root / "reset-lfe.wav").string(),
                                   (root / "reset-report.json").string()};
  pass = pass && resetOutput.init(cache, generation, 4U, 8U).accepted;
  auto partial = syntheticBatch(generation, 0, 2U);
  pass = pass && resetOutput.append(partial).accepted &&
         resetOutput.reset(generation).accepted &&
         resetOutput.append(batch).accepted;
  auto resetFinished = resetOutput.finish(resetPaths);
  pass = pass && resetFinished.accepted &&
         resetFinished.stereo.digest == finished.stereo.digest &&
         resetFinished.lfe.digest == finished.lfe.digest &&
         readAll(resetPaths.stereoWav) == stereoBytes &&
         readAll(resetPaths.lfeWav) == lfeBytes;
  std::filesystem::remove_all(root, ec);
  *reason = pass ? "i0p-self-test-pass" : "i0p-self-test-failed";
  return pass;
}

} // namespace eac3render
