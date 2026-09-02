#include <mln/test/util.hpp>
#include <mln/tile/geometry_tile_data.hpp>
#include <mln/tile/geometry_tile_data_impl.hpp>

#include <mapbox/geometry/wagyu/wagyu.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>

using namespace mln;

static double _signedArea(const GeometryCoordinates& ring) {
    double sum = 0;

    for (std::size_t i = 0, len = ring.size(), j = len - 1; i < len; j = i++) {
        const GeometryCoordinate& p1 = ring[i];
        const GeometryCoordinate& p2 = ring[j];
        sum += (p2.x - p1.x) * (p1.y + p2.y);
    }

    return sum;
}

static GeometryCollection fixupPolygonsWithWagyu(const GeometryCollection& rings) {
    using namespace mapbox::geometry::wagyu;

    wagyu<int32_t> clipper;
    for (const auto& ring : rings) {
        LinearRing<int32_t> path;
        path.reserve(ring.size());
        for (const auto& point : ring) {
            path.emplace_back(point.x, point.y);
        }
        clipper.add_ring(path);
    }

    MultiPolygon<int16_t> multipolygon;
    clipper.execute(clip_type_union, multipolygon, fill_type_even_odd, fill_type_even_odd);

    GeometryCollection result;
    for (const auto& polygon : multipolygon) {
        for (const auto& ring : polygon) {
            result.emplace_back(ring);
        }
    }
    return result;
}

static GeometryCollection makeRing(std::initializer_list<GeometryCoordinate> points) {
    GeometryCoordinates ring(points);
    ring.emplace_back(ring.front());
    GeometryCollection result;
    result.emplace_back(std::move(ring));
    return result;
}

static GeometryCollection makeRegularRing(std::size_t count) {
    GeometryCoordinates ring;
    constexpr double radius = 16000.0;
    constexpr double phase = 0.037;
    for (std::size_t i = 0; i < count; ++i) {
        const double angle = phase + i * 2.0 * std::numbers::pi / count;
        ring.emplace_back(static_cast<int16_t>(std::lround(std::cos(angle) * radius)),
                          static_cast<int16_t>(std::lround(std::sin(angle) * radius)));
    }
    ring.emplace_back(ring.front());
    GeometryCollection result;
    result.emplace_back(std::move(ring));
    return result;
}

TEST(GeometryTileData, classifyRings1) {
    std::vector<GeometryCollection> polygons = classifyRings({{{0, 0}, {0, 40}, {40, 40}, {40, 0}, {0, 0}}});

    // output: 1 polygon
    ASSERT_EQ(polygons.size(), 1u);
    // output: polygon 1 has 1 exterior
    ASSERT_EQ(polygons[0].size(), 1u);
}

TEST(GeometryTileData, classifyRings2) {
    std::vector<GeometryCollection> polygons = classifyRings(
        {{{0, 0}, {0, 40}, {40, 40}, {40, 0}, {0, 0}}, {{10, 10}, {20, 10}, {20, 20}, {10, 10}}});

    // output: 1 polygon
    ASSERT_EQ(polygons.size(), 1u);
    // output: polygon 1 has 1 exterior, 1 interior
    ASSERT_EQ(polygons[0].size(), 2u);
}

TEST(GeometryTileData, limitHoles1) {
    GeometryCollection polygon = {{{0, 0}, {0, 40}, {40, 40}, {40, 0}, {0, 0}},
                                  {{30, 30}, {32, 30}, {32, 32}, {30, 30}},
                                  {{10, 10}, {20, 10}, {20, 20}, {10, 10}}};

    limitHoles(polygon, 1);

    // output: polygon 1 has 1 exterior, 1 interior
    ASSERT_EQ(polygon.size(), 2u);

    // ensure we've kept the right rings (ones with largest areas)
    ASSERT_EQ(polygon[0][0].x, 0);
    ASSERT_EQ(polygon[1][0].x, 10);
}

TEST(GeometryTileData, limitHoles2) {
    GeometryCollection polygon = {{{0, 0}, {0, 40}, {40, 40}, {40, 0}, {0, 0}},
                                  {{10, 10}, {20, 10}, {20, 20}, {10, 10}},
                                  {{30, 30}, {32, 30}, {32, 32}, {30, 30}}};

    limitHoles(polygon, 1);

    // output: polygon 1 has 1 exterior, 1 interior
    ASSERT_EQ(polygon.size(), 2u);

    // ensure we've kept the right rings (ones with largest areas)
    ASSERT_EQ(polygon[0][0].x, 0);
    ASSERT_EQ(polygon[1][0].x, 10);
}

