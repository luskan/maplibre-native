#include <mln/test/util.hpp>
#include <mln/test/fake_file_source.hpp>
#include <mln/test/stub_tile_observer.hpp>
#include <mln/style/sources/custom_geometry_source.hpp>
#include <mln/tile/custom_geometry_tile.hpp>
#include <mln/style/custom_tile_loader.hpp>
#include <mln/style/custom_tile_loader_cache.hpp>

#include <mln/util/run_loop.hpp>
#include <mln/util/timer.hpp>
#include <mln/map/transform.hpp>
#include <mln/renderer/query.hpp>
#include <mln/renderer/tile_parameters.hpp>
#include <mln/style/style.hpp>
#include <mln/style/layers/circle_layer.hpp>
#include <mln/style/layers/circle_layer_impl.hpp>
#include <mln/style/layers/fill_layer.hpp>
#include <mln/style/layers/fill_layer_impl.hpp>
#include <mln/style/layers/symbol_layer.hpp>
#include <mln/style/layers/symbol_layer_impl.hpp>
#include <mln/style/image_impl.hpp>
#include <mln/test/geometry_memo_observer.hpp>
#include <mln/tile/geojson_tile_data.hpp>
#include <mln/style/sources/custom_geometry_source_impl.hpp>
#include <mln/style/custom_tile_conversion.hpp>
#include <mln/style/native_tile_request_state.hpp>
#include <mln/tile/native_geometry_tile_data.hpp>
#include <mln/util/rapidjson.hpp>
#include <mln/util/layout_timing.hpp>
#include <mln/util/scoped.hpp>
#include <mln/annotation/annotation_manager.hpp>
#include <mln/renderer/image_manager.hpp>
#include <mln/renderer/image_manager_observer.hpp>
#include <mln/text/glyph_manager.hpp>
#include <mln/gfx/dynamic_texture_atlas.hpp>
#include <mln/gfx/headless_backend.hpp>
#include <mln/gfx/backend_scope.hpp>
#include <mln/renderer/bucket.hpp>

#include <future>
#include <memory>

using namespace mln;
using namespace mln::style;

class CustomTileTest {
public:
    util::SimpleIdentity uniqueID;
    std::shared_ptr<FileSource> fileSource = std::make_shared<FakeFileSource>();
    TransformState transformState;
    util::RunLoop loop;
    AnnotationManager annotationManager{style};
    std::shared_ptr<ImageManager> imageManager = ImageManager::create();
    std::shared_ptr<GlyphManager> glyphManager = std::make_shared<GlyphManager>();
    gfx::DynamicTextureAtlasPtr dynamicTextureAtlas;

    TileParameters tileParameters;
    style::Style style;

    CustomTileTest()
        : tileParameters{.pixelRatio = 1.0,
                         .debugOptions = MapDebugOptions(),
                         .transformState = transformState,
                         .fileSource = fileSource,
                         .mode = MapMode::Continuous,
                         .annotationManager = annotationManager.makeWeakPtr(),
                         .imageManager = imageManager,
                         .glyphManager = glyphManager,
                         .prefetchZoomDelta = 0,
                         .threadPool = {Scheduler::GetBackground(), uniqueID},
                         .dynamicTextureAtlas = dynamicTextureAtlas,
                         .geometryTileZoomState = {},
                         .traceEvaluationZoom = {}},
          style{fileSource, 1, tileParameters.threadPool} {}

    template <class Predicate>
    bool waitUntil(const Predicate& complete) {
        if (complete()) {
            return true;
        }
        util::Timer timeout;
        timeout.start(Seconds(5), Duration::zero(), [&] { loop.stop(); });
        util::Timer poll;
        poll.start(Milliseconds(1), Milliseconds(1), [&] {
            if (complete()) {
                loop.stop();
            }
        });
        loop.run();
        return complete();
    }
};

TEST(CustomGeometryTile, InvokeFetchTile) {
    CustomTileTest test;

    CircleLayer layer("circle", "source");

    mapbox::feature::feature_collection<double> features;
    features.push_back(mapbox::feature::feature<double>{mapbox::geometry::point<double>(0, 0)});
    CustomTileLoader loader(
        [&](const CanonicalTileID& tileId) {
            EXPECT_EQ(tileId, CanonicalTileID(0, 0, 0));
            test.loop.stop();
        },
        [&](const CanonicalTileID&) {

        });
    auto mb = std::make_shared<Mailbox>(*Scheduler::GetCurrent());
    ActorRef<CustomTileLoader> loaderActor(loader, mb);

    CustomGeometryTile tile(OverscaledTileID(0, 0, 0),
                            "source",
                            test.tileParameters,
                            makeMutable<CustomGeometrySource::TileOptions>(),
                            loaderActor);

    tile.setNecessity(TileNecessity::Required);

    test.loop.run();
}

TEST(CustomGeometryTile, InvokeCancelTile) {
    CustomTileTest test;

    CircleLayer layer("circle", "source");

    mapbox::feature::feature_collection<double> features;
    features.push_back(mapbox::feature::feature<double>{mapbox::geometry::point<double>(0, 0)});

    CustomTileLoader loader([&](const CanonicalTileID&) {},
                            [&](const CanonicalTileID& tileId) {
                                EXPECT_EQ(tileId, CanonicalTileID(0, 0, 0));
                                test.loop.stop();
                            });
    auto mb = std::make_shared<Mailbox>(*Scheduler::GetCurrent());
    ActorRef<CustomTileLoader> loaderActor(loader, mb);

    CustomGeometryTile tile(OverscaledTileID(0, 0, 0),
                            "source",
                            test.tileParameters,
                            makeMutable<CustomGeometrySource::TileOptions>(),
                            loaderActor);

    tile.setNecessity(TileNecessity::Required);
    tile.setNecessity(TileNecessity::Optional);
    test.loop.run();
}

TEST(CustomGeometryTile, InvokeTileChanged) {
    CustomTileTest test;

    CircleLayer layer("circle", "source");

    mapbox::feature::feature_collection<double> features;
    features.push_back(mapbox::feature::feature<double>{mapbox::geometry::point<double>(0, 0)});

    CustomTileLoader loader(nullptr, nullptr);
    auto mb = std::make_shared<Mailbox>(*Scheduler::GetCurrent());
    ActorRef<CustomTileLoader> loaderActor(loader, mb);

    CustomGeometryTile tile(OverscaledTileID(0, 0, 0),
                            "source",
                            test.tileParameters,
                            makeMutable<CustomGeometrySource::TileOptions>(),
                            loaderActor);

    Immutable<LayerProperties> layerProperties = makeMutable<CircleLayerProperties>(
        staticImmutableCast<CircleLayer::Impl>(layer.baseImpl));
    StubTileObserver observer;
    observer.tileChanged = [&](const Tile&) {
        // Once present, the bucket should never "disappear", which would cause
        // flickering.
        ASSERT_TRUE(tile.layerPropertiesUpdated(layerProperties));
    };

    std::vector<Immutable<LayerProperties>> layers{layerProperties};
    tile.setLayers(layers);
    tile.setObserver(&observer);
    tile.setTileData(features);

    while (!tile.isComplete()) {
        test.loop.runOnce();
    }
}

TEST(CustomGeometryTile, InvokeTileChangedProcessedFeatures) {
    CustomTileTest test;

    CircleLayer layer("circle", "source");

    mapbox::feature::feature_collection<double> features;
    features.push_back(mapbox::feature::feature<double>{mapbox::geometry::point<double>(0, 0)});

    CustomTileLoader loader(nullptr, nullptr);
    auto mb = std::make_shared<Mailbox>(*Scheduler::GetCurrent());
    ActorRef<CustomTileLoader> loaderActor(loader, mb);

    CustomGeometryTile tile(OverscaledTileID(0, 0, 0),
                            "source",
                            test.tileParameters,
                            makeMutable<CustomGeometrySource::TileOptions>(),
                            loaderActor);

    Immutable<LayerProperties> layerProperties = makeMutable<CircleLayerProperties>(
        staticImmutableCast<CircleLayer::Impl>(layer.baseImpl));
    StubTileObserver observer;
    observer.tileChanged = [&](const Tile&) {
        ASSERT_TRUE(tile.layerPropertiesUpdated(layerProperties));
    };

    std::vector<Immutable<LayerProperties>> layers{layerProperties};
    tile.setLayers(layers);
    tile.setObserver(&observer);
    tile.setTileData(CustomGeometryTile::processTileData(features, CanonicalTileID(0, 0, 0), {}));

    while (!tile.isComplete()) {
        test.loop.runOnce();
    }
}

TEST(CustomGeometryTile, AbandonedFeatureCompletionRequiresFreshFetch) {
    CustomTileTest test;
    std::size_t fetchCount = 0;
    CustomTileLoader loader(
        [&](const CanonicalTileID&) { ++fetchCount; }, nullptr);
    auto loaderMailbox = std::make_shared<Mailbox>(*Scheduler::GetCurrent());
    ActorRef<CustomTileLoader> loaderActor(loader, loaderMailbox);
    const OverscaledTileID tileID(0, 0, 0);
    CustomGeometryTile first(tileID,
                             "source",
                             test.tileParameters,
                             makeMutable<CustomGeometrySource::TileOptions>(),
                             loaderActor);
    CustomGeometryTile second(tileID,
                              "source",
                              test.tileParameters,
                              makeMutable<CustomGeometrySource::TileOptions>(),
                              loaderActor);
    auto firstMailbox = std::make_shared<Mailbox>(*Scheduler::GetCurrent());
    auto secondMailbox = std::make_shared<Mailbox>(*Scheduler::GetCurrent());
    ActorRef<CustomGeometryTile> firstActor(first, firstMailbox);
    ActorRef<CustomGeometryTile> secondActor(second, secondMailbox);

    const auto firstToken = nextCustomTileRegistrationToken();
    const auto secondToken = nextCustomTileRegistrationToken();
    loader.fetchTile(tileID, firstActor, firstToken);
    EXPECT_EQ(1u, fetchCount);
    loader.removeTile(tileID, firstToken);

    auto features = std::make_shared<FeatureCollection>();
    features->emplace_back(mapbox::geometry::point<double>(0, 0));
    const auto beforeAbandonment = getCustomTileLoaderDataCacheStats();
    loader.setTileFeatures(tileID.canonical, features);
    const auto afterAbandonment = getCustomTileLoaderDataCacheStats();
    EXPECT_EQ(beforeAbandonment.stores, afterAbandonment.stores);
    EXPECT_EQ(beforeAbandonment.bypasses + 1, afterAbandonment.bypasses);

    loader.fetchTile(tileID, secondActor, secondToken);
    EXPECT_EQ(2u, fetchCount);
    loader.setTileFeatures(tileID.canonical, features);
    const auto afterFreshCompletion = getCustomTileLoaderDataCacheStats();
    EXPECT_EQ(afterAbandonment.stores + 1, afterFreshCompletion.stores);
    firstMailbox->close();
    secondMailbox->close();
}

