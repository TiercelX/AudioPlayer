// Gate 0 diagnostic: correlate compressed E-AC-3 packets with libavcodec
// decoded frames. This is intentionally a probe, not a production decoder.

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/samplefmt.h>
}

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct BitReader {
    const std::uint8_t *data = nullptr;
    std::size_t size = 0;
    std::size_t bit = 0;

    bool canRead(unsigned count) const
    {
        return count <= size * 8U - std::min(bit, size * 8U);
    }

    std::uint32_t read(unsigned count)
    {
        if (count == 0 || count > 32 || !canRead(count)) {
            return 0;
        }
        std::uint32_t value = 0;
        for (unsigned i = 0; i < count; ++i) {
            value = (value << 1U) | ((data[bit / 8U] >> (7U - (bit % 8U))) & 1U);
            ++bit;
        }
        return value;
    }
};

struct SyncframeHeader {
    std::size_t offset = 0;
    std::size_t sizeBytes = 0;
    unsigned streamType = 0;
    unsigned substreamId = 0;
    unsigned sampleRate = 0;
    unsigned blocks = 0;
    unsigned channels = 0;
    unsigned acmod = 0;
    bool lfe = false;
};

struct PacketReport {
    int index = 0;
    std::int64_t pts = AV_NOPTS_VALUE;
    std::int64_t duration = 0;
    int bytes = 0;
    int syncframes = 0;
    int independentSyncframes = 0;
    int dependentSyncframes = 0;
    int totalBlocks = 0;
    bool truncated = false;
    bool completeAccessUnitShape = false;
    std::int64_t skipSamples = 0;
    std::int64_t discardPadding = 0;
    std::map<std::string, int> blocksByStream;
    std::vector<unsigned> sampleRates;
};

struct DecodedFrameReport {
    int index = 0;
    int sourcePacketIndex = 0;
    std::int64_t pts = AV_NOPTS_VALUE;
    int samples = 0;
    int channels = 0;
    int sampleRate = 0;
    int format = AV_SAMPLE_FMT_NONE;
    int profile = AV_PROFILE_UNKNOWN;
    std::string layout;
};

const unsigned kSampleRates[] = {48000, 44100, 32000};
const unsigned kReducedSampleRates[] = {24000, 22050, 16000};

unsigned channelCountForAcmod(unsigned acmod, bool lfe)
{
    static constexpr unsigned kBaseChannels[] = {2, 1, 2, 3, 3, 4, 4, 5};
    return kBaseChannels[acmod & 7U] + (lfe ? 1U : 0U);
}

bool parseSyncframeHeader(const std::uint8_t *data,
                          std::size_t size,
                          std::size_t offset,
                          SyncframeHeader *header)
{
    if (!header || offset + 10 > size || data[offset] != 0x0B || data[offset + 1] != 0x77) {
        return false;
    }

    BitReader reader {data + offset, size - offset, 16};
    header->offset = offset;
    header->streamType = reader.read(2);
    header->substreamId = reader.read(3);
    const unsigned frameSizeWords = reader.read(11) + 1U;

    const unsigned fscod = reader.read(2);
    if (fscod == 3) {
        const unsigned fscod2 = reader.read(2);
        if (fscod2 >= std::size(kReducedSampleRates)) {
            return false;
        }
        header->sampleRate = kReducedSampleRates[fscod2];
        header->blocks = 6;
    } else {
        if (fscod >= std::size(kSampleRates)) {
            return false;
        }
        header->sampleRate = kSampleRates[fscod];
        static constexpr unsigned kBlocks[] = {1, 2, 3, 6};
        header->blocks = kBlocks[reader.read(2)];
    }

    header->acmod = reader.read(3);
    header->lfe = reader.read(1) != 0;
    const unsigned bsid = reader.read(5);
    if (bsid < 8 || bsid > 16) {
        return false;
    }

    header->sizeBytes = static_cast<std::size_t>(frameSizeWords) * 2U;
    header->channels = channelCountForAcmod(header->acmod, header->lfe);
    return header->sizeBytes >= 10 && offset + header->sizeBytes <= size;
}

std::string streamKey(const SyncframeHeader &header)
{
    return "st" + std::to_string(header.streamType)
        + "/sid" + std::to_string(header.substreamId);
}

