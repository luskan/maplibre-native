#pragma once

#include <mln/tile/geometry_memo_observer.hpp>

#include <atomic>

namespace mln::test {

class GeometryMemoTestObserver final : public GeometryMemoObserver {
public:
  std::atomic<std::size_t> holders{0}, tableBytes{0}, geometryBytes{0};
  std::atomic<std::size_t> accesses{0}, materializations{0}, repairs{0}, clockSamples{0};
  std::atomic<uint64_t> lastToken{0};

  void onDataCreated(const std::shared_ptr<const Features>&, std::size_t bytes) noexcept override {
    ++holders;
    tableBytes += bytes;
  }
  void onDataDestroyed(std::size_t bytes) noexcept override {
    --holders;
    tableBytes -= bytes;
  }
  uint64_t onGeometryStart() noexcept override {
    ++clockSamples;
    return 17;
  }
  void onGeometryCreated(uint64_t token, bool polygon, std::size_t bytes) noexcept override {
    ++materializations;
    if (polygon) ++repairs;
    geometryBytes += bytes;
    lastToken = token;
  }
  void onGeometryReleased(std::size_t bytes) noexcept override { geometryBytes -= bytes; }
  void onGeometryAccess() noexcept override { ++accesses; }
};

} // namespace mln::test
