// Gate 8N-5h: probe-local config-4 base/dependent PCM assembly.
//
// The two established diagnostic decoders are linked as separate
// implementation units through probe-local contracts, so this adapter reuses
// their exact result/state seams instead of copying either decoder. It owns
// the bounded PCM vectors for one diagnostic AU; it never mixes or substitutes
// dependent channels for base channels and is not production playback, JOC, or
// renderer code.

#include "native-eac3-dependent-pcm.h"
#include "native-eac3-legacy-ac3-diagnostic.h"
#include "native-eac3-block-state.h"
#include "native-eac3-config4-pcm.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace config4pcm {

using eac3native::BsiParseResult;
using eac3native::BsiInfo;
using eac3native::Disposition;
using eac3native::FrameHeader;
using eac3native::ParseResult;
using eac3native::StreamType;

constexpr unsigned kBaseChannels = 6U;
constexpr unsigned kDependentChannels = 4U;
constexpr unsigned kChannels = kBaseChannels + kDependentChannels;
constexpr unsigned kBlocks = 6U;
constexpr unsigned kRate = 48000U;
constexpr std::size_t kSamplesPerAu = 1536U;
constexpr std::size_t kSamplesPerTail = 256U;

const std::array<const char *, kChannels> kLocations = {
    "base.FL", "base.FC", "base.FR", "base.SL", "base.SR", "base.LFE",
    "dependent.L", "dependent.R", "dependent.VHL", "dependent.VHR"};

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
    if (!digest || !peak || !rms || samples.empty()) return false;
    double scale = 0.0;
    double scaledSquares = 0.0;
    *peak = 0.0;
    for (double sample : samples) {
        if (!std::isfinite(sample)) {
            if (reason) *reason = "config4-pcm-non-finite";
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
    if (!std::isfinite(*rms)) return false;
    *digest = eac3native::digestCoefficientVector(samples);
    return true;
}

bool finiteSummary(const ChannelSummary &channel, bool requireEosTail = true)
{
    return channel.samples.size() == kSamplesPerAu
        && channel.eosTail.size() == (requireEosTail ? kSamplesPerTail : 0U)
        && channel.sampleCount == channel.samples.size()
        && channel.eosTailCount == channel.eosTail.size()
        && std::isfinite(channel.peak) && std::isfinite(channel.rms)
        && std::isfinite(channel.eosPeak) && std::isfinite(channel.eosRms);
}

AssemblyResult combine(const legacyac3::Result &base,
                       const dependentpcm::PcmResult &dependent,
                       unsigned auOrdinal, bool requireEosTail = true)
{
    AssemblyResult result;
    result.auOrdinal = auOrdinal;
    if (base.disposition != legacyac3::Disposition::Accepted) {
        result.disposition = base.disposition == legacyac3::Disposition::Unsupported
            ? AssemblyDisposition::Unsupported : AssemblyDisposition::Malformed;
        result.reason = base.reason.empty() ? "base-pcm-rejected" : base.reason;
        return result;
    }
    if (dependent.disposition != dependentpcm::Disposition::Accepted) {
        result.disposition = dependent.disposition == dependentpcm::Disposition::Unsupported
            ? AssemblyDisposition::Unsupported : AssemblyDisposition::Malformed;
        result.reason = dependent.reason.empty()
            ? "dependent-pcm-rejected" : dependent.reason;
        return result;
    }
    if (base.pcm.size() != kBaseChannels
        || dependent.channels.size() != kDependentChannels) {
        result.disposition = AssemblyDisposition::Malformed;
        result.reason = "config4-channel-count";
        return result;
    }
    result.channels.reserve(kChannels);
    for (unsigned channel = 0U; channel < kBaseChannels; ++channel) {
        const legacyac3::PcmSummary &source = base.pcm[channel];
        ChannelSummary summary;
        summary.channel = channel;
        summary.location = kLocations[channel];
        summary.samples = source.samples;
        summary.eosTail = source.eosTail;
        summary.sampleCount = summary.samples.size();
        summary.eosTailCount = summary.eosTail.size();
        std::string summaryReason;
        if (!summarize(summary.samples, &summary.digest, &summary.peak,
                       &summary.rms, &summaryReason)
            || (requireEosTail
                && !summarize(summary.eosTail, &summary.eosDigest,
                              &summary.eosPeak, &summary.eosRms,
                              &summaryReason))) {
            result.disposition = AssemblyDisposition::Malformed;
            result.reason = summaryReason.empty() ? "base-pcm-summary"
                                                   : summaryReason;
            return result;
        }
        if (!finiteSummary(summary, requireEosTail)) {
            result.disposition = AssemblyDisposition::Malformed;
            result.reason = "base-pcm-summary";
            return result;
        }
        result.channels.push_back(summary);
    }
    for (unsigned channel = 0U; channel < kDependentChannels; ++channel) {
        const dependentpcm::ChannelSummary &source = dependent.channels[channel];
        ChannelSummary summary;
        summary.channel = kBaseChannels + channel;
        summary.location = kLocations[summary.channel];
        summary.samples = source.samples;
        summary.eosTail = source.eosTail;
        summary.sampleCount = summary.samples.size();
        summary.eosTailCount = summary.eosTail.size();
        std::string summaryReason;
        if (!summarize(summary.samples, &summary.digest, &summary.peak,
                       &summary.rms, &summaryReason)
            || (requireEosTail
                && !summarize(summary.eosTail, &summary.eosDigest,
                              &summary.eosPeak, &summary.eosRms,
                              &summaryReason))) {
            result.disposition = AssemblyDisposition::Malformed;
            result.reason = summaryReason.empty() ? "dependent-pcm-summary"
                                                   : summaryReason;
            return result;
        }
        if (!finiteSummary(summary, requireEosTail)) {
            result.disposition = AssemblyDisposition::Malformed;
            result.reason = "dependent-pcm-summary";
            return result;
        }
        result.channels.push_back(summary);
    }
    result.baseSampleStart = static_cast<std::size_t>(auOrdinal)
        * kSamplesPerAu;
    result.dependentSampleStart = result.baseSampleStart;
    result.digest = 1469598103934665603ULL;
    mix(&result.digest, result.auOrdinal);
    mix(&result.digest, result.baseSampleStart);
    mix(&result.digest, result.dependentSampleStart);
    for (const ChannelSummary &channel : result.channels) {
        mix(&result.digest, channel.channel);
        mix(&result.digest, channel.digest);
        mix(&result.digest, channel.eosDigest);
    }
    result.disposition = AssemblyDisposition::Accepted;
    result.reason = "config4-base-dependent-diagnostic-assembly";
    return result;
}

class FrameSyntaxBoundaryOwner {
public:
    bool begin(unsigned ordinal, std::size_t startOffset)
    {
        if (poisoned_ || active_ || ordinal != nextOrdinal_) return false;
        active_ = true;
        currentOrdinal_ = ordinal;
        currentStartOffset_ = startOffset;
        return true;
    }

    bool complete(std::size_t endOffset, std::size_t baseEndBit,
                  std::size_t dependentEndBit)
    {
        if (!active_ || endOffset < currentStartOffset_
            || dependentEndBit < baseEndBit) {
            poisoned_ = true;
            active_ = false;
            return false;
        }
        active_ = false;
        lastEndOffset_ = endOffset;
        lastBaseEndBit_ = baseEndBit;
        lastDependentEndBit_ = dependentEndBit;
        ++nextOrdinal_;
        return true;
    }

    void abort()
    {
        active_ = false;
        poisoned_ = true;
    }

    void reset()
    {
        active_ = false;
        poisoned_ = false;
        nextOrdinal_ = 0U;
        currentOrdinal_ = 0U;
        currentStartOffset_ = 0U;
        lastEndOffset_ = 0U;
        lastBaseEndBit_ = 0U;
        lastDependentEndBit_ = 0U;
    }

    bool frameBoundaryClosed() const { return !active_ && !poisoned_; }
    unsigned framesCompleted() const { return nextOrdinal_; }

private:
    bool active_ = false;
    bool poisoned_ = false;
    unsigned nextOrdinal_ = 0U;
    unsigned currentOrdinal_ = 0U;
    std::size_t currentStartOffset_ = 0U;
    std::size_t lastEndOffset_ = 0U;
    std::size_t lastBaseEndBit_ = 0U;
    std::size_t lastDependentEndBit_ = 0U;
};

bool parsePair(const std::vector<std::uint8_t> &framed, std::size_t offset,
               unsigned ordinal, AssemblyResult *first,
               std::size_t *nextOffset,
               legacyac3::StatefulDecoder *baseState = nullptr,
               dependentpcm::StatefulDecoder *dependentState = nullptr,
               bool emitEos = true,
               FrameSyntaxBoundaryOwner *syntaxOwner = nullptr)
{
    if (!first || !nextOffset || offset >= framed.size()) return false;
    if (syntaxOwner && !syntaxOwner->begin(ordinal, offset)) {
        first->disposition = AssemblyDisposition::Malformed;
        first->reason = "frame-syntax-boundary-reuse";
        return false;
    }
    struct BoundaryGuard {
        FrameSyntaxBoundaryOwner *owner = nullptr;
        bool handled = false;
        ~BoundaryGuard()
        {
            if (owner && !handled) owner->abort();
        }
    } boundaryGuard{syntaxOwner};
    const ParseResult baseHeader = eac3native::parseSyncframe(framed, offset);
    if (baseHeader.disposition != Disposition::Accepted || !baseHeader.frame) {
        first->disposition = AssemblyDisposition::Malformed;
        first->reason = "base-frame-parse";
        return false;
    }
    const FrameHeader &base = *baseHeader.frame;
    if (base.streamType != StreamType::LegacyAc3 || base.substreamId != 0U
        || base.sampleRate != kRate || base.blocks != kBlocks
        || base.acmod != 7U || !base.lfe || base.channelCount != kBaseChannels) {
        first->disposition = AssemblyDisposition::Unsupported;
        first->reason = "config4-base-topology";
        return false;
    }
    const std::size_t dependentOffset = offset + base.sizeBytes;
    const ParseResult dependentHeader = eac3native::parseSyncframe(
        framed, dependentOffset);
    if (dependentHeader.disposition != Disposition::Accepted
        || !dependentHeader.frame) {
        first->disposition = AssemblyDisposition::Malformed;
        first->reason = "dependent-frame-parse";
        return false;
    }
    const FrameHeader &dependent = *dependentHeader.frame;
    const BsiParseResult bsi = eac3native::parseEac3Bsi(framed, dependent);
    if (bsi.disposition != Disposition::Accepted || !bsi.info) {
        first->disposition = AssemblyDisposition::Malformed;
        first->reason = "dependent-bsi-parse";
        return false;
    }
    if (dependent.streamType != StreamType::Dependent
        || dependent.substreamId != 0U || dependent.sampleRate != kRate
        || dependent.blocks != kBlocks || dependent.acmod != 5U
        || dependent.lfe || dependent.channelCount != kDependentChannels
        || !bsi.info->chanmape || bsi.info->chanmap != 0xA010U
        || bsi.info->chanmapChannelWeight != kDependentChannels) {
        first->disposition = AssemblyDisposition::Unsupported;
        first->reason = "config4-dependent-topology";
        return false;
    }
    const std::vector<std::uint8_t> baseBytes(
        framed.begin() + static_cast<std::ptrdiff_t>(offset),
        framed.begin() + static_cast<std::ptrdiff_t>(offset + base.sizeBytes));
    const legacyac3::Result basePcm = baseState
        ? (emitEos ? legacyac3::parseEnd(baseBytes, *baseState)
                   : legacyac3::parse(baseBytes, *baseState))
        : legacyac3::parse(baseBytes);
    const dependentpcm::PcmResult dependentPcm = dependentState
        ? (emitEos ? dependentpcm::decodeDependentEnd(
                         framed, dependent, *bsi.info, *dependentState)
                   : dependentpcm::decodeDependent(
                         framed, dependent, *bsi.info, *dependentState))
        : dependentpcm::decodeDependent(framed, dependent, *bsi.info);
    *first = combine(basePcm, dependentPcm, ordinal, emitEos);
    *nextOffset = dependentOffset + dependent.sizeBytes;
    if (first->disposition != AssemblyDisposition::Accepted) {
        return false;
    }
    if (syntaxOwner && !syntaxOwner->complete(
            *nextOffset, base.endBit, dependent.endBit)) {
        boundaryGuard.handled = true;
        first->disposition = AssemblyDisposition::Malformed;
        first->reason = "frame-syntax-boundary-complete";
        return false;
    }
    boundaryGuard.handled = true;
    return true;
}

std::vector<std::uint8_t> readFile(const std::string &path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    if (size <= 0) return {};
    input.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char *>(bytes.data()), size);
    return input ? bytes : std::vector<std::uint8_t>{};
}

