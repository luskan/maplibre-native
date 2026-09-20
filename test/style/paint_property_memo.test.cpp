#include <mln/renderer/paint_property_memo.hpp>
#include <mln/renderer/paint_property_binder.hpp>
#include <mln/style/layers/line_layer_properties.hpp>
#include <mln/style/expression/dsl.hpp>
#include <mln/style/conversion/json.hpp>
#include <mln/test/stub_geometry_tile_feature.hpp>
#include <mln/test/util.hpp>
#include <mln/util/io.hpp>

#include <cstring>
#include <limits>
#include <thread>

using namespace mln;
using namespace mln::style;

namespace {

const CanonicalTileID canonical(12, 2000, 1400);

PropertyExpression<float> parse(const std::string& json, std::optional<float> fallback = {})
{
  auto expression = expression::dsl::createExpression(json.c_str());
  EXPECT_TRUE(expression);
  return PropertyExpression<float>(std::move(expression), fallback);
}

struct Session
{
  paintmemo::Statistics stats;
  paintmemo::Scope scope;
  explicit Session(paintmemo::Mode mode = paintmemo::Mode::Reuse) : scope(stats) { stats.applied = {mode, 17}; }
};

void same(float a, float b) { EXPECT_EQ(0, std::memcmp(&a, &b, sizeof(float))); }

void check(paintmemo::Cache& cache, const PropertyExpression<float>& expression, Range<float> zoom,
           float fallback, const PropertyMap& properties)
{
  StubGeometryTileFeature feature(properties);
  const auto result = cache.evaluate(feature, canonical, {});
  same(result.min, expression.evaluate(expression::EvaluationContext(zoom.min, &feature)
    .withCanonicalTileID(&canonical), fallback));
  same(result.max, expression.evaluate(expression::EvaluationContext(zoom.max, &feature)
    .withCanonicalTileID(&canonical), fallback));
}

const char* width = R"(["interpolate",["linear"],["zoom"],10,["number",["get","x"]],14,["*",["number",["get","x"]],2]])";

} // namespace

TEST(PaintMemo, NumericKeysPreserveExpressionConversionAndSignedZero)
{
  Session session(paintmemo::Mode::Verify);
  auto expression = parse(R"(["number",["get","x"]])", 9.f);
  auto cache = paintmemo::Cache::create(expression, {10, 11}, 7.f);
  ASSERT_TRUE(cache);
  const std::vector<Value> values{int64_t(1), uint64_t(1), 1., -0., 0., int64_t(9007199254740993LL),
    uint64_t(9007199254740992ULL), std::numeric_limits<uint64_t>::max(), true, NullValue{},
    std::string("1"), std::vector<Value>{1.}, PropertyMap{{"a", 1.}},
    std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()};
  for (int repeat = 0; repeat < 2; ++repeat)
  {
    check(*cache, expression, {10, 11}, 7.f, {});
    for (const auto& value : values) check(*cache, expression, {10, 11}, 7.f, {{"x", value}});
  }
  EXPECT_GT(session.stats.hits, 0u);
  EXPECT_EQ(session.stats.hits, session.stats.verifiedHits);
  EXPECT_EQ(0u, session.stats.mismatches);
  EXPECT_EQ(2 * session.stats.calls, session.stats.endpointEvaluations);
  EXPECT_GT(session.stats.bypasses, 0u);
}

TEST(PaintMemo, MissingAndNullRemainDifferentForHas)
{
  Session session;
  auto expression = parse(R"(["case",["has","x"],2,1])");
  auto cache = paintmemo::Cache::create(expression, {10, 11}, 0);
  ASSERT_TRUE(cache);
  for (int repeat = 0; repeat < 2; ++repeat)
  {
    check(*cache, expression, {10, 11}, 0, {});
    check(*cache, expression, {10, 11}, 0, {{"x", NullValue{}}});
  }
  EXPECT_EQ(2u, session.stats.misses);
  EXPECT_EQ(2u, session.stats.hits);
  EXPECT_EQ(4u, session.stats.endpointEvaluations);
}

