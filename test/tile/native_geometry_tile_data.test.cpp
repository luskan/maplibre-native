#include <mln/test/util.hpp>
#include <mln/tile/native_geometry_tile_data.hpp>
#include <mln/tile/geojson_tile_data.hpp>
#include <mln/tile/custom_geometry_tile.hpp>
#include <mln/style/custom_tile_conversion.hpp>
#include <mln/style/expression/dsl.hpp>
#include <mln/style/filter.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <mln/util/rapidjson.hpp>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <array>
#include <barrier>
#include <future>
#include <limits>
#include <type_traits>

using namespace mln;

namespace
{

using RawFeatures = mapbox::feature::feature_collection<int16_t>;

NativeTileMetadata metadata()
{
  style::CustomGeometrySource::TileOptions options;
  options.tileSize = 512;
  options.buffer = 256;
  options.tolerance = 0.375;
  return {CanonicalTileID(5, 16, 15), nativeTileConversionContract(options)};
}

PropertyMap properties()
{
  return {{"null", NullValue{}},
          {"bool", true},
          {"signed", int64_t{-17}},
          {"unsigned", std::numeric_limits<uint64_t>::max()},
          {"double", 1.25},
          {"string", std::string(100, 's')},
          {"array", Value::array_type{NullValue{}, false, int64_t{-1}, uint64_t{2}, 3.5, "text"}},
          {"object", Value::object_type{{"nested", Value::array_type{Value::object_type{{"key", "value"}}}}}}};
}

RawFeatures rawFeatures()
{
  RawFeatures features;
  features.emplace_back(mapbox::geometry::point<int16_t>{-32768, 32767});
  features.emplace_back(mapbox::geometry::multi_point<int16_t>{{10, 20}, {20, 30}, {20, 30}});
  features.emplace_back(mapbox::geometry::line_string<int16_t>{{0, 0}, {100, 20}, {100, 20}});
  features.emplace_back(mapbox::geometry::multi_line_string<int16_t>{{{0, 0}, {50, 80}}, {{0, 0}}, {}});
  features.emplace_back(mapbox::geometry::polygon<int16_t>{{{0, 0}, {100, 100}, {0, 100}, {100, 0}, {0, 0}}});
  features.emplace_back(mapbox::geometry::polygon<int16_t>{{{0, 0}, {100, 0}, {100, 100}, {0, 100}, {0, 0}},
                                                           {{20, 20}, {20, 80}, {80, 80}, {80, 20}, {20, 20}}});
  features.emplace_back(mapbox::geometry::multi_polygon<int16_t>{{{{0, 0}, {100, 0}, {100, 100}, {0, 0}}},
                                                                 {{{200, 200}, {250, 200}, {250, 250}, {200, 200}}}});
  features.emplace_back(mapbox::geometry::polygon<int16_t>{{{0, 0}, {1, 1}, {0, 0}}});
  features.emplace_back(mapbox::geometry::empty{});
  features.emplace_back(mapbox::geometry::polygon<int16_t>{});
  features.emplace_back(mapbox::geometry::geometry_collection<int16_t>{});
  const std::array<FeatureIdentifier, 5> ids{NullValue{}, std::numeric_limits<uint64_t>::max(),
                                             std::numeric_limits<int64_t>::min(), 7.5, std::string(100, 'i')};
  for (std::size_t i = 0; i < features.size(); ++i)
  {
    features[i].properties = properties();
    features[i].properties["order"] = uint64_t{i};
    features[i].id = ids[i % ids.size()];
  }
  return features;
}

NativeTilePayloadPtr payloadFrom(const GeometryTileData& legacy, NativeTileMetadata info = metadata())
{
  NativeTileBuilder builder(std::move(info));
  auto layer = legacy.getLayer("fixture");
  for (std::size_t i = 0; i < layer->featureCount(); ++i)
  {
    auto feature = layer->getFeature(i);
    builder.appendFinal(feature->getType(), feature->getGeometries().clone(), PropertyMap(feature->getProperties()),
                        feature->getID());
  }
  return std::move(builder).seal();
}

NativeTilePayloadPtr fixturePayload()
{
  return payloadFrom(GeoJSONTileData(rawFeatures()));
}

void expectParity(const GeometryTileData& legacy, const NativeTilePayloadPtr& payload)
{
  NativeGeometryTileData data(payload);
  auto expected = legacy.getLayer("first");
  auto actual = data.getLayer("unrelated-name");
  ASSERT_EQ(expected->featureCount(), actual->featureCount());
  EXPECT_EQ(expected->getName(), actual->getName());
  for (std::size_t i = 0; i < actual->featureCount(); ++i)
  {
    auto a = expected->getFeature(i);
    auto b = actual->getFeature(i);
    EXPECT_EQ(a->getType(), b->getType());
    EXPECT_EQ(a->getID(), b->getID());
    EXPECT_EQ(a->getProperties(), b->getProperties());
    EXPECT_EQ(a->getGeometries(), b->getGeometries());
    for (const auto& [key, value] : a->getProperties())
    {
      EXPECT_EQ(std::optional<Value>(value), b->getValue(key));
    }
    EXPECT_EQ(std::nullopt, b->getValue("absent"));
    if (a->getType() != FeatureType::Unknown)
    {
      EXPECT_EQ(convertFeature(*a, payload->metadata().tileID), convertFeature(*b, payload->metadata().tileID));
    }
  }
}

}  // namespace