class AssemblySession {
public:
    using Callback = std::function<bool(const AssemblyResult &)>;

    AssemblyDisposition open()
    {
        if (poisoned_) return AssemblyDisposition::Malformed;
        if (opened_) return AssemblyDisposition::Malformed;
        opened_ = true;
        canceled_ = false;
        flushed_ = false;
        return AssemblyDisposition::Accepted;
    }

    AssemblyDisposition push(const AssemblyResult &result,
                             const Callback &callback)
    {
        if (canceled_) return AssemblyDisposition::Canceled;
        if (poisoned_) return AssemblyDisposition::Malformed;
        if (!opened_ || flushed_ || !callback
            || result.disposition != AssemblyDisposition::Accepted) {
            poisoned_ = !flushed_;
            return AssemblyDisposition::Malformed;
        }
        if (!callback(result)) {
            poisoned_ = true;
            return AssemblyDisposition::Malformed;
        }
        return AssemblyDisposition::Accepted;
    }

    AssemblyDisposition preflight() const
    {
        if (canceled_) return AssemblyDisposition::Canceled;
        if (poisoned_ || !opened_ || flushed_)
            return AssemblyDisposition::Malformed;
        return AssemblyDisposition::Accepted;
    }

    AssemblyDisposition flush()
    {
        if (canceled_ || poisoned_ || !opened_ || flushed_)
            return canceled_ ? AssemblyDisposition::Canceled
                             : AssemblyDisposition::Malformed;
        flushed_ = true;
        return AssemblyDisposition::Accepted;
    }

