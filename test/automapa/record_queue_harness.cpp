#include <functional>
#include <future>
#include <iostream>
#include <thread>
#include <vector>

void queueHook(const char* point);
#define TILE_TRACE_TEST_HOOK(point) queueHook(point)
#include "../../src/mln/util/tile_trace.cpp"

using namespace mln::tiletrace;
std::function<void(const std::string&)> hook;
void queueHook(const char* point) { if (hook) hook(point); }
void dump(const char* name) { std::cout << name << '\n' << snapshotJSON() << '\n'; }
void locked(const std::function<void()>& action)
{
  std::promise<void> held, release;
  auto released = release.get_future();
  std::thread owner([&]
  {
    std::lock_guard<std::mutex> lock(collector().mutex);
    held.set_value(); released.wait();
  });
  held.get_future().wait();
  action();
  release.set_value(); owner.join();
}
Context demand() { return create(1, 2, 3, 10, 1, 1, 10, 0, 2); }
int main()
{
  auto& c = collector();
  configure(true, true); setTestTime(1000);
  Context ctx;
  locked([&] { ctx = demand(); });
  dump("retained_request");
  ctx.publication = 123;
  bindDemand(ctx);
  dump("bound_request");

  configure(true, true);
  Context layout;
  locked([&]
  {
    ctx = demand();
    layout = ctx; layout.id = layout.generation = nextID(); layout.publication = 234;
    layout.kind = Kind::Layout; setTestTime(2000); mark(layout, Layout);
  });
  bindDemand(layout);
  surfaceCreated(99);
  {
    FrameScope scope(2); setTestTime(3000); draw(layout, false);
    setTestTime(4000); swapBegin(99); setTestTime(5000); swapEnd(true);
  }
  dump("submitted_after_deferral");

  configure(true, true);
  locked([&] { ctx = demand(); loss(); });
  dump("baseline_loss_preserved");
  configure(true, true);
  locked([&] { ctx = demand(); });
  configure(false, false);
  dump("paused_with_pending");
  configure(true, false);
  dump("resumed");

  configure(true, true);
  dump("before_full");
  locked([&] { for (size_t i = 0; i <= RecordQueueCapacity; ++i) demand(); });
  dump("full_queue");

  configure(true, true);
  std::promise<void> reserved, publish;
  auto published = publish.get_future();
  std::atomic<bool> once{true};
  hook = [&](const std::string& point)
  {
    if (point == "record_queue_reserved" && once.exchange(false))
    {
      reserved.set_value(); published.wait();
    }
  };
  std::thread producer([] { demand(); });
  reserved.get_future().wait();
  dump("unpublished_head");
  auto later = demand();
  bindDemand(later);
  dump("dependent_refused");
  publish.set_value(); producer.join(); hook = {};
  dump("head_recovered");

  configure(true, true);
  std::promise<void> oldReserved, oldPublish;
  auto oldPublished = oldPublish.get_future(); once = true;
  hook = [&](const std::string& point)
  {
    if (point == "record_queue_reserved" && once.exchange(false))
    {
      oldReserved.set_value(); oldPublished.wait();
    }
  };
  std::thread oldProducer([] { demand(); });
  oldReserved.get_future().wait();
  configure(true, true);
  dump("reset_with_old_reservation");
  oldPublish.set_value(); oldProducer.join(); hook = {};
  dump("old_epoch_discarded");

  for (const auto* point : {"snapshot_copy", "record_snapshot_timestamp"})
  {
    configure(true, true); once = true;
    hook = [&](const std::string& current)
    {
      if (current == point && once.exchange(false))
      {
        std::thread writer([] { demand(); }); writer.join();
      }
    };
    dump(point);
    hook = {};
    dump(point == std::string("snapshot_copy") ? "copy_recovered" : "timestamp_recovered");
  }

  configure(true, true);
  int conflicts = 0;
  bool competing = false;
  hook = [&](const std::string& point)
  {
    if (point == "record_queue_reservation_attempt" && !competing)
    {
      competing = true;
      demand();
      competing = false;
      ++conflicts;
    }
  };
  demand(); hook = {};
  dump("reservation_contention");
  std::cout << "attempt_bound\n{\"conflicts\":" << conflicts << "}\n";

  configure(true, true); dump("before_concurrent");
  std::vector<std::thread> producers;
  for (size_t i = 0; i < 4; ++i)
    producers.emplace_back([] { for (size_t j = 0; j < 200; ++j) demand(); });
  for (auto& writer : producers) writer.join();
  dump("after_concurrent");
  std::cout << "storage\n{\"queueBytes\":" << sizeof(RecordQueue) << "}\n";
}