TEST(PaintMemo, RejectsOtherContextAndUnknownOperators)
{
  Session session;
  for (const std::string json : {
    R"(["number",["id"]])", R"(["number",["feature-state","x"]])",
    R"(["number",["get",["to-string",["get","x"]]]])",
    R"(["number",["get","x",["properties"]]])",
    R"(["case",["==",["geometry-type"],"LineString"],1,["number",["get","x"]]])",
    R"(["*",["number",["get","x"]],["heatmap-density"]])",
    R"(["*",["number",["get","x"]],["line-progress"]])",
    R"(["*",["number",["get","x"]],["number",["accumulated"]]])",
    R"(["*",["number",["get","x"]],["number",["get","y"]],["number",["get","z"]]])",
    R"(["+",["number",["get","x"]],1])"})
  {
    SCOPED_TRACE(json);
    auto expression = parse(json);
    EXPECT_FALSE(paintmemo::Cache::create(expression, {10, 11}, 0));
  }
  EXPECT_EQ(0u, session.stats.admitted);
}

TEST(PaintMemo, LimitsDepthWidthAndPropertyName)
{
  Session session;
  std::string deep = R"(["number",["get","x"]])";
  for (int i = 0; i < 40; ++i) deep = "[\"*\"," + deep + ",2]";
  std::string wide = "[\"case\"";
  for (int i = 0; i < 200; ++i) wide += ",[\"==\",[\"get\",\"x\"]," + std::to_string(i) + "],1";
  wide += ",2]";
  const std::string longName = "[\"number\",[\"get\",\"" + std::string(65, 'x') + "\"]]";
  for (const auto& json : {deep, wide, longName})
  {
    auto expression = parse(json);
    EXPECT_FALSE(paintmemo::Cache::create(expression, {10, 11}, 0));
  }
  auto expression = parse("[\"number\",[\"get\",\"" + std::string(64, 'x') + "\"]]");
  auto cache = paintmemo::Cache::create(expression, {10, 11}, 0);
  ASSERT_TRUE(cache);
  for (int i = 0; i < 2; ++i) check(*cache, expression, {10, 11}, 0, {{std::string(64, 'x'), 3.}});
  EXPECT_EQ(1u, session.stats.hits);
}

TEST(PaintMemo, BoundedMemoryAndHighCardinalityFallBack)
{
  const auto before = paintmemo::liveBytes();
  Session session;
  auto expression = parse(width);
  auto cache = paintmemo::Cache::create(expression, {10, 11}, 0);
  ASSERT_TRUE(cache);
  for (int i = 0; i < 100; ++i) check(*cache, expression, {10, 11}, 0, {{"x", int64_t(i)}});
  EXPECT_EQ(1u, session.stats.capacityFallbacks);
  EXPECT_EQ(0u, session.stats.hits);
  EXPECT_EQ(200u, session.stats.endpointEvaluations);
  std::vector<std::unique_ptr<paintmemo::Cache>> retained;
  while (auto next = paintmemo::Cache::create(expression, {10, 11}, 0)) retained.push_back(std::move(next));
  EXPECT_LE(paintmemo::liveBytes(), paintmemo::MemoryLimit);
  EXPECT_EQ(1u, session.stats.budgetFallbacks);
  retained.clear(); cache.reset();
  EXPECT_EQ(before, paintmemo::liveBytes());
}

