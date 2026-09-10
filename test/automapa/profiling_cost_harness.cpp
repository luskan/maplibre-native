#ifndef TILE_TRACE_SOURCE
#define TILE_TRACE_SOURCE "../../src/mln/util/tile_trace.cpp"
#endif
#include TILE_TRACE_SOURCE
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <vector>

using namespace mln::tiletrace;
using Clock = std::chrono::steady_clock;
volatile size_t observed = 0;
double cpuTimeUs()
{
  timespec time{};
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time)) std::abort();
  return double(time.tv_sec) * 1000000 + double(time.tv_nsec) / 1000;
}
template<class Callback> void measure(const char* mode, size_t count, size_t iterations, Callback callback)
{
  for (size_t i = 0; i < 5; ++i) callback();
  std::array<double, 20> samples, cpuSamples;
  for (size_t sample = 0; sample < samples.size(); ++sample)
  {
    const auto start = Clock::now();
    const auto cpuStart = cpuTimeUs();
    for (size_t i = 0; i < iterations; ++i) callback();
    cpuSamples[sample] = (cpuTimeUs() - cpuStart) / iterations;
    samples[sample] = std::chrono::duration<double, std::micro>(Clock::now() - start).count() / iterations;
  }
  std::sort(samples.begin(), samples.end());
  std::sort(cpuSamples.begin(), cpuSamples.end());
  observed = snapshotJSON().size();
  for (const auto& batch : collector().batches)
    if (batch.id)
    {
      BatchInfo info; info.id = batch.id; info.session = batch.session; info.total = batch.count;
      info.startedUs = 1; info.endedUs = now();
      observed = batchSnapshotJSON(info, true).size();
    }
  std::printf("{\"mode\":\"%s\",\"count\":%zu,\"median_us\":%.3f,\"p95_us\":%.3f,"
              "\"median_cpu_us\":%.3f,\"p95_cpu_us\":%.3f,"
              "\"collector_bytes\":%zu,\"record_bytes\":%zu,\"frame_bytes\":%zu}\n",
              mode, count, samples[10], samples[18], cpuSamples[10], cpuSamples[18],
              sizeof(Collector), sizeof(Record), sizeof(Frame));
}
Context makeDemand(const ViewTile& key, ID view, ID consumer)
{
  return create(1, 2, consumer, key.z, key.x, key.y, key.overscaledZ, key.wrap, 0, view);
}
Context prepare(Context ctx, BatchInfo* info)
{
  ctx.id = ctx.publication = nextID(); ctx.kind = Kind::Publication; mark(ctx, Producer);
  if (info) { ++info->total; trackBatchMember(*info, ctx); }
  batchCacheAdmission(ctx, true, now());
  ctx.id = ctx.generation = nextID(); ctx.kind = Kind::Layout; mark(ctx, Layout); bindDemand(ctx);
  return ctx;
}
void frameFor(const std::vector<Context>& entries)
{
  FrameScope scope(2);
  for (const auto& ctx : entries) { draw(ctx, false); draw(ctx, true); }
  swapBegin(99); swapEnd(true);
}
int main(int argc, char** argv)
{
  const std::string mode = argc > 1 ? argv[1] : "redraw";
  const size_t count = argc > 2 ? std::strtoul(argv[2], nullptr, 10) : 80;
  if (!count || count > Capacity) return 2;
  configure(true, true); surfaceCreated(99);
  if (mode == "bind")
  {
    const ViewTile key{4, 5, 0, 10, 10};
    const auto view = updateView(1, 2, 1, &key, 1);
    std::vector<Context> demands;
    for (size_t i = 0; i < count; ++i) demands.push_back(makeDemand(key, view, i + 1));
    measure(mode.c_str(), count, 4, [&] {
      for (size_t i = 0; i < count; ++i) { demands[i].publication = 100000 + i; bindDemand(demands[i]); }
      for (auto& ctx : demands) { ctx.publication = 0; bindDemand(ctx); }
    });
    return 0;
  }
  if (mode == "arrival")
  {
    const ViewTile key{4, 5, 0, 10, 10};
    const auto view = updateView(1, 2, 1, &key, 1);
    measure(mode.c_str(), count, 20, [&] {
      for (size_t i = 0; i < count; ++i)
      {
        BatchInfo info; info.session = session(); info.id = nextID(); info.startedUs = now();
        auto ctx = prepare(makeDemand(key, view, nextID()), &info);
        FrameScope scope(2); draw(ctx, false); swapBegin(99); swapEnd(true);
      }
    });
    return 0;
  }
  if (mode == "redraw" || mode == "lookup")
  {
    if (count > ViewTileLimit) return 2;
    for (size_t i = 0; i < Capacity; ++i)
    {
      auto ctx = makeDemand({uint32_t(i), 0, 0, 14, 14}, 0, i + 1);
      ctx.publication = nextID(); bindDemand(ctx);
    }
    std::vector<ViewTile> keys;
    for (size_t i = 0; i < count; ++i) keys.push_back({uint32_t(i), 0, 0, 14, 14});
    const auto view = updateView(1, 2, 1, keys.data(), keys.size());
    BatchInfo info; info.id = nextID(); info.session = session(); info.startedUs = now();
    std::vector<Context> entries;
    for (const auto& key : keys)
      entries.push_back(prepare(makeDemand(key, view, nextID()), mode == "redraw" ? &info : nullptr));
    measure(mode.c_str(), count, 50, [&] { frameFor(entries); });
    return 0;
  }
  if (mode == "complete" || mode == "pending" || mode == "repeat")
  {
    if (count > BatchCapacity) return 2;
    std::vector<ViewTile> keys;
    for (size_t i = 0; i < ViewTileLimit; ++i) keys.push_back({uint32_t(i / 8), 0, int16_t(i % 8), 10, 10});
    const auto view = updateView(1, 2, 1, keys.data(), keys.size());
    const auto* current = currentView(collector(), 1, 2, session());
    for (size_t i = 0; i < count; ++i)
    {
      auto& batch = collector().batches[i]; batch.id = nextID(); batch.session = session();
      batch.count = BatchMemberCapacity; batch.dirty = true;
      for (size_t j = 0; j < batch.count; ++j)
      {
        auto& member = batch.members[j]; member = {};
        member.map = 1; member.source = 2; member.view = view; member.publication = nextID();
        member.viewKnown = true; member.viewGap = current->gap; member.tile = keys[(j % 32) * 8];
        member.useCount = 8;
        for (size_t k = 0; k < member.useCount; ++k)
        {
          const auto index = (j % 32) * 8 + k;
          auto& use = member.uses[k]; use.tile = keys[index]; use.requirement = current->requirements[index];
          if (mode == "complete") { use.submittedUs = 1; use.exact = true; }
        }
      }
    }
    publishBatches(collector());
    const ViewTileRange range{0, 0, 0, 0, 10, 10};
    measure(mode.c_str(), count, 20, [&] {
      if (mode == "pending")
        for (size_t i = 0; i < count; ++i)
          for (auto& member : collector().batches[i].members)
            for (auto& use : member.uses) use.failed = false;
      invalidateView(1, 2, &range);
      observed = viewSerial();
    });
    return 0;
  }
  return 2;
}
