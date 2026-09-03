#include <mln/test/util.hpp>

#include <mln/style/custom_tile_conversion.hpp>
#include <mln/style/sources/custom_geometry_source.hpp>
#include <mln/util/constants.hpp>

#include <mapbox/geojsonvt.hpp>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

using namespace mln;
using namespace mln::style;

namespace {

// The options AutoMapa uses for its Mode 2 source.
CustomGeometrySource::TileOptions autoMapaOptions() {
    CustomGeometrySource::TileOptions options;
    options.tileSize = 512;
    options.buffer = 256;
    options.clip = false;
    options.wrap = false;
    return options;
}

mapbox::geojsonvt::TileOptions vtOptions(const CustomGeometrySource::TileOptions& options, uint8_t z) {
    const auto spec = customTileConversionSpec(options, z);
    mapbox::geojsonvt::TileOptions result;
    result.extent = spec.extent;
    result.buffer = spec.buffer;
    result.tolerance = spec.tolerance;
    return result;
}

// A square of the given side, in degrees, placed inside tile z/x/y.
mapbox::feature::feature_collection<double> squareIn(double side, double lon, double lat) {
    mapbox::geometry::linear_ring<double> ring{
        {lon, lat}, {lon + side, lat}, {lon + side, lat + side}, {lon, lat + side}, {lon, lat}};
    mapbox::geometry::polygon<double> polygon{ring};
    mapbox::feature::feature<double> feature{polygon};
    return {feature};
}

// The tile that actually holds the point, so the int16 transform stays in range.
std::pair<uint32_t, uint32_t> tileFor(double lon, double lat, uint8_t z) {
    const auto projected = customTileProject({lon, lat});
    const double n = static_cast<double>(1u << z);
    return {static_cast<uint32_t>(projected.x * n), static_cast<uint32_t>(projected.y * n)};
}

bool conversionKeeps(double side, double lon, double lat, uint8_t z) {
    const auto options = autoMapaOptions();
    const auto features = squareIn(side, lon, lat);
    const auto tile = tileFor(lon, lat, z);
    return !mapbox::geojsonvt::geoJSONToTile(
                features, z, tile.first, tile.second, vtOptions(options, z), options.wrap, options.clip)
                .features.empty();
}

bool predictionKeeps(double side, double lon, double lat, uint8_t z) {
    const auto ring = squareIn(side, lon, lat)[0].geometry.get<mapbox::geometry::polygon<double>>()[0];
    const std::vector<Point<double>> raw(ring.begin(), ring.end());
    return customTileKeepsRing(customTileLonLatRingArea(raw), customTileConversionSpec(autoMapaOptions(), z));
}

// Smallest side the real conversion still keeps, narrowed to one ulp.
double criticalSide(double lon, double lat, uint8_t z) {
    double dropped = 0.0;
    // A whole tile is the widest square worth probing, and one degree is already
    // far above the threshold at the lowest zooms. Anything larger runs off the
    // tile and the int16 transform would overflow.
    double kept = std::min(1.0, 360.0 / static_cast<double>(1u << z));
    for (int i = 0; i < 200; ++i) {
        const double mid = 0.5 * (dropped + kept);
        if (mid <= dropped || mid >= kept) {
            break;
        }
        if (conversionKeeps(mid, lon, lat, z)) {
            kept = mid;
        } else {
            dropped = mid;
        }
    }
    return kept;
}

std::vector<Point<double>> projectRing(const mapbox::geometry::linear_ring<double>& ring) {
    std::vector<Point<double>> projected;
    projected.reserve(ring.size());
    for (const auto& point : ring) {
        projected.push_back(customTileProject(point));
    }
    return projected;
}

} // namespace

TEST(CustomTileConversion, SpecMatchesTheDocumentedThreshold) {
    const auto options = autoMapaOptions();

    for (uint8_t z : {uint8_t(0), uint8_t(5), uint8_t(14)}) {
        const auto spec = customTileConversionSpec(options, z);
        EXPECT_EQ(util::EXTENT, spec.extent);
        EXPECT_EQ(4096, spec.buffer);
        EXPECT_DOUBLE_EQ(6.0, spec.tolerance);

        const double expected = (6.0 / 8192.0) / static_cast<double>(1u << z);
        EXPECT_DOUBLE_EQ(expected, spec.normalizedTolerance);
        EXPECT_DOUBLE_EQ(expected * expected, spec.squaredNormalizedTolerance);
    }
}

TEST(CustomTileConversion, ProjectionMatchesGeoJSONVT) {
    // geojson-vt clamps the projected y, not the latitude, so the poles land on
    // the edges instead of running off to infinity.
    EXPECT_DOUBLE_EQ(0.5, customTileProject({0.0, 0.0}).x);
    EXPECT_DOUBLE_EQ(0.5, customTileProject({0.0, 0.0}).y);
    EXPECT_DOUBLE_EQ(0.0, customTileProject({-180.0, 0.0}).x);
    EXPECT_DOUBLE_EQ(1.0, customTileProject({180.0, 0.0}).x);
    EXPECT_DOUBLE_EQ(0.0, customTileProject({0.0, 90.0}).y);
    EXPECT_DOUBLE_EQ(1.0, customTileProject({0.0, -90.0}).y);

    const double sine = std::sin(52.0 * M_PI / 180);
    EXPECT_DOUBLE_EQ(0.5 - 0.25 * std::log((1 + sine) / (1 - sine)) / M_PI, customTileProject({21.0, 52.0}).y);
}

