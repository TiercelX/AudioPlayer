#include "joc-matrix-trace.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>

namespace eac3joctrace {
namespace {

constexpr unsigned kTimeslots = 24;
constexpr unsigned kSubbands = 64;
constexpr unsigned kSamplesPerUnit = kTimeslots * kSubbands;
constexpr std::array<unsigned, 5> kRepresentativeSubbands = {0, 16, 32, 48, 63};
// At 48 kHz a 64-band QMF has nominal 375 Hz bands. These whole-band
// diagnostics cannot resolve <=80 Hz versus 80-200 Hz boundaries.
constexpr std::array<unsigned, 2> kLowAuditSubbands = {0, 1};

bool validComplex(const eac3qmf::Complex &value)
{
    return std::isfinite(value.real()) && std::isfinite(value.imag());
}

std::string jsonString(const std::string &value)
{
    std::ostringstream result;
    result << '"';
    for (const unsigned char character : value) {
        switch (character) {
        case '\\': result << "\\\\"; break;
        case '"': result << "\\\""; break;
        case '\n': result << "\\n"; break;
        case '\r': result << "\\r"; break;
        case '\t': result << "\\t"; break;
        default:
            if (character < 0x20U) {
                result << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                       << static_cast<unsigned>(character) << std::dec << std::setfill(' ');
            } else {
                result << static_cast<char>(character);
            }
            break;
        }
    }
    result << '"';
    return result.str();
}

std::string number(double value)
{
    if (!std::isfinite(value)) return "null";
    std::ostringstream result;
    result << std::setprecision(std::numeric_limits<double>::max_digits10)
           << std::scientific << value;
    return result.str();
}

std::uint64_t hashBytes(std::uint64_t hash, const std::uint8_t *data, std::size_t size)
{
    constexpr std::uint64_t kPrime = 1099511628211ULL;
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= data[index];
        hash *= kPrime;
    }
    return hash;
}

std::string fileDigest(const std::string &path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) return "unavailable";
    std::array<std::uint8_t, 4096> buffer {};
    std::uint64_t hash = 1469598103934665603ULL;
    while (input) {
        input.read(reinterpret_cast<char *>(buffer.data()), buffer.size());
        const std::streamsize count = input.gcount();
        if (count > 0) hash = hashBytes(hash, buffer.data(), static_cast<std::size_t>(count));
    }
    std::ostringstream result;
    result << "fnv1a64-" << std::hex << std::setw(16) << std::setfill('0') << hash;
    return result.str();
}

std::string qwinDigest(const std::vector<double> &qwin)
{
    std::uint64_t hash = 1469598103934665603ULL;
    for (const double value : qwin) {
        std::uint64_t bits = 0;
        static_assert(sizeof(bits) == sizeof(value));
        std::memcpy(&bits, &value, sizeof(bits));
        hash = hashBytes(hash, reinterpret_cast<const std::uint8_t *>(&bits), sizeof(bits));
    }
    std::ostringstream result;
    result << "fnv1a64-" << std::hex << std::setw(16) << std::setfill('0') << hash;
    return result.str();
}

template <typename T>
void writeScalarArray(std::ostream &out, const std::vector<T> &values)
{
    out << '[';
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index) out << ',';
        out << values[index];
    }
    out << ']';
}

void writeDoubleArray(std::ostream &out, const std::vector<double> &values)
{
    out << '[';
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index) out << ',';
        out << number(values[index]);
    }
    out << ']';
}

void writeBoolArray(std::ostream &out, const std::vector<bool> &values)
{
    out << '[';
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index) out << ',';
        out << (values[index] ? "true" : "false");
    }
    out << ']';
}

} // namespace

