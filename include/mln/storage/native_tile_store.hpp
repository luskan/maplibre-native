#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace mln
{

struct NativeTileStoreLimits
{
  size_t recordBytes = 32 * 1024 * 1024;
  size_t diskBytes = 256 * 1024 * 1024;
  size_t pageCacheBytes = 2 * 1024 * 1024;
  size_t rows = 4096;
};

class NativeTileStore
{
public:
  virtual ~NativeTileStore() = default;
  virtual std::optional<size_t> recordSize(const std::string& digest) = 0;
  virtual std::optional<std::string> read(const std::string& digest, size_t maximumBytes) = 0;
  virtual bool write(const std::string& digest, const std::string& record) = 0;
  virtual void erase(const std::string& digest) = 0;
  virtual void clear() = 0;
  virtual uint64_t storedBytes() = 0;
};

std::unique_ptr<NativeTileStore> createNativeTileStore(const std::string&, NativeTileStoreLimits);

} // namespace mln
