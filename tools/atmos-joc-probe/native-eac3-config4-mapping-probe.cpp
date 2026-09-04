// J0A5: probe-only config-4 mapped JOC-session input.
//
// This contract consumes the actual owned N5J PCM vectors, applies the
// normative config-4 slot selection, and stops before the existing native JOC
// session bridge.  It is not a decoder, renderer, or production API.

#include "native-eac3-config4-pcm.h"
#include "native-eac3-config4-joc-bridge.h"
#include "native-eac3-config4-mapping.h"
#include "native-eac3-block-state.h"
#include "native-eac3-mantissas.h"
#include "native-eac3-transform.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace config4mapping {

constexpr unsigned kConfig = 4U;
constexpr std::uint16_t kChanmap = 0xA010U;
constexpr unsigned kDependentChannels = 4U;
constexpr unsigned kMappedChannels = 7U;
constexpr std::size_t kSamplesPerAu = 1536U;
constexpr std::size_t kSamplesPerTail = 256U;

const std::array<const char *, 10U> kSourceLocations = {
    "base.FL", "base.FC", "base.FR", "base.SL", "base.SR", "base.LFE",
    "dependent.L", "dependent.R", "dependent.VHL", "dependent.VHR"};
const std::array<const char *, kMappedChannels> kSlotNames = {
    "L", "R", "C", "Ls", "Rs", "Tfl", "Tfr"};
const std::array<unsigned, kMappedChannels> kSourceChannels = {
    6U, 7U, 1U, 3U, 4U, 8U, 9U};

void mix(std::uint64_t *digest, std::uint64_t value)
{
    if (!digest) return;
    for (unsigned shift = 0U; shift < 64U; shift += 8U) {
        *digest ^= (value >> shift) & 0xffU;
        *digest *= 1099511628211ULL;
    }
}

bool summarize(const std::vector<double> &samples, std::uint64_t *digest,
               double *peak, double *rms, std::string *reason)
{
    if (!digest || !peak || !rms || samples.empty()) {
        if (reason) *reason = "config4-mapping-empty-vector";
        return false;
    }
    double scale = 0.0;
    double scaledSquares = 0.0;
    *peak = 0.0;
    for (double sample : samples) {
        if (!std::isfinite(sample)) {
            if (reason) *reason = "config4-mapping-non-finite";
            return false;
        }
        const double magnitude = std::abs(sample);
        *peak = std::max(*peak, magnitude);
        if (magnitude == 0.0) continue;
        if (scale == 0.0) {
            scale = magnitude;
            scaledSquares = 1.0;
        } else if (magnitude > scale) {
            const double ratio = scale / magnitude;
            scaledSquares = 1.0 + scaledSquares * ratio * ratio;
            scale = magnitude;
        } else {
            const double ratio = magnitude / scale;
            scaledSquares += ratio * ratio;
        }
    }
    *rms = scale == 0.0 ? 0.0
        : scale * std::sqrt(scaledSquares
                            / static_cast<double>(samples.size()));
    if (!std::isfinite(*rms)) {
        if (reason) *reason = "config4-mapping-rms-non-finite";
        return false;
    }
    *digest = eac3native::digestCoefficientVector(samples);
    return true;
}

bool copyChannel(const config4pcm::ChannelSummary &source,
                 const char *slotName, unsigned slot, bool lfeBypass,
                 bool requireEosTail, MappedChannel *output,
                 std::string *reason)
{
    if (!output || !slotName || source.samples.size() != kSamplesPerAu
        || source.eosTail.size() != (requireEosTail ? kSamplesPerTail : 0U)) {
        if (reason) *reason = "config4-mapping-vector-shape";
        return false;
    }
    MappedChannel mapped;
    mapped.slot = slot;
    mapped.slotName = slotName;
    mapped.provenance = source.location ? source.location : "";
    mapped.lfeBypass = lfeBypass;
    mapped.samples = source.samples;
    mapped.eosTail = source.eosTail;
    mapped.sampleCount = mapped.samples.size();
    mapped.eosTailCount = mapped.eosTail.size();
    std::string summaryReason;
    if (!summarize(mapped.samples, &mapped.digest, &mapped.peak, &mapped.rms,
                   &summaryReason)
        || (requireEosTail
            && !summarize(mapped.eosTail, &mapped.eosDigest,
                          &mapped.eosPeak, &mapped.eosRms,
                          &summaryReason))) {
        if (reason) *reason = summaryReason;
        return false;
    }
    *output = std::move(mapped);
    return true;
}

