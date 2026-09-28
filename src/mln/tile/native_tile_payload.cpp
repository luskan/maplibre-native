#include <mln/tile/native_tile_payload.hpp>
#include <mln/style/custom_tile_conversion.hpp>
#include <mln/tile/native_tile_memory.hpp>

#include <cmath>
#include <bit>
#include <algorithm>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <type_traits>
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

std::size_t valueBytes(const Value&, std::size_t&);

std::size_t stringBytes(const std::string& text)
{
  const auto data = reinterpret_cast<std::uintptr_t>(text.data());
  const auto object = reinterpret_cast<std::uintptr_t>(&text);
  // Inline characters are already counted by the object that contains the string.
  return data >= object && data - object < sizeof(text) ? 0 : addBytes(text.capacity(), 1);
}

std::size_t stringBytes(const std::string& text, std::size_t& cacheExtra)
{
  const auto bytes = stringBytes(text);
  if(bytes) cacheExtra = addBytes(cacheExtra, 64);
  return bytes;
}

std::size_t propertyBytes(const PropertyMap& properties, std::size_t& cacheExtra)
{
  auto bytes = addBytes(storageBytes(properties.size(), sizeof(PropertyMap::value_type)),
                        properties.bucket_count() > 1 ? storageBytes(properties.bucket_count(), sizeof(void*)) : 0);
  cacheExtra = addBytes(cacheExtra, storageBytes(properties.size(), 64));
  if(properties.bucket_count() > 1) cacheExtra = addBytes(cacheExtra, 64);
  for (const auto& [key, value] : properties)
  {
    const auto keyBytes = stringBytes(key, cacheExtra);
    bytes = addBytes(bytes, addBytes(keyBytes, valueBytes(value, cacheExtra)));
  }
  return bytes;
}

