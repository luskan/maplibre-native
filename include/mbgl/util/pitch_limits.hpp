#pragma once

#include <mbgl/math/angles.hpp>

#ifndef AM_MAPLIBRE_RUNTIME_PITCH_LIMIT
#define AM_MAPLIBRE_RUNTIME_PITCH_LIMIT 0
#endif

namespace mbgl {
namespace util {

constexpr double defaultMaxPitchDegrees() noexcept {
    return 70.0;
}

#if AM_MAPLIBRE_RUNTIME_PITCH_LIMIT

double maxPitchDegrees() noexcept;
double maxPitchRadians() noexcept;
void setMaxPitchDegrees(double degrees) noexcept;

#else

constexpr double maxPitchDegrees() noexcept {
    return defaultMaxPitchDegrees();
}

constexpr double maxPitchRadians() noexcept {
    return deg2rad(maxPitchDegrees());
}

inline void setMaxPitchDegrees(double) noexcept {}

#endif

} // namespace util
} // namespace mbgl
