#include "rocksdb_wrapper.h"
#include "test_utils.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using test_utils::Fail;
using test_utils::MakeTempDir;

namespace {

uint64_t ParseU64(const std::vector<uint8_t>& bytes) {
  const std::string s(bytes.begin(), bytes.end());
  return static_cast<uint64_t>(std::stoull(s));
}

uint64_t ReadMarker(grovedb::RocksDbWrapper* db, const std::vector<uint8_t>& key) {
  std::string error;
  std::vector<uint8_t> value;
  bool found = false;
  if (!db->Get(grovedb::ColumnFamilyKind::kDefault, {{'r', 'o', 'o', 't'}}, key, &value, &found, &error)) {
    Fail("read marker failed: " + error);
  }
  if (!found) {
    Fail("missing marker key");
  }
  return ParseU64(value);
}

uint64_t g_cost_check_index = 0;

void AssertCost(const grovedb::BatchCost& cost,
                uint64_t seek,
                uint64_t loaded,
                uint64_t added,
                uint64_t replaced,
                uint64_t removed,
                uint64_t hash_calls) {
  g_cost_check_index += 1;
  if (cost.seek_count != seek) {
    Fail("cost seek mismatch at check " + std::to_string(g_cost_check_index) +
         " expected=" + std::to_string(seek) +
         " actual=" + std::to_string(cost.seek_count));
  }
  if (cost.storage_loaded_bytes != loaded) {
    Fail("cost loaded mismatch at check " + std::to_string(g_cost_check_index) +
         " expected=" + std::to_string(loaded) +
         " actual=" + std::to_string(cost.storage_loaded_bytes));
  }
  if (cost.storage_cost.added_bytes != added) {
    Fail("cost added mismatch at check " + std::to_string(g_cost_check_index) +
         " expected=" + std::to_string(added) +
         " actual=" + std::to_string(cost.storage_cost.added_bytes));
  }
  if (cost.storage_cost.replaced_bytes != replaced) {
    Fail("cost replaced mismatch at check " + std::to_string(g_cost_check_index) +
         " expected=" + std::to_string(replaced) +
         " actual=" + std::to_string(cost.storage_cost.replaced_bytes));
  }
  if (cost.storage_cost.removed_bytes.TotalRemovedBytes() != removed) {
    Fail("cost removed mismatch at check " + std::to_string(g_cost_check_index) +
         " expected=" + std::to_string(removed) +
         " actual=" + std::to_string(cost.storage_cost.removed_bytes.TotalRemovedBytes()));
  }
  if (cost.hash_node_calls != hash_calls) {
    Fail("cost hash mismatch at check " + std::to_string(g_cost_check_index) +
         " expected=" + std::to_string(hash_calls) +
         " actual=" + std::to_string(cost.hash_node_calls));
  }
  std::cout << "COST_CHECK index=" << g_cost_check_index
            << " seek=" << cost.seek_count
            << " loaded=" << cost.storage_loaded_bytes
            << " added=" << cost.storage_cost.added_bytes
            << " replaced=" << cost.storage_cost.replaced_bytes
            << " removed=" << cost.storage_cost.removed_bytes.TotalRemovedBytes()
            << " hash=" << cost.hash_node_calls
            << " status=PASS\n";
}
}  // namespace

