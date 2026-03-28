#include "rocksdb_wrapper.h"
#include "test_utils.h"

#include <filesystem>
#include <string>
#include <vector>

using test_utils::Fail;
using test_utils::MakeTempDir;

namespace {
void ExpectError(const std::string& label,
                 bool ok,
                 const std::string& actual,
                 const std::string& expected) {
  if (ok) {
    Fail(label + " unexpectedly succeeded");
  }
  if (actual != expected) {
    Fail(label + " expected '" + expected + "', got '" + actual + "'");
  }
}

}  // namespace

int main() {
  std::string error;
  const std::vector<std::vector<uint8_t>> path = {{'r', 'o', 'o', 't'}};
  const std::vector<uint8_t> key = {'k'};
  const std::vector<uint8_t> value = {'v'};

  {
    std::array<uint8_t, 32> out{};
    error.clear();
    ExpectError("BuildPrefix null out",
                grovedb::RocksDbWrapper::BuildPrefix(path, nullptr, &error),
                error,
                "output is null");
    error.clear();
    if (!grovedb::RocksDbWrapper::BuildPrefix(path, &out, &error)) {
      Fail("BuildPrefix valid call failed: " + error);
    }
  }

  {
    grovedb::RocksDbWrapper unopened;
    std::vector<uint8_t> out;
    bool found = false;
    bool deleted = false;
    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> rows;
    grovedb::RocksDbWrapper::WriteBatch batch;
    grovedb::BatchCost cost;
    grovedb::RocksDbWrapper::Transaction tx;

    error.clear();
    ExpectError("unopened Put",
                unopened.Put(grovedb::ColumnFamilyKind::kDefault, path, key, value, &error),
                error,
                "database not opened");
    error.clear();
    ExpectError("unopened Get",
                unopened.Get(grovedb::ColumnFamilyKind::kDefault, path, key, &out, &found, &error),
                error,
                "database not opened");
    error.clear();
    ExpectError("unopened Delete",
                unopened.Delete(grovedb::ColumnFamilyKind::kDefault, path, key, &deleted, &error),
                error,
                "database not opened");
    error.clear();
    ExpectError("unopened ScanPrefix",
                unopened.ScanPrefix(grovedb::ColumnFamilyKind::kDefault, path, &rows, &error),
                error,
                "database not opened");
    error.clear();
    ExpectError("unopened DeletePrefix",
                unopened.DeletePrefix(grovedb::ColumnFamilyKind::kDefault, path, &error),
                error,
                "database not opened");
    error.clear();
    ExpectError("unopened Clear",
                unopened.Clear(grovedb::ColumnFamilyKind::kDefault, &error),
                error,
                "database not opened");
    error.clear();
    ExpectError("unopened CreateCheckpoint",
                unopened.CreateCheckpoint(MakeTempDir("storage_api_contract_checkpoint"), &error),
                error,
                "database not opened");
    error.clear();
    ExpectError("unopened BeginTransaction", unopened.BeginTransaction(&tx, &error), error, "database not opened");
    error.clear();
    ExpectError("unopened CommitBatch", unopened.CommitBatch(batch, &error), error, "database not opened");
    error.clear();
    ExpectError("unopened CommitBatchWithCost",
                unopened.CommitBatchWithCost(batch, &cost, &error),
                error,
                "database not opened");
  }

  const std::string dir = MakeTempDir("storage_api_contract_main");
  grovedb::RocksDbWrapper db;
  if (!db.Open(dir, &error)) {
    Fail("open db failed: " + error);
  }
  if (!db.Put(grovedb::ColumnFamilyKind::kDefault, path, key, value, &error)) {
    Fail("seed put failed: " + error);
  }

  {
    bool found = false;
    bool deleted = false;
    std::vector<uint8_t> out;
    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> rows;
    grovedb::RocksDbWrapper::WriteBatch batch;
    grovedb::BatchCost cost;

    error.clear();
    ExpectError("Get null value",
                db.Get(grovedb::ColumnFamilyKind::kDefault, path, key, nullptr, &found, &error),
                error,
                "output is null");
    error.clear();
    ExpectError("Get null found",
                db.Get(grovedb::ColumnFamilyKind::kDefault, path, key, &out, nullptr, &error),
                error,
                "output is null");
    error.clear();
    ExpectError("Delete null deleted",
                db.Delete(grovedb::ColumnFamilyKind::kDefault, path, key, nullptr, &error),
                error,
                "deleted output is null");
    error.clear();
    ExpectError("ScanPrefix null out",
                db.ScanPrefix(grovedb::ColumnFamilyKind::kDefault, path, nullptr, &error),
                error,
                "output is null");
    error.clear();
    ExpectError("CommitBatchWithCost null cost",
                db.CommitBatchWithCost(batch, nullptr, &error),
                error,
                "cost output is null");
  }

  {
    grovedb::RocksDbWrapper::PrefixedContext null_ctx(
        nullptr, grovedb::ColumnFamilyKind::kDefault, path);
    std::vector<uint8_t> out;
    bool found = false;
    bool deleted = false;
    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> rows;
    error.clear();
    ExpectError("PrefixedContext null Put", null_ctx.Put(key, value, &error), error, "storage is null");
    error.clear();
    ExpectError("PrefixedContext null Get",
                null_ctx.Get(key, &out, &found, &error),
                error,
                "storage is null");
    error.clear();
    ExpectError("PrefixedContext null Delete",
                null_ctx.Delete(key, &deleted, &error),
                error,
                "storage is null");
    error.clear();
    ExpectError("PrefixedContext null Scan", null_ctx.Scan(&rows, &error), error, "storage is null");
  }

  {
    grovedb::RocksDbWrapper::Transaction tx;
    if (!db.BeginTransaction(&tx, &error)) {
      Fail("begin transaction failed: " + error);
    }
    bool found = false;
    std::vector<uint8_t> out;
    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> rows;
    error.clear();
    ExpectError("Tx Get null value",
                tx.Get(grovedb::ColumnFamilyKind::kDefault, path, key, nullptr, &found, &error),
                error,
                "output is null");
    error.clear();
    ExpectError("Tx Get null found",
                tx.Get(grovedb::ColumnFamilyKind::kDefault, path, key, &out, nullptr, &error),
                error,
                "output is null");
    error.clear();
    ExpectError("Tx ScanPrefix null out",
                tx.ScanPrefix(grovedb::ColumnFamilyKind::kDefault, path, nullptr, &error),
                error,
                "output is null");
    if (!tx.Rollback(&error)) {
      Fail("transaction rollback failed: " + error);
    }
  }

  {
    grovedb::RocksDbWrapper::WriteBatch batch;
    grovedb::RocksDbWrapper::Transaction tx;
    if (!db.BeginTransaction(&tx, &error)) {
      Fail("begin transaction for batch failed: " + error);
    }
    error.clear();
    ExpectError("CommitBatchWithCost tx null cost",
                db.CommitBatchWithCost(batch, &tx, nullptr, &error),
                error,
                "cost output is null");
    if (!tx.Rollback(&error)) {
      Fail("rollback after null cost check failed: " + error);
    }
  }

  std::filesystem::remove_all(dir);
  return 0;
}
