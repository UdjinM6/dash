#include "rocksdb_wrapper.h"
#include "test_utils.h"

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/utilities/optimistic_transaction_db.h>

using test_utils::Fail;
using test_utils::MakeTempDir;

namespace {

using KeyValue = std::pair<std::vector<uint8_t>, std::vector<uint8_t>>;

std::vector<KeyValue> DumpCf(rocksdb::OptimisticTransactionDB* db,
                             rocksdb::ColumnFamilyHandle* cf) {
  std::vector<KeyValue> out;
  rocksdb::ReadOptions read_options;
  std::unique_ptr<rocksdb::Iterator> it(db->NewIterator(read_options, cf));
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    const rocksdb::Slice key = it->key();
    const rocksdb::Slice value = it->value();
    out.emplace_back(std::vector<uint8_t>(key.data(), key.data() + key.size()),
                     std::vector<uint8_t>(value.data(), value.data() + value.size()));
  }
  if (!it->status().ok()) {
    Fail("iterator failure while dumping cf: " + it->status().ToString());
  }
  return out;
}

std::vector<std::vector<KeyValue>> DumpAllCfs(const std::string& path) {
  rocksdb::Options opts;
  opts.create_if_missing = false;
  opts.create_missing_column_families = false;
  std::vector<rocksdb::ColumnFamilyDescriptor> cfs;
  cfs.emplace_back(rocksdb::kDefaultColumnFamilyName, rocksdb::ColumnFamilyOptions());
  cfs.emplace_back("aux", rocksdb::ColumnFamilyOptions());
  cfs.emplace_back("roots", rocksdb::ColumnFamilyOptions());
  cfs.emplace_back("meta", rocksdb::ColumnFamilyOptions());

  std::vector<rocksdb::ColumnFamilyHandle*> handles;
  rocksdb::OptimisticTransactionDB* db_ptr = nullptr;
  rocksdb::Status status =
      rocksdb::OptimisticTransactionDB::Open(opts, path, cfs, &handles, &db_ptr);
  if (!status.ok()) {
    Fail("failed to open rocksdb for dump: " + status.ToString());
  }
  std::unique_ptr<rocksdb::OptimisticTransactionDB> db(db_ptr);
  std::vector<std::vector<KeyValue>> dump;
  for (rocksdb::ColumnFamilyHandle* handle : handles) {
    dump.push_back(DumpCf(db.get(), handle));
  }
  for (rocksdb::ColumnFamilyHandle* handle : handles) {
    db->DestroyColumnFamilyHandle(handle);
  }
  return dump;
}

void CompareDbDump(const std::string& scenario,
                   const std::string& rust_dir,
                   const std::string& cpp_dir) {
  const auto rust_dump = DumpAllCfs(rust_dir);
  const auto cpp_dump = DumpAllCfs(cpp_dir);
  if (rust_dump != cpp_dump) {
    Fail("raw RocksDB export mismatch for scenario: " + scenario);
  }
}

void RunRustWriter(const std::string& db_path,
                   const std::string& extra_arg,
                   const std::string& bin) {
  std::string cmd =
      test_utils::RustToolsCargoRunPrefix() + "" + bin + " \"" +
      db_path + "\"";
  if (!extra_arg.empty()) {
    cmd.append(" ");
    cmd.append(extra_arg);
  }
  if (std::system(cmd.c_str()) != 0) {
    Fail("failed to run rust writer: " + bin);
  }
}

void WriteCppDefault(const std::string& db_path) {
  grovedb::RocksDbWrapper db;
  std::string error;
  if (!db.Open(db_path, &error)) {
    Fail("cpp open failed: " + error);
  }
  if (!db.Put(grovedb::ColumnFamilyKind::kDefault,
              {{'r', 'o', 'o', 't'}},
              {'k', '1'},
              {'v', '1'},
              &error)) {
    Fail("cpp default put k1 failed: " + error);
  }
  if (!db.Put(grovedb::ColumnFamilyKind::kDefault,
              {{'r', 'o', 'o', 't'}},
              {'k', '2'},
              {'v', '2'},
              &error)) {
    Fail("cpp default put k2 failed: " + error);
  }
  if (!db.Put(grovedb::ColumnFamilyKind::kDefault,
              {{'r', 'o', 'o', 't'}, {'c', 'h', 'i', 'l', 'd'}},
              {'k', '2'},
              {'v', '2'},
              &error)) {
    Fail("cpp default put child/k2 failed: " + error);
  }
  if (!db.Put(grovedb::ColumnFamilyKind::kAux,
              {{'r', 'o', 'o', 't'}},
              {'a', '1'},
              {'a', 'v', '1'},
              &error)) {
    Fail("cpp aux put a1 failed: " + error);
  }
  if (!db.Put(grovedb::ColumnFamilyKind::kRoots,
              {{'r', 'o', 'o', 't'}},
              {'r', '1'},
              {'r', 'v', '1'},
              &error)) {
    Fail("cpp roots put r1 failed: " + error);
  }
  if (!db.Put(grovedb::ColumnFamilyKind::kMeta,
              {{'r', 'o', 'o', 't'}},
              {'m', '1'},
              {'m', 'v', '1'},
              &error)) {
    Fail("cpp meta put m1 failed: " + error);
  }
}

