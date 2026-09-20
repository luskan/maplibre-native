#include <mln/tile/native_geometry_tile_data.hpp>

#include <cassert>
#include <stdexcept>
#include <utility>

namespace mln
{

NativeGeometryTileFeature::NativeGeometryTileFeature(const NativeFeatureRecord& feature) : feature_(feature) {}

FeatureType NativeGeometryTileFeature::getType() const
{
  return feature_.type;
}

std::optional<Value> NativeGeometryTileFeature::getValue(const std::string& key) const
{
  const auto found = feature_.properties.find(key);
  return found != feature_.properties.end() ? std::optional<Value>(found->second) : std::nullopt;
}

const PropertyMap& NativeGeometryTileFeature::getProperties() const
{
  return feature_.properties;
}

FeatureIdentifier NativeGeometryTileFeature::getID() const
{
  return feature_.id;
}

const GeometryCollection& NativeGeometryTileFeature::getGeometries() const
{
  return feature_.geometry;
}

NativeGeometryTileLayer::NativeGeometryTileLayer(NativeTilePayloadPtr payload) : payload_(std::move(payload))
{
  if (!payload_)
  {
    throw std::invalid_argument("Native geometry layer requires a payload");
  }
}

std::size_t NativeGeometryTileLayer::featureCount() const
{
  return payload_->features().size();
}

std::unique_ptr<GeometryTileFeature> NativeGeometryTileLayer::getFeature(std::size_t i) const
{
  assert(i < featureCount());
  return std::make_unique<NativeGeometryTileFeature>(payload_->features()[i]);
}

std::string NativeGeometryTileLayer::getName() const
{
  return "";
}

NativeGeometryTileData::NativeGeometryTileData(NativeTilePayloadPtr payload) : payload_(std::move(payload))
{
  if (!payload_)
  {
    throw std::invalid_argument("Native geometry data requires a payload");
  }
}

std::unique_ptr<GeometryTileData> NativeGeometryTileData::clone() const
{
  auto copy = std::make_unique<NativeGeometryTileData>(payload_);
  copy->trace = trace;
  return copy;
}

std::unique_ptr<GeometryTileLayer> NativeGeometryTileData::getLayer(const std::string&) const
{
  return std::make_unique<NativeGeometryTileLayer>(payload_);
}

}  // namespace mln

#include <mln/style/filter.hpp>
#include <mln/style/expression/literal.hpp>
#include <mln/util/layout_timing.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>

namespace mln
{
namespace
{
using Expr = style::expression::Expression;
using ExprKind = style::expression::Kind;
constexpr std::array<const char*, 5> CandidateKeys{"rc", "cat", "lt", "pc", "mt"};
struct CandidateLimit {};

struct CandidateConstraint
{
  enum class Kind { Equal, All, Any } kind = Kind::Equal;
  size_t key = 0;
  double value = 0;
  std::vector<CandidateConstraint> children;
};

std::vector<const Expr*> candidateChildren(const Expr& expression, size_t limit)
{
  std::vector<const Expr*> children;
  expression.eachChild([&](const Expr& child)
  {
    if (children.size() >= limit) throw CandidateLimit{};
    children.push_back(&child);
  });
  return children;
}

std::optional<CandidateConstraint> candidateEquality(const Expr& property, const Expr& literal)
{
  if (property.getKind() != ExprKind::CompoundExpression || property.getOperator() != "get"
      || literal.getKind() != ExprKind::Literal) return std::nullopt;
  const auto operands = candidateChildren(property, 2);
  if (operands.size() != 1 || operands[0]->getKind() != ExprKind::Literal) return std::nullopt;
  const auto& name = static_cast<const style::expression::Literal&>(*operands[0]).getValue();
  const auto& value = static_cast<const style::expression::Literal&>(literal).getValue();
  if (!name.is<std::string>() || !value.is<double>() || !std::isfinite(value.get<double>())) return std::nullopt;
  const auto found = std::find(CandidateKeys.begin(), CandidateKeys.end(), name.get<std::string>());
  if (found == CandidateKeys.end()) return std::nullopt;
  CandidateConstraint result;
  result.key = std::distance(CandidateKeys.begin(), found);
  result.value = value.get<double>();
  return result;
}

std::optional<CandidateConstraint> compileCandidate(const Expr& expression, const FeatureCandidateLimits& limits,
                                                   size_t depth, size_t& nodes)
{
  if (depth > limits.depth || ++nodes > limits.nodes) throw CandidateLimit{};
  const auto kind = expression.getKind();
  if (kind != ExprKind::Comparison && kind != ExprKind::All && kind != ExprKind::Any) return std::nullopt;
  const auto children = candidateChildren(expression, limits.nodes);
  if (kind == ExprKind::Comparison)
  {
    if (expression.getOperator() != "==" || children.size() != 2) return std::nullopt;
    if (auto result = candidateEquality(*children[0], *children[1])) return result;
    return candidateEquality(*children[1], *children[0]);
  }
  CandidateConstraint result;
  result.kind = kind == ExprKind::All ? CandidateConstraint::Kind::All : CandidateConstraint::Kind::Any;
  for (const auto* child : children)
  {
    auto part = compileCandidate(*child, limits, depth + 1, nodes);
    if (part) result.children.push_back(std::move(*part));
    else if (kind == ExprKind::Any) return std::nullopt;
  }
  if (result.children.empty()) return std::nullopt;
  return result;
}

std::optional<double> candidateNumber(const Value& value)
{
  double result;
  if (value.is<int64_t>()) result = static_cast<double>(value.get<int64_t>());
  else if (value.is<uint64_t>()) result = static_cast<double>(value.get<uint64_t>());
  else if (value.is<double>()) result = value.get<double>();
  else return std::nullopt;
  return std::isfinite(result) ? std::optional<double>(result) : std::nullopt;
}

class CandidateClock
{
public:
  CandidateClock(bool enabled, uint64_t& wall_, uint64_t& cpu_, bool& valid_)
    : active(enabled), wall(wall_), cpu(cpu_), valid(valid_), start(active ? layouttiming::sample() : layouttiming::Stamp{}) {}
  ~CandidateClock()
  {
    if (!active) return;
    const auto end = layouttiming::sample();
    if (end.wall >= start.wall) wall += end.wall - start.wall;
    else valid = false;
    if (start.cpuValid && end.cpuValid && end.cpu >= start.cpu) cpu += end.cpu - start.cpu;
    else valid = false;
  }
private:
  bool active;
  uint64_t& wall;
  uint64_t& cpu;
  bool& valid;
  layouttiming::Stamp start;
};

class NativeFeatureSelection final : public FeatureSelection
{
public:
  NativeFeatureSelection(NativeTilePayloadPtr payload_, featureselection::Statistics& statistics_, bool observe_,
                         FeatureCandidateLimits limits_)
    : payload(std::move(payload_)), statistics(statistics_), observe(observe_), limits(limits_) {}

