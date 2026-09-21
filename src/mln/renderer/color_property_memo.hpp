#pragma once

#include <mln/style/property_expression.hpp>
#include <mln/util/color_memo.hpp>
#include <mln/util/color.hpp>

#include <array>
#include <memory>

namespace mln::colormemo {

class Cache
{
public:
  static std::unique_ptr<Cache> create(const style::PropertyExpression<Color>&, Color);
  ~Cache();
  Cache(const Cache&) = delete;
  Cache& operator=(const Cache&) = delete;

  Color evaluate(const GeometryTileFeature&, const CanonicalTileID&, const style::expression::Value&);

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
    Color value;
    bool used = false;
  };
  Cache(const style::PropertyExpression<Color>&, Color, Policy, std::array<const std::string*, 2>, size_t);
  bool makeKey(const GeometryTileFeature&, Key&) const;
  Color hit(const Entry&, Statistics&, const GeometryTileFeature&, const CanonicalTileID&,
            const style::expression::Value&);

  const style::PropertyExpression<Color> expression;
  const Color defaultValue;
  const Policy applied;
  const std::array<const std::string*, 2> names;
  const size_t nameCount;
  std::array<Entry, 64> entries{};
  const Entry* last = nullptr;
  size_t count = 0;
  bool disabled = false;
};

Color evaluateUncached(const style::PropertyExpression<Color>&, Color, const GeometryTileFeature&,
                      const CanonicalTileID&, const style::expression::Value&);

} // namespace mln::colormemo
