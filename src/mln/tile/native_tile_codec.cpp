#include <mln/tile/native_tile_codec.hpp>
#include <mln/util/compression.hpp>

#include <bit>
#include <cmath>
#include <algorithm>
#include <limits>
#include <optional>
#include <stdexcept>
#include <type_traits>

namespace mln
{
namespace
{
constexpr std::string_view magic = "AMNTILE2";
constexpr size_t headerBytes = 32;
constexpr size_t propertyCharge = sizeof(PropertyMap::value_type) + 96;

[[noreturn]] void invalid()
{
  throw NativeTileCodecInvalidError();
}

void checkCount(size_t value)
{
  if(value > UINT32_MAX) invalid();
}

size_t bucketAllowance(size_t count)
{
#if !defined(__GLIBCXX__) && !defined(_LIBCPP_VERSION)
  throw NativeTileCodecCompatibilityError();
#endif
  // The supported libstdc++ and libc++ policies round hints to primes or powers of two.
  if(count > (std::numeric_limits<size_t>::max() - 2) / 2) invalid();
  return 2 * count + 2;
}

class Budget
{
public:
  explicit Budget(size_t limit_, std::optional<NativeTileCodecLimit> reason_ = {}) : limit(limit_), reason(reason_) {}
  void add(size_t count, size_t width = 1)
  {
    if(width && count > (std::numeric_limits<size_t>::max() - used) / width) invalid();
    const auto required = used + count * width;
    if(required > limit)
    {
      if(reason) throw NativeTileCodecLimitError(*reason, limit, required);
      invalid();
    }
    used = required;
  }
  size_t used = 0;
private:
  size_t limit;
  std::optional<NativeTileCodecLimit> reason;
};

class Writer
{
public:
  Writer(const NativeTileCodecLimits& limits_, std::string* out_)
    : limits(limits_), wire(limits.storedBytes, NativeTileCodecLimit::StoredBytes),
      decoded(limits.decodedBytes, NativeTileCodecLimit::DecodedBytes), out(out_) {}

  void bytes(std::string_view value)
  {
    wire.add(value.size());
    if(out) out->append(value);
  }

  void integer(uint64_t value, size_t width)
  {
    wire.add(width);
    if(out)
      for(size_t i = 0; i < width; ++i) out->push_back(static_cast<char>((value >> (8 * i)) & 255));
  }

  void count(size_t value)
  {
    checkCount(value);
    integer(value, 4);
  }

  void string(std::string_view value)
  {
    checkCount(value.size());
    decoded.add(value.size());
    decoded.add(64);
    count(value.size());
    bytes(value);
  }

  void object(const PropertyMap& values, size_t depth)
  {
    checkCount(values.size());
    if(depth > limits.nesting) throw NativeTileCodecLimitError(NativeTileCodecLimit::Nesting, limits.nesting, depth);
    const auto buckets = values.bucket_count();
    const auto factor = values.max_load_factor();
    if(!std::isfinite(factor) || factor <= 0 || (!values.empty() && !buckets)) invalid();
    checkCount(buckets);
    decoded.add(2 * sizeof(PropertyMap) + 128);
    decoded.add(bucketAllowance(buckets), sizeof(void*));
    decoded.add(values.size(), propertyCharge + sizeof(std::string_view));
    count(values.size());
    count(buckets);
    integer(std::bit_cast<uint32_t>(factor), 4);
    for(const auto& [name, val] : values)
    {
      const auto marker = wire.used;
      integer(0, 4);
      const auto start = wire.used;
      string(name);
      value(val, depth + 1);
      const auto length = wire.used - start;
      checkCount(length);
      if(out)
        for(size_t i = 0; i < 4; ++i) (*out)[marker + i] = static_cast<char>(length >> (8 * i));
    }
  }

  void value(const Value& val, size_t depth)
  {
    if(depth > limits.nesting) throw NativeTileCodecLimitError(NativeTileCodecLimit::Nesting, limits.nesting, depth);
    val.match([&](const auto& v)
    {
      using T = std::decay_t<decltype(v)>;
      if constexpr(std::is_same_v<T, NullValue>) integer(0, 1);
      else if constexpr(std::is_same_v<T, bool>) integer(v ? 2 : 1, 1);
      else if constexpr(std::is_same_v<T, uint64_t>) { integer(3, 1); integer(v, 8); }
      else if constexpr(std::is_same_v<T, int64_t>) { integer(4, 1); integer(std::bit_cast<uint64_t>(v), 8); }
      else if constexpr(std::is_same_v<T, double>) { integer(5, 1); integer(std::bit_cast<uint64_t>(v), 8); }
      else if constexpr(std::is_same_v<T, std::string>) { integer(6, 1); string(v); }
      else if constexpr(std::is_same_v<T, Value::array_type>)
      {
        checkCount(v.size());
        integer(7, 1);
        decoded.add(2 * sizeof(Value::array_type) + 128);
        decoded.add(v.size(), sizeof(Value) + 16);
        count(v.size());
        for(const auto& item : v) value(item, depth + 1);
      }
      else if constexpr(std::is_same_v<T, PropertyMap>) { integer(8, 1); object(v, depth + 1); }
    });
  }

