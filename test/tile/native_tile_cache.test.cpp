#include <mln/test/util.hpp>
#include <mln/tile/native_tile_cache.hpp>
#include <mln/util/rapidjson.hpp>

#include <deque>
#include <map>
#include <stdexcept>

using namespace mln;

namespace
{
class ManualScheduler final : public Scheduler
{
public:
  ~ManualScheduler() override { weak.invalidateWeakPtrs(); }
  void schedule(std::function<void()>&& fn) override
  {
    if(rejectSchedule) throw std::runtime_error("Injected scheduler failure");
    tasks.push_back(std::move(fn));
  }
  void schedule(util::SimpleIdentity, std::function<void()>&& fn) override { schedule(std::move(fn)); }
  mapbox::base::WeakPtr<Scheduler> makeWeakPtr() override { return weak.makeWeakPtr(); }
  void waitForEmpty(util::SimpleIdentity) override { drain(); }
  bool step()
  {
    if(tasks.empty()) return false;
    auto task = std::move(tasks.front());
    tasks.pop_front();
    running = true;
    task();
    task = {};
    running = false;
    return true;
  }
  void drain()
  {
    size_t turns = 0;
    while(step())
    {
      if(++turns > 1000) throw std::runtime_error("Native cache did not become idle");
    }
  }
  bool running = false;
  bool rejectSchedule = false;
  std::deque<std::function<void()>> tasks;
private:
  mapbox::base::WeakPtrFactory<Scheduler> weak{this};
};

struct StoreData
{
  std::map<std::string, std::string> records;
  std::vector<std::string> events;
  std::function<void()> onRead, onWrite;
  size_t opens = 0, reads = 0, writes = 0, clears = 0, closes = 0, erases = 0, maintenance = 0;
  bool failRead = false, failWrite = false, failClear = false;
};

class FakeStore final : public NativeTileStore
{
public:
  FakeStore(std::shared_ptr<StoreData> data_, std::shared_ptr<ManualScheduler> scheduler_)
    : data(std::move(data_)), scheduler(std::move(scheduler_))
  {
    EXPECT_TRUE(scheduler->running);
    ++data->opens;
  }
  ~FakeStore() override
  {
    EXPECT_TRUE(scheduler->running);
    ++data->closes;
  }
  std::optional<size_t> recordSize(const std::string& key) override
  {
    EXPECT_TRUE(scheduler->running);
    const auto found = data->records.find(key);
    return found == data->records.end() ? std::nullopt : std::optional<size_t>(found->second.size());
  }
  std::optional<std::string> read(const std::string& key, size_t maximum) override
  {
    EXPECT_TRUE(scheduler->running);
    ++data->reads;
    data->events.push_back("read");
    if(data->onRead) data->onRead();
    if(data->failRead) throw std::runtime_error("Injected native read failure");
    const auto found = data->records.find(key);
    if(found == data->records.end() || found->second.size() > maximum) return {};
    return found->second;
  }
  bool write(const std::string& key, const std::string& record) override
  {
    EXPECT_TRUE(scheduler->running);
    ++data->writes;
    data->events.push_back("write");
    if(data->onWrite) data->onWrite();
    if(data->failWrite) throw std::runtime_error("Injected native write failure");
    if(data->maintenance) { --data->maintenance; return false; }
    data->records[key] = record;
    return true;
  }
  void erase(const std::string& key) override { ++data->erases; data->records.erase(key); }
  void clear() override
  {
    EXPECT_TRUE(scheduler->running);
    if(data->failClear) throw std::runtime_error("Injected native clear failure");
    ++data->clears;
    data->events.push_back("clear");
    data->records.clear();
  }
  uint64_t storedBytes() override
  {
    uint64_t sum = 0;
    for(const auto& [key, record] : data->records) sum += record.size();
    return sum;
  }
private:
  std::shared_ptr<StoreData> data;
  std::shared_ptr<ManualScheduler> scheduler;
};

NativeTileMetadata metadata()
{
  return {CanonicalTileID(2, 1, 1), nativeTileConversionContract(style::CustomGeometrySource::TileOptions{})};
}

NativeTilePayloadPtr payload(size_t coordinates = 2)
{
  NativeTileBuilder builder(metadata());
  GeometryCoordinates part;
  for(size_t i = 0; i < coordinates; ++i) part.emplace_back(static_cast<int16_t>(i), 0);
  GeometryCollection geometry;
  geometry.push_back(std::move(part));
  builder.appendFinal(FeatureType::LineString, std::move(geometry), PropertyMap{{"road", "test"}}, uint64_t{7});
  return std::move(builder).seal();
}

class NativeCacheTest : public ::testing::Test
{
protected:
  std::shared_ptr<ManualScheduler> scheduler = std::make_shared<ManualScheduler>();
  std::shared_ptr<StoreData> data = std::make_shared<StoreData>();
  NativeTileCache::Config config;
  std::unique_ptr<NativeTileCache> cache;
  NativeTileCache::Policy policy;

