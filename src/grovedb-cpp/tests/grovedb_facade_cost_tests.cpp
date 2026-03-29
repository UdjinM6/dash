#include "element.h"
#include "grovedb.h"
#include "test_utils.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

using test_utils::Fail;
using test_utils::MakeTempDir;

namespace {

void ExpectMutationCost(const grovedb::OperationCost& cost, const std::string& label) {
  const uint32_t removed = cost.storage_cost.removed_bytes.TotalRemovedBytes();
  if (cost.seek_count == 0 && cost.storage_loaded_bytes == 0 && cost.hash_node_calls == 0 &&
      cost.storage_cost.added_bytes == 0 && cost.storage_cost.replaced_bytes == 0 && removed == 0) {
    Fail(label + ": expected non-zero operation cost");
  }
}

}  // namespace

int main() {
  std::string error;
  grovedb::GroveDb db;
  auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
  const std::string dir = MakeTempDir("facade_cost_test_" + std::to_string(now));
  if (!db.Open(dir, &error)) {
    Fail("Open failed: " + error);
  }

  std::vector<uint8_t> tree_element;
  std::vector<uint8_t> item_v1;
  std::vector<uint8_t> item_v2;
  if (!grovedb::EncodeTreeToElementBytes(&tree_element, &error)) {
    Fail("EncodeTreeToElementBytes failed: " + error);
  }
  if (!grovedb::EncodeItemToElementBytes({'v', '1'}, &item_v1, &error) ||
      !grovedb::EncodeItemToElementBytes({'v', '2'}, &item_v2, &error)) {
    Fail("EncodeItemToElementBytes failed: " + error);
  }

  grovedb::OperationCost cost;
  if (!db.Insert({}, {'r', 'o', 'o', 't'}, tree_element, &cost, &error)) {
    Fail("Insert root tree with cost failed: " + error);
  }
  ExpectMutationCost(cost, "insert_root_tree");

  if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', '1'}, item_v1, &cost, &error)) {
    Fail("Insert item with cost failed: " + error);
  }
  ExpectMutationCost(cost, "insert_item");

  if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', '1'}, item_v2, &cost, &error)) {
    Fail("Replace item with cost failed: " + error);
  }
  if (cost.storage_cost.replaced_bytes == 0) {
    Fail("replace_item: expected replaced bytes");
  }

  bool deleted = false;
  if (!db.Delete({{'r', 'o', 'o', 't'}}, {'k', '1'}, &deleted, &cost, &error)) {
    Fail("Delete item with cost failed: " + error);
  }
  if (!deleted) {
    Fail("Delete item should set deleted=true");
  }
  if (cost.storage_cost.removed_bytes.TotalRemovedBytes() == 0) {
    Fail("delete_item: expected removed bytes");
  }

  grovedb::GroveDb::Transaction tx;
  if (!db.StartTransaction(&tx, &error)) {
    Fail("StartTransaction failed: " + error);
  }
  if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', 't', 'x'}, item_v1, &cost, &tx, &error)) {
    Fail("tx insert with cost failed: " + error);
  }
  ExpectMutationCost(cost, "tx_insert_item");
  if (!db.RollbackTransaction(&tx, &error)) {
    Fail("RollbackTransaction failed: " + error);
  }

  // Estimated batch costs should use real per-op cost accounting, not just a
  // fallback non-zero placeholder.
  grovedb::OperationCost estimated_batch_cost;
  std::vector<grovedb::GroveDb::BatchOp> estimated_replace_ops = {
      {grovedb::GroveDb::BatchOp::Kind::kReplace, {{'r', 'o', 'o', 't'}}, {'k', '1'}, item_v2}};
  if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', '1'}, item_v1, &error)) {
    Fail("Setup insert for estimated replace failed: " + error);
  }
  if (!db.EstimatedCaseOperationsForBatch(estimated_replace_ops, &estimated_batch_cost, &error)) {
    Fail("EstimatedCaseOperationsForBatch(replace) failed: " + error);
  }
  if (estimated_batch_cost.storage_cost.replaced_bytes == 0) {
    Fail("estimated_replace_batch: expected replaced bytes");
  }
  bool found = false;
  std::vector<uint8_t> got;
  if (!db.Get({{'r', 'o', 'o', 't'}}, {'k', '1'}, &got, &found, &error) || !found ||
      got != item_v1) {
    Fail("EstimatedCaseOperationsForBatch(replace) should not mutate data");
  }

  std::vector<grovedb::GroveDb::BatchOp> estimated_delete_ops = {
      {grovedb::GroveDb::BatchOp::Kind::kDelete, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {}}};
  if (!db.EstimatedCaseOperationsForBatch(estimated_delete_ops, &estimated_batch_cost, &error)) {
    Fail("EstimatedCaseOperationsForBatch(delete) failed: " + error);
  }
  if (estimated_batch_cost.storage_cost.removed_bytes.TotalRemovedBytes() == 0) {
    Fail("estimated_delete_batch: expected removed bytes");
  }
  if (!db.Get({{'r', 'o', 'o', 't'}}, {'k', '1'}, &got, &found, &error) || !found ||
      got != item_v1) {
    Fail("EstimatedCaseOperationsForBatch(delete) should not mutate data");
  }

  // Patch change_in_bytes should affect estimated cost shape (Rust patch cost
  // model does not report removed_bytes for shrinking patches).
  std::vector<uint8_t> item_long;
  std::vector<uint8_t> item_short;
  if (!grovedb::EncodeItemToElementBytes({'l', 'o', 'n', 'g'}, &item_long, &error) ||
      !grovedb::EncodeItemToElementBytes({'s'}, &item_short, &error)) {
    Fail("Encode patch cost fixture items failed: " + error);
  }
  if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', 'p'}, item_long, &error)) {
    Fail("Setup insert for estimated patch failed: " + error);
  }
  std::vector<grovedb::GroveDb::BatchOp> estimated_replace_shrink_ops = {
      {grovedb::GroveDb::BatchOp::Kind::kReplace, {{'r', 'o', 'o', 't'}}, {'k', 'p'}, item_short}};
  if (!db.EstimatedCaseOperationsForBatch(
          estimated_replace_shrink_ops, &estimated_batch_cost, &error)) {
    Fail("EstimatedCaseOperationsForBatch(replace shrink) failed: " + error);
  }
  const uint32_t replace_shrink_removed =
      estimated_batch_cost.storage_cost.removed_bytes.TotalRemovedBytes();
  if (replace_shrink_removed == 0) {
    Fail("estimated_replace_shrink_batch: expected removed bytes");
  }
  grovedb::GroveDb::BatchOp patch_shrink;
  patch_shrink.kind = grovedb::GroveDb::BatchOp::Kind::kPatch;
  patch_shrink.path = {{'r', 'o', 'o', 't'}};
  patch_shrink.key = {'k', 'p'};
  patch_shrink.element_bytes = item_short;
  patch_shrink.change_in_bytes = -1;
  if (!db.EstimatedCaseOperationsForBatch({patch_shrink}, &estimated_batch_cost, &error)) {
    Fail("EstimatedCaseOperationsForBatch(patch shrink) failed: " + error);
  }
  if (estimated_batch_cost.storage_cost.removed_bytes.TotalRemovedBytes() != 0) {
    Fail("estimated_patch_shrink_batch: expected no removed bytes when change_in_bytes < 0");
  }
  if (estimated_batch_cost.storage_cost.replaced_bytes == 0) {
    Fail("estimated_patch_shrink_batch: expected replaced bytes");
  }
  if (!db.Get({{'r', 'o', 'o', 't'}}, {'k', 'p'}, &got, &found, &error) || !found ||
      got != item_long) {
    Fail("EstimatedCaseOperationsForBatch(patch shrink) should not mutate data");
  }

  // Replace estimate should respect value-defined cost for layered values
  // instead of raw serialized byte length.
  std::vector<uint8_t> layered_tree_short;
  std::vector<uint8_t> layered_tree_long;
  if (!grovedb::EncodeCountTreeToElementBytesWithRootKey(nullptr, 1, &layered_tree_short, &error) ||
      !grovedb::EncodeCountTreeToElementBytesWithRootKey(
          nullptr, (static_cast<uint64_t>(1) << 20), &layered_tree_long, &error)) {
    Fail("Encode layered count tree fixtures failed: " + error);
  }
  if (layered_tree_short.size() == layered_tree_long.size()) {
    Fail("Layered tree fixtures should have different serialized sizes");
  }
  if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', 'l'}, layered_tree_short, &error)) {
    Fail("Setup insert for layered replace estimate failed: " + error);
  }
  std::vector<grovedb::GroveDb::BatchOp> layered_replace_ops = {
      {grovedb::GroveDb::BatchOp::Kind::kReplace, {{'r', 'o', 'o', 't'}}, {'k', 'l'}, layered_tree_long}};
  if (!db.EstimatedCaseOperationsForBatch(layered_replace_ops, &estimated_batch_cost, &error)) {
    Fail("EstimatedCaseOperationsForBatch(layered replace) failed: " + error);
  }
  if (estimated_batch_cost.storage_cost.removed_bytes.TotalRemovedBytes() != 0) {
    Fail("estimated_layered_replace_batch: expected no removed bytes for equal layered cost");
  }
  if (estimated_batch_cost.storage_cost.replaced_bytes == 0) {
    Fail("estimated_layered_replace_batch: expected replaced bytes");
  }
  if (!db.Get({{'r', 'o', 'o', 't'}}, {'k', 'l'}, &got, &found, &error) || !found ||
      got != layered_tree_short) {
    Fail("EstimatedCaseOperationsForBatch(layered replace) should not mutate data");
  }

  // Replace estimate should also respect specialized value-defined cost when
  // serialized size changes but the charged specialized cost stays the same.
  std::vector<uint8_t> specialized_short;
  std::vector<uint8_t> specialized_long;
  if (!grovedb::EncodeItemWithSumItemToElementBytes({'x'}, 1, &specialized_short, &error) ||
      !grovedb::EncodeItemWithSumItemToElementBytes(
          {'x'}, (static_cast<int64_t>(1) << 20), &specialized_long, &error)) {
    Fail("Encode specialized fixtures failed: " + error);
  }
  if (specialized_short.size() == specialized_long.size()) {
    Fail("Specialized fixtures should have different serialized sizes");
  }
  if (!db.Insert({{'r', 'o', 'o', 't'}}, {'k', 's'}, specialized_short, &error)) {
    Fail("Setup insert for specialized replace estimate failed: " + error);
  }
  std::vector<grovedb::GroveDb::BatchOp> specialized_replace_ops = {
      {grovedb::GroveDb::BatchOp::Kind::kReplace,
       {{'r', 'o', 'o', 't'}},
       {'k', 's'},
       specialized_long}};
  if (!db.EstimatedCaseOperationsForBatch(specialized_replace_ops, &estimated_batch_cost, &error)) {
    Fail("EstimatedCaseOperationsForBatch(specialized replace) failed: " + error);
  }
  if (estimated_batch_cost.storage_cost.removed_bytes.TotalRemovedBytes() != 0) {
    Fail("estimated_specialized_replace_batch: expected no removed bytes for equal specialized cost");
  }
  if (estimated_batch_cost.storage_cost.replaced_bytes == 0) {
    Fail("estimated_specialized_replace_batch: expected replaced bytes");
  }
  if (!db.Get({{'r', 'o', 'o', 't'}}, {'k', 's'}, &got, &found, &error) || !found ||
      got != specialized_short) {
    Fail("EstimatedCaseOperationsForBatch(specialized replace) should not mutate data");
  }

  // Disabled-consistency estimated batch path should follow Rust canonicalized
  // execution (same-key last-op wins, then ordered execution) and remain
  // no-write.
  grovedb::GroveDb::BatchApplyOptions disable_consistency_options;
  disable_consistency_options.disable_operation_consistency_check = true;
  std::vector<grovedb::GroveDb::BatchOp> estimated_duplicate_same_key_ops = {
      {grovedb::GroveDb::BatchOp::Kind::kDelete, {{'r', 'o', 'o', 't'}}, {'k', '1'}, {}},
      {grovedb::GroveDb::BatchOp::Kind::kInsert, {{'r', 'o', 'o', 't'}}, {'k', '1'}, item_v2}};
  if (!db.EstimatedCaseOperationsForBatch(
          estimated_duplicate_same_key_ops, disable_consistency_options, &estimated_batch_cost, &error)) {
    Fail("EstimatedCaseOperationsForBatch(disabled consistency duplicate same key) failed: " + error);
  }
  if (!db.Get({{'r', 'o', 'o', 't'}}, {'k', '1'}, &got, &found, &error) || !found ||
      got != item_v1) {
    Fail("EstimatedCaseOperationsForBatch(disabled consistency duplicate same key) should not mutate data");
  }

  std::vector<uint8_t> item_nested;
  if (!grovedb::EncodeItemToElementBytes({'n', 'v'}, &item_nested, &error)) {
    Fail("Encode nested fixture item failed: " + error);
  }
  std::vector<grovedb::GroveDb::BatchOp> estimated_reversed_parent_child_ops = {
      {grovedb::GroveDb::BatchOp::Kind::kInsert, {{'r', 'o', 'o', 't'}, {'o', 'r', 'd', '2'}}, {'n', 'k'}, item_nested},
      {grovedb::GroveDb::BatchOp::Kind::kInsertTree, {{'r', 'o', 'o', 't'}}, {'o', 'r', 'd', '2'}, tree_element}};
  if (!db.EstimatedCaseOperationsForBatch(
          estimated_reversed_parent_child_ops, disable_consistency_options, &estimated_batch_cost, &error)) {
    Fail("EstimatedCaseOperationsForBatch(disabled consistency parent/child reorder) failed: " + error);
  }
  if (db.Get({{'r', 'o', 'o', 't'}, {'o', 'r', 'd', '2'}}, {'n', 'k'}, &got, &found, &error) &&
      found) {
    Fail("EstimatedCaseOperationsForBatch(disabled consistency parent/child reorder) should not mutate data");
  }

  std::filesystem::remove_all(dir);
  return 0;
}
