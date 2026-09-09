#include <mln/util/tile_trace.hpp>
#include <iostream>

using namespace mln::tiletrace;
ID testView = 0;

BatchInfo begin()
{
  setTestTime(1000);
  configure(true, true);
  surfaceCreated(99);
  const ViewTile tile{4, 5, 0, 10, 10};
  testView = updateView(1, 2, 1, &tile, 1);
  BatchInfo batch;
  batch.session = session(); batch.id = nextID(); batch.startedUs = now();
  batch.z = 10; batch.minX = batch.maxX = 4; batch.minY = batch.maxY = 5;
  return batch;
}
Context member(BatchInfo& batch)
{
  auto trace = create(1, 2, nextID(), 10, 4, 5, 10, 0, 0, testView);
  trace.id = trace.publication = nextID(); trace.kind = Kind::Publication;
  mark(trace, Producer);
  ++batch.total; ++batch.extracted;
  trackBatchMember(batch, trace);
  return trace;
}
Context layout(Context trace, bool empty = false)
{
  trace.id = trace.generation = nextID(); trace.kind = Kind::Layout; trace.empty = empty;
  mark(trace, Layout); bindDemand(trace); batchLayoutAccepted(trace, empty);
  return trace;
}
void submit(Context trace, uint64_t time, bool success = true)
{
  setTestTime(time);
  FrameScope frame(2);
  draw(trace, false); swapBegin(99); swapEnd(success);
}
void close(BatchInfo& batch, uint64_t time)
{
  setTestTime(time); batch.endedUs = time; batch.batchMs = (time - batch.startedUs) / 1000;
}
void dump(const char* name, const BatchInfo& batch)
{
  std::cout << name << '\n' << batchSnapshotJSON(batch, true) << '\n';
}
int main()
{
  auto batch = begin(); auto a = member(batch); auto b = member(batch);
  auto la = layout(a); auto lb = layout(b);
  close(batch, 501000); submit(la, 1001000); dump("waiting", batch);
  submit(lb, 1501000, false); dump("failed_swap", batch);
  auto unrelated = la; unrelated.publication = nextID(); submit(unrelated, 2001000);
  dump("unrelated", batch);
  submit(lb, 2601000); dump("complete", batch);
  loss(); setTestTime(9001000); dump("later_loss", batch);

  batch = begin(); a = member(batch); la = layout(a); submit(la, 101000);
  close(batch, 501000); dump("swap_before_close", batch);

  batch = begin(); a = member(batch); b = member(batch);
  layout(a, true); lb = layout(b); close(batch, 501000); submit(lb, 1001000);
  dump("mixed_empty", batch);

  batch = begin(); a = member(batch); layout(a, true); close(batch, 501000);
  dump("empty", batch);

  batch = begin(); a = member(batch); b = member(batch);
  finish(a, Outcome::Overflow); lb = layout(b); close(batch, 501000); submit(lb, 1001000);
  dump("drop", batch);

  batch = begin(); a = member(batch); la = layout(a); finish(la, Outcome::Error);
  close(batch, 501000); dump("layout_error", batch);
  auto other = layout(a); other.consumer = nextID(); submit(other, 1001000);
  dump("shared_recovery", batch);

  batch = begin(); a = member(batch); la = layout(a); ++batch.total;
  close(batch, 501000); submit(la, 1001000); dump("missing_admission", batch);

  batch = begin(); a = member(batch); la = layout(a); close(batch, 501000);
  loss(); submit(la, 1001000); dump("loss", batch);

  batch = begin(); a = member(batch); la = layout(a); close(batch, 501000);
  configure(false); submit(la, 701000); configure(true); submit(la, 1001000);
  dump("pause", batch);

  batch = begin();
  a = create(1, 2, nextID(), 10, 4, 5, 10, 0, 0, testView);
  a.id = a.publication = nextID(); a.kind = Kind::Publication;
  configure(false); ++batch.total; trackBatchMember(batch, a);
  la = layout(a); close(batch, 501000); submit(la, 701000);
  configure(true); submit(la, 1001000); dump("paused_admission", batch);

  batch = begin(); a = member(batch); close(batch, 501000);
  auto demand = a; demand.id = demand.demand; demand.publication = 0; demand.kind = Kind::Demand;
  finish(demand, Outcome::Teardown); dump("teardown_before_binding", batch);

  batch = begin(); a = member(batch); la = layout(a); close(batch, 501000);
  demand = a; demand.id = demand.demand; demand.publication = 0; demand.kind = Kind::Demand;
  finish(demand, Outcome::Teardown); dump("demand_teardown", batch);
  other = layout(a); other.consumer = nextID(); submit(other, 1001000);
  dump("demand_recovery", batch);

  batch = begin(); a = member(batch);
  demand = create(1, 2, nextID(), 10, 4, 5, 10, 0, 0, testView);
  other = a; other.demand = demand.id; layout(other);
  for (int i = 0; i < 1100; ++i) create(1, 2, nextID(), 10, 4, 5, 10, 0, 0, testView);
  close(batch, 501000); finish(demand, Outcome::Cancelled); dump("evicted_receiver", batch);

  batch = begin(); a = member(batch);
  for (int i = 0; i < 8; ++i)
  {
    demand = create(1, 2, nextID(), 10, 4, 5, 10, 0, 0, testView);
    other = a; other.demand = demand.id; other = layout(other);
  }
  close(batch, 501000); submit(other, 1001000); dump("receiver_capacity", batch);

  batch = begin(); a = member(batch); close(batch, 501000); retireSource(a.source);
  dump("teardown", batch);

  batch = begin(); a = member(batch); la = layout(a); close(batch, 501000);
  configure(true, true); dump("session_reset", batch);

  batch = begin(); for (int i = 0; i < 257; ++i) member(batch);
  close(batch, 501000); dump("capacity", batch);

  batch = begin(); a = member(batch); la = layout(a); close(batch, 501000); submit(la, 1001000);
  auto next = batch; next.id = nextID(); next.startedUs = 1101000; next.endedUs = 1201000;
  next.batchMs = 100; next.total = 0; next.extracted = 0; next.inFlight = 3;
  auto c = member(next); dump("old_batch", batch); dump("next_batch", next);
  submit(layout(c), 1401000); dump("next_complete", next);
}
