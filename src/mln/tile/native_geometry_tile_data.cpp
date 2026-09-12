#include <mln/tile/native_geometry_tile_data.hpp>

#include <cassert>
#include <stdexcept>
#include <utility>

namespace mln
{

NativeGeometryTileFeature::NativeGeometryTileFeature(const NativeFeatureRecord& feature) : feature_(feature) {}

FeatureType NativeGeometryTileFeature::getType() const
{
  return feature_.type;
}

std::optional<Value> NativeGeometryTileFeature::getValue(const std::string& key) const
{
  const auto found = feature_.properties.find(key);
  return found != feature_.properties.end() ? std::optional<Value>(found->second) : std::nullopt;
}

const PropertyMap& NativeGeometryTileFeature::getProperties() const
{
  return feature_.properties;
}

FeatureIdentifier NativeGeometryTileFeature::getID() const
{
  return feature_.id;
}

const GeometryCollection& NativeGeometryTileFeature::getGeometries() const
{
  return feature_.geometry;
}

NativeGeometryTileLayer::NativeGeometryTileLayer(NativeTilePayloadPtr payload) : payload_(std::move(payload))
{
  if (!payload_)
  {
    throw std::invalid_argument("Native geometry layer requires a payload");
  }
}

std::size_t NativeGeometryTileLayer::featureCount() const
{
  return payload_->features().size();
}

std::unique_ptr<GeometryTileFeature> NativeGeometryTileLayer::getFeature(std::size_t i) const
{
  assert(i < featureCount());
  return std::make_unique<NativeGeometryTileFeature>(payload_->features()[i]);
}

std::string NativeGeometryTileLayer::getName() const
{
  return "";
}

NativeGeometryTileData::NativeGeometryTileData(NativeTilePayloadPtr payload) : payload_(std::move(payload))
{
  if (!payload_)
  {
    throw std::invalid_argument("Native geometry data requires a payload");
  }
}

std::unique_ptr<GeometryTileData> NativeGeometryTileData::clone() const
{
  auto copy = std::make_unique<NativeGeometryTileData>(payload_);
  copy->trace = trace;
  return copy;
}

std::unique_ptr<GeometryTileLayer> NativeGeometryTileData::getLayer(const std::string&) const
{
  return std::make_unique<NativeGeometryTileLayer>(payload_);
}

}  // namespace mln