TEST(NativeGeometryTileData, MatchesConsumedGeometryPropertiesIDsAndQueries)
{
  GeoJSONTileData legacy(rawFeatures());
  auto payload = payloadFrom(legacy);
  expectParity(legacy, payload);
  EXPECT_EQ(metadata(), payload->metadata());
  static_assert(std::is_const_v<NativeTilePayloadPtr::element_type>);
  static_assert(std::is_same_v<decltype(payload->features()), std::span<const NativeFeatureRecord>>);
  static_assert(!std::is_copy_constructible_v<NativeTilePayload>);
  static_assert(!std::is_copy_constructible_v<NativeTileBuilder>);
}

TEST(NativeGeometryTileData, MatchesVendoredGeographicConversionOutput)
{
  FeatureCollection features;
  features.emplace_back(Point<double>{1.5, 1.5}, properties(), uint64_t{7});
  features.emplace_back(LineString<double>{{0, 0}, {1, 1}, {1, 1}, {3, 2}}, properties(), int64_t{-1});
  features.emplace_back(
      Polygon<double>{{{0, 0}, {3, 0}, {3, 3}, {0, 3}, {0, 0}}, {{1, 1}, {1, 2}, {2, 2}, {2, 1}, {1, 1}}}, properties(),
      std::string("area"));
  style::CustomGeometrySource::TileOptions options;
  options.buffer = 256;
  const auto info = metadata();
  GeoJSONTileData legacy(CustomGeometryTile::processTileData(features, info.tileID, options));
  ASSERT_GT(legacy.getLayer("")->featureCount(), 0u);
  expectParity(legacy, payloadFrom(legacy, info));
}

TEST(NativeGeometryTileData, SharesFinalGeometryAcrossLayersClonesAndAccesses)
{
  auto payload = fixturePayload();
  NativeGeometryTileData data(payload);
  data.trace.map = 11;
  data.trace.source = 22;
  data.trace.publication = 33;
  auto clone = data.clone();
  EXPECT_EQ(data.trace.map, clone->trace.map);
  EXPECT_EQ(data.trace.source, clone->trace.source);
  EXPECT_EQ(data.trace.publication, clone->trace.publication);
  auto first = data.getLayer("fill");
  auto second = clone->getLayer("query");
  EXPECT_EQ("", first->getName());
  EXPECT_EQ("", second->getName());
  for (std::size_t i = 0; i < payload->features().size(); ++i)
  {
    auto a = first->getFeature(i);
    auto b = second->getFeature(i);
    const auto& record = payload->features()[i];
    EXPECT_EQ(&record.geometry, &a->getGeometries());
    EXPECT_EQ(&record.geometry, &b->getGeometries());
    EXPECT_EQ(&record.properties, &b->getProperties());
    for (int access = 0; access < 10; ++access)
    {
      EXPECT_EQ(record.geometry.data(), a->getGeometries().data());
      for (std::size_t part = 0; part < record.geometry.size(); ++part)
      {
        EXPECT_EQ(record.geometry[part].data(), b->getGeometries()[part].data());
      }
    }
  }
}

