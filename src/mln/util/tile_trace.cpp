#include <mln/util/tile_trace.hpp>

#include <algorithm>
#include <atomic>
#include <bitset>
#include <chrono>
#include <mutex>
#include <sstream>
#include <tuple>

#ifndef TILE_TRACE_TEST_HOOK
#define TILE_TRACE_TEST_HOOK(point) ((void)0)
#endif
#ifndef TILE_TRACE_TEST_COUNT
#define TILE_TRACE_TEST_COUNT(point, value) ((void)0)
#endif

namespace mln::tiletrace {
namespace {
constexpr size_t Capacity = 1024;
constexpr size_t StartupCapacity = 64;
constexpr size_t FrameCapacity = 512;
struct Record
{
  Context context;
  ID frame = 0, surface = 0, drawRequirement = 0;
  uint64_t drawViewSerial = 0;
  ID drawOrder = 0;
  uint32_t failedSwaps = 0;
  int mode = -1;
  bool symbol = false;
  bool historyLost = false;
  bool beforeFirstDraw = false, firstUseComplete = false;
  uint64_t lossBaseline = 0;
  std::array<uint64_t, 2> contributionDraw{}, contributionSubmit{}, contributionGeneration{};
  uint64_t submittedDraw = 0;
};
struct Activity
{
  ID id = 0, map = 0;
  uint64_t time = 0;
  int type = 0, value = 0;
};
constexpr size_t BatchCapacity = 16;
constexpr size_t BatchMemberCapacity = 256;
struct ScreenUse
{
  ViewTile tile;
  ID generation = 0, frame = 0, requirement = 0, failureOrder = 0, drawOrder = 0;
  uint64_t submittedUs = 0, drawUs = 0, leftUs = 0, failedUs = 0;
  Outcome failure = Outcome::Pending;
  bool noDraw = false, exact = false, failed = false, unknown = false, originalFailed = false;
};
struct BatchMember
{
  ID publication = 0, map = 0, source = 0, view = 0;
  std::array<ID, 8> demands{};
  std::array<ScreenUse, 8> uses{};
  size_t useCount = 0;
  uint64_t lossBaseline = 0, viewGap = 0, cachedUs = 0, cacheEventUs = 0;
  uint64_t retiredUs = 0, failedUs = 0, admittedUs = 0;
  ID retirementOrder = 0, failureOrder = 0;
  Retirement retirement = Retirement::None;
  Outcome failure = Outcome::Pending;
  ViewTile tile;
  bool viewKnown = false, useOverflow = false;
  bool publicationFailed = false, mlFailed = false, cacheBypassed = false, cacheExact = false;
};
struct View
{
  ID session = 0, map = 0, source = 0, id = 0, style = 0, leftOrder = 0;
  uint64_t gap = 0, captureGeneration = 0;
  size_t count = 0;
  std::array<ViewTile, ViewTileLimit> tiles{};
  std::array<ID, ViewTileLimit> requirements{}, invalidations{};
  std::array<uint64_t, ViewTileLimit> leftTimes{};
  bool retired = false;
};
struct Batch
{
  ID session = 0, id = 0;
  size_t count = 0;
  bool dirty = false;
  std::array<BatchMember, BatchMemberCapacity> members{};
};
struct RecordIndex
{
  static constexpr size_t BucketCount = 2048;
  static_assert(Capacity < UINT16_MAX && BucketCount < UINT16_MAX);
  std::array<uint16_t, BucketCount> heads{};
  std::array<uint16_t, Capacity> next{}, previous{}, buckets{};

