#include "native-eac3-bsi.h"

#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace eac3native;

struct Record {
    FrameHeader frame;
    BsiInfo bsi;
    std::size_t ordinal = 0;
    std::size_t au = 0;
    std::size_t associationSid = 0;
};

struct Report {
    std::size_t frames = 0;
    std::size_t units = 0;
    std::size_t eac3Independent = 0;
    std::size_t eac3IndependentSid0 = 0;
    std::size_t baseSid0 = 0;
    std::size_t legacySid0 = 0;
    std::size_t dependent = 0;
    std::size_t legacy = 0;
    std::size_t type2 = 0;
    std::size_t bsiFailures = 0;
    std::map<std::string, std::size_t> topology;
    std::uint64_t digest = 1469598103934665603ULL;
};

void hashAppend(std::uint64_t *hash, std::uint64_t value)
{
    for (unsigned i = 0; i < 8U; ++i) {
        *hash ^= (value >> (i * 8U)) & 0xffU;
        *hash *= 1099511628211ULL;
    }
}

bool isBaseSid0(const FrameHeader &frame)
{
    return frame.substreamId == 0U
        && (frame.streamType == StreamType::Independent
            || frame.streamType == StreamType::LegacyAc3
            || frame.streamType == StreamType::Ac3Convert);
}

const char *channelMapLocations(std::uint16_t chanmap)
{
    // E.1.3.1.8/Table E.1.4 order.  Pair locations count as two coded
    // channels, but remain one location token in this renderer-neutral log.
    static thread_local std::string locations;
    static constexpr const char *names[16] = {
        "L", "C", "R", "Ls", "Rs", "Lc/Rc", "Lrs/Rrs", "Cs",
        "Ts", "Lsd/Rsd", "Lw/Rw", "Vhl/Vhr", "Vhc", "Lts/Rts",
        "LFE2", "LFE"};
    locations.clear();
    for (unsigned bit = 0; bit < 16U; ++bit) {
        if ((chanmap & (static_cast<std::uint16_t>(1U) << (15U - bit))) == 0U) {
            continue;
        }
        if (!locations.empty()) {
            locations += ",";
        }
        locations += names[bit];
    }
    return locations.c_str();
}

std::string shortTopology(const std::vector<Record> &records)
{
    std::ostringstream value;
    for (const Record &record : records) {
        if (value.tellp() != std::streampos(0)) {
            value << "+";
        }
        value << toString(record.frame.streamType) << ":sid"
              << record.frame.substreamId;
        if (record.frame.streamType == StreamType::Dependent
            && record.bsi.chanmape) {
            value << ":map0x" << std::hex << std::uppercase
                  << record.bsi.chanmap << std::dec;
        }
    }
    return value.str();
}

bool validateUnit(const std::vector<Record> &records, std::string *reason)
{
    if (records.empty() || !isBaseSid0(records.front().frame)) {
        if (reason) {
            *reason = "unit-missing-independent-sid0";
        }
        return false;
    }
    const FrameHeader &base = records.front().frame;
    std::map<std::string, unsigned> blocks;
    std::optional<std::size_t> parentSid;
    for (const Record &record : records) {
        const FrameHeader &frame = record.frame;
        if (frame.sampleRate != base.sampleRate) {
            if (reason) {
                *reason = "unit-sample-rate-mismatch";
            }
            return false;
        }
        if (frame.blocks != base.blocks) {
            if (reason) {
                *reason = "unit-block-count-mismatch";
            }
            return false;
        }
        std::string key;
        if (frame.streamType == StreamType::Dependent) {
            if (!parentSid) {
                if (reason) {
                    *reason = "dependent-without-immediate-independent";
                }
                return false;
            }
            key = "dependent/p" + std::to_string(*parentSid)
                + "/d" + std::to_string(frame.substreamId);
        } else {
            key = std::string(toString(frame.streamType)) + "/sid"
                + std::to_string(frame.substreamId);
            if (frame.streamType == StreamType::Ac3Convert
                && frame.substreamId == 0U) {
                parentSid.reset();
            } else {
                parentSid = frame.substreamId;
            }
        }
        blocks[key] += frame.blocks;
    }
    for (const auto &entry : blocks) {
        if (entry.second != 6U) {
            if (reason) {
                *reason = "unit-stream-does-not-total-six-blocks";
            }
            return false;
        }
    }
    return true;
}

