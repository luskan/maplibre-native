#pragma once

#include <mln/math/angles.hpp>

#ifndef AM_MAPLIBRE_RUNTIME_PITCH_LIMIT
#define AM_MAPLIBRE_RUNTIME_PITCH_LIMIT 0
#endif

namespace mln {
namespace util {

constexpr double defaultMaxPitchDegrees() noexcept {
    return 83.0;
}

#if AM_MAPLIBRE_RUNTIME_PITCH_LIMIT

double maxPitchDegrees() noexcept;
double maxPitchRadians() noexcept;

#else

constexpr double maxPitchDegrees() noexcept {
    return defaultMaxPitchDegrees();
}

constexpr double maxPitchRadians() noexcept {
    return deg2rad(maxPitchDegrees());
}

#endif

} // namespace util
} // namespace mln