TEST(CustomGeometryTile, AbandonedGeoJSONCompletionRequiresFreshFetch) {
    CustomTileTest test;
    std::size_t fetchCount = 0;
    CustomTileLoader loader(
        [&](const CanonicalTileID&) { ++fetchCount; }, nullptr);
    auto loaderMailbox = std::make_shared<Mailbox>(*Scheduler::GetCurrent());
    ActorRef<CustomTileLoader> loaderActor(loader, loaderMailbox);
    const OverscaledTileID tileID(0, 0, 0);
    CustomGeometryTile tile(tileID,
                            "source",
                            test.tileParameters,
                            makeMutable<CustomGeometrySource::TileOptions>(),
                            loaderActor);
    auto tileMailbox = std::make_shared<Mailbox>(*Scheduler::GetCurrent());
    ActorRef<CustomGeometryTile> tileActor(tile, tileMailbox);
    FeatureCollection features;
    features.emplace_back(mapbox::geometry::point<double>(0, 0));
    GeoJSON data{features};

    const auto beforeAbandonment = getCustomTileLoaderDataCacheStats();
    loader.setTileData(tileID.canonical, data);
    const auto afterAbandonment = getCustomTileLoaderDataCacheStats();
    EXPECT_EQ(beforeAbandonment.stores, afterAbandonment.stores);
    EXPECT_EQ(beforeAbandonment.bypasses + 1, afterAbandonment.bypasses);

    loader.fetchTile(tileID, tileActor, nextCustomTileRegistrationToken());
    EXPECT_EQ(1u, fetchCount);
    loader.setTileData(tileID.canonical, data);
    const auto afterFreshCompletion = getCustomTileLoaderDataCacheStats();
    EXPECT_EQ(afterAbandonment.stores + 1, afterFreshCompletion.stores);
    tileMailbox->close();
}

// A tile that dies after being replaced must not unregister its replacement.
// This is the sequence that used to leave a tile gray until something unrelated
// recreated it.
TEST(CustomGeometryTile, RemoveOfReplacedTileKeepsReplacementRegistered) {
    CustomTileTest test;
    std::size_t fetchCount = 0;
    std::size_t cancelCount = 0;
    CustomTileLoader loader([&](const CanonicalTileID&) { ++fetchCount; },
                            [&](const CanonicalTileID&) { ++cancelCount; });
    auto loaderMailbox = std::make_shared<Mailbox>(*Scheduler::GetCurrent());
    ActorRef<CustomTileLoader> loaderActor(loader, loaderMailbox);
    const OverscaledTileID tileID(0, 0, 0);
    CustomGeometryTile first(
        tileID, "source", test.tileParameters, makeMutable<CustomGeometrySource::TileOptions>(), loaderActor);
    CustomGeometryTile second(
        tileID, "source", test.tileParameters, makeMutable<CustomGeometrySource::TileOptions>(), loaderActor);
    auto firstMailbox = std::make_shared<Mailbox>(*Scheduler::GetCurrent());
    auto secondMailbox = std::make_shared<Mailbox>(*Scheduler::GetCurrent());
    ActorRef<CustomGeometryTile> firstActor(first, firstMailbox);
    ActorRef<CustomGeometryTile> secondActor(second, secondMailbox);
    const auto firstToken = nextCustomTileRegistrationToken();
    const auto secondToken = nextCustomTileRegistrationToken();

    CircleLayer layer("circle", "source");
    Immutable<LayerProperties> layerProperties = makeMutable<CircleLayerProperties>(
        staticImmutableCast<CircleLayer::Impl>(layer.baseImpl));
    second.setLayers({layerProperties});

    loader.fetchTile(tileID, firstActor, firstToken);
    EXPECT_EQ(1u, fetchCount);

    // The first tile stops needing the tile but stays registered.
    loader.cancelTile(tileID, firstToken);
    EXPECT_EQ(1u, cancelCount);

    // The replacement must get its own fetch, the one above was cancelled.
    loader.fetchTile(tileID, secondActor, secondToken);
    EXPECT_EQ(2u, fetchCount);

    // The late message from the destroyed first tile arrives now. It must take
    // away only its own registration.
    loader.removeTile(tileID, firstToken);
    EXPECT_EQ(1u, cancelCount) << "removing the dead tile must not cancel the live one";

    // A repeat of that message has nothing left to remove and must be ignored
    // rather than fall back to matching by tile ID.
    const auto beforeRepeat = getCustomTileLoaderRegistrationStats();
    loader.removeTile(tileID, firstToken);
    const auto afterRepeat = getCustomTileLoaderRegistrationStats();
    EXPECT_EQ(beforeRepeat.staleRemovesIgnored + 1, afterRepeat.staleRemovesIgnored);
    EXPECT_EQ(1u, cancelCount);

    auto features = std::make_shared<FeatureCollection>();
    features->emplace_back(mapbox::geometry::point<double>(0, 0));
    features->back().properties["receiver"] = std::string("replacement");
    const auto beforePublish = getCustomTileLoaderDataCacheStats();
    loader.setTileFeatures(tileID.canonical, features);
    const auto afterPublish = getCustomTileLoaderDataCacheStats();
    EXPECT_EQ(beforePublish.stores + 1, afterPublish.stores);

    ASSERT_TRUE(test.waitUntil([&] { return second.isComplete(); }));
    ASSERT_TRUE(second.isRenderable());
    std::vector<Feature> delivered;
    second.querySourceFeatures(delivered, {});
    ASSERT_EQ(1u, delivered.size());
    EXPECT_EQ(features->front().properties, delivered.front().properties);
    EXPECT_FALSE(first.isComplete());

    firstMailbox->close();
    secondMailbox->close();
}

// A remove naming a registration that is already gone must do nothing at all.
TEST(CustomGeometryTile, StaleRemoveAfterInvalidateIsIgnored) {
    CustomTileTest test;
    std::size_t cancelCount = 0;
    CustomTileLoader loader([](const CanonicalTileID&) {},
                            [&](const CanonicalTileID&) { ++cancelCount; });
    auto loaderMailbox = std::make_shared<Mailbox>(*Scheduler::GetCurrent());
    ActorRef<CustomTileLoader> loaderActor(loader, loaderMailbox);
    const OverscaledTileID tileID(0, 0, 0);
    CustomGeometryTile tile(
        tileID, "source", test.tileParameters, makeMutable<CustomGeometrySource::TileOptions>(), loaderActor);
    auto tileMailbox = std::make_shared<Mailbox>(*Scheduler::GetCurrent());
    ActorRef<CustomGeometryTile> tileActor(tile, tileMailbox);
    const auto token = nextCustomTileRegistrationToken();

    loader.fetchTile(tileID, tileActor, token);
    loader.invalidateTile(tileID.canonical);
    const auto afterInvalidate = cancelCount;

    const auto before = getCustomTileLoaderRegistrationStats();
    loader.removeTile(tileID, token);
    const auto after = getCustomTileLoaderRegistrationStats();
    EXPECT_EQ(before.staleRemovesIgnored, after.staleRemovesIgnored);
    EXPECT_EQ(afterInvalidate, cancelCount);

    tileMailbox->close();
}

// Two receivers share one canonical tile, so the producer keeps working until
// the last one gives up.
TEST(CustomGeometryTile, SharedCanonicalCancelsOnlyWhenNobodyWants) {
    CustomTileTest test;
    std::size_t cancelCount = 0;
    CustomTileLoader loader([](const CanonicalTileID&) {},
                            [&](const CanonicalTileID&) { ++cancelCount; });
    auto loaderMailbox = std::make_shared<Mailbox>(*Scheduler::GetCurrent());
    ActorRef<CustomTileLoader> loaderActor(loader, loaderMailbox);
    const OverscaledTileID tileID(0, 0, 0);
    CustomGeometryTile firstTile(
        tileID, "source", test.tileParameters, makeMutable<CustomGeometrySource::TileOptions>(), loaderActor);
    CustomGeometryTile secondTile(
        tileID, "source", test.tileParameters, makeMutable<CustomGeometrySource::TileOptions>(), loaderActor);
    auto firstMailbox = std::make_shared<Mailbox>(*Scheduler::GetCurrent());
    auto secondMailbox = std::make_shared<Mailbox>(*Scheduler::GetCurrent());
    ActorRef<CustomGeometryTile> firstActor(firstTile, firstMailbox);
    ActorRef<CustomGeometryTile> secondActor(secondTile, secondMailbox);
    const auto firstToken = nextCustomTileRegistrationToken();
    const auto secondToken = nextCustomTileRegistrationToken();

    loader.fetchTile(tileID, firstActor, firstToken);
    loader.fetchTile(tileID, secondActor, secondToken);

    loader.cancelTile(tileID, firstToken);
    EXPECT_EQ(0u, cancelCount) << "the second receiver still wants the tile";

    loader.cancelTile(tileID, secondToken);
    EXPECT_EQ(1u, cancelCount);

    firstMailbox->close();
    secondMailbox->close();
}

// Invalidation used to send one cancel per registered receiver.
TEST(CustomGeometryTile, InvalidateCancelsOncePerCanonical) {
    CustomTileTest test;
    std::size_t cancelCount = 0;
    CustomTileLoader loader([](const CanonicalTileID&) {},
                            [&](const CanonicalTileID&) { ++cancelCount; });
    auto loaderMailbox = std::make_shared<Mailbox>(*Scheduler::GetCurrent());
    ActorRef<CustomTileLoader> loaderActor(loader, loaderMailbox);
    const OverscaledTileID tileID(0, 0, 0);
    CustomGeometryTile firstTile(
        tileID, "source", test.tileParameters, makeMutable<CustomGeometrySource::TileOptions>(), loaderActor);
    CustomGeometryTile secondTile(
        tileID, "source", test.tileParameters, makeMutable<CustomGeometrySource::TileOptions>(), loaderActor);
    auto firstMailbox = std::make_shared<Mailbox>(*Scheduler::GetCurrent());
    auto secondMailbox = std::make_shared<Mailbox>(*Scheduler::GetCurrent());
    ActorRef<CustomGeometryTile> firstActor(firstTile, firstMailbox);
    ActorRef<CustomGeometryTile> secondActor(secondTile, secondMailbox);

    loader.fetchTile(tileID, firstActor, nextCustomTileRegistrationToken());
    loader.fetchTile(tileID, secondActor, nextCustomTileRegistrationToken());
    loader.invalidateTile(tileID.canonical);
    EXPECT_EQ(1u, cancelCount);

    firstMailbox->close();
    secondMailbox->close();
}

