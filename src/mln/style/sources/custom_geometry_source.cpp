#include <cstring>
#include <map>
#include <mln/actor/actor.hpp>
#include <mln/actor/scheduler.hpp>
#include <mln/style/custom_tile_loader.hpp>
#include <mln/style/layer.hpp>
#include <mln/style/source_observer.hpp>
#include <mln/style/sources/custom_geometry_source.hpp>
#include <mln/style/sources/custom_geometry_source_impl.hpp>
#include <mln/tile/tile.hpp>
#include <mln/tile/tile_id.hpp>
#include <tuple>
#include <mln/util/tile_range.hpp>

namespace mln {
namespace style {

CustomGeometrySource::CustomGeometrySource(std::string id, const CustomGeometrySource::Options& options)
    : Source(makeMutable<CustomGeometrySource::Impl>(id, options)),
      loader(std::make_unique<Actor<CustomTileLoader>>(
          Scheduler::GetBackground(), options.fetchTileFunction, options.cancelTileFunction, options.tileOptions, options.tracedFetch)) {}

CustomGeometrySource::~CustomGeometrySource() { tiletrace::retireSource(impl().getTileOptions()->traceSource); }

const CustomGeometrySource::Impl& CustomGeometrySource::impl() const {
    return static_cast<const CustomGeometrySource::Impl&>(*baseImpl);
}

void CustomGeometrySource::loadDescription(FileSource&) {
    baseImpl = makeMutable<Impl>(impl(), loader->self());
    loaded = true;
    observer->onSourceLoaded(*this);
}

bool CustomGeometrySource::supportsLayerType(const mln::style::LayerTypeInfo* info) const {
    return mln::underlying_type(Tile::Kind::Geometry) == mln::underlying_type(info->tileKind);
}

void CustomGeometrySource::setTileData(const CanonicalTileID& tileID, const GeoJSON& data) {
    loader->self().invoke(&CustomTileLoader::setTileData, tileID, data);
}

void CustomGeometrySource::setTileFeatures(const CanonicalTileID& tileID,
                                           const std::shared_ptr<const FeatureCollection>& data) {
    loader->self().invoke(&CustomTileLoader::setTileFeatures, tileID, data);
}

void CustomGeometrySource::setTracedTileFeatures(const CanonicalTileID& tileID,
    const std::shared_ptr<const FeatureCollection>& data, tiletrace::Context trace) {
    tiletrace::mark(trace, tiletrace::Dispatched);
    loader->self().invoke(&CustomTileLoader::setTracedTileFeatures, tileID, data, trace);
}

void CustomGeometrySource::invalidateTile(const CanonicalTileID& tileID) {
    const auto options = impl().getTileOptions();
    const tiletrace::ViewTileRange range{tileID.x, tileID.y, tileID.x, tileID.y, tileID.z, tileID.z};
    tiletrace::invalidateView(options->traceMap, options->traceSource, &range);
    loader->self().invoke(&CustomTileLoader::invalidateTile, tileID);
}

void CustomGeometrySource::invalidateRegion(const LatLngBounds& bounds) {
    const auto options = impl().getTileOptions();
    const auto zoom = impl().getZoomRange();
    const auto tiles = util::TileRange::fromLatLngBounds(bounds, zoom.min, zoom.max);
    const tiletrace::ViewTileRange range{tiles.range.min.x, tiles.range.min.y, tiles.range.max.x, tiles.range.max.y,
                                        tiles.zoomRange.min, tiles.zoomRange.max};
    tiletrace::invalidateView(options->traceMap, options->traceSource, &range);
    loader->self().invoke(&CustomTileLoader::invalidateRegion, bounds, zoom);
}

void CustomGeometrySource::clearTileCache() {
    loader->self().invoke(&CustomTileLoader::clearDataCache);
}

Mutable<Source::Impl> CustomGeometrySource::createMutable() const noexcept {
    return staticMutableCast<Source::Impl>(makeMutable<Impl>(impl()));
}

} // namespace style
} // namespace mln
