#include <mln/tile/custom_geometry_tile.hpp>
#include <mln/tile/geojson_tile_data.hpp>
#include <mln/renderer/query.hpp>
#include <mln/renderer/tile_parameters.hpp>
#include <mln/actor/scheduler.hpp>
#include <mln/util/string.hpp>
#include <mln/tile/tile_observer.hpp>
#include <mln/style/custom_tile_loader.hpp>
#include <mln/style/custom_tile_conversion.hpp>

#include <mapbox/geojsonvt.hpp>

#include <utility>

namespace mln {

namespace {

mapbox::geojsonvt::TileOptions makeVTOptions(const style::CustomGeometrySource::TileOptions& options, uint8_t z) {
    const auto spec = style::customTileConversionSpec(options, z);

    mapbox::geojsonvt::TileOptions vtOptions;
    vtOptions.extent = spec.extent;
    vtOptions.buffer = spec.buffer;
    vtOptions.tolerance = spec.tolerance;
    return vtOptions;
}

CustomGeometryTile::TileFeatureCollectionPtr toSharedFeatureCollection(mapbox::feature::feature_collection<int16_t> features) {
    return std::make_shared<const CustomGeometryTile::TileFeatureCollection>(std::move(features));
}

} // namespace

CustomGeometryTile::CustomGeometryTile(const OverscaledTileID& overscaledTileID,
                                       std::string sourceID_,
                                       const TileParameters& parameters,
                                       Immutable<style::CustomGeometrySource::TileOptions> options_,
                                       ActorRef<style::CustomTileLoader> loader_,
                                       TileObserver* observer_)
    : GeometryTile(overscaledTileID, std::move(sourceID_), parameters, observer_),
      necessity(TileNecessity::Optional),
      options(std::move(options_)),
      loader(std::move(loader_)),
      registrationToken(style::nextCustomTileRegistrationToken()),
      mailbox(std::make_shared<Mailbox>(*Scheduler::GetCurrent())),
      actorRef(*this, mailbox) {}

CustomGeometryTile::~CustomGeometryTile() {
    tiletrace::finish(traceDemand, tiletrace::Outcome::Teardown);
    mailbox->close();  // Prevent messages from being delivered to destroyed tile (UAF fix)
    loader.invoke(&style::CustomTileLoader::removeTile, id, registrationToken);
}

CustomGeometryTile::TileFeatureCollectionPtr CustomGeometryTile::processTileData(
    const GeoJSON& geoJSON,
    const CanonicalTileID& tileID,
    const style::CustomGeometrySource::TileOptions& tileOptions) {
    if (geoJSON.is<FeatureCollection>()) {
        return processTileData(geoJSON.get<FeatureCollection>(), tileID, tileOptions);
    }

    return toSharedFeatureCollection(mapbox::geojsonvt::geoJSONToTile(
                                         geoJSON,
                                         tileID.z,
                                         tileID.x,
                                         tileID.y,
                                         makeVTOptions(tileOptions, tileID.z),
                                         tileOptions.wrap,
                                         tileOptions.clip)
                                         .features);
}

CustomGeometryTile::TileFeatureCollectionPtr CustomGeometryTile::processTileData(
    const FeatureCollection& features,
    const CanonicalTileID& tileID,
    const style::CustomGeometrySource::TileOptions& tileOptions) {
    if (features.empty()) {
        return std::make_shared<const TileFeatureCollection>();
    }

    return toSharedFeatureCollection(mapbox::geojsonvt::geoJSONToTile(
                                         features,
                                         tileID.z,
                                         tileID.x,
                                         tileID.y,
                                         makeVTOptions(tileOptions, tileID.z),
                                         tileOptions.wrap,
                                         tileOptions.clip)
                                         .features);
}

void CustomGeometryTile::setTileData(const GeoJSON& geoJSON) {
    setTileData(processTileData(geoJSON, id.canonical, *options));
}

void CustomGeometryTile::setTileData(TileFeatureCollectionPtr featureData) {
    setData(std::make_unique<GeoJSONTileData>(
        featureData ? std::move(featureData) : std::make_shared<const TileFeatureCollection>(),
        options->memoizeGeometry, options->geometryMemoObserver));
}

void CustomGeometryTile::setTracedTileData(TileFeatureCollectionPtr featureData, tiletrace::Context trace) {
    tiletrace::mark(trace, tiletrace::Delivered);
    tiletrace::bindDemand(trace);
    auto data = std::make_unique<GeoJSONTileData>(
        featureData ? std::move(featureData) : std::make_shared<const TileFeatureCollection>(),
        options->memoizeGeometry, options->geometryMemoObserver);
    data->trace = trace;
    setData(std::move(data));
}

void CustomGeometryTile::invalidateTileData() {
    stale = true;
    observer->onTileChanged(*this);
}

// Fetching tile data for custom sources is assumed to be an expensive operation.
//  Only required tiles make fetchTile requests. Attempt to cancel a tile
//  that is no longer required.
void CustomGeometryTile::setNecessity(TileNecessity newNecessity) {
    if (newNecessity != necessity || stale) {
        necessity = newNecessity;
        if (necessity == TileNecessity::Required) {
            tiletrace::finish(traceDemand, tiletrace::Outcome::Superseded);
            traceDemand = tiletrace::create(options->traceMap, options->traceSource, registrationToken,
                id.canonical.z, id.canonical.x, id.canonical.y, id.overscaledZ, id.wrap, tileTraceRole, tileTraceView);
            if (stale || !isRenderable()) {
                loader.invoke(&style::CustomTileLoader::fetchTracedTile, id, actorRef, registrationToken, traceDemand);
            }
            const auto* retained = retainedTraceForDiagnostics();
            if (!stale && isRenderable() && retained) {
                auto reuse = *retained;
                reuse.demand = traceDemand.id;
                reuse.view = traceDemand.view;
                reuse.session = traceDemand.session;
                reuse.origin = tiletrace::Origin::Renderer;
                reuse.time = {};
                reuse.time[tiletrace::Request] = traceDemand.time[tiletrace::Request];
                tiletrace::bindDemand(reuse);
            }
            stale = false;
        } else {
            tiletrace::finish(traceDemand, tiletrace::Outcome::Cancelled);
            if (!isRenderable()) loader.invoke(&style::CustomTileLoader::cancelTile, id, registrationToken);
        }
    }
}

void CustomGeometryTile::querySourceFeatures(std::vector<Feature>& result, const SourceQueryOptions& queryOptions) {
    // Ignore the sourceLayer, there is only one
    auto layer = getData()->getLayer({});

    if (layer) {
        auto featureCount = layer->featureCount();
        for (std::size_t i = 0; i < featureCount; i++) {
            auto feature = layer->getFeature(i);

            // Apply filter, if any
            if (queryOptions.filter && !(*queryOptions.filter)(style::expression::EvaluationContext{
                                           static_cast<float>(id.overscaledZ), feature.get()})) {
                continue;
            }

            result.push_back(convertFeature(*feature, id.canonical));
        }
    }
}

} // namespace mln