  void SetUp() override
  {
    config.store = {32 * 1024, 128 * 1024, 1024, 16};
    config.codec.storedBytes = 32 * 1024;
    config.codec.decodedBytes = 128 * 1024;
    config.retainedBytes = 32 * 1024;
    config.chargedBytes = 512 * 1024;
  }
  void start(bool disk = true)
  {
    cache = std::make_unique<NativeTileCache>(config,
      [data = data, scheduler = scheduler](const std::string&, NativeTileStoreLimits)
        { return std::make_unique<FakeStore>(data, scheduler); }, scheduler);
    policy = cache->configure("native", "test-build", true, disk);
    scheduler->drain();
  }
  void TearDown() override { cache.reset(); scheduler->drain(); }
  uint64_t number(const char* key)
  {
    rapidjson::Document document;
    document.Parse(cache->snapshotJSON().c_str());
    EXPECT_FALSE(document.HasParseError());
    return document[key].GetUint64();
  }
  void populate()
  {
    auto tile = payload();
    cache->persist("tile", tile, policy);
    scheduler->drain();
    ASSERT_EQ(data->records.size(), 1u);
  }
};

TEST_F(NativeCacheTest, ExtractionActivityUsesOwnerLifetimeAndBalancesEveryExit)
{
  start();
  rapidjson::Document initial;
  initial.Parse(cache->snapshotJSON().c_str());
  const std::string owner = initial["ownerID"].GetString();
  const auto created = number("createdUs");
  {
    auto cancelled = cache->beginExtraction();
    {
      auto completed = cache->beginExtraction();
      EXPECT_EQ(number("extractionInFlight"), 2u);
      completed.complete();
      completed.complete();
    }
    EXPECT_EQ(number("extractionInFlight"), 1u);
    EXPECT_EQ(number("extractionCompleted"), 1u);
  }
  EXPECT_EQ(number("extractionStarted"), 2u);
  EXPECT_EQ(number("extractionAborted"), 1u);
  EXPECT_EQ(number("extractionInFlight"), 0u);
  cache->clear(true);
  scheduler->drain();
  cache->trim();
  scheduler->drain();
  cache->configure("native", "other-build", false, false);
  scheduler->drain();
  EXPECT_EQ(number("extractionStarted"), 2u);
  EXPECT_EQ(number("createdUs"), created);
  EXPECT_GE(number("sampledUs"), created);
  {
    auto withoutRetention = cache->beginExtraction();
    withoutRetention.complete();
  }
  EXPECT_EQ(number("extractionStarted"), 3u);
  cache.reset();
  scheduler->drain();
  start();
  rapidjson::Document next;
  next.Parse(cache->snapshotJSON().c_str());
  EXPECT_NE(std::string(next["ownerID"].GetString()), owner);
  EXPECT_EQ(number("extractionStarted"), 0u);
}
}

TEST_F(NativeCacheTest, MemoryPromotionSharesOneAllocationAndClearReleasesOptionalOwnership)
{
  start(false);
  auto tile = payload();
  const auto charge = tile->cacheMemoryCharge();
  ASSERT_TRUE(cache->remember("runtime", tile, policy));
  ASSERT_TRUE(cache->remember("durable", tile, policy));
  EXPECT_EQ(number("entries"), 1u);
  EXPECT_EQ(number("retainedBytes"), charge);
  EXPECT_FALSE(cache->get("runtime", metadata(), policy));
  EXPECT_EQ(cache->get("durable", metadata(), policy), tile);
  const auto used = number("chargedBytes");
  cache->clear(false);
  const auto fresh = cache->policy();
  EXPECT_FALSE(cache->remember("late", tile, policy));
  EXPECT_FALSE(cache->get("durable", metadata(), fresh));
  scheduler->drain();
  EXPECT_EQ(number("retainedBytes"), 0u);
  EXPECT_LT(number("chargedBytes"), used);
  const auto pinned = number("chargedBytes");
  tile.reset();
  EXPECT_EQ(number("chargedBytes"), pinned - charge);
  EXPECT_EQ(data->opens, 0u);
}

