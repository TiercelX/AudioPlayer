#include "native-eac3-joc-qualifier.h"

#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace eac3native;

class BitWriter {
public:
    void bits(unsigned value, unsigned count)
    {
        for (unsigned shift = count; shift-- > 0U;) {
            bits_.push_back(((value >> shift) & 1U) != 0U);
        }
    }

    std::vector<std::uint8_t> bytes() const
    {
        std::vector<std::uint8_t> output((bits_.size() + 7U) / 8U, 0U);
        for (std::size_t index = 0U; index < bits_.size(); ++index) {
            if (bits_[index]) {
                output[index / 8U] |= static_cast<std::uint8_t>(
                    1U << (7U - (index % 8U)));
            }
        }
        return output;
    }

private:
    std::vector<bool> bits_;
};

std::vector<std::uint8_t> validOamdPayload()
{
    BitWriter writer;
    writer.bits(0U, 2U); // version
    writer.bits(0U, 5U); // one object
    writer.bits(0U, 1U); // not dynamic-only
    writer.bits(0U, 4U); // content description
    writer.bits(0U, 1U); // no alternate object data
    writer.bits(0U, 4U); // no elements
    return writer.bytes();
}

std::vector<std::uint8_t> validJocPayload()
{
    BitWriter writer;
    writer.bits(0U, 3U);  // downmix config 0, accepted by Gate 5A
    writer.bits(0U, 6U);  // one object
    writer.bits(0U, 3U);  // ext config 0
    writer.bits(0U, 3U);  // clip gain X
    writer.bits(0U, 5U);  // clip gain Y
    writer.bits(0U, 10U); // sequence count
    writer.bits(0U, 1U);  // object absent
    return writer.bytes();
}

NativeEmdfPayload source(unsigned id, const std::vector<std::uint8_t> &bytes,
                         std::size_t headerStartBit)
{
    NativeEmdfPayload payload;
    payload.id = id;
    payload.sizeBytes = static_cast<unsigned>(bytes.size());
    payload.headerStartBit = headerStartBit;
    payload.dataStartBit = headerStartBit + 37U;
    payload.dataEndBit = payload.dataStartBit + bytes.size() * 8U;
    payload.dataStartByte = payload.dataStartBit / 8U;
    payload.dataEndByte = (payload.dataEndBit + 7U) / 8U;
    payload.bytes = bytes;
    return payload;
}

NativeEmdfResult syntheticEmdf(bool includeOamd, bool includeJoc,
                               bool duplicateJoc = false,
                               bool wrongJocId = false,
                               bool malformedJoc = false)
{
    NativeEmdfResult result;
    result.disposition = EmdfDisposition::Accepted;
    result.frameStartByte = 0U;
    result.frameEndByte = 3072U;
    result.frameStartBit = 0U;
    result.frameEndBit = 3072U * 8U;
    NativeEmdfContainer container;
    container.startBit = 100U;
    container.endBit = 400U;
    if (includeOamd) {
        container.payloads.push_back(source(11U, validOamdPayload(), 120U));
    }
    if (includeJoc) {
        const std::vector<std::uint8_t> payload = malformedJoc
            ? std::vector<std::uint8_t> {}
            : validJocPayload();
        container.payloads.push_back(source(wrongJocId ? 13U : 14U, payload, 180U));
        if (duplicateJoc) {
            container.payloads.push_back(source(14U, validJocPayload(), 240U));
        }
    }
    result.containers.push_back(std::move(container));
    return result;
}

std::vector<std::uint8_t> syntheticConfig3Frame()
{
    BitWriter header;
    header.bits(0x0b77U, 16U);
    header.bits(0U, 2U);       // independent
    header.bits(0U, 3U);       // SID0
    header.bits(1535U, 11U);   // 3072 bytes
    header.bits(0U, 2U);       // 48 kHz
    header.bits(3U, 2U);       // six blocks
    header.bits(7U, 3U);       // five FBW channels
    header.bits(1U, 1U);       // LFE
    header.bits(16U, 5U);      // E-AC-3 bsid
    std::vector<std::uint8_t> frame = header.bytes();
    frame.resize(3072U, 0U);
    return frame;
}

