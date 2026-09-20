#pragma once

#include <mln/style/property_expression.hpp>
#include <mln/util/paint_memo.hpp>

#include <array>
#include <memory>

namespace mln::paintmemo {

class Cache
{
public:
  static std::unique_ptr<Cache> create(const style::PropertyExpression<float>&, Range<float>, float);
  ~Cache();
  Cache(const Cache&) = delete;
  Cache& operator=(const Cache&) = delete;

  Range<float> evaluate(const GeometryTileFeature&, const CanonicalTileID&, const style::expression::Value&);

private:
  struct KeyPart
  {
    uint64_t bits = 0, type = 0;
    bool operator==(const KeyPart& other) const { return bits == other.bits && type == other.type; }
  };
  using Key = std::array<KeyPart, 2>;
  struct Entry
  {
    Key key;
    Range<float> value{0, 0};
    bool used = false;
  };
  Cache(const style::PropertyExpression<float>&, Range<float>, float, Policy,
        std::array<const std::string*, 2>, size_t);
  bool makeKey(const GeometryTileFeature&, Key&) const;

  const style::PropertyExpression<float> expression;
  const Range<float> zoom;
  const float defaultValue;
  const Policy applied;
  const std::array<const std::string*, 2> names;
  const size_t nameCount;
  std::array<Entry, 64> entries{};
  size_t count = 0;
  bool disabled = false;
};

Range<float> evaluateUncached(const style::PropertyExpression<float>&, Range<float>, float,
                             const GeometryTileFeature&, const CanonicalTileID&, const style::expression::Value&);

} // namespace mln::paintmemo
