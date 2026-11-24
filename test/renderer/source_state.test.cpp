#include <mln/test/util.hpp>

#include <mln/renderer/buckets/debug_bucket.hpp>
#include <mln/renderer/paint_property_binder.hpp>
#include <mln/renderer/render_tile.hpp>
#include <mln/renderer/source_state.hpp>
#include <mln/renderer/tile_render_data.hpp>
#include <mln/tile/tile.hpp>

#include <cmath>
#include <map>

using namespace mln;

namespace {

class StubTile final : public Tile {
public:
    explicit StubTile(const OverscaledTileID& id_)
        : Tile(Kind::Geometry, id_, "source") {}
    std::unique_ptr<TileRenderData> createRenderData() override { return nullptr; }
    void cancel() override {}
    bool layerPropertiesUpdated(const Immutable<style::LayerProperties>&) override { return false; }
    void setFeatureState(const LayerFeatureStates& states, FeatureStateUpdate update) override {
        applied.push_back(states);
        updates.push_back(update);
    }

    std::vector<LayerFeatureStates> applied;
    std::vector<FeatureStateUpdate> updates;
};

void renderFrame(SourceFeatureState& state, std::initializer_list<StubTile*> tiles) {
    std::vector<RenderTile> renderTiles;
    for (auto* tile : tiles) {
        renderTiles.emplace_back(tile->id.toUnwrapped(), *tile);
    }
    state.coalesceChanges(renderTiles);
}

} // namespace

TEST(SourceFeatureState, CoalesceChanges) {
    SourceFeatureState state;
    StubTile a({1, 0, 0});
    StubTile b({1, 1, 0});
    const std::optional<std::string> layer{"layer"};

    state.updateState(layer, "1", {{"hover", true}});
    renderFrame(state, {&a});
    ASSERT_EQ(1u, a.applied.size());
    EXPECT_EQ(Value(true), a.applied[0]["layer"]["1"]["hover"]);

    // Nothing changed: nothing is applied again.
    renderFrame(state, {&a});
    EXPECT_EQ(1u, a.applied.size());

    // A tile rendered for the first time gets the stored state.
    renderFrame(state, {&a, &b});
    ASSERT_EQ(1u, b.applied.size());
    EXPECT_EQ(Value(true), b.applied[0]["layer"]["1"]["hover"]);
    EXPECT_EQ(1u, a.applied.size());

    // A tile that is up to date gets only the change.
    state.updateState(layer, "2", {{"hover", true}});
    renderFrame(state, {&a});
    ASSERT_EQ(2u, a.applied.size());
    EXPECT_EQ(1u, a.applied[1]["layer"].size());
    EXPECT_EQ(Value(true), a.applied[1]["layer"]["2"]["hover"]);

    // A tile that was not rendered when the state changed gets the whole state.
    renderFrame(state, {&a, &b});
    EXPECT_EQ(2u, a.applied.size());
    ASSERT_EQ(2u, b.applied.size());
    EXPECT_EQ(2u, b.applied[1]["layer"].size());

    // A removal and a change in the same frame both reach the tile.
    state.removeState(layer, std::string("1"), std::nullopt);
    state.updateState(layer, "3", {{"hover", true}});
    renderFrame(state, {&a, &b});
    ASSERT_EQ(3u, a.applied.size());
    EXPECT_TRUE(a.applied[2]["layer"]["1"].empty());
    EXPECT_EQ(Value(true), a.applied[2]["layer"]["3"]["hover"]);
}

