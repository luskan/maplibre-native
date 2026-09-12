#pragma once

#include <mln/tile/geometry_tile_data.hpp>
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
  std::size_t partCount = 0;
  std::size_t coordinateCount = 0;
  std::size_t propertyCount = 0;
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
  FeatureIdentifier id;
  PropertyMap properties;
  GeometryCollection geometry;
};

class NativeTileBuilder;

class NativeTilePayload final
{
public:
  NativeTilePayload(const NativeTilePayload&) = delete;
  NativeTilePayload& operator=(const NativeTilePayload&) = delete;

  const NativeTileMetadata& metadata() const noexcept { return metadata_; }
  const NativeTileStatistics& statistics() const noexcept { return statistics_; }
  std::span<const NativeFeatureRecord> features() const noexcept { return features_; }

private:
  friend class NativeTileBuilder;
  NativeTilePayload(NativeTileMetadata, NativeTileStatistics, std::vector<NativeFeatureRecord>&&);

  NativeTileMetadata metadata_;
  NativeTileStatistics statistics_;
  std::vector<NativeFeatureRecord> features_;
};

using NativeTilePayloadPtr = std::shared_ptr<const NativeTilePayload>;

class NativeTileBuilder
{
public:
  explicit NativeTileBuilder(NativeTileMetadata);
  NativeTileBuilder(const NativeTileBuilder&) = delete;
  NativeTileBuilder& operator=(const NativeTileBuilder&) = delete;

  // Geometry must already satisfy the declared conversion and normalization contract.
  // The caller transfers ownership and must stop using mutable aliases into the moved values.
  void appendFinal(FeatureType, GeometryCollection&&, PropertyMap&&, FeatureIdentifier);
  // Sealing consumes the builder, including when payload allocation fails.
  NativeTilePayloadPtr seal() &&;

private:
  void checkOpen() const;

  NativeTileMetadata metadata_;
  NativeTileStatistics statistics_;
  std::vector<NativeFeatureRecord> features_;
  bool sealed_ = false;
};

}  // namespace mln
