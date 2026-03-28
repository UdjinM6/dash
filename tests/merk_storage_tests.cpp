#include "merk_storage.h"
#include "test_utils.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

using test_utils::Fail;
using test_utils::MakeTempDir;

namespace {

grovedb::MerkTree::ValueHashFn CustomValueHashFn() {
  return [](const std::vector<uint8_t>& key,
            const std::vector<uint8_t>& value,
            std::vector<uint8_t>* out,
            std::string* error) {
    if (out == nullptr) {
      if (error != nullptr) {
        *error = "custom hash output is null";
      }
      return false;
    }
    out->assign(32, 0);
    for (size_t i = 0; i < key.size(); ++i) {
      (*out)[i % out->size()] ^= static_cast<uint8_t>(key[i] + static_cast<uint8_t>(i));
    }
    for (size_t i = 0; i < value.size(); ++i) {
      (*out)[(i * 7 + 3) % out->size()] ^=
          static_cast<uint8_t>(value[i] ^ static_cast<uint8_t>(31 - (i % 31)));
    }
    (*out)[0] ^= static_cast<uint8_t>(key.size());
    (*out)[31] ^= static_cast<uint8_t>(value.size());
    return true;
  };
}

void PutExportedTree(grovedb::RocksDbWrapper* storage,
                     const std::vector<std::vector<uint8_t>>& path,
                     const std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>>& entries,
                     const std::vector<uint8_t>& root_key,
                     std::string* error) {
  for (const auto& entry : entries) {
    if (!storage->Put(grovedb::ColumnFamilyKind::kDefault, path, entry.first, entry.second, error)) {
      Fail("persist exported entry failed: " + *error);
    }
  }
  if (!storage->Put(grovedb::ColumnFamilyKind::kRoots, path, {'r'}, root_key, error)) {
    Fail("persist exported root key failed: " + *error);
  }
}

void CorruptStoredRootChildKeyToEmpty(grovedb::RocksDbWrapper* storage,
                                      const std::vector<std::vector<uint8_t>>& path,
                                      grovedb::MerkTree* tree,
                                      std::string* error) {
  std::vector<uint8_t> root_key;
  if (tree == nullptr || !tree->RootKey(&root_key) || root_key.empty()) {
    Fail("missing root key for corruption helper");
  }
  std::vector<uint8_t> encoded;
  bool found = false;
  if (!storage->Get(grovedb::ColumnFamilyKind::kDefault, path, root_key, &encoded, &found, error) || !found) {
    Fail("get root encoded node failed: " + *error);
  }
  grovedb::TreeNodeInner inner;
  if (!grovedb::DecodeTreeNodeInner(encoded, &inner, error)) {
    Fail("decode root failed: " + *error);
  }
  if (inner.has_left) {
    inner.left.key.clear();
  } else if (inner.has_right) {
    inner.right.key.clear();
  } else {
    Fail("root should have at least one child");
  }
  std::vector<uint8_t> corrupt;
  if (!grovedb::EncodeTreeNodeInner(inner, &corrupt, error)) {
    Fail("re-encode corrupt root failed: " + *error);
  }
  if (!storage->Put(grovedb::ColumnFamilyKind::kDefault, path, root_key, corrupt, error)) {
    Fail("write corrupt root failed: " + *error);
  }
}

}  // namespace

