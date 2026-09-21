#include <mln/renderer/color_property_memo.hpp>
#include <mln/renderer/paint_property_binder.hpp>
#include <mln/style/layers/line_layer_properties.hpp>
#include <mln/style/layers/fill_layer_properties.hpp>
#include <mln/style/conversion/property_value.hpp>
#include <mln/style/conversion/json.hpp>
#include <mln/style/expression/dsl.hpp>
#include <mln/style/expression/literal.hpp>
#include <mln/test/stub_geometry_tile_feature.hpp>
#include <mln/test/util.hpp>
#include <mln/util/io.hpp>
#include <mln/util/scoped.hpp>

#include <cstring>
#include <cmath>
#include <limits>
#include <thread>

using namespace mln;
using namespace mln::style;

namespace {

const CanonicalTileID canonical(12, 2000, 1400);
const char* colorExpression = R"COLOR(["match",["get","x"],1,"rgba(200,100,30,0.7)",2,"#ffffff","#123456"] )COLOR";

PropertyExpression<Color> parseColor(const std::string& json)
{
  conversion::Error error;
  auto value = conversion::convertJSON<PropertyValue<Color>>(json, error, true, false);
  EXPECT_TRUE(value && value->isExpression()) << error.message;
  if (!value || !value->isExpression()) throw std::runtime_error("Expected a color expression");
  return value->asExpression();
}

struct Session
{
  colormemo::Statistics stats;
  colormemo::Scope scope;
  explicit Session(colormemo::Mode mode = colormemo::Mode::Reuse) : scope(mode == colormemo::Mode::Off ? nullptr : &stats) { stats.applied = {mode, 17}; }
};

void same(Color a, Color b)
{
  EXPECT_EQ(0, std::memcmp(&a.r, &b.r, sizeof(float)));
  EXPECT_EQ(0, std::memcmp(&a.g, &b.g, sizeof(float)));
  EXPECT_EQ(0, std::memcmp(&a.b, &b.b, sizeof(float)));
  EXPECT_EQ(0, std::memcmp(&a.a, &b.a, sizeof(float)));
  EXPECT_EQ(attributeValue(a), attributeValue(b));
}

void check(colormemo::Cache& cache, const PropertyExpression<Color>& expression, const PropertyMap& properties,
           Color fallback = Color::black())
{
  StubGeometryTileFeature feature(properties);
  same(cache.evaluate(feature, canonical, {}), expression.evaluate(
    expression::EvaluationContext(&feature).withCanonicalTileID(&canonical), fallback));
}

std::vector<uint8_t> bytes(const InterleavedVertexBuffer& buffer)
{
  const auto& data = *buffer.sharedVertexVector;
  return {data.data(), data.data() + data.bytes()};
}

} // namespace

TEST(ColorMemo, RealStylesPreservePackedVertexBytesAndVerifyEveryHit)
{
  JSDocument fixtures;
  fixtures.Parse(util::read_file("test/fixtures/automapa/line_colors.json").c_str());
  ASSERT_TRUE(fixtures.IsArray());
  ASSERT_EQ(20u, fixtures.Size());
  for (const auto& fixture : fixtures.GetArray())
  {
    SCOPED_TRACE(fixture["sources"][0].GetString());
    conversion::Error error;
    auto value = conversion::convert<PropertyValue<Color>>(conversion::Convertible(&fixture["expression"]), error, true, false);
    ASSERT_TRUE(value && value->isExpression()) << error.message;
    auto expression = value->asExpression();
    std::vector<uint8_t> expected;
    for (auto mode : {colormemo::Mode::Off, colormemo::Mode::Reuse, colormemo::Mode::Verify})
    {
      Session session(mode);
      LinePaintProperties::PossiblyEvaluated properties;
      properties.get<LineColor>() = PossiblyEvaluatedPropertyValue<Color>(expression);
      PaintPropertyBinders<TypeList<LineColor>> binders(properties, 12);
      size_t index = 0;
      for (int repeat = 0; repeat < 3; ++repeat)
        for (int category = 0; category < 18; ++category)
          for (int adjacent = 0; adjacent < 2; ++adjacent)
          {
            StubGeometryTileFeature feature(FeatureIdentifier(index), FeatureType::LineString, {},
              {{"cat", int64_t(category)}, {"mc", int64_t(category)}});
            binders.populateVertexVectors(feature, (index + 1) * 3, index, {}, {}, canonical);
            ++index;
          }
      auto actual = bytes(binders.interleavedVertexBuffer);
      if (mode == colormemo::Mode::Off) expected = actual;
      else EXPECT_EQ(expected, actual);
      EXPECT_EQ(mode == colormemo::Mode::Off ? 0u : index, session.stats.calls);
      EXPECT_EQ(mode == colormemo::Mode::Off ? 0u : 1u, session.stats.admitted);
      EXPECT_EQ(0u, session.stats.mismatches);
      EXPECT_EQ(0u, session.stats.capacityFallbacks);
      EXPECT_EQ(mode == colormemo::Mode::Verify ? session.stats.hits : 0u, session.stats.verifiedHits);
      EXPECT_EQ(mode == colormemo::Mode::Off ? 0u : index - (mode == colormemo::Mode::Reuse ? session.stats.hits : 0),
                session.stats.evaluations);
    }
  }
}

