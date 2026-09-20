#include <functional>
#include <future>
#include <iostream>
#include <thread>
#include <vector>

void screenHook(const char* point);
#define TILE_TRACE_TEST_HOOK(point) screenHook(point)
#include "../../src/mln/util/tile_trace.cpp"

using namespace mln::tiletrace;
std::function<void(const std::string&)> hook;
void screenHook(const char* point) { if (hook) hook(point); }
void dump(const char* name) { std::cout << name << '\n' << snapshotJSON() << '\n'; }
void locked(const std::function<void()>& action)
{
  std::promise<void> held, release;
  auto released = release.get_future();
  std::thread owner([&]
  {
    std::lock_guard<std::mutex> lock(collector().batchMutex);
    held.set_value(); released.wait();
  });
  held.get_future().wait(); action(); release.set_value(); owner.join();
}
int main(int argc, char** argv)
{
  if (argc != 2) return 2;
  const std::string mode = argv[1];
  configure(true, true); setTestTime(1000);
  auto ctx = create(11, 22, 33, 10, 4, 5, 12, 1, 2);
  ctx.publication = 44;
  auto& c = collector();
  dump("before");
  if (mode == "finish-demand-lock") locked([&] { finish(ctx, Outcome::Superseded); });
  else if (mode == "bind-demand-lock") locked([&] { bindDemand(ctx); });
  else if (mode == "bind-demand-capacity")
  {
    auto& batch = c.batches[0]; batch.id = 55; batch.session = ctx.session; batch.count = 1;
    auto& member = batch.members[0];
    member.publication = ctx.publication; member.map = ctx.map; member.source = ctx.source; member.useCount = 1;
    for (size_t i = 0; i < member.demands.size(); ++i) member.demands[i] = 100 + i;
    bindDemand(ctx);
  }
  else if (mode == "draw-capacity" || mode == "draw-view-lock" || mode == "swap-submit-lock" || mode == "multi-use-swap")
  {
    ctx.kind = Kind::Layout; ctx.generation = ctx.id;
    FrameScope scope(2);
    if (mode == "draw-capacity")
    {
      frame.count = FrameCapacity; draw(ctx, false);
    }
    else if (mode == "draw-view-lock") locked([&] { draw(ctx, false); });
    else
    {
      draw(ctx, false);
      if (mode == "multi-use-swap")
      {
        auto other = ctx; other.id = other.generation = nextID(); other.consumer = 34;
        draw(other, false);
      }
      swapBegin(99); locked([] { swapEnd(true); });
    }
  }
  else if (mode == "no-draw-layout-lock")
  {
    ctx.kind = Kind::Layout; ctx.time[Layout] = 900;
    locked([&] { batchLayoutAccepted(ctx, true); });
  }
  else if (mode == "reset-persistence")
  {
    locked([&] { bindDemand(ctx); }); dump("first");
    configure(true, true); setTestTime(2000);
    batchLoss(ctx.publication, ctx.source, LostEvent::Screen, ctx.session, {500, 66},
              ScreenLossInfo{ScreenLossSite::DrawViewLock, &ctx});
  }
  else if (mode == "claim-order")
  {
    std::promise<void> counted, proceed;
    auto proceeding = proceed.get_future();
    std::atomic<bool> once{true};
    hook = [&](const std::string& point)
    {
      if (point == "screen_site_counted" && once.exchange(false)) { counted.set_value(); proceeding.wait(); }
    };
    std::thread first([&]
    {
      batchLoss(ctx.publication, ctx.source, LostEvent::Screen, ctx.session, {900, 66},
                ScreenLossInfo{ScreenLossSite::DrawViewLock, &ctx});
    });
    counted.get_future().wait();
    batchLoss(ctx.publication, ctx.source, LostEvent::Screen, ctx.session, {950, 67},
              ScreenLossInfo{ScreenLossSite::BindDemandLock, &ctx});
    proceed.set_value(); first.join(); hook = {};
  }
  else if (mode == "concurrent-claims")
  {
    std::promise<void> claimed, publish;
    auto published = publish.get_future();
    hook = [&](const std::string& point)
    {
      if (point == "first_screen_claimed") { claimed.set_value(); published.wait(); }
    };
    std::thread first([&]
    {
      batchLoss(ctx.publication, ctx.source, LostEvent::Screen, ctx.session, {900, 66},
                ScreenLossInfo{ScreenLossSite::DrawViewLock, &ctx});
    });
    claimed.get_future().wait();
    std::vector<std::thread> others;
    for (int i = 0; i < 4; ++i) others.emplace_back([&]
    {
      batchLoss(ctx.publication, ctx.source, LostEvent::Screen, ctx.session, {950, 67},
                ScreenLossInfo{ScreenLossSite::BindDemandLock, &ctx});
    });
    for (auto& other : others) other.join();
    dump("writing"); publish.set_value(); first.join(); hook = {};
  }
  else return 2;
  dump("after");
  std::cout << "size\n{\"diagnosticsBytes\":" << sizeof(LossDiagnostics) << "}\n";
}
