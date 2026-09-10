#ifdef NDEBUG
#error "Snapshot pool checks require assertions"
#endif
void traceTestHook(const char*);
#define TILE_TRACE_TEST_HOOK(point) traceTestHook(point)
#include "../../src/mln/util/tile_trace.cpp"
#include <cassert>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <iostream>
#include <thread>
#include <vector>

using namespace mln::tiletrace;
std::function<void(const char*)> hook;
std::atomic<size_t> publications{0};
thread_local bool bypassHook = false;
void traceTestHook(const char* point)
{
  if (!std::strcmp(point, "batch_published")) ++publications;
  if (!bypassHook && hook) hook(point);
}
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
    assert(cv.wait_for(lock, std::chrono::seconds(5), [&] { return entered; }));
  }
  void release() { std::lock_guard<std::mutex> lock(mutex); released = true; cv.notify_all(); }
};
struct Sample
{
  BatchInfo info;
  std::vector<Context> members;
};
ID view;
void checkPartition()
{
  auto& c = collector();
  std::lock_guard<std::mutex> lock(c.batchMutex);
  std::array<bool, BatchCapacity + 2> used{};
  for (const auto& mapping : c.publishedIndex)
  {
    const auto index = mapping.load();
    assert(index < used.size() && !used[index]); used[index] = true;
  }
  for (const auto index : c.publishedSpare)
  {
    assert(index < used.size() && !used[index]); used[index] = true;
  }
  assert(std::all_of(used.begin(), used.end(), [](bool value) { return value; }));
  for (const auto& batch : c.batches) assert(!batch.dirty);
}
void begin()
{
  setTestTime(1000); configure(true, true); surfaceCreated(99);
  std::array<ViewTile, ViewTileLimit> keys;
  for (size_t i = 0; i < keys.size(); ++i) keys[i] = {uint32_t(i + 4), 5, 0, 10, 10};
  view = updateView(1, 2, 1, keys.data(), keys.size()); checkPartition();
}
Sample sample(size_t count = 1)
{
  const auto start = now() + 100;
  setTestTime(start);
  Sample result;
  auto& info = result.info; info.session = session(); info.id = nextID(); info.startedUs = start;
  for (size_t i = 0; i < count; ++i)
  {
    auto trace = create(1, 2, nextID(), 10, uint32_t(i + 4), 5, 10, 0, 0, view);
    trace.id = trace.publication = nextID(); trace.kind = Kind::Publication; mark(trace, Producer);
    ++info.total; trackBatchMember(info, trace); result.members.push_back(trace);
  }
  setTestTime(start + 10);
  for (const auto& trace : result.members) batchCacheAdmission(trace, true, now());
  {
    FrameScope scope(2);
    setTestTime(start + 20);
    for (const auto& trace : result.members)
    {
      auto accepted = trace; accepted.id = accepted.generation = nextID(); accepted.kind = Kind::Layout;
      mark(accepted, Layout); bindDemand(accepted); draw(accepted, false);
    }
    setTestTime(start + 40); swapBegin(99); swapEnd(true);
  }
  info.endedUs = now(); checkPartition(); return result;
}
std::vector<Sample> fill()
{
  std::vector<Sample> samples;
  for (size_t i = 0; i < BatchCapacity; ++i) samples.push_back(sample(i % 3 + 1));
  return samples;
}
void emit(const std::string& name, const std::string& value) { std::cout << name << '\n' << value << '\n'; }
void dump(const std::string& name, const Sample& value) { emit(name, batchSnapshotJSON(value.info, true)); }
void publish(const Sample& value)
{
  auto trace = value.members.front();
  setTestTime(now() + 1); finish(trace, Outcome::Error); checkPartition();
}
std::vector<unsigned char> bytes(size_t index)
{
  const auto& batch = collector().published[index].batch;
  std::vector<unsigned char> result(sizeof(batch));
  std::memcpy(result.data(), &batch, result.size()); return result;
}
void retainedCases()
{
  begin(); auto values = fill();
  for (size_t i = 0; i < values.size(); ++i) dump("retained_" + std::to_string(i), values[i]);
  auto replacement = sample(); dump("seventeenth", replacement); dump("evicted", values[0]);
  for (size_t i = 1; i < values.size(); ++i) dump("after_eviction_" + std::to_string(i), values[i]);
  begin(); auto large = sample(BatchMemberCapacity);
  for (size_t i = 1; i < BatchCapacity; ++i) sample();
  auto small = sample();
  assert(collector().batches[0].members.back().publication == large.members.back().publication);
  dump("small_replacement", small); dump("large_evicted", large);
}
void pinnedCase()
{
  begin(); auto values = fill(); auto& c = collector();
  const auto pinned = c.publishedIndex[0].load();
  const auto original = bytes(pinned);
  Gate pin; std::atomic<size_t> pins{0};
  hook = [&](const char* point) {
    if (!std::strcmp(point, "snapshot_pinned") && pins.fetch_add(1) == 0) pin.pause();
  };
  std::string result;
  std::thread reader([&] { result = batchSnapshotJSON(values[0].info, true); });
  pin.wait(); const auto before = publications.load();
  publish(values[0]); assert(c.publishedSpare[0] == pinned);
  for (size_t round = 0; round < 32; ++round) for (const auto& value : values) publish(value);
  assert(c.published[pinned].readers == 1 && bytes(pinned) == original);
  assert(publications - before == 513);
  pin.release(); reader.join(); hook = {};
  emit("pinned_old", result); dump("pinned_current", values[0]);
  emit("pinned_progress", "{\"publicationsWhilePinned\":" + std::to_string(publications - before) + '}');
}
void secondSpareCase()
{
  begin(); auto values = fill(); auto& c = collector();
  const auto second = c.publishedIndex[1].load();
  const auto original = bytes(second);
  Gate firstPin, secondPin, claimed;
  std::atomic<size_t> pins{0};
  hook = [&](const char* point) {
    if (!std::strcmp(point, "publication_claimed")) claimed.pause();
    if (!std::strcmp(point, "snapshot_pinned"))
    {
      const auto index = pins.fetch_add(1);
      if (index == 0) firstPin.pause();
      if (index == 1) secondPin.pause();
    }
  };
  std::string result;
  std::thread reader([&] { result = batchSnapshotJSON(values[0].info, true); });
  firstPin.wait(); publish(values[0]);
  std::thread writer([&] { publish(values[1]); });
  claimed.wait(); firstPin.release(); secondPin.wait(); claimed.release(); writer.join();
  assert(c.publishedSpare[1] == second && c.published[second].readers == 1);
  const auto before = publications.load();
  for (size_t round = 0; round < 32; ++round) for (const auto& value : values) publish(value);
  assert(bytes(second) == original);
  assert(publications - before == 512);
  secondPin.release(); reader.join(); hook = {};
  emit("second_spare_result", result);
  emit("second_spare_progress", "{\"publicationsWhilePinned\":" + std::to_string(publications - before) + '}');
}
void staleIndexCase(int mode)
{
  begin(); auto values = fill(); auto& c = collector();
  const auto previous = c.publishedIndex[0].load();
  Gate loaded; std::atomic<size_t> loads{0};
  hook = [&](const char* point) {
    if (!std::strcmp(point, "snapshot_index") && loads.fetch_add(1) == 0) loaded.pause();
  };
  std::string result;
  std::thread reader([&] { result = batchSnapshotJSON(values[0].info, true); });
  loaded.wait();
  if (mode == 0)
  {
    publish(values[0]); publish(values[1]); assert(c.publishedIndex[1] == previous);
  }
  else if (mode == 1)
  {
    publish(values[0]); publish(values[0]); assert(c.publishedIndex[0] == previous);
  }
  else
  {
    auto replacement = sample();
    if (c.publishedIndex[0] != previous) publish(replacement);
    assert(c.publishedIndex[0] == previous);
  }
  loaded.release(); reader.join(); hook = {};
  emit("stale_index_" + std::to_string(mode), result);
}
void resetCase()
{
  begin(); auto values = fill(); auto& c = collector();
  const auto pinned = c.publishedIndex[0].load(); const auto original = bytes(pinned);
  Gate pin; std::atomic<size_t> pins{0};
  hook = [&](const char* point) {
    if (!std::strcmp(point, "snapshot_pinned") && pins.fetch_add(1) == 0) pin.pause();
  };
  std::string result;
  std::thread reader([&] { result = batchSnapshotJSON(values[0].info, true); });
  pin.wait(); begin(); auto fresh = fill();
  for (const auto& value : fresh) publish(value);
  assert(c.published[pinned].readers == 1 && bytes(pinned) == original);
  pin.release(); reader.join(); hook = {};
  assert(result.empty());
  emit("reader_across_reset", "{\"deferred\":true,\"oldSession\":\"" +
    std::to_string(values[0].info.session) + "\"}");
  dump("old_after_reset", values[0]);
  for (size_t i = 0; i < fresh.size(); ++i) dump("reset_fresh_" + std::to_string(i), fresh[i]);
}
void concurrentCase()
{
  begin(); auto values = fill();
  for (size_t i = 0; i < values.size(); ++i) dump("concurrent_expected_" + std::to_string(i), values[i]);
  std::array<std::vector<std::string>, 3> snapshots;
  hook = [&](const char* point) {
    if (std::strcmp(point, "snapshot_pinned")) return;
    size_t pinned = 0;
    for (const auto& buffer : collector().published) pinned += buffer.readers.load() == 1;
    assert(pinned == 1);
  };
  std::array<std::thread, 3> readers;
  for (size_t r = 0; r < readers.size(); ++r)
    readers[r] = std::thread([&, r] {
      for (size_t i = 0; i < 64; ++i) snapshots[r].push_back(batchSnapshotJSON(values[i % values.size()].info, true));
    });
  for (size_t round = 0; round < 32; ++round) for (const auto& value : values) publish(value);
  for (auto& reader : readers) reader.join();
  hook = {}; checkPartition();
  for (size_t r = 0; r < snapshots.size(); ++r)
    for (size_t i = 0; i < snapshots[r].size(); ++i)
      emit("concurrent_" + std::to_string(r) + '_' + std::to_string(i), snapshots[r][i]);
}
int main()
{
  bypassHook = true;
  checkPartition();
  for (size_t i = 0; i < BatchCapacity; ++i) assert(collector().publishedIndex[i] == i);
  emit("storage", "{\"buffers\":" + std::to_string(collector().published.size()) +
    ",\"collectorBytes\":" + std::to_string(sizeof(Collector)) + '}');
  retainedCases(); pinnedCase(); secondSpareCase();
  for (int i = 0; i < 3; ++i) staleIndexCase(i);
  resetCase(); concurrentCase();
}
