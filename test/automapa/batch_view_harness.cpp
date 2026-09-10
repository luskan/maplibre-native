#include <mln/util/tile_trace.hpp>
#include <algorithm>
#include <atomic>
#include <thread>
#include <iostream>
#include <vector>

using namespace mln::tiletrace;
const ViewTile A{4, 5, 0, 10, 10}, B{5, 5, 0, 10, 10}, P{2, 2, 0, 9, 9};
BatchInfo batch;
ID view;
void begin(std::vector<ViewTile> keys = {A, B})
{
  setTestDropViews(false); setTestTime(1000); configure(true, true); surfaceCreated(99);
  batch = {}; batch.session = session(); batch.id = nextID(); batch.startedUs = now();
  view = updateView(1, 2, 1, keys.data(), keys.size());
}
Context member(ViewTile key, ID map = 1, ID source = 2, ID revision = 0)
{
  auto trace = create(map, source, nextID(), key.z, key.x, key.y, key.overscaledZ, key.wrap, 0, revision ? revision : view);
  trace.id = trace.publication = nextID(); trace.kind = Kind::Publication;
  mark(trace, Producer); ++batch.total; ++batch.extracted; trackBatchMember(batch, trace);
  return trace;
}
void cache(Context trace, uint64_t time, bool stored = true)
{
  setTestTime(time); batchCacheAdmission(trace, stored, time);
}
Context layout(Context trace, bool noDraw = false)
{
  trace.id = trace.generation = nextID(); trace.kind = Kind::Layout;
  mark(trace, Layout); bindDemand(trace); batchLayoutAccepted(trace, noDraw); return trace;
}
void swap(Context trace, uint64_t time, bool success = true, const ViewTile* actual = nullptr)
{
  setTestTime(time); FrameScope frame(2); draw(trace, false, actual); swapBegin(99); swapEnd(success);
}
void dump(const char* name, uint64_t timestamp = 100000001)
{
  batch.endedUs = batch.startedUs + 200000; batch.batchMs = 200;
  setTestTime(timestamp); std::cout << name << '\n' << batchSnapshotJSON(batch, true) << '\n';
}
int main()
{
  begin(); auto a = member(A), b = member(B), p = member(P);
  cache(a, 301000); cache(b, 401000); cache(p, 601000);
  auto la = layout(a), lb = layout(b);
  swap(la, 701000); dump("visible_pending");
  swap(lb, 901000); dump("prefetch_never_drawn");
  loss(); dump("closed_after_loss");

  begin(); a = member(A); b = member(B); p = member(P);
  cache(a, 301000); cache(b, 401000); la = layout(a); lb = layout(b);
  swap(la, 701000); swap(lb, 901000); dump("prefetch_cache_pending");
  cache(p, 1601000); dump("prefetch_cache_late");

  begin({A}); a = member(A); p = member(P); cache(a, 301000);
  finish(p, Outcome::Overflow); swap(layout(a), 901000); dump("offscreen_drop");

  begin({A}); p = member(P); cache(p, 301000); dump("all_offscreen");
  begin({}); a = member(A); cache(a, 301000); dump("disabled_view");

  begin({A}); a = member(A); cache(a, 301000, false); swap(layout(a), 901000); dump("cache_off");
  begin(); a = member(A); b = member(B); cache(a, 301000); cache(b, 401000, false); dump("mixed_cache_off");

  begin({A}); a = member(A); mark(a, Converted); dump("conversion_is_not_cache");
  cache(a, 751000); dump("cache_after_conversion");
  auto demand = a; demand.id = demand.demand; demand.kind = Kind::Demand; demand.publication = 0;
  finish(demand, Outcome::Cancelled); dump("cancel_after_cache");
  retireSource(2); dump("retire_after_cache");

  begin({A}); a = member(A); finish(a, Outcome::Error); cache(a, 301000); layout(a, true); dump("error_payload");
  begin({A}); a = member(A); finish(a, Outcome::NoReceiver); dump("no_receiver");

  begin(); a = member(A); b = member(B); cache(a, 301000); cache(b, 401000);
  layout(a, true); swap(layout(b), 901000); dump("filtered_and_drawn");
  begin({A}); a = member(A); cache(a, 301000); layout(a, true); dump("filtered_only");
  begin({A}); a = member(A); cache(a, 301000); la = layout(a); dump("drawable_but_pending");
  swap(la, 901000, false); dump("failed_swap"); swap(la, 1001000); dump("swap_recovered");

  begin({A}); a = member(A); cache(a, 301000); la = layout(a);
  auto unrelated = la; unrelated.publication = nextID(); swap(unrelated, 901000); dump("replacement_not_original");
  auto fallback = la; fallback.z = 9; fallback.x = 2; fallback.y = 2; fallback.overscaledZ = 9;
  swap(fallback, 1001000); dump("fallback_not_ideal");
  auto wrongMap = la; wrongMap.map = 8; swap(wrongMap, 1101000); dump("wrong_map");
  auto wrongSource = la; wrongSource.source = 8; swap(wrongSource, 1201000); dump("wrong_source");
  swap(la, 1401000); dump("correct_generation");

  begin(); a = member(A); b = member(B); cache(a, 301000); cache(b, 401000);
  swap(layout(a), 701000); swap(layout(b), 901000);
  const ViewTile reordered[]{B, A}; auto sameView = updateView(1, 2, 1, reordered, 2);
  std::cout << "same_view_id\n{\"same\":" << (sameView == view ? "true" : "false") << "}\n";
  dump("same_content");
  updateView(1, 2, 1, &A, 1); dump("cover_changed");
  updateView(1, 2, 1, reordered, 2); dump("cover_returned");

  begin({A}); a = member(A); cache(a, 301000); swap(layout(a), 901000);
  updateView(1, 2, 2, &A, 1); dump("style_changed");
  begin({A}); a = member(A); cache(a, 301000); swap(layout(a), 901000);
  invalidateView(1, 2); dump("content_invalidated");
  view = updateView(1, 2, 1, &A, 1);
  auto next = batch; next.id = nextID(); batch = next; batch.total = batch.extracted = 0;
  a = member(A); cache(a, 1301000); swap(layout(a), 1501000); dump("new_batch_after_invalidation");

  begin({A}); a = member(A); cache(a, 301000); swap(layout(a), 901000);
  updateView(1, 2, 1, nullptr, 0); dump("source_hidden");

  auto wrapped = A; wrapped.wrap = 1;
  begin({A, wrapped}); a = member(A); cache(a, 301000); la = layout(a);
  swap(la, 901000); dump("second_wrap_pending");
  swap(la, 1101000, true, &wrapped); dump("actual_drawable_wrap");
  begin({A, wrapped}); a = member(A); cache(a, 301000); la = layout(a);
  setTestTime(901000);
  { FrameScope frame(2); draw(la, false, &A); draw(la, false, &wrapped); swapBegin(99); swapEnd(true); }
  dump("same_frame_two_wraps");

  auto over = A; over.overscaledZ = 11;
  begin({A, over}); a = member(A); cache(a, 301000); layout(a, true);
  auto overscaled = a; overscaled.overscaledZ = 11;
  dump("other_zoom_not_empty"); swap(layout(overscaled), 1101000); dump("mixed_overscaled_uses");

  begin({A}); a = member(A); cache(a, 301000); swap(layout(a), 901000);
  setTestDropViews(true); updateView(1, 2, 1, &B, 1); setTestDropViews(false);
  dump("lost_view_after_completion");
  begin({A}); setTestDropViews(true); updateView(1, 2, 1, &B, 1); setTestDropViews(false);
  b = member(B); cache(b, 301000); dump("lost_view_before_admission");
  begin({A}); invalidateView(1, 2); a = member(A); cache(a, 301000); dump("stale_demand_view");

  begin({A}); std::vector<ViewTile> many(ViewTileLimit + 1, A);
  updateView(1, 2, 1, many.data(), many.size()); a = member(A); cache(a, 301000); dump("view_overflow");
  many.clear(); for (int i = 0; i < 9; ++i) { auto key = A; key.wrap = i; many.push_back(key); }
  begin(many); a = member(A); cache(a, 301000); swap(layout(a), 901000); dump("use_overflow");
  begin({A}); a = member(A); cache(a, 301000); swap(layout(a), 901000);
  for (int i = 0; i < 16; ++i) updateView(100 + i, 200 + i, 1, &A, 1);
  dump("view_eviction");

  begin({A}); a = member(A); const auto otherView = updateView(10, 20, 1, &A, 1);
  b = member(A, 10, 20, otherView); cache(a, 301000); cache(b, 401000);
  swap(layout(a), 901000); dump("other_map_pending");
  swap(layout(b), 1201000); dump("two_maps_complete");

  begin({A}); a = member(A); la = layout(a); loss(); cache(a, 301000); swap(la, 901000); dump("loss_before_endpoints");
  begin({A}); a = member(A); cache(a, 301000); configure(false); configure(true);
  swap(layout(a), 901000); dump("pause_after_cache");
  begin({A}); a = member(A); cache(a, 301000); swap(layout(a), 901000); configure(true, true); dump("reset_session");

  begin({A}); a = member(A); a.origin = Origin::Memory;
  a.time[Features] = 0; cache(a, 51000); swap(layout(a), 101000); dump("worker_cache_reuse");

  begin({A}); a = member(A); cache(a, 301000); swap(layout(a), 901000);
  const ViewTileRange offscreen{2, 2, 2, 2, 9, 9};
  invalidateView(1, 2, &offscreen); dump("offscreen_invalidation");
  const ViewTileRange distant{500, 500, 600, 600, 10, 10};
  invalidateView(1, 2, &distant); dump("distant_region");
  const ViewTileRange intersecting{8, 10, 9, 11, 10, 11};
  invalidateView(1, 2, &intersecting); dump("intersecting_region");
  begin({A}); a = member(A); cache(a, 301000); swap(layout(a), 901000);
  const ViewTileRange wrappedRegion{1000, 0, 5, 20, 10, 10};
  invalidateView(1, 2, &wrappedRegion); dump("wrapped_region");

  begin({A}); a = member(A); cache(a, 301000);
  auto delivered = a; delivered.id = nextID(); mark(delivered, Delivered);
  auto replaced = delivered; finish(replaced, Outcome::Superseded); dump("coalesced_delivery");
  auto replacement = a; replacement.publication = nextID();
  swap(layout(replacement), 701000); dump("replacement_cannot_resolve_delivery");
  swap(layout(delivered), 901000); dump("superseded_delivery_recovery");

  const ViewTile C{6, 5, 0, 10, 10}, D{7, 5, 0, 10, 10};
  auto change = [&](std::initializer_list<ViewTile> keys, uint64_t time) {
    setTestTime(time); view = updateView(1, 2, 1, keys.begin(), keys.size());
  };
  auto cancelDemand = [](Context context, Outcome reason = Outcome::Cancelled) {
    context.id = context.demand; context.publication = 0; context.kind = Kind::Demand;
    finish(context, reason);
  };
  auto delayedDemand = [](ViewTile key) {
    return create(1, 2, nextID(), key.z, key.x, key.y, key.overscaledZ, key.wrap, 0, view);
  };
  auto admit = [&](Context demand) {
    demand.id = demand.publication = nextID(); demand.kind = Kind::Publication;
    mark(demand, Producer); ++batch.total; ++batch.extracted; trackBatchMember(batch, demand);
    return demand;
  };

  begin({A, B, C}); a = member(A); b = member(B); auto c = member(C);
  cache(a, 101000); cache(b, 151000); cache(c, 201000);
  change({B, C, D}, 301000); cancelDemand(a);
  swap(layout(b), 701000); dump("overlap_pending");
  swap(layout(c), 901000); dump("overlap_complete");
  auto oldBatch = batch;
  batch.id = nextID(); batch.total = batch.extracted = 0; batch.startedUs = 1001000;
  auto d = member(D); cache(d, 1101000); swap(layout(d), 1301000); dump("new_tile_new_batch");
  batch = oldBatch; dump("old_batch_after_new_tile");

  begin({A, B}); a = member(A); b = member(B); cache(a, 101000); cache(b, 151000);
  swap(layout(a), 201000); change({B, C}, 301000); cancelDemand(a);
  swap(layout(b), 701000); dump("drawn_tile_leaves");
  change({}, 801000); finish(a, Outcome::Error); retireSource(2); loss(); dump("completed_stays_historical");

  begin({A}); a = member(A); cache(a, 101000);
  change({B}, 201000); cancelDemand(a); dump("left_then_cancelled");
  change({A}, 301000); swap(layout(a), 501000); dump("reentry_does_not_revive");

  begin({A}); a = member(A); cache(a, 101000); la = layout(a);
  setTestTime(201000);
  {
    FrameScope frame(2); draw(la, false);
    change({B}, 301000); cancelDemand(a); change({A}, 401000);
    setTestTime(501000); draw(la, false);
    setTestTime(701000); swapBegin(99); swapEnd(true);
  }
  dump("draw_before_left_swap_after_reentry");

  begin({A}); a = member(A); cache(a, 101000); la = layout(a);
  setTestTime(201000);
  {
    FrameScope frame(2); draw(la, false); change({B}, 301000);
    setTestTime(401000); swapBegin(99); swapEnd(false);
  }
  change({A}, 501000); swap(la, 701000); dump("failed_old_frame_not_rescued_by_reentry");

  begin({A, B}); a = member(A); b = member(B); cache(a, 101000); cache(b, 151000);
  auto badLayout = layout(a); finish(badLayout, Outcome::Error);
  change({B, C}, 301000); swap(layout(b), 701000); dump("failure_before_departure");

  begin({A}); a = member(A); finish(a, Outcome::Error); cache(a, 101000);
  swap(layout(a), 701000); dump("original_error_then_draw");
  begin({A}); a = member(A); cache(a, 101000); swap(layout(a), 201000);
  finish(a, Outcome::Error); dump("original_error_after_draw");

  begin({A, B}); auto pendingA = delayedDemand(A), pendingB = delayedDemand(B);
  change({B, C}, 201000); cancelDemand(pendingA);
  setTestTime(301000); a = admit(pendingA); b = admit(pendingB);
  cache(a, 401000); cache(b, 451000); swap(layout(b), 701000); dump("late_overlap_admission");

  begin({A}); pendingA = delayedDemand(A);
  setTestTime(101000); invalidateView(1, 2); change({B}, 201000);
  setTestTime(301000); a = admit(pendingA); cache(a, 401000); dump("late_invalidated_departure");

  begin({A}); pendingA = delayedDemand(A); change({B}, 201000);
  setTestDropViews(true); change({C}, 301000); setTestDropViews(false);
  setTestTime(401000); a = admit(pendingA); cache(a, 501000); dump("late_departure_with_missing_history");

  begin({A}); pendingA = delayedDemand(A); change({B}, 201000);
  for (unsigned i = 0; i < 40; ++i) updateView(1, 2, i + 2, &B, 1);
  a = admit(pendingA); cache(a, 501000); dump("evicted_origin_before_admission");

  begin({A}); a = member(A); cache(a, 101000); change({B}, 201000);
  for (unsigned i = 0; i < 40; ++i) updateView(1, 2, i + 2, &B, 1);
  loss(); dump("departure_survives_history_eviction");

  begin({A, B}); a = member(A); b = member(B); cache(a, 101000); cache(b, 151000);
  setTestDropViews(true); change({B, C}, 201000); setTestDropViews(false);
  swap(layout(b), 701000); dump("missing_overlap_keeps_uncertainty");

  begin({A, B}); b = member(B); cache(b, 101000); change({B, C}, 201000);
  lb = layout(b); batchLayoutAccepted(lb, true, view); dump("empty_after_overlap");

  begin({A}); a = member(A); cache(a, 101000); la = layout(a);
  setTestTime(201000); invalidateView(1, 2); view = updateView(1, 2, 1, &A, 1);
  batchLayoutAccepted(la, true, view); dump("stale_empty_after_content_invalidation");
  swap(la, 701000); dump("retained_draw_after_content_invalidation");

  begin({A}); a = member(A); cache(a, 101000); la = layout(a);
  setTestTime(201000); view = updateView(1, 2, 2, &A, 1);
  batchLayoutAccepted(la, true, view); dump("empty_after_eligibility_change");

  begin({A, wrapped}); a = member(A); cache(a, 101000); change({wrapped}, 201000);
  la = layout(a); batchLayoutAccepted(la, true, view, &wrapped); dump("actual_key_empty_after_wrap_change");

  begin({A, wrapped}); a = member(A); cache(a, 101000); change({wrapped}, 201000);
  swap(layout(a), 701000, true, &wrapped); dump("one_wrap_left_one_drawn");

  begin({A}); a = member(A); cache(a, 101000);
  const auto anotherView = updateView(10, 20, 1, &A, 1);
  b = member(A, 10, 20, anotherView); cache(b, 151000);
  change({B}, 201000); swap(layout(b), 701000); dump("only_one_map_loses_tile");

  begin({A}); a = member(A); cache(a, 101000); change({B}, 201000);
  finish(a, Outcome::Error); retireSource(2); dump("departure_ignores_later_failure");

  begin({A}); a = member(A); cache(a, 101000); la = layout(a);
  setTestTime(201000); view = updateView(1, 2, 2, &A, 1);
  swap(la, 701000); dump("same_key_style_change_keeps_draw_timer");

  unsigned ordering = 0;
  std::array<int, 6> events{0, 1, 2, 3, 4, 5};
  do
  {
    auto pos = [&](int event) { return std::find(events.begin(), events.end(), event); };
    if (!(pos(0) < pos(3) && pos(3) < pos(4) && pos(1) < pos(5))) continue;
    begin(); a = member(A); b = member(B); p = member(P);
    uint64_t time = 1000, expectedCache = 0, expectedScreen = 0;
    for (int event : events)
    {
      time += 100000;
      switch (event)
      {
        case 0: cache(a, time); la = layout(a); expectedCache = time - 1000; break;
        case 1: cache(b, time); lb = layout(b); expectedCache = time - 1000; break;
        case 2: cache(p, time); expectedCache = time - 1000; break;
        case 3: swap(la, time, false); break;
        case 4: swap(la, time); expectedScreen = time - 1000; break;
        case 5: swap(lb, time); expectedScreen = time - 1000; break;
      }
    }
    const auto name = "ordering_" + std::to_string(ordering++);
    dump(name.c_str());
    std::cout << name << "_expected\n{\"mlUs\":" << expectedCache
              << ",\"displayUs\":" << expectedScreen << "}\n";
  } while (std::next_permutation(events.begin(), events.end()));
  begin({A}); a = member(A); la = layout(a); batch.endedUs = 201000;
  setTestTime(901000);
  std::atomic<bool> start{false};
  std::atomic<unsigned> work{0}, reads{0}, deferred{0};
  std::thread renderer([&] {
    while (!start.load()) std::this_thread::yield();
    for (unsigned i = 0; i < 500; ++i)
    {
      batchCacheAdmission(a, true, 301000);
      FrameScope frame(2); draw(la, false); swapBegin(99); swapEnd(true);
      ++work;
    }
  });
  std::thread views([&] {
    while (!start.load()) std::this_thread::yield();
    for (unsigned i = 0; i < 500; ++i) updateView(1, 2, i + 2, &A, 1);
  });
  std::thread snapshots([&] {
    start = true;
    for (unsigned i = 0; i < 500; ++i)
      if (!batchSnapshotJSON(batch, true).empty()) ++reads;
      else ++deferred;
  });
  renderer.join(); views.join(); snapshots.join();
  std::cout << "concurrent_work\n{\"work\":" << work.load() << ",\"reads\":" << reads.load()
            << ",\"deferred\":" << deferred.load() << "}\n";
  dump("concurrent_final");

}