TEST_F(NativeCacheTest, CoalescesDiskReadsAndPreservesPerConsumerCancellationAndOrigin)
{
  start();
  populate();
  bool alive = true;
  unsigned calls = 0;
  NativeTilePayloadPtr restored;
  cache->readWithOrigin("tile", metadata(), policy, [&] { return alive; },
    [&](NativeTilePayloadPtr value, bool disk) { ++calls; EXPECT_FALSE(value); EXPECT_FALSE(disk); });
  cache->readWithOrigin("tile", metadata(), policy, {}, [&](NativeTilePayloadPtr value, bool disk)
    { ++calls; EXPECT_TRUE(disk); restored = std::move(value); });
  alive = false;
  EXPECT_EQ(calls, 0u);
  scheduler->drain();
  ASSERT_TRUE(restored);
  EXPECT_EQ(calls, 2u);
  EXPECT_EQ(data->reads, 1u);
  EXPECT_EQ(cache->get("tile", metadata(), policy), restored);
  cache->readWithOrigin("tile", metadata(), policy, {}, [&](NativeTilePayloadPtr value, bool disk)
    { ++calls; EXPECT_EQ(value, restored); EXPECT_FALSE(disk); });
  scheduler->drain();
  EXPECT_EQ(calls, 3u);
  EXPECT_EQ(data->reads, 1u);
}

TEST_F(NativeCacheTest, CancelsQueuedWorkBeforeClearAndRejectsOldProducerGeneration)
{
  start();
  populate();
  unsigned replies = 0, hooks = 0, completions = 0;
  cache->read("tile", metadata(), policy, {}, [&](NativeTilePayloadPtr value)
    { ++replies; EXPECT_FALSE(value); });
  cache->persist("late", payload(), policy);
  cache->postBackground(policy, [&](NativeTileStore*) { ++hooks; return true; },
    [&](bool success) { ++completions; EXPECT_FALSE(success); });
  bool cleared = false;
  cache->clear(true, [&](bool success) { cleared = success; EXPECT_TRUE(data->records.empty()); });
  cache->persist("stale", payload(), policy);
  EXPECT_FALSE(cleared);
  scheduler->drain();
  EXPECT_TRUE(cleared);
  EXPECT_EQ(replies, 1u);
  EXPECT_EQ(hooks, 0u);
  EXPECT_EQ(completions, 1u);
  EXPECT_EQ(data->writes, 1u);
  EXPECT_EQ(data->reads, 0u);
  EXPECT_EQ(number("pendingControls"), 0u);
  EXPECT_EQ(number("pendingWrites"), 0u);
}

TEST_F(NativeCacheTest, ClearDuringReadRejectsResultAndDoesNotPopulateMemory)
{
  start();
  populate();
  bool cleared = false;
  unsigned replies = 0;
  data->onRead = [&] { cache->clear(true, [&](bool success) { cleared = success; }); };
  cache->read("tile", metadata(), policy, {}, [&](NativeTilePayloadPtr value)
    { ++replies; EXPECT_FALSE(value); });
  scheduler->drain();
  EXPECT_TRUE(cleared);
  EXPECT_EQ(replies, 1u);
  EXPECT_EQ(number("retainedBytes"), 0u);
  EXPECT_EQ(number("pendingReads"), 0u);
  EXPECT_TRUE(data->records.empty());
}

TEST_F(NativeCacheTest, NewGenerationDemandWaitsForClearBarrier)
{
  start();
  populate();
  bool cleared = false;
  cache->clear(true, [&](bool success) { cleared = success; });
  auto fresh = cache->policy();
  unsigned replies = 0;
  cache->read("tile", metadata(), fresh, {}, [&](NativeTilePayloadPtr value)
    { ++replies; EXPECT_TRUE(cleared); EXPECT_FALSE(value); });
  scheduler->drain();
  EXPECT_EQ(replies, 1u);
  EXPECT_EQ(data->reads, 0u);
}

TEST_F(NativeCacheTest, ReadPriorityInterruptsMaintenanceTurnsAndWritesCoalesce)
{
  start();
  populate();
  data->events.clear();
  data->maintenance = 1;
  auto tile = payload();
  cache->persist("new", tile, policy);
  cache->persist("new", tile, policy);
  ASSERT_TRUE(scheduler->step());
  ASSERT_EQ(data->events, std::vector<std::string>{"write"});
  cache->read("tile", metadata(), policy, {}, [&](NativeTilePayloadPtr value) { EXPECT_TRUE(value); });
  scheduler->drain();
  EXPECT_EQ(data->events, (std::vector<std::string>{"write", "read", "write"}));
  EXPECT_EQ(data->records.size(), 2u);
  EXPECT_EQ(number("maintenanceTurns"), 1u);
}