// A tile that gave up and then wants the tile again must trigger a new fetch.
TEST(CustomGeometryTile, CancelThenFetchIssuesFreshFetch) {
    CustomTileTest test;
    std::size_t fetchCount = 0;
    std::size_t cancelCount = 0;
    CustomTileLoader loader([&](const CanonicalTileID&) { ++fetchCount; },
                            [&](const CanonicalTileID&) { ++cancelCount; });
    auto loaderMailbox = std::make_shared<Mailbox>(*Scheduler::GetCurrent());
    ActorRef<CustomTileLoader> loaderActor(loader, loaderMailbox);
    const OverscaledTileID tileID(0, 0, 0);
    CustomGeometryTile tile(
        tileID, "source", test.tileParameters, makeMutable<CustomGeometrySource::TileOptions>(), loaderActor);
    auto tileMailbox = std::make_shared<Mailbox>(*Scheduler::GetCurrent());
    ActorRef<CustomGeometryTile> tileActor(tile, tileMailbox);
    const auto token = nextCustomTileRegistrationToken();

    loader.fetchTile(tileID, tileActor, token);
    EXPECT_EQ(1u, fetchCount);
    loader.cancelTile(tileID, token);
    EXPECT_EQ(1u, cancelCount);
    loader.fetchTile(tileID, tileActor, token);
    EXPECT_EQ(2u, fetchCount);

    tileMailbox->close();
}

// Publishing does not mean the producer is idle, so a later cancel must still
// reach it. Otherwise an extraction nobody waits for runs to completion.
TEST(CustomGeometryTile, CancelAfterPublishStillReachesProducer) {
    CustomTileTest test;
    std::size_t cancelCount = 0;
    CustomTileLoader loader([](const CanonicalTileID&) {},
                            [&](const CanonicalTileID&) { ++cancelCount; });
    auto loaderMailbox = std::make_shared<Mailbox>(*Scheduler::GetCurrent());
    ActorRef<CustomTileLoader> loaderActor(loader, loaderMailbox);
    const OverscaledTileID tileID(0, 0, 0);
    CustomGeometryTile tile(
        tileID, "source", test.tileParameters, makeMutable<CustomGeometrySource::TileOptions>(), loaderActor);
    auto tileMailbox = std::make_shared<Mailbox>(*Scheduler::GetCurrent());
    ActorRef<CustomGeometryTile> tileActor(tile, tileMailbox);
    const auto token = nextCustomTileRegistrationToken();

    loader.fetchTile(tileID, tileActor, token);
    auto features = std::make_shared<FeatureCollection>();
    features->emplace_back(mapbox::geometry::point<double>(0, 0));
    loader.setTileFeatures(tileID.canonical, features);
    EXPECT_EQ(0u, cancelCount);

    loader.removeTile(tileID, token);
    EXPECT_EQ(1u, cancelCount) << "the producer may still hold work for this tile";

    tileMailbox->close();
}

// invalidateRegion is the style reload path, it must not keep an entry per
// canonical tile it ever touched.
TEST(CustomGeometryTile, InvalidateRegionReapsEmptyEntriesAndCancelsOnce) {
    CustomTileTest test;
    std::size_t fetchCount = 0;
    std::size_t cancelCount = 0;
    CustomTileLoader loader([&](const CanonicalTileID&) { ++fetchCount; },
                            [&](const CanonicalTileID&) { ++cancelCount; });
    auto loaderMailbox = std::make_shared<Mailbox>(*Scheduler::GetCurrent());
    ActorRef<CustomTileLoader> loaderActor(loader, loaderMailbox);
    const OverscaledTileID tileID(0, 0, 0);
    CustomGeometryTile firstTile(
        tileID, "source", test.tileParameters, makeMutable<CustomGeometrySource::TileOptions>(), loaderActor);
    CustomGeometryTile secondTile(
        tileID, "source", test.tileParameters, makeMutable<CustomGeometrySource::TileOptions>(), loaderActor);
    auto firstMailbox = std::make_shared<Mailbox>(*Scheduler::GetCurrent());
    auto secondMailbox = std::make_shared<Mailbox>(*Scheduler::GetCurrent());
    ActorRef<CustomGeometryTile> firstActor(firstTile, firstMailbox);
    ActorRef<CustomGeometryTile> secondActor(secondTile, secondMailbox);
    const auto firstToken = nextCustomTileRegistrationToken();
    const auto secondToken = nextCustomTileRegistrationToken();

    loader.fetchTile(tileID, firstActor, firstToken);
    loader.fetchTile(tileID, secondActor, secondToken);
    loader.invalidateRegion(LatLngBounds::world(), {0, 22});
    EXPECT_EQ(1u, cancelCount) << "one cancel for the canonical tile, not one per receiver";

    // The entry is gone, so a late remove finds nothing and re-registering works.
    const auto before = getCustomTileLoaderRegistrationStats();
    loader.removeTile(tileID, firstToken);
    const auto after = getCustomTileLoaderRegistrationStats();
    EXPECT_EQ(before.staleRemovesIgnored, after.staleRemovesIgnored)
        << "an erased entry is not a stale registration";

    const auto fetchesBefore = fetchCount;
    loader.fetchTile(tileID, secondActor, secondToken);
    EXPECT_EQ(fetchesBefore + 1, fetchCount) << "a receiver that re-registers must get a fresh fetch";

    firstMailbox->close();
    secondMailbox->close();
}

// Both an old and a new receiver of one tile ID must get the data.
TEST(CustomGeometryTile, PublicationReachesBothReceiversOfOneTileId) {
    CustomTileTest test;
    CustomTileLoader loader([](const CanonicalTileID&) {}, [](const CanonicalTileID&) {});
    auto loaderMailbox = std::make_shared<Mailbox>(*Scheduler::GetCurrent());
    ActorRef<CustomTileLoader> loaderActor(loader, loaderMailbox);
    const OverscaledTileID tileID(0, 0, 0);
    CustomGeometryTile firstTile(
        tileID, "source", test.tileParameters, makeMutable<CustomGeometrySource::TileOptions>(), loaderActor);
    CustomGeometryTile secondTile(
        tileID, "source", test.tileParameters, makeMutable<CustomGeometrySource::TileOptions>(), loaderActor);
    auto firstMailbox = std::make_shared<Mailbox>(*Scheduler::GetCurrent());
    auto secondMailbox = std::make_shared<Mailbox>(*Scheduler::GetCurrent());
    ActorRef<CustomGeometryTile> firstActor(firstTile, firstMailbox);
    ActorRef<CustomGeometryTile> secondActor(secondTile, secondMailbox);

    CircleLayer layer("circle", "source");
    Immutable<LayerProperties> layerProperties = makeMutable<CircleLayerProperties>(
        staticImmutableCast<CircleLayer::Impl>(layer.baseImpl));
    std::vector<Immutable<LayerProperties>> layers{layerProperties};
    firstTile.setLayers(layers);
    secondTile.setLayers(layers);

    loader.fetchTile(tileID, firstActor, nextCustomTileRegistrationToken());
    loader.fetchTile(tileID, secondActor, nextCustomTileRegistrationToken());

    auto features = std::make_shared<FeatureCollection>();
    features->emplace_back(mapbox::geometry::point<double>(0, 0));
    loader.setTileFeatures(tileID.canonical, features);
    ASSERT_TRUE(test.waitUntil([&] { return firstTile.isComplete() && secondTile.isComplete(); }));
    EXPECT_TRUE(firstTile.isRenderable());
    EXPECT_TRUE(secondTile.isRenderable());

    firstMailbox->close();
    secondMailbox->close();
}

// The destructor must name this tile's own registration, not just the tile ID.
TEST(CustomGeometryTile, DestructorRemovesOnlyItsOwnRegistration) {
    CustomTileTest test;
    std::size_t fetchCount = 0;
    std::size_t cancelCount = 0;
    CustomTileLoader loader([&](const CanonicalTileID&) { ++fetchCount; },
                            [&](const CanonicalTileID&) { ++cancelCount; });
    auto loaderMailbox = std::make_shared<Mailbox>(*Scheduler::GetCurrent());
    ActorRef<CustomTileLoader> loaderActor(loader, loaderMailbox);
    const OverscaledTileID tileID(0, 0, 0);
    CustomGeometryTile survivor(
        tileID, "source", test.tileParameters, makeMutable<CustomGeometrySource::TileOptions>(), loaderActor);
    auto survivorMailbox = std::make_shared<Mailbox>(*Scheduler::GetCurrent());
    ActorRef<CustomGeometryTile> survivorActor(survivor, survivorMailbox);
    const auto survivorToken = nextCustomTileRegistrationToken();
    loader.fetchTile(tileID, survivorActor, survivorToken);

    CircleLayer layer("circle", "source");
    Immutable<LayerProperties> layerProperties = makeMutable<CircleLayerProperties>(
        staticImmutableCast<CircleLayer::Impl>(layer.baseImpl));
    survivor.setLayers({layerProperties});

    {
        CustomGeometryTile doomed(
            tileID, "source", test.tileParameters, makeMutable<CustomGeometrySource::TileOptions>(), loaderActor);
        doomed.setNecessity(TileNecessity::Required);
        ASSERT_TRUE(test.waitUntil([&] { return fetchCount == 2; }));
    }

    auto features = std::make_shared<FeatureCollection>();
    features->emplace_back(mapbox::geometry::point<double>(0, 0));
    features->back().properties["receiver"] = std::string("survivor");
    const auto beforePublish = getCustomTileLoaderDataCacheStats();
    // Publication follows the destructor message in the same loader mailbox.
    auto publication = loaderActor.ask(&CustomTileLoader::setTileFeatures, tileID.canonical, features);
    ASSERT_TRUE(test.waitUntil([&] { return publication.wait_for(Duration::zero()) == std::future_status::ready; }));
    publication.get();
    EXPECT_EQ(0u, cancelCount) << "the surviving receiver still wants the tile";
    EXPECT_EQ(beforePublish.stores + 1, getCustomTileLoaderDataCacheStats().stores);

    ASSERT_TRUE(test.waitUntil([&] { return survivor.isComplete(); }));
    ASSERT_TRUE(survivor.isRenderable());
    std::vector<Feature> delivered;
    survivor.querySourceFeatures(delivered, {});
    ASSERT_EQ(1u, delivered.size());
    EXPECT_EQ(features->front().properties, delivered.front().properties);

    loader.removeTile(tileID, survivorToken);
    EXPECT_EQ(1u, cancelCount) << "the destructor must have removed the other registration";
    const auto afterRemove = getCustomTileLoaderDataCacheStats();
    EXPECT_EQ(beforePublish.tileCount, afterRemove.tileCount);
    loader.setTileFeatures(tileID.canonical, features);
    EXPECT_EQ(afterRemove.stores, getCustomTileLoaderDataCacheStats().stores);
    EXPECT_EQ(afterRemove.bypasses + 1, getCustomTileLoaderDataCacheStats().bypasses);

    survivorMailbox->close();
}

