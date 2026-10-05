#pragma once

#include <mln/gfx/drawable_data.hpp>
#include <mln/util/color.hpp>
#include <array>

namespace mln::shaders
{
struct alignas(16) LineRoadPropsUBO
{
  Color casingColor;
  std::array<float, 4> guardRect;
  float fillWidth;
  float guardWidth;
  float fillWidthT;
  float casingColorT;
  float maxGuardDistance;
  float length = 0;
  float lengthT = 0;
  float pad3 = 0;
};
static_assert(sizeof(LineRoadPropsUBO) == 64);

struct LineRoadDrawableData : gfx::DrawableData
{
  std::array<float, 4> guardRect;
  bool unguarded = false;
};
}
