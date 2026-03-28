#include "grovedb.h"

#include "binary.h"
#include "element.h"
#include "hash.h"
#include "insert_profile.h"
#include "merk_costs.h"
#include "proof.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <chrono>
#include <optional>

namespace grovedb {

namespace {
constexpr const char* kVisualizerDivergenceExpiryDate = "2026-06-30";
constexpr const char* kVisualizerDivergenceGapId = "visualizer-hook-parity-decision";
constexpr size_t kMaxReferenceHops = 10;

std::string EncodePathCacheKey(const std::vector<std::vector<uint8_t>>& path) {
  std::string encoded;
  size_t reserve_size = 0;
  for (const auto& component : path) {
    reserve_size += 4 + component.size();
  }
  encoded.reserve(reserve_size);
  for (const auto& component : path) {
    const uint32_t len = static_cast<uint32_t>(component.size());
    encoded.push_back(static_cast<char>((len >> 24) & 0xFF));
    encoded.push_back(static_cast<char>((len >> 16) & 0xFF));
    encoded.push_back(static_cast<char>((len >> 8) & 0xFF));
    encoded.push_back(static_cast<char>(len & 0xFF));
    encoded.append(reinterpret_cast<const char*>(component.data()), component.size());
  }
  return encoded;
}

bool FacadeCostDebugEnabled() {
  const char* env = std::getenv("GROVEDB_DEBUG_FACADE_COST");
  return env != nullptr && env[0] != '\0' && env[0] != '0';
}

bool TransactionPoisoned(const GroveDb::Transaction* tx) {
  return tx != nullptr && tx->inner.IsPoisoned();
}

bool EnsureTransactionUsable(const GroveDb::Transaction* tx, std::string* error) {
  if (!TransactionPoisoned(tx)) {
    return true;
  }
  if (error) {
    *error = "transaction is poisoned";
  }
  return false;
}

void InvalidateTransactionInsertCache(GroveDb::Transaction* tx) {
  if (tx == nullptr) {
    return;
  }
  tx->validated_tree_paths.clear();
  tx->insert_cache_path_key.clear();
  if (tx->insert_cache != nullptr) {
    tx->insert_cache->clear();
  }
}

void PoisonTransaction(GroveDb::Transaction* tx) {
  if (tx == nullptr) {
    return;
  }
  tx->inner.Poison();
  InvalidateTransactionInsertCache(tx);
}

void DebugPrintCostStage(const char* op,
                         const char* stage,
                         const OperationCost& cost) {
  if (!FacadeCostDebugEnabled()) {
    return;
  }
  std::cerr << "FACADE_COST\t" << op << "\t" << stage << "\tseek=" << cost.seek_count
            << "\tloaded=" << cost.storage_loaded_bytes << "\thash=" << cost.hash_node_calls
            << "\tadd=" << cost.storage_cost.added_bytes
            << "\treplace=" << cost.storage_cost.replaced_bytes
            << "\tremove=" << cost.storage_cost.removed_bytes.TotalRemovedBytes() << "\n";
}

bool TreeElementHasEmbeddedRootKey(const std::vector<uint8_t>& element_bytes,
                                   bool* has_root_key,
                                   std::string* error) {
  if (has_root_key == nullptr) {
    if (error) {
      *error = "has_root_key output is null";
    }
    return false;
  }
  *has_root_key = false;
  uint64_t variant = 0;
  if (!DecodeElementVariant(element_bytes, &variant, error)) {
    return false;
  }
  switch (variant) {
    case 2: {
      ElementTree v;
      if (!DecodeTreeFromElementBytes(element_bytes, &v, error)) return false;
      *has_root_key = v.root_key.has_value() && !v.root_key->empty();
      return true;
    }
    case 4: {
      ElementSumTree v;
      if (!DecodeSumTreeFromElementBytes(element_bytes, &v, error)) return false;
      *has_root_key = v.root_key.has_value() && !v.root_key->empty();
      return true;
    }
    case 5: {
      ElementBigSumTree v;
      if (!DecodeBigSumTreeFromElementBytes(element_bytes, &v, error)) return false;
      *has_root_key = v.root_key.has_value() && !v.root_key->empty();
      return true;
    }
    case 6: {
      ElementCountTree v;
      if (!DecodeCountTreeFromElementBytes(element_bytes, &v, error)) return false;
      *has_root_key = v.root_key.has_value() && !v.root_key->empty();
      return true;
    }
    case 7: {
      ElementCountSumTree v;
      if (!DecodeCountSumTreeFromElementBytes(element_bytes, &v, error)) return false;
      *has_root_key = v.root_key.has_value() && !v.root_key->empty();
      return true;
    }
    case 8: {
      ElementProvableCountTree v;
      if (!DecodeProvableCountTreeFromElementBytes(element_bytes, &v, error)) return false;
      *has_root_key = v.root_key.has_value() && !v.root_key->empty();
      return true;
    }
    case 10: {
      ElementProvableCountSumTree v;
      if (!DecodeProvableCountSumTreeFromElementBytes(element_bytes, &v, error)) return false;
      *has_root_key = v.root_key.has_value() && !v.root_key->empty();
      return true;
    }
    default:
      if (error) {
        *error = "path element is not a tree";
      }
      return false;
  }
}

bool ConfigureLoadedMerkTree(RocksDbWrapper* storage,
                             RocksDbWrapper::Transaction* tx,
                             const std::vector<std::vector<uint8_t>>& path,
                             MerkTree* tree,
                             std::string* error) {
  if (tree == nullptr) {
    if (error) {
      *error = "tree is null";
    }
    return false;
  }
  tree->SetValueDefinedCostFn(
      [](const std::vector<uint8_t>& element_bytes,
         std::optional<ValueDefinedCostType>* out,
         std::string* err) -> bool {
        return ValueDefinedCostForSerializedElement(element_bytes, out, err);
      });
  tree->SetValueHashFn(
      [storage, tx, path](const std::vector<uint8_t>& key,
                          const std::vector<uint8_t>& value,
                          std::vector<uint8_t>* out,
                          std::string* err) -> bool {
        insert_profile::AddCounter(insert_profile::Counter::kTreeElementHashCalls);
        std::vector<uint8_t> value_hash;
        if (!ValueHash(value, &value_hash, err)) {
          return false;
        }
        uint64_t variant = 0;
        if (!DecodeElementVariant(value, &variant, nullptr)) {
          *out = std::move(value_hash);
          return true;
        }
        const bool is_tree_variant =
            (variant == 2 || variant == 4 || variant == 5 || variant == 6 ||
             variant == 7 || variant == 8 || variant == 10);
        if (!is_tree_variant) {
          *out = std::move(value_hash);
          return true;
        }
        bool has_child_root_key = false;
        if (!TreeElementHasEmbeddedRootKey(value, &has_child_root_key, err)) {
          return false;
        }
        std::vector<uint8_t> child_root_hash;
        if (!has_child_root_key) {
          MerkTree empty_child_tree;
          if (!empty_child_tree.ComputeRootHash(empty_child_tree.GetValueHashFn(), &child_root_hash, err)) {
            return false;
          }
          return CombineHash(value_hash, child_root_hash, out, err);
        }
        std::vector<std::vector<uint8_t>> child_path = path;
        child_path.push_back(key);
        MerkTree child_tree;
        bool ok = false;
        insert_profile::AddCounter(insert_profile::Counter::kChildLoadTreeHashCalls);
        if (tx != nullptr) {
          ok = MerkStorage::LoadTree(storage, tx, child_path, &child_tree, err);
        } else {
          ok = MerkStorage::LoadTree(storage, child_path, &child_tree, err);
        }
        if (!ok) {
          return false;
        }
        if (!ConfigureLoadedMerkTree(storage, tx, child_path, &child_tree, err)) {
          return false;
        }
        insert_profile::AddCounter(insert_profile::Counter::kChildComputeRootHashCalls);
        if (!child_tree.ComputeRootHash(child_tree.GetValueHashFn(), &child_root_hash, err)) {
          return false;
        }
        return CombineHash(value_hash, child_root_hash, out, err);
      });
  return true;
}

bool GetOrLoadConfiguredMerk(RocksDbWrapper* storage,
                             RocksDbWrapper::Transaction* tx,
                             const std::vector<std::vector<uint8_t>>& path,
                             MerkCache* cache,
                             MerkTree** out,
                             std::string* error) {
  if (cache == nullptr || out == nullptr) {
    if (error) {
      *error = "cache load inputs are null";
    }
    return false;
  }
  const bool cache_hit = cache->contains(path);
  insert_profile::AddCounter(cache_hit ? insert_profile::Counter::kMerkCacheHits
                                       : insert_profile::Counter::kMerkCacheMisses);
  if (!cache->getOrLoad(path, tx, out, error)) {
    return false;
  }
  (*out)->AttachStorage(storage, tx, path, ColumnFamilyKind::kDefault);
  return ConfigureLoadedMerkTree(storage, tx, path, *out, error);
}

MerkTree::ValueHashFn MakeValueHashFnWithChildRootOverride(
    const MerkTree::ValueHashFn& fallback,
    const std::vector<uint8_t>& child_key,
    const std::vector<uint8_t>& child_root_hash) {
  return [fallback, child_key, child_root_hash](const std::vector<uint8_t>& key,
                                                const std::vector<uint8_t>& value,
                                                std::vector<uint8_t>* out,
                                                std::string* err) -> bool {
    if (key != child_key) {
      insert_profile::AddCounter(insert_profile::Counter::kChildRootOverrideFallbackHits);
      return fallback(key, value, out, err);
    }
    insert_profile::AddCounter(insert_profile::Counter::kChildRootOverrideHits);
    std::vector<uint8_t> value_hash;
    if (!ValueHash(value, &value_hash, err)) {
      return false;
    }
    uint64_t variant = 0;
    if (!DecodeElementVariant(value, &variant, nullptr)) {
      *out = std::move(value_hash);
      return true;
    }
    const bool is_tree_variant =
        (variant == 2 || variant == 4 || variant == 5 || variant == 6 ||
         variant == 7 || variant == 8 || variant == 10);
    if (!is_tree_variant) {
      *out = std::move(value_hash);
      return true;
    }
    bool has_child_root_key = false;
    if (!TreeElementHasEmbeddedRootKey(value, &has_child_root_key, err)) {
      return false;
    }
    if (!has_child_root_key) {
      return fallback(key, value, out, err);
    }
    return CombineHash(value_hash, child_root_hash, out, err);
  };
}

bool LoadTreeForPath(RocksDbWrapper* storage,
                     RocksDbWrapper::Transaction* tx,
                     const std::vector<std::vector<uint8_t>>& path,
                     MerkTree* tree,
                     OperationCost* cost,
                     MerkCache* cache,
                     std::string* error) {
  if (cache != nullptr) {
    MerkTree* cached_tree = nullptr;
    if (GetOrLoadConfiguredMerk(storage, tx, path, cache, &cached_tree, error)) {
      *tree = std::move(*cached_tree);
      return true;
    }
    return false;
  }
  bool ok = false;
  if (tx != nullptr) {
    ok = MerkStorage::LoadTree(storage, tx, path, tree, cost, error);
  } else {
    ok = MerkStorage::LoadTree(storage, path, tree, cost, error);
  }
  if (!ok) {
    return false;
  }
  return ConfigureLoadedMerkTree(storage, tx, path, tree, error);
}

bool LoadTreeForPath(RocksDbWrapper* storage,
                     RocksDbWrapper::Transaction* tx,
                     const std::vector<std::vector<uint8_t>>& path,
                     MerkTree* tree,
                     OperationCost* cost,
                     std::string* error) {
  return LoadTreeForPath(storage, tx, path, tree, cost, nullptr, error);
}

bool GetPropagatedAggregatesForTree(const MerkTree& tree,
                                    bool needs_count,
                                    bool needs_sum,
                                    uint64_t* out_count,
                                    __int128* out_sum,
                                    const MerkTree::RootAggregate* precomputed_agg,
                                    std::string* error) {
  MerkTree::RootAggregate agg_storage;
  const MerkTree::RootAggregate* agg = precomputed_agg;
  if (agg == nullptr) {
    if (!tree.RootAggregateData(&agg_storage, error)) {
      return false;
    }
    agg = &agg_storage;
  }
  if (needs_count) {
    if (out_count == nullptr) {
      if (error) {
        *error = "count output is null";
      }
      return false;
    }
    if (!agg->has_count) {
      if (!tree.ComputeCount(out_count, error)) {
        return false;
      }
    } else {
      *out_count = agg->count;
    }
  }
  if (needs_sum) {
    if (out_sum == nullptr) {
      if (error) {
        *error = "sum output is null";
      }
      return false;
    }
    if (!agg->has_sum) {
      auto sum_value_fn = [](const std::vector<uint8_t>& value,
                             int64_t* sum,
                             bool* has_sum,
                             std::string* err) -> bool {
        return ExtractSumValueFromElementBytes(value, sum, has_sum, err);
      };
      if (!tree.ComputeSumBig(sum_value_fn, out_sum, error)) {
        return false;
      }
    } else {
      *out_sum = agg->sum;
    }
  }
  return true;
}

bool GetPropagatedAggregatesForTree(const MerkTree& tree,
                                    bool needs_count,
                                    bool needs_sum,
                                    uint64_t* out_count,
                                    __int128* out_sum,
                                    std::string* error) {
  return GetPropagatedAggregatesForTree(
      tree, needs_count, needs_sum, out_count, out_sum, nullptr, error);
}

bool SaveTreeForPath(RocksDbWrapper* storage,
                     RocksDbWrapper::Transaction* tx,
                     const std::vector<std::vector<uint8_t>>& path,
                     MerkTree* tree,
                     RocksDbWrapper::WriteBatch* batch,
                     OperationCost* cost,
                     std::string* error) {
  if (batch != nullptr) {
    if (tx == nullptr) {
      if (error) {
        *error = "batch save requires transaction";
      }
      return false;
    }
    if (cost != nullptr) {
      if (error) {
        *error = "batch save with cost is unsupported";
      }
      return false;
    }
    return MerkStorage::SaveTreeToBatch(storage, tx, path, tree, batch, error);
  }
  if (tx != nullptr) {
    return MerkStorage::SaveTree(storage, tx, path, tree, cost, error);
  }
  return MerkStorage::SaveTree(storage, path, tree, cost, error);
}

bool IsInsertLikeBatchOpKind(GroveDb::BatchOp::Kind kind) {
  return kind == GroveDb::BatchOp::Kind::kInsert || kind == GroveDb::BatchOp::Kind::kReplace ||
         kind == GroveDb::BatchOp::Kind::kPatch ||
         kind == GroveDb::BatchOp::Kind::kInsertTree ||
         kind == GroveDb::BatchOp::Kind::kInsertOrReplace;
}

bool IsConsistencyInsertBelowDeleteKind(GroveDb::BatchOp::Kind kind) {
  // Rust parity: verify_consistency_of_operations only flags delete-subtree
  // conflicts for Replace and InsertOrReplace operations.
  return kind == GroveDb::BatchOp::Kind::kReplace ||
         kind == GroveDb::BatchOp::Kind::kInsertOrReplace;
}

bool BatchOpHasSamePathAndKey(const GroveDb::BatchOp& lhs, const GroveDb::BatchOp& rhs) {
  return lhs.path == rhs.path && lhs.key == rhs.key;
}

bool BatchOpIsExactDuplicate(const GroveDb::BatchOp& lhs, const GroveDb::BatchOp& rhs) {
  return lhs.kind == rhs.kind && BatchOpHasSamePathAndKey(lhs, rhs) &&
         lhs.element_bytes == rhs.element_bytes;
}

bool BatchHasInsertBelowDeleteConflict(const std::vector<GroveDb::BatchOp>& ops) {
  for (const auto& deleted_op : ops) {
    if (deleted_op.kind != GroveDb::BatchOp::Kind::kDelete) {
      continue;
    }
    std::vector<std::vector<uint8_t>> deleted_qualified_path = deleted_op.path;
    deleted_qualified_path.push_back(deleted_op.key);
    for (const auto& insert_op : ops) {
      if (!IsConsistencyInsertBelowDeleteKind(insert_op.kind)) {
        continue;
      }
      if (insert_op.path.size() <= deleted_op.path.size()) {
        continue;
      }
      bool is_prefix = true;
      for (size_t i = 0; i < deleted_qualified_path.size(); ++i) {
        if (deleted_qualified_path[i] != insert_op.path[i]) {
          is_prefix = false;
          break;
        }
      }
      if (is_prefix) {
        return true;
      }
    }
  }
  return false;
}

std::vector<std::vector<std::vector<uint8_t>>> CollectDeletedQualifiedPaths(
    const std::vector<GroveDb::BatchOp>& ops,
    int32_t stop_level = 0) {
  const int32_t effective_stop = std::max(0, stop_level);
  std::vector<std::vector<std::vector<uint8_t>>> deleted_paths;
  deleted_paths.reserve(ops.size());
  for (const auto& op : ops) {
    if (op.kind != GroveDb::BatchOp::Kind::kDelete) {
      continue;
    }
    if (op.path.size() < static_cast<size_t>(effective_stop)) {
      continue;
    }
    std::vector<std::vector<uint8_t>> deleted_qualified_path = op.path;
    deleted_qualified_path.push_back(op.key);
    deleted_paths.push_back(std::move(deleted_qualified_path));
  }
  return deleted_paths;
}

bool IsModificationUnderDeletedTree(
    const GroveDb::BatchOp& op,
    const std::vector<std::vector<std::vector<uint8_t>>>& deleted_qualified_paths) {
  if (!IsInsertLikeBatchOpKind(op.kind)) {
    return false;
  }
  for (const auto& deleted_path : deleted_qualified_paths) {
    if (op.path.size() < deleted_path.size()) {
      continue;
    }
    bool is_prefix = true;
    for (size_t i = 0; i < deleted_path.size(); ++i) {
      if (op.path[i] != deleted_path[i]) {
        is_prefix = false;
        break;
      }
    }
    if (is_prefix) {
      return true;
    }
  }
  return false;
}

bool VerifyBatchOperationConsistency(const std::vector<GroveDb::BatchOp>& ops) {
  for (size_t i = 0; i < ops.size(); ++i) {
    for (size_t j = i + 1; j < ops.size(); ++j) {
      if (BatchOpIsExactDuplicate(ops[i], ops[j])) {
        return false;
      }
      if (BatchOpHasSamePathAndKey(ops[i], ops[j])) {
        return false;
      }
    }
  }
  return !BatchHasInsertBelowDeleteConflict(ops);
}

int RustBatchOpPriority(GroveDb::BatchOp::Kind kind) {
  // Match Rust GroveOp::to_u8 ordering for externally expressible ops.
  switch (kind) {
    case GroveDb::BatchOp::Kind::kDeleteTree:
      return 0;  // DeleteTree
    case GroveDb::BatchOp::Kind::kDelete:
      return 2;  // Delete
    case GroveDb::BatchOp::Kind::kRefreshReference:
      return 5;  // RefreshReference
    case GroveDb::BatchOp::Kind::kReplace:
      return 6;  // Replace
    case GroveDb::BatchOp::Kind::kPatch:
      return 7;  // Patch
    case GroveDb::BatchOp::Kind::kInsert:
    case GroveDb::BatchOp::Kind::kInsertOrReplace:
    case GroveDb::BatchOp::Kind::kInsertTree:
      return 8;  // InsertOrReplace / tree inserts represented as insert-like ops
    case GroveDb::BatchOp::Kind::kInsertOnly:
      return 9;  // InsertOnly
  }
  return 255;
}

bool BatchOpRustExecutionLess(const GroveDb::BatchOp& lhs, const GroveDb::BatchOp& rhs) {
  if (lhs.path.size() != rhs.path.size()) {
    return lhs.path.size() < rhs.path.size();
  }
  if (lhs.path != rhs.path) {
    return lhs.path < rhs.path;
  }
  if (lhs.key != rhs.key) {
    return lhs.key < rhs.key;
  }
  return RustBatchOpPriority(lhs.kind) < RustBatchOpPriority(rhs.kind);
}

std::vector<GroveDb::BatchOp> RustOrderedBatchOps(const std::vector<GroveDb::BatchOp>& ops) {
  std::vector<GroveDb::BatchOp> ordered = ops;
  std::stable_sort(ordered.begin(), ordered.end(), BatchOpRustExecutionLess);
  return ordered;
}

std::vector<GroveDb::BatchOp> RustCanonicalizedBatchOpsForDisabledConsistency(
    const std::vector<GroveDb::BatchOp>& ops) {
  std::map<std::pair<std::vector<std::vector<uint8_t>>, std::vector<uint8_t>>, GroveDb::BatchOp>
      latest_by_path_key;
  for (const auto& op : ops) {
    latest_by_path_key[std::make_pair(op.path, op.key)] = op;
  }

  std::vector<GroveDb::BatchOp> canonicalized;
  canonicalized.reserve(latest_by_path_key.size());
  for (const auto& entry : latest_by_path_key) {
    canonicalized.push_back(entry.second);
  }
  if (canonicalized.size() > 1) {
    std::stable_sort(canonicalized.begin(), canonicalized.end(), BatchOpRustExecutionLess);
  }
  return canonicalized;
}

bool GenerateSingleItemMerkProof(const MerkTree& tree,
                                 const QueryItem& item,
                                 std::vector<uint8_t>* out_merk_proof,
                                 std::vector<uint8_t>* out_root_hash,
                                 std::string* error) {
  if (out_merk_proof == nullptr || out_root_hash == nullptr) {
    if (error) {
      *error = "proof output is null";
    }
    return false;
  }
  if (!item.IsKey()) {
    if (error) {
      *error = "unsupported query shape for proof generation";
    }
    return false;
  }
  std::vector<uint8_t> ignored;
  if (tree.Get(item.start, &ignored)) {
    return tree.GenerateProof(item.start,
                              TargetEncoding::kKv,
                              tree.GetValueHashFn(),
                              out_merk_proof,
                              out_root_hash,
                              &ignored,
                              error);
  }
  return tree.GenerateAbsenceProof(item.start, tree.GetValueHashFn(), out_merk_proof, out_root_hash, error);
}

void ApplyPatchChangeInBytesCostHint(const GroveDb::BatchOp& op, OperationCost* op_cost) {
  if (op_cost == nullptr || op.kind != GroveDb::BatchOp::Kind::kPatch) {
    return;
  }
  // Rust patch estimated costs model a shrinking patch as a rewrite without
  // storage removal accounting (it rewrites bytes in-place semantics rather than
  // reporting a delete delta like replace). Our estimate path measures patch via
  // Insert(...) today, so fold away removed bytes for negative patch deltas to
  // align with Rust's patch-specific cost shape.
  if (op.change_in_bytes < 0 && op_cost->storage_cost.removed_bytes.TotalRemovedBytes() > 0) {
    op_cost->storage_cost.removed_bytes = StorageRemovedBytes::None();
  }
}

bool GenerateMerkProofForQueryItem(const MerkTree& tree,
                                   const QueryItem& item,
                                   const std::optional<uint16_t>& limit,
                                   bool require_value_hash_for_present_key,
                                   std::vector<uint8_t>* out_merk_proof,
                                   std::vector<uint8_t>* out_root_hash,
                                   std::string* error) {
  if (out_merk_proof == nullptr || out_root_hash == nullptr) {
    if (error) {
      *error = "proof output is null";
    }
    return false;
  }
  if (!item.IsRange()) {
    if (item.IsKey() && require_value_hash_for_present_key) {
      // Rust prove_query uses the Merk query-proof engine even for exact-key query
      // items, so use a degenerate inclusive range to match query-proof op
      // sequencing (boundary digests/hashes) and node typing.
      return tree.GenerateRangeProofWithTargetEncoding(item.start,
                                                       item.start,
                                                       /*start_inclusive=*/true,
                                                       /*end_inclusive=*/true,
                                                       TargetEncoding::kKvValueHash,
                                                       tree.GetValueHashFn(),
                                                       out_merk_proof,
                                                       out_root_hash,
                                                       error);
    }
    return GenerateSingleItemMerkProof(tree, item, out_merk_proof, out_root_hash, error);
  }

  std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> entries;
  if (!tree.Export(&entries)) {
    if (error) {
      *error = "range proof export failed";
    }
    return false;
  }

  std::optional<std::vector<uint8_t>> lower_bound = item.LowerBound();
  std::pair<std::optional<std::vector<uint8_t>>, bool> upper = item.UpperBound();
  const std::optional<std::vector<uint8_t>>& upper_bound = upper.first;
  bool end_inclusive = upper.second;
  if (!upper_bound.has_value()) {
    // Rust unbounded-upper query items (`RangeFull`, `RangeFrom`, `RangeAfter`) include
    // the rightmost key when present.
    end_inclusive = true;
  }
  bool start_inclusive = true;
  switch (item.type) {
    case QueryItemType::kRangeAfter:
    case QueryItemType::kRangeAfterTo:
    case QueryItemType::kRangeAfterToInclusive:
      start_inclusive = false;
      break;
    default:
      break;
  }

  if (entries.empty()) {
    std::vector<uint8_t> absence_key;
    if (lower_bound.has_value()) {
      absence_key = *lower_bound;
    } else if (upper_bound.has_value()) {
      absence_key = *upper_bound;
    }
    return tree.GenerateAbsenceProof(
        absence_key, tree.GetValueHashFn(), out_merk_proof, out_root_hash, error);
  }
  std::vector<uint8_t> start_key =
      lower_bound.has_value() ? *lower_bound : entries.front().first;
  std::vector<uint8_t> end_key;
  if (upper_bound.has_value()) {
    end_key = *upper_bound;
  } else {
    // Preserve Rust's truly-unbounded upper behavior in Merk proof generation.
    // Using the current max key as an artificial upper bound collapses some
    // empty-range proofs (e.g. RangeFrom beyond max) and loses boundary paths.
    end_key = entries.back().first;
    end_key.push_back(0xFF);
  }
  if (end_key < start_key ||
      (start_key == end_key && (!start_inclusive || !end_inclusive))) {
    // Preserve range-proof semantics for empty ranges near the tree edges instead
    // of collapsing to an exact-key absence proof (Rust emits a range-proof
    // shape here). We anchor to the nearest existing key and force an empty
    // interval by making one side exclusive.
    if (!entries.empty()) {
      if (!lower_bound.has_value() && upper_bound.has_value() && *upper_bound < entries.front().first) {
        return tree.GenerateAbsenceProof(
            *upper_bound, tree.GetValueHashFn(), out_merk_proof, out_root_hash, error);
      } else if (start_key == end_key && (!start_inclusive || !end_inclusive)) {
        // Keep equal-bound empty range as a range proof; EmitProofOpsForRange can
        // encode the empty interval semantics directly.
      } else {
        std::vector<uint8_t> absence_key = lower_bound.has_value()
                                               ? *lower_bound
                                               : (upper_bound.has_value() ? *upper_bound : start_key);
        return tree.GenerateAbsenceProof(
            absence_key, tree.GetValueHashFn(), out_merk_proof, out_root_hash, error);
      }
    } else {
      std::vector<uint8_t> absence_key = lower_bound.has_value()
                                             ? *lower_bound
                                             : (upper_bound.has_value() ? *upper_bound : start_key);
      return tree.GenerateAbsenceProof(
          absence_key, tree.GetValueHashFn(), out_merk_proof, out_root_hash, error);
    }
  }

  if (limit.has_value()) {
    return tree.GenerateRangeProofWithLimit(start_key,
                                            end_key,
                                            start_inclusive,
                                            end_inclusive,
                                            limit.value(),
                                            tree.GetValueHashFn(),
                                            out_merk_proof,
                                            out_root_hash,
                                            error);
  }
  return tree.GenerateRangeProof(start_key,
                                 end_key,
                                 start_inclusive,
                                 end_inclusive,
                                 tree.GetValueHashFn(),
                                 out_merk_proof,
                                 out_root_hash,
                                 error);
}

bool DecodeVarint(const std::vector<uint8_t>& bytes,
                  size_t* cursor,
                  uint64_t* out,
                  std::string* error) {
  if (cursor == nullptr || out == nullptr) {
    if (error) {
      *error = "decode cursor or output is null";
    }
    return false;
  }
  uint64_t result = 0;
  int shift = 0;
  while (*cursor < bytes.size()) {
    uint8_t byte = bytes[*cursor];
    (*cursor)++;
    result |= static_cast<uint64_t>(byte & 0x7F) << shift;
    if ((byte & 0x80) == 0) {
      *out = result;
      return true;
    }
    shift += 7;
    if (shift >= 64) {
      if (error) {
        *error = "varint is too large";
      }
      return false;
    }
  }
  if (error) {
    *error = "unexpected end of input while decoding varint";
  }
  return false;
}

void AppendVarint(uint64_t value, std::vector<uint8_t>* out) {
  while (true) {
    uint8_t byte = static_cast<uint8_t>(value & 0x7F);
    value >>= 7;
    if (value == 0) {
      out->push_back(byte);
      return;
    }
    out->push_back(static_cast<uint8_t>(byte | 0x80));
  }
}

uint64_t ZigZagEncodeI64(int64_t value) {
  return (static_cast<uint64_t>(value) << 1) ^
         static_cast<uint64_t>(value >> 63);
}

int64_t ZigZagDecodeI64(uint64_t value) {
  return static_cast<int64_t>((value >> 1) ^ (~(value & 1) + 1));
}

unsigned __int128 ZigZagEncodeI128(__int128 value) {
  return (static_cast<unsigned __int128>(value) << 1) ^
         static_cast<unsigned __int128>(value >> 127);
}

__int128 ZigZagDecodeI128(unsigned __int128 value) {
  return static_cast<__int128>((value >> 1) ^ (~(value & 1) + 1));
}

void AppendVarintU128(unsigned __int128 value, std::vector<uint8_t>* out) {
  while (true) {
    uint8_t byte = static_cast<uint8_t>(value & 0x7F);
    value >>= 7;
    if (value == 0) {
      out->push_back(byte);
      return;
    }
    out->push_back(static_cast<uint8_t>(byte | 0x80));
  }
}

bool DecodeVarintU128(const std::vector<uint8_t>& bytes,
                      size_t* cursor,
                      unsigned __int128* out,
                      std::string* error) {
  if (cursor == nullptr || out == nullptr) {
    if (error) {
      *error = "decode cursor or output is null";
    }
    return false;
  }
  unsigned __int128 result = 0;
  int shift = 0;
  while (*cursor < bytes.size()) {
    uint8_t byte = bytes[*cursor];
    (*cursor)++;
    result |= static_cast<unsigned __int128>(byte & 0x7F) << shift;
    if ((byte & 0x80) == 0) {
      *out = result;
      return true;
    }
    shift += 7;
    if (shift >= 128) {
      if (error) {
        *error = "varint is too large";
      }
      return false;
    }
  }
  if (error) {
    *error = "unexpected end of input while decoding varint";
  }
  return false;
}

bool DecodeOptionBytes(const std::vector<uint8_t>& bytes,
                       size_t* cursor,
                       std::vector<uint8_t>* out,
                       std::string* error) {
  if (cursor == nullptr || out == nullptr) {
    if (error) {
      *error = "option output is null";
    }
    return false;
  }
  uint64_t option = 0;
  if (!DecodeVarint(bytes, cursor, &option, error)) {
    return false;
  }
  if (option == 0) {
    out->clear();
    return true;
  }
  if (option != 1) {
    if (error) {
      *error = "invalid option tag";
    }
    return false;
  }
  uint64_t length = 0;
  if (!DecodeVarint(bytes, cursor, &length, error)) {
    return false;
  }
  if (*cursor > bytes.size() || length > static_cast<uint64_t>(bytes.size() - *cursor)) {
    if (error) {
      *error = "vector length exceeds input size";
    }
    return false;
  }
  out->assign(bytes.begin() + static_cast<long>(*cursor),
              bytes.begin() + static_cast<long>(*cursor + length));
  *cursor += static_cast<size_t>(length);
  return true;
}

bool DecodeFlagsOption(const std::vector<uint8_t>& element_bytes,
                       size_t* cursor,
                       std::vector<uint8_t>* out_flags,
                       std::string* error) {
  uint64_t flags_option = 0;
  if (!DecodeVarint(element_bytes, cursor, &flags_option, error)) {
    return false;
  }
  if (flags_option == 0) {
    out_flags->clear();
  } else if (flags_option == 1) {
    uint64_t flags_len = 0;
    if (!DecodeVarint(element_bytes, cursor, &flags_len, error)) {
      return false;
    }
    if (*cursor > element_bytes.size() || flags_len > static_cast<uint64_t>(element_bytes.size() - *cursor)) {
      if (error) {
        *error = "vector length exceeds input size";
      }
      return false;
    }
    out_flags->assign(element_bytes.begin() + static_cast<long>(*cursor),
                      element_bytes.begin() + static_cast<long>(*cursor + flags_len));
    *cursor += static_cast<size_t>(flags_len);
  } else {
    if (error) {
      *error = "invalid flags option tag";
    }
    return false;
  }
  if (*cursor != element_bytes.size()) {
    if (error) {
      *error = "extra bytes after element decode";
    }
    return false;
  }
  return true;
}

bool DecodeSumTreePayload(const std::vector<uint8_t>& element_bytes,
                          int64_t* out_sum,
                          std::vector<uint8_t>* out_flags,
                          std::string* error) {
  if (out_sum == nullptr || out_flags == nullptr) {
    if (error) {
      *error = "sum tree decode output is null";
    }
    return false;
  }
  size_t cursor = 0;
  uint64_t variant = 0;
  if (!DecodeVarint(element_bytes, &cursor, &variant, error)) {
    return false;
  }
  if (variant != 4) {
    if (error) {
      *error = "element is not a SumTree variant";
    }
    return false;
  }
  std::vector<uint8_t> ignored_root_key;
  if (!DecodeOptionBytes(element_bytes, &cursor, &ignored_root_key, error)) {
    return false;
  }
  uint64_t raw_sum = 0;
  if (!DecodeVarint(element_bytes, &cursor, &raw_sum, error)) {
    return false;
  }
  *out_sum = ZigZagDecodeI64(raw_sum);

  return DecodeFlagsOption(element_bytes, &cursor, out_flags, error);
}

bool DecodeTreePayload(const std::vector<uint8_t>& element_bytes,
                       std::vector<uint8_t>* out_flags,
                       std::string* error) {
  if (out_flags == nullptr) {
    if (error) {
      *error = "tree decode output is null";
    }
    return false;
  }
  size_t cursor = 0;
  uint64_t variant = 0;
  if (!DecodeVarint(element_bytes, &cursor, &variant, error)) {
    return false;
  }
  if (variant != 2) {
    if (error) {
      *error = "element is not a Tree variant";
    }
    return false;
  }
  std::vector<uint8_t> ignored_root_key;
  if (!DecodeOptionBytes(element_bytes, &cursor, &ignored_root_key, error)) {
    return false;
  }
  return DecodeFlagsOption(element_bytes, &cursor, out_flags, error);
}

bool DecodeBigSumTreePayload(const std::vector<uint8_t>& element_bytes,
                             __int128* out_sum,
                             std::vector<uint8_t>* out_flags,
                             std::string* error) {
  if (out_sum == nullptr || out_flags == nullptr) {
    if (error) {
      *error = "big sum tree decode output is null";
    }
    return false;
  }
  size_t cursor = 0;
  uint64_t variant = 0;
  if (!DecodeVarint(element_bytes, &cursor, &variant, error)) {
    return false;
  }
  if (variant != 5) {
    if (error) {
      *error = "element is not a BigSumTree variant";
    }
    return false;
  }
  std::vector<uint8_t> ignored_root_key;
  if (!DecodeOptionBytes(element_bytes, &cursor, &ignored_root_key, error)) {
    return false;
  }
  unsigned __int128 raw_sum = 0;
  if (!DecodeVarintU128(element_bytes, &cursor, &raw_sum, error)) {
    return false;
  }
  *out_sum = ZigZagDecodeI128(raw_sum);
  return DecodeFlagsOption(element_bytes, &cursor, out_flags, error);
}

bool DecodeCountTreePayload(const std::vector<uint8_t>& element_bytes,
                            uint64_t* out_count,
                            std::vector<uint8_t>* out_flags,
                            std::string* error) {
  if (out_count == nullptr || out_flags == nullptr) {
    if (error) {
      *error = "count tree decode output is null";
    }
    return false;
  }
  size_t cursor = 0;
  uint64_t variant = 0;
  if (!DecodeVarint(element_bytes, &cursor, &variant, error)) {
    return false;
  }
  if (variant != 6) {
    if (error) {
      *error = "element is not a CountTree variant";
    }
    return false;
  }
  std::vector<uint8_t> ignored_root_key;
  if (!DecodeOptionBytes(element_bytes, &cursor, &ignored_root_key, error)) {
    return false;
  }
  if (!DecodeVarint(element_bytes, &cursor, out_count, error)) {
    return false;
  }
  return DecodeFlagsOption(element_bytes, &cursor, out_flags, error);
}

bool DecodeCountSumTreePayload(const std::vector<uint8_t>& element_bytes,
                               uint64_t* out_count,
                               int64_t* out_sum,
                               std::vector<uint8_t>* out_flags,
                               std::string* error) {
  if (out_count == nullptr || out_sum == nullptr || out_flags == nullptr) {
    if (error) {
      *error = "count sum tree decode output is null";
    }
    return false;
  }
  size_t cursor = 0;
  uint64_t variant = 0;
  if (!DecodeVarint(element_bytes, &cursor, &variant, error)) {
    return false;
  }
  if (variant != 7) {
    if (error) {
      *error = "element is not a CountSumTree variant";
    }
    return false;
  }
  std::vector<uint8_t> ignored_root_key;
  if (!DecodeOptionBytes(element_bytes, &cursor, &ignored_root_key, error)) {
    return false;
  }
  if (!DecodeVarint(element_bytes, &cursor, out_count, error)) {
    return false;
  }
  uint64_t raw_sum = 0;
  if (!DecodeVarint(element_bytes, &cursor, &raw_sum, error)) {
    return false;
  }
  *out_sum = ZigZagDecodeI64(raw_sum);
  return DecodeFlagsOption(element_bytes, &cursor, out_flags, error);
}

bool DecodeProvableCountTreePayload(const std::vector<uint8_t>& element_bytes,
                                    uint64_t* out_count,
                                    std::vector<uint8_t>* out_flags,
                                    std::string* error) {
  if (out_count == nullptr || out_flags == nullptr) {
    if (error) {
      *error = "provable count tree decode output is null";
    }
    return false;
  }
  size_t cursor = 0;
  uint64_t variant = 0;
  if (!DecodeVarint(element_bytes, &cursor, &variant, error)) {
    return false;
  }
  if (variant != 8) {
    if (error) {
      *error = "element is not a ProvableCountTree variant";
    }
    return false;
  }
  std::vector<uint8_t> ignored_root_key;
  if (!DecodeOptionBytes(element_bytes, &cursor, &ignored_root_key, error)) {
    return false;
  }
  if (!DecodeVarint(element_bytes, &cursor, out_count, error)) {
    return false;
  }
  return DecodeFlagsOption(element_bytes, &cursor, out_flags, error);
}

bool DecodeProvableCountSumTreePayload(const std::vector<uint8_t>& element_bytes,
                                       uint64_t* out_count,
                                       int64_t* out_sum,
                                       std::vector<uint8_t>* out_flags,
                                       std::string* error) {
  if (out_count == nullptr || out_sum == nullptr || out_flags == nullptr) {
    if (error) {
      *error = "provable count sum tree decode output is null";
    }
    return false;
  }
  size_t cursor = 0;
  uint64_t variant = 0;
  if (!DecodeVarint(element_bytes, &cursor, &variant, error)) {
    return false;
  }
  if (variant != 10) {
    if (error) {
      *error = "element is not a ProvableCountSumTree variant";
    }
    return false;
  }
  std::vector<uint8_t> ignored_root_key;
  if (!DecodeOptionBytes(element_bytes, &cursor, &ignored_root_key, error)) {
    return false;
  }
  if (!DecodeVarint(element_bytes, &cursor, out_count, error)) {
    return false;
  }
  uint64_t raw_sum = 0;
  if (!DecodeVarint(element_bytes, &cursor, &raw_sum, error)) {
    return false;
  }
  *out_sum = ZigZagDecodeI64(raw_sum);
  return DecodeFlagsOption(element_bytes, &cursor, out_flags, error);
}

bool EncodeSumTreeWithRootKeyAndFlags(const std::vector<uint8_t>* root_key,
                                      int64_t sum,
                                      const std::vector<uint8_t>& flags,
                                      std::vector<uint8_t>* out,
                                      std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  out->clear();
  AppendVarint(4, out);  // Element::SumTree variant
  if (root_key == nullptr) {
    AppendVarint(0, out);  // Option::None
  } else {
    AppendVarint(1, out);  // Option::Some
    AppendVarint(static_cast<uint64_t>(root_key->size()), out);
    out->insert(out->end(), root_key->begin(), root_key->end());
  }
  AppendVarint(ZigZagEncodeI64(sum), out);
  if (flags.empty()) {
    AppendVarint(0, out);  // flags option = None
  } else {
    AppendVarint(1, out);  // flags option = Some
    AppendVarint(static_cast<uint64_t>(flags.size()), out);
    out->insert(out->end(), flags.begin(), flags.end());
  }
  return true;
}

bool EncodeTreeWithRootKeyAndFlags(const std::vector<uint8_t>* root_key,
                                   const std::vector<uint8_t>& flags,
                                   std::vector<uint8_t>* out,
                                   std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  out->clear();
  AppendVarint(2, out);  // Element::Tree variant
  if (root_key == nullptr) {
    AppendVarint(0, out);  // Option::None
  } else {
    AppendVarint(1, out);  // Option::Some
    AppendVarint(static_cast<uint64_t>(root_key->size()), out);
    out->insert(out->end(), root_key->begin(), root_key->end());
  }
  if (flags.empty()) {
    AppendVarint(0, out);  // flags option = None
  } else {
    AppendVarint(1, out);  // flags option = Some
    AppendVarint(static_cast<uint64_t>(flags.size()), out);
    out->insert(out->end(), flags.begin(), flags.end());
  }
  return true;
}

bool EncodeCountTreeWithRootKeyAndFlags(const std::vector<uint8_t>* root_key,
                                        uint64_t count,
                                        const std::vector<uint8_t>& flags,
                                        std::vector<uint8_t>* out,
                                        std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  out->clear();
  AppendVarint(6, out);  // Element::CountTree variant
  if (root_key == nullptr) {
    AppendVarint(0, out);  // Option::None
  } else {
    AppendVarint(1, out);  // Option::Some
    AppendVarint(static_cast<uint64_t>(root_key->size()), out);
    out->insert(out->end(), root_key->begin(), root_key->end());
  }
  AppendVarint(count, out);
  if (flags.empty()) {
    AppendVarint(0, out);  // flags option = None
  } else {
    AppendVarint(1, out);  // flags option = Some
    AppendVarint(static_cast<uint64_t>(flags.size()), out);
    out->insert(out->end(), flags.begin(), flags.end());
  }
  return true;
}

bool EncodeBigSumTreeWithRootKeyAndFlags(const std::vector<uint8_t>* root_key,
                                         __int128 sum,
                                         const std::vector<uint8_t>& flags,
                                         std::vector<uint8_t>* out,
                                         std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  out->clear();
  AppendVarint(5, out);  // Element::BigSumTree variant
  if (root_key == nullptr) {
    AppendVarint(0, out);  // Option::None
  } else {
    AppendVarint(1, out);  // Option::Some
    AppendVarint(static_cast<uint64_t>(root_key->size()), out);
    out->insert(out->end(), root_key->begin(), root_key->end());
  }
  AppendVarintU128(ZigZagEncodeI128(sum), out);
  if (flags.empty()) {
    AppendVarint(0, out);  // flags option = None
  } else {
    AppendVarint(1, out);  // flags option = Some
    AppendVarint(static_cast<uint64_t>(flags.size()), out);
    out->insert(out->end(), flags.begin(), flags.end());
  }
  return true;
}

bool EncodeCountSumTreeWithRootKeyAndFlags(const std::vector<uint8_t>* root_key,
                                           uint64_t count,
                                           int64_t sum,
                                           const std::vector<uint8_t>& flags,
                                           std::vector<uint8_t>* out,
                                           std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  out->clear();
  AppendVarint(7, out);  // Element::CountSumTree variant
  if (root_key == nullptr) {
    AppendVarint(0, out);  // Option::None
  } else {
    AppendVarint(1, out);  // Option::Some
    AppendVarint(static_cast<uint64_t>(root_key->size()), out);
    out->insert(out->end(), root_key->begin(), root_key->end());
  }
  AppendVarint(count, out);
  AppendVarint(ZigZagEncodeI64(sum), out);
  if (flags.empty()) {
    AppendVarint(0, out);  // flags option = None
  } else {
    AppendVarint(1, out);  // flags option = Some
    AppendVarint(static_cast<uint64_t>(flags.size()), out);
    out->insert(out->end(), flags.begin(), flags.end());
  }
  return true;
}

bool EncodeProvableCountTreeWithRootKeyAndFlags(const std::vector<uint8_t>* root_key,
                                                uint64_t count,
                                                const std::vector<uint8_t>& flags,
                                                std::vector<uint8_t>* out,
                                                std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  out->clear();
  AppendVarint(8, out);  // Element::ProvableCountTree variant
  if (root_key == nullptr) {
    AppendVarint(0, out);  // Option::None
  } else {
    AppendVarint(1, out);  // Option::Some
    AppendVarint(static_cast<uint64_t>(root_key->size()), out);
    out->insert(out->end(), root_key->begin(), root_key->end());
  }
  AppendVarint(count, out);
  if (flags.empty()) {
    AppendVarint(0, out);  // flags option = None
  } else {
    AppendVarint(1, out);  // flags option = Some
    AppendVarint(static_cast<uint64_t>(flags.size()), out);
    out->insert(out->end(), flags.begin(), flags.end());
  }
  return true;
}

bool EncodeProvableCountSumTreeWithRootKeyAndFlags(const std::vector<uint8_t>* root_key,
                                                   uint64_t count,
                                                   int64_t sum,
                                                   const std::vector<uint8_t>& flags,
                                                   std::vector<uint8_t>* out,
                                                   std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  out->clear();
  AppendVarint(10, out);  // Element::ProvableCountSumTree variant
  if (root_key == nullptr) {
    AppendVarint(0, out);  // Option::None
  } else {
    AppendVarint(1, out);  // Option::Some
    AppendVarint(static_cast<uint64_t>(root_key->size()), out);
    out->insert(out->end(), root_key->begin(), root_key->end());
  }
  AppendVarint(count, out);
  AppendVarint(ZigZagEncodeI64(sum), out);
  if (flags.empty()) {
    AppendVarint(0, out);  // flags option = None
  } else {
    AppendVarint(1, out);  // flags option = Some
    AppendVarint(static_cast<uint64_t>(flags.size()), out);
    out->insert(out->end(), flags.begin(), flags.end());
  }
  return true;
}

bool ResolveReferenceQualifiedPath(const ReferencePathType& reference_path,
                                   const std::vector<std::vector<uint8_t>>& current_qualified_path,
                                   std::vector<std::vector<uint8_t>>* out,
                                   std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "reference output is null";
    }
    return false;
  }
  if (current_qualified_path.empty()) {
    if (error) {
      *error = "qualified path should always have an element";
    }
    return false;
  }
  out->clear();
  const std::vector<uint8_t>& current_key = current_qualified_path.back();
  const size_t parent_len = current_qualified_path.size() - 1;
  switch (reference_path.kind) {
    case ReferencePathKind::kAbsolute: {
      *out = reference_path.path;
      return true;
    }
    case ReferencePathKind::kUpstreamRootHeight: {
      const size_t keep = static_cast<size_t>(reference_path.height);
      if (keep > parent_len) {
        if (error) {
          *error = "reference stored path cannot satisfy reference constraints";
        }
        return false;
      }
      out->assign(current_qualified_path.begin(), current_qualified_path.begin() + keep);
      out->insert(out->end(), reference_path.path.begin(), reference_path.path.end());
      return true;
    }
    case ReferencePathKind::kUpstreamRootHeightWithParentPathAddition: {
      const size_t keep = static_cast<size_t>(reference_path.height);
      if (keep > parent_len || parent_len == 0) {
        if (error) {
          *error = "reference stored path cannot satisfy reference constraints";
        }
        return false;
      }
      out->assign(current_qualified_path.begin(), current_qualified_path.begin() + keep);
      out->insert(out->end(), reference_path.path.begin(), reference_path.path.end());
      out->push_back(current_qualified_path[parent_len - 1]);
      return true;
    }
    case ReferencePathKind::kUpstreamFromElementHeight: {
      const size_t discard = static_cast<size_t>(reference_path.height);
      if (discard > parent_len) {
        if (error) {
          *error = "reference stored path cannot satisfy reference constraints";
        }
        return false;
      }
      out->assign(current_qualified_path.begin(),
                  current_qualified_path.begin() + (parent_len - discard));
      out->insert(out->end(), reference_path.path.begin(), reference_path.path.end());
      return true;
    }
    case ReferencePathKind::kCousin: {
      if (parent_len == 0) {
        if (error) {
          *error = "reference stored path cannot satisfy reference constraints";
        }
        return false;
      }
      out->assign(current_qualified_path.begin(), current_qualified_path.begin() + (parent_len - 1));
      out->push_back(reference_path.key);
      out->push_back(current_key);
      return true;
    }
    case ReferencePathKind::kRemovedCousin: {
      if (parent_len == 0) {
        if (error) {
          *error = "reference stored path cannot satisfy reference constraints";
        }
        return false;
      }
      out->assign(current_qualified_path.begin(), current_qualified_path.begin() + (parent_len - 1));
      out->insert(out->end(), reference_path.path.begin(), reference_path.path.end());
      out->push_back(current_key);
      return true;
    }
    case ReferencePathKind::kSibling: {
      out->assign(current_qualified_path.begin(), current_qualified_path.begin() + parent_len);
      out->push_back(reference_path.key);
      return true;
    }
  }
  if (error) {
    *error = "unsupported reference path kind";
  }
  return false;
}

