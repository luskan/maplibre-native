#include <mln/renderer/buckets/line_bucket.hpp>
#include <mln/renderer/bucket_parameters.hpp>
#include <mln/style/layers/line_layer_impl.hpp>
#include <mln/util/math.hpp>
#include <mln/util/constants.hpp>
#include <mln/gfx/polyline_generator.hpp>

#include <cassert>
#include <utility>
#include <cmath>
#include <rapidjson/document.h>
#include <mutex>

namespace mln {

using namespace style;

LineBucket::LineBucket(LineBucket::PossiblyEvaluatedLayoutProperties layout_,
                       const std::map<std::string, Immutable<LayerProperties>>& layerPaintProperties,
                       const float zoom_,
                       const uint32_t overscaling_,
                       const float paintZoomBias,
                       const bool useLineWidthZoomCoveringStops)
    : layout(std::move(layout_)),
      zoom(zoom_),
      overscaling(overscaling_) {
    for (const auto& pair : layerPaintProperties) {
        paintPropertyBinders.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(pair.first),
            std::forward_as_tuple(
                getEvaluated<LineLayerProperties>(pair.second), zoom + paintZoomBias, useLineWidthZoomCoveringStops));
    }
}

LineBucket::~LineBucket() {
    sharedVertices->release();
}

void LineBucket::addFeature(const GeometryTileFeature& feature,
                            const GeometryCollection& geometryCollection,
                            const ImagePositions& patternPositions,
                            const PatternLayerMap& patternDependencies,
                            std::size_t index,
                            const CanonicalTileID& canonical) {
    if (layout.get<LineRoadComposite>()) {
        if (auto value = feature.getValue("road_composite_guards")) {
            auto const* encoded = value->getString();
            rapidjson::Document table;
            if (encoded && encoded->size() <= 131072) table.Parse(encoded->c_str());
            if (encoded && !table.HasParseError() && table.IsArray() && table.Size() >= 4
                && table.Size() <= 2052 && table.Size() % 2 == 0) {
                if (roadGuardTableSeen && roadGuardKey != *encoded) roadGuardTableInvalid = true;
                std::vector<Point<double>> points;
                auto project = [&](double lon, double lat) {
                    double const scale = std::ldexp(8192.0, canonical.z);
                    double const y = (0.5 - std::log(std::tan(0.7853981633974483
                        + lat * 0.008726646259971648)) / 6.283185307179586) * scale;
                    return Point<double>{(lon / 360.0 + 0.5) * scale - canonical.x * 8192.0,
                        y - canonical.y * 8192.0};
                };
                for (rapidjson::SizeType i = 0; i < table.Size(); i += 2) {
                    if (!table[i].IsNumber() || !table[i + 1].IsNumber()) {
                        roadGuardTableInvalid = true;
                        break;
                    }
                    double const lon = table[i].GetDouble(), lat = table[i + 1].GetDouble();
                    if (!std::isfinite(lon) || !std::isfinite(lat) || std::abs(lat) > 85.0 || std::abs(lon) > 180.0) {
                        roadGuardTableInvalid = true;
                        break;
                    }
                    points.push_back(project(lon, lat));
                }
                if (!roadGuardTableInvalid) {
                    std::array<float, 4> rect = {static_cast<float>(points[0].x), static_cast<float>(points[1].y),
                        static_cast<float>(points[1].x - points[0].x),
                        static_cast<float>(points[0].y - points[1].y)};
                    if (rect[2] <= 0 || rect[3] <= 0 || rect[2] > 32768 || rect[3] > 32768
                        || (roadGuardTableSeen && rect != roadGuardRect)) {
                        roadGuardTableInvalid = true;
                    } else if (!roadGuardTableSeen) {
                        roadGuardRect = rect;
                        roadGuardKey = *encoded;
                        roadGuardTile = canonical;
                        roadGuards.assign(points.begin() + 2, points.end());
                        roadGuardTableSeen = true;
                    }
                }
            } else {
                roadGuardTableInvalid = true;
            }
        }
    }
    for (auto& line : geometryCollection) {
        addGeometry(line, feature, canonical);
    }

    for (auto& pair : paintPropertyBinders) {
        const auto it = patternDependencies.find(pair.first);
        if (it != patternDependencies.end()) {
            pair.second.populateVertexVectors(
                feature, vertices.elements(), index, patternPositions, it->second, canonical);
        } else {
            pair.second.populateVertexVectors(feature, vertices.elements(), index, patternPositions, {}, canonical);
        }
    }
}