TEST(CustomGeometryTile, MemoSurvivesDeferredPatternAndSymbolLayoutAndQueries) {
  std::vector<Feature> baselineQuery;
  for (bool memoize : {false, true}) {
    tiletrace::configure(true, true);
    layouttiming::configure(true, true);
    Scoped timingCleanup([] { layouttiming::configure(false, true); tiletrace::configure(false, true); });
    auto backend = gfx::HeadlessBackend::Create();
    gfx::BackendScope backendScope{*backend->getRendererBackend()};
    CustomTileTest test;
    test.dynamicTextureAtlas = std::make_shared<gfx::DynamicTextureAtlas>(backend->getRendererBackend()->getContext());
    test.tileParameters.dynamicTextureAtlas = test.dynamicTextureAtlas;
    struct DeferredImages : ImageManagerObserver {
      std::vector<std::function<void()>> completions;
      void onStyleImageMissing(const std::string&, const std::function<void()>& done) override {
        completions.push_back(done);
      }
    } images;
    test.imageManager->setObserver(&images);
    test.imageManager->setLoaded(true);
    auto observer = std::make_shared<test::GeometryMemoTestObserver>();
    auto options = makeMutable<CustomGeometrySource::TileOptions>();
    options->memoizeGeometry = memoize;
    options->geometryMemoObserver = observer;
    CustomTileLoader loader(nullptr, nullptr);
    auto mailbox = std::make_shared<Mailbox>(*Scheduler::GetCurrent());
    ActorRef<CustomTileLoader> loaderActor(loader, mailbox);
    struct InspectableTile : CustomGeometryTile {
      using CustomGeometryTile::CustomGeometryTile;
      using GeometryTile::getLayerRenderData;
      using GeometryTile::getData;
    } tile(OverscaledTileID(0, 0, 0), "source-a", test.tileParameters,
           std::move(options), loaderActor);
    FillLayer fill("fill", "source-a");
    fill.setFillPattern(expression::Image("pattern"));
    SymbolLayer symbol("symbol", "source-a");
    symbol.setIconImage(expression::Image("icon"));
    auto fillMutable = makeMutable<FillLayerProperties>(
      staticImmutableCast<FillLayer::Impl>(fill.baseImpl));
    fillMutable->evaluated.get<FillPattern>() = PossiblyEvaluatedPropertyValue<Faded<expression::Image>>(
      Faded<expression::Image>{.from = "pattern", .to = "pattern"});
    Immutable<LayerProperties> fillProperties = std::move(fillMutable);
    Immutable<LayerProperties> symbolProperties = makeMutable<SymbolLayerProperties>(
      staticImmutableCast<SymbolLayer::Impl>(symbol.baseImpl));
    tile.setLayers({fillProperties, symbolProperties});
    FeatureCollection features;
    features.emplace_back(mapbox::geometry::polygon<double>{{{-5, -5}, {5, -5}, {5, 5}, {-5, 5}, {-5, -5}}});
    features.back().properties["name"] = std::string("polygon");
    features.emplace_back(mapbox::geometry::point<double>{0, 0});
    features.back().properties["name"] = std::string("point");
    auto trace = tiletrace::create(1, 2, 3, 0, 0, 0, 0, 0);
    tile.setTracedTileData(CustomGeometryTile::processTileData(features, CanonicalTileID(0, 0, 0), {}), trace);
    ASSERT_TRUE(test.waitUntil([&] { return images.completions.size() == 2; }));
    EXPECT_FALSE(tile.isComplete());
    test.imageManager->addImage(makeMutable<style::Image::Impl>("pattern", PremultipliedImage({16, 16}), 1.0f));
    test.imageManager->addImage(makeMutable<style::Image::Impl>("icon", PremultipliedImage({16, 16}), 1.0f));
    for (const auto& done : images.completions) done();
    ASSERT_TRUE(test.waitUntil([&] { return !tile.hasPendingRequests(); }));
    test.imageManager->notifyIfMissingImageAdded();
    ASSERT_TRUE(test.waitUntil([&] { return tile.isComplete(); }));
    ASSERT_TRUE(tile.isRenderable());
    rapidjson::Document timingSnapshot;
    timingSnapshot.Parse(layouttiming::snapshotJSON(tiletrace::session()).c_str());
    ASSERT_EQ(1u, timingSnapshot["records"].Size());
    const auto& timing = timingSnapshot["records"][0];
    EXPECT_STREQ("accepted", timing["disposition"].GetString());
    EXPECT_TRUE(timing["valid"].GetBool());
    EXPECT_GT(std::stoull(timing["work"]["callback"]["calls"].GetString()), 0u);
    uint64_t measured = 0;
    for (const auto* section : {"work", "gaps"})
      for (const auto& item : timing[section].GetObject())
        measured += std::stoull(item.value["wallUs"].GetString());
    EXPECT_LE(measured, std::stoull(timing["resultPostedUs"].GetString())
                       - std::stoull(timing["workerReceivedUs"].GetString()));
    auto firstLayer = tile.getData()->getLayer("fill");
    auto secondLayer = tile.getData()->getLayer("symbol");
    auto firstFeature = firstLayer->getFeature(0);
    auto secondFeature = secondLayer->getFeature(0);
    EXPECT_EQ(memoize,
              &firstFeature->getGeometries() == &secondFeature->getGeometries());
    auto* fillData = tile.getLayerRenderData(*fill.baseImpl);
    auto* symbolData = tile.getLayerRenderData(*symbol.baseImpl);
    ASSERT_TRUE(fillData && fillData->bucket && fillData->bucket->hasData());
    ASSERT_TRUE(symbolData && symbolData->bucket && symbolData->bucket->hasData());
    auto retainedIndex = tile.getFeatureIndex();
    ASSERT_TRUE(retainedIndex);
    std::vector<Feature> firstQuery;
    tile.querySourceFeatures(firstQuery, {});
    ASSERT_EQ(features.size(), firstQuery.size());
    tile.setLayers({fillProperties, symbolProperties});
    ASSERT_TRUE(test.waitUntil([&] { return tile.isComplete(); }));
    std::vector<Feature> secondQuery;
    tile.querySourceFeatures(secondQuery, {});
    EXPECT_EQ(firstQuery, secondQuery);
    EXPECT_EQ(features.front().properties, secondQuery.front().properties);
    if (!memoize) baselineQuery = secondQuery;
    else EXPECT_EQ(baselineQuery, secondQuery);
    mailbox->close();
  }
}

TEST(CustomGeometryTile, SourceOptionsKeepMemoizationIndependentAcrossCacheHits) {
  CustomTileTest test;
  struct InspectableTile : CustomGeometryTile {
    using CustomGeometryTile::CustomGeometryTile;
    using GeometryTile::getData;
    bool sharesGeometry() const {
      auto firstLayer = getData()->getLayer("first");
      auto secondLayer = getData()->getLayer("second");
      auto first = firstLayer->getFeature(0);
      auto second = secondLayer->getFeature(0);
      return &first->getGeometries() == &second->getGeometries();
    }
  };
  auto enabled = makeMutable<CustomGeometrySource::TileOptions>();
  enabled->memoizeGeometry = true;
  auto observed = makeMutable<CustomGeometrySource::TileOptions>();
  auto observer = std::make_shared<mln::test::GeometryMemoTestObserver>();
  observed->geometryMemoObserver = observer;
  Immutable<CustomGeometrySource::TileOptions> optionsA = std::move(enabled);
  Immutable<CustomGeometrySource::TileOptions> optionsB = std::move(observed);
  std::size_t fetchesA = 0, fetchesB = 0;
  CustomTileLoader loaderA([&](const CanonicalTileID&) { ++fetchesA; }, nullptr, *optionsA);
  CustomTileLoader loaderB([&](const CanonicalTileID&) { ++fetchesB; }, nullptr, *optionsB);
  auto mailboxA = std::make_shared<Mailbox>(*Scheduler::GetCurrent());
  auto mailboxB = std::make_shared<Mailbox>(*Scheduler::GetCurrent());
  ActorRef<CustomTileLoader> actorA(loaderA, mailboxA), actorB(loaderB, mailboxB);
  const OverscaledTileID tileID(0, 0, 0);
  InspectableTile tileA(tileID, "source-a", test.tileParameters, optionsA, actorA);
  InspectableTile tileB(tileID, "source-b", test.tileParameters, optionsB, actorB);
  InspectableTile cachedA(tileID, "source-a", test.tileParameters, optionsA, actorA);
  InspectableTile cachedB(tileID, "source-b", test.tileParameters, optionsB, actorB);
  CircleLayer layerA("a", "source-a"), layerB("b", "source-b");
  Immutable<LayerProperties> propertiesA = makeMutable<CircleLayerProperties>(
    staticImmutableCast<CircleLayer::Impl>(layerA.baseImpl));
  Immutable<LayerProperties> propertiesB = makeMutable<CircleLayerProperties>(
    staticImmutableCast<CircleLayer::Impl>(layerB.baseImpl));
  tileA.setLayers({propertiesA});
  cachedA.setLayers({propertiesA});
  tileB.setLayers({propertiesB});
  cachedB.setLayers({propertiesB});
  std::array<std::shared_ptr<Mailbox>, 4> receivers;
  for (auto& receiver : receivers) receiver = std::make_shared<Mailbox>(*Scheduler::GetCurrent());
  ActorRef<CustomGeometryTile> receiverA(tileA, receivers[0]), receiverB(tileB, receivers[1]);
  ActorRef<CustomGeometryTile> receiverCachedA(cachedA, receivers[2]), receiverCachedB(cachedB, receivers[3]);
  auto features = std::make_shared<FeatureCollection>();
  features->emplace_back(mapbox::geometry::point<double>{0, 0});
  loaderA.fetchTile(tileID, receiverA, nextCustomTileRegistrationToken());
  loaderB.fetchTile(tileID, receiverB, nextCustomTileRegistrationToken());
  loaderA.setTileFeatures(tileID.canonical, features);
  loaderB.setTileFeatures(tileID.canonical, features);
  ASSERT_TRUE(test.waitUntil([&] { return tileA.isComplete() && tileB.isComplete(); }));
  EXPECT_TRUE(tileA.sharesGeometry());
  EXPECT_FALSE(tileB.sharesGeometry());
  const auto hits = getCustomTileLoaderDataCacheStats().hits;
  loaderA.fetchTile(tileID, receiverCachedA, nextCustomTileRegistrationToken());
  loaderB.fetchTile(tileID, receiverCachedB, nextCustomTileRegistrationToken());
  ASSERT_TRUE(test.waitUntil([&] { return cachedA.isComplete() && cachedB.isComplete(); }));
  EXPECT_EQ(hits + 2, getCustomTileLoaderDataCacheStats().hits);
  EXPECT_EQ(1u, fetchesA);
  EXPECT_EQ(1u, fetchesB);
  EXPECT_TRUE(cachedA.sharesGeometry());
  EXPECT_FALSE(cachedB.sharesGeometry());
  EXPECT_GT(observer->materializations.load(), 0u);
  for (auto& receiver : receivers) receiver->close();
}