  static size_t bucket(ID session, ID publication)
  {
    auto hash = publication ^ session;
    hash ^= hash >> 30;
    hash *= UINT64_C(0xbf58476d1ce4e5b9);
    hash ^= hash >> 27;
    hash *= UINT64_C(0x94d049bb133111eb);
    return (hash ^ (hash >> 31)) & (BucketCount - 1);
  }
  void clear()
  {
    heads.fill(0); next.fill(0); previous.fill(0); buckets.fill(0);
  }
  void remove(size_t slot)
  {
    if (!buckets[slot]) return;
    TILE_TRACE_TEST_HOOK("record_relink");
    auto& head = heads[buckets[slot] - 1];
    if (head == slot + 1)
    {
      head = next[slot];
      if (head) previous[head - 1] = previous[slot];
    }
    else
    {
      next[previous[slot] - 1] = next[slot];
      if (next[slot]) previous[next[slot] - 1] = previous[slot];
      else previous[head - 1] = previous[slot];
    }
    next[slot] = previous[slot] = buckets[slot] = 0;
  }
  void update(size_t slot, const Context& context)
  {
    const auto target = context.id && (context.kind == Kind::Demand || context.kind == Kind::Publication)
      ? static_cast<uint16_t>(bucket(context.session, context.publication) + 1) : uint16_t{0};
    if (buckets[slot] == target) return;
    remove(slot);
    if (!target) return;
    TILE_TRACE_TEST_HOOK("record_relink");
    const auto link = static_cast<uint16_t>(slot + 1);
    auto& head = heads[target - 1];
    buckets[slot] = target;
    if (!head)
    {
      head = link; previous[slot] = link;
      return;
    }
    // The head's previous link holds the tail, so ordered appends do not scan the bucket.
    const auto tail = previous[head - 1];
    if (link < head)
    {
      next[slot] = head; previous[slot] = tail; previous[head - 1] = link; head = link;
    }
    else if (link > tail)
    {
      next[tail - 1] = link; previous[slot] = tail; previous[head - 1] = link;
    }
    else
    {
      auto predecessor = head;
      while (next[predecessor - 1] < link) predecessor = next[predecessor - 1];
      const auto successor = next[predecessor - 1];
      next[slot] = successor; previous[slot] = predecessor;
      next[predecessor - 1] = link; previous[successor - 1] = link;
    }
  }
};
struct Collector
{
  std::mutex mutex, batchMutex, snapshotMutex;
  struct Published
  {
    std::atomic<int> readers{0};
    Batch batch;
  };
  std::array<std::atomic<size_t>, BatchCapacity> publishedIndex{};
  std::array<size_t, 2> publishedSpare{{BatchCapacity, BatchCapacity + 1}};
  std::atomic<ID> serial{1};
  std::atomic<ID> epoch{static_cast<ID>(std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count())};
  std::atomic<bool> capture{true};
  std::atomic<uint64_t> captureGeneration{1};
  std::atomic<ID> viewLossStart{0};
  std::atomic<uint64_t> lost{0}, gaps{0}, viewChanges{1};
  std::atomic<uint64_t> memoryStatsTime{0}, diskStatsTime{0};
  uint64_t evicted = 0, started = 0;
  ID retiredThrough = 0;
  std::array<Record, Capacity> records{};
  RecordIndex recordIndex;
  std::array<Record, StartupCapacity> startup{};
  size_t startupCount = 0;
  std::array<Activity, 128> activity{};
  size_t activityIndex = 0;
  struct DemandOutcome { ID id = 0; Outcome outcome = Outcome::Pending; };
  std::array<DemandOutcome, Capacity> demandOutcomes{};
  ID retiredDemandThrough = 0;
  std::array<Batch, BatchCapacity> batches{};
  size_t nextBatch = 0;
  std::array<View, 16> views{};
  size_t nextView = 0;
  std::array<View, 32> viewHistory{};
  size_t nextViewHistory = 0;
  std::array<std::pair<uintptr_t, ID>, 16> surfaces{};
  std::array<Published, BatchCapacity + 2> published{};
  Collector()
  {
    for (size_t i = 0; i < publishedIndex.size(); ++i) publishedIndex[i].store(i);
  }
};
Collector& collector()
{
  static Collector value;
  return value;
}
enum class LostEvent { Cache, Screen, Outcome, View };
struct EventBoundary
{
  uint64_t time;
  ID order;
};
EventBoundary eventBoundary() { return {now(), nextID()}; }
bool before(EventBoundary a, EventBoundary b)
{
  return std::tie(a.time, a.order) < std::tie(b.time, b.order);
}
bool earlierBoundary(EventBoundary evidence, uint64_t time, ID order)
{
  return !time || before(evidence, {time, order});
}
struct CaptureLoss
{
  std::atomic<bool> ready{false};
  ID publication = 0, source = 0;
  union
  {
    ID viewOrder = 0;
    uint64_t eligibilityUs;
  } boundary;
  uint64_t timestamp = 0;
  LostEvent kind = LostEvent::Screen;
};
struct LossBank
{
  std::atomic<int> readers{0};
  ID epoch = 0;
  std::atomic<size_t> count{0};
  std::atomic<size_t> writers{0};
  std::atomic<uint64_t> overflow{0}, overflowEligibility{0};
  std::array<CaptureLoss, 8192> events{};
  struct Source { std::atomic<ID> key{0}, view{0}; };
  std::array<Source, 512> sources{};
};
std::array<LossBank, 4> lossBanks;
std::atomic<size_t> currentLossBank{0};
std::atomic<ID> unbankedLossEpoch{0};
void firstTime(std::atomic<uint64_t>& slot, uint64_t timestamp)
{
  auto old = slot.load();
  while ((!old || timestamp < old) && !slot.compare_exchange_weak(old, timestamp)) {}
}
void unbankedLoss(ID epoch)
{
  auto old = unbankedLossEpoch.load();
  while (old < epoch && !unbankedLossEpoch.compare_exchange_weak(old, epoch)) {}
}
// Pins keep reset from reusing storage while an event or snapshot still uses it.
struct LossLease
{
  LossBank* bank = nullptr;
  size_t cutoff = 0, count = 0;
  uint64_t overflow = 0, overflowEligibility = 0;
  explicit LossLease(ID epoch)
  {
    for (size_t attempt = 0; attempt < 16; ++attempt)
    {
      const auto index = currentLossBank.load();
      auto& candidate = lossBanks[index];
      auto readers = candidate.readers.load();
      if (readers < 0 || !candidate.readers.compare_exchange_strong(readers, readers + 1)) continue;
      if (currentLossBank.load() == index && candidate.epoch == epoch) { bank = &candidate; return; }
      --candidate.readers;
      if (currentLossBank.load() == index) return;
    }
  }
  ~LossLease() { if (bank) --bank->readers; }
  bool capture()
  {
    if (!bank) return true;
    count = bank->count.load();
    if (bank->writers.load()) return false;
    cutoff = std::min(count, bank->events.size());
    for (size_t i = 0; i < cutoff; ++i)
      if (!bank->events[i].ready.load()) return false;
    overflow = bank->overflow.load();
    overflowEligibility = bank->overflowEligibility.load();
    TILE_TRACE_TEST_HOOK("loss_read");
    // The count detects writers that started and finished between these reads.
    return !bank->writers.load() && count == bank->count.load();
  }
  LossLease(const LossLease&) = delete;
  LossLease& operator=(const LossLease&) = delete;
};
void resetLosses(ID epoch)
{
  const auto current = currentLossBank.load();
  for (size_t offset = 1; offset < lossBanks.size(); ++offset)
  {
    const auto index = (current + offset) % lossBanks.size();
    auto& bank = lossBanks[index];
    int free = 0;
    if (!bank.readers.compare_exchange_strong(free, -1)) continue;
    bank.epoch = epoch;
    bank.count = 0;
    bank.writers = 0;
    bank.overflow = 0;
    bank.overflowEligibility = 0;
    for (auto& event : bank.events) event.ready = false;
    for (auto& source : bank.sources) { source.key = 0; source.view = 0; }
    bank.readers.store(0);
    currentLossBank.store(index);
    return;
  }
  unbankedLoss(epoch);
}
LossBank::Source* sourceSlot(LossBank& bank, ID key, bool create)
{
  if (!key) return nullptr;
  for (size_t i = 0; i < bank.sources.size(); ++i)
  {
    auto& source = bank.sources[(key + i) % bank.sources.size()];
    auto known = source.key.load();
    if (known == key) return &source;
    if (!known && create)
    {
      if (source.key.compare_exchange_strong(known, key) || known == key) return &source;
    }
    if (!known && !create) return nullptr;
  }
  return nullptr;
}
void batchLoss(ID publication, ID source, LostEvent kind, ID epoch, EventBoundary evidence, uint64_t eligibilityUs)
{
  ++collector().lost;
  LossLease lease(epoch);
  if (!lease.bank) { unbankedLoss(epoch); return; }
  auto& bank = *lease.bank;
  struct Writer
  {
    LossBank& bank;
    explicit Writer(LossBank& value) : bank(value) { ++bank.writers; }
    ~Writer() { --bank.writers; }
  } writer(bank);
  const auto timestamp = evidence.time ? evidence.time : 1;
  eligibilityUs = eligibilityUs ? eligibilityUs : 1;
  const auto index = bank.count.fetch_add(1);
  TILE_TRACE_TEST_HOOK("loss_reserved");
  if (index < bank.events.size())
  {
    auto& event = bank.events[index];
    event.publication = publication; event.source = source;
    if (kind == LostEvent::View) event.boundary.viewOrder = evidence.order;
    else event.boundary.eligibilityUs = eligibilityUs;
    event.timestamp = timestamp; event.kind = kind;
    event.ready.store(true);
  }
  else
  {
    firstTime(bank.overflow, timestamp);
    TILE_TRACE_TEST_HOOK("loss_overflow_event");
    firstTime(bank.overflowEligibility, eligibilityUs);
  }
  if (kind == LostEvent::View)
  {
    if (auto* slot = sourceSlot(bank, source, true))
    {
      auto previous = slot->view.load();
      while (previous < evidence.order && !slot->view.compare_exchange_weak(previous, evidence.order)) {}
    }
    else
    {
      firstTime(bank.overflow, timestamp);
      firstTime(bank.overflowEligibility, eligibilityUs);
    }
  }
}
void batchLoss(ID publication, ID source, LostEvent kind, ID epoch, EventBoundary evidence)
{
  batchLoss(publication, source, kind, epoch, evidence, evidence.time);
}
#ifdef TILE_TRACE_TESTING
void batchLoss(ID publication, ID source, LostEvent kind, ID epoch = 0)
{
  batchLoss(publication, source, kind, epoch ? epoch : session(), eventBoundary());
}
#endif
uint64_t viewGap(ID source)
{
  LossLease lease(collector().epoch.load());
  if (!lease.bank) return UINT64_MAX;
  auto* slot = sourceSlot(*lease.bank, source, false);
  const auto order = slot ? slot->view.load() : 0;
  // A delayed loss from before capture resumed cannot invalidate the new view.
  return order < collector().viewLossStart.load() ? 0 : order;
}
struct MemberLoss
{
  uint64_t cache = 0, screen = 0, view = 0;
  ID viewOrder = 0;
  uint64_t screenEligibility = 0;
};
void earliest(uint64_t& value, uint64_t timestamp)
{
  if (!value || timestamp < value) value = timestamp;
}
MemberLoss memberLoss(const LossLease& lease, const BatchMember& member)
{
  if (!lease.bank || unbankedLossEpoch.load() == lease.bank->epoch) return {1, 1, 1, 0, 1};
  const auto& bank = *lease.bank;
  MemberLoss result{lease.overflow, lease.overflow, 0, 0, lease.overflowEligibility};
  for (size_t i = 0; i < lease.cutoff; ++i)
  {
    const auto& event = bank.events[i];
    if (event.source != member.source) continue;
    const bool original = event.publication == member.publication;
    const bool demand = event.publication && std::find(member.demands.begin(), member.demands.end(), event.publication) != member.demands.end();
    if (event.publication && !original && !demand) continue;
    if (!event.publication && event.kind != LostEvent::View && event.timestamp < member.admittedUs) continue;
    if (event.kind == LostEvent::View)
    {
      if (event.boundary.viewOrder > member.view && (!result.view || event.timestamp < result.view ||
          (event.timestamp == result.view && event.boundary.viewOrder < result.viewOrder)))
      { result.view = event.timestamp; result.viewOrder = event.boundary.viewOrder; }
    }
    else
    {
      if (!demand && (event.kind == LostEvent::Cache || event.kind == LostEvent::Outcome)) earliest(result.cache, event.timestamp);
      if (event.kind == LostEvent::Screen || event.kind == LostEvent::Outcome)
      {
        earliest(result.screen, event.timestamp);
        earliest(result.screenEligibility, event.boundary.eligibilityUs);
      }
    }
  }
  return result;
}
bool lostBefore(uint64_t timestamp, uint64_t endpoint = 0)
{
  return timestamp && (!endpoint || timestamp <= endpoint);
}
// Only one reader can pin a buffer, so two shared spares let publication continue while it holds an old snapshot.
void publishBatches(Collector& c)
{
  for (size_t i = 0; i < c.batches.size(); ++i)
  {
    auto& batch = c.batches[i];
    if (!batch.dirty) continue;
    const auto current = c.publishedIndex[i].load();
    for (auto& spare : c.publishedSpare)
    {
      const auto index = spare;
      auto& destination = c.published[index];
      int free = 0;
      if (!destination.readers.compare_exchange_strong(free, -1)) continue;
      TILE_TRACE_TEST_HOOK("publication_claimed");
      destination.batch.session = batch.session;
      destination.batch.id = batch.id;
      destination.batch.count = batch.count;
      std::copy_n(batch.members.begin(), batch.count, destination.batch.members.begin());
      TILE_TRACE_TEST_HOOK("batch_published");
      TILE_TRACE_TEST_COUNT("batch_members_copied", batch.count);
      destination.readers.store(0);
      c.publishedIndex[i].store(index);
      spare = current;
      batch.dirty = false;
      break;
    }
  }
}
struct BatchWrite
{
  Collector& c;
  std::unique_lock<std::mutex> lock;
  explicit BatchWrite(Collector& value) : c(value), lock(c.batchMutex, std::try_to_lock) {}
  explicit operator bool() const { return lock.owns_lock(); }
  ~BatchWrite() { if (lock) publishBatches(c); }
};
struct Frame
{
  ID id = 0, session = 0, surface = 0;
  uint64_t ended = 0, swapStarted = 0;
  int mode = 0;
  bool swapped = false;
  size_t count = 0;
  std::array<Record, FrameCapacity> draws{};
};
thread_local Frame frame;
#ifdef TILE_TRACE_TESTING
std::atomic<uint64_t> testTime{0};
std::atomic<bool> testDropViews{false};
#endif
Record& slot(Collector& c, ID id)
{
  auto& record = c.records[id % Capacity];
  if (record.context.id != id)
  {
    c.recordIndex.remove(id % Capacity);
    if (record.context.id) {
      ++c.evicted;
      c.retiredThrough = std::max(c.retiredThrough, record.context.id);
      for (size_t i = 0; i < c.startupCount; ++i)
        if (c.startup[i].context.id == record.context.id && c.startup[i].context.outcome == Outcome::Pending)
        {
          c.startup[i].context.outcome = Outcome::Truncated;
          c.startup[i].historyLost = true;
        }
    }
    record = {};
    record.historyLost = id <= c.retiredThrough;
    for (size_t i = 0; i < c.startupCount; ++i)
      if (c.startup[i].context.id == id) {
        record = c.startup[i];
        break;
      }
  }
  return record;
}
void archive(Collector& c, const Record& record)
{
  for (size_t i = 0; i < c.startupCount; ++i)
  {
    if (c.startup[i].context.id == record.context.id)
    {
      c.startup[i] = record;
      return;
    }
  }
  if (c.startupCount < StartupCapacity) c.startup[c.startupCount++] = record;
}
void merge(Collector& c, Record& record, const Context& context)
{
  auto times = record.context.time;
  const auto previous = record.context;
  const auto outcome = previous.outcome;
  record.context = context;
  if (context.payloadFormat == PayloadFormat::Unknown) record.context.payloadFormat = previous.payloadFormat;
  if (context.kind == Kind::Demand && !context.publication && previous.publication) {
    record.context.publication = previous.publication;
    record.context.generation = previous.generation;
    record.context.fingerprint = previous.fingerprint;
    record.context.origin = previous.origin;
    record.context.payloadSession = previous.payloadSession;
  }
  for (size_t i = 0; i < StageCount; ++i)
    if (times[i]) record.context.time[i] = times[i];
  if (outcome != Outcome::Pending && outcome != Outcome::Truncated) record.context.outcome = outcome;
  if (previous.kind == Kind::Demand && outcome == Outcome::Submitted)
    record.context.generation = previous.generation;
  if (record.historyLost) {
    for (size_t i = Draw; i < StageCount; ++i) record.context.time[i] = 0;
    record.context.outcome = Outcome::Truncated;
  }
  c.recordIndex.update(static_cast<size_t>(&record - c.records.data()), record.context);
}
ViewTile viewTile(const Context& context)
{
  return {context.x, context.y, context.wrap, context.z, context.overscaledZ};
}
bool sameMember(const BatchMember& member, const Context& context)
{
  return member.publication == context.publication && member.map == context.map && member.source == context.source;
}
bool resolved(const ScreenUse& use) { return use.submittedUs || use.noDraw; }
bool settled(const ScreenUse& use) { return resolved(use) || use.leftUs; }
size_t viewIndex(const View& view, const ViewTile& tile)
{
  const auto end = view.tiles.begin() + view.count;
  const auto found = std::lower_bound(view.tiles.begin(), end, tile);
  return found != end && *found == tile ? static_cast<size_t>(found - view.tiles.begin()) : view.count;
}
const View* currentView(const Collector& c, ID map, ID source, ID session)
{
  if (!c.capture.load()) return nullptr;
  for (const auto& view : c.views)
    if (view.id && view.map == map && view.source == source && view.session == session &&
        view.captureGeneration == c.captureGeneration.load()) return &view;
  return nullptr;
}
const View* originalView(const Collector& c, const Context& context)
{
  const auto matches = [&](const View& view) {
    return context.view && view.id == context.view && view.map == context.map &&
           view.source == context.source && view.session == context.session;
  };
  for (const auto& view : c.views) if (matches(view)) return &view;
  for (const auto& view : c.viewHistory) if (matches(view)) return &view;
  return nullptr;
}
void rememberView(Collector& c, const View& view)
{
  if (view.id) c.viewHistory[c.nextViewHistory++ % c.viewHistory.size()] = view;
}
bool refreshUse(Collector& c, const BatchMember& member, ScreenUse& use, ID session)
{
  if (settled(use)) return false;
  TILE_TRACE_TEST_HOOK("use_refresh");
  const auto state = [&] { return std::make_tuple(use.unknown, use.failed, use.leftUs, use.drawOrder, use.exact); };
  const auto before = state();
  if (member.viewGap != viewGap(member.source)) use.unknown = true;
  const auto* current = currentView(c, member.map, member.source, session);
  if (current)
  {
    const auto index = viewIndex(*current, use.tile);
    if (index < current->count && current->requirements[index] == use.requirement)
    {
      use.failed |= current->invalidations[index] > member.view;
      return before != state();
    }
  }
  for (const auto& past : c.viewHistory)
  {
    if (past.session != session || past.map != member.map || past.source != member.source) continue;
    const auto index = viewIndex(past, use.tile);
    if (index == past.count || past.requirements[index] != use.requirement || !past.leftTimes[index]) continue;
    use.leftUs = past.leftTimes[index];
    use.drawOrder = past.leftOrder;
    use.failed |= past.retired || past.invalidations[index] > member.view;
    use.exact = !use.unknown && !use.originalFailed && member.lossBaseline == c.gaps.load();
    return before != state();
  }
  use.unknown = true;
  return before != state();
}
void refreshBatch(Collector& c, Batch& batch)
{
  for (size_t i = 0; i < batch.count; ++i)
    for (size_t j = 0; j < batch.members[i].useCount; ++j)
      batch.dirty |= refreshUse(c, batch.members[i], batch.members[i].uses[j], batch.session);
}
void refreshSource(Collector& c, ID map, ID source, const View* changedView = nullptr,
                   const std::bitset<ViewTileLimit>* changedKeys = nullptr)
{
  const bool scoped = changedView && changedKeys && changedKeys->count() < changedView->count;
  const auto gap = scoped ? viewGap(source) : 0;
  const auto captureGap = c.gaps.load();
  constexpr size_t RequiredSlots = ViewTileLimit * 2;
  std::array<uint16_t, RequiredSlots> requirements;
  if (scoped)
  {
    requirements.fill(0);
    for (size_t i = 0; i < changedView->count; ++i)
    {
      auto slot = RecordIndex::bucket(0, changedView->requirements[i]) % RequiredSlots;
      while (requirements[slot]) slot = (slot + 1) % RequiredSlots;
      requirements[slot] = static_cast<uint16_t>(i + 1);
    }
  }
  const auto currentRequirement = [&](const ScreenUse& use)
  {
    auto slot = RecordIndex::bucket(0, use.requirement) % RequiredSlots;
    for (size_t i = 0; i < RequiredSlots && requirements[slot]; ++i)
    {
      const auto index = requirements[slot] - 1;
      if (changedView->requirements[index] == use.requirement && changedView->tiles[index] == use.tile)
        return static_cast<size_t>(index);
      slot = (slot + 1) % RequiredSlots;
    }
    return changedView->count;
  };
  for (auto& batch : c.batches)
    for (size_t i = 0; i < batch.count; ++i)
    {
      auto& member = batch.members[i];
      if (member.map != map || member.source != source) continue;
      for (size_t j = 0; j < member.useCount; ++j)
      {
        auto& use = member.uses[j];
        if (settled(use)) continue;
        if (scoped && batch.session == changedView->session && member.viewKnown && !member.useOverflow &&
            member.viewGap == gap && changedView->gap == gap && member.lossBaseline == captureGap)
        {
          const auto index = currentRequirement(use);
          if (index < changedView->count && !(*changedKeys)[index]) continue;
        }
        batch.dirty |= refreshUse(c, member, use, batch.session);
      }
    }
  if (scoped)
  {
    TILE_TRACE_TEST_HOOK("scoped_refresh");
    // Loss writers do not need batchMutex, so a skipped use may have become uncertain.
    if (gap != viewGap(source) || captureGap != c.gaps.load()) refreshSource(c, map, source);
  }
}
void batchOutcome(Collector& c, const Context& context, EventBoundary evidence)
{
  if (context.kind == Kind::Demand && context.outcome != Outcome::Pending)
  {
    auto& demand = c.demandOutcomes[context.id % Capacity];
    if (demand.id && demand.id != context.id) c.retiredDemandThrough = std::max(c.retiredDemandThrough, demand.id);
    demand = {context.id, context.outcome};
  }
  if ((!context.publication && context.kind != Kind::Demand) || context.outcome == Outcome::Pending ||
      context.outcome == Outcome::Submitted || context.outcome == Outcome::Empty) return;
  for (auto& batch : c.batches)
    if (batch.session == context.session)
      for (size_t i = 0; i < batch.count; ++i)
      {
        auto& member = batch.members[i];
        if (context.kind == Kind::Demand)
        {
          if (context.outcome == Outcome::Cancelled) continue;
          if (std::find(member.demands.begin(), member.demands.end(), context.id) == member.demands.end()) continue;
        }
        else
        {
          if (!sameMember(member, context)) continue;
          if (context.kind == Kind::Publication && context.id == context.publication)
          {
            batch.dirty = true;
            if (context.outcome == Outcome::Cancelled || context.outcome == Outcome::Teardown)
            {
              if (earlierBoundary(evidence, member.retiredUs, member.retirementOrder))
              {
                member.retiredUs = evidence.time;
                member.retirementOrder = evidence.order;
                member.retirement = context.outcome == Outcome::Cancelled ? Retirement::Cancelled : Retirement::Shutdown;
              }
              continue;
            }
            if (member.retiredUs && context.outcome == Outcome::NoReceiver &&
                !before(evidence, {member.retiredUs, member.retirementOrder})) continue;
            if (earlierBoundary(evidence, member.failedUs, member.failureOrder))
            {
              member.failedUs = evidence.time;
              member.failureOrder = evidence.order;
              member.failure = context.outcome;
            }
            member.publicationFailed = true;
            for (size_t j = 0; j < member.useCount; ++j)
              if (!settled(member.uses[j])) member.uses[j].originalFailed = true;
            if (!member.cachedUs && !member.cacheBypassed) member.mlFailed = true;
          }
        }
        batch.dirty = true;
        for (size_t j = 0; j < member.useCount; ++j)
        {
          auto& use = member.uses[j];
          if (!(use.tile == viewTile(context))) continue;
          if (context.outcome == Outcome::Error && earlierBoundary(evidence, use.failedUs, use.failureOrder))
          { use.failedUs = evidence.time; use.failureOrder = evidence.order; use.failure = context.outcome; }
          if (!settled(use)) use.failed = true;
        }
      }
}
void batchUse(Collector& c, const Context& context, uint64_t submittedUs, ID frameID, bool noDraw, ID requirement, ID drawOrder)
{
  if (!context.publication || !context.generation || context.outcome == Outcome::Error) return;
  for (auto& batch : c.batches)
    if (batch.session == context.session)
      for (size_t i = 0; i < batch.count; ++i)
      {
        auto& member = batch.members[i];
        if (!sameMember(member, context) || !member.viewKnown) continue;
        for (size_t j = 0; j < member.useCount; ++j)
        {
          auto& use = member.uses[j];
          if (resolved(use) || !(use.tile == viewTile(context)) || !requirement || use.requirement != requirement) continue;
          refreshUse(c, member, use, batch.session);
          if (use.leftUs && (noDraw || !context.time[Draw] || context.time[Draw] > use.leftUs)) continue;
          batch.dirty = true;
          use.leftUs = 0;
          use.submittedUs = submittedUs;
          use.drawUs = noDraw ? context.time[Layout] : context.time[Draw];
          use.drawOrder = drawOrder;
          use.generation = context.generation;
          use.frame = frameID;
          use.noDraw = noDraw;
          const bool originalError = use.originalFailed && (!member.failedUs || !use.drawUs || member.failedUs <= use.drawUs);
          // The captured requirement proves this draw's view, even if a later view was lost.
          use.exact = !originalError && member.lossBaseline == c.gaps.load();
        }
      }
}
void store(const Context& context) noexcept
{
  auto& c = collector();
  if (!context.id || context.session != c.epoch.load() || !c.capture.load()) return;
  std::unique_lock<std::mutex> lock(c.mutex, std::try_to_lock);
  if (!lock) { ++c.lost; return; }
  if (context.session != c.epoch.load()) return;
  if (!c.started) c.started = now();
  auto& record = slot(c, context.id);
  if (!record.context.id) {
    record.lossBaseline = c.lost.load() + c.gaps.load();
    record.beforeFirstDraw = !context.time[Draw];
  }
  merge(c, record, context);
  archive(c, record);
}
void activity(int type, ID map, int value = 0) noexcept
{
  auto& c = collector();
  if (!c.capture.load()) return;
  std::unique_lock<std::mutex> lock(c.mutex, std::try_to_lock);
  if (!lock) { ++c.lost; return; }
  c.activity[c.activityIndex++ % c.activity.size()] = {frame.id, map, now(), type, value};
}
const char* outcomeName(Outcome value)
{
  static const char* names[] = {"pending", "submitted", "ready-empty", "cancelled", "superseded",
    "no-receiver", "stale-fast", "stale-worker", "stale-submit", "overflow", "stale-drain",
    "error", "teardown", "coalesced", "truncated"};
  return names[static_cast<size_t>(value)];
}
const char* originName(Origin value)
{
  static const char* names[] = {"fresh", "memory", "disk", "processed", "renderer", "unknown"};
  return names[static_cast<size_t>(value)];
}
} // namespace

ID nextID() noexcept { return collector().serial.fetch_add(1); }
#ifdef TILE_TRACE_TESTING
void setTestTime(uint64_t value) noexcept { testTime = value; }
void setTestDropViews(bool value) noexcept { testDropViews = value; }
#endif
uint64_t now() noexcept
{
#ifdef TILE_TRACE_TESTING
  if (testTime.load()) return testTime.load();
#endif
  return std::chrono::duration_cast<std::chrono::microseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count();
}
ID session() noexcept { return collector().epoch.load(); }
bool enabled() noexcept { return collector().capture.load(); }
void loss() noexcept { ++collector().lost; }
void cacheSnapshotTime(bool disk, uint64_t timestamp) noexcept {
  (disk ? collector().diskStatsTime : collector().memoryStatsTime).store(timestamp);
}
void configure(bool capture, bool reset)
{
  auto& c = collector();
  std::lock_guard<std::mutex> lock(c.mutex);
  std::lock_guard<std::mutex> batchLock(c.batchMutex);
  const bool changed = c.capture.load() != capture;
  if (c.capture.load() && !capture && !reset) ++c.gaps;
  if (!reset)
  {
    LossLease lease(c.epoch.load());
    if (!lease.bank) resetLosses(c.epoch.load());
  }
  if (reset)
  {
    const auto nextEpoch = c.epoch.load() + 1;
    for (auto& record : c.records) record = {};
    c.recordIndex.clear();
    for (auto& record : c.startup) record = {};
    c.startupCount = 0;
    for (auto& event : c.activity) event = {};
    c.activityIndex = 0;
    for (auto& batch : c.batches) { batch.id = 0; batch.session = 0; batch.count = 0; batch.dirty = true; }
    c.nextBatch = 0;
    for (auto& view : c.views) { view.id = 0; view.map = 0; view.source = 0; view.count = 0; }
    c.nextView = 0;
    for (auto& view : c.viewHistory) view.id = 0;
    c.nextViewHistory = 0;
    resetLosses(nextEpoch);
    for (auto& demand : c.demandOutcomes) demand = {};
    c.retiredDemandThrough = 0;
    publishBatches(c);
    c.lost = 0;
    c.gaps = 0;
    c.evicted = 0;
    c.retiredThrough = 0;
    c.started = now();
    c.epoch.store(nextEpoch);
  }
  if (changed || reset)
  {
    c.viewLossStart = c.serial.load();
    ++c.captureGeneration;
    ++c.viewChanges;
  }
  c.capture = capture;
}
Context create(ID map, ID source, ID consumer, uint8_t z, uint32_t x, uint32_t y,
               uint8_t overscaledZ, int16_t wrap, uint8_t role, ID view, PayloadFormat format)
{
  Context context;
  context.payloadFormat = format;
  if (!enabled() || !map || !source) return context;
  context.session = session(); context.map = map; context.source = source; context.view = view;
  context.id = context.demand = nextID(); context.consumer = consumer;
  context.z = z; context.x = x; context.y = y; context.overscaledZ = overscaledZ; context.wrap = wrap; context.role = role;
  mark(context, Request);
  return context;
}
void mark(Context& context, Stage stage) noexcept
{
  if (!context.id) return;
  if (!context.time[stage]) context.time[stage] = now();
  if (context.outcome == Outcome::Error) finish(context, Outcome::Error);
  else store(context);
}
void markNativeReady(Context& context) noexcept
{
  context.payloadFormat = PayloadFormat::NativeGeometry;
  mark(context, Features);
}
void finish(Context& context, Outcome outcome) noexcept
{
  const auto evidence = eventBoundary();
  context.outcome = outcome;
  if (context.id && context.session == session() && enabled() && outcome != Outcome::Pending &&
      outcome != Outcome::Submitted && outcome != Outcome::Empty &&
      !(context.kind == Kind::Demand && outcome == Outcome::Cancelled))
  {
    auto& c = collector();
    TILE_TRACE_TEST_HOOK("finish_boundary");
    BatchWrite lock(c);
    if (lock) batchOutcome(c, context, evidence);
    else batchLoss(context.kind == Kind::Demand ? context.id : context.publication, context.source,
                   context.kind == Kind::Demand ? LostEvent::Screen : LostEvent::Outcome, context.session, evidence);
  }
  store(context);
}
void bindDemand(const Context& context) noexcept
{
  const auto evidence = eventBoundary();
  auto& c = collector();
  if (!context.demand || context.session != session()) return;
  {
    BatchWrite batchLock(c);
    if (!batchLock) batchLoss(context.publication, context.source, LostEvent::Screen, context.session, evidence);
    else
    {
  for (auto& batch : c.batches)
    if (batch.session == context.session)
      for (size_t i = 0; i < batch.count; ++i)
      {
        auto& member = batch.members[i];
        if (!context.publication || !sameMember(member, context) ||
            std::all_of(member.uses.begin(), member.uses.begin() + member.useCount, settled))
          continue;
        if (std::find(member.demands.begin(), member.demands.end(), context.demand) != member.demands.end()) continue;
        const auto free = std::find(member.demands.begin(), member.demands.end(), 0);
        if (free == member.demands.end()) batchLoss(context.publication, context.source, LostEvent::Screen, context.session, evidence);
        else { *free = context.demand; batch.dirty = true; }
      }
    }
  }
  std::unique_lock<std::mutex> lock(c.mutex, std::try_to_lock);
  if (!lock) { ++c.lost; return; }
  if (context.session != c.epoch.load()) return;
  auto& record = c.records[context.demand % Capacity];
  if (record.context.id != context.demand) { ++c.lost; return; }
  if (record.context.outcome != Outcome::Pending) return;
  record.context.publication = context.publication;
  record.context.generation = context.generation;
  record.context.origin = context.origin;
  record.context.fingerprint = context.fingerprint;
  record.context.payloadSession = context.payloadSession;
  if (context.payloadFormat != PayloadFormat::Unknown) record.context.payloadFormat = context.payloadFormat;
  for (size_t i = Producer; i < StageCount; ++i)
    record.context.time[i] = context.time[i];
  if (context.origin == Origin::Renderer && !context.generation) {
    record.context.outcome = Outcome::Truncated;
    record.historyLost = true;
  }
  if (context.outcome == Outcome::Error) record.context.outcome = Outcome::Error;
  if (context.empty && context.outcome != Outcome::Error && (context.time[Layout] || (context.origin == Origin::Renderer && context.generation))) {
    record.context.outcome = Outcome::Empty;
    for (auto& artifact : c.records)
      if (artifact.context.kind == Kind::Publication && artifact.context.publication == context.publication &&
          artifact.context.outcome == Outcome::Pending) {
        artifact.context.outcome = Outcome::Empty;
        archive(c, artifact);
      }
  }
  c.recordIndex.update(context.demand % Capacity, record.context);
  archive(c, record);
}
void retireSource(ID source) noexcept
{
  const auto evidence = eventBoundary();
  auto& c = collector();
  const auto epoch = c.epoch.load();
  {
  BatchWrite lock(c);
  if (!lock)
  {
    batchLoss(0, source, LostEvent::Outcome, epoch, evidence);
    batchLoss(0, source, LostEvent::View, epoch, evidence);
    ++c.viewChanges;
    return;
  }
  for (auto& view : c.views)
    if (view.id && view.source == source)
    {
      view.retired = true;
      view.leftOrder = evidence.order;
      for (size_t i = 0; i < view.count; ++i) view.leftTimes[i] = evidence.time;
      rememberView(c, view);
      view.id = 0;
    }
  for (auto& batch : c.batches)
  {
    for (size_t i = 0; i < batch.count; ++i)
    {
      auto& member = batch.members[i];
      if (member.source != source) continue;
      if (earlierBoundary(evidence, member.retiredUs, member.retirementOrder))
      { member.retiredUs = evidence.time; member.retirementOrder = evidence.order; member.retirement = Retirement::Shutdown; }
      batch.dirty = true;
    }
    refreshBatch(c, batch);
  }
  ++c.viewChanges;
  }
  std::unique_lock<std::mutex> lock(c.mutex, std::try_to_lock);
  if (!lock) { ++c.lost; return; }
  for (auto& r : c.records)
    if (r.context.source == source && r.context.outcome == Outcome::Pending)
    {
      r.context.outcome = Outcome::Teardown;
      archive(c, r);
    }
}
void pump(ID map, bool begin) noexcept { activity(begin ? 1 : 2, map); }
void frontend(ID map, bool begin) noexcept { activity(begin ? 3 : 4, map); }
FrameScope::FrameScope(int mode) noexcept
{
  // Only entries below count are read, and draw initializes each slot before admitting it.
  frame.id = frame.session = frame.surface = 0;
  frame.ended = frame.swapStarted = 0;
  frame.mode = 0; frame.swapped = false; frame.count = 0;
  if (!enabled()) return;
  frame.id = nextID(); frame.session = session(); frame.mode = mode;
  activity(5, 0, mode);
}
FrameScope::~FrameScope()
{
  if (frame.id && !frame.swapped) activity(8, 0);
  frame.id = 0;
}
void draw(const Context& originalTrace, bool symbol, const ViewTile* actualTile) noexcept
{
  const auto generation = captureGeneration();
  const auto evidence = eventBoundary();
  auto original = originalTrace;
  if (actualTile)
  {
    original.x = actualTile->x; original.y = actualTile->y; original.z = actualTile->z;
    original.overscaledZ = actualTile->overscaledZ; original.wrap = actualTile->wrap;
  }
  if (!frame.id || !original.generation || !enabled()) return;
  for (size_t i = 0; i < frame.count; ++i)
    if (frame.draws[i].context.generation == original.generation &&
        frame.draws[i].context.consumer == original.consumer && frame.draws[i].symbol == symbol &&
        viewTile(frame.draws[i].context) == viewTile(original) &&
        frame.draws[i].drawViewSerial == viewSerial()) return;
  auto context = original;
  if (context.session != frame.session) {
    auto& c = collector();
    std::unique_lock<std::mutex> lock(c.mutex, std::try_to_lock);
    if (!lock) { ++c.lost; return; }
    bool reused = false;
    for (auto link = c.recordIndex.heads[RecordIndex::bucket(frame.session, original.publication)];
         link; link = c.recordIndex.next[link - 1]) {
      TILE_TRACE_TEST_HOOK("record_candidate");
      const auto& record = c.records[link - 1];
      const auto& demand = record.context;
      if (demand.session == frame.session && demand.kind == Kind::Demand &&
          (demand.outcome == Outcome::Pending || demand.outcome == Outcome::Submitted) &&
          demand.consumer == original.consumer &&
          demand.publication == original.publication &&
          demand.origin == Origin::Renderer) {
        context = demand;
        context.generation = original.generation;
        reused = true;
        break;
      }
    }
    if (!reused) return;
  }
  if (frame.count == FrameCapacity) { batchLoss(original.publication, original.source, LostEvent::Screen, original.session, evidence); return; }
  for (size_t i = Draw; i < StageCount; ++i) context.time[i] = 0;
  context.outcome = Outcome::Pending;
  auto& record = frame.draws[frame.count];
  record = {}; record.context = context; record.symbol = symbol; record.frame = frame.id;
  record.context.time[Draw] = evidence.time;
  record.drawOrder = evidence.order;
  TILE_TRACE_TEST_HOOK("draw_view");
  {
    auto& c = collector();
    BatchWrite lock(c);
    if (!lock) batchLoss(context.publication, context.source, LostEvent::Screen, context.session, evidence);
    else if (c.captureGeneration.load() == generation)
    {
      if (const auto* view = currentView(c, context.map, context.source, context.session))
      {
        const auto index = viewIndex(*view, viewTile(context));
        if (index < view->count && view->gap == viewGap(context.source))
          record.drawRequirement = view->requirements[index];
        record.drawViewSerial = c.viewChanges.load();
      }
    }
  }
  store(record.context);
  for (size_t i = 0; i < frame.count; ++i)
    if (frame.draws[i].context.generation == context.generation &&
        frame.draws[i].context.consumer == context.consumer && frame.draws[i].symbol == symbol &&
        viewTile(frame.draws[i].context) == viewTile(context) &&
        frame.draws[i].drawRequirement == record.drawRequirement) return;
  ++frame.count;
}
void frameEnd() noexcept
{
  if (!frame.id) return;
  frame.ended = now();
  activity(6, 0, frame.mode);
}
void swapBegin(uintptr_t surface) noexcept
{
  if (!frame.id) return;
  {
  auto& c = collector();
  std::unique_lock<std::mutex> lock(c.mutex, std::try_to_lock);
  if (lock)
  {
    for (const auto& entry : c.surfaces)
      if (entry.first == surface) frame.surface = entry.second;
  }
  else ++c.lost;
  }
  frameEnd();
  frame.swapStarted = now();
}
void swapEnd(bool success, int error) noexcept
{
  if (!frame.id || frame.session != session()) return;
  const auto time = now();
  frame.swapped = true;
  activity(success ? 7 : 9, 0, error);
  auto& c = collector();
  if (success)
  {
    BatchWrite batchLock(c);
    for (size_t i = 0; i < frame.count; ++i)
    {
      const auto& drawn = frame.draws[i];
      if (batchLock) batchUse(c, drawn.context, time, frame.id, false, drawn.drawRequirement, drawn.drawOrder);
      else batchLoss(drawn.context.publication, drawn.context.source, LostEvent::Screen, drawn.context.session,
                     {time, drawn.drawOrder}, drawn.context.time[Draw]);
    }
  }
  std::unique_lock<std::mutex> lock(c.mutex, std::try_to_lock);
  if (!lock) { ++c.lost; return; }
  if (frame.session != c.epoch.load()) return;
  for (size_t i = 0; i < frame.count; ++i)
  {
    auto drawn = frame.draws[i];

    drawn.context.time[FrameEnd] = frame.ended;
    drawn.context.time[SwapBegin] = frame.swapStarted;
    if (success)
    {
      drawn.context.time[Submitted] = time;
      drawn.context.outcome = Outcome::Submitted;
    }
    auto& record = slot(c, drawn.context.id);
    const bool alreadySubmitted = record.context.time[Submitted] != 0;
    merge(c, record, drawn.context);
    const auto contribution = drawn.symbol ? 1 : 0;
    if (!record.contributionDraw[contribution]) record.contributionDraw[contribution] = drawn.context.time[Draw];
    if (success && !record.historyLost && !record.contributionSubmit[contribution]) {
      record.contributionSubmit[contribution] = time;
      record.contributionGeneration[contribution] = drawn.context.generation;
    }
    if (success && !record.historyLost && !alreadySubmitted) {
      record.submittedDraw = drawn.context.time[Draw];
      record.firstUseComplete = record.beforeFirstDraw && record.lossBaseline == c.lost.load() + c.gaps.load();
    }
    if (!alreadySubmitted)
    {
      record.frame = frame.id; record.surface = frame.surface; record.symbol = drawn.symbol;
      record.mode = frame.mode;
      record.context.time[FrameEnd] = frame.ended;
      record.context.time[SwapBegin] = frame.swapStarted;
    }
    if (!success) ++record.failedSwaps;
    archive(c, record);
    for (auto link = c.recordIndex.heads[RecordIndex::bucket(frame.session, drawn.context.publication)];
         link; link = c.recordIndex.next[link - 1])
    {
      TILE_TRACE_TEST_HOOK("record_candidate");
      auto& demand = c.records[link - 1];
      auto& ctx = demand.context;
      if (ctx.session != frame.session || (ctx.outcome != Outcome::Pending && ctx.outcome != Outcome::Submitted)) continue;
      // Retained layouts can contain buckets from several accepted generations.
      // Reuse takes the generation of the bucket that actually draws.
      const bool receiver = ctx.kind == Kind::Demand && ctx.consumer == drawn.context.consumer &&
        ctx.publication == drawn.context.publication &&
        (ctx.generation == drawn.context.generation || ctx.origin == Origin::Renderer);
      const bool publication = ctx.kind == Kind::Publication && ctx.publication == drawn.context.publication &&
        (ctx.id == ctx.publication || ctx.consumer == drawn.context.consumer);
      if (!receiver && !publication) continue;
      if (!demand.contributionDraw[contribution]) demand.contributionDraw[contribution] = drawn.context.time[Draw];
      if (success && !demand.contributionSubmit[contribution]) {
        demand.contributionSubmit[contribution] = time;
        demand.contributionGeneration[contribution] = drawn.context.generation;
      }
      if (ctx.outcome == Outcome::Submitted) { archive(c, demand); continue; }
      if (!ctx.time[Draw]) ctx.time[Draw] = drawn.context.time[Draw];
      if (!success) { ++demand.failedSwaps; archive(c, demand); continue; }
      for (size_t j = FrameEnd; j < StageCount; ++j) ctx.time[j] = drawn.context.time[j];
      ctx.outcome = Outcome::Submitted;
      if (ctx.kind == Kind::Demand && ctx.origin == Origin::Renderer) ctx.generation = drawn.context.generation;
      demand.frame = frame.id; demand.surface = frame.surface; demand.mode = frame.mode;
      demand.submittedDraw = drawn.context.time[Draw];
      demand.firstUseComplete = demand.beforeFirstDraw && demand.lossBaseline == c.lost.load() + c.gaps.load() && !demand.historyLost;
      archive(c, demand);
    }
  }
}
void surfaceCreated(uintptr_t surface) noexcept
{
  auto& c = collector();
  std::unique_lock<std::mutex> lock(c.mutex, std::try_to_lock);
  if (!lock) { ++c.lost; return; }
  for (auto& entry : c.surfaces)
    if (entry.first == surface) { entry.second = nextID(); return; }
  for (auto& entry : c.surfaces)
    if (!entry.first)
    {
      entry = {surface, nextID()};
      return;
    }
  ++c.lost;
}
void surfaceDestroyed(uintptr_t surface) noexcept
{
  activity(10, surface);
  auto& c = collector();
  std::unique_lock<std::mutex> lock(c.mutex, std::try_to_lock);
  if (!lock) { ++c.lost; return; }
  for (auto& entry : c.surfaces) if (entry.first == surface) entry = {};
}

bool ViewTile::operator==(const ViewTile& other) const noexcept
{
  return std::tie(z, x, y, overscaledZ, wrap) == std::tie(other.z, other.x, other.y, other.overscaledZ, other.wrap);
}
bool ViewTile::operator<(const ViewTile& other) const noexcept
{
  return std::tie(z, x, y, overscaledZ, wrap) < std::tie(other.z, other.x, other.y, other.overscaledZ, other.wrap);
}
uint64_t viewSerial() noexcept { return collector().viewChanges.load(); }
uint64_t captureGeneration() noexcept { return collector().captureGeneration.load(); }
ID updateView(ID map, ID source, ID style, const ViewTile* tiles, size_t count,
              uint64_t expectedCaptureGeneration) noexcept
{
  if (!map || !source) return 0;
  auto& c = collector();
  const auto generation = expectedCaptureGeneration ? expectedCaptureGeneration : c.captureGeneration.load();
  const auto epoch = c.epoch.load();
  const auto active = [&] {
    return c.capture.load() && c.captureGeneration.load() == generation && c.epoch.load() == epoch;
  };
  if (!active()) return 0;
  const auto evidence = eventBoundary();
  const auto missing = [&]() -> ID
  {
    if (active()) { batchLoss(0, source, LostEvent::View, epoch, evidence); ++c.viewChanges; }
    return 0;
  };
#ifdef TILE_TRACE_TESTING
  if (testDropViews.load()) return missing();
#endif
  if (count > ViewTileLimit || (count && !tiles)) return missing();
  std::array<ViewTile, ViewTileLimit> sorted{};
  if (count) std::copy_n(tiles, count, sorted.begin());
  std::sort(sorted.begin(), sorted.begin() + count);
  count = std::unique(sorted.begin(), sorted.begin() + count) - sorted.begin();
  TILE_TRACE_TEST_HOOK("view_prepared");
  BatchWrite lock(c);
  if (!lock) return missing();
  if (!active()) return 0;
  View* view = nullptr;
  for (auto& candidate : c.views)
    if (candidate.map == map && candidate.source == source && candidate.session == c.epoch.load()) view = &candidate;
  if (!view)
  {
    for (auto& candidate : c.views) if (!candidate.id) { view = &candidate; break; }
    if (!view)
    {
      missing();
      view = &c.views[c.nextView++ % c.views.size()];
      rememberView(c, *view);
    }
    *view = {};
  }
  const auto gap = viewGap(source);
  if (view->id && view->captureGeneration == generation && view->style == style &&
      view->gap == gap && view->count == count &&
      std::equal(sorted.begin(), sorted.begin() + count, view->tiles.begin())) return view->id;
  auto previous = *view;
  const bool continuous = previous.id && previous.captureGeneration == generation && previous.gap == gap;
  const auto timestamp = evidence.time;
  previous.leftOrder = evidence.order;
  if (continuous)
    for (size_t i = 0; i < previous.count; ++i)
      if (!std::binary_search(sorted.begin(), sorted.begin() + count, previous.tiles[i]))
        previous.leftTimes[i] = timestamp;
  rememberView(c, previous);
  view->session = c.epoch.load(); view->map = map; view->source = source;
  view->id = nextID(); view->style = style; view->gap = gap; view->count = count;
  view->captureGeneration = generation;
  view->tiles = sorted;
  view->leftTimes.fill(0);
  view->leftOrder = 0;
  view->retired = false;
  for (size_t i = 0; i < count; ++i)
  {
    const auto old = viewIndex(previous, sorted[i]);
    const bool retained = continuous && old < previous.count;
    view->requirements[i] = retained ? previous.requirements[old] : nextID();
    view->invalidations[i] = retained ? previous.invalidations[old] : 0;
  }
  refreshSource(c, map, source);
  ++c.viewChanges;
  return view->id;
}
void invalidateView(ID map, ID source, const ViewTileRange* range) noexcept
{
  if (!map || !source) return;
  auto& c = collector();
  const auto generation = c.captureGeneration.load();
  const auto epoch = c.epoch.load();
  const auto active = [&] {
    return c.capture.load() && c.captureGeneration.load() == generation && c.epoch.load() == epoch;
  };
  if (!active()) return;
  const auto evidence = eventBoundary();
  BatchWrite lock(c);
  if (!lock)
  {
    if (active()) { batchLoss(0, source, LostEvent::View, epoch, evidence); ++c.viewChanges; }
    return;
  }
  if (!active()) return;
  for (auto& view : c.views)
  {
    if (!view.id || view.map != map || view.source != source || view.captureGeneration != generation) continue;
    const auto previous = view;
    ID revision = 0;
    std::bitset<ViewTileLimit> changedKeys;
    for (size_t i = 0; i < view.count; ++i)
    {
      bool intersects = !range;
      if (range && range->maxZ < 32)
      {
        const auto& tile = view.tiles[i];
        if (tile.z < range->minZ || tile.z > range->maxZ) continue;
        const auto dz = range->maxZ - tile.z;
        const auto minX = range->minX >> dz, maxX = range->maxX >> dz;
        const bool xInside = range->minX > range->maxX ? tile.x >= minX || tile.x <= maxX :
          tile.x >= minX && tile.x <= maxX;
        intersects = !tile.z || (xInside && tile.y >= (range->minY >> dz) && tile.y <= (range->maxY >> dz));
      }
      if (!intersects) continue;
      if (!revision) revision = nextID();
      view.invalidations[i] = revision;
      changedKeys.set(i);
    }
    if (revision)
    {
      rememberView(c, previous);
      view.id = revision;
      refreshSource(c, map, source, &view, &changedKeys);
      ++c.viewChanges;
    }
  }
}

void trackBatchMember(const BatchInfo& info, const Context& context) noexcept
{
  const auto evidence = eventBoundary();
  auto& c = collector();
  BatchWrite lock(c);
  if (!lock) { batchLoss(context.publication, context.source, LostEvent::Outcome, context.session, evidence); return; }
  Batch* batch = nullptr;
  for (auto& candidate : c.batches)
    if (candidate.id == info.id && candidate.session == info.session) batch = &candidate;
  if (!batch)
  {
    batch = &c.batches[c.nextBatch++ % BatchCapacity];
    batch->count = 0;
    batch->id = info.id;
    batch->session = info.session;
  }
  if (batch->count == BatchMemberCapacity) { ++c.lost; return; }
  batch->dirty = true;
  auto& member = batch->members[batch->count++];
  member = {};
  member.publication = context.publication;
  member.map = context.map; member.source = context.source; member.view = context.view;
  member.demands[0] = context.demand;
  member.tile = viewTile(context);
  member.admittedUs = evidence.time;
  member.lossBaseline = c.gaps.load();
  member.viewGap = viewGap(context.source);
  member.publicationFailed = !c.capture.load() || !context.publication || context.session != info.session;
  member.mlFailed = member.publicationFailed;
  if (const auto* origin = originalView(c, context))
  {
    member.viewKnown = origin->gap == viewGap(context.source) &&
      origin->captureGeneration == c.captureGeneration.load() && !member.publicationFailed;
    member.viewGap = origin->gap;
    for (size_t i = 0; i < origin->count; ++i)
    {
      const auto& tile = origin->tiles[i];
      if (tile.z != context.z || tile.x != context.x || tile.y != context.y) continue;
      if (member.useCount == member.uses.size())
      {
        member.useOverflow = true;
        ++c.lost;
        break;
      }
      auto& use = member.uses[member.useCount++];
      use.tile = tile;
      use.requirement = origin->requirements[i];
      use.unknown = !member.viewKnown;
      use.originalFailed = member.publicationFailed;
      refreshUse(c, member, use, batch->session);
      const auto& demand = c.demandOutcomes[context.demand % Capacity];
      if (demand.id == context.demand && !settled(use) && use.tile == viewTile(context))
        use.failed |= demand.outcome != Outcome::Pending && demand.outcome != Outcome::Submitted &&
          demand.outcome != Outcome::Empty && demand.outcome != Outcome::Cancelled;
      else if (context.demand <= c.retiredDemandThrough) use.unknown = true;
    }
  }
}

void batchCacheAdmission(const Context& context, bool stored, uint64_t timestamp) noexcept
{
  const auto evidence = eventBoundary();
  auto& c = collector();
  if (!context.publication || !c.capture.load() || context.session != c.epoch.load()) return;
  BatchWrite lock(c);
  if (!lock) { batchLoss(context.publication, context.source, LostEvent::Cache, context.session, {timestamp, evidence.order}); return; }
  if (!timestamp) batchLoss(context.publication, context.source, LostEvent::Cache, context.session, {0, evidence.order});
  for (auto& batch : c.batches)
    if (batch.session == context.session)
      for (size_t i = 0; i < batch.count; ++i)
      {
        auto& member = batch.members[i];
        if (!sameMember(member, context) || member.cachedUs || member.cacheBypassed) continue;
        batch.dirty = true;
        if (context.outcome == Outcome::Error || !timestamp)
        {
          member.mlFailed = true;
          if (context.outcome == Outcome::Error) batchOutcome(c, context, evidence);
          continue;
        }
        member.cachedUs = stored ? timestamp : 0;
        member.cacheEventUs = timestamp;
        member.cacheBypassed = !stored;
        member.cacheExact = member.lossBaseline == c.gaps.load();
      }
}
void retireBatchTile(ID map, ID source, uint8_t z, uint32_t x, uint32_t y, Retirement reason) noexcept
{
  if (!enabled() || !map || !source || reason == Retirement::None) return;
  auto& c = collector();
  const auto evidence = eventBoundary();
  const auto epoch = c.epoch.load();
  BatchWrite lock(c);
  if (!lock) { batchLoss(0, source, LostEvent::Outcome, epoch, evidence); return; }
  for (auto& batch : c.batches)
    if (batch.session == c.epoch.load())
      for (size_t i = 0; i < batch.count; ++i)
      {
        auto& member = batch.members[i];
        if (member.map != map || member.source != source || member.tile.z != z || member.tile.x != x ||
            member.tile.y != y || !earlierBoundary(evidence, member.retiredUs, member.retirementOrder)) continue;
        member.retiredUs = evidence.time;
        member.retirementOrder = evidence.order;
        member.retirement = reason;
        batch.dirty = true;
      }
}

void batchLayoutAccepted(const Context& original, bool noDrawNeeded, ID evaluatedView, const ViewTile* actualTile) noexcept
{
  const EventBoundary evidence{original.time[Layout], nextID()};
  auto context = original;
  if (actualTile)
  {
    context.x = actualTile->x; context.y = actualTile->y; context.z = actualTile->z;
    context.overscaledZ = actualTile->overscaledZ; context.wrap = actualTile->wrap;
  }
  auto& c = collector();
  if (!noDrawNeeded || !c.capture.load() || context.session != c.epoch.load()) return;
  BatchWrite lock(c);
  if (!lock) { batchLoss(context.publication, context.source, LostEvent::Screen, context.session, evidence); return; }
  const auto* view = currentView(c, context.map, context.source, context.session);
  if (!view || view->id != (evaluatedView ? evaluatedView : context.view) || view->gap != viewGap(context.source)) return;
  const auto index = viewIndex(*view, viewTile(context));
  if (index == view->count || view->invalidations[index] > context.view) return;
  if (context.kind == Kind::Layout && context.time[Layout])
    batchUse(c, context, 0, 0, true, view->requirements[index], evidence.order);
}

std::string batchSnapshotJSON(const BatchInfo& info, bool includeMembers)
{
  auto& c = collector();
  const auto sequence = nextID();
  Batch batch;
  const auto currentSession = c.epoch.load();
  const auto losses = c.gaps.load();
  LossLease lossLease(currentSession);
  {
    std::lock_guard<std::mutex> reader(c.snapshotMutex);
    for (size_t i = 0; i < c.publishedIndex.size(); ++i)
    {
      for (;;)
      {
        const auto index = c.publishedIndex[i].load();
        TILE_TRACE_TEST_HOOK("snapshot_index");
        auto& source = c.published[index];
        int free = 0;
        if (!source.readers.compare_exchange_strong(free, 1)) continue;
        if (c.publishedIndex[i].load() != index) { source.readers.store(0); continue; }
        TILE_TRACE_TEST_HOOK("snapshot_pinned");
        if (info.id && source.batch.id == info.id && source.batch.session == info.session)
        {
          batch.id = source.batch.id; batch.session = source.batch.session; batch.count = source.batch.count;
          std::copy_n(source.batch.members.begin(), batch.count, batch.members.begin());
        }
        source.readers.store(0);
        break;
      }
    }
  }
  if (!lossLease.capture()) return {};
  uint64_t visible = 0, drawn = 0, noDraw = 0, left = 0, lastSwap = 0, cached = 0, bypassed = 0, lastCache = 0;
  uint64_t mlExcluded = 0, screenExcluded = 0;
  const bool missing = batch.id != info.id || info.session != currentSession || batch.count != info.total;
  bool screenPartial = missing, mlPartial = missing;
  const char* mlReason = missing ? "membership" : "";
  const char* screenReason = missing ? "membership" : "";
  for (size_t i = 0; i < batch.count; ++i)
  {
    const auto& member = batch.members[i];
    const auto captureLoss = memberLoss(lossLease, member);
    const bool retired = member.retiredUs && (!member.failedUs || member.retiredUs < member.failedUs ||
      (member.retiredUs == member.failedUs && member.retirementOrder < member.failureOrder));
    const bool excludeML = retired && (!member.cacheEventUs || member.retiredUs < member.cacheEventUs) &&
      !lostBefore(captureLoss.cache, member.retiredUs);
    mlExcluded += excludeML;
    if (!excludeML)
    {
      cached += member.cachedUs != 0;
      bypassed += member.cacheBypassed;
      lastCache = std::max(lastCache, member.cachedUs);
      const bool failed = (member.mlFailed && !member.failedUs) || (member.failedUs && (!member.cacheEventUs || member.failedUs <= member.cacheEventUs));
      const bool gap = (member.cachedUs || member.cacheBypassed ? !member.cacheExact : member.lossBaseline != losses) ||
        lostBefore(captureLoss.cache, member.cacheEventUs);
      mlPartial |= failed || gap;
      if (!*mlReason && failed) mlReason = member.failure == Outcome::Pending ? "capture" : outcomeName(member.failure);
      if (!*mlReason && gap) mlReason = "capture";
    }
    if (!member.viewKnown || member.useOverflow)
    {
      screenPartial = true; if (!*screenReason) screenReason = "membership";
    }
    for (size_t j = 0; j < member.useCount; ++j)
    {
      const auto& use = member.uses[j];
      if (retired && (!use.failedUs || member.retiredUs < use.failedUs ||
          (member.retiredUs == use.failedUs && member.retirementOrder < use.failureOrder)) &&
          (!use.drawUs || member.retiredUs < use.drawUs) &&
          !lostBefore(captureLoss.screenEligibility, member.retiredUs) &&
          !lostBefore(captureLoss.view, member.retiredUs))
      { ++screenExcluded; continue; }
      ++visible;
      drawn += use.submittedUs != 0;
      noDraw += use.noDraw;
      left += use.leftUs != 0;
      lastSwap = std::max(lastSwap, use.submittedUs);
      const auto endpoint = use.submittedUs ? use.submittedUs : use.noDraw ? use.drawUs : use.leftUs;
      const auto viewEndpoint = use.drawUs ? use.drawUs : endpoint;
      const bool viewLost = lostBefore(captureLoss.view, viewEndpoint) &&
        (!viewEndpoint || captureLoss.view != viewEndpoint || !use.drawOrder || captureLoss.viewOrder <= use.drawOrder);
      const auto screenLoss = use.leftUs && !resolved(use) ? captureLoss.screenEligibility : captureLoss.screen;
      const bool gap = lostBefore(screenLoss, endpoint) || viewLost ||
        (!settled(use) && (member.viewGap != viewGap(member.source) || member.lossBaseline != losses));
      bool failed = false;
      if (resolved(use)) failed = !use.exact;
      else if (use.leftUs) failed = !use.exact || use.failed;
      else failed = use.unknown || use.failed || member.publicationFailed;
      const auto failureEndpoint = use.drawUs ? use.drawUs : use.leftUs;
      const auto precedesEndpoint = [&](uint64_t time, ID order)
      {
        return time && (!failureEndpoint || time < failureEndpoint ||
          (time == failureEndpoint && (!use.drawOrder || order <= use.drawOrder)));
      };
      // A delayed error can predate a draw that was already recorded as complete.
      failed |= precedesEndpoint(member.failedUs, member.failureOrder) ||
        (!resolved(use) && precedesEndpoint(use.failedUs, use.failureOrder));
      screenPartial |= failed || gap;
      if (!*screenReason && gap) screenReason = "capture";
      if (!*screenReason && failed) screenReason = use.failure != Outcome::Pending ? outcomeName(use.failure) :
        member.failure == Outcome::Pending ? "layout/view" : outcomeName(member.failure);
    }
  }
  const auto mlEligible = info.total - mlExcluded;
  const bool available = info.id && info.endedUs;
  const char* status = !available ? "unavailable" : screenPartial ? "partial" :
    !visible ? (screenExcluded ? "cancelled" : "offscreen") : drawn + noDraw + left < visible ? "waiting" :
    left == visible ? "departed" : !drawn ? "no-draw" : left ? "partial-left" : "complete";
  const char* mlStatus = !available ? "unavailable" : mlPartial ? "partial" : !mlEligible ? "cancelled" :
    bypassed ? "cache-off" : cached < mlEligible ? "waiting" : "complete";
  const auto timestamp = now();
  std::ostringstream out;
  out << "{\"version\":4,\"type\":\"batch\",\"available\":" << (available ? "true" : "false")
      << ",\"sequence\":\"" << sequence << "\",\"session\":\"" << info.session
      << "\",\"id\":\"" << info.id << "\",\"status\":\"" << status
      << "\",\"total\":" << info.total << ",\"cached\":" << info.cached
      << ",\"extracted\":" << info.extracted << ",\"visible\":" << visible
      << ",\"drawn\":" << drawn << ",\"noDraw\":" << noDraw << ",\"left\":" << left
      << ",\"batchMs\":" << info.batchMs
      << ",\"displayUs\":";
  if (available && !screenPartial && drawn && drawn + noDraw + left == visible && lastSwap >= info.startedUs)
    out << lastSwap - info.startedUs;
  else out << "null";
  out << ",\"mlStatus\":\"" << mlStatus << "\",\"mlStored\":" << cached << ",\"mlBypassed\":" << bypassed
      << ",\"mlUs\":";
  if (available && !mlPartial && !bypassed && cached == mlEligible && cached && lastCache >= info.startedUs)
    out << lastCache - info.startedUs;
  else out << "null";
  out << ",\"ageUs\":" << (info.startedUs && timestamp >= info.startedUs ? timestamp - info.startedUs : 0)
      << ",\"minTileMs\":" << info.minTileMs << ",\"maxTileMs\":" << info.maxTileMs
      << ",\"sumTileMs\":" << info.sumTileMs << ",\"z\":" << unsigned(info.z)
      << ",\"minX\":" << info.minX << ",\"maxX\":" << info.maxX
      << ",\"minY\":" << info.minY << ",\"maxY\":" << info.maxY
      << ",\"inFlight\":" << info.inFlight
      << ",\"captureLost\":" << lossLease.count
      << ",\"mlEligible\":" << mlEligible << ",\"mlExcluded\":" << mlExcluded
      << ",\"screenExcluded\":" << screenExcluded
      << ",\"mlReason\":\"" << mlReason << "\",\"screenReason\":\"" << screenReason << "\"";
  if (includeMembers)
  {
    out << ",\"members\":[";
    for (size_t i = 0; i < batch.count; ++i)
    {
      if (i) out << ',';
      const auto& member = batch.members[i];
      out << "{\"publication\":\"" << member.publication << "\",\"map\":\"" << member.map
          << "\",\"source\":\"" << member.source << "\",\"view\":\"" << member.view
          << "\",\"cachedUs\":\"" << member.cachedUs << "\",\"cacheBypassed\":" << member.cacheBypassed
          << ",\"retiredUs\":\"" << member.retiredUs << "\",\"retirement\":" << unsigned(member.retirement)
          << ",\"failure\":\"" << outcomeName(member.failure) << "\""
          << ",\"viewKnown\":" << member.viewKnown
          << ",\"mlFailed\":" << member.mlFailed << ",\"publicationFailed\":" << member.publicationFailed
          << ",\"uses\":[";
      for (size_t j = 0; j < member.useCount; ++j)
      {
        if (j) out << ',';
        const auto& use = member.uses[j];
        out << "{\"z\":" << unsigned(use.tile.z) << ",\"x\":" << use.tile.x << ",\"y\":" << use.tile.y
            << ",\"overscaledZ\":" << unsigned(use.tile.overscaledZ) << ",\"wrap\":" << use.tile.wrap
            << ",\"requirement\":\"" << use.requirement << "\",\"generation\":\"" << use.generation << "\",\"frame\":\"" << use.frame
            << "\",\"submittedUs\":\"" << use.submittedUs << "\",\"noDraw\":" << use.noDraw
            << ",\"leftUs\":\"" << use.leftUs << "\",\"unknown\":" << use.unknown
            << ",\"failure\":\"" << outcomeName(use.failure) << "\""
            << ",\"failed\":" << use.failed << ",\"originalFailed\":" << use.originalFailed
            << ",\"exact\":" << use.exact << '}';
      }
      out << "]}";
    }
    out << ']';
  }
  out << '}';
  if (currentSession != c.epoch.load()) return {};
  return out.str();
}

std::string snapshotJSON()
{
  auto& c = collector();
  std::array<Record, Capacity> records;
  std::array<Record, StartupCapacity> startup;
  std::array<Activity, 128> events;
  size_t startupCount;
  uint64_t evicted, started, snapshotSession, lost, gaps;
  bool capture;
  {
    std::lock_guard<std::mutex> lock(c.mutex);
    records = c.records; startup = c.startup; startupCount = c.startupCount;
    events = c.activity; evicted = c.evicted; started = c.started;
    snapshotSession = c.epoch.load(); lost = c.lost.load(); gaps = c.gaps.load(); capture = c.capture.load();
  }
  const auto timestamp = now();
  std::ostringstream out;
  out << "{\"version\":1,\"available\":true,\"enabled\":" << (capture ? "true" : "false")
      << ",\"session\":\"" << snapshotSession << "\",\"timestampUs\":\"" << timestamp
      << "\",\"startedUs\":\"" << started << "\",\"lost\":" << lost
      << ",\"memoryStatsTimestampUs\":\"" << c.memoryStatsTime.load()
      << "\",\"diskStatsTimestampUs\":\"" << c.diskStatsTime.load() << "\""
      << ",\"captureGaps\":" << gaps << ",\"evicted\":" << evicted << ",\"records\":[";
  bool first = true;
  auto emit = [&](const Record& record, bool early)
  {
    const auto& ctx = record.context;
    if (!ctx.id || ctx.session != snapshotSession) return;
    if (!first) out << ',';
    first = false;
    out << "{\"id\":\"" << ctx.id << "\",\"map\":\"" << ctx.map << "\",\"source\":\"" << ctx.source
        << "\",\"demand\":\"" << ctx.demand << "\",\"publication\":\"" << ctx.publication
        << "\",\"consumer\":\"" << ctx.consumer << "\",\"generation\":\"" << ctx.generation
        << "\",\"payloadSession\":\"" << ctx.payloadSession
        << "\",\"fingerprint\":\"" << ctx.fingerprint << "\",\"frame\":\"" << record.frame
        << "\",\"surface\":\"" << record.surface << "\",\"z\":" << unsigned(ctx.z)
        << ",\"mode\":" << record.mode << ",\"x\":" << ctx.x << ",\"y\":" << ctx.y << ",\"wrap\":" << ctx.wrap
        << ",\"overscaledZ\":" << unsigned(ctx.overscaledZ) << ",\"kind\":" << unsigned(ctx.kind) << ",\"role\":" << unsigned(ctx.role)
        << ",\"origin\":\"" << originName(ctx.origin) << "\",\"outcome\":\"" << outcomeName(ctx.outcome)
        << "\",\"early\":" << (early ? "true" : "false") << ",\"failedSwaps\":" << record.failedSwaps
        << ",\"symbol\":" << (record.symbol ? "true" : "false")
        << ",\"payloadFormat\":\"" << (ctx.payloadFormat == PayloadFormat::NativeGeometry ? "native-geometry" :
            ctx.payloadFormat == PayloadFormat::LegacyFeatures ? "legacy-features" : "unknown")
        << "\",\"conversion\":\"" << (!ctx.time[Loader] || !ctx.time[Converted] ? "unavailable" :
            ctx.payloadFormat == PayloadFormat::NativeGeometry ? "bypassed" :
            ctx.payloadFormat == PayloadFormat::LegacyFeatures ? "geojson-vt" : "unavailable")
        << "\",\"timesUs\":[";
    for (size_t i = 0; i < StageCount; ++i) { if (i) out << ','; out << '\"' << ctx.time[i] << '\"'; }
    out << "],\"firstUseComplete\":" << (record.firstUseComplete ? "true" : "false")
        << ",\"firstDrawComplete\":" << ((record.firstUseComplete ||
            (record.beforeFirstDraw && record.lossBaseline == lost + gaps && !record.historyLost)) ? "true" : "false")
        << ",\"submittedDrawUs\":\"" << record.submittedDraw
        << "\",\"geometrySubmittedUs\":\"" << record.contributionSubmit[0]
        << "\",\"symbolSubmittedUs\":\"" << record.contributionSubmit[1]
        << "\",\"geometryGeneration\":\"" << record.contributionGeneration[0]
        << "\",\"symbolGeneration\":\"" << record.contributionGeneration[1]
        << "\",\"elapsedUs\":[";
    for (const auto start : {Request, Features, Layout, Draw})
    {
      if (start != Request) out << ',';
      if (ctx.time[start] && ctx.time[Submitted] >= ctx.time[start]) out << ctx.time[Submitted] - ctx.time[start];
      else out << "null";
    }
    out << "],\"readyUs\":";
    if (ctx.time[Features] && ctx.time[Layout] >= ctx.time[Features]) out << ctx.time[Layout] - ctx.time[Features];
    else out << "null";
    out << ",\"firstDrawUs\":";
    if (ctx.time[Features] && ctx.time[Draw] >= ctx.time[Features]) out << ctx.time[Draw] - ctx.time[Features];
    else out << "null";
    out << ",\"partitionUs\":[";
    constexpr std::array<Stage, 7> boundaries{Features, Enqueued, Dispatched, Layout, Draw, SwapBegin, Submitted};
    uint64_t known = 0;
    for (size_t i = 1; i < boundaries.size(); ++i) {
      if (i > 1) out << ',';
      const auto a = ctx.time[boundaries[i - 1]], b = ctx.time[boundaries[i]];
      if (a && b >= a && ctx.time[Submitted] >= b && a >= ctx.time[Features]) {
        out << b - a;
        known += b - a;
      } else out << "null";
    }
    out << "],\"unknownUs\":";
    if (ctx.time[Features] && ctx.time[Submitted] >= ctx.time[Features] &&
        ctx.time[Submitted] - ctx.time[Features] >= known)
      out << ctx.time[Submitted] - ctx.time[Features] - known;
    else out << "null";
    out << "}";
  };
  for (const auto& record : records) {
    bool early = false;
    for (size_t i = 0; i < startupCount; ++i) early |= startup[i].context.id == record.context.id;
    emit(record, early);
  }
  for (size_t i = 0; i < startupCount; ++i)
    if (records[startup[i].context.id % Capacity].context.id != startup[i].context.id) emit(startup[i], true);
  out << "],\"activity\":[";
  first = true;
  for (const auto& event : events)
  {
    if (!event.time) continue;
    if (!first) out << ',';
    first = false;
    out << "[\"" << event.time << "\",\"" << event.id << "\",\"" << event.map << "\","
        << event.type << ',' << event.value << ']';
  }
  out << "]}";
  return out.str();
}
} // namespace mln::tiletrace