void RemapReferenceResolutionError(std::string* error) {
  if (error == nullptr) {
    return;
  }
  if (*error == "path not found") {
    *error = "corrupted reference path not found";
  } else if (*error == "path key not found") {
    *error = "corrupted reference path key not found";
  } else if (*error == "path element is not a tree") {
    *error = "corrupted reference path parent layer not found";
  }
}

}

bool GroveDb::Open(const std::string& path, std::string* error) {
  if (!storage_.Open(path, error)) {
    return false;
  }
  non_tx_validated_tree_paths_.clear();
  non_tx_insert_cache_path_key_.clear();
  if (non_tx_insert_cache_ != nullptr) {
    non_tx_insert_cache_->clear();
  }
  if (merk_cache_) {
    merk_cache_->clear();
  }
  opened_ = true;
  return true;
}

bool GroveDb::OpenCheckpoint(const std::string& path, std::string* error) {
  const std::filesystem::path checkpoint_path(path);
  if (checkpoint_path.empty()) {
    if (error) {
      *error = "checkpoint path is empty";
    }
    return false;
  }
  std::error_code ec;
  if (!std::filesystem::exists(checkpoint_path, ec) || ec ||
      !std::filesystem::is_directory(checkpoint_path, ec) || ec) {
    if (error) {
      *error = "checkpoint path does not exist";
    }
    return false;
  }
  const std::filesystem::path current_file = checkpoint_path / "CURRENT";
  if (!std::filesystem::exists(current_file, ec) || ec) {
    if (error) {
      *error = "not a valid checkpoint path";
    }
    return false;
  }
  return Open(path, error);
}

bool GroveDb::Wipe(std::string* error) {
  if (!opened_) {
    if (error) {
      *error = "database not opened";
    }
    return false;
  }
  if (!storage_.Clear(ColumnFamilyKind::kDefault, error)) {
    return false;
  }
  if (!storage_.Clear(ColumnFamilyKind::kAux, error)) {
    return false;
  }
  if (!storage_.Clear(ColumnFamilyKind::kRoots, error)) {
    return false;
  }
  if (!storage_.Clear(ColumnFamilyKind::kMeta, error)) {
    return false;
  }
  non_tx_validated_tree_paths_.clear();
  non_tx_insert_cache_path_key_.clear();
  if (non_tx_insert_cache_ != nullptr) {
    non_tx_insert_cache_->clear();
  }
  if (merk_cache_) {
    merk_cache_->clear();
  }
  return true;
}

bool GroveDb::Flush(std::string* error) {
  if (!opened_) {
    if (error) {
      *error = "database not opened";
    }
    return false;
  }
  return storage_.Flush(error);
}

bool GroveDb::StartVisualizer(const std::string& addr, std::string* error) {
  (void)addr;
  if (error) {
    *error = std::string("visualizer/grovedbg hooks are out of scope for C++ rewrite until ") +
             kVisualizerDivergenceExpiryDate +
             " (Rust `start_visualizer` is behind the `grovedbg` feature; tracking gap " +
             kVisualizerDivergenceGapId + ")";
  }
  return false;
}

bool GroveDb::CreateCheckpoint(const std::string& path, std::string* error) {
  if (!opened_) {
    if (error) {
      *error = "database not opened";
    }
    return false;
  }
  return storage_.CreateCheckpoint(path, error);
}

bool GroveDb::DeleteCheckpoint(const std::string& path, std::string* error) {
  const std::filesystem::path checkpoint_path(path);
  if (checkpoint_path.empty()) {
    if (error) {
      *error = "checkpoint path is empty";
    }
    return false;
  }
  const size_t component_count =
      static_cast<size_t>(std::distance(checkpoint_path.begin(), checkpoint_path.end()));
  if (component_count < 2) {
    if (error) {
      *error = "refusing to delete checkpoint: path too short (safety check)";
    }
    return false;
  }

  std::error_code ec;
  if (!std::filesystem::exists(checkpoint_path, ec) || ec ||
      !std::filesystem::is_directory(checkpoint_path, ec) || ec) {
    if (error) {
      *error = "refusing to delete checkpoint: path does not exist";
    }
    return false;
  }

  const std::filesystem::path current_file = checkpoint_path / "CURRENT";
  if (!std::filesystem::exists(current_file, ec) || ec) {
    if (error) {
      *error = "refusing to delete checkpoint: not a valid checkpoint path";
    }
    return false;
  }

  // Validate the directory is an openable GroveDB checkpoint/database path.
  {
    RocksDbWrapper probe;
    if (!probe.Open(path, error)) {
      return false;
    }
  }

  std::filesystem::remove_all(checkpoint_path, ec);
  if (ec) {
    if (error) {
      *error = "failed to delete checkpoint: " + ec.message();
    }
    return false;
  }
  return true;
}

bool GroveDb::Insert(const std::vector<std::vector<uint8_t>>& path,
                     const std::vector<uint8_t>& key,
                     const std::vector<uint8_t>& element_bytes,
                     std::string* error) {
  return Insert(path, key, element_bytes, nullptr, nullptr, error);
}

bool GroveDb::Insert(const std::vector<std::vector<uint8_t>>& path,
                     const std::vector<uint8_t>& key,
                     const std::vector<uint8_t>& element_bytes,
                     OperationCost* cost,
                     std::string* error) {
  return Insert(path, key, element_bytes, cost, nullptr, error);
}

bool GroveDb::Insert(const std::vector<std::vector<uint8_t>>& path,
                     const std::vector<uint8_t>& key,
                     const std::vector<uint8_t>& element_bytes,
                     Transaction* tx,
                     std::string* error) {
  return Insert(path, key, element_bytes, nullptr, tx, error);
}

bool GroveDb::Insert(const std::vector<std::vector<uint8_t>>& path,
                     const std::vector<uint8_t>& key,
                     const std::vector<uint8_t>& element_bytes,
                     OperationCost* cost,
                     Transaction* tx,
                     std::string* error) {
  insert_profile::SyncLabel();
  if (cost != nullptr) {
    cost->Reset();
  }
  if (!opened_) {
    if (error) {
      *error = "database not opened";
    }
    return false;
  }
  if (key.empty()) {
    if (error) {
      *error = "key is empty";
    }
    return false;
  }
  if (element_bytes.empty()) {
    if (error) {
      *error = "element bytes are empty";
    }
    return false;
  }
  if (tx == nullptr && cost == nullptr) {
    Transaction local_tx;
    if (!StartTransaction(&local_tx, error)) {
      return false;
    }
    std::string non_tx_path_cache_key;
    if (!path.empty()) {
      non_tx_path_cache_key = EncodePathCacheKey(path);
      if (non_tx_insert_cache_path_key_ != non_tx_path_cache_key) {
        non_tx_insert_cache_path_key_.clear();
        if (non_tx_insert_cache_ != nullptr) {
          non_tx_insert_cache_->clear();
        }
      } else if (non_tx_insert_cache_ != nullptr) {
        local_tx.insert_cache = std::move(non_tx_insert_cache_);
        local_tx.insert_cache_path_key = non_tx_path_cache_key;
      }
      if (non_tx_validated_tree_paths_.find(non_tx_path_cache_key) !=
          non_tx_validated_tree_paths_.end()) {
        local_tx.validated_tree_paths.insert(non_tx_path_cache_key);
        local_tx.insert_cache_path_key = non_tx_path_cache_key;
      }
    }
    if (!Insert(path, key, element_bytes, nullptr, &local_tx, error)) {
      std::string rollback_error;
      if (!RollbackTransaction(&local_tx, &rollback_error) && error && !rollback_error.empty()) {
        *error += " (rollback failed: " + rollback_error + ")";
      }
      non_tx_validated_tree_paths_.clear();
      non_tx_insert_cache_path_key_.clear();
      if (non_tx_insert_cache_ != nullptr) {
        non_tx_insert_cache_->clear();
      }
      return false;
    }
    std::unique_ptr<MerkCache> preserved_non_tx_insert_cache;
    std::string preserved_non_tx_insert_cache_path_key;
    if (!non_tx_path_cache_key.empty() &&
        local_tx.insert_cache_path_key == non_tx_path_cache_key &&
        local_tx.insert_cache != nullptr) {
      local_tx.insert_cache->erase(path);
      if (local_tx.insert_cache->size() > 0) {
        preserved_non_tx_insert_cache = std::move(local_tx.insert_cache);
        preserved_non_tx_insert_cache_path_key = non_tx_path_cache_key;
      }
      local_tx.insert_cache_path_key.clear();
      local_tx.validated_tree_paths.clear();
    } else {
      non_tx_insert_cache_path_key_.clear();
      if (non_tx_insert_cache_ != nullptr) {
        non_tx_insert_cache_->clear();
      }
    }
    if (!CommitTransaction(&local_tx, error)) {
      non_tx_validated_tree_paths_.clear();
      non_tx_insert_cache_path_key_.clear();
      if (non_tx_insert_cache_ != nullptr) {
        non_tx_insert_cache_->clear();
      }
      return false;
    }
    if (preserved_non_tx_insert_cache != nullptr &&
        !preserved_non_tx_insert_cache_path_key.empty()) {
      non_tx_insert_cache_ = std::move(preserved_non_tx_insert_cache);
      non_tx_insert_cache_path_key_ = std::move(preserved_non_tx_insert_cache_path_key);
    }
    if (!non_tx_path_cache_key.empty()) {
      non_tx_validated_tree_paths_.insert(std::move(non_tx_path_cache_key));
    }
    return true;
  }
  if (tx != nullptr && tx->state == Transaction::State::kRolledBack) {
    if (!storage_.BeginTransaction(&tx->inner, error)) {
      return false;
    }
    tx->validated_tree_paths.clear();
    tx->state = Transaction::State::kActive;
  }
  if (!EnsureTransactionUsable(tx, error)) {
    return false;
  }
  if (tx != nullptr && tx->state != Transaction::State::kActive) {
    if (error) {
      *error = "transaction is not active";
    }
    return false;
  }
  MerkCache* op_cache = merk_cache_.get();
  if (op_cache == nullptr && tx != nullptr && cost == nullptr && !path.empty()) {
    if (tx->insert_cache == nullptr) {
      tx->insert_cache = std::make_unique<MerkCache>(&storage_);
    }
    op_cache = tx->insert_cache.get();
  }
  const bool use_tx_validated_path_cache = (tx != nullptr && cost == nullptr && !path.empty());
  std::string tx_path_cache_key;
  if (use_tx_validated_path_cache) {
    tx_path_cache_key = EncodePathCacheKey(path);
    if (tx->insert_cache_path_key != tx_path_cache_key) {
      InvalidateTransactionInsertCache(tx);
      tx->insert_cache_path_key = tx_path_cache_key;
      if (tx->insert_cache == nullptr) {
        tx->insert_cache = std::make_unique<MerkCache>(&storage_);
      }
      if (op_cache == nullptr) {
        op_cache = tx->insert_cache.get();
      }
    }
  }
  const bool path_already_validated =
      use_tx_validated_path_cache &&
      tx->validated_tree_paths.find(tx_path_cache_key) != tx->validated_tree_paths.end();
  OperationCost ensure_cost;
  OperationCost* ensure_cost_out = cost != nullptr ? &ensure_cost : nullptr;
  if (!path_already_validated) {
    insert_profile::ScopedStage ensure_scope(insert_profile::Stage::kEnsurePath);
    if (!EnsurePathTreeElements(path, tx, ensure_cost_out, error, op_cache)) {
      if (error) { *error = "Insert:EnsurePathTreeElements: " + *error; }
      return false;
    }
    if (use_tx_validated_path_cache) {
      tx->validated_tree_paths.insert(std::move(tx_path_cache_key));
    }
  }
  DebugPrintCostStage("insert", "ensure_path", ensure_cost);

  MerkTree stack_tree;
  MerkTree* tree_ptr = &stack_tree;
  RocksDbWrapper::Transaction* inner_tx = tx != nullptr ? &tx->inner : nullptr;
  OperationCost load_cost;
  OperationCost* load_cost_out = cost != nullptr ? &load_cost : nullptr;
  if (op_cache != nullptr) {
    insert_profile::ScopedStage load_scope(insert_profile::Stage::kLoadTree);
    if (!GetOrLoadConfiguredMerk(&storage_, inner_tx, path, op_cache, &tree_ptr, error)) {
      if (error) { *error = "Insert:getOrLoad: " + *error; }
      return false;
    }
  } else {
    insert_profile::ScopedStage load_scope(insert_profile::Stage::kLoadTree);
    if (!LoadTreeForPath(&storage_, inner_tx, path, &stack_tree, load_cost_out, error)) {
      return false;
    }
  }
  MerkTree& tree = *tree_ptr;
  DebugPrintCostStage("insert", "load_tree", load_cost);
  OperationCost local_cost;
  OperationCost* write_cost = cost != nullptr ? &local_cost : nullptr;
  bool had_existing_before_insert = false;
  if (write_cost != nullptr && !path.empty()) {
    had_existing_before_insert = tree.Get(key, nullptr);
  }
  {
    insert_profile::ScopedStage leaf_insert_scope(insert_profile::Stage::kLeafInsert);
    if (write_cost != nullptr) {
      if (!tree.Insert(key, element_bytes, write_cost, error)) {
        if (error) { *error = "Insert:tree.Insert(cost): " + *error; }
        return false;
      }
      write_cost->hash_node_calls += 2;
    } else if (!tree.Insert(key, element_bytes, error)) {
      if (error) { *error = "Insert:tree.Insert: " + *error; }
      return false;
    }
  }
  if (tx != nullptr && cost == nullptr) {
    struct AncestorResultSlot {
      size_t owner_depth = 0;
      uint64_t generation = 0;
      bool valid = false;
      bool child_has_root = false;
      std::vector<uint8_t> child_root_key;
      std::vector<uint8_t> child_root_hash;
      MerkTree::RootAggregate aggregate;
    };

    bool transaction_context_tainted = false;
    uint64_t current_insert_generation = 0;
    std::vector<AncestorResultSlot> slots(path.size());

    auto clear_slots_from = [&](size_t start_depth) {
      for (size_t i = start_depth; i < slots.size(); ++i) {
        slots[i] = AncestorResultSlot();
      }
    };

    auto fail_propagation = [&](bool poison_transaction) -> bool {
      clear_slots_from(0);
      if (op_cache != nullptr) {
        op_cache->clear();
      }
      if (poison_transaction) {
        PoisonTransaction(tx);
      }
      return false;
    };

    RocksDbWrapper::WriteBatch op_batch;
    {
      insert_profile::ScopedStage leaf_save_scope(insert_profile::Stage::kLeafSave);
      if (!SaveTreeForPath(&storage_, inner_tx, path, &tree, &op_batch, nullptr, error)) {
        return fail_propagation(false);
      }
    }
    if (!path.empty()) {
      auto populate_slot = [&](size_t owner_depth, MerkTree* child_tree_ptr) -> bool {
        if (owner_depth >= slots.size()) {
          return true;
        }
        ++current_insert_generation;
        clear_slots_from(owner_depth);

        AncestorResultSlot slot;
        slot.owner_depth = owner_depth;
        slot.generation = current_insert_generation;
        slot.child_has_root =
            child_tree_ptr->RootKey(&slot.child_root_key) && !slot.child_root_key.empty();
        if (slot.child_has_root) {
          insert_profile::ScopedStage hash_scope(insert_profile::Stage::kChildRootHash);
          if (!child_tree_ptr->GetCachedRootHash(&slot.child_root_hash, error)) {
            std::string root_hash_error;
            if (!child_tree_ptr->ComputeRootHash(child_tree_ptr->GetValueHashFn(),
                                                 &slot.child_root_hash,
                                                 &root_hash_error)) {
              if (root_hash_error == "encoded node not found") {
                transaction_context_tainted = true;
                if (!storage_.CommitBatch(op_batch, inner_tx, error)) {
                  PoisonTransaction(tx);
                  return false;
                }
                op_batch = RocksDbWrapper::WriteBatch();
                if (!child_tree_ptr->ComputeRootHash(child_tree_ptr->GetValueHashFn(),
                                                     &slot.child_root_hash,
                                                     error)) {
                  PoisonTransaction(tx);
                  return false;
                }
              } else {
                if (error) {
                  *error = std::move(root_hash_error);
                }
                return false;
              }
            }
          }
        }
        if (!child_tree_ptr->RootAggregateData(&slot.aggregate, error)) {
          return false;
        }
        slot.valid = true;
        slots[owner_depth] = std::move(slot);
        return true;
      };

      if (!populate_slot(path.size() - 1, &tree)) {
        return fail_propagation(transaction_context_tainted);
      }

      std::vector<std::vector<uint8_t>> current_path = path;
      MerkTree* child_tree_ptr = &tree;
      while (!current_path.empty()) {
        insert_profile::ScopedStage parent_loop_scope(insert_profile::Stage::kParentLoopTotal);
        std::vector<uint8_t> subtree_key = std::move(current_path.back());
        current_path.pop_back();
        const std::vector<std::vector<uint8_t>>& parent_path = current_path;
        const size_t owner_depth = parent_path.size();

        AncestorResultSlot slot = slots[owner_depth];
        if (!slot.valid || slot.owner_depth != owner_depth ||
            slot.generation != current_insert_generation) {
          if (!populate_slot(owner_depth, child_tree_ptr)) {
            return fail_propagation(transaction_context_tainted);
          }
          slot = slots[owner_depth];
        }
        slots[owner_depth].valid = false;

        MerkTree parent_stack_tree;
        MerkTree* parent_tree_ptr = &parent_stack_tree;
        {
          insert_profile::ScopedStage parent_load_scope(insert_profile::Stage::kParentLoadTree);
          if (op_cache != nullptr) {
            if (!GetOrLoadConfiguredMerk(
                    &storage_, inner_tx, parent_path, op_cache, &parent_tree_ptr, error)) {
              return fail_propagation(transaction_context_tainted);
            }
          } else {
            if (!LoadTreeForPath(&storage_,
                                 inner_tx,
                                 parent_path,
                                 &parent_stack_tree,
                                 nullptr,
                                 error)) {
              return fail_propagation(transaction_context_tainted);
            }
          }
        }
        MerkTree& parent_tree = *parent_tree_ptr;
        std::vector<uint8_t> previous_element;
        if (!parent_tree.Get(subtree_key, &previous_element)) {
          if (error) {
            *error = "path not found during propagation";
          }
          return fail_propagation(transaction_context_tainted);
        }
        // Only compute count when the parent element is a count-bearing tree
        // variant (6=CountTree, 7=CountSumTree, 8=ProvableCountTree,
        // 10=ProvableCountSumTree). Basic Tree (2), SumTree (4), BigSumTree (5)
        // don't use the propagated count.
        uint64_t parent_variant = 0;
        if (!DecodeElementVariant(previous_element, &parent_variant, error)) {
          return fail_propagation(transaction_context_tainted);
        }
        bool needs_count = (parent_variant == 6 || parent_variant == 7 ||
                            parent_variant == 8 || parent_variant == 10);
        bool needs_sum = (parent_variant == 4 || parent_variant == 5 ||
                          parent_variant == 7 || parent_variant == 10);
        uint64_t propagated_count = 0;
        __int128 propagated_sum = 0;
        {
          insert_profile::ScopedStage aggregate_scope(insert_profile::Stage::kAggregatePropagation);
          MerkTree::RootAggregate child_agg = slot.aggregate;
          const bool needs_fallback_flush =
              (needs_count && !child_agg.has_count) || (needs_sum && !child_agg.has_sum);
          if (needs_fallback_flush) {
            transaction_context_tainted = true;
            if (!storage_.CommitBatch(op_batch, inner_tx, error)) {
              PoisonTransaction(tx);
              return fail_propagation(true);
            }
            op_batch = RocksDbWrapper::WriteBatch();
          }
          if ((needs_count && !child_agg.has_count) || (needs_sum && !child_agg.has_sum)) {
            if (!GetPropagatedAggregatesForTree(*child_tree_ptr,
                                                needs_count,
                                                needs_sum,
                                                needs_count ? &propagated_count : nullptr,
                                                needs_sum ? &propagated_sum : nullptr,
                                                &child_agg,
                                                error)) {
              return fail_propagation(transaction_context_tainted);
            }
          } else {
            if (needs_count) {
              propagated_count = child_agg.count;
            }
            if (needs_sum) {
              propagated_sum = child_agg.sum;
            }
          }
          // O(n) aggregate cross-checks: expensive tree walks that verify cached
          // aggregates match full recomputation.  Gated behind an environment
          // variable so they are off by default in production.
          {
            static const bool kVerifyAggregates = []() {
              const char* env = std::getenv("GROVEDB_VERIFY_AGGREGATES");
              return env != nullptr && env[0] == '1' && env[1] == '\0';
            }();
            if (kVerifyAggregates) {
              if (child_agg.has_count) {
                uint64_t full_count = 0;
                bool count_ok = child_tree_ptr->ComputeCount(&full_count, nullptr);
                if (!count_ok || child_agg.count != full_count) {
                  if (error) {
                    *error = "aggregate count invariant violation";
                  }
                  return fail_propagation(transaction_context_tainted);
                }
              }
              if (child_agg.has_sum) {
                __int128 full_sum = 0;
                bool sum_ok =
                    child_tree_ptr->ComputeSumBig(GroveDb::MakeSumValueFn(), &full_sum, nullptr);
                if (!sum_ok || child_agg.sum != full_sum) {
                  if (error) {
                    *error = "aggregate sum invariant violation";
                  }
                  return fail_propagation(transaction_context_tainted);
                }
              }
            }
          }
        }
        const std::vector<uint8_t>* root_key_ptr =
            (slot.child_has_root && !slot.child_root_key.empty()) ? &slot.child_root_key : nullptr;
        std::vector<uint8_t> rewritten_element;
        if (!EncodeTreeElementWithRootKey(previous_element,
                                          root_key_ptr,
                                          propagated_count,
                                          propagated_sum,
                                          &rewritten_element,
                                          error)) {
          return fail_propagation(transaction_context_tainted);
        }
        MerkTree::ValueHashFn original_parent_value_hash_fn = parent_tree.GetValueHashFn();
        if (slot.child_has_root && !slot.child_root_hash.empty()) {
          parent_tree.SetValueHashFn(MakeValueHashFnWithChildRootOverride(
              original_parent_value_hash_fn, subtree_key, slot.child_root_hash));
        }
        {
          insert_profile::ScopedStage parent_insert_scope(insert_profile::Stage::kParentInsert);
          if (!parent_tree.Insert(subtree_key, rewritten_element, error)) {
            if (slot.child_has_root && !slot.child_root_hash.empty()) {
              parent_tree.SetValueHashFn(original_parent_value_hash_fn);
            }
            return fail_propagation(transaction_context_tainted);
          }
        }
        if (slot.child_has_root && !slot.child_root_hash.empty()) {
          parent_tree.SetValueHashFn(original_parent_value_hash_fn);
        }
        {
          insert_profile::ScopedStage parent_save_scope(insert_profile::Stage::kParentSave);
          if (!SaveTreeForPath(&storage_,
                               inner_tx,
                               parent_path,
                               &parent_tree,
                               &op_batch,
                               nullptr,
                               error)) {
            return fail_propagation(transaction_context_tainted);
          }
        }
        if (!parent_path.empty()) {
          if (!populate_slot(parent_path.size() - 1, parent_tree_ptr)) {
            return fail_propagation(transaction_context_tainted);
          }
        }
        child_tree_ptr = parent_tree_ptr;
      }
    }
    {
      insert_profile::ScopedStage commit_scope(insert_profile::Stage::kFinalBatchCommit);
      if (!storage_.CommitBatch(op_batch, inner_tx, error)) {
        PoisonTransaction(tx);
        return fail_propagation(true);
      }
    }
    if (tx->insert_cache != nullptr && !path.empty()) {
      tx->insert_cache->erase(path);
    }
    return true;
  }
  {
    insert_profile::ScopedStage leaf_save_scope(insert_profile::Stage::kLeafSave);
    if (!SaveTreeForPath(&storage_, inner_tx, path, &tree, nullptr, write_cost, error)) {
      return false;
    }
  }
  DebugPrintCostStage("insert", "mutate_and_save", local_cost);
  if (!path.empty()) {
    if (!PropagateSubtreeRootKeyUp(path, tx, write_cost, error, op_cache)) {
      return false;
    }
  }
  if (write_cost != nullptr && !path.empty()) {
    if (!had_existing_before_insert) {
      if (local_cost.seek_count > 0) {
        local_cost.seek_count -= 1;
      }
      local_cost.storage_loaded_bytes += 1;
      const uint32_t add_correction = static_cast<uint32_t>(key.size()) + 3;
      if (local_cost.storage_cost.added_bytes >= add_correction) {
        local_cost.storage_cost.added_bytes -= add_correction;
      }
    } else {
      if (local_cost.seek_count > 0) {
        local_cost.seek_count -= 1;
      }
      local_cost.storage_loaded_bytes += 2;
    }
  }
  DebugPrintCostStage("insert", "after_propagate", local_cost);
  if (cost != nullptr) {
    cost->Add(ensure_cost);
    cost->Add(load_cost);
    cost->Add(local_cost);
    DebugPrintCostStage("insert", "total", *cost);
  }
  return true;
}

