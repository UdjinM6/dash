#include "rocksdb_wrapper.h"
#include "test_utils.h"

#include <filesystem>
#include <string>
#include <vector>

using test_utils::Fail;
using test_utils::MakeTempDir;

namespace {
void ExpectClosedTxError(const std::string& label, bool ok, const std::string& error) {
  if (ok) {
    Fail(label + " unexpectedly succeeded");
  }
  if (error != "transaction not initialized") {
    Fail(label + " expected 'transaction not initialized', got: " + error);
  }
}
}  // namespace

int main() {
  const std::string dir = MakeTempDir("storage_tx_contract_main");
  grovedb::RocksDbWrapper db;
  std::string error;
  if (!db.Open(dir, &error)) {
    Fail("open failed: " + error);
  }

  error.clear();
  if (db.BeginTransaction(nullptr, &error)) {
    Fail("BeginTransaction with null output should fail");
  }
  if (error != "transaction output is null") {
    Fail("unexpected BeginTransaction null error: " + error);
  }

  const std::vector<std::vector<uint8_t>> path = {{'r', 'o', 'o', 't'}};
  const std::vector<uint8_t> key = {'k'};
  const std::vector<uint8_t> value = {'v'};

  grovedb::RocksDbWrapper::Transaction tx;
  if (!db.BeginTransaction(&tx, &error)) {
    Fail("begin tx failed: " + error);
  }
  if (!tx.Put(grovedb::ColumnFamilyKind::kDefault, path, key, value, &error)) {
    Fail("initial put failed: " + error);
  }
  if (!tx.Rollback(&error)) {
    Fail("rollback failed: " + error);
  }

  error.clear();
  ExpectClosedTxError("commit-after-rollback", tx.Commit(&error), error);
  error.clear();
  ExpectClosedTxError("rollback-after-rollback", tx.Rollback(&error), error);
  error.clear();
  ExpectClosedTxError(
      "put-after-rollback",
      tx.Put(grovedb::ColumnFamilyKind::kDefault, path, key, value, &error),
      error);
  error.clear();
  bool found = false;
  std::vector<uint8_t> out;
  ExpectClosedTxError(
      "get-after-rollback",
      tx.Get(grovedb::ColumnFamilyKind::kDefault, path, key, &out, &found, &error),
      error);
  error.clear();
  std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> entries;
  ExpectClosedTxError(
      "scan-after-rollback",
      tx.ScanPrefix(grovedb::ColumnFamilyKind::kDefault, path, &entries, &error),
      error);

  if (!db.BeginTransaction(&tx, &error)) {
    Fail("reuse begin tx failed: " + error);
  }
  if (!tx.Put(grovedb::ColumnFamilyKind::kDefault, path, key, value, &error)) {
    Fail("reuse put failed: " + error);
  }
  if (!tx.Commit(&error)) {
    Fail("reuse commit failed: " + error);
  }

  bool db_found = false;
  out.clear();
  if (!db.Get(grovedb::ColumnFamilyKind::kDefault, path, key, &out, &db_found, &error)) {
    Fail("db get after reuse commit failed: " + error);
  }
  if (!db_found || out != value) {
    Fail("db get after reuse commit returned wrong value");
  }

  error.clear();
  ExpectClosedTxError("rollback-after-commit", tx.Rollback(&error), error);
  error.clear();
  ExpectClosedTxError("commit-after-commit", tx.Commit(&error), error);

  {
    grovedb::RocksDbWrapper::TransactionPrefixedContext null_ctx(
        nullptr, grovedb::ColumnFamilyKind::kDefault, path);
    std::vector<uint8_t> out_ctx;
    bool found_ctx = false;
    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> entries_ctx;
    error.clear();
    if (null_ctx.Put(key, value, &error) || error != "transaction is null") {
      Fail("null prefixed context put contract mismatch");
    }
    error.clear();
    if (null_ctx.Get(key, &out_ctx, &found_ctx, &error) || error != "transaction is null") {
      Fail("null prefixed context get contract mismatch");
    }
    error.clear();
    if (null_ctx.Delete(key, &error) || error != "transaction is null") {
      Fail("null prefixed context delete contract mismatch");
    }
    error.clear();
    if (null_ctx.Scan(&entries_ctx, &error) || error != "transaction is null") {
      Fail("null prefixed context scan contract mismatch");
    }
  }

  std::filesystem::remove_all(dir);
  return 0;
}