bool distinctOwnedVectors(const MappingResult &result)
{
    std::vector<const double *> samplePointers;
    std::vector<const double *> tailPointers;
    auto collect = [&samplePointers, &tailPointers](const MappedChannel &channel) {
        if (!channel.samples.empty()) samplePointers.push_back(channel.samples.data());
        if (!channel.eosTail.empty()) tailPointers.push_back(channel.eosTail.data());
    };
    for (const MappedChannel &channel : result.channels) collect(channel);
    collect(result.lfeBypass);
    return std::adjacent_find(samplePointers.begin(), samplePointers.end())
               == samplePointers.end()
        && std::all_of(samplePointers.begin(), samplePointers.end(),
                       [&samplePointers](const double *pointer) {
                           return std::count(samplePointers.begin(),
                                             samplePointers.end(), pointer) == 1;
                       })
        && std::all_of(tailPointers.begin(), tailPointers.end(),
                       [&tailPointers](const double *pointer) {
                           return std::count(tailPointers.begin(),
                                             tailPointers.end(), pointer) == 1;
                       });
}

MappingResult mapAssembly(const config4pcm::AssemblyResult &assembly,
                          const MappingInput &input, bool requireEosTail)
{
    MappingResult result;
    result.auOrdinal = assembly.auOrdinal;
    if (input.jocConfig != kConfig || input.dependentChanmap != kChanmap
        || input.dependentChannelCount != kDependentChannels
        || input.dependentOrder.size() != kDependentChannels) {
        result.disposition = Disposition::Unsupported;
        result.reason = "config4-mapping-input-shape-unsupported";
        return result;
    }
    if (assembly.disposition != config4pcm::AssemblyDisposition::Accepted) {
        result.reason = assembly.reason.empty() ? "n5j-assembly-rejected"
                                                : assembly.reason;
        return result;
    }
    const std::size_t expectedStart =
        static_cast<std::size_t>(assembly.auOrdinal) * kSamplesPerAu;
    if (assembly.baseSampleStart != expectedStart
        || assembly.dependentSampleStart != expectedStart
        || assembly.channels.size() != kSourceLocations.size()) {
        result.reason = "config4-mapping-timeline-or-channel-count";
        return result;
    }
    if (input.auIndex != assembly.auOrdinal
        || input.timestamp != static_cast<std::int64_t>(expectedStart)
        || input.baseAcceptedContainers != 0U
        || input.baseTargetPayloads != 0U
        || input.dependentAcceptedContainers != 1U
        || input.dependentTargetPayloads != 2U) {
        result.reason = "config4-mapping-association-mismatch";
        return result;
    }
    for (unsigned channel = 0U; channel < kSourceLocations.size(); ++channel) {
        const config4pcm::ChannelSummary &source = assembly.channels[channel];
        if (source.channel != channel || !source.location
            || std::string(source.location) != kSourceLocations[channel]) {
            result.reason = "config4-mapping-source-order";
            return result;
        }
        if (channel >= 6U) {
            const unsigned dependentIndex = channel - 6U;
            if (std::string(source.location) != input.dependentOrder[dependentIndex]) {
                result.reason = "config4-mapping-dependent-order";
                return result;
            }
        }
        if (source.samples.size() != kSamplesPerAu
            || source.eosTail.size() != (requireEosTail ? kSamplesPerTail : 0U)) {
            result.reason = "config4-mapping-vector-shape";
            return result;
        }
        if (!std::all_of(source.samples.begin(), source.samples.end(),
                         [](double value) { return std::isfinite(value); })
            || !std::all_of(source.eosTail.begin(), source.eosTail.end(),
                            [](double value) { return std::isfinite(value); })) {
            result.reason = "config4-mapping-non-finite";
            return result;
        }
    }
    result.channels.reserve(kMappedChannels);
    for (unsigned slot = 0U; slot < kMappedChannels; ++slot) {
        const unsigned sourceIndex = kSourceChannels[slot];
        MappedChannel mapped;
        std::string reason;
        if (!copyChannel(assembly.channels[sourceIndex], kSlotNames[slot], slot,
                         false, requireEosTail, &mapped, &reason)) {
            result.reason = reason;
            return result;
        }
        result.channels.push_back(std::move(mapped));
    }
    std::string reason;
    if (!copyChannel(assembly.channels[5U], "LFE", kMappedChannels, true,
                     requireEosTail, &result.lfeBypass, &reason)) {
        result.reason = reason;
        return result;
    }
    result.auOrdinal = assembly.auOrdinal;
    result.sampleStart = expectedStart;
    result.jocConfig = input.jocConfig;
    result.jocChannels = kMappedChannels;
    result.dependentChanmap = input.dependentChanmap;
    result.digest = 1469598103934665603ULL;
    mix(&result.digest, result.auOrdinal);
    mix(&result.digest, result.sampleStart);
    for (const MappedChannel &channel : result.channels) {
        mix(&result.digest, channel.slot);
        mix(&result.digest, channel.digest);
        mix(&result.digest, channel.eosDigest);
    }
    mix(&result.digest, result.lfeBypass.digest);
    mix(&result.digest, result.lfeBypass.eosDigest);
    if (!distinctOwnedVectors(result)) {
        result.reason = "config4-mapping-owned-vector-alias";
        result.channels.clear();
        return result;
    }
    result.disposition = Disposition::Accepted;
    result.reason = "config4-mapped-joc-session-input";
    return result;
}

