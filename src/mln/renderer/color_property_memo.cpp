#include <mln/renderer/color_property_memo.hpp>
#include <mln/style/expression/literal.hpp>
#include <mln/tile/geometry_tile_data.hpp>

#include <cmath>
#include <cstring>

namespace mln::colormemo {
namespace {

struct StopAnalysis {};

struct Inputs
{
  std::array<const std::string*, 2> names{};
  size_t nameCount = 0, nodes = 0;

  void visit(const style::expression::Expression& expression, size_t depth = 0)
  {
    using namespace style::expression;
    if (++nodes > 512 || depth > 32 || expression.has(~Dependency::Feature)) throw StopAnalysis{};
    const auto op = expression.getOperator();
    switch (expression.getKind())
    {
      case Kind::Literal:
      case Kind::Match:
      case Kind::Case:
      case Kind::Comparison:
        break;
      case Kind::Assertion:
        if (op != "number") throw StopAnalysis{};
        break;
      case Kind::CompoundExpression:
      {
        if (op != "get" && op != "has") throw StopAnalysis{};
        const Expression* key = nullptr;
        size_t children = 0;
        expression.eachChild([&](const Expression& child) {
          if (++children > 1) throw StopAnalysis{};
          key = &child;
        });
        if (!key || key->getKind() != Kind::Literal) throw StopAnalysis{};
        const auto& value = static_cast<const Literal&>(*key).getValue();
        if (!value.is<std::string>()) throw StopAnalysis{};
        const auto& name = value.get<std::string>();
        if (name.size() > 64) throw StopAnalysis{};
        size_t i = 0;
        while (i < nameCount && *names[i] != name) ++i;
        if (i == nameCount)
        {
          if (nameCount == names.size()) throw StopAnalysis{};
          names[nameCount++] = &name;
        }
        break;
      }
      default:
        throw StopAnalysis{};
    }
    expression.eachChild([&](const Expression& child) { visit(child, depth + 1); });
  }
};

Color original(const style::PropertyExpression<Color>& expression, Color defaultValue,
               const GeometryTileFeature& feature, const CanonicalTileID& canonical,
               const style::expression::Value& formattedSection)
{
  if (auto* stats = current()) ++stats->evaluations;
  return expression.evaluate(style::expression::EvaluationContext(&feature)
    .withFormattedSection(&formattedSection).withCanonicalTileID(&canonical), defaultValue);
}

bool finite(Color value)
{
  return std::isfinite(value.r) && std::isfinite(value.g) && std::isfinite(value.b) && std::isfinite(value.a);
}

bool sameBits(Color a, Color b)
{
  return std::memcmp(&a.r, &b.r, sizeof(float)) == 0 && std::memcmp(&a.g, &b.g, sizeof(float)) == 0
    && std::memcmp(&a.b, &b.b, sizeof(float)) == 0 && std::memcmp(&a.a, &b.a, sizeof(float)) == 0;
}

} // namespace

std::unique_ptr<Cache> Cache::create(const style::PropertyExpression<Color>& expression, Color defaultValue)
{
  auto* stats = current();
  if (!stats) return {};
  ++stats->binders;
  if (stats->applied.mode == Mode::Off) return {};
  Inputs inputs;
  try { inputs.visit(expression.getExpression()); }
  catch (const StopAnalysis&) { return {}; }
  catch (const std::bad_alloc&) { ++stats->allocationFallbacks; return {}; }
  if (!inputs.nameCount || !expression.isZoomConstant()) return {};
  ++stats->eligible;
  if (!reserve(sizeof(Cache))) { ++stats->budgetFallbacks; return {}; }
  try
  {
    auto result = std::unique_ptr<Cache>(new Cache(expression, defaultValue, stats->applied,
                                                  inputs.names, inputs.nameCount));
    ++stats->admitted;
    stats->allocatedBytes += sizeof(Cache);
    return result;
  }
  catch (const std::bad_alloc&)
  {
    release(sizeof(Cache));
    ++stats->allocationFallbacks;
    return {};
  }
}

Cache::Cache(const style::PropertyExpression<Color>& expression_, Color defaultValue_, Policy applied_,
             std::array<const std::string*, 2> names_, size_t nameCount_)
  : expression(expression_), defaultValue(defaultValue_), applied(applied_), names(names_), nameCount(nameCount_) {}

Cache::~Cache() { release(sizeof(Cache)); }

bool Cache::makeKey(const GeometryTileFeature& feature, Key& key) const
{
  for (size_t i = 0; i < nameCount; ++i)
  {
    const auto value = feature.getValue(*names[i]);
    if (!value) continue;
    if (value->is<NullValue>()) key[i].type = 1;
    else if (value->is<bool>()) key[i] = {value->get<bool>() ? 1u : 0u, 2};
    else
    {
      const auto number = numericValue<double>(*value);
      if (!number || !std::isfinite(*number)) return false;
      key[i].type = 3;
      std::memcpy(&key[i].bits, &*number, sizeof(double));
    }
  }
  return true;
}

Color Cache::hit(const Entry& entry, Statistics& stats, const GeometryTileFeature& feature,
                 const CanonicalTileID& canonical, const style::expression::Value& formattedSection)
{
  last = &entry;
  ++stats.hits;
  if (applied.mode == Mode::Verify)
  {
    ++stats.verifiedHits;
    const auto result = original(expression, defaultValue, feature, canonical, formattedSection);
    if (!sameBits(result, entry.value))
    {
      ++stats.mismatches;
      disabled = true;
    }
    return result;
  }
  return entry.value;
}

Color Cache::evaluate(const GeometryTileFeature& feature, const CanonicalTileID& canonical,
                      const style::expression::Value& formattedSection)
{
  auto* stats = current();
  if (!stats || !(stats->applied == applied) || disabled)
    return evaluateUncached(expression, defaultValue, feature, canonical, formattedSection);
  ++stats->calls;
  Key key{};
  if (!makeKey(feature, key))
  {
    ++stats->bypasses;
    return original(expression, defaultValue, feature, canonical, formattedSection);
  }
  if (last && last->key == key) return hit(*last, *stats, feature, canonical, formattedSection);
  uint64_t hash = 0;
  for (const auto& part : key)
  {
    uint64_t bits = part.bits + part.type * 0x9e3779b97f4a7c15ULL;
    bits = (bits ^ (bits >> 30)) * 0xbf58476d1ce4e5b9ULL;
    bits = (bits ^ (bits >> 27)) * 0x94d049bb133111ebULL;
    hash = (hash << 7) ^ (hash >> 57) ^ bits ^ (bits >> 31);
  }
  size_t slot = hash % entries.size();
  for (size_t probe = 0; probe < entries.size(); ++probe, slot = (slot + 1) % entries.size())
  {
    auto& entry = entries[slot];
    if (!entry.used)
    {
      ++stats->misses;
      const auto result = original(expression, defaultValue, feature, canonical, formattedSection);
      if (count == 32)
      {
        disabled = true;
        ++stats->capacityFallbacks;
      }
      else if (finite(result))
      {
        entry = {key, result, true};
        last = &entry;
        ++count;
      }
      return result;
    }
    if (entry.key == key) return hit(entry, *stats, feature, canonical, formattedSection);
  }
  disabled = true;
  ++stats->capacityFallbacks;
  ++stats->bypasses;
  return original(expression, defaultValue, feature, canonical, formattedSection);
}

Color evaluateUncached(const style::PropertyExpression<Color>& expression, Color defaultValue,
                       const GeometryTileFeature& feature, const CanonicalTileID& canonical,
                       const style::expression::Value& formattedSection)
{
  if (auto* stats = current())
  {
    ++stats->calls;
    if (stats->applied.mode != Mode::Off) ++stats->bypasses;
  }
  return original(expression, defaultValue, feature, canonical, formattedSection);
}

} // namespace mln::colormemo