PacketReport inspectPacket(const AVPacket *packet, int packetIndex)
{
    PacketReport report;
    report.index = packetIndex;
    report.pts = packet->pts;
    report.duration = packet->duration;
    report.bytes = packet->size;

    std::size_t skipDataSize = 0;
    const std::uint8_t *skipData = av_packet_get_side_data(packet,
                                                            AV_PKT_DATA_SKIP_SAMPLES,
                                                            &skipDataSize);
    if (skipData && skipDataSize >= 8) {
        report.skipSamples = static_cast<std::int64_t>(skipData[0])
            | (static_cast<std::int64_t>(skipData[1]) << 8)
            | (static_cast<std::int64_t>(skipData[2]) << 16)
            | (static_cast<std::int64_t>(skipData[3]) << 24);
        report.discardPadding = static_cast<std::int64_t>(skipData[4])
            | (static_cast<std::int64_t>(skipData[5]) << 8)
            | (static_cast<std::int64_t>(skipData[6]) << 16)
            | (static_cast<std::int64_t>(skipData[7]) << 24);
    }

    std::size_t offset = 0;
    while (offset + 10 <= static_cast<std::size_t>(packet->size)) {
        if (packet->data[offset] != 0x0B || packet->data[offset + 1] != 0x77) {
            ++offset;
            continue;
        }

        SyncframeHeader header;
        if (!parseSyncframeHeader(packet->data,
                                   static_cast<std::size_t>(packet->size),
                                   offset,
                                   &header)) {
            report.truncated = true;
            break;
        }

        ++report.syncframes;
        if (header.streamType == 0) {
            ++report.independentSyncframes;
        } else {
            ++report.dependentSyncframes;
        }
        report.totalBlocks += static_cast<int>(header.blocks);
        report.blocksByStream[streamKey(header)] += static_cast<int>(header.blocks);
        report.sampleRates.push_back(header.sampleRate);
        offset += header.sizeBytes;
    }

    report.completeAccessUnitShape = report.syncframes > 0 && !report.truncated;
    for (const auto &[key, blocks] : report.blocksByStream) {
        (void)key;
        if (blocks != 6) {
            report.completeAccessUnitShape = false;
        }
    }
    return report;
}

std::string errorText(int error)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(error, buffer, sizeof(buffer));
    return buffer;
}

std::string layoutText(const AVChannelLayout &layout)
{
    char buffer[256] = {};
    if (av_channel_layout_describe(&layout, buffer, sizeof(buffer)) < 0) {
        return "unknown";
    }
    return buffer;
}

std::string sampleFormatText(int format)
{
    const char *name = av_get_sample_fmt_name(static_cast<AVSampleFormat>(format));
    return name ? name : "unknown";
}

std::string blockMapText(const std::map<std::string, int> &blocks)
{
    std::ostringstream output;
    bool first = true;
    for (const auto &[key, value] : blocks) {
        if (!first) {
            output << ',';
        }
        first = false;
        output << key << ':' << value;
    }
    return output.str();
}

