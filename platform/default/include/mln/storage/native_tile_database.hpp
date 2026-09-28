#pragma once

#include <mln/storage/native_tile_store.hpp>
#include <mln/storage/sqlite3.hpp>

#include <memory>
#include <optional>
#include <string>

namespace mln
{

using NativeTileDatabaseLimits = NativeTileStoreLimits;

class NativeTileDatabaseRejected : public std::runtime_error
{
public:
  using std::runtime_error::runtime_error;
};

class NativeTileDatabase : public NativeTileStore
{
public:
  NativeTileDatabase(std::string path, NativeTileDatabaseLimits);
  ~NativeTileDatabase() override;
  std::optional<size_t> recordSize(const std::string& digest) override;
  std::optional<std::string> read(const std::string& digest, size_t maximumBytes = SIZE_MAX) override;
  bool write(const std::string& digest, const std::string& record) override;
  void erase(const std::string& digest) override;
  void clear() override;
  uint64_t storedBytes() override;
private:
  void rollback() noexcept;
  NativeTileDatabaseLimits limits;
  std::unique_ptr<mapbox::sqlite::Database> db;
  uint64_t bytesUsed = 0;
  size_t rowsUsed = 0;
  uint64_t pageSize = 0;
  uint64_t maxPages = 0;
};

} // namespace mln