void WriteCppTxCommit(const std::string& db_path) {
  grovedb::RocksDbWrapper db;
  std::string error;
  if (!db.Open(db_path, &error)) {
    Fail("cpp open failed: " + error);
  }
  grovedb::RocksDbWrapper::Transaction tx;
  if (!db.BeginTransaction(&tx, &error)) {
    Fail("begin tx failed: " + error);
  }
  if (!tx.Put(grovedb::ColumnFamilyKind::kDefault,
              {{'r', 'o', 'o', 't'}},
              {'k', '1'},
              {'v', '1'},
              &error)) {
    Fail("tx put k1 failed: " + error);
  }
  if (!tx.Put(grovedb::ColumnFamilyKind::kDefault,
              {{'r', 'o', 'o', 't'}},
              {'k', '2'},
              {'v', '2'},
              &error)) {
    Fail("tx put k2 failed: " + error);
  }
  if (!tx.Delete(grovedb::ColumnFamilyKind::kDefault,
                 {{'r', 'o', 'o', 't'}},
                 {'k', '1'},
                 &error)) {
    Fail("tx delete k1 failed: " + error);
  }
  if (!tx.Put(grovedb::ColumnFamilyKind::kRoots,
              {{'r', 'o', 'o', 't'}},
              {'r', '1'},
              {'r', 'v', '1'},
              &error)) {
    Fail("tx put roots/r1 failed: " + error);
  }
  if (!tx.Commit(&error)) {
    Fail("tx commit failed: " + error);
  }
}

void WriteCppTxRollback(const std::string& db_path) {
  grovedb::RocksDbWrapper db;
  std::string error;
  if (!db.Open(db_path, &error)) {
    Fail("cpp open failed: " + error);
  }
  grovedb::RocksDbWrapper::Transaction tx;
  if (!db.BeginTransaction(&tx, &error)) {
    Fail("begin tx failed: " + error);
  }
  if (!tx.Put(grovedb::ColumnFamilyKind::kDefault,
              {{'r', 'o', 'o', 't'}},
              {'k', '1'},
              {'v', '1'},
              &error)) {
    Fail("tx put k1 failed: " + error);
  }
  if (!tx.Put(grovedb::ColumnFamilyKind::kDefault,
              {{'r', 'o', 'o', 't'}},
              {'k', '2'},
              {'v', '2'},
              &error)) {
    Fail("tx put k2 failed: " + error);
  }
  if (!tx.Delete(grovedb::ColumnFamilyKind::kDefault,
                 {{'r', 'o', 'o', 't'}},
                 {'k', '1'},
                 &error)) {
    Fail("tx delete k1 failed: " + error);
  }
  if (!tx.Put(grovedb::ColumnFamilyKind::kRoots,
              {{'r', 'o', 'o', 't'}},
              {'r', '1'},
              {'r', 'v', '1'},
              &error)) {
    Fail("tx put roots/r1 failed: " + error);
  }
  if (!tx.Rollback(&error)) {
    Fail("tx rollback failed: " + error);
  }
}

void WriteCppBatch(const std::string& db_path) {
  grovedb::RocksDbWrapper db;
  std::string error;
  if (!db.Open(db_path, &error)) {
    Fail("cpp open failed: " + error);
  }
  grovedb::RocksDbWrapper::WriteBatch batch;
  batch.Put(grovedb::ColumnFamilyKind::kDefault,
            {{'r', 'o', 'o', 't'}},
            {'k', '1'},
            {'v', '1'});
  batch.Put(grovedb::ColumnFamilyKind::kDefault,
            {{'r', 'o', 'o', 't'}},
            {'k', '2'},
            {'v', '2'});
  batch.Delete(grovedb::ColumnFamilyKind::kDefault, {{'r', 'o', 'o', 't'}}, {'k', '1'});
  if (!db.CommitBatch(batch, &error)) {
    Fail("commit batch failed: " + error);
  }
}

void RunScenario(const std::string& scenario,
                 const std::string& rust_bin,
                 const std::string& rust_extra,
                 void (*cpp_writer)(const std::string&)) {
  const std::string rust_dir = MakeTempDir("storage_export_parity_rust_" + scenario);
  const std::string cpp_dir = MakeTempDir("storage_export_parity_cpp_" + scenario);
  RunRustWriter(rust_dir, rust_extra, rust_bin);
  cpp_writer(cpp_dir);
  CompareDbDump(scenario, rust_dir, cpp_dir);
  std::filesystem::remove_all(rust_dir);
  std::filesystem::remove_all(cpp_dir);
}

}  // namespace

int main() {
  const char* run = std::getenv("GROVEDB_RUN_RUST_PARITY");
  if (run == nullptr) {
    return 0;
  }

  RunScenario("storage_default", "rust_storage_writer", "", &WriteCppDefault);
  RunScenario("storage_tx_commit", "rust_storage_tx_writer", "", &WriteCppTxCommit);
  RunScenario("storage_tx_rollback", "rust_storage_tx_writer", "rollback", &WriteCppTxRollback);
  RunScenario("storage_batch", "rust_storage_batch_writer", "", &WriteCppBatch);
  return 0;
}
