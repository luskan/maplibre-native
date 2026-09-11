#include <mln/test/util.hpp>
#include <mln/tile/geojson_tile_data.hpp>
#include <mln/tile/tile_id.hpp>

#include <future>
#include <barrier>
#include <mln/test/geometry_memo_observer.hpp>

using namespace mln;

namespace {

std::shared_ptr<const GeoJSONGeometryMemo::Features> memoFeatures() {
  auto features = std::make_shared<GeoJSONGeometryMemo::Features>();
  features->emplace_back(mapbox::geometry::point<int16_t>{30, 50});
  features->emplace_back(mapbox::geometry::line_string<int16_t>{{0, 0}, {100, 20}, {100, 20}});
  features->emplace_back(mapbox::geometry::polygon<int16_t>{
    {{0, 0}, {100, 100}, {0, 100}, {100, 0}, {0, 0}}});
  features->emplace_back(mapbox::geometry::polygon<int16_t>{
    {{0, 0}, {100, 0}, {100, 100}, {0, 100}, {0, 0}},
    {{20, 20}, {20, 80}, {80, 80}, {80, 20}, {20, 20}}});
  features->emplace_back(mapbox::geometry::multi_point<int16_t>{{10, 20}, {20, 30}});
  features->emplace_back(mapbox::geometry::multi_line_string<int16_t>{{{0, 0}, {50, 80}}, {{0, 0}}});
  features->emplace_back(mapbox::geometry::multi_polygon<int16_t>{
    {{{0, 0}, {100, 0}, {100, 100}, {0, 0}}}});
  features->emplace_back(mapbox::geometry::empty{});
  features->emplace_back(mapbox::geometry::polygon<int16_t>{});
  for (std::size_t i = 0; i < features->size(); ++i) {
    (*features)[i].id = uint64_t(i);
    (*features)[i].properties["name"] = std::string("fixture");
    (*features)[i].properties["category"] = int64_t(i);
  }
  return features;
}

} // namespace

TEST(GeoJSONGeometryMemo, MatchesLegacyGeometryPropertiesIDsAndQueries) {
  auto raw = memoFeatures();
  GeoJSONTileData legacy(raw);
  for (bool enabled : {false, true}) {
    auto observer = std::make_shared<test::GeometryMemoTestObserver>();
    GeoJSONTileData measured(raw, enabled, observer);
    auto expected = legacy.getLayer("ignored");
    auto actual = measured.getLayer("another-layer");
    EXPECT_EQ("", actual->getName());
    ASSERT_EQ(expected->featureCount(), actual->featureCount());
    for (std::size_t i = 0; i < raw->size(); ++i) {
      auto a = expected->getFeature(i);
      auto b = actual->getFeature(i);
      EXPECT_EQ(a->getGeometries(), b->getGeometries());
      EXPECT_EQ(a->getProperties(), b->getProperties());
      EXPECT_EQ(a->getID(), b->getID());
      EXPECT_EQ(a->getType(), b->getType());
      EXPECT_EQ(a->getValue("name"), b->getValue("name"));
      EXPECT_EQ(a->getValue("missing"), b->getValue("missing"));
      // The query converter rejects unknown feature types.
      if (a->getType() != FeatureType::Unknown)
        EXPECT_EQ(convertFeature(*a, CanonicalTileID(0, 0, 0)), convertFeature(*b, CanonicalTileID(0, 0, 0)));
    }
  }
}

TEST(GeoJSONGeometryMemo, SharesEntriesAcrossLayersAndRepeatedFeatureWrappers) {
  auto raw = memoFeatures();
  auto observer = std::make_shared<test::GeometryMemoTestObserver>();
  auto memo = std::make_shared<GeoJSONGeometryMemo>(raw, true, observer);
  GeoJSONTileLayer first(raw, memo), second(raw, memo);
  for (std::size_t i = 0; i < raw->size(); ++i) {
    auto a = first.getFeature(i);
    auto b = second.getFeature(i);
    EXPECT_EQ(&a->getGeometries(), &b->getGeometries());
    EXPECT_EQ(&a->getGeometries(), &a->getGeometries());
  }
  EXPECT_EQ(raw->size(), observer->materializations.load());
}

TEST(GeoJSONGeometryMemo, BaselineRetainsOnlyFeatureLocalGeometry) {
  auto raw = memoFeatures();
  auto observer = std::make_shared<test::GeometryMemoTestObserver>();
  auto memo = std::make_shared<GeoJSONGeometryMemo>(raw, false, observer);
  GeoJSONTileLayer layer(raw, memo);
  auto a = layer.getFeature(2);
  auto b = layer.getFeature(2);
  EXPECT_EQ(&a->getGeometries(), &a->getGeometries());
  EXPECT_NE(&a->getGeometries(), &b->getGeometries());
  EXPECT_EQ(2u, observer->materializations.load());
}

TEST(GeoJSONGeometryMemo, LayersSurviveDataAndClonesHaveFreshMemoWithSharedRawFeatures) {
  auto data = std::make_unique<GeoJSONTileData>(memoFeatures(), true);
  data->trace.map = 42;
  auto layer = data->getLayer("fill");
  auto first = layer->getFeature(2);
  const auto* geometry = &first->getGeometries();
  auto clone = data->clone();
  EXPECT_EQ(42u, clone->trace.map);
  auto clonedLayer = clone->getLayer("query");
  auto second = clonedLayer->getFeature(2);
  EXPECT_EQ(&first->getProperties(), &second->getProperties());
  EXPECT_NE(geometry, &second->getGeometries());
  EXPECT_EQ(*geometry, second->getGeometries());
  data.reset();
  clone.reset();
  EXPECT_EQ(geometry, &layer->getFeature(2)->getGeometries());
  auto query = convertFeature(*second, CanonicalTileID(0, 0, 0));
  second.reset();
  clonedLayer.reset();
  EXPECT_EQ(query.properties, first->getProperties());
  first.reset();
  layer.reset();
  EXPECT_EQ(std::string("fixture"), query.properties.at("name").get<std::string>());
}