TEST(PaintMemo, ScopePolicyIsPinnedNestedAndThreadLocal)
{
  Session session;
  auto expression = parse(width);
  auto cache = paintmemo::Cache::create(expression, {10, 11}, 0);
  ASSERT_TRUE(cache);
  check(*cache, expression, {10, 11}, 0, {{"x", 3.}});
  paintmemo::configure(0);
  {
    Session nested(paintmemo::Mode::Off);
    check(*cache, expression, {10, 11}, 0, {{"x", 3.}});
    EXPECT_EQ(0u, nested.stats.hits);
    EXPECT_EQ(2u, nested.stats.endpointEvaluations);
  }
  std::thread thread([] { EXPECT_EQ(nullptr, paintmemo::current()); });
  thread.join();
  check(*cache, expression, {10, 11}, 0, {{"x", 3.}});
  EXPECT_EQ(1u, session.stats.hits);
  EXPECT_FALSE(paintmemo::configure(3));
}

TEST(PaintMemo, ExpressionDefaultsAndZoomIntervalsStayIsolated)
{
  Session session;
  auto first = parse(width, 7.f);
  auto second = parse(width, 9.f);
  auto third = parse(R"(["*",["number",["get","x"]],4])");
  auto a = paintmemo::Cache::create(first, {10, 11}, 2);
  auto b = paintmemo::Cache::create(second, {12, 13}, 3);
  auto c = paintmemo::Cache::create(third, {10, 11}, 4);
  ASSERT_TRUE(a && b && c);
  for (int repeat = 0; repeat < 2; ++repeat)
    for (const PropertyMap& properties : {PropertyMap{}, PropertyMap{{"x", 3.}}})
    {
      check(*a, first, {10, 11}, 2, properties);
      check(*b, second, {12, 13}, 3, properties);
      check(*c, third, {10, 11}, 4, properties);
    }
  EXPECT_EQ(6u, session.stats.hits);
}

TEST(PaintMemo, VerificationMismatchReturnsReferenceAndDisablesReuse)
{
  class ChangingFeature : public StubGeometryTileFeature
  {
  public:
    ChangingFeature() : StubGeometryTileFeature(PropertyMap{}) {}
    bool change = false;
    mutable size_t reads = 0;
    std::optional<Value> getValue(const std::string&) const override
    {
      return Value(change && reads++ > 0 ? 2. : 1.);
    }
  } feature;
  Session session(paintmemo::Mode::Verify);
  auto expression = parse(R"(["number",["get","x"]])");
  auto cache = paintmemo::Cache::create(expression, {10, 11}, 0);
  ASSERT_TRUE(cache);
  cache->evaluate(feature, canonical, {});
  feature.change = true;
  const auto result = cache->evaluate(feature, canonical, {});
  EXPECT_EQ(2.f, result.min);
  EXPECT_EQ(2.f, result.max);
  EXPECT_EQ(1u, session.stats.mismatches);
  EXPECT_EQ(1u, session.stats.verifiedHits);
  feature.change = false;
  cache->evaluate(feature, canonical, {});
  EXPECT_EQ(1u, session.stats.hits);
  EXPECT_EQ(6u, session.stats.endpointEvaluations);
}

