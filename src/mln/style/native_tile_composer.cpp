#include <mln/style/custom_tile_conversion.hpp>
#include <mln/style/native_tile_composer.hpp>

#include <mapbox/geojsonvt/convert.hpp>

#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>

namespace mln
{
namespace
{

namespace vt = mapbox::geojsonvt::detail;

// These output predicates follow geojson-vt tile.hpp for conversion revision 1.
// Projection and simplification use the vendored implementation directly.
class FinalGeometry
{
public:
  FinalGeometry(NativeTileBuilder& builder, const NativeTileMetadata& metadata, GeoJSONFeature& feature,
                double tolerance, bool inspectRoadMetadata)
    : builder_(builder), metadata_(metadata), feature_(feature),
      scale_(std::ldexp(1.0, metadata.tileID.z)),
      inspectRoadMetadata_(inspectRoadMetadata), tolerance_(tolerance),
      squaredTolerance_(tolerance_ * tolerance_)
  {
  }

  void operator()(const vt::vt_empty&)
  {
    emit(FeatureType::Unknown, {});
  }

  void operator()(const vt::vt_point& point)
  {
    GeometryCollection geometry;
    geometry.emplace_back();
    geometry.back().push_back(quantize(point));
    emit(FeatureType::Point, std::move(geometry));
  }

  void operator()(const vt::vt_multi_point& points)
  {
    if (points.empty()) return;
    GeometryCoordinates coordinates;
    coordinates.reserve(points.size());
    for (const auto& point : points) coordinates.push_back(quantize(point));
    GeometryCollection geometry;
    geometry.push_back(std::move(coordinates));
    emit(FeatureType::Point, std::move(geometry));
  }

  void operator()(const vt::vt_line_string& line)
  {
    if (!(line.dist > tolerance_)) return;
    const auto profile = takeRoadProfile(line.size());
    auto coordinates = profile ? roadProfile(line, *profile) : simplify(line);
    if (coordinates.empty()) return;
    GeometryCollection geometry;
    geometry.push_back(std::move(coordinates));
    emit(FeatureType::LineString, std::move(geometry));
  }

  void operator()(const vt::vt_multi_line_string& lines)
  {
    GeometryCollection geometry;
    geometry.reserve(lines.size());
    for (const auto& line : lines)
    {
      if (line.dist > tolerance_) geometry.push_back(simplify(line));
    }
    if (!geometry.empty()) emit(FeatureType::LineString, std::move(geometry));
  }

  void operator()(const vt::vt_polygon& polygon)
  {
    auto geometry = rings(polygon);
    if (!geometry.empty()) emit(FeatureType::Polygon, fixupPolygons(geometry));
  }

  void operator()(const vt::vt_multi_polygon& polygons)
  {
    GeometryCollection geometry;
    for (const auto& polygon : polygons)
    {
      for (const auto& ring : polygon)
      {
        if (ring.area > squaredTolerance_) geometry.push_back(simplify(ring));
      }
    }
    if (!geometry.empty()) emit(FeatureType::Polygon, fixupPolygons(geometry));
  }

  void operator()(const vt::vt_geometry_collection& collection)
  {
    for (const auto& geometry : collection)
    {
      vt::vt_geometry::visit(geometry, [this](const auto& value) { (*this)(value); });
    }
  }

  void finish()
  {
    if (pending_)
    {
      builder_.appendFinal(pending_->type, std::move(pending_->geometry),
                           std::move(feature_.properties), std::move(feature_.id));
    }
  }

private:
  int16_t coordinate(double value) const
  {
    const auto rounded = std::round(value);
    if (!std::isfinite(rounded) || rounded < std::numeric_limits<int16_t>::min() ||
        rounded > std::numeric_limits<int16_t>::max())
    {
      throw std::out_of_range("Native tile coordinate is outside int16 range");
    }
    return static_cast<int16_t>(rounded);
  }

  GeometryCoordinate quantize(const vt::vt_point& point) const
  {
    return {coordinate((point.x * scale_ - metadata_.tileID.x) * metadata_.conversion.extent),
            coordinate((point.y * scale_ - metadata_.tileID.y) * metadata_.conversion.extent)};
  }

  template<class Line>
  GeometryCoordinates simplify(const Line& line) const
  {
    GeometryCoordinates coordinates;
    coordinates.reserve(line.size());
    for (const auto& point : line)
    {
      if (point.z > squaredTolerance_) coordinates.push_back(quantize(point));
    }
    return coordinates;
  }

  struct RoadProfileVertices {
    std::size_t first;
    std::size_t last;
    bool reversed;
  };

