#include <mln/style/native_tile_request_state.hpp>

#include <cstdlib>
#include <new>

namespace
{
thread_local bool rejectAllocation = false;
}

void* operator new(std::size_t size)
{
  if (rejectAllocation) throw std::bad_alloc();
  if (auto* value = std::malloc(size ? size : 1)) return value;
  throw std::bad_alloc();
}
void operator delete(void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }

int main()
{
  using namespace mln;
  NativeRequestState state({});
  auto resolve = [&](CanonicalTileID tile) {
    return state.resolve(tile, [](auto const&) {
      return NativeRequestBinding{"complete-key", std::make_shared<int const>(1)};
    }).ticket;
  };
  auto a = resolve({16, 36593, 21579});
  auto b = resolve({4, 1, 1});
  try
  {
    rejectAllocation = true;
    state.invalidateRegion(LatLngBounds::world());
    rejectAllocation = false;
    if (a.isCurrent() || b.isCurrent()) return 1;
    a = resolve({16, 36593, 21579});
    b = resolve({4, 1, 1});
    rejectAllocation = true;
    state.clear();
    rejectAllocation = false;
    if (a.isCurrent() || b.isCurrent()) return 2;
    a = resolve({16, 36593, 21579});
    rejectAllocation = true;
    state.retire();
    rejectAllocation = false;
    if (a.isCurrent()) return 3;
  }
  catch(...)
  {
    rejectAllocation = false;
    return 4;
  }
  return 0;
}
