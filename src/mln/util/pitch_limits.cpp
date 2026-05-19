#include <mln/util/pitch_limits.hpp>

#if AM_MAPLIBRE_RUNTIME_PITCH_LIMIT

#include <algorithm>
#include <atomic>
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

std::atomic<double>& runtimeMaxPitchDegrees() noexcept {
    static std::atomic<double> value{loadRuntimeMaxPitchDegrees()};
    return value;
}

} // namespace

double maxPitchDegrees() noexcept {
    return runtimeMaxPitchDegrees().load(std::memory_order_relaxed);
}

double maxPitchRadians() noexcept {
    return deg2rad(maxPitchDegrees());
}

void setMaxPitchDegrees(double degrees) noexcept {
    if (!std::isfinite(degrees)) {
        return;
    }
    runtimeMaxPitchDegrees().store(std::clamp(degrees, 0.0, 89.0), std::memory_order_relaxed);
}

} // namespace util
} // namespace mln

#endif
