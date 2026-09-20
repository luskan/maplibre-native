void traceTestHook(const char*);
#define TILE_TRACE_TEST_HOOK(point) traceTestHook(point)
#include "../../src/mln/util/tile_trace.cpp"
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <thread>

using namespace mln::tiletrace;
const ViewTile A{4, 5, 0, 10, 10}, B{5, 5, 0, 10, 10};
BatchInfo info;
ID view;
struct Gate
{
  std::mutex mutex;
  std::condition_variable cv;
  size_t entered = 0;
  bool released = false;
  void pause()
  {
    std::unique_lock<std::mutex> lock(mutex);
    ++entered; cv.notify_all(); cv.wait(lock, [&] { return released; });
  }
  void wait(size_t count = 1)
  {
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [&] { return entered >= count; });
  }
  void release() { std::lock_guard<std::mutex> lock(mutex); released = true; cv.notify_all(); }
};
Gate* activeGate = nullptr;
const char* gatePoint = "";
thread_local bool bypassGate = false;
void traceTestHook(const char* point)
{
  if (!bypassGate && std::strcmp(point, gatePoint) == 0) activeGate->pause();
}
void at(uint64_t timestamp)
{
#ifdef TILE_TRACE_TESTING
  setTestTime(timestamp);
#else
  const auto before = now();
  while (now() == before) std::this_thread::yield();
#endif
}
void begin()
{
  at(1000); configure(true, true);
  const ViewTile keys[]{A, B};
  view = updateView(1, 2, 1, keys, 2);
  info = {}; info.session = session(); info.id = nextID(); info.startedUs = now();
}
Context publication(ViewTile key, bool admit = true)
{
  auto trace = create(1, 2, nextID(), key.z, key.x, key.y, key.overscaledZ, key.wrap, 0, view);
  trace.id = trace.publication = nextID(); trace.kind = Kind::Publication; mark(trace, Producer);
  if (admit) { ++info.total; ++info.extracted; trackBatchMember(info, trace); }
  return trace;
}
Context unknown()
{
  auto trace = publication(B, false);
  const ViewTile keys[]{A, B};
  for (size_t i = 0; i < 34; ++i) view = updateView(1, 2, nextID(), keys, 2);
  ++info.total; ++info.extracted; trackBatchMember(info, trace);
  return trace;
}
Context layout(Context trace)
{
  trace.id = trace.generation = nextID(); trace.kind = Kind::Layout;
  mark(trace, Layout); bindDemand(trace); return trace;
}
void retire(Context trace)
{
  retireBatchTile(trace.map, trace.source, trace.z, trace.x, trace.y, Retirement::NoDemand);
}
void submitted(Context trace)
{
  FrameScope scope(2); draw(trace, false); swapBegin(99); swapEnd(true);
}
void dump(const char* label)
{
  info.endedUs = now();
  std::cout << label << '\n' << batchSnapshotJSON(info, true) << '\n';
}
template<class Callback> void contended(Callback callback)
{
  std::atomic<bool> held{false}, done{false};
  std::thread holder([&] {
    std::lock_guard<std::mutex> lock(collector().batchMutex);
    held = true;
    while (!done) std::this_thread::yield();
  });
  while (!held) std::this_thread::yield();
  callback(); done = true; holder.join();
}
void fillSwapQueue()
{
  Frame empty;
  empty.id = nextID(); empty.session = session(); empty.captureGeneration = captureGeneration();
  for (size_t i = 0; i < SwapQueueCapacity; ++i)
    enqueueSwap(collector(), empty, true, eventBoundary());
}
void fillLossBank()
{
  auto unrelated = create(3, 4, nextID(), 10, 1, 1, 10, 0);
  unrelated.id = unrelated.publication = nextID(); unrelated.kind = Kind::Publication;
  contended([&] { for (size_t i = 0; i < 8192; ++i) batchCacheAdmission(unrelated, true, now()); });
}
int main()
{
  for (int ordering = 0; ordering < 3; ++ordering)
  {
    begin(); auto b = unknown(), a = publication(A);
    at(2000); batchCacheAdmission(a, true, now()); batchCacheAdmission(b, true, now());
    at(3000); submitted(layout(a)); auto accepted = layout(b);
    if (ordering == 0) { at(3500); retire(b); }
    {
      at(4000); FrameScope scope(2); draw(accepted, false);
      if (ordering == 1) { at(5000); retire(b); dump("unknown_before_swap"); }
      at(6000); swapBegin(99); swapEnd(true);
    }
    if (ordering == 2) { at(7000); retire(b); }
    dump(ordering == 0 ? "unknown_retired_before_draw" : ordering == 1 ? "unknown_retired_before_swap" : "unknown_retired_after_swap");
  }
  begin(); auto b = unknown(); retire(b); dump("all_unknown_retired");
  begin();
  ViewTile wrapped[9];
  for (int i = 0; i < 9; ++i) { wrapped[i] = A; wrapped[i].wrap = i; wrapped[i].overscaledZ = 10 + i % 2; }
  view = updateView(1, 2, 1, wrapped, 9);
  auto a = publication(A); retire(a); dump("overflow_uses_retired");

  for (bool overflow : {false, true})
  {
    for (bool zero : {false, true})
    {
      for (bool blocked : {false, true})
      {
        if (overflow && (zero || !blocked)) continue;
        begin(); a = publication(A); b = publication(B);
        at(2000); batchCacheAdmission(a, true, now()); submitted(layout(a));
        if (overflow) fillLossBank();
        at(3000); const auto cacheTime = zero ? 0 : now();
        at(4000); retire(b);
        at(5000);
        if (blocked) contended([&] { batchCacheAdmission(b, true, cacheTime); });
        else batchCacheAdmission(b, true, cacheTime);
        dump(overflow ? "overflow_cache" : zero ? (blocked ? "zero_cache_lost" : "zero_cache_recorded") :
             blocked ? "delayed_cache_lost" : "delayed_cache_recorded");
      }
    }
    begin(); a = publication(A); b = publication(B);
    at(2000); batchCacheAdmission(a, true, now()); batchCacheAdmission(b, true, now());
    at(3000); submitted(layout(a)); auto accepted = layout(b);
    if (overflow) fillLossBank();
    {
      at(4000); FrameScope scope(2); draw(accepted, false);
      at(5000); retire(b); at(7000);
      contended([&] { fillSwapQueue(); swapBegin(99); swapEnd(true); });
    }
    dump(overflow ? "overflow_swap" : "delayed_swap_lost");
  }

  for (int ordering = 0; ordering < 3; ++ordering)
  {
    for (bool reenter : {false, true})
    {
      begin(); a = publication(A); b = publication(B);
      at(2000); batchCacheAdmission(a, true, now()); batchCacheAdmission(b, true, now());
      at(3000); submitted(layout(a)); auto accepted = layout(b);
      {
        FrameScope scope(2);
        at(4000);
        if (ordering != 2) draw(accepted, false);
        if (ordering != 1) at(5000);
        updateView(1, 2, 1, &A, 1);
        if (ordering == 2) { at(6000); draw(accepted, false); }
        if (reenter) { const ViewTile keys[]{A, B}; updateView(1, 2, 1, keys, 2); }
        at(7000); contended([&] { fillSwapQueue(); swapBegin(99); swapEnd(true); });
      }
      dump(ordering == 0 ? (reenter ? "departure_before_reentry" : "departure_before") :
           ordering == 1 ? (reenter ? "departure_equal_reentry" : "departure_equal") :
           reenter ? "departure_after_reentry" : "departure_after");
    }
  }
  begin(); a = publication(A); b = publication(B);
  at(2000); batchCacheAdmission(a, true, now()); batchCacheAdmission(b, true, now());
  at(3000); submitted(layout(a)); auto accepted = layout(b);
  at(4000); retire(b); at(5000);
  contended([&] { submitted(accepted); }); dump("loss_after_retirement");

  begin(); a = publication(A); at(2000); batchCacheAdmission(a, true, now()); accepted = layout(a);
  at(3000); submitted(accepted); at(4000);
  contended([&] { submitted(accepted); }); dump("loss_after_completion");

  begin(); a = publication(A); at(2000); batchCacheAdmission(a, true, now());
  at(3000); accepted = layout(a); at(4000); retire(a); at(5000);
  contended([&] { batchLayoutAccepted(accepted, true, view); }); dump("delayed_no_draw_lost");

  for (const char* point : {"loss_reserved", "loss_overflow_event"})
  {
    for (size_t writers : {1, 2})
    {
      begin(); a = publication(A); b = publication(B); fillLossBank();
      at(3000); const auto cacheTime = now(); at(4000); retire(b); at(5000);
      Gate gate; activeGate = &gate; gatePoint = point;
      std::unique_lock<std::mutex> lock(collector().batchMutex);
      std::thread first([&] { batchCacheAdmission(b, true, cacheTime); });
      std::thread second;
      if (writers == 2) second = std::thread([&] { batchCacheAdmission(b, true, cacheTime); });
      gate.wait(writers);
      info.endedUs = now();
      std::cout << point << '_' << writers << "\n{\"deferred\":"
                << (batchSnapshotJSON(info).empty() ? "true" : "false") << "}\n";
      gate.release(); first.join(); if (second.joinable()) second.join();
      lock.unlock(); gatePoint = "";
      const auto label = std::string(point) + '_' + std::to_string(writers) + "_published";
      dump(label.c_str());
    }
  }
  begin(); a = publication(A); b = publication(B);
  at(3000); auto error = b;
  {
    Gate gate; activeGate = &gate; gatePoint = "finish_boundary";
    std::thread worker([&] { finish(error, Outcome::Error); });
    gate.wait(); at(4000); retire(b); gate.release(); worker.join(); gatePoint = "";
  }
  dump("delayed_error_success");

  for (int scenario = 0; scenario < 4; ++scenario)
  {
    begin(); a = publication(A); accepted = layout(a);
    at(3000); auto error = scenario == 3 ? accepted : a;
    Gate gate; activeGate = &gate; gatePoint = "finish_boundary";
    std::thread worker([&] { finish(error, scenario == 2 ? Outcome::NoReceiver : Outcome::Error); });
    gate.wait();
    if (scenario == 1)
    {
      at(4000); retire(a); at(5000); auto later = a;
      bypassGate = true; finish(later, Outcome::Error); bypassGate = false;
    }
    else if (scenario == 2) retire(a);
    else { at(4000); batchCacheAdmission(a, true, now()); at(5000); submitted(accepted); }
    gate.release(); worker.join(); gatePoint = "";
    dump(scenario == 0 ? "error_before_recorded_draw" : scenario == 1 ? "earliest_error_wins" :
         scenario == 2 ? "earlier_no_receiver_same_clock" : "layout_error_before_recorded_draw");
  }
  begin(); a = publication(A); accepted = layout(a);
  at(2000); batchCacheAdmission(a, true, now()); at(3000); submitted(accepted);
  at(4000); finish(a, Outcome::Error); dump("error_after_recorded_draw");

  begin(); a = publication(A); accepted = layout(a);
  at(2000); batchCacheAdmission(a, true, now());
  {
    at(3000); FrameScope first(2); draw(accepted, false);
    Gate overlap;
    std::thread second([&] {
      at(4000); FrameScope later(2); draw(accepted, false); overlap.pause();
      contended([&] { fillSwapQueue(); swapBegin(99); swapEnd(true); });
    });
    overlap.wait(); at(5000); swapBegin(99); swapEnd(true);
    at(6000); overlap.release(); second.join();
  }
  dump("overlapping_frame_loss_after_completion");

  begin(); a = publication(A); fillLossBank();
  info.endedUs = now();
  {
    Gate gate; activeGate = &gate; gatePoint = "loss_read";
    bool deferred = false;
    std::thread reader([&] { deferred = batchSnapshotJSON(info).empty(); });
    gate.wait(); contended([&] { batchCacheAdmission(a, true, now()); });
    gate.release(); reader.join(); gatePoint = "";
    std::cout << "writer_between_reads\n{\"deferred\":" << (deferred ? "true" : "false") << "}\n";
  }
  begin(); a = publication(A); fillLossBank();
  {
    Gate gate; activeGate = &gate; gatePoint = "loss_overflow_event";
    std::unique_lock<std::mutex> lock(collector().batchMutex);
    const auto old = a;
    std::thread writer([&] { batchCacheAdmission(old, true, now()); });
    gate.wait(); lock.unlock();
    begin(); a = publication(A);
    gate.release(); writer.join(); gatePoint = "";
    at(2000); batchCacheAdmission(a, true, now()); at(3000); submitted(layout(a));
    dump("reset_with_old_overflow_writer");
  }
}