void printUnit(const std::vector<Record> &records, std::size_t index,
               std::size_t sampleStart)
{
    const FrameHeader &base = records.front().frame;
    std::cout << "au[" << index << "] frameCount=" << records.size()
              << " sampleStart=" << sampleStart
              << " sampleCount=" << base.sampleCount
              << " grouping=base-sid0-boundary+six-block-completion"
              << " timecode=UNAVAILABLE topology="
              << shortTopology(records) << '\n';
    for (const Record &record : records) {
        const FrameHeader &frame = record.frame;
        std::cout << "  frame[" << record.ordinal << "] au=" << index
                  << " type=" << toString(frame.streamType)
                  << " sid=" << frame.substreamId
                  << " normalizedOffset=" << frame.offset
                  << " sizeBytes=" << frame.sizeBytes
                  << " startBit=" << frame.offset * 8U
                  << " endBit=" << frame.endBit
                  << " sampleRate=" << frame.sampleRate
                  << " blocks=" << frame.blocks
                  << " acmod=" << frame.acmod
                  << " lfe=" << (frame.lfe ? "YES" : "NO")
                  << " channels=" << frame.channelCount;
        if (frame.streamType == StreamType::Dependent) {
            std::cout << " associationSid=" << record.associationSid
                      << " chanmape=" << (record.bsi.chanmape ? 1 : 0);
            if (record.bsi.chanmape) {
                std::cout << " chanmap=0x" << std::hex << std::uppercase
                          << record.bsi.chanmap << std::dec
                          << " locations="
                          << channelMapLocations(record.bsi.chanmap)
                          << " weight=" << record.bsi.chanmapChannelWeight;
            }
        } else {
            std::cout << " chanmape=NA chanmap=NA";
        }
        std::cout << '\n';
    }
}

void observeBase(Report *report, const std::vector<Record> &records)
{
    if (!report || records.empty()) {
        return;
    }
    const FrameHeader &base = records.front().frame;
    if (!isBaseSid0(base)) {
        return;
    }
    ++report->baseSid0;
    if (base.streamType == StreamType::LegacyAc3) {
        ++report->legacySid0;
    }
}

const char *presentationBase(const Report &report)
{
    const bool eac3 = report.eac3IndependentSid0 != 0U;
    const bool legacy = report.legacySid0 != 0U;
    if (eac3 && !legacy) {
        return "eac3-independent-sid0";
    }
    if (legacy && !eac3) {
        return "legacy-ac3-sid0";
    }
    return eac3 || legacy ? "mixed/unknown" : "unknown";
}

bool runSelfTest()
{
    auto setBits = [](std::vector<std::uint8_t> &bytes, std::size_t bit,
                      unsigned count, unsigned value) {
        for (unsigned i = 0; i < count; ++i) {
            const std::size_t pos = bit + i;
            const std::uint8_t mask = static_cast<std::uint8_t>(
                1U << (7U - (pos % 8U)));
            if (((value >> (count - i - 1U)) & 1U) != 0U) {
                bytes[pos / 8U] |= mask;
            }
        }
    };
    auto synthetic = [&](unsigned type, unsigned sid) {
        std::vector<std::uint8_t> bytes(12U, 0);
        bytes[0] = 0x0b;
        bytes[1] = 0x77;
        setBits(bytes, 16, 2, type);
        setBits(bytes, 18, 3, sid);
        setBits(bytes, 21, 11, 5);
        setBits(bytes, 32, 2, 0);
        setBits(bytes, 34, 2, 3);
        setBits(bytes, 36, 3, 2);
        setBits(bytes, 40, 5, 16);
        return bytes;
    };
    std::size_t cases = 0;
    const ParseResult independent = parseSyncframe(synthetic(0, 0), 0);
    if (independent.disposition != Disposition::Accepted
        || !independent.frame || !isBaseSid0(*independent.frame)) {
        std::cerr << "selfTest=FAIL case=independent-sid0\n";
        return false;
    }
    ++cases;
    const ParseResult dependent = parseSyncframe(synthetic(1, 0), 0);
    AccessUnitAssembler assembler;
    const ProcessResult baseResult = assembler.process(*independent.frame);
    if (dependent.disposition != Disposition::Accepted
        || baseResult.disposition != Disposition::Accepted
        || baseResult.flow != FlowStatus::None) {
        std::cerr << "selfTest=FAIL case=base-process\n";
        return false;
    }
    // A six-block synthetic E-AC-3 base is pending, and the immediate
    // dependent is accepted by the shared assembler.
    const ProcessResult dependentResult = assembler.process(*dependent.frame);
    if (dependentResult.disposition != Disposition::Accepted
        || dependentResult.flow != FlowStatus::None) {
        std::cerr << "selfTest=FAIL case=eac3-base-dependent\n";
        return false;
    }
    ++cases;
    FrameHeader legacy = *independent.frame;
    legacy.streamType = StreamType::LegacyAc3;
    legacy.bsid = 8U;
    legacy.dependent = false;
    legacy.additional = false;
    AccessUnitAssembler legacyAssembler;
    const ProcessResult legacyBaseResult = legacyAssembler.process(legacy);
    const ProcessResult legacyDependentResult = legacyAssembler.process(
        *dependent.frame);
    if (legacyBaseResult.disposition != Disposition::Accepted
        || legacyDependentResult.disposition != Disposition::Accepted
        || legacyDependentResult.flow != FlowStatus::None) {
        std::cerr << "selfTest=FAIL case=legacy-base-dependent\n";
        return false;
    }
    ++cases;
    const Record legacyRecord{legacy, BsiInfo{}, 0, 0, 0};
    const Record dependentRecord{*dependent.frame, BsiInfo{}, 1, 0, 0};
    if (!validateUnit({legacyRecord, dependentRecord}, nullptr)) {
        std::cerr << "selfTest=FAIL case=legacy-unit-validation\n";
        return false;
    }
    ++cases;
    if (std::string(channelMapLocations(0xA010U)) != "L,R,Vhl/Vhr") {
        std::cerr << "selfTest=FAIL case=channel-map-order\n";
        return false;
    }
    ++cases;
    std::cout << "selfTest=PASS cases=" << cases
              << " eac3Base=YES legacyBaseDependent=YES"
              << " channelMapA010=L,R,Vhl/Vhr"
              << " implementation=native-eac3-substream-inventory\n";
    return true;
}

