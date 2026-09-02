#pragma once

#include <mln/actor/actor_ref.hpp>
#include <mln/style/sources/custom_geometry_source.hpp>
#include <mln/tile/tile_id.hpp>
#include <mln/util/geojson.hpp>

#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace mln {

class CustomGeometryTile;

namespace style {

class CustomTileLoader {
public:
    using TileFeatureCollection = mapbox::feature::feature_collection<int16_t>;
    using TileFeatureCollectionPtr = std::shared_ptr<const TileFeatureCollection>;
    CustomTileLoader(const CustomTileLoader&) = delete;
    CustomTileLoader& operator=(const CustomTileLoader&) = delete;

    using OverscaledIDFunctionTuple = std::tuple<uint8_t, int16_t, ActorRef<CustomGeometryTile>>;

    CustomTileLoader(const TileFunction& fetchTileFn,
                     const TileFunction& cancelTileFn,
                     const CustomGeometrySource::TileOptions& tileOptions = {});

    void fetchTile(const OverscaledTileID& tileID, const ActorRef<CustomGeometryTile>& tileRef);
    void cancelTile(const OverscaledTileID& tileID);

    void removeTile(const OverscaledTileID& tileID);
    void setTileData(const CanonicalTileID& tileID, const GeoJSON& data);
    void setTileFeatures(const CanonicalTileID& tileID, std::shared_ptr<const FeatureCollection> data);

    void invalidateTile(const CanonicalTileID&);
    void invalidateRegion(const LatLngBounds&, Range<uint8_t>);
    void clearDataCache();

private:
    void invokeTileFetch(const CanonicalTileID& tileID);
    void invokeTileCancel(const CanonicalTileID& tileID);

    TileFunction fetchTileFunction;
    TileFunction cancelTileFunction;
    CustomGeometrySource::TileOptions tileOptions;
    std::unordered_map<CanonicalTileID, std::vector<OverscaledIDFunctionTuple>> tileCallbackMap;
    // Keep around processed tile-local geometry to serve back for wrapped and over-zoomed tiles.
    std::map<CanonicalTileID, TileFeatureCollectionPtr> dataCache;
    std::mutex dataMutex;
};

} // namespace style
} // namespace mln