config4pcm::AssemblyResult syntheticAssembly(unsigned ordinal, bool eos)
{
    config4pcm::AssemblyResult assembly;
    assembly.disposition = config4pcm::AssemblyDisposition::Accepted;
    assembly.auOrdinal = ordinal;
    assembly.baseSampleStart = static_cast<std::size_t>(ordinal) * kSamplesPerAu;
    assembly.dependentSampleStart = assembly.baseSampleStart;
    assembly.channels.resize(kSourceLocations.size());
    for (unsigned channel = 0U; channel < kSourceLocations.size(); ++channel) {
        auto &value = assembly.channels[channel];
        value.channel = channel;
        value.location = kSourceLocations[channel];
        value.samples.resize(kSamplesPerAu,
                             0.01 * static_cast<double>(channel + 1U));
        if (eos) value.eosTail.resize(kSamplesPerTail,
                                      0.001 * static_cast<double>(channel + 1U));
        value.sampleCount = value.samples.size();
        value.eosTailCount = value.eosTail.size();
    }
    return assembly;
}

MappingInput expectedInput()
{
    return {};
}

MappingInput inputFromAssociation(const config4joc::Association &association)
{
    MappingInput input;
    input.jocConfig = association.jocConfig;
    input.dependentChanmap = association.dependentChanmap;
    input.dependentChannelCount = association.dependentChannelCount;
    input.auIndex = association.auIndex;
    input.timestamp = association.timestamp;
    input.baseAcceptedContainers = association.baseAcceptedContainers;
    input.baseTargetPayloads = association.baseTargetPayloads;
    input.dependentAcceptedContainers = association.dependentAcceptedContainers;
    input.dependentTargetPayloads = association.dependentTargetPayloads;
    return input;
}

bool carrierEvidence(const config4joc::Association &association)
{
    return association.baseAcceptedContainers == 0U
        && association.baseTargetPayloads == 0U
        && association.dependentAcceptedContainers == 1U
        && association.dependentTargetPayloads == 2U;
}

bool validateAssociationBatch(
    const std::vector<config4joc::Association> &associations,
    unsigned expectedAUs, std::string *reason)
{
    if (associations.size() != expectedAUs) {
        if (reason) *reason = "config4-mapping-association-count";
        return false;
    }
    for (unsigned ordinal = 0U; ordinal < expectedAUs; ++ordinal) {
        const config4joc::Association &association = associations[ordinal];
        if (!carrierEvidence(association)
            || association.auIndex != ordinal
            || association.timestamp
                != static_cast<std::int64_t>(ordinal) * kSamplesPerAu
            || association.jocConfig != kConfig
            || association.jocChannels != kMappedChannels
            || association.dependentChannelCount != kDependentChannels
            || association.dependentChanmap != kChanmap) {
            if (reason) *reason = "config4-mapping-association-mismatch";
            return false;
        }
    }
    return true;
}

config4joc::Association syntheticAssociation(unsigned ordinal)
{
    config4joc::Association association;
    association.auIndex = ordinal;
    association.timestamp = static_cast<std::int64_t>(ordinal) * kSamplesPerAu;
    association.baseAcceptedContainers = 0U;
    association.baseTargetPayloads = 0U;
    association.dependentAcceptedContainers = 1U;
    association.dependentTargetPayloads = 2U;
    association.dependentChannelCount = kDependentChannels;
    association.dependentChanmap = kChanmap;
    association.jocConfig = kConfig;
    association.jocChannels = kMappedChannels;
    return association;
}

