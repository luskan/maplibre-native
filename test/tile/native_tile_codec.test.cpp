#include <mln/test/util.hpp>
#include <mln/tile/native_tile_codec.hpp>
#include <mln/tile/native_geometry_tile_data.hpp>
#include <mln/util/compression.hpp>
#include <mln/style/expression/value.hpp>

#include <bit>
#include <limits>

using namespace mln;

namespace
{
NativeTileMetadata metadata()
{
  style::CustomGeometrySource::TileOptions options;
  return {CanonicalTileID(12, 2250, 1400), nativeTileConversionContract(options)};
}

NativeTilePayloadPtr sample()
{
  NativeTileBuilder builder(metadata());
  GeometryCollection geometry{{{-32768, 32767}, {0, 1}, {23, -42}}, {}};
  PropertyMap properties{
    {"rc", uint64_t(6)}, {"cat", int64_t(-3)}, {"bool", false}, {"nil", NullValue{}},
    {"text", std::string("a\0b", 3)}, {"width", 1.25}, {"zero", -0.0},
    {"array", Value::array_type{true, uint64_t(UINT64_MAX), int64_t(INT64_MIN),
       PropertyMap{{"nested", "name"}}}}};
  builder.appendFinal(FeatureType::LineString, std::move(geometry), std::move(properties), std::string("road"));
  builder.appendFinal(FeatureType::Polygon, GeometryCollection{{{0, 0}, {10, 0}, {10, 10}, {0, 0}}},
                      PropertyMap{}, uint64_t(UINT64_MAX));
  builder.appendFinal(FeatureType::Unknown, GeometryCollection{}, PropertyMap{}, NullValue{});
  return std::move(builder).seal();
}

void fixCRC(std::string& bytes)
{
  const auto crc = util::crc32(bytes.data() + 32, bytes.size() - 32);
  for(size_t i = 0; i < 4; ++i) bytes[16 + i] = static_cast<char>((crc >> (8 * i)) & 255);
}

template <class Function>
void expectLimit(Function&& function, NativeTileCodecLimit reason, size_t limit, size_t requiredAtLeast)
{
  try
  {
    function();
    FAIL() << "Expected a typed native codec limit";
  }
  catch(const NativeTileCodecLimitError& error)
  {
    EXPECT_EQ(error.reason, reason);
    EXPECT_EQ(error.limit, limit);
    EXPECT_EQ(error.requiredAtLeast, requiredAtLeast);
  }
  catch(...) { FAIL() << "Expected an eligibility limit, not an unclassified failure"; }
}
}

TEST(NativeTileCodec, PreservesFinalGeometryTypedValuesIDsAndQueries)
{
  const auto original = sample();
  const std::string descriptor("map\0settings", 12);
  const auto encoded = NativeTileCodec::encode(*original, descriptor);
  EXPECT_EQ(encoded.size(), NativeTileCodec::measure(*original, descriptor).storedBytes);
  const auto restored = NativeTileCodec::decode(encoded, descriptor, metadata());
  ASSERT_EQ(original->features().size(), restored->features().size());
  NativeGeometryTileData a(original), b(restored);
  auto al = a.getLayer(""), bl = b.getLayer("");
  for(size_t i = 0; i < original->features().size(); ++i)
  {
    auto af = al->getFeature(i), bf = bl->getFeature(i);
    EXPECT_EQ(af->getType(), bf->getType());
    EXPECT_EQ(af->getID(), bf->getID());
    EXPECT_EQ(af->getGeometries(), bf->getGeometries());
    EXPECT_EQ(af->getProperties(), bf->getProperties());
    if(af->getType() != FeatureType::Unknown)
      EXPECT_EQ(convertFeature(*af, metadata().tileID), convertFeature(*bf, metadata().tileID));
  }
  EXPECT_EQ(std::bit_cast<uint64_t>(restored->properties(restored->features()[0]).at("zero").get<double>()),
            std::bit_cast<uint64_t>(-0.0));
  EXPECT_EQ(restored->properties(restored->features()[0]).count("missing"), 0u);
}

