#include <mln/style/custom_tile_loader.hpp>
#include <mln/style/custom_tile_loader_cache.hpp>
#include <mln/tile/custom_geometry_tile.hpp>
#include <mln/style/native_tile_request_state.hpp>
#include <mln/util/tile_range.hpp>
#include <mln/util/scoped.hpp>
#include <mln/util/logging.hpp>

#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <utility>

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

void eraseCachedTile(std::map<CanonicalTileID, CustomTileLoader::CachedData>& dataCache,
                     const CanonicalTileID& tileID) {
    if (dataCache.erase(tileID) > 0) {
        g_dataCacheTileCount.fetch_sub(1, std::memory_order_relaxed);
    }
}

void clearCachedTiles(std::map<CanonicalTileID, CustomTileLoader::CachedData>& dataCache) {
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
                                   const CustomGeometrySource::TileOptions& tileOptions_,
    std::function<void(const CanonicalTileID&, tiletrace::Context)> tracedFetch_,
    NativeTileCallbacks nativeCallbacks_, std::shared_ptr<NativeRequestState> nativeState_)
    : tracedFetch(std::move(tracedFetch_)), tileOptions(tileOptions_),
      nativeCallbacks(std::move(nativeCallbacks_)), nativeState(std::move(nativeState_)) {
    fetchTileFunction = fetchTileFn;
    cancelTileFunction = cancelTileFn;
    if (tileOptions.dataType == CustomGeometrySource::TileDataType::NativeGeometry) {
        if (!nativeCallbacks.resolve || !nativeCallbacks.fetch || !nativeCallbacks.cancel)
            throw std::invalid_argument("Native loader requires resolve, fetch and cancel callbacks");
        if (!nativeState) nativeState = std::make_shared<NativeRequestState>(tileOptions);
        if (nativeState->contract() != nativeTileConversionContract(tileOptions))
            throw std::invalid_argument("Native loader conversion contract does not match its source");
    } else if (tileOptions.dataType != CustomGeometrySource::TileDataType::LegacyFeatures || nativeState) {
        throw std::invalid_argument("Invalid custom tile data type");
    }
}

CustomTileLoader::~CustomTileLoader() {
    if (nativeState) { nativeState->retire(); clearCachedTiles(dataCache); }
}

uint64_t CustomTileLoader::nativeSourceEpoch() const noexcept {
    return nativeState ? nativeState->sourceEpoch() : 0;
}

bool CustomTileLoader::accepts(const CanonicalTileID& tile, const NativeRequestTicket& ticket) const {
    if (!nativeState || !ticket.isCurrent() || ticket.sourceEpoch() != nativeState->sourceEpoch()
        || ticket.tileID() != tile) return false;
    const auto found = tileCallbackMap.find(tile);
    return found != tileCallbackMap.end() && found->second.nativeTicket == ticket;
}

void CustomTileLoader::fetchTracedTile(const OverscaledTileID& id,
    const ActorRef<CustomGeometryTile>& ref, RegistrationToken token, tiletrace::Context trace) {
    incomingDemand = trace;
    fetchTile(id, ref, token);
    incomingDemand = {};
}