ReconstructionCheck verifyReconstruction(const eac3joc::JocMathFrame &math,
                                         const eac3joc::JocQmfFrame &qmf,
                                         double mismatchTolerance)
{
    ReconstructionCheck result;
    double differenceEnergy = 0.0;
    double referenceEnergy = 0.0;
    if (qmf.inputQmf.size() != math.numChannels || qmf.objects.size() != math.numObjects) {
        result.mismatchCount = 1;
        return result;
    }
    for (unsigned object = 0; object < math.numObjects; ++object) {
        if (qmf.objects[object].size() != kSamplesPerUnit) {
            ++result.mismatchCount;
            continue;
        }
        for (unsigned timeslot = 0; timeslot < kTimeslots; ++timeslot) {
            for (unsigned subband = 0; subband < kSubbands; ++subband) {
                eac3qmf::Complex expected {0.0, 0.0};
                const bool present = object < math.interpolated.size()
                    && math.interpolated[object].present;
                if (present && object < math.interpolated.size()
                    && timeslot < math.interpolated[object].values.size()) {
                    for (unsigned channel = 0; channel < math.numChannels; ++channel) {
                        if (channel >= qmf.inputQmf.size()
                            || qmf.inputQmf[channel].size() != kSamplesPerUnit
                            || channel >= math.interpolated[object].values[timeslot].size()
                            || subband >= math.interpolated[object].values[timeslot][channel].size()) {
                            ++result.mismatchCount;
                            continue;
                        }
                        expected += math.interpolated[object].values[timeslot][channel][subband]
                            * qmf.inputQmf[channel][timeslot * kSubbands + subband];
                    }
                }
                const eac3qmf::Complex actual =
                    qmf.objects[object][timeslot * kSubbands + subband];
                const double difference = std::abs(actual - expected);
                result.maxAbs = std::max(result.maxAbs, difference);
                differenceEnergy += difference * difference;
                referenceEnergy += std::norm(expected);
                ++result.comparedCount;
                if (!validComplex(expected) || !validComplex(actual)
                    || !std::isfinite(difference) || difference > mismatchTolerance) {
                    ++result.mismatchCount;
                }
            }
        }
    }
    result.relativeRms = std::sqrt(differenceEnergy
        / std::max(referenceEnergy, std::numeric_limits<double>::min()));
    return result;
}

bool runSelfTest(std::string *reason)
{
    if (!reason) return false;
    eac3joc::JocMathFrame math;
    math.numChannels = 2;
    math.numObjects = 1;
    math.interpolated.resize(1);
    math.interpolated[0].present = true;
    math.interpolated[0].numChannels = 2;
    math.interpolated[0].values.assign(kTimeslots,
        std::vector<std::vector<double>>(2, std::vector<double>(kSubbands, 0.0)));
    eac3joc::JocQmfFrame qmf;
    qmf.numChannels = 2;
    qmf.numObjects = 1;
    qmf.inputQmf.assign(2, std::vector<eac3qmf::Complex>(kSamplesPerUnit));
    qmf.objects.assign(1, std::vector<eac3qmf::Complex>(kSamplesPerUnit));
    for (unsigned timeslot = 0; timeslot < kTimeslots; ++timeslot) {
        for (unsigned subband = 0; subband < kSubbands; ++subband) {
            for (unsigned channel = 0; channel < 2; ++channel) {
                const eac3qmf::Complex input {
                    0.1 * (channel + 1U) + 0.001 * subband,
                    -0.03 * (timeslot + 1U)};
                qmf.inputQmf[channel][timeslot * kSubbands + subband] = input;
                math.interpolated[0].values[timeslot][channel][subband]
                    = channel == 0 ? 0.25 : -0.5;
                qmf.objects[0][timeslot * kSubbands + subband] +=
                    math.interpolated[0].values[timeslot][channel][subband] * input;
            }
        }
    }
    if (verifyReconstruction(math, qmf).mismatchCount != 0) {
        *reason = "correct-qout-was-rejected";
        return false;
    }
    qmf.objects[0][17] += eac3qmf::Complex {0.25, 0.0};
    if (verifyReconstruction(math, qmf).mismatchCount == 0) {
        *reason = "tampered-qout-was-accepted";
        return false;
    }
    math.interpolated[0].values[0][0][0] =
        std::numeric_limits<double>::quiet_NaN();
    if (verifyReconstruction(math, qmf).mismatchCount == 0) {
        *reason = "nonfinite-expected-was-accepted";
        return false;
    }
    *reason = "independent-qin-matrix-reconstruction-tamper-and-nonfinite-rejection";
    return true;
}