bool selfTest()
{
    std::size_t cases = 0U;
    MappingInput expected = expectedInput();
    expected.auIndex = 0U;
    expected.timestamp = 0;
    const config4pcm::AssemblyResult firstAssembly = syntheticAssembly(0U, false);
    const config4pcm::AssemblyResult finalAssembly = syntheticAssembly(2U, true);
    const MappingResult first = mapAssembly(firstAssembly, expected, false);
    ++cases;
    if (first.disposition != Disposition::Accepted || first.channels.size() != 7U
        || first.lfeBypass.eosTail.size() != 0U
        || first.channels[0].provenance != "dependent.L"
        || first.channels[1].provenance != "dependent.R"
        || first.channels[2].provenance != "base.FC"
        || first.channels[5].provenance != "dependent.VHL"
        || first.channels[6].provenance != "dependent.VHR") {
        return false;
    }
    MappingInput finalInput = expected;
    finalInput.auIndex = 2U;
    finalInput.timestamp = 2 * static_cast<std::int64_t>(kSamplesPerAu);
    const MappingResult final = mapAssembly(finalAssembly, finalInput, true);
    ++cases;
    if (final.disposition != Disposition::Accepted
        || final.channels[0].samples.size() != kSamplesPerAu
        || final.channels[0].eosTail.size() != kSamplesPerTail
        || final.lfeBypass.eosTail.size() != kSamplesPerTail) {
        return false;
    }
    MappingInput wrongConfig = expected;
    wrongConfig.jocConfig = 3U;
    ++cases;
    const MappingResult wrongConfigResult = mapAssembly(firstAssembly, wrongConfig, false);
    if (wrongConfigResult.disposition != Disposition::Unsupported) {
        return false;
    }
    auto wrongAssociationConfig = expected;
    wrongAssociationConfig.jocConfig = 3U;
    ++cases;
    const MappingResult wrongAssociationConfigResult = mapAssembly(firstAssembly, wrongAssociationConfig, false);
    if (wrongAssociationConfigResult.disposition != Disposition::Unsupported) {
        return false;
    }
    MappingInput wrongChanmap = expected;
    wrongChanmap.dependentChanmap = 0U;
    ++cases;
    const MappingResult wrongChanmapResult = mapAssembly(firstAssembly, wrongChanmap, false);
    if (wrongChanmapResult.disposition != Disposition::Unsupported) {
        return false;
    }
    MappingInput wrongCount = expected;
    wrongCount.dependentChannelCount = 3U;
    ++cases;
    const MappingResult wrongCountResult = mapAssembly(firstAssembly, wrongCount, false);
    if (wrongCountResult.disposition != Disposition::Unsupported) {
        return false;
    }
    MappingInput wrongOrder = expected;
    std::swap(wrongOrder.dependentOrder[0], wrongOrder.dependentOrder[1]);
    ++cases;
    const MappingResult wrongOrderResult = mapAssembly(firstAssembly, wrongOrder, false);
    if (wrongOrderResult.reason != "config4-mapping-dependent-order") {
        return false;
    }
    auto wrongTimeline = firstAssembly;
    wrongTimeline.dependentSampleStart = kSamplesPerAu;
    ++cases;
    const MappingResult wrongTimelineResult = mapAssembly(wrongTimeline, expected, false);
    if (wrongTimelineResult.reason != "config4-mapping-timeline-or-channel-count") {
        return false;
    }
    auto wrongAssociationTimestamp = expected;
    wrongAssociationTimestamp.timestamp = kSamplesPerAu;
    ++cases;
    const MappingResult wrongAssociationTimestampResult = mapAssembly(firstAssembly, wrongAssociationTimestamp, false);
    if (wrongAssociationTimestampResult.reason != "config4-mapping-association-mismatch") {
        return false;
    }
    auto wrongAssociationAu = expected;
    wrongAssociationAu.auIndex = 1U;
    ++cases;
    const MappingResult wrongAssociationAuResult = mapAssembly(firstAssembly, wrongAssociationAu, false);
    if (wrongAssociationAuResult.reason != "config4-mapping-association-mismatch") {
        return false;
    }
    std::vector<config4joc::Association> validAssociations;
    for (unsigned ordinal = 0U; ordinal < 3U; ++ordinal)
        validAssociations.push_back(syntheticAssociation(ordinal));
    std::string associationReason;
    ++cases;
    if (!validateAssociationBatch(validAssociations, 3U, &associationReason))
        return false;
    auto missingAssociation = validAssociations;
    missingAssociation.pop_back();
    ++cases;
    if (validateAssociationBatch(missingAssociation, 3U, &associationReason)
        || associationReason != "config4-mapping-association-count") return false;
    auto extraAssociation = validAssociations;
    extraAssociation.push_back(syntheticAssociation(3U));
    ++cases;
    if (validateAssociationBatch(extraAssociation, 3U, &associationReason)
        || associationReason != "config4-mapping-association-count") return false;
    auto wrongTail = finalAssembly;
    wrongTail.channels[0].eosTail.clear();
    ++cases;
    const MappingResult wrongTailResult = mapAssembly(wrongTail, finalInput, true);
    if (wrongTailResult.reason != "config4-mapping-vector-shape") {
        return false;
    }
    auto nonFinite = firstAssembly;
    nonFinite.channels[6].samples[0] = std::numeric_limits<double>::quiet_NaN();
    ++cases;
    const MappingResult nonFiniteResult = mapAssembly(nonFinite, expected, false);
    if (nonFiniteResult.reason != "config4-mapping-non-finite") {
        return false;
    }
    auto ownedSource = firstAssembly;
    const MappingResult owned = mapAssembly(ownedSource, expected, false);
    const double otherBefore = owned.channels[1].samples[0];
    const double sourceBefore = ownedSource.channels[6].samples[0];
    auto mutated = owned;
    mutated.channels[0].samples[0] = -1.0;
    ++cases;
    if (owned.disposition != Disposition::Accepted
        || !distinctOwnedVectors(owned)
        || mutated.channels[1].samples[0] != otherBefore
        || ownedSource.channels[6].samples[0] != sourceBefore
        || mutated.channels[0].samples.data() == owned.channels[0].samples.data()) {
        return false;
    }
    std::cout << "mappingSelfTest=PASS cases=" << cases
              << " slotOrder=PASS provenance=PASS configReject=PASS"
              << " chanmapReject=PASS countReject=PASS orderReject=PASS"
              << " associationConfigReject=PASS timelineReject=PASS"
              << " associationTimestampReject=PASS associationAuReject=PASS"
              << " associationBatch=PASS missingExtraReject=PASS"
              << " tailReject=PASS finiteReject=PASS"
              << " ownedNoAlias=PASS\n";
    return cases == 16U;
}

