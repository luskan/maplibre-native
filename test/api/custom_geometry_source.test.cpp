#include <mln/test/util.hpp>

#include <mln/map/map.hpp>
#include <mln/map/map_options.hpp>
#include <mln/gfx/headless_frontend.hpp>
#include <mln/storage/resource_options.hpp>
#include <mln/style/style.hpp>
#include <mln/style/sources/custom_geometry_source.hpp>
#include <mln/style/layers/fill_layer.hpp>
#include <mln/style/layers/line_layer.hpp>
#include <mln/util/geojson.hpp>
#include <mln/util/io.hpp>
#include <mln/util/mat4.hpp>
#include <mln/util/run_loop.hpp>
#include <mln/tile/custom_geometry_tile.hpp>
#include <mln/tile/geojson_tile_data.hpp>
#include <mln/tile/native_tile_payload.hpp>
#include <mln/renderer/renderer.hpp>
#include <mln/renderer/query.hpp>
#include <mln/style/layers/circle_layer.hpp>
#include <mln/gfx/backend_scope.hpp>
#include <mln/util/rapidjson.hpp>

#include <algorithm>
#include <cstring>
#include <mutex>

#ifdef MLN_TEST_EGL_SWAP
#include <EGL/egl.h>
#endif

using namespace mln;
using namespace mln::style;

TEST(CustomGeometrySource, Grid) {
    util::RunLoop loop;

    HeadlessFrontend frontend{1};
    Map map(frontend,
            MapObserver::nullObserver(),
            MapOptions().withMapMode(MapMode::Static).withSize(frontend.getSize()),
            ResourceOptions().withCachePath(":memory:").withAssetPath("test/fixtures/api/assets"));
    map.getStyle().loadJSON(util::read_file("test/fixtures/api/water.json"));
    map.jumpTo(CameraOptions().withCenter(LatLng{37.8, -122.5}).withZoom(10.0));

    CustomGeometrySource::Options options;
    options.fetchTileFunction = [&map](const mln::CanonicalTileID& tileID) {
        double gridSpacing = 0.1;
        FeatureCollection features;
        const LatLngBounds bounds(tileID);
        for (double y = ceil(bounds.north() / gridSpacing) * gridSpacing;
             y >= floor(bounds.south() / gridSpacing) * gridSpacing;
             y -= gridSpacing) {
            mapbox::geojson::line_string gridLine;
            gridLine.emplace_back(bounds.west(), y);
            gridLine.emplace_back(bounds.east(), y);

            features.emplace_back(gridLine);
        }

        for (double x = floor(bounds.west() / gridSpacing) * gridSpacing;
             x <= ceil(bounds.east() / gridSpacing) * gridSpacing;
             x += gridSpacing) {
            mapbox::geojson::line_string gridLine;
            gridLine.emplace_back(x, bounds.south());
            gridLine.emplace_back(x, bounds.north());

            features.emplace_back(gridLine);
        }
        auto source = static_cast<CustomGeometrySource*>(map.getStyle().getSource("custom"));
        if (source) {
            source->setTileData(tileID, features);
        }
    };

    map.getStyle().addSource(std::make_unique<CustomGeometrySource>("custom", options));

    auto fillLayer = std::make_unique<FillLayer>("landcover", "mapbox");
    fillLayer->setSourceLayer("landcover");
    fillLayer->setFillColor(Color{1.0, 1.0, 0.0, 1.0});
    map.getStyle().addLayer(std::move(fillLayer));

    auto layer = std::make_unique<LineLayer>("grid", "custom");
    layer->setLineColor(Color{1.0, 1.0, 1.0, 1.0});
    map.getStyle().addLayer(std::move(layer));

    test::checkImage("test/fixtures/custom_geometry_source/grid", frontend.render(map).image, 0.0006, 0.1);
}

