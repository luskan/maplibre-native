#pragma once

#include <mln/actor/scheduler.hpp>
#include <mln/storage/native_tile_store.hpp>
#include <mln/tile/native_tile_codec.hpp>
#include <mln/tile/native_tile_memory.hpp>

#include <functional>
#include <memory>
#include <string>

namespace mln
{

class NativeTileCache
{
  struct State;

public:
  class Extraction
  {
  public:
    ~Extraction();
    Extraction(const Extraction&) = delete;
    Extraction& operator=(const Extraction&) = delete;
    void complete();

  private:
    friend class NativeTileCache;
    explicit Extraction(std::shared_ptr<State>);
    void finish(bool);
    std::shared_ptr<State> state;
  };

  struct Config
  {
    NativeTileCodecLimits codec;
    NativeTileStoreLimits store;
    size_t chargedBytes = 128 * 1024 * 1024;
    size_t retainedBytes = 64 * 1024 * 1024;
    size_t maxReads = 32;
    size_t maxWrites = 1;
    size_t maxDeferredWrites = 32;
    size_t deferredWriteBytes = 256 * 1024;
  };

  struct Policy
  {
    uint64_t generation = 0;
    bool enabled = false;
    bool diskEnabled = false;
    std::string nameSpace;
  };

  using Validity = std::function<bool()>;
  using Reply = std::function<void(NativeTilePayloadPtr)>;
  using OriginReply = std::function<void(NativeTilePayloadPtr, bool)>;
  using Completion = std::function<void(bool)>;
  using Factory = std::function<std::unique_ptr<NativeTileStore>(const std::string&, NativeTileStoreLimits)>;
  using BackgroundWork = std::function<bool(NativeTileStore*)>;

  NativeTileCache(Config, Factory, std::shared_ptr<Scheduler> = {});
  ~NativeTileCache();
  NativeTileCache(const NativeTileCache&) = delete;
  NativeTileCache& operator=(const NativeTileCache&) = delete;

  Policy configure(std::string path, std::string nameSpace, bool enabled, bool diskEnabled);
  Policy policy() const;
  // This reports ownership, even when a failed service cannot accept cache work.
  bool enabled() const noexcept;
  NativeTilePayloadPtr get(const std::string& descriptor, const NativeTileMetadata&, const Policy&);
  bool remember(const std::string& descriptor, NativeTilePayloadPtr, const Policy&);

  // Admission rejection replies immediately, while accepted work replies on the background scheduler.
  // Callers always marshal replies to their owning actor.
  void read(std::string descriptor, NativeTileMetadata, Policy, Validity, Reply);
  void readWithOrigin(std::string descriptor, NativeTileMetadata, Policy, Validity, OriginReply);
  void persist(std::string descriptor, NativeTilePayloadPtr, Policy, Validity = {});
  std::shared_ptr<NativeTileMemoryLease> reservePrepared(size_t);
  Extraction beginExtraction();

  bool clear(bool disk, Completion = {});
  void trim();
  void resume();
  // Each invocation performs one bounded chunk. True requests another low-priority turn.
  void postBackground(Policy, BackgroundWork, Completion = {});
  std::string snapshotJSON() const;

private:
  std::shared_ptr<State> state;
};

} // namespace mln