    void cancel() { canceled_ = true; }
    void poison() { poisoned_ = true; }
    void reset()
    {
        opened_ = false;
        poisoned_ = false;
        canceled_ = false;
        flushed_ = false;
    }

private:
    bool opened_ = false;
    bool poisoned_ = false;
    bool canceled_ = false;
    bool flushed_ = false;
};

class StatefulAssemblyOwner {
public:
    using Callback = AssemblySession::Callback;

    AssemblyDisposition open()
    {
        return session_.open();
    }

    AssemblyDisposition processPair(const std::vector<std::uint8_t> &framed,
                                    std::size_t *offset, unsigned ordinal,
                                    bool emitEos, const Callback &callback)
    {
        if (!offset) return AssemblyDisposition::Malformed;
        const AssemblyDisposition ready = session_.preflight();
        if (ready != AssemblyDisposition::Accepted) return ready;
        AssemblyResult result;
        std::size_t nextOffset = *offset;
        if (!parsePair(framed, *offset, ordinal, &result, &nextOffset,
                       &baseState_, &dependentState_, emitEos,
                       &syntaxOwner_)) {
            reason_ = result.reason.empty() ? "stateful-pair-parse" : result.reason;
            session_.poison();
            return AssemblyDisposition::Malformed;
        }
        const AssemblyDisposition pushed = pushResult(result, callback);
        if (pushed != AssemblyDisposition::Accepted)
            reason_ = "assembly-callback-or-lifecycle";
        if (pushed == AssemblyDisposition::Accepted) *offset = nextOffset;
        return pushed;
    }