  std::optional<RoadProfileVertices> takeRoadProfile(std::size_t points)
  {
    if (!inspectRoadMetadata_ || !feature_.properties.contains("xr")) return std::nullopt;
    static const std::string key = "__automapa_mvt_crossing_profile_vertices";
    const auto vertexRange = feature_.properties.find(key);
    if (vertexRange == feature_.properties.end()) return std::nullopt;
    if (!vertexRange->second.is<int64_t>()) return std::nullopt;
    const auto range = vertexRange->second.get<int64_t>();
    const bool reversed = range < 0;
    const uint64_t packed = reversed ? static_cast<uint64_t>(-(range + 1)) : static_cast<uint64_t>(range);
    const auto first = packed >> 32;
    const auto last = packed & 0xffffffffu;
    if (first >= last || last >= points) return std::nullopt;
    feature_.properties.erase(vertexRange);
    return RoadProfileVertices{static_cast<std::size_t>(first), static_cast<std::size_t>(last), reversed};
  }

  GeometryCoordinates roadProfile(const vt::vt_line_string& line, const RoadProfileVertices& range)
  {
    // All copies of the same profile retain the junction vertices and the same path.
    GeometryCoordinates coordinates;
    coordinates.reserve(line.size());
    double distance = 0, start = 0, end = 0;
    for (std::size_t i = 0; i < line.size(); ++i) {
      const auto point = quantize(line[i]);
      if (!coordinates.empty()) {
        const auto& previous = coordinates.back();
        distance += std::hypot(double(point.x) - previous.x, double(point.y) - previous.y);
      }
      coordinates.push_back(point);
      if (i == range.first) start = std::floor(distance * 0.5);
      if (i == range.last) end = std::floor(distance * 0.5);
    }
    const auto length = std::floor(distance * 0.5);
    if (length > 0) {
      const int64_t bounds = std::lround(start / length * 2047) * 2048 + std::lround(end / length * 2047);
      feature_.properties["xt"] = range.reversed ? -1 - bounds : bounds;
    }
    return coordinates;
  }

  GeometryCollection rings(const vt::vt_polygon& polygon) const
  {
    GeometryCollection geometry;
    geometry.reserve(polygon.size());
    for (const auto& ring : polygon)
    {
      if (ring.area > squaredTolerance_) geometry.push_back(simplify(ring));
    }
    return geometry;
  }

  void emit(FeatureType type, GeometryCollection&& geometry)
  {
    // Collection expansion copies properties only for additional output records.
    if (pending_)
    {
      builder_.appendFinal(pending_->type, std::move(pending_->geometry),
                           PropertyMap(feature_.properties), feature_.id);
    }
    pending_.emplace(Pending{type, std::move(geometry)});
  }

  struct Pending
  {
    FeatureType type;
    GeometryCollection geometry;
  };

  NativeTileBuilder& builder_;
  const NativeTileMetadata& metadata_;
  GeoJSONFeature& feature_;
  const double scale_;
  const bool inspectRoadMetadata_;
  const double tolerance_;
  const double squaredTolerance_;
  std::optional<Pending> pending_;
};

} // namespace

NativeGeometryComposer::NativeGeometryComposer(NativeTileMetadata metadata)
  : NativeGeometryComposer(std::move(metadata), true) {}

NativeGeometryComposer::NativeGeometryComposer(NativeTileMetadata metadata, bool inspectRoadMetadata)
  : metadata_(std::move(metadata)), builder_(metadata_), inspectRoadMetadata_(inspectRoadMetadata)
{
  if (metadata_.conversion.clip || metadata_.conversion.wrap)
  {
    throw std::invalid_argument("Native composer requires producer-clipped, unwrapped input");
  }
}

void NativeGeometryComposer::checkOpen() const
{
  if (closed_) throw std::logic_error("Native composer is closed");
}

void NativeGeometryComposer::append(GeoJSONFeature&& feature)
{
  checkOpen();
  try
  {
    mapbox::geometry::for_each_point(feature.geometry, [](const auto& point)
    {
      if (!std::isfinite(point.x) || !std::isfinite(point.y))
      {
        throw std::invalid_argument("Native composer requires finite geographic coordinates");
      }
    });
    const double tolerance = inspectRoadMetadata_ && style::retainShortTileLine(feature)
        ? 0.0 : metadata_.conversion.normalizedTolerance(metadata_.tileID.z);
    auto projected = vt::project{tolerance}(feature.geometry);
    mapbox::geometry::for_each_point(projected, [](const auto& point)
    {
      if (!std::isfinite(point.x) || !std::isfinite(point.y))
      {
        throw std::out_of_range("Native projected coordinate is not finite");
      }
    });
    FinalGeometry final(builder_, metadata_, feature, tolerance, inspectRoadMetadata_);
    vt::vt_geometry::visit(projected, [&final](const auto& value) { final(value); });
    final.finish();
  }
  catch (...)
  {
    closed_ = true;
    throw;
  }
}

NativeTilePayloadPtr NativeGeometryComposer::seal() &&
{
  checkOpen();
  closed_ = true;
  return std::move(builder_).seal();
}

} // namespace mln