struct Writer::Impl {
    std::ofstream output;
    std::vector<bool> previousPresent;
    std::size_t records = 0;
};

Writer::~Writer()
{
    if (impl_) {
        impl_->output.close();
        delete impl_;
    }
}

bool Writer::open(const std::string &outputPath,
                  const std::string &sourcePath,
                  const std::vector<double> &qwin,
                  std::string *reason)
{
    if (!reason || impl_) return false;
    auto *impl = new Impl;
    const std::filesystem::path path(outputPath);
    std::error_code error;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        *reason = "trace-directory-create-failed:" + error.message();
        delete impl;
        return false;
    }
    impl->output.open(path, std::ios::out | std::ios::trunc);
    if (!impl->output) {
        *reason = "trace-open-failed";
        delete impl;
        return false;
    }
    impl_ = impl;
    impl_->output << "{\"recordType\":\"provenance\",\"schema\":\"joc-matrix-trace-v1\","
                  << "\"sourcePath\":" << jsonString(sourcePath)
                  << ",\"sourceFileDigest\":" << jsonString(fileDigest(sourcePath))
                  << ",\"qwinCoefficientCount\":" << qwin.size()
                  << ",\"qwinDigest\":" << jsonString(qwinDigest(qwin))
                  << ",\"qmfTimeslots\":24,\"qmfSubbands\":64,"
                  << "\"qmfSampleRateHz\":48000,\"qmfNominalBandSpacingHz\":375,"
                  << "\"lowBandAuditSubbands\":[0,1],"
                  << "\"lowBandBoundaryLimitation\":\"64-band QMF nominal bands are 0-375 and 375-750 Hz; <=80 and 80-200 Hz cannot be separated here\","
                  << "\"forensicContract\":\"gate5a-header-data-ranges-full-codewords;"
                     "offsetTs-vs-symbol-data-ranges;"
                     "fixed-channel-vs-huffman-delta-vs-raw-coefficient;"
                     "symbol-parameter-band-and-channel;"
                     "payload-hex-codeword-verifiable;"
                     "q-is-differential-quantized\","
                  << "\"reconstructionContract\":\"independent-sum-of-Qin-times-interp\"}\n";
    if (!impl_->output) {
        *reason = "trace-provenance-write-failed";
        return false;
    }
    return true;
}

