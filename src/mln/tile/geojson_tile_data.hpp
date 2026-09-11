#pragma once
#include <mln/tile/geometry_tile_data.hpp>
#include <mln/tile/geojson_geometry_memo.hpp>

namespace mln {

// Implements a simple in-memory Tile type that holds GeoJSON values. A GeoJSON
// tile can only have one layer, and it is always returned regardless of which
// layer is requested.

class GeoJSONTileFeature : public GeometryTileFeature {
public:
    const mapbox::feature::feature<int16_t>& feature;

    GeoJSONTileFeature(const mapbox::feature::feature<int16_t>& feature_,
                       const GeoJSONGeometryMemo* memo_ = nullptr, std::size_t index_ = 0)
        : feature(feature_), memo(memo_), index(index_) {}

    ~GeoJSONTileFeature() override {
        if (memo) memo->release(geometry);
    }

    FeatureType getType() const override { return apply_visitor(ToFeatureType(), feature.geometry); }

    const PropertyMap& getProperties() const override { return feature.properties; }

    FeatureIdentifier getID() const override { return feature.id; }

    const GeometryCollection& getGeometries() const override {
        if (memo) return memo->get(index, geometry);
        if (!geometry) {
            geometry = apply_visitor(ToGeometryCollection(), feature.geometry);

            // https://github.com/mapbox/geojson-vt-cpp/issues/44
            if (getType() == FeatureType::Polygon) {
                geometry = fixupPolygons(*geometry);
            }
        }

        return *geometry;
    }

    std::optional<Value> getValue(const std::string& key) const override {
        auto it = feature.properties.find(key);
        if (it != feature.properties.end()) {
            return std::optional<Value>(it->second);
        }
        return std::optional<Value>();
    }

    mutable std::optional<GeometryCollection> geometry;

private:
    const GeoJSONGeometryMemo* memo;
    const std::size_t index;
};

class GeoJSONTileLayer : public GeometryTileLayer {
public:
    GeoJSONTileLayer(std::shared_ptr<const mapbox::feature::feature_collection<int16_t>> features_,
                     std::shared_ptr<GeoJSONGeometryMemo> memo_ = nullptr)
        : features(std::move(features_)), memo(std::move(memo_)) {}

    std::size_t featureCount() const override { return features->size(); }

    std::unique_ptr<GeometryTileFeature> getFeature(std::size_t i) const override {
        return std::make_unique<GeoJSONTileFeature>((*features)[i], memo.get(), i);
    }

    std::string getName() const override { return ""; }

private:
    std::shared_ptr<const mapbox::feature::feature_collection<int16_t>> features;
    std::shared_ptr<GeoJSONGeometryMemo> memo;
};

class GeoJSONTileData : public GeometryTileData {
public:
    GeoJSONTileData(mapbox::feature::feature_collection<int16_t> features_,
                   bool memoizeGeometry = false, std::shared_ptr<GeometryMemoObserver> observer = nullptr)
        : GeoJSONTileData(std::make_shared<const mapbox::feature::feature_collection<int16_t>>(std::move(features_)),
                          memoizeGeometry, std::move(observer)) {}

    GeoJSONTileData(std::shared_ptr<const mapbox::feature::feature_collection<int16_t>> features_,
                   bool memoizeGeometry = false, std::shared_ptr<GeometryMemoObserver> observer = nullptr)
        : features(std::move(features_)),
          memo(memoizeGeometry || observer
                 ? std::make_shared<GeoJSONGeometryMemo>(features, memoizeGeometry, std::move(observer)) : nullptr) {}

    std::unique_ptr<GeometryTileData> clone() const override {
        auto copy = std::make_unique<GeoJSONTileData>(features, memo && memo->isEnabled(),
                                                     memo ? memo->getObserver() : nullptr);
        copy->trace = trace;
        return copy;
    }

    std::unique_ptr<GeometryTileLayer> getLayer(const std::string&) const override {
        return std::make_unique<GeoJSONTileLayer>(features, memo);
    }

private:
    std::shared_ptr<const mapbox::feature::feature_collection<int16_t>> features;
    std::shared_ptr<GeoJSONGeometryMemo> memo;
};

} // namespace mln