bool runFile(const std::string &path, std::size_t maxUnits)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::cerr << "probeResult=FAIL stage=open reason=file-open-failed path="
                  << path << '\n';
        return false;
    }
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)),
                                          std::istreambuf_iterator<char>());
    const Eb3FramingResult framing = normalizeEb3Framing(bytes);
    if (!framing.ok) {
        std::cerr << "probeResult=FAIL stage=" << toString(framing.stage)
                  << " reason=" << framing.reason << '\n';
        return false;
    }
    const std::vector<std::uint8_t> &framed = framing.normalized;
    Report report;
    std::vector<Record> unit;
    std::size_t offset = 0;
    std::size_t ordinal = 0;
    std::size_t sampleStart = 0;
    std::size_t coveredUnits = 0;
    std::size_t currentAssociationSid = 0;
    bool haveAssociation = false;
    AccessUnitAssembler assembler;
    while (offset < framed.size()) {
        const ParseResult parsed = parseSyncframe(framed, offset);
        if (parsed.disposition != Disposition::Accepted || !parsed.frame) {
            std::cerr << "probeResult=FAIL stage=" << toString(parsed.stage)
                      << " reason=" << parsed.reason
                      << " frame=" << ordinal << " bit=" << offset * 8U
                      << '\n';
            return false;
        }
        const FrameHeader &frame = *parsed.frame;
        const ProcessResult assembled = assembler.process(parsed);
        if (assembled.disposition != Disposition::Accepted
            || assembled.flow != FlowStatus::None) {
            std::cerr << "probeResult=FAIL stage="
                      << toString(assembled.stage)
                      << " reason=" << assembled.reason
                      << " frame=" << ordinal << " bit=" << offset * 8U
                      << '\n';
            return false;
        }
        if (assembled.completed) {
            std::string reason;
            if (!validateUnit(unit, &reason)) {
                std::cerr << "probeResult=FAIL stage=assembly reason=" << reason
                          << " au=" << coveredUnits << '\n';
                return false;
            }
            observeBase(&report, unit);
            ++coveredUnits;
            ++report.units;
            printUnit(unit, coveredUnits - 1U, sampleStart);
            sampleStart += unit.front().frame.sampleCount;
            unit.clear();
            haveAssociation = false;
            if (maxUnits != 0U && coveredUnits >= maxUnits) {
                // The shared assembler has accepted this look-ahead frame,
                // but it belongs to the next bounded unit and is excluded
                // from all inventory counters and digest fields.
                break;
            }
        }
        const BsiParseResult bsi = parseEac3Bsi(framed, frame);
        if (bsi.disposition != Disposition::Accepted || !bsi.info) {
            ++report.bsiFailures;
            std::cerr << "probeResult=FAIL stage=bsi reason=" << bsi.reason
                      << " frame=" << ordinal << " bit=" << offset * 8U
                      << '\n';
            return false;
        }
        Record record{frame, *bsi.info, ordinal, coveredUnits, 0};
        if (frame.streamType == StreamType::Dependent) {
            if (!haveAssociation) {
                std::cerr << "probeResult=FAIL stage=assembly reason=orphan-dependent"
                          << " frame=" << ordinal << " bit=" << offset * 8U
                          << '\n';
                return false;
            }
            record.associationSid = currentAssociationSid;
            ++report.dependent;
        } else {
            currentAssociationSid = frame.substreamId;
            haveAssociation = frame.streamType != StreamType::Ac3Convert;
            if (frame.streamType == StreamType::Independent) {
                ++report.eac3Independent;
                if (frame.substreamId == 0U) {
                    ++report.eac3IndependentSid0;
                }
            } else if (frame.streamType == StreamType::LegacyAc3) {
                ++report.legacy;
            } else if (frame.streamType == StreamType::Ac3Convert) {
                ++report.type2;
            }
        }
        hashAppend(&report.digest, static_cast<unsigned>(frame.streamType));
        hashAppend(&report.digest, frame.substreamId);
        hashAppend(&report.digest, frame.sampleRate);
        hashAppend(&report.digest, frame.blocks);
        hashAppend(&report.digest, frame.sizeBytes);
        hashAppend(&report.digest, frame.acmod);
        hashAppend(&report.digest, frame.lfe ? 1U : 0U);
        hashAppend(&report.digest, bsi.info->chanmape ? 1U : 0U);
        hashAppend(&report.digest, bsi.info->chanmap);
        unit.push_back(std::move(record));
        ++report.frames;
        ++report.topology[std::string(toString(frame.streamType)) + ":sid"
                           + std::to_string(frame.substreamId)];
        offset += frame.sizeBytes;
        ++ordinal;
    }
    if ((maxUnits == 0U || coveredUnits < maxUnits) && !unit.empty()) {
        std::string reason;
        if (!validateUnit(unit, &reason)) {
            std::cerr << "probeResult=FAIL stage=assembly reason=" << reason
                      << " au=" << coveredUnits << '\n';
            return false;
        }
        observeBase(&report, unit);
        ++coveredUnits;
        ++report.units;
        printUnit(unit, coveredUnits - 1U, sampleStart);
        const ProcessResult flushed = assembler.flush();
        if (flushed.disposition != Disposition::Accepted
            || flushed.flow != FlowStatus::None || !flushed.completed) {
            std::cerr << "probeResult=FAIL stage=" << toString(flushed.stage)
                      << " reason=" << flushed.reason << '\n';
            return false;
        }
    }
    if (maxUnits != 0U && report.units != maxUnits) {
        std::cerr << "probeResult=FAIL stage=summary reason=short-input units="
                  << report.units << " requested=" << maxUnits << '\n';
        return false;
    }
    std::cout << "inputBytes=" << bytes.size() << '\n'
              << "normalizedBytes=" << framed.size() << '\n'
              << "eb3Wrapped=" << (framing.wrapped ? "YES" : "NO") << '\n'
              << "eb3WrapperCount=" << framing.wrapperCount << '\n'
              << "frames=" << report.frames << '\n'
              << "accessUnits=" << report.units << '\n'
              << "eac3IndependentFrames=" << report.eac3Independent << '\n'
              << "eac3IndependentSid0Frames="
              << report.eac3IndependentSid0 << '\n'
              << "validBaseSid0=" << (report.baseSid0 != 0U ? "YES" : "NO")
              << '\n'
              << "baseSid0Frames=" << report.baseSid0 << '\n'
              << "legacySid0Frames=" << report.legacySid0 << '\n'
              << "dependentFrames=" << report.dependent << '\n'
              << "legacyFrames=" << report.legacy << '\n'
              << "type2Frames=" << report.type2 << '\n'
              << "bsiFailures=" << report.bsiFailures << '\n'
              << "timeGrouping=ordinal-only-no-container-timecode\n"
              << "presentationBase=" << presentationBase(report) << '\n'
              << "eac3IndependentMain="
              << (report.eac3IndependentSid0 != 0U ? "YES" : "NO") << '\n'
              << "topologyDigest=0x" << std::hex << std::setfill('0')
              << std::setw(16) << report.digest << std::dec << '\n';
    for (const auto &entry : report.topology) {
        std::cout << "topology[" << entry.first << "]=" << entry.second << '\n';
    }
    const bool pass = report.units > 0U && report.bsiFailures == 0U;
    std::cout << "probeResult=" << (pass ? "PASS" : "FAIL")
              << " stage=gate8n-4a-substream-inventory"
              << " assembler=shared-access-unit-boundary"
              << " ffmpegLinked=NO drcApplied=NO\n";
    return pass;
}

void usage()
{
    std::cerr << "Usage: Eac3NativeSubstreamProbe <raw.eac3|raw.eb3>"
                 " [--max-units N]\n"
                 "       Eac3NativeSubstreamProbe --self-test\n";
}

} // namespace

int main(int argc, char **argv)
{
    if (argc == 2 && std::string(argv[1]) == "--self-test") {
        return runSelfTest() ? 0 : 1;
    }
    if (argc < 2 || argc > 4) {
        usage();
        return 2;
    }
    std::size_t maxUnits = 0;
    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "--max-units" && i + 1 < argc) {
            try {
                maxUnits = static_cast<std::size_t>(std::stoull(argv[++i]));
            } catch (...) {
                usage();
                return 2;
            }
        } else {
            usage();
            return 2;
        }
    }
    return runFile(argv[1], maxUnits) ? 0 : 1;
}