TEST(PaintMemo, FactoryCachesBothWidthsAndPreservesVertexBytes)
{
  using Binders = PaintPropertyBinders<TypeList<LineWidth, LineFloorWidth, LineGapWidth>>;
  LinePaintProperties::PossiblyEvaluated properties;
  auto expression = parse(width);
  auto integer = expression;
  integer.setUseIntegerZoom(true);
  properties.get<LineWidth>() = PossiblyEvaluatedPropertyValue<float>(expression);
  properties.get<LineFloorWidth>() = PossiblyEvaluatedPropertyValue<float>(integer);
  properties.get<LineGapWidth>() = PossiblyEvaluatedPropertyValue<float>(expression);
  for (bool covering : {false, true})
  {
    std::vector<uint8_t> expected;
    std::array<float, 2> factors{};
    for (auto mode : {paintmemo::Mode::Off, paintmemo::Mode::Reuse, paintmemo::Mode::Verify})
    {
      Session session(mode);
      Binders binders(properties, 11.5f, covering);
      for (size_t index = 0; index < 8; ++index)
      {
        StubGeometryTileFeature feature(FeatureIdentifier(index), FeatureType::LineString, {}, {{"x", 3.}});
        binders.populateVertexVectors(feature, (index + 1) * 3, index, {}, {}, canonical);
      }
      const auto& data = *binders.interleavedVertexBuffer.sharedVertexVector;
      const std::vector<uint8_t> actual(data.data(), data.data() + data.bytes());
      const std::array<float, 2> interpolation{
        std::get<0>(binders.get<LineWidth>()->interpolationFactor(11.75f)),
        std::get<0>(binders.get<LineFloorWidth>()->interpolationFactor(11.75f))};
      if (mode == paintmemo::Mode::Off) { expected = actual; factors = interpolation; }
      else { EXPECT_EQ(expected, actual); EXPECT_EQ(factors, interpolation); }
      EXPECT_EQ(2u, session.stats.binders);
      EXPECT_EQ(mode == paintmemo::Mode::Off ? 0u : 2u, session.stats.admitted);
      EXPECT_EQ(mode == paintmemo::Mode::Off ? 0u : 14u, session.stats.hits);
      EXPECT_EQ(16u, session.stats.calls);
    }
  }
}

TEST(PaintMemo, FeatureStateUpdatesUseOriginalEvaluator)
{
  using Binders = PaintPropertyBinders<TypeList<LineWidth>>;
  LinePaintProperties::PossiblyEvaluated properties;
  properties.get<LineWidth>() = PossiblyEvaluatedPropertyValue<float>(parse(R"(["interpolate",["linear"],["zoom"],10,["number",["feature-state","x"],1],14,["number",["feature-state","x"],2]])"));
  Session session;
  Binders binders(properties, 10);
  StubGeometryTileFeature feature(FeatureIdentifier(uint64_t(7)), FeatureType::LineString, {}, {});
  binders.populateVertexVectors(feature, 2, 0, {}, {}, canonical);
  auto& binder = *binders.get<LineWidth>();
  binder.updateVertexVector(0, 2, feature, {{"x", 9.}});
  const auto vertex = std::get<0>(binder.getVertexValue(0));
  EXPECT_EQ(9.f, vertex.a1[0]);
  EXPECT_EQ(9.f, vertex.a1[1]);
  EXPECT_EQ(0u, session.stats.admitted);
}

TEST(PaintMemo, ActualAutoMapaWidthExpressionsMatchAcrossZoomAndCategories)
{
  JSDocument fixtures;
  fixtures.Parse(util::read_file("test/fixtures/automapa/line_widths.json").c_str());
  ASSERT_TRUE(fixtures.IsArray());
  for (const auto& fixture : fixtures.GetArray())
  {
    SCOPED_TRACE(fixture["sources"][0].GetString());
    auto expression = PropertyExpression<float>(expression::dsl::createExpression(conversion::Convertible(&fixture["expression"])));
    if (expression.isFeatureConstant()) continue;
    for (float zoom : {9.f, 10.5f, 11.5398f, 13.75f, 14.6792f, 17.f})
    {
      for (auto mode : {paintmemo::Mode::Reuse, paintmemo::Mode::Verify})
      {
        Session session(mode);
        auto cache = paintmemo::Cache::create(expression, {zoom, zoom + 1}, 1);
        ASSERT_TRUE(cache);
        for (int repeat = 0; repeat < 2; ++repeat)
          for (int category = 0; category < 13; ++category)
            for (int lanes = 0; lanes < 2; ++lanes)
              check(*cache, expression, {zoom, zoom + 1}, 1,
                    {{"cat", int64_t(category)}, {"mc", int64_t(category)}, {"ml", int64_t(lanes)}});
        EXPECT_GT(session.stats.hits, 0u);
        EXPECT_EQ(0u, session.stats.mismatches);
        EXPECT_EQ(0u, session.stats.capacityFallbacks);
      }
    }
  }
}
