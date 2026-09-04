#include "oamd-b2a.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

class BitWriter {
public:
    void write(unsigned count, unsigned value)
    {
        for (unsigned bit = 0; bit < count; ++bit) {
            bits_.push_back(static_cast<std::uint8_t>(
                (value >> (count - bit - 1U)) & 1U));
        }
    }

    std::vector<std::uint8_t> bytes(bool nonzeroTail = false,
                                    bool truncate = false) const
    {
        std::vector<std::uint8_t> result((bits_.size() + 7U) / 8U, 0);
        for (std::size_t index = 0; index < bits_.size(); ++index) {
            if (bits_[index] != 0) {
                result[index / 8U]
                    |= static_cast<std::uint8_t>(1U << (7U - index % 8U));
            }
        }
        if (nonzeroTail) {
            if (bits_.size() % 8U == 0) {
                result.push_back(0x80);
            } else if (!result.empty()) {
                result.back() |= 0x01;
            }
        }
        if (truncate && !result.empty()) {
            result.pop_back();
        }
        return result;
    }

    std::size_t bitCount() const { return bits_.size(); }

private:
    std::vector<std::uint8_t> bits_;
};

void writeFullRender(BitWriter *writer, unsigned block, bool includePresence,
                     unsigned presenceMask = 0xf,
                     eac3oamd::B2aBitOrder wireOrder =
                         eac3oamd::B2aBitOrder::Syntax5511Lsb)
{
    if (includePresence) {
        writer->write(4, presenceMask);
    }
    const unsigned positionMask = wireOrder == eac3oamd::B2aBitOrder::Syntax5511Lsb
        ? 0x1U : 0x8U;
    const unsigned zoneMask = wireOrder == eac3oamd::B2aBitOrder::Syntax5511Lsb
        ? 0x2U : 0x4U;
    const unsigned sizeMask = wireOrder == eac3oamd::B2aBitOrder::Syntax5511Lsb
        ? 0x4U : 0x2U;
    const unsigned screenMask = wireOrder == eac3oamd::B2aBitOrder::Syntax5511Lsb
        ? 0x8U : 0x1U;
    if ((presenceMask & positionMask) != 0) {
      if (block == 0) {
        writer->write(6, 0x15);
        writer->write(6, 0x2a);
        writer->write(1, 1);
        writer->write(4, 0x7);
      } else {
        writer->write(1, 1); // differential position
        writer->write(3, 1);
        writer->write(3, 2);
        writer->write(3, 3);
      }
      writer->write(1, 1); // distance specified
      writer->write(1, block == 0 ? 0U : 1U); // finite first, infinity later
      if (block == 0) {
          writer->write(4, 4);
      }
    }
    if ((presenceMask & zoneMask) != 0) {
      writer->write(3, 5); // zone
      writer->write(1, 1); // elevation
    }
    if ((presenceMask & sizeMask) != 0) {
      writer->write(2, block == 0 ? 1U : 2U);
      if (block == 0) {
          writer->write(5, 7);
      } else {
          writer->write(5, 3);
          writer->write(5, 4);
          writer->write(5, 5);
      }
    }
    if ((presenceMask & screenMask) != 0) {
      writer->write(1, 1); // screen reference
      writer->write(3, 2);
      writer->write(2, 1);
    }
    writer->write(1, block & 1U); // snap
}