TEST_F(NativeCacheTest, DeferredBurstPersistsBothLiveTilesWithOneWriterAndReadPriority)
{
  start();
  auto first = payload();
  auto second = payload(3);
  ASSERT_TRUE(cache->remember("first", first, policy));
  ASSERT_TRUE(cache->remember("second", second, policy));
  cache->persist("first", first, policy);
  const auto before = number("chargedBytes");
  cache->persist("second", second, policy);
  const auto deferredBytes = number("deferredWriteBytes");
  cache->persist("second", second, policy);
  EXPECT_EQ(number("pendingWrites"), 2u);
  EXPECT_EQ(number("queuedWrites"), 1u);
  EXPECT_EQ(number("deferredWrites"), 1u);
  EXPECT_EQ(number("chargedBytes"), before + deferredBytes);
  EXPECT_EQ(number("writeDeferrals"), 1u);
  EXPECT_EQ(number("droppedWrites"), 0u);
  data->onWrite = [&] { EXPECT_EQ(number("activeWrites"), 1u); };
  ASSERT_TRUE(scheduler->step());
  EXPECT_EQ(data->records.size(), 1u);
  cache->read("missing", metadata(), policy, {}, [](NativeTilePayloadPtr value) { EXPECT_FALSE(value); });
  ASSERT_TRUE(scheduler->step());
  EXPECT_EQ(number("pendingReads"), 0u);
  EXPECT_EQ(data->records.size(), 1u);
  EXPECT_EQ(number("deferredWrites"), 1u);
  scheduler->drain();
  EXPECT_EQ(data->records.size(), 2u);
  EXPECT_EQ(number("writes"), 2u);
  EXPECT_EQ(number("promotedWrites"), 1u);
  EXPECT_EQ(number("pendingWrites"), 0u);
  EXPECT_EQ(number("deferredWriteBytes"), 0u);
  EXPECT_EQ(number("droppedWrites"), 0u);
  EXPECT_LE(number("peakBytes"), config.chargedBytes);
}

TEST_F(NativeCacheTest, DeferredCandidateDoesNotPinPayloadOrReserveItsAllocation)
{
  start();
  cache->persist("first", payload(), policy);
  auto second = payload(3);
  std::weak_ptr<const NativeTilePayload> weak = second;
  const auto before = number("chargedBytes");
  cache->persist("second", second, policy);
  EXPECT_FALSE(second->cacheLease());
  EXPECT_EQ(number("chargedBytes"), before + number("deferredWriteBytes"));
  second.reset();
  EXPECT_TRUE(weak.expired());
  scheduler->drain();
  EXPECT_EQ(number("writes"), 1u);
  EXPECT_EQ(number("expiredWrites"), 1u);
  EXPECT_EQ(number("droppedWrites"), 1u);
  EXPECT_EQ(number("pendingWrites"), 0u);
}

TEST_F(NativeCacheTest, DeferredCandidateCountAndMetadataBytesAreBounded)
{
  config.maxDeferredWrites = 1;
  start();
  auto tile = payload();
  cache->persist("first", tile, policy);
  cache->persist("second", tile, policy);
  const auto charged = number("chargedBytes");
  cache->persist("third", tile, policy);
  EXPECT_EQ(number("chargedBytes"), charged);
  EXPECT_EQ(number("deferredWrites"), 1u);
  EXPECT_EQ(number("droppedWrites"), 1u);
  EXPECT_LE(number("deferredWriteBytes"), config.deferredWriteBytes);
  scheduler->drain();
  EXPECT_EQ(number("writes"), 2u);
}

TEST_F(NativeCacheTest, DeferredCandidateMetadataLimitRejectsWithoutPinning)
{
  config.deferredWriteBytes = 1;
  start();
  cache->persist("first", payload(), policy);
  auto second = payload();
  const auto charged = number("chargedBytes");
  cache->persist("second", second, policy);
  EXPECT_FALSE(second->cacheLease());
  EXPECT_EQ(number("chargedBytes"), charged);
  EXPECT_EQ(number("deferredWrites"), 0u);
  EXPECT_EQ(number("droppedWrites"), 1u);
  scheduler->drain();
  EXPECT_EQ(number("writes"), 1u);
}

TEST_F(NativeCacheTest, DeferredPromotionRequiresBudgetBeforePinningAndDropsWithoutRetry)
{
  start();
  cache->persist("first", payload(), policy);
  auto second = payload(3);
  cache->persist("second", second, policy);
  ASSERT_TRUE(scheduler->step());
  auto pressure = cache->reservePrepared(config.chargedBytes - number("chargedBytes"));
  ASSERT_TRUE(pressure);
  scheduler->drain();
  EXPECT_FALSE(second->cacheLease());
  EXPECT_EQ(number("promotedWrites"), 0u);
  EXPECT_EQ(number("droppedWrites"), 1u);
  EXPECT_EQ(number("pendingWrites"), 0u);
  EXPECT_EQ(number("writes"), 1u);
  EXPECT_LE(number("peakBytes"), config.chargedBytes);
}

