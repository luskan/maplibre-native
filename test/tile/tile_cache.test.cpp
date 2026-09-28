#include <mln/test/util.hpp>

#include <mln/tile/tile_cache.hpp>

#include <mln/test/fake_file_source.hpp>
#include <mln/tile/tile_loader_impl.hpp>
#include <mln/tile/vector_tile.hpp>
#include <mln/tile/vector_mvt_tile_data.hpp>

#include <mln/actor/scheduler.hpp>
#include <mln/annotation/annotation_manager.hpp>
#include <mln/geometry/feature_index.hpp>
#include <mln/map/transform.hpp>
#include <mln/renderer/buckets/symbol_bucket.hpp>
#include <mln/renderer/image_manager.hpp>
#include <mln/renderer/query.hpp>
#include <mln/renderer/tile_parameters.hpp>
#include <mln/style/layers/symbol_layer.hpp>
#include <mln/style/style.hpp>
#include <mln/test/vector_tile_test.hpp>
#include <mln/text/glyph_manager.hpp>
#include <mln/util/run_loop.hpp>
#include <mln/util/identity.hpp>

#include <memory>
#include <future>

using namespace mln;

namespace {

class InlineReleaseScheduler final : public Scheduler {
public:
    ~InlineReleaseScheduler() override { weak.invalidateWeakPtrs(); }
    void schedule(std::function<void()>&& work) override { ++scheduled; work(); }
    void schedule(util::SimpleIdentity, std::function<void()>&& work) override { schedule(std::move(work)); }
    void waitForEmpty(util::SimpleIdentity) override {}
    mapbox::base::WeakPtr<Scheduler> makeWeakPtr() override { return weak.makeWeakPtr(); }
    size_t scheduled = 0;
private:
    mapbox::base::WeakPtrFactory<Scheduler> weak{this};
};

class VectorTileMock : public VectorTile {
public:
    VectorTileMock(const OverscaledTileID& id_,
                   std::string sourceID_,
                   const TileParameters& parameters,
                   const Tileset& tileset)
        : VectorTile(id_, std::move(sourceID_), parameters, tileset) {
        renderable = true;
    }

    void setData(const std::shared_ptr<const std::string>&) override {}
    ~VectorTileMock() override { if (destroyed) destroyed(); }
    bool allowOptionalRetention() const override { return retainAllowed; }

    util::SimpleIdentity uniqueId;
    bool retainAllowed = true;
    std::function<void()> destroyed;
};

} // namespace

TEST(TileCache, Smoke) {
    VectorTileTest test;
    {
        TileCache cache(test.threadPool, 1);
        const OverscaledTileID id(0, 0, 0);
        auto tile = std::make_unique<VectorTileMock>(id, "source", test.tileParameters, test.tileset);

        cache.add(id, std::move(tile));
        EXPECT_TRUE(cache.has(id));
        cache.clear();
        EXPECT_FALSE(cache.has(id));
    }
}

TEST(TileCache, Issue15926) {
    VectorTileTest test;
    {
        TileCache cache(test.threadPool, 2);
        const OverscaledTileID id0(0, 0, 0);
        const OverscaledTileID id1(1, 0, 0);
        auto tile1 = std::make_unique<VectorTileMock>(id0, "source", test.tileParameters, test.tileset);
        auto tile2 = std::make_unique<VectorTileMock>(id0, "source", test.tileParameters, test.tileset);
        auto tile3 = std::make_unique<VectorTileMock>(id1, "source", test.tileParameters, test.tileset);
        auto tile4 = std::make_unique<VectorTileMock>(id0, "source", test.tileParameters, test.tileset);
        const auto tile1Id = tile1->uniqueId;

        // add
        cache.add(id0, std::move(tile1));
        EXPECT_TRUE(cache.has(id0));

        // adding a key already present doesn't replace the existing item
        cache.add(id0, std::move(tile2));
        EXPECT_EQ(tile1Id, static_cast<VectorTileMock*>(cache.get(id0))->uniqueId);

        // Evict on add
        cache.setSize(1);
        cache.add(id1, std::move(tile3));
        EXPECT_FALSE(cache.has(id0));
        EXPECT_TRUE(cache.has(id1));

        // Evict due to size limit change
        cache.setSize(2);
        cache.add(id0, std::move(tile4));
        EXPECT_TRUE(cache.has(id0));
        EXPECT_TRUE(cache.has(id1));
        cache.setSize(1);
        // older item should be evicted
        EXPECT_TRUE(cache.has(id0));
        EXPECT_FALSE(cache.has(id1));
    }
}

TEST(TileCache, ClearSchedulesDestructionWithoutAnotherSourceUpdate) {
    VectorTileTest test;
    TileCache cache(test.threadPool, 1);
    auto destroyed = std::make_shared<std::promise<void>>();
    auto completion = destroyed->get_future();
    const OverscaledTileID id(0, 0, 0);
    auto tile = std::make_unique<VectorTileMock>(id, "source", test.tileParameters, test.tileset);
    tile->destroyed = [destroyed] { destroyed->set_value(); };
    cache.add(id, std::move(tile));
    cache.clear();
    EXPECT_EQ(std::future_status::ready, completion.wait_for(std::chrono::seconds(5)));
}

TEST(TileCache, DestructionDoesNotStartCleanupOnSameThreadScheduler) {
    VectorTileTest test;
    auto scheduler = std::make_shared<InlineReleaseScheduler>();
    util::SimpleIdentity tag;
    bool destroyed = false;
    {
        TileCache cache(TaggedScheduler(scheduler, tag), 1);
        const OverscaledTileID id(0, 0, 0);
        auto tile = std::make_unique<VectorTileMock>(id, "source", test.tileParameters, test.tileset);
        tile->destroyed = [&] { destroyed = true; };
        cache.add(id, std::move(tile));
    }
    EXPECT_TRUE(destroyed);
    EXPECT_EQ(0u, scheduler->scheduled);
}

TEST(TileCache, UnaccountedTileDeclinesOptionalRetention) {
    VectorTileTest test;
    TileCache cache(test.threadPool, 1);
    const OverscaledTileID id(0, 0, 0);
    auto tile = std::make_unique<VectorTileMock>(id, "source", test.tileParameters, test.tileset);
    tile->retainAllowed = false;
    cache.add(id, std::move(tile));
    EXPECT_FALSE(cache.has(id));
    cache.deferPendingReleases();
}

TEST(TileCache, PolicyChangeDropsPreviouslyRetainedUnaccountedTile) {
    VectorTileTest test;
    TileCache cache(test.threadPool, 1);
    const OverscaledTileID id(0, 0, 0);
    auto tile = std::make_unique<VectorTileMock>(id, "source", test.tileParameters, test.tileset);
    auto* retained = tile.get();
    cache.add(id, std::move(tile));
    ASSERT_TRUE(cache.has(id));
    retained->retainAllowed = false;
    cache.setSize(1);
    EXPECT_FALSE(cache.has(id));
}
