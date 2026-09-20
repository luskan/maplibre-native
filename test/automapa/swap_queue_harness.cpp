#include <functional>
#include <future>
#include <iostream>
#include <thread>
#include <vector>

void swapHook(const char* point);
#define TILE_TRACE_TEST_HOOK(point) swapHook(point)
#include "../../src/mln/util/tile_trace.cpp"

using namespace mln::tiletrace;
std::function<void(const std::string&)> hook;
void swapHook(const char* point) { if (hook) hook(point); }
void dump(const char* name) { std::cout << name << '\n' << snapshotJSON() << '\n'; }
void locked(std::mutex& mutex, const std::function<void()>& action)
{
  std::promise<void> held, release;
  auto released = release.get_future();
  std::thread owner([&]
  {
    std::lock_guard<std::mutex> lock(mutex);
    held.set_value(); released.wait();
  });
  held.get_future().wait(); action(); release.set_value(); owner.join();
}
struct Scene
{
  Context demand, layout;
  BatchInfo info;
  Scene()
  {
    configure(true, true); setTestTime(1000); surfaceCreated(99);
    const ViewTile tile{4, 5, 0, 10, 10};
    const auto view = updateView(1, 2, 3, &tile, 1);
    demand = create(1, 2, 3, 10, 4, 5, 10, 0, 2, view);
    layout = demand; layout.publication = nextID();
    info.id = nextID(); info.session = session(); info.total = info.extracted = 1;
    info.startedUs = 1000; info.endedUs = 2000;
    trackBatchMember(info, layout); batchCacheAdmission(layout, true, 2000);
    layout.id = layout.generation = nextID(); layout.kind = Kind::Layout;
    setTestTime(2000); mark(layout, Layout); bindDemand(layout);
  }
  void batch(const char* label)
  {
    auto value = batchSnapshotJSON(info, true);
    std::cout << label << '\n' << (value.empty() ? "{\"available\":false}" : value) << '\n';
  }
};
void prepare(const Context& trace)
{
  setTestTime(3000); draw(trace, false);
  setTestTime(4000); swapBegin(99); setTestTime(5000);
}
int main(int argc, char** argv)
{
  if (argc != 2) return 2;
  const std::string mode = argv[1];
  Scene scene;
  auto& c = collector();
  if (mode == "batch-lock" || mode == "record-lock" || mode == "multi-use")
  {
    FrameScope scope(2); prepare(scene.layout);
    if (mode == "multi-use") draw(scene.layout, true);
    locked(mode == "record-lock" ? c.mutex : c.batchMutex, [] { swapEnd(true); });
    dump("after"); scene.batch("batch");
  }
  else if (mode == "failed-then-success")
  {
    {
      FrameScope scope(2); prepare(scene.layout);
      locked(c.batchMutex, [] { swapEnd(false, 123); });
    }
    dump("failed");
    {
      FrameScope scope(2); setTestTime(6000); draw(scene.layout, false);
      setTestTime(7000); swapBegin(99); setTestTime(8000); swapEnd(true);
    }
    dump("after"); scene.batch("batch");
  }
  else if (mode == "late-demand" || mode == "late-binding" || mode == "late-member")
  {
    Context later;
    if (mode == "late-binding") later = create(1, 2, 3, 10, 4, 5, 10, 0, 2, scene.layout.view);
    {
      FrameScope scope(2); prepare(scene.layout);
      locked(c.batchMutex, [] { swapEnd(true); });
    }
    if (mode != "late-binding") later = create(1, 2, 3, 10, 4, 5, 10, 0, 2, scene.layout.view);
    auto binding = scene.layout; binding.demand = later.id; binding.origin = Origin::Renderer;
    bindDemand(binding);
    if (mode == "late-member")
    {
      ++scene.info.id; scene.info.total = 1;
      trackBatchMember(scene.info, binding);
    }
    std::cout << "later\n{\"id\":\"" << later.id << "\"}\n";
    dump("after"); scene.batch("batch");
  }
  else if (mode == "pause-before-swap" || mode == "reset-before-swap")
  {
    FrameScope scope(2); prepare(scene.layout);
    if (mode == "pause-before-swap") { configure(false); configure(true); }
    else configure(true, true);
    swapEnd(true); dump("after");
  }
  else if (mode == "reset-unpublished" || mode == "unpublished")
  {
    std::promise<void> reserved, publish;
    auto published = publish.get_future();
    std::atomic<bool> once{true};
    hook = [&](const std::string& point)
    {
      if (point == "swap_queue_reserved" && once.exchange(false)) { reserved.set_value(); published.wait(); }
    };
    std::thread writer([&] { FrameScope scope(2); prepare(scene.layout); swapEnd(true); });
    reserved.get_future().wait();
    if (mode == "reset-unpublished") configure(true, true);
    dump("pending"); scene.batch("pending_batch");
    publish.set_value(); writer.join(); hook = {};
    dump("after"); scene.batch("batch");
  }
  else if (mode == "later-no-draw" || mode == "later-error" || mode == "later-teardown" || mode == "invalidation")
  {
    {
      FrameScope scope(2); prepare(scene.layout);
      locked(c.batchMutex, [] { swapEnd(true); });
    }
    setTestTime(6000);
    if (mode == "later-no-draw")
    {
      auto empty = scene.layout; empty.time[Layout] = 6000;
      batchLayoutAccepted(empty, true);
    }
    else if (mode == "later-error") finish(scene.layout, Outcome::Error);
    else if (mode == "later-teardown") retireSource(2);
    else invalidateView(1, 2);
    dump("after"); scene.batch("batch");
  }
  else if (mode == "reordered" || mode == "equal-time-order")
  {
    FrameScope scope(2); prepare(scene.layout);
    Frame earlier = frame;
    const EventBoundary old{5000, nextID()};
    auto newer = old; newer.order = nextID();
    if (mode == "reordered") newer.time = 6000;
    enqueueSwap(c, frame, true, newer);
    tryDrainSwaps(c, 16);
    enqueueSwap(c, earlier, true, old);
    dump("after"); scene.batch("batch");
  }
  else if (mode == "incremental")
  {
    FrameScope scope(2); prepare(scene.layout);
    while (frame.count < 80) { frame.draws[frame.count] = frame.draws[0]; ++frame.count; }
    enqueueSwap(c, frame, true, eventBoundary());
    tryDrainSwaps(c, 16);
    dump("pending"); scene.batch("pending_batch"); dump("after");
  }
  else if (mode == "sustained-frames")
  {
    for (size_t i = 0; i < 40; ++i)
    {
      FrameScope scope(2);
      setTestTime(3000 + i * 1000); draw(scene.layout, false);
      while (frame.count < 80) { frame.draws[frame.count] = frame.draws[0]; ++frame.count; }
      swapBegin(99); swapEnd(true);
    }
    dump("after"); scene.batch("batch");
  }
  else if (mode == "overflow")
  {
    FrameScope scope(2); prepare(scene.layout);
    locked(c.batchMutex, [&]
    {
      for (size_t i = 0; i < SwapQueueCapacity + 1; ++i) swapEnd(true);
    });
    dump("after"); scene.batch("batch");
  }
  else if (mode == "direct-rebind" || mode == "direct-rebind-evicted")
  {
    auto binding = scene.layout; binding.origin = Origin::Renderer;
    bindDemand(binding);
    auto retained = scene.layout; --retained.session;
    {
      FrameScope scope(2); prepare(retained);
      locked(c.batchMutex, [] { swapEnd(true); });
    }
    binding.publication = nextID(); bindDemand(binding);
    if (mode == "direct-rebind-evicted")
      for (size_t i = 0; i < 3 * Capacity; ++i) create(8, 9, nextID(), 10, 1, 1, 10, 0);
    std::cout << "rebound\n{\"id\":\"" << scene.demand.id
              << "\",\"publication\":\"" << binding.publication << "\"}\n";
    dump("after"); scene.batch("batch");
  }
  else if (mode == "archived-uncertainty")
  {
    FrameScope scope(2); prepare(scene.layout);
    Frame earlier = frame;
    const auto oldOrder = nextID();
    setTestTime(6000); swapEnd(true);
    for (size_t i = 0; i < 3 * Capacity; ++i)
    {
      create(8, 9, nextID(), 10, 1, 1, 10, 0);
      nextID();
    }
    enqueueSwap(c, earlier, true, {5000, oldOrder});
    dump("after");
  }
  else if (mode == "publication-race")
  {
    std::promise<void> claimed, publish;
    auto published = publish.get_future();
    std::atomic<bool> once{true};
    hook = [&](const std::string& point)
    {
      if (point == "publication_claimed" && once.exchange(false)) { claimed.set_value(); published.wait(); }
    };
    std::thread writer([&] { FrameScope scope(2); prepare(scene.layout); swapEnd(true); });
    claimed.get_future().wait();
    scene.batch("pending_batch");
    publish.set_value(); writer.join(); hook = {};
    dump("after"); scene.batch("batch");
  }
  else if (mode == "retry-exhaustion")
  {
    FrameScope scope(2); prepare(scene.layout);
    bool nested = false;
    size_t conflicts = 0;
    hook = [&](const std::string& point)
    {
      if (point != "swap_queue_reservation_attempt" || nested) return;
      nested = true; ++conflicts;
      Frame empty; empty.id = nextID(); empty.session = session(); empty.captureGeneration = captureGeneration();
      enqueueSwap(c, empty, true, eventBoundary()); nested = false;
    };
    swapEnd(true); hook = {};
    std::cout << "attempts\n{\"conflicts\":" << conflicts << "}\n";
    dump("after");
  }
  else if (mode == "rejected-old")
  {
    FrameScope scope(2); prepare(scene.layout);
    Frame earlier = frame;
    const auto oldOrder = nextID();
    setTestTime(6000); swapEnd(true);
    locked(c.batchMutex, [&]
    {
      for (size_t i = 0; i < SwapQueueCapacity; ++i)
        enqueueSwap(c, frame, true, {7000 + i, nextID()});
      enqueueSwap(c, earlier, true, {5000, oldOrder});
    });
    dump("after");
    configure(true, true); dump("reset");
  }
  else if (mode == "surface-isolation")
  {
    FrameScope scope(2); prepare(scene.layout);
    locked(c.mutex, [&]
    {
      surfaceCreated(100);
      frame.surface = 0;
      swapBegin(100);
      std::cout << "surface\n{\"id\":\"" << frame.surface << "\"}\n";
      surfaceDestroyed(100);
    });
    dump("after");
    c.lossDiagnostics.snapshotCopyActive=true;
    locked(c.surfaceMutex, [] { surfaceCreated(101); swapBegin(101); surfaceDestroyed(101); });
    c.lossDiagnostics.snapshotCopyActive=false;
    dump("contended");
  }
  else if (mode == "short-batch-lock")
  {
    {
      FrameScope scope(2); prepare(scene.layout);
      locked(c.batchMutex, [] { swapEnd(true); });
    }
    std::promise<void> raw, resume;
    auto resumed=resume.get_future();
    std::atomic<bool> once{true};
    hook=[&](const std::string& point)
    {
      if (point=="swap_records_apply" && once.exchange(false)) { raw.set_value(); resumed.wait(); }
    };
    std::thread consumer([&] { tryDrainSwaps(c, FrameCapacity); });
    raw.get_future().wait();
    setTestTime(6000);
    auto next=create(1,2,4,10,4,5,10,0,2,scene.layout.view);
    next.publication=nextID(); next.id=next.generation=nextID(); next.kind=Kind::Layout;
    mark(next,Layout);
    auto info=scene.info; info.id=nextID(); trackBatchMember(info,next);
    auto unrelated=create(8,9,10,10,4,5,10,0);
    unrelated.kind=Kind::Publication; unrelated.publication=nextID(); finish(unrelated,Outcome::Error);
    Frame completed; completed.id=nextID(); completed.session=session(); completed.captureGeneration=captureGeneration();
    completed.count=1; completed.ended=completed.swapStarted=6500;
    completed.draws[0].context=next; completed.draws[0].context.time[Draw]=6000;
    completed.draws[0].drawOrder=nextID();
    enqueueSwap(c,completed,true,{7000,nextID()});
    scene.batch("pending_batch");
    auto snapshot=std::async(std::launch::async,[] { return snapshotJSON(); });
    std::cout << "during\n{\"rawBlocked\":"
              << (snapshot.wait_for(std::chrono::milliseconds(10))==std::future_status::timeout ? "true" : "false")
              << ",\"nextId\":\"" << next.id << "\"}\n";
    resume.set_value();consumer.join();
    const auto result=snapshot.get();hook={};
    std::cout << "after\n" << result << '\n';
  }
  else if (mode == "snapshot-race")
  {
    bool once = true;
    hook = [&](const std::string& point)
    {
      if (point != "record_snapshot_timestamp" || !once) return;
      once = false;
      Frame completed; completed.id = nextID(); completed.session = session();
      completed.captureGeneration = captureGeneration();
      std::thread writer([&] { enqueueSwap(c, completed, true, {6000, nextID()}); });
      writer.join();
    };
    dump("pending"); hook = {}; dump("after");
  }
  else return 2;
}