TEST_F(NativeCacheTest, DeferredPromotionReusesChargeAttachedAfterAdmission)
{
  start();
  cache->persist("first", payload(), policy);
  auto second = payload(3);
  cache->persist("second", second, policy);
  ASSERT_FALSE(second->cacheLease());
  ASSERT_TRUE(cache->remember("second", second, policy));
  const auto lease = second->cacheLease();
  ASSERT_TRUE(lease);
  scheduler->drain();
  EXPECT_EQ(second->cacheLease(), lease);
  EXPECT_EQ(number("writes"), 2u);
  EXPECT_EQ(number("droppedWrites"), 0u);
}

TEST_F(NativeCacheTest, DeferredCandidatesRespectCancellationAndAllPolicyBarriers)
{
  start();
  auto tile = payload();
  bool wanted = true;
  cache->persist("first", tile, policy);
  cache->persist("cancelled", tile, policy, [&] { return wanted; });
  data->onWrite = [&] { wanted = false; };
  scheduler->drain();
  EXPECT_EQ(number("writes"), 1u);
  EXPECT_EQ(number("droppedWrites"), 1u);
  data->onWrite = {};
  for(unsigned barrier = 0; barrier < 4; ++barrier)
  {
    policy = cache->configure("native", "test-build", true, true);
    cache->resume();
    policy = cache->policy();
    scheduler->drain();
    cache->persist("queued", tile, policy);
    cache->persist("deferred", tile, policy);
    ASSERT_EQ(number("deferredWrites"), 1u);
    switch(barrier)
    {
      case 0: ASSERT_TRUE(cache->clear(false)); break;
      case 1: cache->configure("native", "test-build", false, false); break;
      case 2: cache->trim(); break;
      case 3: cache->configure("native", "replacement", true, true); break;
    }
    scheduler->drain();
    EXPECT_EQ(number("pendingWrites"), 0u);
    EXPECT_EQ(number("deferredWriteBytes"), 0u);
    EXPECT_EQ(number("writes"), 1u);
    EXPECT_EQ(number("droppedWrites"), 3u + 2u * barrier);
  }
}

TEST_F(NativeCacheTest, DeferredPromotionRechecksValidityBeforeCommit)
{
  start();
  auto tile = payload();
  cache->persist("first", tile, policy);
  unsigned checks = 0;
  cache->persist("cancelled", tile, policy, [&] { return ++checks < 3; });
  scheduler->drain();
  EXPECT_EQ(checks, 3u);
  EXPECT_EQ(number("promotedWrites"), 1u);
  EXPECT_EQ(number("writes"), 1u);
  EXPECT_EQ(number("droppedWrites"), 1u);
  EXPECT_EQ(number("pendingWrites"), 0u);
}

TEST_F(NativeCacheTest, BoundsPendingReadsAndReleasesCancelledWaitersExactlyOnce)
{
  config.maxReads = 2;
  start();
  unsigned replies = 0;
  for(size_t i = 0; i < 3; ++i)
    cache->read("missing", metadata(), policy, [] { return false; },
      [&](NativeTilePayloadPtr value) { ++replies; EXPECT_FALSE(value); });
  EXPECT_EQ(replies, 1u);
  EXPECT_EQ(number("pendingReads"), 2u);
  scheduler->drain();
  EXPECT_EQ(replies, 3u);
  EXPECT_EQ(number("pendingReads"), 0u);
  EXPECT_EQ(data->opens, 0u);
}

TEST_F(NativeCacheTest, SharedBudgetBlocksPersistenceAndPreparedReservations)
{
  start();
  const auto available = config.chargedBytes - number("chargedBytes");
  auto pin = cache->reservePrepared(available);
  ASSERT_TRUE(pin);
  EXPECT_FALSE(cache->reservePrepared(1));
  auto tile = payload();
  EXPECT_FALSE(cache->remember("tile", tile, policy));
  cache->persist("tile", tile, policy);
  unsigned replies = 0;
  cache->read("tile", metadata(), policy, {}, [&](NativeTilePayloadPtr value)
    { ++replies; EXPECT_FALSE(value); });
  EXPECT_EQ(replies, 1u);
  scheduler->drain();
  EXPECT_EQ(data->opens, 0u);
  EXPECT_EQ(number("chargedBytes"), config.chargedBytes);
  pin.reset();
  ASSERT_TRUE(cache->remember("tile", tile, policy));
  EXPECT_LE(number("peakBytes"), config.chargedBytes);
}