  bool build(const std::vector<const style::Filter*>& filters)
  {
    if (payload->features().size() > limits.features || payload->features().size() > UINT32_MAX)
      throw CandidateLimit{};
    size_t atoms = 0;
    for (const auto* filter : filters)
    {
      if (!filter->expression) continue;
      std::optional<CandidateConstraint> constraint;
      try
      {
        size_t nodes = 0;
        constraint = compileCandidate(**filter->expression, limits, 0, nodes);
      }
      catch (const CandidateLimit&) { ++statistics.limitFallbacks; }
      if (!constraint) continue;
      ++statistics.eligibleGroups;
      addAtoms(*constraint, atoms);
      constraints.emplace(filter->expression->get(), std::move(*constraint));
    }
    if (statistics.eligibleGroups < 2 || constraints.empty()) return false;
    for (size_t i = 0; i < payload->features().size(); ++i)
      for (size_t key = 0; key < postings.size(); ++key)
      {
        auto& values = postings[key];
        if (values.empty()) continue;
        const auto& properties = payload->features()[i].properties;
        const auto property = properties.find(CandidateKeys[key]);
        if (property == properties.end()) continue;
        const auto number = candidateNumber(property->second);
        if (!number) continue;
        const auto found = values.find(*number);
        if (found == values.end()) continue;
        auto& indices = found->second;
        if (indices.size() == indices.capacity())
        {
          const size_t capacity = indices.capacity() ? indices.capacity() * 2 : 8;
          const size_t extra = (capacity - indices.capacity()) * sizeof(uint32_t);
          if (extra > limits.postingBytes || statistics.postingCapacityBytes > limits.postingBytes - extra)
            throw CandidateLimit{};
          indices.reserve(capacity);
          statistics.postingCapacityBytes += extra;
        }
        indices.push_back(static_cast<uint32_t>(i));
      }
    ++statistics.builds;
    return true;
  }