TEST(NativeTileCodec, EmptyTileRoundTripAndIdentityRejection)
{
  NativeTileBuilder builder(metadata());
  const auto tile = std::move(builder).seal();
  const auto record = NativeTileCodec::encode(*tile, "key");
  EXPECT_TRUE(NativeTileCodec::decode(record, "key", metadata())->features().empty());
  EXPECT_THROW(NativeTileCodec::decode(record, "other", metadata()), std::runtime_error);
  auto different = metadata();
  different.tileID = CanonicalTileID(12, 2251, 1400);
  EXPECT_THROW(NativeTileCodec::decode(record, "key", different), std::runtime_error);
  different = metadata();
  different.conversion.buffer++;
  EXPECT_THROW(NativeTileCodec::decode(record, "key", different), std::runtime_error);
}

TEST(NativeTileCodec, RejectsTruncationCorruptionAndUnexpectedBytes)
{
  const auto record = NativeTileCodec::encode(*sample(), "key");
  for(size_t length = 0; length < record.size(); ++length)
    EXPECT_THROW(NativeTileCodec::decode(std::string_view(record).substr(0, length), "key", metadata()),
                 std::runtime_error);
  for(size_t i = 0; i < record.size(); ++i)
  {
    auto corrupt = record;
    corrupt[i] ^= 1;
    EXPECT_THROW(NativeTileCodec::decode(corrupt, "key", metadata()), std::runtime_error);
  }
  EXPECT_THROW(NativeTileCodec::decode(record + "x", "key", metadata()), std::runtime_error);
}

TEST(NativeTileCodec, EnforcesWireDecodedAndDescriptorBudgets)
{
  const auto tile = sample();
  const auto size = NativeTileCodec::measure(*tile, "key");
  const auto record = NativeTileCodec::encode(*tile, "key");
  NativeTileCodecLimits limits;
  limits.storedBytes = size.storedBytes;
  limits.decodedBytes = size.decodedCharge;
  EXPECT_NO_THROW(NativeTileCodec::decode(record, "key", metadata(), limits));
  limits.decodedBytes--;
  EXPECT_THROW(NativeTileCodec::decode(record, "key", metadata(), limits), std::runtime_error);
  EXPECT_THROW(NativeTileCodec::encode(*tile, "key", limits), std::runtime_error);
  limits.decodedBytes = size.decodedCharge;
  limits.storedBytes--;
  EXPECT_THROW(NativeTileCodec::decode(record, "key", metadata(), limits), std::runtime_error);
  EXPECT_THROW(NativeTileCodec::encode(*tile, "key", limits), std::runtime_error);
  limits = {};
  limits.descriptorBytes = 2;
  EXPECT_THROW(NativeTileCodec::decode(record, "key", metadata(), limits), std::runtime_error);
  EXPECT_THROW(NativeTileCodec::encode(*tile, "key", limits), std::runtime_error);
}

TEST(NativeTileCodec, ReportsExactWriterLimitThresholdsWithoutChangingReaderErrors)
{
  const auto tile = sample();
  const auto size = NativeTileCodec::measure(*tile, "key");
  NativeTileCodecLimits exact;
  exact.storedBytes = size.storedBytes;
  exact.decodedBytes = size.decodedCharge;
  exact.descriptorBytes = 3;
  exact.nesting = 4;
  EXPECT_NO_THROW(NativeTileCodec::measure(*tile, "key", exact));
  const auto bytes = NativeTileCodec::encode(*tile, "key", exact);
  EXPECT_NO_THROW(NativeTileCodec::decode(bytes, "key", metadata(), exact));
  for(unsigned i = 0; i < 4; ++i)
  {
    auto limits = exact;
    size_t threshold = 0;
    switch(i)
    {
      case 0: threshold = limits.storedBytes--; break;
      case 1: threshold = limits.decodedBytes--; break;
      case 2: threshold = limits.descriptorBytes--; break;
      case 3: threshold = limits.nesting--; break;
    }
    const auto reason = static_cast<NativeTileCodecLimit>(i);
    expectLimit([&] { NativeTileCodec::measure(*tile, "key", limits); }, reason, threshold - 1, threshold);
    expectLimit([&] { NativeTileCodec::encode(*tile, "key", limits); }, reason, threshold - 1, threshold);
    EXPECT_THROW(NativeTileCodec::decode(bytes, "key", metadata(), limits), NativeTileCodecInvalidError);
  }
}

