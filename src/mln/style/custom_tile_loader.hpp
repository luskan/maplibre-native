#pragma once

#include <mln/actor/actor_ref.hpp>
#include <mln/style/sources/custom_geometry_source.hpp>
#include <mln/tile/tile_id.hpp>
#include <mln/util/geojson.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace mln {

class CustomGeometryTile;

namespace style {

// Tells one tile instance apart from another that uses the same tile ID. Without
// it a dying tile can unregister the live tile that replaced it.
using CustomTileRegistrationToken = std::uint64_t;

// Uses a process-wide counter so different loader instances do not reuse tokens.
CustomTileRegistrationToken nextCustomTileRegistrationToken();

struct CustomTileLoaderRegistrationStats
{
    uint64_t staleRemovesIgnored = 0;
    uint64_t staleCancelsIgnored = 0;
    uint64_t cancelsSuppressed = 0;
    uint64_t fetchesIssued = 0;
    // How many receivers ever shared one tile ID at the same time. Above 1 means
    // a tile was replaced while its twin was still registered, which is the
    // situation that used to lose the replacement.
    uint64_t maxSharedTileIdDepth = 0;
};

CustomTileLoaderRegistrationStats getCustomTileLoaderRegistrationStats();

class CustomTileLoader {
public:
    using TileFeatureCollection = mapbox::feature::feature_collection<int16_t>;
    using TileFeatureCollectionPtr = std::shared_ptr<const TileFeatureCollection>;
    CustomTileLoader(const CustomTileLoader&) = delete;
    CustomTileLoader& operator=(const CustomTileLoader&) = delete;

    using RegistrationToken = CustomTileRegistrationToken;

    CustomTileLoader(const TileFunction& fetchTileFn,
                     const TileFunction& cancelTileFn,
                     const CustomGeometrySource::TileOptions& tileOptions = {},
                     std::function<void(const CanonicalTileID&, tiletrace::Context)> tracedFetch = {});

    void fetchTile(const OverscaledTileID& tileID,
                   const ActorRef<CustomGeometryTile>& tileRef,
                   RegistrationToken token);
    void fetchTracedTile(const OverscaledTileID&, const ActorRef<CustomGeometryTile>&,
                         RegistrationToken, tiletrace::Context);
    void setTracedTileFeatures(const CanonicalTileID&, std::shared_ptr<const FeatureCollection>, tiletrace::Context);
    void cancelTile(const OverscaledTileID& tileID, RegistrationToken token);

    void removeTile(const OverscaledTileID& tileID, RegistrationToken token);
    void setTileData(const CanonicalTileID& tileID, const GeoJSON& data);
    void setTileFeatures(const CanonicalTileID& tileID, std::shared_ptr<const FeatureCollection> data);

    void invalidateTile(const CanonicalTileID&);
    void invalidateRegion(const LatLngBounds&, Range<uint8_t>);
    void clearDataCache();

private:
    struct TileRegistration {
        uint8_t overscaledZ;
        int16_t wrap;
        RegistrationToken token;
        ActorRef<CustomGeometryTile> tileRef;
        // Cleared by cancelTile, set again by fetchTile. The producer is only
        // stopped once no registration wants the tile any more.
        bool wantsData;
        tiletrace::Context trace;
    };

    // One canonical tile can feed several receivers, so they share one producer fetch.
    struct CanonicalEntry {
        std::vector<TileRegistration> registrations;
        // True from the moment a fetch is asked for until a cancel is sent or the
        // entry dies. Publishing does not clear it, the producer may still hold
        // work for this tile after one receiver got its data.
        bool producerWanted = false;
    };

    void invokeTileFetch(const CanonicalTileID& tileID);
    void invokeTileCancel(const CanonicalTileID& tileID,
                          tiletrace::Retirement reason = tiletrace::Retirement::NoDemand);
    // Both expect dataMutex to be held.
    void releaseProducerIfUnwanted(const CanonicalTileID& tileID, CanonicalEntry& entry);
    void dropEntryIfEmpty(const CanonicalTileID& tileID);

    std::function<void(const CanonicalTileID&, tiletrace::Context)> tracedFetch;
    tiletrace::Context incomingDemand;
    TileFunction fetchTileFunction;
    TileFunction cancelTileFunction;
    CustomGeometrySource::TileOptions tileOptions;
    std::unordered_map<CanonicalTileID, CanonicalEntry> tileCallbackMap;
    // Keep around processed tile-local geometry to serve back for wrapped and over-zoomed tiles.
public:
    struct CachedData {
        TileFeatureCollectionPtr features;
        tiletrace::Context trace;
        CachedData() = default;
        CachedData(TileFeatureCollectionPtr value) : features(std::move(value)) {}
        CachedData& operator=(TileFeatureCollectionPtr value) { features = std::move(value); trace = {}; return *this; }
    };
private:
    std::map<CanonicalTileID, CachedData> dataCache;
    std::mutex dataMutex;
};

} // namespace style
} // namespace mln
