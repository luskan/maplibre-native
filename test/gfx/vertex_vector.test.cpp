#include <mln/gfx/polyline_generator.hpp>
#include <mln/gfx/vertex_vector.hpp>
#include <mln/renderer/buckets/line_bucket.hpp>
#include <mln/test/util.hpp>

using namespace mln::gfx;

TEST(VertexVector, PendingByteRangeFollowsBufferUploads) {
    VertexVector<uint8_t> vertices;
    vertices.extend(64, 0);
    vertices.updateModified();
    vertices.updateModifiedRange(3, 4);
    EXPECT_TRUE(vertices.getModifiedBytes().empty());

    vertices.setBuffer(std::make_unique<VertexBufferBase>());
    vertices.updateModifiedRange(12, 20);
    vertices.updateModifiedRange(4, 8);
    EXPECT_EQ(4u, vertices.getModifiedBytes().begin);
    EXPECT_EQ(20u, vertices.getModifiedBytes().end);

    vertices.recordBufferUpdate();
    EXPECT_TRUE(vertices.getModifiedBytes().empty());
    vertices.updateModifiedRange(40, 48);
    EXPECT_EQ(40u, vertices.getModifiedBytes().begin);
    EXPECT_EQ(48u, vertices.getModifiedBytes().end);
}

TEST(VertexVector, WholeBufferChangesInvalidatePendingByteRanges) {
    VertexVector<uint8_t> vertices;
    vertices.extend(64, 0);
    vertices.updateModified();
    vertices.setBuffer(std::make_unique<VertexBufferBase>());
    vertices.updateModifiedRange(12, 20);

    vertices.at(0) = 1;
    vertices.updateModifiedRange(30, 32);
    EXPECT_TRUE(vertices.getModifiedBytes().empty());
    vertices.updateModifiedRange(40, 48);
    EXPECT_TRUE(vertices.getModifiedBytes().empty());

    vertices.recordBufferUpdate();
    vertices.updateModifiedRange(5, 7);
    EXPECT_EQ(5u, vertices.getModifiedBytes().begin);
    vertices.updateModified(true);
    EXPECT_TRUE(vertices.getModifiedBytes().empty());
}

TEST(PolylineGenerator, TrafficDistancesKeepLongPathsAndDistinguishEndpointCaps) {
    using namespace mln;
    VertexVector<LineLayoutVertex> vertices;
    VertexVector<float> distances;
    std::vector<SegmentBase> segments;
    IndexVector<Triangles> triangles;
    PolylineGenerator<LineLayoutVertex, SegmentBase> generator(vertices, LineBucket::layoutVertex, segments,
        [](std::size_t offset, std::size_t first) { return SegmentBase(offset, first); },
        [](SegmentBase& segment) -> SegmentBase& { return segment; }, triangles);
    PolylineGeneratorOptions options;
    options.type = FeatureType::LineString;
    options.beginCap = options.endCap = style::LineCapType::Round;
    options.pathDistances = &distances;
    generator.generate({{-10000, 0}, {10000, 0}, {10000, 3000}}, options);
    ASSERT_EQ(vertices.elements(), distances.elements());
    bool sawStartCap = false;
    bool sawEndCap = false;
    bool sawJoin = false;
    for (size_t i = 0; i < vertices.elements(); ++i) {
        const auto value = distances.at(i);
        if (value < 0) {
            EXPECT_TRUE(value == -1.0f || value == -23001.0f);
            sawStartCap |= value == -1.0f;
            sawEndCap |= value == -23001.0f;
        } else {
            EXPECT_LE(value, 23000.0f);
            sawJoin |= value == 20000.0f;
        }
    }
    EXPECT_TRUE(sawStartCap);
    EXPECT_TRUE(sawJoin);
    EXPECT_TRUE(sawEndCap);
}