TEST(NativeGeometryTileData, LayersSurviveDataAndHolderReleaseOrders)
{
  for (bool cacheFirst : {false, true})
  {
    auto payload = fixturePayload();
    std::weak_ptr<const NativeTilePayload> lifetime = payload;
    std::array<NativeTilePayloadPtr, 2> caches{payload, payload};
    auto data = std::make_unique<NativeGeometryTileData>(payload);
    auto clone = data->clone();
    auto anotherClone = clone->clone();
    auto layer = anotherClone->getLayer("");
    auto feature = layer->getFeature(0);
    auto query = convertFeature(*feature, payload->metadata().tileID);
    payload.reset();
    if (cacheFirst) caches = {};
    clone.reset();
    data.reset();
    anotherClone.reset();
    if (!cacheFirst) caches = {};
    ASSERT_FALSE(lifetime.expired());
    EXPECT_EQ(std::numeric_limits<uint64_t>::max(), feature->getProperties().at("unsigned").get<uint64_t>());
    EXPECT_FALSE(feature->getGeometries().empty());
    feature.reset();
    layer.reset();
    EXPECT_TRUE(lifetime.expired());
    EXPECT_EQ(std::string(100, 's'), query.properties.at("string").get<std::string>());
    EXPECT_EQ(properties().at("object"), query.properties.at("object"));
  }
}

TEST(NativeGeometryTileData, ConcurrentConstReadsShareGeometryWithoutInitialization)
{
  auto payload = fixturePayload();
  NativeGeometryTileData data(payload);
  auto layer = data.getLayer("");
  auto sharedFeature = layer->getFeature(5);
  std::barrier start(8);
  std::vector<std::future<bool>> readers;
  for (int thread = 0; thread < 8; ++thread)
  {
    readers.emplace_back(std::async(std::launch::async,
                                    [&]
                                    {
                                      auto clone = data.clone();
                                      auto localLayer = clone->getLayer("thread");
                                      auto localFeature = localLayer->getFeature(5);
                                      start.arrive_and_wait();
                                      bool valid = true;
                                      for (int i = 0; i < 100; ++i)
                                      {
                                        valid &= &sharedFeature->getGeometries() == &payload->features()[5].geometry;
                                        valid &= &localFeature->getGeometries() == &sharedFeature->getGeometries();
                                        valid &= &localFeature->getProperties() == &sharedFeature->getProperties();
                                        valid &= localFeature->getValue("object") == sharedFeature->getValue("object");
                                        valid &= localFeature->getID() == sharedFeature->getID();
                                      }
                                      return valid;
                                    }));
  }
  for (auto& reader : readers) EXPECT_TRUE(reader.get());
}

TEST(NativeGeometryTileData, EmptyPayloadIsValidAndAbsentPayloadIsRejected)
{
  NativeTileBuilder builder(metadata());
  auto payload = std::move(builder).seal();
  NativeGeometryTileData data(payload);
  EXPECT_EQ(0u, data.clone()->getLayer("anything")->featureCount());
  EXPECT_EQ("", data.getLayer("")->getName());
  EXPECT_EQ(0u, payload->statistics().featureCount);
  EXPECT_EQ(0u, payload->statistics().coordinateCount);
  EXPECT_EQ(sizeof(NativeTilePayload), payload->statistics().estimatedRetainedBytes);
  EXPECT_THROW(NativeGeometryTileData{nullptr}, std::invalid_argument);
  EXPECT_THROW(NativeGeometryTileLayer{nullptr}, std::invalid_argument);
}

