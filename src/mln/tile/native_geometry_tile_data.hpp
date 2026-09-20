#pragma once

#include <mln/tile/native_tile_payload.hpp>

namespace mln
{

class NativeGeometryTileFeature final : public GeometryTileFeature
{
public:
  explicit NativeGeometryTileFeature(const NativeFeatureRecord&);
  FeatureType getType() const override;
  std::optional<Value> getValue(const std::string&) const override;
  const PropertyMap& getProperties() const override;
  FeatureIdentifier getID() const override;
  const GeometryCollection& getGeometries() const override;

private:
  const NativeFeatureRecord& feature_;
};

class NativeGeometryTileLayer final : public GeometryTileLayer
{
public:
  explicit NativeGeometryTileLayer(NativeTilePayloadPtr);
  std::size_t featureCount() const override;
  std::unique_ptr<GeometryTileFeature> getFeature(std::size_t) const override;
  std::string getName() const override;

private:
  NativeTilePayloadPtr payload_;
};

class NativeGeometryTileData final : public GeometryTileData
{
public:
  explicit NativeGeometryTileData(NativeTilePayloadPtr);
  std::unique_ptr<GeometryTileData> clone() const override;
  std::unique_ptr<GeometryTileLayer> getLayer(const std::string&) const override;
  std::unique_ptr<FeatureSelection> createFeatureSelection(
      const std::vector<const style::Filter*>&, featureselection::Statistics&, bool,
      const FeatureCandidateLimits& = {}) const override;

private:
  NativeTilePayloadPtr payload_;
};

}  // namespace mln
