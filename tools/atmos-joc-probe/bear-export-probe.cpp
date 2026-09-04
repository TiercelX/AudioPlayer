#include "bear-export.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool require(bool condition, const char *message)
{
    if (!condition)
        std::cerr << "FAIL " << message << '\n';
    return condition;
}

bool selfTest()
{
    const auto directory =
        std::filesystem::temp_directory_path() / "audioplayer-bear-export-schema-test";
    const auto cleanup = [&directory]() {
        std::filesystem::remove(directory / "bundle.incomplete");
        std::filesystem::remove(directory / "bundle.complete");
        std::filesystem::remove(directory / "metadata.jsonl");
        std::filesystem::remove(directory / "bundle-provenance.json");
        std::filesystem::remove(directory / "batch-00000000.bin");
        std::filesystem::remove(directory / "batch-00000000.bin.tmp");
        std::filesystem::remove(directory);
    };
    cleanup();

    eac3bear::Exporter exporter(directory.string());
    std::string reason;
    if (!require(exporter.open(&reason), reason.c_str()))
        return false;

    eac3gate6c::Batch batch;
    batch.outputStart = 0;
    batch.outputEnd = 2;
    batch.objects.assign(15, std::vector<float> {0.25F, -0.5F});
    batch.lfe = {0.1F, -0.2F, 0.3F};

    eac3gate6c::MetadataUpdate update;
    update.sourcePosition = 0;
    update.blockIndex = 2;
    update.rampDuration = 1536;
    update.objectIndex = 7;
    auto &state = update.state;
    state.objectIndex = 7;
    state.active = true;
    state.gainMinusInfinity = false;
    state.gainDb = -3.0F;
    state.priority = 0.75F;
    state.position.valid = true;
    state.position.screenAnchored = false;
    state.position.x = 0.2F;
    state.position.y = 0.3F;
    state.position.z = -0.1F;
    state.position.standardX = 0.2F;
    state.position.standardY = 0.3F;
    state.position.standardZ = -0.1F;
    state.position.distanceSpecified = true;
    state.position.distanceFactor = 0.5F;
    state.position.screenFactor = 0.25F;
    state.position.depthFactor = 0.125F;
    state.position.extendedPrecisionPresent = {true, false, true};
    state.position.extendedPrecision = {11, 0, -7};
    state.effectiveSizePresent = true;
    state.effectiveSizeIndex = 4;
    state.size = {0.21F, 0.32F, 0.43F};
    state.zoneConstraints[2] = false;
    state.elevation = false;
    state.snap = true;
    state.trimDisabled = true;
    state.divergencePresent = true;
    state.divergenceReused = true;
    state.divergenceMode = 1;
    state.divergenceIndex = 3;
    state.divergence = 0.4F;
    batch.metadata.push_back(update);

    if (!require(exporter.append(batch, &reason), reason.c_str())
        || !require(exporter.finish(&reason), reason.c_str())) {
        cleanup();
        return false;
    }

    std::ifstream metadata(directory / "metadata.jsonl");
    const std::string line((std::istreambuf_iterator<char>(metadata)),
                           std::istreambuf_iterator<char>());
    bool pass = true;
    pass &= require(line.find("\"metadataSchema\":\"eac3-oamd-renderer-neutral\"") != std::string::npos,
                    "metadata schema");
    pass &= require(line.find("\"metadataSchemaVersion\":1") != std::string::npos,
                    "metadata schema version");
    pass &= require(line.find("\"priority\":0.75") != std::string::npos,
                    "priority");
    pass &= require(line.find("\"extentPresence\":\"non-zero\"") != std::string::npos,
                    "non-zero extent presence");
    pass &= require(line.find("\"extent\":{\"width\":0.21,\"height\":0.43,\"depth\":0.32}") != std::string::npos,
                    "extent semantic axis order");
    pass &= require(line.find("\"sourceSizeIndex\":4") != std::string::npos,
                    "source size index");
    pass &= require(line.find("\"zoneConstraints\":[true,true,false,true,true,true]") != std::string::npos,
                    "zone constraints");
    pass &= require(line.find("\"extendedPrecisionPresent\":[true,false,true]") != std::string::npos,
                    "extended precision presence");
    pass &= require(line.find("\"divergence\":{\"present\":true,\"reused\":true,\"mode\":1,\"index\":3,\"value\":0.4}") != std::string::npos,
                    "divergence");
    pass &= require(line.find("\"unsupportedProperties\":{\"diffuse\":\"not-carried\",\"warp\":\"not-carried\",\"trimElement\":\"not-carried\"}") != std::string::npos,
                    "unsupported provenance");

    std::ifstream provenance(directory / "bundle-provenance.json");
    const std::string provenanceText((std::istreambuf_iterator<char>(provenance)),
                                     std::istreambuf_iterator<char>());
    pass &= require(provenanceText.find("eac3-bear-bundle-provenance-v1") != std::string::npos,
                    "bundle provenance");
    pass &= require(provenanceText.find("sourceVerified\":false") != std::string::npos,
                    "legacy provenance explicitly unverified");
    provenance.close();

    std::ifstream binary(directory / "batch-00000000.bin", std::ios::binary);
    char magic[4] {};
    std::uint32_t version = 0, objects = 0, samples = 0, lfe = 0;
    std::int64_t start = 0, end = 0;
    binary.read(magic, sizeof(magic));
    binary.read(reinterpret_cast<char *>(&version), sizeof(version));
    binary.read(reinterpret_cast<char *>(&objects), sizeof(objects));
    binary.read(reinterpret_cast<char *>(&samples), sizeof(samples));
    binary.read(reinterpret_cast<char *>(&start), sizeof(start));
    binary.read(reinterpret_cast<char *>(&end), sizeof(end));
    binary.seekg(static_cast<std::streamoff>(15 * 2 * sizeof(float)), std::ios::cur);
    binary.read(reinterpret_cast<char *>(&lfe), sizeof(lfe));
    pass &= require(std::string(magic, sizeof(magic)) == "BSCN", "binary magic");
    pass &= require(version == 2 && objects == 15 && samples == 2, "binary v2 header");
    pass &= require(start == 0 && end == 2 && lfe == 3, "binary timeline/LFE");
    pass &= require(std::filesystem::file_size(directory / "batch-00000000.bin") == 168,
                    "binary v2 size unchanged");

    metadata.close();
    binary.close();
    cleanup();
    const auto verifiedRoot =
        std::filesystem::temp_directory_path() / "audioplayer-bear-export-provenance-test";
    const auto verifiedBundle = verifiedRoot / "bundle";
    const auto verifiedSource = verifiedRoot / "source.ec3";
    std::filesystem::remove_all(verifiedRoot);
    std::filesystem::create_directories(verifiedBundle);
    {
        std::ofstream source(verifiedSource, std::ios::binary | std::ios::trunc);
        source << "source-fixture";
    }
    eac3bear::Exporter verifiedExporter(verifiedBundle.string(), verifiedSource.string());
    pass &= require(verifiedExporter.open(&reason), "verified provenance open");
    pass &= require(verifiedExporter.finish(&reason), "verified provenance finish");
    std::ifstream verifiedProvenance(verifiedBundle / "bundle-provenance.json");
    const std::string verifiedText((std::istreambuf_iterator<char>(verifiedProvenance)),
                                   std::istreambuf_iterator<char>());
    verifiedProvenance.close();
    pass &= require(verifiedText.find("sourceVerified\":true") != std::string::npos,
                    "verified provenance flag");
    pass &= require(verifiedText.find("fnv1a64-16bc74255fe78e18") != std::string::npos,
                    "standard FNV-1a-64 fixture digest");
    std::filesystem::remove_all(verifiedRoot);
    if (pass)
        std::cout << "PASS bear-export-schema-self-test cases=2 binaryVersion=2\n";
    return pass;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 2 || std::string(argv[1]) != "--self-test") {
        std::cerr << "Usage: Eac3BearExportProbe --self-test\n";
        return 2;
    }
    return selfTest() ? 0 : 1;
}
