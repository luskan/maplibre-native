#include <mln/test/util.hpp>
#include <mln/style/native_tile_composer.hpp>
#include <mln/tile/custom_geometry_tile.hpp>
#include <mln/tile/geojson_tile_data.hpp>
#include <mln/tile/native_geometry_tile_data.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <random>

using namespace mln;

namespace
{
namespace geo = mapbox::geojson;
const CanonicalTileID tile(5, 16, 16);

style::CustomGeometrySource::TileOptions options()
{
  style::CustomGeometrySource::TileOptions value;
  value.tileSize = 512;
  value.buffer = 256;
  value.tolerance = 0.375;
  return value;
}

geo::point point(double x, double y, const CanonicalTileID& id = tile)
{
  const double scale = std::ldexp(1.0, id.z);
  const double longitude = ((id.x + x / 8192.0) / scale - 0.5) * 360.0;
  const double latitude = std::atan(std::sinh(M_PI * (1.0 - 2.0 * (id.y + y / 8192.0) / scale))) *
                          180.0 / M_PI;
  return {longitude, latitude};
}

geo::line_string line(std::initializer_list<std::pair<double, double>> coordinates)
{
  geo::line_string result;
  for (const auto& [x, y] : coordinates) result.push_back(point(x, y));
  return result;
}

geo::linear_ring ring(std::initializer_list<std::pair<double, double>> coordinates)
{
  const auto points = line(coordinates);
  return {points.begin(), points.end()};
}

NativeTilePayloadPtr compose(const FeatureCollection& features, const CanonicalTileID& id = tile,
                             style::CustomGeometrySource::TileOptions spec = options())
{
  NativeGeometryComposer composer({id, nativeTileConversionContract(spec)});
  for (const auto& feature : features) composer.append(GeoJSONFeature(feature));
  return std::move(composer).seal();
}

void parity(const FeatureCollection& features, const CanonicalTileID& id = tile,
            style::CustomGeometrySource::TileOptions spec = options())
{
  auto raw = CustomGeometryTile::processTileData(features, id, spec);
  GeoJSONTileData legacy(raw);
  auto expected = legacy.getLayer("arbitrary");
  NativeGeometryTileData native(compose(features, id, spec));
  auto actual = native.getLayer("another-name");
  ASSERT_EQ(expected->featureCount(), actual->featureCount());
  for (std::size_t i = 0; i < actual->featureCount(); ++i)
  {
    SCOPED_TRACE(i);
    auto a = actual->getFeature(i);
    auto e = expected->getFeature(i);
    EXPECT_EQ(e->getType(), a->getType());
    EXPECT_EQ(e->getID(), a->getID());
    EXPECT_EQ(e->getProperties(), a->getProperties());
    EXPECT_EQ(e->getGeometries(), a->getGeometries());
    if (e->getType() != FeatureType::Unknown)
      EXPECT_EQ(convertFeature(*e, id).geometry, convertFeature(*a, id).geometry);
  }
}

FeatureCollection families()
{
  FeatureCollection features;
  features.emplace_back(point(-4096, 4096));
  features.emplace_back(geo::multi_point{point(0, 0), point(10, 20), point(10, 20)});
  features.emplace_back(line({{0, 0}, {64, 6}, {128, 0}, {128, 0}}));
  features.emplace_back(geo::multi_line_string{line({{0, 0}, {80, 40}}), {}, line({{0, 0}})});
  features.emplace_back(geo::polygon{
    ring({{0, 0}, {800, 0}, {800, 800}, {0, 800}, {0, 0}}),
    ring({{100, 100}, {100, 200}, {200, 200}, {200, 100}, {100, 100}})});
  features.emplace_back(geo::polygon{ring({{0, 0}, {100, 100}, {0, 100}, {100, 0}, {0, 0}})});
  features.emplace_back(geo::multi_polygon{
    {ring({{0, 0}, {100, 0}, {100, 100}, {0, 100}, {0, 0}})},
    {ring({{50, 0}, {150, 0}, {150, 100}, {50, 100}, {50, 0}})}});
  features.emplace_back(geo::geometry_collection{
    point(1, 2), geo::geometry_collection{
      geo::line_string{}, point(3, 4), geo::geometry_collection{geo::empty{}, point(5, 6)}}});
  features.emplace_back(geo::empty{});
  features.emplace_back(geo::multi_point{});
  features.emplace_back(geo::line_string{});
  features.emplace_back(line({{0, 0}}));
  features.emplace_back(line({{0, 0}, {0, 0}}));
  features.emplace_back(geo::polygon{});
  features.emplace_back(geo::polygon{ring({{0, 0}, {1, 1}, {0, 0}})});
  features.emplace_back(geo::multi_line_string{});
  features.emplace_back(geo::multi_polygon{geo::polygon{}, geo::polygon{geo::linear_ring{}}});
  features.emplace_back(geo::geometry_collection{});
  const std::array<FeatureIdentifier, 5> ids{NullValue{}, std::numeric_limits<uint64_t>::max(),
    std::numeric_limits<int64_t>::min(), 1.25, std::string(100, 'i')};
  for (std::size_t i = 0; i < features.size(); ++i)
  {
    features[i].id = ids[i % ids.size()];
    features[i].properties = {
      {"null", NullValue{}}, {"bool", true}, {"signed", int64_t{-17}},
      {"unsigned", std::numeric_limits<uint64_t>::max()}, {"double", 1.25},
      {"string", std::string(100, 's')},
      {"array", Value::array_type{false, int64_t{-1}, uint64_t{2}, 3.5, "text"}},
      {"object", Value::object_type{{"nested", Value::array_type{Value::object_type{{"key", "value"}}}}}},
      {"order", uint64_t{i}}};
  }
  return features;
}
}

