#pragma once

#include <mln/tile/geometry_tile_data.hpp>
#include <array>
#include <functional>
#include <mutex>
#include <mln/tile/tile_id.hpp>
#include <mln/style/sources/custom_geometry_source.hpp>

namespace mln
{

struct NativeTileConversionContract
{
  static constexpr uint32_t CurrentConversionRevision = 1;
  static constexpr uint32_t CurrentNormalizationRevision = 1;

  uint32_t conversionRevision = CurrentConversionRevision;
  uint32_t normalizationRevision = CurrentNormalizationRevision;
  uint16_t extent = util::EXTENT;
  uint16_t buffer = 0;
  double tolerance = 0.0;
  bool clip = false;
  bool wrap = false;

  double normalizedTolerance(uint8_t z) const noexcept;
  bool operator==(const NativeTileConversionContract&) const = default;
};

NativeTileConversionContract nativeTileConversionContract(const style::CustomGeometrySource::TileOptions&);

struct NativeTileMetadata
{
  CanonicalTileID tileID;
  NativeTileConversionContract conversion;
  bool operator==(const NativeTileMetadata&) const = default;
};

struct NativeTileStatistics
{
  std::size_t featureCount = 0;
  std::array<std::size_t, 4> featureTypeCounts{};
  std::array<std::size_t, 4> featureTypeGeometryBytes{};
  std::size_t partCount = 0;
  std::size_t coordinateCount = 0;
  std::size_t propertyCount = 0;
  std::size_t propertySetCount = 0;
  std::size_t recordBytes = 0;
  std::size_t geometryBytes = 0;
  std::size_t propertyBytes = 0;
  std::size_t identifierBytes = 0;
  // Includes container and external string capacities, but not allocator or hash-node link overhead.
  // Shared-pointer control blocks and consuming wrappers are not part of this payload estimate.
  std::size_t estimatedRetainedBytes = 0;
};

struct NativeFeatureRecord
{
  FeatureType type;
  uint32_t propertySet;
  FeatureIdentifier id;
  GeometryCollection geometry;
};

class NativeTileBuilder;
class NativeTileMemoryLease;

class NativeTilePayload final
{
public:
  NativeTilePayload(const NativeTilePayload&) = delete;
  NativeTilePayload& operator=(const NativeTilePayload&) = delete;

  const NativeTileMetadata& metadata() const noexcept { return metadata_; }
  const NativeTileStatistics& statistics() const noexcept { return statistics_; }
  std::span<const NativeFeatureRecord> features() const noexcept { return features_; }
  std::span<const PropertyMap> propertySets() const noexcept { return propertySets_; }
  const PropertyMap& properties(const NativeFeatureRecord& feature) const
  {
    return propertySets_.at(feature.propertySet);
  }
  std::shared_ptr<NativeTileMemoryLease> cacheLease() const;
  bool attachCacheLease(std::shared_ptr<NativeTileMemoryLease>) const;
  std::size_t cacheMemoryCharge() const noexcept { return cacheMemoryCharge_; }

private:
  friend class NativeTileBuilder;
  NativeTilePayload(NativeTileMetadata, NativeTileStatistics, std::vector<NativeFeatureRecord>&&,
                    std::vector<PropertyMap>&&);

  // Release cache credits after the geometry and property containers are destroyed.
  mutable std::mutex cacheMutex_;
  mutable std::shared_ptr<NativeTileMemoryLease> cacheLease_;
  NativeTileMetadata metadata_;
  NativeTileStatistics statistics_;
  std::vector<NativeFeatureRecord> features_;
  std::vector<PropertyMap> propertySets_;
  std::size_t cacheMemoryCharge_ = 0;
};

using NativeTilePayloadPtr = std::shared_ptr<const NativeTilePayload>;

struct NativeTileBuildOptions
{
  size_t propertyIndexSlots = 4096;
  size_t propertyIndexProbes = 16;
  bool compact = true;
  std::function<void()> allocationGate;
};

class NativeTileBuilder
{
public:
  explicit NativeTileBuilder(NativeTileMetadata, NativeTileBuildOptions = {});
  void reserveFeatures(std::size_t count);
  NativeTileBuilder(const NativeTileBuilder&) = delete;
  NativeTileBuilder& operator=(const NativeTileBuilder&) = delete;

  // Geometry must already satisfy the declared conversion and normalization contract.
  // The caller transfers ownership and must stop using mutable aliases into the moved values.
  void appendFinal(FeatureType, GeometryCollection&&, PropertyMap&&, FeatureIdentifier);
  // Sealing consumes the builder, including when payload allocation fails.
  NativeTilePayloadPtr seal() &&;

private:
  friend class NativeTileCodec;
  struct PropertySlot
  {
    uint64_t hash = 0;
    uint32_t index = UINT32_MAX;
    uint32_t age = 0;
  };
  static_assert(sizeof(PropertySlot) == 16);
  void checkOpen() const;
  void beforeAllocation() const;
  uint32_t internProperties(PropertyMap&&);
  void reservePropertySets(size_t);
  uint32_t importProperties(PropertyMap&&);
  void appendIndexed(FeatureType, GeometryCollection&&, uint32_t, FeatureIdentifier);

  NativeTileMetadata metadata_;
  NativeTileStatistics statistics_;
  std::vector<NativeFeatureRecord> features_;
  std::vector<PropertyMap> propertySets_;
  std::vector<PropertySlot> propertyIndex_;
  NativeTileBuildOptions options_;
  uint32_t propertyClock_ = 0;
  bool sealed_ = false;
  bool failed_ = false;
};

}  // namespace mln