TEST(CustomGeometrySource, GridSharedFeatures) {
    util::RunLoop loop;

    HeadlessFrontend frontend{1};
    Map map(frontend,
            MapObserver::nullObserver(),
            MapOptions().withMapMode(MapMode::Static).withSize(frontend.getSize()),
            ResourceOptions().withCachePath(":memory:").withAssetPath("test/fixtures/api/assets"));
    map.getStyle().loadJSON(util::read_file("test/fixtures/api/water.json"));
    map.jumpTo(CameraOptions().withCenter(LatLng{37.8, -122.5}).withZoom(10.0));

    CustomGeometrySource::Options options;
    options.fetchTileFunction = [&map](const mln::CanonicalTileID& tileID) {
        double gridSpacing = 0.1;
        auto features = std::make_shared<FeatureCollection>();
        const LatLngBounds bounds(tileID);
        for (double y = ceil(bounds.north() / gridSpacing) * gridSpacing;
             y >= floor(bounds.south() / gridSpacing) * gridSpacing;
             y -= gridSpacing) {
            mapbox::geojson::line_string gridLine;
            gridLine.emplace_back(bounds.west(), y);
            gridLine.emplace_back(bounds.east(), y);

            features->emplace_back(gridLine);
        }

        for (double x = floor(bounds.west() / gridSpacing) * gridSpacing;
             x <= ceil(bounds.east() / gridSpacing) * gridSpacing;
             x += gridSpacing) {
            mapbox::geojson::line_string gridLine;
            gridLine.emplace_back(x, bounds.south());
            gridLine.emplace_back(x, bounds.north());

            features->emplace_back(gridLine);
        }
        auto source = static_cast<CustomGeometrySource*>(map.getStyle().getSource("custom"));
        if (source) {
            source->setTileFeatures(tileID, features);
        }
    };

    map.getStyle().addSource(std::make_unique<CustomGeometrySource>("custom", options));

    auto fillLayer = std::make_unique<FillLayer>("landcover", "mapbox");
    fillLayer->setSourceLayer("landcover");
    fillLayer->setFillColor(Color{1.0, 1.0, 0.0, 1.0});
    map.getStyle().addLayer(std::move(fillLayer));

    auto layer = std::make_unique<LineLayer>("grid", "custom");
    layer->setLineColor(Color{1.0, 1.0, 1.0, 1.0});
    map.getStyle().addLayer(std::move(layer));

    test::checkImage("test/fixtures/custom_geometry_source/grid", frontend.render(map).image, 0.0006, 0.1);
}

namespace {

std::shared_ptr<const FeatureCollection> nativeSceneFeatures()
{
  auto features = std::make_shared<FeatureCollection>();
  features->emplace_back(Point<double>{-45, 20}, PropertyMap{{"name", "point"}}, uint64_t{1});
  features->emplace_back(LineString<double>{{-100, -20}, {100, -20}}, PropertyMap{{"name", "line"}}, uint64_t{2});
  features->emplace_back(Polygon<double>{{{20, 20}, {80, 20}, {80, 60}, {20, 60}, {20, 20}}},
                         PropertyMap{{"name", "polygon"}}, uint64_t{3});
  return features;
}

struct SourceScene
{
  util::RunLoop loop;
  std::shared_ptr<const FeatureCollection> features = nativeSceneFeatures();
  std::atomic<unsigned> resolverCalls{0};
  const bool empty;
  const bool fail;
  const bool lateError;
  std::mutex mutex;
  tiletrace::BatchInfo lastBatch;
  NativeRequestTicket lastTicket;
  NativeTilePayloadPtr lastPayload;
  CustomGeometrySource* source = nullptr;
  CustomGeometrySource::TileOptions tileOptions;
  HeadlessFrontend frontend{1};
  Map map{frontend, MapObserver::nullObserver(), MapOptions().withMapMode(MapMode::Static).withSize(frontend.getSize()),
          ResourceOptions().withCachePath(":memory:")};

  explicit SourceScene(bool native, bool empty_ = false, bool fail_ = false, bool lateError_ = false)
    : empty(empty_), fail(fail_), lateError(lateError_)
  {
    map.getStyle().loadJSON(R"({"version":8,"sources":{},"layers":[{"id":"background","type":"background","paint":{"background-color":"white"}}]})");
    map.jumpTo(CameraOptions().withCenter(LatLng{0, 0}).withZoom(0));
    installSource(native);
  }

  void replaceSource(bool native)
  {
    for (const auto* id : {"point", "line", "fill"}) map.getStyle().removeLayer(id);
    auto previous = map.getStyle().removeSource("geometry");
    previous.reset();
    source = nullptr;
    installSource(native);
  }