bool Writer::append(std::size_t unitIndex,
                    std::int64_t sourceStart,
                    const eac3joc::FrameReport &report,
                    const eac3joc::JocMathFrame &math,
                    const eac3joc::JocQmfFrame &qmf,
                    const std::vector<float> &lfe,
                    std::string *reason)
{
    if (!impl_ || !impl_->output || !reason) return false;
    if (qmf.config != report.downmixConfigIndex || qmf.inputQmf.size() != math.numChannels) {
        *reason = "trace-frame-shape-mismatch";
        return false;
    }
    if (lfe.size() != kSamplesPerUnit
        || !std::all_of(lfe.begin(), lfe.end(),
                        [](float value) { return std::isfinite(value); })) {
        *reason = "trace-lfe-shape-or-finite-mismatch";
        return false;
    }
    if (!report.forensic || report.forensic->objects.size() != math.numObjects) {
        *reason = "trace-forensic-capture-missing";
        return false;
    }
    const ReconstructionCheck check = verifyReconstruction(math, qmf);
    const auto &identities = eac3joc::inputIdentities(report.downmixConfigIndex);
    impl_->output << "{\"recordType\":\"au\",\"unit\":" << unitIndex
                  << ",\"sourceStart\":" << sourceStart
                  << ",\"jocSequenceCount\":" << report.sequenceCount
                  << ",\"config\":" << report.downmixConfigIndex
                  << ",\"numChannels\":" << math.numChannels
                  << ",\"numObjects\":" << math.numObjects
                  << ",\"payloadBitCount\":" << report.forensic->payloadBitCount
                  << ",\"payloadHex\":" << jsonString(report.forensic->payloadHex)
                  << ",\"syntaxBitEnd\":" << report.forensic->syntaxBitEnd
                  << ",\"stateReset\":" << (math.stateReset || qmf.stateReset ? "true" : "false")
                  << ",\"sequenceDiscontinuity\":" << (math.sequenceDiscontinuity ? "true" : "false")
                  << ",\"inputIdentities\":[";
    for (std::size_t channel = 0; channel < identities.size(); ++channel) {
        if (channel) impl_->output << ',';
        impl_->output << jsonString(eac3joc::channelIdentityText(identities[channel]));
    }
    impl_->output << "],\"qin\":[";
    for (unsigned channel = 0; channel < math.numChannels; ++channel) {
        if (channel) impl_->output << ',';
        double energy = 0.0;
        for (const auto &value : qmf.inputQmf[channel]) energy += std::norm(value);
        impl_->output << "{\"identity\":"
                      << jsonString(channel < identities.size()
                          ? eac3joc::channelIdentityText(identities[channel]) : "UNKNOWN")
                      << ",\"energy\":" << number(energy)
                      << ",\"rms\":" << number(std::sqrt(energy / kSamplesPerUnit)) << '}';
    }
    impl_->output << "],\"qmfLowSubbands\":[";
    for (std::size_t lowIndex = 0; lowIndex < kLowAuditSubbands.size(); ++lowIndex) {
        if (lowIndex) impl_->output << ',';
        const unsigned subband = kLowAuditSubbands[lowIndex];
        impl_->output << "{\"subband\":" << subband
                      << ",\"nominalLowHz\":" << number(375.0 * subband)
                      << ",\"nominalHighHz\":" << number(375.0 * (subband + 1U))
                      << ",\"qinEnergy\":[";
        for (unsigned channel = 0; channel < math.numChannels; ++channel) {
            if (channel) impl_->output << ',';
            double energy = 0.0;
            for (unsigned timeslot = 0; timeslot < kTimeslots; ++timeslot) {
                energy += std::norm(qmf.inputQmf[channel][timeslot * kSubbands + subband]);
            }
            impl_->output << number(energy);
        }
        impl_->output << "],\"objects\":[";
        for (unsigned object = 0; object < math.numObjects; ++object) {
            if (object) impl_->output << ',';
            const bool present = object < math.interpolated.size()
                && math.interpolated[object].present;
            double incoherent = 0.0;
            double coherent = 0.0;
            double actual = 0.0;
            double maxRowNormSquared = 0.0;
            double maxCoefficient = 0.0;
            std::size_t maxNonzeroCoefficients = 0;
            std::size_t multiInputRows = 0;
            for (unsigned timeslot = 0; timeslot < kTimeslots; ++timeslot) {
                const std::size_t offset = timeslot * kSubbands + subband;
                if (object < qmf.objects.size() && offset < qmf.objects[object].size()) {
                    actual += std::norm(qmf.objects[object][offset]);
                }
                eac3qmf::Complex sum {0.0, 0.0};
                double rowNormSquared = 0.0;
                std::size_t nonzeroCoefficients = 0;
                if (present && object < math.interpolated.size()
                    && timeslot < math.interpolated[object].values.size()) {
                    for (unsigned channel = 0; channel < math.numChannels; ++channel) {
                        const double coefficient = math.interpolated[object]
                            .values[timeslot][channel][subband];
                        const eac3qmf::Complex contribution = coefficient
                            * qmf.inputQmf[channel][offset];
                        sum += contribution;
                        incoherent += std::norm(contribution);
                        rowNormSquared += coefficient * coefficient;
                        maxCoefficient = std::max(maxCoefficient, std::abs(coefficient));
                        if (std::abs(coefficient) > 1.0e-12) ++nonzeroCoefficients;
                    }
                }
                maxNonzeroCoefficients = std::max(maxNonzeroCoefficients, nonzeroCoefficients);
                if (nonzeroCoefficients > 1) ++multiInputRows;
                coherent += std::norm(sum);
                maxRowNormSquared = std::max(maxRowNormSquared, rowNormSquared);
            }
            impl_->output << "{\"object\":" << object
                          << ",\"present\":" << (present ? "true" : "false")
                          << ",\"incoherentBaselineEnergy\":" << number(incoherent)
                          << ",\"coherentExpectedEnergy\":" << number(coherent)
                          << ",\"actualQoutEnergy\":" << number(actual)
                          << ",\"crossTermEnergy\":" << number(coherent - incoherent)
                          << ",\"actualToIncoherentRatio\":"
                          << (incoherent > 0.0 ? number(actual / incoherent) : "null")
                          << ",\"maxRowNorm\":" << number(std::sqrt(maxRowNormSquared))
                          << ",\"maxNonzeroCoefficientsPerRow\":" << maxNonzeroCoefficients
                          << ",\"multiInputRowTimeslots\":" << multiInputRows
                          << ",\"maxAbsCoefficient\":" << number(maxCoefficient) << '}';
        }
        impl_->output << "]}";
    }
    double lfeEnergy = 0.0;
    double lfePeak = 0.0;
    std::size_t lfeNonzero = 0;
    for (const float value : lfe) {
        lfeEnergy += static_cast<double>(value) * static_cast<double>(value);
        lfePeak = std::max(lfePeak, std::abs(static_cast<double>(value)));
        if (value != 0.0f) ++lfeNonzero;
    }
    impl_->output << "],\"lfePcm\":{\"sampleCount\":" << lfe.size()
                  << ",\"energy\":" << number(lfeEnergy)
                  << ",\"rms\":" << number(std::sqrt(lfeEnergy / lfe.size()))
                  << ",\"peak\":" << number(lfePeak)
                  << ",\"nonzeroSamples\":" << lfeNonzero
                  << "},\"objects\":[";
    for (unsigned object = 0; object < math.numObjects; ++object) {
        if (object) impl_->output << ',';
        const eac3joc::JocObjectSummary *source = object < report.objects.size()
            ? &report.objects[object] : nullptr;
        const bool present = object < math.interpolated.size() && math.interpolated[object].present;
        impl_->output << "{\"object\":" << object
                      << ",\"present\":" << (present ? "true" : "false")
                      << ",\"sparse\":" << (source && source->sparse ? "true" : "false")
                      << ",\"numBands\":" << (source ? source->numBands : 0U)
                      << ",\"quantIndex\":" << (source ? source->quantIndex : 0U)
                      << ",\"slopeIndex\":" << (source ? source->slopeIndex : 0U)
                      << ",\"numDataPoints\":" << (source ? source->numDataPoints : 0U);
        impl_->output << ",\"q\":[";
        if (object < math.quantized.size()) {
            const auto &q = math.quantized[object].values;
            for (std::size_t dp = 0; dp < q.size(); ++dp) {
                if (dp) impl_->output << ',';
                impl_->output << '[';
                for (std::size_t channel = 0; channel < q[dp].size(); ++channel) {
                    if (channel) impl_->output << ',';
                    writeScalarArray(impl_->output, q[dp][channel]);
                }
                impl_->output << ']';
            }
        }
        impl_->output << "],\"dq\":[";
        if (object < math.dequantized.size()) {
            const auto &dq = math.dequantized[object].values;
            for (std::size_t dp = 0; dp < dq.size(); ++dp) {
                if (dp) impl_->output << ',';
                impl_->output << '[';
                for (std::size_t channel = 0; channel < dq[dp].size(); ++channel) {
                    if (channel) impl_->output << ',';
                    writeDoubleArray(impl_->output, dq[dp][channel]);
                }
                impl_->output << ']';
            }
        }
        impl_->output << "],\"quantizedSemantics\":\"differential-quantized-index\""
                      << ",\"rawSymbolSemantics\":\"huffman-decoded-symbols-in-forensic\"";
        if (report.forensic && object < report.forensic->objects.size()) {
            const eac3joc::JocObjectTrace &objectTrace = report.forensic->objects[object];
            impl_->output << ",\"forensic\":{\"headerBitOffset\":"
                          << objectTrace.headerBitOffset
                          << ",\"headerBitEnd\":" << objectTrace.headerBitEnd
                          << ",\"dataBitOffset\":"
                          << (objectTrace.hasDataRange
                              ? std::to_string(objectTrace.dataBitOffset) : "null")
                          << ",\"dataBitEnd\":"
                          << (objectTrace.hasDataRange
                              ? std::to_string(objectTrace.dataBitEnd) : "null")
                          << ",\"dataPoints\":[";
            for (std::size_t dp = 0; dp < objectTrace.dataPoints.size(); ++dp) {
                if (dp) impl_->output << ',';
                const eac3joc::JocDataPointTrace &dataPoint = objectTrace.dataPoints[dp];
                impl_->output << "{\"offsetTsBitOffset\":"
                              << (dataPoint.hasOffsetTsRange
                                  ? std::to_string(dataPoint.offsetTsBitOffset) : "null")
                              << ",\"offsetTsBitEnd\":"
                              << (dataPoint.hasOffsetTsRange
                                  ? std::to_string(dataPoint.offsetTsBitEnd) : "null")
                              << ",\"symbolDataBitOffset\":"
                              << (dataPoint.hasSymbolDataRange
                                  ? std::to_string(dataPoint.symbolDataBitOffset) : "null")
                              << ",\"symbolDataBitEnd\":"
                              << (dataPoint.hasSymbolDataRange
                                  ? std::to_string(dataPoint.symbolDataBitEnd) : "null")
                              << ",\"symbols\":[";
                for (std::size_t symbol = 0; symbol < dataPoint.symbols.size(); ++symbol) {
                    if (symbol) impl_->output << ',';
                    const eac3joc::JocHuffmanSymbolTrace &trace = dataPoint.symbols[symbol];
                    impl_->output << "{\"kind\":"
                                  << jsonString(eac3joc::symbolKindText(trace.kind))
                                  << ",\"table\":" << jsonString(trace.table)
                                  << ",\"bitOffset\":" << trace.bitOffset
                                  << ",\"bitLength\":" << trace.bitLength
                                  << ",\"codeword\":" << jsonString(trace.codeword)
                                  << ",\"symbol\":" << trace.symbol
                                  << ",\"parameterBand\":"
                                  << (trace.parameterBand >= 0
                                      ? std::to_string(trace.parameterBand) : "null")
                                  << ",\"inputChannel\":"
                                  << (trace.inputChannel >= 0
                                      ? std::to_string(trace.inputChannel) : "null")
                                  << ",\"resolvedInputChannel\":"
                                  << (trace.resolvedInputChannel >= 0
                                      ? std::to_string(trace.resolvedInputChannel) : "null")
                                  << '}';
                }
                impl_->output << "]}";
            }
            impl_->output << "]}";
        }
        if (source && source->sparse) {
            impl_->output << ",\"channelIndexMod\":[";
            for (std::size_t dp = 0; dp < source->dataPoints.size(); ++dp) {
                if (dp) impl_->output << ',';
                impl_->output << '[';
                unsigned previous = 0;
                for (std::size_t band = 0; band < source->dataPoints[dp].channelIndices.size(); ++band) {
                    if (band) impl_->output << ',';
                    const unsigned index = source->dataPoints[dp].channelIndices[band];
                    const unsigned current = band == 0 ? index : (previous + index) % math.numChannels;
                    impl_->output << current;
                    previous = index;
                }
                impl_->output << ']';
            }
            impl_->output << ']';
        }
        impl_->output << ",\"interpRepresentative\":[";
        for (std::size_t rep = 0; rep < kRepresentativeSubbands.size(); ++rep) {
            if (rep) impl_->output << ',';
            const unsigned subband = kRepresentativeSubbands[rep];
            impl_->output << "{\"subband\":" << subband << ",\"channels\":[";
            for (unsigned channel = 0; channel < math.numChannels; ++channel) {
                if (channel) impl_->output << ',';
                impl_->output << '[';
                for (unsigned timeslot = 0; timeslot < kTimeslots; ++timeslot) {
                    if (timeslot) impl_->output << ',';
                    double value = 0.0;
                    if (present && object < math.interpolated.size()
                        && timeslot < math.interpolated[object].values.size()
                        && channel < math.interpolated[object].values[timeslot].size()
                        && subband < math.interpolated[object].values[timeslot][channel].size()) {
                        value = math.interpolated[object].values[timeslot][channel][subband];
                    }
                    impl_->output << number(value);
                }
                impl_->output << ']';
            }
            impl_->output << "]}";
        }
        impl_->output << "],\"contributions\":[";
        for (unsigned channel = 0; channel < math.numChannels; ++channel) {
            if (channel) impl_->output << ',';
            double energy = 0.0;
            for (unsigned timeslot = 0; timeslot < kTimeslots; ++timeslot) {
                for (unsigned subband = 0; subband < kSubbands; ++subband) {
                    double coefficient = 0.0;
                    if (present && object < math.interpolated.size()) {
                        coefficient = math.interpolated[object].values[timeslot][channel][subband];
                    }
                    energy += std::norm(coefficient
                        * qmf.inputQmf[channel][timeslot * kSubbands + subband]);
                }
            }
            impl_->output << "{\"identity\":"
                          << jsonString(channel < identities.size()
                              ? eac3joc::channelIdentityText(identities[channel]) : "UNKNOWN")
                          << ",\"energy\":" << number(energy)
                          << ",\"rms\":" << number(std::sqrt(energy / kSamplesPerUnit)) << '}';
        }
        double qoutEnergy = 0.0;
        if (object < qmf.objects.size()) {
            for (const auto &value : qmf.objects[object]) qoutEnergy += std::norm(value);
        }
        impl_->output << "],\"qoutEnergy\":" << number(qoutEnergy)
                      << ",\"qoutRms\":" << number(std::sqrt(qoutEnergy / kSamplesPerUnit)) << '}';
    }
    std::vector<bool> presentNow(math.numObjects, false);
    for (unsigned object = 0; object < math.numObjects; ++object) {
        presentNow[object] = object < math.interpolated.size() && math.interpolated[object].present;
    }
    std::vector<bool> becamePresent(math.numObjects, false);
    for (unsigned object = 0; object < math.numObjects && object < impl_->previousPresent.size(); ++object) {
        becamePresent[object] = !impl_->previousPresent[object] && presentNow[object];
    }
    impl_->output << "],\"presentNow\":";
    writeBoolArray(impl_->output, presentNow);
    impl_->output << ",\"becamePresentAfterAbsent\":";
    writeBoolArray(impl_->output, becamePresent);
    impl_->output << ",\"absentPolicy\":\"zero-output-and-clear-previous-state\",\"reconstructionCheck\":{"
                  << "\"maxAbs\":" << number(check.maxAbs)
                  << ",\"relativeRms\":" << number(check.relativeRms)
                  << ",\"mismatchCount\":" << check.mismatchCount
                  << ",\"comparedCount\":" << check.comparedCount << "}}\n";
    if (!impl_->output) {
        *reason = "trace-write-failed";
        return false;
    }
    impl_->previousPresent = std::move(presentNow);
    ++impl_->records;
    return true;
}

bool Writer::close(std::string *reason)
{
    if (!impl_) return true;
    impl_->output.flush();
    const bool pass = static_cast<bool>(impl_->output);
    impl_->output.close();
    if (!pass && reason) *reason = "trace-close-failed";
    return pass;
}

} // namespace eac3joctrace