void CustomTileLoader::fetchTile(const OverscaledTileID& tileID,
                                const ActorRef<CustomGeometryTile>& tileRef,
                                RegistrationToken token) {
    std::vector<NativeCancellation> cancellations;
    Scoped notify([&] { notifyNativeCancellations(cancellations); });
    NativeRequestResolution resolution;
    if (nativeState) resolution = nativeState->resolve(tileID.canonical, nativeCallbacks.resolve);
    NativeRequestTicket previousTicket;
    std::unique_lock guard(dataMutex);
    auto& entry = tileCallbackMap[tileID.canonical];
    if (nativeState && resolution.ticket && entry.nativeTicket != resolution.ticket) {
        previousTicket = entry.nativeTicket;
        for (const auto& previous : entry.registrations) {
            if (previous.token != token) previous.tileRef.invoke(&CustomGeometryTile::invalidateTileData);
        }
        if (entry.producerWanted)
            invokeTileCancel(tileID.canonical, cancellations, tiletrace::Retirement::Invalidated, previousTicket);
        entry.producerWanted = false;
        entry.registrations.clear();
        eraseCachedTile(dataCache, tileID.canonical);
        entry.nativeTicket = resolution.ticket;
        entry.nativePublication.reset();
    }
    incomingDemand.payloadFormat = nativeState ? tiletrace::PayloadFormat::NativeGeometry
                                               : tiletrace::PayloadFormat::LegacyFeatures;
    auto registration = std::find_if(entry.registrations.begin(), entry.registrations.end(),
        [token](const TileRegistration& candidate) { return candidate.token == token; });
    if (registration == entry.registrations.end()) {
        entry.registrations.push_back(
            TileRegistration{tileID.overscaledZ, tileID.wrap, token, tileRef, true, incomingDemand});
        const auto sameTileId = std::count_if(entry.registrations.begin(), entry.registrations.end(),
            [&tileID](const TileRegistration& candidate) {
                return candidate.overscaledZ == tileID.overscaledZ && candidate.wrap == tileID.wrap;
            });
        const auto depth = static_cast<uint64_t>(sameTileId);
        auto known = g_maxSharedTileIdDepth.load(std::memory_order_relaxed);
        while (depth > known && !g_maxSharedTileIdDepth.compare_exchange_weak(known, depth)) {}
    } else {
        registration->tileRef = tileRef;
        registration->wantsData = true;
        registration->trace = incomingDemand;
    }
    if (nativeState && !resolution.ticket.isCurrent()) {
        tiletrace::finish(incomingDemand, tiletrace::Outcome::StaleFast);
        tileRef.invoke(&CustomGeometryTile::invalidateTileData);
        return;
    }
    if (resolution.error) {
        guard.unlock();
        notifyNativeCancellations(cancellations);
        setNativeTileError(tileID.canonical, resolution.ticket, resolution.error, incomingDemand);
        return;
    }

    const bool cacheEnabled = isCustomTileLoaderDataCacheEnabled();
    auto cached = cacheEnabled ? dataCache.find(tileID.canonical) : dataCache.end();
    if (nativeState && cached != dataCache.end() && cached->second.nativeTicket != resolution.ticket) {
        eraseCachedTile(dataCache, tileID.canonical);
        cached = dataCache.end();
    }
    if (cached != dataCache.end()) {
        g_dataCacheHits.fetch_add(1, std::memory_order_relaxed);
        if (incomingDemand.id && !cached->second.trace.publication)
            cached->second.trace.publication = tiletrace::nextID();
        auto trace = cached->second.trace;
        trace.kind = tiletrace::Kind::Publication;
        trace.id = incomingDemand.id ? tiletrace::nextID() : 0;
        trace.session = incomingDemand.session;
        trace.map = incomingDemand.map;
        trace.source = incomingDemand.source;
        trace.role = incomingDemand.role;
        trace.view = incomingDemand.view;
        trace.demand = incomingDemand.demand;
        trace.consumer = token;
        trace.z = tileID.canonical.z;
        trace.x = tileID.canonical.x;
        trace.y = tileID.canonical.y;
        trace.overscaledZ = tileID.overscaledZ;
        trace.wrap = tileID.wrap;
        trace.origin = tiletrace::Origin::Processed;
        trace.time = {};
        trace.time[tiletrace::Request] = incomingDemand.time[tiletrace::Request];
        if (nativeState)
            tileRef.invoke(&CustomGeometryTile::setNativeTileData, std::get<NativeTilePayloadPtr>(cached->second.payload),
                           resolution.ticket, trace);
        else
            tileRef.invoke(&CustomGeometryTile::setTracedTileData,
                           std::get<TileFeatureCollectionPtr>(cached->second.payload), trace);
        return;
    }
    if (!cacheEnabled) g_dataCacheBypasses.fetch_add(1, std::memory_order_relaxed);
    // A previous request may have been dropped, so each miss still reaches the producer.
    entry.producerWanted = true;
    if (nativeState) {
        const auto ticket = entry.nativeTicket;
        const auto trace = incomingDemand;
        guard.unlock();
        notifyNativeCancellations(cancellations);
        bool stillWanted = false;
        {
            std::lock_guard currentGuard(dataMutex);
            const auto current = tileCallbackMap.find(tileID.canonical);
            if (accepts(tileID.canonical, ticket) && current->second.producerWanted) {
                stillWanted = std::any_of(current->second.registrations.begin(), current->second.registrations.end(),
                    [token](const TileRegistration& receiver) { return receiver.token == token && receiver.wantsData; });
            }
        }
        if (!stillWanted) {
            auto stale = trace;
            const bool current = ticket.isCurrent();
            tiletrace::finish(stale, current ? tiletrace::Outcome::Cancelled : tiletrace::Outcome::StaleFast);
            if (!current) tileRef.invoke(&CustomGeometryTile::invalidateTileData);
            return;
        }
        g_fetchesIssued.fetch_add(1, std::memory_order_relaxed);
        try { nativeCallbacks.fetch(tileID.canonical, ticket, trace); }
        catch (...) { setNativeTileError(tileID.canonical, ticket, std::current_exception(), trace); }
    } else {
        g_fetchesIssued.fetch_add(1, std::memory_order_relaxed);
        if (tracedFetch) tracedFetch(tileID.canonical, incomingDemand);
        else invokeTileFetch(tileID.canonical);
    }
}

