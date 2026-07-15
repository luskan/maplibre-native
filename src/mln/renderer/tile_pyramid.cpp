#include <mln/renderer/tile_pyramid.hpp>
#include <mln/renderer/paint_parameters.hpp>
#include <mln/renderer/render_source.hpp>
#include <mln/renderer/tile_parameters.hpp>
#include <mln/renderer/query.hpp>
#include <mln/map/transform.hpp>
#include <mln/math/clamp.hpp>
#include <mln/actor/scheduler.hpp>
#include <mln/util/tile_cover.hpp>
#include <mln/util/tile_range.hpp>
#include <mln/util/enum.hpp>
#include <mln/util/logging.hpp>

#include <mln/algorithm/update_renderables.hpp>

#include <mapbox/geometry/envelope.hpp>

#include <array>
#include <cmath>
#include <algorithm>
#include <string>

namespace mln {

using namespace style;

namespace {
TileObserver nullObserver;
const std::map<OverscaledTileID, std::unique_ptr<Tile>> emptyPrefetchedTiles;

bool shouldLogTileDiagnostic(size_t count) {
    return count > 0 && (count <= 50 || (count % 100) == 0);
}

bool isAutomapaDiagnosticSource(const std::string& sourceID) {
    return sourceID.rfind("automapa-", 0) == 0;
}

std::string boolDigit(bool value) {
    return value ? "1" : "0";
}

std::string tileState(const Tile* tile) {
    if (!tile) {
        return "missing";
    }

    return "renderable=" + boolDigit(tile->isRenderable()) + ",loaded=" + boolDigit(tile->isLoaded()) +
           ",complete=" + boolDigit(tile->isComplete()) + ",pending=" + boolDigit(tile->isPending()) +
           ",triedCache=" + boolDigit(tile->hasTriedCache());
}

char tileStateCode(const Tile* tile) {
    if (!tile) {
        return 'M';
    }
    if (tile->isRenderable()) {
        return 'R';
    }
    if (tile->isPending()) {
        return 'P';
    }
    if (tile->isLoaded()) {
        return 'L';
    }
    if (tile->hasTriedCache()) {
        return 'T';
    }
    return 'N';
}

struct CoverageDiagnostics {
    bool covered = false;
    bool exact = false;
    bool parent = false;
    size_t childSlots = 0;
    size_t descendants = 0;
};

CoverageDiagnostics coverageForIdeal(
    const UnwrappedTileID& idealTile,
    const std::map<UnwrappedTileID, std::reference_wrapper<Tile>>& renderedTiles) {
    CoverageDiagnostics out;
    std::array<bool, 4> coveredChildren{{false, false, false, false}};
    const auto idealChildren = idealTile.children();

    for (const auto& entry : renderedTiles) {
        const auto& renderedTile = entry.first;
        if (renderedTile.wrap != idealTile.wrap) {
            continue;
        }

        if (renderedTile == idealTile) {
            out.exact = true;
            out.covered = true;
            continue;
        }

        if (idealTile.isChildOf(renderedTile)) {
            out.parent = true;
            out.covered = true;
            continue;
        }

        if (renderedTile.isChildOf(idealTile)) {
            ++out.descendants;
            const auto childAtNextZoom = renderedTile.canonical.scaledTo(idealTile.canonical.z + 1);
            for (size_t i = 0; i < idealChildren.size(); ++i) {
                if (idealChildren[i].canonical == childAtNextZoom) {
                    coveredChildren[i] = true;
                    break;
                }
            }
        }
    }

    for (bool childCovered : coveredChildren) {
        if (childCovered) {
            ++out.childSlots;
        }
    }

    out.covered = out.covered || out.childSlots == coveredChildren.size();
    return out;
}

std::string childStateSummary(const OverscaledTileID& idealTile,
                              const std::map<OverscaledTileID, std::unique_ptr<Tile>>& tiles,
                              Range<uint8_t> zoomRange) {
    if (idealTile.overscaledZ >= zoomRange.max) {
        return "n/a";
    }

    std::string result;
    const uint8_t childOverscaledZ = idealTile.overscaledZ + 1;
    const auto children = idealTile.canonical.children();
    for (size_t i = 0; i < children.size(); ++i) {
        const OverscaledTileID childID(childOverscaledZ, idealTile.wrap, children[i]);
        const auto it = tiles.find(childID);
        if (!result.empty()) {
            result += ",";
        }
        result += std::to_string(i);
        result += ":";
        result += tileStateCode(it == tiles.end() ? nullptr : it->second.get());
    }
    return result;
}

std::pair<std::string, std::string> nearestParentState(const OverscaledTileID& idealTile,
                                                       const std::map<OverscaledTileID, std::unique_ptr<Tile>>& tiles,
                                                       Range<uint8_t> zoomRange) {
    for (int32_t z = static_cast<int32_t>(idealTile.overscaledZ) - 1; z >= zoomRange.min; --z) {
        const auto parentID = idealTile.scaledTo(static_cast<uint8_t>(z));
        const auto it = tiles.find(parentID);
        if (it != tiles.end()) {
            return {util::toString(parentID), tileState(it->second.get())};
        }
    }
    return {"none", "missing"};
}
} // namespace

TilePyramid::TilePyramid(const TaggedScheduler& threadPool_)
    : cache(threadPool_),
      observer(&nullObserver) {}

TilePyramid::~TilePyramid() = default;

bool TilePyramid::isLoaded() const {
    for (const auto& pair : tiles) {
        if (!pair.second->isComplete()) {
            return false;
        }
    }

    return true;
}

Tile* TilePyramid::getTile(const OverscaledTileID& tileID) {
    auto it = tiles.find(tileID);
    return it == tiles.end() ? cache.get(tileID) : it->second.get();
}

const Tile* TilePyramid::getRenderedTile(const UnwrappedTileID& tileID) const {
    auto it = renderedTiles.find(tileID);
    return it != renderedTiles.end() ? &it->second.get() : nullptr;
}

void TilePyramid::update(const std::vector<Immutable<style::LayerProperties>>& layers,
                         const bool needsRendering,
                         const bool needsRelayout,
                         const TileParameters& parameters,
                         const style::Source::Impl& sourceImpl,
                         const uint16_t tileSize,
                         const Range<uint8_t> zoomRange,
                         std::optional<LatLngBounds> bounds,
                         std::function<std::unique_ptr<Tile>(const OverscaledTileID&, TileObserver*)> createTile) {
    // If we need a relayout, abandon any cached tiles; they're now stale.
    if (needsRelayout) {
        cache.clear();
    }

    // If we're not going to render anything, move our existing tiles into
    // the cache (if they're not stale) or abandon them, and return.
    if (!needsRendering) {
        for (auto& entry : tiles) {
            if (!needsRelayout) {
                // These tiles are invisible, we set optional necessity
                // for them and thus suppress network requests on
                // tiles expiration (see `OnlineFileRequest`).
                entry.second->setNecessity(TileNecessity::Optional);
                cache.add(entry.first, std::move(entry.second));
            } else {
                cache.deferredRelease(std::move(entry.second));
            }
        }

        tiles.clear();
        renderedTiles.clear();
        cache.deferPendingReleases();

        return;
    }

    handleWrapJump(static_cast<float>(parameters.transformState.getLatLng().longitude()));

    // Optionally shift the zoom level
    double zoom = util::clamp<double>(parameters.transformState.getZoom() + parameters.tileLodZoomShift,
                                      parameters.transformState.getMinZoom(),
                                      parameters.transformState.getMaxZoom());

    const auto type = sourceImpl.type;
    // Determine the overzooming/underzooming amounts and required tiles.
    int32_t overscaledZoom = util::coveringZoomLevel(zoom, type, tileSize);
    int32_t tileZoom = overscaledZoom;
    int32_t panZoom = zoomRange.max;

    const std::optional<uint8_t>& sourcePrefetchZoomDelta = sourceImpl.getPrefetchZoomDelta();
    const std::optional<uint8_t>& maxParentTileOverscaleFactor = sourceImpl.getMaxOverscaleFactorForParentTiles();
    const Duration minimumUpdateInterval = sourceImpl.getMinimumTileUpdateInterval();
    const bool isVolatile = sourceImpl.isVolatile();

    std::vector<OverscaledTileID> idealTiles;
    std::vector<OverscaledTileID> panTiles;

    util::TileCoverParameters tileCoverParameters = {.transformState = parameters.transformState,
                                                     .tileLodMinRadius = parameters.tileLodMinRadius,
                                                     .tileLodScale = parameters.tileLodScale,
                                                     .tileLodPitchThreshold = parameters.tileLodPitchThreshold,
                                                     .tileLodMode = parameters.tileLodMode};

    if (std::cmp_greater_equal(overscaledZoom, zoomRange.min)) {
        int32_t idealZoom = std::min<int32_t>(zoomRange.max, overscaledZoom);

        // Make sure we're not reparsing overzoomed raster tiles.
        if (type == SourceType::Raster) {
            tileZoom = idealZoom;
        }

        // Only attempt prefetching in continuous mode.
        if (parameters.mode == MapMode::Continuous && type != style::SourceType::GeoJSON &&
            type != style::SourceType::Annotations) {
            // Request lower zoom level tiles (if configured to do so) in an attempt
            // to show something on the screen faster at the cost of a little of bandwidth.
            const uint8_t prefetchZoomDelta = sourcePrefetchZoomDelta ? *sourcePrefetchZoomDelta
                                                                      : parameters.prefetchZoomDelta;
            if (prefetchZoomDelta) {
                panZoom = std::max<int32_t>(tileZoom - prefetchZoomDelta, zoomRange.min);
            }

            if (panZoom < idealZoom) {
                panTiles = util::tileCover(tileCoverParameters, panZoom, zoomRange);
            }
        }

        idealTiles = util::tileCover(tileCoverParameters, idealZoom, zoomRange, tileZoom);
        if (parameters.mode == MapMode::Tile && type != SourceType::Raster && type != SourceType::RasterDEM &&
            idealTiles.size() > 1) {
            mln::Log::Warning(mln::Event::General,
                              "Provided camera options returned " + std::to_string(idealTiles.size()) +
                                  " tiles, only " + util::toString(idealTiles[0]) + " is taken in Tile mode.");
            idealTiles = {idealTiles[0]};
        }
    }

    // Stores a list of all the tiles that we're definitely going to retain.
    // There are two kinds of tiles we need: the ideal tiles determined by the
    // tile cover. They may not yet be in use because they're still loading. In
    // addition to that, we also need to retain all tiles that we're actively
    // using, e.g. as a replacement for tile that aren't loaded yet.
    std::set<OverscaledTileID> retain;

    auto retainTileFn = [&](Tile& tile, TileNecessity necessity) -> void {
        if (retain.emplace(tile.id).second) {
            tile.setUpdateParameters({.minimumUpdateInterval = minimumUpdateInterval, .isVolatile = isVolatile});
            tile.setNecessity(necessity);
        }

        if (needsRelayout) {
            tile.setLayers(layers, parameters);
        }
    };
    auto getTileFn = [&](const OverscaledTileID& tileID) -> Tile* {
        auto it = tiles.find(tileID);
        return it == tiles.end() ? nullptr : it->second.get();
    };

    // The min and max zoom for TileRange are based on the updateRenderables
    // algorithm. Tiles are created at the ideal tile zoom or at lower zoom
    // levels. Child tiles are used from the cache, but not created.
    std::optional<util::TileRange> tileRange = std::nullopt;
    if (bounds) {
        int32_t maxZoom = (parameters.tileLodMode == TileLodMode::Distance)
                              ? zoomRange.max
                              : std::min(tileZoom, static_cast<int32_t>(zoomRange.max));
        tileRange = util::TileRange::fromLatLngBounds(*bounds, zoomRange.min, maxZoom);
    }
    auto createTileFn = [&](const OverscaledTileID& tileID) -> Tile* {
        if (tileRange && !tileRange->contains(tileID.canonical)) {
            return nullptr;
        }
        std::unique_ptr<Tile> tile = cache.pop(tileID);
        if (!tile) {
            tile = createTile(tileID, observer);
            if (!tile) return nullptr;
            tile->setLayers(layers, parameters);
        }

        return tiles.emplace(tileID, std::move(tile)).first->second.get();
    };

    auto previouslyRenderedTiles = std::move(renderedTiles);

    auto renderTileFn = [&](const UnwrappedTileID& tileID, Tile& tile) {
        addRenderTile(tileID, tile);
        previouslyRenderedTiles.erase(tileID); // Still rendering this tile, no need for special fading logic.
        tile.markRenderedIdeal();
    };

    renderedTiles.clear();

    if (!panTiles.empty()) {
        algorithm::updateRenderables(
            getTileFn,
            createTileFn,
            retainTileFn,
            [](const UnwrappedTileID&, Tile&) {},
            panTiles,
            emptyPrefetchedTiles,
            zoomRange,
            maxParentTileOverscaleFactor);
    }

    algorithm::updateRenderables(getTileFn,
                                 createTileFn,
                                 retainTileFn,
                                 renderTileFn,
                                 idealTiles,
                                 tiles,
                                 zoomRange,
                                 maxParentTileOverscaleFactor);

    for (auto previouslyRenderedTile : previouslyRenderedTiles) {
        Tile& tile = previouslyRenderedTile.second;
        tile.markRenderedPreviously();
        if (tile.holdForFade()) {
            // Since it was rendered in the last frame, we know we have it
            // Don't mark the tile "Required" to avoid triggering a new network request
            retainTileFn(tile, TileNecessity::Optional);
            addRenderTile(previouslyRenderedTile.first, tile);
        }
    }

    const bool logAutomapaDiagnostics = isAutomapaDiagnosticSource(sourceImpl.id);
    if (!diagnosticSelfTestLogged) {
        diagnosticSelfTestLogged = true;
        Log::Warning(Event::General,
                     "[MLTileDiag] reason=self-test source=" + sourceImpl.id + " zoom=" + std::to_string(zoom) +
                         " coverZ=" + std::to_string(overscaledZoom) + " tileZ=" + std::to_string(tileZoom) +
                         " idealTiles=" + std::to_string(idealTiles.size()) +
                         " renderedTiles=" + std::to_string(renderedTiles.size()) +
                         " activeTiles=" + std::to_string(tiles.size()) +
                         " layers=" + std::to_string(layers.size()));
    }
    if (logAutomapaDiagnostics) {
        for (const auto& idealTile : idealTiles) {
            const auto idealRenderTile = idealTile.toUnwrapped();
            const auto coverage = coverageForIdeal(idealRenderTile, renderedTiles);
            if (coverage.covered) {
                continue;
            }

            const auto count = ++coverageGapLogCount;
            if (!shouldLogTileDiagnostic(count)) {
                continue;
            }

            const auto idealIt = tiles.find(idealTile);
            const auto parent = nearestParentState(idealTile, tiles, zoomRange);
            const std::string reason = coverage.descendants > 0 ? "partial-child-coverage" : "no-rendered-coverage";
            Log::Warning(Event::General,
                         "[MLTileGap] source=" + sourceImpl.id + " reason=" + reason + " count=" +
                             std::to_string(count) + " zoom=" + std::to_string(zoom) +
                             " coverZ=" + std::to_string(overscaledZoom) +
                             " tileZ=" + std::to_string(tileZoom) + " idealData=" + util::toString(idealTile) +
                             " idealRender=" + util::toString(idealRenderTile) +
                             " idealState=" + tileState(idealIt == tiles.end() ? nullptr : idealIt->second.get()) +
                             " parent=" + parent.first + " parentState=" + parent.second +
                             " children=" + childStateSummary(idealTile, tiles, zoomRange) +
                             " childSlots=" + std::to_string(coverage.childSlots) + "/4" +
                             " descendants=" + std::to_string(coverage.descendants) +
                             " renderedTiles=" + std::to_string(renderedTiles.size()) +
                             " activeTiles=" + std::to_string(tiles.size()) +
                             " retained=" + std::to_string(retain.size()) +
                             " idealTiles=" + std::to_string(idealTiles.size()) +
                             " panTiles=" + std::to_string(panTiles.size()) +
                             " mode=" + std::to_string(static_cast<int>(parameters.mode)) +
                             " minZ=" + std::to_string(zoomRange.min) + " maxZ=" + std::to_string(zoomRange.max) +
                             " maxParent=" +
                             (maxParentTileOverscaleFactor ? std::to_string(*maxParentTileOverscaleFactor) : "none"));
        }
    }

    if (type != SourceType::Annotations && cacheEnabled) {
        auto conservativeCacheSize = static_cast<size_t>(
            std::max(static_cast<double>(parameters.transformState.getSize().width) / tileSize, 1.0) *
            std::max(static_cast<double>(parameters.transformState.getSize().height) / tileSize, 1.0) *
            (parameters.transformState.getMaxZoom() - parameters.transformState.getMinZoom() + 1) * 0.5);
        cache.setSize(conservativeCacheSize);
    } else {
        cache.setSize(0);
    }

    // Remove stale tiles. This goes through the (sorted!) tiles map and retain
    // set in lockstep and removes items from tiles that don't have the
    // corresponding key in the retain set.
    {
        auto tilesIt = tiles.begin();
        auto retainIt = retain.begin();
        while (tilesIt != tiles.end()) {
            if (retainIt == retain.end() || tilesIt->first < *retainIt) {
                // Remove the tile from the map.
                // If it requires re-layout, discard it asynchronously, otherwise keep it in the cache
                const auto key = tilesIt->first;
                if (std::unique_ptr<Tile> tile = std::move(tiles.extract(tilesIt++).mapped())) {
                    if (needsRelayout) {
                        cache.deferredRelease(std::move(tile));
                    } else {
                        tile->setNecessity(TileNecessity::Optional);
                        cache.add(key, std::move(tile));
                    }
                }
            } else {
                if (!(*retainIt < tilesIt->first)) {
                    ++tilesIt;
                }
                ++retainIt;
            }
        }
    }

    for (auto& pair : tiles) {
        pair.second->setShowCollisionBoxes(parameters.debugOptions & MapDebugOptions::Collision);
    }

    // Initialize renderable tiles and update the contained layer render data.
    for (auto& entry : renderedTiles) {
        Tile& tile = entry.second;
        assert(tile.isRenderable());
        tile.usedByRenderedLayers = false;

        const bool holdForFade = tile.holdForFade();
        for (const auto& layerProperties : layers) {
            const auto* typeInfo = layerProperties->baseImpl->getTypeInfo();
            if (holdForFade && typeInfo->fadingTiles == LayerTypeInfo::FadingTiles::NotRequired) {
                continue;
            }
            tile.usedByRenderedLayers |= tile.layerPropertiesUpdated(layerProperties);
        }

        if (logAutomapaDiagnostics && !tile.usedByRenderedLayers) {
            const auto count = ++coverageGapLogCount;
            if (shouldLogTileDiagnostic(count)) {
                Log::Warning(Event::General,
                             "[MLTileGap] source=" + sourceImpl.id + " reason=rendered-tile-no-layer-data count=" +
                                 std::to_string(count) + " tile=" + util::toString(tile.id) +
                                 " renderedAs=" + util::toString(entry.first) + " state=" + tileState(&tile) +
                                 " layers=" + std::to_string(layers.size()) +
                                 " renderedTiles=" + std::to_string(renderedTiles.size()) +
                                 " activeTiles=" + std::to_string(tiles.size()) +
                                 " zoom=" + std::to_string(zoom));
            }
        }
    }

    cache.deferPendingReleases();
}

void TilePyramid::handleWrapJump(float lng) {
    // On top of the regular z/x/y values, TileIDs have a `wrap` value that specify
    // which cppy of the world the tile belongs to. For example, at `lng: 10` you
    // might render z/x/y/0 while at `lng: 370` you would render z/x/y/1.
    //
    // When lng values get wrapped (going from `lng: 370` to `long: 10`) you expect
    // to see the same thing on the screen (370 degrees and 10 degrees is the same
    // place in the world) but all the TileIDs will have different wrap values.
    //
    // In order to make this transition seamless, we calculate the rounded difference of
    // "worlds" between the last frame and the current frame. If the map panned by
    // a world, then we can assign all the tiles new TileIDs with updated wrap values.
    // For example, assign z/x/y/1 a new id: z/x/y/0. It is the same tile, just rendered
    // in a different position.
    //
    // This enables us to reuse the tiles at more ideal locations and prevent flickering.

    const float lngDifference = lng - prevLng;
    const float worldDifference = lngDifference / 360.f;
    const auto wrapDelta = static_cast<int16_t>(std::round(worldDifference));
    prevLng = lng;

    if (wrapDelta) {
        std::map<OverscaledTileID, std::unique_ptr<Tile>> newTiles;
        std::map<UnwrappedTileID, std::reference_wrapper<Tile>> newRenderTiles;
        for (auto& tile : tiles) {
            auto newID = tile.second->id.unwrapTo(tile.second->id.wrap + wrapDelta);
            tile.second->id = newID;
            newTiles.emplace(newID, std::move(tile.second));
        }
        tiles = std::move(newTiles);

        for (auto& tile : renderedTiles) {
            UnwrappedTileID newID = tile.first.unwrapTo(tile.first.wrap + wrapDelta);
            newRenderTiles.emplace(newID, tile.second);
        }
        renderedTiles = std::move(newRenderTiles);
    }
}

std::unordered_map<std::string, std::vector<Feature>> TilePyramid::queryRenderedFeatures(
    const ScreenLineString& geometry,
    const TransformState& transformState,
    const std::unordered_map<std::string, const RenderLayer*>& layers,
    const RenderedQueryOptions& options,
    const mat4& projMatrix,
    const SourceFeatureState& featureState) const {
    std::unordered_map<std::string, std::vector<Feature>> result;
    if (renderedTiles.empty() || geometry.empty()) {
        return result;
    }

    LineString<double> queryGeometry;
    queryGeometry.reserve(geometry.size());

    for (const auto& p : geometry) {
        queryGeometry.push_back(
            TileCoordinate::fromScreenCoordinate(transformState, 0, {p.x, transformState.getSize().height - p.y}).p);
    }

    mapbox::geometry::box<double> box = mapbox::geometry::envelope(queryGeometry);

    auto cmp = [](const UnwrappedTileID& a, const UnwrappedTileID& b) {
        return std::tie(a.canonical.z, a.canonical.y, a.wrap, a.canonical.x) <
               std::tie(b.canonical.z, b.canonical.y, b.wrap, b.canonical.x);
    };

    std::map<UnwrappedTileID, std::reference_wrapper<Tile>, decltype(cmp)> sortedTiles{
        renderedTiles.begin(), renderedTiles.end(), cmp};

    auto maxPitchScaleFactor = transformState.maxPitchScaleFactor();

    for (const auto& entry : sortedTiles) {
        const UnwrappedTileID& id = entry.first;
        Tile& tile = entry.second;

        const auto scale = static_cast<float>(transformState.getScale() /
                                              (1 << id.canonical.z)); // equivalent to std::pow(2,
                                                                      // transformState.getZoom() - id.canonical.z);
        auto queryPadding = maxPitchScaleFactor * tile.getQueryPadding(layers) * util::EXTENT / util::tileSize_D /
                            scale;

        GeometryCoordinate tileSpaceBoundsMin = TileCoordinate::toGeometryCoordinate(id, box.min);
        if (tileSpaceBoundsMin.x - queryPadding >= util::EXTENT ||
            tileSpaceBoundsMin.y - queryPadding >= util::EXTENT) {
            continue;
        }

        GeometryCoordinate tileSpaceBoundsMax = TileCoordinate::toGeometryCoordinate(id, box.max);
        if (tileSpaceBoundsMax.x + queryPadding < 0 || tileSpaceBoundsMax.y + queryPadding < 0) {
            continue;
        }

        GeometryCoordinates tileSpaceQueryGeometry;
        tileSpaceQueryGeometry.reserve(queryGeometry.size());
        for (const auto& c : queryGeometry) {
            tileSpaceQueryGeometry.push_back(TileCoordinate::toGeometryCoordinate(id, c));
        }

        tile.queryRenderedFeatures(
            result, tileSpaceQueryGeometry, transformState, layers, options, projMatrix, featureState);
    }

    return result;
}

std::vector<Feature> TilePyramid::querySourceFeatures(const SourceQueryOptions& options) const {
    std::vector<Feature> result;

    for (const auto& pair : tiles) {
        pair.second->querySourceFeatures(result, options);
    }

    return result;
}

void TilePyramid::setCacheEnabled(bool enable) {
    cacheEnabled = enable;
}

void TilePyramid::reduceMemoryUse() {
    cache.clear();
}

void TilePyramid::setObserver(TileObserver* observer_) {
    observer = observer_;
}

void TilePyramid::dumpDebugLogs() const {
    for (const auto& pair : tiles) {
        pair.second->dumpDebugLogs();
    }
}

void TilePyramid::clearAll() {
    fadingTiles = false;
    tiles.clear();
    renderedTiles.clear();
    cache.clear();
}

void TilePyramid::addRenderTile(const UnwrappedTileID& tileID, Tile& tile) {
    assert(tile.isRenderable());
    renderedTiles.emplace(tileID, tile);
}

void TilePyramid::updateFadingTiles() {
    fadingTiles = false;
    for (auto& entry : renderedTiles) {
        Tile& tile = entry.second;
        if (tile.holdForFade()) {
            fadingTiles = true;
            tile.performedFadePlacement();
        }
    }
}

} // namespace mln