TEST(CustomGeometryTile, MemoPolicyDoesNotChangeConversionAndIsCopiedAtSourceCreation) {
  CustomTileTest test;
  CustomGeometrySource::Options options;
  CustomGeometrySource source("any-source-name", options);
  EXPECT_FALSE(source.impl().getTileOptions()->memoizeGeometry);
  options.tileOptions.memoizeGeometry = true;
  options.tileOptions.geometryMemoObserver = std::make_shared<mln::test::GeometryMemoTestObserver>();
  EXPECT_FALSE(source.impl().getTileOptions()->memoizeGeometry);
  EXPECT_FALSE(source.impl().getTileOptions()->geometryMemoObserver);
  CustomGeometrySource another("another-name", options);
  EXPECT_TRUE(another.impl().getTileOptions()->memoizeGeometry);

  FeatureCollection features;
  features.emplace_back(mapbox::geometry::polygon<double>{{{-5, -5}, {5, -5}, {5, 5}, {-5, -5}}});
  const CanonicalTileID id(0, 0, 0);
  const auto first = CustomGeometryTile::processTileData(features, id, *source.impl().getTileOptions());
  const auto second = CustomGeometryTile::processTileData(features, id, *another.impl().getTileOptions());
  EXPECT_EQ(*first, *second);
  const auto plain = customTileConversionSpec(*source.impl().getTileOptions(), 0);
  const auto measured = customTileConversionSpec(*another.impl().getTileOptions(), 0);
  EXPECT_EQ(plain.extent, measured.extent);
  EXPECT_EQ(plain.buffer, measured.buffer);
  EXPECT_EQ(plain.tolerance, measured.tolerance);
}

namespace {

CustomGeometrySource::TileOptions nativeOptions()
{
  CustomGeometrySource::TileOptions options;
  options.dataType = CustomGeometrySource::TileDataType::NativeGeometry;
  return options;
}

class InspectableCustomTile : public CustomGeometryTile
{
public:
  using CustomGeometryTile::CustomGeometryTile;
  const GeometryTileData* data() const { return getData(); }
  bool acceptsResult() const { return acceptsPendingDataResult(); }
};

class NativeTileTest : public CustomTileTest
{
public:
  const bool cacheWasEnabled = isCustomTileLoaderDataCacheEnabled();
  CustomGeometrySource::TileOptions options = nativeOptions();
  std::shared_ptr<NativeRequestState> state = std::make_shared<NativeRequestState>(options);
  std::string key = "first-key";
  std::shared_ptr<const int> inputs = std::make_shared<const int>(1);
  std::vector<NativeRequestTicket> requests;
  std::vector<NativeRequestTicket> cancellations;
  std::function<NativeRequestBinding(const CanonicalTileID&)> resolver;
  std::function<void(const CanonicalTileID&, const NativeRequestTicket&)> cancellationAction;
  NativeTileCallbacks callbacks{
    [&](const CanonicalTileID& tile) {
      return resolver ? resolver(tile) : NativeRequestBinding{key, inputs};
    },
    [&](const CanonicalTileID&, const NativeRequestTicket& ticket, tiletrace::Context) { requests.push_back(ticket); },
    [&](const CanonicalTileID& tile, const NativeRequestTicket& ticket) {
      cancellations.push_back(ticket);
      if (cancellationAction) cancellationAction(tile, ticket);
    }};
  CustomTileLoader loader{nullptr, nullptr, options, {}, callbacks, state};
  std::shared_ptr<Mailbox> loaderMailbox = std::make_shared<Mailbox>(*Scheduler::GetCurrent());
  ActorRef<CustomTileLoader> loaderActor{loader, loaderMailbox};

  struct Receiver
  {
    StubTileObserver observer;
    std::unique_ptr<InspectableCustomTile> tile;
    std::shared_ptr<Mailbox> mailbox;
    ActorRef<CustomGeometryTile> actor;
    const CustomTileRegistrationToken token = nextCustomTileRegistrationToken();
    explicit Receiver(NativeTileTest& test, const OverscaledTileID& id)
      : tile(std::make_unique<InspectableCustomTile>(id, "source", test.tileParameters,
             makeMutable<CustomGeometrySource::TileOptions>(test.options), test.loaderActor,
             &observer, test.loader.nativeSourceEpoch())),
        mailbox(std::make_shared<Mailbox>(*Scheduler::GetCurrent())), actor(*tile, mailbox)
    {
      CircleLayer layer("circle", "source");
      tile->setLayers({makeMutable<CircleLayerProperties>(staticImmutableCast<CircleLayer::Impl>(layer.baseImpl))});
    }
    ~Receiver() { observer.tileChanged = {}; observer.tileError = {}; mailbox->close(); }
  };

  NativeTileTest() { setCustomTileLoaderDataCacheEnabled(true); }
  ~NativeTileTest() { loaderMailbox->close(); setCustomTileLoaderDataCacheEnabled(cacheWasEnabled); }

  void fetch(Receiver& receiver)
  {
    loader.fetchTile(receiver.tile->id, receiver.actor, receiver.token);
  }

  NativeTilePayloadPtr payload(const CanonicalTileID& tile, const std::string& name = "native")
  {
    NativeTileBuilder builder({tile, state->contract()});
    builder.appendFinal(FeatureType::Point, {{{4096, 4096}}}, {{"name", name}}, uint64_t{7});
    return std::move(builder).seal();
  }

  void publish(const CanonicalTileID& tile, const NativeRequestTicket& ticket, NativeTilePayloadPtr value)
  {
    loader.setTracedTilePayload(tile, ticket, std::move(value), {});
  }
};

std::shared_ptr<GeometryTile::LayoutResult> emptyLayout()
{
  return std::make_shared<GeometryTile::LayoutResult>(mln::unordered_map<std::string, LayerRenderData>{},
      nullptr, gfx::GlyphAtlas{}, gfx::ImageAtlas{}, nullptr);
}

} // namespace

TEST(CustomGeometryTile, NativeLayoutTimingFollowsAcceptedWorkerResult)
{
  tiletrace::configure(true, true);
  layouttiming::configure(true, true);
  Scoped cleanup([] { layouttiming::configure(false, true); tiletrace::configure(false, true); });
  NativeTileTest test;
  const OverscaledTileID id(0, 0, 0);
  NativeTileTest::Receiver receiver(test, id);
  auto trace = tiletrace::create(1, 2, receiver.token, 0, 0, 0, 0, 0, 2, 0,
                                 tiletrace::PayloadFormat::NativeGeometry);
  test.loader.fetchTracedTile(id, receiver.actor, receiver.token, trace);
  ASSERT_EQ(1u, test.requests.size());
  trace.id = trace.publication = tiletrace::nextID();
  trace.kind = tiletrace::Kind::Publication;
  trace.payloadSession = trace.session;
  tiletrace::mark(trace, tiletrace::Producer);
  tiletrace::mark(trace, tiletrace::Worker);
  tiletrace::mark(trace, tiletrace::Features);
  test.loader.setTracedTilePayload(id.canonical, test.requests.back(), test.payload(id.canonical), trace);
  ASSERT_TRUE(test.waitUntil([&] { return receiver.tile->isComplete(); }));
  rapidjson::Document snapshot;
  snapshot.Parse(layouttiming::snapshotJSON(tiletrace::session()).c_str());
  ASSERT_FALSE(snapshot.HasParseError());
  ASSERT_TRUE(snapshot["available"].GetBool());
  const auto& records = snapshot["records"];
  ASSERT_EQ(1u, records.Size());
  const auto& result = records[0];
  EXPECT_STREQ("accepted", result["disposition"].GetString());
  EXPECT_TRUE(result["valid"].GetBool());
  EXPECT_EQ(std::to_string(trace.publication), result["publication"].GetString());
  EXPECT_EQ(std::to_string(receiver.token), result["consumer"].GetString());
  EXPECT_STRNE("0", result["layoutId"].GetString());
  EXPECT_STREQ(result["resultCorrelation"].GetString(), result["ownerCorrelation"].GetString());
  const char* times[] = {"deliveredUs", "ownerQueuedUs", "workerReceivedUs", "resultPostedUs",
                         "ownerReceivedUs", "ownerAcceptedUs"};
  uint64_t previous = 0;
  for (const auto* name : times)
  {
    const auto value = std::stoull(result[name].GetString());
    EXPECT_GT(value, 0u);
    EXPECT_GE(value, previous);
    previous = value;
  }
  EXPECT_STREQ("1", result["work"]["parse"]["calls"].GetString());
  EXPECT_STREQ("1", result["work"]["finalize"]["calls"].GetString());
  ASSERT_EQ(1u, result["topGroups"].Size());
  EXPECT_STREQ("circle", result["topGroups"][0]["name"].GetString());
  layouttiming::configure(false);
  unsigned changed = 0;
  receiver.observer.tileChanged = [&](const Tile&) { ++changed; };
  CircleLayer updated("updated-circle", "source");
  receiver.tile->setLayers({makeMutable<CircleLayerProperties>(staticImmutableCast<CircleLayer::Impl>(updated.baseImpl))});
  ASSERT_TRUE(test.waitUntil([&] { return changed > 0; }));
  snapshot.Parse(layouttiming::snapshotJSON(tiletrace::session()).c_str());
  EXPECT_EQ(1u, snapshot["records"].Size());
  EXPECT_FALSE(snapshot["enabled"].GetBool());
  EXPECT_STREQ("0", snapshot["stale"].GetString());
}

TEST(CustomGeometryTile, NativeLayoutTimingDisabledKeepsNormalCompletion)
{
  tiletrace::configure(true, true);
  layouttiming::configure(false, true);
  Scoped cleanup([] { tiletrace::configure(false, true); });
  NativeTileTest test;
  const OverscaledTileID id(0, 0, 0);
  NativeTileTest::Receiver receiver(test, id);
  auto trace = tiletrace::create(1, 2, receiver.token, 0, 0, 0, 0, 0);
  test.loader.fetchTracedTile(id, receiver.actor, receiver.token, trace);
  test.loader.setTracedTilePayload(id.canonical, test.requests.back(), test.payload(id.canonical), trace);
  ASSERT_TRUE(test.waitUntil([&] { return receiver.tile->isComplete(); }));
  rapidjson::Document snapshot;
  snapshot.Parse(layouttiming::snapshotJSON(tiletrace::session()).c_str());
  EXPECT_FALSE(snapshot["enabled"].GetBool());
  EXPECT_TRUE(snapshot["records"].Empty());
}

TEST(CustomGeometryTile, NativeLayoutTimingKeepsOwnerRejectionSeparate)
{
  tiletrace::configure(true, true);
  layouttiming::configure(true, true);
  Scoped cleanup([] { layouttiming::configure(false, true); tiletrace::configure(false, true); });
  NativeTileTest test;
  const OverscaledTileID id(0, 0, 0);
  NativeTileTest::Receiver receiver(test, id);
  auto trace = tiletrace::create(1, 2, receiver.token, 0, 0, 0, 0, 0);
  tiletrace::mark(trace, tiletrace::Delivered);
  layouttiming::Tracker timing;
  timing.start(layouttiming::makeSeed(trace, 0), tiletrace::now());
  auto result = emptyLayout();
  result->trace = trace;
  result->trace.kind = tiletrace::Kind::Layout;
  result->timing = timing.result(tiletrace::now());
  receiver.tile->onLayout(std::move(result), 0);
  rapidjson::Document snapshot;
  snapshot.Parse(layouttiming::snapshotJSON(tiletrace::session()).c_str());
  ASSERT_EQ(1u, snapshot["records"].Size());
  EXPECT_STREQ("correlation-rejected", snapshot["records"][0]["disposition"].GetString());
  EXPECT_STREQ("0", snapshot["records"][0]["ownerAcceptedUs"].GetString());
  EXPECT_FALSE(receiver.tile->isComplete());
}