    AssemblyDisposition flush()
    {
        return session_.flush();
    }

    void cancel() { session_.cancel(); }

    AssemblyDisposition pushResult(const AssemblyResult &result,
                                   const Callback &callback)
    {
        return session_.push(result, callback);
    }

    bool decoderStateUsed() const
    {
        for (const auto &source : baseState_.ditherSources)
            if (source.generatedSamples() != 0U) return true;
        for (const auto &source : dependentState_.ditherSources)
            if (source.generatedSamples() != 0U) return true;
        return false;
    }

    bool decoderStateClear() const
    {
        if (decoderStateUsed()) return false;
        for (const auto &transform : baseState_.transformChannels)
            if (transform.hasPendingOverlap()) return false;
        for (const auto &transform : dependentState_.transformChannels)
            if (transform.hasPendingOverlap()) return false;
        return true;
    }

    bool syntaxBoundaryClosed() const
    {
        return syntaxOwner_.frameBoundaryClosed();
    }

    unsigned syntaxFramesCompleted() const
    {
        return syntaxOwner_.framesCompleted();
    }

    bool primeStateForSelfTest()
    {
        const std::vector<double> coefficients(
            eac3native::kEac3TransformCoefficients, 0.0);
        baseState_.ditherSources[0].next(0U, 0U);
        dependentState_.ditherSources[0].next(0U, 0U);
        const eac3native::TransformOutput baseBlock =
            baseState_.transformChannels[0].processBlock(coefficients, false);
        const eac3native::TransformOutput dependentBlock =
            dependentState_.transformChannels[0].processBlock(coefficients,
                                                              false);
        return baseBlock.ok && dependentBlock.ok
            && baseState_.transformChannels[0].hasPendingOverlap()
            && dependentState_.transformChannels[0].hasPendingOverlap();
    }

    void reset()
    {
        session_.reset();
        baseState_.reset();
        dependentState_.reset();
        syntaxOwner_.reset();
    }

    const std::string &reason() const { return reason_; }

private:
    AssemblySession session_;
    legacyac3::StatefulDecoder baseState_;
    dependentpcm::StatefulDecoder dependentState_;
    FrameSyntaxBoundaryOwner syntaxOwner_;
    std::string reason_;
};