TEST(NativeTilePayload, MovesGeometryPropertiesAndLongIDsIntoSealedStorage)
{
  NativeTileBuilder builder(metadata());
  GeometryCollection geometry;
  geometry.reserve(4);
  GeometryCoordinates part;
  part.reserve(20);
  part.emplace_back(1, 2);
  part.emplace_back(3, 4);
  geometry.push_back(std::move(part));
  auto values = properties();
  std::string id(100, 'i');
  const auto* parts = geometry.data();
  const auto* coordinates = geometry[0].data();
  const auto* property = &values.at("object");
  const auto* idCharacters = id.data();
  const auto geometryBytes =
      geometry.capacity() * sizeof(GeometryCoordinates) + geometry[0].capacity() * sizeof(GeometryCoordinate);
  builder.appendFinal(FeatureType::LineString, std::move(geometry), std::move(values), std::move(id));
  for (int i = 0; i < 20; ++i)
  {
    builder.appendFinal(FeatureType::Unknown, {}, {}, NullValue{});
  }
  auto payload = std::move(builder).seal();
  const auto& first = payload->features()[0];
  EXPECT_EQ(parts, first.geometry.data());
  EXPECT_EQ(coordinates, first.geometry[0].data());
  EXPECT_EQ(property, &first.properties.at("object"));
  EXPECT_EQ(idCharacters, first.id.get<std::string>().data());
  const auto& stats = payload->statistics();
  EXPECT_EQ(21u, stats.featureCount);
  EXPECT_EQ(1u, stats.partCount);
  EXPECT_EQ(2u, stats.coordinateCount);
  EXPECT_EQ(properties().size(), stats.propertyCount);
  EXPECT_EQ(geometryBytes, stats.geometryBytes);
  EXPECT_EQ(geometryBytes, stats.featureTypeGeometryBytes[static_cast<std::size_t>(FeatureType::LineString)]);
  EXPECT_EQ(0u, stats.featureTypeGeometryBytes[static_cast<std::size_t>(FeatureType::Unknown)]);
  EXPECT_EQ(0u, stats.featureTypeGeometryBytes[static_cast<std::size_t>(FeatureType::Point)]);
  EXPECT_EQ(0u, stats.featureTypeGeometryBytes[static_cast<std::size_t>(FeatureType::Polygon)]);
  EXPECT_GE(stats.recordBytes, 21u * sizeof(NativeFeatureRecord));
  EXPECT_GT(stats.propertyBytes, first.properties.size() * sizeof(PropertyMap::value_type));
  EXPECT_EQ(first.id.get<std::string>().capacity() + 1, stats.identifierBytes);
  EXPECT_EQ(
      sizeof(NativeTilePayload) + stats.recordBytes + stats.geometryBytes + stats.propertyBytes + stats.identifierBytes,
      stats.estimatedRetainedBytes);
}

TEST(NativeTilePayload, RetainsGenericFeatureTypeCounts)
{
  auto payload = fixturePayload();
  GeoJSONTileData legacy(rawFeatures());
  auto layer = legacy.getLayer("");
  std::array<std::size_t, 4> expected{};
  for (std::size_t i = 0; i < layer->featureCount(); ++i)
    ++expected[static_cast<std::size_t>(layer->getFeature(i)->getType())];
  EXPECT_EQ(expected, payload->statistics().featureTypeCounts);
}

