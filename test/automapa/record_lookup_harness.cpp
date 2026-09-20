#include <cstddef>
void traceHook(const char*);
void traceCount(const char*, size_t);
#define TILE_TRACE_TEST_HOOK(point) traceHook(point)
#define TILE_TRACE_TEST_COUNT(point, value) traceCount(point, value)
#ifndef TILE_TRACE_SOURCE
#define TILE_TRACE_SOURCE "../../src/mln/util/tile_trace.cpp"
#endif
#include TILE_TRACE_SOURCE
#include <cassert>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

using namespace mln::tiletrace;
size_t visits = 0, relinks = 0, refreshes = 0, copies = 0, membersCopied = 0;
bool injectViewLoss = false;
void traceCount(const char*, size_t value) { membersCopied += value; }
void traceHook(const char* point)
{
  if (!std::strcmp(point, "record_candidate")) ++visits;
  if (!std::strcmp(point, "record_relink")) ++relinks;
  if (!std::strcmp(point, "use_refresh")) ++refreshes;
  if (!std::strcmp(point, "batch_published")) ++copies;
  if (injectViewLoss && !std::strcmp(point, "scoped_refresh"))
  {
    injectViewLoss = false;
    std::thread writer([] { invalidateView(1, 2); });
    writer.join();
  }
}
void counts(const char* name)
{
  std::cout << name << "\n{\"visits\":" << visits << ",\"relinks\":" << relinks
            << ",\"refreshes\":" << refreshes << ",\"copies\":" << copies
            << ",\"bytes\":" << membersCopied * sizeof(BatchMember) << "}\n";
}
void clearCounts() { visits = relinks = refreshes = copies = membersCopied = 0; }
void checkIndex()
{
#ifdef TILE_TRACE_INDEX_TESTS
  auto& c = collector();
  std::array<bool, Capacity> seen{};
  for (size_t bucket = 0; bucket < RecordIndex::BucketCount; ++bucket)
  {
    uint16_t previous = 0;
    for (auto link = c.recordIndex.heads[bucket]; link; link = c.recordIndex.next[link - 1])
    {
      assert(link <= Capacity && link > previous && !seen[link - 1]);
      seen[link - 1] = true;
      const auto& ctx = c.records[link - 1].context;
      assert(ctx.id && (ctx.kind == Kind::Demand || ctx.kind == Kind::Publication));
      assert(RecordIndex::bucket(ctx.session, ctx.publication) == bucket);
      assert(c.recordIndex.buckets[link - 1] == bucket + 1);
      if (previous) assert(c.recordIndex.previous[link - 1] == previous);
      previous = link;
    }
    if (c.recordIndex.heads[bucket]) assert(c.recordIndex.previous[c.recordIndex.heads[bucket] - 1] == previous);
  }
  for (size_t i = 0; i < Capacity; ++i)
  {
    const auto& ctx = c.records[i].context;
    assert(seen[i] == bool(ctx.id && (ctx.kind == Kind::Demand || ctx.kind == Kind::Publication)));
  }
#endif
}
std::vector<ID> matches(const Context& draw, bool indexed)
{
  auto& c = collector(); std::vector<ID> result;
  const auto accept = [&](size_t slot) {
    const auto& ctx = c.records[slot].context;
    if (ctx.session != draw.session || (ctx.outcome != Outcome::Pending && ctx.outcome != Outcome::Submitted)) return;
    const bool demand = ctx.kind == Kind::Demand && ctx.consumer == draw.consumer &&
      ctx.publication == draw.publication && (ctx.generation == draw.generation || ctx.origin == Origin::Renderer);
    const bool publication = ctx.kind == Kind::Publication && ctx.publication == draw.publication &&
      (ctx.id == ctx.publication || ctx.consumer == draw.consumer);
    if (demand || publication) result.push_back(ctx.id);
  };
#ifdef TILE_TRACE_INDEX_TESTS
  if (indexed)
  {
    for (auto link = c.recordIndex.heads[RecordIndex::bucket(draw.session, draw.publication)];
         link; link = c.recordIndex.next[link - 1]) accept(link - 1);
    return result;
  }
#endif
  for (size_t i = 0; i < Capacity; ++i) accept(i);
  return result;
}
const ViewTile A{4, 5, 0, 10, 10}, B{5, 5, 0, 10, 10};
BatchInfo info;
ID view;
void begin()
{
  setTestTime(1000); configure(true, true); checkIndex();
  const ViewTile keys[]{A, B}; view = updateView(1, 2, 1, keys, 2);
  info = {}; info.session = session(); info.id = nextID(); info.startedUs = 1000; info.endedUs = 2000;
  clearCounts();
}
Context demand(ID consumer, ID publication = 0)
{
  auto ctx = create(1, 2, consumer, 10, 4, 5, 10, 0, 0, view);
  ctx.publication = publication; store(ctx); checkIndex(); return ctx;
}
Context accepted(Context ctx, ID generation)
{
  ctx.id = ctx.generation = generation; ctx.kind = Kind::Layout;
  mark(ctx, Layout); bindDemand(ctx); checkIndex(); return ctx;
}
void submit(Context ctx, bool symbol = false, bool success = true)
{
  assert(matches(ctx, true) == matches(ctx, false));
  FrameScope scope(2); draw(ctx, symbol); swapBegin(99); swapEnd(success); checkIndex();
}
void snapshot(const char* name) { std::cout << name << '\n' << snapshotJSON() << '\n'; }
Context batchMember(ViewTile key)
{
  auto ctx = create(1, 2, nextID(), key.z, key.x, key.y, key.overscaledZ, key.wrap, 0, view);
  ctx.id = ctx.publication = nextID(); ctx.kind = Kind::Publication; mark(ctx, Producer);
  ++info.total; trackBatchMember(info, ctx); return ctx;
}
void batch(const char* name) { std::cout << name << '\n' << batchSnapshotJSON(info, true) << '\n'; }
void capacityFixture(bool pending)
{
  begin(); std::vector<ViewTile> keys;
  for (size_t i = 0; i < ViewTileLimit; ++i) keys.push_back({uint32_t(i / 8), 0, int16_t(i % 8), 10, 10});
  view = updateView(1, 2, 1, keys.data(), keys.size());
  const auto* current = currentView(collector(), 1, 2, session());
  for (size_t i = 0; i < BatchCapacity; ++i)
  {
    auto& b = collector().batches[i]; b.id = nextID(); b.session = session();
    b.count = BatchMemberCapacity; b.dirty = true;
    for (size_t j = 0; j < b.count; ++j)
    {
      auto& m = b.members[j]; m = {}; m.map = 1; m.source = 2;
      m.view = view; m.viewKnown = true; m.publication = nextID(); m.useCount = 8;
      m.tile = keys[(j % 32) * 8]; m.viewGap = current->gap;
      for (size_t k = 0; k < 8; ++k)
      {
        auto& use = m.uses[k]; const auto key = (j % 32) * 8 + k;
        use.tile = keys[key]; use.requirement = current->requirements[key];
        use.submittedUs = pending && !i && !j && !k ? 0 : 1; use.exact = true;
      }
    }
  }
  publishBatches(collector()); clearCounts();
}
int main()
{
  collector().epoch = 1000;
  begin(); auto first = demand(10, 9000), other = demand(11, 9000);
  auto producer = first; producer.id = producer.publication; producer.kind = Kind::Publication; store(producer);
  auto alias = producer; alias.id = nextID(); alias.consumer = 11; store(alias); checkIndex();
  auto layout = accepted(first, 8000), otherLayout = accepted(other, 8001);
  setTestTime(3000); submit(layout); snapshot("semantic_first_receiver");
  auto late = demand(12, 9000); auto lateLayout = accepted(late, 8002);
  setTestTime(4000); submit(layout, true); submit(otherLayout, false, false);
  setTestTime(5000); submit(otherLayout); submit(lateLayout); snapshot("semantic_late_receivers");
  clearCounts(); setTestTime(6000); submit(layout); counts("stable_redraw");

  begin(); first = demand(20); layout = accepted(first, 7000);
  setTestTime(3000); submit(layout); snapshot("semantic_zero_publication");
  const auto old = layout; configure(true, true); checkIndex();
  first = demand(20); first.origin = Origin::Renderer; first.generation = 7000; bindDemand(first); checkIndex();
  setTestTime(4000); submit(old, true); snapshot("semantic_zero_retained");

  begin(); first = demand(30); layout = accepted(first, 6000);
  auto binding = first; binding.publication = 4000; bindDemand(binding); checkIndex();
  store(first); checkIndex(); snapshot("semantic_preserved_binding");
  bindDemand(first); checkIndex(); snapshot("semantic_rebound_zero");
  first.publication = 4001; bindDemand(first); checkIndex();
  for (size_t i = 0; i < Capacity + 20; ++i)
  {
    auto d = demand(100 + i, 5000 + i); finish(d, Outcome::Cancelled); checkIndex();
  }
  store(first); checkIndex(); snapshot("semantic_startup_restored");

  begin(); first = {};
  first.session = session(); first.map = 1; first.source = 2; first.view = view;
  first.id = first.demand = 100 * Capacity + 100; first.consumer = 40;
  first.publication = 9000; first.generation = 400 * Capacity + 10;
  first.z = first.overscaledZ = 10; first.x = 4; first.y = 5;
  mark(first, Request); checkIndex();
  layout = accepted(first, first.generation);
  for (size_t i = 0; i < Capacity; ++i)
    if (!collector().records[i].context.id)
    {
      auto filler = layout;
      filler.id = filler.generation = 700 * Capacity + i;
      filler.publication = 0;
      mark(filler, Layout);
    }
  checkIndex();
  {
    FrameScope scope(2); draw(layout, false);
    auto collision = first; collision.id = collision.demand = layout.id + Capacity;
    mark(collision, Request); checkIndex();
    assert(collector().records[10].context.id == collision.id);
    setTestTime(3000); swapBegin(99); swapEnd(true); checkIndex();
    assert(collector().records[100].context.outcome == Outcome::Submitted);
  }
  snapshot("semantic_primary_evicts_candidate");
  retireSource(2); checkIndex(); snapshot("semantic_source_retired");

  begin(); auto a = batchMember(A), b = batchMember(B);
  batchCacheAdmission(a, true, 2000); batchCacheAdmission(b, true, 2000);
  auto la = accepted(a, nextID()), lb = accepted(b, nextID());
  setTestTime(3000); submit(la); submit(lb); snapshot("semantic_completed_details");
  clearCounts(); setTestTime(4000); const ViewTileRange narrow{4, 5, 4, 5, 10, 10};
  const auto oldSerial = viewSerial(); invalidateView(1, 2, &narrow); counts("completed_invalidation");
  assert(viewSerial() > oldSerial); batch("semantic_completed_batch");
  setTestTime(5000); retireBatchTile(1, 2, 10, 4, 5, Retirement::Invalidated); batch("semantic_later_retirement");

  begin(); a = batchMember(A); b = batchMember(B);
  clearCounts(); setTestTime(3000); invalidateView(1, 2, &narrow);
  counts("scoped_pending"); batch("semantic_pending_batch");
  clearCounts(); setTestTime(4000); invalidateView(1, 2, &narrow); counts("repeated_pending");
  const ViewTileRange noMatch{100, 100, 100, 100, 10, 10};
  clearCounts(); const auto serial = viewSerial(); invalidateView(1, 2, &noMatch); counts("no_overlap");
  assert(viewSerial() == serial);
  auto delayed = create(1, 2, 100, 10, 4, 5, 10, 0, 0, view);
  delayed.id = delayed.publication = nextID(); delayed.kind = Kind::Publication;
  ++info.total; trackBatchMember(info, delayed); batch("semantic_delayed_admission");

  begin(); a = batchMember(A); b = batchMember(B);
  collector().batches[0].dirty = true; clearCounts(); invalidateView(1, 2, &narrow);
  counts("already_dirty"); batch("semantic_already_dirty");

  begin(); const ViewTile wrapped[]{A, {4, 5, 1, 10, 12}, B};
  view = updateView(1, 2, 1, wrapped, 3); a = batchMember(A);
  la = accepted(a, nextID()); setTestTime(3000); submit(la);
  clearCounts(); invalidateView(1, 2, &narrow); counts("wrapped_scope"); batch("semantic_wrapped_scope");
  updateView(1, 2, 1, nullptr, 0); clearCounts();
  const auto emptySerial = viewSerial(); invalidateView(1, 2);
  assert(emptySerial == viewSerial()); counts("empty_view");

  capacityFixture(false); const ViewTileRange oneKey{0, 0, 0, 0, 10, 10};
  invalidateView(1, 2, &oneKey); counts("capacity_completed");
  capacityFixture(true); invalidateView(1, 2, &oneKey); counts("capacity_one_pending");
  clearCounts(); invalidateView(1, 2, &oneKey); counts("capacity_repeated");

#ifdef TILE_TRACE_INDEX_TESTS
  begin(); a = batchMember(A); b = batchMember(B);
  injectViewLoss = true; clearCounts(); invalidateView(1, 2, &narrow);
  assert(!injectViewLoss); batch("loss_during_scope");
  assert(collector().batches[0].members[1].uses[0].unknown);

  begin(); first = demand(50, 123456); layout = accepted(first, 900000);
  for (size_t i = 0; i < 300; ++i)
  {
    auto d = demand(1000 + i, 700000 + i);
    while (RecordIndex::bucket(session(), d.publication) == RecordIndex::bucket(session(), first.publication))
      ++d.publication;
    store(d); checkIndex();
  }
  clearCounts(); submit(layout); counts("unrelated_occupancy");
  begin(); first = demand(60, 123456); layout = accepted(first, 900000);
  clearCounts(); submit(layout); counts("low_occupancy");

  begin();
  ID pub = 1; const auto target = RecordIndex::bucket(session(), 0);
  for (size_t i = 0; i < Capacity; ++i)
  {
    while (RecordIndex::bucket(session(), pub) != target) ++pub;
    Context ctx; ctx.id = i + 100000; ctx.session = session(); ctx.publication = pub++;
    ctx.kind = i % 2 ? Kind::Demand : Kind::Publication; ctx.consumer = i; store(ctx); checkIndex();
  }
  for (size_t i = 0; i < Capacity; ++i)
  {
    const auto ctx = collector().records[i].context;
    assert(matches(ctx, true) == matches(ctx, false));
  }
  for (size_t i = 0; i < Capacity; ++i)
  {
    auto ctx = collector().records[i].context; ctx.id += Capacity; ctx.publication = i;
    store(ctx); checkIndex();
  }
  std::cout << "collision_invariants\n{\"ok\":true}\n";
#endif
}
