#include <cassert>
#include <iostream>

size_t probes = 0;
void retentionHook(const char* point);
#define TILE_TRACE_TEST_HOOK(point) retentionHook(point)
#include "../../src/mln/util/tile_trace.cpp"

using namespace mln::tiletrace;

void retentionHook(const char* point)
{
  if (std::string(point) == "record_probe") ++probes;
}

void dump(const char* name)
{
  std::cout << name << '\n' << snapshotJSON() << '\n';
}

Context request(ID id, ID consumer = 11, uint8_t role = 1)
{
  auto& c = collector();
  c.serial = id;
  return create(1, 2, consumer, 10, 571, 337, 10, 0, role);
}

void submit(const Context& layout)
{
  surfaceCreated(99);
  FrameScope frame(2);
  setTestTime(5000);
  draw(layout, false);
  setTestTime(6000);
  swapBegin(99);
  setTestTime(7000);
  swapEnd(true);
}

int main()
{
  auto& c = collector();
  configure(true, true);
  setTestTime(1000);
  auto a = request(55056);
  auto delivery = a;
  delivery.id = 55058;
  delivery.kind = Kind::Publication;
  delivery.publication = 52;
  delivery.origin = Origin::Processed;
  setTestTime(2000);
  mark(delivery, Delivered);
  bindDemand(delivery);
  auto b = request(55060, 12, 2);
  b.publication = 28354;
  bindDemand(b);
  auto layout = b;
  layout.id = layout.generation = 56080;
  layout.kind = Kind::Layout;
  setTestTime(3000);
  mark(layout, Layout);
  bindDemand(layout);
  dump("collision_retained");
  auto late = delivery;
  late.id = late.generation = 58720;
  late.kind = Kind::Layout;
  setTestTime(4000);
  mark(late, Layout);
  bindDemand(late);
  dump("late_bound");
  submit(late);
  dump("late_submitted");

  configure(true, true);
  setTestTime(1000);
  a = request(1023);
  b = request(2047, 12, 2);
  auto third = request(3071, 13, 2);
  b.publication = 12345;
  bindDemand(b);
  mark(a, Delivered);
  mark(third, Delivered);
  dump("wrapped_chain");
  layout = b;
  layout.id = layout.generation = 777;
  layout.kind = Kind::Layout;
  mark(layout, Layout);
  bindDemand(layout);
  submit(layout);
  dump("wrapped_submitted");
  configure(true, true);
  dump("reset_chain");
  b = request(2047);
  b.publication = 12346;
  bindDemand(b);
  dump("reset_rebound");

  configure(true, true);
  setTestTime(1000);
  for (size_t i = 0; i < Capacity; ++i) request(1024 + i);
  dump("full_before");
  auto replacement = request(2048);
  dump("full_replaced");
  a = replacement;
  a.id = a.demand = 1024;
  a.publication = 4567;
  bindDemand(a);
  dump("evicted_bind_rejected");
  mark(a, Delivered);
  dump("archived_history_stays_truncated");

  configure(true, true);
  for (size_t i = 0; i < Capacity; ++i) request(1024 + i);
  request(1024 + Capacity + 100);
  auto old = replacement;
  old.session = session();
  old.id = old.demand = 1124;
  old.time = {};
  mark(old, Delivered);
  dump("unarchived_history_stays_truncated");

  configure(true, true);
  for (size_t i = 0; i < Capacity; ++i) request((i + 1) * Capacity + 1023);
  dump("full_collision_chain");
  auto last = c.records[1022].context;
  probes = 0;
  last.publication = 9999;
  bindDemand(last);
  std::cout << "full_existing_probes\n{\"probes\":" << probes << "}\n";
  dump("full_existing_updated");
  probes = 0;
  auto missing = last;
  missing.id = missing.demand = (Capacity + 2) * Capacity + 1023;
  bindDemand(missing);
  std::cout << "full_missing_probes\n{\"probes\":" << probes << "}\n";
  dump("full_missing_rejected");
  std::cout << "storage\n{\"capacity\":" << Capacity << ",\"collectorBytes\":" << sizeof(Collector) << "}\n";
}