TEST(SourceFeatureState, SkipsRepeatedValuesAndPublishesWholeChangedFeature) {
    SourceFeatureState state;
    StubTile a({1, 0, 0});
    StubTile b({1, 1, 0});
    const std::optional<std::string> layer{"roads"};
    state.updateState(layer, "1", {{"along", uint64_t{20}}, {"reverse", uint64_t{30}}});
    state.updateState(layer, "2", {{"along", uint64_t{40}}});
    renderFrame(state, {&a});
    ASSERT_EQ(1u, a.applied.size());

    state.updateState(layer, "1", {{"along", uint64_t{21}}});
    state.updateState(layer, "1", {{"along", uint64_t{20}}});
    renderFrame(state, {&a, &b});
    EXPECT_EQ(1u, a.applied.size());
    ASSERT_EQ(1u, b.applied.size());
    EXPECT_EQ(b.applied.front(), a.applied.front());

    state.updateState(layer, "1", {{"along", uint64_t{21}}});
    state.updateState(layer, "2", {{"along", uint64_t{40}}});
    renderFrame(state, {&a, &b});
    ASSERT_EQ(2u, a.applied.size());
    EXPECT_EQ(1u, a.applied.back().at("roads").size());
    EXPECT_EQ((FeatureState{{"along", uint64_t{21}}, {"reverse", uint64_t{30}}}),
              a.applied.back().at("roads").at("1"));
    EXPECT_EQ(b.applied.back(), a.applied.back());
}

TEST(SourceFeatureState, PreservesTheSignOfZero) {
    SourceFeatureState state;
    StubTile tile({1, 0, 0});
    const std::optional<std::string> layer{"roads"};
    state.updateState(layer, "1", {{"value", 0.0}});
    renderFrame(state, {&tile});
    state.updateState(layer, "1", {{"value", -0.0}});
    renderFrame(state, {&tile});
    ASSERT_EQ(2u, tile.applied.size());
    const auto* value = tile.applied.back().at("roads").at("1").at("value").getDouble();
    ASSERT_NE(nullptr, value);
    EXPECT_TRUE(std::signbit(*value));
}

TEST(SourceFeatureState, RemovesPairsOfPropertiesFromMultipleFeatures) {
    SourceFeatureState state;
    StubTile tile({1, 0, 0});
    const std::optional<std::string> layer{"roads"};
    for (const std::string id : {"1", "2"}) {
        state.updateState(layer, id, {{"left", 20.0}, {"right", 30.0}, {"keep", true}});
    }
    renderFrame(state, {&tile});
    for (const std::string id : {"1", "2"}) {
        state.removeState(layer, id, "left");
        state.removeState(layer, id, "right");
    }
    renderFrame(state, {&tile});
    ASSERT_EQ(2u, tile.applied.size());
    for (const std::string id : {"1", "2"}) {
        EXPECT_EQ((FeatureState{{"keep", true}}), tile.applied.back().at("roads").at(id));
    }
}

TEST(SourceFeatureState, SnapshotsForgetRemovedFeaturesAndLayers) {
    SourceFeatureState state;
    StubTile cached({1, 0, 0});
    StubTile visible({1, 1, 0});
    const std::optional<std::string> expired{"old"};
    const std::optional<std::string> active{"current"};
    state.updateState(expired, "1", {{"color", true}});
    state.updateState(active, "1", {{"color", false}});
    renderFrame(state, {&cached, &visible});

    state.removeState(expired, std::nullopt, std::nullopt);
    renderFrame(state, {&visible});
    EXPECT_EQ(FeatureStateUpdate::Delta, visible.updates.back());
    EXPECT_TRUE(visible.applied.back().at("old").at("1").empty());
    renderFrame(state, {&cached, &visible});
    EXPECT_EQ(FeatureStateUpdate::Snapshot, cached.updates.back());
    EXPECT_FALSE(cached.applied.back().contains("old"));
    EXPECT_EQ((FeatureState{{"color", false}}), cached.applied.back().at("current").at("1"));

    for (unsigned id = 2; id < 1024; ++id) {
        state.removeState(active, std::to_string(id - 1), "color");
        state.updateState(active, std::to_string(id), {{"color", true}});
        renderFrame(state, {});
    }
    renderFrame(state, {&visible});
    EXPECT_EQ(FeatureStateUpdate::Snapshot, visible.updates.back());
    EXPECT_EQ((FeatureStates{{"1023", {{"color", true}}}}), visible.applied.back().at("current"));

    state.removeState(active, "1023", "color");
    renderFrame(state, {&visible});
    renderFrame(state, {&cached});
    EXPECT_EQ(FeatureStateUpdate::Snapshot, cached.updates.back());
    EXPECT_TRUE(cached.applied.back().empty());
}

