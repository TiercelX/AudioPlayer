#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

struct BitReader {
    const std::vector<std::uint8_t> &bytes;
    std::size_t byteOffset = 0;
    std::size_t bitPos = 0;

    bool canRead(unsigned count) const
    {
        const std::size_t absoluteBitPos = byteOffset * 8U + bitPos;
        return absoluteBitPos <= bytes.size() * 8U
            && count <= (bytes.size() * 8U - absoluteBitPos);
    }

    std::uint32_t read(unsigned count)
    {
        if (count == 0 || count > 32 || !canRead(count)) {
            return 0;
        }
        std::uint32_t value = 0;
        for (unsigned index = 0; index < count; ++index) {
            const std::size_t absoluteBitPos = byteOffset * 8U + bitPos;
            value = (value << 1U) | ((bytes[absoluteBitPos / 8U] >> (7U - (absoluteBitPos % 8U))) & 1U);
            ++bitPos;
        }
        return value;
    }
};

struct FrameHeader {
    std::size_t offset = 0;
    std::size_t sizeBytes = 0;
    unsigned streamType = 0;
    unsigned substreamId = 0;
    unsigned sampleRate = 0;
    unsigned blocks = 0;
    unsigned channelCount = 0;
    unsigned acmod = 0;
    bool lfe = false;
};

const unsigned kSampleRates[] = {48000, 44100, 32000};
const unsigned kReducedSampleRates[] = {24000, 22050, 16000};

unsigned channelCountForAcmod(unsigned acmod, bool lfe)
{
    static constexpr unsigned kBaseChannels[] = {2, 1, 2, 3, 3, 4, 4, 5};
    return kBaseChannels[acmod & 7U] + (lfe ? 1U : 0U);
}

bool parseFrameHeader(const std::vector<std::uint8_t> &bytes,
                      std::size_t offset,
                      FrameHeader *header)
{
    if (!header || offset + 10 > bytes.size() || bytes[offset] != 0x0b || bytes[offset + 1] != 0x77) {
        return false;
    }

    BitReader reader{bytes, offset, 16};
    header->offset = offset;
    const unsigned streamType = reader.read(2);
    const unsigned substreamId = reader.read(3);
    const unsigned frameSizeWords = reader.read(11) + 1U;
    const unsigned fscod = reader.read(2);
    unsigned sampleRate = 0;
    unsigned blocks = 6;
    if (fscod == 3) {
        const unsigned fscod2 = reader.read(2);
        if (fscod2 >= std::size(kReducedSampleRates)) {
            return false;
        }
        sampleRate = kReducedSampleRates[fscod2];
    } else {
        if (fscod >= std::size(kSampleRates)) {
            return false;
        }
        sampleRate = kSampleRates[fscod];
        const unsigned numBlocksCode = reader.read(2);
        static constexpr unsigned kBlocks[] = {1, 2, 3, 6};
        blocks = kBlocks[numBlocksCode];
    }
    const unsigned acmod = reader.read(3);
    const bool lfe = reader.read(1) != 0;
    const unsigned bsid = reader.read(5);
    if (bsid < 8 || bsid > 16) {
        return false;
    }

    const std::size_t sizeBytes = static_cast<std::size_t>(frameSizeWords) * 2U;
    if (sizeBytes < 10 || offset + sizeBytes > bytes.size()) {
        return false;
    }
    header->sizeBytes = sizeBytes;
    header->streamType = streamType;
    header->substreamId = substreamId;
    header->sampleRate = sampleRate;
    header->blocks = blocks;
    header->acmod = acmod;
    header->lfe = lfe;
    header->channelCount = channelCountForAcmod(acmod, lfe);
    return true;
}

void printUsage()
{
    std::cerr << "Usage: AtmosJocProbe <raw.eac3|raw.ec3|raw.eb3> [--max-frames N]\n";
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        printUsage();
        return 2;
    }
    std::size_t maxFrames = 0;
    for (int index = 2; index < argc; ++index) {
        const std::string option = argv[index];
        if (option == "--max-frames" && index + 1 < argc) {
            try {
                maxFrames = static_cast<std::size_t>(std::stoull(argv[++index]));
            } catch (...) {
                printUsage();
                return 2;
            }
        } else {
            printUsage();
            return 2;
        }
    }

    std::ifstream input(argv[1], std::ios::binary);
    if (!input) {
        std::cerr << "probeResult=FAIL stage=open path=" << argv[1] << '\n';
        return 1;
    }
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)),
                                          std::istreambuf_iterator<char>());
    if (bytes.size() < 10) {
        std::cerr << "probeResult=FAIL stage=read reason=file-too-small bytes=" << bytes.size() << '\n';
        return 1;
    }

    std::map<unsigned, std::size_t> streamTypeCounts;
    std::map<unsigned, std::size_t> substreamCounts;
    std::size_t offset = 0;
    std::size_t frameCount = 0;
    std::size_t dependentFrameCount = 0;
    std::size_t totalAudioBytes = 0;
    FrameHeader firstHeader;
    bool first = true;
    while (offset + 10 <= bytes.size()) {
        if (bytes[offset] != 0x0b || bytes[offset + 1] != 0x77) {
            ++offset;
            continue;
        }
        FrameHeader header;
        if (!parseFrameHeader(bytes, offset, &header)) {
            ++offset;
            continue;
        }
        if (first) {
            firstHeader = header;
            first = false;
        }
        ++frameCount;
        if (header.streamType == 1) {
            ++dependentFrameCount;
        }
        ++streamTypeCounts[header.streamType];
        ++substreamCounts[header.substreamId];
        totalAudioBytes += header.sizeBytes;
        offset += header.sizeBytes;
        if (maxFrames != 0 && frameCount >= maxFrames) {
            break;
        }
    }

    std::cout << "inputBytes=" << bytes.size() << '\n';
    std::cout << "frames=" << frameCount << '\n';
    std::cout << "dependentFrames=" << dependentFrameCount << '\n';
    std::cout << "framedBytes=" << totalAudioBytes << '\n';
    if (!first) {
        std::cout << "sampleRate=" << firstHeader.sampleRate << '\n';
        std::cout << "firstFrameBytes=" << firstHeader.sizeBytes << '\n';
        std::cout << "firstFrameBlocks=" << firstHeader.blocks << '\n';
        std::cout << "firstFrameChannels=" << firstHeader.channelCount << '\n';
    }
    for (const auto &[streamType, count] : streamTypeCounts) {
        std::cout << "streamType[" << streamType << "]=" << count << '\n';
    }
    for (const auto &[substreamId, count] : substreamCounts) {
        std::cout << "substreamId[" << substreamId << "]=" << count << '\n';
    }
    const bool pass = frameCount > 0 && totalAudioBytes > 0 && !first;
    std::cout << "probeResult=" << (pass ? "PASS" : "FAIL")
              << " stage=eac3-syncframe-parser implementation=self-written" << '\n';
    return pass ? 0 : 1;
}
