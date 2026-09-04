#include "native-eac3-bsi.h"

#include <array>
#include <limits>

namespace eac3native {
namespace {

BsiParseResult fail(Disposition disposition, FailureStage stage,
                    const char *reason)
{
    BsiParseResult result;
    result.disposition = disposition;
    result.stage = stage;
    result.reason = reason;
    return result;
}

bool read(BoundedBitReader *reader, unsigned count, std::uint32_t *value)
{
    return reader && reader->read(count, value);
}

bool skip(BoundedBitReader *reader, unsigned count)
{
    return reader && reader->skip(count);
}

unsigned channelWeight(std::uint16_t chanmap)
{
    // Table E.1.4: single-channel locations have weight one, while the
    // Lc/Rc, Lrs/Rrs, Lsd/Rsd, Lw/Rw, Vhl/Vhr, and Lts/Rts entries are pairs.
    constexpr std::array<unsigned, 16> weights = {
        1, 1, 1, 1, 1, 2, 2, 1,
        1, 2, 2, 2, 1, 2, 1, 1};
    unsigned total = 0;
    for (unsigned bit = 0; bit < weights.size(); ++bit) {
        if ((chanmap & (static_cast<std::uint16_t>(1U)
                        << (15U - bit))) != 0U) {
            total += weights[bit];
        }
    }
    return total;
}

bool readScale4(BoundedBitReader *reader)
{
    std::uint32_t value = 0;
    return read(reader, 1, &value) && (!value || read(reader, 4, &value));
}

bool readScale6(BoundedBitReader *reader)
{
    std::uint32_t value = 0;
    return read(reader, 1, &value) && (!value || read(reader, 6, &value));
}

bool readMixOption4(BoundedBitReader *reader, unsigned mixdeflen,
                    unsigned *mixdataFillBits)
{
    const std::size_t optionStart = reader->position();
    std::uint32_t value = 0;
    if (!read(reader, 1, &value)) { // mixdata2e
        return false;
    }
    if (value != 0U) {
        if (!read(reader, 1, &value) // premixcmpsel
            || !read(reader, 1, &value) // drcsrc
            || !read(reader, 3, &value) // premixcmpscl
            || !readScale4(reader) // extpgmlscle + optional value
            || !readScale4(reader) // extpgmcscle
            || !readScale4(reader) // extpgmrscle
            || !readScale4(reader) // extpgmlsscle
            || !readScale4(reader) // extpgmrsscle
            || !readScale4(reader) // extpgmlfescle
            || !readScale4(reader)) { // dmixscle
            return false;
        }
        if (!read(reader, 1, &value)) { // addche
            return false;
        }
        if (value != 0U
            && (!readScale4(reader) || !readScale4(reader))) {
            return false;
        }
    }
    if (!read(reader, 1, &value)) { // mixdata3e
        return false;
    }
    if (value != 0U) {
        if (!read(reader, 5, &value) // spchdat
            || !read(reader, 1, &value)) { // addspchdate
            return false;
        }
        if (value != 0U
            && (!read(reader, 5, &value) // spchdat1
                || !read(reader, 2, &value) // spchan1att
                || !read(reader, 1, &value))) { // addspchdat1e
            return false;
        }
        if (value != 0U
            && (!read(reader, 5, &value) || !read(reader, 3, &value))) {
            return false;
        }
    }

    const std::size_t totalBits =
        (static_cast<std::size_t>(mixdeflen) + 2U) * 8U;
    const std::size_t optionBits = reader->position() - optionStart;
    if (optionBits > totalBits) {
        return false;
    }
    const std::size_t mixdataBits = totalBits - optionBits;
    if (mixdataBits > std::numeric_limits<unsigned>::max()) {
        return false;
    }
    if (!skip(reader, static_cast<unsigned>(mixdataBits))) {
        return false;
    }
    const unsigned fillBits = static_cast<unsigned>(
        (8U - (reader->position() % 8U)) % 8U);
    if (!skip(reader, fillBits)) {
        return false;
    }
    if (mixdataFillBits) {
        *mixdataFillBits = fillBits;
    }
    return true;
}

} // namespace

BsiParseResult parseEac3Bsi(const std::vector<std::uint8_t> &bytes,
                            const FrameHeader &frame)
{
    if (frame.streamType == StreamType::LegacyAc3) {
        BsiInfo info;
        info.legacyAc3 = true;
        info.bsid = frame.bsid;
        info.acmod = frame.acmod;
        info.lfeon = frame.lfe;
        BsiParseResult result;
        result.disposition = Disposition::Accepted;
        result.stage = FailureStage::None;
        result.reason = "legacy-ac3-bsi-not-parsed";
        result.info = info;
        return result;
    }
    if (frame.streamType == StreamType::Reserved
        || frame.streamType == StreamType::Dependent
               && frame.substreamId > 7U) {
        return fail(Disposition::Unsupported, FailureStage::Header,
                    "unsupported-bsi-stream");
    }
    if (frame.bsid < 11U || frame.bsid > 16U) {
        return fail(Disposition::Unsupported, FailureStage::Header,
                    "unsupported-bsi-bsid");
    }
    if (frame.offset > bytes.size()
        || frame.endBit > bytes.size() * 8U
        || frame.endBit < frame.offset * 8U + 45U) {
        return fail(Disposition::Malformed, FailureStage::Bounds,
                    "bsi-frame-boundary");
    }

    BoundedBitReader reader(bytes.data(), bytes.size(),
                            frame.offset * 8U + 45U, frame.endBit);
    BsiInfo info;
    info.strmtyp = static_cast<unsigned>(frame.streamType);
    info.substreamId = frame.substreamId;
    info.acmod = frame.acmod;
    info.lfeon = frame.lfe;
    info.bsid = frame.bsid;
    info.dualMono = frame.acmod == 0U;
    std::uint32_t value = 0;

    if (!read(&reader, 5, &value)) {
        return fail(Disposition::Malformed, FailureStage::Header,
                    "truncated-dialnorm");
    }
    info.dialnorm = value;
    if (!read(&reader, 1, &value)) {
        return fail(Disposition::Malformed, FailureStage::Header,
                    "truncated-compre");
    }
    info.compre = value != 0U;
    if (info.compre && !read(&reader, 8, &value)) {
        return fail(Disposition::Malformed, FailureStage::Header,
                    "truncated-compr");
    }
    if (info.compre) {
        info.comprPresent = true;
        info.compr = value;
    }
    if (info.dualMono) {
        if (!read(&reader, 5, &value) || !read(&reader, 1, &value)) {
            return fail(Disposition::Malformed, FailureStage::Header,
                        "truncated-dual-mono-bsi");
        }
        info.compr2e = value != 0U;
        if (info.compr2e && !read(&reader, 8, &value)) {
            return fail(Disposition::Malformed, FailureStage::Header,
                        "truncated-compr2");
        }
    }

    if (frame.streamType == StreamType::Dependent) {
        if (!read(&reader, 1, &value)) {
            return fail(Disposition::Malformed, FailureStage::Header,
                        "truncated-chanmape");
        }
        info.chanmape = value != 0U;
        if (info.chanmape) {
            if (!read(&reader, 16, &value)) {
                return fail(Disposition::Malformed, FailureStage::Header,
                            "truncated-chanmap");
            }
            info.chanmap = static_cast<std::uint16_t>(value);
            info.chanmapChannelWeight = channelWeight(info.chanmap);
            if (info.chanmapChannelWeight != frame.channelCount) {
                return fail(Disposition::Malformed, FailureStage::Header,
                            "chanmap-channel-count-mismatch");
            }
        }
    }

    if (!read(&reader, 1, &value)) {
        return fail(Disposition::Malformed, FailureStage::Header,
                    "truncated-mixmdate");
    }
    info.mixmdate = value != 0U;
    if (info.mixmdate) {
        if (frame.acmod > 2U && !skip(&reader, 2U)) {
            return fail(Disposition::Malformed, FailureStage::Header,
                        "truncated-dmixmod");
        }
        if ((frame.acmod & 1U) != 0U && frame.acmod > 2U
            && (!skip(&reader, 3U) || !skip(&reader, 3U))) {
            return fail(Disposition::Malformed, FailureStage::Header,
                        "truncated-front-mix-level");
        }
        if ((frame.acmod & 4U) != 0U
            && (!skip(&reader, 3U) || !skip(&reader, 3U))) {
            return fail(Disposition::Malformed, FailureStage::Header,
                        "truncated-surround-mix-level");
        }
        if (frame.lfe) {
            if (!read(&reader, 1U, &value)) {
                return fail(Disposition::Malformed, FailureStage::Header,
                            "truncated-lfe-mix-level-code");
            }
            info.lfemixlevcode = value != 0U;
            if (info.lfemixlevcode) {
                if (!read(&reader, 5U, &value)) {
                    return fail(Disposition::Malformed, FailureStage::Header,
                                "truncated-lfe-mix-level");
                }
                info.lfemixlevcod = value;
            }
        }
        if (frame.streamType == StreamType::Independent) {
            if (!readScale6(&reader)) {
                return fail(Disposition::Malformed, FailureStage::Header,
                            "truncated-pgmscl");
            }
            if (frame.acmod == 0U && !readScale6(&reader)) {
                return fail(Disposition::Malformed, FailureStage::Header,
                            "truncated-pgmscl2");
            }
            if (!readScale6(&reader) || !read(&reader, 2, &value)) {
                return fail(Disposition::Malformed, FailureStage::Header,
                            "truncated-extpgm-mixdef");
            }
            info.mixdef = value;
            if (info.mixdef == 1U) {
                if (!skip(&reader, 5U)) {
                    return fail(Disposition::Malformed, FailureStage::Header,
                                "truncated-mixdef1");
                }
            } else if (info.mixdef == 2U) {
                if (!skip(&reader, 12U)) {
                    return fail(Disposition::Malformed, FailureStage::Header,
                                "truncated-mixdef2");
                }
            } else if (info.mixdef == 3U) {
                if (!read(&reader, 5, &value)) {
                    return fail(Disposition::Malformed, FailureStage::Header,
                                "truncated-mixdeflen");
                }
                info.mixdeflen = value;
                if (!readMixOption4(&reader, info.mixdeflen,
                                    &info.mixdataFillBits)) {
                    return fail(Disposition::Malformed, FailureStage::Header,
                                "truncated-mixdef3");
                }
            }
            if (frame.acmod < 2U) {
                if (!read(&reader, 1, &value)) {
                    return fail(Disposition::Malformed, FailureStage::Header,
                                "truncated-paninfoe");
                }
                info.paninfoe = value != 0U;
                if (info.paninfoe && !skip(&reader, 14U)) {
                    return fail(Disposition::Malformed, FailureStage::Header,
                                "truncated-paninfo");
                }
                if (frame.acmod == 0U) {
                    if (!read(&reader, 1, &value)) {
                        return fail(Disposition::Malformed, FailureStage::Header,
                                    "truncated-paninfo2e");
                    }
                    info.paninfo2e = value != 0U;
                    if (info.paninfo2e && !skip(&reader, 14U)) {
                        return fail(Disposition::Malformed, FailureStage::Header,
                                    "truncated-paninfo2");
                    }
                }
            }
            if (!read(&reader, 1, &value)) {
                return fail(Disposition::Malformed, FailureStage::Header,
                            "truncated-frmmixcfginfoe");
            }
            info.frmmixcfginfoe = value != 0U;
            if (info.frmmixcfginfoe) {
                if (frame.blocks == 1U) {
                    if (!skip(&reader, 5U)) {
                        return fail(Disposition::Malformed, FailureStage::Header,
                                    "truncated-blkmixcfginfo");
                    }
                } else {
                    for (unsigned block = 0; block < frame.blocks; ++block) {
                        if (!read(&reader, 1, &value)
                            || (value != 0U && !skip(&reader, 5U))) {
                            return fail(Disposition::Malformed, FailureStage::Header,
                                        "truncated-blkmixcfginfo");
                        }
                    }
                }
            }
        }
    }

    if (!read(&reader, 1, &value)) {
        return fail(Disposition::Malformed, FailureStage::Header,
                    "truncated-infomdate");
    }
    info.infomdate = value != 0U;
    if (info.infomdate) {
        if (!skip(&reader, 3U) // bsmod
            || !skip(&reader, 1U) // copyrightb
            || !skip(&reader, 1U)) { // origbs
            return fail(Disposition::Malformed, FailureStage::Header,
                        "truncated-info-metadata");
        }
        if (frame.acmod == 2U && (!skip(&reader, 2U) || !skip(&reader, 2U))) {
            return fail(Disposition::Malformed, FailureStage::Header,
                        "truncated-info-2-0");
        }
        if (frame.acmod >= 6U && !skip(&reader, 2U)) {
            return fail(Disposition::Malformed, FailureStage::Header,
                        "truncated-info-surround");
        }
        if (!read(&reader, 1, &value)) {
            return fail(Disposition::Malformed, FailureStage::Header,
                        "truncated-audprodie");
        }
        if (value != 0U
            && (!skip(&reader, 5U) || !skip(&reader, 2U) || !skip(&reader, 1U))) {
            return fail(Disposition::Malformed, FailureStage::Header,
                        "truncated-info-programme");
        }
        if (frame.acmod == 0U) {
            if (!read(&reader, 1, &value)) {
                return fail(Disposition::Malformed, FailureStage::Header,
                            "truncated-audprodi2e");
            }
            if (value != 0U
                && (!skip(&reader, 5U) || !skip(&reader, 2U)
                    || !skip(&reader, 1U))) {
                return fail(Disposition::Malformed, FailureStage::Header,
                            "truncated-info-programme2");
            }
        }
        if (!skip(&reader, 1U)) {
            return fail(Disposition::Malformed, FailureStage::Header,
                        "truncated-sourcefscod");
        }
    }
    if (frame.streamType == StreamType::Independent && frame.blocks != 6U) {
        if (!read(&reader, 1, &value)) {
            return fail(Disposition::Malformed, FailureStage::Header,
                        "truncated-convsync");
        }
        info.convsyncPresent = true;
        info.convsync = value != 0U;
    }
    if (frame.streamType == StreamType::Ac3Convert) {
        info.blkidPresent = frame.blocks != 6U;
        info.blkid = frame.blocks == 6U;
        if (info.blkidPresent) {
            if (!read(&reader, 1, &value)) {
                return fail(Disposition::Malformed, FailureStage::Header,
                            "truncated-blkid");
            }
            info.blkid = value != 0U;
        }
        if (info.blkid) {
            if (!read(&reader, 6, &value)) {
                return fail(Disposition::Malformed, FailureStage::Header,
                            "truncated-frmsizecod");
            }
            info.frmsizecodPresent = true;
            info.frmsizecod = value;
        }
    }
    if (!read(&reader, 1, &value)) {
        return fail(Disposition::Malformed, FailureStage::Header,
                    "truncated-addbsie");
    }
    info.addbsie = value != 0U;
    if (info.addbsie) {
        if (!read(&reader, 6, &value)) {
            return fail(Disposition::Malformed, FailureStage::Header,
                        "truncated-addbsil");
        }
        info.addbsil = value;
        const std::size_t addbsiBits =
            (static_cast<std::size_t>(info.addbsil) + 1U) * 8U;
        if (addbsiBits > std::numeric_limits<unsigned>::max()
            || !skip(&reader, static_cast<unsigned>(addbsiBits))) {
            return fail(Disposition::Malformed, FailureStage::Header,
                        "truncated-addbsi");
        }
    }

    info.bsiParsed = true;
    info.bsiEndBit = reader.position() - frame.offset * 8U;
    BsiParseResult result;
    result.disposition = Disposition::Accepted;
    result.stage = FailureStage::None;
    result.info = info;
    return result;
}

} // namespace eac3native