  FeatureCandidates candidates(const style::Filter& filter,
                               const style::expression::EvaluationContext& context) override
  {
    CandidateClock clock(observe, statistics.queryUs, statistics.queryCpuUs, statistics.cpuAvailable);
    const auto fallback = [&]()
    {
      ++statistics.fallbackGroups;
      return FeatureCandidates{payload->features().size(), std::nullopt};
    };
    const auto found = filter.expression ? constraints.find(filter.expression->get()) : constraints.end();
    if (found == constraints.end()) return fallback();
    try
    {
      std::vector<uint32_t> indices;
      collect(found->second, indices);
      std::sort(indices.begin(), indices.end());
      indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
      statistics.scratchCapacityBytes = std::max(statistics.scratchCapacityBytes,
                                                 uint64_t(indices.capacity() * sizeof(uint32_t)));
      if (indices.size() == payload->features().size()) return fallback();
      if (statistics.applied.mode == featureselection::Mode::Verify)
      {
        CandidateClock verification(observe, statistics.verifyUs, statistics.verifyCpuUs, statistics.cpuAvailable);
        if (!verify(filter, context, indices))
        {
          ++statistics.verificationFailures;
          return fallback();
        }
        ++statistics.verifiedGroups;
      }
      ++statistics.indexedGroups;
      return {payload->features().size(), std::move(indices)};
    }
    catch (const CandidateLimit&) { ++statistics.limitFallbacks; }
    catch (const std::bad_alloc&) { ++statistics.limitFallbacks; }
    return fallback();
  }

private:
  void addAtoms(const CandidateConstraint& constraint, size_t& atoms)
  {
    if (constraint.kind == CandidateConstraint::Kind::Equal)
    {
      if (++atoms > limits.atoms) throw CandidateLimit{};
      postings[constraint.key].try_emplace(constraint.value);
    }
    else for (const auto& child : constraint.children) addAtoms(child, atoms);
  }
  size_t estimate(const CandidateConstraint& constraint) const
  {
    if (constraint.kind == CandidateConstraint::Kind::Equal)
      return postings[constraint.key].at(constraint.value).size();
    size_t count = constraint.kind == CandidateConstraint::Kind::All ? payload->features().size() : 0;
    for (const auto& child : constraint.children)
    {
      const auto size = estimate(child);
      if (constraint.kind == CandidateConstraint::Kind::All) count = std::min(count, size);
      else count = std::min(payload->features().size(), count + size);
    }
    return count;
  }
  void collect(const CandidateConstraint& constraint, std::vector<uint32_t>& output) const
  {
    if (constraint.kind == CandidateConstraint::Kind::Equal)
    {
      const auto& values = postings[constraint.key].at(constraint.value);
      if (values.size() > limits.scratchBytes / sizeof(uint32_t)
          || output.size() > limits.scratchBytes / sizeof(uint32_t) - values.size()) throw CandidateLimit{};
      output.reserve(output.size() + values.size());
      statistics.scratchCapacityBytes = std::max(statistics.scratchCapacityBytes,
                                                 uint64_t(output.capacity() * sizeof(uint32_t)));
      output.insert(output.end(), values.begin(), values.end());
    }
    else if (constraint.kind == CandidateConstraint::Kind::All)
    {
      const auto best = std::min_element(constraint.children.begin(), constraint.children.end(),
        [&](const auto& a, const auto& b) { return estimate(a) < estimate(b); });
      collect(*best, output);
    }
    else for (const auto& child : constraint.children) collect(child, output);
  }
  bool matches(size_t index, const style::Filter& filter, style::expression::EvaluationContext context) const
  {
    NativeGeometryTileFeature feature(payload->features()[index]);
    context.feature = &feature;
    return filter(context);
  }
  bool verify(const style::Filter& filter, const style::expression::EvaluationContext& context,
              const std::vector<uint32_t>& indices) const
  {
    size_t candidate = 0;
    const auto next = [&]() -> std::optional<uint32_t>
    {
      while (candidate < indices.size())
      {
        const auto index = indices[candidate++];
        if (matches(index, filter, context)) return index;
      }
      return std::nullopt;
    };
    auto match = next();
    for (size_t i = 0; i < payload->features().size(); ++i)
    {
      if (!matches(i, filter, context)) continue;
      if (!match || *match != i) return false;
      match = next();
    }
    return !match;
  }

  NativeTilePayloadPtr payload;
  featureselection::Statistics& statistics;
  bool observe;
  FeatureCandidateLimits limits;
  std::array<std::map<double, std::vector<uint32_t>>, CandidateKeys.size()> postings;
  std::map<const Expr*, CandidateConstraint> constraints;
};
} // namespace

std::unique_ptr<FeatureSelection> NativeGeometryTileData::createFeatureSelection(
    const std::vector<const style::Filter*>& filters, featureselection::Statistics& statistics,
    bool observe, const FeatureCandidateLimits& limits) const
{
  if (statistics.applied.mode == featureselection::Mode::FullScan) return {};
  CandidateClock clock(observe, statistics.buildUs, statistics.buildCpuUs, statistics.cpuAvailable);
  try
  {
    auto result = std::make_unique<NativeFeatureSelection>(payload_, statistics, observe, limits);
    if (result->build(filters)) return result;
  }
  catch (const CandidateLimit&) { ++statistics.limitFallbacks; }
  catch (const std::bad_alloc&) { ++statistics.limitFallbacks; }
  return {};
}
} // namespace mln
