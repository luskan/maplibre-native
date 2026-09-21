#pragma once

#include <mln/util/feature_selection.hpp>
#include <mln/util/paint_memo.hpp>
#include <mln/util/color_memo.hpp>

#include <atomic>
#include <mutex>
#include <unordered_map>

namespace mln {

struct RenderOptimizationPolicy
{
  featureselection::Policy selection;
  paintmemo::Policy paint;
  colormemo::Policy color;

  bool operator==(const RenderOptimizationPolicy& other) const noexcept
  {
    return selection.mode == other.selection.mode && selection.generation == other.selection.generation
      && paint == other.paint && color == other.color;
  }

  static RenderOptimizationPolicy requested() noexcept
  {
    return {featureselection::policy(), paintmemo::policy(), colormemo::policy()};
  }
};

// A source opts in explicitly. Accepted layouts are tracked without enabling timing capture.
class RenderOptimizationState
{
public:
  struct Snapshot
  {
    size_t required = 0, applied = 0, errors = 0;
    bool active() const noexcept { return required != 0 && required == applied && errors == 0; }
  };

  std::atomic<bool> enabled{true};

  void necessity(uint64_t token, bool required)
  {
    std::lock_guard lock(mutex);
    tiles[token].required = required;
  }

  void pending(uint64_t token, bool error = false)
  {
    std::lock_guard lock(mutex);
    auto& tile = tiles[token];
    tile.accepted = false;
    tile.error = error;
  }

  void accept(uint64_t token, RenderOptimizationPolicy policy)
  {
    std::lock_guard lock(mutex);
    auto& tile = tiles[token];
    tile.policy = policy;
    tile.accepted = true;
    tile.error = false;
  }

  void remove(uint64_t token)
  {
    std::lock_guard lock(mutex);
    tiles.erase(token);
  }

  Snapshot snapshot(RenderOptimizationPolicy requested) const
  {
    std::lock_guard lock(mutex);
    Snapshot result;
    for (const auto& entry : tiles)
    {
      const auto& tile = entry.second;
      if (!tile.required) continue;
      ++result.required;
      result.applied += enabled.load() && tile.accepted && tile.policy == requested;
      result.errors += tile.error;
    }
    return result;
  }

private:
  struct TileState
  {
    RenderOptimizationPolicy policy;
    bool required = false, accepted = false, error = false;
  };
  mutable std::mutex mutex;
  std::unordered_map<uint64_t, TileState> tiles;
};

} // namespace mln
