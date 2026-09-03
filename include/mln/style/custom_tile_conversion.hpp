#pragma once

#include <mln/style/sources/custom_geometry_source.hpp>
#include <mln/util/geometry.hpp>

#include <cstdint>
#include <vector>

namespace mln {
namespace style {

/// The numbers geojson-vt derives for one custom tile. They let a caller tell
/// in advance which rings the conversion is going to drop.
struct CustomTileConversionSpec {
    uint16_t extent = 0;
    uint16_t buffer = 0;
    /// Simplification tolerance in extent units.
    double tolerance = 0.0;
    /// The same tolerance in the normalized space where rings are measured.
    double normalizedTolerance = 0.0;
    /// What a ring area is compared against, geojson-vt keeps a ring above it.
    double squaredNormalizedTolerance = 0.0;
};

/// z is the canonical tile zoom that geojson-vt is given.
CustomTileConversionSpec customTileConversionSpec(const CustomGeometrySource::TileOptions& options, uint8_t z);

/// The projection geojson-vt applies to a point before it measures a ring.
Point<double> customTileProject(const Point<double>& lonLat);

/// Absolute shoelace area of an already projected ring. Like geojson-vt this
/// walks consecutive pairs only, so the ring has to be closed already.
double customTileRingArea(const std::vector<Point<double>>& projectedRing);

/// Same area, but for a ring still in lon/lat. It projects while it sums, so a
/// caller does not have to build the projected ring first.
double customTileLonLatRingArea(const std::vector<Point<double>>& lonLatRing);

/// False when geojson-vt drops this ring. A polygon survives when any of its
/// rings passes, so check them all. This is the exact comparison with no room
/// for rounding, use customTileSurelyDropsRing to decide whether to skip work.
bool customTileKeepsRing(double ringArea, const CustomTileConversionSpec& spec);

/// How far a computed ring area can sit from the exact one, in normalized
/// units. The shoelace adds products of coordinates in [0,1], so the error
/// grows with the ring size. Longitudes outside [-180, 180] project past that
/// range and need a wider bound than this.
double customTileRingAreaTolerance(size_t ringSize);

/// True when the ring is small enough that geojson-vt is certain to drop it.
/// It leaves room for rounding, so it never rejects a ring MapLibre keeps.
/// Prefer this over comparing an area yourself.
bool customTileSurelyDropsRing(double ringArea, size_t ringSize, const CustomTileConversionSpec& spec);

} // namespace style
} // namespace mln
