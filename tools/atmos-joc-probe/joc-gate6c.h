#pragma once

#include "joc-synthesis.h"
#include "oamd-b2b.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace eac3gate6c {

constexpr unsigned kDynamicObjectCount = 15U;
constexpr std::size_t kSamplesPerUnit = 1536U;
constexpr int kCommonDelaySamples = eac3joc::kSynthesisAlgorithmicDelaySamples;

struct MetadataUpdate {
    std::int64_t sourcePosition = 0;
    unsigned blockIndex = 0;
    unsigned rampDuration = 0;
    unsigned objectIndex = 0;
    eac3oamd::B2bObjectState state;
};

struct UnitInput {
    std::size_t unitIndex = 0;
    std::int64_t sourceStart = 0;
    eac3joc::JocQmfFrame qmf;
    std::vector<float> lfe;
    std::vector<MetadataUpdate> metadata;
};

struct Batch {
    std::size_t unitIndex = 0;
    bool flush = false;
    std::int64_t sourceStart = 0;
    std::int64_t sourceEnd = 0;
    std::int64_t outputStart = 0;
    std::int64_t outputEnd = 0;
    std::vector<std::vector<float>> objects;
    std::vector<float> lfe;
    std::vector<MetadataUpdate> metadata;
};

struct Config {
    std::vector<double> qwin;
    std::int64_t decodedSourceSamples = 0;
    std::int64_t skipSamples = 0;
    std::int64_t discardPadding = 0;
    std::size_t callbackCapacity = 1;
};

struct Report {
    bool pass = false;
    std::size_t cases = 0;
    std::size_t unitsAccepted = 0;
    std::size_t batchesEmitted = 0;
    std::size_t metadataEmitted = 0;
    std::size_t flushes = 0;
    std::size_t callbackRejects = 0;
    std::size_t transactionalRejects = 0;
    std::size_t resetCases = 0;
    std::size_t mappingCases = 0;
    std::size_t timingCases = 0;
    std::size_t metadataCases = 0;
    std::size_t flushCases = 0;
    std::size_t callbackCases = 0;
    std::size_t conversionCases = 0;
    std::string reason;
};

bool decodeSampleOffset(const eac3oamd::B2aFrame &frame,
                        std::int64_t *samples, std::string *reason);
bool decodeRampDuration(const eac3oamd::B2aBlockUpdate &block,
                        unsigned *samples, std::string *reason);

using Callback = std::function<bool(const Batch &)>;

class Assembler {
public:
    explicit Assembler(Config config = {});

    bool processUnit(const UnitInput &input, const Callback &callback,
                     std::string *reason);
    bool flush(const Callback &callback, std::string *reason);
    void reset();

    const Report &report() const { return report_; }

private:
    Config config_;
    eac3joc::JocSynthesisState synthesis_;
    bool haveUnit_ = false;
    bool flushed_ = false;
    std::size_t nextUnitIndex_ = 0;
    std::int64_t nextSourceStart_ = 0;
    std::vector<float> lfeDelay_;
    std::vector<MetadataUpdate> carriedMetadata_;
    std::vector<MetadataUpdate> latestPreTrim_;
    std::vector<bool> latestPreTrimValid_;
    bool preTrimBoundaryEmitted_ = false;
    Report report_;
};

Report runSelfTest(const std::vector<double> &qwin);

} // namespace eac3gate6c
