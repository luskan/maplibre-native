#ifdef NDEBUG
#error "Capture integration checks require assertions"
#endif
#include <mln/renderer/tile_pyramid.hpp>
#include <mln/util/tile_trace.hpp>
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <new>

std::atomic<size_t> allocations{0};
std::atomic<bool> toggleOnAllocation{false};
mln::tiletrace::ID replacementView = 0;
void* operator new(size_t size)
{
  ++allocations;
  if (toggleOnAllocation.exchange(false))
  {
    mln::tiletrace::configure(false); mln::tiletrace::configure(true);
    const mln::tiletrace::ViewTile key{4, 5, 0, 10, 10};
    replacementView = mln::tiletrace::updateView(1, 2, 3, &key, 1);
  }
  if (auto* value = std::malloc(size ? size : 1)) return value;
  throw std::bad_alloc();
}
void operator delete(void* value) noexcept { std::free(value); }
void operator delete(void* value, size_t) noexcept { std::free(value); }

int main()
{
  using namespace mln;
  using namespace mln::tiletrace;
  configure(true, true);
  TilePyramid pyramid(TaggedScheduler(nullptr, 0));
  pyramid.traceMap = 1; pyramid.traceSource = 2; pyramid.traceStyle = 1;
  const std::vector<OverscaledTileID> keys{{10, 4, 5}, {10, 5, 5}};
  pyramid.updateTraceView(keys); pyramid.updateTraceView(keys);
  const auto oldView = pyramid.traceView;
  configure(false); configure(true);
  pyramid.updateTraceView(keys);
  assert(pyramid.traceView && pyramid.traceView != oldView);
  std::cout << "resume_without_frame\n{\"fresh\":true}\n";

  configure(false);
  const auto serial = viewSerial();
  const auto before = allocations.load();
  for (size_t i = 0; i < 10000; ++i) pyramid.updateTraceView(keys);
  assert(!pyramid.traceView && allocations == before && viewSerial() == serial);
  std::cout << "paused_pyramid\n{\"allocations\":0,\"viewChanges\":0}\n";

  configure(true);
  toggleOnAllocation = true;
  pyramid.updateTraceView(keys);
  assert(!toggleOnAllocation && !pyramid.traceView && replacementView);
  pyramid.traceStyle = 3;
  const std::vector<OverscaledTileID> replacement{{10, 4, 5}};
  pyramid.updateTraceView(replacement);
  assert(pyramid.traceView == replacementView);
  std::cout << "toggle_while_preparing_keys\n{\"staleRejected\":true}\n";
}
