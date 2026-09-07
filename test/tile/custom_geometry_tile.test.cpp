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
#include <mln/annotation/annotation_manager.hpp>
#include <mln/renderer/image_manager.hpp>
#include <mln/text/glyph_manager.hpp>
#include <mln/gfx/dynamic_texture_atlas.hpp>

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
                         .geometryTileZoomState = {}},
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
