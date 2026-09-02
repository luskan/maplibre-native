#include <mln/tile/geometry_tile_data.hpp>
#include <mln/tile/geometry_tile_data_impl.hpp>
#include <mln/tile/tile_id.hpp>
#include <mln/math/angles.hpp>
#include <mln/math/clamp.hpp>
#include <mln/util/instrumentation.hpp>
#include <mln/util/math.hpp>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4005)
#endif

#include <mapbox/geometry/wagyu/wagyu.hpp>

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <numbers>

#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

using namespace std::numbers;

namespace mln {
namespace {

constexpr std::size_t maxSimpleRingVertices = 32;
constexpr char simplePolygonFixupEnv[] = "AM_MAPLIBRE_SIMPLE_POLYGON_FIXUP";
constexpr char validateSimplePolygonFixupEnv[] = "AM_MAPLIBRE_VALIDATE_SIMPLE_POLYGON_FIXUP";

#ifdef __ANDROID__
constexpr char simplePolygonFixupProperty[] = "debug.automapa.mln.simple_fixup";
constexpr char validateSimplePolygonFixupProperty[] = "debug.automapa.mln.val_fixup";
#endif

bool parseRuntimeFlag(const char* value, bool defaultValue) noexcept {
    if (value == nullptr || *value == '\0') return defaultValue;
    if (std::strcmp(value, "0") == 0) return false;
    if (std::strcmp(value, "1") == 0) return true;
    return defaultValue;
}

bool loadRuntimeFlag(const char* environmentName,
#ifdef __ANDROID__
                     const char* propertyName,
#endif
                     bool defaultValue) noexcept {
#ifdef __ANDROID__
    char propertyValue[PROP_VALUE_MAX] = {};
    if (__system_property_get(propertyName, propertyValue) > 0) {
        return parseRuntimeFlag(propertyValue, defaultValue);
    }
#endif

#ifdef _WIN32
    char* environmentValue = nullptr;
    std::size_t environmentValueSize = 0;
    if (_dupenv_s(&environmentValue, &environmentValueSize, environmentName) != 0) return defaultValue;
    const bool result = parseRuntimeFlag(environmentValue, defaultValue);
    std::free(environmentValue);
    return result;
#else
    return parseRuntimeFlag(std::getenv(environmentName), defaultValue);
#endif
}

std::atomic_bool& simplePolygonFixupEnabledState() noexcept {
    static std::atomic_bool enabled{loadRuntimeFlag(simplePolygonFixupEnv,
#ifdef __ANDROID__
                                                    simplePolygonFixupProperty,
#endif
                                                    true)};
    return enabled;
}

std::int64_t crossProduct(const GeometryCoordinate& a, const GeometryCoordinate& b, const GeometryCoordinate& c) {
    const auto abx = static_cast<std::int64_t>(b.x) - a.x;
    const auto aby = static_cast<std::int64_t>(b.y) - a.y;
    const auto acx = static_cast<std::int64_t>(c.x) - a.x;
    const auto acy = static_cast<std::int64_t>(c.y) - a.y;
    return abx * acy - aby * acx;
}

bool pointOnSegment(const GeometryCoordinate& point, const GeometryCoordinate& start, const GeometryCoordinate& end) {
    return point.x >= std::min(start.x, end.x) && point.x <= std::max(start.x, end.x) &&
           point.y >= std::min(start.y, end.y) && point.y <= std::max(start.y, end.y);
}

bool segmentsIntersect(const GeometryCoordinate& a,
                       const GeometryCoordinate& b,
                       const GeometryCoordinate& c,
                       const GeometryCoordinate& d) {
    const auto abc = crossProduct(a, b, c);
    const auto abd = crossProduct(a, b, d);
    const auto cda = crossProduct(c, d, a);
    const auto cdb = crossProduct(c, d, b);

    if (abc == 0 && pointOnSegment(c, a, b)) return true;
    if (abd == 0 && pointOnSegment(d, a, b)) return true;
    if (cda == 0 && pointOnSegment(a, c, d)) return true;
    if (cdb == 0 && pointOnSegment(b, c, d)) return true;

    return (abc > 0) != (abd > 0) && (cda > 0) != (cdb > 0);
}

bool hasSafeLineDistance(const GeometryCoordinate& point,
                         const GeometryCoordinate& start,
                         const GeometryCoordinate& end) {
    const auto dx = std::abs(static_cast<std::int64_t>(end.x) - start.x);
    const auto dy = std::abs(static_cast<std::int64_t>(end.y) - start.y);
    // A distance greater than sqrt(2) keeps snap rounding away from unrelated edges.
    return std::abs(crossProduct(start, end, point)) > 2 * std::max(dx, dy);
}

std::optional<GeometryCollection> tryFixupAxisAlignedRectangle(const GeometryCoordinates& ring, std::int64_t area) {
    if (ring.size() != 5 || area == 0) return std::nullopt;

    auto minX = ring.front().x;
    auto maxX = ring.front().x;
    auto minY = ring.front().y;
    auto maxY = ring.front().y;
    for (std::size_t i = 0; i < 4; ++i) {
        const auto& current = ring[i];
        const auto& next = ring[i + 1];
        if ((current.x == next.x) == (current.y == next.y)) return std::nullopt;
        minX = std::min(minX, current.x);
        maxX = std::max(maxX, current.x);
        minY = std::min(minY, current.y);
        maxY = std::max(maxY, current.y);
    }
    if (minX == maxX || minY == maxY) return std::nullopt;

    for (std::size_t i = 0; i < 4; ++i) {
        const auto& point = ring[i];
        if ((point.x != minX && point.x != maxX) || (point.y != minY && point.y != maxY)) {
            return std::nullopt;
        }
    }

    GeometryCoordinates outputRing;
    outputRing.reserve(5);
    if (area > 0) {
        outputRing = {{maxX, minY}, {maxX, maxY}, {minX, maxY}, {minX, minY}, {maxX, minY}};
    } else {
        outputRing = {{minX, maxY}, {minX, minY}, {maxX, minY}, {maxX, maxY}, {minX, maxY}};
    }

    GeometryCollection result;
    result.emplace_back(std::move(outputRing));
    return result;
}

bool validateSimplePolygonFixup() {
    // Set to 1 to compare each fast result with Wagyu before returning it.
    static const bool enabled = loadRuntimeFlag(validateSimplePolygonFixupEnv,
#ifdef __ANDROID__
                                                validateSimplePolygonFixupProperty,
#endif
                                                false);
    return enabled;
}

double signedArea(const GeometryCoordinates& ring) {
    double sum = 0;

    for (std::size_t i = 0, len = ring.size(), j = len - 1; i < len; j = i++) {
        const GeometryCoordinate& p1 = ring[i];
        const GeometryCoordinate& p2 = ring[j];
        sum += (p2.x - p1.x) * (p1.y + p2.y);
    }

    return sum;
}

LinearRing<int32_t> toWagyuPath(const GeometryCoordinates& ring) {
    MLN_TRACE_FUNC();

    LinearRing<int32_t> result;
    result.reserve(ring.size());
    for (const auto& p : ring) {
        result.emplace_back(p.x, p.y);
    }
    return result;
}

GeometryCollection toGeometryCollection(MultiPolygon<int16_t>&& multipolygon) {
    MLN_TRACE_FUNC();

    GeometryCollection result;
    for (auto& polygon : multipolygon) {
        for (auto& ring : polygon) {
            result.emplace_back(std::move(ring));
        }
    }
    return result;
}

GeometryCollection fixupPolygonsWithWagyu(const GeometryCollection& rings) {
    MLN_TRACE_FUNC();

    using namespace mapbox::geometry::wagyu;

    wagyu<int32_t> clipper;

    for (const auto& ring : rings) {
        clipper.add_ring(toWagyuPath(ring));
    }

    MultiPolygon<int16_t> multipolygon;
    clipper.execute(clip_type_union, multipolygon, fill_type_even_odd, fill_type_even_odd);

    return toGeometryCollection(std::move(multipolygon));
}
} // namespace

namespace detail {

bool isSimplePolygonFixupEnabled() noexcept {
    return simplePolygonFixupEnabledState().load(std::memory_order_relaxed);
}

void setSimplePolygonFixupEnabled(bool enabled) noexcept {
    simplePolygonFixupEnabledState().store(enabled, std::memory_order_relaxed);
}

std::optional<GeometryCollection> tryFixupSimplePolygon(const GeometryCollection& rings) {
    if (rings.size() != 1) return std::nullopt;

    const auto& ring = rings.front();
    if (ring.size() < 4 || ring.size() > maxSimpleRingVertices + 1 || ring.front() != ring.back()) {
        return std::nullopt;
    }

    const std::size_t vertexCount = ring.size() - 1;
    std::size_t lowestVertex = 0;
    bool lowestVertexIsUnique = true;
    std::int64_t area = 0;

    for (std::size_t i = 0; i < vertexCount; ++i) {
        const auto& current = ring[i];
        const auto& next = ring[(i + 1) % vertexCount];
        const auto& previous = ring[(i + vertexCount - 1) % vertexCount];

        if (current.y < ring[lowestVertex].y) {
            lowestVertex = i;
            lowestVertexIsUnique = true;
        } else if (i != lowestVertex && current.y == ring[lowestVertex].y) {
            lowestVertexIsUnique = false;
        }

        if (crossProduct(previous, current, next) == 0) return std::nullopt;

        area += static_cast<std::int64_t>(current.x) * next.y - static_cast<std::int64_t>(next.x) * current.y;

        for (std::size_t j = 0; j < i; ++j) {
            if (ring[j] == current) return std::nullopt;
        }
    }

    if (!lowestVertexIsUnique) return tryFixupAxisAlignedRectangle(ring, area);
    if (area == 0) return std::nullopt;

    for (std::size_t i = 0; i < vertexCount; ++i) {
        const std::size_t iNext = (i + 1) % vertexCount;
        for (std::size_t j = i + 1; j < vertexCount; ++j) {
            const std::size_t jNext = (j + 1) % vertexCount;
            if (iNext == j || jNext == i) continue;
            if (segmentsIntersect(ring[i], ring[iNext], ring[j], ring[jNext])) return std::nullopt;
        }

        for (std::size_t j = 0; j < vertexCount; ++j) {
            const std::size_t jNext = (j + 1) % vertexCount;
            if (j == i || jNext == i) continue;
            if (!hasSafeLineDistance(ring[i], ring[j], ring[jNext])) return std::nullopt;
        }
    }

    GeometryCoordinates outputRing;
    outputRing.reserve(ring.size());
    for (std::size_t i = 0; i < vertexCount; ++i) {
        const std::size_t index = area > 0 ? (lowestVertex + i) % vertexCount
                                           : (lowestVertex + vertexCount - i) % vertexCount;
        outputRing.emplace_back(ring[index]);
    }
    outputRing.emplace_back(outputRing.front());

    GeometryCollection result;
    result.emplace_back(std::move(outputRing));
    return result;
}

} // namespace detail

GeometryCollection fixupPolygons(const GeometryCollection& rings) {
    MLN_TRACE_FUNC();

    if (detail::isSimplePolygonFixupEnabled()) {
        if (auto result = detail::tryFixupSimplePolygon(rings)) {
            if (validateSimplePolygonFixup()) {
                auto reference = fixupPolygonsWithWagyu(rings);
                if (*result != reference) std::abort();
            }
            return std::move(*result);
        }
    }

    return fixupPolygonsWithWagyu(rings);
}

std::vector<GeometryCollection> classifyRings(const GeometryCollection& rings) {
    MLN_TRACE_FUNC();

    std::vector<GeometryCollection> polygons;

    std::size_t len = rings.size();

    if (len <= 1) {
        polygons.emplace_back(rings.clone());
        return polygons;
    }

    GeometryCollection polygon;
    int8_t ccw = 0;

    for (const auto& ring : rings) {
        double area = signedArea(ring);
        if (area == 0) continue;

        if (ccw == 0) {
            ccw = (area < 0 ? -1 : 1);
        }

        if (ccw == (area < 0 ? -1 : 1) && !polygon.empty()) {
            polygons.emplace_back(std::move(polygon));
            polygon = GeometryCollection();
        }

        polygon.emplace_back(ring);
    }

    if (!polygon.empty()) {
        polygons.emplace_back(std::move(polygon));
    }

    return polygons;
}

void limitHoles(GeometryCollection& polygon, uint32_t maxHoles) {
    MLN_TRACE_FUNC();

    if (polygon.size() > 1 + maxHoles) {
        std::nth_element(
            polygon.begin() + 1, polygon.begin() + 1 + maxHoles, polygon.end(), [](const auto& a, const auto& b) {
                return std::fabs(signedArea(a)) > std::fabs(signedArea(b));
            });
        polygon.resize(1 + maxHoles);
    }
}

Feature::geometry_type convertGeometry(const GeometryTileFeature& geometryTileFeature, const CanonicalTileID& tileID) {
    MLN_TRACE_FUNC();

    const double size = util::EXTENT * std::pow(2, tileID.z);
    const double x0 = util::EXTENT * static_cast<double>(tileID.x);
    const double y0 = util::EXTENT * static_cast<double>(tileID.y);

    auto tileCoordinatesToLatLng = [&](const Point<int16_t>& p) {
        double y2 = 180 - (p.y + y0) * 360 / size;
        return Point<double>((p.x + x0) * 360 / size - 180, std::atan(std::exp(y2 * pi / 180)) * 360.0 / pi - 90.0);
    };

    const GeometryCollection& geometries = geometryTileFeature.getGeometries();

    switch (geometryTileFeature.getType()) {
        case FeatureType::Unknown: {
            assert(false);
            return Point<double>(NAN, NAN);
        }

        case FeatureType::Point: {
            MultiPoint<double> multiPoint;
            for (const auto& p : geometries.at(0)) {
                multiPoint.push_back(tileCoordinatesToLatLng(p));
            }
            if (multiPoint.size() == 1) {
                return multiPoint[0];
            } else {
                return multiPoint;
            }
        }

        case FeatureType::LineString: {
            MultiLineString<double> multiLineString;
            for (const auto& g : geometries) {
                LineString<double> lineString;
                for (const auto& p : g) {
                    lineString.push_back(tileCoordinatesToLatLng(p));
                }
                multiLineString.push_back(std::move(lineString));
            }
            if (multiLineString.size() == 1) {
                return multiLineString[0];
            } else {
                return multiLineString;
            }
        }

        case FeatureType::Polygon: {
            MultiPolygon<double> multiPolygon;
            for (const auto& pg : classifyRings(geometries)) {
                Polygon<double> polygon;
                for (const auto& r : pg) {
                    LinearRing<double> linearRing;
                    for (const auto& p : r) {
                        linearRing.push_back(tileCoordinatesToLatLng(p));
                    }
                    polygon.push_back(std::move(linearRing));
                }
                multiPolygon.push_back(std::move(polygon));
            }
            if (multiPolygon.size() == 1) {
                return multiPolygon[0];
            } else {
                return multiPolygon;
            }
        }
    }

    // Unreachable, but placate GCC.
    return Point<double>();
}

GeometryCollection convertGeometry(const Feature::geometry_type& geometryTileFeature, const CanonicalTileID& tileID) {
    MLN_TRACE_FUNC();

    const double size = util::EXTENT * std::pow(2, tileID.z);
    const double x0 = util::EXTENT * static_cast<double>(tileID.x);
    const double y0 = util::EXTENT * static_cast<double>(tileID.y);

    auto latLonToTileCoodinates = [&](const Point<double>& c) {
        Point<int16_t> p;

        auto x = (c.x + 180.0) * size / 360.0 - x0;
        p.x = int16_t(util::clamp<int64_t>(
            static_cast<int16_t>(x), std::numeric_limits<int16_t>::min(), std::numeric_limits<int16_t>::max()));

        auto y = (180 - (std::log(std::tan((c.y + 90) * pi / 360.0)) * 180 / pi)) * size / 360 - y0;
        p.y = int16_t(util::clamp<int64_t>(
            static_cast<int16_t>(y), std::numeric_limits<int16_t>::min(), std::numeric_limits<int16_t>::max()));

        return p;
    };

    return geometryTileFeature.match(
        [&](const Point<double>& point) -> GeometryCollection { return {{latLonToTileCoodinates(point)}}; },
        [&](const MultiPoint<double>& points) -> GeometryCollection {
            MultiPoint<int16_t> result;
            result.reserve(points.size());
            for (const auto& p : points) {
                result.emplace_back(latLonToTileCoodinates(p));
            }
            return {std::move(result)};
        },
        [&](const LineString<double>& lineString) -> GeometryCollection {
            LineString<int16_t> result;
            result.reserve(lineString.size());
            for (const auto& p : lineString) {
                result.emplace_back(latLonToTileCoodinates(p));
            }
            return {std::move(result)};
        },
        [&](const MultiLineString<double>& lineStrings) -> GeometryCollection {
            GeometryCollection result;
            result.reserve(lineStrings.size());
            for (const auto& line : lineStrings) {
                LineString<int16_t> temp;
                temp.reserve(line.size());
                for (const auto& p : line) {
                    temp.emplace_back(latLonToTileCoodinates(p));
                }
                result.emplace_back(temp);
            }
            return result;
        },
        [&](const Polygon<double>& polygon) -> GeometryCollection {
            GeometryCollection result;
            result.reserve(polygon.size());
            for (const auto& ring : polygon) {
                LinearRing<int16_t> temp;
                temp.reserve(ring.size());
                for (const auto& p : ring) {
                    temp.emplace_back(latLonToTileCoodinates(p));
                }
                result.emplace_back(temp);
            }
            return result;
        },
        [&](const MultiPolygon<double>& polygons) -> GeometryCollection {
            GeometryCollection result;
            result.reserve(polygons.size());
            for (const auto& pg : polygons) {
                for (const auto& r : pg) {
                    LinearRing<int16_t> ring;
                    ring.reserve(r.size());
                    for (const auto& p : r) {
                        ring.emplace_back(latLonToTileCoodinates(p));
                    }
                    result.emplace_back(ring);
                }
            }
            return result;
        },
        [](const auto&) -> GeometryCollection { return GeometryCollection(); });
}

Feature convertFeature(const GeometryTileFeature& geometryTileFeature, const CanonicalTileID& tileID) {
    MLN_TRACE_FUNC();

    Feature feature{convertGeometry(geometryTileFeature, tileID)};
    feature.properties = geometryTileFeature.getProperties();
    feature.id = geometryTileFeature.getID();
    return feature;
}

const PropertyMap& GeometryTileFeature::getProperties() const {
    static const PropertyMap dummy;
    return dummy;
}

const GeometryCollection& GeometryTileFeature::getGeometries() const {
    static const GeometryCollection dummy;
    return dummy;
}

GeometryCollectionFloat roundPolygonCorners(GeometryCollection& polygon, double desiredCornerDistance) {
    const int arcPoints = 3;
    const double maxEdgeLenPercent = 0.2;
    const double sinParallelTreshold = sin(util::deg2rad(5));
    GeometryCollectionFloat roundedCornerPolygon;

    for (auto ring : polygon) {
        GeometryCoordinatesFloat roundedCornerRing;

        std::size_t nVertices = ring.size() - 1;
        for (std::size_t i = 0; i < nVertices; i++) {
            auto prevPoint = convertPoint<double>(ring[(i - 1 + nVertices) % nVertices]);
            auto cornerPoint = convertPoint<double>(ring[i]);
            auto nextPoint = convertPoint<double>(ring[(i + 1) % nVertices]);

            auto edge1Len = util::dist<double>(cornerPoint, prevPoint);
            auto edge2Len = util::dist<double>(cornerPoint, nextPoint);
            if (edge1Len == 0.0 || edge2Len == 0.0) {
                // Duplicate vertex: no direction to round, keep it as-is.
                roundedCornerRing.emplace_back(convertPoint<float>(ring[i]));
                continue;
            }

            // Compute the start and end points of the rounded corner
            auto edge1Vector = util::normal<double>(prevPoint, cornerPoint);
            auto edge2Vector = util::normal<double>(cornerPoint, nextPoint);

            auto edge1MaxCornerDistance = edge1Len * maxEdgeLenPercent;
            auto edge2MaxCornerDistance = edge2Len * maxEdgeLenPercent;
            auto cornerDistance = std::min({desiredCornerDistance, edge1MaxCornerDistance, edge2MaxCornerDistance});

            auto startPoint = cornerPoint - edge1Vector * cornerDistance;
            auto endPoint = cornerPoint + edge2Vector * cornerDistance;

            // Perpendicular directions
            auto perp1Vector = util::perp(edge1Vector);
            auto perp2Vector = util::perp(edge2Vector);

            // Ensure perpendiculars point toward same side
            if (util::crossProduct(edge1Vector, edge2Vector) < 0) {
                perp1Vector *= -1.0;
                perp2Vector *= -1.0;
            }

            // Center = intersection of perpendiculars
            auto perpCrossProduct = util::crossProduct(perp1Vector, perp2Vector);
            if (std::abs(perpCrossProduct) < sinParallelTreshold) {
                roundedCornerRing.emplace_back(convertPoint<float>(ring[i]));
                continue;
            }
            auto t = util::crossProduct(endPoint - startPoint, perp2Vector) / perpCrossProduct;
            auto centerPoint = startPoint + perp1Vector * t;

            // Add the start point of the rounded corner
            roundedCornerRing.emplace_back(convertPoint<float>(startPoint));

            // Generate points along the arc
            auto radius = util::dist<double>(startPoint, centerPoint);
            auto startAngle = std::atan2(startPoint.y - centerPoint.y, startPoint.x - centerPoint.x);
            auto arcAngle = util::angle_between(startPoint - centerPoint, endPoint - centerPoint);
            for (int k = 1; k <= arcPoints; k++) {
                double angle = startAngle + arcAngle * k / (arcPoints + 1);
                auto arcPoint = Point<double>(centerPoint.x + std::cos(angle) * radius,
                                              centerPoint.y + std::sin(angle) * radius);
                roundedCornerRing.emplace_back(convertPoint<float>(arcPoint));
            }

            // Add the end point of the rounded corner
            roundedCornerRing.emplace_back(convertPoint<float>(endPoint));
        }
        // Close the ring by repeating the first point at the end
        roundedCornerRing.emplace_back(roundedCornerRing[0]);

        // Add the ring to the rounded corner polygon
        roundedCornerPolygon.emplace_back(roundedCornerRing);
    }
    return roundedCornerPolygon;
}
} // namespace mln
