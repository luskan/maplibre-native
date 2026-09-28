#include <mln/test/util.hpp>
#include <mln/storage/native_tile_database.hpp>
#include <mln/util/sha256.hpp>

#include <filesystem>
#include <fstream>

using namespace mln;

namespace
{
class NativeDatabaseTest : public ::testing::Test
{
protected:
  std::string path;
  NativeTileDatabaseLimits limits {256, 1024, 1024};
  void SetUp() override
  {
    static size_t serial = 0;
    path = (std::filesystem::temp_directory_path() /
      ("mln-native-cache-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
       + "-" + std::to_string(++serial) + ".db")).string();
  }
  void TearDown() override
  {
    std::filesystem::remove(path);
    std::filesystem::remove(path + "-journal");
  }
};
}

TEST_F(NativeDatabaseTest, PersistsBoundedBinaryRecordsAcrossReopen)
{
  const auto key = util::SHA256::hash("descriptor");
  std::string record(150, '\0');
  record[73] = static_cast<char>(255);
  {
    NativeTileDatabase db(path, limits);
    EXPECT_FALSE(db.read(key));
    EXPECT_TRUE(db.write(key, record));
    EXPECT_EQ(db.read(key), record);
    EXPECT_EQ(db.storedBytes(), record.size());
  }
  NativeTileDatabase db(path, limits);
  EXPECT_EQ(db.read(key), record);
  db.erase(key);
  EXPECT_FALSE(db.read(key));
}

TEST_F(NativeDatabaseTest, RejectsSizeAndMalformedSqlFieldsBeforeMaterialization)
{
  const auto key = util::SHA256::hash("oversized");
  NativeTileDatabase db(path, limits);
  EXPECT_FALSE(db.write(key, std::string(257, 'x')));
  EXPECT_FALSE(db.write(key, "short"));
  auto raw = mapbox::sqlite::Database::open(path, mapbox::sqlite::ReadWriteCreate);
  auto insert = [&](const std::string& sql)
  {
    mapbox::sqlite::Statement statement(raw, sql.c_str());
    mapbox::sqlite::Query query(statement);
    query.bindBlob(1, key.data(), key.size(), false);
    query.run();
  };
  insert("INSERT INTO native_tiles(key,data,bytes,accessed) VALUES(?1,zeroblob(1000000),1000000,0)");
  EXPECT_FALSE(db.read(key));
  raw.exec("DELETE FROM native_tiles");
  insert("INSERT INTO native_tiles(key,data,bytes,accessed) VALUES(?1,zeroblob(24),'invalid',0)");
  EXPECT_FALSE(db.read(key));
  raw.exec("DELETE FROM native_tiles");
  insert("INSERT INTO native_tiles(key,data,bytes,accessed) VALUES(?1,'abcdefghijklmnopqrstuvwx',24,0)");
  EXPECT_FALSE(db.read(key));
}

TEST_F(NativeDatabaseTest, EvictsAndClearsWithinLogicalQuota)
{
  NativeTileDatabase db(path, limits);
  const std::string record(256, 'x');
  for(size_t i = 0; i < 8; ++i)
  {
    EXPECT_TRUE(db.write(util::SHA256::hash(std::to_string(i)), record));
    EXPECT_LE(db.storedBytes(), limits.diskBytes);
  }
  EXPECT_FALSE(db.read(util::SHA256::hash("0")));
  EXPECT_EQ(db.read(util::SHA256::hash("7")), record);
  db.clear();
  EXPECT_EQ(db.storedBytes(), 0u);
}

TEST_F(NativeDatabaseTest, FailedCommitRollsBackAndAllowsLaterWrites)
{
  NativeTileDatabase db(path, limits);
  const auto key = util::SHA256::hash("commit");
  EXPECT_TRUE(db.write(key, std::string(100, 'a')));
  auto reader = mapbox::sqlite::Database::open(path, mapbox::sqlite::ReadWriteCreate);
  reader.exec("BEGIN");
  {
    mapbox::sqlite::Statement statement(reader, "SELECT data FROM native_tiles");
    mapbox::sqlite::Query query(statement);
    ASSERT_TRUE(query.run());
  }
  EXPECT_THROW(db.write(key, std::string(100, 'b')), mapbox::sqlite::Exception);
  reader.exec("ROLLBACK");
  EXPECT_EQ(db.read(key), std::string(100, 'a'));
  EXPECT_TRUE(db.write(key, std::string(100, 'c')));
  EXPECT_EQ(db.read(key), std::string(100, 'c'));
}

