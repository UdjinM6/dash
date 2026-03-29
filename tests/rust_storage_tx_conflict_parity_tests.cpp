#include "rocksdb_wrapper.h"
#include "test_utils.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using test_utils::Fail;
using test_utils::MakeTempDir;

namespace {

std::vector<uint8_t> GetOrFail(grovedb::RocksDbWrapper* db,
                               const std::vector<uint8_t>& key,
                               bool* found) {
  std::string error;
  std::vector<uint8_t> value;
  grovedb::RocksDbWrapper::PrefixedContext ctx(
      db, grovedb::ColumnFamilyKind::kDefault, {{'r', 'o', 'o', 't'}});
  if (!ctx.Get(key, &value, found, &error)) {
    Fail("read failed: " + error);
  }
  return value;
}

void RunCppScenario(const std::string& dir, const std::string& scenario) {
  grovedb::RocksDbWrapper db;
  std::string error;
  if (!db.Open(dir, &error)) {
    Fail("open C++ db failed: " + error);
  }
  if (scenario == "same_key") {
    grovedb::RocksDbWrapper::Transaction tx1;
    grovedb::RocksDbWrapper::Transaction tx2;
    if (!db.BeginTransaction(&tx1, &error)) {
      Fail("begin C++ tx1 failed: " + error);
    }
    if (!db.BeginTransaction(&tx2, &error)) {
      Fail("begin C++ tx2 failed: " + error);
    }
    if (!tx1.Put(grovedb::ColumnFamilyKind::kDefault, {{'r', 'o', 'o', 't'}}, {'k'}, {'v', '1'},
                 &error)) {
      Fail("C++ tx1 put failed: " + error);
    }
    if (!tx2.Put(grovedb::ColumnFamilyKind::kDefault, {{'r', 'o', 'o', 't'}}, {'k'}, {'v', '2'},
                 &error)) {
      Fail("C++ tx2 put failed: " + error);
    }
    if (!tx1.Commit(&error)) {
      Fail("C++ tx1 commit failed: " + error);
    }
    if (tx2.Commit(&error)) {
      Fail("C++ tx2 should conflict in same_key scenario");
    }
    tx2.Rollback(&error);
    return;
  }

  if (scenario == "disjoint") {
    grovedb::RocksDbWrapper::Transaction tx1;
    grovedb::RocksDbWrapper::Transaction tx2;
    if (!db.BeginTransaction(&tx1, &error)) {
      Fail("begin C++ tx1 failed: " + error);
    }
    if (!db.BeginTransaction(&tx2, &error)) {
      Fail("begin C++ tx2 failed: " + error);
    }
    if (!tx1.Put(grovedb::ColumnFamilyKind::kDefault,
                 {{'r', 'o', 'o', 't'}},
                 {'k', '1'},
                 {'v', '1'},
                 &error)) {
      Fail("C++ tx1 put disjoint failed: " + error);
    }
    if (!tx2.Put(grovedb::ColumnFamilyKind::kDefault,
                 {{'r', 'o', 'o', 't'}},
                 {'k', '2'},
                 {'v', '2'},
                 &error)) {
      Fail("C++ tx2 put disjoint failed: " + error);
    }
    if (!tx1.Commit(&error)) {
      Fail("C++ tx1 disjoint commit failed: " + error);
    }
    if (!tx2.Commit(&error)) {
      Fail("C++ tx2 disjoint commit failed: " + error);
    }
    return;
  }

  if (scenario == "delete_then_put_same_key") {
    grovedb::RocksDbWrapper::PrefixedContext seeded(
        &db, grovedb::ColumnFamilyKind::kDefault, {{'r', 'o', 'o', 't'}});
    if (!seeded.Put({'k'}, {'s', 'e', 'e', 'd'}, &error)) {
      Fail("C++ seed write failed: " + error);
    }
    grovedb::RocksDbWrapper::Transaction tx1;
    grovedb::RocksDbWrapper::Transaction tx2;
    if (!db.BeginTransaction(&tx1, &error)) {
      Fail("begin C++ tx1 failed: " + error);
    }
    if (!db.BeginTransaction(&tx2, &error)) {
      Fail("begin C++ tx2 failed: " + error);
    }
    if (!tx1.Delete(grovedb::ColumnFamilyKind::kDefault, {{'r', 'o', 'o', 't'}}, {'k'}, &error)) {
      Fail("C++ tx1 delete failed: " + error);
    }
    if (!tx2.Put(grovedb::ColumnFamilyKind::kDefault, {{'r', 'o', 'o', 't'}}, {'k'}, {'v', '2'},
                 &error)) {
      Fail("C++ tx2 put after delete failed: " + error);
    }
    if (!tx1.Commit(&error)) {
      Fail("C++ tx1 delete commit failed: " + error);
    }
    if (tx2.Commit(&error)) {
      Fail("C++ tx2 should conflict after tx1 delete");
    }
    tx2.Rollback(&error);
    return;
  }

  if (scenario == "delete_prefix_then_put_same_key") {
    grovedb::RocksDbWrapper::PrefixedContext seeded(
        &db, grovedb::ColumnFamilyKind::kDefault, {{'r', 'o', 'o', 't'}});
    if (!seeded.Put({'p'}, {'s', 'e', 'e', 'd'}, &error)) {
      Fail("C++ prefix seed write failed: " + error);
    }
    grovedb::RocksDbWrapper::Transaction tx1;
    grovedb::RocksDbWrapper::Transaction tx2;
    if (!db.BeginTransaction(&tx1, &error)) {
      Fail("begin C++ tx1 failed: " + error);
    }
    if (!db.BeginTransaction(&tx2, &error)) {
      Fail("begin C++ tx2 failed: " + error);
    }
    if (!tx1.Delete(grovedb::ColumnFamilyKind::kDefault, {{'r', 'o', 'o', 't'}}, {'p'}, &error)) {
      Fail("C++ tx1 prefix-delete failed: " + error);
    }
    if (!tx2.Put(grovedb::ColumnFamilyKind::kDefault, {{'r', 'o', 'o', 't'}}, {'p'}, {'v', '2'},
                 &error)) {
      Fail("C++ tx2 put after prefix-delete failed: " + error);
    }
    if (!tx1.Commit(&error)) {
      Fail("C++ tx1 prefix-delete commit failed: " + error);
    }
    if (tx2.Commit(&error)) {
      Fail("C++ tx2 should conflict after tx1 prefix-delete");
    }
    tx2.Rollback(&error);
    return;
  }

  if (scenario == "delete_prefix_multi_then_put_same_key") {
    grovedb::RocksDbWrapper::PrefixedContext seeded(
        &db, grovedb::ColumnFamilyKind::kDefault, {{'r', 'o', 'o', 't'}});
    if (!seeded.Put({'p', '1'}, {'s', 'e', 'e', 'd', '1'}, &error)) {
      Fail("C++ multi-prefix seed p1 failed: " + error);
    }
    if (!seeded.Put({'p', '2'}, {'s', 'e', 'e', 'd', '2'}, &error)) {
      Fail("C++ multi-prefix seed p2 failed: " + error);
    }
    grovedb::RocksDbWrapper::Transaction tx1;
    grovedb::RocksDbWrapper::Transaction tx2;
    if (!db.BeginTransaction(&tx1, &error)) {
      Fail("begin C++ tx1 failed: " + error);
    }
    if (!db.BeginTransaction(&tx2, &error)) {
      Fail("begin C++ tx2 failed: " + error);
    }
    if (!tx1.DeletePrefix(grovedb::ColumnFamilyKind::kDefault, {{'r', 'o', 'o', 't'}}, &error)) {
      Fail("C++ tx1 multi-prefix delete failed: " + error);
    }
    if (!tx2.Put(grovedb::ColumnFamilyKind::kDefault, {{'r', 'o', 'o', 't'}}, {'p', '1'},
                 {'v', '2'}, &error)) {
      Fail("C++ tx2 put after multi-prefix-delete failed: " + error);
    }
    if (!tx1.Commit(&error)) {
      Fail("C++ tx1 multi-prefix-delete commit failed: " + error);
    }
    if (tx2.Commit(&error)) {
      Fail("C++ tx2 should conflict after tx1 multi-prefix-delete");
    }
    tx2.Rollback(&error);
    return;
  }

  if (scenario == "delete_then_put_disjoint") {
    grovedb::RocksDbWrapper::PrefixedContext seeded(
        &db, grovedb::ColumnFamilyKind::kDefault, {{'r', 'o', 'o', 't'}});
    if (!seeded.Put({'k', '1'}, {'s', 'e', 'e', 'd', '1'}, &error)) {
      Fail("C++ disjoint seed k1 failed: " + error);
    }
    if (!seeded.Put({'k', '2'}, {'s', 'e', 'e', 'd', '2'}, &error)) {
      Fail("C++ disjoint seed k2 failed: " + error);
    }
    grovedb::RocksDbWrapper::Transaction tx1;
    grovedb::RocksDbWrapper::Transaction tx2;
    if (!db.BeginTransaction(&tx1, &error)) {
      Fail("begin C++ tx1 failed: " + error);
    }
    if (!db.BeginTransaction(&tx2, &error)) {
      Fail("begin C++ tx2 failed: " + error);
    }
    if (!tx1.Delete(grovedb::ColumnFamilyKind::kDefault, {{'r', 'o', 'o', 't'}}, {'k', '1'},
                    &error)) {
      Fail("C++ tx1 disjoint delete failed: " + error);
    }
    if (!tx2.Put(grovedb::ColumnFamilyKind::kDefault, {{'r', 'o', 'o', 't'}}, {'k', '2'},
                 {'v', '2'}, &error)) {
      Fail("C++ tx2 disjoint put failed: " + error);
    }
    if (!tx1.Commit(&error)) {
      Fail("C++ tx1 disjoint delete commit failed: " + error);
    }
    if (!tx2.Commit(&error)) {
      Fail("C++ tx2 disjoint put commit failed: " + error);
    }
    return;
  }

  Fail("unknown scenario: " + scenario);
}

void AssertStateEqual(const std::string& rust_dir, const std::string& cpp_dir) {
  grovedb::RocksDbWrapper rust_db;
  grovedb::RocksDbWrapper cpp_db;
  std::string error;
  if (!rust_db.Open(rust_dir, &error)) {
    Fail("open rust db failed: " + error);
  }
  if (!cpp_db.Open(cpp_dir, &error)) {
    Fail("open cpp db failed: " + error);
  }

  const std::vector<std::vector<uint8_t>> keys = {
      {'k'}, {'k', '1'}, {'k', '2'}, {'p'}, {'p', '1'}, {'p', '2'}};
  for (const auto& key : keys) {
    bool rust_found = false;
    bool cpp_found = false;
    const std::vector<uint8_t> rust_value = GetOrFail(&rust_db, key, &rust_found);
    const std::vector<uint8_t> cpp_value = GetOrFail(&cpp_db, key, &cpp_found);
    if (rust_found != cpp_found) {
      Fail("state mismatch for key presence");
    }
    if (rust_found && rust_value != cpp_value) {
      Fail("state mismatch for key value");
    }
  }
}

void RunScenario(const std::string& scenario) {
  const std::string rust_dir = MakeTempDir("rust_tx_conflict_parity_rust_" + scenario);
  const std::string cpp_dir = MakeTempDir("rust_tx_conflict_parity_cpp_" + scenario);

  std::string rust_cmd =
      test_utils::RustToolsCargoRunPrefix() + ""
      "rust_storage_tx_conflict_writer \"" +
      rust_dir + "\" " + scenario;
  if (std::system(rust_cmd.c_str()) != 0) {
    Fail("failed to run rust tx conflict writer for scenario: " + scenario);
  }
  RunCppScenario(cpp_dir, scenario);
  AssertStateEqual(rust_dir, cpp_dir);

  std::filesystem::remove_all(rust_dir);
  std::filesystem::remove_all(cpp_dir);
}
}  // namespace

int main() {
  const char* run = std::getenv("GROVEDB_RUN_RUST_PARITY");
  if (run == nullptr) {
    return 0;
  }

  RunScenario("same_key");
  RunScenario("disjoint");
  RunScenario("delete_then_put_same_key");
  RunScenario("delete_prefix_then_put_same_key");
  RunScenario("delete_prefix_multi_then_put_same_key");
  RunScenario("delete_then_put_disjoint");
  return 0;
}
