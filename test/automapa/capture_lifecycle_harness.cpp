#ifdef NDEBUG
#error "Capture lifecycle checks require assertions"
#endif
void traceTestHook(const char*);
#define TILE_TRACE_TEST_HOOK(point) traceTestHook(point)
#include "../../src/mln/util/tile_trace.cpp"
#include <cassert>
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
  bool entered = false, released = false;
  void pause()
  {
    std::unique_lock<std::mutex> lock(mutex);
    entered = true; cv.notify_all(); cv.wait(lock, [&] { return released; });
  }
  void wait()
  {
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [&] { return entered; });
  }
  void release() { std::lock_guard<std::mutex> lock(mutex); released = true; cv.notify_all(); }
};
Gate* activeGate = nullptr;
const char* gatePoint = "";
thread_local bool bypassGate = false;
void traceTestHook(const char* point)
{
  if (!bypassGate && activeGate && std::strcmp(point, gatePoint) == 0) activeGate->pause();
}
void at(uint64_t value) { setTestTime(value); }
void newBatch()
{
  info = {}; info.session = session(); info.id = nextID(); info.startedUs = now();
}
void begin()
{
  at(1000); configure(true, true); surfaceCreated(99);
  const ViewTile keys[]{A, B}; view = updateView(1, 2, 1, keys, 2); newBatch();
}
Context publication(ViewTile key = A, bool admit = true)
{
  auto trace = create(1, 2, nextID(), key.z, key.x, key.y, key.overscaledZ, key.wrap, 0, view);
  trace.id = trace.publication = nextID(); trace.kind = Kind::Publication; mark(trace, Producer);
  if (admit) { ++info.total; trackBatchMember(info, trace); }
  return trace;
}
Context layout(Context trace)
{
  trace.id = trace.generation = nextID(); trace.kind = Kind::Layout;
  mark(trace, Layout); bindDemand(trace); return trace;
}
void cache(const Context& trace) { batchCacheAdmission(trace, true, now()); }
void submit(const Context& trace, bool success = true)
{
  FrameScope scope(2); draw(trace, false); swapBegin(99); swapEnd(success);
}
void dump(const char* label, BatchInfo batch)
{
  batch.endedUs = now();
  std::cout << label << '\n' << batchSnapshotJSON(batch, true) << '\n';
}
void dump(const char* label) { dump(label, info); }
void resume()
{
  configure(true);
  const ViewTile keys[]{A, B}; view = updateView(1, 2, 1, keys, 2);
}
void checkHeader(bool active)
{
  assert((frame.id != 0) == active);
  assert(frame.session == (active ? session() : 0));
  assert(!frame.surface && !frame.ended && !frame.swapStarted && !frame.swapped && !frame.count);
  assert(frame.mode == (active ? 2 : 0));
}
void frameCases()
{
  begin(); auto a = publication(), b = publication(B); cache(a); cache(b);
  auto la = layout(a), lb = layout(b);
  {
    FrameScope scope(2); checkHeader(true);
    for (size_t i = 0; i < FrameCapacity / 2; ++i)
    {
      auto trace = la; trace.id = trace.generation = nextID(); trace.consumer = nextID();
      draw(trace, false); draw(trace, true);
    }
    assert(frame.count == FrameCapacity);
    swapBegin(99); swapEnd(false);
  }
  const auto tail = frame.draws.back().context.id;
  at(2000);
  {
    FrameScope scope(2); checkHeader(true);
    assert(frame.draws.back().context.id == tail);
    swapBegin(123456); assert(!frame.surface); swapEnd(true);
  }
  dump("empty_after_full_failed");
  at(3000); submit(lb); dump("smaller_after_full");
  at(4000);
  { FrameScope scope(2); draw(la, false); }
  at(5000); { FrameScope scope(2); checkHeader(true); swapBegin(99); swapEnd(true); }
  dump("empty_after_abandoned");
  at(6000); submit(la); dump("frame_recovered");
  configure(false);
  { FrameScope scope(2); checkHeader(false); draw(la, false); swapBegin(99); swapEnd(true); assert(!frame.count); }
  assert(frame.draws.back().context.id == tail);
  configure(true);
  { FrameScope scope(2); checkHeader(true); }
  std::cout << "frame_storage\n{\"reused\":true}\n";
}
void pauseCases()
{
  begin(); auto a = publication(); at(2000); cache(a); auto la = layout(a); submit(la);
  dump("completed_before_pause");
  at(3000); configure(false); const auto pausedSerial = viewSerial();
  const auto generation = captureGeneration();
  for (size_t i = 0; i < 10000; ++i)
  {
    assert(!updateView(1, 2, 2, i % 2 ? &A : &B, 1));
    invalidateView(1, 2);
  }
  assert(!updateView(1, 2, 2, nullptr, ViewTileLimit + 1));
  configure(false);
  assert(viewSerial() == pausedSerial && captureGeneration() == generation);
  assert(collector().gaps == 1 && collector().lost == 0);
  at(4000); resume(); dump("completed_after_pause");
  newBatch(); a = publication(); at(5000); cache(a); submit(layout(a)); dump("fresh_after_long_pause");

  begin(); a = publication(); at(2000); cache(a); la = layout(a);
  const auto oldView = view;
  const auto oldRequirement = currentView(collector(), 1, 2, session())->requirements[0];
  at(3000); configure(false); at(4000); resume();
  assert(view != oldView);
  assert(currentView(collector(), 1, 2, session())->requirements[0] != oldRequirement);
  const auto* historical = originalView(collector(), a);
  assert(historical && !historical->leftTimes[0] && !historical->retired);
  at(5000); submit(la); dump("interrupted_same_view");
  const auto interrupted = info;
  newBatch(); auto fresh = publication(); cache(fresh); submit(layout(fresh)); dump("fresh_without_paused_frame");
  dump("interrupted_after_fresh", interrupted);
}
void delayedCases()
{
  for (bool historical : {false, true})
  {
    begin(); auto a = publication(A, false);
    if (historical) view = updateView(1, 2, 2, &B, 1);
    at(2000); configure(false); at(3000); resume();
    ++info.total; trackBatchMember(info, a); cache(a);
    batchLayoutAccepted(layout(a), true, a.view);
    submit(layout(a));
    dump(historical ? "delayed_historical_admission" : "delayed_current_admission");
  }
  begin(); auto a = publication(); cache(a); auto la = layout(a);
  at(2000); configure(false); configure(true);
  batchLayoutAccepted(la, true, view); dump("stale_no_draw_acknowledgement");
}
void intraFrameCase()
{
  begin(); auto a = publication(); cache(a); auto la = layout(a);
  FrameScope scope(2); at(2000); draw(la, false);
  const auto requirement = frame.draws[0].drawRequirement;
  at(3000); configure(false); at(4000); resume();
  at(5000); draw(la, false);
  assert(frame.count == 2 && frame.draws[1].drawRequirement != requirement);
  at(6000); swapBegin(99); swapEnd(true); dump("pause_inside_frame");
}
void preparationRace(bool reset)
{
  begin(); Gate gate; activeGate = &gate; gatePoint = "view_prepared";
  ID stale = 99;
  at(2000);
  std::thread writer([&] { stale = updateView(1, 2, 2, &B, 1); });
  gate.wait();
  if (reset) configure(true, true);
  else { configure(false); configure(true); }
  at(3000); view = updateView(1, 2, 3, &A, 1);
  gate.release(); writer.join(); activeGate = nullptr;
  assert(stale == 0);
  const auto* current = currentView(collector(), 1, 2, session());
  assert(current && current->id == view && current->style == 3 && current->tiles[0] == A);
  newBatch(); auto a = publication(); at(4000); cache(a); submit(layout(a));
  dump(reset ? "reset_during_view_preparation" : "toggle_during_view_preparation");
}
void expectedGenerationCase()
{
  begin(); const auto old = captureGeneration(); configure(false); resume();
  assert(!updateView(1, 2, 2, &B, 1, old));
  const auto* current = currentView(collector(), 1, 2, session());
  assert(current && current->id == view && current->count == 2);
  std::cout << "stale_prepared_keys\n{\"rejected\":true}\n";
}
void drawRace()
{
  begin(); auto a = publication(); cache(a); const auto la = layout(a);
  const auto oldBatch = info;
  Gate gate; activeGate = &gate; gatePoint = "draw_view";
  at(2000);
  std::thread writer([&] {
    FrameScope scope(2); draw(la, false);
    assert(frame.count == 1 && !frame.draws[0].drawRequirement);
    assert(frame.draws[0].context.time[Draw] == 2000);
    draw(la, false);
    assert(frame.count == 2 && frame.draws[1].drawRequirement && frame.draws[1].context.time[Draw] == 4000);
    swapBegin(99); swapEnd(true);
  });
  gate.wait();
  at(3000); configure(false); at(4000); resume();
  newBatch(); auto b = publication(B); cache(b);
  gate.release(); writer.join(); activeGate = nullptr;
  at(5000); submit(layout(b)); dump("fresh_after_interrupted_draw");
  dump("draw_crossed_capture_boundary", oldBatch);
}
void lossRace(bool newerLoss)
{
  begin(); auto interrupted = publication(); cache(interrupted); const auto oldBatch = info;
  Gate gate; activeGate = &gate; gatePoint = "loss_reserved";
  at(2000);
  std::thread writer([&] { updateView(1, 2, 1, nullptr, ViewTileLimit + 1); });
  gate.wait();
  at(3000); configure(false); at(4000); resume();
  newBatch(); auto a = publication(); cache(a); auto la = layout(a);
  if (newerLoss) { at(4500); updateView(1, 2, 1, nullptr, ViewTileLimit + 1); }
  const auto before = viewGap(2);
  gate.release(); writer.join(); activeGate = nullptr;
  assert(viewGap(2) == before);
  at(5000); submit(la);
  dump(newerLoss ? "new_loss_survives_old_writer" : "old_loss_after_resume");
  dump(newerLoss ? "interrupted_with_new_loss" : "interrupted_with_old_loss", oldBatch);
}
int main()
{
  bypassGate = true;
  frameCases(); pauseCases(); delayedCases(); intraFrameCase();
  preparationRace(false); preparationRace(true); expectedGenerationCase(); drawRace();
  lossRace(false); lossRace(true);
}
