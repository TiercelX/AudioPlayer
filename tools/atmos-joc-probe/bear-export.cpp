#include "bear-export.h"
#include <filesystem>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <cstdint>
#include <array>

namespace eac3bear {
namespace {

const char *extentPresenceText(const eac3oamd::B2bObjectState &state)
{
    if (!state.effectiveSizePresent)
        return "absent";
    const auto &size = state.size;
    const bool nonZero = std::fabs(size[0]) > 0.0F
        || std::fabs(size[1]) > 0.0F || std::fabs(size[2]) > 0.0F;
    return nonZero ? "non-zero" : "explicit-zero";
}

void writeBoolArray(std::ostream &out, const std::array<bool, 6> &values)
{
    out << '[';
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) out << ',';
        out << (values[i] ? "true" : "false");
    }
    out << ']';
}

void writeBoolArray(std::ostream &out, const std::array<bool, 3> &values)
{
    out << '[';
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) out << ',';
        out << (values[i] ? "true" : "false");
    }
    out << ']';
}

void writeIntArray(std::ostream &out, const std::array<int, 3> &values)
{
    out << '[' << values[0] << ',' << values[1] << ',' << values[2] << ']';
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
        default: result << static_cast<char>(character); break;
        }
    }
    result << '"';
    return result.str();
}

std::uint64_t hashBytes(std::uint64_t hash, const std::uint8_t *data, std::size_t size)
{
    constexpr std::uint64_t prime = 1099511628211ULL;
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= data[index];
        hash *= prime;
    }
    return hash;
}

std::string fileDigest(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) return "unavailable";
    std::array<std::uint8_t, 4096> buffer {};
    std::uint64_t hash = 14695981039346656037ULL;
    while (input) {
        input.read(reinterpret_cast<char *>(buffer.data()), buffer.size());
        const std::streamsize count = input.gcount();
        if (count > 0) hash = hashBytes(hash, buffer.data(), static_cast<std::size_t>(count));
    }
    std::ostringstream result;
    result << "fnv1a64-" << std::hex << std::setw(16) << std::setfill('0') << hash;
    return result.str();
}

} // namespace

Exporter::Exporter(const std::string &directory, const std::string &sourcePath)
    : dir_(directory), sourcePath_(sourcePath) {}
