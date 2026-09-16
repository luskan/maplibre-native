#pragma once

#include <mln/util/tile_trace.hpp>

#include <array>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>

namespace mln::layouttiming {

enum class Phase : uint8_t { Parse, Finalize, Callback, Count };
enum class Gap : uint8_t { Inputs, Coalescing, Dependencies, Idle, Count };
enum class Disposition : uint8_t
{
  Accepted, CorrelationRejected, InputRejected, Replaced, Reset, Error, Obsolete, Destroyed, None
};

struct Stamp
{
  uint64_t wall = 0, cpu = 0;
  bool cpuValid = false;
};

struct Measure
{
  uint64_t wall = 0, cpu = 0, calls = 0;
  bool cpuValid = true;
};

struct GapMeasure
{
  uint64_t wall = 0, intervals = 0, pendingIntervals = 0;
  uint32_t stateMask = 0;
};

struct Group
{
  std::array<char, 80> name{};
  uint64_t parseOrdinal = 0, features = 0;
  Measure work;
  bool nameTruncated = false, layoutRequired = false;
};

struct Seed
{
  uint64_t generation = 0, session = 0, inputId = 0, correlation = 0;
  uint64_t delivered = 0, queued = 0, captureGeneration = 0;
};

struct Profile
{
  Seed seed;
  uint64_t received = 0, posted = 0, ended = 0;
  uint64_t resultOrdinal = 0, resultsBefore = 0, groupsSeen = 0;
  std::array<Measure, static_cast<size_t>(Phase::Count)> work{};
  std::array<GapMeasure, static_cast<size_t>(Gap::Count)> gaps{};
  std::array<Group, 8> groups{};
  size_t groupCount = 0;
  bool valid = true;
};

bool enabled() noexcept;
void configure(bool enable, bool reset = false) noexcept;
Seed makeSeed(const tiletrace::Context&, uint64_t correlation) noexcept;
Stamp sample() noexcept;
void publish(const tiletrace::Context&, const Profile&, Disposition, uint64_t ownerReceived = 0,
             uint64_t ownerCorrelation = 0, uint64_t resultCorrelation = 0) noexcept;
std::string snapshotJSON(uint64_t captureSession);

class Tracker
{
public:
  void start(Seed seed, uint64_t received) noexcept;
  bool active() const noexcept;
  void enter(uint64_t at) noexcept;
  void leave(uint64_t at, Gap, unsigned state, bool pending) noexcept;
  void add(Phase, Stamp start, Stamp end) noexcept;
  void addGroup(Group) noexcept;
  void invalidate() noexcept { value.valid = false; }
  uint64_t nextParseOrdinal() const noexcept { return value.work[0].calls + 1; }
  void terminate(Disposition reason) noexcept { terminal = reason; }
  Disposition terminalReason() const noexcept { return terminal; }
  Profile result(uint64_t posted) noexcept;
  Profile finish(uint64_t ended) noexcept;
  void clear() noexcept { *this = {}; }

private:
  void closeGap(uint64_t at) noexcept;
  Profile value;
  uint64_t gapStart = 0, resultCount = 0;
  unsigned depth = 0;
  Gap gap = Gap::Idle;
  Disposition terminal = Disposition::None;
};

class WorkScope
{
public:
  WorkScope(Tracker&, Phase) noexcept;
  ~WorkScope();
  void finish() noexcept;
  WorkScope(const WorkScope&) = delete;
  WorkScope& operator=(const WorkScope&) = delete;

private:
  Tracker* tracker;
  Phase phase;
  Stamp start;
  int exceptions;
};

class GroupScope
{
public:
  GroupScope(Tracker&, std::string_view name, uint64_t features, bool required) noexcept;
  ~GroupScope();
  GroupScope(const GroupScope&) = delete;
  GroupScope& operator=(const GroupScope&) = delete;

private:
  Tracker* tracker;
  Stamp start;
  Group group;
};

} // namespace mln::layouttiming
