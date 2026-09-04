#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace eac3render {

constexpr std::uint64_t kInitialGeneration = 1U;

enum class ElementKind {
    Bed,
    Object,
};

// These are the only bed labels accepted by the first scene contract.  The
// label, rather than an interleaved channel index, is the identity used by a
// future speaker-layout adapter.
enum class BedLabel {
    FL,
    FR,
    FC,
    SL,
    SR,
    TFL,
    TFR,
    LFE,
};

const char *bedLabelName(BedLabel label);

struct ListenerPose {
    std::array<float, 3> position {0.0F, 0.0F, 0.0F};
    // Quaternion is stored as w, x, y, z.  Only identity is accepted by this
    // slice; head tracking is deliberately outside Gate 8B-1.
    std::array<float, 4> orientation {1.0F, 0.0F, 0.0F, 0.0F};

    static ListenerPose identity();
    bool isIdentity() const;
};

struct RenderElement {
    ElementKind kind = ElementKind::Object;
    std::uint64_t generation = kInitialGeneration;
    std::uint64_t objectId = 0U;
    BedLabel bedLabel = BedLabel::FL;
    std::uint32_t sourceChannel = 0U;
    std::int64_t timestampSamples = 0;
    std::int64_t durationSamples = 0;

    static RenderElement bed(BedLabel label, std::uint32_t sourceChannel,
                             std::int64_t timestampSamples,
                             std::int64_t durationSamples,
                             std::uint64_t generation = kInitialGeneration);
    static RenderElement object(std::uint64_t objectId,
                                std::uint32_t sourceChannel,
                                std::int64_t timestampSamples,
                                std::int64_t durationSamples,
                                std::uint64_t generation = kInitialGeneration);
};

struct MetadataPoint {
    std::uint64_t generation = kInitialGeneration;
    std::uint64_t objectId = 0U;
    std::int64_t timestampSamples = 0;
    float gain = 1.0F;
    std::array<float, 3> position {0.0F, 0.0F, 0.0F};
};

class MetadataTimeline {
public:
    explicit MetadataTimeline(std::uint64_t generation = kInitialGeneration);

    bool append(const MetadataPoint &point, std::string *reason = nullptr);
    bool reset(std::uint64_t generation, std::string *reason = nullptr);

    std::uint64_t generation() const { return generation_; }
    std::size_t size() const { return points_.size(); }
    const std::vector<MetadataPoint> &points() const { return points_; }
    std::string snapshot() const;

private:
    std::uint64_t generation_ = kInitialGeneration;
    std::vector<MetadataPoint> points_;
};

class RenderScene {
public:
    explicit RenderScene(std::uint64_t generation = kInitialGeneration);

    bool addBed(const RenderElement &element, std::string *reason = nullptr);
    bool addObject(const RenderElement &element, std::string *reason = nullptr);
    bool appendMetadata(const MetadataPoint &point, std::string *reason = nullptr);
    bool reset(std::uint64_t generation, std::string *reason = nullptr);

    std::uint64_t generation() const { return generation_; }
    const ListenerPose &listenerPose() const { return listenerPose_; }
    const std::vector<RenderElement> &elements() const { return elements_; }
    const MetadataTimeline &metadata() const { return metadata_; }
    std::string snapshot() const;

private:
    bool addElement(const RenderElement &element, ElementKind expectedKind,
                    std::string *reason);

    std::uint64_t generation_ = kInitialGeneration;
    ListenerPose listenerPose_ = ListenerPose::identity();
    std::vector<RenderElement> elements_;
    MetadataTimeline metadata_;
};

} // namespace eac3render