TEST(ColorMemo, ExactKeysIncludeSignedZeroMissingNullAndNumericConversion)
{
  Session session(colormemo::Mode::Verify);
  auto expression = parseColor(R"COLOR(["case",["has","x"],"rgba(1,2,3,0.25)","#ff0000"])COLOR" );
  auto cache = colormemo::Cache::create(expression, Color::black());
  ASSERT_TRUE(cache);
  for (int repeat = 0; repeat < 2; ++repeat)
  {
    check(*cache, expression, {});
    for (const Value& value : std::vector<Value>{NullValue{}, true, false, -0., 0., int64_t(1), uint64_t(1), 1.,
        int64_t(9007199254740993LL), uint64_t(9007199254740992ULL), std::numeric_limits<uint64_t>::max()})
    {
      check(*cache, expression, {{"x", value}});
      check(*cache, expression, {{"x", value}});
    }
  }
  EXPECT_EQ(9u, session.stats.misses);
  EXPECT_EQ(session.stats.hits, session.stats.verifiedHits);
  EXPECT_EQ(0u, session.stats.mismatches);
}

TEST(ColorMemo, UnsupportedValuesBypassEvenAfterLastKeyHit)
{
  Session session(colormemo::Mode::Verify);
  auto expression = parseColor(R"(["case",["has","x"],"#112233","#445566"])" );
  auto cache = colormemo::Cache::create(expression, Color::black());
  ASSERT_TRUE(cache);
  check(*cache, expression, {{"x", 1.}});
  check(*cache, expression, {{"x", 1.}});
  for (const Value& value : std::vector<Value>{std::string("1"), std::vector<Value>{1.}, PropertyMap{{"x", 1.}},
      std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
    check(*cache, expression, {{"x", value}});
  EXPECT_EQ(1u, session.stats.hits);
  EXPECT_EQ(5u, session.stats.bypasses);
  EXPECT_EQ(session.stats.calls, session.stats.evaluations);
}

TEST(ColorMemo, VerificationChecksLastKeyAndDisablesOnMismatch)
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
  Session session(colormemo::Mode::Verify);
  auto expression = parseColor(colorExpression);
  auto cache = colormemo::Cache::create(expression, Color::black());
  ASSERT_TRUE(cache);
  cache->evaluate(feature, canonical, {});
  feature.change = true;
  same(cache->evaluate(feature, canonical, {}), Color::white());
  EXPECT_EQ(1u, session.stats.mismatches);
  EXPECT_EQ(1u, session.stats.verifiedHits);
  feature.change = false;
  cache->evaluate(feature, canonical, {});
  EXPECT_EQ(1u, session.stats.hits);
  EXPECT_EQ(1u, session.stats.bypasses);
}

TEST(ColorMemo, LastKeyCannotBypassScopeOrGeneration)
{
  Session session;
  auto expression = parseColor(colorExpression);
  auto cache = colormemo::Cache::create(expression, Color::black());
  ASSERT_TRUE(cache);
  check(*cache, expression, {{"x", 1.}});
  {
    Session nested;
    nested.stats.applied.generation = 18;
    check(*cache, expression, {{"x", 1.}});
    EXPECT_EQ(0u, nested.stats.hits);
    EXPECT_EQ(1u, nested.stats.evaluations);
  }
  {
    colormemo::Scope absent(nullptr);
    check(*cache, expression, {{"x", 1.}});
  }
  std::thread thread([] { EXPECT_EQ(nullptr, colormemo::current()); });
  thread.join();
  check(*cache, expression, {{"x", 1.}});
  EXPECT_EQ(1u, session.stats.hits);
  EXPECT_FALSE(colormemo::configure(3));
}

TEST(ColorMemo, RejectsUnaccountedInputsAndBoundsExpressionAnalysis)
{
  Session session;
  for (const std::string json : {
    R"(["case",["==",["id"],1],"#112233","#445566"])",
    R"(["case",["==",["feature-state","x"],1],"#112233","#445566"])",
    R"(["match",["get","x",["properties"]],1,"#112233","#445566"])",
    R"(["match",["get",["to-string",["get","x"]]],1,"#112233","#445566"])",
    R"(["match",["geometry-type"],"LineString","#112233","#445566"])",
    R"(["case",["==",["get","x"],1],"#112233",["==",["get","y"],1],"#223344",["==",["get","z"],1],"#334455","#445566"])",
    R"(["interpolate",["linear"],["zoom"],10,["match",["get","x"],1,"#112233","#445566"],14,"#ffffff"])",
    R"(["to-color",["get","x"]])"})
  {
    SCOPED_TRACE(json);
    EXPECT_FALSE(colormemo::Cache::create(parseColor(json), Color::black()));
  }
  std::string wide = "[\"case\"";
  for (int i = 0; i < 200; ++i) wide += ",[\"==\",[\"get\",\"x\"]," + std::to_string(i) + "],\"#112233\"";
  wide += ",\"#445566\"]";
  EXPECT_FALSE(colormemo::Cache::create(parseColor(wide), Color::black()));
  std::string deep = "\"#112233\"";
  for (int i = 0; i < 40; ++i) deep = "[\"case\",[\"has\",\"x\"]," + deep + ",\"#445566\"]";
  EXPECT_FALSE(colormemo::Cache::create(parseColor(deep), Color::black()));
  auto longName = "[\"match\",[\"get\",\"" + std::string(65, 'x') + "\"],1,\"#112233\",\"#445566\"]";
  EXPECT_FALSE(colormemo::Cache::create(parseColor(longName), Color::black()));
}

TEST(ColorMemo, SeparateMemoryBudgetAndCardinalityBound)
{
  Session session;
  const auto colorBefore = colormemo::liveBytes();
  const auto widthReservation = paintmemo::MemoryLimit - paintmemo::liveBytes();
  ASSERT_TRUE(paintmemo::reserve(widthReservation));
  Scoped releaseWidth([&] { paintmemo::release(widthReservation); });
  auto expression = parseColor(R"(["case",["has","x"],"#112233","#445566"])" );
  auto cache = colormemo::Cache::create(expression, Color::black());
  ASSERT_TRUE(cache);
  for (int i = 0; i < 100; ++i) check(*cache, expression, {{"x", int64_t(i)}});
  EXPECT_EQ(1u, session.stats.capacityFallbacks);
  EXPECT_EQ(100u, session.stats.evaluations);
  std::vector<std::unique_ptr<colormemo::Cache>> retained;
  while (auto next = colormemo::Cache::create(expression, Color::black())) retained.push_back(std::move(next));
  EXPECT_LE(colormemo::liveBytes(), colormemo::MemoryLimit);
  EXPECT_EQ(1u, session.stats.budgetFallbacks);
  EXPECT_EQ(paintmemo::MemoryLimit, paintmemo::liveBytes());
  retained.clear();cache.reset();
  EXPECT_EQ(colorBefore, colormemo::liveBytes());
}

TEST(ColorMemo, DefaultsAndExpressionLifetimesStayIsolated)
{
  Session session;
  auto expression = parseColor(R"(["case",["==",["number",["get","x"]],1],"#112233","#445566"])" );
  auto cache = colormemo::Cache::create(expression, Color::red());
  auto second = colormemo::Cache::create(expression, Color::blue());
  ASSERT_TRUE(cache && second);
  for (int i = 0; i < 2; ++i)
  {
    check(*cache, expression, {}, Color::red());
    check(*second, expression, {}, Color::blue());
  }
  cache = colormemo::Cache::create(parseColor(colorExpression), Color::black());
  ASSERT_TRUE(cache);
  check(*cache, parseColor(colorExpression), {{"x", 1.}});
  check(*cache, parseColor(colorExpression), {{"x", 1.}});
}

TEST(ColorMemo, NonfiniteResultsAreNeverRetained)
{
  Session session;
  auto expression = parseColor(R"(["case",["==",["number",["get","x"]],1],"#112233","#445566"])" );
  Color fallback = Color::black();
  fallback.r = std::numeric_limits<float>::quiet_NaN();
  auto cache = colormemo::Cache::create(expression, fallback);
  ASSERT_TRUE(cache);
  StubGeometryTileFeature feature(PropertyMap{});
  for (int i = 0; i < 2; ++i) EXPECT_TRUE(std::isnan(cache->evaluate(feature, canonical, {}).r));
  EXPECT_EQ(2u, session.stats.misses);
  EXPECT_EQ(0u, session.stats.hits);
}

TEST(ColorMemo, OtherPropertiesConstantsAndCompositeColorsDoNotUseMemo)
{
  Session session;
  const auto expression = parseColor(colorExpression);
  FillPaintProperties::PossiblyEvaluated fill;
  fill.get<FillColor>() = PossiblyEvaluatedPropertyValue<Color>(expression);
  PaintPropertyBinders<TypeList<FillColor>> fillBinders(fill, 12);
  LinePaintProperties::PossiblyEvaluated line;
  line.get<LineColor>() = PossiblyEvaluatedPropertyValue<Color>(Color::red());
  PaintPropertyBinders<TypeList<LineColor>> constant(line, 12);
  line.get<LineColor>() = PossiblyEvaluatedPropertyValue<Color>(parseColor(
    R"(["interpolate",["linear"],["zoom"],10,["match",["get","x"],1,"#112233","#445566"],14,"#ffffff"])"));
  PaintPropertyBinders<TypeList<LineColor>> composite(line, 12);
  StubGeometryTileFeature feature({{"x", 1.}});
  for (int i = 0; i < 2; ++i)
  {
    fillBinders.populateVertexVectors(feature, (i + 1) * 2, i, {}, {}, canonical);
    constant.populateVertexVectors(feature, (i + 1) * 2, i, {}, {}, canonical);
    composite.populateVertexVectors(feature, (i + 1) * 2, i, {}, {}, canonical);
  }
  EXPECT_EQ(0u, session.stats.binders);
  EXPECT_EQ(0u, session.stats.calls);
}

TEST(ColorMemo, FeatureStateUpdatesUseOriginalEvaluator)
{
  auto expression = parseColor(R"(["case",["==",["feature-state","selected"],true],"#ff0000","#0000ff"])" );
  Session session;
  LinePaintProperties::PossiblyEvaluated properties;
  properties.get<LineColor>() = PossiblyEvaluatedPropertyValue<Color>(expression);
  PaintPropertyBinders<TypeList<LineColor>> binders(properties, 12);
  StubGeometryTileFeature feature(FeatureIdentifier(uint64_t(7)), FeatureType::LineString, {}, {});
  binders.populateVertexVectors(feature, 2, 0, {}, {}, canonical);
  auto& binder = *binders.get<LineColor>();
  binder.updateVertexVector(0, 2, feature, {{"selected", true}});
  const auto packed = attributeValue(Color::red());
  const auto vertex = std::get<0>(binder.getVertexValue(0));
  EXPECT_EQ(packed[0], vertex.a1[0]);
  EXPECT_EQ(packed[1], vertex.a1[1]);
  EXPECT_EQ(0u, session.stats.admitted);
  EXPECT_EQ(1u, session.stats.evaluations);
}