TEST(NativeGeometryComposer, MatchesAllGeometryFamiliesPropertiesIDsAndQueries)
{
  parity(families());
}

TEST(NativeGeometryComposer, StrictToleranceAndDuplicateVertices)
{
  FeatureCollection features;
  for (const double length : {5.999, 6.0, 6.001})
    features.emplace_back(line({{0, 0}, {length, 0}}));
  const auto payload = compose(features);
  ASSERT_EQ(1u, payload->features().size());
  parity(features);
  for (const double height : {5.999, 6.0, 6.001})
  {
    features.emplace_back(line({{0, 0}, {64, height}, {128, 0}, {128, 0}}));
    features.emplace_back(geo::polygon{ring({{0, 0}, {6, 0}, {6, height}, {0, height}, {0, 0}})});
  }
  parity(features);
  auto zero = options();
  zero.tolerance = 0;
  parity(features, tile, zero);
}

TEST(NativeGeometryComposer, RepairsAfterQuantizationAndAcrossMultiPolygons)
{
  FeatureCollection features;
  features.emplace_back(geo::polygon{ring({
    {0, 0}, {80, 0}, {80, 80}, {40.4, 80}, {40.4, 10}, {39.6, 10}, {39.6, 80}, {0, 80}, {0, 0}})});
  features.emplace_back(geo::polygon{
    ring({{0, 0}, {0, 100}, {100, 100}, {100, 0}, {0, 0}}),
    ring({{20, 20}, {80, 20}, {80, 80}, {20, 80}, {20, 20}})});
  features.emplace_back(geo::multi_polygon{
    {ring({{0, 0}, {100, 0}, {100, 100}, {0, 100}, {0, 0}})},
    {ring({{50, 50}, {150, 50}, {150, 150}, {50, 150}, {50, 50}})}});
  parity(features);
}

TEST(NativeGeometryComposer, SeededDifferentialGeometry)
{
  std::mt19937 random(1701);
  std::uniform_real_distribution<double> position(-4096, 12288);
  std::uniform_real_distribution<double> delta(-30, 30);
  FeatureCollection features;
  for (unsigned i = 0; i < 200; ++i)
  {
    const auto x = position(random);
    const auto y = position(random);
    geo::line_string points;
    for (unsigned j = 0; j < 12; ++j)
      points.push_back(point(x + j * 20 + delta(random), y + delta(random)));
    features.emplace_back(points);
    geo::linear_ring polygon(points.begin(), points.end());
    polygon.push_back(polygon.front());
    features.emplace_back(geo::polygon{std::move(polygon)});
  }
  parity(features);
}

TEST(NativeGeometryComposer, RoundingAndBothCoordinateLimits)
{
  FeatureCollection features;
  for (const double x : {-32768.49, -32768.0, -0.5, 0.5, 32767.0, 32767.49})
    features.emplace_back(point(x, 0));
  auto payload = compose(features);
  ASSERT_EQ(features.size(), payload->features().size());
  const std::array<int16_t, 6> expected{-32768, -32768, -1, 1, 32767, 32767};
  for (std::size_t i = 0; i < expected.size(); ++i)
    EXPECT_EQ(expected[i], payload->features()[i].geometry[0][0].x);
  parity(features);
}