TEST(NativeTilePayload, SealPreventsFurtherMutationOrResealing)
{
  NativeTileBuilder builder(metadata());
  builder.appendFinal(FeatureType::Point, {{{1, 2}}}, {}, uint64_t{1});
  auto payload = std::move(builder).seal();
  EXPECT_THROW(builder.appendFinal(FeatureType::Point, {{{3, 4}}}, {}, uint64_t{2}), std::logic_error);
  EXPECT_THROW(std::move(builder).seal(), std::logic_error);
  ASSERT_EQ(1u, payload->features().size());
  EXPECT_EQ(GeometryCoordinate(1, 2), payload->features()[0].geometry[0][0]);
}

TEST(NativeTilePayload, RejectsUnsupportedGeometryAndReleasesUnusedTriangleOwner)
{
  NativeTileBuilder builder(metadata());
  EXPECT_THROW(builder.appendFinal(static_cast<FeatureType>(255), {}, {}, NullValue{}), std::invalid_argument);
  EXPECT_THROW(builder.appendFinal(FeatureType::Unknown, {{{1, 2}}}, {}, NullValue{}), std::invalid_argument);
  GeometryCollection geometry{{{1, 2}}};
  std::array<uint32_t, 3> triangles{0, 1, 2};
  geometry.setTriangles(nullptr, triangles);
  EXPECT_THROW(builder.appendFinal(FeatureType::Polygon, std::move(geometry), {}, NullValue{}), std::invalid_argument);
  auto owner = std::make_shared<int>(1);
  std::weak_ptr<int> lifetime = owner;
  geometry.setTriangles(std::shared_ptr<const mlt::MapLibreTile>(owner, nullptr), {});
  owner.reset();
  builder.appendFinal(FeatureType::Point, std::move(geometry), {}, NullValue{});
  EXPECT_TRUE(lifetime.expired());
  auto payload = std::move(builder).seal();
  EXPECT_EQ(1u, payload->features().size());
  EXPECT_TRUE(payload->features()[0].geometry.getTriangles().empty());
}

TEST(NativeTilePayload, ConversionContractUsesEffectiveSourceOptions)
{
  style::CustomGeometrySource::TileOptions options;
  options.tileSize = 512;
  options.buffer = 256;
  options.tolerance = 0.375;
  const auto contract = nativeTileConversionContract(options);
  EXPECT_EQ(8192, contract.extent);
  EXPECT_EQ(4096, contract.buffer);
  EXPECT_DOUBLE_EQ(6, contract.tolerance);
  EXPECT_FALSE(contract.clip);
  EXPECT_FALSE(contract.wrap);
  for (uint8_t z : {0, 5, 12, 22, 31})
  {
    EXPECT_DOUBLE_EQ(style::customTileConversionSpec(options, z).normalizedTolerance, contract.normalizedTolerance(z));
  }
  EXPECT_DOUBLE_EQ(6.0 / (8192.0 * 4294967296.0), contract.normalizedTolerance(32));
  options.tileSize = 1024;
  options.clip = true;
  options.wrap = true;
  auto changed = nativeTileConversionContract(options);
  EXPECT_EQ(2048, changed.buffer);
  EXPECT_DOUBLE_EQ(3, changed.tolerance);
  EXPECT_TRUE(changed.clip);
  EXPECT_TRUE(changed.wrap);
  EXPECT_NE(contract, changed);
  options.memoizeGeometry = true;
  options.traceMap = 123;
  options.traceSource = 456;
  EXPECT_EQ(changed, nativeTileConversionContract(options));
}

