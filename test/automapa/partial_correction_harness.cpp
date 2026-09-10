#include "../../src/mln/util/tile_trace.cpp"
#include <iostream>
#include <thread>

using namespace mln::tiletrace;
const ViewTile A{4, 5, 0, 10, 10}, B{5, 5, 0, 10, 10}, C{6, 5, 0, 10, 10}, P{2, 2, 0, 9, 9};
BatchInfo info;
ID view;
void begin()
{
  setTestTime(1000); configure(true, true);
  info = {}; info.session = session(); info.id = nextID(); info.startedUs = 1000; info.endedUs = 201000;
  info.batchMs = 200;
  const ViewTile keys[]{A, B, C};
  view = updateView(1, 2, 1, keys, 3);
}
Context member(ViewTile key = A)
{
  auto trace = create(1, 2, nextID(), key.z, key.x, key.y, key.overscaledZ, key.wrap, 0, view);
  trace.id = trace.publication = nextID(); trace.kind = Kind::Publication; mark(trace, Producer);
  ++info.total; ++info.extracted; trackBatchMember(info, trace); return trace;
}
void cache(Context trace, uint64_t time = 301000)
{
  setTestTime(time); batchCacheAdmission(trace, true, time);
}
Context layout(Context trace)
{
  trace.id = trace.generation = nextID(); trace.kind = Kind::Layout; mark(trace, Layout); bindDemand(trace); return trace;
}
void submit(Context trace, uint64_t time = 701000)
{
  setTestTime(time); FrameScope frame(2); draw(trace, false); swapBegin(99); swapEnd(true);
}
void retire(Context trace, Retirement reason = Retirement::NoDemand)
{
  retireBatchTile(trace.map, trace.source, trace.z, trace.x, trace.y, reason);
}
void dump(const char* label)
{
  std::cout << label << '\n' << batchSnapshotJSON(info, true) << '\n';
}
template<class Callback> void locked(std::mutex& mutex, Callback callback)
{
  std::lock_guard<std::mutex> lock(mutex);
  std::thread event(callback); event.join();
}
int main()
{
  for (bool cancelFirst : {false, true})
  {
    begin(); auto a = member(), b = member(B), c = member(C), p = member(P); info.extracted = 3;
    cache(a); cache(b); cache(p);
    setTestTime(401000);
    if (cancelFirst) finish(c, Outcome::Cancelled);
    const ViewTile keys[]{A, B}; updateView(1, 2, 1, keys, 2);
    if (!cancelFirst) finish(c, Outcome::Cancelled);
    submit(layout(a)); submit(layout(b)); dump(cancelFirst ? "cancel_before_left" : "cancel_after_left");
  }
  begin(); auto a = member(); finish(a, Outcome::Cancelled); dump("all_cancelled");
  begin(); a = member(); cache(a); setTestTime(401000); retire(a); dump("cancel_after_cache");
  begin(); a = member(); cache(a); submit(layout(a)); setTestTime(801000); retire(a); dump("cancel_after_swap");
  begin(); a = member(); setTestTime(401000); retire(a); finish(a, Outcome::NoReceiver); dump("late_no_receiver");
  begin(); a = member(); finish(a, Outcome::NoReceiver); dump("unknown_no_receiver");
  begin(); a = member(); finish(a, Outcome::Error); setTestTime(401000); retire(a); dump("error_before_cancel");
  begin(); a = member(); a.outcome = Outcome::Error; mark(a, Features); setTestTime(401000); retire(a); dump("marked_error_before_cancel");
  begin(); a = member(); cache(a); auto accepted = layout(a); finish(accepted, Outcome::Error);
  setTestTime(401000); retire(a); dump("layout_error_before_cancel");
  begin(); a = member(); accepted = layout(a); retire(a); finish(accepted, Outcome::Error); dump("layout_error_after_cancel_same_clock");
  begin(); a = member(); setTestTime(401000); retire(a); batchCacheAdmission(a, true, 301000); dump("earlier_cache_late_callback");
  begin(); a = member(); accepted = layout(a);
  {
    setTestTime(301000); FrameScope frame(2); draw(accepted, false);
    setTestTime(401000); retire(a); setTestTime(701000); swapBegin(99); swapEnd(true);
  }
  dump("draw_before_cancel_swap_after");
  begin(); a = member(); auto demand = a; demand.id = demand.demand; demand.publication = 0; demand.kind = Kind::Demand;
  locked(collector().batchMutex, [&] { finish(demand, Outcome::Cancelled); finish(a, Outcome::Empty); });
  cache(a); submit(layout(a)); dump("harmless_finish_contention");
  begin(); a = member();
  locked(collector().mutex, [&] { pump(999, true); cache(a); submit(layout(a)); });
  dump("optional_collector_contention");
  begin(); a = member();
  locked(collector().batchMutex, [&] { cache(a); });
  setTestTime(401000); retire(a); dump("lost_cache_not_excluded");
  begin(); a = member();
  locked(collector().batchMutex, [&] { retireSource(a.source); }); dump("lost_source_teardown");
  begin(); a = member(); auto old = a;
  info.id = nextID(); info.total = info.extracted = 0; a = member(B);
  setTestTime(201000); batchLoss(old.publication, old.source, LostEvent::Cache);
  cache(a); submit(layout(a)); dump("another_publication_loss");
  begin(); a = member(); setTestTime(201000);
  std::thread cacheLoss([&] { batchLoss(a.publication, a.source, LostEvent::Cache); });
  std::thread screenLoss([&] { batchLoss(a.publication, a.source, LostEvent::Screen); });
  cacheLoss.join(); screenLoss.join(); cache(a); submit(layout(a)); dump("simultaneous_loss_categories");
  begin(); a = member(); cache(a); submit(layout(a)); setTestTime(801000);
  batchLoss(a.publication, a.source, LostEvent::Cache); batchLoss(a.publication, a.source, LostEvent::Screen);
  dump("loss_after_endpoints");
  begin(); a = member();
  {
    LossLease oldBank(session());
    begin(); a = member();
    const auto index = oldBank.bank->count.fetch_add(1);
    auto& event = oldBank.bank->events[index];
    event.publication = a.publication; event.source = a.source; event.timestamp = 201000;
    event.kind = LostEvent::Outcome; event.boundary.eligibilityUs = event.timestamp; event.ready = true;
    cache(a); submit(layout(a)); dump("old_writer_after_reset");
  }
  begin(); a = member();
  {
    LossLease lease(session()); const auto index = lease.bank->count.fetch_add(1);
    std::cout << "unfinished_loss\n{\"deferred\":" << (batchSnapshotJSON(info).empty() ? "true" : "false") << "}\n";
    auto& event = lease.bank->events[index]; event.publication = a.publication; event.source = a.source;
    event.timestamp = now(); event.kind = LostEvent::Cache; event.boundary.eligibilityUs = event.timestamp; event.ready = true;
  }
  cache(a); submit(layout(a)); dump("published_loss");
  begin();
  demand = create(1, 2, nextID(), A.z, A.x, A.y, A.overscaledZ, A.wrap, 0, view);
  locked(collector().batchMutex, [&] { finish(demand, Outcome::Error); });
  setTestTime(201000); a = demand; a.id = a.publication = nextID(); a.kind = Kind::Publication; a.outcome = Outcome::Pending;
  ++info.total; trackBatchMember(info, a); cache(a); submit(layout(a)); dump("lost_demand_before_admission");
  begin(); a = member(); setTestTime(201000);
  {
    LossLease lease(session()); auto& event = lease.bank->events[lease.bank->count.fetch_add(1)];
    event.publication = 0; event.source = a.source; event.boundary.viewOrder = nextID();
    event.timestamp = now(); event.kind = LostEvent::View; event.ready = true;
  }
  cache(a); submit(layout(a)); dump("view_loss_before_stamp");
  begin();
  a = create(1, 2, nextID(), A.z, A.x, A.y, A.overscaledZ, A.wrap, 0, view);
  a.id = a.publication = nextID(); a.kind = Kind::Publication;
  {
    LossLease lease(session()); auto& event = lease.bank->events[lease.bank->count.fetch_add(1)];
    event.publication = 0; event.source = a.source; event.boundary.viewOrder = nextID();
    event.timestamp = 101000; event.kind = LostEvent::View; event.ready = true;
  }
  setTestTime(201000); ++info.total; trackBatchMember(info, a); cache(a); submit(layout(a));
  dump("view_loss_before_admission_and_stamp");
  begin(); a = member(); accepted = layout(a);
  {
    setTestTime(301000); FrameScope frame(2); draw(accepted, false);
    setTestTime(401000); batchLoss(0, a.source, LostEvent::View);
    setTestTime(701000); swapBegin(99); swapEnd(true);
  }
  dump("view_loss_after_draw");
  begin(); a = member(); finish(a, Outcome::NoReceiver); retire(a); dump("error_before_cancel_same_clock");
  begin(); a = member();
  for (int i = 0; i < 8193; ++i) batchLoss(a.publication, a.source, LostEvent::Cache);
  cache(a); submit(layout(a)); dump("loss_capacity");
  begin(); a = member(); cache(a); submit(layout(a)); dump("reset_after_capacity");
  begin(); a = member(); accepted = layout(a);
  std::atomic<bool> stop{false}; std::atomic<int> reads{0};
  std::thread reader([&] { while (!stop) { batchSnapshotJSON(info); ++reads; } });
  std::thread details([&] { while (!stop) snapshotJSON(); });
  cache(a);
  for (int i = 0; i < 1000; ++i) submit(accepted, 701000 + i);
  stop = true; reader.join(); details.join(); dump("polling_with_real_endpoints");
  std::cout << "polling_counts\n{\"reads\":" << reads.load() << "}\n";
  const auto before = collector().publishedIndex[0].load(); submit(accepted, 901000);
  std::cout << "no_recopy_completed\n{\"same\":" << (before == collector().publishedIndex[0].load() ? "true" : "false") << "}\n";
}
