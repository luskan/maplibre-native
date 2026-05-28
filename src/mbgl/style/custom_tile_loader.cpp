#include <mbgl/style/custom_tile_loader.hpp>
#include <mbgl/tile/custom_geometry_tile.hpp>
#include <mbgl/util/logging.hpp>
#include <mbgl/util/string.hpp>
#include <mbgl/util/tile_range.hpp>

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

} // namespace

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
    auto cachedTileData = dataCache.find(tileID.canonical);
    if (cachedTileData != dataCache.end()) {
        tileRef.invoke(kSetProcessedTileData, cachedTileData->second);
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
        dataCache.erase(tileID.canonical);
    }
}

void CustomTileLoader::setTileData(const CanonicalTileID& tileID, const GeoJSON& data) {
    auto featureData = CustomGeometryTile::processTileData(data, tileID, tileOptions);
    std::lock_guard<std::mutex> guard(dataMutex);
    auto iter = tileCallbackMap.find(tileID);
    if (iter != tileCallbackMap.end()) {
        for (const auto& tuple : iter->second) {
            auto actor = std::get<2>(tuple);
            actor.invoke(kSetProcessedTileData, featureData);
        }
    }
    dataCache[tileID] = std::move(featureData);
}

void CustomTileLoader::setTileFeatures(const CanonicalTileID& tileID, std::shared_ptr<const FeatureCollection> data) {
    const auto inputFeatureCount = data ? data->size() : 0;
    auto featureData = data ? CustomGeometryTile::processTileData(*data, tileID, tileOptions)
                            : std::make_shared<const TileFeatureCollection>();
    const auto processedFeatureCount = featureData ? featureData->size() : 0;
    std::lock_guard<std::mutex> guard(dataMutex);
    auto iter = tileCallbackMap.find(tileID);
    const auto callbackCount = iter != tileCallbackMap.end() ? iter->second.size() : 0;
    if (iter != tileCallbackMap.end()) {
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
    dataCache[tileID] = std::move(featureData);
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
    dataCache.erase(tileID);
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
                dataCache.erase(idtuple.first);
            }
            idtuple.second.clear();
        }
    }
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