TEST(NativeTileCodec, InvalidIdentityFramingAndDeclaredChargeAreNeverWriterLimits)
{
  const auto tile = sample();
  EXPECT_THROW(NativeTileCodec::measure(*tile, ""), NativeTileCodecInvalidError);
  EXPECT_THROW(NativeTileCodec::encode(*tile, ""), NativeTileCodecInvalidError);
  const auto bytes = NativeTileCodec::encode(*tile, "key");
  EXPECT_THROW(NativeTileCodec::decode(bytes, "other", metadata()), NativeTileCodecInvalidError);
  EXPECT_THROW(NativeTileCodec::decode(bytes.substr(0, bytes.size() - 1), "key", metadata()),
               NativeTileCodecInvalidError);
  auto corrupt = bytes;
  corrupt.back() ^= 1;
  EXPECT_THROW(NativeTileCodec::inspect(corrupt), NativeTileCodecInvalidError);
  corrupt = bytes;
  for(size_t i = 0; i < 8; ++i) corrupt[24 + i] = 0;
  corrupt[24] = 1;
  EXPECT_NO_THROW(NativeTileCodec::inspect(corrupt));
  EXPECT_THROW(NativeTileCodec::decode(corrupt, "key", metadata()), NativeTileCodecInvalidError);
  corrupt = bytes;
  const size_t featureCount = 32 + 4 + 3 + 31;
  for(size_t i = 0; i < 4; ++i) corrupt[featureCount + i] = static_cast<char>(255);
  fixCRC(corrupt);
  EXPECT_THROW(NativeTileCodec::decode(corrupt, "key", metadata()), NativeTileCodecInvalidError);
}

TEST(NativeTileCodec, RejectsForgedCountsBeforeAllocatingContainers)
{
  auto record = NativeTileCodec::encode(*sample(), "key");
  const size_t featureCount = 32 + 4 + 3 + 31;
  for(size_t i = 0; i < 4; ++i) record[featureCount + i] = static_cast<char>(255);
  fixCRC(record);
  EXPECT_THROW(NativeTileCodec::decode(record, "key", metadata()), std::runtime_error);
}

TEST(NativeTileCodec, BoundsNestedProperties)
{
  NativeTileBuilder builder(metadata());
  Value value = NullValue{};
  for(size_t i = 0; i < 20; ++i) value = Value::array_type{std::move(value)};
  builder.appendFinal(FeatureType::Unknown, GeometryCollection{}, PropertyMap{{"deep", std::move(value)}}, NullValue{});
  const auto tile = std::move(builder).seal();
  EXPECT_THROW(NativeTileCodec::encode(*tile, "key"), std::runtime_error);
  NativeTileCodecLimits limits;
  limits.nesting = 32;
  const auto bytes = NativeTileCodec::encode(*tile, "key", limits);
  EXPECT_THROW(NativeTileCodec::decode(bytes, "key", metadata()), std::runtime_error);
  EXPECT_NO_THROW(NativeTileCodec::decode(bytes, "key", metadata(), limits));
}