TEST(CustomGeometryTile, NativeCacheSharesPayloadAcrossWrappedAndOverscaledReceivers)
{
  NativeTileTest test;
  const CanonicalTileID canonical(0, 0, 0);
  NativeTileTest::Receiver first(test, OverscaledTileID(0, 0, canonical));
  NativeTileTest::Receiver wrapped(test, OverscaledTileID(0, 1, canonical));
  NativeTileTest::Receiver overzoomed(test, OverscaledTileID(2, 0, canonical));
  test.fetch(first);
  test.fetch(wrapped);
  ASSERT_EQ(2u, test.requests.size());
  EXPECT_EQ(test.requests[0], test.requests[1]);
  auto payload = test.payload(canonical);
  const auto before = getCustomTileLoaderDataCacheStats();
  test.publish(canonical, test.requests[0], payload);
  ASSERT_TRUE(test.waitUntil([&] { return first.tile->isComplete() && wrapped.tile->isComplete(); }));
  EXPECT_EQ(before.stores + 1, getCustomTileLoaderDataCacheStats().stores);
  test.fetch(overzoomed);
  ASSERT_TRUE(test.waitUntil([&] { return overzoomed.tile->isComplete(); }));
  EXPECT_EQ(2u, test.requests.size());
  EXPECT_EQ(before.hits + 1, getCustomTileLoaderDataCacheStats().hits);
  for (auto* receiver : {&first, &wrapped, &overzoomed})
  {
    ASSERT_NE(nullptr, receiver->tile->data());
    auto layer = receiver->tile->data()->getLayer("any-name");
    ASSERT_EQ(1u, layer->featureCount());
    auto feature = layer->getFeature(0);
    EXPECT_EQ(&payload->features()[0].geometry, &feature->getGeometries());
    EXPECT_EQ(payload->features()[0].properties, feature->getProperties());
    std::vector<Feature> queried;
    receiver->tile->querySourceFeatures(queried, {});
    ASSERT_EQ(1u, queried.size());
    EXPECT_EQ(uint64_t{7}, queried[0].id.get<uint64_t>());
  }
}

TEST(CustomGeometryTile, NativeCacheOffClearAndDroppedAttemptsKeepFetchSemantics)
{
  NativeTileTest test;
  const OverscaledTileID id(0, 0, 0);
  NativeTileTest::Receiver first(test, id), second(test, id);
  test.fetch(first);
  test.fetch(first);
  ASSERT_EQ(2u, test.requests.size());
  EXPECT_EQ(test.requests[0], test.requests[1]);
  setCustomTileLoaderDataCacheEnabled(false);
  const auto before = getCustomTileLoaderDataCacheStats();
  test.publish(id.canonical, test.requests[0], test.payload(id.canonical));
  ASSERT_TRUE(test.waitUntil([&] { return first.tile->isComplete(); }));
  EXPECT_EQ(before.stores, getCustomTileLoaderDataCacheStats().stores);
  test.fetch(second);
  ASSERT_EQ(3u, test.requests.size());
  EXPECT_EQ(test.requests[0], test.requests[2]);
  setCustomTileLoaderDataCacheEnabled(true);
  test.publish(id.canonical, test.requests[2], test.payload(id.canonical));
  ASSERT_TRUE(test.waitUntil([&] { return second.tile->isComplete(); }));
  test.loader.clearDataCache();
  EXPECT_FALSE(test.requests[0].isCurrent());
  test.fetch(first);
  ASSERT_EQ(4u, test.requests.size());
  EXPECT_TRUE(test.requests.back().isCurrent());
  EXPECT_NE(test.requests[0], test.requests.back());
}

TEST(CustomGeometryTile, NativeCancellationAndRemovalKeepOtherReceivers)
{
  NativeTileTest test;
  const OverscaledTileID id(0, 0, 0);
  NativeTileTest::Receiver first(test, id), second(test, id);
  test.fetch(first);
  test.fetch(second);
  test.loader.cancelTile(id, first.token);
  test.loader.removeTile(id, first.token);
  EXPECT_TRUE(test.cancellations.empty());
  const auto before = getCustomTileLoaderRegistrationStats();
  test.loader.removeTile(id, first.token);
  EXPECT_EQ(before.staleRemovesIgnored + 1, getCustomTileLoaderRegistrationStats().staleRemovesIgnored);
  test.publish(id.canonical, test.requests[0], test.payload(id.canonical));
  ASSERT_TRUE(test.waitUntil([&] { return second.tile->isComplete(); }));
  EXPECT_FALSE(first.tile->isComplete());
  test.loader.cancelTile(id, second.token);
  ASSERT_EQ(1u, test.cancellations.size());
  EXPECT_EQ(test.requests[0], test.cancellations[0]);
  EXPECT_TRUE(test.requests[0].isCurrent());
}

TEST(CustomGeometryTile, NativeChangedKeyRejectsOldLoaderAndQueuedOwnerDeliveries)
{
  NativeTileTest test;
  const OverscaledTileID id(0, 0, 0);
  NativeTileTest::Receiver first(test, id), replacement(test, id);
  test.fetch(first);
  const auto old = test.requests.back();
  auto oldPayload = test.payload(id.canonical, "old");
  test.publish(id.canonical, old, oldPayload);
  test.key = "new-key";
  test.inputs = std::make_shared<const int>(2);
  test.fetch(replacement);
  ASSERT_EQ(2u, test.requests.size());
  EXPECT_FALSE(old.isCurrent());
  const auto before = getCustomTileLoaderDataCacheStats();
  test.publish(id.canonical, old, oldPayload);
  EXPECT_EQ(before.stores, getCustomTileLoaderDataCacheStats().stores);
  auto current = test.payload(id.canonical, "new");
  test.publish(id.canonical, test.requests.back(), current);
  ASSERT_TRUE(test.waitUntil([&] { return replacement.tile->isComplete(); }));
  EXPECT_FALSE(first.tile->isComplete());
  std::vector<Feature> queried;
  replacement.tile->querySourceFeatures(queried, {});
  ASSERT_EQ(1u, queried.size());
  EXPECT_EQ("new", queried[0].properties.at("name").get<std::string>());
}

TEST(CustomGeometryTile, NativeInvalidationRejectsLayoutAndErrorBeforeAcceptance)
{
  NativeTileTest test;
  const OverscaledTileID id(0, 0, 0);
  NativeTileTest::Receiver receiver(test, id);
  test.fetch(receiver);
  const auto ticket = test.requests.back();
  receiver.tile->setNativeTileData(test.payload(id.canonical), ticket, {});
  auto& observer = receiver.observer;
  unsigned changed = 0, errors = 0;
  observer.tileChanged = [&](const Tile&) { ++changed; };
  observer.tileError = [&](const Tile&, std::exception_ptr) { ++errors; };
  receiver.tile->setObserver(&observer);
  test.state->invalidate(id.canonical);
  EXPECT_FALSE(receiver.tile->acceptsResult());
  auto rejected = emptyLayout();
  rejected->trace.id = tiletrace::nextID();
  auto inspected = rejected;
  receiver.tile->onLayout(std::move(rejected), 2);
  EXPECT_EQ(0u, inspected->trace.time[tiletrace::Layout]);
  EXPECT_EQ(tiletrace::Outcome::StaleWorker, inspected->trace.outcome);
  receiver.tile->onError(std::make_exception_ptr(std::runtime_error("old layout")), 2);
  EXPECT_FALSE(receiver.tile->isComplete());
  EXPECT_EQ(0u, changed);
  EXPECT_EQ(0u, errors);
}

TEST(CustomGeometryTile, NativeProducerErrorDoesNotRevalidateOldDataDuringStyleReparse)
{
  NativeTileTest test;
  const OverscaledTileID id(0, 0, 0);
  NativeTileTest::Receiver receiver(test, id);
  test.fetch(receiver);
  const auto first = test.requests.back();
  receiver.tile->setNativeTileData(test.payload(id.canonical, "old"), first, {});
  ASSERT_TRUE(test.waitUntil([&] { return receiver.tile->isComplete(); }));
  test.key = "replacement";
  auto next = test.state->resolve(id.canonical, [&](const auto&) { return NativeRequestBinding{test.key, test.inputs}; }).ticket;
  auto& observer = receiver.observer;
  unsigned changed = 0, errors = 0;
  observer.tileChanged = [&](const Tile&) { ++changed; };
  observer.tileError = [&](const Tile&, std::exception_ptr) { ++errors; };
  receiver.tile->setObserver(&observer);
  receiver.tile->setNativeTileError(next, std::make_exception_ptr(std::runtime_error("producer failed")), {});
  EXPECT_EQ(1u, errors);
  EXPECT_TRUE(receiver.tile->isComplete());
  EXPECT_FALSE(receiver.tile->acceptsResult());
  CircleLayer layer("circle", "source");
  receiver.tile->setLayers({makeMutable<CircleLayerProperties>(staticImmutableCast<CircleLayer::Impl>(layer.baseImpl))});
  receiver.tile->onLayout(emptyLayout(), 4);
  receiver.tile->onError(std::make_exception_ptr(std::runtime_error("old error")), 2);
  EXPECT_EQ(0u, changed);
  EXPECT_EQ(1u, errors);
  receiver.tile->setNativeTileData(test.payload(id.canonical, "new"), next, {});
  ASSERT_TRUE(test.waitUntil([&] { return receiver.tile->isComplete(); }));
  EXPECT_TRUE(receiver.tile->acceptsResult());
  EXPECT_GT(changed, 0u);
  std::vector<Feature> queried;
  receiver.tile->querySourceFeatures(queried, {});
  ASSERT_EQ(1u, queried.size());
  EXPECT_EQ("new", queried[0].properties.at("name").get<std::string>());
}

TEST(CustomGeometryTile, NativeResolverInvalidationNotifiesAnUnregisteredFirstReceiver)
{
  NativeTileTest test;
  const OverscaledTileID id(0, 0, 0);
  NativeTileTest::Receiver receiver(test, id);
  auto& observer = receiver.observer;
  bool changed = false;
  observer.tileChanged = [&](const Tile&) { changed = true; };
  receiver.tile->setObserver(&observer);
  test.resolver = [&](const auto&) { test.loader.clearDataCache(); return NativeRequestBinding{test.key, test.inputs}; };
  test.fetch(receiver);
  EXPECT_TRUE(test.requests.empty());
  ASSERT_TRUE(test.waitUntil([&] { return changed; }));
  test.resolver = {};
  test.fetch(receiver);
  ASSERT_EQ(1u, test.requests.size());
  EXPECT_TRUE(test.requests.back().isCurrent());
}