int main() {
  std::string error;
  auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
  std::string dir = MakeTempDir("merk_storage_test_" + std::to_string(now));

  grovedb::RocksDbWrapper storage;
  if (!storage.Open(dir, &error)) {
    Fail("open rocksdb failed: " + error);
  }

  grovedb::MerkTree tree;
  if (!tree.Insert({'a'}, {'1'}, &error) ||
      !tree.Insert({'b'}, {'2'}, &error) ||
      !tree.Insert({'c'}, {'3'}, &error)) {
    Fail("insert tree failed: " + error);
  }

  if (!grovedb::MerkStorage::SaveTree(&storage, {{'r', 'o', 'o', 't'}}, &tree, &error)) {
    Fail("save tree failed: " + error);
  }

  grovedb::MerkTree loaded;
  if (!grovedb::MerkStorage::LoadTree(&storage, {{'r', 'o', 'o', 't'}}, &loaded, &error)) {
    Fail("load tree failed: " + error);
  }

  std::vector<uint8_t> value;
  if (!loaded.Get({'b'}, &value) || value != std::vector<uint8_t>({'2'})) {
    Fail("loaded tree missing value");
  }
  std::vector<uint8_t> loaded_root_key;
  if (!loaded.RootKey(&loaded_root_key) || loaded_root_key.empty()) {
    Fail("loaded tree missing root key");
  }
  // Rust parity: reopen-by-root-key should target the merk data column family.
  grovedb::MerkTree reopened_by_root;
  if (!reopened_by_root.LoadEncodedTree(&storage, {{'r', 'o', 'o', 't'}}, loaded_root_key, &error)) {
    Fail("LoadEncodedTree default-cf reopen failed: " + error);
  }
  if (!reopened_by_root.Get({'a'}, &value) || value != std::vector<uint8_t>({'1'})) {
    Fail("reopened-by-root tree missing value");
  }

  if (!grovedb::MerkStorage::ClearTree(&storage, {{'r', 'o', 'o', 't'}}, &error)) {
    Fail("clear tree failed: " + error);
  }
  grovedb::MerkTree cleared;
  if (!grovedb::MerkStorage::LoadTree(&storage, {{'r', 'o', 'o', 't'}}, &cleared, &error)) {
    Fail("load cleared tree failed: " + error);
  }
  if (cleared.Get({'a'}, &value)) {
    Fail("cleared tree should be empty");
  }

  // Lazy load should still be able to compute root hash from metadata.
  grovedb::MerkTree hash_tree;
  if (!hash_tree.Insert({'a'}, {'1'}, &error) ||
      !hash_tree.Insert({'b'}, {'2'}, &error) ||
      !hash_tree.Insert({'c'}, {'3'}, &error) ||
      !hash_tree.Insert({'d'}, {'4'}, &error)) {
    Fail("insert hash tree failed: " + error);
  }
  std::vector<uint8_t> expected_hash;
  if (!hash_tree.ComputeRootHash(grovedb::MerkTree::ValueHashFn(), &expected_hash, &error)) {
    Fail("compute root hash failed: " + error);
  }
  if (!grovedb::MerkStorage::SaveTree(&storage, {{'l', 'a', 'z', 'y'}}, &hash_tree, &error)) {
    Fail("save hash tree failed: " + error);
  }
  grovedb::MerkTree lazy_loaded;
  if (!grovedb::MerkStorage::LoadTree(&storage, {{'l', 'a', 'z', 'y'}}, &lazy_loaded, &error)) {
    Fail("load lazy tree failed: " + error);
  }
  std::vector<uint8_t> loaded_hash;
  if (!lazy_loaded.ComputeRootHash(grovedb::MerkTree::ValueHashFn(), &loaded_hash, &error)) {
    Fail("compute lazy root hash failed: " + error);
  }
  if (loaded_hash != expected_hash) {
    Fail("lazy root hash mismatch");
  }

  std::vector<uint8_t> proof_lazy;
  std::vector<uint8_t> root_lazy;
  std::vector<uint8_t> value_lazy;
  if (!lazy_loaded.GenerateProof({'c'},
                                 grovedb::TargetEncoding::kKv,
                                 grovedb::MerkTree::ValueHashFn(),
                                 &proof_lazy,
                                 &root_lazy,
                                 &value_lazy,
                                 &error)) {
    Fail("lazy proof generation failed: " + error);
  }
  std::vector<uint8_t> proof_full;
  std::vector<uint8_t> root_full;
  std::vector<uint8_t> value_full;
  if (!hash_tree.GenerateProof({'c'},
                               grovedb::TargetEncoding::kKv,
                               grovedb::MerkTree::ValueHashFn(),
                               &proof_full,
                               &root_full,
                               &value_full,
                               &error)) {
    Fail("full proof generation failed: " + error);
  }
  if (proof_lazy != proof_full) {
    Fail("lazy proof bytes mismatch");
  }
  if (root_lazy != root_full) {
    Fail("lazy proof root mismatch");
  }
  if (value_lazy != value_full) {
    Fail("lazy proof value mismatch");
  }

  std::vector<uint8_t> range_proof_lazy;
  std::vector<uint8_t> range_root_lazy;
  if (!lazy_loaded.GenerateRangeProof({'b'},
                                      {'d'},
                                      true,
                                      false,
                                      grovedb::MerkTree::ValueHashFn(),
                                      &range_proof_lazy,
                                      &range_root_lazy,
                                      &error)) {
    Fail("lazy range proof generation failed: " + error);
  }
  std::vector<uint8_t> range_proof_full;
  std::vector<uint8_t> range_root_full;
  if (!hash_tree.GenerateRangeProof({'b'},
                                    {'d'},
                                    true,
                                    false,
                                    grovedb::MerkTree::ValueHashFn(),
                                    &range_proof_full,
                                    &range_root_full,
                                    &error)) {
    Fail("full range proof generation failed: " + error);
  }
  if (range_proof_lazy != range_proof_full || range_root_lazy != range_root_full) {
    Fail("lazy range proof mismatch");
  }

  std::vector<uint8_t> absence_proof_lazy;
  std::vector<uint8_t> absence_root_lazy;
  if (!lazy_loaded.GenerateAbsenceProof({'z'},
                                        grovedb::MerkTree::ValueHashFn(),
                                        &absence_proof_lazy,
                                        &absence_root_lazy,
                                        &error)) {
    Fail("lazy absence proof generation failed: " + error);
  }
  std::vector<uint8_t> absence_proof_full;
  std::vector<uint8_t> absence_root_full;
  if (!hash_tree.GenerateAbsenceProof({'z'},
                                      grovedb::MerkTree::ValueHashFn(),
                                      &absence_proof_full,
                                      &absence_root_full,
                                      &error)) {
    Fail("full absence proof generation failed: " + error);
  }
  if (absence_proof_lazy != absence_proof_full || absence_root_lazy != absence_root_full) {
    Fail("lazy absence proof mismatch");
  }

  if (!hash_tree.RebuildHashCaches(&error)) {
    Fail("RebuildHashCaches failed: " + error);
  }
  std::vector<uint8_t> cached_root_hash;
  if (!hash_tree.GetCachedRootHash(&cached_root_hash, &error)) {
    Fail("GetCachedRootHash after rebuild failed: " + error);
  }
  std::vector<uint8_t> computed_root_hash;
  if (!hash_tree.ComputeRootHash(grovedb::MerkTree::ValueHashFn(), &computed_root_hash, &error)) {
    Fail("ComputeRootHash after rebuild failed: " + error);
  }
  if (cached_root_hash != computed_root_hash) {
    Fail("cached root hash should match computed root hash after rebuild");
  }

  std::vector<uint8_t> absence_proof_cached;
  std::vector<uint8_t> absence_root_cached;
  if (!hash_tree.GenerateAbsenceProof({'z'},
                                      grovedb::MerkTree::ValueHashFn(),
                                      &absence_proof_cached,
                                      &absence_root_cached,
                                      &error)) {
    Fail("canonical absence proof generation failed: " + error);
  }
  if (absence_root_cached != cached_root_hash || absence_root_cached != computed_root_hash) {
    Fail("canonical absence proof should reuse the cached root hash");
  }

  std::vector<uint8_t> limited_range_proof_cached;
  std::vector<uint8_t> limited_range_root_cached;
  if (!hash_tree.GenerateRangeProofWithLimit({'a'},
                                             {'d'},
                                             true,
                                             true,
                                             3,
                                             grovedb::MerkTree::ValueHashFn(),
                                             &limited_range_proof_cached,
                                             &limited_range_root_cached,
                                             &error)) {
    Fail("canonical limited range proof generation failed: " + error);
  }
  if (limited_range_root_cached != cached_root_hash || limited_range_root_cached != computed_root_hash) {
    Fail("canonical limited range proof should reuse the cached root hash");
  }

  grovedb::MerkTree custom_hash_tree;
  if (!custom_hash_tree.Insert({'a'}, {'1'}, &error) ||
      !custom_hash_tree.Insert({'b'}, {'2'}, &error) ||
      !custom_hash_tree.Insert({'c'}, {'3'}, &error) ||
      !custom_hash_tree.Insert({'d'}, {'4'}, &error)) {
    Fail("insert custom_hash_tree failed: " + error);
  }
  grovedb::MerkTree::ValueHashFn custom_hash_fn = CustomValueHashFn();
  custom_hash_tree.SetValueHashFn(custom_hash_fn);

  std::vector<uint8_t> custom_root_hash;
  if (!custom_hash_tree.ComputeRootHash(custom_hash_fn, &custom_root_hash, &error)) {
    Fail("ComputeRootHash with custom hash failed: " + error);
  }

  std::vector<uint8_t> custom_absence_proof;
  std::vector<uint8_t> custom_absence_root;
  if (!custom_hash_tree.GenerateAbsenceProof({'z'},
                                             custom_hash_fn,
                                             &custom_absence_proof,
                                             &custom_absence_root,
                                             &error)) {
    Fail("custom absence proof generation failed: " + error);
  }
  if (custom_absence_root != custom_root_hash) {
    Fail("custom absence proof should recompute root hash");
  }

  std::vector<uint8_t> custom_range_proof;
  std::vector<uint8_t> custom_range_root;
  if (!custom_hash_tree.GenerateRangeProofWithLimit({'a'},
                                                    {'d'},
                                                    true,
                                                    true,
                                                    3,
                                                    custom_hash_fn,
                                                    &custom_range_proof,
                                                    &custom_range_root,
                                                    &error)) {
    Fail("custom limited range proof generation failed: " + error);
  }
  if (custom_range_root != custom_root_hash) {
    Fail("custom limited range proof should recompute root hash");
  }

  {
    const std::vector<std::vector<uint8_t>> custom_reload_path = {
        {'c', 'u', 's', 't', 'o', 'm', '_', 'r', 'e', 'l', 'o', 'a', 'd'}};
    grovedb::MerkTree persisted_custom_source;
    if (!persisted_custom_source.Insert({'a'}, {'1'}, &error) ||
        !persisted_custom_source.Insert({'b'}, {'2'}, &error) ||
        !persisted_custom_source.Insert({'c'}, {'3'}, &error) ||
        !persisted_custom_source.Insert({'d'}, {'4'}, &error) ||
        !persisted_custom_source.Insert({'e'}, {'5'}, &error)) {
      Fail("insert persisted_custom_source failed: " + error);
    }
    if (!grovedb::MerkStorage::SaveTree(
            &storage, custom_reload_path, &persisted_custom_source, &error)) {
      Fail("save persisted_custom_source failed: " + error);
    }

    grovedb::MerkTree persisted_custom_loaded;
    if (!grovedb::MerkStorage::LoadTree(
            &storage, custom_reload_path, &persisted_custom_loaded, &error)) {
      Fail("load persisted_custom_loaded failed: " + error);
    }
    if (!persisted_custom_loaded.RebuildHashCaches(&error)) {
      Fail("RebuildHashCaches before hash-policy change failed: " + error);
    }
    persisted_custom_loaded.SetValueHashFn(custom_hash_fn);

    std::vector<uint8_t> persisted_custom_root;
    if (!persisted_custom_loaded.ComputeRootHash(custom_hash_fn, &persisted_custom_root, &error)) {
      Fail("ComputeRootHash on persisted_custom_loaded failed: " + error);
    }

    grovedb::MerkTree expected_custom_tree;
    if (!expected_custom_tree.Insert({'a'}, {'1'}, &error) ||
        !expected_custom_tree.Insert({'b'}, {'2'}, &error) ||
        !expected_custom_tree.Insert({'c'}, {'3'}, &error) ||
        !expected_custom_tree.Insert({'d'}, {'4'}, &error) ||
        !expected_custom_tree.Insert({'e'}, {'5'}, &error)) {
      Fail("insert expected_custom_tree failed: " + error);
    }
    expected_custom_tree.SetValueHashFn(custom_hash_fn);
    std::vector<uint8_t> expected_custom_root;
    if (!expected_custom_tree.ComputeRootHash(custom_hash_fn, &expected_custom_root, &error)) {
      Fail("ComputeRootHash on expected_custom_tree failed: " + error);
    }
    if (persisted_custom_root != expected_custom_root) {
      Fail("persisted tree should not reuse stale child hashes after SetValueHashFn");
    }

    std::vector<uint8_t> persisted_custom_proof;
    std::vector<uint8_t> persisted_custom_proof_root;
    if (!persisted_custom_loaded.GenerateRangeProofWithLimit({'a'},
                                                             {'e'},
                                                             true,
                                                             true,
                                                             5,
                                                             custom_hash_fn,
                                                             &persisted_custom_proof,
                                                             &persisted_custom_proof_root,
                                                             &error)) {
      Fail("GenerateRangeProofWithLimit on persisted_custom_loaded failed: " + error);
    }
    if (persisted_custom_proof_root != expected_custom_root) {
      Fail("custom proof root should match recomputed root after SetValueHashFn");
    }
  }

  {
    const std::vector<std::vector<uint8_t>> export_source_path = {
        {'e', 'x', 'p', 'o', 'r', 't', '_', 's', 'r', 'c'}};
    const std::vector<std::vector<uint8_t>> export_import_path = {
        {'e', 'x', 'p', 'o', 'r', 't', '_', 'd', 's', 't'}};
    grovedb::MerkTree export_tree;
    if (!export_tree.Insert({'a'}, {'1'}, &error) ||
        !export_tree.Insert({'b'}, {'2'}, &error) ||
        !export_tree.Insert({'c'}, {'3'}, &error) ||
        !export_tree.Insert({'d'}, {'4'}, &error) ||
        !export_tree.Insert({'e'}, {'5'}, &error)) {
      Fail("insert export_tree failed: " + error);
    }
    if (!grovedb::MerkStorage::SaveTree(&storage, export_source_path, &export_tree, &error)) {
      Fail("save export_tree failed: " + error);
    }

    grovedb::MerkTree export_loaded;
    if (!grovedb::MerkStorage::LoadTree(&storage, export_source_path, &export_loaded, &error)) {
      Fail("load export_tree failed: " + error);
    }
    if (!export_loaded.RebuildHashCaches(&error)) {
      Fail("RebuildHashCaches before export failed: " + error);
    }
    export_loaded.SetValueHashFn(grovedb::MerkTree::ValueHashFn());

    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> exported_entries;
    std::vector<uint8_t> exported_root_key;
    if (!export_loaded.ExportEncodedNodesForKeys({{'a'}, {'b'}, {'c'}, {'d'}, {'e'}},
                                                 &exported_entries,
                                                 &exported_root_key,
                                                 grovedb::MerkTree::ValueHashFn(),
                                                 &error)) {
      Fail("ExportEncodedNodesForKeys from lazy tree failed: " + error);
    }
    if (exported_entries.size() != 5) {
      Fail("expected all exported entries from lazy tree");
    }
    PutExportedTree(&storage, export_import_path, exported_entries, exported_root_key, &error);

    grovedb::MerkTree imported_tree;
    if (!grovedb::MerkStorage::LoadTree(&storage, export_import_path, &imported_tree, &error)) {
      Fail("load imported exported tree failed: " + error);
    }
    if (!imported_tree.Validate(&error)) {
      Fail("imported exported tree validation failed: " + error);
    }

    std::vector<uint8_t> exported_source_root;
    if (!export_loaded.ComputeRootHash(grovedb::MerkTree::ValueHashFn(), &exported_source_root, &error)) {
      Fail("compute export source root failed: " + error);
    }
    std::vector<uint8_t> imported_root;
    if (!imported_tree.ComputeRootHash(grovedb::MerkTree::ValueHashFn(), &imported_root, &error)) {
      Fail("compute imported export root failed: " + error);
    }
    if (imported_root != exported_source_root) {
      Fail("imported exported tree root mismatch");
    }
  }

  std::vector<uint8_t> chunk_proof_lazy;
  if (!lazy_loaded.GenerateChunkProof(2,
                                      grovedb::MerkTree::ValueHashFn(),
                                      false,
                                      &chunk_proof_lazy,
                                      &error)) {
    Fail("lazy chunk proof generation failed: " + error);
  }
  std::vector<uint8_t> chunk_proof_full;
  if (!hash_tree.GenerateChunkProof(2,
                                    grovedb::MerkTree::ValueHashFn(),
                                    false,
                                    &chunk_proof_full,
                                    &error)) {
    Fail("full chunk proof generation failed: " + error);
  }
  if (chunk_proof_lazy != chunk_proof_full) {
    Fail("lazy chunk proof mismatch");
  }

  std::vector<uint8_t> chunk_count_lazy;
  if (!lazy_loaded.GenerateChunkProof(2,
                                      grovedb::MerkTree::ValueHashFn(),
                                      true,
                                      &chunk_count_lazy,
                                      &error)) {
    Fail("lazy chunk count proof generation failed: " + error);
  }
  std::vector<uint8_t> chunk_count_full;
  if (!hash_tree.GenerateChunkProof(2,
                                    grovedb::MerkTree::ValueHashFn(),
                                    true,
                                    &chunk_count_full,
                                    &error)) {
    Fail("full chunk count proof generation failed: " + error);
  }
  if (chunk_count_lazy != chunk_count_full) {
    Fail("lazy chunk count proof mismatch");
  }

  // Modifying a lazily loaded tree should persist and reload cleanly.
  if (!lazy_loaded.Insert({'e'}, {'5'}, &error)) {
    Fail("insert into lazy tree failed: " + error);
  }
  bool deleted = false;
  if (!lazy_loaded.Delete({'b'}, &deleted, &error)) {
    Fail("delete from lazy tree failed: " + error);
  }
  if (!deleted) {
    Fail("expected delete to remove key");
  }
  std::vector<uint8_t> modified_hash;
  if (!lazy_loaded.ComputeRootHash(grovedb::MerkTree::ValueHashFn(), &modified_hash, &error)) {
    Fail("compute modified hash failed: " + error);
  }
  if (!grovedb::MerkStorage::SaveTree(&storage,
                                      {{'l', 'a', 'z', 'y'}},
                                      &lazy_loaded,
                                      &error)) {
    Fail("save modified tree failed: " + error);
  }
  grovedb::MerkTree modified_reload;
  if (!grovedb::MerkStorage::LoadTree(&storage,
                                      {{'l', 'a', 'z', 'y'}},
                                      &modified_reload,
                                      &error)) {
    Fail("reload modified tree failed: " + error);
  }
  if (!modified_reload.Get({'a'}, &value) || value != std::vector<uint8_t>({'1'})) {
    Fail("modified reload missing value");
  }
  if (modified_reload.Get({'b'}, &value)) {
    Fail("modified reload should not include deleted key");
  }
  if (!modified_reload.Get({'e'}, &value) || value != std::vector<uint8_t>({'5'})) {
    Fail("modified reload missing inserted key");
  }
  std::vector<uint8_t> reloaded_hash;
  if (!modified_reload.ComputeRootHash(grovedb::MerkTree::ValueHashFn(),
                                       &reloaded_hash,
                                       &error)) {
    Fail("compute reloaded hash failed: " + error);
  }
  if (reloaded_hash != modified_hash) {
    Fail("modified root hash mismatch");
  }

  // SaveTree must not consume dirty keys before a durable write. If export
  // fails, the pending persistence set must remain so a later retry can
  // succeed without rebuilding the tree state.
  {
    const std::vector<std::vector<uint8_t>> retry_path = {
        {'s', 'a', 'v', 'e', '_', 'r', 'e', 't', 'r', 'y'}};
    grovedb::MerkTree retry_tree;
    if (!retry_tree.Insert({'a'}, {'1'}, &error) ||
        !retry_tree.Insert({'b'}, {'2'}, &error)) {
      Fail("retry_tree insert failed: " + error);
    }
    retry_tree.ClearCachedHashes();
    retry_tree.SetValueHashFn([](const std::vector<uint8_t>&,
                                 const std::vector<uint8_t>&,
                                 std::vector<uint8_t>*,
                                 std::string* err) {
      if (err != nullptr) {
        *err = "injected export failure";
      }
      return false;
    });

    std::vector<std::vector<uint8_t>> dirty_before_failure;
    retry_tree.SnapshotDirtyKeys(&dirty_before_failure);
    if (dirty_before_failure.size() != 2) {
      Fail("retry_tree should have two dirty keys before failed save");
    }
    if (grovedb::MerkStorage::SaveTree(&storage, retry_path, &retry_tree, &error)) {
      Fail("SaveTree should fail when value hash export fails");
    }
    if (error != "injected export failure") {
      Fail("unexpected SaveTree failure error: " + error);
    }

    std::vector<std::vector<uint8_t>> dirty_after_failure;
    retry_tree.SnapshotDirtyKeys(&dirty_after_failure);
    if (dirty_after_failure != dirty_before_failure) {
      Fail("failed SaveTree should preserve dirty keys for retry");
    }

    retry_tree.SetValueHashFn(grovedb::MerkTree::ValueHashFn());
    if (!grovedb::MerkStorage::SaveTree(&storage, retry_path, &retry_tree, &error)) {
      Fail("SaveTree retry failed: " + error);
    }
    std::vector<std::vector<uint8_t>> dirty_after_success;
    retry_tree.SnapshotDirtyKeys(&dirty_after_success);
    if (!dirty_after_success.empty()) {
      Fail("successful SaveTree should acknowledge persisted dirty keys");
    }

    grovedb::MerkTree retry_reload;
    if (!grovedb::MerkStorage::LoadTree(&storage, retry_path, &retry_reload, &error)) {
      Fail("retry_tree reload failed: " + error);
    }
    if (!retry_reload.Get({'a'}, &value) || value != std::vector<uint8_t>({'1'})) {
      Fail("retry_tree reload missing key 'a'");
    }
    if (!retry_reload.Get({'b'}, &value) || value != std::vector<uint8_t>({'2'})) {
      Fail("retry_tree reload missing key 'b'");
    }
  }

  // The cost-tracking SaveTree path must still durably commit data and roots
  // together so a round-trip reload sees the saved tree.
  {
    const std::vector<std::vector<uint8_t>> cost_path = {
        {'c', 'o', 's', 't', '_', 's', 'a', 'v', 'e'}};
    grovedb::MerkTree cost_tree;
    if (!cost_tree.Insert({'a'}, {'1'}, &error) ||
        !cost_tree.Insert({'b'}, {'2'}, &error) ||
        !cost_tree.Insert({'c'}, {'3'}, &error)) {
      Fail("cost_tree insert failed: " + error);
    }
    grovedb::OperationCost save_cost;
    if (!grovedb::MerkStorage::SaveTree(&storage, cost_path, &cost_tree, &save_cost, &error)) {
      Fail("SaveTree with cost failed: " + error);
    }
    if (save_cost.hash_node_calls == 0) {
      Fail("SaveTree with cost should record hash work");
    }

    grovedb::MerkTree cost_reload;
    if (!grovedb::MerkStorage::LoadTree(&storage, cost_path, &cost_reload, &error)) {
      Fail("cost_tree reload failed: " + error);
    }
    if (!cost_reload.Get({'a'}, &value) || value != std::vector<uint8_t>({'1'})) {
      Fail("cost_tree reload missing key 'a'");
    }
    if (!cost_reload.Get({'b'}, &value) || value != std::vector<uint8_t>({'2'})) {
      Fail("cost_tree reload missing key 'b'");
    }
    if (!cost_reload.Get({'c'}, &value) || value != std::vector<uint8_t>({'3'})) {
      Fail("cost_tree reload missing key 'c'");
    }
  }

  // Clearing a tree and saving must update persisted-root tracking so a later
  // same-root-key reinsert rewrites the roots entry.
  grovedb::MerkTree single;
  if (!single.Insert({'s'}, {'1'}, &error)) {
    Fail("insert single tree failed: " + error);
  }
  if (!grovedb::MerkStorage::SaveTree(&storage, {{'s', 'i', 'n', 'g', 'l', 'e'}}, &single, &error)) {
    Fail("save single tree failed: " + error);
  }

  bool single_deleted = false;
  if (!single.Delete({'s'}, &single_deleted, &error)) {
    Fail("delete single key failed: " + error);
  }
  if (!single_deleted) {
    Fail("single delete did not report deleted");
  }
  if (!grovedb::MerkStorage::SaveTree(&storage, {{'s', 'i', 'n', 'g', 'l', 'e'}}, &single, &error)) {
    Fail("save empty single tree failed: " + error);
  }

  if (!single.Insert({'s'}, {'1'}, &error)) {
    Fail("reinsert single key failed: " + error);
  }
  if (!grovedb::MerkStorage::SaveTree(&storage, {{'s', 'i', 'n', 'g', 'l', 'e'}}, &single, &error)) {
    Fail("save single tree after reinsert failed: " + error);
  }

  grovedb::MerkTree single_reload;
  if (!grovedb::MerkStorage::LoadTree(
          &storage, {{'s', 'i', 'n', 'g', 'l', 'e'}}, &single_reload, &error)) {
    Fail("reload single tree failed: " + error);
  }
  if (!single_reload.Get({'s'}, &value) || value != std::vector<uint8_t>({'1'})) {
    Fail("reloaded single tree missing reinserted value");
  }

  // Empty SaveTree should still finalize lifecycle state on the same instance:
  // consume dirty/deleted trackers and attach storage for post-save reuse.
  {
    const std::vector<std::vector<uint8_t>> empty_save_path = {
        {'e', 'm', 'p', 't', 'y', '_', 's', 'a', 'v', 'e'}};
    grovedb::MerkTree empty_tree;
    if (empty_tree.IsLazyLoading()) {
      Fail("empty_tree should not be lazy before first save");
    }
    if (!grovedb::MerkStorage::SaveTree(&storage, empty_save_path, &empty_tree, &error)) {
      Fail("save empty_tree failed: " + error);
    }
    if (!empty_tree.IsLazyLoading()) {
      Fail("empty_tree should be lazy after empty save");
    }
    if (!empty_tree.Insert({'k'}, {'v'}, &error)) {
      Fail("empty_tree insert after empty save failed: " + error);
    }
    if (!grovedb::MerkStorage::SaveTree(&storage, empty_save_path, &empty_tree, &error)) {
      Fail("re-save empty_tree with key failed: " + error);
    }
    grovedb::MerkTree empty_tree_reload;
    if (!grovedb::MerkStorage::LoadTree(&storage, empty_save_path, &empty_tree_reload, &error)) {
      Fail("reload empty_tree path failed: " + error);
    }
    if (!empty_tree_reload.Get({'k'}, &value) || value != std::vector<uint8_t>({'v'})) {
      Fail("empty_tree reload missing inserted key");
    }
  }

  // SaveTreeToBatch empty-tree path should clear within the same batch, even
  // when the same path was staged earlier in that batch.
  grovedb::RocksDbWrapper::Transaction tx;
  if (!storage.BeginTransaction(&tx, &error)) {
    Fail("begin tx failed: " + error);
  }
  grovedb::RocksDbWrapper::WriteBatch tx_batch;
  grovedb::MerkTree tx_tree;
  if (!tx_tree.Insert({'k'}, {'v'}, &error)) {
    Fail("tx tree insert failed: " + error);
  }
  const std::vector<std::vector<uint8_t>> tx_path = {{'b', 'a', 't', 'c', 'h'}};
  if (!grovedb::MerkStorage::SaveTreeToBatch(&storage, &tx, tx_path, &tx_tree, &tx_batch, &error)) {
    Fail("SaveTreeToBatch non-empty failed: " + error);
  }
  bool tx_tree_deleted = false;
  if (!tx_tree.Delete({'k'}, &tx_tree_deleted, &error)) {
    Fail("tx tree delete failed: " + error);
  }
  if (!tx_tree_deleted) {
    Fail("tx tree delete did not remove inserted key");
  }
  if (!grovedb::MerkStorage::SaveTreeToBatch(&storage, &tx, tx_path, &tx_tree, &tx_batch, &error)) {
    Fail("SaveTreeToBatch empty failed: " + error);
  }
  // Reuse the same merk instance after an empty SaveTreeToBatch and stage new
  // writes in the same tx to verify tracker finalization parity.
  if (!tx_tree.Insert({'m'}, {'n'}, &error)) {
    Fail("tx tree insert after empty SaveTreeToBatch failed: " + error);
  }
  if (!grovedb::MerkStorage::SaveTreeToBatch(&storage, &tx, tx_path, &tx_tree, &tx_batch, &error)) {
    Fail("SaveTreeToBatch post-empty reinsert failed: " + error);
  }
  if (!storage.CommitBatch(tx_batch, &tx, &error)) {
    Fail("commit tx batch (post-empty reinsert) failed: " + error);
  }
  if (!tx.Commit(&error)) {
    Fail("commit tx (post-empty reinsert) failed: " + error);
  }
  grovedb::MerkTree tx_reload;
  if (!grovedb::MerkStorage::LoadTree(&storage, tx_path, &tx_reload, &error)) {
    Fail("reload tx path (post-empty reinsert) failed: " + error);
  }
  if (tx_reload.Get({'k'}, &value)) {
    Fail("tx reload should not contain previously cleared key 'k'");
  }
  if (!tx_reload.Get({'m'}, &value) || value != std::vector<uint8_t>({'n'})) {
    Fail("tx reload missing key inserted after empty SaveTreeToBatch");
  }

  // SaveTreeToBatch should attach transactional storage context so the same
  // merk instance can keep operating in lazy mode without an explicit reopen.
  {
    grovedb::RocksDbWrapper::Transaction attach_tx;
    if (!storage.BeginTransaction(&attach_tx, &error)) {
      Fail("begin attach tx failed: " + error);
    }
    grovedb::RocksDbWrapper::WriteBatch attach_batch;
    grovedb::MerkTree batch_attach_tree;
    for (char c = 'a'; c <= 'f'; ++c) {
      if (!batch_attach_tree.Insert({static_cast<uint8_t>(c)},
                                    {static_cast<uint8_t>(c + 1)},
                                    &error)) {
        Fail("batch_attach_tree insert failed: " + error);
      }
    }
    if (batch_attach_tree.IsLazyLoading()) {
      Fail("batch_attach_tree should not be lazy before SaveTreeToBatch");
    }
    const std::vector<std::vector<uint8_t>> attach_batch_path = {
        {'b', 'a', 't', 'c', 'h', '_', 'a', 't', 't', 'a', 'c', 'h'}};
    if (!grovedb::MerkStorage::SaveTreeToBatch(
            &storage, &attach_tx, attach_batch_path, &batch_attach_tree, &attach_batch, &error)) {
      Fail("SaveTreeToBatch attach path failed: " + error);
    }
    if (!batch_attach_tree.IsLazyLoading()) {
      Fail("batch_attach_tree should be lazy after SaveTreeToBatch");
    }
    std::vector<uint8_t> attach_batch_val;
    if (!batch_attach_tree.Get({'c'}, &attach_batch_val) ||
        attach_batch_val != std::vector<uint8_t>({static_cast<uint8_t>('d')})) {
      Fail("batch_attach_tree Get after SaveTreeToBatch failed");
    }
    if (!storage.CommitBatch(attach_batch, &attach_tx, &error)) {
      Fail("commit attach batch failed: " + error);
    }
    if (!attach_tx.Commit(&error)) {
      Fail("commit attach tx failed: " + error);
    }
    grovedb::MerkTree attach_batch_reload;
    if (!grovedb::MerkStorage::LoadTree(
            &storage, attach_batch_path, &attach_batch_reload, &error)) {
      Fail("reload attach batch tree failed: " + error);
    }
    if (!attach_batch_reload.Get({'f'}, &attach_batch_val) ||
        attach_batch_val != std::vector<uint8_t>({static_cast<uint8_t>('g')})) {
      Fail("reload attach batch tree missing expected value");
    }
  }

  // Post-commit fallback behavior: a merk attached via SaveTreeToBatch should
  // keep lazy traversal working even after the tx object is closed.
  {
    const std::vector<std::vector<uint8_t>> tx_fallback_path = {
        {'t', 'x', '_', 'f', 'a', 'l', 'l', 'b', 'a', 'c', 'k'}};
    grovedb::MerkTree baseline_tree;
    for (char c = 'a'; c <= 'h'; ++c) {
      if (!baseline_tree.Insert({static_cast<uint8_t>(c)},
                                {static_cast<uint8_t>(c + 1)},
                                &error)) {
        Fail("baseline_tree insert failed: " + error);
      }
    }
    if (!grovedb::MerkStorage::SaveTree(&storage, tx_fallback_path, &baseline_tree, &error)) {
      Fail("save baseline_tree failed: " + error);
    }

    grovedb::RocksDbWrapper::Transaction tx_fallback;
    if (!storage.BeginTransaction(&tx_fallback, &error)) {
      Fail("begin tx_fallback failed: " + error);
    }
    grovedb::MerkTree tx_lazy_tree;
    if (!grovedb::MerkStorage::LoadTree(
            &storage, &tx_fallback, tx_fallback_path, &tx_lazy_tree, &error)) {
      Fail("load tx_lazy_tree failed: " + error);
    }
    if (!tx_lazy_tree.IsLazyLoading()) {
      Fail("tx_lazy_tree should be lazy after tx load");
    }
    if (!tx_lazy_tree.Insert({'i'}, {'j'}, &error)) {
      Fail("insert into tx_lazy_tree failed: " + error);
    }
    grovedb::RocksDbWrapper::WriteBatch tx_fallback_batch;
    if (!grovedb::MerkStorage::SaveTreeToBatch(
            &storage,
            &tx_fallback,
            tx_fallback_path,
            &tx_lazy_tree,
            &tx_fallback_batch,
            &error)) {
      Fail("SaveTreeToBatch tx_fallback failed: " + error);
    }
    if (!storage.CommitBatch(tx_fallback_batch, &tx_fallback, &error)) {
      Fail("commit tx_fallback batch failed: " + error);
    }
    if (!tx_fallback.Commit(&error)) {
      Fail("commit tx_fallback failed: " + error);
    }

    std::vector<uint8_t> fallback_value;
    if (!tx_lazy_tree.Get({'a'}, &fallback_value) ||
        fallback_value != std::vector<uint8_t>({'b'})) {
      grovedb::MerkTree::NodeMeta fallback_meta;
      std::string meta_error;
      if (!tx_lazy_tree.GetNodeMeta({'a'}, &fallback_meta, &meta_error)) {
        Fail("tx_lazy_tree post-commit lazy get failed; meta error: " + meta_error);
      }
      Fail("tx_lazy_tree post-commit lazy get failed; meta lookup unexpectedly succeeded");
    }
    std::vector<uint8_t> fallback_hash;
    if (!tx_lazy_tree.ComputeRootHash(grovedb::MerkTree::ValueHashFn(),
                                      &fallback_hash,
                                      &error)) {
      Fail("tx_lazy_tree post-commit root hash failed: " + error);
    }
    if (fallback_hash.size() != 32) {
      Fail("tx_lazy_tree post-commit hash should be 32 bytes");
    }
    if (!tx_lazy_tree.Get({'i'}, &fallback_value) ||
        fallback_value != std::vector<uint8_t>({'j'})) {
      Fail("tx_lazy_tree post-commit missing inserted key");
    }
  }

  // GetNodeMeta lazy traversal: loading a tree from storage should allow
  // GetNodeMeta to succeed for non-root keys without calling EnsureFullyLoaded
  // first (P-057 regression).
  {
    grovedb::MerkTree meta_tree;
    for (char c = 'a'; c <= 'g'; ++c) {
      if (!meta_tree.Insert({static_cast<uint8_t>(c)}, {static_cast<uint8_t>(c + 1)}, &error)) {
        Fail("meta_tree insert failed: " + error);
      }
    }
    const std::vector<std::vector<uint8_t>> meta_path = {{'m', 'e', 't', 'a'}};
    if (!grovedb::MerkStorage::SaveTree(&storage, meta_path, &meta_tree, &error)) {
      Fail("save meta_tree failed: " + error);
    }
    grovedb::MerkTree meta_loaded;
    if (!grovedb::MerkStorage::LoadTree(&storage, meta_path, &meta_loaded, &error)) {
      Fail("load meta_tree failed: " + error);
    }
    // Query leaf-level keys that require lazy child loading to reach.
    for (char c = 'a'; c <= 'g'; ++c) {
      grovedb::MerkTree::NodeMeta nm;
      if (!meta_loaded.GetNodeMeta({static_cast<uint8_t>(c)}, &nm, &error)) {
        Fail(std::string("GetNodeMeta lazy failed for key '") + c + "': " + error);
      }
    }
    // Leaf nodes should have no children.
    grovedb::MerkTree::NodeMeta leaf_meta;
    if (!meta_loaded.GetNodeMeta({'a'}, &leaf_meta, &error)) {
      Fail("GetNodeMeta lazy leaf failed: " + error);
    }
    if (leaf_meta.has_left || leaf_meta.has_right) {
      // 'a' is the smallest key - it's a leaf in the AVL tree, so at minimum
      // it should have no left child.
      if (leaf_meta.has_left) {
        Fail("leaf 'a' should not have a left child");
      }
    }
  }

  // Missing persisted child nodes during lazy traversal must surface as
  // corruption instead of being rewritten into normal subtree absence.
  {
    const std::vector<std::vector<uint8_t>> corrupt_path = {
        {'c', 'o', 'r', 'r', 'u', 'p', 't'}};
    grovedb::MerkTree corrupt_tree;
    for (char c = 'a'; c <= 'e'; ++c) {
      if (!corrupt_tree.Insert({static_cast<uint8_t>(c)},
                               {static_cast<uint8_t>(c + 1)},
                               &error)) {
        Fail("corrupt_tree insert failed: " + error);
      }
    }
    if (!grovedb::MerkStorage::SaveTree(&storage, corrupt_path, &corrupt_tree, &error)) {
      Fail("save corrupt_tree failed: " + error);
    }

    grovedb::MerkTree corrupt_loaded;
    if (!grovedb::MerkStorage::LoadTree(&storage, corrupt_path, &corrupt_loaded, &error)) {
      Fail("load corrupt_tree failed: " + error);
    }

    std::vector<uint8_t> corrupt_root_key;
    if (!corrupt_loaded.RootKey(&corrupt_root_key) || corrupt_root_key.empty()) {
      Fail("corrupt_loaded missing root key");
    }
    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> persisted_nodes;
    if (!storage.ScanPrefix(
            grovedb::ColumnFamilyKind::kDefault, corrupt_path, &persisted_nodes, &error)) {
      Fail("scan corrupt_path failed: " + error);
    }
    std::vector<uint8_t> missing_child_key;
    for (const auto& kv : persisted_nodes) {
      if (kv.first != corrupt_root_key) {
        missing_child_key = kv.first;
        break;
      }
    }
    if (missing_child_key.empty()) {
      Fail("failed to select non-root persisted child key");
    }
    bool deleted_persisted_child = false;
    if (!storage.Delete(grovedb::ColumnFamilyKind::kDefault,
                        corrupt_path,
                        missing_child_key,
                        &deleted_persisted_child,
                        &error)) {
      Fail("delete persisted child node failed: " + error);
    }
    if (!deleted_persisted_child) {
      Fail("expected persisted child node delete to report success");
    }

    grovedb::MerkTree::NodeMeta corrupt_meta;
    if (corrupt_loaded.GetNodeMeta(missing_child_key, &corrupt_meta, &error)) {
      Fail("lazy traversal should fail when a persisted child node is missing");
    }
    if (error != "encoded node not found") {
      Fail("unexpected lazy corruption error: " + error);
    }
  }

  // Cross-tx lazy-load after commit: a merk attached to a transaction should
  // continue to work via base storage fallback after the transaction commits.
  // This validates that the same merk instance remains usable for lazy traversal
  // without requiring an explicit reopen (P-057 regression).
  {
    const std::vector<std::vector<uint8_t>> cross_tx_path = {{'c', 'r', 'o', 's', 's', '_', 't', 'x'}};
    // Phase 1: insert initial data and persist.
    {
      grovedb::MerkTree initial_tree;
      if (!initial_tree.Insert({'k', '1'}, {'v', '1'}, &error)) {
        Fail("initial_tree insert k1 failed: " + error);
      }
      if (!initial_tree.Insert({'k', '2'}, {'v', '2'}, &error)) {
        Fail("initial_tree insert k2 failed: " + error);
      }
      if (!grovedb::MerkStorage::SaveTree(&storage, cross_tx_path, &initial_tree, &error)) {
        Fail("save initial_tree failed: " + error);
      }
    }
    // Phase 2: open transaction and load merk attached to that transaction.
    grovedb::RocksDbWrapper::Transaction cross_tx;
    if (!storage.BeginTransaction(&cross_tx, &error)) {
      Fail("begin cross_tx failed: " + error);
    }
    grovedb::MerkTree tx_merk;
    if (!grovedb::MerkStorage::LoadTree(&storage, &cross_tx, cross_tx_path, &tx_merk, &error)) {
      Fail("load tx_merk failed: " + error);
    }
    // Verify initial state is visible through tx-attached merk.
    std::vector<uint8_t> tx_val;
    if (!tx_merk.Get({'k', '1'}, &tx_val) || tx_val != std::vector<uint8_t>({'v', '1'})) {
      Fail("tx_merk should see k1=v1 before mutation");
    }
    // Phase 3: mutate through the tx-attached merk and commit.
    if (!tx_merk.Insert({'k', '1'}, {'v', '1', '_'}, &error)) {
      Fail("tx_merk replace k1 failed: " + error);
    }
    bool deleted = false;
    if (!tx_merk.Delete({'k', '2'}, &deleted, &error)) {
      Fail("tx_merk delete k2 failed: " + error);
    }
    if (!deleted) {
      Fail("tx_merk delete k2 should report deleted");
    }
    if (!tx_merk.Insert({'k', '3'}, {'v', '3'}, &error)) {
      Fail("tx_merk insert k3 failed: " + error);
    }
    grovedb::RocksDbWrapper::WriteBatch cross_batch;
    if (!grovedb::MerkStorage::SaveTreeToBatch(&storage, &cross_tx, cross_tx_path, &tx_merk, &cross_batch, &error)) {
      Fail("SaveTreeToBatch cross_tx failed: " + error);
    }
    if (!storage.CommitBatch(cross_batch, &cross_tx, &error)) {
      Fail("commit cross_tx batch failed: " + error);
    }
    if (!cross_tx.Commit(&error)) {
      Fail("commit cross_tx failed: " + error);
    }
    // Phase 4: tx_merk (attached to the now-committed tx) should continue to work
    // via base storage fallback for lazy traversal without explicit reopen.
    if (!tx_merk.Get({'k', '1'}, &tx_val) || tx_val != std::vector<uint8_t>({'v', '1', '_'})) {
      Fail("tx_merk should see k1=v1_ after commit via base storage fallback");
    }
    if (tx_merk.Get({'k', '2'}, &tx_val)) {
      Fail("tx_merk should not see k2 after commit deleted it");
    }
    if (!tx_merk.Get({'k', '3'}, &tx_val) || tx_val != std::vector<uint8_t>({'v', '3'})) {
      Fail("tx_merk should see k3=v3 after commit via base storage fallback");
    }
    // Root hash should still be computable via base storage fallback.
    std::vector<uint8_t> tx_hash;
    if (!tx_merk.ComputeRootHash(grovedb::MerkTree::ValueHashFn(), &tx_hash, &error)) {
      Fail("tx_merk ComputeRootHash after commit failed: " + error);
    }
    if (tx_hash.size() != 32) {
      Fail("tx_merk root hash should be 32 bytes");
    }
    // Additional lazy traversal: GetNodeMeta should work post-commit.
    grovedb::MerkTree::NodeMeta meta_k1;
    if (!tx_merk.GetNodeMeta({'k', '1'}, &meta_k1, &error)) {
      Fail("tx_merk GetNodeMeta k1 failed post-commit: " + error);
    }
  }

  // Post-save attach-storage: after SaveTree, the tree should retain a storage
  // context and have lazy_loading_ enabled so that subsequent operations (Get,
  // Insert, Delete, proofs) work via lazy-load instead of requiring a separate
  // LoadTree round-trip. This matches Rust's post-commit merk behavior where
  // the merk keeps its storage backend reference (P-072 regression).
  {
    grovedb::MerkTree attach_tree;
    // Build a tree with enough nodes that children are not trivially accessible.
    for (char c = 'a'; c <= 'f'; ++c) {
      if (!attach_tree.Insert({static_cast<uint8_t>(c)},
                              {static_cast<uint8_t>(c + 1)},
                              &error)) {
        Fail("attach_tree insert failed: " + error);
      }
    }
    // Before save, tree should NOT be in lazy mode.
    if (attach_tree.IsLazyLoading()) {
      Fail("tree should not be lazy before save");
    }
    const std::vector<std::vector<uint8_t>> attach_path = {{'a', 't', 'c', 'h'}};
    if (!grovedb::MerkStorage::SaveTree(&storage, attach_path, &attach_tree, &error)) {
      Fail("save attach_tree failed: " + error);
    }
    // After save, tree should be in lazy-load mode with storage context.
    if (!attach_tree.IsLazyLoading()) {
      Fail("tree should be lazy after save");
    }
    // Get should still work on the saved tree (via in-memory + lazy fallback).
    std::vector<uint8_t> attach_val;
    if (!attach_tree.Get({'c'}, &attach_val) ||
        attach_val != std::vector<uint8_t>({static_cast<uint8_t>('d')})) {
      Fail("attach_tree Get after save failed");
    }
    // Insert into the tree after save should work (lazy-load traversal).
    if (!attach_tree.Insert({'g'}, {'h'}, &error)) {
      Fail("attach_tree Insert after save failed: " + error);
    }
    // Delete from the tree after save should work.
    bool attach_deleted = false;
    if (!attach_tree.Delete({'a'}, &attach_deleted, &error)) {
      Fail("attach_tree Delete after save failed: " + error);
    }
    if (!attach_deleted) {
      Fail("attach_tree Delete should have removed 'a'");
    }
    // Root hash should still be computable.
    std::vector<uint8_t> attach_hash;
    if (!attach_tree.ComputeRootHash(grovedb::MerkTree::ValueHashFn(),
                                     &attach_hash,
                                     &error)) {
      Fail("attach_tree ComputeRootHash after save failed: " + error);
    }
    if (attach_hash.size() != 32) {
      Fail("attach_tree root hash should be 32 bytes");
    }
    // Re-save and reload to verify state is correct.
    if (!grovedb::MerkStorage::SaveTree(&storage, attach_path, &attach_tree, &error)) {
      Fail("re-save attach_tree failed: " + error);
    }
    grovedb::MerkTree attach_reload;
    if (!grovedb::MerkStorage::LoadTree(&storage, attach_path, &attach_reload, &error)) {
      Fail("reload attach_tree failed: " + error);
    }
    if (attach_reload.Get({'a'}, &attach_val)) {
      Fail("attach_reload should not have deleted key 'a'");
    }
    if (!attach_reload.Get({'g'}, &attach_val) ||
        attach_val != std::vector<uint8_t>({'h'})) {
      Fail("attach_reload missing inserted key 'g'");
    }
    std::vector<uint8_t> reload_hash;
    if (!attach_reload.ComputeRootHash(grovedb::MerkTree::ValueHashFn(),
                                       &reload_hash,
                                       &error)) {
      Fail("attach_reload ComputeRootHash failed: " + error);
    }
    if (reload_hash != attach_hash) {
      Fail("attach_tree root hash mismatch after re-save and reload");
    }
  }

  // Child subtree clear lifecycle: verify that clearing a child subtree
  // does not affect the parent subtree, and that re-insertion into the
  // cleared child works correctly while parent remains intact.
  // This validates Merk clear() semantics in a nested subtree context.
  {
    const std::vector<std::vector<uint8_t>> parent_path = {{'p', 'a', 'r', 'e', 'n', 't'}};
    const std::vector<std::vector<uint8_t>> child_path = {{'p', 'a', 'r', 'e', 'n', 't'}, {'c', 'h', 'i', 'l', 'd'}};

    // Phase 1: create parent with k1=v1, k2=v2
    {
      grovedb::MerkTree parent_tree;
      if (!parent_tree.Insert({'k', '1'}, {'v', '1'}, &error)) {
        Fail("parent_tree insert k1 failed: " + error);
      }
      if (!parent_tree.Insert({'k', '2'}, {'v', '2'}, &error)) {
        Fail("parent_tree insert k2 failed: " + error);
      }
      if (!grovedb::MerkStorage::SaveTree(&storage, parent_path, &parent_tree, &error)) {
        Fail("save parent_tree failed: " + error);
      }
    }

    // Phase 2: create child with ck1=cv1, ck2=cv2
    {
      grovedb::MerkTree child_tree;
      if (!child_tree.Insert({'c', 'k', '1'}, {'c', 'v', '1'}, &error)) {
        Fail("child_tree insert ck1 failed: " + error);
      }
      if (!child_tree.Insert({'c', 'k', '2'}, {'c', 'v', '2'}, &error)) {
        Fail("child_tree insert ck2 failed: " + error);
      }
      if (!grovedb::MerkStorage::SaveTree(&storage, child_path, &child_tree, &error)) {
        Fail("save child_tree failed: " + error);
      }
    }

    // Phase 3: verify parent and child state before clear
    {
      grovedb::MerkTree parent_loaded;
      if (!grovedb::MerkStorage::LoadTree(&storage, parent_path, &parent_loaded, &error)) {
        Fail("load parent_tree before clear failed: " + error);
      }
      std::vector<uint8_t> val;
      if (!parent_loaded.Get({'k', '1'}, &val) || val != std::vector<uint8_t>({'v', '1'})) {
        Fail("parent k1 should be v1 before clear");
      }
      if (!parent_loaded.Get({'k', '2'}, &val) || val != std::vector<uint8_t>({'v', '2'})) {
        Fail("parent k2 should be v2 before clear");
      }

      grovedb::MerkTree child_loaded;
      if (!grovedb::MerkStorage::LoadTree(&storage, child_path, &child_loaded, &error)) {
        Fail("load child_tree before clear failed: " + error);
      }
      if (!child_loaded.Get({'c', 'k', '1'}, &val) || val != std::vector<uint8_t>({'c', 'v', '1'})) {
        Fail("child ck1 should be cv1 before clear");
      }
      if (!child_loaded.Get({'c', 'k', '2'}, &val) || val != std::vector<uint8_t>({'c', 'v', '2'})) {
        Fail("child ck2 should be cv2 before clear");
      }
    }

    // Phase 4: clear child subtree
    {
      if (!grovedb::MerkStorage::ClearTree(&storage, child_path, &error)) {
        Fail("clear child_tree failed: " + error);
      }
    }

    // Phase 5: verify parent intact, child empty
    {
      grovedb::MerkTree parent_loaded;
      if (!grovedb::MerkStorage::LoadTree(&storage, parent_path, &parent_loaded, &error)) {
        Fail("load parent_tree after clear failed: " + error);
      }
      std::vector<uint8_t> val;
      if (!parent_loaded.Get({'k', '1'}, &val) || val != std::vector<uint8_t>({'v', '1'})) {
        Fail("parent k1 should still be v1 after child clear");
      }
      if (!parent_loaded.Get({'k', '2'}, &val) || val != std::vector<uint8_t>({'v', '2'})) {
        Fail("parent k2 should still be v2 after child clear");
      }

      grovedb::MerkTree child_loaded;
      if (!grovedb::MerkStorage::LoadTree(&storage, child_path, &child_loaded, &error)) {
        Fail("load child_tree after clear failed: " + error);
      }
      if (child_loaded.Get({'c', 'k', '1'}, &val)) {
        Fail("child ck1 should be cleared");
      }
      if (child_loaded.Get({'c', 'k', '2'}, &val)) {
        Fail("child ck2 should be cleared");
      }
    }

    // Phase 6: re-insert into child (ck3=cv3)
    {
      grovedb::MerkTree child_tree;
      if (!child_tree.Insert({'c', 'k', '3'}, {'c', 'v', '3'}, &error)) {
        Fail("child_tree insert ck3 failed: " + error);
      }
      if (!grovedb::MerkStorage::SaveTree(&storage, child_path, &child_tree, &error)) {
        Fail("save child_tree after reinsert failed: " + error);
      }
    }

    // Phase 7: final verify - parent intact, child has ck3=cv3
    {
      grovedb::MerkTree parent_loaded;
      if (!grovedb::MerkStorage::LoadTree(&storage, parent_path, &parent_loaded, &error)) {
        Fail("load parent_tree final failed: " + error);
      }
      std::vector<uint8_t> val;
      if (!parent_loaded.Get({'k', '1'}, &val) || val != std::vector<uint8_t>({'v', '1'})) {
        Fail("parent k1 should still be v1 in final verify");
      }

      grovedb::MerkTree child_loaded;
      if (!grovedb::MerkStorage::LoadTree(&storage, child_path, &child_loaded, &error)) {
        Fail("load child_tree final failed: " + error);
      }
      if (!child_loaded.Get({'c', 'k', '3'}, &val) || val != std::vector<uint8_t>({'c', 'v', '3'})) {
        Fail("child ck3 should be cv3 in final verify");
      }
    }
  }

  // Clear-and-reopen lifecycle: clear a persisted merk, verify empty-root hash
  // parity, then reinsert a disjoint key set and verify old keys remain absent.
  {
    const std::vector<std::vector<uint8_t>> clear_reopen_path = {
        {'c', 'l', 'e', 'a', 'r', '_', 'r', 'e', 'o', 'p', 'e', 'n'}};
    grovedb::MerkTree initial_tree;
    if (!initial_tree.Insert({'k', '1'}, {'v', '1'}, &error) ||
        !initial_tree.Insert({'k', '2'}, {'v', '2'}, &error) ||
        !initial_tree.Insert({'k', '3'}, {'v', '3'}, &error)) {
      Fail("clear_reopen initial insert failed: " + error);
    }
    if (!grovedb::MerkStorage::SaveTree(&storage, clear_reopen_path, &initial_tree, &error)) {
      Fail("clear_reopen initial save failed: " + error);
    }
    if (!grovedb::MerkStorage::ClearTree(&storage, clear_reopen_path, &error)) {
      Fail("clear_reopen clear failed: " + error);
    }

    grovedb::MerkTree after_clear;
    if (!grovedb::MerkStorage::LoadTree(&storage, clear_reopen_path, &after_clear, &error)) {
      Fail("clear_reopen load after clear failed: " + error);
    }
    std::vector<uint8_t> should_be_missing;
    if (after_clear.Get({'k', '1'}, &should_be_missing) ||
        after_clear.Get({'k', '2'}, &should_be_missing) ||
        after_clear.Get({'k', '3'}, &should_be_missing)) {
      Fail("clear_reopen tree should be empty after clear");
    }
    std::vector<uint8_t> clear_hash;
    if (!after_clear.ComputeRootHash(grovedb::MerkTree::ValueHashFn(), &clear_hash, &error)) {
      Fail("clear_reopen compute hash after clear failed: " + error);
    }
    grovedb::MerkTree expected_empty;
    std::vector<uint8_t> expected_empty_hash;
    if (!expected_empty.ComputeRootHash(
            grovedb::MerkTree::ValueHashFn(), &expected_empty_hash, &error)) {
      Fail("clear_reopen compute expected empty hash failed: " + error);
    }
    if (clear_hash != expected_empty_hash) {
      Fail("clear_reopen hash after clear should equal empty-tree hash");
    }

    if (!after_clear.Insert({'k', '1', '0'}, {'v', '1', '0'}, &error) ||
        !after_clear.Insert({'k', '2', '0'}, {'v', '2', '0'}, &error)) {
      Fail("clear_reopen reinsert failed: " + error);
    }
    if (!grovedb::MerkStorage::SaveTree(&storage, clear_reopen_path, &after_clear, &error)) {
      Fail("clear_reopen save after reinsert failed: " + error);
    }

    grovedb::MerkTree final_tree;
    if (!grovedb::MerkStorage::LoadTree(&storage, clear_reopen_path, &final_tree, &error)) {
      Fail("clear_reopen final load failed: " + error);
    }
    std::vector<uint8_t> out;
    if (final_tree.Get({'k', '1'}, &out) || final_tree.Get({'k', '2'}, &out) ||
        final_tree.Get({'k', '3'}, &out)) {
      Fail("clear_reopen old keys should remain absent");
    }
    if (!final_tree.Get({'k', '1', '0'}, &out) || out != std::vector<uint8_t>({'v', '1', '0'})) {
      Fail("clear_reopen missing k10=v10");
    }
    if (!final_tree.Get({'k', '2', '0'}, &out) || out != std::vector<uint8_t>({'v', '2', '0'})) {
      Fail("clear_reopen missing k20=v20");
    }
  }

  // Child merk reopen after parent mutation: validate that reopening a child
  // merk after parent subtree mutation correctly reflects the parent's changes
  // through lazy-link traversal.
  {
    const std::vector<std::vector<uint8_t>> parent_path = {
        {'c', 'h', 'i', 'l', 'd', '_', 'p', 'a', 'r', 'e', 'n', 't'}};
    const std::vector<std::vector<uint8_t>> child_path = {
        {'c', 'h', 'i', 'l', 'd', '_', 'p', 'a', 'r', 'e', 'n', 't'},
        {'c', 'h', 'i', 'l', 'd'}};

    // Phase 1: Create parent with pk1=pv1, pk2=pv2
    {
      grovedb::MerkTree parent_tree;
      if (!parent_tree.Insert({'p', 'k', '1'}, {'p', 'v', '1'}, &error) ||
          !parent_tree.Insert({'p', 'k', '2'}, {'p', 'v', '2'}, &error)) {
        Fail("child_reopen parent initial insert failed: " + error);
      }
      if (!grovedb::MerkStorage::SaveTree(&storage, parent_path, &parent_tree, &error)) {
        Fail("child_reopen parent initial save failed: " + error);
      }
    }

    // Phase 2: Create child with ck1=cv1, ck2=cv2
    {
      grovedb::MerkTree child_tree;
      if (!child_tree.Insert({'c', 'k', '1'}, {'c', 'v', '1'}, &error) ||
          !child_tree.Insert({'c', 'k', '2'}, {'c', 'v', '2'}, &error)) {
        Fail("child_reopen child initial insert failed: " + error);
      }
      if (!grovedb::MerkStorage::SaveTree(&storage, child_path, &child_tree, &error)) {
        Fail("child_reopen child initial save failed: " + error);
      }
    }

    // Phase 3: Mutate parent - delete pk1, insert pk3=pv3
    {
      grovedb::MerkTree parent_tree;
      if (!grovedb::MerkStorage::LoadTree(&storage, parent_path, &parent_tree, &error)) {
        Fail("child_reopen parent load for mutation failed: " + error);
      }
      bool deleted = false;
      if (!parent_tree.Delete({'p', 'k', '1'}, &deleted, &error)) {
        Fail("child_reopen parent delete pk1 failed: " + error);
      }
      if (!deleted) {
        Fail("child_reopen parent delete pk1 should have deleted something");
      }
      if (!parent_tree.Insert({'p', 'k', '3'}, {'p', 'v', '3'}, &error)) {
        Fail("child_reopen parent insert pk3 failed: " + error);
      }
      if (!grovedb::MerkStorage::SaveTree(&storage, parent_path, &parent_tree, &error)) {
        Fail("child_reopen parent save after mutation failed: " + error);
      }
    }

    // Phase 4: Reopen child merk and verify it's still accessible with correct data
    // This validates that the child merk can be reopened independently after parent mutation
    {
      grovedb::MerkTree child_reopened;
      if (!grovedb::MerkStorage::LoadTree(&storage, child_path, &child_reopened, &error)) {
        Fail("child_reopen child reopen after parent mutation failed: " + error);
      }

      std::vector<uint8_t> val;
      // Verify child data is intact
      if (!child_reopened.Get({'c', 'k', '1'}, &val) ||
          val != std::vector<uint8_t>({'c', 'v', '1'})) {
        Fail("child_reopen ck1 should still be cv1 after parent mutation");
      }
      if (!child_reopened.Get({'c', 'k', '2'}, &val) ||
          val != std::vector<uint8_t>({'c', 'v', '2'})) {
        Fail("child_reopen ck2 should still be cv2 after parent mutation");
      }

      // Verify child can compute root hash (validates lazy-link traversal)
      std::vector<uint8_t> child_hash;
      if (!child_reopened.ComputeRootHash(grovedb::MerkTree::ValueHashFn(), &child_hash, &error)) {
        Fail("child_reopen child root hash computation failed after parent mutation: " + error);
      }

      // Verify expected child hash matches
      grovedb::MerkTree expected_child;
      std::vector<uint8_t> expected_hash;
      if (!expected_child.Insert({'c', 'k', '1'}, {'c', 'v', '1'}, &error) ||
          !expected_child.Insert({'c', 'k', '2'}, {'c', 'v', '2'}, &error)) {
        Fail("child_reopen expected child insert failed: " + error);
      }
      if (!expected_child.ComputeRootHash(grovedb::MerkTree::ValueHashFn(), &expected_hash,
                                          &error)) {
        Fail("child_reopen expected child hash computation failed: " + error);
      }
      if (child_hash != expected_hash) {
        Fail("child_reopen child root hash mismatch after parent mutation");
      }
    }

    // Phase 5: Verify parent mutation persisted correctly
    {
      grovedb::MerkTree parent_loaded;
      if (!grovedb::MerkStorage::LoadTree(&storage, parent_path, &parent_loaded, &error)) {
        Fail("child_reopen parent load for verification failed: " + error);
      }

      std::vector<uint8_t> val;
      // pk1 should be deleted
      if (parent_loaded.Get({'p', 'k', '1'}, &val)) {
        Fail("child_reopen pk1 should be deleted after parent mutation");
      }
      // pk2 should still exist
      if (!parent_loaded.Get({'p', 'k', '2'}, &val) ||
          val != std::vector<uint8_t>({'p', 'v', '2'})) {
        Fail("child_reopen pk2 should still be pv2 after parent mutation");
      }
      // pk3 should exist
      if (!parent_loaded.Get({'p', 'k', '3'}, &val) ||
          val != std::vector<uint8_t>({'p', 'v', '3'})) {
        Fail("child_reopen pk3 should be pv3 after parent mutation");
      }
    }
  }

  // Test A — corrupt stored link causes incremental export failure.
  {
    const std::vector<std::vector<uint8_t>> a_path = {{'a', '_', 'p'}};
    grovedb::MerkTree a_tree;
    for (char c = 'a'; c <= 'e'; ++c) {
      if (!a_tree.Insert({static_cast<uint8_t>(c)},
                          {static_cast<uint8_t>(c + 1)},
                          &error)) {
        Fail("a_tree insert failed: " + error);
      }
    }
    if (!grovedb::MerkStorage::SaveTree(&storage, a_path, &a_tree, &error)) {
      Fail("a save tree failed: " + error);
    }

    CorruptStoredRootChildKeyToEmpty(&storage, a_path, &a_tree, &error);

    // Load tree lazily, insert a value to dirty the root.
    grovedb::MerkTree a_loaded;
    if (!grovedb::MerkStorage::LoadTree(&storage, a_path, &a_loaded, &error)) {
      Fail("a load corrupt tree failed: " + error);
    }
    if (!a_loaded.Insert({'z'}, {'z'}, &error)) {
      Fail("a insert into corrupt tree failed: " + error);
    }

    // ExportEncodedNodesForKeys should fail.
    std::vector<std::vector<uint8_t>> a_dirty;
    a_loaded.SnapshotDirtyKeys(&a_dirty);
    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> a_exported;
    std::vector<uint8_t> a_exp_root;
    error.clear();
    if (a_loaded.ExportEncodedNodesForKeys(a_dirty,
                                           &a_exported,
                                           &a_exp_root,
                                           grovedb::MerkTree::ValueHashFn(),
                                           &error)) {
      Fail("a export should fail on corrupt stored link");
    }
    if (error.find("child meta is present but has empty key") == std::string::npos) {
      Fail("a unexpected export error: " + error);
    }
  }

  // Test B — Lazy traversal on corrupted storage fails.
  {
    const std::vector<std::vector<uint8_t>> b_path = {{'b', '_', 'p', 'a', 't', 'h'}};
    grovedb::MerkTree b_tree;
    for (char c = 'a'; c <= 'e'; ++c) {
      if (!b_tree.Insert({static_cast<uint8_t>(c)},
                         {static_cast<uint8_t>(c + 1)},
                         &error)) {
        Fail("b_tree insert failed: " + error);
      }
    }
    if (!grovedb::MerkStorage::SaveTree(&storage, b_path, &b_tree, &error)) {
      Fail("b save tree failed: " + error);
    }

    CorruptStoredRootChildKeyToEmpty(&storage, b_path, &b_tree, &error);

    // Load tree lazily.  Do NOT call EnsureFullyLoaded; instead exercise
    // the lazy traversal path that goes through EnsureChildLoaded.
    grovedb::MerkTree b_loaded;
    if (!grovedb::MerkStorage::LoadTree(&storage, b_path, &b_loaded, &error)) {
      Fail("b load corrupt tree failed: " + error);
    }
    // Determine which child side is corrupt so we know which key to query.
    // If left child was corrupted, querying any key less than root should
    // trigger EnsureChildLoaded on the left side.  We use GetNodeMeta
    // which also goes through EnsureChildLoaded.
    std::vector<uint8_t> b_root_key2;
    if (!b_loaded.RootKey(&b_root_key2) || b_root_key2.empty()) {
      Fail("b missing root key for traversal");
    }
    // Query a key that requires traversing the corrupt subtree.
    // For a 5-key tree {a..e}, the root is typically 'b' or 'c'.
    // Querying 'a' will try left subtree if root > 'a'.
    error.clear();
    grovedb::MerkTree::NodeMeta b_meta;
    if (b_loaded.GetNodeMeta({'a'}, &b_meta, &error)) {
      // If 'a' happened to be root, try 'e' which goes right.
      error.clear();
      if (b_loaded.GetNodeMeta({'e'}, &b_meta, &error)) {
        Fail("b lazy traversal should fail on at least one side of corrupt storage");
      }
    }
    if (error.find("child meta is present but has empty key") == std::string::npos) {
      Fail("b unexpected lazy traversal error: " + error);
    }
  }

  // Test C — Non-regression: ordinary lazy-loaded export succeeds.
  // Build tree, save, reload lazily, insert new key, save again (incremental),
  // then reload and verify all original + new keys are reachable.
  {
    const std::vector<std::vector<uint8_t>> c_path = {{'c', '_', 'n', 'r'}};
    grovedb::MerkTree c_tree;
    for (char c = 'a'; c <= 'e'; ++c) {
      if (!c_tree.Insert({static_cast<uint8_t>(c)},
                         {static_cast<uint8_t>(c + 1)},
                         &error)) {
        Fail("c_tree insert failed: " + error);
      }
    }
    if (!grovedb::MerkStorage::SaveTree(&storage, c_path, &c_tree, &error)) {
      Fail("c save tree failed: " + error);
    }

    // Load lazily (don't call EnsureFullyLoaded).
    grovedb::MerkTree c_loaded;
    if (!grovedb::MerkStorage::LoadTree(&storage, c_path, &c_loaded, &error)) {
      Fail("c load tree failed: " + error);
    }
    if (!c_loaded.IsLazyLoading()) {
      Fail("c_loaded should be lazy");
    }

    // Insert a new key that dirties the root but leaves children unloaded.
    if (!c_loaded.Insert({'f'}, {'g'}, &error)) {
      Fail("c insert failed: " + error);
    }
    for (char c = 'a'; c <= 'f'; ++c) {
      std::vector<uint8_t> val;
      if (!c_loaded.Get({static_cast<uint8_t>(c)}, &val)) {
        Fail(std::string("c loaded missing key before save '") + c + "'");
      }
    }

    // Incremental save (uses ExportEncodedNodesForKeys internally).
    if (!grovedb::MerkStorage::SaveTree(&storage, c_path, &c_loaded, &error)) {
      Fail("c incremental save failed: " + error);
    }

    // Reload and verify all original + new keys are present.
    grovedb::MerkTree c_reloaded;
    if (!grovedb::MerkStorage::LoadTree(&storage, c_path, &c_reloaded, &error)) {
      Fail("c reload failed: " + error);
    }
    std::string present_keys;
    for (char c = 'a'; c <= 'f'; ++c) {
      std::vector<uint8_t> val;
      if (!c_reloaded.Get({static_cast<uint8_t>(c)}, &val)) {
        std::vector<uint8_t> root_key;
        if (c_reloaded.RootKey(&root_key) && !root_key.empty()) {
          present_keys += " root=" + std::string(root_key.begin(), root_key.end());
        }
        Fail(std::string("c reloaded missing key '") + c + "'" + present_keys);
      }
      present_keys.push_back(c);
      if (val != std::vector<uint8_t>({static_cast<uint8_t>(c + 1)})) {
        Fail(std::string("c reloaded wrong value for '") + c + "'");
      }
    }
  }

  std::filesystem::remove_all(dir);
  return 0;
}
