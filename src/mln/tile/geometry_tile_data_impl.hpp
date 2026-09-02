#pragma once

#include <mln/tile/geometry_tile_data.hpp>

#include <optional>

namespace mln::detail {

std::optional<GeometryCollection> tryFixupSimplePolygon(const GeometryCollection&);
bool isSimplePolygonFixupEnabled() noexcept;
// This changes only future fixup calls. Restart or reload tiles before comparing modes.
void setSimplePolygonFixupEnabled(bool) noexcept;

} // namespace mln::detail
