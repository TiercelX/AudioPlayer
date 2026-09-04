#include "native-eac3-emdf.h"

#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {
using namespace eac3native;

std::uint64_t mix(std::uint64_t digest, std::uint64_t value)
{
    for (unsigned shift = 0U; shift < 64U; shift += 8U) {
        digest ^= (value >> shift) & 0xffU;
        digest *= 1099511628211ULL;
    }
    return digest;
}

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
            if (bits_[index]) output[index / 8U] |= static_cast<std::uint8_t>(
                1U << (7U - (index % 8U)));
        }
        return output;
    }

    std::size_t size() const { return bits_.size(); }

private:
    std::vector<bool> bits_;
};

std::vector<std::uint8_t> validUnknownContainer()
{
    BitWriter body;
    body.bits(0U, 2U); // version
    body.bits(0U, 3U); // key id
    body.bits(5U, 5U); // unknown payload id
    body.bits(0U, 1U); // sample offset absent
    body.bits(0U, 1U); // duration absent
    body.bits(0U, 1U); // group absent
    body.bits(0U, 1U); // language absent
    body.bits(1U, 1U); // discard unknown payload
    body.bits(2U, 8U); // payload size, variable-bit terminator
    body.bits(0U, 1U);
    body.bits(0xaaU, 8U);
    body.bits(0x55U, 8U);
    body.bits(0U, 5U); // payload terminator
    body.bits(1U, 2U); // primary protection code
    body.bits(0U, 2U); // secondary protection code
    body.bits(0U, 8U); // primary protection bytes
    while (body.size() % 8U != 0U) body.bits(0U, 1U);
    const std::vector<std::uint8_t> bodyBytes = body.bytes();
    BitWriter output;
    output.bits(0x5838U, 16U);
    output.bits(static_cast<unsigned>(bodyBytes.size()), 16U);
    for (std::uint8_t value : bodyBytes) output.bits(value, 8U);
    return output.bytes();
}

std::vector<std::uint8_t> syntheticConfig3Frame()
{
    BitWriter header;
    header.bits(0x0b77U, 16U); // syncword
    header.bits(0U, 2U);       // independent stream type
    header.bits(0U, 3U);       // SID0
    header.bits(1535U, 11U);   // 3072-byte frame
    header.bits(0U, 2U);       // 48 kHz
    header.bits(3U, 2U);       // six blocks
    header.bits(7U, 3U);       // 5 FBW channels
    header.bits(1U, 1U);       // LFE
    header.bits(16U, 5U);      // E-AC-3 bsid
    std::vector<std::uint8_t> frame = header.bytes();
    frame.resize(3072U, 0U);
    return frame;
}