TEST(GeometryTileData, limitHoles3) {
    // real world polygon with interior rings with negative areas
    // that need to be sorted in `limitHoles` by comparing absolute
    // area not signed
    GeometryCollection polygon = {
        {{7336, -248}, {7304, -248}, {7272, -168}, {7176, -200}, {7080, -136}, {7048, -56},  {7128, -8},
         {7176, -56},  {7288, -56},  {7316, 0},    {6918, 0},    {6904, -40},  {6984, -72},  {6952, -88},
         {6952, -168}, {6888, -88},  {6856, -88},  {6856, -8},   {6872, 0},    {6170, 0},    {6184, -40},
         {6136, -72},  {6104, -56},  {6132, 0},    {6028, 0},    {6104, -152}, {6184, -200}, {6206, -256},
         {6272, -256}, {6264, -248}, {6248, -120}, {6280, -136}, {6280, -232}, {6288, -256}, {6790, -256},
         {6792, -248}, {6800, -256}, {7058, -256}, {7064, -248}, {7096, -256}, {7338, -256}, {7336, -248}},
        {{6344, -104}, {6264, -8}, {6392, -72}, {6360, -200}, {6344, -104}},
        {{6744, -24}, {6760, -72}, {6728, -104}, {6744, -24}},
        {{6616, -104}, {6648, -88}, {6632, -72}, {6664, -56}, {6664, -120}, {6616, -104}}};

    // make a copy for later testing
    GeometryCollection original(polygon);

    ASSERT_EQ(polygon.size(), 4u);
    ASSERT_EQ(_signedArea(polygon.at(0)), 515360); // exterior
    ASSERT_EQ(_signedArea(polygon.at(1)), -12288); // biggest interior ring
    ASSERT_EQ(_signedArea(polygon.at(2)), -2048);  // smallest interior ring
    ASSERT_EQ(_signedArea(polygon.at(3)), -3072);  // second largest interior ring

    limitHoles(polygon, 2);

    // output: polygon 1 has 1 exterior, 2 interior
    ASSERT_EQ(polygon.size(), 3u);

    // ensure we've kept the exterior ring
    ASSERT_EQ(original.at(0), polygon.at(0));

    // ensure we've kept the two largest interior rings
    ASSERT_EQ(original.at(1), polygon.at(1));
    ASSERT_EQ(original.at(3), polygon.at(2));
}

TEST(GeometryTileData, simplePolygonFixupFastPathMatchesWagyu) {
    const std::vector<GeometryCoordinates> fixtures = {
        {{3, 1}, {8, 9}, {1, 7}},
        {{7, 0}, {10, 4}, {8, 9}, {3, 10}, {0, 4}, {2, 1}},
        {{4, 0}, {9, 3}, {7, 8}, {4, 5}, {1, 9}, {-1, 4}},
        {{-32768, -32768}, {32767, -32767}, {30000, 32767}, {-30000, 30000}},
    };

    for (const auto& fixture : fixtures) {
        for (bool reverse : {false, true}) {
            auto points = fixture;
            if (reverse) std::reverse(points.begin(), points.end());
            for (std::size_t rotation = 0; rotation < points.size(); ++rotation) {
                GeometryCoordinates ring;
                for (std::size_t i = 0; i < points.size(); ++i) {
                    ring.emplace_back(points[(rotation + i) % points.size()]);
                }
                ring.emplace_back(ring.front());
                GeometryCollection input;
                input.emplace_back(std::move(ring));

                auto fastResult = detail::tryFixupSimplePolygon(input);
                ASSERT_TRUE(fastResult);
                const auto reference = fixupPolygonsWithWagyu(input);
                EXPECT_EQ(*fastResult, reference);
                EXPECT_EQ(fixupPolygons(input), reference);
            }
        }
    }
}

TEST(GeometryTileData, simplePolygonFixupFastPathMatchesRectangles) {
    for (int16_t minX : {-32768, -4000, 0, 3000}) {
        for (int16_t minY : {-32768, -5000, 0, 2000}) {
            const int16_t maxX = minX == -32768 ? -30000 : static_cast<int16_t>(minX + 1000);
            const int16_t maxY = minY == -32768 ? -29000 : static_cast<int16_t>(minY + 1500);
            GeometryCoordinates points = {{minX, minY}, {minX, maxY}, {maxX, maxY}, {maxX, minY}};

            for (bool reverse : {false, true}) {
                if (reverse) std::reverse(points.begin(), points.end());
                for (std::size_t rotation = 0; rotation < points.size(); ++rotation) {
                    GeometryCoordinates ring;
                    for (std::size_t i = 0; i < points.size(); ++i) {
                        ring.emplace_back(points[(rotation + i) % points.size()]);
                    }
                    ring.emplace_back(ring.front());
                    GeometryCollection input;
                    input.emplace_back(std::move(ring));

                    auto fastResult = detail::tryFixupSimplePolygon(input);
                    ASSERT_TRUE(fastResult);
                    const auto reference = fixupPolygonsWithWagyu(input);
                    EXPECT_EQ(*fastResult, reference);
                    EXPECT_EQ(fixupPolygons(input), reference);
                }
            }
        }
    }
}