bool GroveDb::InsertItem(const std::vector<std::vector<uint8_t>>& path,
                         const std::vector<uint8_t>& key,
                         const std::vector<uint8_t>& value,
                         std::string* error) {
  return InsertItem(path, key, value, nullptr, nullptr, error);
}

bool GroveDb::InsertItem(const std::vector<std::vector<uint8_t>>& path,
                         const std::vector<uint8_t>& key,
                         const std::vector<uint8_t>& value,
                         OperationCost* cost,
                         std::string* error) {
  return InsertItem(path, key, value, cost, nullptr, error);
}

bool GroveDb::InsertItem(const std::vector<std::vector<uint8_t>>& path,
                         const std::vector<uint8_t>& key,
                         const std::vector<uint8_t>& value,
                         Transaction* tx,
                         std::string* error) {
  return InsertItem(path, key, value, nullptr, tx, error);
}

bool GroveDb::InsertItem(const std::vector<std::vector<uint8_t>>& path,
                         const std::vector<uint8_t>& key,
                         const std::vector<uint8_t>& value,
                         OperationCost* cost,
                         Transaction* tx,
                         std::string* error) {
  std::vector<uint8_t> element_bytes;
  if (!EncodeItemToElementBytes(value, &element_bytes, error)) {
    return false;
  }
  return Insert(path, key, element_bytes, cost, tx, error);
}

bool GroveDb::InsertEmptyTree(const std::vector<std::vector<uint8_t>>& path,
                              const std::vector<uint8_t>& key,
                              std::string* error) {
  return InsertEmptyTree(path, key, nullptr, nullptr, error);
}

bool GroveDb::InsertEmptyTree(const std::vector<std::vector<uint8_t>>& path,
                              const std::vector<uint8_t>& key,
                              OperationCost* cost,
                              std::string* error) {
  return InsertEmptyTree(path, key, cost, nullptr, error);
}

bool GroveDb::InsertEmptyTree(const std::vector<std::vector<uint8_t>>& path,
                              const std::vector<uint8_t>& key,
                              Transaction* tx,
                              std::string* error) {
  return InsertEmptyTree(path, key, nullptr, tx, error);
}

bool GroveDb::InsertEmptyTree(const std::vector<std::vector<uint8_t>>& path,
                              const std::vector<uint8_t>& key,
                              OperationCost* cost,
                              Transaction* tx,
                              std::string* error) {
  std::vector<uint8_t> element_bytes;
  if (!EncodeTreeToElementBytes(&element_bytes, error)) {
    return false;
  }
  return Insert(path, key, element_bytes, cost, tx, error);
}

bool GroveDb::InsertBigSumTree(const std::vector<std::vector<uint8_t>>& path,
                               const std::vector<uint8_t>& key,
                               std::string* error) {
  return InsertBigSumTree(path, key, nullptr, nullptr, error);
}

bool GroveDb::InsertBigSumTree(const std::vector<std::vector<uint8_t>>& path,
                               const std::vector<uint8_t>& key,
                               OperationCost* cost,
                               std::string* error) {
  return InsertBigSumTree(path, key, cost, nullptr, error);
}

bool GroveDb::InsertBigSumTree(const std::vector<std::vector<uint8_t>>& path,
                               const std::vector<uint8_t>& key,
                               Transaction* tx,
                               std::string* error) {
  return InsertBigSumTree(path, key, nullptr, tx, error);
}

bool GroveDb::InsertBigSumTree(const std::vector<std::vector<uint8_t>>& path,
                               const std::vector<uint8_t>& key,
                               OperationCost* cost,
                               Transaction* tx,
                               std::string* error) {
  std::vector<uint8_t> element_bytes;
  if (!EncodeBigSumTreeToElementBytesWithRootKey(nullptr, 0, &element_bytes, error)) {
    return false;
  }
  return Insert(path, key, element_bytes, cost, tx, error);
}

bool GroveDb::InsertCountTree(const std::vector<std::vector<uint8_t>>& path,
                              const std::vector<uint8_t>& key,
                              std::string* error) {
  return InsertCountTree(path, key, nullptr, nullptr, error);
}

bool GroveDb::InsertCountTree(const std::vector<std::vector<uint8_t>>& path,
                              const std::vector<uint8_t>& key,
                              OperationCost* cost,
                              std::string* error) {
  return InsertCountTree(path, key, cost, nullptr, error);
}

bool GroveDb::InsertCountTree(const std::vector<std::vector<uint8_t>>& path,
                              const std::vector<uint8_t>& key,
                              Transaction* tx,
                              std::string* error) {
  return InsertCountTree(path, key, nullptr, tx, error);
}

bool GroveDb::InsertCountTree(const std::vector<std::vector<uint8_t>>& path,
                              const std::vector<uint8_t>& key,
                              OperationCost* cost,
                              Transaction* tx,
                              std::string* error) {
  std::vector<uint8_t> element_bytes;
  if (!EncodeCountTreeToElementBytesWithRootKey(nullptr, 0, &element_bytes, error)) {
    return false;
  }
  return Insert(path, key, element_bytes, cost, tx, error);
}

bool GroveDb::InsertProvableCountTree(const std::vector<std::vector<uint8_t>>& path,
                                      const std::vector<uint8_t>& key,
                                      std::string* error) {
  return InsertProvableCountTree(path, key, nullptr, nullptr, error);
}

bool GroveDb::InsertProvableCountTree(const std::vector<std::vector<uint8_t>>& path,
                                      const std::vector<uint8_t>& key,
                                      OperationCost* cost,
                                      std::string* error) {
  return InsertProvableCountTree(path, key, cost, nullptr, error);
}

bool GroveDb::InsertProvableCountTree(const std::vector<std::vector<uint8_t>>& path,
                                      const std::vector<uint8_t>& key,
                                      Transaction* tx,
                                      std::string* error) {
  return InsertProvableCountTree(path, key, nullptr, tx, error);
}

bool GroveDb::InsertProvableCountTree(const std::vector<std::vector<uint8_t>>& path,
                                      const std::vector<uint8_t>& key,
                                      OperationCost* cost,
                                      Transaction* tx,
                                      std::string* error) {
  std::vector<uint8_t> element_bytes;
  if (!EncodeProvableCountTreeToElementBytesWithRootKey(nullptr, 0, &element_bytes, error)) {
    return false;
  }
  return Insert(path, key, element_bytes, cost, tx, error);
}

bool GroveDb::InsertProvableCountSumTree(const std::vector<std::vector<uint8_t>>& path,
                                         const std::vector<uint8_t>& key,
                                         std::string* error) {
  return InsertProvableCountSumTree(path, key, nullptr, nullptr, error);
}

bool GroveDb::InsertProvableCountSumTree(const std::vector<std::vector<uint8_t>>& path,
                                         const std::vector<uint8_t>& key,
                                         OperationCost* cost,
                                         std::string* error) {
  return InsertProvableCountSumTree(path, key, cost, nullptr, error);
}

bool GroveDb::InsertProvableCountSumTree(const std::vector<std::vector<uint8_t>>& path,
                                         const std::vector<uint8_t>& key,
                                         Transaction* tx,
                                         std::string* error) {
  return InsertProvableCountSumTree(path, key, nullptr, tx, error);
}

bool GroveDb::InsertProvableCountSumTree(const std::vector<std::vector<uint8_t>>& path,
                                         const std::vector<uint8_t>& key,
                                         OperationCost* cost,
                                         Transaction* tx,
                                         std::string* error) {
  std::vector<uint8_t> element_bytes;
  if (!EncodeProvableCountSumTreeToElementBytesWithRootKey(nullptr, 0, 0, &element_bytes, error)) {
    return false;
  }
  return Insert(path, key, element_bytes, cost, tx, error);
}

bool GroveDb::InsertSumItem(const std::vector<std::vector<uint8_t>>& path,
                            const std::vector<uint8_t>& key,
                            int64_t sum_value,
                            std::string* error) {
  return InsertSumItem(path, key, sum_value, nullptr, nullptr, error);
}

bool GroveDb::InsertSumItem(const std::vector<std::vector<uint8_t>>& path,
                            const std::vector<uint8_t>& key,
                            int64_t sum_value,
                            OperationCost* cost,
                            std::string* error) {
  return InsertSumItem(path, key, sum_value, cost, nullptr, error);
}

bool GroveDb::InsertSumItem(const std::vector<std::vector<uint8_t>>& path,
                            const std::vector<uint8_t>& key,
                            int64_t sum_value,
                            Transaction* tx,
                            std::string* error) {
  return InsertSumItem(path, key, sum_value, nullptr, tx, error);
}

bool GroveDb::InsertSumItem(const std::vector<std::vector<uint8_t>>& path,
                            const std::vector<uint8_t>& key,
                            int64_t sum_value,
                            OperationCost* cost,
                            Transaction* tx,
                            std::string* error) {
  std::vector<uint8_t> element_bytes;
  if (!EncodeSumItemToElementBytes(sum_value, &element_bytes, error)) {
    return false;
  }
  return Insert(path, key, element_bytes, cost, tx, error);
}

bool GroveDb::InsertItemWithSum(const std::vector<std::vector<uint8_t>>& path,
                                 const std::vector<uint8_t>& key,
                                 const std::vector<uint8_t>& value,
                                 int64_t sum_value,
                                 std::string* error) {
  return InsertItemWithSum(path, key, value, sum_value, nullptr, nullptr, error);
}

bool GroveDb::InsertItemWithSum(const std::vector<std::vector<uint8_t>>& path,
                                 const std::vector<uint8_t>& key,
                                 const std::vector<uint8_t>& value,
                                 int64_t sum_value,
                                 OperationCost* cost,
                                 std::string* error) {
  return InsertItemWithSum(path, key, value, sum_value, cost, nullptr, error);
}

bool GroveDb::InsertItemWithSum(const std::vector<std::vector<uint8_t>>& path,
                                 const std::vector<uint8_t>& key,
                                 const std::vector<uint8_t>& value,
                                 int64_t sum_value,
                                 Transaction* tx,
                                 std::string* error) {
  return InsertItemWithSum(path, key, value, sum_value, nullptr, tx, error);
}

bool GroveDb::InsertItemWithSum(const std::vector<std::vector<uint8_t>>& path,
                                 const std::vector<uint8_t>& key,
                                 const std::vector<uint8_t>& value,
                                 int64_t sum_value,
                                 OperationCost* cost,
                                 Transaction* tx,
                                 std::string* error) {
  std::vector<uint8_t> element_bytes;
  if (!EncodeItemWithSumItemToElementBytes(value, sum_value, &element_bytes, error)) {
    return false;
  }
  return Insert(path, key, element_bytes, cost, tx, error);
}

bool GroveDb::InsertReference(const std::vector<std::vector<uint8_t>>& path,
                              const std::vector<uint8_t>& key,
                              const ReferencePathType& reference_path,
                              std::string* error) {
  return InsertReference(path, key, reference_path, nullptr, nullptr, error);
}

bool GroveDb::InsertReference(const std::vector<std::vector<uint8_t>>& path,
                              const std::vector<uint8_t>& key,
                              const ReferencePathType& reference_path,
                              OperationCost* cost,
                              std::string* error) {
  return InsertReference(path, key, reference_path, cost, nullptr, error);
}

bool GroveDb::InsertReference(const std::vector<std::vector<uint8_t>>& path,
                              const std::vector<uint8_t>& key,
                              const ReferencePathType& reference_path,
                              Transaction* tx,
                              std::string* error) {
  return InsertReference(path, key, reference_path, nullptr, tx, error);
}

bool GroveDb::InsertReference(const std::vector<std::vector<uint8_t>>& path,
                              const std::vector<uint8_t>& key,
                              const ReferencePathType& reference_path,
                              OperationCost* cost,
                              Transaction* tx,
                              std::string* error) {
  ElementReference ref;
  ref.reference_path = reference_path;
  std::vector<uint8_t> element_bytes;
  if (!EncodeReferenceToElementBytes(ref, &element_bytes, error)) {
    return false;
  }
  return Insert(path, key, element_bytes, cost, tx, error);
}

bool GroveDb::InsertIfNotExists(const std::vector<std::vector<uint8_t>>& path,
                                const std::vector<uint8_t>& key,
                                const std::vector<uint8_t>& element_bytes,
                                bool* inserted,
                                std::string* error) {
  return InsertIfNotExists(path, key, element_bytes, inserted, nullptr, nullptr, error);
}

bool GroveDb::InsertIfNotExists(const std::vector<std::vector<uint8_t>>& path,
                                const std::vector<uint8_t>& key,
                                const std::vector<uint8_t>& element_bytes,
                                bool* inserted,
                                OperationCost* cost,
                                std::string* error) {
  return InsertIfNotExists(path, key, element_bytes, inserted, cost, nullptr, error);
}

bool GroveDb::InsertIfNotExists(const std::vector<std::vector<uint8_t>>& path,
                                const std::vector<uint8_t>& key,
                                const std::vector<uint8_t>& element_bytes,
                                bool* inserted,
                                Transaction* tx,
                                std::string* error) {
  return InsertIfNotExists(path, key, element_bytes, inserted, nullptr, tx, error);
}

bool GroveDb::InsertIfNotExists(const std::vector<std::vector<uint8_t>>& path,
                                const std::vector<uint8_t>& key,
                                const std::vector<uint8_t>& element_bytes,
                                bool* inserted,
                                OperationCost* cost,
                                Transaction* tx,
                                std::string* error) {
  if (cost != nullptr) {
    cost->Reset();
  }
  if (inserted == nullptr) {
    if (error) {
      *error = "inserted output is null";
    }
    return false;
  }
  *inserted = false;
  bool found = false;
  const bool has_ok =
      tx != nullptr ? Has(path, key, &found, tx, error) : Has(path, key, &found, error);
  if (!has_ok) {
    return false;
  }
  if (found) {
    return true;
  }
  if (!Insert(path, key, element_bytes, cost, tx, error)) {
    return false;
  }
  *inserted = true;
  return true;
}

bool GroveDb::InsertIfChangedValue(const std::vector<std::vector<uint8_t>>& path,
                                   const std::vector<uint8_t>& key,
                                   const std::vector<uint8_t>& element_bytes,
                                   bool* inserted,
                                   std::vector<uint8_t>* previous_element_bytes,
                                   bool* previous_element_found,
                                   std::string* error) {
  return InsertIfChangedValue(path,
                              key,
                              element_bytes,
                              inserted,
                              previous_element_bytes,
                              previous_element_found,
                              nullptr,
                              nullptr,
                              error);
}

bool GroveDb::InsertIfChangedValue(const std::vector<std::vector<uint8_t>>& path,
                                   const std::vector<uint8_t>& key,
                                   const std::vector<uint8_t>& element_bytes,
                                   bool* inserted,
                                   std::vector<uint8_t>* previous_element_bytes,
                                   bool* previous_element_found,
                                   OperationCost* cost,
                                   std::string* error) {
  return InsertIfChangedValue(path,
                              key,
                              element_bytes,
                              inserted,
                              previous_element_bytes,
                              previous_element_found,
                              cost,
                              nullptr,
                              error);
}

bool GroveDb::InsertIfChangedValue(const std::vector<std::vector<uint8_t>>& path,
                                   const std::vector<uint8_t>& key,
                                   const std::vector<uint8_t>& element_bytes,
                                   bool* inserted,
                                   std::vector<uint8_t>* previous_element_bytes,
                                   bool* previous_element_found,
                                   Transaction* tx,
                                   std::string* error) {
  return InsertIfChangedValue(path,
                              key,
                              element_bytes,
                              inserted,
                              previous_element_bytes,
                              previous_element_found,
                              nullptr,
                              tx,
                              error);
}

bool GroveDb::InsertIfChangedValue(const std::vector<std::vector<uint8_t>>& path,
                                   const std::vector<uint8_t>& key,
                                   const std::vector<uint8_t>& element_bytes,
                                   bool* inserted,
                                   std::vector<uint8_t>* previous_element_bytes,
                                   bool* previous_element_found,
                                   OperationCost* cost,
                                   Transaction* tx,
                                   std::string* error) {
  if (cost != nullptr) {
    cost->Reset();
  }
  if (inserted == nullptr) {
    if (error) {
      *error = "inserted output is null";
    }
    return false;
  }
  if (previous_element_found == nullptr) {
    if (error) {
      *error = "previous element found output is null";
    }
    return false;
  }
  *inserted = false;
  *previous_element_found = false;
  if (previous_element_bytes != nullptr) {
    previous_element_bytes->clear();
  }
  std::vector<uint8_t> existing;
  bool found = false;
  const bool get_ok = tx != nullptr
                          ? Get(path, key, &existing, &found, tx, error)
                          : Get(path, key, &existing, &found, error);
  if (!get_ok) {
    return false;
  }
  if (!found) {
    if (!Insert(path, key, element_bytes, cost, tx, error)) {
      return false;
    }
    *inserted = true;
    return true;
  }
  if (existing == element_bytes) {
    return true;
  }
  if (previous_element_bytes != nullptr) {
    *previous_element_bytes = existing;
  }
  *previous_element_found = true;
  if (!Insert(path, key, element_bytes, cost, tx, error)) {
    return false;
  }
  *inserted = true;
  return true;
}

bool GroveDb::InsertIfNotExistsReturnExisting(
    const std::vector<std::vector<uint8_t>>& path,
    const std::vector<uint8_t>& key,
    const std::vector<uint8_t>& element_bytes,
    std::vector<uint8_t>* existing_element_bytes,
    bool* had_existing,
    std::string* error) {
  return InsertIfNotExistsReturnExisting(path,
                                         key,
                                         element_bytes,
                                         existing_element_bytes,
                                         had_existing,
                                         nullptr,
                                         nullptr,
                                         error);
}

bool GroveDb::InsertIfNotExistsReturnExisting(
    const std::vector<std::vector<uint8_t>>& path,
    const std::vector<uint8_t>& key,
    const std::vector<uint8_t>& element_bytes,
    std::vector<uint8_t>* existing_element_bytes,
    bool* had_existing,
    OperationCost* cost,
    std::string* error) {
  return InsertIfNotExistsReturnExisting(path,
                                         key,
                                         element_bytes,
                                         existing_element_bytes,
                                         had_existing,
                                         cost,
                                         nullptr,
                                         error);
}

bool GroveDb::InsertIfNotExistsReturnExisting(
    const std::vector<std::vector<uint8_t>>& path,
    const std::vector<uint8_t>& key,
    const std::vector<uint8_t>& element_bytes,
    std::vector<uint8_t>* existing_element_bytes,
    bool* had_existing,
    Transaction* tx,
    std::string* error) {
  return InsertIfNotExistsReturnExisting(path,
                                         key,
                                         element_bytes,
                                         existing_element_bytes,
                                         had_existing,
                                         nullptr,
                                         tx,
                                         error);
}

bool GroveDb::InsertIfNotExistsReturnExisting(
    const std::vector<std::vector<uint8_t>>& path,
    const std::vector<uint8_t>& key,
    const std::vector<uint8_t>& element_bytes,
    std::vector<uint8_t>* existing_element_bytes,
    bool* had_existing,
    OperationCost* cost,
    Transaction* tx,
    std::string* error) {
  if (cost != nullptr) {
    cost->Reset();
  }
  if (had_existing == nullptr) {
    if (error) {
      *error = "had existing output is null";
    }
    return false;
  }
  *had_existing = false;
  if (existing_element_bytes != nullptr) {
    existing_element_bytes->clear();
  }
  std::vector<uint8_t> existing;
  bool found = false;
  const bool get_ok = tx != nullptr
                          ? Get(path, key, &existing, &found, tx, error)
                          : Get(path, key, &existing, &found, error);
  if (!get_ok) {
    return false;
  }
  if (found) {
    *had_existing = true;
    if (existing_element_bytes != nullptr) {
      *existing_element_bytes = existing;
    }
    return true;
  }
  if (!Insert(path, key, element_bytes, cost, tx, error)) {
    return false;
  }
  return true;
}

bool GroveDb::Delete(const std::vector<std::vector<uint8_t>>& path,
                     const std::vector<uint8_t>& key,
                     bool* deleted,
                     std::string* error) {
  return Delete(path, key, deleted, nullptr, nullptr, error);
}

bool GroveDb::Delete(const std::vector<std::vector<uint8_t>>& path,
                     const std::vector<uint8_t>& key,
                     bool* deleted,
                     OperationCost* cost,
                     std::string* error) {
  return Delete(path, key, deleted, cost, nullptr, error);
}

bool GroveDb::Delete(const std::vector<std::vector<uint8_t>>& path,
                     const std::vector<uint8_t>& key,
                     bool* deleted,
                     Transaction* tx,
                     std::string* error) {
  return Delete(path, key, deleted, nullptr, tx, error);
}

bool GroveDb::Delete(const std::vector<std::vector<uint8_t>>& path,
                     const std::vector<uint8_t>& key,
                     bool* deleted,
                     OperationCost* cost,
                     Transaction* tx,
                     std::string* error) {
  if (deleted == nullptr) {
    if (error) {
      *error = "deleted output is null";
    }
    return false;
  }
  *deleted = false;
  if (cost != nullptr) {
    cost->Reset();
  }
  if (!opened_) {
    if (error) {
      *error = "database not opened";
    }
    return false;
  }
  if (key.empty()) {
    if (error) {
      *error = "key is empty";
    }
    return false;
  }
  if (tx == nullptr && cost == nullptr) {
    Transaction local_tx;
    if (!StartTransaction(&local_tx, error)) {
      return false;
    }
    if (!Delete(path, key, deleted, nullptr, &local_tx, error)) {
      *deleted = false;
      std::string rollback_error;
      if (!RollbackTransaction(&local_tx, &rollback_error) && error && !rollback_error.empty()) {
        *error += " (rollback failed: " + rollback_error + ")";
      }
      return false;
    }
    return CommitTransaction(&local_tx, error);
  }
  if (tx != nullptr && tx->state == Transaction::State::kRolledBack) {
    if (!storage_.BeginTransaction(&tx->inner, error)) {
      return false;
    }
    tx->state = Transaction::State::kActive;
    InvalidateTransactionInsertCache(tx);
  }
  if (tx != nullptr) {
    InvalidateTransactionInsertCache(tx);
  }
  if (tx != nullptr && tx->state != Transaction::State::kActive) {
    if (error) {
      *error = "transaction is not active";
    }
    return false;
  }
  OperationCost ensure_cost;
  OperationCost* ensure_cost_out = cost != nullptr ? &ensure_cost : nullptr;
  if (!EnsurePathTreeElements(path, tx, ensure_cost_out, error)) {
    return false;
  }
  DebugPrintCostStage("delete", "ensure_path", ensure_cost);

  MerkTree* tree_ptr = nullptr;
  std::unique_ptr<MerkTree> owned_tree;
  RocksDbWrapper::Transaction* inner_tx = tx != nullptr ? &tx->inner : nullptr;
  OperationCost load_cost;
  OperationCost* load_cost_out = cost != nullptr ? &load_cost : nullptr;
  if (merk_cache_ != nullptr && load_cost_out == nullptr) {
    if (!GetOrLoadConfiguredMerk(&storage_, inner_tx, path, merk_cache_.get(), &tree_ptr, error)) {
      return false;
    }
  } else {
    owned_tree = std::make_unique<MerkTree>();
    tree_ptr = owned_tree.get();
    if (!LoadTreeForPath(&storage_, inner_tx, path, tree_ptr, load_cost_out, error)) {
      return false;
    }
  }
  MerkTree& tree = *tree_ptr;
  DebugPrintCostStage("delete", "load_tree", load_cost);
  OperationCost local_cost;
  OperationCost* write_cost = cost != nullptr ? &local_cost : nullptr;
  if (write_cost != nullptr) {
    if (!tree.Delete(key, deleted, write_cost, error)) {
      return false;
    }
  } else if (!tree.Delete(key, deleted, error)) {
    return false;
  }
  if (!*deleted) {
    if (cost != nullptr) {
      cost->Add(ensure_cost);
      cost->Add(load_cost);
      cost->Add(local_cost);
      DebugPrintCostStage("delete", "total", *cost);
    }
    return true;
  }
  if (tx != nullptr && cost == nullptr) {
    RocksDbWrapper::WriteBatch op_batch;
    if (!SaveTreeForPath(&storage_, inner_tx, path, &tree, &op_batch, nullptr, error)) {
      return false;
    }
    if (!path.empty()) {
      auto load_tree_allow_missing_encoded_node = [&](const std::vector<std::vector<uint8_t>>& p,
                                                     MerkTree* out_tree) -> bool {
        if (!LoadTreeForPath(&storage_, inner_tx, p, out_tree, nullptr, error)) {
          if (error != nullptr && *error == "encoded node not found") {
            error->clear();
            *out_tree = MerkTree();
            return true;
          }
          return false;
        }
        return true;
      };
      std::vector<uint8_t> child_root_key;
      bool child_has_root = tree.RootKey(&child_root_key) && !child_root_key.empty();
      uint64_t propagated_count = 0;
      bool count_computed = false;
      std::vector<std::vector<uint8_t>> child_path = path;
      while (!child_path.empty()) {
        const std::vector<uint8_t> subtree_key = child_path.back();
        std::vector<std::vector<uint8_t>> parent_path = child_path;
        parent_path.pop_back();
        MerkTree parent_stack_tree;
        MerkTree* parent_tree_ptr = &parent_stack_tree;
        if (merk_cache_ != nullptr) {
          if (!GetOrLoadConfiguredMerk(
                  &storage_, inner_tx, parent_path, merk_cache_.get(), &parent_tree_ptr, error)) {
            return false;
          }
        } else {
          if (!LoadTreeForPath(&storage_, inner_tx, parent_path, &parent_stack_tree, nullptr, error)) {
            return false;
          }
        }
        MerkTree& parent_tree = *parent_tree_ptr;
        // Parent node hashing for tree elements may depend on the child
        // subtree's persisted bytes/hash. Flush the current child/descendant
        // writes before recomputing the parent node hash.
        if (!storage_.CommitBatch(op_batch, inner_tx, error)) {
          return false;
        }
        op_batch = RocksDbWrapper::WriteBatch();
        MerkTree refreshed_child_tree;
        if (!load_tree_allow_missing_encoded_node(child_path, &refreshed_child_tree)) {
          return false;
        }
        child_has_root = refreshed_child_tree.RootKey(&child_root_key) && !child_root_key.empty();
        std::vector<uint8_t> previous_element;
        if (!parent_tree.Get(subtree_key, &previous_element)) {
          if (error) {
            *error = "path not found";
          }
          return false;
        }
        uint64_t parent_variant = 0;
        if (!DecodeElementVariant(previous_element, &parent_variant, error)) {
          return false;
        }
        bool needs_count = (parent_variant == 6 || parent_variant == 7 ||
                            parent_variant == 8 || parent_variant == 10);
        if (needs_count && !count_computed) {
          if (!storage_.CommitBatch(op_batch, inner_tx, error)) {
            return false;
          }
          op_batch = RocksDbWrapper::WriteBatch();
          if (!GetPropagatedAggregatesForTree(refreshed_child_tree,
                                              true,
                                              false,
                                              &propagated_count,
                                              nullptr,
                                              error)) {
            return false;
          }
          count_computed = true;
        }
        // Compute propagated sum for sum-bearing tree variants (4=SumTree, 5=BigSumTree, 7=CountSumTree, 10=ProvableCountSumTree)
        __int128 propagated_sum = 0;
        bool needs_sum = (parent_variant == 4 || parent_variant == 5 ||
                          parent_variant == 7 || parent_variant == 10);
        if (needs_sum) {
          if (!GetPropagatedAggregatesForTree(refreshed_child_tree,
                                              false,
                                              true,
                                              nullptr,
                                              &propagated_sum,
                                              error)) {
            return false;
          }
        }
        const std::vector<uint8_t>* root_key_ptr =
            (child_has_root && !child_root_key.empty()) ? &child_root_key : nullptr;
        std::vector<uint8_t> rewritten_element;
        if (!EncodeTreeElementWithRootKey(previous_element,
                                          root_key_ptr,
                                          propagated_count,
                                          propagated_sum,
                                          &rewritten_element,
                                          error)) {
          return false;
        }
        if (!parent_tree.Insert(subtree_key, rewritten_element, error)) {
          return false;
        }
        if (!SaveTreeForPath(&storage_,
                             inner_tx,
                             parent_path,
                             &parent_tree,
                             &op_batch,
                             nullptr,
                             error)) {
          return false;
        }
        if (!parent_path.empty()) {
          child_has_root = parent_tree.RootKey(&child_root_key) && !child_root_key.empty();
          if (needs_count) {
            if (!storage_.CommitBatch(op_batch, inner_tx, error)) {
              return false;
            }
            op_batch = RocksDbWrapper::WriteBatch();
            MerkTree parent_count_tree;
            if (!LoadTreeForPath(&storage_, inner_tx, parent_path, &parent_count_tree, nullptr, error)) {
              return false;
            }
            if (!GetPropagatedAggregatesForTree(parent_count_tree,
                                                true,
                                                false,
                                                &propagated_count,
                                                nullptr,
                                                error)) {
              return false;
            }
          }
          count_computed = needs_count;
        }
        child_path = parent_path;
      }
    }
    if (!storage_.CommitBatch(op_batch, inner_tx, error)) {
      return false;
    }
  } else if (!SaveTreeForPath(&storage_, inner_tx, path, &tree, nullptr, write_cost, error)) {
    return false;
  }
  bool tree_is_empty_after_delete = false;
  {
    std::vector<uint8_t> root_key_after_delete;
    tree_is_empty_after_delete = !tree.RootKey(&root_key_after_delete) || root_key_after_delete.empty();
  }
  DebugPrintCostStage("delete", "mutate_and_save", local_cost);
  if (!path.empty() && !PropagateSubtreeRootKeyUp(path, tx, write_cost, error)) {
    return false;
  }
  if (write_cost != nullptr && !path.empty() && tree_is_empty_after_delete) {
    // Rust counts extra persistence work when deleting the last element from a subtree.
    local_cost.seek_count += 3;
    local_cost.storage_loaded_bytes +=
        static_cast<uint64_t>(local_cost.storage_cost.removed_bytes.TotalRemovedBytes()) +
        static_cast<uint64_t>(local_cost.storage_cost.replaced_bytes) + 2;
    local_cost.hash_node_calls += 1;
  }
  DebugPrintCostStage("delete", "after_propagate", local_cost);
  if (cost != nullptr) {
    cost->Add(ensure_cost);
    cost->Add(load_cost);
    cost->Add(local_cost);
    DebugPrintCostStage("delete", "total", *cost);
  }
  return true;
}

bool GroveDb::ClearSubtree(const std::vector<std::vector<uint8_t>>& path,
                           std::string* error) {
  return ClearSubtree(path, nullptr, nullptr, error);
}

bool GroveDb::ClearSubtree(const std::vector<std::vector<uint8_t>>& path,
                           OperationCost* cost,
                           std::string* error) {
  return ClearSubtree(path, cost, nullptr, error);
}

bool GroveDb::ClearSubtree(const std::vector<std::vector<uint8_t>>& path,
                           Transaction* tx,
                           std::string* error) {
  return ClearSubtree(path, nullptr, tx, error);
}

bool GroveDb::ClearSubtree(const std::vector<std::vector<uint8_t>>& path,
                           OperationCost* cost,
                           Transaction* tx,
                           std::string* error) {
  if (cost != nullptr) {
    cost->Reset();
  }
  if (!opened_) {
    if (error) {
      *error = "database not opened";
    }
    return false;
  }
  if (tx != nullptr && tx->state == Transaction::State::kRolledBack) {
    if (!storage_.BeginTransaction(&tx->inner, error)) {
      return false;
    }
    tx->state = Transaction::State::kActive;
  }
  if (tx != nullptr && tx->state != Transaction::State::kActive) {
    if (error) {
      *error = "transaction is not active";
    }
    return false;
  }

  OperationCost ensure_cost;
  OperationCost* ensure_cost_out = cost != nullptr ? &ensure_cost : nullptr;
  if (!EnsurePathTreeElements(path, tx, ensure_cost_out, error)) {
    return false;
  }

  RocksDbWrapper::Transaction* inner_tx = tx != nullptr ? &tx->inner : nullptr;
  MerkTree subtree_tree;
  OperationCost load_cost;
  OperationCost* load_cost_out = cost != nullptr ? &load_cost : nullptr;
  if (!LoadTreeForPath(&storage_, inner_tx, path, &subtree_tree, load_cost_out, error)) {
    return false;
  }

  std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> entries;
  if (!subtree_tree.Export(&entries)) {
    if (error) {
      *error = "failed to iterate subtree entries";
    }
    return false;
  }

  std::vector<std::vector<uint8_t>> keys_to_delete;
  keys_to_delete.reserve(entries.size());
  for (const auto& entry : entries) {
    uint64_t variant = 0;
    if (!DecodeElementVariant(entry.second, &variant, error)) {
      return false;
    }
    if (IsTreeElementVariant(variant)) {
      if (error) {
        *error = "options do not allow to clear this merk tree as it contains subtrees";
      }
      return false;
    }
    keys_to_delete.push_back(entry.first);
  }

  OperationCost clear_cost;
  OperationCost* clear_cost_out = cost != nullptr ? &clear_cost : nullptr;
  for (const auto& key : keys_to_delete) {
    bool deleted = false;
    if (clear_cost_out != nullptr) {
      if (!subtree_tree.Delete(key, &deleted, clear_cost_out, error)) {
        return false;
      }
    } else {
      if (!subtree_tree.Delete(key, &deleted, error)) {
        return false;
      }
    }
    if (!deleted) {
      if (error) {
        *error = "path key not found";
      }
      return false;
    }
  }

  if (!SaveTreeForPath(&storage_, inner_tx, path, &subtree_tree, nullptr, clear_cost_out, error)) {
    return false;
  }
  if (!path.empty() && !PropagateSubtreeRootKeyUp(path, tx, clear_cost_out, error)) {
    return false;
  }

  if (cost != nullptr) {
    cost->Add(ensure_cost);
    cost->Add(load_cost);
    cost->Add(clear_cost);
  }
  return true;
}