TEST_F(NativeDatabaseTest, ReplacesWithoutDoubleChargingAndRejectsWrongKey)
{
  NativeTileDatabase db(path, limits);
  const auto key = util::SHA256::hash("replace");
  EXPECT_TRUE(db.write(key, std::string(200, 'a')));
  EXPECT_TRUE(db.write(key, std::string(150, 'b')));
  EXPECT_EQ(db.storedBytes(), 150u);
  EXPECT_THROW(db.read(std::string(31, 'x')), std::invalid_argument);
}

TEST_F(NativeDatabaseTest, BoundsEvictionAndLetsReadsRunBetweenMaintenanceTurns)
{
  limits = {4096, 4096, 1024, 128};
  NativeTileDatabase db(path, limits);
  for(size_t i = 0; i < 100; ++i)
    ASSERT_TRUE(db.write(util::SHA256::hash(std::to_string(i)), std::string(24, 'x')));
  const auto key = util::SHA256::hash("large");
  EXPECT_FALSE(db.write(key, std::string(4000, 'y')));
  EXPECT_EQ(db.storedBytes(), (100u - 32u) * 24u);
  EXPECT_TRUE(db.read(util::SHA256::hash("99")));
  bool written = false;
  for(size_t i = 0; i < 4 && !written; ++i) written = db.write(key, std::string(4000, 'y'));
  EXPECT_TRUE(written);
  EXPECT_EQ(db.read(key), std::string(4000, 'y'));
}

TEST_F(NativeDatabaseTest, RejectsOversizedExistingDatabaseAndBoundsRows)
{
  {
    NativeTileDatabase db(path, limits);
  }
  {
    auto raw = mapbox::sqlite::Database::open(path, mapbox::sqlite::ReadWriteCreate);
    raw.exec("CREATE TABLE junk(data BLOB)");
    raw.exec("INSERT INTO junk VALUES(zeroblob(1000000))");
  }
  EXPECT_THROW(NativeTileDatabase(path, limits), std::runtime_error);
  std::filesystem::remove(path);
  limits.rows = 4;
  NativeTileDatabase db(path, limits);
  for(size_t i = 0; i < 10; ++i)
    ASSERT_TRUE(db.write(util::SHA256::hash(std::to_string(i)), std::string(24, 'x')));
  EXPECT_EQ(db.storedBytes(), 96u);
  EXPECT_FALSE(db.read(util::SHA256::hash("0")));
  EXPECT_TRUE(db.read(util::SHA256::hash("9")));
  const auto key = util::SHA256::hash("9");
  EXPECT_EQ(db.recordSize(key), 24u);
  EXPECT_FALSE(db.read(key, 23));
}

TEST_F(NativeDatabaseTest, RejectsMalformedSqlTypesOnReopen)
{
  const std::string key(32, 'k');
  const char* values[] {
    "CAST(?1 AS TEXT),zeroblob(24),24,0",
    "?1,CAST(zeroblob(65536) AS TEXT),65536,0",
    "?1,zeroblob(24),CAST(zeroblob(65536) AS TEXT),0",
    "?1,zeroblob(24),24,CAST(zeroblob(4096) AS TEXT)",
    "?1,zeroblob(24),23,0"
  };
  for(const auto* fields : values)
  {
    SCOPED_TRACE(fields);
    {
      NativeTileDatabase db(path, limits);
    }
    {
      auto raw = mapbox::sqlite::Database::open(path, mapbox::sqlite::ReadWriteCreate);
      const auto sql = std::string("INSERT INTO native_tiles(key,data,bytes,accessed) VALUES(") + fields + ")";
      mapbox::sqlite::Statement statement(raw, sql.c_str());
      mapbox::sqlite::Query query(statement);
      query.bindBlob(1, key.data(), key.size(), false);
      query.run();
    }
    EXPECT_THROW(NativeTileDatabase(path, limits), std::runtime_error);
    std::filesystem::remove(path);
  }
}