void CustomTileLoader::cancelTile(const OverscaledTileID& tileID, RegistrationToken token) {
    std::vector<NativeCancellation> cancellations;
    Scoped notify([&] { notifyNativeCancellations(cancellations); });
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
    releaseProducerIfUnwanted(tileID.canonical, entry->second, cancellations);
}

void CustomTileLoader::removeTile(const OverscaledTileID& tileID, RegistrationToken token) {
    std::vector<NativeCancellation> cancellations;
    Scoped notify([&] { notifyNativeCancellations(cancellations); });
    NativeRequestTicket released;
    std::scoped_lock guard(dataMutex);
    auto entry = tileCallbackMap.find(tileID.canonical);
    if (entry == tileCallbackMap.end()) return;
    released = entry->second.nativeTicket;
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
    releaseProducerIfUnwanted(tileID.canonical, entry->second, cancellations);
    dropEntryIfEmpty(tileID.canonical);
}

void CustomTileLoader::releaseProducerIfUnwanted(const CanonicalTileID& tileID, CanonicalEntry& entry,
                                               std::vector<NativeCancellation>& cancellations) {
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
    invokeTileCancel(tileID, cancellations);
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
    if (nativeState) return;
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
    publish(tileID, featureData, {}, {}, false);
}

void CustomTileLoader::setTileFeatures(const CanonicalTileID& tileID, std::shared_ptr<const FeatureCollection> data) {
    setTracedTileFeatures(tileID, std::move(data), {});
}

void CustomTileLoader::setTracedTileFeatures(const CanonicalTileID& tileID,
    std::shared_ptr<const FeatureCollection> data, tiletrace::Context trace) {
    if (nativeState) { tiletrace::finish(trace, tiletrace::Outcome::StaleDrain); return; }
    trace.payloadFormat = tiletrace::PayloadFormat::LegacyFeatures;
    tiletrace::mark(trace, tiletrace::Loader);
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
        tiletrace::finish(trace, tiletrace::Outcome::NoReceiver);
        return;
    }

    TileFeatureCollectionPtr featureData;
    try {
        featureData = data ? CustomGeometryTile::processTileData(*data, tileID, tileOptions)
                           : std::make_shared<const TileFeatureCollection>();
    } catch (...) {
        tiletrace::finish(trace, tiletrace::Outcome::Error);
        throw;
    }
    tiletrace::mark(trace, tiletrace::Converted);
    trace.empty = featureData->empty();
    publish(tileID, featureData, trace);
}

void CustomTileLoader::setTracedTilePayload(const CanonicalTileID& tileID, NativeRequestTicket ticket,
                                          NativeTilePayloadPtr payload, tiletrace::Context trace) {
    trace.payloadFormat = tiletrace::PayloadFormat::NativeGeometry;
    tiletrace::mark(trace, tiletrace::Loader);
    {
        std::lock_guard guard(dataMutex);
        if (!accepts(tileID, ticket)) {
            tiletrace::finish(trace, tiletrace::Outcome::StaleDrain);
            return;
        }
    }
    if (!payload || ticket.binding().key.empty() || payload->metadata().tileID != tileID
        || payload->metadata().conversion != nativeState->contract()) {
        setNativeTileError(tileID, ticket,
                          std::make_exception_ptr(std::invalid_argument("Native tile payload does not match its request")),
                          trace);
        return;
    }
    tiletrace::mark(trace, tiletrace::Converted);
    trace.empty = payload->features().empty();
    publish(tileID, std::move(payload), trace, std::move(ticket));
}

