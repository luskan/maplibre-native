#pragma once

#include <mln/style/native_tile_request.hpp>
#include <mln/tile/native_tile_payload.hpp>

#include <exception>
#include <functional>
#include <map>
#include <mutex>

namespace mln {

struct NativeRequestResolution
{
  NativeRequestTicket ticket;
  std::exception_ptr error;
};

class NativeRequestState final
{
public:
  explicit NativeRequestState(const style::CustomGeometrySource::TileOptions&);
  ~NativeRequestState();
  NativeRequestResolution resolve(const CanonicalTileID&,
                                  const std::function<NativeRequestBinding(const CanonicalTileID&)>&);
  void invalidate(const CanonicalTileID&);
  void invalidateRegion(const LatLngBounds&);
  void clear();
  void retire();
  uint64_t sourceEpoch() const noexcept;
  const NativeTileConversionContract& contract() const noexcept { return contract_; }

private:
  void retireSlot(const std::shared_ptr<NativeCanonicalState>&);

  const std::shared_ptr<NativeSourceIdentity> source_;
  const NativeTileConversionContract contract_;
  std::mutex mutex_;
  std::map<CanonicalTileID, std::weak_ptr<NativeCanonicalState>> slots_;
  uint64_t nextContentEpoch_ = 0;
  unsigned pruneCountdown_ = 64;
};

} // namespace mln
