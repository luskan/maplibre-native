#include <mln/util/pitch_limits.hpp>

#if AM_MAPLIBRE_RUNTIME_PITCH_LIMIT

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace mln {
namespace util {

namespace {

double loadRuntimeMaxPitchDegrees() noexcept {
    char const* value = std::getenv("AM_MAPLIBRE_MAX_PITCH_DEG");
    if (!value || !*value) {
        return defaultMaxPitchDegrees();
    }

    char* end = nullptr;
    double const parsed = std::strtod(value, &end);
    if (end == value || !std::isfinite(parsed)) {
        return defaultMaxPitchDegrees();
    }

    return std::clamp(parsed, 0.0, 89.0);
}

} // namespace

double maxPitchDegrees() noexcept {
    static double const value = loadRuntimeMaxPitchDegrees();
    return value;
}

double maxPitchRadians() noexcept {
    return deg2rad(maxPitchDegrees());
}

} // namespace util
} // namespace mln

#endif