bool GroveDb::DeleteSubtree(const std::vector<std::vector<uint8_t>>& path,
                            const std::vector<uint8_t>& key,
                            std::string* error) {
  return DeleteSubtree(path, key, nullptr, nullptr, error);
}

bool GroveDb::DeleteSubtree(const std::vector<std::vector<uint8_t>>& path,
                            const std::vector<uint8_t>& key,
                            OperationCost* cost,
                            std::string* error) {
  return DeleteSubtree(path, key, cost, nullptr, error);
}

bool GroveDb::DeleteSubtree(const std::vector<std::vector<uint8_t>>& path,
                            const std::vector<uint8_t>& key,
                            Transaction* tx,
                            std::string* error) {
  return DeleteSubtree(path, key, nullptr, tx, error);
}

bool GroveDb::DeleteSubtree(const std::vector<std::vector<uint8_t>>& path,
                            const std::vector<uint8_t>& key,
                            OperationCost* cost,
                            Transaction* tx,
                            std::string* error) {
  if (cost != nullptr) {
    cost->Reset();
  }
  if (!opened_) {
    if (error) {
      *error = "database not opened";
    }
    return false;
  }
  if (tx != nullptr && tx->state == Transaction::State::kRolledBack) {
    if (!storage_.BeginTransaction(&tx->inner, error)) {
      return false;
    }
    tx->state = Transaction::State::kActive;
  }
  if (tx != nullptr && tx->state != Transaction::State::kActive) {
    if (error) {
      *error = "transaction is not active";
    }
    return false;
  }
  if (path.empty()) {
    if (error) {
      *error = "cannot delete subtree at root path";
    }
    return false;
  }

  OperationCost ensure_cost;
  OperationCost* ensure_cost_out = cost != nullptr ? &ensure_cost : nullptr;
  if (!EnsurePathTreeElements(path, tx, ensure_cost_out, error)) {
    return false;
  }

  RocksDbWrapper::Transaction* inner_tx = tx != nullptr ? &tx->inner : nullptr;

  // First, get the element to verify it's a tree
  MerkTree parent_tree;
  OperationCost parent_load_cost;
  OperationCost* parent_load_cost_out = cost != nullptr ? &parent_load_cost : nullptr;
  if (!LoadTreeForPath(&storage_, inner_tx, path, &parent_tree, parent_load_cost_out, error)) {
    return false;
  }

  std::vector<uint8_t> element_bytes;
  if (!parent_tree.Get(key, &element_bytes)) {
    if (error) {
      *error = "failed to get element from parent tree";
    }
    return false;
  }
  if (element_bytes.empty()) {
    if (error) {
      *error = "subtree element not found";
    }
    return false;
  }

  uint64_t variant = 0;
  if (!DecodeElementVariant(element_bytes, &variant, error)) {
    return false;
  }
  if (!IsTreeElementVariant(variant)) {
    if (error) {
      *error = "element at path is not a tree";
    }
    return false;
  }

  // Load the subtree merk to recursively delete nested subtrees
  MerkTree subtree_tree;
  OperationCost subtree_load_cost;
  OperationCost* subtree_load_cost_out = cost != nullptr ? &subtree_load_cost : nullptr;
  std::vector<std::vector<uint8_t>> subtree_path = path;
  subtree_path.push_back(key);
  if (!LoadTreeForPath(&storage_, inner_tx, subtree_path, &subtree_tree, subtree_load_cost_out, error)) {
    return false;
  }

  // Recursively delete all nested subtrees first
  std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> entries;
  if (!subtree_tree.Export(&entries)) {
    if (error) {
      *error = "failed to iterate subtree entries";
    }
    return false;
  }

  for (const auto& entry : entries) {
    uint64_t entry_variant = 0;
    if (!DecodeElementVariant(entry.second, &entry_variant, error)) {
      return false;
    }
    if (IsTreeElementVariant(entry_variant)) {
      if (!DeleteSubtree(subtree_path, entry.first, cost, tx, error)) {
        return false;
      }
    }
  }

  // Clear ALL entries from subtree merk (recursive calls already cleaned nested data)
  for (const auto& entry : entries) {
    bool deleted = false;
    OperationCost delete_cost;
    if (!subtree_tree.Delete(entry.first, &deleted, &delete_cost, error)) {
      return false;
    }
    if (cost != nullptr) {
      cost->Add(delete_cost);
    }
  }

  // Save the now-empty subtree merk
  OperationCost subtree_save_cost;
  OperationCost* subtree_save_cost_out = cost != nullptr ? &subtree_save_cost : nullptr;
  if (!SaveTreeForPath(&storage_, inner_tx, subtree_path, &subtree_tree, nullptr, subtree_save_cost_out, error)) {
    return false;
  }

  // Now delete the subtree element itself from the parent
  bool deleted = false;
  OperationCost delete_cost;
  // Delete() requires cost to be non-null, so always provide it
  if (!parent_tree.Delete(key, &deleted, &delete_cost, error)) {
    return false;
  }
  if (!deleted) {
    if (error) {
      *error = "subtree element not found for deletion";
    }
    return false;
  }
  // Accumulate cost if caller requested it
  if (cost != nullptr) {
    cost->Add(delete_cost);
  }

  // Save the parent merk
  OperationCost parent_save_cost;
  OperationCost* parent_save_cost_out = cost != nullptr ? &parent_save_cost : nullptr;
  if (!SaveTreeForPath(&storage_, inner_tx, path, &parent_tree, nullptr, parent_save_cost_out, error)) {
    return false;
  }

  // Propagate changes upward
  if (!path.empty() && !PropagateSubtreeRootKeyUp(path, tx, parent_save_cost_out, error)) {
    return false;
  }

  if (cost != nullptr) {
    cost->Add(ensure_cost);
    cost->Add(parent_load_cost);
    cost->Add(subtree_load_cost);
    cost->Add(subtree_save_cost);
    cost->Add(delete_cost);
    cost->Add(parent_save_cost);
  }
  return true;
}

bool GroveDb::DeleteSubtreeForVersion(const std::vector<std::vector<uint8_t>>& path,
                                      const std::vector<uint8_t>& key,
                                      const GroveVersion& version,
                                      std::string* error) {
  return DeleteSubtreeForVersion(path, key, version, nullptr, nullptr, error);
}

bool GroveDb::DeleteSubtreeForVersion(const std::vector<std::vector<uint8_t>>& path,
                                      const std::vector<uint8_t>& key,
                                      const GroveVersion& version,
                                      OperationCost* cost,
                                      std::string* error) {
  return DeleteSubtreeForVersion(path, key, version, cost, nullptr, error);
}

bool GroveDb::DeleteSubtreeForVersion(const std::vector<std::vector<uint8_t>>& path,
                                      const std::vector<uint8_t>& key,
                                      const GroveVersion& version,
                                      Transaction* tx,
                                      std::string* error) {
  return DeleteSubtreeForVersion(path, key, version, nullptr, tx, error);
}

bool GroveDb::DeleteSubtreeForVersion(const std::vector<std::vector<uint8_t>>& path,
                                      const std::vector<uint8_t>& key,
                                      const GroveVersion& version,
                                      OperationCost* cost,
                                      Transaction* tx,
                                      std::string* error) {
  if (!version.IsSupported()) {
    if (error) {
      *error = "unsupported grove version: " + version.ToString();
    }
    return false;
  }
  return DeleteSubtree(path, key, cost, tx, error);
}

bool GroveDb::DeleteIfEmptyTree(const std::vector<std::vector<uint8_t>>& path,
                                const std::vector<uint8_t>& key,
                                bool* deleted,
                                std::string* error) {
  return DeleteIfEmptyTree(path, key, deleted, nullptr, nullptr, error);
}

bool GroveDb::DeleteIfEmptyTree(const std::vector<std::vector<uint8_t>>& path,
                                const std::vector<uint8_t>& key,
                                bool* deleted,
                                OperationCost* cost,
                                std::string* error) {
  return DeleteIfEmptyTree(path, key, deleted, cost, nullptr, error);
}

bool GroveDb::DeleteIfEmptyTree(const std::vector<std::vector<uint8_t>>& path,
                                const std::vector<uint8_t>& key,
                                bool* deleted,
                                Transaction* tx,
                                std::string* error) {
  return DeleteIfEmptyTree(path, key, deleted, nullptr, tx, error);
}

bool GroveDb::DeleteIfEmptyTree(const std::vector<std::vector<uint8_t>>& path,
                                const std::vector<uint8_t>& key,
                                bool* deleted,
                                OperationCost* cost,
                                Transaction* tx,
                                std::string* error) {
  if (deleted == nullptr) {
    if (error) {
      *error = "deleted output is null";
    }
    return false;
  }
  *deleted = false;
  if (cost != nullptr) {
    cost->Reset();
  }
  if (!opened_) {
    if (error) {
      *error = "database not opened";
    }
    return false;
  }
  if (key.empty()) {
    if (error) {
      *error = "key is empty";
    }
    return false;
  }
  if (tx != nullptr && tx->state == Transaction::State::kRolledBack) {
    if (!storage_.BeginTransaction(&tx->inner, error)) {
      return false;
    }
    tx->state = Transaction::State::kActive;
  }
  if (tx != nullptr && tx->state != Transaction::State::kActive) {
    if (error) {
      *error = "transaction is not active";
    }
    return false;
  }

  OperationCost ensure_cost;
  OperationCost* ensure_cost_out = cost != nullptr ? &ensure_cost : nullptr;
  if (!EnsurePathTreeElements(path, tx, ensure_cost_out, error)) {
    return false;
  }

  RocksDbWrapper::Transaction* inner_tx = tx != nullptr ? &tx->inner : nullptr;
  MerkTree parent_tree;
  OperationCost parent_load_cost;
  OperationCost* parent_load_cost_out = cost != nullptr ? &parent_load_cost : nullptr;
  if (!LoadTreeForPath(&storage_, inner_tx, path, &parent_tree, parent_load_cost_out, error)) {
    return false;
  }
  std::vector<uint8_t> existing_raw;
  if (!parent_tree.Get(key, &existing_raw)) {
    if (error) {
      *error = "path key not found";
    }
    if (cost != nullptr) {
      cost->Add(ensure_cost);
      cost->Add(parent_load_cost);
    }
    return false;
  }

  uint64_t variant = 0;
  if (!DecodeElementVariant(existing_raw, &variant, error)) {
    return false;
  }
  if (!IsTreeElementVariant(variant)) {
    if (error) {
      *error = "delete tree operation requires a tree element";
    }
    if (cost != nullptr) {
      cost->Add(ensure_cost);
      cost->Add(parent_load_cost);
    }
    return false;
  }

  std::vector<std::vector<uint8_t>> subtree_path = path;
  subtree_path.push_back(key);
  MerkTree subtree_tree;
  OperationCost subtree_load_cost;
  OperationCost* subtree_load_cost_out = cost != nullptr ? &subtree_load_cost : nullptr;
  if (!LoadTreeForPath(&storage_, inner_tx, subtree_path, &subtree_tree, subtree_load_cost_out, error)) {
    return false;
  }
  std::vector<uint8_t> subtree_root_key;
  const bool is_empty = !subtree_tree.RootKey(&subtree_root_key) || subtree_root_key.empty();
  if (!is_empty) {
    if (cost != nullptr) {
      cost->Add(ensure_cost);
      cost->Add(parent_load_cost);
      cost->Add(subtree_load_cost);
    }
    return true;
  }

  OperationCost delete_cost;
  OperationCost* delete_cost_out = cost != nullptr ? &delete_cost : nullptr;
  if (!Delete(path, key, deleted, delete_cost_out, tx, error)) {
    return false;
  }
  if (cost != nullptr) {
    cost->Add(ensure_cost);
    cost->Add(parent_load_cost);
    cost->Add(subtree_load_cost);
    cost->Add(delete_cost);
  }
  return true;
}

bool GroveDb::DeleteUpTreeWhileEmpty(const std::vector<std::vector<uint8_t>>& path,
                                     const std::vector<uint8_t>& key,
                                     uint16_t* deleted_count,
                                     std::string* error) {
  return DeleteUpTreeWhileEmpty(path, key, deleted_count, nullptr, nullptr, error);
}

bool GroveDb::DeleteUpTreeWhileEmpty(const std::vector<std::vector<uint8_t>>& path,
                                     const std::vector<uint8_t>& key,
                                     uint16_t* deleted_count,
                                     OperationCost* cost,
                                     std::string* error) {
  return DeleteUpTreeWhileEmpty(path, key, deleted_count, cost, nullptr, error);
}

bool GroveDb::DeleteUpTreeWhileEmpty(const std::vector<std::vector<uint8_t>>& path,
                                     const std::vector<uint8_t>& key,
                                     uint16_t* deleted_count,
                                     Transaction* tx,
                                     std::string* error) {
  return DeleteUpTreeWhileEmpty(path, key, deleted_count, nullptr, tx, error);
}