std::size_t valueBytes(const Value& value, std::size_t& cacheExtra)
{
  return value.match([&](const std::string& text) { return stringBytes(text, cacheExtra); },
                     [&](const Value::array_type& values)
                     {
                       cacheExtra = addBytes(cacheExtra, 128);
                       auto bytes = addBytes(sizeof(values), storageBytes(values.capacity(), sizeof(Value)));
                       for (const auto& item : values)
                       {
                         bytes = addBytes(bytes, valueBytes(item, cacheExtra));
                       }
                       return bytes;
                     },
                     [&](const Value::object_type& values)
                     {
                       cacheExtra = addBytes(cacheExtra, 128);
                       return addBytes(sizeof(values), propertyBytes(values, cacheExtra));
                     },
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

bool sameProperties(const PropertyMap&, const PropertyMap&);

bool sameValue(const Value& a, const Value& b)
{
  return a.match([&](const auto& value)
  {
    using T = std::decay_t<decltype(value)>;
    if(!b.is<T>()) return false;
    const auto& other = b.get<T>();
    if constexpr(std::is_same_v<T, NullValue>) return true;
    else if constexpr(std::is_same_v<T, double>)
      return std::bit_cast<uint64_t>(value) == std::bit_cast<uint64_t>(other);
    else if constexpr(std::is_same_v<T, PropertyMap>) return sameProperties(value, other);
    else if constexpr(std::is_same_v<T, Value::array_type>)
    {
      if(value.size() != other.size()) return false;
      for(size_t i = 0; i < value.size(); ++i)
        if(!sameValue(value[i], other[i])) return false;
      return true;
    }
    else return value == other;
  });
}

bool sameProperties(const PropertyMap& a, const PropertyMap& b)
{
  if(a.size() != b.size()) return false;
  auto other = b.begin();
  for(const auto& [name, value] : a)
  {
    if(name != other->first || !sameValue(value, other->second)) return false;
    ++other;
  }
  return true;
}

class PropertyHash
{
public:
  std::optional<uint64_t> object(const PropertyMap& values, size_t depth = 0)
  {
    if(depth > 16) valid = false;
    integer(values.size());
    for(const auto& [name, item] : values)
    {
      if(!valid) break;
      string(name);
      value(item, depth + 1);
    }
    return valid ? std::optional<uint64_t>(hash) : std::nullopt;
  }
private:
  void bytes(std::string_view text)
  {
    if(text.size() > remaining) { valid = false; return; }
    remaining -= text.size();
    for(unsigned char c : text) { hash ^= c; hash *= 1099511628211ULL; }
  }
  void integer(uint64_t number)
  {
    char data[8];
    for(size_t i = 0; i < 8; ++i) data[i] = static_cast<char>(number >> (8 * i));
    bytes(std::string_view(data, 8));
  }
  void string(std::string_view text) { integer(text.size()); bytes(text); }
  void value(const Value& item, size_t depth)
  {
    if(depth > 16 || !valid) { valid = false; return; }
    item.match([&](const auto& v)
    {
      using T = std::decay_t<decltype(v)>;
      if constexpr(std::is_same_v<T, NullValue>) integer(0);
      else if constexpr(std::is_same_v<T, bool>) integer(v ? 2 : 1);
      else if constexpr(std::is_same_v<T, uint64_t>) { integer(3); integer(v); }
      else if constexpr(std::is_same_v<T, int64_t>) { integer(4); integer(std::bit_cast<uint64_t>(v)); }
      else if constexpr(std::is_same_v<T, double>) { integer(5); integer(std::bit_cast<uint64_t>(v)); }
      else if constexpr(std::is_same_v<T, std::string>) { integer(6); string(v); }
      else if constexpr(std::is_same_v<T, Value::array_type>)
      {
        integer(7); integer(v.size());
        for(const auto& part : v) { if(!valid) break; value(part, depth + 1); }
      }
      else if constexpr(std::is_same_v<T, PropertyMap>) { integer(8); object(v, depth + 1); }
    });
  }
  uint64_t hash = 14695981039346656037ULL;
  size_t remaining = 64 * 1024;
  bool valid = true;
};

template<class Container>
void compact(Container& values, const NativeTileBuildOptions& options)
{
  if(!options.compact || values.capacity() == values.size()) return;
  try
  {
    if(options.allocationGate) options.allocationGate();
    values.shrink_to_fit();
  }
  catch(const std::bad_alloc&) {}
}

void validateGeometry(FeatureType type, const GeometryCollection& geometry)
{
  if((type != FeatureType::Unknown && type != FeatureType::Point && type != FeatureType::LineString &&
      type != FeatureType::Polygon) || (type == FeatureType::Unknown && !geometry.empty()) ||
      !geometry.getTriangles().empty())
    throw std::invalid_argument("Unsupported native feature geometry");
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
                                     std::vector<NativeFeatureRecord>&& features, std::vector<PropertyMap>&& properties)
    : metadata_(std::move(metadata)), statistics_(statistics), features_(std::move(features)),
      propertySets_(std::move(properties))
{
}

std::shared_ptr<NativeTileMemoryLease> NativeTilePayload::cacheLease() const
{
  std::lock_guard lock(cacheMutex_);
  return cacheLease_;
}

bool NativeTilePayload::attachCacheLease(std::shared_ptr<NativeTileMemoryLease> lease) const
{
  if(!lease) return false;
  std::lock_guard lock(cacheMutex_);
  if(cacheLease_) return cacheLease_ == lease;
  if(!lease->bind(this, cacheMemoryCharge_)) return false;
  cacheLease_ = std::move(lease);
  return true;
}

NativeTileBuilder::NativeTileBuilder(NativeTileMetadata metadata, NativeTileBuildOptions options)
  : metadata_(std::move(metadata)), options_(std::move(options))
{
  validateMetadata(metadata_);
  if(options_.propertyIndexSlots > 4096 || options_.propertyIndexProbes > 16 || !options_.propertyIndexProbes)
    throw std::invalid_argument("Invalid native property index limits");
}

void NativeTileBuilder::checkOpen() const
{
  if (sealed_ || failed_)
  {
    throw std::logic_error("Native tile builder is already sealed");
  }
}

void NativeTileBuilder::reserveFeatures(std::size_t count)
{
  checkOpen();
  if(count > UINT32_MAX) throw std::length_error("Native feature count overflow");
  try
  {
    if(count > features_.capacity()) beforeAllocation();
    features_.reserve(count);
  }
  catch(...) { failed_ = true; throw; }
}

void NativeTileBuilder::beforeAllocation() const
{
  if(options_.allocationGate) options_.allocationGate();
}

void NativeTileBuilder::reservePropertySets(size_t count)
{
  checkOpen();
  if(count > UINT32_MAX) throw std::length_error("Native property set count overflow");
  try
  {
    if(count > propertySets_.capacity()) beforeAllocation();
    propertySets_.reserve(count);
  }
  catch(...) { failed_ = true; throw; }
}

uint32_t NativeTileBuilder::importProperties(PropertyMap&& values)
{
  checkOpen();
  try
  {
    if(propertySets_.size() >= UINT32_MAX) throw std::length_error("Native property set count overflow");
    const auto index = static_cast<uint32_t>(propertySets_.size());
    if(propertySets_.size() == propertySets_.capacity()) beforeAllocation();
    propertySets_.push_back(std::move(values));
    return index;
  }
  catch(...) { failed_ = true; throw; }
}

uint32_t NativeTileBuilder::internProperties(PropertyMap&& values)
{
  if(!options_.propertyIndexSlots) return importProperties(std::move(values));
  const auto hash = PropertyHash{}.object(values);
  if(!hash) return importProperties(std::move(values));
  if(propertyIndex_.empty())
  {
    beforeAllocation();
    propertyIndex_.resize(options_.propertyIndexSlots);
  }
  PropertySlot* replacement = nullptr;
  ++propertyClock_;
  for(size_t probe = 0; probe < std::min(options_.propertyIndexProbes, propertyIndex_.size()); ++probe)
  {
    auto& slot = propertyIndex_[(*hash + probe) % propertyIndex_.size()];
    if(slot.index == UINT32_MAX) { replacement = &slot; break; }
    if(slot.hash == *hash && sameProperties(values, propertySets_[slot.index]))
    {
      slot.age = propertyClock_;
      PropertyMap{}.swap(values);
      return slot.index;
    }
    if(!replacement || slot.age < replacement->age) replacement = &slot;
  }
  const auto index = importProperties(std::move(values));
  if(replacement) *replacement = {*hash, index, propertyClock_};
  return index;
}

void NativeTileBuilder::appendFinal(FeatureType type, GeometryCollection&& geometry, PropertyMap&& properties,
                                    FeatureIdentifier id)
{
  checkOpen();
  validateGeometry(type, geometry);
  try
  {
    const auto index = internProperties(std::move(properties));
    appendIndexed(type, std::move(geometry), index, std::move(id));
  }
  catch(...) { failed_ = true; throw; }
}

void NativeTileBuilder::appendIndexed(FeatureType type, GeometryCollection&& geometry, uint32_t properties,
                                      FeatureIdentifier id)
{
  checkOpen();
  validateGeometry(type, geometry);
  if(properties >= propertySets_.size()) throw std::invalid_argument("Invalid native property set index");
  try
  {
    if(features_.size() >= UINT32_MAX) throw std::length_error("Native feature count overflow");
    geometry.setTriangles(nullptr, {});
    for(auto& part : geometry) compact(part, options_);
    compact(geometry, options_);
    auto statistics = statistics_;
    // Until seal(), this private estimate accumulates cache allocation allowances only.
    auto& cacheExtra = statistics.estimatedRetainedBytes;
    statistics.featureCount = addBytes(statistics.featureCount, 1);
    ++statistics.featureTypeCounts[static_cast<std::size_t>(type)];
    statistics.partCount = addBytes(statistics.partCount, geometry.size());
    statistics.propertyCount = addBytes(statistics.propertyCount, propertySets_[properties].size());
    statistics.geometryBytes =
        addBytes(statistics.geometryBytes, storageBytes(geometry.capacity(), sizeof(GeometryCoordinates)));
    if(geometry.capacity()) cacheExtra = addBytes(cacheExtra, 64);
    for (const auto& part : geometry)
    {
      statistics.coordinateCount = addBytes(statistics.coordinateCount, part.size());
      statistics.geometryBytes =
          addBytes(statistics.geometryBytes, storageBytes(part.capacity(), sizeof(GeometryCoordinate)));
      if(part.capacity()) cacheExtra = addBytes(cacheExtra, 64);
    }
    statistics.featureTypeGeometryBytes[static_cast<std::size_t>(type)] +=
      statistics.geometryBytes - statistics_.geometryBytes;
    if (id.is<std::string>())
    {
      statistics.identifierBytes = addBytes(statistics.identifierBytes, stringBytes(id.get<std::string>(), cacheExtra));
    }
    if(features_.size() == features_.capacity()) beforeAllocation();
    features_.push_back({type, properties, std::move(id), std::move(geometry)});
    statistics_ = statistics;
  }
  catch(...) { failed_ = true; throw; }
}

NativeTilePayloadPtr NativeTileBuilder::seal() &&
{
  checkOpen();
  sealed_ = true;
  std::vector<PropertySlot>{}.swap(propertyIndex_);
  compact(features_, options_);
  compact(propertySets_, options_);
  statistics_.propertySetCount = propertySets_.size();
  statistics_.propertyBytes = storageBytes(propertySets_.capacity(), sizeof(PropertyMap));
  for(const auto& values : propertySets_)
    statistics_.propertyBytes = addBytes(statistics_.propertyBytes,
                                        propertyBytes(values, statistics_.estimatedRetainedBytes));
  statistics_.recordBytes = storageBytes(features_.capacity(), sizeof(NativeFeatureRecord));
  auto bytes = addBytes(sizeof(NativeTilePayload), statistics_.recordBytes);
  bytes = addBytes(bytes, statistics_.geometryBytes);
  bytes = addBytes(bytes, statistics_.propertyBytes);
  bytes = addBytes(bytes, statistics_.identifierBytes);
  const auto cacheCharge = addBytes(addBytes(bytes, statistics_.estimatedRetainedBytes),
                                    128 + (propertySets_.capacity() ? 64 : 0));
  statistics_.estimatedRetainedBytes = bytes;
  beforeAllocation();
  auto* payload = new NativeTilePayload(metadata_, statistics_, std::move(features_), std::move(propertySets_));
  payload->cacheMemoryCharge_ = cacheCharge;
  return NativeTilePayloadPtr(payload);
}

}  // namespace mln
