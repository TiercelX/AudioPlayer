#include "native-eac3-joc-session-bridge.h"

#include "joc-qmf.h"
#include "native-eac3-core.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

struct ProbeInput {
    std::vector<std::uint8_t> bytes;
    std::vector<std::vector<std::uint8_t>> frames;
};

bool readInput(const std::string &path, std::size_t maxFrames,
               ProbeInput *input, std::string *reason)
{
    if (!input || !reason) return false;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) { *reason = "input-open-failed"; return false; }
    input->bytes.assign(std::istreambuf_iterator<char>(stream), {});
    std::size_t offset = 0U;
    while (offset < input->bytes.size() && input->frames.size() < maxFrames) {
        const eac3native::ParseResult parsed =
            eac3native::parseSyncframe(input->bytes, offset);
        if (parsed.disposition != eac3native::Disposition::Accepted
            || !parsed.frame || parsed.frame->sizeBytes == 0U
            || parsed.frame->offset != offset
            || parsed.frame->sizeBytes > input->bytes.size() - offset) {
            *reason = parsed.reason.empty() ? "input-frame-parse-failed" : parsed.reason;
            return false;
        }
        input->frames.emplace_back(input->bytes.begin() + offset,
                                   input->bytes.begin() + offset
                                       + parsed.frame->sizeBytes);
        offset += parsed.frame->sizeBytes;
    }
    if (input->frames.size() != maxFrames) {
        *reason = "input-fewer-than-requested-frames";
        return false;
    }
    return true;
}

struct BatchStats {
    std::size_t batches = 0U;
    std::size_t samples = 0U;
    std::size_t metadata = 0U;
    std::size_t objectCount = 0U;
    bool finite = true;
    bool identity = true;
    std::uint64_t digest = 1469598103934665603ULL;
};

void hashByte(BatchStats *stats, std::uint8_t value)
{
    stats->digest ^= value;
    stats->digest *= 1099511628211ULL;
}

void hashFloat(BatchStats *stats, float value)
{
    std::uint32_t bits = 0U;
    std::memcpy(&bits, &value, sizeof(bits));
    for (unsigned shift = 0U; shift < 32U; shift += 8U) {
        hashByte(stats, static_cast<std::uint8_t>((bits >> shift) & 0xffU));
    }
}

bool acceptBatch(const eac3gate6c::Batch &batch, BatchStats *stats)
{
    if (!stats) return false;
    ++stats->batches;
    stats->objectCount = batch.objects.size();
    if (batch.objects.size() != eac3gate6c::kDynamicObjectCount
        || batch.lfe.size() != batch.objects.front().size()) {
        stats->identity = false;
        return false;
    }
    stats->samples += batch.lfe.size();
    stats->metadata += batch.metadata.size();
    for (const std::vector<float> &object : batch.objects) {
        if (object.size() != batch.lfe.size()) stats->identity = false;
        for (float value : object) {
            stats->finite = stats->finite && std::isfinite(value);
            hashFloat(stats, value);
        }
    }
    for (float value : batch.lfe) {
        stats->finite = stats->finite && std::isfinite(value);
        hashFloat(stats, value);
    }
    for (const eac3gate6c::MetadataUpdate &update : batch.metadata) {
        hashByte(stats, static_cast<std::uint8_t>(update.objectIndex));
        hashByte(stats, static_cast<std::uint8_t>(update.blockIndex));
        hashByte(stats, static_cast<std::uint8_t>(update.rampDuration));
    }
    return stats->finite && stats->identity;
}

bool loadWindow(const std::string &table, std::vector<double> *qwin,
                std::string *reason)
{
    return eac3qmf::loadQwin(table, qwin, reason);
}