  void installSource(bool native)
  {
    tileOptions.dataType = native ? CustomGeometrySource::TileDataType::NativeGeometry
                                  : CustomGeometrySource::TileDataType::LegacyFeatures;
    tileOptions.traceMap = 901;
    tileOptions.traceSource = 902;
    CustomGeometrySource::Options options;
    options.zoomRange = {0, 0};
    options.tileOptions = tileOptions;
    options.nativeCallbacks.resolve = [&](const CanonicalTileID&) {
      ++resolverCalls;
      return NativeRequestBinding{"scene-v1", features};
    };
    options.nativeCallbacks.cancel = [](const CanonicalTileID&, const NativeRequestTicket&) {};
    options.nativeCallbacks.fetch = [&](const CanonicalTileID& tile, const NativeRequestTicket& ticket,
                                       tiletrace::Context trace) {
      if (fail) throw std::runtime_error("synthetic native producer failure");
      trace = publication(trace);
      NativeTileBuilder builder({tile, nativeTileConversionContract(tileOptions)});
      if (!empty)
      {
        GeoJSONTileData reference(CustomGeometryTile::processTileData(*features, tile, tileOptions));
        auto layer = reference.getLayer("");
        for (std::size_t i = 0; i < layer->featureCount(); ++i)
        {
          auto feature = layer->getFeature(i);
          builder.appendFinal(feature->getType(), feature->getGeometries().clone(),
                              PropertyMap(feature->getProperties()), feature->getID());
        }
      }
      auto payload = std::move(builder).seal();
      tiletrace::markNativeReady(trace);
      recordBatch(tile, trace);
      {
        std::lock_guard guard(mutex);
        lastTicket = ticket;
        lastPayload = payload;
      }
      tiletrace::mark(trace, tiletrace::Enqueued);
      source->setTracedTilePayload(tile, ticket, std::move(payload), trace);
      if (lateError)
        source->setNativeTileError(tile, ticket,
            std::make_exception_ptr(std::runtime_error("duplicate producer error after native publication")), trace);
    };
    options.tracedFetch = [&](const CanonicalTileID& tile, tiletrace::Context trace) {
      trace = publication(trace);
      trace.payloadFormat = tiletrace::PayloadFormat::LegacyFeatures;
      tiletrace::mark(trace, tiletrace::Features);
      tiletrace::mark(trace, tiletrace::Enqueued);
      source->setTracedTileFeatures(tile, features, trace);
    };
    auto owned = std::make_unique<CustomGeometrySource>("geometry", options);
    source = owned.get();
    map.getStyle().addSource(std::move(owned));
    auto fill = std::make_unique<FillLayer>("fill", "geometry");
    fill->setFillColor(Color{0, 1, 0, 1});
    map.getStyle().addLayer(std::move(fill));
    auto line = std::make_unique<LineLayer>("line", "geometry");
    line->setLineColor(Color{0, 0, 1, 1});
    line->setLineWidth(3);
    map.getStyle().addLayer(std::move(line));
    auto point = std::make_unique<CircleLayer>("point", "geometry");
    point->setCircleColor(Color{1, 0, 0, 1});
    point->setCircleRadius(8);
    map.getStyle().addLayer(std::move(point));
  }

  static tiletrace::Context publication(tiletrace::Context trace)
  {
    if (trace.id)
    {
      trace.id = trace.publication = tiletrace::nextID();
      trace.kind = tiletrace::Kind::Publication;
      trace.origin = tiletrace::Origin::Fresh;
      tiletrace::mark(trace, tiletrace::Producer);
      tiletrace::mark(trace, tiletrace::Worker);
    }
    return trace;
  }

  void recordBatch(const CanonicalTileID& tile, const tiletrace::Context& trace)
  {
    if (!trace.id) return;
    tiletrace::BatchInfo batch;
    batch.session = trace.session;
    batch.id = tiletrace::nextID();
    batch.startedUs = trace.time[tiletrace::Producer];
    batch.endedUs = tiletrace::now();
    batch.batchMs = (batch.endedUs - batch.startedUs) / 1000;
    batch.total = batch.extracted = 1;
    batch.z = tile.z;
    batch.minX = batch.maxX = tile.x;
    batch.minY = batch.maxY = tile.y;
    tiletrace::trackBatchMember(batch, trace);
    std::lock_guard guard(mutex);
    lastBatch = batch;
  }

