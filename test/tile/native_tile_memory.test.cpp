#include <mln/test/util.hpp>
#include <mln/tile/native_tile_memory.hpp>
#include <mln/tile/native_tile_codec.hpp>

using namespace mln;

TEST(NativeTileMemory, CreditsFollowSharedOwnershipAndSplits)
{
  NativeTileMemoryBudget budget(100);
  auto lease = budget.reserve(90);
  ASSERT_TRUE(lease);
  EXPECT_FALSE(budget.reserve(11));
  auto child = lease->split(40);
  EXPECT_EQ(budget.snapshot().used, 90u);
  auto otherOwner = child;
  lease->shrink(10);
  EXPECT_EQ(budget.snapshot().used, 50u);
  child.reset();
  EXPECT_EQ(budget.snapshot().used, 50u);
  otherOwner.reset();
  EXPECT_EQ(budget.snapshot().used, 10u);
  EXPECT_THROW(lease->split(11), std::logic_error);
  EXPECT_THROW(lease->shrink(11), std::logic_error);
  lease.reset();
  EXPECT_EQ(budget.snapshot().used, 0u);
  EXPECT_EQ(budget.snapshot().peak, 90u);
}

TEST(NativeTileMemory, PayloadRetainsCreditsUntilItsLastConsumerReleasesIt)
{
  NativeTileMetadata meta {CanonicalTileID(0, 0, 0),
    nativeTileConversionContract(style::CustomGeometrySource::TileOptions{})};
  NativeTileBuilder builder(meta);
  builder.appendFinal(FeatureType::LineString, GeometryCollection{{{0, 0}, {100, 100}}},
    PropertyMap{{"rc", uint64_t(6)}}, NullValue{});
  auto payload = std::move(builder).seal();
  const auto charge = payload->cacheMemoryCharge();
  NativeTileMemoryBudget budget(charge);
  auto lease = budget.reserve(charge);
  ASSERT_TRUE(payload->attachCacheLease(lease));
  auto consumer = payload;
  payload.reset();
  lease.reset();
  EXPECT_EQ(budget.snapshot().used, charge);
  EXPECT_FALSE(budget.reserve(1));
  consumer.reset();
  EXPECT_EQ(budget.snapshot().used, 0u);
}

TEST(NativeTileMemory, DecodedAllowanceCoversConservativePayloadCharge)
{
  NativeTileMetadata meta {CanonicalTileID(0, 0, 0),
    nativeTileConversionContract(style::CustomGeometrySource::TileOptions{})};
  NativeTileBuilder builder(meta);
  GeometryCollection parts;
  for(size_t i = 0; i < 200; ++i) parts.push_back(GeometryCoordinates{{0, 0}, {1, 1}});
  builder.appendFinal(FeatureType::LineString, std::move(parts),
    PropertyMap{{"values", Value::array_type{PropertyMap{}, Value::array_type{}, std::string(100, 'x')}}}, NullValue{});
  const auto original = std::move(builder).seal();
  const auto bytes = NativeTileCodec::encode(*original, "key");
  const auto info = NativeTileCodec::inspect(bytes);
  NativeTileMemoryBudget budget(info.decodedCharge);
  auto reservation = budget.reserve(info.decodedCharge);
  auto decoded = NativeTileCodec::decode(bytes, "key", meta);
  EXPECT_LE(decoded->cacheMemoryCharge(), info.decodedCharge);
  reservation->shrink(decoded->cacheMemoryCharge());
  ASSERT_TRUE(decoded->attachCacheLease(reservation));
  EXPECT_EQ(budget.snapshot().used, decoded->cacheMemoryCharge());
}

TEST(NativeTileMemory, BoundCreditsCannotBeReducedOrSharedByDifferentAllocations)
{
  NativeTileMetadata meta {CanonicalTileID(0, 0, 0),
    nativeTileConversionContract(style::CustomGeometrySource::TileOptions{})};
  NativeTileBuilder a(meta), b(meta);
  auto first = std::move(a).seal(), second = std::move(b).seal();
  NativeTileMemoryBudget budget(first->cacheMemoryCharge());
  auto lease = budget.reserve(first->cacheMemoryCharge());
  ASSERT_TRUE(first->attachCacheLease(lease));
  EXPECT_TRUE(first->attachCacheLease(lease));
  EXPECT_FALSE(second->attachCacheLease(lease));
  EXPECT_THROW(lease->shrink(0), std::logic_error);
  EXPECT_THROW(lease->split(1), std::logic_error);
  first.reset();
  EXPECT_FALSE(second->attachCacheLease(lease));
  EXPECT_GT(budget.snapshot().used, 0u);
  lease.reset();
  EXPECT_EQ(budget.snapshot().used, 0u);
}

TEST(NativeTileMemory, BuilderCompactsGeometryAndPreservesNestedAllocationAllowances)
{
  NativeTileMetadata meta {CanonicalTileID(0, 0, 0),
    nativeTileConversionContract(style::CustomGeometrySource::TileOptions{})};
  NativeTileBuilder builder(meta);
  builder.reserveFeatures(16);
  GeometryCollection geometry;
  geometry.reserve(7);
  for(size_t i = 0; i < 2; ++i)
  {
    GeometryCoordinates part;
    part.reserve(32);
    if(!i) part.emplace_back(10, 20);
    geometry.push_back(std::move(part));
  }
  PropertyMap nested;
  nested.reserve(32);
  nested.emplace(std::string(80, 'k'), std::string(100, 'v'));
  Value::array_type values;
  values.reserve(8);
  values.push_back(std::string(80, 's'));
  values.push_back(std::move(nested));
  values.push_back(Value::array_type{});
  PropertyMap properties;
  properties.reserve(32);
  properties.emplace("list", std::move(values));
  properties.emplace("plain", "short");
  builder.appendFinal(FeatureType::LineString, std::move(geometry), std::move(properties), std::string(80, 'i'));
  builder.appendFinal(FeatureType::Unknown, GeometryCollection{}, PropertyMap{}, NullValue{});
  const auto payload = std::move(builder).seal();
  const auto& first = payload->features().front();
  const auto& array = payload->properties(first).at("list").get<Value::array_type>();
  EXPECT_EQ(first.geometry.capacity(), 2u);
  EXPECT_EQ(first.geometry[1].capacity(), 0u);
  EXPECT_GE(array.capacity(), 8u);
  const auto& statistics = payload->statistics();
  EXPECT_EQ(statistics.recordBytes, 2 * sizeof(NativeFeatureRecord));
  EXPECT_EQ(statistics.estimatedRetainedBytes, sizeof(NativeTilePayload) + statistics.recordBytes
    + statistics.geometryBytes + statistics.propertyBytes + statistics.identifierBytes);
  const size_t bucketArrays = (payload->properties(first).bucket_count() > 1)
    + (array[1].get<PropertyMap>().bucket_count() > 1)
    + (payload->properties(payload->features()[1]).bucket_count() > 1);
  // Two geometry buffers, three property nodes, four external strings and three recursive wrappers.
  const size_t cacheExtra = 192 + (2 + 3 + 4 + bucketArrays) * 64 + 3 * 128;
  EXPECT_EQ(payload->cacheMemoryCharge(), statistics.estimatedRetainedBytes + cacheExtra);
}