bool selfTest()
{
    const std::vector<std::uint8_t> valid = validUnknownContainer();
    const NativeEmdfResult unknown = parseNativeEmdfContainer(
        valid, 0U, valid.size() * 8U);
    const bool unknownCase = unknown.disposition == EmdfDisposition::Accepted
        && unknown.containers.size() == 1U
        && unknown.containers[0].payloads.size() == 1U
        && unknown.containers[0].payloads[0].id == 5U
        && unknown.containers[0].payloads[0].bytes
            == std::vector<std::uint8_t>({0xaaU, 0x55U})
        && unknown.containers[0].payloads[0].dataEndBit
            - unknown.containers[0].payloads[0].dataStartBit == 16U
        && unknown.containers[0].payloads[0].dataStartByte
            == unknown.containers[0].payloads[0].dataStartBit / 8U
        && unknown.containers[0].payloads[0].dataEndByte
            == (unknown.containers[0].payloads[0].dataEndBit + 7U) / 8U
        && unknown.containers[0].protectionPrimaryBits == 8U;
    const std::vector<std::uint8_t> truncated = {0x58U, 0x38U, 0x00U, 0x04U};
    const bool truncation = parseNativeEmdfContainer(
        truncated, 0U, truncated.size() * 8U).disposition == EmdfDisposition::Malformed;
    const bool bounds = parseNativeEmdfContainer(
        valid, valid.size() * 8U - 8U, valid.size() * 8U).disposition
        == EmdfDisposition::Malformed;
    const std::vector<std::uint8_t> badProtection = {0x58U, 0x38U, 0x00U, 0x04U,
                                                      0x00U, 0x00U, 0x00U, 0x00U};
    const bool protection = parseNativeEmdfContainer(
        badProtection, 0U, badProtection.size() * 8U).disposition
        == EmdfDisposition::Malformed;
    const std::vector<std::uint8_t> falseSync = {0x00U, 0x58U, 0x38U, 0x00U,
                                                  0x00U, 0x00U};
    const NativeEmdfResult noFalseSync = parseNativeEmdfContainer(
        falseSync, 8U, falseSync.size() * 8U);
    const bool falseSyncCase = noFalseSync.disposition == EmdfDisposition::Malformed
        && noFalseSync.containers.empty();
    const std::vector<std::uint8_t> exactFrame = syntheticConfig3Frame();
    const NativeEmdfResult exact = extractNativeEac3Emdf(exactFrame, 0U);
    std::vector<std::uint8_t> trailingFrame = exactFrame;
    trailingFrame.push_back(0U);
    const NativeEmdfResult trailing = extractNativeEac3Emdf(trailingFrame, 0U);
    const bool exactFrameCase = exact.disposition == EmdfDisposition::Accepted
        && exact.frameStartByte == 0U
        && exact.frameEndByte == exactFrame.size();
    const bool trailingFrameCase = trailing.disposition == EmdfDisposition::Malformed
        && trailing.reason == "native-emdf-requires-exactly-one-syncframe";
    const bool pass = unknownCase && truncation && bounds && protection
        && falseSyncCase && exactFrameCase && trailingFrameCase;
    std::cout << "selfTest=" << (pass ? "PASS" : "FAIL")
              << " cases=7 unknownPayload=" << (unknownCase ? "PASS" : "FAIL")
              << " truncation=" << (truncation ? "PASS" : "FAIL")
              << " bounds=" << (bounds ? "PASS" : "FAIL")
              << " protection=" << (protection ? "PASS" : "FAIL")
              << " falseSync=" << (falseSyncCase ? "PASS" : "FAIL")
              << " exactFrame=" << (exactFrameCase ? "PASS" : "FAIL")
              << " trailingFrame=" << (trailingFrameCase ? "PASS" : "FAIL")
              << " unknownReason=" << unknown.reason
              << " drcApplied=NO ffmpegLinked=NO\n";
    return pass;
}

