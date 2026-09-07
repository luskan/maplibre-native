#pragma once

#include <mln/tile/geometry_tile.hpp>
#include <mln/style/sources/custom_geometry_source.hpp>
#include <mln/util/feature.hpp>
#include <mln/util/geojson.hpp>
#include <mln/actor/mailbox.hpp>

#include <cstdint>
#include <memory>

namespace mln {

class TileParameters;

namespace style {
class CustomTileLoader;
} // namespace style

class CustomGeometryTile : public GeometryTile {
public:
    using TileFeatureCollection = mapbox::feature::feature_collection<int16_t>;
    using TileFeatureCollectionPtr = std::shared_ptr<const TileFeatureCollection>;

    CustomGeometryTile(const OverscaledTileID&,
                       std::string,
                       const TileParameters&,
                       Immutable<style::CustomGeometrySource::TileOptions>,
                       ActorRef<style::CustomTileLoader> loader,
                       TileObserver* observer = nullptr);
    ~CustomGeometryTile() override;

    static TileFeatureCollectionPtr processTileData(const GeoJSON&,
                                                    const CanonicalTileID&,
                                                    const style::CustomGeometrySource::TileOptions&);
    static TileFeatureCollectionPtr processTileData(const FeatureCollection&,
                                                    const CanonicalTileID&,
                                                    const style::CustomGeometrySource::TileOptions&);

    void setTileData(const GeoJSON& geoJSON);
    void setTileData(TileFeatureCollectionPtr featureData);
    void invalidateTileData();

    void setNecessity(TileNecessity) final;

    void querySourceFeatures(std::vector<Feature>& result, const SourceQueryOptions&) override;

private:
    bool stale = true;
    std::string sourceID;
    TileNecessity necessity;
    Immutable<style::CustomGeometrySource::TileOptions> options;
    ActorRef<style::CustomTileLoader> loader;
    // Identifies this tile in the loader, so a late message from a destroyed tile
    // cannot unregister the tile that reused its tile ID.
    const std::uint64_t registrationToken;
    std::shared_ptr<Mailbox> mailbox;
    ActorRef<CustomGeometryTile> actorRef;
};

} // namespace mln
