#include <mln/tile/native_tile_cache.hpp>
#include <mln/util/sha256.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace mln
{
namespace
{
std::atomic<uint64_t> nextOwner{1};

uint64_t activityTimeUs()
{
  return std::chrono::duration_cast<std::chrono::microseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::shared_ptr<Scheduler> cacheScheduler()
{
  // The shared pool must not be destroyed by its own last cleanup task.
  static const auto* scheduler = new std::shared_ptr<Scheduler>(Scheduler::GetBackground());
  return *scheduler;
}

size_t addSize(size_t a, size_t b)
{
  if(b > std::numeric_limits<size_t>::max() - a) throw std::length_error("Native cache size overflow");
  return a + b;
}

bool valid(const NativeTileCache::Validity& check) noexcept
{
  try { return !check || check(); }
  catch(...) { return false; }
}

void complete(NativeTileCache::Completion callback, bool success) noexcept
{
  try { if(callback) callback(success); }
  catch(...) {}
}

void integer(std::string& out, uint64_t value, size_t width)
{
  for(size_t i = 0; i < width; ++i) out.push_back(static_cast<char>((value >> (8 * i)) & 255));
}

std::string identity(const std::string& descriptor, const NativeTileMetadata& metadata,
                     const NativeTileCache::Policy& policy, const NativeTileCodecLimits& limits)
{
  if(descriptor.empty() || policy.nameSpace.empty()) return {};
  const auto size = addSize(addSize(descriptor.size(), policy.nameSpace.size()), 47);
  if(size > limits.descriptorBytes) return {};
  std::string result;
  result.reserve(size);
  integer(result, policy.nameSpace.size(), 8);
  result += policy.nameSpace;
  integer(result, descriptor.size(), 8);
  result += descriptor;
  integer(result, metadata.tileID.z, 1);
  integer(result, metadata.tileID.x, 4);
  integer(result, metadata.tileID.y, 4);
  const auto& c = metadata.conversion;
  integer(result, c.conversionRevision, 4);
  integer(result, c.normalizationRevision, 4);
  integer(result, c.extent, 2);
  integer(result, c.buffer, 2);
  integer(result, std::bit_cast<uint64_t>(c.tolerance), 8);
  integer(result, c.clip, 1);
  integer(result, c.wrap, 1);
  return result;
}

std::string jsonQuoted(const std::string& value)
{
  constexpr char hex[] = "0123456789abcdef";
  std::string result = "\"";
  for(unsigned char c : value)
  {
    if(c == '"' || c == '\\') { result += '\\'; result += static_cast<char>(c); }
    else if(c < 32 || c >= 127)
    {
      result += "\\u00";
      result += hex[c >> 4];
      result += hex[c & 15];
    }
    else result += static_cast<char>(c);
  }
  return result + '"';
}
}

struct NativeTileCache::State : std::enable_shared_from_this<State>
{
  struct Waiter
  {
    std::shared_ptr<NativeTileMemoryLease> charge;
    Validity validity;
    OriginReply reply;
    std::atomic<bool> finished{false};

    void finish(NativeTilePayloadPtr payload, bool fromDisk = false) noexcept
    {
      if(finished.exchange(true)) return;
      auto callback = std::move(reply);
      try { if(callback) callback(payload, payload && fromDisk); }
      catch(...) {}
    }
  };

  struct ReadJob
  {
    std::shared_ptr<NativeTileMemoryLease> charge;
    std::string key;
    NativeTileMetadata metadata{CanonicalTileID(0, 0, 0), {}};
    Policy policy;
    std::vector<std::shared_ptr<Waiter>> waiters;
  };

  struct WriteJob
  {
    std::shared_ptr<NativeTileMemoryLease> metadataCharge;
    std::shared_ptr<NativeTileMemoryLease> wireCharge;
    std::string key;
    NativeTilePayloadPtr payload;
    std::weak_ptr<const NativeTilePayload> weakPayload;
    std::weak_ptr<NativeTileMemoryLease> weakCharge;
    Policy policy;
    Validity validity;
    std::string record;
    size_t payloadBytes = 0;
    size_t metadataBytes = 0;
    size_t turns = 0;
    bool persisted = false;
  };

  struct Hook
  {
    std::shared_ptr<NativeTileMemoryLease> charge;
    Policy policy;
    BackgroundWork work;
    Completion completion;
  };

  struct Control
  {
    uint64_t sequence = 0;
    Policy policy;
    bool disk = false;
    Completion completion;
  };

  struct PayloadSample
  {
    uint32_t z = 0, x = 0, y = 0;
    size_t charge = 0, features = 0, propertySets = 0, properties = 0, geometry = 0, records = 0, estimated = 0;
  };

  struct Entry
  {
    std::shared_ptr<NativeTileMemoryLease> metadataCharge;
    std::weak_ptr<const NativeTilePayload> weak;
    NativeTilePayloadPtr retained;
    size_t bytes = 0;
    uint64_t touched = 0;
    uint64_t generation = 0;
    bool keep = false;
    PayloadSample sample;
  };

  enum class WriteStage { Preparation, Codec, Store };

  struct WriteLimitInfo
  {
    bool present = false;
    NativeTileCodecLimit reason = NativeTileCodecLimit::StoredBytes;
    size_t limit = 0, requiredAtLeast = 0, payloadCharge = 0;
    uint32_t z = 0, x = 0, y = 0;
    uint64_t generation = 0;
  };

  State(Config config_, Factory factory_, std::shared_ptr<Scheduler> scheduler_)
    : config(std::move(config_)), factory(std::move(factory_)),
      scheduler(scheduler_ ? scheduler_ : cacheScheduler()), retryDefaultScheduler(!scheduler_),
      budget(config.chargedBytes)
  {
    if(!scheduler || !config.maxReads || config.maxReads > 32 || config.maxWrites != 1
       || config.maxDeferredWrites > 32 || config.deferredWriteBytes > 2 * 1024 * 1024 || !config.store.rows
       || config.store.rows > 4096 || config.retainedBytes > config.chargedBytes
       || !config.codec.descriptorBytes || config.codec.descriptorBytes > 64 * 1024
       || config.codec.storedBytes < 32 || !config.codec.decodedBytes || config.codec.nesting > 64
       || config.store.recordBytes < 32 || config.store.recordBytes > config.store.diskBytes)
      throw std::invalid_argument("Invalid native cache configuration");
    baseCharge = budget.reserve(addSize(72 * 1024, config.store.rows * sizeof(NativeTilePayloadPtr)));
    if(!baseCharge) throw std::invalid_argument("Native cache budget cannot hold its indexes");
    releases.reserve(config.store.rows);
    current.generation = 1;
  }

  bool matches(const Policy& policy, bool disk = false, bool allowGate = false) const
  {
    return !closed && (allowGate || !gate) && current.enabled && policy.enabled
      && policy.generation == current.generation && policy.nameSpace == current.nameSpace
      && (!disk || (policy.diskEnabled && current.diskEnabled));
  }

  bool currentPolicy(const Policy& policy, bool disk = false) const
  {
    std::lock_guard lock(mutex);
    return matches(policy, disk);
  }

  void markIdle()
  {
    for(auto& [key, entry] : entries) entry.keep = false;
    retainedBytes = 0;
  }

  void cleanup()
  {
    {
      std::lock_guard lock(mutex);
      for(auto& [key, entry] : entries)
        if(!entry.keep && entry.retained)
        {
          releaseBytes += entry.bytes;
          releases.push_back(std::move(entry.retained));
        }
    }
    releases.clear();
    std::lock_guard lock(mutex);
    releaseBytes = 0;
    std::erase_if(entries, [&](const auto& item)
    {
      return !item.second.retained && (item.second.weak.expired() || item.second.generation != current.generation);
    });
  }

  bool hasWork() const
  {
    if(resetPending || !controls.empty() || !reads.empty() || !writes.empty() || !writeCandidates.empty() || hook)
      return true;
    return std::any_of(entries.begin(), entries.end(), [](const auto& item)
      { return !item.second.keep && bool(item.second.retained); });
  }

  void kick() noexcept
  {
    const auto self = shared_from_this();
    {
      std::lock_guard lock(mutex);
      if(scheduled || !hasWork()) return;
      scheduled = true;
    }
    try { scheduler->schedule([self] { self->pump(); }); }
    catch(...)
    {
      if(retryDefaultScheduler)
      {
        try { scheduler->schedule([self] { self->pump(); }); return; }
        catch(...) {}
      }
      failPending();
    }
  }

  void closeStore()
  {
    store.reset();
    storeCharge.reset();
    storeEpoch = 0;
    std::lock_guard lock(mutex);
    storeReady = false;
  }

  NativeTileStore* openStore(const Policy& policy, bool force = false)
  {
    std::string requestedPath;
    {
      std::lock_guard lock(mutex);
      if(closed || policy.generation != current.generation || (!force && !matches(policy, true))) return nullptr;
      requestedPath = path;
    }
    if(store && storeEpoch == policy.generation) return store.get();
    if(requestedPath.empty() || !factory || failedStoreEpoch == policy.generation) return nullptr;
    closeStore();
    auto reservation = reserveWorker(addSize(config.store.pageCacheBytes, 64 * 1024));
    if(!reservation) return nullptr;
    try
    {
      auto created = factory(requestedPath, config.store);
      if(!created) throw std::runtime_error("Native tile store is unavailable");
      const auto bytes = created->storedBytes();
      std::lock_guard lock(mutex);
      if(closed || policy.generation != current.generation) return nullptr;
      store = std::move(created);
      storeCharge = std::move(reservation);
      storeEpoch = policy.generation;
      storeBytes = bytes;
      storeReady = true;
      return store.get();
    }
    catch(...)
    {
      failedStoreEpoch = policy.generation;
      std::lock_guard lock(mutex);
      ++storeFailures;
      return nullptr;
    }
  }

  std::shared_ptr<NativeTileMemoryLease> reserveWorker(size_t amount)
  {
    if(auto lease = budget.reserve(amount)) return lease;
    {
      std::lock_guard lock(mutex);
      markIdle();
    }
    cleanup();
    return budget.reserve(amount);
  }

  NativeTilePayloadPtr get(const std::string& key, const NativeTileMetadata& metadata, const Policy& policy)
  {
    NativeTilePayloadPtr payload;
    std::lock_guard lock(mutex);
    if(!matches(policy) || key.empty()) return {};
    const auto found = entries.find(key);
    if(found == entries.end() || found->second.generation != policy.generation) return {};
    payload = found->second.weak.lock();
    if(!payload || payload->metadata() != metadata) return {};
    found->second.touched = ++clock;
    ++memoryHits;
    return payload;
  }

  bool remember(const std::string& key, const NativeTilePayloadPtr& payload, const Policy& policy)
  {
    NativeTilePayloadPtr existing;
    std::string replacementKey = key;
    std::unique_lock lock(mutex);
    if(!payload || key.empty() || !matches(policy)) return false;
    std::erase_if(entries, [](const auto& item) { return !item.second.retained && item.second.weak.expired(); });
    auto found = entries.find(key);
    if(found != entries.end())
    {
      existing = found->second.weak.lock();
      if(existing && existing != payload) return false;
    }
    std::shared_ptr<NativeTileMemoryLease> entryCharge;
    if(found == entries.end())
    {
      entryCharge = budget.reserve(addSize(key.size(), 512));
      if(!entryCharge) { markIdle(); return false; }
      const auto alias = std::find_if(entries.begin(), entries.end(), [&](const auto& item)
        { return !item.second.weak.owner_before(payload) && !payload.owner_before(item.second.weak); });
      if(alias != entries.end())
      {
        auto node = entries.extract(alias);
        node.key() = std::move(replacementKey);
        node.mapped().metadataCharge = std::move(entryCharge);
        found = entries.insert(std::move(node)).position;
      }
      else if(entries.size() >= config.store.rows) return false;
    }
    auto lease = payload->cacheLease();
    if(lease && lease->owner() != budget.identity()) return false;
    if(!lease)
    {
      lease = budget.reserve(payload->cacheMemoryCharge());
      if(!lease) { markIdle(); return false; }
      if(!payload->attachCacheLease(lease)) return false;
    }
    if(found == entries.end())
      found = entries.emplace(key, Entry{std::move(entryCharge), {}, {}, 0, 0, policy.generation, false, {}}).first;
    auto& entry = found->second;
    entry.weak = payload;
    entry.bytes = payload->cacheMemoryCharge();
    entry.touched = ++clock;
    entry.generation = policy.generation;
    const auto& tile = payload->metadata().tileID;
    const auto& stats = payload->statistics();
    entry.sample = {tile.z, tile.x, tile.y, entry.bytes, stats.featureCount, stats.propertySetCount,
                    stats.propertyBytes, stats.geometryBytes, stats.recordBytes, stats.estimatedRetainedBytes};
    if(!suspended && !entry.keep && entry.bytes <= config.retainedBytes)
    {
      while(entry.bytes > config.retainedBytes - retainedBytes)
      {
        auto oldest = entries.end();
        for(auto it = entries.begin(); it != entries.end(); ++it)
          if(it->second.keep && (oldest == entries.end() || it->second.touched < oldest->second.touched)) oldest = it;
        if(oldest == entries.end()) break;
        oldest->second.keep = false;
        retainedBytes -= oldest->second.bytes;
      }
      entry.retained = payload;
      entry.keep = true;
      retainedBytes += entry.bytes;
    }
    return true;
  }

  void finishRead(const std::shared_ptr<ReadJob>& job, NativeTilePayloadPtr payload, bool fromDisk = false)
  {
    std::vector<std::shared_ptr<Waiter>> waiters;
    {
      std::lock_guard lock(mutex);
      waiters = std::move(job->waiters);
      readCount -= waiters.size();
      activeRead.reset();
    }
    bool wanted = false;
    for(const auto& waiter : waiters)
      wanted |= !waiter->finished.load() && valid(waiter->validity);
    if(payload && wanted && currentPolicy(job->policy))
    {
      try
      {
        if(auto existing = get(job->key, job->metadata, job->policy); existing && existing != payload)
        {
          payload = std::move(existing);
          fromDisk = false;
        }
        remember(job->key, payload, job->policy);
      }
      catch(...) {}
    }
    for(const auto& waiter : waiters)
      waiter->finish(payload && currentPolicy(job->policy) && valid(waiter->validity) ? payload : nullptr, fromDisk);
  }

  bool wanted(const std::shared_ptr<ReadJob>& job)
  {
    std::array<std::shared_ptr<Waiter>, 32> waiters;
    size_t count;
    {
      std::lock_guard lock(mutex);
      count = job->waiters.size();
      std::copy(job->waiters.begin(), job->waiters.end(), waiters.begin());
    }
    for(size_t i = 0; i < count; ++i)
      if(!waiters[i]->finished.load() && valid(waiters[i]->validity)) return true;
    return false;
  }

  void runRead(const std::shared_ptr<ReadJob>& job)
  {
    NativeTilePayloadPtr payload;
    bool fromDisk = false;
    try
    {
      const bool needed = wanted(job);
      if(needed && currentPolicy(job->policy)) payload = get(job->key, job->metadata, job->policy);
      if(needed && !payload && currentPolicy(job->policy, true))
      {
        if(auto* database = openStore(job->policy))
        {
          const auto digest = util::SHA256::hash(job->key);
          if(const auto bytes = database->recordSize(digest);
             bytes && *bytes <= config.codec.storedBytes && *bytes <= config.store.recordBytes)
          {
            auto wireCharge = reserveWorker(addSize(addSize(*bytes, *bytes), 256));
            if(wireCharge)
            {
              auto record = database->read(digest, *bytes);
              if(record && wanted(job) && currentPolicy(job->policy, true))
              {
                try
                {
                  const auto size = NativeTileCodec::inspect(*record, config.codec);
                  auto decodedCharge = reserveWorker(size.decodedCharge);
                  if(decodedCharge)
                  {
                    payload = NativeTileCodec::decode(*record, job->key, job->metadata, config.codec);
                    if(payload->cacheMemoryCharge() > decodedCharge->size())
                      throw std::runtime_error("Native decoded payload exceeded its charge");
                    decodedCharge->shrink(payload->cacheMemoryCharge());
                    if(!payload->attachCacheLease(decodedCharge))
                      throw std::runtime_error("Native decoded payload lease could not be attached");
                    fromDisk = true;
                    std::lock_guard lock(mutex);
                    ++diskHits;
                  }
                }
                catch(const std::bad_alloc&)
                {
                  payload.reset();
                }
                catch(const NativeTileCodecCompatibilityError&)
                {
                  payload.reset();
                  database->erase(digest);
                  std::lock_guard lock(mutex);
                  ++incompatibleRecords;
                }
                catch(...)
                {
                  payload.reset();
                  database->erase(digest);
                  std::lock_guard lock(mutex);
                  ++rejected;
                }
              }
            }
          }
        }
      }
    }
    catch(...)
    {
      payload.reset();
      std::lock_guard lock(mutex);
      ++storeFailures;
    }
    if(!payload)
    {
      std::lock_guard lock(mutex);
      ++misses;
    }
    finishRead(job, std::move(payload), fromDisk);
  }

  bool runWrite(const std::shared_ptr<WriteJob>& job)
  {
    if(!currentPolicy(job->policy, true) || !valid(job->validity)) return false;
    {
      std::lock_guard lock(mutex);
      if(suspended) return false;
    }
    WriteStage stage = WriteStage::Preparation;
    WriteLimitInfo limitInfo;
    try
    {
      if(!job->payload && job->record.empty())
      {
        // Hold the existing allocation's credit, or reserve it, before taking a strong payload reference.
        auto lease = job->weakCharge.lock();
        if(!lease) lease = budget.reserve(job->payloadBytes);
        if(!lease) return false;
        auto payload = job->weakPayload.lock();
        if(!payload)
        {
          std::lock_guard lock(mutex);
          ++expiredWrites;
          return false;
        }
        auto attached = payload->cacheLease();
        if(attached)
        {
          if(attached->owner() != budget.identity()) return false;
          lease = std::move(attached);
        }
        else if(!payload->attachCacheLease(lease))
        {
          attached = payload->cacheLease();
          if(!attached || attached->owner() != budget.identity()) return false;
          lease = std::move(attached);
        }
        if(!currentPolicy(job->policy, true) || !valid(job->validity)) return false;
        {
          std::lock_guard lock(mutex);
          if(suspended) return false;
          ++promotedWrites;
        }
        job->payload = std::move(payload);
        job->weakPayload.reset();
        job->weakCharge.reset();
      }
      stage = WriteStage::Store;
      auto* database = openStore(job->policy);
      if(!database) return false;
      if(job->record.empty())
      {
        stage = WriteStage::Codec;
        const auto& tile = job->payload->metadata().tileID;
        limitInfo = {true, NativeTileCodecLimit::StoredBytes, 0, 0, job->payload->cacheMemoryCharge(),
                     tile.z, tile.x, tile.y, job->policy.generation};
        const auto size = NativeTileCodec::measure(*job->payload, job->key, config.codec);
        auto wireCharge = reserveWorker(addSize(addSize(size.storedBytes, size.storedBytes), 256));
        if(!wireCharge) return false;
        job->record = NativeTileCodec::encode(*job->payload, job->key, config.codec);
        job->wireCharge = std::move(wireCharge);
        job->payload.reset();
      }
      if(!currentPolicy(job->policy, true) || !valid(job->validity)) return false;
      stage = WriteStage::Store;
      const bool written = database->write(util::SHA256::hash(job->key), job->record);
      const auto bytes = database->storedBytes();
      std::lock_guard lock(mutex);
      storeBytes = bytes;
      if(written)
      {
        ++writtenCount;
        job->persisted = true;
      }
      else ++maintenanceTurns;
      return !written && ++job->turns <= config.store.rows / 32 + 1 && matches(job->policy, true) && !suspended;
    }
    catch(const NativeTileCodecLimitError& error)
    {
      const auto reason = static_cast<size_t>(error.reason);
      if(stage == WriteStage::Codec && reason < writeLimits.size())
      {
        std::lock_guard lock(mutex);
        ++writeIneligible;
        ++writeLimits[reason];
        limitInfo.reason = error.reason;
        limitInfo.limit = error.limit;
        limitInfo.requiredAtLeast = error.requiredAtLeast;
        lastWriteLimit = limitInfo;
      }
      else recordWriteFailure(stage);
      return false;
    }
    catch(const NativeTileCodecInvalidError&) { recordWriteFailure(stage, true); return false; }
    catch(const std::bad_alloc&) { recordWriteFailure(stage, false, true); return false; }
    catch(...) { recordWriteFailure(stage); return false; }
  }

  void recordWriteFailure(WriteStage stage, bool invalid = false, bool allocation = false)
  {
    std::lock_guard lock(mutex);
    ++writeFailures;
    if(allocation) ++writeAllocationFailures;
    else if(stage == WriteStage::Store) ++writeStoreFailures;
    else if(stage == WriteStage::Codec && invalid) ++writeCodecInvalidFailures;
    else ++writeOtherFailures;
  }

  void runReset()
  {
    std::shared_ptr<Hook> cancelledHook;
    {
      std::lock_guard lock(mutex);
      resetPending = false;
      markIdle();
      if(hook && (closed || hook->policy.generation != current.generation || suspended))
        cancelledHook = std::move(hook);
    }
    closeStore();
    cleanup();
    for(;;)
    {
      std::shared_ptr<ReadJob> job;
      {
        std::lock_guard lock(mutex);
        const auto found = std::find_if(reads.begin(), reads.end(), [&](const auto& item)
          { return closed || item->policy.generation != current.generation; });
        if(found == reads.end()) break;
        job = std::move(*found);
        reads.erase(found);
      }
      finishRead(job, {});
    }
    for(;;)
    {
      std::shared_ptr<WriteJob> job;
      {
        std::lock_guard lock(mutex);
        const auto found = std::find_if(writes.begin(), writes.end(), [&](const auto& item)
          { return closed || item->policy.generation != current.generation; });
        if(found == writes.end()) break;
        job = std::move(*found);
        writes.erase(found);
        ++droppedWrites;
      }
    }
    discardCandidates(false);
    if(cancelledHook)
    {
      cancelledHook->work = {};
      complete(std::move(cancelledHook->completion), false);
    }
    std::lock_guard lock(mutex);
    gate = resetPending || !controls.empty();
  }

  void discardCandidates(bool all)
  {
    for(;;)
    {
      std::shared_ptr<WriteJob> job;
      {
        std::lock_guard lock(mutex);
        const auto found = std::find_if(writeCandidates.begin(), writeCandidates.end(), [&](const auto& item)
          { return all || closed || suspended || item->policy.generation != current.generation; });
        if(found == writeCandidates.end()) break;
        job = std::move(*found);
        writeCandidates.erase(found);
        deferredBytes -= job->metadataBytes;
        ++droppedWrites;
      }
    }
  }

  void finishClear(uint64_t sequence, Completion callback, bool success) noexcept
  {
    {
      std::lock_guard lock(mutex);
      ++clearCompleted;
      if(!success) ++clearFailures;
      if(sequence >= lastClearCompleted)
      {
        lastClearCompleted = sequence;
        lastClearSuccess = success;
      }
    }
    complete(std::move(callback), success);
  }

  void runControl(Control& control)
  {
    bool success = false;
    {
      std::lock_guard lock(mutex);
      success = !closed && control.policy.generation == current.generation;
    }
    if(success && control.disk)
    {
      try
      {
        auto* database = openStore(control.policy, true);
        success = database != nullptr;
        if(database)
        {
          database->clear();
          std::lock_guard lock(mutex);
          storeBytes = 0;
          ++clearCount;
        }
      }
      catch(...) { success = false; }
    }
    bool releaseStore = false;
    {
      std::lock_guard lock(mutex);
      success &= !closed && control.policy.generation == current.generation;
      gate = resetPending || !controls.empty();
      releaseStore = closed || suspended || !current.enabled || !current.diskEnabled;
    }
    if(releaseStore) closeStore();
    finishClear(std::exchange(control.sequence, 0), std::move(control.completion), success);
  }

  void runHook(const std::shared_ptr<Hook>& job)
  {
    bool success = false;
    bool repeat = false;
    if(currentPolicy(job->policy))
    {
      try
      {
        repeat = job->work(job->policy.diskEnabled ? openStore(job->policy) : nullptr);
        success = currentPolicy(job->policy);
      }
      catch(...) {}
    }
    {
      std::lock_guard lock(mutex);
      success = success && !suspended && matches(job->policy);
      if(!success) repeat = false;
      if(!repeat && hook == job) hook.reset();
    }
    if(!repeat)
    {
      job->work = {};
      complete(std::move(job->completion), success);
    }
  }

  void pump()
  {
    enum class Kind { None, Reset, Control, Read, Write, Hook } kind = Kind::None;
    Control control;
    std::shared_ptr<ReadJob> read;
    std::shared_ptr<WriteJob> write;
    std::shared_ptr<Hook> background;
    {
      std::lock_guard lock(mutex);
      if(resetPending) kind = Kind::Reset;
      else if(!controls.empty())
      {
        kind = Kind::Control;
        control = std::move(controls.front());
        controls.pop_front();
      }
      else if(!reads.empty())
      {
        kind = Kind::Read;
        read = std::move(reads.front());
        reads.pop_front();
        activeRead = read;
      }
      else if(!writes.empty())
      {
        kind = Kind::Write;
        write = std::move(writes.front());
        writes.pop_front();
        activeWrite = write;
      }
      else if(!writeCandidates.empty())
      {
        kind = Kind::Write;
        write = std::move(writeCandidates.front());
        writeCandidates.pop_front();
        deferredBytes -= write->metadataBytes;
        activeWrite = write;
      }
      else if(hook)
      {
        kind = Kind::Hook;
        background = hook;
      }
    }
    try
    {
      cleanup();
      switch(kind)
      {
        case Kind::Reset: runReset(); break;
        case Kind::Control: runControl(control); break;
        case Kind::Read: runRead(read); break;
        case Kind::Write:
        {
          const bool repeat = runWrite(write);
          std::lock_guard lock(mutex);
          activeWrite.reset();
          if(repeat) writes.push_back(write);
          else
          {
            ++completedWrites;
            if(!write->persisted) ++droppedWrites;
          }
          break;
        }
        case Kind::Hook: runHook(background); break;
        case Kind::None: break;
      }
      cleanup();
    }
    catch(...)
    {
      if(read) finishRead(read, {});
      if(control.sequence) finishClear(control.sequence, std::move(control.completion), false);
      if(background)
      {
        background->work = {};
        complete(std::move(background->completion), false);
      }
      std::lock_guard lock(mutex);
      if(write && !write->persisted) ++droppedWrites;
      activeWrite.reset();
      if(hook == background) hook.reset();
      ++storeFailures;
    }
    read.reset();
    write.reset();
    background.reset();
    {
      std::lock_guard lock(mutex);
      scheduled = false;
    }
    kick();
  }

  void failPending() noexcept
  {
    {
      std::lock_guard lock(mutex);
      closed = true;
      // Failure must not give independent loader or renderer caches permission to retain tiles.
      ++current.generation;
      ++storeFailures;
      markIdle();
    }
    // The scheduler cannot accept cleanup. Release optional CPU owners here, without touching SQLite.
    cleanup();
    for(;;)
    {
      std::shared_ptr<WriteJob> job;
      {
        std::lock_guard lock(mutex);
        if(writes.empty()) break;
        job = std::move(writes.front());
        writes.pop_front();
        ++droppedWrites;
      }
    }
    discardCandidates(true);
    for(;;)
    {
      std::shared_ptr<ReadJob> job;
      {
        std::lock_guard lock(mutex);
        if(reads.empty()) break;
        job = std::move(reads.front());
        reads.pop_front();
      }
      finishRead(job, {});
    }
    for(;;)
    {
      uint64_t sequence = 0;
      Completion callback;
      {
        std::lock_guard lock(mutex);
        if(controls.empty()) break;
        sequence = controls.front().sequence;
        callback = std::move(controls.front().completion);
        controls.pop_front();
      }
      finishClear(sequence, std::move(callback), false);
    }
    std::shared_ptr<Hook> job;
    {
      std::lock_guard lock(mutex);
      job = std::move(hook);
    }
    if(job)
    {
      job->work = {};
      complete(std::move(job->completion), false);
    }
    std::lock_guard lock(mutex);
    resetPending = false;
    gate = false;
    scheduled = false;
  }

  const Config config;
  const Factory factory;
  const std::shared_ptr<Scheduler> scheduler;
  const bool retryDefaultScheduler;
  NativeTileMemoryBudget budget;
  std::shared_ptr<NativeTileMemoryLease> baseCharge;
  mutable std::mutex mutex;
  Policy current;
  std::atomic<bool> enabledFlag{false};
  std::string path;
  bool closed = false, suspended = false, gate = false, resetPending = false, scheduled = false;
  std::map<std::string, Entry> entries;
  std::vector<NativeTilePayloadPtr> releases;
  size_t retainedBytes = 0, readCount = 0, releaseBytes = 0;
  uint64_t clock = 0;
  std::deque<Control> controls;
  std::deque<std::shared_ptr<ReadJob>> reads;
  std::deque<std::shared_ptr<WriteJob>> writes;
  std::deque<std::shared_ptr<WriteJob>> writeCandidates;
  size_t deferredBytes = 0;
  std::shared_ptr<ReadJob> activeRead;
  std::shared_ptr<WriteJob> activeWrite;
  std::shared_ptr<Hook> hook;
  std::shared_ptr<NativeTileMemoryLease> storeCharge;
  std::unique_ptr<NativeTileStore> store;
  uint64_t storeEpoch = 0, failedStoreEpoch = 0, storeBytes = 0;
  bool storeReady = false;
  const uint64_t ownerID = nextOwner.fetch_add(1);
  const uint64_t createdUs = activityTimeUs();
  uint64_t extractionStarted = 0, extractionCompleted = 0, extractionAborted = 0;
  uint64_t memoryHits = 0, diskHits = 0, misses = 0, rejected = 0, writtenCount = 0;
  uint64_t incompatibleRecords = 0;
  uint64_t writeFailures = 0, storeFailures = 0, droppedWrites = 0, completedWrites = 0;
  uint64_t writeIneligible = 0, writeCodecInvalidFailures = 0, writeAllocationFailures = 0;
  uint64_t writeStoreFailures = 0, writeOtherFailures = 0;
  std::array<uint64_t, 4> writeLimits{};
  WriteLimitInfo lastWriteLimit;
  uint64_t writeDeferrals = 0, promotedWrites = 0, expiredWrites = 0;
  uint64_t maintenanceTurns = 0, clearCount = 0;
  uint64_t clearRequests = 0, clearCompleted = 0, clearFailures = 0, lastClearCompleted = 0;
  bool lastClearSuccess = false;
};

NativeTileCache::NativeTileCache(Config config, Factory factory, std::shared_ptr<Scheduler> scheduler)
  : state(std::make_shared<State>(std::move(config), std::move(factory), std::move(scheduler))) {}

NativeTileCache::~NativeTileCache()
{
  {
    std::lock_guard lock(state->mutex);
    state->closed = true;
    state->current.enabled = false;
    state->enabledFlag.store(false);
    state->current.diskEnabled = false;
    ++state->current.generation;
    state->resetPending = true;
    state->gate = true;
  }
  state->kick();
}

NativeTileCache::Policy NativeTileCache::configure(std::string path, std::string nameSpace,
                                                bool enabled, bool diskEnabled)
{
  if(path.size() > 4096 || nameSpace.size() > 1024 || (enabled && nameSpace.empty()))
    throw std::invalid_argument("Invalid native cache path or namespace");
  Policy result{0, enabled, diskEnabled, std::move(nameSpace)};
  Policy next = result;
  {
    std::lock_guard lock(state->mutex);
    if(state->closed) throw std::runtime_error("Native tile cache is closed");
    if(state->path != path || state->current.nameSpace != result.nameSpace || state->current.enabled != enabled
       || state->current.diskEnabled != diskEnabled)
    {
      next.generation = state->current.generation + 1;
      state->path = std::move(path);
      state->current = std::move(next);
      state->enabledFlag.store(enabled);
      state->resetPending = true;
      state->gate = true;
    }
    result.generation = state->current.generation;
  }
  state->kick();
  return result;
}

NativeTileCache::Policy NativeTileCache::policy() const
{
  std::lock_guard lock(state->mutex);
  return state->current;
}

bool NativeTileCache::enabled() const noexcept
{
  return state->enabledFlag.load();
}

NativeTilePayloadPtr NativeTileCache::get(const std::string& descriptor, const NativeTileMetadata& metadata,
                                        const Policy& policy)
{
  try { return state->get(identity(descriptor, metadata, policy, state->config.codec), metadata, policy); }
  catch(...) { return {}; }
}

bool NativeTileCache::remember(const std::string& descriptor, NativeTilePayloadPtr payload, const Policy& policy)
{
  try
  {
    if(!payload) return false;
    const bool result = state->remember(identity(descriptor, payload->metadata(), policy, state->config.codec),
                                        payload, policy);
    state->kick();
    return result;
  }
  catch(...) { return false; }
}

void NativeTileCache::read(std::string descriptor, NativeTileMetadata metadata, Policy policy,
                          Validity validity, Reply reply)
{
  OriginReply adapted;
  try
  {
    adapted = [reply](NativeTilePayloadPtr payload, bool) { if(reply) reply(std::move(payload)); };
  }
  catch(...)
  {
    try { if(reply) reply({}); } catch(...) {}
    return;
  }
  readWithOrigin(std::move(descriptor), std::move(metadata), std::move(policy),
                 std::move(validity), std::move(adapted));
}

void NativeTileCache::readWithOrigin(std::string descriptor, NativeTileMetadata metadata, Policy policy,
                                    Validity validity, OriginReply reply)
{
  std::shared_ptr<State::Waiter> waiter;
  try
  {
    waiter = std::make_shared<State::Waiter>();
    waiter->validity = std::move(validity);
    waiter->reply = std::move(reply);
    auto key = identity(descriptor, metadata, policy, state->config.codec);
    bool admitted = false;
    {
      std::lock_guard lock(state->mutex);
      if(!key.empty() && state->matches(policy, false, true) && state->readCount < state->config.maxReads)
      {
        waiter->charge = state->budget.reserve(1024);
        if(waiter->charge)
        {
          auto job = state->activeRead;
          if(!job || job->key != key || job->policy.generation != policy.generation)
          {
            job.reset();
            for(const auto& queued : state->reads)
              if(queued->key == key && queued->policy.generation == policy.generation) { job = queued; break; }
          }
          if(!job)
          {
            auto charge = state->budget.reserve(addSize(key.size(), 2048));
            if(!charge)
            {
              waiter->charge.reset();
            }
            else
            {
              job = std::make_shared<State::ReadJob>();
              job->charge = std::move(charge);
              job->key = std::move(key);
              job->metadata = std::move(metadata);
              job->policy = std::move(policy);
              job->waiters.reserve(state->config.maxReads);
              state->reads.push_back(job);
            }
          }
          if(job)
          {
            job->waiters.push_back(waiter);
            ++state->readCount;
            admitted = true;
          }
        }
      }
    }
    if(admitted) state->kick();
    else waiter->finish({});
  }
  catch(...)
  {
    if(waiter) waiter->finish({});
    else { try { if(reply) reply({}, false); } catch(...) {} }
  }
}

void NativeTileCache::persist(std::string descriptor, NativeTilePayloadPtr payload, Policy policy, Validity validity)
{
  try
  {
    if(!payload) return;
    auto key = identity(descriptor, payload->metadata(), policy, state->config.codec);
    auto job = std::make_shared<State::WriteJob>();
    job->key = std::move(key);
    job->policy = std::move(policy);
    job->validity = std::move(validity);
    {
      std::lock_guard lock(state->mutex);
      if(job->key.empty() || !state->matches(job->policy, true, true) || state->suspended)
      {
        ++state->droppedWrites;
        return;
      }
      if(state->activeWrite && state->activeWrite->key == job->key
         && state->activeWrite->policy.generation == job->policy.generation) return;
      for(const auto& queued : state->writes)
        if(queued->key == job->key && queued->policy.generation == job->policy.generation) return;
      for(const auto& queued : state->writeCandidates)
        if(queued->key == job->key && queued->policy.generation == job->policy.generation) return;
      const bool defer = !state->writeCandidates.empty()
        || state->writes.size() + bool(state->activeWrite) >= state->config.maxWrites;
      job->metadataBytes = addSize(addSize(job->key.capacity(), job->policy.nameSpace.capacity()), 1024);
      if(defer && (state->writeCandidates.size() >= state->config.maxDeferredWrites
                   || job->metadataBytes > state->config.deferredWriteBytes - state->deferredBytes))
      {
        ++state->droppedWrites;
        return;
      }
      job->metadataCharge = state->budget.reserve(job->metadataBytes);
      if(!job->metadataCharge) { ++state->droppedWrites; return; }
      auto lease = payload->cacheLease();
      if(lease && lease->owner() != state->budget.identity()) { ++state->droppedWrites; return; }
      if(defer)
      {
        job->weakPayload = payload;
        job->weakCharge = lease;
        job->payloadBytes = payload->cacheMemoryCharge();
        state->writeCandidates.push_back(job);
        state->deferredBytes += job->metadataBytes;
        ++state->writeDeferrals;
      }
      else
      {
        if(!lease)
        {
          lease = state->budget.reserve(payload->cacheMemoryCharge());
          if(!lease || !payload->attachCacheLease(lease)) { ++state->droppedWrites; return; }
        }
        job->payload = std::move(payload);
        state->writes.push_back(job);
      }
    }
    state->kick();
  }
  catch(...)
  {
    std::lock_guard lock(state->mutex);
    ++state->droppedWrites;
  }
}

std::shared_ptr<NativeTileMemoryLease> NativeTileCache::reservePrepared(size_t bytes)
{
  try
  {
    std::lock_guard lock(state->mutex);
    if(!bytes || state->closed || !state->current.enabled || state->suspended) return {};
    return state->budget.reserve(bytes);
  }
  catch(...) { return {}; }
}

bool NativeTileCache::clear(bool disk, Completion completion)
{
  bool queued = false;
  uint64_t sequence = 0;
  State::Control control;
  try
  {
    std::lock_guard lock(state->mutex);
    sequence = ++state->clearRequests;
    if(!state->closed && state->controls.size() < 32)
    {
      control.sequence = sequence;
      control.policy = state->current;
      ++control.policy.generation;
      control.disk = disk;
      state->controls.push_back(std::move(control));
      state->controls.back().completion = std::move(completion);
      ++state->current.generation;
      state->resetPending = true;
      state->gate = true;
      queued = true;
    }
  }
  catch(...) {}
  if(queued) state->kick();
  else state->finishClear(sequence, std::move(completion), false);
  return queued;
}

void NativeTileCache::trim()
{
  {
    std::lock_guard lock(state->mutex);
    if(state->closed) return;
    state->suspended = true;
    ++state->current.generation;
    state->resetPending = true;
    state->gate = true;
  }
  state->kick();
}

void NativeTileCache::resume()
{
  {
    std::lock_guard lock(state->mutex);
    if(state->closed || !state->suspended) return;
    state->suspended = false;
    ++state->current.generation;
    state->resetPending = true;
    state->gate = true;
  }
  state->kick();
}

void NativeTileCache::postBackground(Policy policy, BackgroundWork work, Completion completion)
{
  bool queued = false;
  std::shared_ptr<State::Hook> job;
  try
  {
    job = std::make_shared<State::Hook>();
    std::lock_guard lock(state->mutex);
    if(work && state->matches(policy, false, true) && !state->suspended && !state->hook)
    {
      auto charge = state->budget.reserve(addSize(policy.nameSpace.size(), 1024));
      if(charge)
      {
        job->policy = std::move(policy);
        job->work = std::move(work);
        job->completion = std::move(completion);
        job->charge = std::move(charge);
        state->hook = job;
        queued = true;
      }
    }
  }
  catch(...) {}
  if(queued) state->kick();
  else
  {
    if(job)
    {
      job->work = {};
      if(job->completion) complete(std::move(job->completion), false);
    }
    work = {};
    complete(std::move(completion), false);
  }
}

NativeTileCache::Extraction::Extraction(std::shared_ptr<State> value) : state(std::move(value))
{
  std::lock_guard lock(state->mutex);
  ++state->extractionStarted;
}

NativeTileCache::Extraction::~Extraction()
{
  finish(false);
}

void NativeTileCache::Extraction::finish(bool success)
{
  auto active = std::move(state);
  if(!active) return;
  std::lock_guard lock(active->mutex);
  if(success) ++active->extractionCompleted;
  else ++active->extractionAborted;
}

void NativeTileCache::Extraction::complete()
{
  finish(true);
}

NativeTileCache::Extraction NativeTileCache::beginExtraction()
{
  return Extraction(state);
}

std::string NativeTileCache::snapshotJSON() const
{
  std::lock_guard lock(state->mutex);
  const auto memory = state->budget.snapshot();
  size_t pendingReleaseBytes = state->releaseBytes;
  for(const auto& [key, entry] : state->entries)
    if(!entry.keep && entry.retained) pendingReleaseBytes += entry.bytes;
  std::ostringstream out;
  out << "{\"version\":1,\"owner\":\"maplibre-native\",\"generation\":" << state->current.generation
      << ",\"activityScope\":\"owner-lifetime\",\"ownerID\":\"" << state->ownerID << '"'
      << ",\"createdUs\":" << state->createdUs << ",\"sampledUs\":" << activityTimeUs()
      << ",\"extractionStarted\":" << state->extractionStarted
      << ",\"extractionCompleted\":" << state->extractionCompleted
      << ",\"extractionAborted\":" << state->extractionAborted
      << ",\"extractionInFlight\":" << state->extractionStarted - state->extractionCompleted - state->extractionAborted
      << ",\"enabled\":" << (state->current.enabled ? "true" : "false")
      << ",\"available\":" << (state->closed ? "false" : "true")
      << ",\"closed\":" << (state->closed ? "true" : "false")
      << ",\"diskEnabled\":" << (state->current.diskEnabled ? "true" : "false")
      << ",\"suspended\":" << (state->suspended ? "true" : "false")
      << ",\"namespace\":" << jsonQuoted(state->current.nameSpace)
      << ",\"capacityBytes\":" << memory.capacity << ",\"usedBytes\":" << memory.used
      << ",\"chargedLimitBytes\":" << memory.capacity << ",\"chargedBytes\":" << memory.used
      << ",\"peakBytes\":" << memory.peak << ",\"budgetDenied\":" << memory.denied
      << ",\"retainedBytes\":" << state->retainedBytes
      << ",\"retainedLimitBytes\":" << state->config.retainedBytes
      << ",\"entries\":" << state->entries.size() << ",\"pendingReads\":" << state->readCount
      << ",\"pendingWrites\":" << state->writes.size() + bool(state->activeWrite) + state->writeCandidates.size()
      << ",\"queuedWrites\":" << state->writes.size()
      << ",\"activeWrites\":" << (state->activeWrite ? 1 : 0)
      << ",\"writeLimit\":" << state->config.maxWrites
      << ",\"deferredWrites\":" << state->writeCandidates.size()
      << ",\"deferredWriteBytes\":" << state->deferredBytes
      << ",\"deferredWriteLimit\":" << state->config.maxDeferredWrites
      << ",\"deferredWriteLimitBytes\":" << state->config.deferredWriteBytes
      << ",\"writeDeferrals\":" << state->writeDeferrals << ",\"promotedWrites\":" << state->promotedWrites
      << ",\"expiredWrites\":" << state->expiredWrites
      << ",\"pendingControls\":" << state->controls.size() + state->resetPending + state->gate
      << ",\"pendingReleaseBytes\":" << pendingReleaseBytes
      << ",\"backgroundPending\":" << (state->hook ? "true" : "false")
      << ",\"running\":" << (state->scheduled ? "true" : "false")
      << ",\"storeReady\":" << (state->storeReady ? "true" : "false")
      << ",\"storeBytes\":" << state->storeBytes << ",\"memoryHits\":" << state->memoryHits
      << ",\"diskHits\":" << state->diskHits << ",\"misses\":" << state->misses
      << ",\"rejected\":" << state->rejected << ",\"writes\":" << state->writtenCount
      << ",\"incompatibleRecords\":" << state->incompatibleRecords
      << ",\"writeFailures\":" << state->writeFailures << ",\"storeFailures\":" << state->storeFailures
      << ",\"writeIneligible\":" << state->writeIneligible
      << ",\"writeLimitStoredBytes\":" << state->writeLimits[0]
      << ",\"writeLimitDecodedBytes\":" << state->writeLimits[1]
      << ",\"writeLimitDescriptorBytes\":" << state->writeLimits[2]
      << ",\"writeLimitNesting\":" << state->writeLimits[3]
      << ",\"writeCodecInvalidFailures\":" << state->writeCodecInvalidFailures
      << ",\"writeAllocationFailures\":" << state->writeAllocationFailures
      << ",\"writeStoreFailures\":" << state->writeStoreFailures
      << ",\"writeOtherFailures\":" << state->writeOtherFailures
      << ",\"droppedWrites\":" << state->droppedWrites << ",\"maintenanceTurns\":" << state->maintenanceTurns
      << ",\"clears\":" << state->clearCount
      << ",\"clearRequests\":" << state->clearRequests << ",\"clearCompleted\":" << state->clearCompleted
      << ",\"clearFailures\":" << state->clearFailures << ",\"lastClearRequest\":" << state->clearRequests
      << ",\"lastClearCompleted\":" << state->lastClearCompleted << ",\"lastClearSuccess\":";
  if(!state->clearRequests || state->lastClearCompleted != state->clearRequests) out << "null";
  else out << (state->lastClearSuccess ? "true" : "false");
  out << ",\"lastWriteLimit\":";
  if(!state->lastWriteLimit.present) out << "null";
  else
  {
    const auto& value = state->lastWriteLimit;
    out << "{\"reason\":" << static_cast<unsigned>(value.reason) << ",\"limit\":" << value.limit
        << ",\"requiredAtLeast\":" << value.requiredAtLeast << ",\"payloadCharge\":" << value.payloadCharge
        << ",\"z\":" << value.z << ",\"x\":" << value.x << ",\"y\":" << value.y
        << ",\"generation\":" << value.generation << '}';
  }
  out << ",\"payloadSample\":[";
  size_t visited = 0, sampled = 0;
  for(const auto& [key, entry] : state->entries)
  {
    if(visited++ == 16) break;
    if(entry.weak.expired()) continue;
    if(sampled++) out << ',';
    const auto& value = entry.sample;
    out << "{\"z\":" << value.z << ",\"x\":" << value.x << ",\"y\":" << value.y
        << ",\"chargeBytes\":" << value.charge
        << ",\"retained\":" << (entry.keep ? "true" : "false")
        << ",\"features\":" << value.features << ",\"propertySets\":" << value.propertySets
        << ",\"propertyBytes\":" << value.properties << ",\"geometryBytes\":" << value.geometry
        << ",\"recordBytes\":" << value.records << ",\"estimatedBytes\":" << value.estimated << '}';
  }
  out << "],\"payloadSampleTruncated\":" << (state->entries.size() > 16 ? "true" : "false") << '}';
  return out.str();
}

} // namespace mln