TEST(SourceFeatureState, CachedPaintResetsOnlyLocalIdsMissingFromSnapshot) {
    FeaturePaintRanges paint;
    const CanonicalTileID tile{1, 0, 0};
    paint.add("1", {0, 0, 4}, tile, {});
    paint.add("1", {0, 8, 12}, tile, {});
    paint.add("2", {1, 4, 8}, tile, {});
    paint.add("3", {2, 12, 16}, tile, {});
    std::vector<std::pair<std::size_t, bool>> applied;
    const auto assign = [&](const auto& ranges, const FeatureState& state) {
        for (const auto& range : ranges) {
            applied.emplace_back(range.start, !state.empty());
        }
    };
    paint.update({{"1", {{"color", true}}}, {"not-on-tile", {{"color", true}}}}, FeatureStateUpdate::Delta, assign);
    EXPECT_EQ((decltype(applied){{0, true}, {8, true}}), applied);
    applied.clear();

    paint.update({{"2", {{"color", true}}}}, FeatureStateUpdate::Snapshot, assign);
    EXPECT_EQ((decltype(applied){{0, false}, {8, false}, {4, true}}), applied);
    applied.clear();

    paint.update({{"2", {}}}, FeatureStateUpdate::Delta, assign);
    EXPECT_EQ((decltype(applied){{4, false}}), applied);
    applied.clear();

    paint.update({}, FeatureStateUpdate::Snapshot, assign);
    EXPECT_TRUE(applied.empty());
}

TEST(SourceFeatureState, LargeSnapshotAndSmallDeltaUpdateAllLocalRanges) {
    FeaturePaintRanges paint;
    const CanonicalTileID tile{1, 0, 0};
    paint.add("road", {0, 0, 4}, tile, {});
    paint.add("other", {1, 4, 8}, tile, {});
    paint.add("road", {2, 8, 12}, tile, {});

    FeatureStates snapshot;
    for (unsigned i = 0; i < 256; ++i) {
        snapshot.emplace(std::to_string(i), FeatureState{{"speed", 10.0}});
    }
    const FeatureState slow{{"speed", 20.0}};
    const FeatureState fast{{"speed", 80.0}};
    snapshot.emplace("road", slow);
    snapshot.emplace("other", fast);
    std::multimap<std::size_t, FeatureState> writes;
    const auto record = [&](const auto& ranges, const FeatureState& state) {
        for (const auto& range : ranges) writes.emplace(range.start, state);
    };

    paint.update(snapshot, FeatureStateUpdate::Snapshot, record);
    EXPECT_EQ((decltype(writes){{0, slow}, {4, fast}, {8, slow}}), writes);
    writes.clear();

    paint.update({{"road", fast}}, FeatureStateUpdate::Delta, record);
    EXPECT_EQ((decltype(writes){{0, fast}, {8, fast}}), writes);
    writes.clear();

    paint.update({}, FeatureStateUpdate::Snapshot, record);
    EXPECT_EQ((decltype(writes){{0, {}}, {4, {}}, {8, {}}}), writes);
}

TEST(SourceFeatureState, LargeUpdatesClearExplicitAndMissingLocalIds) {
    FeaturePaintRanges paint;
    const CanonicalTileID tile{1, 0, 0};
    paint.add("removed", {0, 0, 4}, tile, {});
    paint.add("empty", {1, 4, 8}, tile, {});
    const FeatureState active{{"color", true}};
    std::multimap<std::size_t, FeatureState> writes;
    const auto record = [&](const auto& ranges, const FeatureState& state) {
        for (const auto& range : ranges) writes.emplace(range.start, state);
    };
    paint.update({{"removed", active}, {"empty", active}}, FeatureStateUpdate::Delta, record);
    writes.clear();

    FeatureStates states{{"empty", {}}, {"away1", active}, {"away2", active}};
    paint.update(states, FeatureStateUpdate::Delta, record);
    EXPECT_EQ((decltype(writes){{4, {}}}), writes);
    writes.clear();

    states.erase("empty");
    states.emplace("away3", active);
    paint.update(states, FeatureStateUpdate::Snapshot, record);
    EXPECT_EQ((decltype(writes){{0, {}}}), writes);
    writes.clear();

    paint.update({}, FeatureStateUpdate::Snapshot, record);
    EXPECT_TRUE(writes.empty());
}