bool runFile(const std::string &path, std::size_t maxFrames)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::cerr << "probeResult=FAIL reason=file-open-failed\n";
        return false;
    }
    const std::vector<std::uint8_t> bytes(
        (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const Eb3FramingResult framing = normalizeEb3Framing(bytes);
    if (!framing.ok) {
        std::cerr << "probeResult=FAIL reason=" << framing.reason << '\n';
        return false;
    }
    const std::vector<std::uint8_t> &framed = framing.normalized;
    std::size_t offset = 0U;
    std::size_t accepted = 0U;
    std::size_t containerCount = 0U;
    std::size_t payloadCount = 0U;
    std::map<unsigned, std::size_t> payloadIds;
    std::uint64_t digest = 1469598103934665603ULL;
    bool valid = true;
    std::string firstCandidateReason;
    std::size_t firstFrameEndByte = 0U;
    std::size_t firstPayloadDataStartBit = 0U;
    std::size_t firstPayloadDataEndBit = 0U;
    std::size_t firstPayloadDataStartByte = 0U;
    std::size_t firstPayloadDataEndByte = 0U;
    bool firstPayloadSeen = false;
    while (offset < framed.size() && accepted < maxFrames) {
        const ParseResult parsed = parseSyncframe(framed, offset);
        if (parsed.disposition != Disposition::Accepted || !parsed.frame
            || parsed.frame->sizeBytes > framed.size() - offset) {
            std::cerr << "probeResult=FAIL frame=" << accepted
                      << " reason=frame-parse-or-bounds\n";
            return false;
        }
        const FrameHeader &frame = *parsed.frame;
        if (accepted == 0U) firstFrameEndByte = frame.sizeBytes;
        const std::vector<std::uint8_t> packet(
            framed.begin() + static_cast<std::ptrdiff_t>(offset),
            framed.begin() + static_cast<std::ptrdiff_t>(offset + frame.sizeBytes));
        const NativeEmdfResult result = extractNativeEac3Emdf(packet, accepted);
        if (firstCandidateReason.empty()) firstCandidateReason = result.reason;
        if (result.disposition != EmdfDisposition::Accepted) {
            std::cerr << "probeResult=FAIL frame=" << accepted
                      << " reason=" << result.reason << '\n';
            return false;
        }
        valid = valid && result.frameStartBit == 0U
            && result.frameStartByte == 0U
            && result.frameEndByte == packet.size()
            && result.frameEndBit == packet.size() * 8U;
        for (const NativeEmdfContainer &container : result.containers) {
            ++containerCount;
            valid = valid && container.startBit < container.endBit
                && container.endBit <= result.frameEndBit;
            for (const NativeEmdfPayload &payload : container.payloads) {
                valid = valid && payload.headerStartBit >= container.startBit
                    && payload.headerStartBit < payload.dataStartBit
                    && payload.dataStartBit <= payload.dataEndBit
                    && payload.dataEndBit <= container.endBit
                    && payload.dataEndBit - payload.dataStartBit
                        == static_cast<std::size_t>(payload.sizeBytes) * 8U
                    && payload.dataStartByte == payload.dataStartBit / 8U
                    && payload.dataEndByte == (payload.dataEndBit + 7U) / 8U
                    && payload.dataStartByte <= payload.dataEndByte
                    && payload.dataEndByte <= (container.endBit + 7U) / 8U;
                if (!firstPayloadSeen) {
                    firstPayloadDataStartBit = payload.dataStartBit;
                    firstPayloadDataEndBit = payload.dataEndBit;
                    firstPayloadDataStartByte = payload.dataStartByte;
                    firstPayloadDataEndByte = payload.dataEndByte;
                    firstPayloadSeen = true;
                }
                ++payloadCount;
                ++payloadIds[payload.id];
                digest = mix(digest, payload.id);
                digest = mix(digest, payload.bytes.size());
                for (std::uint8_t value : payload.bytes) digest = mix(digest, value);
            }
        }
        offset += frame.sizeBytes;
        ++accepted;
    }
    const std::size_t payload11 = payloadIds[11U];
    const std::size_t payload14 = payloadIds[14U];
    const bool pass = accepted == maxFrames && valid && containerCount == maxFrames
        && payload11 == maxFrames && payload14 == maxFrames;
    std::cout << "auCount=" << accepted << '\n'
              << "containerCount=" << containerCount << '\n'
              << "firstFrameByteBounds=0:" << firstFrameEndByte << '\n'
              << "firstPayloadDataBounds=bits:" << firstPayloadDataStartBit
              << ':' << firstPayloadDataEndBit
              << " bytes:" << firstPayloadDataStartByte
              << ':' << firstPayloadDataEndByte << '\n'
              << "payloadCount=" << payloadCount << '\n'
              << "payload11Count=" << payload11 << '\n'
              << "payload14Count=" << payload14 << '\n'
              << "firstCandidateReason=" << (firstCandidateReason.empty() ? "none" : firstCandidateReason) << '\n'
              << "payloadCounts=";
    bool first = true;
    for (const auto &[id, count] : payloadIds) {
        if (!first) std::cout << ',';
        first = false;
        std::cout << id << ':' << count;
    }
    std::cout << '\n' << "payloadDigest=" << std::hex << digest << std::dec << '\n'
              << "containerNonOverlap=" << (valid ? "PASS" : "FAIL") << '\n'
              << "probeResult=" << (pass ? "PASS" : "FAIL")
              << " stage=gate-j0a1-native-emdf\n";
    return pass;
}

void usage()
{
    std::cerr << "Usage: Eac3NativeEmdfProbe --self-test\n"
                 "       Eac3NativeEmdfProbe <config3.eac3> [--max-frames N]\n";
}

} // namespace

int main(int argc, char **argv)
{
    if (argc == 2 && std::string(argv[1]) == "--self-test") return selfTest() ? 0 : 1;
    if (argc < 2 || argc > 4) {
        usage();
        return 2;
    }
    std::size_t maxFrames = 10U;
    if (argc == 4 && std::string(argv[2]) == "--max-frames") {
        try {
            maxFrames = static_cast<std::size_t>(std::stoull(argv[3]));
        } catch (...) {
            usage();
            return 2;
        }
    } else if (argc != 2) {
        usage();
        return 2;
    }
    return runFile(argv[1], maxFrames) ? 0 : 1;
}