void printUsage()
{
    std::cerr << "Usage: Eac3Gate0Probe <container> [--max-packets N]\n";
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        printUsage();
        return 2;
    }

    int maxPackets = 100;
    for (int index = 2; index < argc; ++index) {
        const std::string option = argv[index];
        if (option == "--max-packets" && index + 1 < argc) {
            maxPackets = std::max(1, std::atoi(argv[++index]));
        } else {
            printUsage();
            return 2;
        }
    }

    AVFormatContext *format = nullptr;
    int result = avformat_open_input(&format, argv[1], nullptr, nullptr);
    if (result < 0) {
        std::cerr << "probeResult=FAIL stage=avformat_open_input error=" << errorText(result) << '\n';
        return 1;
    }

    result = avformat_find_stream_info(format, nullptr);
    if (result < 0) {
        std::cerr << "probeResult=FAIL stage=avformat_find_stream_info error=" << errorText(result) << '\n';
        avformat_close_input(&format);
        return 1;
    }

    const AVCodec *codec = nullptr;
    const int streamIndex = av_find_best_stream(format,
                                                AVMEDIA_TYPE_AUDIO,
                                                -1,
                                                -1,
                                                &codec,
                                                0);
    if (streamIndex < 0 || !codec) {
        std::cerr << "probeResult=FAIL stage=av_find_best_stream error=" << errorText(streamIndex) << '\n';
        avformat_close_input(&format);
        return 1;
    }

    AVCodecContext *decoder = avcodec_alloc_context3(codec);
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    if (!decoder || !packet || !frame) {
        std::cerr << "probeResult=FAIL stage=allocation\n";
        avcodec_free_context(&decoder);
        av_packet_free(&packet);
        av_frame_free(&frame);
        avformat_close_input(&format);
        return 1;
    }

    const AVStream *stream = format->streams[streamIndex];
    result = avcodec_parameters_to_context(decoder, stream->codecpar);
    if (result >= 0) {
        result = avcodec_open2(decoder, codec, nullptr);
    }
    if (result < 0) {
        std::cerr << "probeResult=FAIL stage=avcodec_open2 error=" << errorText(result) << '\n';
        avcodec_free_context(&decoder);
        av_packet_free(&packet);
        av_frame_free(&frame);
        avformat_close_input(&format);
        return 1;
    }

    std::vector<PacketReport> packetReports;
    std::vector<DecodedFrameReport> frameReports;
    int packetIndex = 0;
    int frameIndex = 0;
    int lastSentPacketIndex = -1;

    auto receiveFrames = [&]() -> bool {
        while (true) {
            result = avcodec_receive_frame(decoder, frame);
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
                return true;
            }
            if (result < 0) {
                std::cerr << "probeResult=FAIL stage=avcodec_receive_frame error=" << errorText(result) << '\n';
                return false;
            }

            DecodedFrameReport report;
            report.index = frameIndex++;
            report.sourcePacketIndex = lastSentPacketIndex;
            report.pts = frame->best_effort_timestamp != AV_NOPTS_VALUE
                ? frame->best_effort_timestamp
                : frame->pts;
            report.samples = frame->nb_samples;
            report.channels = frame->ch_layout.nb_channels;
            report.sampleRate = frame->sample_rate;
            report.format = frame->format;
            report.profile = decoder->profile;
            report.layout = layoutText(frame->ch_layout);
            frameReports.push_back(report);
            av_frame_unref(frame);
        }
    };

    while (packetReports.size() < static_cast<std::size_t>(maxPackets)
           && (result = av_read_frame(format, packet)) >= 0) {
        if (packet->stream_index != streamIndex) {
            av_packet_unref(packet);
            continue;
        }

        ++packetIndex;
        packetReports.push_back(inspectPacket(packet, packetIndex));
        lastSentPacketIndex = packetIndex;
        result = avcodec_send_packet(decoder, packet);
        av_packet_unref(packet);
        if (result < 0 && result != AVERROR(EAGAIN)) {
            std::cerr << "probeResult=FAIL stage=avcodec_send_packet error=" << errorText(result) << '\n';
            avcodec_free_context(&decoder);
            av_packet_free(&packet);
            av_frame_free(&frame);
            avformat_close_input(&format);
            return 1;
        }
        if (!receiveFrames()) {
            avcodec_free_context(&decoder);
            av_packet_free(&packet);
            av_frame_free(&frame);
            avformat_close_input(&format);
            return 1;
        }
    }

    result = avcodec_send_packet(decoder, nullptr);
    if (result >= 0 || result == AVERROR_EOF) {
        receiveFrames();
    }

    const AVCodecParameters *parameters = stream->codecpar;
    int packetShapeCount = 0;
    int packetDuration1536Count = 0;
    int independentSyncframeCount = 0;
    int dependentSyncframeCount = 0;
    for (const PacketReport &report : packetReports) {
        packetShapeCount += report.completeAccessUnitShape ? 1 : 0;
        independentSyncframeCount += report.independentSyncframes;
        dependentSyncframeCount += report.dependentSyncframes;
        const std::int64_t samples = av_rescale_q(report.duration,
                                                  stream->time_base,
                                                  AVRational {1, parameters->sample_rate});
        packetDuration1536Count += samples == 1536 ? 1 : 0;
    }

    int frame1536Count = 0;
    std::int64_t packetSamples = 0;
    std::int64_t decodedSamples = 0;
    std::int64_t skipSamples = 0;
    std::int64_t discardPadding = 0;
    for (const DecodedFrameReport &report : frameReports) {
        frame1536Count += report.samples == 1536 ? 1 : 0;
        decodedSamples += report.samples;
    }
    for (const PacketReport &report : packetReports) {
        packetSamples += av_rescale_q(report.duration,
                                      stream->time_base,
                                      AVRational {1, parameters->sample_rate});
        skipSamples += report.skipSamples;
        discardPadding += report.discardPadding;
    }

    for (const PacketReport &report : packetReports) {
        const std::int64_t durationSamples = av_rescale_q(report.duration,
                                                           stream->time_base,
                                                           AVRational {1, parameters->sample_rate});
        std::cout << "packet[" << report.index << "]"
                  << " pts=" << report.pts
                  << " durationSamples=" << durationSamples
                  << " bytes=" << report.bytes
                  << " syncframes=" << report.syncframes
                  << " independentSyncframes=" << report.independentSyncframes
                  << " dependentSyncframes=" << report.dependentSyncframes
                  << " totalBlocks=" << report.totalBlocks
                  << " blocksByStream=" << blockMapText(report.blocksByStream)
                  << " completeAccessUnitShape=" << (report.completeAccessUnitShape ? 1 : 0)
                  << " skipSamples=" << report.skipSamples
                  << " discardPadding=" << report.discardPadding
                  << " truncated=" << (report.truncated ? 1 : 0) << '\n';
    }
    for (const DecodedFrameReport &report : frameReports) {
        std::cout << "frame[" << report.index << "]"
                  << " sourcePacket=" << report.sourcePacketIndex
                  << " pts=" << report.pts
                  << " samples=" << report.samples
                  << " rate=" << report.sampleRate
                  << " channels=" << report.channels
                  << " layout=" << report.layout
                  << " format=" << sampleFormatText(report.format)
                  << " profile=" << report.profile << '\n';
    }

    const bool hasPackets = !packetReports.empty();
    const bool hasFrames = !frameReports.empty();
    const bool packetShapePass = hasPackets && packetShapeCount == static_cast<int>(packetReports.size());
    const bool packetDurationPass = hasPackets && packetDuration1536Count == static_cast<int>(packetReports.size());
    const bool nativePcmFormatPass = hasFrames
        && std::all_of(frameReports.begin(), frameReports.end(), [&](const DecodedFrameReport &report) {
               return report.sampleRate == decoder->sample_rate
                   && report.channels == decoder->ch_layout.nb_channels
                   && report.format == AV_SAMPLE_FMT_FLTP;
           });
    const std::int64_t expectedDecodedSamples = std::max<std::int64_t>(
        0, packetSamples - skipSamples - discardPadding);
    const bool nativePcmSampleCountPass = nativePcmFormatPass
        && decodedSamples == expectedDecodedSamples;

    std::cout << "input=" << argv[1] << '\n'
              << "audioStreamIndex=" << streamIndex << '\n'
              << "codecId=" << parameters->codec_id << '\n'
              << "codecName=" << (codec->name ? codec->name : "unknown") << '\n'
              << "codecProfile=" << decoder->profile << '\n'
              << "codecSampleRate=" << decoder->sample_rate << '\n'
              << "codecChannels=" << decoder->ch_layout.nb_channels << '\n'
              << "codecLayout=" << layoutText(decoder->ch_layout) << '\n'
              << "packets=" << packetReports.size() << '\n'
              << "packetShapePassCount=" << packetShapeCount << '\n'
              << "packetDuration1536Count=" << packetDuration1536Count << '\n'
              << "independentSyncframeCount=" << independentSyncframeCount << '\n'
              << "dependentSyncframeCount=" << dependentSyncframeCount << '\n'
              << "decodedFrames=" << frameReports.size() << '\n'
              << "decodedFrames1536Count=" << frame1536Count << '\n'
              << "packetSamples=" << packetSamples << '\n'
              << "packetSkipSamples=" << skipSamples << '\n'
              << "packetDiscardPadding=" << discardPadding << '\n'
              << "decodedSamples=" << decodedSamples << '\n'
              << "expectedDecodedSamplesAfterPacketTrim=" << expectedDecodedSamples << '\n'
              << "packetEqualsComplete1536AccessUnit="
              << (packetShapePass && packetDurationPass ? "PASS" : "FAIL") << '\n'
              << "nativePcmFormat=" << (nativePcmFormatPass ? "PASS" : "FAIL") << '\n'
              << "nativePcmSampleCountAfterPacketTrim="
              << (nativePcmSampleCountPass ? "PASS_WITH_CODEC_PRIMING" : "INCONCLUSIVE") << '\n'
              << "packetToFrameOneToOne=INCONCLUSIVE reason=codec_skip_samples_or_delay\n"
              << "probeResult=" << (hasPackets && hasFrames ? "PASS" : "FAIL")
              << " stage=gate0-packet-pcm-correlation implementation=libavcodec-plus-self-written-framing\n";

    avcodec_free_context(&decoder);
    av_packet_free(&packet);
    av_frame_free(&frame);
    avformat_close_input(&format);
    return hasPackets && hasFrames ? 0 : 1;
}