TEST(CustomGeometryTile, NativeErrorsCompletePendingRequestsAndRejectOldTickets)
{
  NativeTileTest test;
  const OverscaledTileID id(0, 0, 0);
  NativeTileTest::Receiver receiver(test, id);
  auto& observer = receiver.observer;
  unsigned errors = 0;
  observer.tileError = [&](const Tile&, std::exception_ptr) { ++errors; };
  receiver.tile->setObserver(&observer);
  test.resolver = [](const auto&) { return NativeRequestBinding{}; };
  test.fetch(receiver);
  ASSERT_TRUE(test.waitUntil([&] { return errors == 1; }));
  EXPECT_TRUE(receiver.tile->isComplete());
  EXPECT_FALSE(receiver.tile->isRenderable());
  test.resolver = {};
  test.fetch(receiver);
  ASSERT_EQ(1u, test.requests.size());
  const auto old = test.requests[0];
  test.loader.invalidateTile(id.canonical);
  test.fetch(receiver);
  test.loader.setNativeTileError(id.canonical, old, std::make_exception_ptr(std::runtime_error("stale")), {});
  test.publish(id.canonical, test.requests.back(), test.payload(id.canonical));
  ASSERT_TRUE(test.waitUntil([&] { return receiver.tile->isRenderable(); }));
  EXPECT_EQ(1u, errors);
}

TEST(CustomGeometryTile, NativeQueuedOppositeFormatsCannotPopulateTheNewLoader)
{
  NativeTileTest test;
  const OverscaledTileID id(0, 0, 0);
  NativeTileTest::Receiver native(test, id);
  auto geographic = std::make_shared<FeatureCollection>();
  geographic->emplace_back(Point<double>{0, 0}, PropertyMap{{"name", "legacy"}}, uint64_t{99});
  test.loaderActor.invoke(&CustomTileLoader::setTileFeatures, id.canonical, geographic);
  test.fetch(native);
  const auto ticket = test.requests.back();
  auto nativePayload = test.payload(id.canonical);
  const auto before = getCustomTileLoaderDataCacheStats();
  test.publish(id.canonical, ticket, nativePayload);
  ASSERT_TRUE(test.waitUntil([&] { return native.tile->isComplete(); }));
  EXPECT_EQ(before.stores + 1, getCustomTileLoaderDataCacheStats().stores);
  EXPECT_EQ("native", native.tile->data()->getLayer("")->getFeature(0)->getValue("name")->get<std::string>());

  CustomTileLoader legacyLoader(nullptr, nullptr);
  auto loaderMailbox = std::make_shared<Mailbox>(*Scheduler::GetCurrent());
  ActorRef<CustomTileLoader> loaderActor(legacyLoader, loaderMailbox);
  InspectableCustomTile legacy(id, "source", test.tileParameters,
      makeMutable<CustomGeometrySource::TileOptions>(), loaderActor);
  auto receiverMailbox = std::make_shared<Mailbox>(*Scheduler::GetCurrent());
  ActorRef<CustomGeometryTile> receiverActor(legacy, receiverMailbox);
  CircleLayer layer("circle", "source");
  legacy.setLayers({makeMutable<CircleLayerProperties>(staticImmutableCast<CircleLayer::Impl>(layer.baseImpl))});
  legacyLoader.fetchTile(id, receiverActor, nextCustomTileRegistrationToken());
  loaderActor.invoke(&CustomTileLoader::setTracedTilePayload, id.canonical, ticket, nativePayload, tiletrace::Context{});
  loaderActor.invoke(&CustomTileLoader::setTileFeatures, id.canonical, geographic);
  ASSERT_TRUE(test.waitUntil([&] { return legacy.isComplete(); }));
  EXPECT_EQ("legacy", legacy.data()->getLayer("")->getFeature(0)->getValue("name")->get<std::string>());
  EXPECT_TRUE(legacy.acceptsResult());
  receiverMailbox->close();
  loaderMailbox->close();
}

TEST(CustomGeometryTile, NativeSourceEpochRejectsAnotherLiveSourceTicket)
{
  NativeTileTest test;
  const OverscaledTileID id(0, 0, 0);
  NativeTileTest::Receiver receiver(test, id);
  NativeRequestState foreign(test.options);
  auto foreignTicket = foreign.resolve(id.canonical, [&](const auto&) {
    return NativeRequestBinding{test.key, test.inputs};
  }).ticket;
  test.fetch(receiver);
  const auto before = getCustomTileLoaderDataCacheStats();
  auto payload = test.payload(id.canonical);
  test.publish(id.canonical, foreignTicket, payload);
  receiver.tile->setNativeTileData(payload, foreignTicket, {});
  EXPECT_EQ(before.stores, getCustomTileLoaderDataCacheStats().stores);
  EXPECT_FALSE(receiver.tile->isComplete());
  test.publish(id.canonical, test.requests.back(), payload);
  ASSERT_TRUE(test.waitUntil([&] { return receiver.tile->isComplete(); }));
}

TEST(CustomGeometryTile, NativeStyleOnlyReparseKeepsTheCurrentDataToken)
{
  NativeTileTest test;
  const OverscaledTileID id(0, 0, 0);
  NativeTileTest::Receiver receiver(test, id);
  test.fetch(receiver);
  const auto ticket = test.requests.back();
  auto payload = test.payload(id.canonical);
  test.publish(id.canonical, ticket, payload);
  ASSERT_TRUE(test.waitUntil([&] { return receiver.tile->isComplete(); }));
  CircleLayer layer("circle", "source");
  layer.setCircleRadius(10);
  receiver.tile->setLayers({makeMutable<CircleLayerProperties>(staticImmutableCast<CircleLayer::Impl>(layer.baseImpl))});
  ASSERT_TRUE(test.waitUntil([&] { return receiver.tile->isComplete(); }));
  EXPECT_TRUE(ticket.isCurrent());
  EXPECT_TRUE(receiver.tile->acceptsResult());
  EXPECT_EQ(1u, test.requests.size());
  EXPECT_EQ(&payload->features()[0].geometry,
            &receiver.tile->data()->getLayer("")->getFeature(0)->getGeometries());
}

TEST(CustomGeometryTile, NativeCacheHitAfterCaptureResetKeepsValidityAndDoesNotRepeatReadiness)
{
  struct CaptureRestore
  {
    bool enabled = tiletrace::enabled();
    ~CaptureRestore() { tiletrace::configure(enabled, true); }
  } restore;
  tiletrace::configure(false, true);
  NativeTileTest test;
  const OverscaledTileID id(0, 0, 0);
  NativeTileTest::Receiver first(test, id), second(test, id);
  test.fetch(first);
  auto ticket = test.requests.back();
  auto payload = test.payload(id.canonical);
  test.publish(id.canonical, ticket, payload);
  ASSERT_TRUE(test.waitUntil([&] { return first.tile->isComplete(); }));
  tiletrace::configure(true, true);
  auto demand = tiletrace::create(11, 22, second.token, 0, 0, 0, 0, 0, 2, 0,
                                 tiletrace::PayloadFormat::NativeGeometry);
  test.loader.fetchTracedTile(id, second.actor, second.token, demand);
  ASSERT_TRUE(test.waitUntil([&] { return second.tile->isComplete(); }));
  EXPECT_TRUE(ticket.isCurrent());
  EXPECT_EQ(1u, test.requests.size());
  const auto& trace = second.tile->data()->trace;
  EXPECT_EQ(tiletrace::PayloadFormat::NativeGeometry, trace.payloadFormat);
  EXPECT_EQ(tiletrace::Origin::Processed, trace.origin);
  EXPECT_EQ(0u, trace.time[tiletrace::Features]);
  EXPECT_EQ(0u, trace.time[tiletrace::Loader]);
  EXPECT_EQ(0u, trace.time[tiletrace::Converted]);
  EXPECT_EQ(&payload->features()[0].geometry,
            &second.tile->data()->getLayer("")->getFeature(0)->getGeometries());
}

TEST(CustomGeometryTile, DefaultGeometryResultHookPreservesSuccessAndErrorCallbacks)
{
  CustomTileTest test;
  StubTileObserver observer;
  unsigned changed = 0, errors = 0;
  observer.tileChanged = [&](const Tile&) { ++changed; };
  observer.tileError = [&](const Tile&, std::exception_ptr) { ++errors; };
  GeometryTile tile(OverscaledTileID(0, 0, 0), "ordinary", test.tileParameters, &observer);
  tile.onLayout(nullptr, 0);
  EXPECT_TRUE(tile.isComplete());
  EXPECT_TRUE(tile.isRenderable());
  EXPECT_EQ(1u, changed);
  tile.onError(std::make_exception_ptr(std::runtime_error("current")), 0);
  EXPECT_EQ(1u, errors);
  tile.setLayers({});
  tile.onError(std::make_exception_ptr(std::runtime_error("old")), 0);
  EXPECT_EQ(1u, errors);
  tile.onError(std::make_exception_ptr(std::runtime_error("current")), 1);
  EXPECT_EQ(2u, errors);
  EXPECT_TRUE(tile.isComplete());
}

TEST(CustomGeometryTile, NativeCancellationRunsAfterUnlockAndAllowsReentry)
{
  NativeTileTest test;
  const OverscaledTileID id(0, 0, 0);
  NativeTileTest::Receiver receiver(test, id);
  test.fetch(receiver);
  auto old = test.requests.back();
  test.cancellationAction = [&](const CanonicalTileID&, const NativeRequestTicket&) { test.loader.clearDataCache(); };
  test.loader.cancelTile(id, receiver.token);
  ASSERT_EQ(1u, test.cancellations.size());
  EXPECT_EQ(old, test.cancellations[0]);
  EXPECT_FALSE(old.isCurrent());
  test.cancellationAction = {};
  test.fetch(receiver);
  ASSERT_EQ(2u, test.requests.size());
  EXPECT_TRUE(test.requests.back().isCurrent());
}

TEST(CustomGeometryTile, NativeThrowingCancelDoesNotAbortReplacementOrOtherCancellations)
{
  NativeTileTest test;
  NativeTileTest::Receiver first(test, OverscaledTileID(0, 0, 0)), second(test, OverscaledTileID(1, 0, 0));
  test.fetch(first);
  const auto old = test.requests.back();
  test.cancellationAction = [](const CanonicalTileID&, const NativeRequestTicket&) {
    throw std::runtime_error("synthetic cancel failure");
  };
  test.key = "replacement";
  EXPECT_NO_THROW(test.fetch(first));
  ASSERT_EQ(2u, test.requests.size());
  EXPECT_FALSE(old.isCurrent());
  EXPECT_TRUE(test.requests.back().isCurrent());
  test.fetch(second);
  ASSERT_EQ(3u, test.requests.size());
  EXPECT_NO_THROW(test.loader.clearDataCache());
  EXPECT_EQ(3u, test.cancellations.size());
  for (const auto& request : test.requests) EXPECT_FALSE(request.isCurrent());
}

