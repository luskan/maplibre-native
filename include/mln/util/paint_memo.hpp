#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace mln::paintmemo {

enum class Mode : uint8_t { Off, Reuse, Verify };

struct Policy
{
  Mode mode = Mode::Off;
  uint64_t generation = 0;
};

inline bool operator==(Policy a, Policy b) noexcept
{
  return a.mode == b.mode && a.generation == b.generation;
}

struct Statistics
{
  Policy applied;
  uint64_t scopes = 0, binders = 0, eligible = 0, admitted = 0;
  uint64_t calls = 0, hits = 0, misses = 0, bypasses = 0, endpointEvaluations = 0;
  uint64_t verifiedHits = 0, mismatches = 0, capacityFallbacks = 0, budgetFallbacks = 0;
  uint64_t allocationFallbacks = 0, allocatedBytes = 0;
};

namespace detail {
inline std::atomic<uint64_t> requested{0}, live{0}, peak{0};
inline thread_local Statistics* current = nullptr;
} // namespace detail

inline Policy policy() noexcept
{
  const auto value = detail::requested.load();
  return {static_cast<Mode>(value % 4), value / 4};
}

inline bool configure(unsigned mode) noexcept
{
  if (mode > 2) return false;
  auto previous = detail::requested.load();
  while (!detail::requested.compare_exchange_weak(previous, ((previous / 4) + 1) * 4 + mode)) {}
  return true;
}

constexpr uint64_t MemoryLimit = 1024 * 1024;
inline uint64_t liveBytes() noexcept { return detail::live.load(); }
inline uint64_t peakBytes() noexcept { return detail::peak.load(); }

inline bool reserve(size_t bytes) noexcept
{
  auto previous = detail::live.load();
  do
  {
    if (bytes > MemoryLimit || previous > MemoryLimit - bytes) return false;
  } while (!detail::live.compare_exchange_weak(previous, previous + bytes));
  auto peak = detail::peak.load();
  while (peak < previous + bytes && !detail::peak.compare_exchange_weak(peak, previous + bytes)) {}
  return true;
}

inline void release(size_t bytes) noexcept { detail::live.fetch_sub(bytes); }
inline Statistics* current() noexcept { return detail::current; }

class Scope
{
public:
  explicit Scope(Statistics& value) noexcept : previous(detail::current) { detail::current = &value; }
  ~Scope() { detail::current = previous; }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;
private:
  Statistics* previous;
};

} // namespace mln::paintmemo
