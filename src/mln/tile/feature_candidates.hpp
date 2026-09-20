#pragma once

#include <mln/util/feature_selection.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace mln {
namespace style {
class Filter;
namespace expression { class EvaluationContext; }
}

struct FeatureCandidateLimits
{
  size_t depth = 12, nodes = 64, atoms = 512, features = 1000000;
  size_t postingBytes = 8 * 1024 * 1024, scratchBytes = 16 * 1024 * 1024;
};

struct FeatureCandidates
{
  size_t fullCount = 0;
  std::optional<std::vector<uint32_t>> indices;
  size_t size() const noexcept { return indices ? indices->size() : fullCount; }
  size_t operator[](size_t position) const noexcept { return indices ? (*indices)[position] : position; }
};

class FeatureSelection
{
public:
  virtual ~FeatureSelection() = default;
  virtual FeatureCandidates candidates(const style::Filter&, const style::expression::EvaluationContext&) = 0;
};

inline FeatureCandidates selectFeatureCandidates(FeatureSelection* selection, size_t count,
  const style::Filter& filter, const style::expression::EvaluationContext& context,
  featureselection::Statistics* statistics)
{
  if (selection) return selection->candidates(filter, context);
  if (statistics) ++statistics->fallbackGroups;
  return {count, std::nullopt};
}

} // namespace mln