  std::vector<Feature> sourceFeatures()
  {
    auto result = frontend.getRenderer()->querySourceFeatures("geometry");
    sort(result);
    return result;
  }

  std::vector<Feature> renderedFeatures()
  {
    auto result = frontend.getRenderer()->queryRenderedFeatures(ScreenBox{{0, 0}, {256, 256}});
    sort(result);
    return result;
  }

  static void sort(std::vector<Feature>& values)
  {
    std::sort(values.begin(), values.end(), [](const Feature& a, const Feature& b) {
      return a.id.get<uint64_t>() < b.id.get<uint64_t>();
    });
  }
};

struct TraceCaptureScope
{
  const bool wasEnabled = tiletrace::enabled();
  TraceCaptureScope() { tiletrace::configure(true, true); }
  ~TraceCaptureScope() { tiletrace::configure(wasEnabled, true); }
};

} // namespace

TEST(CustomGeometrySource, NativePointLineFillRenderingAndQueriesMatchLegacy)
{
  PremultipliedImage legacyImage;
  std::vector<Feature> legacySource, legacyRendered;
  {
    SourceScene legacy(false);
    legacyImage = legacy.frontend.render(legacy.map).image;
    legacySource = legacy.sourceFeatures();
    legacyRendered = legacy.renderedFeatures();
    EXPECT_EQ(0u, legacy.resolverCalls);
  }
  SourceScene native(true);
  const auto result = native.frontend.render(native.map);
  EXPECT_GT(native.resolverCalls, 0u);
  EXPECT_EQ(legacyImage.size, result.image.size);
  EXPECT_EQ(0, std::memcmp(legacyImage.data.get(), result.image.data.get(), legacyImage.bytes()));
  ASSERT_EQ(3u, native.sourceFeatures().size());
  ASSERT_GE(native.renderedFeatures().size(), 3u);
  EXPECT_EQ(legacySource, native.sourceFeatures());
  EXPECT_EQ(legacyRendered, native.renderedFeatures());
}

TEST(CustomGeometrySource, NativeEmptyPayloadAndProducerFailureRemainDifferent)
{
  {
    SourceScene empty(true, true);
    EXPECT_TRUE(empty.frontend.render(empty.map).image.valid());
    EXPECT_TRUE(empty.sourceFeatures().empty());
    EXPECT_TRUE(empty.renderedFeatures().empty());
  }
  SourceScene failure(true, false, true);
  EXPECT_THROW(failure.frontend.render(failure.map), std::runtime_error);
}

TEST(CustomGeometrySource, NativeInitializationRequiresAllCallbacks)
{
  util::RunLoop loop;
  CustomGeometrySource::Options options;
  options.tileOptions.dataType = CustomGeometrySource::TileDataType::NativeGeometry;
  EXPECT_THROW(CustomGeometrySource("native", options), std::invalid_argument);
  options.nativeCallbacks.resolve = [](const CanonicalTileID&) {
    return NativeRequestBinding{"key", std::make_shared<const int>(1)};
  };
  options.nativeCallbacks.fetch = [](const CanonicalTileID&, const NativeRequestTicket&, tiletrace::Context) {};
  EXPECT_THROW(CustomGeometrySource("native", options), std::invalid_argument);
  options.nativeCallbacks.cancel = [](const CanonicalTileID&, const NativeRequestTicket&) {};
  EXPECT_NO_THROW(CustomGeometrySource("native", options));
}