TEST(NativeTileCodec, ChargesEmptyRecursiveContainers)
{
  NativeTileBuilder builder(metadata());
  Value::array_type values;
  for(size_t i = 0; i < 1000; ++i)
  {
    values.emplace_back(PropertyMap{});
    values.emplace_back(Value::array_type{});
  }
  builder.appendFinal(FeatureType::Unknown, GeometryCollection{}, PropertyMap{{"values", std::move(values)}},
                      NullValue{});
  const auto tile = std::move(builder).seal();
  const auto measured = NativeTileCodec::measure(*tile, "key");
  EXPECT_GE(measured.decodedCharge, 1000 * (2 * sizeof(Value) + sizeof(PropertyMap) + sizeof(Value::array_type)));
  const auto bytes = NativeTileCodec::encode(*tile, "key");
  NativeTileCodecLimits limits;
  limits.decodedBytes = measured.decodedCharge - 1;
  EXPECT_THROW(NativeTileCodec::decode(bytes, "key", metadata(), limits), std::runtime_error);
  limits.decodedBytes++;
  const auto restored = NativeTileCodec::decode(bytes, "key", metadata(), limits);
  EXPECT_GE(measured.decodedCharge, restored->statistics().estimatedRetainedBytes
    + 1000 * (sizeof(PropertyMap) + sizeof(Value::array_type)));
}

TEST(NativeTileCodec, SharedPropertyTableRoundTripsWithoutPerFeatureMaps)
{
  NativeTileBuilder builder(metadata());
  for(size_t i = 0; i < 1000; ++i)
    builder.appendFinal(FeatureType::Point, {{{1, 2}}}, PropertyMap{{"rc", int64_t(6)}, {"cat", int64_t(2)}},
                        uint64_t(i));
  const auto tile = std::move(builder).seal();
  const auto bytes = NativeTileCodec::encode(*tile, "key");
  const auto restored = NativeTileCodec::decode(bytes, "key", metadata());
  EXPECT_EQ(bytes.substr(0, 8), "AMNTILE2");
  EXPECT_EQ(restored->propertySets().size(), 1u);
  EXPECT_EQ(restored->statistics().propertyCount, 2000u);
  EXPECT_EQ(&restored->properties(restored->features()[0]), &restored->properties(restored->features()[999]));
  EXPECT_LE(restored->cacheMemoryCharge(), NativeTileCodec::inspect(bytes).decodedCharge);
  auto old = bytes;
  old[7] = '1';
  EXPECT_THROW(NativeTileCodec::inspect(old), NativeTileCodecInvalidError);
}

TEST(NativeTileCodec, PreservesObjectOrderAcrossCollisionAndRehashHistories)
{
  for(int history = 0; history < 4; ++history)
  {
    PropertyMap values;
    values.max_load_factor(64.0f);
    values.rehash(2);
    for(int i = 0; values.size() < 30; ++i)
    {
      const auto key = "key" + std::to_string(i);
      if(values.bucket(key) == 0) values.emplace(key, int64_t(i));
    }
    if(history & 1)
    {
      const auto name = values.begin()->first;
      auto value = values.begin()->second;
      values.erase(name);
      values.emplace(name, std::move(value));
    }
    if(history & 2) { values.rehash(67); values.max_load_factor(0.1f); }
    PropertyMap empty;
    empty.reserve(100);
    PropertyMap properties{{"nested", Value::array_type{values, std::move(empty)}}, {"plain", values}};
    const auto originalString = style::expression::toString(style::expression::toExpressionValue(Value(properties)));
    std::vector<std::string> originalOrder;
    for(const auto& [name, value] : properties) originalOrder.push_back(name);
    NativeTileBuilder builder(metadata());
    builder.appendFinal(FeatureType::Unknown, {}, std::move(properties), NullValue{});
    const auto tile = std::move(builder).seal();
    const auto record = NativeTileCodec::encode(*tile, "key");
    const auto restored = NativeTileCodec::decode(record, "key", metadata());
    const auto& actual = restored->properties(restored->features()[0]);
    EXPECT_EQ(style::expression::toString(style::expression::toExpressionValue(Value(actual))), originalString);
    std::vector<std::string> actualOrder;
    for(const auto& [name, value] : actual) actualOrder.push_back(name);
    EXPECT_EQ(actualOrder, originalOrder);
  }
}