TEST(NativeGeometryComposer, RangeFailuresCloseTheWholeComposer)
{
  for (const double x : {-32768.5, 32767.5, 100000.0})
  {
    NativeGeometryComposer composer({tile, nativeTileConversionContract(options())});
    composer.append(GeoJSONFeature(point(0, 0)));
    EXPECT_THROW(composer.append(GeoJSONFeature(geo::geometry_collection{point(10, 0), point(x, 0)})),
                 std::out_of_range);
    EXPECT_THROW(std::move(composer).seal(), std::logic_error);
    EXPECT_THROW(composer.append(GeoJSONFeature(point(0, 0))), std::logic_error);
  }
  for (const double y : {-33000.0, 33000.0})
    EXPECT_THROW(compose(FeatureCollection{GeoJSONFeature(point(0, y))}), std::out_of_range);
}

TEST(NativeGeometryComposer, NonfiniteInputsFailEvenInsideDroppedGeometry)
{
  for (const double invalid : {std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::infinity(),
                               -std::numeric_limits<double>::infinity()})
  {
    for (const auto invalidPoint : {geo::point{invalid, 0}, geo::point{0, invalid}})
    {
      NativeGeometryComposer composer({tile, nativeTileConversionContract(options())});
      EXPECT_THROW(composer.append(GeoJSONFeature(geo::line_string{invalidPoint})), std::invalid_argument);
      EXPECT_THROW(std::move(composer).seal(), std::logic_error);
    }
  }
}

TEST(NativeGeometryComposer, SupportsZoom32WithoutLegacyOracle)
{
  for (const uint8_t zoom : {uint8_t{31}, uint8_t{32}})
  {
    const auto middle = static_cast<uint32_t>(uint64_t{1} << (zoom - 1));
    const CanonicalTileID id(zoom, middle, middle);
    auto payload = compose({GeoJSONFeature(point(0, 0, id)), GeoJSONFeature(point(512, 0, id))}, id);
    ASSERT_EQ(2u, payload->features().size());
    EXPECT_EQ(GeometryCoordinate(0, 0), payload->features()[0].geometry[0][0]);
    EXPECT_EQ(GeometryCoordinate(512, 0), payload->features()[1].geometry[0][0]);
    EXPECT_EQ(zoom, payload->metadata().tileID.z);
  }
}

TEST(NativeGeometryComposer, FiniteInputsWithProjectionOrOutputOverflowAreRejected)
{
  const double largest = std::numeric_limits<double>::max();
  for (const auto input : {geo::point{largest, 0}, geo::point{0, largest}})
  {
    NativeGeometryComposer composer({tile, nativeTileConversionContract(options())});
    EXPECT_THROW(composer.append(GeoJSONFeature(input)), std::out_of_range);
    EXPECT_THROW(std::move(composer).seal(), std::logic_error);
  }
}

TEST(NativeGeometryComposer, InvalidMetadataAndUnsupportedWrappingAreRejected)
{
  auto metadata = NativeTileMetadata{tile, nativeTileConversionContract(options())};
  metadata.tileID.z = 33;
  EXPECT_THROW(NativeGeometryComposer{metadata}, std::invalid_argument);
  metadata.tileID = tile;
  metadata.tileID.x = 32;
  EXPECT_THROW(NativeGeometryComposer{metadata}, std::invalid_argument);
  metadata.tileID = tile;
  metadata.conversion.wrap = true;
  EXPECT_THROW(NativeGeometryComposer{metadata}, std::invalid_argument);
  metadata.conversion.wrap = false;
  metadata.conversion.clip = true;
  EXPECT_THROW(NativeGeometryComposer{metadata}, std::invalid_argument);
}

TEST(NativeGeometryComposer, MovesFinalPropertiesAndIDAndSealsOnlyOnce)
{
  auto feature = families().front();
  feature.id = std::string(100, 'i');
  const auto* text = feature.properties.at("string").getString()->data();
  const auto* id = feature.id.get<std::string>().data();
  NativeGeometryComposer composer({tile, nativeTileConversionContract(options())});
  composer.append(std::move(feature));
  auto payload = std::move(composer).seal();
  ASSERT_EQ(1u, payload->features().size());
  EXPECT_EQ(text, payload->features()[0].properties.at("string").getString()->data());
  EXPECT_EQ(id, payload->features()[0].id.get<std::string>().data());
  EXPECT_THROW(composer.append(GeoJSONFeature(point(0, 0))), std::logic_error);
  EXPECT_THROW(std::move(composer).seal(), std::logic_error);
}

TEST(NativeGeometryComposer, SuccessfulEmptyTileIsDistinctFromFailure)
{
  auto payload = compose({});
  ASSERT_TRUE(payload);
  EXPECT_TRUE(payload->features().empty());
}