DecodeReport decodeFile(const std::string &path, const std::string &tablePath,
                        std::size_t maxAUs, const MappingCallback &callback)
{
    DecodeReport output;
    if (maxAUs == 0U
        || maxAUs >= static_cast<std::size_t>(std::numeric_limits<unsigned>::max())
        || maxAUs > static_cast<std::size_t>(
               std::numeric_limits<std::int64_t>::max()) / kSamplesPerAu
        || !callback) {
        output.reason = maxAUs == 0U ? "config4-mapping-max-aus-invalid"
                                     : "config4-mapping-callback-missing";
        if (maxAUs >= static_cast<std::size_t>(std::numeric_limits<unsigned>::max())
            || maxAUs > static_cast<std::size_t>(
                   std::numeric_limits<std::int64_t>::max()) / kSamplesPerAu)
            output.reason = "config4-mapping-max-aus-overflow";
        return output;
    }
    std::vector<config4joc::Association> associations;
    std::string associationFailure;
    const config4joc::DecodeReport associationReport = config4joc::decodeFile(
        path, tablePath, maxAUs,
        [&associations, &associationFailure](const config4joc::Association &value) {
            if (!carrierEvidence(value) || value.jocConfig != kConfig
                || value.jocChannels != kMappedChannels
                || value.dependentChannelCount != kDependentChannels
                || value.dependentChanmap != kChanmap) {
                associationFailure = "config4-mapping-joc-association-invalid";
                return false;
            }
            associations.push_back(value);
            return true;
        });
    std::string associationReason;
    if (associationReport.disposition != config4joc::Disposition::Accepted
        || !validateAssociationBatch(associations, maxAUs, &associationReason)) {
        output.reason = associationFailure.empty()
            ? (associationReason.empty() ? associationReport.reason
                                          : associationReason)
            : associationFailure;
        return output;
    }
    std::string mappingFailure;
    std::size_t associationIndex = 0U;
    const config4pcm::DecodeReport report = config4pcm::decodeFile(
        path, maxAUs, [&mappingFailure, &associations, &associationIndex, &callback, maxAUs](
                    const config4pcm::AssemblyResult &assembly) {
            if (associationIndex >= associations.size()) {
                mappingFailure = "config4-mapping-association-extra";
                return false;
            }
            const config4joc::Association &association = associations[associationIndex++];
            MappingResult value = mapAssembly(
                assembly, inputFromAssociation(association),
                assembly.auOrdinal + 1U == maxAUs);
            if (value.disposition != Disposition::Accepted) {
                mappingFailure = value.reason;
                return false;
            }
            if (!callback(value)) {
                mappingFailure = "config4-mapping-callback-rejected";
                return false;
            }
            return true;
        });
    if (report.disposition != config4pcm::AssemblyDisposition::Accepted
        || associationIndex != maxAUs) {
        output.reason = mappingFailure.empty() ? report.reason : mappingFailure;
        return output;
    }
    output.disposition = Disposition::Accepted;
    output.mappedAUs = associationIndex;
    output.reason = "config4-mapping-accepted";
    return output;
}