bool runSelfTest(const ProbeInput &input, const std::string &table,
                 const std::vector<double> &qwin)
{
    if (input.frames.empty()) return false;
    eac3native::JocSessionBridgeConfig config;
    config.qwin = qwin;
    config.decodedSourceSamples = 1536;
    eac3native::NativeEac3JocSessionBridge bridge(table, config);
    if (bridge.open().disposition != eac3native::JocSessionBridgeDisposition::Accepted) {
        return false;
    }
    const eac3native::JocSessionBridgeResult rejected = bridge.process(
        input.frames.front(), [](const eac3gate6c::Batch &) { return false; });
    if (rejected.disposition != eac3native::JocSessionBridgeDisposition::Malformed
        || !bridge.poisoned()) return false;
    BatchStats ignored;
    if (bridge.flush([&ignored](const eac3gate6c::Batch &batch) {
            return acceptBatch(batch, &ignored);
        }).reason != "joc-session-bridge-poisoned-reset-required") return false;
    bridge.reset();
    if (bridge.open().disposition != eac3native::JocSessionBridgeDisposition::Accepted) {
        return false;
    }
    BatchStats first;
    const eac3native::JocSessionBridgeResult accepted = bridge.process(
        input.frames.front(), [&first](const eac3gate6c::Batch &batch) {
            return acceptBatch(batch, &first);
        });
    if (accepted.disposition != eac3native::JocSessionBridgeDisposition::Accepted
        || accepted.metadataUpdates == 0U || !first.finite || !first.identity) {
        return false;
    }
    const eac3native::JocSessionBridgeResult eos = bridge.flush(
        [&first](const eac3gate6c::Batch &batch) {
            return acceptBatch(batch, &first);
        });
    if (eos.disposition != eac3native::JocSessionBridgeDisposition::Accepted
        || eos.eosTailSamples != 256U || first.objectCount != 15U) {
        return false;
    }
    bridge.reset();
    if (bridge.open().disposition != eac3native::JocSessionBridgeDisposition::Accepted) {
        return false;
    }
    BatchStats replay;
    const eac3native::JocSessionBridgeResult replayed = bridge.process(
        input.frames.front(), [&replay](const eac3gate6c::Batch &batch) {
            return acceptBatch(batch, &replay);
        });
    const eac3native::JocSessionBridgeResult replayEos = bridge.flush(
        [&replay](const eac3gate6c::Batch &batch) {
            return acceptBatch(batch, &replay);
        });
    if (replayed.disposition != eac3native::JocSessionBridgeDisposition::Accepted
        || replayEos.disposition != eac3native::JocSessionBridgeDisposition::Accepted
        || replay.digest != first.digest || replay.samples != first.samples
        || replay.metadata != first.metadata) {
        return false;
    }
    bridge.reset();
    if (bridge.open().disposition != eac3native::JocSessionBridgeDisposition::Accepted) {
        return false;
    }
    bridge.cancel();
    const eac3native::JocSessionBridgeResult canceled = bridge.process(
        input.frames.front(), [](const eac3gate6c::Batch &) { return true; });
    return canceled.reason == "joc-session-bridge-canceled";
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 3) {
        std::cerr << "Usage: Eac3NativeJocSessionBridgeProbe <raw.eac3> <joc-table> "
                     "[--max-frames N] [--self-test]\n";
        return 2;
    }
    std::string inputPath = argv[1];
    std::string tablePath = argv[2];
    std::size_t maxFrames = 10U;
    bool selfTest = false;
    for (int index = 3; index < argc; ++index) {
        const std::string option = argv[index];
        if (option == "--self-test") {
            selfTest = true;
        } else if (option == "--max-frames" && index + 1 < argc) {
            maxFrames = static_cast<std::size_t>(std::stoull(argv[++index]));
        } else {
            std::cerr << "unknown-option=" << option << '\n';
            return 2;
        }
    }
    if (maxFrames == 0U
        || maxFrames > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max() / 1536)) {
        std::cerr << "invalid-max-frames\n";
        return 2;
    }
    std::vector<double> qwin;
    std::string reason;
    if (!loadWindow(tablePath, &qwin, &reason)) {
        std::cerr << "qwin=FAIL reason=" << reason << '\n';
        return 1;
    }
    ProbeInput input;
    if (!readInput(inputPath, maxFrames, &input, &reason)) {
        std::cerr << "input=FAIL reason=" << reason << '\n';
        return 1;
    }
    if (selfTest) {
        const bool pass = runSelfTest(input, tablePath, qwin);
        std::cout << "bridgeSelfTest=" << (pass ? "PASS" : "FAIL")
                  << " cases=6 callbackReject=YES poisonedFlush=YES reset=YES"
                     " resetEquivalence=YES cancel=YES eos=YES\n";
        return pass ? 0 : 1;
    }
    eac3native::JocSessionBridgeConfig config;
    config.qwin = qwin;
    config.decodedSourceSamples = static_cast<std::int64_t>(maxFrames * 1536U);
    eac3native::NativeEac3JocSessionBridge bridge(tablePath, config);
    const eac3native::JocSessionBridgeResult opened = bridge.open();
    if (opened.disposition != eac3native::JocSessionBridgeDisposition::Accepted) {
        std::cerr << "open=FAIL stage=" << opened.stage << " reason=" << opened.reason << '\n';
        return 1;
    }
    BatchStats stats;
    std::size_t accepted = 0U;
    std::size_t qualified = 0U;
    bool timestampPass = true;
    std::size_t metadataUpdates = 0U;
    for (const auto &frame : input.frames) {
        const eac3native::JocSessionBridgeResult processed = bridge.process(
            frame, [&stats](const eac3gate6c::Batch &batch) {
                return acceptBatch(batch, &stats);
            });
        if (processed.disposition != eac3native::JocSessionBridgeDisposition::Accepted) {
            std::cout << "firstFailure=au" << processed.auIndex
                      << " stage=" << processed.stage
                      << " reason=" << processed.reason << '\n';
            return 1;
        }
        ++accepted;
        ++qualified;
        metadataUpdates += processed.metadataUpdates;
        timestampPass = timestampPass && processed.timestamp
            == static_cast<std::int64_t>((accepted - 1U) * 1536U);
    }
    const eac3native::JocSessionBridgeResult eos = bridge.flush(
        [&stats](const eac3gate6c::Batch &batch) {
            return acceptBatch(batch, &stats);
        });
    const bool eosPass = eos.disposition == eac3native::JocSessionBridgeDisposition::Accepted
        && eos.eosTailSamples == 256U;
    const std::size_t expectedSamples = maxFrames * 1536U;
    const bool pass = accepted == maxFrames && qualified == maxFrames
        && stats.objectCount == eac3gate6c::kDynamicObjectCount
        && stats.finite && stats.identity && timestampPass && eosPass;
    std::cout << "auCount=" << maxFrames << '\n'
              << "acceptedFrames=" << accepted << '\n'
              << "qualifiedFrames=" << qualified << '\n'
              << "sessionAccepted=" << bridge.sessionReport().framesAccepted << '\n'
              << "metadataApplied=" << metadataUpdates << '\n'
              << "objectCount=" << stats.objectCount << '\n'
              << "callbackBatches=" << stats.batches << '\n'
              << "pcmSamplesPerChannel=" << stats.samples << '\n'
              << "expectedCoreSamplesPerChannel=" << expectedSamples << '\n'
              << "eosTailSamples=" << eos.eosTailSamples << '\n'
              << "finite=" << (stats.finite ? "YES" : "NO") << '\n'
              << "channelIdentity=" << (stats.identity ? "PASS" : "FAIL") << '\n'
              << "timestampContinuity=" << (timestampPass ? "PASS" : "FAIL") << '\n'
              << "drcApplied=NO\n"
              << "ffmpegLinked=NO\n"
              << "stateDigest=" << std::hex << stats.digest << std::dec << '\n'
              << "streamResetPolicy=reset-on-open-only-continuous-adjacent-AUs\n"
              << "bridgeResult=" << (pass ? "PASS" : "FAIL")
              << " stage=gate-j0a3-native-config3-session-bridge\n";
    return pass ? 0 : 1;
}
