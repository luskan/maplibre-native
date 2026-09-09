#include <mln/util/tile_trace.hpp>
#include <iostream>
#include <thread>
#include <vector>

using namespace mln::tiletrace;

void dump(const char* name)
{
  std::cout << name << '\n' << snapshotJSON() << '\n';
}
Context layout(Context demand, ID publication = 100)
{
  auto trace = demand;
  trace.id = nextID(); trace.kind = Kind::Layout;
  trace.publication = publication; trace.generation = trace.id;
  trace.origin = Origin::Fresh;
  const auto start = demand.time[Request];
  setTestTime(start + 1000); mark(trace, Features);
  setTestTime(start + 251000); mark(trace, Enqueued);
  setTestTime(start + 252000); mark(trace, Dispatched);
  setTestTime(start + 259000); mark(trace, Layout);
  bindDemand(trace);
  return trace;
}
void submit(const Context& trace, bool success)
{
  FrameScope scope(5);
  setTestTime(270000); draw(trace, false); draw(trace, false);
  setTestTime(280000); swapBegin(99);
  setTestTime(290000); swapEnd(success, success ? 0 : 0x300D);
}
int main()
{
  configure(true, true); surfaceCreated(99); setTestTime(1000);
  auto demand = create(1, 2, 3, 10, 4, 5, 12, 1);
  auto trace = layout(demand);
  submit(trace, false); dump("failed");
  {
    FrameScope scope(5);
    setTestTime(300000); swapBegin(99);
    setTestTime(310000); swapEnd(true);
  }
  dump("unrelated_swap");
  {
    FrameScope scope(5);
    setTestTime(320000); draw(trace, false);
    setTestTime(330000); swapBegin(99);
    setTestTime(340000); swapEnd(true);
  }
  dump("submitted");
  finish(demand, Outcome::Cancelled); dump("lifecycle_after_submit");
  {
    FrameScope scope(5);
    setTestTime(350000); draw(trace, true);
    setTestTime(360000); swapBegin(99);
    setTestTime(370000); swapEnd(true);
  }
  dump("symbols");
  surfaceDestroyed(99); surfaceCreated(99);
  setTestTime(400000);
  auto reuse = create(1, 2, 3, 10, 4, 5, 12, 1);
  auto retained = trace;
  retained.demand = reuse.id; retained.time = {};
  retained.origin = Origin::Renderer;
  retained.generation = nextID();
  retained.time[Request] = reuse.time[Request]; bindDemand(retained);
  {
    FrameScope scope(6);
    setTestTime(410000); draw(trace, false);
    setTestTime(420000); swapBegin(99);
    setTestTime(430000); swapEnd(true);
  }
  dump("reuse");
  setTestTime(500000);
  auto next = create(1, 2, 3, 10, 4, 5, 12, 1);
  auto expected = trace; expected.demand = next.id; expected.generation = nextID();
  expected.origin = Origin::Fresh; expected.time[Layout] = now(); bindDemand(expected);
  {
    FrameScope scope(5); draw(trace, false); swapBegin(99); swapEnd(true);
  }
  dump("wrong_generation");
  finish(next, Outcome::Cancelled); dump("cancelled");
  for (int i = 0; i < 2200; ++i) create(1, 12, 3000 + i, 10, i, 0, 10, 0);
  {
    FrameScope scope(5);
    setTestTime(530000); draw(trace, false);
    setTestTime(540000); swapBegin(99);
    setTestTime(550000); swapEnd(true);
  }
  dump("redraw_after_eviction");
  configure(true, true); mark(trace, Draw); dump("reset");
  setTestTime(560000);
  auto resetReuse = create(1, 2, 3, 10, 4, 5, 12, 1);
  auto rebound = trace; rebound.session = resetReuse.session; rebound.demand = resetReuse.id;
  rebound.origin = Origin::Renderer; rebound.time = {}; bindDemand(rebound);
  {
    FrameScope scope(5);
    setTestTime(570000); draw(trace, false);
    setTestTime(580000); swapBegin(99);
    setTestTime(590000); swapEnd(true);
  }
  dump("reset_reuse");
  {
    auto symbols = trace;
    symbols.id = symbols.generation = nextID();
    FrameScope scope(5);
    setTestTime(591000); draw(symbols, true);
    setTestTime(592000); swapBegin(99);
    setTestTime(593000); swapEnd(true);
  }
  dump("reset_reuse_symbols");
  configure(true, true);
  setTestTime(600000);
  auto empty = create(4, 5, 6, 10, 1, 1, 10, 0);
  auto emptyLayout = layout(empty, 200); emptyLayout.empty = true;
  bindDemand(emptyLayout); finish(emptyLayout, Outcome::Empty); dump("empty");
  auto dropped = create(4, 5, 7, 10, 2, 1, 10, 0);
  auto publication = dropped; publication.id = publication.publication = nextID();
  publication.kind = Kind::Publication; finish(publication, Outcome::Overflow); dump("drop");
  retireSource(5); dump("teardown");
  setTestTime(1000000);
  auto supersededEmpty = create(7, 8, 9, 10, 1, 1, 10, 0);
  auto delivery = supersededEmpty;
  delivery.id = nextID(); delivery.publication = 300; delivery.empty = true;
  mark(delivery, Delivered); bindDemand(delivery); dump("empty_not_accepted");
  auto replacement = layout(supersededEmpty, 301);
  {
    FrameScope scope(2);
    setTestTime(1300000); draw(replacement, false);
    setTestTime(1310000); swapBegin(99);
    setTestTime(1320000); swapEnd(true);
  }
  dump("empty_superseded");
  auto missing = create(7, 8, 10, 10, 2, 1, 10, 0);
  missing.id = nextID(); missing.kind = Kind::Layout; missing.generation = missing.id;
  missing.publication = 302; missing.origin = Origin::Fresh;
  setTestTime(1330000); mark(missing, Features);
  setTestTime(1340000); mark(missing, Dispatched);
  setTestTime(1350000); mark(missing, Layout); bindDemand(missing);
  {
    FrameScope scope(2);
    setTestTime(1360000); draw(missing, false);
    setTestTime(1370000); swapBegin(99);
    setTestTime(1380000); swapEnd(true);
  }
  dump("missing_stage");
  auto untracked = create(7, 8, 11, 10, 3, 1, 10, 0);
  untracked.origin = Origin::Renderer; bindDemand(untracked); dump("untracked_reuse");
  {
    auto skipped = create(7, 8, 12, 10, 4, 1, 10, 0);
    auto skippedLayout = layout(skipped, 303);
    FrameScope scope(2);
    setTestTime(now() + 10000); draw(skippedLayout, false);
  }
  dump("skipped_swap");
  auto incomplete = create(7, 8, 13, 10, 5, 1, 10, 0);
  auto incompleteLayout = layout(incomplete, 304);
  loss();
  {
    FrameScope scope(2);
    setTestTime(now() + 10000); draw(incompleteLayout, false);
    swapBegin(99); swapEnd(true);
  }
  dump("loss_before_submit");
  auto receiverA = create(7, 8, 14, 10, 6, 1, 10, 0);
  auto receiverB = create(7, 8, 15, 10, 6, 1, 12, 1);
  auto shared = receiverA; shared.id = shared.publication = nextID(); shared.kind = Kind::Publication;
  setTestTime(now() + 1000); mark(shared, Features);
  auto deliveryA = shared; deliveryA.id = nextID();
  auto deliveryB = shared; deliveryB.id = nextID(); deliveryB.consumer = receiverB.consumer;
  deliveryB.demand = receiverB.id; deliveryB.overscaledZ = 12; deliveryB.wrap = 1;
  mark(deliveryA, Delivered); bindDemand(deliveryA);
  mark(deliveryB, Delivered); bindDemand(deliveryB);
  auto layoutB = deliveryB; layoutB.id = layoutB.generation = nextID(); layoutB.kind = Kind::Layout;
  setTestTime(now() + 1000); mark(layoutB, Layout); bindDemand(layoutB);
  {
    FrameScope scope(5); setTestTime(now() + 1000); draw(layoutB, false);
    swapBegin(99); swapEnd(true);
  }
  dump("fanout");
  auto paused = create(7, 8, 16, 10, 7, 1, 10, 0);
  auto pausedLayout = layout(paused, 305);
  configure(false, false);
  {
    FrameScope scope(2); setTestTime(now() + 1000); draw(pausedLayout, false);
    swapBegin(99); swapEnd(true);
  }
  configure(true, false);
  {
    FrameScope scope(2); setTestTime(now() + 1000); draw(pausedLayout, false);
    swapBegin(99); swapEnd(true);
  }
  dump("pause_gap");
  for (int i = 0; i < 2200; ++i) create(4, 8, 8 + i, 10, i, 1, 10, 0);
  dump("bounded");
  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i) threads.emplace_back([i] {
    for (int j = 0; j < 1000; ++j) create(4, 9, 10000 + i * 1000 + j, 10, j, i, 10, 0);
  });
  for (auto& thread : threads) thread.join();
  dump("concurrent");
}