  void identifier(const FeatureIdentifier& id)
  {
    id.match([&](const auto& v)
    {
      using T = std::decay_t<decltype(v)>;
      if constexpr(std::is_same_v<T, NullValue>) integer(0, 1);
      else if constexpr(std::is_same_v<T, uint64_t>) { integer(3, 1); integer(v, 8); }
      else if constexpr(std::is_same_v<T, int64_t>) { integer(4, 1); integer(std::bit_cast<uint64_t>(v), 8); }
      else if constexpr(std::is_same_v<T, double>) { integer(5, 1); integer(std::bit_cast<uint64_t>(v), 8); }
      else if constexpr(std::is_same_v<T, std::string>) { integer(6, 1); string(v); }
    });
  }

  const NativeTileCodecLimits& limits;
  Budget wire;
  Budget decoded;
private:
  std::string* out;
};

void body(Writer& w, const NativeTilePayload& payload, std::string_view descriptor)
{
  if(descriptor.empty()) invalid();
  checkCount(descriptor.size());
  if(descriptor.size() > w.limits.descriptorBytes)
    throw NativeTileCodecLimitError(NativeTileCodecLimit::DescriptorBytes, w.limits.descriptorBytes, descriptor.size());
  w.decoded.add(sizeof(NativeTilePayload) + 256);
  w.string(descriptor);
  const auto& meta = payload.metadata();
  const auto& c = meta.conversion;
  w.integer(meta.tileID.z, 1);
  w.integer(meta.tileID.x, 4);
  w.integer(meta.tileID.y, 4);
  w.integer(c.conversionRevision, 4);
  w.integer(c.normalizationRevision, 4);
  w.integer(c.extent, 2);
  w.integer(c.buffer, 2);
  w.integer(std::bit_cast<uint64_t>(c.tolerance), 8);
  w.integer(c.clip, 1);
  w.integer(c.wrap, 1);
  w.count(payload.features().size());
  w.decoded.add(payload.features().size(), sizeof(NativeFeatureRecord) + 64);
  if(payload.propertySets().size() > payload.features().size()) invalid();
  w.count(payload.propertySets().size());
  w.decoded.add(payload.propertySets().size(), sizeof(PropertyMap) + 1);
  for(const auto& properties : payload.propertySets()) w.object(properties, 0);
  for(const auto& feature : payload.features())
  {
    w.integer(static_cast<uint8_t>(feature.type), 1);
    w.identifier(feature.id);
    if(feature.propertySet >= payload.propertySets().size()) invalid();
    w.integer(feature.propertySet, 4);
    w.count(feature.geometry.size());
    w.decoded.add(feature.geometry.size(), sizeof(GeometryCoordinates) + 64);
    for(const auto& part : feature.geometry)
    {
      w.count(part.size());
      w.decoded.add(part.size(), sizeof(GeometryCoordinate));
      for(const auto& p : part)
      {
        w.integer(std::bit_cast<uint16_t>(p.x), 2);
        w.integer(std::bit_cast<uint16_t>(p.y), 2);
      }
    }
  }
}

class Reader
{
public:
  Reader(std::string_view data_, const NativeTileCodecLimits& limits_)
    : data(data_), limits(limits_), ownedBudget(limits.decodedBytes), decoded(ownedBudget) {}
  Reader(std::string_view data_, const NativeTileCodecLimits& limits_, Budget& shared)
    : data(data_), limits(limits_), ownedBudget(limits.decodedBytes), decoded(shared) {}

  uint64_t integer(size_t width)
  {
    if(width > data.size() - offset) invalid();
    uint64_t value = 0;
    for(size_t i = 0; i < width; ++i)
      value |= uint64_t(static_cast<unsigned char>(data[offset++])) << (8 * i);
    return value;
  }

