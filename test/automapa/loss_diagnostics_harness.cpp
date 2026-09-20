#include <functional>
#include <future>
#include <iostream>
#include <thread>
#include <vector>

void lossHook(const char* point);
#define TILE_TRACE_TEST_HOOK(point) lossHook(point)
#include "../../src/mln/util/tile_trace.cpp"

using namespace mln::tiletrace;
std::function<void(const std::string&)> hook;
void lossHook(const char* point)
{
  if (hook) hook(point);
}
void dump(const char* name)
{
  std::cout << name << '\n' << snapshotJSON() << '\n';
}
void locked(std::mutex& mutex, const std::function<void()>& action)
{
  std::promise<void> held, release;
  auto released = release.get_future();
  std::thread owner([&]
  {
    std::lock_guard<std::mutex> lock(mutex);
    held.set_value();
    released.wait();
  });
  held.get_future().wait();
  action();
  release.set_value();
  owner.join();
}
int main()
{
  auto& c = collector();
  configure(true, true);
  dump("initial");
  locked(c.mutex, [] { create(1, 2, 3, 10, 1, 1, 10, 0); });
  dump("store_lock");
  locked(c.mutex, [] { pump(1, true); });
  dump("activity_lock");
  auto demand = create(1, 2, 3, 10, 1, 1, 10, 0);
  locked(c.mutex, [&] { bindDemand(demand); });
  dump("bind_lock");
  auto missing = demand;
  missing.demand += 1000;
  bindDemand(missing);
  dump("bind_missing");
  locked(c.mutex, [] { retireSource(2); });
  dump("retire_lock");
  {
    FrameScope scope(2);
    auto retained = demand;
    retained.session -= 1;
    retained.generation = 1;
    locked(c.mutex, [&] { draw(retained, false); });
    dump("retained_lock");
    locked(c.surfaceMutex, [] { swapBegin(99); });
    dump("swap_surface_lock");
    locked(c.mutex, [] { swapEnd(false); });
    dump("swap_records_lock");
  }
  locked(c.surfaceMutex, [] { surfaceCreated(99); });
  dump("surface_create_lock");
  for (uintptr_t i = 1; i <= c.surfaces.size() + 1; ++i) surfaceCreated(i);
  dump("surface_capacity");
  locked(c.surfaceMutex, [] { surfaceDestroyed(99); });
  dump("surface_destroy_lock");
  BatchInfo info;
  info.id = nextID(); info.session = session();
  for (size_t i = 0; i <= BatchMemberCapacity; ++i) trackBatchMember(info, demand);
  dump("member_capacity");
  configure(true, true);
  dump("reset");
  std::array<ViewTile, 9> tiles;
  for (size_t i = 0; i < tiles.size(); ++i) tiles[i] = {1, 1, static_cast<int16_t>(i), 10, 10};
  const auto view = updateView(1, 2, 3, tiles.data(), tiles.size());
  auto publication = create(1, 2, 3, 10, 1, 1, 10, 0, 2, view);
  publication.id = publication.publication = nextID(); publication.kind = Kind::Publication;
  mark(publication, Producer);
  info.id = nextID(); info.session = session();
  trackBatchMember(info, publication);
  dump("use_capacity");
  batchLoss(1, 2, LostEvent::Cache);
  batchLoss(1, 2, LostEvent::Screen);
  batchLoss(1, 2, LostEvent::Outcome);
  batchLoss(1, 2, LostEvent::View);
  loss();
  dump("batch_and_explicit");

  configure(true, true);
  dump("before_observer");
  bool once = true;
  hook = [&](const std::string& point)
  {
    if (once && point == "snapshot_copy")
    {
      once = false;
      std::thread writer([] { pump(1, true); });
      writer.join();
    }
  };
  dump("observer_loss");
  hook = {};
  dump("after_observer");

  once = true;
  hook = [&](const std::string& point)
  {
    if (once && point == "loss_diagnostic_write")
    {
      once = false;
      std::cout << "during_writer\n" << lossDiagnosticsJSON(session(), c.lost.load()) << '\n';
    }
  };
  loss();
  hook = {};
  dump("after_writer");
  once = true;
  hook = [&](const std::string& point)
  {
    if (once && point == "loss_diagnostic_read")
    {
      once = false;
      configure(true, true);
    }
  };
  dump("read_crossing_reset");
  hook = {};
  hook = [&](const std::string& point)
  {
    if (point == "loss_reset")
      std::cout << "during_reset\n" << lossDiagnosticsJSON(session(), c.lost.load()) << '\n';
  };
  configure(true, true);
  hook = {};
  dump("after_reset");

  auto old = create(1, 2, 3, 10, 1, 1, 10, 0);
  std::promise<void> held, release;
  auto released = release.get_future();
  std::thread owner;
  once = true;
  hook = [&](const std::string& point)
  {
    if (once && point == "store_before_lock")
    {
      once = false;
      configure(true, true);
      owner = std::thread([&]
      {
        std::lock_guard<std::mutex> lock(c.mutex);
        held.set_value();
        released.wait();
      });
      held.get_future().wait();
    }
  };
  mark(old, Producer);
  hook = {};
  release.set_value(); owner.join();
  noteLoss(LossReason::Explicit, old.session);
  dump("foreign_epoch");

  dump("before_concurrent");
  std::vector<std::thread> writers;
  for (int i = 0; i < 4; ++i) writers.emplace_back([] { for (int j = 0; j < 100; ++j) loss(); });
  for (auto& writer : writers) writer.join();
  dump("after_concurrent");
  std::cout << "size\n{\"lossDiagnosticsBytes\":" << sizeof(LossDiagnostics) << "}\n";
}