DecodeReport decodeFile(const std::string &path, unsigned maxAUs,
                        const AssemblyCallback &callback)
{
    DecodeReport report;
    if (maxAUs == 0U || !callback) {
        report.reason = maxAUs == 0U ? "config4-pcm-max-aus-invalid"
                                     : "config4-pcm-callback-missing";
        return report;
    }
    const std::vector<std::uint8_t> bytes = readFile(path);
    if (bytes.empty()) {
        report.reason = "config4-pcm-input-read";
        return report;
    }
    const eac3native::Eb3FramingResult framing =
        eac3native::normalizeEb3Framing(bytes);
    if (!framing.ok || framing.normalized.empty()) {
        report.reason = "config4-pcm-framing-" + framing.reason;
        return report;
    }
    StatefulAssemblyOwner owner;
    if (owner.open() != AssemblyDisposition::Accepted) {
        report.reason = "config4-pcm-owner-open";
        return report;
    }
    std::size_t offset = 0U;
    for (; report.observedAUs < maxAUs; ++report.observedAUs) {
        const AssemblyDisposition disposition = owner.processPair(
            framing.normalized, &offset, report.observedAUs,
            report.observedAUs + 1U == maxAUs, callback);
        if (disposition != AssemblyDisposition::Accepted) {
            report.disposition = disposition;
            report.reason = owner.reason().empty()
                ? "config4-pcm-owner-process" : owner.reason();
            return report;
        }
        ++report.callbacks;
    }
    const AssemblyDisposition flushed = owner.flush();
    if (flushed != AssemblyDisposition::Accepted) {
        report.disposition = flushed;
        report.reason = owner.reason().empty()
            ? "config4-pcm-owner-flush" : owner.reason();
        return report;
    }
    report.disposition = AssemblyDisposition::Accepted;
    report.reason = "config4-pcm-decode-accepted";
    return report;
}

legacyac3::Result syntheticBase()
{
    legacyac3::Result result;
    result.disposition = legacyac3::Disposition::Accepted;
    result.pcm.resize(kBaseChannels);
    for (unsigned channel = 0U; channel < kBaseChannels; ++channel) {
        result.pcm[channel].channel = channel;
        result.pcm[channel].lfe = channel == 5U;
        result.pcm[channel].sampleCount = kSamplesPerAu;
        result.pcm[channel].eosTailCount = kSamplesPerTail;
        result.pcm[channel].digest = 0x100U + channel;
        result.pcm[channel].eosDigest = 0x200U + channel;
        result.pcm[channel].peak = 0.25;
        result.pcm[channel].rms = 0.125;
        result.pcm[channel].eosPeak = 0.125;
        result.pcm[channel].eosRms = 0.0625;
        result.pcm[channel].samples.assign(
            kSamplesPerAu, 0.25 + static_cast<double>(channel) * 0.01);
        result.pcm[channel].eosTail.assign(
            kSamplesPerTail, 0.125 + static_cast<double>(channel) * 0.01);
    }
    return result;
}

dependentpcm::PcmResult syntheticDependent()
{
    dependentpcm::PcmResult result;
    result.disposition = dependentpcm::Disposition::Accepted;
    result.channels.resize(kDependentChannels);
    for (unsigned channel = 0U; channel < kDependentChannels; ++channel) {
        result.channels[channel].channel = channel;
        result.channels[channel].location =
            dependentpcm::kLocations[channel];
        result.channels[channel].sampleCount = kSamplesPerAu;
        result.channels[channel].eosTailCount = kSamplesPerTail;
        result.channels[channel].digest = 0x300U + channel;
        result.channels[channel].eosDigest = 0x400U + channel;
        result.channels[channel].peak = 0.5;
        result.channels[channel].rms = 0.25;
        result.channels[channel].eosPeak = 0.25;
        result.channels[channel].eosRms = 0.125;
        result.channels[channel].samples.assign(
            kSamplesPerAu, 0.5 + static_cast<double>(channel) * 0.01);
        result.channels[channel].eosTail.assign(
            kSamplesPerTail, 0.25 + static_cast<double>(channel) * 0.01);
    }
    return result;
}