bool selfTest(const std::string &tablePath)
{
    const NativeJocQualification qualified = qualifyNativeEac3Emdf(
        syntheticEmdf(true, true), tablePath, 4U, 6144);
    const bool qualifiedCase = qualified.disposition
        == JocQualificationDisposition::Qualified
        && qualified.auIndex == 4U && qualified.timestamp == 6144
        && qualified.oamdAccepted && qualified.jocAccepted
        && qualified.joc.numChannels == 5U;
    const bool missingCase = qualifyNativeEac3Emdf(
        syntheticEmdf(true, false), tablePath, 0U, 0).reason
        == "joc-qualifier-payload14-missing";
    const bool duplicateCase = qualifyNativeEac3Emdf(
        syntheticEmdf(true, true, true), tablePath, 0U, 0).reason
        == "joc-qualifier-payload14-duplicate";
    const bool wrongIdCase = qualifyNativeEac3Emdf(
        syntheticEmdf(true, true, false, true), tablePath, 0U, 0).reason
        == "joc-qualifier-payload14-missing";
    const NativeJocQualification malformed = qualifyNativeEac3Emdf(
        syntheticEmdf(true, true, false, false, true), tablePath, 0U, 0);
    const bool malformedCase = malformed.disposition
        == JocQualificationDisposition::Malformed
        && malformed.stage == "payload14";
    NativeEac3JocQualifier state(tablePath);
    const NativeJocQualification stateFailure = state.process(syntheticConfig3Frame());
    const bool resetCase = stateFailure.disposition == JocQualificationDisposition::Malformed
        && state.poisoned();
    state.reset();
    const bool resetClearsCase = !state.poisoned()
        && state.framesProcessed() == 0U && state.nextTimestamp() == 0;
    const bool pass = qualifiedCase && missingCase && duplicateCase && wrongIdCase
        && malformedCase && resetCase && resetClearsCase;
    std::cout << "selfTest=" << (pass ? "PASS" : "FAIL")
              << " cases=7 qualified=" << (qualifiedCase ? "PASS" : "FAIL")
              << " missing=" << (missingCase ? "PASS" : "FAIL")
              << " duplicate=" << (duplicateCase ? "PASS" : "FAIL")
              << " wrongId=" << (wrongIdCase ? "PASS" : "FAIL")
              << " malformed=" << (malformedCase ? "PASS" : "FAIL")
              << " poison=" << (resetCase ? "PASS" : "FAIL")
              << " reset=" << (resetClearsCase ? "PASS" : "FAIL")
              << " drcApplied=NO ffmpegLinked=NO\n";
    return pass;
}

std::uint64_t mix(std::uint64_t digest, std::uint64_t value)
{
    for (unsigned shift = 0U; shift < 64U; shift += 8U) {
        digest ^= (value >> shift) & 0xffU;
        digest *= 1099511628211ULL;
    }
    return digest;
}

