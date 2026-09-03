#include <mln/style/custom_tile_conversion.hpp>
#include <mln/util/constants.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>

namespace mln {
namespace style {

CustomTileConversionSpec customTileConversionSpec(const CustomGeometrySource::TileOptions& options, uint8_t z) {
    auto scale = util::EXTENT / options.tileSize;
    assert(util::EXTENT % options.tileSize == 0);

    CustomTileConversionSpec spec;
    spec.extent = util::EXTENT;
    spec.buffer = static_cast<uint16_t>(::round(scale * options.buffer));
    spec.tolerance = scale * options.tolerance;
    spec.normalizedTolerance = (spec.tolerance / spec.extent) / static_cast<double>(1u << z);
    spec.squaredNormalizedTolerance = spec.normalizedTolerance * spec.normalizedTolerance;
    return spec;
}

Point<double> customTileProject(const Point<double>& lonLat) {
    const double sine = std::sin(lonLat.y * M_PI / 180);
    const double x = lonLat.x / 360 + 0.5;
    const double y = std::max(std::min(0.5 - 0.25 * std::log((1 + sine) / (1 - sine)) / M_PI, 1.0), 0.0);
    return {x, y};
}

double customTileRingArea(const std::vector<Point<double>>& projectedRing) {
    const size_t len = projectedRing.size();
    if (len == 0) {
        return 0.0;
    }

    double area = 0.0;
    for (size_t i = 0; i < len - 1; ++i) {
        const auto& a = projectedRing[i];
        const auto& b = projectedRing[i + 1];
        area += a.x * b.y - b.x * a.y;
    }
    return std::abs(area / 2);
}

double customTileLonLatRingArea(const std::vector<Point<double>>& lonLatRing) {
    const size_t len = lonLatRing.size();
    if (len == 0) {
        return 0.0;
    }

    double area = 0.0;
    Point<double> a = customTileProject(lonLatRing[0]);
    for (size_t i = 1; i < len; ++i) {
        const Point<double> b = customTileProject(lonLatRing[i]);
        area += a.x * b.y - b.x * a.y;
        a = b;
    }
    return std::abs(area / 2);
}

bool customTileKeepsRing(double ringArea, const CustomTileConversionSpec& spec) {
    return ringArea > spec.squaredNormalizedTolerance;
}

double customTileRingAreaTolerance(size_t ringSize) {
    // Every term is a product of coordinates in [0,1] and the running sum can
    // reach the ring size, so size squared times one ulp bounds the error. It
    // also covers a compiler contracting the two products into one multiply
    // and add, which rounds once instead of twice.
    const double n = static_cast<double>(ringSize);
    return n * n * std::numeric_limits<double>::epsilon();
}

bool customTileSurelyDropsRing(double ringArea, size_t ringSize, const CustomTileConversionSpec& spec) {
    return ringArea + customTileRingAreaTolerance(ringSize) < spec.squaredNormalizedTolerance;
}

} // namespace style
} // namespace mln
