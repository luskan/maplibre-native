#pragma once

#include <mln/util/color.hpp>
#include <array>

namespace mln {

// Widths are in logical pixels by road category. Insets are fractions of the full road width.
struct alignas(16) LineTrafficPaint {
    std::array<float, 20> widths{};
    float singleLaneScale = 1;
    float multiLaneScale = 1;
    float enabled = 0;
    float padding = 0;
    Color outline{0, 0, 0, 0};
    float monoInset = 0;
    float splitOuterInset = 0;
    float splitInnerInset = 0;
    float inverseTexelWidth = 1;
};
static_assert(sizeof(LineTrafficPaint) == 8 * 16);

} // namespace mln