TEST_F(NativeCacheTest, DecodedLimitReportsIneligibilityAndBoundedSourceEvidence)
{
  config.codec.decodedBytes = 1;
  start();
  const auto tile = payload();
  cache->persist("oversized", tile, policy);
  scheduler->drain();
  EXPECT_EQ(number("writeIneligible"), 1u);
  EXPECT_EQ(number("writeLimitDecodedBytes"), 1u);
  EXPECT_EQ(number("writeLimitStoredBytes"), 0u);
  EXPECT_EQ(number("writeFailures"), 0u);
  EXPECT_EQ(number("storeFailures"), 0u);
  EXPECT_EQ(number("droppedWrites"), 1u);
  EXPECT_EQ(data->writes, 0u);
  rapidjson::Document status;
  status.Parse(cache->snapshotJSON().c_str());
  const auto& limit = status["lastWriteLimit"];
  EXPECT_EQ(limit["reason"].GetUint(), static_cast<unsigned>(NativeTileCodecLimit::DecodedBytes));
  EXPECT_EQ(limit["limit"].GetUint64(), 1u);
  EXPECT_GT(limit["requiredAtLeast"].GetUint64(), 1u);
  EXPECT_EQ(limit["payloadCharge"].GetUint64(), tile->cacheMemoryCharge());
  EXPECT_EQ(limit["z"].GetUint(), tile->metadata().tileID.z);
  EXPECT_EQ(limit["x"].GetUint(), tile->metadata().tileID.x);
  EXPECT_EQ(limit["y"].GetUint(), tile->metadata().tileID.y);
  EXPECT_EQ(limit["generation"].GetUint64(), policy.generation);
}

TEST_F(NativeCacheTest, WireLimitIsNotAWriteFailure)
{
  config.codec.storedBytes = 32;
  start();
  cache->persist("oversized", payload(), policy);
  scheduler->drain();
  EXPECT_EQ(number("writeIneligible"), 1u);
  EXPECT_EQ(number("writeLimitStoredBytes"), 1u);
  EXPECT_EQ(number("writeFailures"), 0u);
  EXPECT_EQ(number("droppedWrites"), 1u);
  EXPECT_EQ(data->writes, 0u);
}

TEST_F(NativeCacheTest, NestingLimitIsNotAWriteFailure)
{
  config.codec.nesting = 0;
  start();
  cache->persist("nested", payload(), policy);
  scheduler->drain();
  EXPECT_EQ(number("writeIneligible"), 1u);
  EXPECT_EQ(number("writeLimitNesting"), 1u);
  EXPECT_EQ(number("writeFailures"), 0u);
  EXPECT_EQ(number("droppedWrites"), 1u);
  EXPECT_EQ(data->writes, 0u);
}

TEST_F(NativeCacheTest, StoreAndAllocationExceptionsRemainFailuresIncludingTypedCodecExceptions)
{
  start();
  for(unsigned kind = 0; kind < 4; ++kind)
  {
    data->onWrite = [kind]
    {
      switch(kind)
      {
        case 0: throw std::runtime_error("Injected database failure");
        case 1: throw NativeTileCodecLimitError(NativeTileCodecLimit::DecodedBytes, 1, 2);
        case 2: throw NativeTileCodecInvalidError();
        default: throw std::bad_alloc();
      }
    };
    cache->persist("failed", payload(), policy);
    scheduler->drain();
    EXPECT_EQ(number("writeFailures"), kind + 1);
    EXPECT_EQ(number("droppedWrites"), kind + 1);
    EXPECT_EQ(number("writeIneligible"), 0u);
  }
  EXPECT_EQ(number("writeStoreFailures"), 3u);
  EXPECT_EQ(number("writeAllocationFailures"), 1u);
  EXPECT_EQ(number("writeCodecInvalidFailures"), 0u);
  EXPECT_EQ(number("writes"), 0u);
  rapidjson::Document status;
  status.Parse(cache->snapshotJSON().c_str());
  EXPECT_TRUE(status["lastWriteLimit"].IsNull());
}

TEST_F(NativeCacheTest, BadRecordsAndStoreExceptionsReturnOneSafeMiss)
{
  start();
  populate();
  auto& record = data->records.begin()->second;
  record.back() ^= 1;
  unsigned replies = 0;
  cache->read("tile", metadata(), policy, {}, [&](NativeTilePayloadPtr value)
    { ++replies; EXPECT_FALSE(value); });
  scheduler->drain();
  EXPECT_EQ(replies, 1u);
  EXPECT_EQ(data->erases, 1u);
  EXPECT_EQ(number("rejected"), 1u);
  populate();
  data->failRead = true;
  cache->read("tile", metadata(), policy, {}, [&](NativeTilePayloadPtr value)
    { ++replies; EXPECT_FALSE(value); throw std::runtime_error("Reply failure"); });
  scheduler->drain();
  EXPECT_EQ(replies, 2u);
  EXPECT_EQ(number("pendingReads"), 0u);
  EXPECT_EQ(data->records.size(), 1u);
}

