#include "merk_storage.h"

#include <cstdlib>
#include <iostream>
#include <unordered_set>

#include "element.h"
#include "insert_profile.h"
#include "merk_costs.h"

namespace grovedb {

namespace {
bool DebugRootPathLoadEnabled() {
  const char* env = std::getenv("GROVEDB_DEBUG_ROOT_PATH_LOAD");
  return env != nullptr && env[0] != '\0' && env[0] != '0';
}

bool IsSingleRootPath(const std::vector<std::vector<uint8_t>>& path) {
  return path.size() == 1 && path[0] == std::vector<uint8_t>({'r', 'o', 'o', 't'});
}

bool IsTreeElementVariantForFallback(uint64_t variant) {
  return variant == 2 || variant == 4 || variant == 5 || variant == 6 || variant == 7 ||
         variant == 8 || variant == 10;
}

bool DecodeEmbeddedTreeRootKey(const std::vector<uint8_t>& element_bytes,
                               std::vector<uint8_t>* out_root_key,
                               std::string* error) {
  if (out_root_key == nullptr) {
    if (error) {
      *error = "embedded root key output is null";
    }
    return false;
  }
  out_root_key->clear();

  uint64_t variant = 0;
  if (!DecodeElementVariant(element_bytes, &variant, error)) {
    return false;
  }
  if (!IsTreeElementVariantForFallback(variant)) {
    if (error) {
      *error = "path element is not a tree";
    }
    return false;
  }

  switch (variant) {
    case 2: {
      ElementTree v;
      if (!DecodeTreeFromElementBytes(element_bytes, &v, error)) return false;
      if (v.root_key.has_value()) *out_root_key = *v.root_key;
      return true;
    }
    case 4: {
      ElementSumTree v;
      if (!DecodeSumTreeFromElementBytes(element_bytes, &v, error)) return false;
      if (v.root_key.has_value()) *out_root_key = *v.root_key;
      return true;
    }
    case 5: {
      ElementBigSumTree v;
      if (!DecodeBigSumTreeFromElementBytes(element_bytes, &v, error)) return false;
      if (v.root_key.has_value()) *out_root_key = *v.root_key;
      return true;
    }
    case 6: {
      ElementCountTree v;
      if (!DecodeCountTreeFromElementBytes(element_bytes, &v, error)) return false;
      if (v.root_key.has_value()) *out_root_key = *v.root_key;
      return true;
    }
    case 7: {
      ElementCountSumTree v;
      if (!DecodeCountSumTreeFromElementBytes(element_bytes, &v, error)) return false;
      if (v.root_key.has_value()) *out_root_key = *v.root_key;
      return true;
    }
    case 8: {
      ElementProvableCountTree v;
      if (!DecodeProvableCountTreeFromElementBytes(element_bytes, &v, error)) return false;
      if (v.root_key.has_value()) *out_root_key = *v.root_key;
      return true;
    }
    case 10: {
      ElementProvableCountSumTree v;
      if (!DecodeProvableCountSumTreeFromElementBytes(element_bytes, &v, error)) return false;
      if (v.root_key.has_value()) *out_root_key = *v.root_key;
      return true;
    }
    default:
      if (error) {
        *error = "unsupported tree element variant";
      }
      return false;
  }
}

bool ReadSubtreeRootKeyFallback(RocksDbWrapper* storage,
                                RocksDbWrapper::Transaction* transaction,
                                const std::vector<std::vector<uint8_t>>& path,
                                std::vector<uint8_t>* root_key,
                                bool* found,
                                std::string* error) {
  if (root_key == nullptr || found == nullptr) {
    if (error) {
      *error = "fallback root key output is null";
    }
    return false;
  }
  root_key->clear();
  *found = false;
  if (path.empty()) {
    return true;
  }

  std::vector<std::vector<uint8_t>> parent_path(path.begin(), path.end() - 1);
  MerkTree parent;
  if (!MerkStorage::LoadTree(storage, transaction, parent_path, &parent, error)) {
    return false;
  }

  std::vector<uint8_t> parent_element;
  if (!parent.Get(path.back(), &parent_element)) {
    return true;
  }
  if (!DecodeEmbeddedTreeRootKey(parent_element, root_key, error)) {
    return false;
  }
  *found = true;
  return true;
}
}  // namespace

bool MerkStorage::SaveTree(RocksDbWrapper* storage,
                           const std::vector<std::vector<uint8_t>>& path,
                           MerkTree* tree,
                           std::string* error) {
  return SaveTree(storage, nullptr, path, tree, nullptr, error);
}

bool MerkStorage::SaveTree(RocksDbWrapper* storage,
                           const std::vector<std::vector<uint8_t>>& path,
                           MerkTree* tree,
                           OperationCost* cost,
                           std::string* error) {
  return SaveTree(storage, nullptr, path, tree, cost, error);
}

bool MerkStorage::SaveTree(RocksDbWrapper* storage,
                           RocksDbWrapper::Transaction* transaction,
                           const std::vector<std::vector<uint8_t>>& path,
                           MerkTree* tree,
                           std::string* error) {
  return SaveTree(storage, transaction, path, tree, nullptr, error);
}

bool MerkStorage::SaveTree(RocksDbWrapper* storage,
                           RocksDbWrapper::Transaction* transaction,
                           const std::vector<std::vector<uint8_t>>& path,
                           MerkTree* tree,
                           OperationCost* cost,
                           std::string* error) {
  if (storage == nullptr || tree == nullptr) {
    if (error) {
      *error = "storage or tree is null";
    }
    return false;
  }
  std::vector<uint8_t> root_key;
  const std::vector<uint8_t> root_key_key = {'r'};
  std::vector<uint8_t> current_root;
  bool has_root = tree->RootKey(&current_root);
  if (!has_root || current_root.empty()) {
    std::vector<std::vector<uint8_t>> ignored_dirty_keys;
    std::vector<std::vector<uint8_t>> ignored_deleted_keys;
    tree->ConsumeDirtyKeys(&ignored_dirty_keys);
    tree->ConsumeDeletedKeys(&ignored_deleted_keys);
    if (transaction != nullptr) {
      if (!transaction->DeletePrefix(ColumnFamilyKind::kDefault, path, error)) {
        return false;
      }
      if (!transaction->DeletePrefix(ColumnFamilyKind::kRoots, path, error)) {
        return false;
      }
    } else {
      if (!storage->DeletePrefix(ColumnFamilyKind::kDefault, path, error)) {
        return false;
      }
      if (!storage->DeletePrefix(ColumnFamilyKind::kRoots, path, error)) {
        return false;
      }
    }
    if (cost != nullptr) {
      // Prefix-delete cost accounting is not exposed in RocksDbWrapper today.
      // Keep storage byte accounting conservative and only mark hash work for prefix derivation.
      cost->hash_node_calls += 2;
    }
    tree->MarkPersistedRootKey(std::vector<uint8_t>());
    if (transaction != nullptr) {
      tree->AttachStorage(storage, transaction, path, ColumnFamilyKind::kDefault);
    } else {
      tree->AttachStorage(storage, path, ColumnFamilyKind::kDefault);
    }
    return true;
  }
  root_key = std::move(current_root);

  std::vector<std::vector<uint8_t>> dirty_keys;
  std::vector<std::vector<uint8_t>> deleted_keys;
  tree->SnapshotDirtyKeys(&dirty_keys);
  tree->SnapshotDeletedKeys(&deleted_keys);
  insert_profile::AddCounter(insert_profile::Counter::kDirtyKeyCount,
                             static_cast<uint64_t>(dirty_keys.size()));

  std::unordered_set<std::string> dirty_set;
  dirty_set.reserve(dirty_keys.size());
  for (const auto& key : dirty_keys) {
    dirty_set.insert(std::string(reinterpret_cast<const char*>(key.data()), key.size()));
  }

  std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> entries;
  if (!dirty_keys.empty()) {
    {
      insert_profile::ScopedStage export_scope(insert_profile::Stage::kExportDirtyNodes);
      if (!tree->ExportEncodedNodesForKeys(dirty_keys,
                                           &entries,
                                           &root_key,
                                           tree->GetValueHashFn(),
                                           error)) {
        if (error && error->empty()) {
          *error = "failed to export encoded tree";
        }
        return false;
      }
    }
  }
  insert_profile::AddCounter(insert_profile::Counter::kExportedEntryCount,
                             static_cast<uint64_t>(entries.size()));

  RocksDbWrapper::WriteBatch data_batch;
  for (const auto& key : deleted_keys) {
    std::string key_str(reinterpret_cast<const char*>(key.data()), key.size());
    if (dirty_set.find(key_str) != dirty_set.end()) {
      continue;
    }
    data_batch.Delete(ColumnFamilyKind::kDefault, path, key);
  }
  for (const auto& kv : entries) {
    data_batch.Put(ColumnFamilyKind::kDefault, path, kv.first, kv.second);
  }
  RocksDbWrapper::WriteBatch roots_batch;
  bool write_roots_entry = !tree->InitialRootKeyEqualsCurrent();
  if (write_roots_entry) {
    roots_batch.Put(ColumnFamilyKind::kRoots, path, root_key_key, root_key);
  }
  RocksDbWrapper::WriteBatch full_batch = data_batch;
  if (write_roots_entry) {
    full_batch.Append(roots_batch);
  }

  if (cost != nullptr) {
    if (!storage->CommitBatchWithCost(full_batch, transaction, cost, error)) {
      return false;
    }
  } else {
    if (!storage->CommitBatch(full_batch, transaction, error)) {
      return false;
    }
  }
  tree->AcknowledgeDirtyKeys(dirty_keys);
  tree->AcknowledgeDeletedKeys(deleted_keys);
  tree->MarkPersistedRootKey(root_key);
  // Attach storage context so the tree can lazy-load children on demand after
  // save, matching Rust's post-commit behavior where the merk retains its
  // storage backend reference.
  if (transaction != nullptr) {
    tree->AttachStorage(storage, transaction, path, ColumnFamilyKind::kDefault);
  } else {
    tree->AttachStorage(storage, path, ColumnFamilyKind::kDefault);
  }
  return true;
}

bool MerkStorage::SaveTreeToBatch(RocksDbWrapper* storage,
                                  RocksDbWrapper::Transaction* transaction,
                                  const std::vector<std::vector<uint8_t>>& path,
                                  MerkTree* tree,
                                  RocksDbWrapper::WriteBatch* batch,
                                  std::string* error) {
  if (storage == nullptr || transaction == nullptr || tree == nullptr || batch == nullptr) {
    if (error) {
      *error = "storage, transaction, tree, or batch is null";
    }
    return false;
  }

  const std::vector<uint8_t> root_key_key = {'r'};
  std::vector<uint8_t> root_key;
  std::vector<uint8_t> current_root;
  bool has_root = tree->RootKey(&current_root);
  if (!has_root || current_root.empty()) {
    std::vector<std::vector<uint8_t>> ignored_dirty_keys;
    std::vector<std::vector<uint8_t>> ignored_deleted_keys;
    tree->ConsumeDirtyKeys(&ignored_dirty_keys);
    tree->ConsumeDeletedKeys(&ignored_deleted_keys);
    batch->DeletePrefix(ColumnFamilyKind::kDefault, path);
    batch->DeletePrefix(ColumnFamilyKind::kRoots, path);
    tree->MarkPersistedRootKey(std::vector<uint8_t>());
    tree->AttachStorage(storage, transaction, path, ColumnFamilyKind::kDefault);
    return true;
  }
  root_key = std::move(current_root);

  std::vector<std::vector<uint8_t>> dirty_keys;
  std::vector<std::vector<uint8_t>> deleted_keys;
  tree->SnapshotDirtyKeys(&dirty_keys);
  tree->SnapshotDeletedKeys(&deleted_keys);
  insert_profile::AddCounter(insert_profile::Counter::kDirtyKeyCount,
                             static_cast<uint64_t>(dirty_keys.size()));

  std::unordered_set<std::string> dirty_set;
  dirty_set.reserve(dirty_keys.size());
  for (const auto& key : dirty_keys) {
    dirty_set.insert(std::string(reinterpret_cast<const char*>(key.data()), key.size()));
  }

  std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> entries;
  if (!dirty_keys.empty()) {
    {
      insert_profile::ScopedStage export_scope(insert_profile::Stage::kExportDirtyNodes);
      if (!tree->ExportEncodedNodesForKeys(
              dirty_keys, &entries, &root_key, tree->GetValueHashFn(), error)) {
        if (error && error->empty()) {
          *error = "failed to export encoded tree";
        }
        return false;
      }
    }
  }
  insert_profile::AddCounter(insert_profile::Counter::kExportedEntryCount,
                             static_cast<uint64_t>(entries.size()));

  for (const auto& key : deleted_keys) {
    std::string key_str(reinterpret_cast<const char*>(key.data()), key.size());
    if (dirty_set.find(key_str) != dirty_set.end()) {
      continue;
    }
    batch->Delete(ColumnFamilyKind::kDefault, path, key);
  }
  for (const auto& kv : entries) {
    batch->Put(ColumnFamilyKind::kDefault, path, kv.first, kv.second);
  }
  if (!tree->InitialRootKeyEqualsCurrent()) {
    batch->Put(ColumnFamilyKind::kRoots, path, root_key_key, root_key);
  }
  tree->AcknowledgeDirtyKeys(dirty_keys);
  tree->AcknowledgeDeletedKeys(deleted_keys);
  tree->MarkPersistedRootKey(root_key);
  // Keep a transactional storage context attached after batch-save so the
  // same merk instance can continue lazy-load traversal without reopen. Keep
  // base storage as a fallback for post-commit lazy traversal.
  tree->AttachStorage(storage, transaction, path, ColumnFamilyKind::kDefault);
  return true;
}

bool MerkStorage::LoadTree(RocksDbWrapper* storage,
                           const std::vector<std::vector<uint8_t>>& path,
                           MerkTree* tree,
                           std::string* error) {
  return LoadTree(storage, nullptr, path, tree, nullptr, error);
}

bool MerkStorage::LoadTree(RocksDbWrapper* storage,
                           const std::vector<std::vector<uint8_t>>& path,
                           MerkTree* tree,
                           OperationCost* cost,
                           std::string* error) {
  return LoadTree(storage, nullptr, path, tree, cost, error);
}

bool MerkStorage::LoadTree(RocksDbWrapper* storage,
                           RocksDbWrapper::Transaction* transaction,
                           const std::vector<std::vector<uint8_t>>& path,
                           MerkTree* tree,
                           std::string* error) {
  return LoadTree(storage, transaction, path, tree, nullptr, error);
}

bool MerkStorage::LoadTree(RocksDbWrapper* storage,
                           RocksDbWrapper::Transaction* transaction,
                           const std::vector<std::vector<uint8_t>>& path,
                           MerkTree* tree,
                           OperationCost* cost,
                           std::string* error) {
  if (storage == nullptr || tree == nullptr) {
    if (error) {
      *error = "storage or tree is null";
    }
    return false;
  }
  *tree = MerkTree();
  const std::vector<uint8_t> root_key_key = {'r'};
  std::vector<uint8_t> root_key;
  bool found = false;
  if (transaction != nullptr) {
    if (!transaction->Get(ColumnFamilyKind::kRoots, path, root_key_key, &root_key, &found, error)) {
      return false;
    }
  } else {
    if (!storage->Get(ColumnFamilyKind::kRoots, path, root_key_key, &root_key, &found, error)) {
      return false;
    }
  }
  if (cost != nullptr) {
    cost->seek_count += 1;
    if (found) {
      cost->storage_loaded_bytes += static_cast<uint64_t>(root_key.size());
    }
  }
  if (DebugRootPathLoadEnabled() && IsSingleRootPath(path)) {
    std::cerr << "LOADTREE path=root roots_found=" << (found ? "1" : "0")
              << " roots_key_len=" << root_key.size();
    if (!root_key.empty()) {
      std::cerr << " roots_key='"
                << std::string(root_key.begin(), root_key.end()) << "'";
    }
    std::cerr << "\n";
  }
  if (!found && !path.empty()) {
    if (!ReadSubtreeRootKeyFallback(storage, transaction, path, &root_key, &found, error)) {
      return false;
    }
  }
  if (!found || root_key.empty()) {
    if (cost != nullptr && !path.empty()) {
      const uint32_t subtree_key_len = static_cast<uint32_t>(path.back().size());
      cost->storage_loaded_bytes += LayeredValueByteCostSizeForKeyAndValueLengths(
          subtree_key_len, 3, TreeFeatureTypeTag::kBasic);
    }
    return true;
  }
  if (cost != nullptr) {
    std::vector<uint8_t> root_node;
    bool root_found = false;
    if (transaction != nullptr) {
      if (!transaction->Get(
              ColumnFamilyKind::kDefault, path, root_key, &root_node, &root_found, error)) {
        return false;
      }
    } else {
      if (!storage->Get(ColumnFamilyKind::kDefault, path, root_key, &root_node, &root_found, error)) {
        return false;
      }
    }
    cost->seek_count += 1;
    if (root_found) {
      cost->storage_loaded_bytes += static_cast<uint64_t>(root_node.size());
    }
  }
  if (transaction != nullptr) {
    return tree->LoadEncodedTree(
        transaction, path, root_key, ColumnFamilyKind::kDefault, true, error);
  }
  return tree->LoadEncodedTree(storage, path, root_key, ColumnFamilyKind::kDefault, true, error);
}

bool MerkStorage::ClearTree(RocksDbWrapper* storage,
                            const std::vector<std::vector<uint8_t>>& path,
                            std::string* error) {
  if (storage == nullptr) {
    if (error) {
      *error = "storage is null";
    }
    return false;
  }
  if (!storage->DeletePrefix(ColumnFamilyKind::kDefault, path, error)) {
    return false;
  }
  if (!storage->DeletePrefix(ColumnFamilyKind::kRoots, path, error)) {
    return false;
  }
  return true;
}

}  // namespace grovedb
