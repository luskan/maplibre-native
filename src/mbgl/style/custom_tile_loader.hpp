#pragma once

#include <mbgl/actor/actor_ref.hpp>
#include <mbgl/style/sources/custom_geometry_source.hpp>
#include <mbgl/tile/tile_id.hpp>
#include <mbgl/util/geojson.hpp>

#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace mbgl {

class CustomGeometryTile;

namespace style {

class CustomTileLoader {
public:
    using TileFeatureCollection = mapbox::feature::feature_collection<int16_t>;
    using TileFeatureCollectionPtr = std::shared_ptr<const TileFeatureCollection>;
    CustomTileLoader(const CustomTileLoader&) = delete;
    CustomTileLoader& operator=(const CustomTileLoader&) = delete;

    using OverscaledIDFunctionTuple = std::tuple<uint8_t, int16_t, ActorRef<CustomGeometryTile>>;

    CustomTileLoader(std::string sourceID,
                     const TileFunction& fetchTileFn,
                     const TileFunction& cancelTileFn,
                     const CustomGeometrySource::TileOptions& tileOptions = {});

    void fetchTile(const OverscaledTileID& tileID, const ActorRef<CustomGeometryTile>& tileRef);
    void cancelTile(const OverscaledTileID& tileID);

    void removeTile(const OverscaledTileID& tileID);
    void setTileData(const CanonicalTileID& tileID, const GeoJSON& data);
    void setTileFeatures(const CanonicalTileID& tileID, std::shared_ptr<const FeatureCollection> data);

    void invalidateTile(const CanonicalTileID&);
    void invalidateRegion(const LatLngBounds&, Range<uint8_t>);

private:
    void invokeTileFetch(const CanonicalTileID& tileID);
    void invokeTileCancel(const CanonicalTileID& tileID);

    TileFunction fetchTileFunction;
    TileFunction cancelTileFunction;
    std::string sourceID;
    CustomGeometrySource::TileOptions tileOptions;
    size_t noCallbackDataLogCount = 0;
    std::unordered_map<CanonicalTileID, std::vector<OverscaledIDFunctionTuple>> tileCallbackMap;
    // Keep around processed tile-local geometry to serve back for wrapped and over-zoomed tiles.
    std::map<CanonicalTileID, TileFeatureCollectionPtr> dataCache;
    std::mutex dataMutex;
};

} // namespace style
} // namespace mbgl
