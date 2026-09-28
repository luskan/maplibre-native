#include <mln/tile/native_tile_memory.hpp>

#include <algorithm>
#include <stdexcept>

namespace mln
{

NativeTileMemoryLease::NativeTileMemoryLease(Key, std::shared_ptr<NativeTileMemoryState> state_, size_t bytes_)
  : state(std::move(state_)), bytes(bytes_) {}

NativeTileMemoryLease::~NativeTileMemoryLease()
{
  std::lock_guard lock(state->mutex);
  state->used -= bytes;
}

size_t NativeTileMemoryLease::size() const
{
  std::lock_guard lock(state->mutex);
  return bytes;
}

void NativeTileMemoryLease::shrink(size_t target)
{
  std::lock_guard lock(state->mutex);
  if(target > bytes || (allocation && target != bytes))
    throw std::logic_error("Cannot resize a bound native cache reservation");
  state->used -= bytes - target;
  bytes = target;
}

std::shared_ptr<NativeTileMemoryLease> NativeTileMemoryLease::split(size_t amount)
{
  std::lock_guard lock(state->mutex);
  if(amount > bytes || allocation) throw std::logic_error("Cannot split a bound native cache reservation");
  auto result = std::make_shared<NativeTileMemoryLease>(Key{}, state, amount);
  bytes -= amount;
  return result;
}

bool NativeTileMemoryLease::bind(const void* value, size_t minimumBytes)
{
  std::lock_guard lock(state->mutex);
  if(!value || allocation || bytes < minimumBytes) return false;
  allocation = value;
  return true;
}

NativeTileMemoryBudget::NativeTileMemoryBudget(size_t capacity) : state(std::make_shared<NativeTileMemoryState>())
{
  state->capacity = capacity;
}

std::shared_ptr<NativeTileMemoryLease> NativeTileMemoryBudget::reserve(size_t amount)
{
  std::lock_guard lock(state->mutex);
  if(amount > state->capacity - state->used)
  {
    ++state->denied;
    return {};
  }
  auto lease = std::make_shared<NativeTileMemoryLease>(NativeTileMemoryLease::Key{}, state, amount);
  state->used += amount;
  state->peak = std::max(state->peak, state->used);
  return lease;
}

NativeTileMemoryBudget::Snapshot NativeTileMemoryBudget::snapshot() const
{
  std::lock_guard lock(state->mutex);
  return {state->capacity, state->used, state->peak, state->denied};
}

} // namespace mln