  size_t count(size_t minimumWire, size_t allocationCharge)
  {
    const auto value = static_cast<size_t>(integer(4));
    if(minimumWire && value > (data.size() - offset) / minimumWire) invalid();
    decoded.add(value, allocationCharge);
    return value;
  }

  std::string string()
  {
    const auto size = count(1, 1);
    decoded.add(64);
    std::string value(data.substr(offset, size));
    offset += size;
    return value;
  }

  PropertyMap object(size_t depth)
  {
    if(depth > limits.nesting) invalid();
    decoded.add(2 * sizeof(PropertyMap) + 128);
    const auto size = count(9, propertyCharge + sizeof(std::string_view));
    const auto buckets = static_cast<size_t>(integer(4));
    const auto factor = std::bit_cast<float>(static_cast<uint32_t>(integer(4)));
    if(!std::isfinite(factor) || factor <= 0 || (size && !buckets)) invalid();
    const auto maximumBuckets = bucketAllowance(buckets);
    decoded.add(maximumBuckets, sizeof(void*));
    std::vector<std::string_view> entries;
    entries.reserve(size);
    for(size_t i = 0; i < size; ++i)
    {
      const auto length = static_cast<size_t>(integer(4));
      if(length < 5 || length > data.size() - offset) invalid();
      entries.push_back(data.substr(offset, length));
      offset += length;
    }
    PropertyMap result;
    const auto insertionFactor = static_cast<float>(size / std::max<size_t>(buckets, 1) + 2);
    result.max_load_factor(insertionFactor);
    result.rehash(buckets);
    if(result.bucket_count() > maximumBuckets) throw NativeTileCodecCompatibilityError();
    for(auto it = entries.rbegin(); it != entries.rend(); ++it)
    {
      Reader entry(*it, limits, decoded);
      auto name = entry.string();
      auto val = entry.value(depth + 1);
      if(entry.offset != entry.data.size()) invalid();
      if(!result.emplace(std::move(name), std::move(val)).second) invalid();
      val = NullValue{};
    }
    result.max_load_factor(factor);
    auto actual = result.begin();
    for(const auto& span : entries)
    {
      Reader entry(span, limits);
      const auto length = static_cast<size_t>(entry.integer(4));
      if(length > span.size() - entry.offset) invalid();
      if(actual == result.end() || actual->first != span.substr(entry.offset, length))
        throw NativeTileCodecCompatibilityError();
      ++actual;
    }
    return result;
  }

  Value value(size_t depth)
  {
    if(depth > limits.nesting) invalid();
    switch(integer(1))
    {
      case 0: return NullValue{};
      case 1: return false;
      case 2: return true;
      case 3: return integer(8);
      case 4: return std::bit_cast<int64_t>(integer(8));
      case 5: return std::bit_cast<double>(integer(8));
      case 6: return string();
      case 7:
      {
        decoded.add(2 * sizeof(Value::array_type) + 128);
        const auto size = count(1, sizeof(Value) + 16);
        Value::array_type result;
        result.reserve(size);
        for(size_t i = 0; i < size; ++i) result.push_back(value(depth + 1));
        return result;
      }
      case 8: return object(depth + 1);
      default: invalid();
    }
  }

  FeatureIdentifier identifier()
  {
    switch(integer(1))
    {
      case 0: return NullValue{};
      case 3: return integer(8);
      case 4: return std::bit_cast<int64_t>(integer(8));
      case 5: return std::bit_cast<double>(integer(8));
      case 6: return string();
      default: invalid();
    }
  }

