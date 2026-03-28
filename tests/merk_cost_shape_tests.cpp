#include "merk.h"
#include "test_utils.h"

#include <iostream>
#include <string>
#include <vector>

using test_utils::Fail;

namespace {
uint64_t g_merk_cost_case_count = 0;

std::vector<uint8_t> Key(const char* s) {
  return std::vector<uint8_t>(s, s + std::char_traits<char>::length(s));
}

void AssertInsertCost(const grovedb::OperationCost& cost, const std::string& label) {
  if (cost.storage_cost.added_bytes == 0 || cost.hash_node_calls == 0) {
    Fail(label + ": insert cost should have added bytes and hash calls");
  }
  if (cost.storage_cost.replaced_bytes != 0 ||
      cost.storage_cost.removed_bytes.TotalRemovedBytes() != 0) {
    Fail(label + ": insert cost should not have replaced/removed bytes");
  }
}

void AssertReplaceCost(const grovedb::OperationCost& cost, const std::string& label) {
  if (cost.storage_cost.replaced_bytes == 0 || cost.hash_node_calls == 0) {
    Fail(label + ": replace cost should have replaced bytes and hash calls");
  }
  if (cost.storage_cost.removed_bytes.TotalRemovedBytes() != 0) {
    Fail(label + ": replace cost should not report removed bytes");
  }
}

void AssertDeleteCost(const grovedb::OperationCost& cost, const std::string& label) {
  if (cost.storage_cost.removed_bytes.TotalRemovedBytes() == 0 || cost.hash_node_calls == 0) {
    Fail(label + ": delete cost should have removed bytes and hash calls");
  }
}

void AssertMissingDeleteCost(const grovedb::OperationCost& cost, const std::string& label) {
  if (cost.storage_cost.added_bytes != 0 || cost.storage_cost.replaced_bytes != 0 ||
      cost.storage_cost.removed_bytes.TotalRemovedBytes() != 0) {
    Fail(label + ": missing delete should not mutate storage cost");
  }
}

void RunCase(grovedb::TreeFeatureTypeTag tag, const std::string& label) {
  grovedb::MerkTree tree;
  tree.SetTreeFeatureTag(tag);
  std::string error;
  grovedb::OperationCost cost;

  if (!tree.Insert(Key("k"), Key("v1"), &cost, &error)) {
    Fail(label + ": insert failed: " + error);
  }
  AssertInsertCost(cost, label);

  cost.Reset();
  if (!tree.Insert(Key("k"), Key("v2"), &cost, &error)) {
    Fail(label + ": replace failed: " + error);
  }
  AssertReplaceCost(cost, label);

  cost.Reset();
  bool deleted = false;
  if (!tree.Delete(Key("k"), &deleted, &cost, &error)) {
    Fail(label + ": delete failed: " + error);
  }
  if (!deleted) {
    Fail(label + ": delete should mark key as deleted");
  }
  AssertDeleteCost(cost, label);

  cost.Reset();
  deleted = true;
  if (!tree.Delete(Key("missing"), &deleted, &cost, &error)) {
    Fail(label + ": missing delete failed: " + error);
  }
  if (deleted) {
    Fail(label + ": missing delete should not mark deleted");
  }
  AssertMissingDeleteCost(cost, label);
  g_merk_cost_case_count += 1;
  std::cout << "MERK_COST_CASE label=" << label << " status=PASS\n";
}

}  // namespace

int main() {
  RunCase(grovedb::TreeFeatureTypeTag::kBasic, "basic");
  RunCase(grovedb::TreeFeatureTypeTag::kSum, "sum");
  RunCase(grovedb::TreeFeatureTypeTag::kBigSum, "big-sum");
  RunCase(grovedb::TreeFeatureTypeTag::kCount, "count");
  RunCase(grovedb::TreeFeatureTypeTag::kCountSum, "count-sum");
  RunCase(grovedb::TreeFeatureTypeTag::kProvableCount, "prov-count");
  RunCase(grovedb::TreeFeatureTypeTag::kProvableCountSum, "prov-count-sum");
  std::cout << "MERK_COST_SUMMARY checked=" << g_merk_cost_case_count << " status=PASS\n";
  return 0;
}
