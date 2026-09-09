#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace mln::tiletrace {

using ID = uint64_t;
enum Stage : uint8_t
{
  Request, Producer, Worker, Features, Enqueued, Dispatched, Loader, Converted,
  Delivered, Layout, Draw, FrameEnd, SwapBegin, Submitted, StageCount
};
enum class Origin : uint8_t { Fresh, Memory, Disk, Processed, Renderer, Unknown };
enum class Outcome : uint8_t
{
  Pending, Submitted, Empty, Cancelled, Superseded, NoReceiver, StaleFast,
  StaleWorker, StaleSubmit, Overflow, StaleDrain, Error, Teardown, Coalesced, Truncated
};
enum class Kind : uint8_t { Demand, Publication, Layout };
enum class Retirement : uint8_t { None, Cancelled, NoDemand, Invalidated, CacheClear, Shutdown };

struct Context
{
  ID session = 0, map = 0, source = 0, id = 0, demand = 0, publication = 0;
  ID consumer = 0, generation = 0, fingerprint = 0, payloadSession = 0, view = 0;
  uint32_t x = 0, y = 0;
  int16_t wrap = 0;
  uint8_t z = 0, overscaledZ = 0, role = 0;
  Kind kind = Kind::Demand;
  Origin origin = Origin::Unknown;
  Outcome outcome = Outcome::Pending;
  bool empty = false;
  std::array<uint64_t, StageCount> time{};
};

#ifdef TILE_TRACE_TESTING
void setTestTime(uint64_t) noexcept;
void setTestDropViews(bool) noexcept;
#endif
ID nextID() noexcept;
uint64_t now() noexcept;
ID session() noexcept;
bool enabled() noexcept;
void configure(bool capture, bool reset = false);
Context create(ID map, ID source, ID consumer, uint8_t z, uint32_t x, uint32_t y,
               uint8_t overscaledZ, int16_t wrap, uint8_t role = 0, ID view = 0);
void mark(Context&, Stage) noexcept;
void finish(Context&, Outcome) noexcept;
void bindDemand(const Context&) noexcept;
void retireSource(ID source) noexcept;
void pump(ID map, bool begin) noexcept;
void frontend(ID map, bool begin) noexcept;
void loss() noexcept;
void cacheSnapshotTime(bool disk, uint64_t timestamp) noexcept;
std::string snapshotJSON();

struct BatchInfo
{
  ID session = 0, id = 0;
  uint64_t startedUs = 0, endedUs = 0;
  uint64_t total = 0, cached = 0, extracted = 0;
  int64_t batchMs = 0, minTileMs = 0, maxTileMs = 0, sumTileMs = 0;
  uint32_t minX = 0, maxX = 0, minY = 0, maxY = 0;
  uint8_t z = 0;
  int inFlight = 0;
};

void trackBatchMember(const BatchInfo&, const Context&) noexcept;
void batchCacheAdmission(const Context&, bool stored, uint64_t timestamp) noexcept;
void retireBatchTile(ID map, ID source, uint8_t z, uint32_t x, uint32_t y, Retirement) noexcept;
struct ViewTile;
void batchLayoutAccepted(const Context&, bool noDrawNeeded, ID evaluatedView = 0,
                         const ViewTile* actualTile = nullptr) noexcept;
std::string batchSnapshotJSON(const BatchInfo&, bool includeMembers = false);

struct ViewTile
{
  uint32_t x = 0, y = 0;
  int16_t wrap = 0;
  uint8_t z = 0, overscaledZ = 0;
  bool operator==(const ViewTile&) const noexcept;
  bool operator<(const ViewTile&) const noexcept;
};
constexpr size_t ViewTileLimit = 256;
uint64_t viewSerial() noexcept;
ID updateView(ID map, ID source, ID style, const ViewTile*, size_t count) noexcept;
struct ViewTileRange
{
  uint32_t minX = 0, minY = 0, maxX = 0, maxY = 0;
  uint8_t minZ = 0, maxZ = 0;
};
void invalidateView(ID map, ID source, const ViewTileRange* range = nullptr) noexcept;

// The scope belongs to one outer surface render, including all contributing maps.
class FrameScope
{
public:
  explicit FrameScope(int mode) noexcept;
  ~FrameScope();
  FrameScope(const FrameScope&) = delete;
  FrameScope& operator=(const FrameScope&) = delete;
};
void draw(const Context&, bool symbol, const ViewTile* actualTile = nullptr) noexcept;
void frameEnd() noexcept;
void swapBegin(uintptr_t surface) noexcept;
void swapEnd(bool success, int error = 0) noexcept;
void surfaceCreated(uintptr_t surface) noexcept;
void surfaceDestroyed(uintptr_t surface) noexcept;

} // namespace mln::tiletrace