bool selfTest()
{
    auto baseSource = syntheticBase();
    auto dependentSource = syntheticDependent();
    AssemblyResult synthetic = combine(baseSource, dependentSource, 0U);
    if (synthetic.disposition != AssemblyDisposition::Accepted
        || synthetic.channels.size() != kChannels) {
        std::cerr << "selfTest=FAIL case=ordering-shape\n";
        return false;
    }
    bool ordering = true;
    for (unsigned channel = 0U; channel < kChannels; ++channel)
        ordering = ordering && synthetic.channels[channel].channel == channel
            && synthetic.channels[channel].location == kLocations[channel];
    for (unsigned left = 0U; left < kChannels; ++left) {
        for (unsigned right = left + 1U; right < kChannels; ++right)
            ordering = ordering
                && std::string(synthetic.channels[left].location)
                    != std::string(synthetic.channels[right].location);
    }
    if (!ordering) {
        std::cerr << "selfTest=FAIL case=ordering-identity\n";
        return false;
    }
    auto mismatched = syntheticDependent();
    mismatched.channels.pop_back();
    if (combine(syntheticBase(), mismatched, 0U).reason
        != "config4-channel-count") {
        std::cerr << "selfTest=FAIL case=mismatch\n";
        return false;
    }
    bool tailSeparated = true;
    for (const ChannelSummary &channel : synthetic.channels)
        tailSeparated = tailSeparated
            && channel.sampleCount != channel.eosTailCount
            && finiteSummary(channel);
    if (!tailSeparated) {
        std::cerr << "selfTest=FAIL case=tail-separation\n";
        return false;
    }
    bool noAlias = true;
    for (unsigned left = 0U; left < kChannels; ++left) {
        for (unsigned right = left + 1U; right < kChannels; ++right)
            noAlias = noAlias
                && synthetic.channels[left].samples.data()
                    != synthetic.channels[right].samples.data()
                && synthetic.channels[left].eosTail.data()
                    != synthetic.channels[right].eosTail.data();
    }
    const double sourceBefore = baseSource.pcm[0].samples[0];
    const double assembledOtherBefore = synthetic.channels[1].samples[0];
    const double dependentSourceBefore = dependentSource.channels[0].samples[0];
    synthetic.channels[0].samples[0] = -1.0;
    noAlias = noAlias
        && synthetic.channels[1].samples[0] == assembledOtherBefore
        && baseSource.pcm[0].samples[0] == sourceBefore
        && dependentSource.channels[0].samples[0] == dependentSourceBefore;
    if (!noAlias) {
        std::cerr << "selfTest=FAIL case=no-alias\n";
        return false;
    }
    legacyac3::StatefulDecoder stateOwner;
    stateOwner.ditherSources[0].next(0U, 0U);
    const std::vector<double> continuityCoefficients(
        eac3native::kEac3TransformCoefficients, 0.0);
    const eac3native::TransformOutput continuityFirst =
        stateOwner.transformChannels[0].processBlock(continuityCoefficients,
                                                     false);
    const eac3native::TransformOutput continuitySecond =
        stateOwner.transformChannels[0].processBlock(continuityCoefficients,
                                                     false);
    const eac3native::TransformFlushOutput continuityTail =
        stateOwner.transformChannels[0].flush();
    const bool statefulOwner = continuityFirst.ok && continuitySecond.ok
        && continuityFirst.samples.size() == eac3native::kEac3OutputSamples
        && continuitySecond.samples.size() == eac3native::kEac3OutputSamples
        && continuityTail.ok && continuityTail.hadTail
        && continuityTail.samples.size() == eac3native::kEac3OutputSamples
        && stateOwner.ditherSources[0].generatedSamples() == 1U;
    stateOwner.reset();
    if (!statefulOwner || stateOwner.transformChannels[0].hasPendingOverlap()
        || stateOwner.ditherSources[0].generatedSamples() != 0U) {
        std::cerr << "selfTest=FAIL case=stateful-owner\n";
        return false;
    }
    std::size_t callbackCount = 0U;
    AssemblySession session;
    if (session.open() != AssemblyDisposition::Accepted
        || session.push(synthetic, [&callbackCount](const AssemblyResult &value) {
               ++callbackCount;
               return value.baseSampleStart == value.dependentSampleStart
                   && value.channels.size() == kChannels;
           }) != AssemblyDisposition::Accepted
        || session.flush() != AssemblyDisposition::Accepted
        || callbackCount != 1U) {
        std::cerr << "selfTest=FAIL case=lifecycle-accept\n";
        return false;
    }
    if (session.push(synthetic, [](const AssemblyResult &) { return true; })
            != AssemblyDisposition::Malformed) {
        std::cerr << "selfTest=FAIL case=post-flush\n";
        return false;
    }
    session.reset();
    session.open();
    session.cancel();
    if (session.push(synthetic, [](const AssemblyResult &) { return true; })
            != AssemblyDisposition::Canceled) {
        std::cerr << "selfTest=FAIL case=cancel\n";
        return false;
    }
    session.reset();
    session.open();
    if (session.push(synthetic, [](const AssemblyResult &) { return false; })
            != AssemblyDisposition::Malformed
        || session.flush() != AssemblyDisposition::Malformed) {
        std::cerr << "selfTest=FAIL case=backpressure-poison\n";
        return false;
    }
    StatefulAssemblyOwner owner;
    if (owner.open() != AssemblyDisposition::Accepted
        || owner.pushResult(synthetic, [](const AssemblyResult &) {
               return false;
           }) != AssemblyDisposition::Malformed
        || owner.pushResult(synthetic, [](const AssemblyResult &) {
               return true;
           }) != AssemblyDisposition::Malformed
        || owner.flush() != AssemblyDisposition::Malformed) {
        std::cerr << "selfTest=FAIL case=owner-callback-poison\n";
        return false;
    }
    owner.reset();
    std::size_t canceledOffset = 0U;
    owner.open();
    owner.cancel();
    if (owner.processPair({}, &canceledOffset, 0U, false,
                          [](const AssemblyResult &) { return true; })
            != AssemblyDisposition::Canceled
        || owner.flush() != AssemblyDisposition::Canceled) {
        std::cerr << "selfTest=FAIL case=owner-cancel\n";
        return false;
    }
    owner.reset();
    std::size_t failedOffset = 0U;
    owner.open();
    const bool ownerParsePoison =
        owner.processPair({0U}, &failedOffset, 0U, false,
                          [](const AssemblyResult &) { return true; })
            == AssemblyDisposition::Malformed
        && !owner.syntaxBoundaryClosed()
        && owner.pushResult(synthetic, [](const AssemblyResult &) {
               return true;
           }) == AssemblyDisposition::Malformed
        && owner.flush() == AssemblyDisposition::Malformed;
    if (!ownerParsePoison) {
        std::cerr << "selfTest=FAIL case=owner-parse-poison\n";
        return false;
    }
    std::size_t ownerCallbacks = 0U;
    owner.reset();
    const bool ownerPrimed = owner.primeStateForSelfTest()
        && owner.decoderStateUsed();
    owner.reset();
    const bool ownerReset = owner.decoderStateClear()
        && owner.syntaxBoundaryClosed()
        && owner.open() == AssemblyDisposition::Accepted
        && owner.pushResult(synthetic, [&ownerCallbacks](const AssemblyResult &value) {
               ++ownerCallbacks;
               return value.channels.size() == kChannels
                   && value.baseSampleStart == value.dependentSampleStart;
           }) == AssemblyDisposition::Accepted
        && owner.flush() == AssemblyDisposition::Accepted
        && ownerCallbacks == 1U;
    if (!ownerPrimed || !ownerReset) {
        std::cerr << "selfTest=FAIL case=owner-reset-reopen\n";
        return false;
    }
    owner.reset();
    FrameSyntaxBoundaryOwner syntaxOwner;
    const bool syntaxBoundary = syntaxOwner.begin(0U, 0U)
        && !syntaxOwner.begin(1U, 100U)
        && syntaxOwner.complete(50U, 32U, 40U)
        && syntaxOwner.framesCompleted() == 1U
        && syntaxOwner.begin(1U, 50U)
        && syntaxOwner.complete(100U, 80U, 96U)
        && syntaxOwner.frameBoundaryClosed();
    syntaxOwner.reset();
    FrameSyntaxBoundaryOwner abortedBoundary;
    const bool syntaxAbortStarted = abortedBoundary.begin(0U, 0U);
    abortedBoundary.abort();
    const bool syntaxAbortLocked = syntaxAbortStarted
        && !abortedBoundary.begin(0U, 0U)
        && !abortedBoundary.complete(10U, 8U, 10U)
        && !abortedBoundary.frameBoundaryClosed();
    abortedBoundary.reset();
    const bool syntaxAbortReset = abortedBoundary.frameBoundaryClosed()
        && abortedBoundary.begin(0U, 0U)
        && abortedBoundary.complete(10U, 8U, 10U)
        && abortedBoundary.frameBoundaryClosed();
    if (!ownerParsePoison || !syntaxBoundary || !syntaxOwner.frameBoundaryClosed()
        || !syntaxAbortLocked || !syntaxAbortReset) {
        std::cerr << "selfTest=FAIL case=frame-syntax-boundary\n";
        return false;
    }
    std::cout << "selfTest=PASS cases=17 ordering=PASS noAlias=PASS"
              << " mismatch=PASS tailSeparation=PASS lifecycle=PASS"
              << " statefulOwner=PASS ownerLifecycle=PASS"
              << " syntaxBoundary=PASS abortReset=PASS"
              << " earlyParsePoison=PASS multiAuContinuity=PASS\n";
    return true;
}

