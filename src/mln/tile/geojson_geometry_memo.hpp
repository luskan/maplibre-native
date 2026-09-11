#pragma once

#include <mln/tile/geometry_tile_data.hpp>
#include <mln/tile/geometry_memo_observer.hpp>

#include <atomic>
#include <mutex>

namespace mln {

class GeoJSONGeometryMemo {
public:
  using Features = mapbox::feature::feature_collection<int16_t>;

  GeoJSONGeometryMemo(std::shared_ptr<const Features>, bool memoizeGeometry,
                     std::shared_ptr<GeometryMemoObserver> = nullptr);
  ~GeoJSONGeometryMemo();
  GeoJSONGeometryMemo(const GeoJSONGeometryMemo&) = delete;
  GeoJSONGeometryMemo& operator=(const GeoJSONGeometryMemo&) = delete;

  bool isEnabled() const { return memoize; }
  const std::shared_ptr<GeometryMemoObserver>& getObserver() const { return observer; }
  const GeometryCollection& get(std::size_t, std::optional<GeometryCollection>&) const;
  void release(const std::optional<GeometryCollection>&) const;

private:
  struct Entry {
    std::once_flag ready;
    std::optional<GeometryCollection> geometry;
  };

  GeometryCollection build(std::size_t) const;
  std::shared_ptr<const Features> features;
  const bool memoize;
  const std::shared_ptr<GeometryMemoObserver> observer;
  std::unique_ptr<Entry[]> entries;
  mutable std::atomic<std::size_t> retained{0};
};

} // namespace mln
