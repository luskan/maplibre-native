#include <mln/util/pitch_limits.hpp>

#if AM_MAPLIBRE_RUNTIME_PITCH_LIMIT

#include <algorithm>
#include <cmath>
#include <cstdlib>

#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

namespace mln {
namespace util {

namespace {

constexpr char kMaxPitchEnvVar[] = "AM_MAPLIBRE_MAX_PITCH_DEG";

#ifdef __ANDROID__
constexpr char kMaxPitchAndroidProperty[] = "debug.automapa.maplibre.pitch";
#endif

bool parseMaxPitchDegrees(char const* value, double& result) noexcept {
    if (!value || !*value) {
        return false;
    }
    char* end = nullptr;
    double const parsed = std::strtod(value, &end);
    if (end == value || !std::isfinite(parsed)) {
        return false;
    }

    result = std::clamp(parsed, 0.0, 89.0);
    return true;
}

double loadRuntimeMaxPitchDegrees() noexcept {
    double parsed = 0.0;

#ifdef __ANDROID__
    char propertyValue[PROP_VALUE_MAX] = {};
    if (__system_property_get(kMaxPitchAndroidProperty, propertyValue) > 0 &&
        parseMaxPitchDegrees(propertyValue, parsed)) {
        return parsed;
    }
#endif

    if (parseMaxPitchDegrees(std::getenv(kMaxPitchEnvVar), parsed)) {
        return parsed;
    }

    return defaultMaxPitchDegrees();
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
