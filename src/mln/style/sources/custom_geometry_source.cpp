#include <cstring>
#include <map>
#include <mln/actor/actor.hpp>
#include <mln/actor/scheduler.hpp>
#include <mln/style/custom_tile_loader.hpp>
#include <mln/style/native_tile_request_state.hpp>
#include <mln/style/layer.hpp>
#include <mln/style/source_observer.hpp>
#include <mln/style/sources/custom_geometry_source.hpp>
#include <mln/style/sources/custom_geometry_source_impl.hpp>
#include <mln/tile/tile.hpp>
#include <mln/tile/tile_id.hpp>
#include <tuple>
#include <stdexcept>
#include <mln/util/tile_range.hpp>

namespace mln {
namespace style {

namespace {
std::shared_ptr<NativeRequestState> makeNativeState(const CustomGeometrySource::Options& options)
{
    if (options.tileOptions.dataType == CustomGeometrySource::TileDataType::LegacyFeatures) return nullptr;
    if (options.tileOptions.dataType != CustomGeometrySource::TileDataType::NativeGeometry
        || !options.nativeCallbacks.resolve || !options.nativeCallbacks.fetch || !options.nativeCallbacks.cancel)
        throw std::invalid_argument("Native source requires resolve, fetch and cancel callbacks");
    return std::make_shared<NativeRequestState>(options.tileOptions);
}
}

CustomGeometrySource::CustomGeometrySource(std::string id, const CustomGeometrySource::Options& options)
    : CustomGeometrySource(std::move(id), options, makeNativeState(options)) {}

CustomGeometrySource::CustomGeometrySource(std::string id, const Options& options,
                                         std::shared_ptr<NativeRequestState> state)
    : Source(makeMutable<CustomGeometrySource::Impl>(id, options, state ? state->sourceEpoch() : 0)),
      nativeState(std::move(state)),
      loader(std::make_unique<Actor<CustomTileLoader>>(
          Scheduler::GetBackground(), options.fetchTileFunction, options.cancelTileFunction, options.tileOptions,
          options.tracedFetch, options.nativeCallbacks, nativeState)) {}

CustomGeometrySource::~CustomGeometrySource() {
    if (nativeState) nativeState->retire();
    tiletrace::retireSource(impl().getTileOptions()->traceSource);
}

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
    if (nativeState) return;
    loader->self().invoke(&CustomTileLoader::setTileData, tileID, data);
}

void CustomGeometrySource::setTileFeatures(const CanonicalTileID& tileID,
                                           const std::shared_ptr<const FeatureCollection>& data) {
    if (nativeState) return;
    loader->self().invoke(&CustomTileLoader::setTileFeatures, tileID, data);
}

void CustomGeometrySource::setTracedTileFeatures(const CanonicalTileID& tileID,
    const std::shared_ptr<const FeatureCollection>& data, tiletrace::Context trace) {
    if (nativeState) { tiletrace::finish(trace, tiletrace::Outcome::StaleSubmit); return; }
    trace.payloadFormat = tiletrace::PayloadFormat::LegacyFeatures;
    tiletrace::mark(trace, tiletrace::Dispatched);
    loader->self().invoke(&CustomTileLoader::setTracedTileFeatures, tileID, data, trace);
}

void CustomGeometrySource::setTracedTilePayload(const CanonicalTileID& tileID, const NativeRequestTicket& ticket,
                                               std::shared_ptr<const NativeTilePayload> data, tiletrace::Context trace)
{
    trace.payloadFormat = tiletrace::PayloadFormat::NativeGeometry;
    if (!nativeState || ticket.sourceEpoch() != nativeState->sourceEpoch() || !ticket.isCurrent()
        || ticket.tileID() != tileID)
    {
        tiletrace::finish(trace, tiletrace::Outcome::StaleSubmit);
        return;
    }
    tiletrace::mark(trace, tiletrace::Dispatched);
    loader->self().invoke(&CustomTileLoader::setTracedTilePayload, tileID, ticket, std::move(data), trace);
}

void CustomGeometrySource::setNativeTileError(const CanonicalTileID& tileID, const NativeRequestTicket& ticket,
                                            std::exception_ptr error, tiletrace::Context trace)
{
    trace.payloadFormat = tiletrace::PayloadFormat::NativeGeometry;
    if (!nativeState || ticket.sourceEpoch() != nativeState->sourceEpoch() || !ticket.isCurrent()
        || ticket.tileID() != tileID)
    {
        tiletrace::finish(trace, tiletrace::Outcome::StaleSubmit);
        return;
    }
    tiletrace::mark(trace, tiletrace::Dispatched);
    loader->self().invoke(&CustomTileLoader::setNativeTileError, tileID, ticket, std::move(error), trace);
}

void CustomGeometrySource::invalidateTile(const CanonicalTileID& tileID) {
    if (nativeState) nativeState->invalidate(tileID);
    const auto options = impl().getTileOptions();
    const tiletrace::ViewTileRange range{tileID.x, tileID.y, tileID.x, tileID.y, tileID.z, tileID.z};
    tiletrace::invalidateView(options->traceMap, options->traceSource, &range);
    loader->self().invoke(&CustomTileLoader::invalidateTile, tileID);
}

void CustomGeometrySource::invalidateRegion(const LatLngBounds& bounds) {
    if (nativeState) nativeState->invalidateRegion(bounds);
    const auto options = impl().getTileOptions();
    const auto zoom = impl().getZoomRange();
    const auto tiles = util::TileRange::fromLatLngBounds(bounds, zoom.min, zoom.max);
    const tiletrace::ViewTileRange range{tiles.range.min.x, tiles.range.min.y, tiles.range.max.x, tiles.range.max.y,
                                        tiles.zoomRange.min, tiles.zoomRange.max};
    tiletrace::invalidateView(options->traceMap, options->traceSource, &range);
    loader->self().invoke(&CustomTileLoader::invalidateRegion, bounds, zoom);
}

void CustomGeometrySource::clearTileCache() {
    if (nativeState) nativeState->clear();
    loader->self().invoke(&CustomTileLoader::clearDataCache);
}

Mutable<Source::Impl> CustomGeometrySource::createMutable() const noexcept {
    return staticMutableCast<Source::Impl>(makeMutable<Impl>(impl()));
}

} // namespace style
} // namespace mln
