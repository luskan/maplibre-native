#include <mbgl/style/custom_tile_loader.hpp>
#include <mbgl/style/custom_tile_loader_cache.hpp>
#include <mbgl/tile/custom_geometry_tile.hpp>
#include <mbgl/util/logging.hpp>
#include <mbgl/util/string.hpp>
#include <mbgl/util/tile_range.hpp>

#include <atomic>

namespace mbgl {
namespace style {

namespace {

constexpr auto kSetProcessedTileData =
    static_cast<void (CustomGeometryTile::*)(CustomGeometryTile::TileFeatureCollectionPtr)>(
        &CustomGeometryTile::setTileData);

bool shouldLogTileDiagnostic(size_t count) {
    return count > 0 && (count <= 50 || (count % 100) == 0);
}

bool isAutomapaDiagnosticSource(const std::string& sourceID) {
    return sourceID.rfind("automapa-", 0) == 0;
}

std::atomic<bool> g_dataCacheEnabled{true};
std::atomic<uint64_t> g_dataCacheHits{0};
std::atomic<uint64_t> g_dataCacheStores{0};
std::atomic<uint64_t> g_dataCacheBypasses{0};
std::atomic<std::size_t> g_dataCacheTileCount{0};

void eraseCachedTile(std::map<CanonicalTileID, CustomTileLoader::TileFeatureCollectionPtr>& dataCache,
                     const CanonicalTileID& tileID) {
    if (dataCache.erase(tileID) > 0) {
        g_dataCacheTileCount.fetch_sub(1, std::memory_order_relaxed);
    }
}

void clearCachedTiles(std::map<CanonicalTileID, CustomTileLoader::TileFeatureCollectionPtr>& dataCache) {
    const auto size = dataCache.size();
    dataCache.clear();
    if (size > 0) {
        g_dataCacheTileCount.fetch_sub(size, std::memory_order_relaxed);
    }
}

} // namespace

void setCustomTileLoaderDataCacheEnabled(bool enabled) {
    g_dataCacheEnabled.store(enabled, std::memory_order_release);
}

bool isCustomTileLoaderDataCacheEnabled() {
    return g_dataCacheEnabled.load(std::memory_order_acquire);
}

CustomTileLoaderDataCacheStats getCustomTileLoaderDataCacheStats() {
    CustomTileLoaderDataCacheStats stats;
    stats.enabled = isCustomTileLoaderDataCacheEnabled();
    stats.hits = g_dataCacheHits.load(std::memory_order_relaxed);
    stats.stores = g_dataCacheStores.load(std::memory_order_relaxed);
    stats.bypasses = g_dataCacheBypasses.load(std::memory_order_relaxed);
    stats.tileCount = g_dataCacheTileCount.load(std::memory_order_relaxed);
    return stats;
}

CustomTileLoader::CustomTileLoader(std::string sourceID_,
                                   const TileFunction& fetchTileFn,
                                   const TileFunction& cancelTileFn,
                                   const CustomGeometrySource::TileOptions& tileOptions_)
    : sourceID(std::move(sourceID_)),
      tileOptions(tileOptions_) {
    fetchTileFunction = fetchTileFn;
    cancelTileFunction = cancelTileFn;
}

void CustomTileLoader::fetchTile(const OverscaledTileID& tileID, const ActorRef<CustomGeometryTile>& tileRef) {
    std::lock_guard<std::mutex> guard(dataMutex);
    const bool cacheEnabled = isCustomTileLoaderDataCacheEnabled();
    auto cachedTileData = cacheEnabled ? dataCache.find(tileID.canonical) : dataCache.end();
    if (cachedTileData != dataCache.end()) {
        g_dataCacheHits.fetch_add(1, std::memory_order_relaxed);
        tileRef.invoke(kSetProcessedTileData, cachedTileData->second);
    } else if (!cacheEnabled) {
        g_dataCacheBypasses.fetch_add(1, std::memory_order_relaxed);
    }
    auto tileCallbacks = tileCallbackMap.find(tileID.canonical);
    if (tileCallbacks == tileCallbackMap.end()) {
        auto tuple = std::make_tuple(tileID.overscaledZ, tileID.wrap, tileRef);
        tileCallbackMap.insert({tileID.canonical, std::vector<OverscaledIDFunctionTuple>(1, tuple)});
    } else {
        for (auto& iter : tileCallbacks->second) {
            if (std::get<0>(iter) == tileID.overscaledZ && std::get<1>(iter) == tileID.wrap) {
                std::get<2>(iter) = tileRef;
                return;
            }
        }
        tileCallbacks->second.emplace_back(std::make_tuple(tileID.overscaledZ, tileID.wrap, tileRef));
    }
    if (cachedTileData == dataCache.end()) {
        invokeTileFetch(tileID.canonical);
    }
}

void CustomTileLoader::cancelTile(const OverscaledTileID& tileID) {
    std::lock_guard<std::mutex> guard(dataMutex);
    if (tileCallbackMap.find(tileID.canonical) != tileCallbackMap.end()) {
        invokeTileCancel(tileID.canonical);
    }
}

void CustomTileLoader::removeTile(const OverscaledTileID& tileID) {
    std::lock_guard<std::mutex> guard(dataMutex);
    auto tileCallbacks = tileCallbackMap.find(tileID.canonical);
    if (tileCallbacks == tileCallbackMap.end()) return;
    for (auto iter = tileCallbacks->second.begin(); iter != tileCallbacks->second.end(); iter++) {
        if (std::get<0>(*iter) == tileID.overscaledZ && std::get<1>(*iter) == tileID.wrap) {
            tileCallbacks->second.erase(iter);
            invokeTileCancel(tileID.canonical);
            break;
        }
    }
    if (tileCallbacks->second.empty()) {
        tileCallbackMap.erase(tileCallbacks);
        eraseCachedTile(dataCache, tileID.canonical);
    }
}

void CustomTileLoader::setTileData(const CanonicalTileID& tileID, const GeoJSON& data) {
    auto featureData = CustomGeometryTile::processTileData(data, tileID, tileOptions);
    std::lock_guard<std::mutex> guard(dataMutex);
    auto iter = tileCallbackMap.find(tileID);
    // invalidateRegion clears vectors without erasing keys
    const bool hasActiveCallbacks = iter != tileCallbackMap.end() && !iter->second.empty();
    if (hasActiveCallbacks) {
        for (const auto& tuple : iter->second) {
            auto actor = std::get<2>(tuple);
            actor.invoke(kSetProcessedTileData, featureData);
        }
    }
    // do not cache tiles nobody waits for, invalidation would never reach them
    if (isCustomTileLoaderDataCacheEnabled() && hasActiveCallbacks) {
        const bool inserted = dataCache.find(tileID) == dataCache.end();
        dataCache[tileID] = std::move(featureData);
        if (inserted) {
            g_dataCacheTileCount.fetch_add(1, std::memory_order_relaxed);
        }
        g_dataCacheStores.fetch_add(1, std::memory_order_relaxed);
    } else {
        eraseCachedTile(dataCache, tileID);
        g_dataCacheBypasses.fetch_add(1, std::memory_order_relaxed);
    }
}

void CustomTileLoader::setTileFeatures(const CanonicalTileID& tileID, std::shared_ptr<const FeatureCollection> data) {
    const auto inputFeatureCount = data ? data->size() : 0;
    auto featureData = data ? CustomGeometryTile::processTileData(*data, tileID, tileOptions)
                            : std::make_shared<const TileFeatureCollection>();
    const auto processedFeatureCount = featureData ? featureData->size() : 0;
    std::lock_guard<std::mutex> guard(dataMutex);
    auto iter = tileCallbackMap.find(tileID);
    const auto callbackCount = iter != tileCallbackMap.end() ? iter->second.size() : 0;
    // invalidateRegion clears vectors without erasing keys
    const bool hasActiveCallbacks = iter != tileCallbackMap.end() && !iter->second.empty();
    if (hasActiveCallbacks) {
        for (const auto& tuple : iter->second) {
            auto actor = std::get<2>(tuple);
            actor.invoke(kSetProcessedTileData, featureData);
        }
    } else if (isAutomapaDiagnosticSource(sourceID)) {
        const auto count = ++noCallbackDataLogCount;
        if (shouldLogTileDiagnostic(count)) {
            Log::Warning(Event::General,
                         "[MLTileData] source=" + sourceID + " reason=no-active-callback count=" +
                             std::to_string(count) + " tile=" + util::toString(tileID) +
                             " inputFeatures=" + std::to_string(inputFeatureCount) +
                             " processedFeatures=" + std::to_string(processedFeatureCount) +
                             " callbacks=" + std::to_string(callbackCount) +
                             " cacheBefore=" + std::to_string(dataCache.size()));
        }
    }
    // do not cache tiles nobody waits for, invalidation would never reach them
    if (isCustomTileLoaderDataCacheEnabled() && hasActiveCallbacks) {
        const bool inserted = dataCache.find(tileID) == dataCache.end();
        dataCache[tileID] = std::move(featureData);
        if (inserted) {
            g_dataCacheTileCount.fetch_add(1, std::memory_order_relaxed);
        }
        g_dataCacheStores.fetch_add(1, std::memory_order_relaxed);
    } else {
        eraseCachedTile(dataCache, tileID);
        g_dataCacheBypasses.fetch_add(1, std::memory_order_relaxed);
    }
}

void CustomTileLoader::invalidateTile(const CanonicalTileID& tileID) {
    std::lock_guard<std::mutex> guard(dataMutex);
    auto tileCallbacks = tileCallbackMap.find(tileID);
    if (tileCallbacks == tileCallbackMap.end()) {
        return;
    }
    for (auto& iter : tileCallbacks->second) {
        auto actor = std::get<2>(iter);
        actor.invoke(&CustomGeometryTile::invalidateTileData);
        invokeTileCancel(tileID);
    }
    tileCallbackMap.erase(tileCallbacks);
    eraseCachedTile(dataCache, tileID);
}

void CustomTileLoader::invalidateRegion(const LatLngBounds& bounds, Range<uint8_t>) {
    std::lock_guard<std::mutex> guard(dataMutex);
    std::map<uint8_t, util::TileRange> tileRanges;
    for (auto& idtuple : tileCallbackMap) {
        auto zoom = idtuple.first.z;
        auto tileRange = tileRanges.find(zoom);
        if (tileRange == tileRanges.end()) {
            tileRange = tileRanges.emplace(std::make_pair(zoom, util::TileRange::fromLatLngBounds(bounds, zoom))).first;
        }
        if (tileRange->second.contains(idtuple.first)) {
            for (auto iter = idtuple.second.begin(); iter != idtuple.second.end(); iter++) {
                auto actor = std::get<2>(*iter);
                actor.invoke(&CustomGeometryTile::invalidateTileData);
                invokeTileCancel(idtuple.first);
                eraseCachedTile(dataCache, idtuple.first);
            }
            idtuple.second.clear();
        }
    }
}

void CustomTileLoader::clearDataCache() {
    std::lock_guard<std::mutex> guard(dataMutex);
    for (auto& idtuple : tileCallbackMap) {
        for (auto& iter : idtuple.second) {
            auto actor = std::get<2>(iter);
            actor.invoke(&CustomGeometryTile::invalidateTileData);
            invokeTileCancel(idtuple.first);
        }
    }
    tileCallbackMap.clear();
    clearCachedTiles(dataCache);
}

void CustomTileLoader::invokeTileFetch(const CanonicalTileID& tileID) {
    if (fetchTileFunction != nullptr) {
        fetchTileFunction(tileID);
    }
}

void CustomTileLoader::invokeTileCancel(const CanonicalTileID& tileID) {
    if (cancelTileFunction != nullptr) {
        cancelTileFunction(tileID);
    }
}

} // namespace style
} // namespace mbgl