TEST(CustomTileConversion, RingAreaNeedsAClosedRing) {
    // The closing segment is what makes the sum right. Dropping it changes the
    // answer unless the first and last points are collinear with the origin.
    const mapbox::geometry::linear_ring<double> closed{{1, 1}, {5, 1}, {1, 4}, {1, 1}};
    const mapbox::geometry::linear_ring<double> open{{1, 1}, {5, 1}, {1, 4}};

    std::vector<Point<double>> closedRaw(closed.begin(), closed.end());
    std::vector<Point<double>> openRaw(open.begin(), open.end());

    EXPECT_DOUBLE_EQ(6.0, customTileRingArea(closedRaw));
    EXPECT_DOUBLE_EQ(7.5, customTileRingArea(openRaw));
    EXPECT_DOUBLE_EQ(0.0, customTileRingArea({}));
}

TEST(CustomTileConversion, LonLatAreaMatchesProjectThenMeasure) {
    const mapbox::geometry::linear_ring<double> ring{
        {20.0, 52.0}, {20.01, 52.0}, {20.01, 52.008}, {20.0, 52.008}, {20.0, 52.0}};
    const std::vector<Point<double>> raw(ring.begin(), ring.end());

    EXPECT_EQ(customTileRingArea(projectRing(ring)), customTileLonLatRingArea(raw));
    EXPECT_DOUBLE_EQ(0.0, customTileLonLatRingArea({}));
}

TEST(CustomTileConversion, PredicateAgreesWithTheRealConversion) {
    const auto options = autoMapaOptions();
    const uint8_t z = 5;
    const uint32_t x = 17;
    const uint32_t y = 10;

    // A coarse sweep from far below the threshold to far above it.
    for (double side : {0.00001, 0.0001, 0.0005, 0.001, 0.005, 0.05, 0.5}) {
        const auto features = squareIn(side, 20.0, 52.0);
        const auto tile = mapbox::geojsonvt::geoJSONToTile(
            features, z, x, y, vtOptions(options, z), options.wrap, options.clip);

        const auto& ring = features[0].geometry.get<mapbox::geometry::polygon<double>>()[0];
        const std::vector<Point<double>> raw(ring.begin(), ring.end());
        const bool predicted =
            customTileKeepsRing(customTileLonLatRingArea(raw), customTileConversionSpec(options, z));

        EXPECT_EQ(predicted, !tile.features.empty()) << "side " << side;
    }
}

TEST(CustomTileConversion, PredictionMatchesTheRealConversionAtTheThreshold) {
    for (uint8_t z : {uint8_t(0), uint8_t(1), uint8_t(5), uint8_t(9), uint8_t(14), uint8_t(18), uint8_t(20)}) {
        const double kept = criticalSide(20.0, 52.0, z);
        const double dropped = std::nextafter(kept, 0.0);

        EXPECT_TRUE(conversionKeeps(kept, 20.0, 52.0, z)) << "z " << int(z);
        EXPECT_FALSE(conversionKeeps(dropped, 20.0, 52.0, z)) << "z " << int(z);

        // One ulp on either side of the flip, which is where a wrong zoom
        // normalization would show up. Above about z17 the flip is set by the
        // cancellation floor instead, so keep the low zooms, they are the ones
        // checking the normalization.
        EXPECT_TRUE(predictionKeeps(kept, 20.0, 52.0, z)) << "z " << int(z);
        EXPECT_FALSE(predictionKeeps(dropped, 20.0, 52.0, z)) << "z " << int(z);
    }
}

TEST(CustomTileConversion, ClampedLatitudeLeavesNoArea) {
    // Past the mercator limit the projected y clamps, so every corner lands on
    // the same edge and the ring measures zero. Without the clamp this ring
    // would be well above the threshold.
    const double side = 0.05;
    const uint8_t z = 5;

    EXPECT_FALSE(conversionKeeps(side, 20.0, 88.0, z));
    EXPECT_FALSE(predictionKeeps(side, 20.0, 88.0, z));
    EXPECT_TRUE(conversionKeeps(side, 20.0, 52.0, z));
}

TEST(CustomTileConversion, SurelyDropsLeavesRoomForRounding) {
    const auto spec = customTileConversionSpec(autoMapaOptions(), 5);
    const double threshold = spec.squaredNormalizedTolerance;

    // Anything the conversion keeps must never be reported as a sure drop.
    EXPECT_FALSE(customTileSurelyDropsRing(threshold, 5, spec));
    EXPECT_FALSE(customTileSurelyDropsRing(std::nextafter(threshold, 0.0), 5, spec));
    EXPECT_FALSE(customTileSurelyDropsRing(threshold - customTileRingAreaTolerance(5), 5, spec));
    EXPECT_TRUE(customTileSurelyDropsRing(0.0, 5, spec));

    // The room grows with the ring, and it must cover a lost bit of the sum.
    EXPECT_GT(customTileRingAreaTolerance(50), customTileRingAreaTolerance(5));
    EXPECT_GT(customTileRingAreaTolerance(5), 1e-16);
}

TEST(CustomTileConversion, ThresholdIsExclusiveAtTheBoundary) {
    const auto spec = customTileConversionSpec(autoMapaOptions(), 5);
    const double threshold = spec.squaredNormalizedTolerance;

    EXPECT_FALSE(customTileKeepsRing(threshold, spec));
    EXPECT_FALSE(customTileKeepsRing(std::nextafter(threshold, 0.0), spec));
    EXPECT_TRUE(customTileKeepsRing(std::nextafter(threshold, 1.0), spec));
}