bool runFile(const std::string &path, const std::string &tablePath,
             std::size_t maxFrames)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::cerr << "probeResult=FAIL stage=open reason=file-open-failed\n";
        return false;
    }
    const std::vector<std::uint8_t> bytes(
        (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const Eb3FramingResult framing = normalizeEb3Framing(bytes);
    if (!framing.ok) {
        std::cerr << "probeResult=FAIL stage=framing reason=" << framing.reason << '\n';
        return false;
    }
    const std::vector<std::uint8_t> &framed = framing.normalized;
    NativeEac3JocQualifier qualifier(tablePath);
    std::size_t offset = 0U;
    std::size_t seen = 0U;
    std::size_t accepted = 0U;
    std::size_t qualified = 0U;
    std::size_t unsupported = 0U;
    std::size_t malformed = 0U;
    std::size_t oamdPass = 0U;
    std::size_t jocPass = 0U;
    std::size_t jocUnsupported = 0U;
    std::size_t jocMalformed = 0U;
    std::map<unsigned, std::size_t> downmixConfigs;
    std::uint64_t digest = 1469598103934665603ULL;
    std::string firstFailureStage;
    std::string firstFailureReason;
    std::size_t firstPayload11Start = 0U;
    std::size_t firstPayload11End = 0U;
    std::size_t firstPayload14Start = 0U;
    std::size_t firstPayload14End = 0U;
    bool firstPayload11Seen = false;
    bool firstPayload14Seen = false;
    unsigned firstJocDownmixConfig = 0U;
    unsigned firstJocChannels = 0U;
    unsigned firstJocObjects = 0U;
    unsigned firstOamdVersion = 0U;
    unsigned firstOamdObjects = 0U;
    bool firstJocReportSeen = false;
    bool firstOamdReportSeen = false;
    bool continuity = true;
    while (offset < framed.size() && seen < maxFrames) {
        const ParseResult parsed = parseSyncframe(framed, offset);
        if (parsed.disposition != Disposition::Accepted || !parsed.frame
            || parsed.frame->sizeBytes > framed.size() - offset) {
            std::cerr << "probeResult=FAIL frame=" << seen
                      << " stage=frame reason=frame-parse-or-bounds\n";
            return false;
        }
        const std::vector<std::uint8_t> packet(
            framed.begin() + static_cast<std::ptrdiff_t>(offset),
            framed.begin() + static_cast<std::ptrdiff_t>(offset + parsed.frame->sizeBytes));
        const NativeJocQualification result = qualifier.process(packet);
        continuity = continuity && result.auIndex == accepted
            && result.timestamp == static_cast<std::int64_t>(accepted) * 1536;
        ++seen;
        if (result.emdf.disposition == EmdfDisposition::Accepted) {
            ++accepted;
        }
        if (result.disposition == JocQualificationDisposition::Qualified) {
            ++qualified;
        } else if (result.disposition == JocQualificationDisposition::Unsupported) {
            ++unsupported;
            if (firstFailureReason.empty()) {
                firstFailureStage = result.stage;
                firstFailureReason = result.reason;
            }
        } else {
            ++malformed;
            if (firstFailureReason.empty()) {
                firstFailureStage = result.stage;
                firstFailureReason = result.reason;
            }
        }
        if (result.hasOamdReport) {
            if (result.oamdAccepted) ++oamdPass;
            if (!firstOamdReportSeen) {
                firstOamdVersion = result.oamd.version;
                firstOamdObjects = result.oamd.objectCount;
                firstOamdReportSeen = true;
            }
        }
        if (result.hasJocReport) {
            downmixConfigs[result.joc.downmixConfigIndex]++;
            if (!firstJocReportSeen) {
                firstJocDownmixConfig = result.joc.downmixConfigIndex;
                firstJocChannels = result.joc.numChannels;
                firstJocObjects = result.joc.numObjects;
                firstJocReportSeen = true;
            }
            if (result.jocAccepted) ++jocPass;
            else if (result.joc.disposition == eac3joc::ParseDisposition::Unsupported) ++jocUnsupported;
            else ++jocMalformed;
        }
        for (const NativeJocPayloadSource &source : result.payloadSources) {
            digest = mix(digest, source.id);
            digest = mix(digest, source.dataStartBit);
            digest = mix(digest, source.dataEndBit);
            if (source.id == 11U && !firstPayload11Seen) {
                firstPayload11Start = source.dataStartBit;
                firstPayload11End = source.dataEndBit;
                firstPayload11Seen = true;
            }
            if (source.id == 14U && !firstPayload14Seen) {
                firstPayload14Start = source.dataStartBit;
                firstPayload14End = source.dataEndBit;
                firstPayload14Seen = true;
            }
        }
        offset += parsed.frame->sizeBytes;
    }
    const bool pass = seen == maxFrames && accepted == maxFrames
        && malformed == 0U && continuity && qualifier.framesProcessed() == accepted;
    std::cout << "auCount=" << seen << '\n'
              << "acceptedFrames=" << accepted << '\n'
              << "qualifiedJocFrames=" << qualified << '\n'
              << "unsupportedFrames=" << unsupported << '\n'
              << "malformedFrames=" << malformed << '\n'
              << "oamdParsedPassCount=" << oamdPass << '\n'
              << "jocParsedPassCount=" << jocPass << '\n'
              << "jocUnsupportedCount=" << jocUnsupported << '\n'
              << "jocMalformedCount=" << jocMalformed << '\n'
              << "firstOamdVersion=" << firstOamdVersion
              << " firstOamdObjects=" << firstOamdObjects << '\n'
              << "firstJocDownmixConfig=" << firstJocDownmixConfig
              << " firstJocChannels=" << firstJocChannels
              << " firstJocObjects=" << firstJocObjects << '\n'
              << "jocDownmixConfigs=";
    bool first = true;
    for (const auto &[config, count] : downmixConfigs) {
        if (!first) std::cout << ',';
        first = false;
        std::cout << config << ':' << count;
    }
    std::cout << '\n'
              << "firstPayload11DataBits=" << firstPayload11Start << ':'
              << firstPayload11End << '\n'
              << "firstPayload14DataBits=" << firstPayload14Start << ':'
              << firstPayload14End << '\n'
              << "timestampContinuity=" << (continuity ? "PASS" : "FAIL") << '\n'
              << "firstFailureStage=" << firstFailureStage << '\n'
              << "firstFailureReason=" << firstFailureReason << '\n'
              << "stateDigest=" << std::hex << digest << std::dec << '\n'
              << "qualificationDisposition="
              << (qualified == accepted ? "PASS" : "UNSUPPORTED") << '\n'
              << "drcApplied=NO ffmpegLinked=NO\n"
              << "probeResult=" << (pass ? "PASS" : "FAIL")
              << " stage=gate-j0a2-native-joc-qualification\n";
    return pass;
}

void usage()
{
    std::cerr << "Usage: Eac3NativeJocQualifierProbe --self-test [joc-table-path]\n"
                 "       Eac3NativeJocQualifierProbe <config3.eac3> [--max-frames N]"
                 " [--joc-table path]\n";
}

} // namespace

int main(int argc, char **argv)
{
    const std::string defaultTable = "docs/dev/ts_103420_tables.c";
    if (argc >= 2 && std::string(argv[1]) == "--self-test") {
        const std::string tablePath = argc >= 3 ? argv[2] : defaultTable;
        return selfTest(tablePath) ? 0 : 1;
    }
    if (argc < 2 || argc > 6) {
        usage();
        return 2;
    }
    std::size_t maxFrames = 10U;
    std::string tablePath = defaultTable;
    for (int index = 2; index < argc; ++index) {
        const std::string option = argv[index];
        if (option == "--max-frames" && index + 1 < argc) {
            try {
                maxFrames = static_cast<std::size_t>(std::stoull(argv[++index]));
            } catch (...) {
                usage();
                return 2;
            }
        } else if (option == "--joc-table" && index + 1 < argc) {
            tablePath = argv[++index];
        } else {
            usage();
            return 2;
        }
    }
    return runFile(argv[1], tablePath, maxFrames) ? 0 : 1;
}
