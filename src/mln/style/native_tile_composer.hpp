#pragma once

#include <mln/tile/native_tile_payload.hpp>

namespace mln
{

// Composes producer-clipped geographic features with clip and wrap disabled.
// A failed append or seal permanently closes the composer.
class NativeGeometryComposer
{
public:
  explicit NativeGeometryComposer(NativeTileMetadata);

  // Properties and IDs are moved into final records, so mutable aliases must be relinquished.
  void append(GeoJSONFeature&&);
  NativeTilePayloadPtr seal() &&;

private:
  void checkOpen() const;

  const NativeTileMetadata metadata_;
  NativeTileBuilder builder_;
  bool closed_ = false;
};

} // namespace mln
