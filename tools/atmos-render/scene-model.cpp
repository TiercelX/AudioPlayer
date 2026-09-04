#include "scene-model.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

namespace eac3render {
namespace {

void setReason(std::string *reason, const char *value)
{
    if (reason) {
        *reason = value;
    }
}

bool finiteArray(const std::array<float, 3> &values)
{
    return std::all_of(values.begin(), values.end(), [](float value) {
        return std::isfinite(value);
    });
}

bool validTimeRange(std::int64_t start, std::int64_t duration)
{
    if (start < 0 || duration <= 0) {
        return false;
    }

    return duration <= std::numeric_limits<std::int64_t>::max() - start;
}

bool sameTimestamp(const MetadataPoint &left, const MetadataPoint &right)
{
    return left.objectId == right.objectId
        && left.timestampSamples == right.timestampSamples;
}

} // namespace

const char *bedLabelName(BedLabel label)
{
    switch (label) {
    case BedLabel::FL: return "FL";
    case BedLabel::FR: return "FR";
    case BedLabel::FC: return "FC";
    case BedLabel::SL: return "SL";
    case BedLabel::SR: return "SR";
    case BedLabel::TFL: return "TFL";
    case BedLabel::TFR: return "TFR";
    case BedLabel::LFE: return "LFE";
    }
    return "unknown";
}

ListenerPose ListenerPose::identity()
{
    return {};
}

bool ListenerPose::isIdentity() const
{
    return position == std::array<float, 3> {0.0F, 0.0F, 0.0F}
        && orientation == std::array<float, 4> {1.0F, 0.0F, 0.0F, 0.0F};
}

RenderElement RenderElement::bed(BedLabel label, std::uint32_t sourceChannel,
                                 std::int64_t timestampSamples,
                                 std::int64_t durationSamples,
                                 std::uint64_t generation)
{
    RenderElement element;
    element.kind = ElementKind::Bed;
    element.generation = generation;
    element.bedLabel = label;
    element.sourceChannel = sourceChannel;
    element.timestampSamples = timestampSamples;
    element.durationSamples = durationSamples;
    return element;
}

RenderElement RenderElement::object(std::uint64_t objectId,
                                    std::uint32_t sourceChannel,
                                    std::int64_t timestampSamples,
                                    std::int64_t durationSamples,
                                    std::uint64_t generation)
{
    RenderElement element;
    element.kind = ElementKind::Object;
    element.generation = generation;
    element.objectId = objectId;
    element.sourceChannel = sourceChannel;
    element.timestampSamples = timestampSamples;
    element.durationSamples = durationSamples;
    return element;
}

MetadataTimeline::MetadataTimeline(std::uint64_t generation)
    : generation_(generation)
{
}

bool MetadataTimeline::append(const MetadataPoint &point, std::string *reason)
{
    if (generation_ == 0U) {
        setReason(reason, "metadata-invalid-generation");
        return false;
    }
    if (point.generation == 0U || point.generation != generation_) {
        setReason(reason, "metadata-generation-mismatch");
        return false;
    }
    if (point.objectId == 0U) {
        setReason(reason, "metadata-object-id-zero");
        return false;
    }
    if (point.timestampSamples < 0) {
        setReason(reason, "metadata-negative-timestamp");
        return false;
    }
    if (!std::isfinite(point.gain) || !finiteArray(point.position)) {
        setReason(reason, "metadata-non-finite-value");
        return false;
    }
    if (std::any_of(points_.begin(), points_.end(),
                    [&point](const MetadataPoint &existing) {
                        return sameTimestamp(existing, point);
                    })) {
        setReason(reason, "metadata-duplicate-object-timestamp");
        return false;
    }

    points_.push_back(point);
    return true;
}

bool MetadataTimeline::reset(std::uint64_t generation, std::string *reason)
{
    if (generation == 0U || generation <= generation_) {
        setReason(reason, "metadata-generation-not-advanced");
        return false;
    }

    generation_ = generation;
    points_.clear();
    return true;
}

std::string MetadataTimeline::snapshot() const
{
    std::vector<MetadataPoint> ordered = points_;
    std::sort(ordered.begin(), ordered.end(), [](const MetadataPoint &left,
                                                 const MetadataPoint &right) {
        if (left.objectId != right.objectId) {
            return left.objectId < right.objectId;
        }
        return left.timestampSamples < right.timestampSamples;
    });

    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::fixed << std::setprecision(9)
           << "generation=" << generation_ << ";points=" << ordered.size();
    for (const MetadataPoint &point : ordered) {
        output << "|object=" << point.objectId
               << ",t=" << point.timestampSamples
               << ",gain=" << point.gain
               << ",position=" << point.position[0] << ','
               << point.position[1] << ',' << point.position[2];
    }
    return output.str();
}

RenderScene::RenderScene(std::uint64_t generation)
    : generation_(generation)
    , metadata_(generation)
{
}

bool RenderScene::addBed(const RenderElement &element, std::string *reason)
{
    return addElement(element, ElementKind::Bed, reason);
}

bool RenderScene::addObject(const RenderElement &element, std::string *reason)
{
    return addElement(element, ElementKind::Object, reason);
}

bool RenderScene::addElement(const RenderElement &element,
                             ElementKind expectedKind,
                             std::string *reason)
{
    if (element.kind != expectedKind) {
        setReason(reason, "element-kind-mismatch");
        return false;
    }
    if (generation_ == 0U) {
        setReason(reason, "scene-invalid-generation");
        return false;
    }
    if (element.generation != generation_) {
        setReason(reason, "element-generation-mismatch");
        return false;
    }
    if (!validTimeRange(element.timestampSamples, element.durationSamples)) {
        setReason(reason, "element-invalid-time-range");
        return false;
    }
    if (expectedKind == ElementKind::Object && element.objectId == 0U) {
        setReason(reason, "object-id-zero");
        return false;
    }
    if (expectedKind == ElementKind::Bed && element.objectId != 0U) {
        setReason(reason, "bed-object-id-nonzero");
        return false;
    }

    const bool duplicate = std::any_of(
        elements_.begin(), elements_.end(), [&element, expectedKind](const RenderElement &existing) {
            if (existing.kind != expectedKind) {
                return false;
            }
            if (expectedKind == ElementKind::Object) {
                return existing.objectId == element.objectId;
            }
            return existing.bedLabel == element.bedLabel;
        });
    if (duplicate) {
        setReason(reason, expectedKind == ElementKind::Object
                              ? "duplicate-object-id"
                              : "duplicate-bed-label");
        return false;
    }

    elements_.push_back(element);
    return true;
}

bool RenderScene::appendMetadata(const MetadataPoint &point, std::string *reason)
{
    if (generation_ == 0U) {
        setReason(reason, "scene-invalid-generation");
        return false;
    }

    const auto object = std::find_if(
        elements_.begin(), elements_.end(), [&point](const RenderElement &element) {
            return element.kind == ElementKind::Object
                && element.objectId == point.objectId;
        });
    if (object == elements_.end()) {
        setReason(reason, "metadata-object-not-in-scene");
        return false;
    }

    const std::int64_t objectEnd =
        object->timestampSamples + object->durationSamples;
    if (point.timestampSamples < object->timestampSamples
        || point.timestampSamples >= objectEnd) {
        setReason(reason, "metadata-timestamp-out-of-range");
        return false;
    }
    return metadata_.append(point, reason);
}

bool RenderScene::reset(std::uint64_t generation, std::string *reason)
{
    if (generation == 0U || generation <= generation_) {
        setReason(reason, "scene-generation-not-advanced");
        return false;
    }

    generation_ = generation;
    listenerPose_ = ListenerPose::identity();
    elements_.clear();
    return metadata_.reset(generation, reason);
}

std::string RenderScene::snapshot() const
{
    std::vector<RenderElement> ordered = elements_;
    std::sort(ordered.begin(), ordered.end(), [](const RenderElement &left,
                                                 const RenderElement &right) {
        if (left.kind != right.kind) {
            return left.kind < right.kind;
        }
        if (left.kind == ElementKind::Object && left.objectId != right.objectId) {
            return left.objectId < right.objectId;
        }
        if (left.kind == ElementKind::Bed && left.bedLabel != right.bedLabel) {
            return left.bedLabel < right.bedLabel;
        }
        return left.sourceChannel < right.sourceChannel;
    });

    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::fixed << std::setprecision(9)
           << "generation=" << generation_
           << ";listener=identity"
           << ";elements=" << ordered.size();
    for (const RenderElement &element : ordered) {
        output << '|';
        if (element.kind == ElementKind::Bed) {
            output << "bed=" << bedLabelName(element.bedLabel);
        } else {
            output << "object=" << element.objectId;
        }
        output << ",channel=" << element.sourceChannel
               << ",t=" << element.timestampSamples
               << ",duration=" << element.durationSamples;
    }
    output << ";metadata=" << metadata_.snapshot();
    return output.str();
}

} // namespace eac3render
