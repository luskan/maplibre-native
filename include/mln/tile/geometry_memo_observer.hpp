#pragma once

#include <mln/util/geojson.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace mln {

// Callbacks may run concurrently and must not call geometry, source, or map APIs.
// Shared ownership lets observations outlive their source.
class GeometryMemoObserver {
public:
  using Features = mapbox::feature::feature_collection<int16_t>;
  virtual ~GeometryMemoObserver() = default;

  virtual void onDataCreated(const std::shared_ptr<const Features>&, std::size_t memoTableBytes) noexcept = 0;
  virtual void onDataDestroyed(std::size_t memoTableBytes) noexcept = 0;

  // Tokens are passive clock samples, so failed materializations need no completion callback.
  virtual uint64_t onGeometryStart() noexcept = 0;
  virtual void onGeometryCreated(uint64_t token, bool polygon, std::size_t retainedBytes) noexcept = 0;
  virtual void onGeometryReleased(std::size_t retainedBytes) noexcept = 0;
  virtual void onGeometryAccess() noexcept = 0;
};

} // namespace mln