TEST(GeometryTileData, simplePolygonFixupFastPathMatchesRandomTriangles) {
    std::mt19937 random(0xC04);
    std::uniform_int_distribution<int16_t> coordinate(-8192, 8192);
    std::size_t checked = 0;

    for (std::size_t attempt = 0; attempt < 20000 && checked < 2000; ++attempt) {
        GeometryCoordinates points = {
            {coordinate(random), coordinate(random)},
            {coordinate(random), coordinate(random)},
            {coordinate(random), coordinate(random)},
        };
        if (points[0].y == points[1].y || points[0].y == points[2].y || points[1].y == points[2].y) continue;

        auto input = makeRing({points[0], points[1], points[2]});
        auto fastResult = detail::tryFixupSimplePolygon(input);
        if (!fastResult) continue;

        const auto reference = fixupPolygonsWithWagyu(input);
        EXPECT_EQ(*fastResult, reference);
        EXPECT_EQ(fixupPolygons(input), reference);
        ++checked;
    }

    EXPECT_EQ(checked, 2000u);
}

TEST(GeometryTileData, simplePolygonFixupFastPathMatchesRandomRings) {
    std::mt19937 random(0x51A9);
    std::uniform_int_distribution<int> vertexCount(4, 16);
    std::uniform_real_distribution<double> phase(0.0, 1.0);
    std::uniform_int_distribution<int> radius(2000, 4000);
    std::size_t checked = 0;

    for (std::size_t sample = 0; sample < 2000; ++sample) {
        GeometryCoordinates ring;
        const int count = vertexCount(random);
        const double offset = phase(random) * 2.0 * std::numbers::pi / count;
        for (int i = 0; i < count; ++i) {
            const double angle = offset + i * 2.0 * std::numbers::pi / count;
            const int pointRadius = radius(random);
            ring.emplace_back(static_cast<int16_t>(std::lround(std::cos(angle) * pointRadius)),
                              static_cast<int16_t>(std::lround(std::sin(angle) * pointRadius)));
        }
        if (sample % 2 != 0) std::reverse(ring.begin(), ring.end());
        ring.emplace_back(ring.front());

        GeometryCollection input;
        input.emplace_back(std::move(ring));
        auto fastResult = detail::tryFixupSimplePolygon(input);
        if (!fastResult) continue;

        const auto reference = fixupPolygonsWithWagyu(input);
        EXPECT_EQ(*fastResult, reference);
        EXPECT_EQ(fixupPolygons(input), reference);
        ++checked;
    }

    EXPECT_GE(checked, 1000u);
}

TEST(GeometryTileData, simplePolygonFixupFallsBackForUncertainGeometry) {
    auto expectFallback = [](const char* name, GeometryCollection input) {
        SCOPED_TRACE(name);
        EXPECT_FALSE(detail::tryFixupSimplePolygon(input));
        EXPECT_EQ(fixupPolygons(input), fixupPolygonsWithWagyu(input));
    };

    expectFallback("tied minimum", {{{0, 0}, {5, 0}, {6, 5}, {0, 6}, {0, 0}}});
    expectFallback("self intersection", {{{0, 0}, {4, 5}, {0, 5}, {5, 0}, {0, 0}}});
    expectFallback("collinear", {{{0, 0}, {4, 0}, {2, 0}, {4, 5}, {0, 0}}});
    expectFallback("self touch", {{{0, 0}, {5, 4}, {0, 8}, {0, 4}, {3, 4}, {0, 0}}});
    expectFallback("repeated vertex", {{{0, 0}, {4, 5}, {0, 8}, {4, 5}, {8, 8}, {0, 0}}});
    expectFallback("hole", {{{0, 0}, {4, 7}, {8, 1}, {0, 0}}, {{2, 2}, {3, 3}, {4, 2}, {2, 2}}});
    expectFallback("open ring", {{{0, 0}, {3, 6}, {8, 1}}});
    expectFallback("unsafe clearance", {{{6171, -6248}, {-2763, -2754}, {4327, -5527}, {6171, -6248}}});
}

