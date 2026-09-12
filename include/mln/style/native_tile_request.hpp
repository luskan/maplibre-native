#pragma once

#include <mln/tile/tile_id.hpp>

#include <atomic>
#include <memory>
#include <string>

namespace mln {

class NativeRequestState;
struct NativeSourceIdentity;
struct NativeCanonicalState;

struct NativeRequestBinding
{
  // The key describes all inputs that affect output, without request or profiler identity.
  std::string key;
  std::shared_ptr<const void> inputs;
};

class NativeInputValidity final
{
public:
  bool isCurrent() const noexcept;
  uint64_t sourceEpoch() const noexcept;
  uint64_t contentEpoch() const noexcept { return contentEpoch_; }
  const CanonicalTileID& tileID() const noexcept { return tileID_; }
  const NativeRequestBinding& binding() const noexcept { return binding_; }

private:
  friend class NativeRequestState;
  NativeInputValidity(std::shared_ptr<const NativeSourceIdentity>, std::shared_ptr<NativeCanonicalState>,
                      CanonicalTileID, uint64_t, NativeRequestBinding);

  const std::shared_ptr<const NativeSourceIdentity> source_;
  const std::shared_ptr<NativeCanonicalState> canonical_;
  const CanonicalTileID tileID_;
  const uint64_t contentEpoch_;
  const NativeRequestBinding binding_;
};

class NativeRequestTicket
{
public:
  NativeRequestTicket() = default;
  explicit operator bool() const noexcept { return bool(validity_); }
  bool isCurrent() const noexcept { return validity_ && validity_->isCurrent(); }
  uint64_t sourceEpoch() const noexcept { return validity_ ? validity_->sourceEpoch() : 0; }
  uint64_t contentEpoch() const noexcept { return validity_ ? validity_->contentEpoch() : 0; }
  const CanonicalTileID& tileID() const;
  const NativeRequestBinding& binding() const;
  std::shared_ptr<const NativeInputValidity> validity() const noexcept { return validity_; }
  bool operator==(const NativeRequestTicket&) const = default;

private:
  friend class NativeRequestState;
  explicit NativeRequestTicket(std::shared_ptr<const NativeInputValidity> value) : validity_(std::move(value)) {}
  std::shared_ptr<const NativeInputValidity> validity_;
};

} // namespace mln