std::vector<std::uint8_t> makeObjectBody(unsigned objectCount,
                                          unsigned blockCount,
                                          bool branches,
                                          bool additional,
                                          bool nonzeroPadding = false,
                                          bool truncate = false,
                                          std::size_t *bodyBits = nullptr,
                                          bool objectInBedOrIsf = false,
                                          unsigned sampleOffsetCode = 2,
                                          unsigned mixedRenderMask = 0xf,
                                          int helperObjectIndex = -1,
                                          eac3oamd::B2aBitOrder mixedRenderOrder =
                                              eac3oamd::B2aBitOrder::Syntax5511Lsb)
{
    BitWriter writer;
    writer.write(2, sampleOffsetCode);
    if (sampleOffsetCode == 1U) {
        writer.write(2, 2); // sample_offset_idx
    } else if (sampleOffsetCode == 2U) {
        writer.write(5, 17); // sample_offset_bits
    }
    writer.write(3, blockCount - 1U);
    for (unsigned block = 0; block < blockCount; ++block) {
        writer.write(6, block * 3U);
        if (branches && block == 0) {
            writer.write(2, 3);
            writer.write(1, 1);
            writer.write(4, 9);
        } else if (branches && block == 1) {
            writer.write(2, 3);
            writer.write(1, 0);
            writer.write(11, 321);
        } else {
            writer.write(2, 0);
        }
    }
    writer.write(1, 1); // reserved data not present
    for (unsigned object = 0; object < objectCount; ++object) {
        for (unsigned block = 0; block < blockCount; ++block) {
            const bool inactive = branches && blockCount >= 8U
                && object == 0 && block == blockCount - 1U;
            writer.write(1, inactive ? 1U : 0U);
            if (inactive) {
                writer.write(1, additional && object == 0 && block == blockCount - 1U
                                  ? 1U : 0U);
                if (additional && object == 0 && block == blockCount - 1U) {
                    writer.write(4, 0);
                    writer.write(8, 0xa5);
                }
                continue;
            }
            if (block == 0) {
                // Full basic info is implicit status 01 on the first block.
                writer.write(2, 2); // gain index with six raw gain bits
                writer.write(6, 31);
                writer.write(1, 0); // non-default priority
                writer.write(5, 12);
            } else if (branches && block == 1) {
                writer.write(2, 3); // mixed basic update
                writer.write(1, 1); // gain present
                writer.write(1, 1); // priority present
                writer.write(2, 1); // gain index 01
                writer.write(1, 1); // default priority
            } else if (branches && block == 2) {
                writer.write(2, 2); // full reuse, no basic payload
            } else {
                writer.write(2, 0); // default basic status
            }
            const bool thisObjectInBedOrIsf = objectInBedOrIsf
                && (helperObjectIndex < 0
                    || static_cast<int>(object) == helperObjectIndex);
            if (thisObjectInBedOrIsf) {
                writer.write(1, 0); // no additional table data
                continue;
            }
            if (block == 0) {
                writeFullRender(&writer, block, false);
            } else if (branches && block == 1) {
                writer.write(2, 3); // mixed render info
                writeFullRender(&writer, block, true, mixedRenderMask, mixedRenderOrder);
            } else if (branches && block == 2) {
                writer.write(2, 2); // full reuse render info
            } else {
                writer.write(2, 0); // default render info
            }
            const bool hasAdditional = additional && object == 0 && block == 0;
            writer.write(1, hasAdditional ? 1U : 0U);
            if (hasAdditional) {
                writer.write(4, 0); // one byte total additional block
                writer.write(8, 0x5a);
            }
        }
    }
    if (nonzeroPadding) {
        writer.write(1, 1);
    }
    if (bodyBits) {
        *bodyBits = writer.bitCount();
    }
    return writer.bytes(false, truncate);
}

bool expect(const eac3oamd::B2aFrame &frame,
            eac3oamd::B2aDisposition disposition,
            const char *label)
{
    if (frame.disposition != disposition) {
        std::cerr << "b2aSelfTest=FAIL case=" << label
                  << " disposition=" << eac3oamd::b2aDispositionText(frame.disposition)
                  << " reason=" << frame.reason << '\n';
        return false;
    }
    return true;
}