void CustomTileLoader::setNativeTileError(const CanonicalTileID& tileID, NativeRequestTicket ticket,
                                        std::exception_ptr error, tiletrace::Context trace) {
    trace.payloadFormat = tiletrace::PayloadFormat::NativeGeometry;
    std::vector<TileRegistration> callbacks;
    bool published = false;
    {
        std::lock_guard guard(dataMutex);
        if (!accepts(tileID, ticket)) {
            tiletrace::finish(trace, tiletrace::Outcome::StaleDrain);
            return;
        }
        const auto& entry = tileCallbackMap.find(tileID)->second;
        callbacks = entry.registrations;
        published = entry.nativePublication
            && (*entry.nativePublication == trace.publication || trace.kind == tiletrace::Kind::Demand);
        const auto cached = dataCache.find(tileID);
        if (cached != dataCache.end() && cached->second.nativeTicket != ticket) eraseCachedTile(dataCache, tileID);
    }
    if (!error) error = std::make_exception_ptr(std::runtime_error("Native tile request failed"));
    if (!published) tiletrace::finish(trace, callbacks.empty() ? tiletrace::Outcome::NoReceiver : tiletrace::Outcome::Error);
    else {
        trace.publication = trace.demand = trace.generation = trace.payloadSession = 0;
        trace.time = {};
        trace.origin = tiletrace::Origin::Unknown;
    }
    for (const auto& receiver : callbacks) {
        auto delivery = deliveryTrace(tileID, trace, receiver);
        delivery.kind = tiletrace::Kind::Publication;
        receiver.tileRef.invoke(&CustomGeometryTile::setNativeTileError, ticket, error, delivery);
    }
}

tiletrace::Context CustomTileLoader::deliveryTrace(const CanonicalTileID& tileID, const tiletrace::Context& trace,
                                                 const TileRegistration& receiver) {
    auto delivery = trace;
    delivery.id = receiver.trace.id ? tiletrace::nextID() : 0;
    if (delivery.session != receiver.trace.session) {
        delivery.session = receiver.trace.session;
        delivery.time = {};
        delivery.origin = tiletrace::Origin::Unknown;
    }
    delivery.map = receiver.trace.map;
    delivery.source = receiver.trace.source;
    delivery.demand = receiver.trace.demand;
    delivery.role = receiver.trace.role;
    delivery.view = receiver.trace.view;
    delivery.consumer = receiver.token;
    delivery.overscaledZ = receiver.overscaledZ;
    delivery.wrap = receiver.wrap;
    delivery.z = tileID.z;
    delivery.x = tileID.x;
    delivery.y = tileID.y;
    delivery.time[tiletrace::Request] = receiver.trace.time[tiletrace::Request];
    return delivery;
}

void CustomTileLoader::publish(const CanonicalTileID& tileID, ProcessedTilePayload payload,
                              tiletrace::Context trace, NativeRequestTicket ticket, bool traced) {
    trace.payloadFormat = nativeState ? tiletrace::PayloadFormat::NativeGeometry : tiletrace::PayloadFormat::LegacyFeatures;
    std::vector<TileRegistration> callbacks;
    CachedData replaced;
    bool cached = false;
    uint64_t cacheTime = 0;
    {
        std::lock_guard guard(dataMutex);
        if (nativeState && !accepts(tileID, ticket)) {
            tiletrace::finish(trace, tiletrace::Outcome::StaleDrain);
            return;
        }
        const auto entry = tileCallbackMap.find(tileID);
        if (entry != tileCallbackMap.end()) callbacks = entry->second.registrations;
        const auto old = dataCache.find(tileID);
        if (old != dataCache.end()) replaced = std::move(old->second);
        if (isCustomTileLoaderDataCacheEnabled() && !callbacks.empty()) {
            const bool inserted = old == dataCache.end();
            auto& stored = dataCache[tileID];
            stored.payload = payload;
            stored.trace = trace;
            stored.nativeTicket = ticket;
            cached = true;
            if (inserted) g_dataCacheTileCount.fetch_add(1, std::memory_order_relaxed);
            g_dataCacheStores.fetch_add(1, std::memory_order_relaxed);
        } else {
            eraseCachedTile(dataCache, tileID);
            g_dataCacheBypasses.fetch_add(1, std::memory_order_relaxed);
        }
        if (nativeState && !callbacks.empty()) entry->second.nativePublication = trace.publication;
        if (traced) cacheTime = tiletrace::now();
    }
    if (traced && !callbacks.empty()) tiletrace::batchCacheAdmission(trace, cached, cacheTime);
    if (callbacks.empty()) tiletrace::finish(trace, tiletrace::Outcome::NoReceiver);
    for (const auto& receiver : callbacks) {
        if (!traced) {
            receiver.tileRef.invoke(kSetProcessedTileData, std::get<TileFeatureCollectionPtr>(payload));
            continue;
        }
        auto delivery = deliveryTrace(tileID, trace, receiver);
        if (nativeState)
            receiver.tileRef.invoke(&CustomGeometryTile::setNativeTileData,
                                   std::get<NativeTilePayloadPtr>(payload), ticket, delivery);
        else receiver.tileRef.invoke(&CustomGeometryTile::setTracedTileData,
                                     std::get<TileFeatureCollectionPtr>(payload), delivery);
    }
}

