#include <mln/style/native_tile_request_state.hpp>
#include <mln/util/tile_range.hpp>

#include <stdexcept>
#include <utility>

namespace mln {

struct NativeSourceIdentity
{
  explicit NativeSourceIdentity(uint64_t value) : epoch(value) {}
  const uint64_t epoch;
  std::atomic<bool> active{true};
};

struct NativeCanonicalState
{
  uint64_t revision = 0;
  std::atomic<uint64_t> currentEpoch{0};
  std::weak_ptr<NativeInputValidity> current;
};

namespace {
std::atomic<uint64_t> sourceEpochCounter{0};
}

NativeInputValidity::NativeInputValidity(std::shared_ptr<const NativeSourceIdentity> source,
                                       std::shared_ptr<NativeCanonicalState> canonical,
                                       CanonicalTileID tile, uint64_t epoch, NativeRequestBinding binding)
  : source_(std::move(source)), canonical_(std::move(canonical)), tileID_(tile), contentEpoch_(epoch),
    binding_(std::move(binding))
{
}

bool NativeInputValidity::isCurrent() const noexcept
{
  return canonical_->currentEpoch.load(std::memory_order_acquire) == contentEpoch_
      && source_->active.load(std::memory_order_acquire);
}

uint64_t NativeInputValidity::sourceEpoch() const noexcept
{
  return source_->epoch;
}

const CanonicalTileID& NativeRequestTicket::tileID() const
{
  if (!validity_) throw std::logic_error("Empty native request ticket");
  return validity_->tileID();
}

const NativeRequestBinding& NativeRequestTicket::binding() const
{
  if (!validity_) throw std::logic_error("Empty native request ticket");
  return validity_->binding();
}

NativeRequestState::NativeRequestState(const style::CustomGeometrySource::TileOptions& options)
  : source_(std::make_shared<NativeSourceIdentity>(sourceEpochCounter.fetch_add(1, std::memory_order_relaxed) + 1)),
    contract_(nativeTileConversionContract(options))
{
}

NativeRequestState::~NativeRequestState()
{
  retire();
}

uint64_t NativeRequestState::sourceEpoch() const noexcept
{
  return source_->epoch;
}

NativeRequestResolution NativeRequestState::resolve(
    const CanonicalTileID& tile, const std::function<NativeRequestBinding(const CanonicalTileID&)>& resolver)
{
  std::shared_ptr<NativeCanonicalState> slot;
  uint64_t revision;
  {
    std::lock_guard lock(mutex_);
    if (!source_->active.load(std::memory_order_acquire)) return {};
    if (--pruneCountdown_ == 0)
    {
      std::erase_if(slots_, [](const auto& entry) { return entry.second.expired(); });
      pruneCountdown_ = 64;
    }
    auto& weak = slots_[tile];
    slot = weak.lock();
    if (!slot)
    {
      slot = std::make_shared<NativeCanonicalState>();
      weak = slot;
    }
    revision = slot->revision;
  }

  NativeRequestBinding binding;
  NativeRequestResolution result;
  try
  {
    binding = resolver(tile);
    if (binding.key.empty() || !binding.inputs)
      throw std::invalid_argument("Native binding requires a full key and owned inputs");
  }
  catch (...)
  {
    result.error = std::current_exception();
    binding = {};
  }

  std::shared_ptr<NativeInputValidity> previous;
  uint64_t epoch;
  {
    std::lock_guard lock(mutex_);
    if (!source_->active.load(std::memory_order_acquire) || revision != slot->revision) return {};
    previous = slot->current.lock();
    if (previous && previous->isCurrent() && previous->binding().key == binding.key)
    {
      result.ticket = NativeRequestTicket(previous);
      return result;
    }
    else
    {
      epoch = ++nextContentEpoch_;
    }
  }
  auto current = std::shared_ptr<NativeInputValidity>(
      new NativeInputValidity(source_, slot, tile, epoch, std::move(binding)));
  {
    std::lock_guard lock(mutex_);
    if (!source_->active.load(std::memory_order_acquire) || revision != slot->revision) return {};
    ++slot->revision;
    slot->currentEpoch.store(epoch, std::memory_order_release);
    slot->current = current;
    result.ticket = NativeRequestTicket(current);
  }
  return result;
}

void NativeRequestState::retireSlot(const std::shared_ptr<NativeCanonicalState>& slot)
{
  ++slot->revision;
  slot->currentEpoch.store(0, std::memory_order_release);
  slot->current.reset();
}

void NativeRequestState::invalidate(const CanonicalTileID& tile)
{
  std::shared_ptr<NativeCanonicalState> slot;
  {
    std::lock_guard lock(mutex_);
    const auto found = slots_.find(tile);
    if (found != slots_.end() && (slot = found->second.lock())) retireSlot(slot);
  }
}

void NativeRequestState::invalidateRegion(const LatLngBounds& bounds)
{
  std::lock_guard lock(mutex_);
  for (const auto& [tile, weak] : slots_)
  {
    auto range = util::TileRange::fromLatLngBounds(bounds, tile.z);
    if (range.contains(tile))
    {
      if (auto slot = weak.lock()) retireSlot(slot);
    }
  }
}

void NativeRequestState::clear()
{
  std::lock_guard lock(mutex_);
  // Slots own only weak validity references, so retirement cannot destroy caller inputs.
  // Avoid allocating here because clear is also used during failure recovery and teardown.
  for (const auto& [tile, weak] : slots_)
  {
    if (auto slot = weak.lock()) retireSlot(slot);
  }
  slots_.clear();
}

void NativeRequestState::retire()
{
  source_->active.store(false, std::memory_order_release);
  clear();
}

} // namespace mln