TEST_F(NativeCacheTest, TrimCancelsChunkHookAndRuntimeReservationsCanRestart)
{
  start(false);
  unsigned chunks = 0, replies = 0;
  auto scratch = cache->reservePrepared(4096);
  ASSERT_TRUE(scratch);
  cache->postBackground(policy, [&, scratch](NativeTileStore* store)
    { EXPECT_EQ(store, nullptr); ++chunks; return true; },
    [&](bool success) { ++replies; EXPECT_FALSE(success); });
  scratch.reset();
  ASSERT_TRUE(scheduler->step());
  EXPECT_EQ(chunks, 1u);
  cache->trim();
  EXPECT_FALSE(cache->reservePrepared(1));
  scheduler->drain();
  EXPECT_EQ(chunks, 1u);
  EXPECT_EQ(replies, 1u);
  cache->resume();
  policy = cache->policy();
  EXPECT_TRUE(cache->reservePrepared(4096));
  cache->postBackground(policy, [&](NativeTileStore* store)
    { EXPECT_EQ(store, nullptr); ++chunks; return false; },
    [&](bool success) { ++replies; EXPECT_TRUE(success); });
  scheduler->drain();
  EXPECT_EQ(chunks, 2u);
  EXPECT_EQ(replies, 2u);
}

TEST_F(NativeCacheTest, DestructionIsDeferredAndRepliesSurviveOwnerTeardown)
{
  start();
  populate();
  const auto closes = data->closes;
  unsigned replies = 0;
  cache->read("tile", metadata(), policy, {}, [&](NativeTilePayloadPtr value)
    { ++replies; EXPECT_FALSE(value); });
  cache.reset();
  EXPECT_EQ(replies, 0u);
  EXPECT_EQ(data->closes, closes);
  scheduler->drain();
  EXPECT_EQ(replies, 1u);
  EXPECT_EQ(data->closes, closes + 1);
}

TEST_F(NativeCacheTest, DisableDuringWriteFinishesOnlyRunningTransactionThenFencesQueuedWrites)
{
  start();
  unsigned calls = 0;
  data->onWrite = [&]
    { ++calls; cache->configure("native", "test-build", false, false); };
  cache->persist("first", payload(), policy);
  cache->persist("second", payload(), policy);
  scheduler->drain();
  EXPECT_EQ(calls, 1u);
  EXPECT_FALSE(cache->enabled());
  EXPECT_EQ(number("pendingWrites"), 0u);
  EXPECT_EQ(data->records.size(), 1u);
  cache->persist("obsolete", payload(), policy);
  scheduler->drain();
  EXPECT_EQ(data->records.size(), 1u);
}

TEST_F(NativeCacheTest, ClearWhileDisabledReleasesStoreBeforeAcknowledgment)
{
  start();
  populate();
  cache->configure("native", "test-build", false, false);
  scheduler->drain();
  const auto baseline = number("chargedBytes");
  bool acknowledged = false;
  cache->clear(true, [&](bool success)
  {
    acknowledged = true;
    EXPECT_TRUE(success);
    EXPECT_TRUE(data->records.empty());
    EXPECT_EQ(data->opens, data->closes);
    EXPECT_EQ(number("chargedBytes"), baseline);
  });
  scheduler->drain();
  EXPECT_TRUE(acknowledged);
}

TEST_F(NativeCacheTest, ClearAdmissionAndFailedCompletionAreDistinctAndReported)
{
  start();
  populate();
  data->failClear = true;
  unsigned replies = 0;
  EXPECT_TRUE(cache->clear(true, [&](bool success) { ++replies; EXPECT_FALSE(success); }));
  EXPECT_EQ(replies, 0u);
  EXPECT_EQ(number("clearRequests"), 1u);
  EXPECT_EQ(number("clearCompleted"), 0u);
  scheduler->drain();
  EXPECT_EQ(replies, 1u);
  EXPECT_EQ(number("clearCompleted"), 1u);
  EXPECT_EQ(number("clearFailures"), 1u);
  EXPECT_EQ(number("pendingControls"), 0u);
  rapidjson::Document status;
  status.Parse(cache->snapshotJSON().c_str());
  EXPECT_FALSE(status["lastClearSuccess"].GetBool());
  data->failClear = false;
  EXPECT_TRUE(cache->clear(true, [&](bool success) { ++replies; EXPECT_TRUE(success); }));
  scheduler->drain();
  EXPECT_EQ(replies, 2u);
  EXPECT_EQ(number("clearCompleted"), 2u);
  EXPECT_EQ(number("clearFailures"), 1u);
  status.Parse(cache->snapshotJSON().c_str());
  EXPECT_TRUE(status["lastClearSuccess"].GetBool());
}