bool LineBucket::prepareRoadGuardImage(gfx::Context& context) {
    using Key = std::tuple<gfx::Context*, CanonicalTileID, std::string>;
    static std::mutex mutex;
    static std::map<Key, std::weak_ptr<RoadGuardField>> fields;
    std::lock_guard lock(mutex);
    if (!roadGuardTableSeen || roadGuardTableInvalid) return false;
    if (roadGuardField) return true;
    for (auto it = fields.begin(); it != fields.end();) {
        if (it->second.expired()) it = fields.erase(it);
        else ++it;
    }
    Key const key{&context, roadGuardTile, roadGuardKey};
    if (auto shared = fields[key].lock()) {
        roadGuardField = std::move(shared);
        return true;
    }
    roadGuardField = std::make_shared<RoadGuardField>();
    fields[key] = roadGuardField;
    constexpr int size = 256;
    roadGuardField->image = std::make_shared<PremultipliedImage>(Size{size, size});
    double const dx = roadGuardRect[2] / size;
    double const dy = roadGuardRect[3] / size;
    double const margin = std::hypot(dx, dy) * 0.5;
    std::vector<double> distances(size * size, maxRoadGuardDistance);
    for (auto const& point : roadGuards) {
        int const minX = std::max(0, static_cast<int>(std::floor(
            (point.x - maxRoadGuardDistance - margin - roadGuardRect[0]) / dx)));
        int const maxX = std::min(size - 1, static_cast<int>(std::ceil(
            (point.x + maxRoadGuardDistance + margin - roadGuardRect[0]) / dx)));
        int const minY = std::max(0, static_cast<int>(std::floor(
            (point.y - maxRoadGuardDistance - margin - roadGuardRect[1]) / dy)));
        int const maxY = std::min(size - 1, static_cast<int>(std::ceil(
            (point.y + maxRoadGuardDistance + margin - roadGuardRect[1]) / dy)));
        for (int y = minY; y <= maxY; ++y) {
            for (int x = minX; x <= maxX; ++x) {
                double const distance = std::max(0.0, std::hypot(
                    roadGuardRect[0] + (x + 0.5) * dx - point.x,
                    roadGuardRect[1] + (y + 0.5) * dy - point.y) - margin);
                auto& current = distances[y * size + x];
                current = std::min(current, distance);
            }
        }
    }
    for (std::size_t i = 0; i < distances.size(); ++i) {
        auto const distance = static_cast<uint16_t>(std::floor(distances[i]));
        roadGuardField->image->data[i * 4] = distance >> 8;
        roadGuardField->image->data[i * 4 + 1] = distance & 255;
        roadGuardField->image->data[i * 4 + 2] = 0;
        roadGuardField->image->data[i * 4 + 3] = 255;
    }
    return true;
}