eac3oamd::B2aSelfTestReport runSynthetic()
{
    eac3oamd::B2aSelfTestReport report;
    const std::vector<bool> dynamicOne(1, false);
    std::size_t oneBits = 0;
    const auto one = makeObjectBody(1, 1, true, true, false, false, &oneBits);
    const auto oneFrame = eac3oamd::parseObjectElement(one, oneBits, 1, dynamicOne);
    if (!expect(oneFrame, eac3oamd::B2aDisposition::Pass, "one-block-all-branches")
        || oneFrame.objectInfoBlockCount != 1 || oneFrame.objectInfo.size() != 1
        || !oneFrame.objectInfo[0].additionalDataPresent) {
        report.reason = "one-block-all-branches-failed";
        return report;
    }
    ++report.cases;
    ++report.oneBlockCases;
    ++report.branchCases;
    ++report.additionalDataCases;

    std::size_t offset0Bits = 0;
    const auto offset0 = makeObjectBody(1, 1, false, false, false, false,
                                        &offset0Bits, false, 0);
    const auto offset0Frame = eac3oamd::parseObjectElement(offset0, offset0Bits, 1,
                                                            dynamicOne);
    std::size_t offset1Bits = 0;
    const auto offset1 = makeObjectBody(1, 1, false, false, false, false,
                                        &offset1Bits, false, 1);
    const auto offset1Frame = eac3oamd::parseObjectElement(offset1, offset1Bits, 1,
                                                            dynamicOne);
    if (!expect(offset0Frame, eac3oamd::B2aDisposition::Pass, "sample-offset-code-0")
        || !expect(offset1Frame, eac3oamd::B2aDisposition::Pass, "sample-offset-code-1")
        || !offset1Frame.sampleOffsetIndexPresent) {
        report.reason = "sample-offset-coding-failed";
        return report;
    }
    report.cases += 2;
    report.sampleOffsetCases = 2;

    std::size_t reservedBits = 0;
    const auto reserved = makeObjectBody(1, 1, false, false, false, false,
                                         &reservedBits, false, 3);
    if (!expect(eac3oamd::parseObjectElement(reserved, reservedBits, 1, dynamicOne),
                eac3oamd::B2aDisposition::Unsupported, "reserved-sample-offset-code")) {
        report.reason = "reserved-sample-offset-code-failed";
        return report;
    }
    ++report.cases;
    ++report.reservedCases;

    // Raw wire masks are encoded independently of the parser.  Under
    // 5.5.11/LSB, 0x1/0x2/0x4/0x8 mean position/zone/size/screen.
    const unsigned transmittedMasks[] = {1U, 2U, 4U, 8U};
    for (unsigned field = 0; field < 4; ++field) {
        std::size_t presenceBits = 0;
        const auto presenceBody = makeObjectBody(1, 2, true, false, false, false,
                                                 &presenceBits, false, 2,
                                                 transmittedMasks[field], -1,
                                                 eac3oamd::B2aBitOrder::Syntax5511Lsb);
        const auto lsbFrame = eac3oamd::parseObjectElement(
            presenceBody, presenceBits, 1, dynamicOne,
            eac3oamd::B2aBitOrder::Syntax5511Lsb);
        const bool lsbMapped = lsbFrame.disposition == eac3oamd::B2aDisposition::Pass
            && lsbFrame.objectInfo.size() > 1
            && lsbFrame.objectInfo[1].renderInfoPresence[field];
        if (!expect(lsbFrame, eac3oamd::B2aDisposition::Pass,
                    "individual-presence-lsb") || !lsbMapped) {
            const unsigned lsbStatus = lsbFrame.objectInfo.size() > 1
                ? lsbFrame.objectInfo[1].renderInfoStatus : 99U;
            report.reason = "individual-presence-bit-order-failed field="
                + std::to_string(field) + ":lsb="
                + std::to_string(lsbStatus)
                + ":lsbDisp=" + eac3oamd::b2aDispositionText(lsbFrame.disposition)
                + ":lsbReason=" + lsbFrame.reason;
            return report;
        }
        ++report.cases;
        ++report.individualPresenceCases;
    }

    // Independent Table 31 control: raw mask 0x8 must select position.
    std::size_t table31PresenceBits = 0;
    const auto table31PresenceBody = makeObjectBody(
        1, 2, true, false, false, false, &table31PresenceBits, false, 2, 8U, -1,
        eac3oamd::B2aBitOrder::Table31Msb);
    const auto table31PresenceFrame = eac3oamd::parseObjectElement(
        table31PresenceBody, table31PresenceBits, 1, dynamicOne,
        eac3oamd::B2aBitOrder::Table31Msb);
    const bool table31PositionOnly = table31PresenceFrame.objectInfo.size() > 1
        && table31PresenceFrame.objectInfo[1].renderInfoPresence[0]
        && !table31PresenceFrame.objectInfo[1].renderInfoPresence[1]
        && !table31PresenceFrame.objectInfo[1].renderInfoPresence[2]
        && !table31PresenceFrame.objectInfo[1].renderInfoPresence[3];
    if (!expect(table31PresenceFrame, eac3oamd::B2aDisposition::Pass,
                "table31-raw-mask-8-position") || !table31PositionOnly) {
        report.reason = "table31-raw-mask-8-position-failed";
        return report;
    }
    ++report.cases;
    ++report.table31PresenceCases;

    std::size_t eightBits = 0;
    const auto eight = makeObjectBody(1, 8, true, false, false, false, &eightBits);
    const auto eightFrame = eac3oamd::parseObjectElement(eight, eightBits, 1, dynamicOne);
    if (!expect(eightFrame, eac3oamd::B2aDisposition::Pass, "eight-block-branches")
        || eightFrame.objectInfoBlockCount != 8 || eightFrame.objectInfo.size() != 8) {
        report.reason = "eight-block-branches-failed";
        return report;
    }
    ++report.cases;
    ++report.eightBlockCases;
    ++report.branchCases;

    std::size_t inactiveBits = 0;
    const auto bed = makeObjectBody(1, 1, false, false, false, false, &inactiveBits,
                                    true);
    const auto inactiveFrame = eac3oamd::parseObjectElement(
        bed, inactiveBits, 1, std::vector<bool>(1, true));
    if (!expect(inactiveFrame, eac3oamd::B2aDisposition::Pass, "bed-object-default-render")) {
        report.reason = "bed-object-default-render-failed";
        return report;
    }
    ++report.cases;
    ++report.branchCases;

    // Dynamic-only + LFE carries a single LFE-only bed record at object 0,
    // while the object element still declares and serializes all 16 records.
    std::size_t lfeHelperBits = 0;
    const auto lfeHelperBody = makeObjectBody(16, 1, false, false, false, false,
                                              &lfeHelperBits, true, 2, 0xf, 0);
    std::vector<bool> lfeHelperMap(16, false);
    lfeHelperMap[0] = true;
    std::vector<bool> wrongLfeHelperMap(16, false);
    wrongLfeHelperMap[1] = true;
    for (const eac3oamd::B2aBitOrder order : {
             eac3oamd::B2aBitOrder::Syntax5511Lsb,
             eac3oamd::B2aBitOrder::Table31Msb}) {
        const auto helperFrame = eac3oamd::parseObjectElement(
            lfeHelperBody, lfeHelperBits, 16, lfeHelperMap, order);
        const bool helperShape = helperFrame.objectInfo.size() == 16U
            && helperFrame.objectInfo[0].renderInfoStatus == 0U
            && helperFrame.objectInfo[1].renderInfoStatus == 1U;
        if (!expect(helperFrame, eac3oamd::B2aDisposition::Pass,
                    "dynamic-lfe-index0-helper") || !helperShape) {
            report.reason = "dynamic-lfe-index0-helper-failed";
            return report;
        }
        ++report.cases;
        ++report.lfeHelperCases;

        const auto wrongFrame = eac3oamd::parseObjectElement(
            lfeHelperBody, lfeHelperBits, 16, wrongLfeHelperMap, order);
        if (!expect(wrongFrame, eac3oamd::B2aDisposition::Malformed,
                    "dynamic-lfe-wrong-helper-index")) {
            report.reason = "dynamic-lfe-wrong-helper-index-not-rejected";
            return report;
        }
        ++report.cases;
        ++report.lfeWrongIndexCases;
    }

    std::size_t malformedBits = 0;
    const auto truncated = makeObjectBody(1, 8, true, true, false, true, &malformedBits);
    if (!expect(eac3oamd::parseObjectElement(truncated, malformedBits, 1, dynamicOne),
                eac3oamd::B2aDisposition::Malformed, "late-truncation")) {
        report.reason = "late-truncation-failed";
        return report;
    }
    ++report.cases;
    ++report.truncatedCases;

    std::size_t paddingBits = 0;
    const auto nonzeroPadding = makeObjectBody(1, 1, false, false, true, false, &paddingBits);
    if (!expect(eac3oamd::parseObjectElement(nonzeroPadding, paddingBits, 1, dynamicOne),
                eac3oamd::B2aDisposition::Malformed, "nonzero-padding")) {
        report.reason = "nonzero-padding-failed";
        return report;
    }
    ++report.cases;
    ++report.paddingCases;

    std::size_t arithmeticBits = 0;
    const auto arithmetic = makeObjectBody(1, 1, false, false, false, false, &arithmeticBits);
    if (!expect(eac3oamd::parseObjectElement(arithmetic, arithmeticBits, 160,
                                             std::vector<bool>(160, false)),
                eac3oamd::B2aDisposition::Malformed, "object-count-arithmetic")) {
        report.reason = "object-count-arithmetic-failed";
        return report;
    }
    ++report.cases;
    ++report.arithmeticCases;

    const auto reservedSample = makeObjectBody(1, 1, false, false);
    // The helper starts with sample_offset_code=2; construct the reserved
    // code explicitly while retaining the remainder for bounded rejection.
    BitWriter reservedWriter;
    reservedWriter.write(2, 3);
    reservedWriter.write(1, 0);
    const auto reservedBytes = reservedWriter.bytes();
    if (!expect(eac3oamd::parseObjectElement(reservedBytes, reservedWriter.bitCount(), 1,
                                             dynamicOne),
                eac3oamd::B2aDisposition::Malformed, "reserved-sample-truncation")) {
        report.reason = "reserved-sample-truncation-failed";
        return report;
    }
    ++report.cases;
    ++report.truncatedCases;
    (void)reservedSample;
    report.pass = true;
    report.reason = "bounded-raw-object-update-branches";
    return report;
}

} // namespace