TEST_F(NativeCacheTest, RejectedClearCompletesOnceWithoutChangingGeneration)
{
  start(false);
  unsigned replies = 0;
  for(size_t i = 0; i < 32; ++i)
    ASSERT_TRUE(cache->clear(false, [&](bool) { ++replies; }));
  const auto before = cache->policy().generation;
  EXPECT_FALSE(cache->clear(false, [&](bool success) { ++replies; EXPECT_FALSE(success); }));
  EXPECT_EQ(cache->policy().generation, before);
  EXPECT_EQ(replies, 1u);
  scheduler->drain();
  EXPECT_EQ(replies, 33u);
  EXPECT_EQ(number("clearRequests"), 33u);
  EXPECT_EQ(number("clearCompleted"), 33u);
  EXPECT_EQ(number("clearFailures"), 32u);
  EXPECT_EQ(number("lastClearCompleted"), 33u);
  EXPECT_EQ(number("pendingControls"), 0u);
}

TEST_F(NativeCacheTest, SchedulerFailurePreservesOwnershipFenceAndRejectsAllNewCacheWork)
{
  start();
  populate();
  auto tile = payload();
  ASSERT_TRUE(cache->remember("active", tile, policy));
  auto optional = payload();
  ASSERT_TRUE(cache->remember("optional", optional, policy));
  std::weak_ptr<const NativeTilePayload> weakOptional = optional;
  optional.reset();
  scheduler->drain();
  const auto activeCharge = tile->cacheMemoryCharge();
  const auto openStores = data->opens - data->closes;
  scheduler->rejectSchedule = true;
  unsigned replies = 0;
  cache->read("tile", metadata(), policy, {}, [&](NativeTilePayloadPtr value)
  {
    ++replies;
    EXPECT_FALSE(value);
    cache->trim();
    cache->resume();
    EXPECT_FALSE(cache->remember("reentrant", tile, cache->policy()));
  });
  EXPECT_EQ(replies, 1u);
  EXPECT_TRUE(cache->enabled());
  EXPECT_TRUE(cache->policy().enabled);
  EXPECT_EQ(number("retainedBytes"), 0u);
  EXPECT_EQ(number("entries"), 0u);
  EXPECT_TRUE(weakOptional.expired());
  EXPECT_EQ(data->opens - data->closes, openStores);
  const auto withActive = number("chargedBytes");
  tile.reset();
  EXPECT_EQ(number("chargedBytes"), withActive - activeCharge);
  EXPECT_EQ(number("storeFailures"), 1u);
  EXPECT_EQ(number("pendingReads"), 0u);
  rapidjson::Document status;
  status.Parse(cache->snapshotJSON().c_str());
  EXPECT_TRUE(status["closed"].GetBool());
  EXPECT_FALSE(status["available"].GetBool());
  auto current = cache->policy();
  EXPECT_FALSE(cache->get("active", metadata(), current));
  EXPECT_FALSE(cache->remember("new", payload(), current));
  EXPECT_FALSE(cache->reservePrepared(1));
  cache->persist("new", payload(), current);
  cache->read("new", metadata(), current, {}, [&](NativeTilePayloadPtr value)
    { ++replies; EXPECT_FALSE(value); });
  EXPECT_EQ(replies, 2u);
  EXPECT_THROW(cache->configure("native", "test-build", true, true), std::runtime_error);
  EXPECT_THROW(cache->configure("native", "test-build", false, false), std::runtime_error);
  EXPECT_TRUE(cache->enabled());
  scheduler->rejectSchedule = false;
}

TEST_F(NativeCacheTest, SchedulerFailureReleasesQueuedWriterPinWithoutClosingStoreOnCaller)
{
  start();
  auto tile = payload();
  auto deferred = payload(3);
  std::weak_ptr<const NativeTilePayload> weak = tile;
  std::weak_ptr<const NativeTilePayload> weakDeferred = deferred;
  cache->postBackground(policy, [&, tile, deferred](NativeTileStore*)
  {
    cache->persist("queued", tile, policy);
    cache->persist("deferred", deferred, policy);
    EXPECT_EQ(number("deferredWrites"), 1u);
    scheduler->rejectSchedule = true;
    return false;
  });
  tile.reset();
  deferred.reset();
  ASSERT_TRUE(scheduler->step());
  EXPECT_TRUE(weak.expired());
  EXPECT_TRUE(weakDeferred.expired());
  EXPECT_EQ(number("pendingWrites"), 0u);
  EXPECT_EQ(number("deferredWrites"), 0u);
  EXPECT_EQ(number("deferredWriteBytes"), 0u);
  EXPECT_EQ(number("droppedWrites"), 2u);
  EXPECT_EQ(data->writes, 0u);
  EXPECT_EQ(data->opens, 1u);
  EXPECT_EQ(data->closes, 0u);
  EXPECT_TRUE(cache->enabled());
  scheduler->rejectSchedule = false;
}