void LineBucket::addGeometry(const GeometryCoordinates& coordinates,
                             const GeometryTileFeature& feature,
                             const CanonicalTileID& canonical) {
    // Ignore empty coordinates.
    if (coordinates.empty()) {
        return;
    }
    gfx::PolylineGenerator<LineLayoutVertex, SegmentBase> generator(
        vertices,
        LineBucket::layoutVertex,
        segments,
        [](std::size_t vertexOffset, std::size_t indexOffset) -> SegmentBase {
            return SegmentBase(vertexOffset, indexOffset);
        },
        [](auto& seg) -> SegmentBase& { return seg; },
        triangles);

    gfx::PolylineGeneratorOptions options;

    options.type = feature.getType();
    const std::size_t len = [&coordinates] {
        std::size_t l = coordinates.size();
        // If the line has duplicate vertices at the end, adjust length to remove them.
        while (l >= 2 && coordinates[l - 1] == coordinates[l - 2]) {
            l--;
        }
        return l;
    }();

    const std::size_t first = [&coordinates, &len] {
        std::size_t i = 0;
        // If the line has duplicate vertices at the start, adjust index to remove them.
        while (i + 1 < len && coordinates[i] == coordinates[i + 1]) {
            i++;
        }
        return i;
    }();

    // Ignore invalid geometry.
    const std::size_t minLen = (options.type == FeatureType::Polygon ? 3 : 2);
    if (len < minLen) {
        // Warn once, but only if the source geometry is invalid, not if de-duplication made it invalid.
        // This happens, e.g., when attempting to use a GeoJSON `MultiPoint`
        // or `MLNPointCollectionFeature` as the source for a line layer.
        // Unfortunately, we cannot show the layer or source name from here.
        if (coordinates.size() < minLen) {
            static bool warned = false; // not thread-safe, there's a small chance of warning more than once
            if (!warned) {
                warned = true;
                Log::Warning(Event::General, "Invalid geometry in line layer");
            }
        }
        return;
    }

    const auto clip_start = feature.getValue("mapbox_clip_start");
    const auto clip_end = feature.getValue("mapbox_clip_end");
    if (clip_start && clip_end) {
        double total_length = 0.0;
        for (std::size_t i = first; i < len - 1; ++i) {
            total_length += util::dist<double>(coordinates[i], coordinates[i + 1]);
        }

        options.clipDistances = gfx::PolylineGeneratorDistances{
            *numericValue<double>(*clip_start), *numericValue<double>(*clip_end), total_length};
    }

    options.joinType = layout.evaluate<LineJoin>(zoom, feature, canonical);
    options.miterLimit = options.joinType == LineJoinType::Bevel ? 1.05f
                                                                 : static_cast<float>(layout.get<LineMiterLimit>());
    options.beginCap = layout.get<LineCap>();
    options.endCap = options.type == FeatureType::Polygon ? LineCapType::Butt : LineCapType(layout.get<LineCap>());
    options.roundLimit = layout.get<LineRoundLimit>();
    options.overscaling = overscaling;

    generator.generate(coordinates, options);
}

void LineBucket::upload([[maybe_unused]] gfx::UploadPass& uploadPass) {
    uploaded = true;
}

bool LineBucket::hasData() const {
    return !segments.empty();
}

namespace {
template <class Property>
float get(const LinePaintProperties::PossiblyEvaluated& evaluated,
          const std::string& id,
          const std::map<std::string, LineBinders>& paintPropertyBinders) {
    auto it = paintPropertyBinders.find(id);
    if (it == paintPropertyBinders.end() || !it->second.statistics<Property>().max()) {
        return evaluated.get<Property>().constantOr(Property::defaultValue());
    } else {
        return *it->second.statistics<Property>().max();
    }
}
} // namespace

float LineBucket::getQueryRadius(const RenderLayer& layer) const {
    const auto& evaluated = getEvaluated<LineLayerProperties>(layer.evaluatedProperties);
    const std::array<float, 2>& translate = evaluated.get<LineTranslate>();
    float offset = get<LineOffset>(evaluated, layer.getID(), paintPropertyBinders);
    float lineWidth = get<LineWidth>(evaluated, layer.getID(), paintPropertyBinders);
    float gapWidth = get<LineGapWidth>(evaluated, layer.getID(), paintPropertyBinders);
    if (gapWidth) {
        lineWidth = gapWidth + 2 * lineWidth;
    }

    return lineWidth / 2.0f + std::abs(offset) + util::length(translate[0], translate[1]);
}

void LineBucket::update(const FeatureStates& states,
                        const GeometryTileLayer& layer,
                        const std::string& layerID,
                        const ImagePositions& imagePositions) {
    auto it = paintPropertyBinders.find(layerID);
    if (it != paintPropertyBinders.end()) {
        it->second.updateVertexVectors(states, layer, imagePositions);
        uploaded = false;

        sharedVertices->updateModified();
    }
}

} // namespace mln