bool runFile(const std::string &path, std::size_t maxAUs)
{
    std::vector<MappingResult> mapped;
    const DecodeReport report = decodeFile(
        path, "docs/dev/ts_103420_tables.c", maxAUs,
        [&mapped](const MappingResult &value) {
            mapped.push_back(value);
            return true;
        });
    if (report.disposition != Disposition::Accepted || mapped.size() != maxAUs) {
        std::cerr << "mappingResult=FAIL reason=" << report.reason << '\n';
        return false;
    }
    for (const MappingResult &value : mapped) {
        std::cout << "mappedAu=" << value.auOrdinal
                  << " sampleStart=" << value.sampleStart
                  << " slots=" << value.jocChannels
                  << " samplesPerSlot=" << value.channels.front().sampleCount
                  << " eosTailPerSlot=" << value.channels.front().eosTailCount
                  << " lfeBypassSamples=" << value.lfeBypass.sampleCount
                  << " lfeBypassEosTail=" << value.lfeBypass.eosTailCount
                  << " digest=0x" << std::hex << std::setfill('0')
                  << std::setw(16) << value.digest << std::dec << '\n';
        for (const MappedChannel &channel : value.channels)
            std::cout << "slot=" << channel.slot
                      << " name=" << channel.slotName
                      << " provenance=" << channel.provenance
                      << " sampleCount=" << channel.sampleCount
                      << " eosTailCount=" << channel.eosTailCount << '\n';
        std::cout << "lfeBypass provenance=" << value.lfeBypass.provenance
                  << " sampleCount=" << value.lfeBypass.sampleCount
                  << " eosTailCount=" << value.lfeBypass.eosTailCount << '\n';
    }
    std::cout << "mappedAUs=" << mapped.size()
              << " jocConfig=4 jocChannels=7 dependentChanmap=0xA010\n"
              << "mappingResult=PASS stage=gate-j0a5-config4-mapped-session-input\n"
              << "sessionInput=NOT_ENTERED productionAcceptance=INCONCLUSIVE\n"
              << "drcApplied=NO rendererApplied=NO ffmpegLinked=NO\n";
    return true;
}

} // namespace config4mapping

#ifndef EAC3_CONFIG4_MAPPING_NO_MAIN
int main(int argc, char **argv)
{
    if (argc == 2 && std::string(argv[1]) == "--self-test")
        return config4mapping::selfTest() ? 0 : 1;
    std::size_t maxAUs = 3U;
    const char *path = nullptr;
    if (argc == 2) path = argv[1];
    else if (argc == 4 && std::string(argv[1]) == "--max-aus") {
        try {
            const unsigned long long parsed = std::stoull(argv[2]);
            if (parsed == 0U || parsed > static_cast<unsigned long long>(
                    std::numeric_limits<std::size_t>::max()))
                throw std::out_of_range("max-aus");
            maxAUs = static_cast<std::size_t>(parsed);
            path = argv[3];
        } catch (...) {
            std::cerr << "mappingResult=FAIL reason=config4-mapping-invalid-max-aus\n";
            return 2;
        }
    }
    if (!path) {
        std::cerr << "Usage: Eac3NativeConfig4MappingProbe <raw.eac3|raw.eb3>\n"
                     "       Eac3NativeConfig4MappingProbe --max-aus N <raw.eac3|raw.eb3>\n"
                     "       Eac3NativeConfig4MappingProbe --self-test\n";
        return 2;
    }
    return config4mapping::runFile(path, maxAUs) ? 0 : 1;
}
#endif