void CustomTileLoader::invalidateTile(const CanonicalTileID& tileID) {
    std::vector<NativeCancellation> cancellations;
    Scoped notify([&] { notifyNativeCancellations(cancellations); });
    if (nativeState) nativeState->invalidate(tileID);
    NativeRequestTicket released;
    std::scoped_lock guard(dataMutex);
    auto tileCallbacks = tileCallbackMap.find(tileID);
    if (tileCallbacks == tileCallbackMap.end()) {
        return;
    }
    released = tileCallbacks->second.nativeTicket;
    for (auto& registration : tileCallbacks->second.registrations) {
        registration.tileRef.invoke(&CustomGeometryTile::invalidateTileData);
    }
    const bool wasWanted = tileCallbacks->second.producerWanted;
    tileCallbackMap.erase(tileCallbacks);
    eraseCachedTile(dataCache, tileID);
    // One cancel for the canonical tile, the receivers shared a single fetch.
    if (wasWanted) {
        invokeTileCancel(tileID, cancellations, tiletrace::Retirement::Invalidated, released);
    }
}

void CustomTileLoader::invalidateRegion(const LatLngBounds& bounds, Range<uint8_t>) {
    std::vector<NativeCancellation> cancellations;
    Scoped notify([&] { notifyNativeCancellations(cancellations); });
    if (nativeState) nativeState->invalidateRegion(bounds);
    std::vector<NativeRequestTicket> released;
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
            if (nativeState) released.push_back(idtuple.second.nativeTicket);
            for (auto& registration : idtuple.second.registrations) {
                registration.tileRef.invoke(&CustomGeometryTile::invalidateTileData);
            }
            if (!idtuple.second.registrations.empty()) {
                eraseCachedTile(dataCache, idtuple.first);
            }
            // One cancel for the canonical tile, the receivers shared a single fetch.
            if (idtuple.second.producerWanted) {
                idtuple.second.producerWanted = false;
                invokeTileCancel(idtuple.first, cancellations, tiletrace::Retirement::Invalidated,
                                 idtuple.second.nativeTicket);
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
    std::vector<NativeCancellation> cancellations;
    Scoped notify([&] { notifyNativeCancellations(cancellations); });
    if (nativeState) nativeState->clear();
    std::vector<NativeRequestTicket> released;
    std::lock_guard<std::mutex> guard(dataMutex);
    for (auto& idtuple : tileCallbackMap) {
        if (nativeState) released.push_back(idtuple.second.nativeTicket);
        for (auto& registration : idtuple.second.registrations) {
            registration.tileRef.invoke(&CustomGeometryTile::invalidateTileData);
        }
        if (idtuple.second.producerWanted) {
            idtuple.second.producerWanted = false;
            invokeTileCancel(idtuple.first, cancellations, tiletrace::Retirement::CacheClear, idtuple.second.nativeTicket);
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

void CustomTileLoader::invokeTileCancel(const CanonicalTileID& tileID, std::vector<NativeCancellation>& cancellations,
                                      tiletrace::Retirement reason,
                                      NativeRequestTicket ticket) {
    tiletrace::retireBatchTile(tileOptions.traceMap, tileOptions.traceSource,
                             tileID.z, tileID.x, tileID.y, reason);
    if (nativeState) {
        if (!ticket) {
            const auto found = tileCallbackMap.find(tileID);
            if (found != tileCallbackMap.end()) ticket = found->second.nativeTicket;
        }
        if (ticket) cancellations.push_back({tileID, std::move(ticket)});
    } else if (cancelTileFunction != nullptr) {
        cancelTileFunction(tileID);
    }
}

void CustomTileLoader::notifyNativeCancellations(std::vector<NativeCancellation>& cancellations) noexcept {
    const auto pending = std::exchange(cancellations, {});
    for (const auto& cancellation : pending) {
        try { nativeCallbacks.cancel(cancellation.tile, cancellation.ticket); }
        catch (...) { Log::Error(Event::General, "Native tile cancellation callback threw an exception"); }
    }
}

} // namespace style
} // namespace mln
