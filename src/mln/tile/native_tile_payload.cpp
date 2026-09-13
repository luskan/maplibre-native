#include <mln/tile/native_tile_payload.hpp>
#include <mln/style/custom_tile_conversion.hpp>

#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace mln
{
namespace
{

std::size_t addBytes(std::size_t a, std::size_t b)
{
  if (b > std::numeric_limits<std::size_t>::max() - a)
  {
    throw std::length_error("Native tile statistics overflow");
  }
  return a + b;
}

std::size_t storageBytes(std::size_t count, std::size_t size)
{
  if (count > std::numeric_limits<std::size_t>::max() / size)
  {
    throw std::length_error("Native tile storage estimate overflow");
  }
  return count * size;
}

std::size_t valueBytes(const Value&);

std::size_t stringBytes(const std::string& text)
{
  const auto data = reinterpret_cast<std::uintptr_t>(text.data());
  const auto object = reinterpret_cast<std::uintptr_t>(&text);
  // Inline characters are already counted by the object that contains the string.
  return data >= object && data - object < sizeof(text) ? 0 : addBytes(text.capacity(), 1);
}

std::size_t propertyBytes(const PropertyMap& properties)
{
  auto bytes = addBytes(storageBytes(properties.size(), sizeof(PropertyMap::value_type)),
                        properties.bucket_count() > 1 ? storageBytes(properties.bucket_count(), sizeof(void*)) : 0);
  for (const auto& [key, value] : properties)
  {
    bytes = addBytes(bytes, addBytes(stringBytes(key), valueBytes(value)));
  }
  return bytes;
}

std::size_t valueBytes(const Value& value)
{
  return value.match([](const std::string& text) { return stringBytes(text); },
                     [](const Value::array_type& values)
                     {
                       auto bytes = addBytes(sizeof(values), storageBytes(values.capacity(), sizeof(Value)));
                       for (const auto& item : values)
                       {
                         bytes = addBytes(bytes, valueBytes(item));
                       }
                       return bytes;
                     },
                     [](const Value::object_type& values) { return addBytes(sizeof(values), propertyBytes(values)); },
                     [](const auto&) { return std::size_t{0}; });
}

void validateMetadata(const NativeTileMetadata& metadata)
{
  const auto& tile = metadata.tileID;
  const auto& contract = metadata.conversion;
  if (tile.z > 32 || tile.x >= (uint64_t{1} << tile.z) || tile.y >= (uint64_t{1} << tile.z) ||
      contract.extent != util::EXTENT ||
      contract.conversionRevision != NativeTileConversionContract::CurrentConversionRevision ||
      contract.normalizationRevision != NativeTileConversionContract::CurrentNormalizationRevision ||
      !std::isfinite(contract.tolerance) || contract.tolerance < 0)
  {
    throw std::invalid_argument("Unsupported native tile metadata");
  }
}

}  // namespace

double NativeTileConversionContract::normalizedTolerance(uint8_t z) const noexcept
{
  return std::ldexp(tolerance / extent, -static_cast<int>(z));
}

NativeTileConversionContract nativeTileConversionContract(const style::CustomGeometrySource::TileOptions& options)
{
  if (!options.tileSize || util::EXTENT % options.tileSize != 0 || !std::isfinite(options.tolerance) ||
      options.tolerance < 0 ||
      (util::EXTENT / options.tileSize) * uint32_t{options.buffer} > std::numeric_limits<uint16_t>::max() ||
      !std::isfinite((util::EXTENT / options.tileSize) * options.tolerance))
  {
    throw std::invalid_argument("Unsupported native tile conversion options");
  }
  const auto spec = style::customTileConversionSpec(options, 0);
  NativeTileConversionContract contract;
  contract.extent = spec.extent;
  contract.buffer = spec.buffer;
  contract.tolerance = spec.tolerance;
  contract.clip = options.clip;
  contract.wrap = options.wrap;
  return contract;
}

NativeTilePayload::NativeTilePayload(NativeTileMetadata metadata, NativeTileStatistics statistics,
                                     std::vector<NativeFeatureRecord>&& features)
    : metadata_(std::move(metadata)), statistics_(statistics), features_(std::move(features))
{
}

NativeTileBuilder::NativeTileBuilder(NativeTileMetadata metadata) : metadata_(std::move(metadata))
{
  validateMetadata(metadata_);
}

void NativeTileBuilder::checkOpen() const
{
  if (sealed_)
  {
    throw std::logic_error("Native tile builder is already sealed");
  }
}

void NativeTileBuilder::appendFinal(FeatureType type, GeometryCollection&& geometry, PropertyMap&& properties,
                                    FeatureIdentifier id)
{
  checkOpen();
  if ((type != FeatureType::Unknown && type != FeatureType::Point && type != FeatureType::LineString &&
       type != FeatureType::Polygon) ||
      (type == FeatureType::Unknown && !geometry.empty()) || !geometry.getTriangles().empty())
  {
    throw std::invalid_argument("Unsupported native feature geometry");
  }

  auto statistics = statistics_;
  statistics.featureCount = addBytes(statistics.featureCount, 1);
  ++statistics.featureTypeCounts[static_cast<std::size_t>(type)];
  statistics.partCount = addBytes(statistics.partCount, geometry.size());
  statistics.propertyCount = addBytes(statistics.propertyCount, properties.size());
  statistics.geometryBytes =
      addBytes(statistics.geometryBytes, storageBytes(geometry.capacity(), sizeof(GeometryCoordinates)));
  for (const auto& part : geometry)
  {
    statistics.coordinateCount = addBytes(statistics.coordinateCount, part.size());
    statistics.geometryBytes =
        addBytes(statistics.geometryBytes, storageBytes(part.capacity(), sizeof(GeometryCoordinate)));
  }
  statistics.featureTypeGeometryBytes[static_cast<std::size_t>(type)] += statistics.geometryBytes - statistics_.geometryBytes;
  statistics.propertyBytes = addBytes(statistics.propertyBytes, propertyBytes(properties));
  if (id.is<std::string>())
  {
    statistics.identifierBytes = addBytes(statistics.identifierBytes, stringBytes(id.get<std::string>()));
  }
  // Ring-only payloads must not retain an unused owner of precomputed triangles.
  geometry.setTriangles(nullptr, {});
  features_.push_back({type, std::move(id), std::move(properties), std::move(geometry)});
  statistics_ = statistics;
}

NativeTilePayloadPtr NativeTileBuilder::seal() &&
{
  checkOpen();
  statistics_.recordBytes = storageBytes(features_.capacity(), sizeof(NativeFeatureRecord));
  auto bytes = addBytes(sizeof(NativeTilePayload), statistics_.recordBytes);
  bytes = addBytes(bytes, statistics_.geometryBytes);
  bytes = addBytes(bytes, statistics_.propertyBytes);
  statistics_.estimatedRetainedBytes = addBytes(bytes, statistics_.identifierBytes);
  sealed_ = true;
  return NativeTilePayloadPtr(new NativeTilePayload(metadata_, statistics_, std::move(features_)));
}

}  // namespace mln