TEST(GeometryTileData, simplePolygonFixupMatchesWagyuForRandomUncertainGeometry) {
    std::mt19937 random(0xFA11BAC);
    std::uniform_int_distribution<int> offset(-4000, 4000);
    std::uniform_int_distribution<int> size(2, 500);
    std::uniform_int_distribution<int> inset(0, 1000);

    for (std::size_t sample = 0; sample < 1000; ++sample) {
        SCOPED_TRACE(sample);
        const int x = offset(random);
        const int y = offset(random);
        const int extent = size(random);
        auto point = [](int pointX, int pointY) {
            return GeometryCoordinate{static_cast<int16_t>(pointX), static_cast<int16_t>(pointY)};
        };

        GeometryCollection collapsed = {{point(x, y),
                                         point(x + extent, y + extent),
                                         point(x + 2 * extent, y + 2 * extent),
                                         point(x + 3 * extent, y),
                                         point(x, y)}};
        EXPECT_FALSE(detail::tryFixupSimplePolygon(collapsed));
        EXPECT_EQ(fixupPolygons(collapsed), fixupPolygonsWithWagyu(collapsed));

        GeometryCollection selfTouching = {{point(x, y),
                                            point(x + 3 * extent, y),
                                            point(x + 3 * extent, y + 3 * extent),
                                            point(x + extent, y + extent),
                                            point(x, y + 3 * extent),
                                            point(x + extent, y + extent),
                                            point(x, y)}};
        EXPECT_FALSE(detail::tryFixupSimplePolygon(selfTouching));
        EXPECT_EQ(fixupPolygons(selfTouching), fixupPolygonsWithWagyu(selfTouching));

        GeometryCollection withHole = {{point(x, y),
                                        point(x, y + 4 * extent),
                                        point(x + 4 * extent, y + 4 * extent),
                                        point(x + 4 * extent, y),
                                        point(x, y)},
                                       {point(x + extent, y + extent),
                                        point(x + 3 * extent, y + extent),
                                        point(x + 2 * extent, y + 3 * extent),
                                        point(x + extent, y + extent)}};
        EXPECT_FALSE(detail::tryFixupSimplePolygon(withHole));
        EXPECT_EQ(fixupPolygons(withHole), fixupPolygonsWithWagyu(withHole));

        const int edgeInset = inset(random);
        GeometryCoordinates clipEdgeRing = {
            {static_cast<int16_t>(-32768 + edgeInset), static_cast<int16_t>(-32768 + edgeInset)},
            {static_cast<int16_t>(32767 - edgeInset), static_cast<int16_t>(-22000 + edgeInset)},
            {static_cast<int16_t>(22000 - edgeInset), static_cast<int16_t>(32767 - edgeInset)},
        };
        if (sample % 2 != 0) std::reverse(clipEdgeRing.begin(), clipEdgeRing.end());
        clipEdgeRing.emplace_back(clipEdgeRing.front());
        GeometryCollection clipEdgeInput;
        clipEdgeInput.emplace_back(std::move(clipEdgeRing));
        auto fastResult = detail::tryFixupSimplePolygon(clipEdgeInput);
        ASSERT_TRUE(fastResult);
        const auto reference = fixupPolygonsWithWagyu(clipEdgeInput);
        EXPECT_EQ(*fastResult, reference);
        EXPECT_EQ(fixupPolygons(clipEdgeInput), reference);
    }
}

TEST(GeometryTileData, simplePolygonFixupBoundsValidationCost) {
    auto boundedInput = makeRegularRing(32);
    auto fastResult = detail::tryFixupSimplePolygon(boundedInput);
    ASSERT_TRUE(fastResult);
    EXPECT_EQ(*fastResult, fixupPolygonsWithWagyu(boundedInput));

    auto longInput = makeRegularRing(33);
    EXPECT_FALSE(detail::tryFixupSimplePolygon(longInput));
    EXPECT_EQ(fixupPolygons(longInput), fixupPolygonsWithWagyu(longInput));
}

TEST(GeometryTileData, simplePolygonFixupRuntimeRollback) {
    const bool initialValue = detail::isSimplePolygonFixupEnabled();
    const auto input = makeRing({{3, 1}, {8, 9}, {1, 7}});
    const auto reference = fixupPolygonsWithWagyu(input);

    detail::setSimplePolygonFixupEnabled(false);
    EXPECT_FALSE(detail::isSimplePolygonFixupEnabled());
    EXPECT_EQ(fixupPolygons(input), reference);

    detail::setSimplePolygonFixupEnabled(true);
    EXPECT_TRUE(detail::isSimplePolygonFixupEnabled());
    EXPECT_EQ(fixupPolygons(input), reference);

    detail::setSimplePolygonFixupEnabled(initialValue);
}