  std::string_view data;
  const NativeTileCodecLimits& limits;
  Budget ownedBudget;
  Budget& decoded;
  size_t offset = 0;
};
} // namespace

NativeTileEncodingSize NativeTileCodec::measure(const NativeTilePayload& payload, std::string_view descriptor,
                                                const NativeTileCodecLimits& limits)
{
  Writer w(limits, nullptr);
  w.wire.add(headerBytes);
  body(w, payload, descriptor);
  return {w.wire.used, w.decoded.used};
}

std::string NativeTileCodec::encode(const NativeTilePayload& payload, std::string_view descriptor,
                                  const NativeTileCodecLimits& limits)
{
  const auto size = measure(payload, descriptor, limits);
  std::string result;
  result.reserve(size.storedBytes);
  Writer w(limits, &result);
  w.bytes(magic);
  w.integer(size.storedBytes, 8);
  w.integer(0, 4);
  w.integer(0, 4);
  w.integer(size.decodedCharge, 8);
  body(w, payload, descriptor);
  const auto crc = util::crc32(result.data() + headerBytes, result.size() - headerBytes);
  for(size_t i = 0; i < 4; ++i) result[16 + i] = static_cast<char>((crc >> (8 * i)) & 255);
  return result;
}

NativeTileEncodingSize NativeTileCodec::inspect(std::string_view bytes, const NativeTileCodecLimits& limits)
{
  if(bytes.size() < headerBytes || bytes.size() > limits.storedBytes || bytes.substr(0, 8) != magic) invalid();
  Reader r(bytes, limits);
  r.offset = 8;
  if(r.integer(8) != bytes.size()) invalid();
  const auto crc = r.integer(4);
  if(r.integer(4) != 0) invalid();
  const auto decodedCharge = r.integer(8);
  if(!decodedCharge || decodedCharge > limits.decodedBytes
     || crc != util::crc32(bytes.data() + headerBytes, bytes.size() - headerBytes)) invalid();
  return {bytes.size(), static_cast<size_t>(decodedCharge)};
}

NativeTilePayloadPtr NativeTileCodec::decode(std::string_view bytes, std::string_view expectedDescriptor,
                                           const NativeTileMetadata& expected, const NativeTileCodecLimits& limits)
{
  const auto size = inspect(bytes, limits);
  if(expectedDescriptor.empty() || expectedDescriptor.size() > limits.descriptorBytes) invalid();
  auto bounded = limits;
  bounded.decodedBytes = size.decodedCharge;
  Reader r(bytes, bounded);
  r.offset = headerBytes;
  r.decoded.add(sizeof(NativeTilePayload) + 256);
  const auto descriptorSize = r.count(1, 1);
  r.decoded.add(64);
  if(descriptorSize != expectedDescriptor.size() || bytes.substr(r.offset, descriptorSize) != expectedDescriptor)
    invalid();
  r.offset += descriptorSize;
  const auto z = r.integer(1);
  const auto x = r.integer(4);
  const auto y = r.integer(4);
  if(z != expected.tileID.z || x != expected.tileID.x || y != expected.tileID.y) invalid();
  const auto& c = expected.conversion;
  if(r.integer(4) != c.conversionRevision || r.integer(4) != c.normalizationRevision
     || r.integer(2) != c.extent || r.integer(2) != c.buffer
     || r.integer(8) != std::bit_cast<uint64_t>(c.tolerance)
     || r.integer(1) != static_cast<uint64_t>(c.clip) || r.integer(1) != static_cast<uint64_t>(c.wrap)) invalid();
  const auto features = r.count(10, sizeof(NativeFeatureRecord) + 64);
  const auto propertySets = r.count(12, sizeof(PropertyMap) + 1);
  if(propertySets > features) invalid();
  NativeTileBuildOptions buildOptions;
  buildOptions.compact = false;
  buildOptions.propertyIndexSlots = 0;
  NativeTileBuilder builder(expected, std::move(buildOptions));
  builder.reserveFeatures(features);
  builder.reservePropertySets(propertySets);
  std::vector<uint8_t> used(propertySets, 0);
  for(size_t i = 0; i < propertySets; ++i) builder.importProperties(r.object(0));
  for(size_t i = 0; i < features; ++i)
  {
    const auto type = r.integer(1);
    if(type > static_cast<uint8_t>(FeatureType::Polygon)) invalid();
    auto id = r.identifier();
    const auto properties = static_cast<uint32_t>(r.integer(4));
    if(properties >= propertySets) invalid();
    used[properties] = 1;
    const auto parts = r.count(4, sizeof(GeometryCoordinates) + 64);
    GeometryCollection geometry;
    geometry.reserve(parts);
    for(size_t p = 0; p < parts; ++p)
    {
      const auto count = r.count(4, sizeof(GeometryCoordinate));
      GeometryCoordinates coordinates;
      coordinates.reserve(count);
      for(size_t k = 0; k < count; ++k)
      {
        const auto px = std::bit_cast<int16_t>(static_cast<uint16_t>(r.integer(2)));
        const auto py = std::bit_cast<int16_t>(static_cast<uint16_t>(r.integer(2)));
        coordinates.emplace_back(px, py);
      }
      geometry.push_back(std::move(coordinates));
    }
    builder.appendIndexed(static_cast<FeatureType>(type), std::move(geometry), properties, std::move(id));
  }
  if(r.offset != bytes.size() || r.decoded.used != size.decodedCharge
     || std::find(used.begin(), used.end(), 0) != used.end()) invalid();
  return std::move(builder).seal();
}

} // namespace mln