int main()
{
    const auto report = runSynthetic();
    std::cout << "oamdB2aSelfTest=" << (report.pass ? "PASS" : "FAIL")
              << " totalCases=" << report.cases
              << " reason=" << report.reason << '\n'
              << "oamdB2aBranchCases=" << report.branchCases << '\n'
              << "oamdB2aSampleOffsetCases=" << report.sampleOffsetCases << '\n'
              << "oamdB2aIndividualPresenceCases="
              << report.individualPresenceCases << '\n'
              << "oamdB2aTable31PresenceCases="
              << report.table31PresenceCases << '\n'
              << "oamdB2aOneBlockCases=" << report.oneBlockCases << '\n'
              << "oamdB2aEightBlockCases=" << report.eightBlockCases << '\n'
              << "oamdB2aTruncatedCases=" << report.truncatedCases << '\n'
              << "oamdB2aArithmeticCases=" << report.arithmeticCases << '\n'
              << "oamdB2aReservedCases=" << report.reservedCases << '\n'
              << "oamdB2aPaddingCases=" << report.paddingCases << '\n'
              << "oamdB2aAdditionalDataCases=" << report.additionalDataCases << '\n'
              << "oamdB2aLfeHelperCases=" << report.lfeHelperCases << '\n'
              << "oamdB2aLfeWrongIndexCases=" << report.lfeWrongIndexCases << '\n'
              << "oamdB2aResult=" << (report.pass ? "PASS" : "FAIL")
              << " stage=gate6b2a-raw-object-update-syntax\n";
    return report.pass ? 0 : 1;
}