TEST(NativeTilePayload, RejectsInvalidConversionOptionsAndMetadata)
{
  style::CustomGeometrySource::TileOptions options;
  for (uint16_t size : {0, 3, 16384})
  {
    options.tileSize = size;
    EXPECT_THROW(nativeTileConversionContract(options), std::invalid_argument);
  }
  options.tileSize = 1;
  EXPECT_THROW(nativeTileConversionContract(options), std::invalid_argument);
  options.tileSize = 512;
  for (double tolerance : {-1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
  {
    options.tolerance = tolerance;
    EXPECT_THROW(nativeTileConversionContract(options), std::invalid_argument);
  }
  for (int invalid = 0; invalid < 5; ++invalid)
  {
    auto info = metadata();
    if (invalid == 0) info.conversion.extent = 4096;
    if (invalid == 1) info.conversion.conversionRevision = 0;
    if (invalid == 2) info.conversion.normalizationRevision = 0;
    if (invalid == 3) info.conversion.tolerance = std::numeric_limits<double>::quiet_NaN();
    if (invalid == 4) info.tileID.z = 33;
    EXPECT_THROW(NativeTileBuilder{info}, std::invalid_argument);
  }
}

namespace
{
style::Filter selectionFilter(const char* text)
{
  auto expression = style::expression::dsl::createExpression(text);
  if (!expression) throw std::runtime_error("Invalid test filter");
  style::Filter filter;
  filter.expression = std::shared_ptr<const style::expression::Expression>(std::move(expression));
  return filter;
}
NativeTilePayloadPtr selectionPayload()
{
  std::vector<PropertyMap> values{
    {{"rc", int64_t{6}}}, {{"rc", uint64_t{6}}}, {{"rc", 6.0}}, {{"rc", "6"}},
    {{"rc", NullValue{}}}, {}, {{"rc", int64_t{5}}}, {{"rc", int64_t{5}}, {"cat", int64_t{3}}},
    {{"rc", int64_t{5}}, {"cat", NullValue{}}}, {{"rc", int64_t{5}}, {"cat", int64_t{10}}},
    {{"rc", uint64_t{9007199254740993ULL}}}, {{"rc", int64_t{-1}}}, {{"rc", -0.0}},
    {{"rc", std::numeric_limits<double>::infinity()}}, {{"rc", std::numeric_limits<double>::quiet_NaN()}},
    {{"rc", true}}, {{"rc", Value::array_type{int64_t{6}}}}};
  NativeTileBuilder builder(metadata());
  for (size_t i = 0; i < values.size(); ++i)
    builder.appendFinal(FeatureType::Point, {{{100, 100}}}, std::move(values[i]), uint64_t{i});
  return std::move(builder).seal();
}
std::vector<size_t> selectionMatches(const GeometryTileLayer& layer, const style::Filter& filter,
                                    const FeatureCandidates& candidates)
{
  std::vector<size_t> result;
  const auto canonical = metadata().tileID;
  for (size_t position = 0; position < candidates.size(); ++position)
  {
    const auto i = candidates[position];
    auto feature = layer.getFeature(i);
    if (filter(style::expression::EvaluationContext(5.0f, feature.get()).withCanonicalTileID(&canonical)))
      result.push_back(i);
  }
  return result;
}
}

TEST(NativeGeometryTileData, CandidateIndexPreservesOrderedMatchesAndFullQueries)
{
  auto payload = selectionPayload();
  NativeGeometryTileData data(payload);
  auto layer = data.getLayer("");
  const std::vector<const char*> expressions{
    R"(["==",["get","rc"],6])", R"(["==",6,["get","rc"]])",
    R"(["any",["==",["get","cat"],10],["all",["!",["has","cat"]],["==",["get","rc"],5]]])",
    R"(["all",["==",["get","rc"],5],["!",["has","cat"]]])",
    R"(["==",["get","rc"],9007199254740992])", R"(["==",["get","rc"],0])",
    R"(["==",["get","rc"],-1])", R"(["==",["get","rc"],999])",
    R"(["any",["==",["get","rc"],6],["has","cat"]])",
    R"(["==",["get","rc"],null])", R"(["==",["get","rc"],"6"])",
    R"(["==",["get","rc",["literal",{"rc":6}]],6])",
    R"(["all",["==",["get","rc"],6],[">",["zoom"],2]])"};
  std::vector<style::Filter> filters;
  for (const auto* text : expressions) filters.push_back(selectionFilter(text));
  std::vector<const style::Filter*> pointers;
  for (const auto& filter : filters) pointers.push_back(&filter);
  for (const auto mode : {featureselection::Mode::Indexed, featureselection::Mode::Verify})
  {
    featureselection::Statistics statistics;
    statistics.applied.mode = mode;
    auto selection = data.createFeatureSelection(pointers, statistics, false);
    ASSERT_TRUE(selection);
    const auto canonical = metadata().tileID;
    for (size_t f = 0; f < filters.size(); ++f)
    {
      const auto candidates = selection->candidates(filters[f],
        style::expression::EvaluationContext(5.0f).withCanonicalTileID(&canonical));
      if (f < 8 || f == 12) EXPECT_TRUE(candidates.indices.has_value());
      else EXPECT_FALSE(candidates.indices.has_value());
      if (candidates.indices)
      {
        EXPECT_TRUE(std::is_sorted(candidates.indices->begin(), candidates.indices->end()));
        EXPECT_EQ(candidates.indices->end(), std::adjacent_find(candidates.indices->begin(), candidates.indices->end()));
      }
      EXPECT_EQ(selectionMatches(*layer, filters[f], {layer->featureCount(), std::nullopt}),
                selectionMatches(*layer, filters[f], candidates));
    }
    EXPECT_EQ(0u, statistics.verificationFailures);
    EXPECT_GT(statistics.indexedGroups, 0u);
    if (mode == featureselection::Mode::Verify) EXPECT_EQ(statistics.indexedGroups, statistics.verifiedGroups);
    selection.reset();
    auto clone = data.clone();
    auto queried = clone->getLayer("");
    ASSERT_EQ(payload->features().size(), queried->featureCount());
    for (size_t i = 0; i < queried->featureCount(); ++i) EXPECT_EQ(uint64_t{i}, queried->getFeature(i)->getID().get<uint64_t>());
  }
}

TEST(NativeGeometryTileData, CandidateLimitsFallBackWithoutPartialResults)
{
  NativeGeometryTileData data(selectionPayload());
  auto layer = data.getLayer("");
  auto filter = selectionFilter(R"(["==",["get","rc"],6])");
  const std::vector<const style::Filter*> filters{&filter, &filter};
  for (unsigned restriction = 0; restriction < 4; ++restriction)
  {
    FeatureCandidateLimits limits;
    if (restriction == 0) limits.features = 0;
    if (restriction == 1) limits.postingBytes = 1;
    if (restriction == 2) limits.atoms = 0;
    if (restriction == 3) limits.nodes = 0;
    featureselection::Statistics statistics;
    statistics.applied.mode = featureselection::Mode::Indexed;
    auto selection = data.createFeatureSelection(filters, statistics, false, limits);
    EXPECT_FALSE(selection);
    EXPECT_GT(statistics.limitFallbacks, 0u);
  }
  FeatureCandidateLimits limits;
  limits.scratchBytes = 1;
  featureselection::Statistics statistics;
  statistics.applied.mode = featureselection::Mode::Verify;
  auto selection = data.createFeatureSelection(filters, statistics, false, limits);
  ASSERT_TRUE(selection);
  const auto result = selection->candidates(filter, style::expression::EvaluationContext(5.0f));
  EXPECT_FALSE(result.indices);
  EXPECT_EQ(layer->featureCount(), result.size());
  EXPECT_EQ(0u, statistics.indexedGroups);
  EXPECT_EQ(0u, statistics.verifiedGroups);
  EXPECT_EQ(1u, statistics.limitFallbacks);

  auto unionFilter = selectionFilter(R"(["any",["==",["get","rc"],6],["==",["get","rc"],5]])");
  statistics = {};
  statistics.applied.mode = featureselection::Mode::Verify;
  limits.scratchBytes = 16;
  selection = data.createFeatureSelection({&unionFilter, &unionFilter}, statistics, false, limits);
  ASSERT_TRUE(selection);
  EXPECT_FALSE(selection->candidates(unionFilter, style::expression::EvaluationContext(5.0f)).indices);
  EXPECT_EQ(12u, statistics.scratchCapacityBytes);
  EXPECT_EQ(1u, statistics.limitFallbacks);
  EXPECT_EQ(0u, statistics.verifiedGroups);
}

TEST(NativeGeometryTileData, CandidateAdmissionAndEmptyResultsAreDistinct)
{
  NativeGeometryTileData data(selectionPayload());
  auto absent = selectionFilter(R"(["==",["get","rc"],999])");
  auto present = selectionFilter(R"(["==",["get","rc"],6])");
  featureselection::Statistics statistics;
  statistics.applied.mode = featureselection::Mode::Indexed;
  EXPECT_FALSE(data.createFeatureSelection({&absent}, statistics, false));
  statistics = {};
  statistics.applied.mode = featureselection::Mode::Verify;
  auto selection = data.createFeatureSelection({&absent, &present}, statistics, false);
  ASSERT_TRUE(selection);
  const auto empty = selection->candidates(absent, style::expression::EvaluationContext(5.0f));
  ASSERT_TRUE(empty.indices);
  EXPECT_EQ(0u, empty.size());
  EXPECT_EQ(1u, statistics.verifiedGroups);
  EXPECT_EQ(0u, statistics.verificationFailures);
}

TEST(NativeGeometryTileData, CandidateIndexMatchesShippedStyleFilters)
{
  auto root = std::filesystem::path(__FILE__).parent_path();
  for (unsigned i = 0; i < 4; ++i) root = root.parent_path();
  root /= "fam/assets/maplibre_styles";
  ASSERT_TRUE(std::filesystem::exists(root));
  NativeTileBuilder builder(metadata());
  for (uint64_t i = 0; i < 100; ++i)
  {
    PropertyMap p;
    for (const auto* key : {"rc", "cat", "lt", "pc", "mt"})
      if (i % 7) p[key] = int64_t(i % 20);
    if (i % 3) p["nm"] = std::string("name");
    if (i % 5) p["rf"] = std::string("ref");
    builder.appendFinal(FeatureType::Point, {{{100, 100}}}, std::move(p), i);
  }
  NativeGeometryTileData data(std::move(builder).seal());
  auto layer = data.getLayer("");
  for (const auto* name : {"mvt_day.json", "mvt_night.json", "mvt_overlay_day.json", "mvt_overlay_night.json"})
  {
    std::ifstream input(root / name);
    std::stringstream contents;
    contents << input.rdbuf();
    rapidjson::Document style;
    style.Parse(contents.str().c_str());
    ASSERT_FALSE(style.HasParseError());
    std::vector<style::Filter> filters;
    for (const auto& item : style["layers"].GetArray())
    {
      if (!item.HasMember("source") || std::string(item["source"].GetString()) != "automapa-roads"
          || !item.HasMember("filter")) continue;
      rapidjson::StringBuffer buffer;
      rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
      item["filter"].Accept(writer);
      filters.push_back(selectionFilter(buffer.GetString()));
    }
    std::vector<const style::Filter*> pointers;
    for (const auto& filter : filters) pointers.push_back(&filter);
    featureselection::Statistics statistics;
    statistics.applied.mode = featureselection::Mode::Verify;
    auto selection = data.createFeatureSelection(pointers, statistics, false);
    ASSERT_TRUE(selection);
    const auto canonical = metadata().tileID;
    for (const auto& filter : filters)
    {
      const auto candidates = selection->candidates(filter,
        style::expression::EvaluationContext(5.0f).withCanonicalTileID(&canonical));
      EXPECT_EQ(selectionMatches(*layer, filter, {layer->featureCount(), std::nullopt}),
                selectionMatches(*layer, filter, candidates));
    }
    EXPECT_EQ(0u, statistics.verificationFailures);
    EXPECT_GT(statistics.verifiedGroups, 0u);
  }
}