TEST_F(NativeDatabaseTest, RejectsMalformedRecordsDuringRemovalAndEviction)
{
  limits.rows = 4;
  NativeTileDatabase db(path, limits);
  for(size_t i = 0; i < limits.rows; ++i)
    ASSERT_TRUE(db.write(util::SHA256::hash(std::to_string(i)), std::string(24, 'x')));
  const auto key = util::SHA256::hash("0");
  auto raw = mapbox::sqlite::Database::open(path, mapbox::sqlite::ReadWriteCreate);
  const auto update = [&](const char* sql)
  {
    mapbox::sqlite::Statement statement(raw, sql);
    mapbox::sqlite::Query query(statement);
    query.bindBlob(1, key.data(), key.size(), false);
    query.run();
  };
  update("UPDATE native_tiles SET data=CAST(zeroblob(65536) AS TEXT),bytes=65536 WHERE key=?1");
  EXPECT_FALSE(db.recordSize(key));
  EXPECT_FALSE(db.read(key));
  EXPECT_THROW(db.erase(key), std::runtime_error);
  const auto next = util::SHA256::hash("next");
  EXPECT_THROW(db.write(next, std::string(24, 'y')), std::runtime_error);
  EXPECT_EQ(db.storedBytes(), 96u);
  update("UPDATE native_tiles SET data=zeroblob(24),bytes=24 WHERE key=?1");
  EXPECT_NO_THROW(db.erase(key));
  EXPECT_EQ(db.storedBytes(), 72u);
  EXPECT_TRUE(db.write(next, std::string(24, 'y')));
}

TEST_F(NativeDatabaseTest, PhysicalPressureEvictsBeforeLogicalOrRowQuota)
{
  {
    auto raw = mapbox::sqlite::Database::open(path, mapbox::sqlite::ReadWriteCreate);
    raw.exec("PRAGMA page_size=512");
    raw.exec("PRAGMA user_version=1");
  }
  limits = {256, 32 * 1024, 1024, 4096};
  NativeTileDatabase db(path, limits);
  constexpr size_t count = 500;
  ASSERT_LT(count * 24, limits.diskBytes);
  ASSERT_LT(count, limits.rows);
  for(size_t i = 0; i < count; ++i)
  {
    const auto key = util::SHA256::hash(std::to_string(i));
    bool written = false;
    for(size_t turn = 0; turn < 16 && !written; ++turn)
      written = db.write(key, std::string(24, 'x'));
    ASSERT_TRUE(written) << i;
  }
  EXPECT_FALSE(db.read(util::SHA256::hash("0")));
  EXPECT_TRUE(db.read(util::SHA256::hash(std::to_string(count - 1))));
  EXPECT_LT(db.storedBytes(), count * 24);
  const auto maxPages = (limits.diskBytes + limits.recordBytes) / 512 + 32;
  EXPECT_LE(std::filesystem::file_size(path), maxPages * 512);
}

TEST_F(NativeDatabaseTest, FailedMaintenanceCommitRestoresAccountingAndRows)
{
  limits = {4096, 4096, 1024, 128};
  NativeTileDatabase db(path, limits);
  for(size_t i = 0; i < 100; ++i)
    ASSERT_TRUE(db.write(util::SHA256::hash(std::to_string(i)), std::string(24, 'x')));
  auto reader = mapbox::sqlite::Database::open(path, mapbox::sqlite::ReadWriteCreate);
  reader.exec("BEGIN");
  {
    mapbox::sqlite::Statement statement(reader, "SELECT data FROM native_tiles LIMIT 1");
    mapbox::sqlite::Query query(statement);
    ASSERT_TRUE(query.run());
  }
  const auto key = util::SHA256::hash("large");
  EXPECT_THROW(db.write(key, std::string(4000, 'y')), mapbox::sqlite::Exception);
  EXPECT_EQ(db.storedBytes(), 2400u);
  reader.exec("ROLLBACK");
  EXPECT_TRUE(db.read(util::SHA256::hash("0")));
  EXPECT_TRUE(db.read(util::SHA256::hash("99")));
  EXPECT_FALSE(db.write(key, std::string(4000, 'y')));
  EXPECT_EQ(db.storedBytes(), (100u - 32u) * 24u);
}

