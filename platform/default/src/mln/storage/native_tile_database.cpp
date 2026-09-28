#include <mln/storage/native_tile_database.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <limits>
#include <stdexcept>

namespace mln
{
namespace
{
using namespace mapbox::sqlite;

// CASE rejects non-BLOB values before length() can materialize a large SQL string.
constexpr const char* recordSizeExpression =
  "CASE WHEN typeof(id)<>'integer' OR typeof(key)<>'blob' THEN -1 "
  "WHEN length(key)<>32 OR typeof(data)<>'blob' THEN -1 "
  "WHEN typeof(bytes)<>'integer' OR typeof(accessed)<>'integer' THEN -1 "
  "WHEN length(data)<24 OR bytes<>length(data) THEN -1 ELSE length(data) END";

uint64_t scalar(Database& db, const char* sql)
{
  Statement statement(db, sql);
  Query query(statement);
  if(!query.run()) throw std::runtime_error("Missing native cache database result");
  const auto result = query.get<int64_t>(0);
  if(result < 0) throw std::runtime_error("Invalid native cache database size");
  return static_cast<uint64_t>(result);
}

void validKey(const std::string& key)
{
  if(key.size() != 32) throw std::invalid_argument("Native cache requires a SHA256 key");
}
}

NativeTileDatabase::NativeTileDatabase(std::string path, NativeTileDatabaseLimits limits_) : limits(limits_)
{
  if(path.empty() || limits.recordBytes < 24 || limits.recordBytes > limits.diskBytes
     || limits.diskBytes > static_cast<size_t>(INT64_MAX) / 2 || limits.pageCacheBytes < 1024
     || !limits.rows || limits.rows > 4096)
    throw std::invalid_argument("Invalid native tile database limits");
  db = std::make_unique<Database>(Database::open(path, ReadWriteCreate));
  db->setBusyTimeout(std::chrono::milliseconds(25));
  db->exec("PRAGMA mmap_size=0");
  db->exec("PRAGMA cache_size=-" + std::to_string(limits.pageCacheBytes / 1024));
  db->exec("PRAGMA temp_store=FILE");
  db->exec("PRAGMA journal_mode=DELETE");
  db->exec("PRAGMA synchronous=NORMAL");
  pageSize = scalar(*db, "PRAGMA page_size");
  if(!pageSize) throw NativeTileDatabaseRejected("Invalid native cache page size");
  maxPages = (limits.diskBytes + limits.recordBytes) / pageSize + 32;
  const auto cap = "PRAGMA max_page_count=" + std::to_string(maxPages);
  const auto effectiveMaxPages = scalar(*db, cap.c_str());
  if(!effectiveMaxPages || effectiveMaxPages > maxPages || scalar(*db, "PRAGMA page_count") > effectiveMaxPages)
    throw NativeTileDatabaseRejected("Native tile database exceeds its physical quota");
  maxPages = effectiveMaxPages;
  db->exec("CREATE TABLE IF NOT EXISTS native_tiles ("
           "id INTEGER PRIMARY KEY, key BLOB NOT NULL UNIQUE CHECK(length(key)=32), "
           "data BLOB NOT NULL, bytes INTEGER NOT NULL, accessed INTEGER NOT NULL)");
  if(scalar(*db, "SELECT COUNT(*) FROM sqlite_master WHERE name='native_tiles' AND type='table'") != 1
     || scalar(*db, "SELECT COUNT(*) FROM pragma_table_info('native_tiles')") != 5
     || scalar(*db, "SELECT COUNT(*) FROM pragma_table_info('native_tiles') WHERE "
       "(name='id' AND type='INTEGER' AND pk=1) OR (name='key' AND type='BLOB' AND pk=0) OR "
       "(name='data' AND type='BLOB' AND pk=0) OR (name='bytes' AND type='INTEGER' AND pk=0) OR "
       "(name='accessed' AND type='INTEGER' AND pk=0)") != 5)
    throw NativeTileDatabaseRejected("Unsupported native tile database schema");
  {
    const auto sql = std::string("SELECT ") + recordSizeExpression + " FROM native_tiles NOT INDEXED LIMIT 4097";
    Statement sizes(*db, sql.c_str());
    Query query(sizes);
    while(query.run())
    {
      const auto bytes = query.get<int64_t>(0);
      if(bytes < 24 || static_cast<uint64_t>(bytes) > limits.recordBytes
         || ++rowsUsed > limits.rows || static_cast<uint64_t>(bytes) > limits.diskBytes - bytesUsed)
        throw NativeTileDatabaseRejected("Invalid native tile database record or quota");
      bytesUsed += static_cast<uint64_t>(bytes);
    }
  }
  db->exec("CREATE INDEX IF NOT EXISTS native_tiles_access ON native_tiles(accessed,id)");
}

NativeTileDatabase::~NativeTileDatabase() = default;

std::optional<size_t> NativeTileDatabase::recordSize(const std::string& digest)
{
  validKey(digest);
  if(!db) throw std::runtime_error("Native tile database is unavailable");
  const auto sql = std::string("SELECT ") + recordSizeExpression + " FROM native_tiles WHERE key=?1";
  Statement statement(*db, sql.c_str());
  Query query(statement);
  query.bindBlob(1, digest.data(), digest.size(), false);
  if(!query.run()) return std::nullopt;
  const auto bytes = query.get<int64_t>(0);
  if(bytes < 24 || static_cast<uint64_t>(bytes) > limits.recordBytes) return std::nullopt;
  return static_cast<size_t>(bytes);
}

std::optional<std::string> NativeTileDatabase::read(const std::string& digest, size_t maximumBytes)
{
  validKey(digest);
  if(!db) throw std::runtime_error("Native tile database is unavailable");
  const auto sql = std::string("SELECT CASE WHEN (") + recordSizeExpression
    + ") BETWEEN 24 AND ?2 THEN data END FROM native_tiles WHERE key=?1";
  Statement statement(*db, sql.c_str());
  Query query(statement);
  query.bindBlob(1, digest.data(), digest.size(), false);
  query.bind(2, static_cast<int64_t>(std::min(limits.recordBytes, maximumBytes)));
  if(!query.run()) return std::nullopt;
  return query.get<std::optional<std::string>>(0);
}

uint64_t NativeTileDatabase::storedBytes()
{
  if(!db) throw std::runtime_error("Native tile database is unavailable");
  return bytesUsed;
}

void NativeTileDatabase::rollback() noexcept
{
  try { db->exec("ROLLBACK"); }
  catch(...) { db.reset(); }
}

bool NativeTileDatabase::write(const std::string& digest, const std::string& record)
{
  validKey(digest);
  if(!db) throw std::runtime_error("Native tile database is unavailable");
  if(record.size() < 24 || record.size() > limits.recordBytes) return false;
  const auto beforeBytes = bytesUsed;
  const auto beforeRows = rowsUsed;
  db->exec("BEGIN IMMEDIATE");
  try
  {
    erase(digest);
    const auto neededPages = (record.size() + pageSize - 1) / pageSize + 8;
    const auto needsEviction = [&]
    {
      const auto pages = scalar(*db, "PRAGMA page_count");
      const auto free = scalar(*db, "PRAGMA freelist_count");
      return bytesUsed > limits.diskBytes - record.size() || rowsUsed >= limits.rows
        || free > pages || pages - free + neededPages > maxPages;
    };
    size_t evicted = 0;
    while(needsEviction())
    {
      if(evicted++ == 32)
      {
        db->exec("COMMIT");
        return false;
      }
      const auto sql = std::string("SELECT id,") + recordSizeExpression
        + " FROM native_tiles ORDER BY accessed,id LIMIT 1";
      Statement oldest(*db, sql.c_str());
      int64_t row;
      uint64_t bytes;
      {
        Query query(oldest);
        if(!query.run())
        {
          db->exec("COMMIT");
          return false;
        }
        row = query.get<int64_t>(0);
        const auto size = query.get<int64_t>(1);
        if(size < 24 || static_cast<uint64_t>(size) > limits.recordBytes
           || static_cast<uint64_t>(size) > bytesUsed || !rowsUsed)
          throw std::runtime_error("Invalid native tile cache entry");
        bytes = static_cast<uint64_t>(size);
      }
      Statement remove(*db, "DELETE FROM native_tiles WHERE id=?1");
      Query query(remove);
      query.bind(1, row);
      query.run();
      if(query.changes() != 1) throw std::runtime_error("Native cache eviction changed outside its owner");
      bytesUsed -= bytes;
      --rowsUsed;
    }
    {
      Statement statement(*db, "INSERT INTO native_tiles(key,data,bytes,accessed) "
        "VALUES(?1,?2,?3,CAST(strftime('%s','now') AS INTEGER))");
      Query query(statement);
      query.bindBlob(1, digest.data(), digest.size(), false);
      query.bindBlob(2, record.data(), record.size(), false);
      query.bind(3, static_cast<int64_t>(record.size()));
      query.run();
    }
    db->exec("COMMIT");
    bytesUsed += record.size();
    ++rowsUsed;
    return true;
  }
  catch(...)
  {
    bytesUsed = beforeBytes;
    rowsUsed = beforeRows;
    rollback();
    throw;
  }
}

void NativeTileDatabase::erase(const std::string& digest)
{
  validKey(digest);
  if(!db) throw std::runtime_error("Native tile database is unavailable");
  uint64_t size = 0;
  bool present = false;
  {
    const auto sql = std::string("SELECT ") + recordSizeExpression + " FROM native_tiles WHERE key=?1";
    Statement lookup(*db, sql.c_str());
    Query query(lookup);
    query.bindBlob(1, digest.data(), digest.size(), false);
    if(query.run())
    {
      const auto bytes = query.get<int64_t>(0);
      if(bytes < 24 || static_cast<uint64_t>(bytes) > limits.recordBytes
         || static_cast<uint64_t>(bytes) > bytesUsed || !rowsUsed)
        throw std::runtime_error("Native cache accounting changed outside its owner");
      size = static_cast<uint64_t>(bytes);
      present = true;
    }
  }
  Statement statement(*db, "DELETE FROM native_tiles WHERE key=?1");
  Query query(statement);
  query.bindBlob(1, digest.data(), digest.size(), false);
  query.run();
  if(query.changes() != static_cast<uint64_t>(present))
    throw std::runtime_error("Native cache removal changed outside its owner");
  if(present)
  {
    bytesUsed -= size;
    --rowsUsed;
  }
}

void NativeTileDatabase::clear()
{
  if(!db) throw std::runtime_error("Native tile database is unavailable");
  db->exec("DELETE FROM native_tiles");
  bytesUsed = 0;
  rowsUsed = 0;
  db->exec("PRAGMA shrink_memory");
}

std::unique_ptr<NativeTileStore> createNativeTileStore(const std::string& path, NativeTileStoreLimits limits)
{
  try { return std::make_unique<NativeTileDatabase>(path, limits); }
  catch(const NativeTileDatabaseRejected&) {}
  catch(const mapbox::sqlite::Exception& error)
  {
    if(error.code != mapbox::sqlite::ResultCode::Corrupt && error.code != mapbox::sqlite::ResultCode::NotADB) throw;
  }
  const std::filesystem::path owned(path);
  if(owned.filename() != "native-tiles-v1.db")
    throw NativeTileDatabaseRejected("Recovery is restricted to the owned native tile database");
  const char* suffixes[] {"", "-journal", "-wal", "-shm"};
  for(const auto* suffix : suffixes)
  {
    const auto file = path + suffix;
    const auto status = std::filesystem::symlink_status(file);
    if(std::filesystem::exists(status) && !std::filesystem::is_regular_file(status))
      throw NativeTileDatabaseRejected("Native tile database recovery requires regular files");
  }
  for(const auto* suffix : suffixes) std::filesystem::remove(path + suffix);
  return std::make_unique<NativeTileDatabase>(path, limits);
}

} // namespace mln
