#include "rocksdb_wrapper.h"
#include "test_utils.h"

#include <chrono>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

using test_utils::Fail;
using test_utils::MakeTempDir;

namespace {
std::string MakeUniqueTempDir() {
  static std::atomic<uint64_t> counter{0};
  auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
  auto unique = counter.fetch_add(1, std::memory_order_relaxed);
  return MakeTempDir("rust_parity_" + std::to_string(now) + "_" + std::to_string(unique));
}
}  // namespace

int main() {
  const char* run = std::getenv("GROVEDB_RUN_RUST_PARITY");
  if (run == nullptr) {
    return 0;
  }
  std::string dir = MakeUniqueTempDir();
  std::string cmd =
      test_utils::RustToolsCargoRunPrefix() + "rust_storage_writer \"" +
      dir + "\"";
  if (std::system(cmd.c_str()) != 0) {
    Fail("failed to run rust storage writer");
  }

  grovedb::RocksDbWrapper db;
  std::string error;
  if (!db.Open(dir, &error)) {
    Fail("open db failed: " + error);
  }

  {
    grovedb::RocksDbWrapper::PrefixedContext ctx(
        &db, grovedb::ColumnFamilyKind::kDefault, {{'r', 'o', 'o', 't'}});
    std::vector<uint8_t> value;
    bool found = false;
    if (!ctx.Get({'k', '1'}, &value, &found, &error)) {
      Fail("get default failed: " + error);
    }
    if (!found || value != std::vector<uint8_t>({'v', '1'})) {
      Fail("default value mismatch");
    }
    if (!ctx.Get({'k', '2'}, &value, &found, &error)) {
      Fail("get default k2 failed: " + error);
    }
    if (!found || value != std::vector<uint8_t>({'v', '2'})) {
      Fail("default value k2 mismatch");
    }
  }

  {
    grovedb::RocksDbWrapper::PrefixedIterator it;
    if (!it.Init(&db, grovedb::ColumnFamilyKind::kDefault, {{'r', 'o', 'o', 't'}}, &error)) {
      Fail("iterator init failed: " + error);
    }
    if (!it.SeekToFirst(&error)) {
      Fail("iterator seek failed: " + error);
    }
    if (!it.Valid()) {
      Fail("iterator expected valid");
    }
    std::vector<uint8_t> key;
    if (!it.Key(&key, &error) || key != std::vector<uint8_t>({'k', '1'})) {
      Fail("iterator first key mismatch");
    }
    if (!it.Next(&error)) {
      Fail("iterator next failed: " + error);
    }
    if (!it.Valid()) {
      Fail("iterator expected second valid");
    }
    if (!it.Key(&key, &error) || key != std::vector<uint8_t>({'k', '2'})) {
      Fail("iterator second key mismatch");
    }
    if (!it.SeekToLast(&error)) {
      Fail("iterator seek last failed: " + error);
    }
    if (!it.Valid()) {
      Fail("iterator expected valid at last");
    }
    if (!it.Key(&key, &error) || key != std::vector<uint8_t>({'k', '2'})) {
      Fail("iterator last key mismatch");
    }
    if (!it.Prev(&error)) {
      Fail("iterator prev failed: " + error);
    }
    if (!it.Valid()) {
      Fail("iterator expected valid after prev");
    }
    if (!it.Key(&key, &error) || key != std::vector<uint8_t>({'k', '1'})) {
      Fail("iterator prev key mismatch");
    }
  }

  {
    grovedb::RocksDbWrapper::PrefixedContext ctx(
        &db,
        grovedb::ColumnFamilyKind::kDefault,
        {{'r', 'o', 'o', 't'}, {'c', 'h', 'i', 'l', 'd'}});
    std::vector<uint8_t> value;
    bool found = false;
    if (!ctx.Get({'k', '2'}, &value, &found, &error)) {
      Fail("get child failed: " + error);
    }
    if (!found || value != std::vector<uint8_t>({'v', '2'})) {
      Fail("child value mismatch");
    }
  }

  {
    grovedb::RocksDbWrapper::PrefixedContext ctx(
        &db, grovedb::ColumnFamilyKind::kAux, {{'r', 'o', 'o', 't'}});
    std::vector<uint8_t> value;
    bool found = false;
    if (!ctx.Get({'a', '1'}, &value, &found, &error)) {
      Fail("get aux failed: " + error);
    }
    if (!found || value != std::vector<uint8_t>({'a', 'v', '1'})) {
      Fail("aux value mismatch");
    }
  }

  {
    grovedb::RocksDbWrapper::PrefixedContext ctx(
        &db, grovedb::ColumnFamilyKind::kRoots, {{'r', 'o', 'o', 't'}});
    std::vector<uint8_t> value;
    bool found = false;
    if (!ctx.Get({'r', '1'}, &value, &found, &error)) {
      Fail("get roots failed: " + error);
    }
    if (!found || value != std::vector<uint8_t>({'r', 'v', '1'})) {
      Fail("roots value mismatch");
    }
  }

  {
    grovedb::RocksDbWrapper::PrefixedContext ctx(
        &db, grovedb::ColumnFamilyKind::kMeta, {{'r', 'o', 'o', 't'}});
    std::vector<uint8_t> value;
    bool found = false;
    if (!ctx.Get({'m', '1'}, &value, &found, &error)) {
      Fail("get meta failed: " + error);
    }
    if (!found || value != std::vector<uint8_t>({'m', 'v', '1'})) {
      Fail("meta value mismatch");
    }
  }

  {
    std::string staged_dir = MakeUniqueTempDir();
    std::string staged_cmd =
        test_utils::RustToolsCargoRunPrefix() + "rust_storage_writer \"" +
        staged_dir + "\" tx_ctx_batch_visibility";
    if (std::system(staged_cmd.c_str()) != 0) {
      Fail("failed to run rust storage writer (tx_ctx_batch_visibility)");
    }

    grovedb::RocksDbWrapper staged_db;
    if (!staged_db.Open(staged_dir, &error)) {
      Fail("open staged db failed: " + error);
    }
    grovedb::RocksDbWrapper::PrefixedContext staged_ctx(
        &staged_db, grovedb::ColumnFamilyKind::kDefault, {{'r', 'o', 'o', 't'}});
    std::vector<uint8_t> value;
    bool found = false;
    if (!staged_ctx.Get({'k', '1'}, &value, &found, &error)) {
      Fail("get staged k1 failed: " + error);
    }
    if (!found || value != std::vector<uint8_t>({'v', '1'})) {
      Fail("staged tx context parity value mismatch");
    }
    std::filesystem::remove_all(staged_dir);
  }

  {
    std::string staged_dir = MakeUniqueTempDir();
    std::string staged_cmd =
        test_utils::RustToolsCargoRunPrefix() + "rust_storage_writer \"" +
        staged_dir + "\" tx_ctx_no_batch_noop_multi_context";
    if (std::system(staged_cmd.c_str()) != 0) {
      Fail("failed to run rust storage writer (tx_ctx_no_batch_noop_multi_context)");
    }

    grovedb::RocksDbWrapper staged_db;
    if (!staged_db.Open(staged_dir, &error)) {
      Fail("open staged db failed: " + error);
    }
    std::vector<uint8_t> value;
    bool found = false;

    grovedb::RocksDbWrapper::PrefixedContext root_ctx(
        &staged_db, grovedb::ColumnFamilyKind::kDefault, {{'r', 'o', 'o', 't'}});
    if (!root_ctx.Get({'k', '_', 's', 'e', 'e', 'd'}, &value, &found, &error)) {
      Fail("get root seeded key failed: " + error);
    }
    if (!found || value != std::vector<uint8_t>({'v', '_', 's', 'e', 'e', 'd'})) {
      Fail("root seeded key mismatch for no-batch tx context mode");
    }
    if (!root_ctx.Get({'k', '_', 'n', 'e', 'w'}, &value, &found, &error)) {
      Fail("get root no-batch key failed: " + error);
    }
    if (found) {
      Fail("root no-batch key should not be persisted");
    }

    grovedb::RocksDbWrapper::PrefixedContext other_aux_ctx(
        &staged_db, grovedb::ColumnFamilyKind::kAux, {{'o', 't', 'h', 'e', 'r'}});
    if (!other_aux_ctx.Get({'a', '_', 's', 'e', 'e', 'd'}, &value, &found, &error)) {
      Fail("get aux seeded key failed: " + error);
    }
    if (!found || value != std::vector<uint8_t>({'a', 'v', '_', 's', 'e', 'e', 'd'})) {
      Fail("aux seeded key mismatch for no-batch tx context mode");
    }
    if (!other_aux_ctx.Get({'a', '_', 'n', 'e', 'w'}, &value, &found, &error)) {
      Fail("get aux no-batch key failed: " + error);
    }
    if (found) {
      Fail("aux no-batch key should not be persisted");
    }

    std::filesystem::remove_all(staged_dir);
  }

  {
    std::string staged_dir = MakeUniqueTempDir();
    std::string staged_cmd =
        test_utils::RustToolsCargoRunPrefix() + "rust_storage_writer \"" +
        staged_dir + "\" tx_ctx_batch_delete_visibility";
    if (std::system(staged_cmd.c_str()) != 0) {
      Fail("failed to run rust storage writer (tx_ctx_batch_delete_visibility)");
    }

    grovedb::RocksDbWrapper staged_db;
    if (!staged_db.Open(staged_dir, &error)) {
      Fail("open staged db failed: " + error);
    }
    std::vector<uint8_t> value;
    bool found = false;

    grovedb::RocksDbWrapper::PrefixedContext root_ctx(
        &staged_db, grovedb::ColumnFamilyKind::kDefault, {{'r', 'o', 'o', 't'}});
    if (!root_ctx.Get({'k', '_', 's', 'e', 'e', 'd'}, &value, &found, &error)) {
      Fail("get deleted staged key failed: " + error);
    }
    if (found) {
      Fail("staged delete visibility mode should commit deleted key");
    }

    std::filesystem::remove_all(staged_dir);
  }

  {
    std::string staged_dir = MakeUniqueTempDir();
    std::string staged_cmd =
        test_utils::RustToolsCargoRunPrefix() + "rust_storage_writer \"" +
        staged_dir + "\" tx_ctx_batch_cross_context_visibility";
    if (std::system(staged_cmd.c_str()) != 0) {
      Fail("failed to run rust storage writer (tx_ctx_batch_cross_context_visibility)");
    }

    grovedb::RocksDbWrapper staged_db;
    if (!staged_db.Open(staged_dir, &error)) {
      Fail("open staged db failed: " + error);
    }

    grovedb::RocksDbWrapper::PrefixedContext root_ctx(
        &staged_db, grovedb::ColumnFamilyKind::kDefault, {{'r', 'o', 'o', 't'}});
    std::vector<uint8_t> value;
    bool found = false;
    if (!root_ctx.Get({'k', '_', 'n', 'e', 'w'}, &value, &found, &error)) {
      Fail("get cross-context staged key failed: " + error);
    }
    if (!found || value != std::vector<uint8_t>({'v', '_', 'n', 'e', 'w'})) {
      Fail("cross-context visibility mode should have committed key");
    }

    std::filesystem::remove_all(staged_dir);
  }

  {
    std::string staged_dir = MakeUniqueTempDir();
    std::string staged_cmd =
        test_utils::RustToolsCargoRunPrefix() + "rust_storage_writer \"" +
        staged_dir + "\" tx_ctx_batch_cross_context_delete_visibility";
    if (std::system(staged_cmd.c_str()) != 0) {
      Fail("failed to run rust storage writer (tx_ctx_batch_cross_context_delete_visibility)");
    }

    grovedb::RocksDbWrapper staged_db;
    if (!staged_db.Open(staged_dir, &error)) {
      Fail("open staged db failed: " + error);
    }

    grovedb::RocksDbWrapper::PrefixedContext root_ctx(
        &staged_db, grovedb::ColumnFamilyKind::kDefault, {{'r', 'o', 'o', 't'}});
    std::vector<uint8_t> value;
    bool found = false;
    if (!root_ctx.Get({'k', '_', 's', 'e', 'e', 'd'}, &value, &found, &error)) {
      Fail("get cross-context staged delete key failed: " + error);
    }
    if (found) {
      Fail("cross-context delete visibility mode should have deleted key");
    }

    std::filesystem::remove_all(staged_dir);
  }

  {
    std::string staged_dir = MakeUniqueTempDir();
    std::string staged_cmd =
        test_utils::RustToolsCargoRunPrefix() + "rust_storage_writer \"" +
        staged_dir + "\" tx_ctx_batch_cross_context_aux_visibility";
    if (std::system(staged_cmd.c_str()) != 0) {
      Fail("failed to run rust storage writer (tx_ctx_batch_cross_context_aux_visibility)");
    }

    grovedb::RocksDbWrapper staged_db;
    if (!staged_db.Open(staged_dir, &error)) {
      Fail("open staged db failed: " + error);
    }

    grovedb::RocksDbWrapper::PrefixedContext root_ctx(
        &staged_db, grovedb::ColumnFamilyKind::kAux, {{'r', 'o', 'o', 't'}});
    std::vector<uint8_t> value;
    bool found = false;
    if (!root_ctx.Get({'a', '_', 'n', 'e', 'w'}, &value, &found, &error)) {
      Fail("get cross-context staged aux key failed: " + error);
    }
    if (!found || value != std::vector<uint8_t>({'a', 'v', '_', 'n', 'e', 'w'})) {
      Fail("cross-context aux visibility mode should have committed aux key");
    }

    std::filesystem::remove_all(staged_dir);
  }

  {
    std::string staged_dir = MakeUniqueTempDir();
    std::string staged_cmd =
        test_utils::RustToolsCargoRunPrefix() + "rust_storage_writer \"" +
        staged_dir + "\" tx_ctx_batch_cross_context_roots_meta_visibility";
    if (std::system(staged_cmd.c_str()) != 0) {
      Fail(
          "failed to run rust storage writer (tx_ctx_batch_cross_context_roots_meta_visibility)");
    }

    grovedb::RocksDbWrapper staged_db;
    if (!staged_db.Open(staged_dir, &error)) {
      Fail("open staged db failed: " + error);
    }

    std::vector<uint8_t> value;
    bool found = false;

    grovedb::RocksDbWrapper::PrefixedContext roots_ctx(
        &staged_db, grovedb::ColumnFamilyKind::kRoots, {{'r', 'o', 'o', 't'}});
    if (!roots_ctx.Get({'r', '_', 'n', 'e', 'w'}, &value, &found, &error)) {
      Fail("get cross-context staged roots key failed: " + error);
    }
    if (!found || value != std::vector<uint8_t>({'r', 'v', '_', 'n', 'e', 'w'})) {
      Fail("cross-context roots visibility mode should have committed roots key");
    }

    grovedb::RocksDbWrapper::PrefixedContext meta_ctx(
        &staged_db, grovedb::ColumnFamilyKind::kMeta, {{'r', 'o', 'o', 't'}});
    if (!meta_ctx.Get({'m', '_', 's', 'e', 'e', 'd'}, &value, &found, &error)) {
      Fail("get cross-context staged meta key failed: " + error);
    }
    if (found) {
      Fail("cross-context roots/meta visibility mode should have deleted meta key");
    }

    std::filesystem::remove_all(staged_dir);
  }

  {
    std::string staged_dir = MakeUniqueTempDir();
    std::string staged_cmd =
        test_utils::RustToolsCargoRunPrefix() + "rust_storage_writer \"" +
        staged_dir + "\" tx_ctx_multi_batch_orchestration";
    if (std::system(staged_cmd.c_str()) != 0) {
      Fail("failed to run rust storage writer (tx_ctx_multi_batch_orchestration)");
    }

    grovedb::RocksDbWrapper staged_db;
    if (!staged_db.Open(staged_dir, &error)) {
      Fail("open staged db failed: " + error);
    }

    std::vector<uint8_t> value;
    bool found = false;
    grovedb::RocksDbWrapper::PrefixedContext root_ctx(
        &staged_db, grovedb::ColumnFamilyKind::kDefault, {{'r', 'o', 'o', 't'}});
    if (!root_ctx.Get({'k', '_', 'n', 'e', 'w'}, &value, &found, &error)) {
      Fail("get multi-batch default key failed: " + error);
    }
    if (!found || value != std::vector<uint8_t>({'v', '_', 'n', 'e', 'w'})) {
      Fail("multi-batch orchestration default key mismatch");
    }

    grovedb::RocksDbWrapper::PrefixedContext aux_ctx(
        &staged_db, grovedb::ColumnFamilyKind::kAux, {{'r', 'o', 'o', 't'}});
    if (!aux_ctx.Get({'a', '_', 'n', 'e', 'w'}, &value, &found, &error)) {
      Fail("get multi-batch aux key failed: " + error);
    }
    if (!found || value != std::vector<uint8_t>({'a', 'v', '_', 'n', 'e', 'w'})) {
      Fail("multi-batch orchestration aux key mismatch");
    }

    std::filesystem::remove_all(staged_dir);
  }

  {
    std::string staged_dir = MakeUniqueTempDir();
    std::string staged_cmd =
        test_utils::RustToolsCargoRunPrefix() + "rust_storage_writer \"" +
        staged_dir + "\" tx_ctx_iterator_staged_writes";
    if (std::system(staged_cmd.c_str()) != 0) {
      Fail("failed to run rust storage writer (tx_ctx_iterator_staged_writes)");
    }

    grovedb::RocksDbWrapper staged_db;
    if (!staged_db.Open(staged_dir, &error)) {
      Fail("open staged db failed: " + error);
    }

    grovedb::RocksDbWrapper::PrefixedContext root_ctx(
        &staged_db, grovedb::ColumnFamilyKind::kDefault, {{'r', 'o', 'o', 't'}});

    std::vector<uint8_t> value;
    bool found = false;
    if (!root_ctx.Get({'k', '1'}, &value, &found, &error)) {
      Fail("get k1 failed: " + error);
    }
    if (!found || value != std::vector<uint8_t>({'v', '1'})) {
      Fail("k1 value mismatch");
    }
    if (!root_ctx.Get({'k', '2'}, &value, &found, &error)) {
      Fail("get k2 failed: " + error);
    }
    if (!found || value != std::vector<uint8_t>({'v', '2'})) {
      Fail("k2 value mismatch - staged insert should be persisted");
    }
    found = false;
    value.clear();
    if (!root_ctx.Get({'k', '3'}, &value, &found, &error)) {
      Fail("get k3 failed: " + error);
    }
    if (found) {
      Fail("k3 should be deleted by staged write");
    }
    if (!root_ctx.Get({'k', '4'}, &value, &found, &error)) {
      Fail("get k4 failed: " + error);
    }
    if (!found || value != std::vector<uint8_t>({'v', '4'})) {
      Fail("k4 value mismatch - staged insert should be persisted");
    }

    std::filesystem::remove_all(staged_dir);
  }

  {
    std::string staged_dir = MakeUniqueTempDir();
    std::string staged_cmd =
        test_utils::RustToolsCargoRunPrefix() + "rust_storage_writer \"" +
        staged_dir + "\" tx_ctx_delete_prefix_visibility";
    if (std::system(staged_cmd.c_str()) != 0) {
      Fail("failed to run rust storage writer (tx_ctx_delete_prefix_visibility)");
    }

    grovedb::RocksDbWrapper staged_db;
    if (!staged_db.Open(staged_dir, &error)) {
      Fail("open staged db failed: " + error);
    }

    std::vector<uint8_t> value;
    bool found = false;
    grovedb::RocksDbWrapper::PrefixedContext ctx(
        &staged_db, grovedb::ColumnFamilyKind::kDefault, {{'r', 'o', 'o', 't'}});

    // k1 should remain after staged deletes
    if (!ctx.Get({'k', '1'}, &value, &found, &error)) {
      Fail("get k1 failed: " + error);
    }
    if (!found || value != std::vector<uint8_t>({'v', '1'})) {
      Fail("k1 should remain after staged deletes");
    }

    // k2 should be deleted
    if (!ctx.Get({'k', '2'}, &value, &found, &error)) {
      Fail("get k2 failed: " + error);
    }
    if (found) {
      Fail("k2 should be deleted by staged delete");
    }

    // k3 should be deleted
    if (!ctx.Get({'k', '3'}, &value, &found, &error)) {
      Fail("get k3 failed: " + error);
    }
    if (found) {
      Fail("k3 should be deleted by staged delete");
    }

    // k4 should remain after staged deletes
    if (!ctx.Get({'k', '4'}, &value, &found, &error)) {
      Fail("get k4 failed: " + error);
    }
    if (!found || value != std::vector<uint8_t>({'v', '4'})) {
      Fail("k4 should remain after staged deletes");
    }

    std::filesystem::remove_all(staged_dir);
  }

  {
    std::string staged_dir = MakeUniqueTempDir();
    std::string staged_cmd =
        test_utils::RustToolsCargoRunPrefix() + "rust_storage_writer \"" +
        staged_dir + "\" tx_ctx_aux_batch_composition";
    if (std::system(staged_cmd.c_str()) != 0) {
      Fail("failed to run rust storage writer (tx_ctx_aux_batch_composition)");
    }

    grovedb::RocksDbWrapper staged_db;
    if (!staged_db.Open(staged_dir, &error)) {
      Fail("open staged db failed: " + error);
    }

    std::vector<uint8_t> value;
    bool found = false;
    grovedb::RocksDbWrapper::PrefixedContext aux_ctx(
        &staged_db, grovedb::ColumnFamilyKind::kAux, {{'r', 'o', 'o', 't'}});

    // a1 should persist
    if (!aux_ctx.Get({'a', '1'}, &value, &found, &error)) {
      Fail("get a1 failed: " + error);
    }
    if (!found || value != std::vector<uint8_t>({'v', '1'})) {
      Fail("a1 should persist after aux batch composition");
    }

    // a3 should be inserted
    if (!aux_ctx.Get({'a', '3'}, &value, &found, &error)) {
      Fail("get a3 failed: " + error);
    }
    if (!found || value != std::vector<uint8_t>({'v', '3'})) {
      Fail("a3 should be inserted by aux batch composition");
    }

    // a2 should be deleted
    if (!aux_ctx.Get({'a', '2'}, &value, &found, &error)) {
      Fail("get a2 failed: " + error);
    }
    if (found) {
      Fail("a2 should be deleted by aux batch composition");
    }

    std::filesystem::remove_all(staged_dir);
  }

  {
    std::string staged_dir = MakeUniqueTempDir();
    std::string staged_cmd =
        test_utils::RustToolsCargoRunPrefix() + "rust_storage_writer \"" +
        staged_dir + "\" tx_ctx_iterator_roots_meta_staged";
    if (std::system(staged_cmd.c_str()) != 0) {
      Fail("failed to run rust storage writer (tx_ctx_iterator_roots_meta_staged)");
    }

    grovedb::RocksDbWrapper staged_db;
    if (!staged_db.Open(staged_dir, &error)) {
      Fail("open staged db failed: " + error);
    }

    // Verify roots CF: r1 and r2 should exist
    std::vector<uint8_t> value;
    bool found = false;
    grovedb::RocksDbWrapper::PrefixedContext roots_ctx(
        &staged_db, grovedb::ColumnFamilyKind::kRoots, {{'r', 'o', 'o', 't'}});

    if (!roots_ctx.Get({'r', '1'}, &value, &found, &error)) {
      Fail("get r1 failed: " + error);
    }
    if (!found || value != std::vector<uint8_t>({'r', 'v', '1'})) {
      Fail("r1 should exist after roots/meta staged writes");
    }

    if (!roots_ctx.Get({'r', '2'}, &value, &found, &error)) {
      Fail("get r2 failed: " + error);
    }
    if (!found || value != std::vector<uint8_t>({'r', 'v', '2'})) {
      Fail("r2 should be inserted by roots/meta staged writes");
    }

    // Verify meta CF: m1 should be deleted
    grovedb::RocksDbWrapper::PrefixedContext meta_ctx(
        &staged_db, grovedb::ColumnFamilyKind::kMeta, {{'r', 'o', 'o', 't'}});

    if (!meta_ctx.Get({'m', '1'}, &value, &found, &error)) {
      Fail("get m1 failed: " + error);
    }
    if (found) {
      Fail("m1 should be deleted by roots/meta staged writes");
    }

    std::filesystem::remove_all(staged_dir);
  }

  {
    std::string staged_dir = MakeUniqueTempDir();
    std::string staged_cmd =
        test_utils::RustToolsCargoRunPrefix() + "rust_storage_writer \"" +
        staged_dir + "\" tx_ctx_iterator_over_staged_roots_meta";
    if (std::system(staged_cmd.c_str()) != 0) {
      Fail("failed to run rust storage writer (tx_ctx_iterator_over_staged_roots_meta)");
    }

    grovedb::RocksDbWrapper staged_db;
    if (!staged_db.Open(staged_dir, &error)) {
      Fail("open staged db failed: " + error);
    }

    // Verify roots CF: r1 and r2 should exist after committed tx
    std::vector<uint8_t> value;
    bool found = false;
    grovedb::RocksDbWrapper::PrefixedContext roots_ctx(
        &staged_db, grovedb::ColumnFamilyKind::kRoots, {{'r', 'o', 'o', 't'}});

    if (!roots_ctx.Get({'r', '1'}, &value, &found, &error)) {
      Fail("get r1 failed: " + error);
    }
    if (!found || value != std::vector<uint8_t>({'r', 'v', '1'})) {
      Fail("r1 should exist after iterator over staged roots/meta");
    }

    if (!roots_ctx.Get({'r', '2'}, &value, &found, &error)) {
      Fail("get r2 failed: " + error);
    }
    if (!found || value != std::vector<uint8_t>({'r', 'v', '2'})) {
      Fail("r2 should be inserted by iterator over staged roots/meta");
    }

    // Verify meta CF: m1 should be deleted, m2 should exist after committed tx
    grovedb::RocksDbWrapper::PrefixedContext meta_ctx(
        &staged_db, grovedb::ColumnFamilyKind::kMeta, {{'r', 'o', 'o', 't'}});

    if (!meta_ctx.Get({'m', '1'}, &value, &found, &error)) {
      Fail("get m1 failed: " + error);
    }
    if (found) {
      Fail("m1 should be deleted by iterator over staged roots/meta");
    }

    if (!meta_ctx.Get({'m', '2'}, &value, &found, &error)) {
      Fail("get m2 failed: " + error);
    }
    if (!found || value != std::vector<uint8_t>({'m', 'v', '2'})) {
      Fail("m2 should be inserted by iterator over staged roots/meta");
    }

    std::filesystem::remove_all(staged_dir);
  }

  std::filesystem::remove_all(dir);
  return 0;
}