TEST(CustomGeometryTile, NativeErrorFanoutRebasesCaptureAndReceiverIdentity)
{
  const auto capture = tiletrace::enabled();
  Scoped restore([&] { tiletrace::configure(capture, true); });
  for (int mode = 0; mode < 3; ++mode)
  {
    tiletrace::configure(mode != 0, true);
    NativeTileTest test;
    const CanonicalTileID canonical(0, 0, 0);
    NativeTileTest::Receiver first(test, OverscaledTileID(0, 0, canonical));
    NativeTileTest::Receiver wrapped(test, OverscaledTileID(2, 1, canonical));
    NativeTileTest::Receiver overzoomed(test, OverscaledTileID(3, 0, canonical));
    auto original = tiletrace::create(11, 22, first.token, 0, 0, 0, 0, 0, 2, 0,
                                     tiletrace::PayloadFormat::NativeGeometry);
    test.loader.fetchTracedTile(first.tile->id, first.actor, first.token, original);
    const auto ticket = test.requests.back();
    if (original.id)
    {
      original.id = original.publication = tiletrace::nextID();
      original.kind = tiletrace::Kind::Publication;
      original.origin = tiletrace::Origin::Fresh;
      tiletrace::mark(original, tiletrace::Producer);
    }
    if (mode != 2) tiletrace::configure(true, true);
    auto wrapDemand = tiletrace::create(11, 22, wrapped.token, 0, 0, 0, 2, 1, 3, 0,
                                       tiletrace::PayloadFormat::NativeGeometry);
    auto zoomDemand = tiletrace::create(11, 22, overzoomed.token, 0, 0, 0, 3, 0, 2, 0,
                                       tiletrace::PayloadFormat::NativeGeometry);
    test.loader.fetchTracedTile(wrapped.tile->id, wrapped.actor, wrapped.token, wrapDemand);
    test.loader.fetchTracedTile(overzoomed.tile->id, overzoomed.actor, overzoomed.token, zoomDemand);
    test.loader.setNativeTileError(canonical, ticket,
        std::make_exception_ptr(std::runtime_error("producer error")), original);
    ASSERT_TRUE(test.waitUntil([&] { return wrapped.tile->isComplete() && overzoomed.tile->isComplete(); }));
    rapidjson::Document document;
    document.Parse(tiletrace::snapshotJSON());
    ASSERT_FALSE(document.HasParseError());
    for (const auto& expected : {wrapDemand, zoomDemand})
    {
      bool demandFound = false, layoutFound = false;
      for (const auto& record : document["records"].GetArray())
      {
        const auto id = std::stoull(record["id"].GetString());
        const auto demand = std::stoull(record["demand"].GetString());
        if (id == expected.id || (demand == expected.id && record["kind"].GetUint() == unsigned(tiletrace::Kind::Layout)))
        {
          EXPECT_STREQ("error", record["outcome"].GetString());
          EXPECT_STREQ("native-geometry", record["payloadFormat"].GetString());
          EXPECT_STREQ("22", record["source"].GetString());
          EXPECT_EQ(expected.role, record["role"].GetUint());
          EXPECT_EQ(expected.wrap, record["wrap"].GetInt());
          EXPECT_EQ(expected.overscaledZ, record["overscaledZ"].GetUint());
          EXPECT_EQ(expected.time[tiletrace::Request], std::stoull(record["timesUs"][tiletrace::Request].GetString()));
          demandFound |= id == expected.id;
          layoutFound |= record["kind"].GetUint() == unsigned(tiletrace::Kind::Layout);
        }
        if (mode == 2 && id == original.id)
          EXPECT_STREQ("0", record["timesUs"][tiletrace::Delivered].GetString());
      }
      EXPECT_TRUE(demandFound);
      EXPECT_TRUE(layoutFound);
    }
  }
}

TEST(CustomGeometryTile, NativePublicSourceRetiresTicketsBeforeItsLoaderDrains)
{
  CustomTileTest test;
  const OverscaledTileID id(0, 0, 0);
  for (int operation = 0; operation < 4; ++operation)
  {
    std::promise<NativeRequestTicket> issued;
    auto ticketFuture = issued.get_future();
    std::promise<void> resume;
    auto resumedFuture = resume.get_future();
    bool resumed = false;
    CustomGeometrySource::Options options;
    options.tileOptions = nativeOptions();
    options.nativeCallbacks.resolve = [](const CanonicalTileID&) {
      return NativeRequestBinding{"public-source", std::make_shared<const int>(1)};
    };
    options.nativeCallbacks.fetch = [&](const CanonicalTileID&, const NativeRequestTicket& ticket, tiletrace::Context) {
      issued.set_value(ticket);
      resumedFuture.wait();
    };
    options.nativeCallbacks.cancel = [](const CanonicalTileID&, const NativeRequestTicket&) {};
    auto source = std::make_unique<CustomGeometrySource>("public-source", options);
    Scoped unblock([&] { if (!resumed) { resumed = true; resume.set_value(); } });
    source->loadDescription(*test.fileSource);
    const auto loader = *source->impl().getTileLoader();
    InspectableCustomTile receiver(id, "public-source", test.tileParameters,
        source->impl().getTileOptions(), loader, nullptr, source->impl().getNativeSourceEpoch());
    auto mailbox = std::make_shared<Mailbox>(*Scheduler::GetCurrent());
    ActorRef<CustomGeometryTile> actor(receiver, mailbox);
    loader.invoke(&CustomTileLoader::fetchTile, id, actor, nextCustomTileRegistrationToken());
    ASSERT_EQ(std::future_status::ready, ticketFuture.wait_for(std::chrono::seconds(2)));
    auto ticket = ticketFuture.get();
    ASSERT_TRUE(ticket.isCurrent());
    NativeTileBuilder builder({id.canonical, nativeTileConversionContract(options.tileOptions)});
    source->setTracedTilePayload(id.canonical, ticket, std::move(builder).seal());
    if (operation == 3)
    {
      auto release = std::async(std::launch::async, [&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (ticket.isCurrent() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        const bool retired = !ticket.isCurrent();
        resume.set_value();
        return retired;
      });
      source.reset();
      resumed = true;
      EXPECT_TRUE(release.get());
    }
    else
    {
      if (operation == 0) source->invalidateTile(id.canonical);
      if (operation == 1) source->invalidateRegion(LatLngBounds(id.canonical));
      if (operation == 2) source->clearTileCache();
      EXPECT_FALSE(ticket.isCurrent());
      resumed = true;
      resume.set_value();
    }
    mailbox->close();
  }
}

TEST(CustomGeometryTile, NativeReplacementDoesNotFetchAfterReentrantReceiverCancellationOrRemoval)
{
  for (bool remove : {false, true})
  {
    NativeTileTest test;
    const OverscaledTileID id(0, 0, 0);
    NativeTileTest::Receiver receiver(test, id);
    test.fetch(receiver);
    const auto old = test.requests.back();
    const auto issued = getCustomTileLoaderRegistrationStats().fetchesIssued;
    test.cancellationAction = [&](const CanonicalTileID&, const NativeRequestTicket& ticket) {
      if (ticket != old) return;
      if (remove) test.loader.removeTile(id, receiver.token);
      else test.loader.cancelTile(id, receiver.token);
    };
    test.key = "replacement";
    test.fetch(receiver);
    EXPECT_EQ(1u, test.requests.size());
    EXPECT_EQ(issued, getCustomTileLoaderRegistrationStats().fetchesIssued);
    ASSERT_EQ(2u, test.cancellations.size());
    EXPECT_TRUE(test.cancellations.back().isCurrent());
    const auto cancelled = test.cancellations.back();
    test.cancellationAction = {};
    test.fetch(receiver);
    ASSERT_EQ(2u, test.requests.size());
    EXPECT_EQ(cancelled, test.requests.back());
  }
}

TEST(CustomGeometryTile, NativeLateProducerErrorDoesNotDiscardSuccessfulPendingLayoutOrCache)
{
  const auto capture = tiletrace::enabled();
  Scoped restore([&] { tiletrace::configure(capture, true); });
  for (bool cached : {false, true})
  {
    for (bool publicationID : {false, true})
    {
      tiletrace::configure(true, true);
      NativeTileTest test;
      setCustomTileLoaderDataCacheEnabled(cached);
      const OverscaledTileID id(0, 0, 0);
      NativeTileTest::Receiver receiver(test, id);
      unsigned errors = 0;
      receiver.observer.tileError = [&](const Tile&, std::exception_ptr) { ++errors; };
      auto demand = tiletrace::create(11, 22, receiver.token, 0, 0, 0, 0, 0, 2, 0,
                                     tiletrace::PayloadFormat::NativeGeometry);
      test.loader.fetchTracedTile(id, receiver.actor, receiver.token, demand);
      const auto ticket = test.requests.back();
      auto trace = demand;
      if (publicationID)
      {
        trace.id = trace.publication = tiletrace::nextID();
        trace.kind = tiletrace::Kind::Publication;
        tiletrace::mark(trace, tiletrace::Producer);
      }
      tiletrace::markNativeReady(trace);
      auto payload = test.payload(id.canonical);
      test.loader.setTracedTilePayload(id.canonical, ticket, payload, trace);
      test.loader.setNativeTileError(id.canonical, ticket,
          std::make_exception_ptr(std::runtime_error("late same-publication failure")), trace);
      test.loader.setNativeTileError(id.canonical, ticket,
          std::make_exception_ptr(std::runtime_error("late admission failure")), demand);
      auto other = trace;
      other.id = other.publication = tiletrace::nextID();
      other.kind = tiletrace::Kind::Publication;
      test.loader.setNativeTileError(id.canonical, ticket,
          std::make_exception_ptr(std::runtime_error("separate failed attempt")), other);
      ASSERT_TRUE(test.waitUntil([&] { return receiver.tile->isComplete(); }));
      EXPECT_TRUE(receiver.tile->isRenderable());
      EXPECT_EQ(0u, errors);
      EXPECT_EQ(&payload->features()[0].geometry,
                &receiver.tile->data()->getLayer("")->getFeature(0)->getGeometries());
      rapidjson::Document document;
      document.Parse(tiletrace::snapshotJSON());
      ASSERT_FALSE(document.HasParseError());
      bool separateError = false;
      for (const auto& record : document["records"].GetArray())
      {
        const auto recordID = std::stoull(record["id"].GetString());
        if (recordID == trace.id || recordID == demand.id)
          EXPECT_STRNE("error", record["outcome"].GetString());
        if (recordID == other.id)
        {
          EXPECT_STREQ("error", record["outcome"].GetString());
          separateError = true;
        }
      }
      EXPECT_TRUE(separateError);
      const auto before = getCustomTileLoaderDataCacheStats();
      NativeTileTest::Receiver second(test, id);
      test.fetch(second);
      if (cached)
      {
        ASSERT_TRUE(test.waitUntil([&] { return second.tile->isComplete(); }));
        EXPECT_EQ(before.hits + 1, getCustomTileLoaderDataCacheStats().hits);
        EXPECT_EQ(1u, test.requests.size());
      }
      else
      {
        EXPECT_EQ(2u, test.requests.size());
        test.loader.setNativeTileError(id.canonical, ticket,
            std::make_exception_ptr(std::runtime_error("receiver without bytes failed")), trace);
        ASSERT_TRUE(test.waitUntil([&] { return second.tile->isComplete(); }));
        EXPECT_FALSE(second.tile->isRenderable());
      }
      receiver.tile->onError(std::make_exception_ptr(std::runtime_error("real layout failure")), 2);
      EXPECT_EQ(1u, errors);
    }
  }
}