TEST_F(NativeDatabaseTest, PublicFactoryKeepsBoundedStoreInterface)
{
  auto store = createNativeTileStore(path, limits);
  const auto key = util::SHA256::hash("factory");
  const std::string record(100, 'x');
  ASSERT_TRUE(store->write(key, record));
  EXPECT_EQ(store->recordSize(key), record.size());
  EXPECT_FALSE(store->read(key, record.size() - 1));
  EXPECT_EQ(store->read(key, record.size()), record);
  store->clear();
  EXPECT_EQ(store->storedBytes(), 0u);
}

TEST_F(NativeDatabaseTest, OwnedFactoryRecoversCorruptionAndPreservesNeighbor)
{
  const auto directory = std::filesystem::path(path + ".owned");
  std::filesystem::create_directory(directory);
  const auto owned = directory / "native-tiles-v1.db";
  const auto neighbor = directory / "keep.txt";
  {
    std::ofstream(owned, std::ios::binary) << "corrupt database";
    std::ofstream(neighbor) << "preserved";
  }
  {
    auto store = createNativeTileStore(owned.string(), limits);
    EXPECT_EQ(store->storedBytes(), 0u);
    EXPECT_TRUE(store->write(util::SHA256::hash("recovered"), std::string(24, 'x')));
  }
  std::ifstream input(neighbor);
  std::string kept;
  input >> kept;
  EXPECT_EQ(kept, "preserved");
  input.close();
  std::filesystem::remove_all(directory);
}

TEST_F(NativeDatabaseTest, OwnedFactoryRecoversRejectedQuotaAndSchema)
{
  const auto directory = std::filesystem::path(path + ".owned");
  std::filesystem::create_directory(directory);
  const auto owned = directory / "native-tiles-v1.db";
  {
    NativeTileDatabase database(owned.string(), limits);
  }
  {
    auto raw = mapbox::sqlite::Database::open(owned.string(), mapbox::sqlite::ReadWriteCreate);
    raw.exec("CREATE TABLE junk(data BLOB)");
    raw.exec("INSERT INTO junk VALUES(zeroblob(1000000))");
  }
  {
    auto store = createNativeTileStore(owned.string(), limits);
    EXPECT_EQ(store->storedBytes(), 0u);
  }
  std::filesystem::remove(owned);
  {
    auto raw = mapbox::sqlite::Database::open(owned.string(), mapbox::sqlite::ReadWriteCreate);
    raw.exec("CREATE TABLE native_tiles(wrong TEXT)");
  }
  {
    auto store = createNativeTileStore(owned.string(), limits);
    EXPECT_TRUE(store->write(util::SHA256::hash("schema"), std::string(24, 'x')));
  }
  std::filesystem::remove_all(directory);
}

TEST_F(NativeDatabaseTest, FactoryDoesNotDeleteUnownedOrNonRegularPaths)
{
  {
    std::ofstream(path, std::ios::binary) << "corrupt database";
  }
  EXPECT_THROW(createNativeTileStore(path, limits), std::runtime_error);
  EXPECT_EQ(std::filesystem::file_size(path), 16u);
  const auto directory = std::filesystem::path(path + ".owned");
  std::filesystem::create_directory(directory);
  const auto owned = directory / "native-tiles-v1.db";
  std::filesystem::create_symlink(path, owned);
  EXPECT_THROW(createNativeTileStore(owned.string(), limits), std::runtime_error);
  EXPECT_TRUE(std::filesystem::is_symlink(owned));
  EXPECT_EQ(std::filesystem::file_size(path), 16u);
  std::filesystem::remove(owned);
  std::filesystem::create_directory(owned);
  EXPECT_THROW(createNativeTileStore(owned.string(), limits), std::exception);
  EXPECT_TRUE(std::filesystem::is_directory(owned));
  std::filesystem::remove_all(directory);
}