bool runFile(const std::string &path)
{
    const std::vector<std::uint8_t> bytes = readFile(path);
    if (bytes.empty()) {
        std::cerr << "probeResult=FAIL reason=input-read\n";
        return false;
    }
    const eac3native::Eb3FramingResult framing =
        eac3native::normalizeEb3Framing(bytes);
    if (!framing.ok || framing.normalized.empty()) {
        std::cerr << "probeResult=FAIL reason=framing-" << framing.reason << '\n';
        return false;
    }
    const std::vector<std::uint8_t> &framed = framing.normalized;
    std::size_t offset = 0U;
    StatefulAssemblyOwner owner;
    if (owner.open() != AssemblyDisposition::Accepted) {
        std::cerr << "probeResult=FAIL reason=stateful-owner-open\n";
        return false;
    }
    std::size_t callbackCount = 0U;
    bool callbackValid = false;
    const StatefulAssemblyOwner::Callback callback =
        [&callbackCount, &callbackValid](const AssemblyResult &value) {
            ++callbackCount;
            const bool eosExpected = value.auOrdinal == 2U;
            callbackValid = value.channels.size() == kChannels
                && value.baseSampleStart
                    == static_cast<std::size_t>(value.auOrdinal) * kSamplesPerAu
                && value.dependentSampleStart == value.baseSampleStart;
            for (unsigned channel = 0U; channel < value.channels.size(); ++channel)
                callbackValid = callbackValid
                    && value.channels[channel].channel == channel
                    && value.channels[channel].location == kLocations[channel]
                    && finiteSummary(value.channels[channel], eosExpected);
            for (unsigned left = 0U; left < value.channels.size(); ++left) {
                for (unsigned right = left + 1U;
                     right < value.channels.size(); ++right) {
                    const auto &leftChannel = value.channels[left];
                    const auto &rightChannel = value.channels[right];
                    if (!leftChannel.samples.empty()
                        && !rightChannel.samples.empty()
                        && leftChannel.samples.data()
                            == rightChannel.samples.data())
                        callbackValid = false;
                    if (eosExpected && !leftChannel.eosTail.empty()
                        && !rightChannel.eosTail.empty()
                        && leftChannel.eosTail.data()
                            == rightChannel.eosTail.data())
                        callbackValid = false;
                }
            }
            std::cout << "au=" << value.auOrdinal
                      << " sampleStart=" << value.baseSampleStart
                      << " channels=" << value.channels.size()
                      << " samplesPerChannel="
                      << (value.channels.empty()
                              ? 0U : value.channels.front().sampleCount)
                      << " eosTailPerChannel="
                      << (value.channels.empty()
                              ? 0U : value.channels.front().eosTailCount)
                      << " digest=0x" << std::hex << std::setfill('0')
                      << std::setw(16) << value.digest << std::dec << '\n';
            if (eosExpected) {
                for (const ChannelSummary &channel : value.channels)
                    std::cout << "channel=" << channel.channel
                              << " location=" << channel.location
                              << " sampleCount=" << channel.sampleCount
                              << " digest=0x" << std::hex << std::setfill('0')
                              << std::setw(16) << channel.digest << std::dec
                              << " eosTailCount=" << channel.eosTailCount
                              << " eosDigest=0x" << std::hex
                              << std::setw(16) << channel.eosDigest << std::dec
                              << '\n';
            }
            return callbackValid;
        };
    AssemblyDisposition lastPush = AssemblyDisposition::Malformed;
    unsigned observedAUs = 0U;
    for (; observedAUs < 3U; ++observedAUs) {
        lastPush = owner.processPair(framed, &offset, observedAUs,
                                     observedAUs == 2U, callback);
        if (lastPush != AssemblyDisposition::Accepted) break;
    }
    const AssemblyDisposition flushed = owner.flush();
    if (observedAUs != 3U)
        std::cerr << "statefulOwnerFailure=" << owner.reason() << '\n';
    const bool pass = observedAUs == 3U
        && lastPush == AssemblyDisposition::Accepted
        && flushed == AssemblyDisposition::Accepted
        && callbackCount == 3U && callbackValid
        && owner.syntaxFramesCompleted() == 3U
        && owner.syntaxBoundaryClosed();
    std::cout << "observedAUs=" << observedAUs << " assembledAUs="
              << callbackCount
              << " statefulContinuity=" << (pass ? "PASS" : "FAIL")
              << " eos=FINAL_AU_ONLY\n"
              << "callbackCount=" << callbackCount
              << " callbackBoundary=" << (callbackValid ? "PASS" : "FAIL")
              << " statefulOwner=PASS syntaxOwnership=FRAME_LOCAL_RESET\n";
    std::cout << "pcmAvailability=PCM_AVAILABLE_DIAGNOSTIC_CONFIG4_ASSEMBLY"
              << " productionAcceptance=INCONCLUSIVE\n"
              << "drcApplied=NO j0Applied=NO rendererApplied=NO ffmpegLinked=NO\n"
              << "probeResult=" << (pass ? "PASS" : "FAIL")
              << " stage=gate8n-5j-config4-frame-boundary\n";
    return pass;
}

} // namespace config4pcm

#ifndef EAC3_CONFIG4_PCM_NO_MAIN
int main(int argc, char **argv)
{
    if (argc == 2 && std::string(argv[1]) == "--self-test")
        return config4pcm::selfTest() ? 0 : 1;
    if (argc != 2) {
        std::cerr << "Usage: Eac3NativeConfig4PcmAssemblyProbe <raw.eac3|raw.eb3>\n"
                     "       Eac3NativeConfig4PcmAssemblyProbe --self-test\n";
        return 2;
    }
    return config4pcm::runFile(argv[1]) ? 0 : 1;
}
#endif