#ifdef MLN_TEST_EGL_SWAP
TEST(CustomGeometrySource, NativeReadinessAdmissionDrawAndSuccessfulSwapUseExistingEndpoints)
{
  TraceCaptureScope capture;
  SourceScene scene(true, false, false, true);
  {
    tiletrace::FrameScope frame(2);
    ASSERT_TRUE(scene.frontend.render(scene.map).image.valid());
    tiletrace::frameEnd();
    gfx::BackendScope backend(*scene.frontend.getBackend());
    const auto display = eglGetCurrentDisplay();
    const auto surface = eglGetCurrentSurface(EGL_DRAW);
    ASSERT_NE(EGL_NO_DISPLAY, display);
    ASSERT_NE(EGL_NO_SURFACE, surface);
    tiletrace::swapBegin(reinterpret_cast<uintptr_t>(surface));
    const auto swapped = eglSwapBuffers(display, surface);
    const auto error = swapped == EGL_TRUE ? EGL_SUCCESS : eglGetError();
    tiletrace::swapEnd(swapped == EGL_TRUE, error);
    ASSERT_EQ(EGL_TRUE, swapped);
  }
  tiletrace::BatchInfo batch;
  {
    std::lock_guard guard(scene.mutex);
    batch = scene.lastBatch;
  }
  rapidjson::Document detail;
  detail.Parse(tiletrace::snapshotJSON());
  ASSERT_FALSE(detail.HasParseError());
  bool submitted = false;
  for (const auto& record : detail["records"].GetArray())
  {
    if (std::string(record["outcome"].GetString()) == "submitted")
    {
      EXPECT_STREQ("native-geometry", record["payloadFormat"].GetString());
      EXPECT_STREQ("bypassed", record["conversion"].GetString());
      const auto& times = record["timesUs"];
      EXPECT_GT(std::stoull(times[tiletrace::Features].GetString()), 0u);
      EXPECT_GE(std::stoull(times[tiletrace::Submitted].GetString()), std::stoull(times[tiletrace::Draw].GetString()));
      submitted = true;
    }
  }
  EXPECT_TRUE(submitted);
  rapidjson::Document completed;
  completed.Parse(tiletrace::batchSnapshotJSON(batch, true));
  ASSERT_FALSE(completed.HasParseError());
  EXPECT_STREQ("complete", completed["mlStatus"].GetString());
  EXPECT_STREQ("complete", completed["status"].GetString());
  EXPECT_EQ(1u, completed["mlStored"].GetUint());
  EXPECT_GT(completed["displayUs"].GetUint64(), 0u);
}
#endif

TEST(CustomGeometrySource, NativeRecreationRejectsQueuedAndLatePreviousRepresentations)
{
  SourceScene scene(true);
  const auto original = scene.frontend.render(scene.map);
  NativeRequestTicket previousTicket;
  NativeTilePayloadPtr previousPayload;
  {
    std::lock_guard guard(scene.mutex);
    previousTicket = scene.lastTicket;
    previousPayload = scene.lastPayload;
  }
  const CanonicalTileID canonical(0, 0, 0);
  NativeTileBuilder builder(previousPayload->metadata());
  builder.appendFinal(FeatureType::Point, {{{4096, 4096}}}, {{"name", "old-native"}}, uint64_t{99});
  auto oldNative = std::move(builder).seal();
  scene.source->setTracedTilePayload(canonical, previousTicket, oldNative);
  scene.replaceSource(false);
  EXPECT_FALSE(previousTicket.isCurrent());
  scene.source->setTracedTilePayload(canonical, previousTicket, oldNative);
  auto legacy = scene.frontend.render(scene.map);
  EXPECT_EQ(0, std::memcmp(original.image.data.get(), legacy.image.data.get(), original.image.bytes()));

  auto oldLegacy = std::make_shared<FeatureCollection>();
  oldLegacy->emplace_back(Point<double>{0, 0}, PropertyMap{{"name", "old-legacy"}}, uint64_t{99});
  scene.source->setTileFeatures(canonical, oldLegacy);
  scene.replaceSource(true);
  scene.source->setTracedTileFeatures(canonical, oldLegacy, {});
  scene.source->setTracedTilePayload(canonical, previousTicket, oldNative);
  auto native = scene.frontend.render(scene.map);
  EXPECT_EQ(0, std::memcmp(original.image.data.get(), native.image.data.get(), original.image.bytes()));
  auto features = scene.sourceFeatures();
  ASSERT_EQ(3u, features.size());
  for (const auto& feature : features) EXPECT_LT(feature.id.get<uint64_t>(), 99u);
  std::lock_guard guard(scene.mutex);
  EXPECT_NE(previousTicket.sourceEpoch(), scene.lastTicket.sourceEpoch());
  EXPECT_TRUE(scene.lastTicket.isCurrent());
}