int main() {
  const char* run = std::getenv("GROVEDB_RUN_RUST_PARITY");
  if (run == nullptr) {
    return 0;
  }
  const std::string rust_dir = MakeTempDir("rust_cost_shape_parity_rust");
  const std::string cpp_dir = MakeTempDir("rust_cost_shape_parity_cpp");

  std::string cmd =
      test_utils::RustToolsCargoRunPrefix() + "rust_storage_cost_shape_writer \"" +
      rust_dir + "\"";
  if (std::system(cmd.c_str()) != 0) {
    Fail("failed to run rust storage cost-shape writer");
  }

  grovedb::RocksDbWrapper rust_db;
  grovedb::RocksDbWrapper cpp_db;
  std::string error;
  if (!rust_db.Open(rust_dir, &error)) {
    Fail("open rust db failed: " + error);
  }
  if (!cpp_db.Open(cpp_dir, &error)) {
    Fail("open cpp db failed: " + error);
  }

  const uint64_t overlay_seek = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','o','v','e','r','l','a','y','_','s','e','e','k'});
  const uint64_t overlay_loaded = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','o','v','e','r','l','a','y','_','l','o','a','d','e','d'});
  const uint64_t overlay_added = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','o','v','e','r','l','a','y','_','a','d','d','e','d'});
  const uint64_t overlay_replaced = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','o','v','e','r','l','a','y','_','r','e','p','l','a','c','e','d'});
  const uint64_t overlay_removed = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','o','v','e','r','l','a','y','_','r','e','m','o','v','e','d'});
  const uint64_t overlay_hash = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','o','v','e','r','l','a','y','_','h','a','s','h'});

  const uint64_t mixed_seek = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','m','i','x','e','d','_','s','e','e','k'});
  const uint64_t mixed_loaded = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','m','i','x','e','d','_','l','o','a','d','e','d'});
  const uint64_t mixed_added = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','m','i','x','e','d','_','a','d','d','e','d'});
  const uint64_t mixed_replaced = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','m','i','x','e','d','_','r','e','p','l','a','c','e','d'});
  const uint64_t mixed_removed = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','m','i','x','e','d','_','r','e','m','o','v','e','d'});
  const uint64_t mixed_hash = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','m','i','x','e','d','_','h','a','s','h'});
  const uint64_t replace_seek = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','r','e','p','l','a','c','e','_','s','e','e','k'});
  const uint64_t replace_loaded = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','r','e','p','l','a','c','e','_','l','o','a','d','e','d'});
  const uint64_t replace_added = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','r','e','p','l','a','c','e','_','a','d','d','e','d'});
  const uint64_t replace_replaced = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','r','e','p','l','a','c','e','_','r','e','p','l','a','c','e','d'});
  const uint64_t replace_removed = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','r','e','p','l','a','c','e','_','r','e','m','o','v','e','d'});
  const uint64_t replace_hash = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','r','e','p','l','a','c','e','_','h','a','s','h'});
  const uint64_t delete_seek = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','d','e','l','e','t','e','_','s','e','e','k'});
  const uint64_t delete_loaded = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','d','e','l','e','t','e','_','l','o','a','d','e','d'});
  const uint64_t delete_added = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','d','e','l','e','t','e','_','a','d','d','e','d'});
  const uint64_t delete_replaced = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','d','e','l','e','t','e','_','r','e','p','l','a','c','e','d'});
  const uint64_t delete_removed = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','d','e','l','e','t','e','_','r','e','m','o','v','e','d'});
  const uint64_t delete_hash = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','d','e','l','e','t','e','_','h','a','s','h'});
  const uint64_t delete_missing_seek = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','d','e','l','e','t','e','_','m','i','s','s','i','n','g','_','s','e','e','k'});
  const uint64_t delete_missing_loaded = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','d','e','l','e','t','e','_','m','i','s','s','i','n','g','_','l','o','a','d','e','d'});
  const uint64_t delete_missing_added = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','d','e','l','e','t','e','_','m','i','s','s','i','n','g','_','a','d','d','e','d'});
  const uint64_t delete_missing_replaced = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','d','e','l','e','t','e','_','m','i','s','s','i','n','g','_','r','e','p','l','a','c','e','d'});
  const uint64_t delete_missing_removed = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','d','e','l','e','t','e','_','m','i','s','s','i','n','g','_','r','e','m','o','v','e','d'});
  const uint64_t delete_missing_hash = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','d','e','l','e','t','e','_','m','i','s','s','i','n','g','_','h','a','s','h'});
  const uint64_t deep_put_seek = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','d','e','e','p','_','p','u','t','_','s','e','e','k'});
  const uint64_t deep_put_loaded = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','d','e','e','p','_','p','u','t','_','l','o','a','d','e','d'});
  const uint64_t deep_put_added = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','d','e','e','p','_','p','u','t','_','a','d','d','e','d'});
  const uint64_t deep_put_replaced = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','d','e','e','p','_','p','u','t','_','r','e','p','l','a','c','e','d'});
  const uint64_t deep_put_removed = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','d','e','e','p','_','p','u','t','_','r','e','m','o','v','e','d'});
  const uint64_t deep_put_hash = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','d','e','e','p','_','p','u','t','_','h','a','s','h'});
  const uint64_t deep_delete_seek = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','d','e','e','p','_','d','e','l','e','t','e','_','s','e','e','k'});
  const uint64_t deep_delete_loaded = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','d','e','e','p','_','d','e','l','e','t','e','_','l','o','a','d','e','d'});
  const uint64_t deep_delete_added = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','d','e','e','p','_','d','e','l','e','t','e','_','a','d','d','e','d'});
  const uint64_t deep_delete_replaced = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','d','e','e','p','_','d','e','l','e','t','e','_','r','e','p','l','a','c','e','d'});
  const uint64_t deep_delete_removed = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','d','e','e','p','_','d','e','l','e','t','e','_','r','e','m','o','v','e','d'});
  const uint64_t deep_delete_hash = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','d','e','e','p','_','d','e','l','e','t','e','_','h','a','s','h'});
  const uint64_t deep_replace_seek = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','d','e','e','p','_','r','e','p','l','a','c','e','_','s','e','e','k'});
  const uint64_t deep_replace_loaded = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','d','e','e','p','_','r','e','p','l','a','c','e','_','l','o','a','d','e','d'});
  const uint64_t deep_replace_added = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','d','e','e','p','_','r','e','p','l','a','c','e','_','a','d','d','e','d'});
  const uint64_t deep_replace_replaced = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','d','e','e','p','_','r','e','p','l','a','c','e','_','r','e','p','l','a','c','e','d'});
  const uint64_t deep_replace_removed = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','d','e','e','p','_','r','e','p','l','a','c','e','_','r','e','m','o','v','e','d'});
  const uint64_t deep_replace_hash = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','d','e','e','p','_','r','e','p','l','a','c','e','_','h','a','s','h'});
  const uint64_t deep_delete_missing_seek = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','d','e','e','p','_','d','e','l','e','t','e','_','m','i','s','s','i','n','g','_','s','e','e','k'});
  const uint64_t deep_delete_missing_loaded = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','d','e','e','p','_','d','e','l','e','t','e','_','m','i','s','s','i','n','g','_','l','o','a','d','e','d'});
  const uint64_t deep_delete_missing_added = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','d','e','e','p','_','d','e','l','e','t','e','_','m','i','s','s','i','n','g','_','a','d','d','e','d'});
  const uint64_t deep_delete_missing_replaced = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','d','e','e','p','_','d','e','l','e','t','e','_','m','i','s','s','i','n','g','_','r','e','p','l','a','c','e','d'});
  const uint64_t deep_delete_missing_removed = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','d','e','e','p','_','d','e','l','e','t','e','_','m','i','s','s','i','n','g','_','r','e','m','o','v','e','d'});
  const uint64_t deep_delete_missing_hash = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','d','e','e','p','_','d','e','l','e','t','e','_','m','i','s','s','i','n','g','_','h','a','s','h'});
  const uint64_t deep_mixed_seek = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','d','e','e','p','_','m','i','x','e','d','_','s','e','e','k'});
  const uint64_t deep_mixed_loaded = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','d','e','e','p','_','m','i','x','e','d','_','l','o','a','d','e','d'});
  const uint64_t deep_mixed_added = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','d','e','e','p','_','m','i','x','e','d','_','a','d','d','e','d'});
  const uint64_t deep_mixed_replaced = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','d','e','e','p','_','m','i','x','e','d','_','r','e','p','l','a','c','e','d'});
  const uint64_t deep_mixed_removed = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','d','e','e','p','_','m','i','x','e','d','_','r','e','m','o','v','e','d'});
  const uint64_t deep_mixed_hash = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','d','e','e','p','_','m','i','x','e','d','_','h','a','s','h'});
  const uint64_t cf_replace_seek = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','c','f','_','r','e','p','l','a','c','e','_','s','e','e','k'});
  const uint64_t cf_replace_loaded = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','c','f','_','r','e','p','l','a','c','e','_','l','o','a','d','e','d'});
  const uint64_t cf_replace_added = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','c','f','_','r','e','p','l','a','c','e','_','a','d','d','e','d'});
  const uint64_t cf_replace_replaced = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','c','f','_','r','e','p','l','a','c','e','_','r','e','p','l','a','c','e','d'});
  const uint64_t cf_replace_removed = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','c','f','_','r','e','p','l','a','c','e','_','r','e','m','o','v','e','d'});
  const uint64_t cf_replace_hash = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','c','f','_','r','e','p','l','a','c','e','_','h','a','s','h'});
  const uint64_t cf_delete_seek = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','c','f','_','d','e','l','e','t','e','_','s','e','e','k'});
  const uint64_t cf_delete_loaded = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','c','f','_','d','e','l','e','t','e','_','l','o','a','d','e','d'});
  const uint64_t cf_delete_added = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','c','f','_','d','e','l','e','t','e','_','a','d','d','e','d'});
  const uint64_t cf_delete_replaced = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','c','f','_','d','e','l','e','t','e','_','r','e','p','l','a','c','e','d'});
  const uint64_t cf_delete_removed = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','c','f','_','d','e','l','e','t','e','_','r','e','m','o','v','e','d'});
  const uint64_t cf_delete_hash = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','c','f','_','d','e','l','e','t','e','_','h','a','s','h'});
  const uint64_t cf_missing_delete_seek = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','c','f','_','m','i','s','s','i','n','g','_','d','e','l','e','t','e','_','s','e','e','k'});
  const uint64_t cf_missing_delete_loaded = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','c','f','_','m','i','s','s','i','n','g','_','d','e','l','e','t','e','_','l','o','a','d','e','d'});
  const uint64_t cf_missing_delete_added = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','c','f','_','m','i','s','s','i','n','g','_','d','e','l','e','t','e','_','a','d','d','e','d'});
  const uint64_t cf_missing_delete_replaced = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','c','f','_','m','i','s','s','i','n','g','_','d','e','l','e','t','e','_','r','e','p','l','a','c','e','d'});
  const uint64_t cf_missing_delete_removed = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','c','f','_','m','i','s','s','i','n','g','_','d','e','l','e','t','e','_','r','e','m','o','v','e','d'});
  const uint64_t cf_missing_delete_hash = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','c','f','_','m','i','s','s','i','n','g','_','d','e','l','e','t','e','_','h','a','s','h'});
  const uint64_t cf_mixed_seek = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','c','f','_','m','i','x','e','d','_','s','e','e','k'});
  const uint64_t cf_mixed_loaded = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','c','f','_','m','i','x','e','d','_','l','o','a','d','e','d'});
  const uint64_t cf_mixed_added = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','c','f','_','m','i','x','e','d','_','a','d','d','e','d'});
  const uint64_t cf_mixed_replaced = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','c','f','_','m','i','x','e','d','_','r','e','p','l','a','c','e','d'});
  const uint64_t cf_mixed_removed = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','c','f','_','m','i','x','e','d','_','r','e','m','o','v','e','d'});
  const uint64_t cf_mixed_hash = ReadMarker(&rust_db, {'m','_','c','o','s','t','_','c','f','_','m','i','x','e','d','_','h','a','s','h'});

  {
    grovedb::RocksDbWrapper::WriteBatch batch;
    batch.Put(grovedb::ColumnFamilyKind::kDefault, {{'r','o','o','t'}}, {'o','3'}, {'o','v','1'});
    batch.Put(grovedb::ColumnFamilyKind::kDefault, {{'r','o','o','t'}}, {'o','3'}, {'o','v','2'});
    batch.Delete(grovedb::ColumnFamilyKind::kDefault, {{'r','o','o','t'}}, {'o','3'});
    grovedb::BatchCost cost{};
    if (!cpp_db.CommitBatchWithCost(batch, &cost, &error)) {
      Fail("cpp overlay batch cost failed: " + error);
    }
    AssertCost(cost,
               overlay_seek,
               overlay_loaded,
               overlay_added,
               overlay_replaced,
               overlay_removed,
               overlay_hash);
  }

  if (!cpp_db.Put(grovedb::ColumnFamilyKind::kAux, {{'r', 'o', 'o', 't'}}, {'c', '1'},
                  {'o', 'l', 'd', 'v'}, &error)) {
    Fail("cpp cf replace seed failed: " + error);
  }
  {
    grovedb::RocksDbWrapper::WriteBatch batch;
    batch.Put(grovedb::ColumnFamilyKind::kAux, {{'r','o','o','t'}}, {'c','1'},
              {'n','e','w','v'});
    grovedb::BatchCost cost{};
    if (!cpp_db.CommitBatchWithCost(batch, &cost, &error)) {
      Fail("cpp cf replace batch cost failed: " + error);
    }
    AssertCost(cost, cf_replace_seek, cf_replace_loaded, cf_replace_added, cf_replace_replaced,
               cf_replace_removed, cf_replace_hash);
  }
  if (!cpp_db.Put(grovedb::ColumnFamilyKind::kAux, {{'r', 'o', 'o', 't'}}, {'c', '2'},
                  {'o', 'l', 'd', 'v'}, &error)) {
    Fail("cpp tx cf replace seed failed: " + error);
  }
  {
    grovedb::RocksDbWrapper::WriteBatch batch;
    batch.Put(grovedb::ColumnFamilyKind::kAux, {{'r','o','o','t'}}, {'c','2'},
              {'n','e','w','v'});
    grovedb::RocksDbWrapper::Transaction tx;
    if (!cpp_db.BeginTransaction(&tx, &error)) {
      Fail("cpp begin tx cf replace failed: " + error);
    }
    grovedb::BatchCost cost{};
    if (!cpp_db.CommitBatchWithCost(batch, &tx, &cost, &error)) {
      Fail("cpp tx cf replace batch cost failed: " + error);
    }
    AssertCost(cost, cf_replace_seek, cf_replace_loaded, cf_replace_added, cf_replace_replaced,
               cf_replace_removed, cf_replace_hash);
    if (!tx.Rollback(&error)) {
      Fail("cpp tx cf replace rollback failed: " + error);
    }
  }

  if (!cpp_db.Put(grovedb::ColumnFamilyKind::kMeta, {{'r', 'o', 'o', 't'}}, {'c', '1'},
                  {'o', 'l', 'd', 'v'}, &error)) {
    Fail("cpp cf delete seed failed: " + error);
  }
  {
    grovedb::RocksDbWrapper::WriteBatch batch;
    batch.Delete(grovedb::ColumnFamilyKind::kMeta, {{'r','o','o','t'}}, {'c','1'});
    grovedb::BatchCost cost{};
    if (!cpp_db.CommitBatchWithCost(batch, &cost, &error)) {
      Fail("cpp cf delete batch cost failed: " + error);
    }
    AssertCost(cost, cf_delete_seek, cf_delete_loaded, cf_delete_added, cf_delete_replaced,
               cf_delete_removed, cf_delete_hash);
  }
  if (!cpp_db.Put(grovedb::ColumnFamilyKind::kMeta, {{'r', 'o', 'o', 't'}}, {'c', '2'},
                  {'o', 'l', 'd', 'v'}, &error)) {
    Fail("cpp tx cf delete seed failed: " + error);
  }
  {
    grovedb::RocksDbWrapper::WriteBatch batch;
    batch.Delete(grovedb::ColumnFamilyKind::kMeta, {{'r','o','o','t'}}, {'c','2'});
    grovedb::RocksDbWrapper::Transaction tx;
    if (!cpp_db.BeginTransaction(&tx, &error)) {
      Fail("cpp begin tx cf delete failed: " + error);
    }
    grovedb::BatchCost cost{};
    if (!cpp_db.CommitBatchWithCost(batch, &tx, &cost, &error)) {
      Fail("cpp tx cf delete batch cost failed: " + error);
    }
    AssertCost(cost, cf_delete_seek, cf_delete_loaded, cf_delete_added, cf_delete_replaced,
               cf_delete_removed, cf_delete_hash);
    if (!tx.Rollback(&error)) {
      Fail("cpp tx cf delete rollback failed: " + error);
    }
  }

  {
    grovedb::RocksDbWrapper::WriteBatch batch;
    batch.Delete(grovedb::ColumnFamilyKind::kRoots, {{'r','o','o','t'}}, {'c','1'});
    grovedb::BatchCost cost{};
    if (!cpp_db.CommitBatchWithCost(batch, &cost, &error)) {
      Fail("cpp cf missing-delete batch cost failed: " + error);
    }
    AssertCost(cost, cf_missing_delete_seek, cf_missing_delete_loaded, cf_missing_delete_added,
               cf_missing_delete_replaced, cf_missing_delete_removed, cf_missing_delete_hash);
  }
  {
    grovedb::RocksDbWrapper::WriteBatch batch;
    batch.Delete(grovedb::ColumnFamilyKind::kRoots, {{'r','o','o','t'}}, {'c','2'});
    grovedb::RocksDbWrapper::Transaction tx;
    if (!cpp_db.BeginTransaction(&tx, &error)) {
      Fail("cpp begin tx cf missing-delete failed: " + error);
    }
    grovedb::BatchCost cost{};
    if (!cpp_db.CommitBatchWithCost(batch, &tx, &cost, &error)) {
      Fail("cpp tx cf missing-delete batch cost failed: " + error);
    }
    AssertCost(cost, cf_missing_delete_seek, cf_missing_delete_loaded, cf_missing_delete_added,
               cf_missing_delete_replaced, cf_missing_delete_removed, cf_missing_delete_hash);
    if (!tx.Rollback(&error)) {
      Fail("cpp tx cf missing-delete rollback failed: " + error);
    }
  }

  {
    grovedb::RocksDbWrapper::WriteBatch batch;
    batch.Put(grovedb::ColumnFamilyKind::kDefault, {{'r','o','o','t'}}, {'c','3'}, {'v','a'});
    batch.Put(grovedb::ColumnFamilyKind::kAux, {{'r','o','o','t'}}, {'c','3'}, {'v','b'});
    batch.Put(grovedb::ColumnFamilyKind::kMeta, {{'r','o','o','t'}}, {'c','3'}, {'v','c'});
    batch.Delete(grovedb::ColumnFamilyKind::kRoots, {{'r','o','o','t'}}, {'c','3'});
    grovedb::BatchCost cost{};
    if (!cpp_db.CommitBatchWithCost(batch, &cost, &error)) {
      Fail("cpp mixed-cf batch cost failed: " + error);
    }
    AssertCost(cost, cf_mixed_seek, cf_mixed_loaded, cf_mixed_added, cf_mixed_replaced,
               cf_mixed_removed, cf_mixed_hash);
  }
  {
    grovedb::RocksDbWrapper::WriteBatch batch;
    batch.Put(grovedb::ColumnFamilyKind::kDefault, {{'r','o','o','t'}}, {'c','4'}, {'v','a'});
    batch.Put(grovedb::ColumnFamilyKind::kAux, {{'r','o','o','t'}}, {'c','4'}, {'v','b'});
    batch.Put(grovedb::ColumnFamilyKind::kMeta, {{'r','o','o','t'}}, {'c','4'}, {'v','c'});
    batch.Delete(grovedb::ColumnFamilyKind::kRoots, {{'r','o','o','t'}}, {'c','4'});
    grovedb::RocksDbWrapper::Transaction tx;
    if (!cpp_db.BeginTransaction(&tx, &error)) {
      Fail("cpp begin tx mixed-cf failed: " + error);
    }
    grovedb::BatchCost cost{};
    if (!cpp_db.CommitBatchWithCost(batch, &tx, &cost, &error)) {
      Fail("cpp tx mixed-cf batch cost failed: " + error);
    }
    AssertCost(cost, cf_mixed_seek, cf_mixed_loaded, cf_mixed_added, cf_mixed_replaced,
               cf_mixed_removed, cf_mixed_hash);
    if (!tx.Rollback(&error)) {
      Fail("cpp tx mixed-cf rollback failed: " + error);
    }
  }

  {
    grovedb::RocksDbWrapper::WriteBatch batch;
    batch.Put(grovedb::ColumnFamilyKind::kDefault, {{'r','o','o','t'}}, {'o','1'}, {'o','v','1'});
    batch.Put(grovedb::ColumnFamilyKind::kDefault, {{'r','o','o','t'}}, {'o','1'}, {'o','v','2'});
    batch.Delete(grovedb::ColumnFamilyKind::kDefault, {{'r','o','o','t'}}, {'o','1'});
    grovedb::RocksDbWrapper::Transaction tx;
    if (!cpp_db.BeginTransaction(&tx, &error)) {
      Fail("cpp begin tx overlay failed: " + error);
    }
    grovedb::BatchCost cost{};
    if (!cpp_db.CommitBatchWithCost(batch, &tx, &cost, &error)) {
      Fail("cpp tx overlay batch cost failed: " + error);
    }
    AssertCost(cost,
               overlay_seek,
               overlay_loaded,
               overlay_added,
               overlay_replaced,
               overlay_removed,
               overlay_hash);
    if (!tx.Rollback(&error)) {
      Fail("cpp tx overlay rollback failed: " + error);
    }
  }

  {
    grovedb::RocksDbWrapper::WriteBatch batch;
    batch.Put(grovedb::ColumnFamilyKind::kDefault, {{'r','o','o','t'}}, {'n','1'}, {'v','a'});
    batch.Put(grovedb::ColumnFamilyKind::kDefault, {{'r','o','o','t'}}, {'n','1'}, {'v','b'});
    batch.Put(grovedb::ColumnFamilyKind::kDefault, {{'r','o','o','t'}}, {'n','2'}, {'v','c'});
    batch.Delete(grovedb::ColumnFamilyKind::kDefault, {{'r','o','o','t'}}, {'n','1'});
    batch.Put(grovedb::ColumnFamilyKind::kDefault, {{'r','o','o','t'}}, {'n','2'}, {'v','d'});
    grovedb::BatchCost cost{};
    if (!cpp_db.CommitBatchWithCost(batch, &cost, &error)) {
      Fail("cpp mixed batch cost failed: " + error);
    }
    AssertCost(cost, mixed_seek, mixed_loaded, mixed_added, mixed_replaced, mixed_removed, mixed_hash);
  }

  {
    grovedb::RocksDbWrapper::WriteBatch batch;
    batch.Put(grovedb::ColumnFamilyKind::kDefault, {{'r','o','o','t'}}, {'m','1'}, {'v','a'});
    batch.Put(grovedb::ColumnFamilyKind::kDefault, {{'r','o','o','t'}}, {'m','1'}, {'v','b'});
    batch.Put(grovedb::ColumnFamilyKind::kDefault, {{'r','o','o','t'}}, {'m','2'}, {'v','c'});
    batch.Delete(grovedb::ColumnFamilyKind::kDefault, {{'r','o','o','t'}}, {'m','1'});
    batch.Put(grovedb::ColumnFamilyKind::kDefault, {{'r','o','o','t'}}, {'m','2'}, {'v','d'});
    grovedb::RocksDbWrapper::Transaction tx;
    if (!cpp_db.BeginTransaction(&tx, &error)) {
      Fail("cpp begin tx mixed failed: " + error);
    }
    grovedb::BatchCost cost{};
    if (!cpp_db.CommitBatchWithCost(batch, &tx, &cost, &error)) {
      Fail("cpp tx mixed batch cost failed: " + error);
    }
    AssertCost(cost, mixed_seek, mixed_loaded, mixed_added, mixed_replaced, mixed_removed, mixed_hash);
    if (!tx.Rollback(&error)) {
      Fail("cpp tx mixed rollback failed: " + error);
    }
  }

  if (!cpp_db.Put(grovedb::ColumnFamilyKind::kDefault, {{'r', 'o', 'o', 't'}}, {'r', '1'},
                  {'o', 'l', 'd', 'v'}, &error)) {
    Fail("cpp replace seed failed: " + error);
  }

  {
    grovedb::RocksDbWrapper::WriteBatch batch;
    batch.Put(grovedb::ColumnFamilyKind::kDefault, {{'r','o','o','t'}}, {'r','1'},
              {'n','e','w','v'});
    grovedb::BatchCost cost{};
    if (!cpp_db.CommitBatchWithCost(batch, &cost, &error)) {
      Fail("cpp replace batch cost failed: " + error);
    }
    AssertCost(cost, replace_seek, replace_loaded, replace_added, replace_replaced, replace_removed,
               replace_hash);
  }

  if (!cpp_db.Put(grovedb::ColumnFamilyKind::kDefault, {{'r', 'o', 'o', 't'}}, {'r', '2'},
                  {'o', 'l', 'd', 'v'}, &error)) {
    Fail("cpp tx replace seed failed: " + error);
  }

  {
    grovedb::RocksDbWrapper::WriteBatch batch;
    batch.Put(grovedb::ColumnFamilyKind::kDefault, {{'r','o','o','t'}}, {'r','2'},
              {'n','e','w','v'});
    grovedb::RocksDbWrapper::Transaction tx;
    if (!cpp_db.BeginTransaction(&tx, &error)) {
      Fail("cpp begin tx replace failed: " + error);
    }
    grovedb::BatchCost cost{};
    if (!cpp_db.CommitBatchWithCost(batch, &tx, &cost, &error)) {
      Fail("cpp tx replace batch cost failed: " + error);
    }
    AssertCost(cost, replace_seek, replace_loaded, replace_added, replace_replaced, replace_removed,
               replace_hash);
    if (!tx.Rollback(&error)) {
      Fail("cpp tx replace rollback failed: " + error);
    }
  }

  if (!cpp_db.Put(grovedb::ColumnFamilyKind::kDefault, {{'r', 'o', 'o', 't'}}, {'d', '1'},
                  {'o', 'l', 'd', 'v'}, &error)) {
    Fail("cpp delete seed failed: " + error);
  }

  {
    grovedb::RocksDbWrapper::WriteBatch batch;
    batch.Delete(grovedb::ColumnFamilyKind::kDefault, {{'r','o','o','t'}}, {'d','1'});
    grovedb::BatchCost cost{};
    if (!cpp_db.CommitBatchWithCost(batch, &cost, &error)) {
      Fail("cpp delete batch cost failed: " + error);
    }
    AssertCost(cost, delete_seek, delete_loaded, delete_added, delete_replaced, delete_removed,
               delete_hash);
  }

  if (!cpp_db.Put(grovedb::ColumnFamilyKind::kDefault, {{'r', 'o', 'o', 't'}}, {'d', '2'},
                  {'o', 'l', 'd', 'v'}, &error)) {
    Fail("cpp tx delete seed failed: " + error);
  }

  {
    grovedb::RocksDbWrapper::WriteBatch batch;
    batch.Delete(grovedb::ColumnFamilyKind::kDefault, {{'r','o','o','t'}}, {'d','2'});
    grovedb::RocksDbWrapper::Transaction tx;
    if (!cpp_db.BeginTransaction(&tx, &error)) {
      Fail("cpp begin tx delete failed: " + error);
    }
    grovedb::BatchCost cost{};
    if (!cpp_db.CommitBatchWithCost(batch, &tx, &cost, &error)) {
      Fail("cpp tx delete batch cost failed: " + error);
    }
    AssertCost(cost, delete_seek, delete_loaded, delete_added, delete_replaced, delete_removed,
               delete_hash);
    if (!tx.Rollback(&error)) {
      Fail("cpp tx delete rollback failed: " + error);
    }
  }

  {
    grovedb::RocksDbWrapper::WriteBatch batch;
    batch.Delete(grovedb::ColumnFamilyKind::kDefault, {{'r','o','o','t'}}, {'x','1'});
    grovedb::BatchCost cost{};
    if (!cpp_db.CommitBatchWithCost(batch, &cost, &error)) {
      Fail("cpp missing-delete batch cost failed: " + error);
    }
    AssertCost(cost,
               delete_missing_seek,
               delete_missing_loaded,
               delete_missing_added,
               delete_missing_replaced,
               delete_missing_removed,
               delete_missing_hash);
  }

  {
    grovedb::RocksDbWrapper::WriteBatch batch;
    batch.Delete(grovedb::ColumnFamilyKind::kDefault, {{'r','o','o','t'}}, {'x','2'});
    grovedb::RocksDbWrapper::Transaction tx;
    if (!cpp_db.BeginTransaction(&tx, &error)) {
      Fail("cpp begin tx missing-delete failed: " + error);
    }
    grovedb::BatchCost cost{};
    if (!cpp_db.CommitBatchWithCost(batch, &tx, &cost, &error)) {
      Fail("cpp tx missing-delete batch cost failed: " + error);
    }
    AssertCost(cost,
               delete_missing_seek,
               delete_missing_loaded,
               delete_missing_added,
               delete_missing_replaced,
               delete_missing_removed,
               delete_missing_hash);
    if (!tx.Rollback(&error)) {
      Fail("cpp tx missing-delete rollback failed: " + error);
    }
  }

  {
    grovedb::RocksDbWrapper::WriteBatch batch;
    batch.Put(grovedb::ColumnFamilyKind::kDefault, {{'r','o','o','t'}, {'c','h','i','l','d'}},
              {'p','1'}, {'p','v'});
    grovedb::BatchCost cost{};
    if (!cpp_db.CommitBatchWithCost(batch, &cost, &error)) {
      Fail("cpp deep-path put batch cost failed: " + error);
    }
    AssertCost(cost,
               deep_put_seek,
               deep_put_loaded,
               deep_put_added,
               deep_put_replaced,
               deep_put_removed,
               deep_put_hash);
  }

  {
    grovedb::RocksDbWrapper::WriteBatch batch;
    batch.Put(grovedb::ColumnFamilyKind::kDefault, {{'r','o','o','t'}, {'c','h','i','l','d'}},
              {'p','2'}, {'p','v'});
    grovedb::RocksDbWrapper::Transaction tx;
    if (!cpp_db.BeginTransaction(&tx, &error)) {
      Fail("cpp begin tx deep-path put failed: " + error);
    }
    grovedb::BatchCost cost{};
    if (!cpp_db.CommitBatchWithCost(batch, &tx, &cost, &error)) {
      Fail("cpp tx deep-path put batch cost failed: " + error);
    }
    AssertCost(cost,
               deep_put_seek,
               deep_put_loaded,
               deep_put_added,
               deep_put_replaced,
               deep_put_removed,
               deep_put_hash);
    if (!tx.Rollback(&error)) {
      Fail("cpp tx deep-path put rollback failed: " + error);
    }
  }

  if (!cpp_db.Put(grovedb::ColumnFamilyKind::kDefault,
                  {{'r', 'o', 'o', 't'}, {'c', 'h', 'i', 'l', 'd'}},
                  {'q', '1'},
                  {'o', 'l', 'd', 'v'},
                  &error)) {
    Fail("cpp deep-path delete seed failed: " + error);
  }

  {
    grovedb::RocksDbWrapper::WriteBatch batch;
    batch.Delete(grovedb::ColumnFamilyKind::kDefault,
                 {{'r','o','o','t'}, {'c','h','i','l','d'}},
                 {'q','1'});
    grovedb::BatchCost cost{};
    if (!cpp_db.CommitBatchWithCost(batch, &cost, &error)) {
      Fail("cpp deep-path delete batch cost failed: " + error);
    }
    AssertCost(cost,
               deep_delete_seek,
               deep_delete_loaded,
               deep_delete_added,
               deep_delete_replaced,
               deep_delete_removed,
               deep_delete_hash);
  }

  if (!cpp_db.Put(grovedb::ColumnFamilyKind::kDefault,
                  {{'r', 'o', 'o', 't'}, {'c', 'h', 'i', 'l', 'd'}},
                  {'q', '2'},
                  {'o', 'l', 'd', 'v'},
                  &error)) {
    Fail("cpp tx deep-path delete seed failed: " + error);
  }

  {
    grovedb::RocksDbWrapper::WriteBatch batch;
    batch.Delete(grovedb::ColumnFamilyKind::kDefault,
                 {{'r','o','o','t'}, {'c','h','i','l','d'}},
                 {'q','2'});
    grovedb::RocksDbWrapper::Transaction tx;
    if (!cpp_db.BeginTransaction(&tx, &error)) {
      Fail("cpp begin tx deep-path delete failed: " + error);
    }
    grovedb::BatchCost cost{};
    if (!cpp_db.CommitBatchWithCost(batch, &tx, &cost, &error)) {
      Fail("cpp tx deep-path delete batch cost failed: " + error);
    }
    AssertCost(cost,
               deep_delete_seek,
               deep_delete_loaded,
               deep_delete_added,
               deep_delete_replaced,
               deep_delete_removed,
               deep_delete_hash);
    if (!tx.Rollback(&error)) {
      Fail("cpp tx deep-path delete rollback failed: " + error);
    }
  }

  if (!cpp_db.Put(grovedb::ColumnFamilyKind::kDefault,
                  {{'r', 'o', 'o', 't'}, {'c', 'h', 'i', 'l', 'd'}},
                  {'w', '1'},
                  {'o', 'l', 'd', 'v'},
                  &error)) {
    Fail("cpp deep-path replace seed failed: " + error);
  }

  {
    grovedb::RocksDbWrapper::WriteBatch batch;
    batch.Put(grovedb::ColumnFamilyKind::kDefault,
              {{'r','o','o','t'}, {'c','h','i','l','d'}},
              {'w','1'},
              {'n','e','w','v'});
    grovedb::BatchCost cost{};
    if (!cpp_db.CommitBatchWithCost(batch, &cost, &error)) {
      Fail("cpp deep-path replace batch cost failed: " + error);
    }
    AssertCost(cost,
               deep_replace_seek,
               deep_replace_loaded,
               deep_replace_added,
               deep_replace_replaced,
               deep_replace_removed,
               deep_replace_hash);
  }

  if (!cpp_db.Put(grovedb::ColumnFamilyKind::kDefault,
                  {{'r', 'o', 'o', 't'}, {'c', 'h', 'i', 'l', 'd'}},
                  {'w', '2'},
                  {'o', 'l', 'd', 'v'},
                  &error)) {
    Fail("cpp tx deep-path replace seed failed: " + error);
  }

  {
    grovedb::RocksDbWrapper::WriteBatch batch;
    batch.Put(grovedb::ColumnFamilyKind::kDefault,
              {{'r','o','o','t'}, {'c','h','i','l','d'}},
              {'w','2'},
              {'n','e','w','v'});
    grovedb::RocksDbWrapper::Transaction tx;
    if (!cpp_db.BeginTransaction(&tx, &error)) {
      Fail("cpp begin tx deep-path replace failed: " + error);
    }
    grovedb::BatchCost cost{};
    if (!cpp_db.CommitBatchWithCost(batch, &tx, &cost, &error)) {
      Fail("cpp tx deep-path replace batch cost failed: " + error);
    }
    AssertCost(cost,
               deep_replace_seek,
               deep_replace_loaded,
               deep_replace_added,
               deep_replace_replaced,
               deep_replace_removed,
               deep_replace_hash);
    if (!tx.Rollback(&error)) {
      Fail("cpp tx deep-path replace rollback failed: " + error);
    }
  }

  {
    grovedb::RocksDbWrapper::WriteBatch batch;
    batch.Delete(grovedb::ColumnFamilyKind::kDefault,
                 {{'r','o','o','t'}, {'c','h','i','l','d'}},
                 {'z','1'});
    grovedb::BatchCost cost{};
    if (!cpp_db.CommitBatchWithCost(batch, &cost, &error)) {
      Fail("cpp deep-path missing-delete batch cost failed: " + error);
    }
    AssertCost(cost,
               deep_delete_missing_seek,
               deep_delete_missing_loaded,
               deep_delete_missing_added,
               deep_delete_missing_replaced,
               deep_delete_missing_removed,
               deep_delete_missing_hash);
  }

  {
    grovedb::RocksDbWrapper::WriteBatch batch;
    batch.Delete(grovedb::ColumnFamilyKind::kDefault,
                 {{'r','o','o','t'}, {'c','h','i','l','d'}},
                 {'z','2'});
    grovedb::RocksDbWrapper::Transaction tx;
    if (!cpp_db.BeginTransaction(&tx, &error)) {
      Fail("cpp begin tx deep-path missing-delete failed: " + error);
    }
    grovedb::BatchCost cost{};
    if (!cpp_db.CommitBatchWithCost(batch, &tx, &cost, &error)) {
      Fail("cpp tx deep-path missing-delete batch cost failed: " + error);
    }
    AssertCost(cost,
               deep_delete_missing_seek,
               deep_delete_missing_loaded,
               deep_delete_missing_added,
               deep_delete_missing_replaced,
               deep_delete_missing_removed,
               deep_delete_missing_hash);
    if (!tx.Rollback(&error)) {
      Fail("cpp tx deep-path missing-delete rollback failed: " + error);
    }
  }

  {
    grovedb::RocksDbWrapper::WriteBatch batch;
    batch.Put(grovedb::ColumnFamilyKind::kDefault,
              {{'r','o','o','t'}, {'g','r','a','n','d'}},
              {'u','3'},
              {'v','a'});
    batch.Put(grovedb::ColumnFamilyKind::kDefault,
              {{'r','o','o','t'}, {'g','r','a','n','d'}},
              {'u','3'},
              {'v','b'});
    batch.Put(grovedb::ColumnFamilyKind::kDefault,
              {{'r','o','o','t'}, {'g','r','a','n','d'}},
              {'u','4'},
              {'v','c'});
    batch.Delete(grovedb::ColumnFamilyKind::kDefault,
                 {{'r','o','o','t'}, {'g','r','a','n','d'}},
                 {'u','3'});
    batch.Put(grovedb::ColumnFamilyKind::kDefault,
              {{'r','o','o','t'}, {'g','r','a','n','d'}},
              {'u','4'},
              {'v','d'});
    grovedb::BatchCost cost{};
    if (!cpp_db.CommitBatchWithCost(batch, &cost, &error)) {
      Fail("cpp deep-path mixed batch cost failed: " + error);
    }
    AssertCost(cost,
               deep_mixed_seek,
               deep_mixed_loaded,
               deep_mixed_added,
               deep_mixed_replaced,
               deep_mixed_removed,
               deep_mixed_hash);
  }

  {
    grovedb::RocksDbWrapper::WriteBatch batch;
    batch.Put(grovedb::ColumnFamilyKind::kDefault,
              {{'r','o','o','t'}, {'g','r','a','n','d'}},
              {'u','1'},
              {'v','a'});
    batch.Put(grovedb::ColumnFamilyKind::kDefault,
              {{'r','o','o','t'}, {'g','r','a','n','d'}},
              {'u','1'},
              {'v','b'});
    batch.Put(grovedb::ColumnFamilyKind::kDefault,
              {{'r','o','o','t'}, {'g','r','a','n','d'}},
              {'u','2'},
              {'v','c'});
    batch.Delete(grovedb::ColumnFamilyKind::kDefault,
                 {{'r','o','o','t'}, {'g','r','a','n','d'}},
                 {'u','1'});
    batch.Put(grovedb::ColumnFamilyKind::kDefault,
              {{'r','o','o','t'}, {'g','r','a','n','d'}},
              {'u','2'},
              {'v','d'});
    grovedb::RocksDbWrapper::Transaction tx;
    if (!cpp_db.BeginTransaction(&tx, &error)) {
      Fail("cpp begin tx deep-path mixed failed: " + error);
    }
    grovedb::BatchCost cost{};
    if (!cpp_db.CommitBatchWithCost(batch, &tx, &cost, &error)) {
      Fail("cpp tx deep-path mixed batch cost failed: " + error);
    }
    AssertCost(cost,
               deep_mixed_seek,
               deep_mixed_loaded,
               deep_mixed_added,
               deep_mixed_replaced,
               deep_mixed_removed,
               deep_mixed_hash);
    if (!tx.Rollback(&error)) {
      Fail("cpp tx deep-path mixed rollback failed: " + error);
    }
  }

  std::filesystem::remove_all(rust_dir);
  std::filesystem::remove_all(cpp_dir);
  std::cout << "COST_CHECK_SUMMARY checked=" << g_cost_check_index << " status=PASS\n";
  return 0;
}
uint64_t g_cost_check_index = 0;