bool Exporter::open(std::string *reason) {
    try {
        if (std::filesystem::exists(dir_) && !std::filesystem::is_empty(dir_))
            throw std::runtime_error("target-directory-must-be-empty");
        std::filesystem::create_directories(dir_);
        std::ofstream marker(std::filesystem::path(dir_) / "bundle.incomplete",
                             std::ios::trunc);
        if (!marker) throw std::runtime_error("marker-open");
        std::ofstream metadata(std::filesystem::path(dir_) / "metadata.jsonl", std::ios::trunc);
        if (!metadata) throw std::runtime_error("metadata-open");
        marker << "BEARSCENE\nversion=2\nsampleRate=48000\nobjects=15\n";
        const auto provenancePath = std::filesystem::path(dir_) / "bundle-provenance.json";
        std::ofstream provenance(provenancePath, std::ios::trunc);
        if (!provenance) throw std::runtime_error("provenance-open");
        if (sourcePath_.empty()) {
            provenance << "{\"schema\":\"eac3-bear-bundle-provenance-v1\","
                       << "\"sourcePath\":null,\"sourceFileDigest\":null,"
                       << "\"sourceVerified\":false,\"note\":\"legacy caller did not provide source path\"}\n";
        } else {
            const auto absolute = std::filesystem::absolute(sourcePath_).lexically_normal();
            const auto digest = fileDigest(absolute);
            provenance << "{\"schema\":\"eac3-bear-bundle-provenance-v1\","
                       << "\"sourcePath\":" << jsonString(absolute.string())
                       << ",\"sourceFileDigest\":" << jsonString(digest)
                       << ",\"digestAlgorithm\":\"FNV-1a-64\",\"sourceVerified\":"
                       << ((digest != "unavailable") ? "true" : "false") << "}\n";
        }
        if (!provenance) throw std::runtime_error("provenance-write");
        opened_ = true;
        return true;
    } catch (const std::exception &e) {
        if (reason) *reason = std::string("bear-export-open:") + e.what();
        return false;
    }
}
bool Exporter::append(const eac3gate6c::Batch &batch, std::string *reason) {
    if (!opened_) { if (reason) *reason = "bear-export-not-open"; return false; }
    try {
        std::ostringstream name;
        name << "batch-" << std::setw(8) << std::setfill('0') << batches_;
        const auto base = std::filesystem::path(dir_) / name.str();
        const auto bin = base.string() + ".bin.tmp";
        std::ofstream out(bin, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("batch-open");
        const std::uint32_t objects = static_cast<std::uint32_t>(batch.objects.size());
        const std::uint32_t samples = batch.objects.empty() ? 0U : static_cast<std::uint32_t>(batch.objects.front().size());
        const std::uint32_t version = 2U;
        out.write("BSCN", 4); out.write(reinterpret_cast<const char*>(&version), 4);
        out.write(reinterpret_cast<const char*>(&objects), 4);
        out.write(reinterpret_cast<const char*>(&samples), 4);
        out.write(reinterpret_cast<const char*>(&batch.outputStart), sizeof(batch.outputStart));
        out.write(reinterpret_cast<const char*>(&batch.outputEnd), sizeof(batch.outputEnd));
        for (const auto &object : batch.objects) out.write(reinterpret_cast<const char*>(object.data()), object.size() * sizeof(float));
        const std::uint32_t lfe = static_cast<std::uint32_t>(batch.lfe.size());
        out.write(reinterpret_cast<const char*>(&lfe), 4);
        out.write(reinterpret_cast<const char*>(batch.lfe.data()), batch.lfe.size() * sizeof(float));
        if (!out) throw std::runtime_error("batch-write");
        out.close();
        std::filesystem::rename(bin, base.string() + ".bin");
        std::ofstream meta(std::filesystem::path(dir_) / "metadata.jsonl", std::ios::app);
        if (!meta) throw std::runtime_error("metadata-open");
        meta << "{\"metadataSchema\":\"eac3-oamd-renderer-neutral\",\"metadataSchemaVersion\":1"
             << ",\"batch\":" << batches_ << ",\"flush\":" << (batch.flush ? "true" : "false")
             << ",\"outputStart\":" << batch.outputStart << ",\"outputEnd\":" << batch.outputEnd
             << ",\"metadataCount\":" << batch.metadata.size() << ",\"objects\":" << objects
             << ",\"updates\":[";
        for (std::size_t index = 0; index < batch.metadata.size(); ++index) {
            const auto &u = batch.metadata[index];
            const auto &p = u.state.position;
            if (index) meta << ',';
            meta << "{\"sourcePosition\":" << u.sourcePosition
                 << ",\"blockIndex\":" << u.blockIndex
                 << ",\"rampDuration\":" << u.rampDuration
                 << ",\"objectIndex\":" << u.objectIndex
                 << ",\"active\":" << (u.state.active ? "true" : "false")
                 << ",\"gainMinusInfinity\":" << (u.state.gainMinusInfinity ? "true" : "false")
                 << ",\"gainDb\":" << u.state.gainDb
                 << ",\"positionValid\":" << (p.valid ? "true" : "false")
                 << ",\"screenAnchored\":" << (p.screenAnchored ? "true" : "false")
                 << ",\"x\":" << p.x << ",\"y\":" << p.y << ",\"z\":" << p.z
                 << ",\"codedX\":" << p.codedX << ",\"codedY\":" << p.codedY
                 << ",\"codedZ\":" << p.codedZ
                 << ",\"standardX\":" << p.standardX << ",\"standardY\":" << p.standardY
                 << ",\"standardZ\":" << p.standardZ
                 << ",\"basicValid\":" << (u.state.basicValid ? "true" : "false")
                 << ",\"renderValid\":" << (u.state.renderValid ? "true" : "false")
                 << ",\"lfeHelper\":" << (u.state.lfeHelper ? "true" : "false")
                 << ",\"priority\":" << u.state.priority
                 << ",\"extentPresence\":\"" << extentPresenceText(u.state) << "\""
                 << ",\"effectiveSizePresent\":"
                 << (u.state.effectiveSizePresent ? "true" : "false")
                 << ",\"extent\":{\"width\":" << u.state.size[0]
                 << ",\"height\":" << u.state.size[2]
                 << ",\"depth\":" << u.state.size[1] << '}'
                 << ",\"sourceSizeIndex\":" << u.state.effectiveSizeIndex
                 << ",\"zoneConstraints\":";
            writeBoolArray(meta, u.state.zoneConstraints);
            meta << ",\"elevation\":" << (u.state.elevation ? "true" : "false")
                 << ",\"snap\":" << (u.state.snap ? "true" : "false")
                 << ",\"distanceSpecified\":" << (p.distanceSpecified ? "true" : "false")
                 << ",\"distanceInfinite\":" << (p.distanceInfinite ? "true" : "false")
                 << ",\"distanceFactor\":" << p.distanceFactor
                 << ",\"screenFactor\":" << p.screenFactor
                 << ",\"depthFactor\":" << p.depthFactor
                 << ",\"extendedPrecisionPresent\":";
            writeBoolArray(meta, p.extendedPrecisionPresent);
            meta << ",\"extendedPrecision\":";
            writeIntArray(meta, p.extendedPrecision);
            meta << ",\"trim\":{\"present\":false,\"objectDisabled\":"
                 << (u.state.trimDisabled ? "true" : "false") << '}'
                 << ",\"divergence\":{\"present\":"
                 << (u.state.divergencePresent ? "true" : "false")
                 << ",\"reused\":" << (u.state.divergenceReused ? "true" : "false")
                 << ",\"mode\":" << u.state.divergenceMode
                 << ",\"index\":" << u.state.divergenceIndex
                 << ",\"value\":" << u.state.divergence << '}'
                 << ",\"unsupportedProperties\":{\"diffuse\":\"not-carried\",\"warp\":\"not-carried\",\"trimElement\":\"not-carried\"}"
                 << "}";
        }
        meta << "]}\n";
        ++batches_; metadata_ += batch.metadata.size();
        return true;
    } catch (const std::exception &e) {
        if (reason) *reason = std::string("bear-export-append:") + e.what();
        return false;
    }
}
bool Exporter::finish(std::string *reason) {
    if (!opened_) { if (reason) *reason = "bear-export-not-open"; return false; }
    try {
        std::filesystem::rename(std::filesystem::path(dir_) / "bundle.incomplete",
                                std::filesystem::path(dir_) / "bundle.complete");
        opened_ = false; return true;
    } catch (const std::exception &e) {
        if (reason) *reason = std::string("bear-export-finish:") + e.what();
        return false;
    }
}
}
