#include <mln/tile/geojson_geometry_memo.hpp>

namespace mln {
namespace {

std::size_t geometryBytes(const GeometryCollection& geometry) {
  std::size_t bytes = geometry.capacity() * sizeof(GeometryCoordinates);
  for (const auto& ring : geometry) bytes += ring.capacity() * sizeof(GeometryCoordinate);
  return bytes;
}

} // namespace

GeoJSONGeometryMemo::GeoJSONGeometryMemo(std::shared_ptr<const Features> features_, bool memoizeGeometry,
                                       std::shared_ptr<GeometryMemoObserver> observer_)
  : features(std::move(features_)), memoize(memoizeGeometry), observer(std::move(observer_)),
    entries(memoize ? std::make_unique<Entry[]>(features->size()) : nullptr) {
  if (observer) observer->onDataCreated(features, memoize ? features->size() * sizeof(Entry) : 0);
}

GeoJSONGeometryMemo::~GeoJSONGeometryMemo() {
  if (observer) {
    observer->onGeometryReleased(retained.load(std::memory_order_relaxed));
    observer->onDataDestroyed(memoize ? features->size() * sizeof(Entry) : 0);
  }
}

GeometryCollection GeoJSONGeometryMemo::build(std::size_t index) const {
  const auto token = observer ? observer->onGeometryStart() : 0;
  const auto& feature = (*features)[index];
  auto result = apply_visitor(ToGeometryCollection(), feature.geometry);
  const bool polygon = apply_visitor(ToFeatureType(), feature.geometry) == FeatureType::Polygon;
  if (polygon) result = fixupPolygons(result);
  if (observer) {
    const auto bytes = geometryBytes(result);
    if (memoize) retained.fetch_add(bytes, std::memory_order_relaxed);
    observer->onGeometryCreated(token, polygon, bytes);
  }
  return result;
}

const GeometryCollection& GeoJSONGeometryMemo::get(std::size_t index,
                                                  std::optional<GeometryCollection>& local) const {
  if (memoize) {
    auto& entry = entries[index];
    std::call_once(entry.ready, [&] { entry.geometry = build(index); });
    if (observer) observer->onGeometryAccess();
    return *entry.geometry;
  }
  if (!local) local = build(index);
  if (observer) observer->onGeometryAccess();
  return *local;
}

void GeoJSONGeometryMemo::release(const std::optional<GeometryCollection>& local) const {
  if (observer && local) observer->onGeometryReleased(geometryBytes(*local));
}

} // namespace mln