TEST(NativeTileCodec, RejectsPropertyIndexAndLayoutAttacksBeforeLargeAllocations)
{
  NativeTileBuilder builder(metadata());
  builder.appendFinal(FeatureType::Unknown, {}, {}, NullValue{});
  const auto tile = std::move(builder).seal();
  const auto original = NativeTileCodec::encode(*tile, "key");
  constexpr size_t featureCount = 32 + 4 + 3 + 31;
  constexpr size_t poolCount = featureCount + 4;
  constexpr size_t object = poolCount + 4;
  auto replace = [&](size_t offset, uint32_t value)
  {
    auto bytes = original;
    for(size_t i = 0; i < 4; ++i) bytes[offset + i] = static_cast<char>(value >> (8 * i));
    fixCRC(bytes);
    return bytes;
  };
  for(const auto offset : {poolCount, object + 4, object + 12 + 2})
    EXPECT_THROW(NativeTileCodec::decode(replace(offset, UINT32_MAX), "key", metadata()),
                 NativeTileCodecInvalidError);
  for(const auto bits : {uint32_t(0), uint32_t(0x7f800000), uint32_t(0x7fc00000), uint32_t(0xbf800000)})
    EXPECT_THROW(NativeTileCodec::decode(replace(object + 8, bits), "key", metadata()),
                 NativeTileCodecInvalidError);
  EXPECT_NO_THROW(NativeTileCodec::decode(replace(object + 8, 1), "key", metadata()));
}

TEST(NativeTileCodec, AllowsChargedDuplicateSetsButRejectsUnusedSets)
{
  NativeTileBuildOptions options;
  options.propertyIndexSlots = 0;
  NativeTileBuilder builder(metadata(), options);
  builder.appendFinal(FeatureType::Unknown, {}, {}, NullValue{});
  builder.appendFinal(FeatureType::Unknown, {}, {}, NullValue{});
  const auto tile = std::move(builder).seal();
  auto bytes = NativeTileCodec::encode(*tile, "key");
  const auto restored = NativeTileCodec::decode(bytes, "key", metadata());
  EXPECT_EQ(restored->propertySets().size(), 2u);
  constexpr size_t secondIndex = 32 + 4 + 3 + 31 + 4 + 4 + 2 * 12 + 10 + 2;
  for(size_t i = 0; i < 4; ++i) bytes[secondIndex + i] = 0;
  fixCRC(bytes);
  EXPECT_THROW(NativeTileCodec::decode(bytes, "key", metadata()), NativeTileCodecInvalidError);
}

TEST(NativeTileCodec, WideRecursivePropertiesFitTheirDeclaredAllocationAllowance)
{
  PropertyMap values;
  for(size_t i = 0; i < 1000; ++i)
    values.emplace("key" + std::to_string(i), i % 2 ? Value(Value::array_type{}) : Value(PropertyMap{}));
  NativeTileBuilder builder(metadata());
  builder.appendFinal(FeatureType::Unknown, {}, std::move(values), NullValue{});
  const auto original = std::move(builder).seal();
  const auto record = NativeTileCodec::encode(*original, "key");
  const auto info = NativeTileCodec::inspect(record);
  NativeTileCodecLimits limits;
  limits.decodedBytes = info.decodedCharge;
  const auto restored = NativeTileCodec::decode(record, "key", metadata(), limits);
  EXPECT_LE(restored->cacheMemoryCharge(), info.decodedCharge);
  EXPECT_EQ(restored->properties(restored->features()[0]).size(), 1000u);
}
