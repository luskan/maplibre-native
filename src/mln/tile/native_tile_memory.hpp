#pragma once

#include <cstddef>
#include <memory>
#include <mutex>

namespace mln
{

struct NativeTileMemoryState
{
  std::mutex mutex;
  size_t capacity = 0;
  size_t used = 0;
  size_t peak = 0;
  size_t denied = 0;
};

class NativeTileMemoryLease
{
  struct Key
  {
  private:
    Key() = default;
    friend class NativeTileMemoryLease;
    friend class NativeTileMemoryBudget;
  };
  friend class NativeTileMemoryBudget;
public:
  NativeTileMemoryLease(Key, std::shared_ptr<NativeTileMemoryState>, size_t);
  ~NativeTileMemoryLease();
  NativeTileMemoryLease(const NativeTileMemoryLease&) = delete;
  NativeTileMemoryLease& operator=(const NativeTileMemoryLease&) = delete;
  size_t size() const;
  void shrink(size_t);
  std::shared_ptr<NativeTileMemoryLease> split(size_t);
  bool bind(const void* allocation, size_t minimumBytes);
  const void* owner() const noexcept { return state.get(); }
private:
  std::shared_ptr<NativeTileMemoryState> state;
  size_t bytes;
  const void* allocation = nullptr;
};

class NativeTileMemoryBudget
{
public:
  explicit NativeTileMemoryBudget(size_t);
  std::shared_ptr<NativeTileMemoryLease> reserve(size_t);
  struct Snapshot { size_t capacity, used, peak, denied; };
  Snapshot snapshot() const;
  const void* identity() const noexcept { return state.get(); }
private:
  std::shared_ptr<NativeTileMemoryState> state;
};

} // namespace mln
