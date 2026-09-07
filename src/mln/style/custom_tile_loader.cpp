#include <mln/style/custom_tile_loader.hpp>
#include <mln/style/custom_tile_loader_cache.hpp>
#include <mln/tile/custom_geometry_tile.hpp>
#include <mln/util/tile_range.hpp>

#include <algorithm>
#include <atomic>

namespace mln {
namespace style {

namespace {

constexpr auto kSetProcessedTileData =
    static_cast<void (CustomGeometryTile::*)(CustomGeometryTile::TileFeatureCollectionPtr)>(
        &CustomGeometryTile::setTileData);

std::atomic<bool> g_dataCacheEnabled{true};
std::atomic<uint64_t> g_dataCacheHits{0};
std::atomic<uint64_t> g_dataCacheStores{0};
std::atomic<uint64_t> g_dataCacheBypasses{0};
std::atomic<std::size_t> g_dataCacheTileCount{0};

std::atomic<uint64_t> g_registrationToken{0};
std::atomic<uint64_t> g_staleRemovesIgnored{0};
std::atomic<uint64_t> g_staleCancelsIgnored{0};
std::atomic<uint64_t> g_cancelsSuppressed{0};
std::atomic<uint64_t> g_fetchesIssued{0};
std::atomic<uint64_t> g_maxSharedTileIdDepth{0};

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

CustomTileRegistrationToken nextCustomTileRegistrationToken() {
    return g_registrationToken.fetch_add(1, std::memory_order_relaxed) + 1;
}

CustomTileLoaderRegistrationStats getCustomTileLoaderRegistrationStats() {
    CustomTileLoaderRegistrationStats stats;
    stats.staleRemovesIgnored = g_staleRemovesIgnored.load(std::memory_order_relaxed);
    stats.staleCancelsIgnored = g_staleCancelsIgnored.load(std::memory_order_relaxed);
    stats.cancelsSuppressed = g_cancelsSuppressed.load(std::memory_order_relaxed);
    stats.fetchesIssued = g_fetchesIssued.load(std::memory_order_relaxed);
    stats.maxSharedTileIdDepth = g_maxSharedTileIdDepth.load(std::memory_order_relaxed);
    return stats;
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

CustomTileLoader::CustomTileLoader(const TileFunction& fetchTileFn,
                                   const TileFunction& cancelTileFn,
                                   const CustomGeometrySource::TileOptions& tileOptions_)
    : tileOptions(tileOptions_) {
    fetchTileFunction = fetchTileFn;
    cancelTileFunction = cancelTileFn;
}

void CustomTileLoader::fetchTile(const OverscaledTileID& tileID,
                                const ActorRef<CustomGeometryTile>& tileRef,
                                RegistrationToken token) {
    std::scoped_lock guard(dataMutex);
    const bool cacheEnabled = isCustomTileLoaderDataCacheEnabled();
    auto cachedTileData = cacheEnabled ? dataCache.find(tileID.canonical) : dataCache.end();
    if (cachedTileData != dataCache.end()) {
        g_dataCacheHits.fetch_add(1, std::memory_order_relaxed);
        tileRef.invoke(kSetProcessedTileData, cachedTileData->second);
    } else if (!cacheEnabled) {
        g_dataCacheBypasses.fetch_add(1, std::memory_order_relaxed);
    }

    auto& entry = tileCallbackMap[tileID.canonical];
    auto registration = std::find_if(entry.registrations.begin(),
                                     entry.registrations.end(),
                                     [token](const TileRegistration& candidate) {
                                         return candidate.token == token;
                                     });
    if (registration == entry.registrations.end()) {
        entry.registrations.push_back(
            TileRegistration{tileID.overscaledZ, tileID.wrap, token, tileRef, true});
        // Count how deep one tile ID gets shared. More than one receiver for the
        // same zoom and wrap means a tile was replaced before its twin went away.
        const auto sameTileId = std::count_if(entry.registrations.begin(),
                                              entry.registrations.end(),
                                              [&tileID](const TileRegistration& candidate) {
                                                  return candidate.overscaledZ == tileID.overscaledZ &&
                                                         candidate.wrap == tileID.wrap;
                                              });
        auto depth = static_cast<uint64_t>(sameTileId);
        auto known = g_maxSharedTileIdDepth.load(std::memory_order_relaxed);
        while (depth > known && !g_maxSharedTileIdDepth.compare_exchange_weak(known, depth)) {
        }
    } else {
        registration->tileRef = tileRef;
        registration->wantsData = true;
    }

    if (cachedTileData != dataCache.end()) {
        return;
    }
    // Always ask the producer on a miss. Suppressing the call would mean trusting
    // an earlier fetch that may already have been cancelled or dropped, and the
    // producer is the only side that knows whether work is still queued.
    entry.producerWanted = true;
    g_fetchesIssued.fetch_add(1, std::memory_order_relaxed);
    invokeTileFetch(tileID.canonical);
}

void CustomTileLoader::cancelTile(const OverscaledTileID& tileID, RegistrationToken token) {
    std::scoped_lock guard(dataMutex);
    auto entry = tileCallbackMap.find(tileID.canonical);
    if (entry == tileCallbackMap.end()) return;
    auto registration = std::find_if(entry->second.registrations.begin(),
                                     entry->second.registrations.end(),
                                     [token](const TileRegistration& candidate) {
                                         return candidate.token == token;
                                     });
    if (registration == entry->second.registrations.end()) {
        g_staleCancelsIgnored.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // Keep the registration, the tile can become required again.
    registration->wantsData = false;
    releaseProducerIfUnwanted(tileID.canonical, entry->second);
}

void CustomTileLoader::removeTile(const OverscaledTileID& tileID, RegistrationToken token) {
    std::scoped_lock guard(dataMutex);
    auto entry = tileCallbackMap.find(tileID.canonical);
    if (entry == tileCallbackMap.end()) return;
    auto registration = std::find_if(entry->second.registrations.begin(),
                                     entry->second.registrations.end(),
                                     [token](const TileRegistration& candidate) {
                                         return candidate.token == token;
                                     });
    if (registration == entry->second.registrations.end()) {
        // A late message from a tile whose registration is already gone. Removing
        // anything by tile ID here would strand the tile that took its place.
        g_staleRemovesIgnored.fetch_add(1, std::memory_order_relaxed);
        dropEntryIfEmpty(tileID.canonical);
        return;
    }
    entry->second.registrations.erase(registration);
    releaseProducerIfUnwanted(tileID.canonical, entry->second);
    dropEntryIfEmpty(tileID.canonical);
}

void CustomTileLoader::releaseProducerIfUnwanted(const CanonicalTileID& tileID, CanonicalEntry& entry) {
    const bool stillWanted = std::any_of(entry.registrations.begin(),
                                         entry.registrations.end(),
                                         [](const TileRegistration& candidate) {
                                             return candidate.wantsData;
                                         });
    if (stillWanted) {
        if (entry.producerWanted) {
            g_cancelsSuppressed.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
    if (!entry.producerWanted) {
        return;
    }
    entry.producerWanted = false;
    invokeTileCancel(tileID);
}

void CustomTileLoader::dropEntryIfEmpty(const CanonicalTileID& tileID) {
    auto entry = tileCallbackMap.find(tileID);
    if (entry == tileCallbackMap.end() || !entry->second.registrations.empty()) {
        return;
    }
    tileCallbackMap.erase(entry);
    eraseCachedTile(dataCache, tileID);
}

void CustomTileLoader::setTileData(const CanonicalTileID& tileID, const GeoJSON& data) {
    bool hasActiveCallbacks = false;
    {
        std::lock_guard<std::mutex> guard(dataMutex);
        const auto iter = tileCallbackMap.find(tileID);
        hasActiveCallbacks = iter != tileCallbackMap.end() && !iter->second.registrations.empty();
        if (!hasActiveCallbacks) {
            eraseCachedTile(dataCache, tileID);
        }
    }
    if (!hasActiveCallbacks) {
        g_dataCacheBypasses.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    auto featureData = CustomGeometryTile::processTileData(data, tileID, tileOptions);
    std::vector<ActorRef<CustomGeometryTile>> callbacks;
    {
        std::lock_guard<std::mutex> guard(dataMutex);
        const auto iter = tileCallbackMap.find(tileID);
        if (iter != tileCallbackMap.end()) {
            for (const auto& registration : iter->second.registrations) {
                callbacks.push_back(registration.tileRef);
            }
        }
        if (isCustomTileLoaderDataCacheEnabled() && !callbacks.empty()) {
            const bool inserted = dataCache.find(tileID) == dataCache.end();
            dataCache[tileID] = featureData;
            if (inserted) {
                g_dataCacheTileCount.fetch_add(1, std::memory_order_relaxed);
            }
            g_dataCacheStores.fetch_add(1, std::memory_order_relaxed);
        } else {
            eraseCachedTile(dataCache, tileID);
            g_dataCacheBypasses.fetch_add(1, std::memory_order_relaxed);
        }
    }
    for (const auto& tileRef : callbacks) {
        tileRef.invoke(kSetProcessedTileData, featureData);
    }
}

void CustomTileLoader::setTileFeatures(const CanonicalTileID& tileID, std::shared_ptr<const FeatureCollection> data) {
    bool hasActiveCallbacks = false;
    {
        std::lock_guard<std::mutex> guard(dataMutex);
        const auto iter = tileCallbackMap.find(tileID);
        hasActiveCallbacks = iter != tileCallbackMap.end() && !iter->second.registrations.empty();
        if (!hasActiveCallbacks) {
            eraseCachedTile(dataCache, tileID);
        }
    }
    if (!hasActiveCallbacks) {
        g_dataCacheBypasses.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    auto featureData = data ? CustomGeometryTile::processTileData(*data, tileID, tileOptions)
                            : std::make_shared<const TileFeatureCollection>();
    std::vector<ActorRef<CustomGeometryTile>> callbacks;
    {
        std::lock_guard<std::mutex> guard(dataMutex);
        const auto iter = tileCallbackMap.find(tileID);
        if (iter != tileCallbackMap.end()) {
            for (const auto& registration : iter->second.registrations) {
                callbacks.push_back(registration.tileRef);
            }
        }
        if (isCustomTileLoaderDataCacheEnabled() && !callbacks.empty()) {
            const bool inserted = dataCache.find(tileID) == dataCache.end();
            dataCache[tileID] = featureData;
            if (inserted) {
                g_dataCacheTileCount.fetch_add(1, std::memory_order_relaxed);
            }
            g_dataCacheStores.fetch_add(1, std::memory_order_relaxed);
        } else {
            eraseCachedTile(dataCache, tileID);
            g_dataCacheBypasses.fetch_add(1, std::memory_order_relaxed);
        }
    }
    for (const auto& tileRef : callbacks) {
        tileRef.invoke(kSetProcessedTileData, featureData);
    }
}

void CustomTileLoader::invalidateTile(const CanonicalTileID& tileID) {
    std::scoped_lock guard(dataMutex);
    auto tileCallbacks = tileCallbackMap.find(tileID);
    if (tileCallbacks == tileCallbackMap.end()) {
        return;
    }
    for (auto& registration : tileCallbacks->second.registrations) {
        registration.tileRef.invoke(&CustomGeometryTile::invalidateTileData);
    }
    const bool wasWanted = tileCallbacks->second.producerWanted;
    tileCallbackMap.erase(tileCallbacks);
    eraseCachedTile(dataCache, tileID);
    // One cancel for the canonical tile, the receivers shared a single fetch.
    if (wasWanted) {
        invokeTileCancel(tileID);
    }
}

void CustomTileLoader::invalidateRegion(const LatLngBounds& bounds, Range<uint8_t>) {
    std::scoped_lock guard(dataMutex);
    std::map<uint8_t, util::TileRange> tileRanges;
    // Entries left without receivers, erased below so the map does not keep every
    // canonical tile that was ever invalidated.
    std::vector<CanonicalTileID> emptied;
    for (auto& idtuple : tileCallbackMap) {
        auto zoom = idtuple.first.z;
        auto tileRange = tileRanges.find(zoom);
        if (tileRange == tileRanges.end()) {
            tileRange = tileRanges.emplace(std::make_pair(zoom, util::TileRange::fromLatLngBounds(bounds, zoom))).first;
        }
        if (tileRange->second.contains(idtuple.first)) {
            for (auto& registration : idtuple.second.registrations) {
                registration.tileRef.invoke(&CustomGeometryTile::invalidateTileData);
            }
            if (!idtuple.second.registrations.empty()) {
                eraseCachedTile(dataCache, idtuple.first);
            }
            // One cancel for the canonical tile, the receivers shared a single fetch.
            if (idtuple.second.producerWanted) {
                idtuple.second.producerWanted = false;
                invokeTileCancel(idtuple.first);
            }
            idtuple.second.registrations.clear();
            emptied.push_back(idtuple.first);
        }
    }
    for (const auto& tileID : emptied) {
        dropEntryIfEmpty(tileID);
    }
}

void CustomTileLoader::clearDataCache() {
    std::lock_guard<std::mutex> guard(dataMutex);
    for (auto& idtuple : tileCallbackMap) {
        for (auto& registration : idtuple.second.registrations) {
            registration.tileRef.invoke(&CustomGeometryTile::invalidateTileData);
        }
        if (idtuple.second.producerWanted) {
            idtuple.second.producerWanted = false;
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
} // namespace mln
