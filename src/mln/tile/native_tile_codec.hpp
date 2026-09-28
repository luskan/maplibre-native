#pragma once

#include <mln/tile/native_tile_payload.hpp>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace mln
{

struct NativeTileCodecLimits
{
  size_t storedBytes = 32 * 1024 * 1024;
  size_t decodedBytes = 96 * 1024 * 1024;
  size_t descriptorBytes = 16 * 1024;
  size_t nesting = 16;
};

struct NativeTileEncodingSize
{
  size_t storedBytes = 0;
  size_t decodedCharge = 0;
};

enum class NativeTileCodecLimit : uint8_t
{
  StoredBytes = 0,
  DecodedBytes = 1,
  DescriptorBytes = 2,
  Nesting = 3
};

class NativeTileCodecLimitError final : public std::runtime_error
{
public:
  NativeTileCodecLimitError(NativeTileCodecLimit reason_, size_t limit_, size_t requiredAtLeast_)
    : std::runtime_error("Native tile exceeds codec limit"), reason(reason_), limit(limit_),
      requiredAtLeast(requiredAtLeast_) {}
  const NativeTileCodecLimit reason;
  const size_t limit;
  // This is the first rejected charge or depth, not the complete encoded tile size.
  const size_t requiredAtLeast;
};

class NativeTileCodecInvalidError final : public std::runtime_error
{
public:
  NativeTileCodecInvalidError() : std::runtime_error("Invalid native tile record") {}
};

class NativeTileCodecCompatibilityError final : public std::runtime_error
{
public:
  NativeTileCodecCompatibilityError() : std::runtime_error("Native property layout is incompatible") {}
};

class NativeTileCodec
{
public:
  static NativeTileEncodingSize inspect(std::string_view, const NativeTileCodecLimits& = {});
  static NativeTileEncodingSize measure(const NativeTilePayload&, std::string_view descriptor,
                                        const NativeTileCodecLimits& = {});
  static std::string encode(const NativeTilePayload&, std::string_view descriptor,
                            const NativeTileCodecLimits& = {});
  static NativeTilePayloadPtr decode(std::string_view, std::string_view expectedDescriptor,
                                    const NativeTileMetadata&, const NativeTileCodecLimits& = {});
};

} // namespace mln