TEST(GeoJSONGeometryMemo, ConcurrentLayerAndSameFeatureReadsInitializeOnce) {
  auto raw = memoFeatures();
  auto observer = std::make_shared<test::GeometryMemoTestObserver>();
  auto memo = std::make_shared<GeoJSONGeometryMemo>(raw, true, observer);
  GeoJSONTileLayer layer(raw, memo);
  auto sharedFeature = layer.getFeature(2);
  std::barrier start(8);
  std::vector<std::future<const GeometryCollection*>> readers;
  for (int i = 0; i < 8; ++i) {
    readers.emplace_back(std::async(std::launch::async, [&, i] {
      auto feature = layer.getFeature(2);
      start.arrive_and_wait();
      const auto* value = &(i % 2 ? sharedFeature.get() : feature.get())->getGeometries();
      for (int j = 0; j < 50; ++j) EXPECT_EQ(value, &feature->getGeometries());
      return value;
    }));
  }
  const auto* expected = readers.front().get();
  for (std::size_t i = 1; i < readers.size(); ++i) EXPECT_EQ(expected, readers[i].get());
  EXPECT_EQ(1u, observer->materializations.load());
}

TEST(GeoJSONGeometryMemo, EmptyDataAndDefaultRemainLegacy) {
  GeoJSONTileData empty(GeoJSONGeometryMemo::Features{}, true);
  EXPECT_EQ(0u, empty.clone()->getLayer("")->featureCount());
  GeoJSONTileData ordinary(memoFeatures());
  auto firstLayer = ordinary.getLayer("");
  auto secondLayer = ordinary.getLayer("");
  auto first = firstLayer->getFeature(2);
  auto second = secondLayer->getFeature(2);
  EXPECT_NE(&first->getGeometries(), &second->getGeometries());
}

TEST(GeoJSONGeometryMemo, ReportsLiveGeometryAndFreshCloneTables) {
  auto observer = std::make_shared<test::GeometryMemoTestObserver>();
  auto raw = memoFeatures();
  std::weak_ptr<const GeoJSONGeometryMemo::Features> weakRaw = raw;
  {
    GeoJSONTileData data(raw, true, observer);
    const auto oneTable = observer->tableBytes.load();
    auto clone = data.clone();
    EXPECT_EQ(oneTable * 2, observer->tableBytes.load());
    auto layer = data.getLayer("");
    auto feature = layer->getFeature(2);
    feature->getGeometries();
    const auto oneGeometry = observer->geometryBytes.load();
    EXPECT_GT(oneGeometry, 0u);
    clone->getLayer("")->getFeature(2)->getGeometries();
    EXPECT_EQ(oneGeometry * 2, observer->geometryBytes.load());
    clone.reset();
    EXPECT_EQ(oneGeometry, observer->geometryBytes.load());
  }
  EXPECT_EQ(0u, observer->holders.load());
  EXPECT_EQ(0u, observer->geometryBytes.load());
  EXPECT_EQ(0u, observer->tableBytes.load());
  raw.reset();
  EXPECT_TRUE(weakRaw.expired());
}

TEST(GeoJSONGeometryMemo, ObserverSurvivesOwnersUntilLastLayerIsReleased) {
  auto observer = std::make_shared<test::GeometryMemoTestObserver>();
  std::weak_ptr<GeometryMemoObserver> weakObserver = observer;
  auto data = std::make_unique<GeoJSONTileData>(memoFeatures(), true, observer);
  auto clone = data->clone();
  auto layer = clone->getLayer("");
  observer.reset();
  data.reset();
  clone.reset();
  EXPECT_FALSE(weakObserver.expired());
  auto feature = layer->getFeature(2);
  EXPECT_FALSE(feature->getGeometries().empty());
  feature.reset();
  layer.reset();
  EXPECT_TRUE(weakObserver.expired());
}

TEST(GeoJSONGeometryMemo, MeasurementAndMemoizationAreIndependent) {
  auto observer = std::make_shared<test::GeometryMemoTestObserver>();
  {
    GeoJSONTileData baseline(memoFeatures(), false, observer);
    auto layer = baseline.getLayer("");
    auto first = layer->getFeature(2);
    auto second = layer->getFeature(2);
    first->getGeometries();
    first->getGeometries();
    second->getGeometries();
    EXPECT_EQ(3u, observer->accesses.load());
    EXPECT_EQ(2u, observer->materializations.load());
    EXPECT_EQ(2u, observer->repairs.load());
    EXPECT_EQ(2u, observer->clockSamples.load());
    EXPECT_EQ(17u, observer->lastToken.load());
    EXPECT_EQ(0u, observer->tableBytes.load());
  }
  EXPECT_EQ(0u, observer->geometryBytes.load());
  GeoJSONTileData memoWithoutMeasurements(memoFeatures(), true);
  auto layer = memoWithoutMeasurements.getLayer("");
  auto first = layer->getFeature(2);
  auto second = layer->getFeature(2);
  EXPECT_EQ(&first->getGeometries(), &second->getGeometries());
  EXPECT_EQ(3u, observer->accesses.load());
}