bool GroveDb::DeleteUpTreeWhileEmpty(const std::vector<std::vector<uint8_t>>& path,
                                     const std::vector<uint8_t>& key,
                                     uint16_t* deleted_count,
                                     OperationCost* cost,
                                     Transaction* tx,
                                     std::string* error) {
  if (deleted_count == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  *deleted_count = 0;
  if (cost != nullptr) {
    cost->Reset();
  }
  if (!opened_) {
    if (error) {
      *error = "database not opened";
    }
    return false;
  }
  if (path.empty()) {
    if (error) {
      *error = "root tree leaves currently cannot be deleted";
    }
    return false;
  }
  if (key.empty()) {
    if (error) {
      *error = "key is empty";
    }
    return false;
  }
  if (tx != nullptr && tx->state == Transaction::State::kRolledBack) {
    if (!storage_.BeginTransaction(&tx->inner, error)) {
      return false;
    }
    tx->state = Transaction::State::kActive;
  }
  if (tx != nullptr && tx->state != Transaction::State::kActive) {
    if (error) {
      *error = "transaction is not active";
    }
    return false;
  }

  std::vector<std::vector<uint8_t>> current_path = path;
  std::vector<uint8_t> current_key = key;
  bool strict_non_empty_check = true;
  while (true) {
    std::vector<uint8_t> current_element;
    bool found = false;
    if (!Get(current_path, current_key, &current_element, &found, tx, error)) {
      return false;
    }
    if (!found) {
      if (error) {
        *error = "path key not found";
      }
      return false;
    }

    uint64_t variant = 0;
    if (!DecodeElementVariant(current_element, &variant, error)) {
      return false;
    }
    if (IsTreeElementVariant(variant)) {
      std::vector<std::vector<uint8_t>> subtree_path = current_path;
      subtree_path.push_back(current_key);
      bool is_empty = false;
      if (!IsEmptyTree(subtree_path, &is_empty, tx, error)) {
        return false;
      }
      if (!is_empty) {
        if (strict_non_empty_check) {
          if (error) {
            *error =
                "trying to do a delete operation for a non empty tree, but options not allowing this";
          }
          return false;
        }
        return true;
      }
    }

    bool deleted_this_level = false;
    OperationCost delete_cost;
    OperationCost* delete_cost_out = cost != nullptr ? &delete_cost : nullptr;
    if (!Delete(current_path, current_key, &deleted_this_level, delete_cost_out, tx, error)) {
      return false;
    }
    if (!deleted_this_level) {
      if (error) {
        *error = "path key not found";
      }
      return false;
    }
    if (cost != nullptr) {
      cost->Add(delete_cost);
    }
    if (*deleted_count != static_cast<uint16_t>(0xFFFF)) {
      *deleted_count = static_cast<uint16_t>(*deleted_count + 1);
    }

    if (current_path.empty()) {
      return true;
    }
    current_key = current_path.back();
    current_path.pop_back();
    if (current_path.empty()) {
      return true;
    }
    strict_non_empty_check = false;
  }
}

bool GroveDb::DeleteUpTreeWhileEmpty(const std::vector<std::vector<uint8_t>>& path,
                                     const std::vector<uint8_t>& key,
                                     uint16_t* deleted_count,
                                     uint16_t stop_path_height,
                                     std::string* error) {
  return DeleteUpTreeWhileEmpty(path, key, deleted_count, stop_path_height, nullptr, nullptr,
                                error);
}

bool GroveDb::DeleteUpTreeWhileEmpty(const std::vector<std::vector<uint8_t>>& path,
                                     const std::vector<uint8_t>& key,
                                     uint16_t* deleted_count,
                                     uint16_t stop_path_height,
                                     OperationCost* cost,
                                     std::string* error) {
  return DeleteUpTreeWhileEmpty(path, key, deleted_count, stop_path_height, cost, nullptr, error);
}

bool GroveDb::DeleteUpTreeWhileEmpty(const std::vector<std::vector<uint8_t>>& path,
                                     const std::vector<uint8_t>& key,
                                     uint16_t* deleted_count,
                                     uint16_t stop_path_height,
                                     Transaction* tx,
                                     std::string* error) {
  return DeleteUpTreeWhileEmpty(path, key, deleted_count, stop_path_height, nullptr, tx, error);
}

bool GroveDb::DeleteUpTreeWhileEmpty(const std::vector<std::vector<uint8_t>>& path,
                                     const std::vector<uint8_t>& key,
                                     uint16_t* deleted_count,
                                     uint16_t stop_path_height,
                                     OperationCost* cost,
                                     Transaction* tx,
                                     std::string* error) {
  if (deleted_count == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  *deleted_count = 0;
  if (cost != nullptr) {
    cost->Reset();
  }
  if (!opened_) {
    if (error) {
      *error = "database not opened";
    }
    return false;
  }
  if (path.empty()) {
    if (error) {
      *error = "root tree leaves currently cannot be deleted";
    }
    return false;
  }
  if (key.empty()) {
    if (error) {
      *error = "key is empty";
    }
    return false;
  }
  if (tx != nullptr && tx->state == Transaction::State::kRolledBack) {
    if (!storage_.BeginTransaction(&tx->inner, error)) {
      return false;
    }
    tx->state = Transaction::State::kActive;
  }
  if (tx != nullptr && tx->state != Transaction::State::kActive) {
    if (error) {
      *error = "transaction is not active";
    }
    return false;
  }

  std::vector<std::vector<uint8_t>> current_path = path;
  std::vector<uint8_t> current_key = key;
  bool strict_non_empty_check = true;
  while (true) {
    // Check if we've reached the stop path height
    if (current_path.size() <= stop_path_height) {
      return true;
    }

    std::vector<uint8_t> current_element;
    bool found = false;
    if (!Get(current_path, current_key, &current_element, &found, tx, error)) {
      return false;
    }
    if (!found) {
      if (error) {
        *error = "path key not found";
      }
      return false;
    }

    uint64_t variant = 0;
    if (!DecodeElementVariant(current_element, &variant, error)) {
      return false;
    }
    if (IsTreeElementVariant(variant)) {
      std::vector<std::vector<uint8_t>> subtree_path = current_path;
      subtree_path.push_back(current_key);
      bool is_empty = false;
      if (!IsEmptyTree(subtree_path, &is_empty, tx, error)) {
        return false;
      }
      if (!is_empty) {
        if (strict_non_empty_check) {
          if (error) {
            *error =
                "trying to do a delete operation for a non empty tree, but options not allowing this";
          }
          return false;
        }
        return true;
      }
    }

    bool deleted_this_level = false;
    OperationCost delete_cost;
    OperationCost* delete_cost_out = cost != nullptr ? &delete_cost : nullptr;
    if (!Delete(current_path, current_key, &deleted_this_level, delete_cost_out, tx, error)) {
      return false;
    }
    if (!deleted_this_level) {
      if (error) {
        *error = "path key not found";
      }
      return false;
    }
    if (cost != nullptr) {
      cost->Add(delete_cost);
    }
    if (*deleted_count != static_cast<uint16_t>(0xFFFF)) {
      *deleted_count = static_cast<uint16_t>(*deleted_count + 1);
    }

    if (current_path.empty()) {
      return true;
    }
    current_key = current_path.back();
    current_path.pop_back();
    if (current_path.empty()) {
      return true;
    }
    strict_non_empty_check = false;
  }
}

bool GroveDb::Get(const std::vector<std::vector<uint8_t>>& path,
                  const std::vector<uint8_t>& key,
                  std::vector<uint8_t>* element_bytes,
                  bool* found,
                  std::string* error) {
  return Get(path, key, element_bytes, found, nullptr, error);
}

bool GroveDb::GetRaw(const std::vector<std::vector<uint8_t>>& path,
                     const std::vector<uint8_t>& key,
                     std::vector<uint8_t>* element_bytes,
                     bool* found,
                     std::string* error) {
  return GetRaw(path, key, element_bytes, found, nullptr, nullptr, error);
}

bool GroveDb::GetRaw(const std::vector<std::vector<uint8_t>>& path,
                     const std::vector<uint8_t>& key,
                     std::vector<uint8_t>* element_bytes,
                     bool* found,
                     OperationCost* cost,
                     std::string* error) {
  return GetRaw(path, key, element_bytes, found, cost, nullptr, error);
}

bool GroveDb::FollowReference(const std::vector<std::vector<uint8_t>>& path,
                              const std::vector<uint8_t>& key,
                              std::vector<uint8_t>* element_bytes,
                              bool* found,
                              std::string* error) {
  return FollowReference(path, key, element_bytes, found, nullptr, nullptr, error);
}

bool GroveDb::FollowReference(const std::vector<std::vector<uint8_t>>& path,
                              const std::vector<uint8_t>& key,
                              std::vector<uint8_t>* element_bytes,
                              bool* found,
                              OperationCost* cost,
                              std::string* error) {
  return FollowReference(path, key, element_bytes, found, cost, nullptr, error);
}

bool GroveDb::Get(const std::vector<std::vector<uint8_t>>& path,
                  const std::vector<uint8_t>& key,
                  std::vector<uint8_t>* element_bytes,
                  bool* found,
                  Transaction* tx,
                  std::string* error) {
  if (!GetRaw(path, key, element_bytes, found, nullptr, tx, error)) {
    return false;
  }
  if (!*found) {
    return true;
  }
  uint64_t variant = 0;
  if (!DecodeElementVariant(*element_bytes, &variant, error)) {
    return false;
  }
  if (variant != 1) {
    return true;
  }
  return FollowReference(path, key, element_bytes, found, tx, error);
}

bool GroveDb::GetRaw(const std::vector<std::vector<uint8_t>>& path,
                     const std::vector<uint8_t>& key,
                     std::vector<uint8_t>* element_bytes,
                     bool* found,
                     Transaction* tx,
                     std::string* error) {
  return GetRaw(path, key, element_bytes, found, nullptr, tx, error);
}

bool GroveDb::GetRaw(const std::vector<std::vector<uint8_t>>& path,
                     const std::vector<uint8_t>& key,
                     std::vector<uint8_t>* element_bytes,
                     bool* found,
                     OperationCost* cost,
                     Transaction* tx,
                     std::string* error) {
  if (element_bytes == nullptr || found == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  *found = false;
  element_bytes->clear();
  if (cost != nullptr) {
    cost->Reset();
  }
  if (!opened_) {
    if (error) {
      *error = "database not opened";
    }
    return false;
  }
  if (key.empty()) {
    if (error) {
      *error = "key is empty";
    }
    return false;
  }
  if (tx != nullptr && tx->state == Transaction::State::kUninitialized) {
    if (error) {
      *error = "transaction is not active";
    }
    return false;
  }
  if (tx != nullptr && tx->state == Transaction::State::kCommitted) {
    if (error) {
      *error = "transaction is committed";
    }
    return false;
  }
  Transaction* read_tx = (tx != nullptr && tx->state == Transaction::State::kActive) ? tx : nullptr;
  OperationCost ensure_cost;
  OperationCost* ensure_cost_out = cost != nullptr ? &ensure_cost : nullptr;
  if (!EnsurePathTreeElements(path, read_tx, ensure_cost_out, error)) {
    return false;
  }
  if (cost != nullptr) {
    cost->Add(ensure_cost);
  }

  std::vector<uint8_t> subtree_root_key;
  bool subtree_exists = false;
  if (!ReadSubtreeRootKey(path, read_tx, &subtree_root_key, &subtree_exists, error)) {
    return false;
  }
  if (!subtree_exists) {
    return true;
  }

  MerkTree tree;
  RocksDbWrapper::Transaction* inner_tx = read_tx != nullptr ? &read_tx->inner : nullptr;
  OperationCost load_cost;
  OperationCost* load_cost_out = cost != nullptr ? &load_cost : nullptr;
  if (!LoadTreeForPath(&storage_, inner_tx, path, &tree, load_cost_out, error)) {
    return false;
  }
  if (cost != nullptr) {
    cost->Add(load_cost);
  }
  *found = tree.Get(key, element_bytes);
  return true;
}

bool GroveDb::GetRawOptional(const std::vector<std::vector<uint8_t>>& path,
                             const std::vector<uint8_t>& key,
                             std::vector<uint8_t>* element_bytes,
                             bool* found,
                             std::string* error) {
  return GetRawOptional(path, key, element_bytes, found, nullptr, nullptr, error);
}

bool GroveDb::GetRawOptional(const std::vector<std::vector<uint8_t>>& path,
                             const std::vector<uint8_t>& key,
                             std::vector<uint8_t>* element_bytes,
                             bool* found,
                             OperationCost* cost,
                             std::string* error) {
  return GetRawOptional(path, key, element_bytes, found, cost, nullptr, error);
}

bool GroveDb::GetRawOptional(const std::vector<std::vector<uint8_t>>& path,
                             const std::vector<uint8_t>& key,
                             std::vector<uint8_t>* element_bytes,
                             bool* found,
                             Transaction* tx,
                             std::string* error) {
  return GetRawOptional(path, key, element_bytes, found, nullptr, tx, error);
}

bool GroveDb::GetRawOptional(const std::vector<std::vector<uint8_t>>& path,
                             const std::vector<uint8_t>& key,
                             std::vector<uint8_t>* element_bytes,
                             bool* found,
                             OperationCost* cost,
                             Transaction* tx,
                             std::string* error) {
  if (element_bytes == nullptr || found == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  *found = false;
  element_bytes->clear();
  if (cost != nullptr) {
    cost->Reset();
  }
  if (!opened_) {
    if (error) {
      *error = "database not opened";
    }
    return false;
  }
  if (key.empty()) {
    if (error) {
      *error = "key is empty";
    }
    return false;
  }
  if (tx != nullptr && tx->state == Transaction::State::kUninitialized) {
    if (error) {
      *error = "transaction is not active";
    }
    return false;
  }
  if (tx != nullptr && tx->state == Transaction::State::kCommitted) {
    if (error) {
      *error = "transaction is committed";
    }
    return false;
  }
  Transaction* read_tx = (tx != nullptr && tx->state == Transaction::State::kActive) ? tx : nullptr;
  OperationCost ensure_cost;
  OperationCost* ensure_cost_out = cost != nullptr ? &ensure_cost : nullptr;
  // Unlike GetRaw, GetRawOptional does not fail for missing paths - returns found=false.
  if (!EnsurePathTreeElementsOptional(path, read_tx, ensure_cost_out, error)) {
    return false;
  }
  if (cost != nullptr) {
    cost->Add(ensure_cost);
  }

  MerkTree tree;
  RocksDbWrapper::Transaction* inner_tx = read_tx != nullptr ? &read_tx->inner : nullptr;
  OperationCost load_cost;
  OperationCost* load_cost_out = cost != nullptr ? &load_cost : nullptr;
  if (!LoadTreeForPath(&storage_, inner_tx, path, &tree, load_cost_out, error)) {
    return false;
  }
  if (cost != nullptr) {
    cost->Add(load_cost);
  }
  *found = tree.Get(key, element_bytes);
  return true;
}

bool GroveDb::GetRawCachingOptional(const std::vector<std::vector<uint8_t>>& path,
                                    const std::vector<uint8_t>& key,
                                    std::vector<uint8_t>* element_bytes,
                                    bool* found,
                                    bool allow_cache,
                                    std::string* error) {
  return GetRawCachingOptional(path, key, element_bytes, found, allow_cache, nullptr, nullptr, error);
}

bool GroveDb::GetRawCachingOptional(const std::vector<std::vector<uint8_t>>& path,
                                    const std::vector<uint8_t>& key,
                                    std::vector<uint8_t>* element_bytes,
                                    bool* found,
                                    bool allow_cache,
                                    OperationCost* cost,
                                    std::string* error) {
  return GetRawCachingOptional(path, key, element_bytes, found, allow_cache, cost, nullptr, error);
}

bool GroveDb::GetRawCachingOptional(const std::vector<std::vector<uint8_t>>& path,
                                    const std::vector<uint8_t>& key,
                                    std::vector<uint8_t>* element_bytes,
                                    bool* found,
                                    bool allow_cache,
                                    Transaction* tx,
                                    std::string* error) {
  return GetRawCachingOptional(path, key, element_bytes, found, allow_cache, nullptr, tx, error);
}

bool GroveDb::GetRawCachingOptional(const std::vector<std::vector<uint8_t>>& path,
                                    const std::vector<uint8_t>& key,
                                    std::vector<uint8_t>* element_bytes,
                                    bool* found,
                                    bool allow_cache,
                                    OperationCost* cost,
                                    Transaction* tx,
                                    std::string* error) {
  if (element_bytes == nullptr || found == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  *found = false;
  element_bytes->clear();
  if (cost != nullptr) {
    cost->Reset();
  }
  if (!opened_) {
    if (error) {
      *error = "database not opened";
    }
    return false;
  }
  if (key.empty()) {
    if (error) {
      *error = "key is empty";
    }
    return false;
  }
  if (tx != nullptr && tx->state == Transaction::State::kUninitialized) {
    if (error) {
      *error = "transaction is not active";
    }
    return false;
  }
  if (tx != nullptr && tx->state == Transaction::State::kCommitted) {
    if (error) {
      *error = "transaction is committed";
    }
    return false;
  }
  Transaction* read_tx = (tx != nullptr && tx->state == Transaction::State::kActive) ? tx : nullptr;
  OperationCost ensure_cost;
  OperationCost* ensure_cost_out = cost != nullptr ? &ensure_cost : nullptr;
  if (!EnsurePathTreeElementsOptional(path, read_tx, ensure_cost_out, error)) {
    return false;
  }
  if (cost != nullptr) {
    cost->Add(ensure_cost);
  }

  MerkTree tree;
  RocksDbWrapper::Transaction* inner_tx = read_tx != nullptr ? &read_tx->inner : nullptr;
  OperationCost load_cost;
  OperationCost* load_cost_out = cost != nullptr ? &load_cost : nullptr;
  // Load tree with optional caching - when allow_cache is false, bypass merk_cache_
  if (allow_cache) {
    if (!LoadTreeForPath(&storage_, inner_tx, path, &tree, load_cost_out, error)) {
      return false;
    }
  } else {
    // Bypass cache by passing nullptr explicitly
    if (!LoadTreeForPath(&storage_, inner_tx, path, &tree, load_cost_out, nullptr, error)) {
      return false;
    }
  }
  if (cost != nullptr) {
    cost->Add(load_cost);
  }
  *found = tree.Get(key, element_bytes);
  return true;
}

bool GroveDb::GetCachingOptional(const std::vector<std::vector<uint8_t>>& path,
                                 const std::vector<uint8_t>& key,
                                 std::vector<uint8_t>* element_bytes,
                                 bool* found,
                                 bool allow_cache,
                                 std::string* error) {
  return GetCachingOptional(path, key, element_bytes, found, allow_cache, nullptr, nullptr, error);
}

bool GroveDb::GetCachingOptional(const std::vector<std::vector<uint8_t>>& path,
                                 const std::vector<uint8_t>& key,
                                 std::vector<uint8_t>* element_bytes,
                                 bool* found,
                                 bool allow_cache,
                                 OperationCost* cost,
                                 std::string* error) {
  return GetCachingOptional(path, key, element_bytes, found, allow_cache, cost, nullptr, error);
}

bool GroveDb::GetCachingOptional(const std::vector<std::vector<uint8_t>>& path,
                                 const std::vector<uint8_t>& key,
                                 std::vector<uint8_t>* element_bytes,
                                 bool* found,
                                 bool allow_cache,
                                 Transaction* tx,
                                 std::string* error) {
  return GetCachingOptional(path, key, element_bytes, found, allow_cache, nullptr, tx, error);
}

bool GroveDb::GetCachingOptional(const std::vector<std::vector<uint8_t>>& path,
                                 const std::vector<uint8_t>& key,
                                 std::vector<uint8_t>* element_bytes,
                                 bool* found,
                                 bool allow_cache,
                                 OperationCost* cost,
                                 Transaction* tx,
                                 std::string* error) {
  auto decode_item_value = [error](const std::vector<uint8_t>& raw_element,
                                   std::vector<uint8_t>* out_value) -> bool {
    uint64_t raw_variant = 0;
    if (!DecodeElementVariant(raw_element, &raw_variant, error)) {
      return false;
    }
    if (raw_variant == 0) {
      ElementItem item;
      if (!DecodeItemFromElementBytes(raw_element, &item, error)) {
        return false;
      }
      *out_value = std::move(item.value);
      return true;
    }
    if (raw_variant == 9) {
      ElementItemWithSum item;
      if (!DecodeItemWithSumItemFromElementBytes(raw_element, &item, error)) {
        return false;
      }
      *out_value = std::move(item.value);
      return true;
    }
    if (raw_variant == 3) {
      ElementSumItem sum_item;
      if (!DecodeSumItemFromElementBytes(raw_element, &sum_item, error)) {
        return false;
      }
      out_value->clear();
      AppendVarint(ZigZagEncodeI64(sum_item.sum), out_value);
      return true;
    }
    if (error) {
      *error = "the reference must result in an item";
    }
    return false;
  };

  if (element_bytes == nullptr || found == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  *found = false;
  element_bytes->clear();
  if (cost != nullptr) {
    cost->Reset();
  }
  if (!opened_) {
    if (error) {
      *error = "database not opened";
    }
    return false;
  }
  if (key.empty()) {
    if (error) {
      *error = "key is empty";
    }
    return false;
  }
  if (tx != nullptr && tx->state == Transaction::State::kUninitialized) {
    if (error) {
      *error = "transaction is not active";
    }
    return false;
  }
  if (tx != nullptr && tx->state == Transaction::State::kCommitted) {
    if (error) {
      *error = "transaction is committed";
    }
    return false;
  }
  Transaction* read_tx = (tx != nullptr && tx->state == Transaction::State::kActive) ? tx : nullptr;
  // GetRawCachingOptional to get raw element without following references
  if (!GetRawCachingOptional(path, key, element_bytes, found, allow_cache, cost, read_tx, error)) {
    return false;
  }
  if (!*found) {
    return true;
  }
  // Check if element is a reference and follow it if needed
  uint64_t variant = 0;
  if (!DecodeElementVariant(*element_bytes, &variant, error)) {
    return false;
  }
  if (variant != 1) {
    return decode_item_value(*element_bytes, element_bytes);
  }
  // Follow reference chain using FollowReference logic with cache bypass
  std::vector<std::vector<uint8_t>> current_qualified_path = path;
  current_qualified_path.push_back(key);
  std::vector<std::vector<std::vector<uint8_t>>> visited;
  size_t hops_left = kMaxReferenceHops;
  while (hops_left > 0) {
    for (const auto& seen : visited) {
      if (seen == current_qualified_path) {
        if (error) {
          *error = "cyclic reference";
        }
        return false;
      }
    }
    if (current_qualified_path.empty()) {
      if (error) {
        *error = "empty reference";
      }
      return false;
    }
    visited.push_back(current_qualified_path);

    const std::vector<uint8_t>& current_key = current_qualified_path.back();
    std::vector<std::vector<uint8_t>> current_path(current_qualified_path.begin(),
                                                   current_qualified_path.end() - 1);

    std::vector<uint8_t> raw_element;
    bool raw_found = false;
    if (!GetRawCachingOptional(current_path, current_key, &raw_element, &raw_found, allow_cache, cost, read_tx, error)) {
      return false;
    }
    if (!raw_found) {
      if (error) {
        *error = "corrupted reference path key not found";
      }
      return false;
    }
    uint64_t elem_variant = 0;
    if (!DecodeElementVariant(raw_element, &elem_variant, error)) {
      return false;
    }
    if (elem_variant != 1) {
      *found = true;
      return decode_item_value(raw_element, element_bytes);
    }
    ElementReference reference;
    if (!DecodeReferenceFromElementBytes(raw_element, &reference, error)) {
      return false;
    }

    std::vector<std::vector<uint8_t>> next_qualified_path;
    if (!ResolveReferenceQualifiedPath(reference.reference_path, current_qualified_path, &next_qualified_path, error)) {
      return false;
    }
    current_qualified_path = std::move(next_qualified_path);
    hops_left -= 1;
  }

  if (error) {
    *error = "reference limit exceeded";
  }
  return false;
}

bool GroveDb::FollowReference(const std::vector<std::vector<uint8_t>>& path,
                              const std::vector<uint8_t>& key,
                              std::vector<uint8_t>* element_bytes,
                              bool* found,
                              Transaction* tx,
                              std::string* error) {
  return FollowReference(path, key, element_bytes, found, nullptr, tx, error);
}

bool GroveDb::FollowReference(const std::vector<std::vector<uint8_t>>& path,
                              const std::vector<uint8_t>& key,
                              std::vector<uint8_t>* element_bytes,
                              bool* found,
                              OperationCost* cost,
                              Transaction* tx,
                              std::string* error) {
  if (element_bytes == nullptr || found == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  *found = false;
  element_bytes->clear();
  if (cost != nullptr) {
    cost->Reset();
  }
  if (!opened_) {
    if (error) {
      *error = "database not opened";
    }
    return false;
  }
  if (key.empty()) {
    if (error) {
      *error = "key is empty";
    }
    return false;
  }
  if (tx != nullptr && tx->state == Transaction::State::kUninitialized) {
    if (error) {
      *error = "transaction is not active";
    }
    return false;
  }
  if (tx != nullptr && tx->state == Transaction::State::kCommitted) {
    if (error) {
      *error = "transaction is committed";
    }
    return false;
  }
  Transaction* read_tx = (tx != nullptr && tx->state == Transaction::State::kActive) ? tx : nullptr;
  RocksDbWrapper::Transaction* inner_tx = read_tx != nullptr ? &read_tx->inner : nullptr;

  std::vector<std::vector<uint8_t>> current_qualified_path = path;
  current_qualified_path.push_back(key);
  std::vector<std::vector<std::vector<uint8_t>>> visited;
  visited.reserve(kMaxReferenceHops + 1);

  size_t hops_left = kMaxReferenceHops;
  while (hops_left > 0) {
    for (const auto& seen : visited) {
      if (seen == current_qualified_path) {
        if (error) {
          *error = "cyclic reference";
        }
        return false;
      }
    }
    if (current_qualified_path.empty()) {
      if (error) {
        *error = "empty reference";
      }
      return false;
    }
    visited.push_back(current_qualified_path);

    const std::vector<uint8_t>& current_key = current_qualified_path.back();
    std::vector<std::vector<uint8_t>> current_path(current_qualified_path.begin(),
                                                   current_qualified_path.end() - 1);

    OperationCost ensure_cost;
    OperationCost* ensure_cost_out = cost != nullptr ? &ensure_cost : nullptr;
    if (!EnsurePathTreeElements(current_path, read_tx, ensure_cost_out, error)) {
      RemapReferenceResolutionError(error);
      return false;
    }
    if (cost != nullptr) {
      cost->Add(ensure_cost);
    }

    MerkTree tree;
    OperationCost load_cost;
    OperationCost* load_cost_out = cost != nullptr ? &load_cost : nullptr;
    if (!LoadTreeForPath(&storage_, inner_tx, current_path, &tree, load_cost_out, error)) {
      RemapReferenceResolutionError(error);
      return false;
    }
    if (cost != nullptr) {
      cost->Add(load_cost);
    }

    std::vector<uint8_t> raw_element;
    if (!tree.Get(current_key, &raw_element)) {
      // Rust facade follow_reference reports a missing starting key as "not found",
      // while missing keys after at least one reference hop indicate a broken chain.
      if (visited.size() == 1) {
        *found = false;
        element_bytes->clear();
        return true;
      }
      if (error) {
        *error = "corrupted reference path key not found";
      }
      return false;
    }
    uint64_t variant = 0;
    if (!DecodeElementVariant(raw_element, &variant, error)) {
      return false;
    }
    if (variant != 1) {
      *element_bytes = std::move(raw_element);
      *found = true;
      return true;
    }
    ElementReference reference;
    if (!DecodeReferenceFromElementBytes(raw_element, &reference, error)) {
      return false;
    }

    std::vector<std::vector<uint8_t>> next_qualified_path;
    if (!ResolveReferenceQualifiedPath(
            reference.reference_path, current_qualified_path, &next_qualified_path, error)) {
      return false;
    }
    current_qualified_path = std::move(next_qualified_path);
    hops_left -= 1;
  }

  if (error) {
    *error = "reference limit exceeded";
  }
  return false;
}

bool GroveDb::Has(const std::vector<std::vector<uint8_t>>& path,
                  const std::vector<uint8_t>& key,
                  bool* found,
                  std::string* error) {
  return Has(path, key, found, nullptr, error);
}

bool GroveDb::Has(const std::vector<std::vector<uint8_t>>& path,
                  const std::vector<uint8_t>& key,
                  bool* found,
                  Transaction* tx,
                  std::string* error) {
  if (found == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  std::vector<uint8_t> ignored;
  return GetRaw(path, key, &ignored, found, tx, error);
}

bool GroveDb::HasRaw(const std::vector<std::vector<uint8_t>>& path,
                     const std::vector<uint8_t>& key,
                     bool* found,
                     std::string* error) {
  return HasRaw(path, key, found, static_cast<OperationCost*>(nullptr),
                static_cast<Transaction*>(nullptr), error);
}

bool GroveDb::HasRaw(const std::vector<std::vector<uint8_t>>& path,
                     const std::vector<uint8_t>& key,
                     bool* found,
                     OperationCost* cost,
                     std::string* error) {
  return HasRaw(path, key, found, cost, static_cast<Transaction*>(nullptr), error);
}

bool GroveDb::HasRaw(const std::vector<std::vector<uint8_t>>& path,
                     const std::vector<uint8_t>& key,
                     bool* found,
                     Transaction* tx,
                     std::string* error) {
  return HasRaw(path, key, found, static_cast<OperationCost*>(nullptr), tx, error);
}

bool GroveDb::HasRaw(const std::vector<std::vector<uint8_t>>& path,
                     const std::vector<uint8_t>& key,
                     bool* found,
                     OperationCost* cost,
                     Transaction* tx,
                     std::string* error) {
  if (found == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  // HasRaw checks key existence without following references
  // It uses GetRaw internally but discards the element bytes
  std::vector<uint8_t> ignored;
  return GetRaw(path, key, &ignored, found, cost, tx, error);
}

bool GroveDb::HasCachingOptional(const std::vector<std::vector<uint8_t>>& path,
                                 const std::vector<uint8_t>& key,
                                 bool* found,
                                 bool allow_cache,
                                 std::string* error) {
  return HasCachingOptional(path, key, found, allow_cache,
                            static_cast<OperationCost*>(nullptr),
                            static_cast<Transaction*>(nullptr), error);
}

bool GroveDb::HasCachingOptional(const std::vector<std::vector<uint8_t>>& path,
                                 const std::vector<uint8_t>& key,
                                 bool* found,
                                 bool allow_cache,
                                 OperationCost* cost,
                                 std::string* error) {
  return HasCachingOptional(path, key, found, allow_cache, cost,
                            static_cast<Transaction*>(nullptr), error);
}

bool GroveDb::HasCachingOptional(const std::vector<std::vector<uint8_t>>& path,
                                 const std::vector<uint8_t>& key,
                                 bool* found,
                                 bool allow_cache,
                                 Transaction* tx,
                                 std::string* error) {
  return HasCachingOptional(path, key, found, allow_cache,
                            static_cast<OperationCost*>(nullptr), tx, error);
}

bool GroveDb::HasCachingOptional(const std::vector<std::vector<uint8_t>>& path,
                                 const std::vector<uint8_t>& key,
                                 bool* found,
                                 bool allow_cache,
                                 OperationCost* cost,
                                 Transaction* tx,
                                 std::string* error) {
  if (found == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  // HasCachingOptional checks key existence without following references
  // It uses GetRawCachingOptional internally but discards the element bytes
  std::vector<uint8_t> ignored;
  return GetRawCachingOptional(path, key, &ignored, found, allow_cache, cost, tx, error);
}

bool GroveDb::RootKey(std::vector<uint8_t>* out_key,
                      bool* found,
                      std::string* error) {
  return RootKey(out_key, found, nullptr, nullptr, error);
}

bool GroveDb::RootKey(std::vector<uint8_t>* out_key,
                      bool* found,
                      OperationCost* cost,
                      std::string* error) {
  return RootKey(out_key, found, cost, nullptr, error);
}

bool GroveDb::RootKey(std::vector<uint8_t>* out_key,
                      bool* found,
                      Transaction* tx,
                      std::string* error) {
  return RootKey(out_key, found, nullptr, tx, error);
}

bool GroveDb::RootKey(std::vector<uint8_t>* out_key,
                      bool* found,
                      OperationCost* cost,
                      Transaction* tx,
                      std::string* error) {
  if (out_key == nullptr || found == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  out_key->clear();
  *found = false;
  if (!opened_) {
    if (error) {
      *error = "database not opened";
    }
    return false;
  }
  if (tx != nullptr && tx->state == Transaction::State::kUninitialized) {
    if (error) {
      *error = "transaction is not active";
    }
    return false;
  }
  if (tx != nullptr && tx->state == Transaction::State::kCommitted) {
    if (error) {
      *error = "transaction is committed";
    }
    return false;
  }
  Transaction* read_tx = (tx != nullptr && tx->state == Transaction::State::kActive) ? tx : nullptr;
  MerkTree root_tree;
  RocksDbWrapper::Transaction* inner_tx = read_tx != nullptr ? &read_tx->inner : nullptr;
  std::vector<std::vector<uint8_t>> root_path;
  if (!LoadTreeForPath(&storage_, inner_tx, root_path, &root_tree, cost, error)) {
    return false;
  }
  // Accumulate cost for root_key operation
  if (cost != nullptr) {
    cost->seek_count += 1;
  }
  std::vector<uint8_t> root_key;
  const bool has_root_key = root_tree.RootKey(&root_key) && !root_key.empty();
  if (has_root_key) {
    *out_key = root_key;
  }
  *found = has_root_key;
  return true;
}

bool GroveDb::RootHash(std::vector<uint8_t>* out_hash,
                       std::string* error) {
  return RootHash(out_hash, nullptr, error);
}

bool GroveDb::RootHash(std::vector<uint8_t>* out_hash,
                       Transaction* tx,
                       std::string* error) {
  if (out_hash == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  out_hash->clear();
  if (!opened_) {
    if (error) {
      *error = "database not opened";
    }
    return false;
  }
  if (tx != nullptr && tx->state == Transaction::State::kUninitialized) {
    if (error) {
      *error = "transaction is not active";
    }
    return false;
  }
  if (tx != nullptr && tx->state == Transaction::State::kCommitted) {
    if (error) {
      *error = "transaction is committed";
    }
    return false;
  }
  Transaction* read_tx = (tx != nullptr && tx->state == Transaction::State::kActive) ? tx : nullptr;
  MerkTree root_tree;
  RocksDbWrapper::Transaction* inner_tx = read_tx != nullptr ? &read_tx->inner : nullptr;
  std::vector<std::vector<uint8_t>> root_path;
  if (!LoadTreeForPath(&storage_, inner_tx, root_path, &root_tree, nullptr, error)) {
    return false;
  }
  return root_tree.GetCachedRootHash(out_hash, error);
}

bool GroveDb::RootHashForVersion(const GroveVersion& version,
                                 std::vector<uint8_t>* out_hash,
                                 std::string* error) {
  return RootHashForVersion(version, out_hash, nullptr, error);
}

bool GroveDb::RootHashForVersion(const GroveVersion& version,
                                 std::vector<uint8_t>* out_hash,
                                 Transaction* tx,
                                 std::string* error) {
  if (!version.IsSupported()) {
    if (error) {
      *error = "unsupported grove version: " + version.ToString();
    }
    return false;
  }
  return RootHash(out_hash, tx, error);
}

bool GroveDb::VerifyGroveDb(bool verify_references,
                             bool allow_cache,
                             std::vector<VerificationIssue>* issues,
                             std::string* error) {
  return VerifyGroveDb(verify_references, allow_cache, issues, nullptr, error);
}

bool GroveDb::VerifyGroveDb(bool verify_references,
                             bool allow_cache,
                             std::vector<VerificationIssue>* issues,
                             Transaction* tx,
                             std::string* error) {
  if (issues == nullptr) {
    if (error) {
      *error = "issues output is null";
    }
    return false;
  }
  issues->clear();
  if (!opened_) {
    if (error) {
      *error = "database not opened";
    }
    return false;
  }
  if (tx != nullptr && tx->state == Transaction::State::kUninitialized) {
    if (error) {
      *error = "transaction is not active";
    }
    return false;
  }
  if (tx != nullptr && tx->state == Transaction::State::kCommitted) {
    if (error) {
      *error = "transaction is committed";
    }
    return false;
  }

  Transaction* read_tx = (tx != nullptr && tx->state == Transaction::State::kActive) ? tx : nullptr;
  RocksDbWrapper::Transaction* inner_tx = read_tx != nullptr ? &read_tx->inner : nullptr;

  std::vector<std::vector<uint8_t>> root_path;
  MerkTree root_tree;
  if (!LoadTreeForPath(&storage_, inner_tx, root_path, &root_tree, nullptr, error)) {
    return false;
  }

  return VerifyMerkAndSubmerks(&storage_, inner_tx, root_path, &root_tree,
                                verify_references, allow_cache, issues, error);
}

bool GroveDb::VerifyGroveDbForVersion(const GroveVersion& version,
                                       bool verify_references,
                                       bool allow_cache,
                                       std::vector<VerificationIssue>* issues,
                                       std::string* error) {
  return VerifyGroveDbForVersion(version, verify_references, allow_cache, issues, nullptr, error);
}

bool GroveDb::VerifyGroveDbForVersion(const GroveVersion& version,
                                       bool verify_references,
                                       bool allow_cache,
                                       std::vector<VerificationIssue>* issues,
                                       Transaction* tx,
                                       std::string* error) {
  if (!version.IsSupported()) {
    if (error) {
      *error = "unsupported grove version: " + version.ToString();
    }
    return false;
  }
  return VerifyGroveDb(verify_references, allow_cache, issues, tx, error);
}

bool GroveDb::VerifyMerkAndSubmerks(RocksDbWrapper* storage,
                                    RocksDbWrapper::Transaction* tx,
                                    const std::vector<std::vector<uint8_t>>& path,
                                    MerkTree* merk,
                                    bool verify_references,
                                    bool allow_cache,
                                    std::vector<VerificationIssue>* issues,
                                    std::string* error) {
  (void)verify_references;
  (void)allow_cache;

  RocksDbWrapper::PrefixedIterator iter;
  if (!iter.Init(storage, ColumnFamilyKind::kDefault, path, error)) {
    return false;
  }

  if (!iter.SeekToFirst(error)) {
    return false;
  }

  while (iter.Valid()) {
    std::vector<uint8_t> key;
    std::vector<uint8_t> element_value;

    if (!iter.Key(&key, error)) {
      return false;
    }
    if (!iter.Value(&element_value, error)) {
      return false;
    }

    uint64_t element_variant = element_value.empty() ? 0 : element_value[0];

    if (element_variant == 2 ||
        element_variant == 4 ||
        element_variant == 5 ||
        element_variant == 6 ||
        element_variant == 7 ||
        element_variant == 8 ||
        element_variant == 10) {
      std::vector<uint8_t> node_value;
      std::vector<uint8_t> node_value_hash;
      if (merk->GetValueAndValueHash(key, &node_value, &node_value_hash)) {
        std::vector<std::vector<uint8_t>> child_path = path;
        child_path.push_back(key);

        MerkTree child_tree;
        if (LoadTreeForPath(storage, tx, child_path, &child_tree, nullptr, error)) {
          std::vector<uint8_t> root_hash;
          if (child_tree.GetCachedRootHash(&root_hash, error)) {
            std::vector<uint8_t> computed_value_hash;
            if (merk->GetValueHashFn()) {
              if (!merk->GetValueHashFn()(key, node_value, &computed_value_hash, error)) {
                return false;
              }
            } else {
              if (!ValueHash(node_value, &computed_value_hash, error)) {
                return false;
              }
            }

            std::vector<uint8_t> combined_value_hash;
            if (!CombineHash(computed_value_hash, root_hash, &combined_value_hash, error)) {
              return false;
            }

            if (combined_value_hash != node_value_hash) {
              VerificationIssue issue;
              issue.path = child_path;
              issue.root_hash = root_hash;
              issue.expected_value_hash = combined_value_hash;
              issue.actual_value_hash = node_value_hash;
              issues->push_back(issue);
            }

            if (!VerifyMerkAndSubmerks(storage, tx, child_path, &child_tree,
                                        verify_references, true, issues, error)) {
              return false;
            }
          }
        }
      }
    }

    if (!iter.Next(error)) {
      return false;
    }
  }

  return true;
}

bool GroveDb::IsEmptyTree(const std::vector<std::vector<uint8_t>>& path,
                          bool* is_empty,
                          std::string* error) {
  return IsEmptyTree(path, is_empty, nullptr, error);
}

bool GroveDb::IsEmptyTree(const std::vector<std::vector<uint8_t>>& path,
                          bool* is_empty,
                          Transaction* tx,
                          std::string* error) {
  if (is_empty == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  *is_empty = false;
  if (!opened_) {
    if (error) {
      *error = "database not opened";
    }
    return false;
  }
  if (tx != nullptr && tx->state == Transaction::State::kUninitialized) {
    if (error) {
      *error = "transaction is not active";
    }
    return false;
  }
  if (tx != nullptr && tx->state == Transaction::State::kCommitted) {
    if (error) {
      *error = "transaction is committed";
    }
    return false;
  }
  Transaction* read_tx = (tx != nullptr && tx->state == Transaction::State::kActive) ? tx : nullptr;
  if (!EnsurePathTreeElements(path, read_tx, nullptr, error)) {
    return false;
  }

  MerkTree tree;
  RocksDbWrapper::Transaction* inner_tx = read_tx != nullptr ? &read_tx->inner : nullptr;
  if (!LoadTreeForPath(&storage_, inner_tx, path, &tree, nullptr, error)) {
    return false;
  }
  std::vector<uint8_t> root_key;
  *is_empty = !tree.RootKey(&root_key) || root_key.empty();
  return true;
}

bool GroveDb::CheckSubtreeExistsInvalidPath(
    const std::vector<std::vector<uint8_t>>& path,
    std::string* error) {
  return CheckSubtreeExistsInvalidPath(path, nullptr, error);
}

bool GroveDb::CheckSubtreeExistsInvalidPath(
    const std::vector<std::vector<uint8_t>>& path,
    Transaction* tx,
    std::string* error) {
  if (!opened_) {
    if (error) {
      *error = "database not opened";
    }
    return false;
  }
  if (tx != nullptr && tx->state == Transaction::State::kUninitialized) {
    if (error) {
      *error = "transaction is not active";
    }
    return false;
  }
  if (tx != nullptr && tx->state == Transaction::State::kCommitted) {
    if (error) {
      *error = "transaction is committed";
    }
    return false;
  }
  if (path.empty()) {
    return true;
  }

  Transaction* read_tx = (tx != nullptr && tx->state == Transaction::State::kActive) ? tx : nullptr;
  std::vector<std::vector<uint8_t>> parent_path(path.begin(), path.end() - 1);
  const std::vector<uint8_t>& parent_key = path.back();
  if (!EnsurePathTreeElements(parent_path, read_tx, nullptr, error)) {
    return false;
  }

  MerkTree parent_tree;
  RocksDbWrapper::Transaction* inner_tx = read_tx != nullptr ? &read_tx->inner : nullptr;
  if (!LoadTreeForPath(&storage_, inner_tx, parent_path, &parent_tree, nullptr, error)) {
    return false;
  }

  std::vector<uint8_t> parent_element;
  if (!parent_tree.Get(parent_key, &parent_element)) {
    if (error) {
      *error = "subtree doesn't exist";
    }
    return false;
  }
  uint64_t variant = 0;
  if (!DecodeElementVariant(parent_element, &variant, error)) {
    return false;
  }
  if (variant == 2 || variant == 4 || variant == 5 || variant == 6 || variant == 7) {
    return true;
  }
  if (error) {
    *error = "subtree doesn't exist";
  }
  return false;
}

bool GroveDb::CheckSubtreeExistsInvalidPathForVersion(
    const std::vector<std::vector<uint8_t>>& path,
    const GroveVersion& version,
    std::string* error) {
  return CheckSubtreeExistsInvalidPathForVersion(path, version, nullptr, error);
}

bool GroveDb::CheckSubtreeExistsInvalidPathForVersion(
    const std::vector<std::vector<uint8_t>>& path,
    const GroveVersion& version,
    Transaction* tx,
    std::string* error) {
  if (!version.IsSupported()) {
    if (error) {
      *error = "unsupported grove version: " + version.ToString();
    }
    return false;
  }
  return CheckSubtreeExistsInvalidPath(path, tx, error);
}

bool GroveDb::FindSubtrees(
    const std::vector<std::vector<uint8_t>>& path,
    std::vector<std::vector<std::vector<uint8_t>>>* out_subtrees,
    std::string* error) {
  return FindSubtrees(path, out_subtrees, nullptr, nullptr, error);
}

bool GroveDb::FindSubtrees(
    const std::vector<std::vector<uint8_t>>& path,
    std::vector<std::vector<std::vector<uint8_t>>>* out_subtrees,
    OperationCost* cost,
    std::string* error) {
  return FindSubtrees(path, out_subtrees, cost, nullptr, error);
}

bool GroveDb::FindSubtrees(
    const std::vector<std::vector<uint8_t>>& path,
    std::vector<std::vector<std::vector<uint8_t>>>* out_subtrees,
    Transaction* tx,
    std::string* error) {
  return FindSubtrees(path, out_subtrees, nullptr, tx, error);
}

bool GroveDb::FindSubtrees(
    const std::vector<std::vector<uint8_t>>& path,
    std::vector<std::vector<std::vector<uint8_t>>>* out_subtrees,
    OperationCost* cost,
    Transaction* tx,
    std::string* error) {
  if (cost != nullptr) {
    *cost = OperationCost();
  }
  if (out_subtrees == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  out_subtrees->clear();
  if (!opened_) {
    if (error) {
      *error = "database not opened";
    }
    return false;
  }
  if (tx != nullptr && tx->state == Transaction::State::kUninitialized) {
    if (error) {
      *error = "transaction is not active";
    }
    return false;
  }
  if (tx != nullptr && tx->state == Transaction::State::kCommitted) {
    if (error) {
      *error = "transaction is committed";
    }
    return false;
  }
  Transaction* read_tx = (tx != nullptr && tx->state == Transaction::State::kActive) ? tx : nullptr;
  OperationCost path_cost;
  OperationCost* path_cost_out = cost != nullptr ? &path_cost : nullptr;
  if (!EnsurePathTreeElements(path, read_tx, path_cost_out, error)) {
    return false;
  }
  if (cost != nullptr) {
    cost->Add(path_cost);
  }

  std::vector<std::vector<std::vector<uint8_t>>> queue;
  queue.push_back(path);
  if (!path.empty()) {
    out_subtrees->push_back(path);
  }
  RocksDbWrapper::Transaction* inner_tx = read_tx != nullptr ? &read_tx->inner : nullptr;

  while (!queue.empty()) {
    std::vector<std::vector<uint8_t>> current_path = queue.back();
    queue.pop_back();

    MerkTree tree;
    OperationCost load_cost;
    OperationCost* load_cost_out = cost != nullptr ? &load_cost : nullptr;
    if (!LoadTreeForPath(&storage_, inner_tx, current_path, &tree, load_cost_out, error)) {
      return false;
    }
    if (cost != nullptr) {
      cost->Add(load_cost);
    }

    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> entries;
    if (!tree.Export(&entries)) {
      if (error) {
        *error = "failed to iterate subtree entries";
      }
      return false;
    }

    for (const auto& entry : entries) {
      uint64_t variant = 0;
      if (!DecodeElementVariant(entry.second, &variant, error)) {
        return false;
      }
      if (!IsTreeElementVariant(variant)) {
        continue;
      }
      std::vector<std::vector<uint8_t>> child_path = current_path;
      child_path.push_back(entry.first);
      queue.push_back(child_path);
      out_subtrees->push_back(std::move(child_path));
    }
  }
  return true;
}

bool GroveDb::FindSubtreesByKinds(
    const std::vector<std::vector<uint8_t>>& path,
    std::vector<std::vector<std::vector<uint8_t>>>* out_subtrees,
    const std::vector<uint64_t>* tree_kinds_filter,
    std::string* error) {
  return FindSubtreesByKinds(path, out_subtrees, tree_kinds_filter, nullptr, nullptr, error);
}

bool GroveDb::FindSubtreesByKinds(
    const std::vector<std::vector<uint8_t>>& path,
    std::vector<std::vector<std::vector<uint8_t>>>* out_subtrees,
    const std::vector<uint64_t>* tree_kinds_filter,
    OperationCost* cost,
    std::string* error) {
  return FindSubtreesByKinds(path, out_subtrees, tree_kinds_filter, cost, nullptr, error);
}

bool GroveDb::FindSubtreesByKinds(
    const std::vector<std::vector<uint8_t>>& path,
    std::vector<std::vector<std::vector<uint8_t>>>* out_subtrees,
    const std::vector<uint64_t>* tree_kinds_filter,
    Transaction* tx,
    std::string* error) {
  return FindSubtreesByKinds(path, out_subtrees, tree_kinds_filter, nullptr, tx, error);
}

bool GroveDb::FindSubtreesByKinds(
    const std::vector<std::vector<uint8_t>>& path,
    std::vector<std::vector<std::vector<uint8_t>>>* out_subtrees,
    const std::vector<uint64_t>* tree_kinds_filter,
    OperationCost* cost,
    Transaction* tx,
    std::string* error) {
  if (cost != nullptr) {
    *cost = OperationCost();
  }
  if (out_subtrees == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  out_subtrees->clear();
  if (!opened_) {
    if (error) {
      *error = "database not opened";
    }
    return false;
  }
  if (tx != nullptr && tx->state == Transaction::State::kUninitialized) {
    if (error) {
      *error = "transaction is not active";
    }
    return false;
  }
  if (tx != nullptr && tx->state == Transaction::State::kCommitted) {
    if (error) {
      *error = "transaction is committed";
    }
    return false;
  }
  Transaction* read_tx = (tx != nullptr && tx->state == Transaction::State::kActive) ? tx : nullptr;
  OperationCost path_cost;
  OperationCost* path_cost_out = cost != nullptr ? &path_cost : nullptr;
  if (!EnsurePathTreeElements(path, read_tx, path_cost_out, error)) {
    return false;
  }
  if (cost != nullptr) {
    cost->Add(path_cost);
  }

  RocksDbWrapper::Transaction* inner_tx = read_tx != nullptr ? &read_tx->inner : nullptr;

  std::vector<std::vector<std::vector<uint8_t>>> queue;
  queue.push_back(path);

  // Only add root path to results if it passes the filter
  bool root_matches = true;
  if (tree_kinds_filter != nullptr && !tree_kinds_filter->empty() && !path.empty()) {
    std::vector<std::vector<uint8_t>> parent_path(path.begin(), path.end() - 1);
    MerkTree parent_tree;
    OperationCost parent_cost;
    OperationCost* parent_cost_out = cost != nullptr ? &parent_cost : nullptr;
    if (!LoadTreeForPath(&storage_, inner_tx, parent_path, &parent_tree, parent_cost_out, error)) {
      return false;
    }
    if (cost != nullptr) {
      cost->Add(parent_cost);
    }
    std::vector<uint8_t> element_bytes;
    if (!parent_tree.Get(path.back(), &element_bytes) || element_bytes.empty()) {
      if (error) {
        *error = "failed to get root path element for filter check";
      }
      return false;
    }
    uint64_t variant = 0;
    if (!DecodeElementVariant(element_bytes, &variant, error)) {
      return false;
    }
    root_matches = false;
    for (uint64_t allowed_kind : *tree_kinds_filter) {
      if (variant == allowed_kind) {
        root_matches = true;
        break;
      }
    }
  }
  if (root_matches && !path.empty()) {
    out_subtrees->push_back(path);
  }

  while (!queue.empty()) {
    std::vector<std::vector<uint8_t>> current_path = queue.back();
    queue.pop_back();

    MerkTree tree;
    OperationCost load_cost;
    OperationCost* load_cost_out = cost != nullptr ? &load_cost : nullptr;
    if (!LoadTreeForPath(&storage_, inner_tx, current_path, &tree, load_cost_out, error)) {
      return false;
    }
    if (cost != nullptr) {
      cost->Add(load_cost);
    }

    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> entries;
    if (!tree.Export(&entries)) {
      if (error) {
        *error = "failed to iterate subtree entries";
      }
      return false;
    }

    for (const auto& entry : entries) {
      uint64_t variant = 0;
      if (!DecodeElementVariant(entry.second, &variant, error)) {
        return false;
      }
      if (!IsTreeElementVariant(variant)) {
        continue;
      }
      std::vector<std::vector<uint8_t>> child_path = current_path;
      child_path.push_back(entry.first);
      // Always enqueue for traversal (children may match even if parent doesn't)
      queue.push_back(child_path);
      // Only add to results if filter matches
      if (tree_kinds_filter != nullptr && !tree_kinds_filter->empty()) {
        bool matches_filter = false;
        for (uint64_t allowed_kind : *tree_kinds_filter) {
          if (variant == allowed_kind) {
            matches_filter = true;
            break;
          }
        }
        if (!matches_filter) {
          continue;
        }
      }
      out_subtrees->push_back(std::move(child_path));
    }
  }
  return true;
}

bool GroveDb::CountSubtrees(
    const std::vector<std::vector<uint8_t>>& path,
    uint64_t* out_count,
    std::string* error) {
  return CountSubtrees(path, out_count, nullptr, nullptr, error);
}

bool GroveDb::CountSubtrees(
    const std::vector<std::vector<uint8_t>>& path,
    uint64_t* out_count,
    OperationCost* cost,
    std::string* error) {
  return CountSubtrees(path, out_count, cost, nullptr, error);
}

bool GroveDb::CountSubtrees(
    const std::vector<std::vector<uint8_t>>& path,
    uint64_t* out_count,
    Transaction* tx,
    std::string* error) {
  return CountSubtrees(path, out_count, nullptr, tx, error);
}

bool GroveDb::CountSubtrees(
    const std::vector<std::vector<uint8_t>>& path,
    uint64_t* out_count,
    OperationCost* cost,
    Transaction* tx,
    std::string* error) {
  if (cost != nullptr) {
    *cost = OperationCost();
  }
  if (out_count == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  *out_count = 0;
  if (!opened_) {
    if (error) {
      *error = "database not opened";
    }
    return false;
  }
  if (tx != nullptr && tx->state == Transaction::State::kUninitialized) {
    if (error) {
      *error = "transaction is not active";
    }
    return false;
  }
  if (tx != nullptr && tx->state == Transaction::State::kCommitted) {
    if (error) {
      *error = "transaction is committed";
    }
    return false;
  }
  Transaction* read_tx = (tx != nullptr && tx->state == Transaction::State::kActive) ? tx : nullptr;
  OperationCost path_cost;
  OperationCost* path_cost_out = cost != nullptr ? &path_cost : nullptr;
  if (!EnsurePathTreeElements(path, read_tx, path_cost_out, error)) {
    return false;
  }
  if (cost != nullptr) {
    cost->Add(path_cost);
  }

  std::vector<std::vector<std::vector<uint8_t>>> queue;
  queue.push_back(path);
  RocksDbWrapper::Transaction* inner_tx = read_tx != nullptr ? &read_tx->inner : nullptr;

  while (!queue.empty()) {
    std::vector<std::vector<uint8_t>> current_path = queue.back();
    queue.pop_back();

    MerkTree tree;
    OperationCost load_cost;
    OperationCost* load_cost_out = cost != nullptr ? &load_cost : nullptr;
    if (!LoadTreeForPath(&storage_, inner_tx, current_path, &tree, load_cost_out, error)) {
      return false;
    }
    if (cost != nullptr) {
      cost->Add(load_cost);
    }

    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> entries;
    if (!tree.Export(&entries)) {
      if (error) {
        *error = "failed to iterate subtree entries";
      }
      return false;
    }

    for (const auto& entry : entries) {
      uint64_t variant = 0;
      if (!DecodeElementVariant(entry.second, &variant, error)) {
        return false;
      }
      if (!IsTreeElementVariant(variant)) {
        continue;
      }
      *out_count = *out_count + 1;
      std::vector<std::vector<uint8_t>> child_path = current_path;
      child_path.push_back(entry.first);
      queue.push_back(child_path);
    }
  }
  return true;
}

bool GroveDb::CountSubtreesForVersion(
    const std::vector<std::vector<uint8_t>>& path,
    const GroveVersion& version,
    uint64_t* out_count,
    std::string* error) {
  return CountSubtreesForVersion(path, version, out_count, nullptr, error);
}

bool GroveDb::CountSubtreesForVersion(
    const std::vector<std::vector<uint8_t>>& path,
    const GroveVersion& version,
    uint64_t* out_count,
    Transaction* tx,
    std::string* error) {
  if (!version.IsSupported()) {
    if (error) {
      *error = "unsupported grove version: " + version.ToString();
    }
    return false;
  }
  return CountSubtrees(path, out_count, tx, error);
}

bool GroveDb::GetSubtreeRoot(
    const std::vector<std::vector<uint8_t>>& path,
    std::vector<uint8_t>* out_root_key,
    std::vector<uint8_t>* out_element_bytes,
    std::string* error) {
  return GetSubtreeRoot(path, out_root_key, out_element_bytes, nullptr, nullptr, error);
}

bool GroveDb::GetSubtreeRoot(
    const std::vector<std::vector<uint8_t>>& path,
    std::vector<uint8_t>* out_root_key,
    std::vector<uint8_t>* out_element_bytes,
    OperationCost* cost,
    std::string* error) {
  return GetSubtreeRoot(path, out_root_key, out_element_bytes, cost, nullptr, error);
}

bool GroveDb::GetSubtreeRoot(
    const std::vector<std::vector<uint8_t>>& path,
    std::vector<uint8_t>* out_root_key,
    std::vector<uint8_t>* out_element_bytes,
    Transaction* tx,
    std::string* error) {
  return GetSubtreeRoot(path, out_root_key, out_element_bytes, nullptr, tx, error);
}

bool GroveDb::GetSubtreeRoot(
    const std::vector<std::vector<uint8_t>>& path,
    std::vector<uint8_t>* out_root_key,
    std::vector<uint8_t>* out_element_bytes,
    OperationCost* cost,
    Transaction* tx,
    std::string* error) {
  if (cost != nullptr) {
    *cost = OperationCost();
  }
  if (out_root_key == nullptr || out_element_bytes == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  out_root_key->clear();
  out_element_bytes->clear();
  if (!opened_) {
    if (error) {
      *error = "database not opened";
    }
    return false;
  }
  if (tx != nullptr && tx->state == Transaction::State::kUninitialized) {
    if (error) {
      *error = "transaction is not active";
    }
    return false;
  }
  if (tx != nullptr && tx->state == Transaction::State::kCommitted) {
    if (error) {
      *error = "transaction is committed";
    }
    return false;
  }
  Transaction* read_tx = (tx != nullptr && tx->state == Transaction::State::kActive) ? tx : nullptr;
  OperationCost path_cost;
  OperationCost* path_cost_out = cost != nullptr ? &path_cost : nullptr;
  if (!EnsurePathTreeElements(path, read_tx, path_cost_out, error)) {
    return false;
  }
  if (cost != nullptr) {
    cost->Add(path_cost);
  }

  // Load the merk tree at the given path
  MerkTree tree;
  OperationCost load_cost;
  OperationCost* load_cost_out = cost != nullptr ? &load_cost : nullptr;
  RocksDbWrapper::Transaction* inner_tx = read_tx != nullptr ? &read_tx->inner : nullptr;
  if (!LoadTreeForPath(&storage_, inner_tx, path, &tree, load_cost_out, error)) {
    return false;
  }
  if (cost != nullptr) {
    cost->Add(load_cost);
  }

  // Get the root key and element from the merk tree
  std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> entries;
  if (!tree.Export(&entries)) {
    if (error) {
      *error = "failed to iterate subtree entries";
    }
    return false;
  }

  // The root element is the first tree element in the merk
  // For a subtree, the root key is the last component of the path
  // and the element is the tree element that points to this subtree
  if (path.empty()) {
    if (error) {
      *error = "path is empty - use RootKey for root merk";
    }
    return false;
  }

  // The root key of the subtree is the last path component
  *out_root_key = path.back();

  // We need to find the parent and get the element that points to this subtree
  // For the root-level subtrees, we look in the root merk
  std::vector<std::vector<uint8_t>> parent_path;
  for (size_t i = 0; i < path.size() - 1; ++i) {
    parent_path.push_back(path[i]);
  }

  MerkTree parent_tree;
  OperationCost parent_load_cost;
  OperationCost* parent_load_cost_out = cost != nullptr ? &parent_load_cost : nullptr;
  if (!LoadTreeForPath(&storage_, inner_tx, parent_path, &parent_tree, parent_load_cost_out, error)) {
    return false;
  }
  if (cost != nullptr) {
    cost->Add(parent_load_cost);
  }

  std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> parent_entries;
  if (!parent_tree.Export(&parent_entries)) {
    if (error) {
      *error = "failed to iterate parent entries";
    }
    return false;
  }

  // Find the element for the subtree key
  const uint8_t* subtree_key = path.back().data();
  size_t subtree_key_size = path.back().size();
  for (const auto& entry : parent_entries) {
    if (entry.first.size() == subtree_key_size &&
        std::memcmp(entry.first.data(), subtree_key, subtree_key_size) == 0) {
      *out_element_bytes = entry.second;
      return true;
    }
  }

  if (error) {
    *error = "subtree root element not found in parent";
  }
  return false;
}

bool GroveDb::IsEmptyTreeForVersion(const std::vector<std::vector<uint8_t>>& path,
                                    const GroveVersion& version,
                                    bool* is_empty,
                                    std::string* error) {
  return IsEmptyTreeForVersion(path, version, is_empty, nullptr, error);
}

bool GroveDb::IsEmptyTreeForVersion(const std::vector<std::vector<uint8_t>>& path,
                                    const GroveVersion& version,
                                    bool* is_empty,
                                    Transaction* tx,
                                    std::string* error) {
  if (!version.IsSupported()) {
    if (error) {
      *error = "unsupported grove version: " + version.ToString();
    }
    return false;
  }
  return IsEmptyTree(path, is_empty, tx, error);
}

bool GroveDb::DeleteIfEmptyTreeForVersion(const std::vector<std::vector<uint8_t>>& path,
                                          const std::vector<uint8_t>& key,
                                          const GroveVersion& version,
                                          bool* deleted,
                                          std::string* error) {
  return DeleteIfEmptyTreeForVersion(path, key, version, deleted, nullptr, error);
}

bool GroveDb::DeleteIfEmptyTreeForVersion(const std::vector<std::vector<uint8_t>>& path,
                                          const std::vector<uint8_t>& key,
                                          const GroveVersion& version,
                                          bool* deleted,
                                          Transaction* tx,
                                          std::string* error) {
  if (!version.IsSupported()) {
    if (error) {
      *error = "unsupported grove version: " + version.ToString();
    }
    return false;
  }
  return DeleteIfEmptyTree(path, key, deleted, tx, error);
}

bool GroveDb::PutAux(const std::vector<uint8_t>& key,
                     const std::vector<uint8_t>& value,
                     std::string* error) {
  return PutAux(key, value, nullptr, error);
}

bool GroveDb::PutAux(const std::vector<uint8_t>& key,
                     const std::vector<uint8_t>& value,
                     Transaction* tx,
                     std::string* error) {
  if (!opened_) {
    if (error) {
      *error = "database not opened";
    }
    return false;
  }
  if (key.empty()) {
    if (error) {
      *error = "key is empty";
    }
    return false;
  }
  if (tx != nullptr && tx->state != Transaction::State::kActive) {
    if (error) {
      *error = "transaction is not active";
    }
    return false;
  }
  if (tx != nullptr) {
    return tx->inner.Put(ColumnFamilyKind::kAux, {}, key, value, error);
  }
  return storage_.Put(ColumnFamilyKind::kAux, {}, key, value, error);
}

bool GroveDb::GetAux(const std::vector<uint8_t>& key,
                     std::vector<uint8_t>* value,
                     bool* found,
                     std::string* error) {
  return GetAux(key, value, found, nullptr, error);
}

bool GroveDb::GetAux(const std::vector<uint8_t>& key,
                     std::vector<uint8_t>* value,
                     bool* found,
                     Transaction* tx,
                     std::string* error) {
  if (value == nullptr || found == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  *found = false;
  value->clear();
  if (!opened_) {
    if (error) {
      *error = "database not opened";
    }
    return false;
  }
  if (key.empty()) {
    if (error) {
      *error = "key is empty";
    }
    return false;
  }
  if (tx != nullptr && tx->state != Transaction::State::kActive) {
    if (error) {
      *error = "transaction is not active";
    }
    return false;
  }
  if (tx != nullptr) {
    return tx->inner.Get(ColumnFamilyKind::kAux, {}, key, value, found, error);
  }
  return storage_.Get(ColumnFamilyKind::kAux, {}, key, value, found, error);
}

bool GroveDb::DeleteAux(const std::vector<uint8_t>& key,
                        bool* deleted,
                        std::string* error) {
  return DeleteAux(key, deleted, nullptr, error);
}

bool GroveDb::DeleteAux(const std::vector<uint8_t>& key,
                        bool* deleted,
                        Transaction* tx,
                        std::string* error) {
  if (deleted == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  *deleted = false;
  if (!opened_) {
    if (error) {
      *error = "database not opened";
    }
    return false;
  }
  if (key.empty()) {
    if (error) {
      *error = "key is empty";
    }
    return false;
  }
  if (tx != nullptr && tx->state != Transaction::State::kActive) {
    if (error) {
      *error = "transaction is not active";
    }
    return false;
  }
  if (tx != nullptr) {
    return tx->inner.Delete(ColumnFamilyKind::kAux, {}, key, error);
  }
  return storage_.Delete(ColumnFamilyKind::kAux, {}, key, deleted, error);
}

bool GroveDb::HasAux(const std::vector<uint8_t>& key,
                     bool* exists,
                     std::string* error) {
  return HasAux(key, exists, nullptr, error);
}

bool GroveDb::HasAux(const std::vector<uint8_t>& key,
                     bool* exists,
                     Transaction* tx,
                     std::string* error) {
  if (exists == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  *exists = false;
  if (!opened_) {
    if (error) {
      *error = "database not opened";
    }
    return false;
  }
  if (key.empty()) {
    if (error) {
      *error = "key is empty";
    }
    return false;
  }
  if (tx != nullptr && tx->state != Transaction::State::kActive) {
    if (error) {
      *error = "transaction is not active";
    }
    return false;
  }
  // HasAux is implemented using GetAux and checking the found flag
  std::vector<uint8_t> ignored_value;
  if (tx != nullptr) {
    return tx->inner.Get(ColumnFamilyKind::kAux, {}, key, &ignored_value, exists, error);
  }
  return storage_.Get(ColumnFamilyKind::kAux, {}, key, &ignored_value, exists, error);
}

bool GroveDb::QueryRange(
    const std::vector<std::vector<uint8_t>>& path,
    const std::vector<uint8_t>& start_key,
    const std::vector<uint8_t>& end_key,
    bool start_inclusive,
    bool end_inclusive,
    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>>* out,
    std::string* error) {
  return QueryRange(
      path, start_key, end_key, start_inclusive, end_inclusive, out, nullptr, error);
}

bool GroveDb::QueryRange(
    const std::vector<std::vector<uint8_t>>& path,
    const std::vector<uint8_t>& start_key,
    const std::vector<uint8_t>& end_key,
    bool start_inclusive,
    bool end_inclusive,
    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>>* out,
    Transaction* tx,
    std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  out->clear();
  if (!opened_) {
    if (error) {
      *error = "database not opened";
    }
    return false;
  }
  if (tx != nullptr && tx->state == Transaction::State::kUninitialized) {
    if (error) {
      *error = "transaction is not active";
    }
    return false;
  }
  if (tx != nullptr && tx->state == Transaction::State::kCommitted) {
    if (error) {
      *error = "transaction is committed";
    }
    return false;
  }
  if (!end_key.empty() && !start_key.empty() && end_key < start_key) {
    if (error) {
      *error = "range end precedes start";
    }
    return false;
  }
  Transaction* read_tx = (tx != nullptr && tx->state == Transaction::State::kActive) ? tx : nullptr;
  if (!EnsurePathTreeElements(path, read_tx, nullptr, error)) {
    return false;
  }

  MerkTree tree;
  RocksDbWrapper::Transaction* inner_tx = read_tx != nullptr ? &read_tx->inner : nullptr;
  if (!LoadTreeForPath(&storage_, inner_tx, path, &tree, nullptr, error)) {
    return false;
  }
  std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> entries;
  if (!tree.Export(&entries)) {
    if (error) {
      *error = "range export failed";
    }
    return false;
  }
  out->reserve(entries.size());
  for (const auto& kv : entries) {
    const auto& key = kv.first;
    if (!start_key.empty()) {
      if (start_inclusive) {
        if (key < start_key) {
          continue;
        }
      } else {
        if (key <= start_key) {
          continue;
        }
      }
    }
    if (!end_key.empty()) {
      if (end_inclusive) {
        if (key > end_key) {
          continue;
        }
      } else {
        if (key >= end_key) {
          continue;
        }
      }
    }
    out->push_back(kv);
  }
  return true;
}

bool GroveDb::QueryRaw(
    const PathQuery& path_query,
    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>>* out,
    std::string* error) {
  return QueryRaw(path_query, out, nullptr, error);
}

bool GroveDb::QueryRaw(
    const PathQuery& path_query,
    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>>* out,
    Transaction* tx,
    std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  out->clear();
  if (!opened_) {
    if (error) {
      *error = "database not opened";
    }
    return false;
  }
  if (tx != nullptr && tx->state == Transaction::State::kUninitialized) {
    if (error) {
      *error = "transaction is not active";
    }
    return false;
  }
  if (tx != nullptr && tx->state == Transaction::State::kCommitted) {
    if (error) {
      *error = "transaction is committed";
    }
    return false;
  }

  if (path_query.query.query.items.empty()) {
    if (error) {
      *error = "query items are empty";
    }
    return false;
  }
  if (path_query.query.query.default_subquery_branch.subquery_path.has_value() ||
      path_query.query.query.default_subquery_branch.subquery != nullptr ||
      path_query.query.query.conditional_subquery_branches.has_value() ||
      path_query.query.query.add_parent_tree_on_subquery) {
    if (error) {
      *error = "unsupported query shape for query_raw";
    }
    return false;
  }

  Transaction* read_tx = (tx != nullptr && tx->state == Transaction::State::kActive) ? tx : nullptr;
  if (!EnsurePathTreeElements(path_query.path, read_tx, nullptr, error)) {
    return false;
  }
  MerkTree tree;
  RocksDbWrapper::Transaction* inner_tx = read_tx != nullptr ? &read_tx->inner : nullptr;
  if (!LoadTreeForPath(&storage_, inner_tx, path_query.path, &tree, nullptr, error)) {
    return false;
  }

  std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> entries;
  if (!tree.Export(&entries)) {
    if (error) {
      *error = "query export failed";
    }
    return false;
  }

  size_t offset = path_query.query.offset.value_or(0);
  const size_t limit = path_query.query.limit.has_value()
                           ? static_cast<size_t>(path_query.query.limit.value())
                           : std::numeric_limits<size_t>::max();
  out->reserve(entries.size());

  auto append_if_match = [&](const std::pair<std::vector<uint8_t>, std::vector<uint8_t>>& kv) {
    bool matches = false;
    for (const auto& item : path_query.query.query.items) {
      if (item.Contains(kv.first)) {
        matches = true;
        break;
      }
    }
    if (!matches) {
      return;
    }
    if (offset > 0) {
      --offset;
      return;
    }
    if (out->size() >= limit) {
      return;
    }
    out->push_back(kv);
  };

  if (path_query.query.query.left_to_right) {
    for (const auto& kv : entries) {
      append_if_match(kv);
      if (out->size() >= limit) {
        break;
      }
    }
  } else {
    for (size_t i = entries.size(); i > 0; --i) {
      append_if_match(entries[i - 1]);
      if (out->size() >= limit) {
        break;
      }
    }
  }

  return true;
}

bool GroveDb::QueryKeyElementPairs(const PathQuery& path_query,
                                   std::vector<KeyElementPair>* out,
                                   std::string* error) {
  return QueryKeyElementPairs(path_query, out, nullptr, error);
}

bool GroveDb::QueryKeyElementPairs(const PathQuery& path_query,
                                   std::vector<KeyElementPair>* out,
                                   Transaction* tx,
                                   std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  out->clear();
  if (!opened_) {
    if (error) {
      *error = "database not opened";
    }
    return false;
  }
  if (tx != nullptr && tx->state == Transaction::State::kUninitialized) {
    if (error) {
      *error = "transaction is not active";
    }
    return false;
  }
  if (tx != nullptr && tx->state == Transaction::State::kCommitted) {
    if (error) {
      *error = "transaction is committed";
    }
    return false;
  }

  if (path_query.query.query.items.empty()) {
    if (error) {
      *error = "query items are empty";
    }
    return false;
  }
  if (path_query.query.query.default_subquery_branch.subquery_path.has_value() ||
      path_query.query.query.default_subquery_branch.subquery != nullptr ||
      path_query.query.query.conditional_subquery_branches.has_value() ||
      path_query.query.query.add_parent_tree_on_subquery) {
    if (error) {
      *error = "unsupported query shape for query_key_element_pairs";
    }
    return false;
  }

  Transaction* read_tx = (tx != nullptr && tx->state == Transaction::State::kActive) ? tx : nullptr;
  if (!EnsurePathTreeElements(path_query.path, read_tx, nullptr, error)) {
    return false;
  }
  MerkTree tree;
  RocksDbWrapper::Transaction* inner_tx = read_tx != nullptr ? &read_tx->inner : nullptr;
  if (!LoadTreeForPath(&storage_, inner_tx, path_query.path, &tree, nullptr, error)) {
    return false;
  }

  std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> entries;
  if (!tree.Export(&entries)) {
    if (error) {
      *error = "query export failed";
    }
    return false;
  }

  size_t offset = path_query.query.offset.value_or(0);
  const size_t limit = path_query.query.limit.has_value()
                           ? static_cast<size_t>(path_query.query.limit.value())
                           : std::numeric_limits<size_t>::max();
  out->reserve(entries.size());

  if (path_query.query.query.left_to_right) {
    for (const auto& kv : entries) {
      bool matches = false;
      for (const auto& item : path_query.query.query.items) {
        if (item.Contains(kv.first)) {
          matches = true;
          break;
        }
      }
      if (!matches) {
        continue;
      }
      if (offset > 0) {
        --offset;
        continue;
      }
      if (out->size() >= limit) {
        break;
      }

      // Resolve references to get the final element bytes
      std::vector<uint8_t> resolved_element_bytes;
      bool resolved_found = false;
      if (!Get(path_query.path, kv.first, &resolved_element_bytes, &resolved_found, tx, error)) {
        return false;
      }
      if (resolved_found) {
        KeyElementPair pair;
        pair.key = kv.first;
        pair.element_bytes = std::move(resolved_element_bytes);
        out->push_back(std::move(pair));
      }
    }
  } else {
    for (size_t i = entries.size(); i > 0; --i) {
      const auto& kv = entries[i - 1];
      bool matches = false;
      for (const auto& item : path_query.query.query.items) {
        if (item.Contains(kv.first)) {
          matches = true;
          break;
        }
      }
      if (!matches) {
        continue;
      }
      if (offset > 0) {
        --offset;
        continue;
      }
      if (out->size() >= limit) {
        break;
      }

      // Resolve references to get the final element bytes
      std::vector<uint8_t> resolved_element_bytes;
      bool resolved_found = false;
      if (!Get(path_query.path, kv.first, &resolved_element_bytes, &resolved_found, tx, error)) {
        return false;
      }
      if (resolved_found) {
        KeyElementPair pair;
        pair.key = kv.first;
        pair.element_bytes = std::move(resolved_element_bytes);
        out->push_back(std::move(pair));
      }
    }
  }

  return true;
}

bool GroveDb::QueryRawKeysOptional(const PathQuery& path_query,
                                   std::vector<PathKeyOptionalElement>* out,
                                   std::string* error) {
  return QueryRawKeysOptional(path_query, out, nullptr, error);
}

bool GroveDb::QueryKeysOptional(const PathQuery& path_query,
                                std::vector<PathKeyOptionalElement>* out,
                                std::string* error) {
  return QueryKeysOptional(path_query, out, nullptr, error);
}

bool GroveDb::QueryKeysOptional(const PathQuery& path_query,
                                std::vector<PathKeyOptionalElement>* out,
                                Transaction* tx,
                                std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  out->clear();
  if (!opened_) {
    if (error) {
      *error = "database not opened";
    }
    return false;
  }
  if (!path_query.query.limit.has_value()) {
    if (error) {
      *error = "limits must be set in query_keys_optional";
    }
    return false;
  }
  if (path_query.query.offset.has_value()) {
    if (error) {
      *error = "offsets are not supported in query_raw_keys_optional";
    }
    return false;
  }

  if (path_query.query.query.items.empty()) {
    if (error) {
      *error = "query items are empty";
    }
    return false;
  }
  if (path_query.query.query.default_subquery_branch.subquery_path.has_value() ||
      path_query.query.query.default_subquery_branch.subquery != nullptr ||
      path_query.query.query.conditional_subquery_branches.has_value() ||
      path_query.query.query.add_parent_tree_on_subquery) {
    if (error) {
      *error = "unsupported query shape for query_keys_optional";
    }
    return false;
  }
  for (const auto& item : path_query.query.query.items) {
    if (!item.IsKey()) {
      if (error) {
        *error = "unsupported query item for query_keys_optional";
      }
      return false;
    }
  }

  std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> key_element_pairs;
  if (!QueryRaw(path_query, &key_element_pairs, tx, error)) {
    return false;
  }

  std::map<std::vector<uint8_t>, std::vector<uint8_t>> elements_by_key;
  for (auto& pair : key_element_pairs) {
    elements_by_key.emplace(std::move(pair.first), std::move(pair.second));
  }

  const size_t max_results = static_cast<size_t>(path_query.query.limit.value());
  out->reserve(max_results);
  std::vector<std::vector<uint8_t>> terminal_keys;
  terminal_keys.reserve(path_query.query.query.items.size());
  for (const auto& item : path_query.query.query.items) {
    terminal_keys.push_back(item.start);
  }
  std::sort(terminal_keys.begin(), terminal_keys.end());
  terminal_keys.erase(std::unique(terminal_keys.begin(), terminal_keys.end()), terminal_keys.end());
  if (!path_query.query.query.left_to_right) {
    std::reverse(terminal_keys.begin(), terminal_keys.end());
  }
  for (const auto& key : terminal_keys) {
    if (out->size() >= max_results) {
      break;
    }
    PathKeyOptionalElement row;
    row.path = path_query.path;
    row.key = key;
    auto it = elements_by_key.find(row.key);
    if (it != elements_by_key.end()) {
      row.element_found = true;
      std::vector<uint8_t> resolved_element_bytes;
      bool resolved_found = false;
      if (!Get(row.path, row.key, &resolved_element_bytes, &resolved_found, tx, error)) {
        return false;
      }
      if (resolved_found) {
        row.element_bytes = std::move(resolved_element_bytes);
      } else {
        row.element_found = false;
      }
    }
    out->push_back(std::move(row));
  }
  return true;
}

bool GroveDb::QueryRawKeysOptional(const PathQuery& path_query,
                                   std::vector<PathKeyOptionalElement>* out,
                                   Transaction* tx,
                                   std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  out->clear();
  if (!opened_) {
    if (error) {
      *error = "database not opened";
    }
    return false;
  }
  if (!path_query.query.limit.has_value()) {
    if (error) {
      *error = "limits must be set in query_raw_keys_optional";
    }
    return false;
  }
  if (path_query.query.offset.has_value()) {
    if (error) {
      *error = "offsets are not supported in query_raw_keys_optional";
    }
    return false;
  }

  if (path_query.query.query.items.empty()) {
    if (error) {
      *error = "query items are empty";
    }
    return false;
  }
  if (path_query.query.query.default_subquery_branch.subquery_path.has_value() ||
      path_query.query.query.default_subquery_branch.subquery != nullptr ||
      path_query.query.query.conditional_subquery_branches.has_value() ||
      path_query.query.query.add_parent_tree_on_subquery) {
    if (error) {
      *error = "unsupported query shape for query_raw_keys_optional";
    }
    return false;
  }
  for (const auto& item : path_query.query.query.items) {
    if (!item.IsKey()) {
      if (error) {
        *error = "unsupported query item for query_raw_keys_optional";
      }
      return false;
    }
  }

  std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> key_element_pairs;
  if (!QueryRaw(path_query, &key_element_pairs, tx, error)) {
    return false;
  }

  std::map<std::vector<uint8_t>, std::vector<uint8_t>> elements_by_key;
  for (auto& pair : key_element_pairs) {
    elements_by_key.emplace(std::move(pair.first), std::move(pair.second));
  }

  const size_t max_results = static_cast<size_t>(path_query.query.limit.value());
  out->reserve(max_results);
  std::vector<std::vector<uint8_t>> terminal_keys;
  terminal_keys.reserve(path_query.query.query.items.size());
  for (const auto& item : path_query.query.query.items) {
    terminal_keys.push_back(item.start);
  }
  std::sort(terminal_keys.begin(), terminal_keys.end());
  terminal_keys.erase(std::unique(terminal_keys.begin(), terminal_keys.end()), terminal_keys.end());
  if (!path_query.query.query.left_to_right) {
    std::reverse(terminal_keys.begin(), terminal_keys.end());
  }
  for (const auto& key : terminal_keys) {
    if (out->size() >= max_results) {
      break;
    }
    PathKeyOptionalElement row;
    row.path = path_query.path;
    row.key = key;
    auto it = elements_by_key.find(row.key);
    if (it != elements_by_key.end()) {
      row.element_found = true;
      row.element_bytes = it->second;
    }
    out->push_back(std::move(row));
  }
  return true;
}

bool GroveDb::QueryItemValue(const PathQuery& path_query,
                             std::vector<std::vector<uint8_t>>* out_values,
                             std::string* error) {
  return QueryItemValue(path_query, out_values, nullptr, error);
}

bool GroveDb::QueryItemValue(const PathQuery& path_query,
                             std::vector<std::vector<uint8_t>>* out_values,
                             Transaction* tx,
                             std::string* error) {
  if (out_values == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  out_values->clear();
  if (!opened_) {
    if (error) {
      *error = "database not opened";
    }
    return false;
  }
  if (tx != nullptr && tx->state == Transaction::State::kUninitialized) {
    if (error) {
      *error = "transaction is not active";
    }
    return false;
  }
  if (tx != nullptr && tx->state == Transaction::State::kCommitted) {
    if (error) {
      *error = "transaction is committed";
    }
    return false;
  }

  std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> entries;
  if (!QueryRaw(path_query, &entries, tx, error)) {
    return false;
  }

  out_values->reserve(entries.size());
  for (const auto& kv : entries) {
    const std::vector<uint8_t>& key = kv.first;
    const std::vector<uint8_t>& element_bytes = kv.second;
    uint64_t variant = 0;
    if (!DecodeElementVariant(element_bytes, &variant, error)) {
      return false;
    }

    if (variant == 0) {
      ElementItem item;
      if (!DecodeItemFromElementBytes(element_bytes, &item, error)) {
        return false;
      }
      out_values->push_back(std::move(item.value));
      continue;
    }
    if (variant == 9) {
      ElementItemWithSum item;
      if (!DecodeItemWithSumItemFromElementBytes(element_bytes, &item, error)) {
        return false;
      }
      out_values->push_back(std::move(item.value));
      continue;
    }
    if (variant == 3) {
      ElementSumItem sum_item;
      if (!DecodeSumItemFromElementBytes(element_bytes, &sum_item, error)) {
        return false;
      }
      std::vector<uint8_t> encoded_sum;
      AppendVarint(ZigZagEncodeI64(sum_item.sum), &encoded_sum);
      out_values->push_back(std::move(encoded_sum));
      continue;
    }
    if (variant == 1) {
      std::vector<uint8_t> resolved;
      bool found = false;
      if (!Get(path_query.path, key, &resolved, &found, tx, error)) {
        return false;
      }
      if (!found) {
        if (error) {
          *error = "the reference must result in an item";
        }
        return false;
      }
      uint64_t resolved_variant = 0;
      if (!DecodeElementVariant(resolved, &resolved_variant, error)) {
        return false;
      }
      if (resolved_variant == 0) {
        ElementItem item;
        if (!DecodeItemFromElementBytes(resolved, &item, error)) {
          return false;
        }
        out_values->push_back(std::move(item.value));
        continue;
      }
      if (resolved_variant == 9) {
        ElementItemWithSum item;
        if (!DecodeItemWithSumItemFromElementBytes(resolved, &item, error)) {
          return false;
        }
        out_values->push_back(std::move(item.value));
        continue;
      }
      if (resolved_variant == 3) {
        ElementSumItem sum_item;
        if (!DecodeSumItemFromElementBytes(resolved, &sum_item, error)) {
          return false;
        }
        std::vector<uint8_t> encoded_sum;
        AppendVarint(ZigZagEncodeI64(sum_item.sum), &encoded_sum);
        out_values->push_back(std::move(encoded_sum));
        continue;
      }
      if (error) {
        *error = "the reference must result in an item";
      }
      return false;
    }

    if (error) {
      *error = "path_queries can only refer to items and references";
    }
    return false;
  }

  return true;
}

bool GroveDb::QueryItemValueForVersion(
    const PathQuery& path_query,
    const GroveVersion& version,
    std::vector<std::vector<uint8_t>>* out_values,
    std::string* error) {
  return QueryItemValueForVersion(path_query, version, out_values, nullptr, error);
}

bool GroveDb::QueryItemValueForVersion(
    const PathQuery& path_query,
    const GroveVersion& version,
    std::vector<std::vector<uint8_t>>* out_values,
    Transaction* tx,
    std::string* error) {
  if (!version.IsSupported()) {
    if (error) {
      *error = "unsupported grove version: " + version.ToString();
    }
    return false;
  }
  return QueryItemValue(path_query, out_values, tx, error);
}

bool GroveDb::QueryItemValueOrSum(const PathQuery& path_query,
                                  std::vector<QueryItemOrSumValue>* out,
                                  std::string* error) {
  return QueryItemValueOrSum(path_query, out, nullptr, error);
}

bool GroveDb::QueryItemValueOrSum(const PathQuery& path_query,
                                  std::vector<QueryItemOrSumValue>* out,
                                  Transaction* tx,
                                  std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  out->clear();
  if (!opened_) {
    if (error) {
      *error = "database not opened";
    }
    return false;
  }
  if (tx != nullptr && tx->state == Transaction::State::kUninitialized) {
    if (error) {
      *error = "transaction is not active";
    }
    return false;
  }
  if (tx != nullptr && tx->state == Transaction::State::kCommitted) {
    if (error) {
      *error = "transaction is committed";
    }
    return false;
  }
  // QueryItemValueOrSum requires limit to be set
  if (!path_query.query.limit.has_value()) {
    if (error) {
      *error = "QueryItemValueOrSum missing limit: limits must be set";
    }
    return false;
  }
  // QueryItemValueOrSum does not support offset
  if (path_query.query.offset.has_value()) {
    if (error) {
      *error = "QueryItemValueOrSum offset: offsets are not supported";
    }
    return false;
  }

  std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> entries;
  if (!QueryRaw(path_query, &entries, tx, error)) {
    return false;
  }

  out->reserve(entries.size());
  for (const auto& kv : entries) {
    const std::vector<uint8_t>& key = kv.first;
    const std::vector<uint8_t>& element_bytes = kv.second;
    uint64_t variant = 0;
    if (!DecodeElementVariant(element_bytes, &variant, error)) {
      return false;
    }

    // Variant 0: Item
    if (variant == 0) {
      ElementItem item;
      if (!DecodeItemFromElementBytes(element_bytes, &item, error)) {
        return false;
      }
      QueryItemOrSumValue result;
      result.kind = QueryItemOrSumValue::Kind::kItemData;
      result.item_data = std::move(item.value);
      out->push_back(std::move(result));
      continue;
    }
    // Variant 9: ItemWithSum
    if (variant == 9) {
      ElementItemWithSum item;
      if (!DecodeItemWithSumItemFromElementBytes(element_bytes, &item, error)) {
        return false;
      }
      QueryItemOrSumValue result;
      result.kind = QueryItemOrSumValue::Kind::kItemDataWithSumValue;
      result.item_data = std::move(item.value);
      result.sum_value = item.sum;
      out->push_back(std::move(result));
      continue;
    }
    // Variant 3: SumItem
    if (variant == 3) {
      ElementSumItem sum_item;
      if (!DecodeSumItemFromElementBytes(element_bytes, &sum_item, error)) {
        return false;
      }
      QueryItemOrSumValue result;
      result.kind = QueryItemOrSumValue::Kind::kSumValue;
      result.sum_value = sum_item.sum;
      out->push_back(std::move(result));
      continue;
    }
    // Variant 4: SumTree
    if (variant == 4) {
      int64_t sum_value = 0;
      bool has_sum = false;
      if (!ExtractSumValueFromElementBytes(element_bytes, &sum_value, &has_sum, error)) {
        return false;
      }
      QueryItemOrSumValue result;
      result.kind = QueryItemOrSumValue::Kind::kSumValue;
      result.sum_value = sum_value;
      out->push_back(std::move(result));
      continue;
    }
    // Variant 5: BigSumTree
    if (variant == 5) {
      __int128 big_sum_value = 0;
      bool has_sum = false;
      if (!ExtractBigSumValueFromElementBytes(element_bytes, &big_sum_value, &has_sum, error)) {
        return false;
      }
      QueryItemOrSumValue result;
      result.kind = QueryItemOrSumValue::Kind::kBigSumValue;
      // Store as little-endian bytes
      for (int i = 0; i < 16; ++i) {
        result.big_sum_value.push_back(static_cast<uint8_t>((big_sum_value >> (i * 8)) & 0xFF));
      }
      out->push_back(std::move(result));
      continue;
    }
    // Variant 6: CountTree
    if (variant == 6) {
      uint64_t count_value = 0;
      bool has_count = false;
      if (!ExtractCountValueFromElementBytes(element_bytes, &count_value, &has_count, error)) {
        return false;
      }
      QueryItemOrSumValue result;
      result.kind = QueryItemOrSumValue::Kind::kCountValue;
      result.count_value = static_cast<int64_t>(count_value);
      out->push_back(std::move(result));
      continue;
    }
    // Variant 7: CountSumTree
    if (variant == 7) {
      uint64_t count_value = 0;
      int64_t sum_value = 0;
      bool has_count_sum = false;
      if (!ExtractCountSumValueFromElementBytes(element_bytes, &count_value, &sum_value, &has_count_sum, error)) {
        return false;
      }
      QueryItemOrSumValue result;
      result.kind = QueryItemOrSumValue::Kind::kCountSumValue;
      result.count_value = static_cast<int64_t>(count_value);
      result.sum_value = sum_value;
      out->push_back(std::move(result));
      continue;
    }
    // Variant 8: ProvableCountTree
    if (variant == 8) {
      uint64_t count_value = 0;
      bool has_count = false;
      if (!ExtractCountValueFromElementBytes(element_bytes, &count_value, &has_count, error)) {
        return false;
      }
      QueryItemOrSumValue result;
      result.kind = QueryItemOrSumValue::Kind::kCountValue;
      result.count_value = static_cast<int64_t>(count_value);
      out->push_back(std::move(result));
      continue;
    }
    // Variant 10: ProvableCountSumTree
    if (variant == 10) {
      uint64_t count_value = 0;
      int64_t sum_value = 0;
      bool has_count_sum = false;
      if (!ExtractProvableCountSumValueFromElementBytes(element_bytes, &count_value, &sum_value, &has_count_sum, error)) {
        return false;
      }
      QueryItemOrSumValue result;
      result.kind = QueryItemOrSumValue::Kind::kCountSumValue;
      result.count_value = static_cast<int64_t>(count_value);
      result.sum_value = sum_value;
      out->push_back(std::move(result));
      continue;
    }
    // Variant 1: Reference - follow to target
    if (variant == 1) {
      std::vector<uint8_t> resolved;
      bool found = false;
      if (!Get(path_query.path, key, &resolved, &found, tx, error)) {
        return false;
      }
      if (!found) {
        if (error) {
          *error = "the reference must result in an element";
        }
        return false;
      }
      uint64_t resolved_variant = 0;
      if (!DecodeElementVariant(resolved, &resolved_variant, error)) {
        return false;
      }
      
      // Recursively handle resolved element
      if (resolved_variant == 0) {
        ElementItem item;
        if (!DecodeItemFromElementBytes(resolved, &item, error)) {
          return false;
        }
        QueryItemOrSumValue result;
        result.kind = QueryItemOrSumValue::Kind::kItemData;
        result.item_data = std::move(item.value);
        out->push_back(std::move(result));
        continue;
      }
      if (resolved_variant == 9) {
        ElementItemWithSum item;
        if (!DecodeItemWithSumItemFromElementBytes(resolved, &item, error)) {
          return false;
        }
        QueryItemOrSumValue result;
        result.kind = QueryItemOrSumValue::Kind::kItemDataWithSumValue;
        result.item_data = std::move(item.value);
        result.sum_value = item.sum;
        out->push_back(std::move(result));
        continue;
      }
      if (resolved_variant == 3) {
        ElementSumItem sum_item;
        if (!DecodeSumItemFromElementBytes(resolved, &sum_item, error)) {
          return false;
        }
        QueryItemOrSumValue result;
        result.kind = QueryItemOrSumValue::Kind::kSumValue;
        result.sum_value = sum_item.sum;
        out->push_back(std::move(result));
        continue;
      }
      if (resolved_variant == 4) {
        int64_t sum_value = 0;
        bool has_sum = false;
        if (!ExtractSumValueFromElementBytes(resolved, &sum_value, &has_sum, error)) {
          return false;
        }
        QueryItemOrSumValue result;
        result.kind = QueryItemOrSumValue::Kind::kSumValue;
        result.sum_value = sum_value;
        out->push_back(std::move(result));
        continue;
      }
      if (resolved_variant == 5) {
        __int128 big_sum_value = 0;
        bool has_sum = false;
        if (!ExtractBigSumValueFromElementBytes(resolved, &big_sum_value, &has_sum, error)) {
          return false;
        }
        QueryItemOrSumValue result;
        result.kind = QueryItemOrSumValue::Kind::kBigSumValue;
        // Store as little-endian bytes
        for (int i = 0; i < 16; ++i) {
          result.big_sum_value.push_back(static_cast<uint8_t>((big_sum_value >> (i * 8)) & 0xFF));
        }
        out->push_back(std::move(result));
        continue;
      }
      if (resolved_variant == 6) {
        uint64_t count_value = 0;
        bool has_count = false;
        if (!ExtractCountValueFromElementBytes(resolved, &count_value, &has_count, error)) {
          return false;
        }
        QueryItemOrSumValue result;
        result.kind = QueryItemOrSumValue::Kind::kCountValue;
        result.count_value = static_cast<int64_t>(count_value);
        out->push_back(std::move(result));
        continue;
      }
      if (resolved_variant == 7) {
        uint64_t count_value = 0;
        int64_t sum_value = 0;
        bool has_count_sum = false;
        if (!ExtractCountSumValueFromElementBytes(resolved, &count_value, &sum_value, &has_count_sum, error)) {
          return false;
        }
        QueryItemOrSumValue result;
        result.kind = QueryItemOrSumValue::Kind::kCountSumValue;
        result.count_value = static_cast<int64_t>(count_value);
        result.sum_value = sum_value;
        out->push_back(std::move(result));
        continue;
      }
      if (resolved_variant == 8) {
        uint64_t count_value = 0;
        bool has_count = false;
        if (!ExtractCountValueFromElementBytes(resolved, &count_value, &has_count, error)) {
          return false;
        }
        QueryItemOrSumValue result;
        result.kind = QueryItemOrSumValue::Kind::kCountValue;
        result.count_value = static_cast<int64_t>(count_value);
        out->push_back(std::move(result));
        continue;
      }
      if (resolved_variant == 10) {
        uint64_t count_value = 0;
        int64_t sum_value = 0;
        bool has_count_sum = false;
        if (!ExtractProvableCountSumValueFromElementBytes(resolved, &count_value, &sum_value, &has_count_sum, error)) {
          return false;
        }
        QueryItemOrSumValue result;
        result.kind = QueryItemOrSumValue::Kind::kCountSumValue;
        result.count_value = static_cast<int64_t>(count_value);
        result.sum_value = sum_value;
        out->push_back(std::move(result));
        continue;
      }
      if (error) {
        *error = "the reference must result in a value-bearing element";
      }
      return false;
    }

    if (error) {
      *error = "path_queries can only refer to value-bearing elements";
    }
    return false;
  }

  return true;
}

bool GroveDb::QuerySums(const PathQuery& path_query,
                        std::vector<int64_t>* out_sums,
                        std::string* error) {
  return QuerySums(path_query, out_sums, nullptr, error);
}

bool GroveDb::QuerySums(const PathQuery& path_query,
                        std::vector<int64_t>* out_sums,
                        Transaction* tx,
                        std::string* error) {
  if (out_sums == nullptr) {
    if (error) {
      *error = "output is null";
    }
    return false;
  }
  out_sums->clear();
  if (!opened_) {
    if (error) {
      *error = "database not opened";
    }
    return false;
  }
  if (tx != nullptr && tx->state == Transaction::State::kUninitialized) {
    if (error) {
      *error = "transaction is not active";
    }
    return false;
  }
  if (tx != nullptr && tx->state == Transaction::State::kCommitted) {
    if (error) {
      *error = "transaction is committed";
    }
    return false;
  }

  if (!path_query.query.limit.has_value()) {
    if (error) {
      *error = "limits must be set in query_sums";
    }
    return false;
  }
  if (path_query.query.offset.has_value()) {
    if (error) {
      *error = "offsets are not supported in query_sums";
    }
    return false;
  }

  if (path_query.query.query.items.empty()) {
    if (error) {
      *error = "query items are empty";
    }
    return false;
  }
  if (path_query.query.query.default_subquery_branch.subquery_path.has_value() ||
      path_query.query.query.default_subquery_branch.subquery != nullptr ||
      path_query.query.query.conditional_subquery_branches.has_value() ||
      path_query.query.query.add_parent_tree_on_subquery) {
    if (error) {
      *error = "unsupported query shape for query_sums";
    }
    return false;
  }

  std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> entries;
  if (!QueryRaw(path_query, &entries, tx, error)) {
    return false;
  }

  const size_t offset = path_query.query.offset.value_or(0);
  const size_t limit = path_query.query.limit.has_value()
                           ? static_cast<size_t>(path_query.query.limit.value())
                           : std::numeric_limits<size_t>::max();

  size_t skipped = 0;
  out_sums->reserve(entries.size());

  for (const auto& kv : entries) {
    const auto& element_bytes = kv.second;
    
    uint64_t variant = 0;
    if (!DecodeElementVariant(element_bytes, &variant, error)) {
      return false;
    }
    
    // Element variant 3 = SumItem
    // We only want pure SumItem elements (variant == 3)
    if (variant == 3) {
      ElementSumItem sum_item;
      if (!DecodeSumItemFromElementBytes(element_bytes, &sum_item, error)) {
        return false;
      }
      
      if (skipped < offset) {
        ++skipped;
        continue;
      }
      if (out_sums->size() >= limit) {
        break;
      }
      out_sums->push_back(sum_item.sum);
    }
  }

  return true;
}

bool GroveDb::QuerySumsForVersion(
    const PathQuery& path_query,
    const GroveVersion& version,
    std::vector<int64_t>* out_sums,
    std::string* error) {
  return QuerySumsForVersion(path_query, version, out_sums, nullptr, error);
}

bool GroveDb::QuerySumsForVersion(
    const PathQuery& path_query,
    const GroveVersion& version,
    std::vector<int64_t>* out_sums,
    Transaction* tx,
    std::string* error) {
  if (!version.IsSupported()) {
    if (error) {
      *error = "unsupported grove version: " + version.ToString();
    }
    return false;
  }
  return QuerySums(path_query, out_sums, tx, error);
}

bool GroveDb::QueryRawForVersion(
    const PathQuery& path_query,
    const GroveVersion& version,
    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>>* out,
    std::string* error) {
  return QueryRawForVersion(path_query, version, out, nullptr, error);
}

bool GroveDb::QueryRawForVersion(
    const PathQuery& path_query,
    const GroveVersion& version,
    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>>* out,
    Transaction* tx,
    std::string* error) {
  if (!version.IsSupported()) {
    if (error) {
      *error = "unsupported grove version: " + version.ToString();
    }
    return false;
  }
  return QueryRaw(path_query, out, tx, error);
}

bool GroveDb::QueryKeysOptionalForVersion(
    const PathQuery& path_query,
    const GroveVersion& version,
    std::vector<PathKeyOptionalElement>* out,
    std::string* error) {
  return QueryKeysOptionalForVersion(path_query, version, out, nullptr, error);
}

bool GroveDb::QueryKeysOptionalForVersion(
    const PathQuery& path_query,
    const GroveVersion& version,
    std::vector<PathKeyOptionalElement>* out,
    Transaction* tx,
    std::string* error) {
  if (!version.IsSupported()) {
    if (error) {
      *error = "unsupported grove version: " + version.ToString();
    }
    return false;
  }
  return QueryKeysOptional(path_query, out, tx, error);
}

bool GroveDb::ProveQuery(const PathQuery& query,
                         std::vector<uint8_t>* out_proof,
                         std::string* error) {
  if (out_proof == nullptr) {
    if (error) {
      *error = "proof output is null";
    }
    return false;
  }
  out_proof->clear();
  if (!opened_) {
    if (error) {
      *error = "database not opened";
    }
    return false;
  }
  if (query.query.offset.has_value() && query.query.offset.value() != 0) {
    if (error) {
      *error = "proved path queries can not have offsets";
    }
    return false;
  }
  if (query.query.limit.has_value() && query.query.limit.value() == 0) {
    if (error) {
      *error = "proved path queries can not be for limit 0";
    }
    return false;
  }
  if (query.query.query.items.empty()) {
    if (error) {
      *error = "query has no items";
    }
    return false;
  }
  if (query.query.query.items.size() != 1) {
    if (error) {
      *error = "proof generation currently supports exactly one query item";
    }
    return false;
  }
  if (!query.query.query.left_to_right) {
    if (error) {
      *error = "proof generation currently supports only left-to-right query order";
    }
    return false;
  }
  if (!EnsurePathTreeElements(query.path, nullptr, nullptr, error)) {
    return false;
  }

  const auto resolve_subquery_branch_for_key =
      [](const Query& q, const std::vector<uint8_t>& key) -> const SubqueryBranch* {
    if (q.conditional_subquery_branches) {
      for (const auto& entry : *q.conditional_subquery_branches) {
        if (entry.first.Contains(key)) {
          return &entry.second;
        }
      }
    }
    if (q.default_subquery_branch.subquery || q.default_subquery_branch.subquery_path) {
      return &q.default_subquery_branch;
    }
    return nullptr;
  };

  const auto query_has_subquery = [](const Query& q) -> bool {
    return q.default_subquery_branch.subquery != nullptr ||
           q.default_subquery_branch.subquery_path.has_value() ||
           q.conditional_subquery_branches.has_value();
  };

  std::function<bool(const std::vector<std::vector<uint8_t>>&,
                     const Query&,
                     std::optional<uint16_t>,
                     bool,
                     GroveLayerProof*)>
      build_layer_for_query;
  std::function<bool(const std::vector<std::vector<uint8_t>>&,
                     const std::vector<std::vector<uint8_t>>&,
                     const Query*,
                     std::optional<uint16_t>,
                     GroveLayerProof*)>
      build_layer_for_wrapped_subquery;

  build_layer_for_query = [&](const std::vector<std::vector<uint8_t>>& path,
                              const Query& layer_query,
                              std::optional<uint16_t> layer_limit,
                              bool require_link_hash,
                              GroveLayerProof* out_layer) -> bool {
    if (out_layer == nullptr) {
      if (error) {
        *error = "proof output is null";
      }
      return false;
    }
    if (layer_query.items.empty()) {
      if (error) {
        *error = "query has no items";
      }
      return false;
    }
    if (layer_query.items.size() != 1) {
      if (error) {
        *error = "proof generation currently supports exactly one query item";
      }
      return false;
    }
    if (!layer_query.left_to_right) {
      if (error) {
        *error = "proof generation currently supports only left-to-right query order";
      }
      return false;
    }
    if (!EnsurePathTreeElements(path, nullptr, nullptr, error)) {
      return false;
    }

    MerkTree tree;
    if (!LoadTreeForPath(&storage_, nullptr, path, &tree, nullptr, error)) {
      return false;
    }

    std::vector<uint8_t> child_root_hash;
    if (!GenerateMerkProofForQueryItem(tree,
                                       layer_query.items.front(),
                                       layer_limit,
                                       require_link_hash || query_has_subquery(layer_query),
                                       &out_layer->merk_proof,
                                       &child_root_hash,
                                       error)) {
      return false;
    }
    if (child_root_hash.size() != 32) {
      if (error) {
        *error = "invalid generated root hash length";
      }
      return false;
    }

    out_layer->lower_layers.clear();
    out_layer->prove_options_tag = 1;

    std::vector<std::vector<uint8_t>> proof_keys;
    if (!CollectProofKeys(out_layer->merk_proof, &proof_keys, error)) {
      return false;
    }
    for (const auto& proof_key : proof_keys) {
      std::vector<uint8_t> value;
      if (!tree.Get(proof_key, &value)) {
        continue;
      }
      uint64_t variant = 0;
      if (!DecodeElementVariant(value, &variant, error)) {
        return false;
      }
      const SubqueryBranch* branch = resolve_subquery_branch_for_key(layer_query, proof_key);
      const TreeFeatureTypeTag tree_tag = tree.GetTreeFeatureTag();
      const bool provable_count =
          (tree_tag == TreeFeatureTypeTag::kProvableCount ||
           tree_tag == TreeFeatureTypeTag::kProvableCountSum);
      if (variant == 1) {  // Element::Reference
        std::vector<uint8_t> resolved_value;
        bool found = false;
        if (!FollowReference(path, proof_key, &resolved_value, &found, error)) {
          if (error) {
            *error = "reference proof rewrite failed for key '" +
                     std::string(proof_key.begin(), proof_key.end()) + "': " + *error;
          }
          return false;
        }
        if (!found) {
          if (error) {
            *error = "reference proof rewrite target not found for key '" +
                     std::string(proof_key.begin(), proof_key.end()) + "'";
          }
          return false;
        }
        std::vector<uint8_t> rewritten;
        bool ref_rewritten = RewriteMerkProofForReference(out_layer->merk_proof,
                                                          proof_key,
                                                          value,
                                                          resolved_value,
                                                          provable_count,
                                                          &rewritten,
                                                          error);
        if (!ref_rewritten) {
          if (error && *error == "reference key not found in proof") {
            // Some range proof shapes include the reference key only as a hash/digest
            // boundary node; Rust leaves those unchanged.
          } else if (error) {
            *error = "reference proof rewrite failed for key '" +
                     std::string(proof_key.begin(), proof_key.end()) + "': " + *error;
            return false;
          } else {
            return false;
          }
        } else {
          out_layer->merk_proof = std::move(rewritten);
        }
      }
      if (layer_query.items.front().IsRange() &&
          IsTreeElementVariant(variant) &&
          (branch == nullptr ||
           (!branch->subquery_path.has_value() && branch->subquery == nullptr))) {
        const bool key_in_query_range = layer_query.items.front().Contains(proof_key);
        std::vector<uint8_t> rewritten;
        bool ok = false;
        if (key_in_query_range) {
          std::vector<uint8_t> tree_value_hash;
          if (!tree.GetValueHashFn()(proof_key, value, &tree_value_hash, error)) {
            *error = "tree proof value-hash rewrite hash failed for key '" +
                     std::string(proof_key.begin(), proof_key.end()) + "': " + *error;
            return false;
          }
          ok = RewriteMerkProofForValueHashKey(out_layer->merk_proof,
                                               proof_key,
                                               value,
                                               tree_value_hash,
                                               provable_count,
                                               &rewritten,
                                               error);
          if (!ok && error && *error != "value-hash key not found in proof") {
            *error = "tree proof value-hash rewrite failed for key '" +
                     std::string(proof_key.begin(), proof_key.end()) + "': " + *error;
            return false;
          }
        } else {
          ok = RewriteMerkProofForDigestKey(out_layer->merk_proof,
                                            proof_key,
                                            value,
                                            provable_count,
                                            &rewritten,
                                            error);
          if (!ok && error && *error != "digest key not found in proof") {
            *error = "tree proof digest rewrite failed for key '" +
                     std::string(proof_key.begin(), proof_key.end()) + "': " + *error;
            return false;
          }
        }
        if (ok) {
          out_layer->merk_proof = std::move(rewritten);
        }
      }
      if (!IsTreeElementVariant(variant)) {
        continue;
      }
      if (branch == nullptr ||
          (!branch->subquery_path.has_value() && branch->subquery == nullptr)) {
        continue;
      }

      std::vector<std::vector<uint8_t>> child_path = path;
      child_path.push_back(proof_key);
      GroveLayerProof child_layer;
      const std::vector<std::vector<uint8_t>> wrapped_path =
          branch->subquery_path.value_or(std::vector<std::vector<uint8_t>>{});
      if (!build_layer_for_wrapped_subquery(
              child_path, wrapped_path, branch->subquery.get(), layer_limit, &child_layer)) {
        return false;
      }
      out_layer->lower_layers.emplace_back(proof_key, std::make_unique<GroveLayerProof>(std::move(child_layer)));
    }
    return true;
  };

  build_layer_for_wrapped_subquery =
      [&](const std::vector<std::vector<uint8_t>>& base_path,
          const std::vector<std::vector<uint8_t>>& wrapped_subpath,
          const Query* final_query,
          std::optional<uint16_t> final_limit,
          GroveLayerProof* out_layer) -> bool {
    if (wrapped_subpath.empty()) {
      if (final_query == nullptr) {
        if (error) {
          *error = "subquery path requires terminal query";
        }
        return false;
      }
      return build_layer_for_query(
          base_path, *final_query, final_limit, query_has_subquery(*final_query), out_layer);
    }

    Query wrapper_query = Query::NewSingleKey(wrapped_subpath.front());
    if (!build_layer_for_query(base_path, wrapper_query, std::nullopt, true, out_layer)) {
      return false;
    }

    std::vector<std::vector<uint8_t>> next_path = base_path;
    next_path.push_back(wrapped_subpath.front());
    std::vector<std::vector<uint8_t>> remaining(
        wrapped_subpath.begin() + 1, wrapped_subpath.end());
    Query leaf_query;
    if (final_query == nullptr && remaining.empty()) {
      leaf_query = Query::NewSingleKey(wrapped_subpath.front());
      final_query = &leaf_query;
    }
    GroveLayerProof child_layer;
    if (!build_layer_for_wrapped_subquery(
            next_path, remaining, final_query, final_limit, &child_layer)) {
      return false;
    }
    out_layer->lower_layers.emplace_back(wrapped_subpath.front(), std::make_unique<GroveLayerProof>(std::move(child_layer)));
    return true;
  };

  std::function<bool(const std::vector<std::vector<uint8_t>>&,
                     const std::vector<std::vector<uint8_t>>&,
                     const Query&,
                     GroveLayerProof*)>
      build_layer_for_path_wrappers;
  build_layer_for_path_wrappers =
      [&](const std::vector<std::vector<uint8_t>>& parent_path,
          const std::vector<std::vector<uint8_t>>& remaining_path,
          const Query& leaf_query,
          GroveLayerProof* out_layer) -> bool {
    if (remaining_path.empty()) {
      return build_layer_for_query(
          parent_path, leaf_query, query.query.limit, query_has_subquery(leaf_query), out_layer);
    }
    Query wrapper_query = Query::NewSingleKey(remaining_path.front());
    if (!build_layer_for_query(parent_path, wrapper_query, std::nullopt, true, out_layer)) {
      return false;
    }
    std::vector<std::vector<uint8_t>> child_path = parent_path;
    child_path.push_back(remaining_path.front());
    std::vector<std::vector<uint8_t>> next_remaining(
        remaining_path.begin() + 1, remaining_path.end());
    GroveLayerProof child_layer;
    if (!build_layer_for_path_wrappers(child_path, next_remaining, leaf_query, &child_layer)) {
      return false;
    }
    out_layer->lower_layers.emplace_back(remaining_path.front(), std::make_unique<GroveLayerProof>(std::move(child_layer)));
    return true;
  };

  GroveLayerProof layer;
  if (!build_layer_for_path_wrappers({}, query.path, query.query.query, &layer)) {
    return false;
  }

  layer.prove_options_tag = 1;
  return EncodeGroveDbProof(layer, out_proof, error);
}

bool GroveDb::ProveQueryForVersion(const PathQuery& query,
                                   const GroveVersion& version,
                                   std::vector<uint8_t>* out_proof,
                                   std::string* error) {
  if (!version.IsSupported()) {
    if (error) {
      *error = "unsupported grove version: " + version.ToString();
    }
    return false;
  }
  return ProveQuery(query, out_proof, error);
}

bool GroveDb::ApplyBatch(const std::vector<BatchOp>& ops, std::string* error) {
  const BatchApplyOptions options;
  return ApplyBatch(ops, options, nullptr, error);
}

bool GroveDb::ApplyBatch(const std::vector<BatchOp>& ops,
                         const BatchApplyOptions& options,
                         std::string* error) {
  return ApplyBatch(ops, options, nullptr, error);
}

bool GroveDb::ApplyBatch(const std::vector<BatchOp>& ops, Transaction* tx, std::string* error) {
  const BatchApplyOptions options;
  return ApplyBatch(ops, options, tx, error);
}

bool GroveDb::ApplyBatch(const std::vector<BatchOp>& ops,
                         const BatchApplyOptions& options,
                         Transaction* tx,
                         std::string* error) {
  if (options.batch_pause_height > 0) {
    if (error) {
      *error = "use ApplyPartialBatch for batch_pause_height > 0";
    }
    return false;
  }
  return ApplyBatchInternal(ops, options, tx, 0, nullptr, error);
}

bool GroveDb::ApplyPartialBatch(const std::vector<BatchOp>& ops,
                                const BatchApplyOptions& options,
                                Transaction* tx,
                                OpsByLevelPath* leftover_ops,
                                std::string* error) {
  const int32_t stop_level = std::max(0, options.batch_pause_height);
  if (leftover_ops != nullptr) {
    leftover_ops->clear();
  }
  return ApplyBatchInternal(ops, options, tx, stop_level, leftover_ops, error);
}

bool GroveDb::ContinuePartialApplyBatch(const OpsByLevelPath& previous_leftover,
                                        const std::vector<BatchOp>& additional_ops,
                                        const BatchApplyOptions& options,
                                        Transaction* tx,
                                        OpsByLevelPath* new_leftover_ops,
                                        std::string* error) {
  std::vector<BatchOp> merged_ops;
  size_t reserve_count = additional_ops.size();
  for (const auto& entry : previous_leftover) {
    reserve_count += entry.second.size();
    for (const auto& op : entry.second) {
      if (op.path.size() != entry.first) {
        if (error) {
          *error = "leftover op level/path mismatch";
        }
        return false;
      }
    }
  }
  merged_ops.reserve(reserve_count);
  for (const auto& entry : previous_leftover) {
    merged_ops.insert(merged_ops.end(), entry.second.begin(), entry.second.end());
  }
  merged_ops.insert(merged_ops.end(), additional_ops.begin(), additional_ops.end());

  const int32_t stop_level = std::max(0, options.batch_pause_height);
  if (new_leftover_ops != nullptr) {
    new_leftover_ops->clear();
  }
  return ApplyBatchInternal(merged_ops, options, tx, stop_level, new_leftover_ops, error);
}

bool GroveDb::ApplyBatchInternal(const std::vector<BatchOp>& ops,
                                 const BatchApplyOptions& options,
                                 Transaction* tx,
                                 int32_t stop_level,
                                 OpsByLevelPath* leftover_ops,
                                 std::string* error) {
  InvalidateTransactionInsertCache(tx);
  if (!opened_) {
    if (error) {
      *error = "database not opened";
    }
    return false;
  }
  if (leftover_ops != nullptr) {
    leftover_ops->clear();
  }
  const int32_t effective_stop = std::max(0, stop_level);
  // Save any existing cache (for nested batch) and create a fresh one.
  auto outer_cache = std::move(merk_cache_);
  merk_cache_ = std::make_unique<MerkCache>(&storage_);
  auto restore_outer_cache = [&]() { merk_cache_ = std::move(outer_cache); };
  if (!options.disable_operation_consistency_check && !VerifyBatchOperationConsistency(ops)) {
    restore_outer_cache();
    if (error) {
      *error = "batch operations fail consistency checks";
    }
    return false;
  }
  const std::vector<BatchOp>* exec_ops = &ops;
  std::vector<BatchOp> canonicalized_ops;
  if (ops.size() > 1) {
    if (options.disable_operation_consistency_check) {
      canonicalized_ops = RustCanonicalizedBatchOpsForDisabledConsistency(ops);
    } else {
      canonicalized_ops = RustOrderedBatchOps(ops);
    }
    exec_ops = &canonicalized_ops;
  }
  Transaction local_tx;
  Transaction* write_tx = tx;
  if (write_tx == nullptr) {
    if (!StartTransaction(&local_tx, error)) {
      restore_outer_cache();
      return false;
    }
    write_tx = &local_tx;
  }

  const auto cleanup_on_failure = [&]() {
    restore_outer_cache();
    std::string rollback_error;
    if (!RollbackTransaction(write_tx, &rollback_error) && error != nullptr &&
        !rollback_error.empty()) {
      *error += " (rollback failed: " + rollback_error + ")";
    }
  };
  const auto deleted_qualified_paths = CollectDeletedQualifiedPaths(*exec_ops, effective_stop);

  for (size_t op_idx = 0; op_idx < exec_ops->size(); ++op_idx) {
    const auto& op = (*exec_ops)[op_idx];
    if (op.path.size() < static_cast<size_t>(effective_stop)) {
      if (leftover_ops != nullptr) {
        (*leftover_ops)[static_cast<uint32_t>(op.path.size())].push_back(op);
      }
      continue;
    }
    if (IsModificationUnderDeletedTree(op, deleted_qualified_paths)) {
      cleanup_on_failure();
      if (error) {
        *error = "modification of tree when it will be deleted";
      }
      return false;
    }
    // Rust parity: validate_insertion_does_not_override rejects any existing
    // element; validate_insertion_does_not_override_tree rejects only tree
    // elements (non-tree overrides are allowed).
    if ((options.validate_insertion_does_not_override ||
         options.validate_insertion_does_not_override_tree) &&
        (op.kind == BatchOp::Kind::kInsert || op.kind == BatchOp::Kind::kInsertOnly ||
         op.kind == BatchOp::Kind::kReplace || op.kind == BatchOp::Kind::kPatch ||
         op.kind == BatchOp::Kind::kInsertOrReplace || op.kind == BatchOp::Kind::kInsertTree)) {
      // Use non-transactional Get for validation to avoid lazy loading issues.
      // The validation checks if the key existed BEFORE the batch started.
      // Non-transactional reads see the committed state, which is correct
      // since the data being validated against was committed before the batch.
      std::vector<uint8_t> existing_raw;
      bool existing_found = false;
      if (!Get(op.path, op.key, &existing_raw, &existing_found, nullptr, error)) {
        if (error) { *error = "ApplyBatch:validate_override Get[" + std::to_string(op_idx) + "]: " + *error; }
        cleanup_on_failure();
        return false;
      }
      if (existing_found) {
        if (options.validate_insertion_does_not_override) {
          cleanup_on_failure();
          if (error) {
            *error = "attempting to overwrite a tree";
          }
          return false;
        }
        if (options.validate_insertion_does_not_override_tree) {
          uint64_t variant = 0;
          if (!DecodeElementVariant(existing_raw, &variant, error)) {
            cleanup_on_failure();
            return false;
          }
          if (IsTreeElementVariant(variant)) {
            cleanup_on_failure();
            if (error) {
              *error = "insertion not allowed to override a tree";
            }
            return false;
          }
        }
      }
    }
    if (op.kind == BatchOp::Kind::kInsertOnly) {
      if (!Insert(op.path, op.key, op.element_bytes, write_tx, error)) {
        cleanup_on_failure();
        return false;
      }
      continue;
    }
    if (op.kind == BatchOp::Kind::kReplace) {
      if (!Insert(op.path, op.key, op.element_bytes, write_tx, error)) {
        cleanup_on_failure();
        return false;
      }
      continue;
    }
    if (op.kind == BatchOp::Kind::kInsert) {
      if (!Insert(op.path, op.key, op.element_bytes, write_tx, error)) {
        cleanup_on_failure();
        return false;
      }
      continue;
    }
    if (op.kind == BatchOp::Kind::kInsertOrReplace) {
      if (!Insert(op.path, op.key, op.element_bytes, write_tx, error)) {
        cleanup_on_failure();
        return false;
      }
      continue;
    }
    if (op.kind == BatchOp::Kind::kPatch) {
      if (!Insert(op.path, op.key, op.element_bytes, write_tx, error)) {
        cleanup_on_failure();
        return false;
      }
      continue;
    }
    if (op.kind == BatchOp::Kind::kInsertTree) {
      uint64_t variant = 0;
      if (!DecodeElementVariant(op.element_bytes, &variant, error)) {
        cleanup_on_failure();
        return false;
      }
      if (!IsTreeElementVariant(variant)) {
        cleanup_on_failure();
        if (error) {
          *error = "kInsertTree requires a tree element";
        }
        return false;
      }
      if (!Insert(op.path, op.key, op.element_bytes, write_tx, error)) {
        cleanup_on_failure();
        return false;
      }
      continue;
    }
    if (op.kind == BatchOp::Kind::kDelete) {
      bool deleted = false;
      if (!Delete(op.path, op.key, &deleted, write_tx, error)) {
        cleanup_on_failure();
        return false;
      }
      if (deleted) {
        std::vector<std::vector<uint8_t>> subtree_path = op.path;
        subtree_path.push_back(op.key);
        if (!write_tx->inner.DeletePrefix(ColumnFamilyKind::kDefault, subtree_path, error) ||
            !write_tx->inner.DeletePrefix(ColumnFamilyKind::kRoots, subtree_path, error)) {
          cleanup_on_failure();
          return false;
        }
      }
      continue;
    }
    if (op.kind == BatchOp::Kind::kDeleteTree) {
      std::vector<uint8_t> existing_raw;
      bool existing_found = false;
      if (!Get(op.path, op.key, &existing_raw, &existing_found, write_tx, error)) {
        cleanup_on_failure();
        return false;
      }
      if (!existing_found) {
        continue;
      }
      uint64_t variant = 0;
      if (!DecodeElementVariant(existing_raw, &variant, error)) {
        cleanup_on_failure();
        return false;
      }
      if (!IsTreeElementVariant(variant)) {
        cleanup_on_failure();
        if (error) {
          *error = "delete tree operation requires a tree element";
        }
        return false;
      }
      // Rust parity: batch DeleteTree operation does not consult non-empty tree
      // deletion option flags; it deletes the tree key directly.
      bool deleted = false;
      if (!Delete(op.path, op.key, &deleted, write_tx, error)) {
        cleanup_on_failure();
        return false;
      }
      continue;
    }
    if (op.kind == BatchOp::Kind::kRefreshReference) {
      // Rust parity: trust_refresh_reference skips validating the existing
      // on-disk element and trusts the provided reference payload.
      if (!options.trust_refresh_reference) {
        std::vector<uint8_t> existing_raw;
        bool existing_found = false;
        if (!GetRaw(op.path, op.key, &existing_raw, &existing_found, write_tx, error)) {
          cleanup_on_failure();
          return false;
        }
        if (!existing_found) {
          cleanup_on_failure();
          if (error) {
            *error = "trying to refresh a reference that does not exist";
          }
          return false;
        }
        uint64_t existing_variant = 0;
        if (!DecodeElementVariant(existing_raw, &existing_variant, error)) {
          cleanup_on_failure();
          return false;
        }
        if (existing_variant != 1) {  // 1 = Element::Reference
          cleanup_on_failure();
          if (error) {
            *error = "trying to refresh an element that is not a reference";
          }
          return false;
        }
      }
      // Validate the new element is also a reference.
      uint64_t new_variant = 0;
      if (!DecodeElementVariant(op.element_bytes, &new_variant, error)) {
        cleanup_on_failure();
        return false;
      }
      if (new_variant != 1) {
        cleanup_on_failure();
        if (error) {
          *error = "refresh reference element_bytes must encode a reference";
        }
        return false;
      }
      // Write the updated reference element.
      if (!Insert(op.path, op.key, op.element_bytes, write_tx, error)) {
        cleanup_on_failure();
        return false;
      }
      continue;
    }
  }
  if (!PropagateCachedRootHashes(&write_tx->inner, effective_stop, error)) {
    cleanup_on_failure();
    return false;
  }
  if (tx == nullptr) {
    bool commit_result = CommitTransaction(write_tx, error);
    if (!commit_result && error) { *error = "ApplyBatch:CommitTransaction: " + *error; }
    // Do not restore the outer cache - the batch may have modified trees that
    // are cached in the outer cache, and those cached trees would be stale.
    // Discard both caches to force reload from storage on next access.
    merk_cache_.reset();
    return commit_result;
  }
  // For external transactions, also discard the cache to avoid stale data.
  merk_cache_.reset();
  return true;
}

bool GroveDb::ValidateBatch(const std::vector<BatchOp>& ops, std::string* error) {
  const BatchApplyOptions options;
  return ValidateBatch(ops, options, error);
}

bool GroveDb::ValidateBatch(const std::vector<BatchOp>& ops,
                            const BatchApplyOptions& options,
                            std::string* error) {
  if (!opened_) {
    if (error) {
      *error = "database not opened";
    }
    return false;
  }
  Transaction tx;
  if (!StartTransaction(&tx, error)) {
    return false;
  }
  const bool ok = ApplyBatch(ops, options, &tx, error);
  std::string rollback_error;
  if (!RollbackTransaction(&tx, &rollback_error)) {
    if (error && !rollback_error.empty()) {
      if (ok) {
        *error = rollback_error;
      } else {
        *error += " (rollback failed: " + rollback_error + ")";
      }
    }
    return false;
  }
  return ok;
}

bool GroveDb::EstimatedCaseOperationsForBatch(const std::vector<BatchOp>& ops,
                                              OperationCost* cost,
                                              std::string* error) {
  const BatchApplyOptions options;
  return EstimatedCaseOperationsForBatch(ops, options, cost, error);
}

bool GroveDb::EstimatedCaseOperationsForBatch(const std::vector<BatchOp>& ops,
                                              const BatchApplyOptions& options,
                                              OperationCost* cost,
                                              std::string* error) {
  if (cost == nullptr) {
    if (error) {
      *error = "cost output is null";
    }
    return false;
  }
  cost->Reset();
  if (!opened_) {
    if (error) {
      *error = "database not opened";
    }
    return false;
  }
  const int32_t effective_stop = std::max(0, options.batch_pause_height);
  if (!options.disable_operation_consistency_check && !VerifyBatchOperationConsistency(ops)) {
    if (error) {
      *error = "batch operations fail consistency checks";
    }
    return false;
  }

  // Disable merk cache during estimate to avoid loading corrupted tree state
  // from storage. The cache may contain stale references from previous
  // operations, and creating a fresh empty cache causes tree loads to
  // encounter cycles in the persisted state. By setting to nullptr, all
  // tree loads go directly to storage without cache interference.
  auto outer_cache = std::move(merk_cache_);
  merk_cache_ = nullptr;
  auto restore_outer_cache = [&]() { merk_cache_ = std::move(outer_cache); };

  auto run_estimate = [&]() -> bool {
    const std::vector<BatchOp>* exec_ops = &ops;
    std::vector<BatchOp> canonicalized_ops;
    if (ops.size() > 1) {
      if (options.disable_operation_consistency_check) {
        canonicalized_ops = RustCanonicalizedBatchOpsForDisabledConsistency(ops);
      } else {
        canonicalized_ops = RustOrderedBatchOps(ops);
      }
      exec_ops = &canonicalized_ops;
    }
    Transaction tx;
    if (!StartTransaction(&tx, error)) {
      return false;
    }
    const auto deleted_qualified_paths = CollectDeletedQualifiedPaths(*exec_ops, effective_stop);

    for (size_t op_idx = 0; op_idx < exec_ops->size(); ++op_idx) {
      const auto& op = (*exec_ops)[op_idx];
      if (op.path.size() < static_cast<size_t>(effective_stop)) {
        continue;
      }
      if (IsModificationUnderDeletedTree(op, deleted_qualified_paths)) {
        std::string rollback_error;
        if (!RollbackTransaction(&tx, &rollback_error) && error != nullptr &&
            !rollback_error.empty()) {
          *error += " (rollback failed: " + rollback_error + ")";
        }
        if (error) {
          *error = "modification of tree when it will be deleted";
        }
        return false;
      }
      if ((options.validate_insertion_does_not_override ||
           options.validate_insertion_does_not_override_tree) &&
          (op.kind == BatchOp::Kind::kInsert || op.kind == BatchOp::Kind::kInsertOnly ||
           op.kind == BatchOp::Kind::kReplace || op.kind == BatchOp::Kind::kPatch ||
           op.kind == BatchOp::Kind::kInsertOrReplace || op.kind == BatchOp::Kind::kInsertTree)) {
        std::vector<uint8_t> existing_raw;
        bool existing_found = false;
        if (!Get(op.path, op.key, &existing_raw, &existing_found, nullptr, error)) {
          std::string rollback_error;
          if (!RollbackTransaction(&tx, &rollback_error) && error != nullptr &&
              !rollback_error.empty()) {
            *error += " (rollback failed: " + rollback_error + ")";
          }
          if (error) {
            *error = "EstimatedCaseOperationsForBatch:validate_override Get[" +
                     std::to_string(op_idx) + "]: " + *error;
          }
          return false;
        }
        if (existing_found) {
          if (options.validate_insertion_does_not_override) {
            std::string rollback_error;
            if (!RollbackTransaction(&tx, &rollback_error) && error != nullptr &&
                !rollback_error.empty()) {
              *error += " (rollback failed: " + rollback_error + ")";
            }
            if (error) {
              *error = "attempting to overwrite a tree";
            }
            return false;
          }
          if (options.validate_insertion_does_not_override_tree) {
            uint64_t variant = 0;
            if (!DecodeElementVariant(existing_raw, &variant, error)) {
              std::string rollback_error;
              if (!RollbackTransaction(&tx, &rollback_error) && error != nullptr &&
                  !rollback_error.empty()) {
                *error += " (rollback failed: " + rollback_error + ")";
              }
              return false;
            }
            if (IsTreeElementVariant(variant)) {
              std::string rollback_error;
              if (!RollbackTransaction(&tx, &rollback_error) && error != nullptr &&
                  !rollback_error.empty()) {
                *error += " (rollback failed: " + rollback_error + ")";
              }
              if (error) {
                *error = "insertion not allowed to override a tree";
              }
              return false;
            }
          }
        }
      }

      OperationCost op_cost;
      std::vector<uint8_t> existing_replace_raw;
      bool existing_replace_found = false;
      if (op.kind == BatchOp::Kind::kReplace) {
        if (!Get(op.path, op.key, &existing_replace_raw, &existing_replace_found, &tx, error)) {
          std::string rollback_error;
          if (!RollbackTransaction(&tx, &rollback_error) && error != nullptr &&
              !rollback_error.empty()) {
            *error += " (rollback failed: " + rollback_error + ")";
          }
          return false;
        }
      }
      bool ok = false;
      if (op.kind == BatchOp::Kind::kInsertOnly || op.kind == BatchOp::Kind::kReplace ||
          op.kind == BatchOp::Kind::kInsert || op.kind == BatchOp::Kind::kInsertOrReplace ||
          op.kind == BatchOp::Kind::kPatch) {
        ok = Insert(op.path, op.key, op.element_bytes, &op_cost, &tx, error);
      } else if (op.kind == BatchOp::Kind::kInsertTree) {
        uint64_t variant = 0;
        if (!DecodeElementVariant(op.element_bytes, &variant, error)) {
          std::string rollback_error;
          if (!RollbackTransaction(&tx, &rollback_error) && error != nullptr &&
              !rollback_error.empty()) {
            *error += " (rollback failed: " + rollback_error + ")";
          }
          return false;
        }
        if (!IsTreeElementVariant(variant)) {
          std::string rollback_error;
          if (!RollbackTransaction(&tx, &rollback_error) && error != nullptr &&
              !rollback_error.empty()) {
            *error += " (rollback failed: " + rollback_error + ")";
          }
          if (error) {
            *error = "kInsertTree requires a tree element";
          }
          return false;
        }
        ok = Insert(op.path, op.key, op.element_bytes, &op_cost, &tx, error);
      } else if (op.kind == BatchOp::Kind::kDelete) {
        bool deleted = false;
        ok = Delete(op.path, op.key, &deleted, &op_cost, &tx, error);
      } else if (op.kind == BatchOp::Kind::kDeleteTree) {
        std::vector<uint8_t> existing_raw;
        bool existing_found = false;
        if (!Get(op.path, op.key, &existing_raw, &existing_found, &tx, error)) {
          std::string rollback_error;
          if (!RollbackTransaction(&tx, &rollback_error) && error != nullptr &&
              !rollback_error.empty()) {
            *error += " (rollback failed: " + rollback_error + ")";
          }
          return false;
        }
        if (!existing_found) {
          ok = true;
        } else {
          uint64_t variant = 0;
          if (!DecodeElementVariant(existing_raw, &variant, error)) {
            std::string rollback_error;
            if (!RollbackTransaction(&tx, &rollback_error) && error != nullptr &&
                !rollback_error.empty()) {
              *error += " (rollback failed: " + rollback_error + ")";
            }
            return false;
          }
          if (!IsTreeElementVariant(variant)) {
            std::string rollback_error;
            if (!RollbackTransaction(&tx, &rollback_error) && error != nullptr &&
                !rollback_error.empty()) {
              *error += " (rollback failed: " + rollback_error + ")";
            }
            if (error) {
              *error = "delete tree operation requires a tree element";
            }
            return false;
          }
          bool deleted_if_empty = false;
          ok = DeleteIfEmptyTree(op.path, op.key, &deleted_if_empty, &op_cost, &tx, error);
          if (!ok) {
            std::string rollback_error;
            if (!RollbackTransaction(&tx, &rollback_error) && error != nullptr &&
                !rollback_error.empty()) {
              *error += " (rollback failed: " + rollback_error + ")";
            }
            return false;
          }
          if (!deleted_if_empty) {
            if (!options.allow_deleting_non_empty_trees) {
              if (!options.deleting_non_empty_trees_returns_error) {
                ok = true;
              } else {
                std::string rollback_error;
                if (!RollbackTransaction(&tx, &rollback_error) && error != nullptr &&
                    !rollback_error.empty()) {
                  *error += " (rollback failed: " + rollback_error + ")";
                }
                if (error) {
                  *error =
                      "trying to do a delete operation for a non empty tree, but options not allowing this";
                }
                return false;
              }
            } else {
              bool deleted = false;
              ok = Delete(op.path, op.key, &deleted, &op_cost, &tx, error);
            }
          }
        }
      } else if (op.kind == BatchOp::Kind::kRefreshReference) {
        if (!options.trust_refresh_reference) {
          std::vector<uint8_t> existing_raw;
          bool existing_found = false;
          if (!GetRaw(op.path, op.key, &existing_raw, &existing_found, &tx, error)) {
            std::string rollback_error;
            if (!RollbackTransaction(&tx, &rollback_error) && error != nullptr &&
                !rollback_error.empty()) {
              *error += " (rollback failed: " + rollback_error + ")";
            }
            return false;
          }
          if (!existing_found) {
            std::string rollback_error;
            if (!RollbackTransaction(&tx, &rollback_error) && error != nullptr &&
                !rollback_error.empty()) {
              *error += " (rollback failed: " + rollback_error + ")";
            }
            if (error) {
              *error = "trying to refresh a reference that does not exist";
            }
            return false;
          }
          uint64_t existing_variant = 0;
          if (!DecodeElementVariant(existing_raw, &existing_variant, error)) {
            std::string rollback_error;
            if (!RollbackTransaction(&tx, &rollback_error) && error != nullptr &&
                !rollback_error.empty()) {
              *error += " (rollback failed: " + rollback_error + ")";
            }
            return false;
          }
          if (existing_variant != 1) {
            std::string rollback_error;
            if (!RollbackTransaction(&tx, &rollback_error) && error != nullptr &&
                !rollback_error.empty()) {
              *error += " (rollback failed: " + rollback_error + ")";
            }
            if (error) {
              *error = "trying to refresh an element that is not a reference";
            }
            return false;
          }
        }
        uint64_t new_variant = 0;
        if (!DecodeElementVariant(op.element_bytes, &new_variant, error)) {
          std::string rollback_error;
          if (!RollbackTransaction(&tx, &rollback_error) && error != nullptr &&
              !rollback_error.empty()) {
            *error += " (rollback failed: " + rollback_error + ")";
          }
          return false;
        }
        if (new_variant != 1) {
          std::string rollback_error;
          if (!RollbackTransaction(&tx, &rollback_error) && error != nullptr &&
              !rollback_error.empty()) {
            *error += " (rollback failed: " + rollback_error + ")";
          }
          if (error) {
            *error = "refresh reference element_bytes must encode a reference";
          }
          return false;
        }
        ok = Insert(op.path, op.key, op.element_bytes, &op_cost, &tx, error);
      }

      if (!ok) {
        std::string rollback_error;
        if (!RollbackTransaction(&tx, &rollback_error) && error != nullptr &&
            !rollback_error.empty()) {
          *error += " (rollback failed: " + rollback_error + ")";
        }
        return false;
      }
      if (op.kind == BatchOp::Kind::kReplace && existing_replace_found &&
          op_cost.storage_cost.removed_bytes.TotalRemovedBytes() == 0) {
        const uint32_t key_len = static_cast<uint32_t>(op.key.size());
        auto paid_value_len = [&](const std::vector<uint8_t>& element_bytes,
                                  uint32_t* out) -> bool {
          if (out == nullptr) {
            if (error) {
              *error = "paid value length output is null";
            }
            return false;
          }
          std::optional<ValueDefinedCostType> defined_cost;
          if (!ValueDefinedCostForSerializedElement(element_bytes, &defined_cost, error)) {
            return false;
          }
          if (defined_cost.has_value()) {
            if (defined_cost->kind == ValueDefinedCostType::Kind::kLayered) {
              *out = LayeredValueByteCostSizeForKeyAndValueLengths(
                  key_len, defined_cost->cost, TreeFeatureTypeTag::kBasic);
            } else {
              *out = NodeValueByteCostSize(key_len, defined_cost->cost, TreeFeatureTypeTag::kBasic);
            }
            return true;
          }
          *out = NodeValueByteCostSize(
              key_len, static_cast<uint32_t>(element_bytes.size()), TreeFeatureTypeTag::kBasic);
          return true;
        };
        uint32_t old_paid_value_len = 0;
        uint32_t new_paid_value_len = 0;
        if (!paid_value_len(existing_replace_raw, &old_paid_value_len) ||
            !paid_value_len(op.element_bytes, &new_paid_value_len)) {
          std::string rollback_error;
          if (!RollbackTransaction(&tx, &rollback_error) && error != nullptr &&
              !rollback_error.empty()) {
            *error += " (rollback failed: " + rollback_error + ")";
          }
          return false;
        }
        if (new_paid_value_len < old_paid_value_len) {
          op_cost.storage_cost.removed_bytes.Add(
              StorageRemovedBytes::Basic(old_paid_value_len - new_paid_value_len));
        }
      }
      ApplyPatchChangeInBytesCostHint(op, &op_cost);
      if (op_cost.seek_count == 0 && op_cost.storage_loaded_bytes == 0 &&
          op_cost.storage_cost.added_bytes == 0 && op_cost.storage_cost.replaced_bytes == 0 &&
          op_cost.storage_cost.removed_bytes.TotalRemovedBytes() == 0) {
        op_cost.seek_count = 1;
      }
      cost->Add(op_cost);
    }

    std::string rollback_error;
    if (!RollbackTransaction(&tx, &rollback_error)) {
      if (error && !rollback_error.empty()) {
        *error = rollback_error;
      }
      return false;
    }
    return true;
  };

  const bool result = run_estimate();
  restore_outer_cache();
  return result;
}

bool GroveDb::StartTransaction(Transaction* out, std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "transaction output is null";
    }
    return false;
  }
  if (!opened_) {
    if (error) {
      *error = "database not opened";
    }
    return false;
  }
  if (out->state == Transaction::State::kActive) {
    if (error) {
      *error = "transaction output already active";
    }
    return false;
  }
  if (!storage_.BeginTransaction(&out->inner, error)) {
    return false;
  }
  InvalidateTransactionInsertCache(out);
  out->state = Transaction::State::kActive;
  out->inner.ClearPoison();
  return true;
}

bool GroveDb::CommitTransaction(Transaction* tx, std::string* error) {
  if (tx == nullptr) {
    if (error) {
      *error = "transaction is null";
    }
    return false;
  }
  if (tx->state == Transaction::State::kRolledBack) {
    InvalidateTransactionInsertCache(tx);
    tx->state = Transaction::State::kCommitted;
    non_tx_validated_tree_paths_.clear();
    non_tx_insert_cache_path_key_.clear();
    if (non_tx_insert_cache_ != nullptr) {
      non_tx_insert_cache_->clear();
    }
    return true;
  }
  if (tx->state != Transaction::State::kActive) {
    if (error) {
      *error = "transaction is not active";
    }
    return false;
  }
  if (!EnsureTransactionUsable(tx, error)) {
    return false;
  }
  if (!tx->inner.Commit(error)) {
    return false;
  }
  InvalidateTransactionInsertCache(tx);
  tx->state = Transaction::State::kCommitted;
  non_tx_validated_tree_paths_.clear();
  non_tx_insert_cache_path_key_.clear();
  if (non_tx_insert_cache_ != nullptr) {
    non_tx_insert_cache_->clear();
  }
  if (merk_cache_) {
    merk_cache_->clear();
  }
  return true;
}

bool GroveDb::RollbackTransaction(Transaction* tx, std::string* error) {
  if (tx == nullptr) {
    if (error) {
      *error = "transaction is null";
    }
    return false;
  }
  if (tx->state == Transaction::State::kRolledBack) {
    InvalidateTransactionInsertCache(tx);
    return true;
  }
  if (tx->state != Transaction::State::kActive) {
    if (error) {
      *error = "transaction is not active";
    }
    return false;
  }
  if (!tx->inner.Rollback(error)) {
    return false;
  }
  InvalidateTransactionInsertCache(tx);
  tx->state = Transaction::State::kRolledBack;
  tx->inner.ClearPoison();
  non_tx_validated_tree_paths_.clear();
  non_tx_insert_cache_path_key_.clear();
  if (non_tx_insert_cache_ != nullptr) {
    non_tx_insert_cache_->clear();
  }
  if (merk_cache_) {
    merk_cache_->clear();
  }
  return true;
}

bool GroveDb::EnsurePathTreeElements(const std::vector<std::vector<uint8_t>>& path,
                                     Transaction* tx,
                                     OperationCost* cost,
                                     std::string* error) {
  return EnsurePathTreeElements(path, tx, cost, error, nullptr);
}

bool GroveDb::EnsurePathTreeElements(const std::vector<std::vector<uint8_t>>& path,
                                     Transaction* tx,
                                     OperationCost* cost,
                                     std::string* error,
                                     MerkCache* cache) {
  std::vector<std::vector<uint8_t>> current_path;
  RocksDbWrapper::Transaction* inner_tx = tx != nullptr ? &tx->inner : nullptr;
  for (const auto& subtree_key : path) {
    MerkTree parent_stack;
    MerkTree* parent_ptr = &parent_stack;
    if (cache != nullptr) {
      if (!GetOrLoadConfiguredMerk(
              &storage_, inner_tx, current_path, cache, &parent_ptr, error)) {
        return false;
      }
    } else if (!LoadTreeForPath(&storage_, inner_tx, current_path, &parent_stack, nullptr, error)) {
      return false;
    }
    MerkTree& parent = *parent_ptr;
    std::vector<uint8_t> raw_value;
    if (!parent.Get(subtree_key, &raw_value)) {
      if (error) {
        *error = "path not found";
      }
      return false;
    }
    if (cost != nullptr) {
      cost->seek_count += 1;
      cost->storage_loaded_bytes += static_cast<uint64_t>(raw_value.size());
    }
    uint64_t variant = 0;
    if (!DecodeElementVariant(raw_value, &variant, error)) {
      return false;
    }
    if (!IsTreeElementVariant(variant)) {
      if (error) {
        *error = "path element is not a tree";
      }
      return false;
    }
    current_path.push_back(subtree_key);
  }
  return true;
}

bool GroveDb::EnsurePathTreeElementsOptional(const std::vector<std::vector<uint8_t>>& path,
                                             Transaction* tx,
                                             OperationCost* cost,
                                             std::string* error) {
  std::vector<std::vector<uint8_t>> current_path;
  RocksDbWrapper::Transaction* inner_tx = tx != nullptr ? &tx->inner : nullptr;
  for (const auto& subtree_key : path) {
    MerkTree parent;
    if (!LoadTreeForPath(&storage_, inner_tx, current_path, &parent, nullptr, error)) {
      return false;
    }
    std::vector<uint8_t> raw_value;
    if (!parent.Get(subtree_key, &raw_value)) {
      // Unlike EnsurePathTreeElements, return true with found=false semantics
      // by clearing the path (caller will see found=false from tree.Get).
      return true;
    }
    if (cost != nullptr) {
      cost->seek_count += 1;
      cost->storage_loaded_bytes += static_cast<uint64_t>(raw_value.size());
    }
    uint64_t variant = 0;
    if (!DecodeElementVariant(raw_value, &variant, error)) {
      return false;
    }
    if (!IsTreeElementVariant(variant)) {
      if (error) {
        *error = "path element is not a tree";
      }
      return false;
    }
    current_path.push_back(subtree_key);
  }
  return true;
}

bool GroveDb::ReadSubtreeRootKey(const std::vector<std::vector<uint8_t>>& path,
                                 Transaction* tx,
                                 std::vector<uint8_t>* root_key,
                                 bool* found,
                                 std::string* error) {
  if (root_key == nullptr || found == nullptr) {
    if (error) {
      *error = "root key output is null";
    }
    return false;
  }
  const std::vector<uint8_t> root_key_key = {'r'};
  bool ok = false;
  if (tx != nullptr) {
    ok = tx->inner.Get(ColumnFamilyKind::kRoots, path, root_key_key, root_key, found, error);
  } else {
    ok = storage_.Get(ColumnFamilyKind::kRoots, path, root_key_key, root_key, found, error);
  }
  if (!ok || *found || path.empty()) {
    return ok;
  }

  // Rust-written fixtures may rely on the subtree root key embedded in the parent
  // tree element without a mirrored kRoots CF entry. Fall back to decoding that
  // embedded root key so reads remain cross-compatible.
  std::vector<std::vector<uint8_t>> parent_path(path.begin(), path.end() - 1);
  RocksDbWrapper::Transaction* inner_tx = tx != nullptr ? &tx->inner : nullptr;
  MerkTree parent;
  if (!LoadTreeForPath(&storage_, inner_tx, parent_path, &parent, nullptr, error)) {
    return false;
  }

  std::vector<uint8_t> parent_element;
  if (!parent.Get(path.back(), &parent_element)) {
    root_key->clear();
    *found = false;
    return true;
  }

  uint64_t variant = 0;
  if (!DecodeElementVariant(parent_element, &variant, error)) {
    return false;
  }
  if (!IsTreeElementVariant(variant)) {
    if (error) {
      *error = "path element is not a tree";
    }
    return false;
  }

  root_key->clear();
  *found = true;
  switch (variant) {
    case 2: {
      ElementTree tree_element;
      if (!DecodeTreeFromElementBytes(parent_element, &tree_element, error)) {
        return false;
      }
      if (tree_element.root_key.has_value()) {
        *root_key = *tree_element.root_key;
      }
      return true;
    }
    case 4: {
      ElementSumTree tree_element;
      if (!DecodeSumTreeFromElementBytes(parent_element, &tree_element, error)) {
        return false;
      }
      if (tree_element.root_key.has_value()) {
        *root_key = *tree_element.root_key;
      }
      return true;
    }
    case 5: {
      ElementBigSumTree tree_element;
      if (!DecodeBigSumTreeFromElementBytes(parent_element, &tree_element, error)) {
        return false;
      }
      if (tree_element.root_key.has_value()) {
        *root_key = *tree_element.root_key;
      }
      return true;
    }
    case 6: {
      ElementCountTree tree_element;
      if (!DecodeCountTreeFromElementBytes(parent_element, &tree_element, error)) {
        return false;
      }
      if (tree_element.root_key.has_value()) {
        *root_key = *tree_element.root_key;
      }
      return true;
    }
    case 7: {
      ElementCountSumTree tree_element;
      if (!DecodeCountSumTreeFromElementBytes(parent_element, &tree_element, error)) {
        return false;
      }
      if (tree_element.root_key.has_value()) {
        *root_key = *tree_element.root_key;
      }
      return true;
    }
    case 8: {
      ElementProvableCountTree tree_element;
      if (!DecodeProvableCountTreeFromElementBytes(parent_element, &tree_element, error)) {
        return false;
      }
      if (tree_element.root_key.has_value()) {
        *root_key = *tree_element.root_key;
      }
      return true;
    }
    case 10: {
      ElementProvableCountSumTree tree_element;
      if (!DecodeProvableCountSumTreeFromElementBytes(parent_element, &tree_element, error)) {
        return false;
      }
      if (tree_element.root_key.has_value()) {
        *root_key = *tree_element.root_key;
      }
      return true;
    }
    default:
      if (error) {
        *error = "unsupported tree element variant";
      }
      return false;
  }
}

MerkTree::SumValueFn GroveDb::MakeSumValueFn() {
  return [](const std::vector<uint8_t>& value, int64_t* out_sum, bool* has_sum, std::string*) -> bool {
    if (out_sum == nullptr || has_sum == nullptr) {
      return false;
    }
    *out_sum = 0;
    *has_sum = false;
    if (value.empty()) {
      return true;
    }
    uint64_t variant = 0;
    size_t cursor = 0;
    if (!DecodeVarint(value, &cursor, &variant, nullptr)) {
      return true;
    }
    // SumItem = 3
    if (variant == 3) {
      uint64_t raw_sum = 0;
      if (DecodeVarint(value, &cursor, &raw_sum, nullptr)) {
        // ZigZag decode: (raw >> 1) ^ -(raw & 1)
        *out_sum = static_cast<int64_t>((raw_sum >> 1) ^ (~(raw_sum & 1) + 1));
        *has_sum = true;
      }
    } else if (variant == 9) {
      // ItemWithSumItem = 9
      std::vector<uint8_t> ignored_item;
      if (DecodeOptionBytes(value, &cursor, &ignored_item, nullptr)) {
        uint64_t raw_sum = 0;
        if (DecodeVarint(value, &cursor, &raw_sum, nullptr)) {
          // ZigZag decode
          *out_sum = static_cast<int64_t>((raw_sum >> 1) ^ (~(raw_sum & 1) + 1));
          *has_sum = true;
        }
      }
    } else if (variant == 5) {
      // BigSumTree = 5 - extract i128 sum and clamp to i64
      std::vector<uint8_t> ignored_root_key;
      if (DecodeOptionBytes(value, &cursor, &ignored_root_key, nullptr)) {
        unsigned __int128 raw_sum = 0;
        if (DecodeVarintU128(value, &cursor, &raw_sum, nullptr)) {
          // ZigZag decode for i128: (raw >> 1) ^ -(raw & 1)
          __int128 decoded = static_cast<__int128>((raw_sum >> 1) ^ (~(raw_sum & 1) + 1));
          // Clamp to i64 range
          if (decoded > static_cast<__int128>(INT64_MAX)) {
            *out_sum = INT64_MAX;
          } else if (decoded < static_cast<__int128>(INT64_MIN)) {
            *out_sum = INT64_MIN;
          } else {
            *out_sum = static_cast<int64_t>(decoded);
          }
          *has_sum = true;
        }
      }
    } else if (variant == 7 || variant == 10) {
      // CountSumTree = 7, ProvableCountSumTree = 10
      std::vector<uint8_t> ignored_root_key;
      if (DecodeOptionBytes(value, &cursor, &ignored_root_key, nullptr)) {
        uint64_t ignored_count;
        if (DecodeVarint(value, &cursor, &ignored_count, nullptr)) {
          uint64_t raw_sum = 0;
          if (DecodeVarint(value, &cursor, &raw_sum, nullptr)) {
            // ZigZag decode
            *out_sum = static_cast<int64_t>((raw_sum >> 1) ^ (~(raw_sum & 1) + 1));
            *has_sum = true;
          }
        }
      }
    }
    return true;
  };
}

bool GroveDb::EncodeTreeElementWithRootKey(const std::vector<uint8_t>& previous_element,
                                           const std::vector<uint8_t>* root_key,
                                           uint64_t propagated_count,
                                           __int128 propagated_sum,
                                           std::vector<uint8_t>* updated_element,
                                           std::string* error) {
  if (updated_element == nullptr) {
    if (error) {
      *error = "updated element output is null";
    }
    return false;
  }
  uint64_t variant = 0;
  if (!DecodeElementVariant(previous_element, &variant, error)) {
    return false;
  }
  switch (variant) {
    case 2: {
      std::vector<uint8_t> flags;
      if (!DecodeTreePayload(previous_element, &flags, error)) {
        return false;
      }
      return EncodeTreeWithRootKeyAndFlags(root_key, flags, updated_element, error);
    }
    case 4: {
      std::vector<uint8_t> flags;
      int64_t ignored_sum = 0;
      if (!DecodeSumTreePayload(previous_element, &ignored_sum, &flags, error)) {
        return false;
      }
      // Use propagated sum for SumTree (variant 4)
      return EncodeSumTreeWithRootKeyAndFlags(
          root_key, static_cast<int64_t>(propagated_sum), flags, updated_element, error);
    }
    case 5: {
      std::vector<uint8_t> flags;
      __int128 ignored_sum = 0;
      if (!DecodeBigSumTreePayload(previous_element, &ignored_sum, &flags, error)) {
        return false;
      }
      // Use propagated sum for BigSumTree (variant 5)
      return EncodeBigSumTreeWithRootKeyAndFlags(root_key, propagated_sum, flags, updated_element, error);
    }
    case 6: {
      std::vector<uint8_t> flags;
      uint64_t ignored_count = 0;
      if (!DecodeCountTreePayload(previous_element, &ignored_count, &flags, error)) {
        return false;
      }
      return EncodeCountTreeWithRootKeyAndFlags(
          root_key, propagated_count, flags, updated_element, error);
    }
    case 7: {
      uint64_t count = 0;
      int64_t ignored_sum = 0;
      std::vector<uint8_t> flags;
      if (!DecodeCountSumTreePayload(previous_element, &count, &ignored_sum, &flags, error)) {
        return false;
      }
      return EncodeCountSumTreeWithRootKeyAndFlags(
          root_key, propagated_count, static_cast<int64_t>(propagated_sum), flags, updated_element, error);
    }
    case 8: {
      uint64_t count = 0;
      std::vector<uint8_t> flags;
      if (!DecodeProvableCountTreePayload(previous_element, &count, &flags, error)) {
        return false;
      }
      return EncodeProvableCountTreeWithRootKeyAndFlags(
          root_key, propagated_count, flags, updated_element, error);
    }
    case 10: {
      uint64_t count = 0;
      int64_t ignored_sum = 0;
      std::vector<uint8_t> flags;
      if (!DecodeProvableCountSumTreePayload(previous_element, &count, &ignored_sum, &flags, error)) {
        return false;
      }
      return EncodeProvableCountSumTreeWithRootKeyAndFlags(
          root_key, propagated_count, static_cast<int64_t>(propagated_sum), flags, updated_element, error);
    }
    default:
      if (error) {
        *error = "tree variant root-key rewrite not implemented";
      }
      return false;
  }
}

bool GroveDb::PropagateSubtreeRootKeyUp(const std::vector<std::vector<uint8_t>>& path,
                                        Transaction* tx,
                                        OperationCost* cost,
                                        std::string* error) {
  return PropagateSubtreeRootKeyUp(path, tx, cost, error, nullptr);
}

bool GroveDb::PropagateSubtreeRootKeyUp(const std::vector<std::vector<uint8_t>>& path,
                                        Transaction* tx,
                                        OperationCost* cost,
                                        std::string* error,
                                        MerkCache* cache) {
  if (path.empty()) {
    return true;
  }
  std::vector<std::vector<uint8_t>> child_path = path;
  RocksDbWrapper::Transaction* inner_tx = tx != nullptr ? &tx->inner : nullptr;
  MerkTree child_stack_tree;
  MerkTree* child_tree_ptr = &child_stack_tree;
  auto load_child = [&](const std::vector<std::vector<uint8_t>>& p,
                        MerkTree** out_tree) -> bool {
    if (cache != nullptr) {
      return GetOrLoadConfiguredMerk(&storage_, inner_tx, p, cache, out_tree, error);
    }
    *out_tree = &child_stack_tree;
    return LoadTreeForPath(&storage_, inner_tx, p, &child_stack_tree, nullptr, error);
  };
  if (!load_child(child_path, &child_tree_ptr)) {
    // After deleting the last node in a subtree, the subtree storage can be
    // cleared before the parent tree element's embedded root key is rewritten.
    // Rust-compat fallback root-key decoding may then point at a just-deleted
    // node and fail this initial child reload with "encoded node not found".
    if (error != nullptr && *error == "encoded node not found") {
      error->clear();
      child_stack_tree = MerkTree();
      child_tree_ptr = &child_stack_tree;
    } else {
      return false;
    }
  }
  while (!child_path.empty()) {
    const std::vector<uint8_t> subtree_key = child_path.back();
    std::vector<std::vector<uint8_t>> parent_path = child_path;
    parent_path.pop_back();

    std::vector<uint8_t> child_root_key;
    MerkTree& child_tree = *child_tree_ptr;
    bool child_has_root = child_tree.RootKey(&child_root_key) && !child_root_key.empty();
    const std::vector<uint8_t>* root_key_ptr = child_has_root ? &child_root_key : nullptr;
    if (cost != nullptr && root_key_ptr != nullptr) {
      cost->hash_node_calls += 3;
    }

    MerkTree parent_stack_tree;
    MerkTree* parent_tree_ptr = &parent_stack_tree;
    if (cache != nullptr) {
      if (!GetOrLoadConfiguredMerk(
              &storage_, inner_tx, parent_path, cache, &parent_tree_ptr, error)) {
        return false;
      }
    } else if (!LoadTreeForPath(&storage_, inner_tx, parent_path, &parent_stack_tree, nullptr, error)) {
      return false;
    }
    std::vector<uint8_t> previous_element;
    MerkTree& parent_tree = *parent_tree_ptr;
    if (!parent_tree.Get(subtree_key, &previous_element)) {
      if (error) {
        *error = "path not found";
      }
      return false;
    }
    uint64_t parent_variant = 0;
    if (!DecodeElementVariant(previous_element, &parent_variant, error)) {
      return false;
    }
    // Only compute count for count-bearing tree variants
    uint64_t propagated_count = 0;
    if (parent_variant == 6 || parent_variant == 7 ||
        parent_variant == 8 || parent_variant == 10) {
      if (!GetPropagatedAggregatesForTree(child_tree,
                                          true,
                                          false,
                                          &propagated_count,
                                          nullptr,
                                          error)) {
        return false;
      }
    }
    // Compute propagated sum for sum-bearing tree variants (4=SumTree, 5=BigSumTree, 7=CountSumTree, 10=ProvableCountSumTree)
    __int128 propagated_sum = 0;
    if (parent_variant == 4 || parent_variant == 5 ||
        parent_variant == 7 || parent_variant == 10) {
      if (!GetPropagatedAggregatesForTree(child_tree,
                                          false,
                                          true,
                                          nullptr,
                                          &propagated_sum,
                                          error)) {
        return false;
      }
    }
    std::vector<uint8_t> rewritten_element;
    if (!EncodeTreeElementWithRootKey(previous_element,
                                      root_key_ptr,
                                      propagated_count,
                                      propagated_sum,
                                      &rewritten_element,
                                      error)) {
      return false;
    }

    OperationCost local_cost;
    OperationCost* write_cost = cost != nullptr ? &local_cost : nullptr;
    if (write_cost != nullptr) {
      if (!parent_tree.Insert(subtree_key, rewritten_element, write_cost, error)) {
        return false;
      }
    } else if (!parent_tree.Insert(subtree_key, rewritten_element, error)) {
      return false;
    }
    if (!SaveTreeForPath(
            &storage_, inner_tx, parent_path, &parent_tree, nullptr, write_cost, error)) {
      return false;
    }
    if (cost != nullptr) {
      cost->Add(local_cost);
    }
    if (cache != nullptr) {
      child_tree_ptr = parent_tree_ptr;
    } else {
      child_stack_tree = std::move(parent_tree);
      child_tree_ptr = &child_stack_tree;
    }
    child_path = parent_path;
  }
  return true;
}

bool GroveDb::GetMerkCached(const std::vector<std::vector<uint8_t>>& path,
                             RocksDbWrapper::Transaction* tx,
                             MerkTree** out,
                             std::string* error) {
  if (merk_cache_ == nullptr) {
    if (error) {
      *error = "merk cache not initialized";
    }
    return false;
  }
  return GetOrLoadConfiguredMerk(&storage_, tx, path, merk_cache_.get(), out, error);
}

bool GroveDb::PropagateCachedRootHashes(RocksDbWrapper::Transaction* tx,
                                        int32_t stop_depth,
                                        std::string* error) {
  if (merk_cache_ == nullptr) {
    return true;
  }
  const int32_t effective_stop_depth = std::max(0, stop_depth);

  std::vector<std::vector<std::vector<uint8_t>>> cached_paths = merk_cache_->getCachedPaths();
  if (cached_paths.empty()) {
    return true;
  }

  std::sort(cached_paths.begin(), cached_paths.end(),
            [](const std::vector<std::vector<uint8_t>>& a, const std::vector<std::vector<uint8_t>>& b) {
              return a.size() > b.size();
            });

  for (const auto& path : cached_paths) {
    if (path.empty()) {
      continue;
    }
    if (path.size() < static_cast<size_t>(effective_stop_depth)) {
      continue;
    }

    MerkTree* child_tree_ptr = nullptr;
    if (!GetOrLoadConfiguredMerk(&storage_, tx, path, merk_cache_.get(), &child_tree_ptr, error)) {
      return false;
    }
    MerkTree& child_tree = *child_tree_ptr;

    std::vector<uint8_t> child_root_key;
    bool child_has_root = child_tree.RootKey(&child_root_key) && !child_root_key.empty();

    std::vector<std::vector<uint8_t>> child_path = path;
    while (child_path.size() > static_cast<size_t>(effective_stop_depth)) {
      const std::vector<uint8_t> subtree_key = child_path.back();
      std::vector<std::vector<uint8_t>> parent_path = child_path;
      parent_path.pop_back();

      MerkTree* parent_tree_ptr = nullptr;
      if (!GetOrLoadConfiguredMerk(
              &storage_, tx, parent_path, merk_cache_.get(), &parent_tree_ptr, error)) {
        return false;
      }
      MerkTree& parent_tree = *parent_tree_ptr;

      std::vector<uint8_t> previous_element;
      if (!parent_tree.Get(subtree_key, &previous_element)) {
        if (error) {
          *error = "path not found";
        }
        return false;
      }

      uint64_t parent_variant = 0;
      if (!DecodeElementVariant(previous_element, &parent_variant, error)) {
        return false;
      }
      // Recompute count for count-bearing tree variants
      uint64_t propagated_count = 0;
      bool needs_count = (parent_variant == 6 || parent_variant == 7 ||
                          parent_variant == 8 || parent_variant == 10);
      if (needs_count) {
        if (!GetPropagatedAggregatesForTree(child_tree,
                                            true,
                                            false,
                                            &propagated_count,
                                            nullptr,
                                            error)) {
          return false;
        }
      }
      // Compute propagated sum for sum-bearing tree variants (4=SumTree, 5=BigSumTree, 7=CountSumTree, 10=ProvableCountSumTree)
      __int128 propagated_sum = 0;
      bool needs_sum = (parent_variant == 4 || parent_variant == 5 ||
                        parent_variant == 7 || parent_variant == 10);
      if (needs_sum) {
        if (!GetPropagatedAggregatesForTree(child_tree,
                                            false,
                                            true,
                                            nullptr,
                                            &propagated_sum,
                                            error)) {
          return false;
        }
      }

      const std::vector<uint8_t>* root_key_ptr =
          (child_has_root && !child_root_key.empty()) ? &child_root_key : nullptr;
      std::vector<uint8_t> rewritten_element;
      if (!EncodeTreeElementWithRootKey(previous_element,
                                        root_key_ptr,
                                        propagated_count,
                                        propagated_sum,
                                        &rewritten_element,
                                        error)) {
        return false;
      }

      if (!parent_tree.Insert(subtree_key, rewritten_element, error)) {
        return false;
      }

      RocksDbWrapper::WriteBatch batch;
      if (!SaveTreeForPath(&storage_, tx, parent_path, &parent_tree, &batch, nullptr, error)) {
        return false;
      }
      if (!storage_.CommitBatch(batch, tx, error)) {
        return false;
      }

      if (!parent_path.empty()) {
        child_has_root = parent_tree.RootKey(&child_root_key) && !child_root_key.empty();
      }
      child_path = parent_path;
    }
  }

  return true;
}

bool GroveDb::IsTreeElementVariant(uint64_t variant) {
  return variant == 2 || variant == 4 || variant == 5 || variant == 6 || variant == 7 ||
         variant == 8 || variant == 10;
}

}  // namespace grovedb
