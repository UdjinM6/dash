#include "merk.h"

#include "element.h"
#include "hash.h"
#include "proof.h"
#include "test_utils.h"

#include <map>
#include <limits>
#include <string>
#include <vector>

using test_utils::Fail;

namespace {

std::vector<uint8_t> KeyFromByte(uint8_t value) {
  return std::vector<uint8_t>{value};
}

std::vector<uint8_t> KeyFromString(const std::string& value) {
  return std::vector<uint8_t>(value.begin(), value.end());
}

void ExpectRoot(grovedb::MerkTree* tree, uint8_t expected) {
  if (tree == nullptr) {
    Fail("tree is null");
  }
  std::vector<uint8_t> root;
  if (!tree->RootKey(&root)) {
    Fail("missing root key");
  }
  if (root.size() != 1 || root[0] != expected) {
    Fail("unexpected root key");
  }
}

grovedb::MerkTree::ValueHashFn DefaultValueHashFn() {
  return [](const std::vector<uint8_t>&,
            const std::vector<uint8_t>& value,
            std::vector<uint8_t>* out,
            std::string* error) -> bool {
    return grovedb::ValueHash(value, out, error);
  };
}

void ExpectContainsKey(const std::vector<std::vector<uint8_t>>& keys,
                       const std::vector<uint8_t>& key,
                       const std::string& label) {
  for (const auto& entry : keys) {
    if (entry == key) {
      return;
    }
  }
  Fail("expected proof keys to contain " + label);
}

std::vector<uint8_t> EncodeAggregateValueForTag(grovedb::TreeFeatureTypeTag tag,
                                                int index,
                                                std::string* error) {
  std::vector<uint8_t> out;
  switch (tag) {
    case grovedb::TreeFeatureTypeTag::kBasic:
      if (!grovedb::EncodeItemToElementBytes(KeyFromByte(static_cast<uint8_t>(index & 0xFF)), &out, error)) {
        return {};
      }
      break;
    case grovedb::TreeFeatureTypeTag::kSum: {
      if (!grovedb::EncodeSumTreeToElementBytesWithRootKey(nullptr, (index % 17) - 8, &out, error)) {
        return {};
      }
      break;
    }
    case grovedb::TreeFeatureTypeTag::kBigSum: {
      // Keep values within int64 for cross-checking against ComputeSumBig(sum_fn),
      // whose callback surface is int64-based.
      __int128 value = static_cast<__int128>(500000) + static_cast<__int128>(index) * 97;
      if ((index % 2) == 0) {
        value = -value;
      }
      if (!grovedb::EncodeBigSumTreeToElementBytesWithRootKey(nullptr, value, &out, error)) {
        return {};
      }
      break;
    }
    case grovedb::TreeFeatureTypeTag::kCount:
      if (!grovedb::EncodeCountTreeToElementBytesWithRootKey(nullptr, 0, &out, error)) {
        return {};
      }
      break;
    case grovedb::TreeFeatureTypeTag::kCountSum:
      if (!grovedb::EncodeCountSumTreeToElementBytesWithRootKey(nullptr, 0, (index % 23) - 11, &out, error)) {
        return {};
      }
      break;
    case grovedb::TreeFeatureTypeTag::kProvableCount:
      if (!grovedb::EncodeProvableCountTreeToElementBytesWithRootKey(nullptr, 0, &out, error)) {
        return {};
      }
      break;
    case grovedb::TreeFeatureTypeTag::kProvableCountSum:
      if (!grovedb::EncodeProvableCountSumTreeToElementBytesWithRootKey(
              nullptr, 0, (index % 19) - 9, &out, error)) {
        return {};
      }
      break;
  }
  return out;
}

void ExpectRootAggregateMatchesSlow(grovedb::MerkTree* tree,
                                    grovedb::TreeFeatureTypeTag tag,
                                    const std::string& label,
                                    std::string* error) {
  if (tree == nullptr) {
    Fail(label + ": tree is null");
  }
  grovedb::MerkTree::RootAggregate agg;
  if (!tree->RootAggregateData(&agg, error)) {
    Fail(label + ": RootAggregateData failed: " + *error);
  }

  const bool expected_has_count =
      (tag == grovedb::TreeFeatureTypeTag::kCount ||
       tag == grovedb::TreeFeatureTypeTag::kCountSum ||
       tag == grovedb::TreeFeatureTypeTag::kProvableCount ||
       tag == grovedb::TreeFeatureTypeTag::kProvableCountSum);
  const bool expected_has_sum =
      (tag == grovedb::TreeFeatureTypeTag::kSum ||
       tag == grovedb::TreeFeatureTypeTag::kBigSum ||
       tag == grovedb::TreeFeatureTypeTag::kCountSum ||
       tag == grovedb::TreeFeatureTypeTag::kProvableCountSum);
  if (agg.has_count != expected_has_count || agg.has_sum != expected_has_sum) {
    Fail(label + ": RootAggregateData feature flags mismatch");
  }

  if (expected_has_count) {
    uint64_t full_count = 0;
    if (!tree->ComputeCount(&full_count, error)) {
      Fail(label + ": ComputeCount failed: " + *error);
    }
    if (agg.count != full_count) {
      Fail(label + ": count mismatch");
    }
  }

  if (expected_has_sum) {
    grovedb::MerkTree::SumValueFn sum_fn;
    if (tag == grovedb::TreeFeatureTypeTag::kBigSum) {
      sum_fn = [](const std::vector<uint8_t>& value,
                  int64_t* out_sum,
                  bool* has_sum,
                  std::string* err) -> bool {
        __int128 big_sum = 0;
        bool has_big_sum = false;
        if (!grovedb::ExtractBigSumValueFromElementBytes(value, &big_sum, &has_big_sum, err)) {
          return false;
        }
        *has_sum = has_big_sum;
        if (!has_big_sum) {
          *out_sum = 0;
          return true;
        }
        if (big_sum > static_cast<__int128>(std::numeric_limits<int64_t>::max()) ||
            big_sum < static_cast<__int128>(std::numeric_limits<int64_t>::min())) {
          if (err) {
            *err = "big sum does not fit in int64 for test cross-check";
          }
          return false;
        }
        *out_sum = static_cast<int64_t>(big_sum);
        return true;
      };
    } else {
      sum_fn = [](const std::vector<uint8_t>& value,
                  int64_t* out_sum,
                  bool* has_sum,
                  std::string* err) -> bool {
        return grovedb::ExtractSumValueFromElementBytes(value, out_sum, has_sum, err);
      };
    }
    __int128 full_sum = 0;
    if (!tree->ComputeSumBig(sum_fn, &full_sum, error)) {
      Fail(label + ": ComputeSumBig failed: " + *error);
    }
    if (agg.sum != full_sum) {
      Fail(label + ": sum mismatch");
    }
  }
}
}  // namespace

int main() {
  grovedb::MerkTree tree;
  std::string error;

  std::vector<uint8_t> insert_order = {30, 20, 40, 10, 25, 35, 50, 5, 15, 27};
  for (uint8_t key : insert_order) {
    std::vector<uint8_t> value = {static_cast<uint8_t>(key + 1)};
    if (!tree.Insert(KeyFromByte(key), value, &error)) {
      Fail("insert failed: " + error);
    }
    if (!tree.Validate(&error)) {
      Fail("avl validation failed after insert: " + error);
    }
  }

  for (uint8_t key : insert_order) {
    std::vector<uint8_t> value;
    if (!tree.Get(KeyFromByte(key), &value)) {
      Fail("missing key after insert");
    }
    if (value.size() != 1 || value[0] != static_cast<uint8_t>(key + 1)) {
      Fail("unexpected value after insert");
    }
  }

  std::vector<uint8_t> delete_order = {20, 40, 30, 5, 27};
  for (uint8_t key : delete_order) {
    bool deleted = false;
    if (!tree.Delete(KeyFromByte(key), &deleted, &error)) {
      Fail("delete failed: " + error);
    }
    if (!deleted) {
      Fail("expected key to be deleted");
    }
    if (!tree.Validate(&error)) {
      Fail("avl validation failed after delete: " + error);
    }
    std::vector<uint8_t> value;
    if (tree.Get(KeyFromByte(key), &value)) {
      Fail("deleted key still present");
    }
  }

  {
    grovedb::MerkTree ll;
    if (!ll.Insert(KeyFromByte(3), KeyFromByte(4), &error) ||
        !ll.Insert(KeyFromByte(2), KeyFromByte(3), &error) ||
        !ll.Insert(KeyFromByte(1), KeyFromByte(2), &error)) {
      Fail("insert failed for ll");
    }
    if (!ll.Validate(&error)) {
      Fail("ll validation failed");
    }
    ExpectRoot(&ll, 2);
  }

  {
    grovedb::MerkTree rr;
    if (!rr.Insert(KeyFromByte(1), KeyFromByte(2), &error) ||
        !rr.Insert(KeyFromByte(2), KeyFromByte(3), &error) ||
        !rr.Insert(KeyFromByte(3), KeyFromByte(4), &error)) {
      Fail("insert failed for rr");
    }
    if (!rr.Validate(&error)) {
      Fail("rr validation failed");
    }
    ExpectRoot(&rr, 2);
  }

  {
    grovedb::MerkTree lr;
    if (!lr.Insert(KeyFromByte(3), KeyFromByte(4), &error) ||
        !lr.Insert(KeyFromByte(1), KeyFromByte(2), &error) ||
        !lr.Insert(KeyFromByte(2), KeyFromByte(3), &error)) {
      Fail("insert failed for lr");
    }
    if (!lr.Validate(&error)) {
      Fail("lr validation failed");
    }
    ExpectRoot(&lr, 2);
  }

  {
    grovedb::MerkTree rl;
    if (!rl.Insert(KeyFromByte(1), KeyFromByte(2), &error) ||
        !rl.Insert(KeyFromByte(3), KeyFromByte(4), &error) ||
        !rl.Insert(KeyFromByte(2), KeyFromByte(3), &error)) {
      Fail("insert failed for rl");
    }
    if (!rl.Validate(&error)) {
      Fail("rl validation failed");
    }
    ExpectRoot(&rl, 2);
  }

  {
    grovedb::MerkTree delete_ll;
    std::vector<uint8_t> keys = {3, 2, 4, 1};
    for (uint8_t key : keys) {
      if (!delete_ll.Insert(KeyFromByte(key), KeyFromByte(key + 1), &error)) {
        Fail("insert failed for delete ll");
      }
    }
    bool deleted = false;
    if (!delete_ll.Delete(KeyFromByte(4), &deleted, &error) || !deleted) {
      Fail("delete failed for delete ll");
    }
    if (!delete_ll.Validate(&error)) {
      Fail("delete ll validation failed");
    }
    ExpectRoot(&delete_ll, 2);
  }

  {
    grovedb::MerkTree delete_rr;
    std::vector<uint8_t> keys = {2, 1, 3, 4};
    for (uint8_t key : keys) {
      if (!delete_rr.Insert(KeyFromByte(key), KeyFromByte(key + 1), &error)) {
        Fail("insert failed for delete rr");
      }
    }
    bool deleted = false;
    if (!delete_rr.Delete(KeyFromByte(1), &deleted, &error) || !deleted) {
      Fail("delete failed for delete rr");
    }
    if (!delete_rr.Validate(&error)) {
      Fail("delete rr validation failed");
    }
    ExpectRoot(&delete_rr, 3);
  }

  {
    grovedb::MerkTree cost_tree;
    grovedb::OperationCost cost;
    if (!cost_tree.Insert(KeyFromByte(1), KeyFromByte(2), &cost, &error)) {
      Fail("insert with cost failed");
    }
    if (cost.storage_cost.added_bytes == 0 || cost.hash_node_calls == 0) {
      Fail("insert cost missing add/hash data");
    }
    cost.Reset();
    if (!cost_tree.Insert(KeyFromByte(1), KeyFromByte(3), &cost, &error)) {
      Fail("replace with cost failed");
    }
    if (cost.storage_cost.replaced_bytes == 0 || cost.hash_node_calls == 0) {
      Fail("replace cost missing replaced/hash data");
    }
    cost.Reset();
    bool deleted = false;
    if (!cost_tree.Delete(KeyFromByte(1), &deleted, &cost, &error)) {
      Fail("delete with cost failed");
    }
    if (!deleted || cost.storage_cost.removed_bytes.TotalRemovedBytes() == 0 ||
        cost.hash_node_calls == 0) {
      Fail("delete cost missing remove/hash data");
    }
  }

  {
    grovedb::MerkTree empty_tree;
    grovedb::MerkTree::RootAggregate agg;
    if (!empty_tree.RootAggregateData(&agg, &error)) {
      Fail("RootAggregateData failed for empty tree: " + error);
    }
    if (agg.has_count || agg.has_sum || agg.count != 0 || agg.sum != 0) {
      Fail("unexpected RootAggregateData for empty tree");
    }
  }

  {
    const std::vector<grovedb::TreeFeatureTypeTag> tags = {
        grovedb::TreeFeatureTypeTag::kSum,
        grovedb::TreeFeatureTypeTag::kBigSum,
        grovedb::TreeFeatureTypeTag::kCount,
        grovedb::TreeFeatureTypeTag::kCountSum,
        grovedb::TreeFeatureTypeTag::kProvableCount,
        grovedb::TreeFeatureTypeTag::kProvableCountSum,
    };
    for (size_t tag_index = 0; tag_index < tags.size(); ++tag_index) {
      grovedb::TreeFeatureTypeTag tag = tags[tag_index];
      grovedb::MerkTree agg_tree;
      agg_tree.SetTreeFeatureTag(tag);
      for (int i = 0; i < 128; ++i) {
        std::vector<uint8_t> value = EncodeAggregateValueForTag(tag, i + static_cast<int>(tag_index) * 1000, &error);
        if (value.empty() && !error.empty()) {
          Fail("encode aggregate test value failed: " + error);
        }
        if (!agg_tree.Insert(KeyFromString("k" + std::to_string(i)), value, &error)) {
          Fail("aggregate test insert failed: " + error);
        }
      }
      const std::string tag_label = "aggregate test tag=" + std::to_string(static_cast<int>(tag));
      ExpectRootAggregateMatchesSlow(&agg_tree, tag, tag_label + " before delete", &error);
      for (int i = 0; i < 32; ++i) {
        bool deleted = false;
        if (!agg_tree.Delete(KeyFromString("k" + std::to_string(i * 2)), &deleted, &error)) {
          Fail("aggregate test delete failed: " + error);
        }
        if (!deleted) {
          Fail("aggregate test expected delete=true");
        }
      }
      ExpectRootAggregateMatchesSlow(&agg_tree, tag, tag_label + " after delete", &error);
      if (!agg_tree.Validate(&error)) {
        Fail("aggregate test validate failed: " + error);
      }
    }
  }

  {
    grovedb::MerkTree sum_tree_empty;
    auto sum_fn = [](const std::vector<uint8_t>& value,
                     int64_t* out_sum,
                     bool* has_sum,
                     std::string* err) -> bool {
      return grovedb::ExtractSumValueFromElementBytes(value, out_sum, has_sum, err);
    };
    int64_t sum = 0;
    uint64_t count = 0;
    if (!sum_tree_empty.ComputeSum(sum_fn, &sum, &error)) {
      Fail("compute sum failed for empty tree: " + error);
    }
    if (!sum_tree_empty.ComputeCountAndSum(sum_fn, &count, &sum, &error)) {
      Fail("compute count/sum failed for empty tree: " + error);
    }
    if (sum != 0 || count != 0) {
      Fail("unexpected sum/count for empty tree");
    }

    grovedb::MerkTree mixed_sum_tree;
    std::vector<uint8_t> sum_item;
    if (!grovedb::EncodeSumItemToElementBytes(-5, &sum_item, &error)) {
      Fail("encode sum item failed: " + error);
    }
    if (!mixed_sum_tree.Insert(KeyFromByte(1), sum_item, &error)) {
      Fail("insert sum item failed: " + error);
    }
    std::vector<uint8_t> item_with_sum;
    if (!grovedb::EncodeItemWithSumItemToElementBytes(KeyFromByte(9), 12, &item_with_sum,
                                                      &error)) {
      Fail("encode item with sum failed: " + error);
    }
    if (!mixed_sum_tree.Insert(KeyFromByte(2), item_with_sum, &error)) {
      Fail("insert item with sum failed: " + error);
    }
    std::vector<uint8_t> item;
    if (!grovedb::EncodeItemToElementBytes(KeyFromByte(7), &item, &error)) {
      Fail("encode item failed: " + error);
    }
    if (!mixed_sum_tree.Insert(KeyFromByte(3), item, &error)) {
      Fail("insert plain item failed: " + error);
    }
    if (!mixed_sum_tree.ComputeSum(sum_fn, &sum, &error)) {
      Fail("compute sum failed for mixed tree: " + error);
    }
    if (sum != 7) {
      Fail("unexpected mixed sum");
    }
  }

  {
    grovedb::MerkTree sum_tree;
    auto sum_fn = [](const std::vector<uint8_t>& value,
                     int64_t* out_sum,
                     bool* has_sum,
                     std::string* err) -> bool {
      return grovedb::ExtractSumValueFromElementBytes(value, out_sum, has_sum, err);
    };

    std::vector<uint8_t> sum_value;
    if (!grovedb::EncodeSumItemToElementBytes(10, &sum_value, &error)) {
      Fail("encode sum item failed: " + error);
    }
    if (!sum_tree.Insert(KeyFromByte(1), sum_value, &error)) {
      Fail("insert sum item failed: " + error);
    }

    std::vector<uint8_t> item_with_sum;
    if (!grovedb::EncodeItemWithSumItemToElementBytes(KeyFromByte(9), 7, &item_with_sum,
                                                      &error)) {
      Fail("encode item with sum failed: " + error);
    }
    if (!sum_tree.Insert(KeyFromByte(2), item_with_sum, &error)) {
      Fail("insert item with sum failed: " + error);
    }

    std::vector<uint8_t> plain_item;
    if (!grovedb::EncodeItemToElementBytes(KeyFromByte(5), &plain_item, &error)) {
      Fail("encode item failed: " + error);
    }
    if (!sum_tree.Insert(KeyFromByte(3), plain_item, &error)) {
      Fail("insert plain item failed: " + error);
    }

    int64_t sum = 0;
    if (!sum_tree.ComputeSum(sum_fn, &sum, &error)) {
      Fail("compute sum failed: " + error);
    }
    if (sum != 17) {
      Fail("unexpected sum");
    }

    uint64_t count = 0;
    if (!sum_tree.ComputeCountAndSum(sum_fn, &count, &sum, &error)) {
      Fail("compute count/sum failed: " + error);
    }
    if (count != 3 || sum != 17) {
      Fail("unexpected count/sum");
    }

    __int128 big_sum = 0;
    if (!sum_tree.ComputeSumBig(sum_fn, &big_sum, &error)) {
      Fail("compute big sum failed: " + error);
    }
    if (big_sum != 17) {
      Fail("unexpected big sum");
    }
  }

  {
    grovedb::MerkTree proof_tree;
    auto value_hash_fn = DefaultValueHashFn();
    const std::vector<uint8_t> key_a = KeyFromString("a");
    const std::vector<uint8_t> key_b = KeyFromString("b");
    const std::vector<uint8_t> key_c = KeyFromString("c");
    const std::vector<uint8_t> val_a = KeyFromString("va");
    const std::vector<uint8_t> val_b = KeyFromString("vb");
    const std::vector<uint8_t> val_c = KeyFromString("vc");
    if (!proof_tree.Insert(key_a, val_a, &error) ||
        !proof_tree.Insert(key_b, val_b, &error) ||
        !proof_tree.Insert(key_c, val_c, &error)) {
      Fail("insert failed for proof tree: " + error);
    }
    std::vector<uint8_t> root_hash;
    if (!proof_tree.ComputeRootHash(value_hash_fn, &root_hash, &error)) {
      Fail("compute root hash failed: " + error);
    }

    std::vector<uint8_t> proof;
    std::vector<uint8_t> proof_root;
    std::vector<uint8_t> proof_value;
    if (!proof_tree.GenerateProof(key_b,
                                  grovedb::TargetEncoding::kKv,
                                  value_hash_fn,
                                  &proof,
                                  &proof_root,
                                  &proof_value,
                                  &error)) {
      Fail("generate proof failed: " + error);
    }
    if (proof_root != root_hash) {
      Fail("proof root hash mismatch");
    }
    if (proof_value != val_b) {
      Fail("proof value mismatch");
    }
    std::vector<std::vector<uint8_t>> proof_keys;
    if (!grovedb::CollectProofKeys(proof, &proof_keys, &error)) {
      Fail("collect proof keys failed: " + error);
    }
    ExpectContainsKey(proof_keys, key_b, "b");

    std::vector<uint8_t> absence_proof;
    std::vector<uint8_t> absence_root;
    const std::vector<uint8_t> absent_key = KeyFromString("bb");
    if (!proof_tree.GenerateAbsenceProof(absent_key,
                                         value_hash_fn,
                                         &absence_proof,
                                         &absence_root,
                                         &error)) {
      Fail("generate absence proof failed: " + error);
    }
    if (absence_root != root_hash) {
      Fail("absence root hash mismatch");
    }

    std::vector<uint8_t> range_proof;
    std::vector<uint8_t> range_root;
    if (!proof_tree.GenerateRangeProof(key_a,
                                       key_c,
                                       true,
                                       true,
                                       value_hash_fn,
                                       &range_proof,
                                       &range_root,
                                       &error)) {
      Fail("generate range proof failed: " + error);
    }
    if (range_root != root_hash) {
      Fail("range root hash mismatch");
    }
  }

  {
    grovedb::MerkTree count_tree;
    auto value_hash_fn = DefaultValueHashFn();
    if (!count_tree.Insert(KeyFromString("a"), KeyFromString("va"), &error) ||
        !count_tree.Insert(KeyFromString("b"), KeyFromString("vb"), &error) ||
        !count_tree.Insert(KeyFromString("c"), KeyFromString("vc"), &error)) {
      Fail("insert failed for count tree: " + error);
    }
    std::vector<uint8_t> root_hash;
    uint64_t count = 0;
    if (!count_tree.ComputeRootHashWithCount(value_hash_fn, &root_hash, &count, &error)) {
      Fail("compute root hash with count failed: " + error);
    }
    if (count != 3) {
      Fail("unexpected count for count tree");
    }

    std::vector<uint8_t> proof;
    std::vector<uint8_t> proof_root;
    std::vector<uint8_t> proof_value;
    if (!count_tree.GenerateProofWithCount(KeyFromString("b"),
                                           grovedb::TargetEncoding::kKv,
                                           value_hash_fn,
                                           &proof,
                                           &proof_root,
                                           &proof_value,
                                           &error)) {
      Fail("generate proof with count failed: " + error);
    }
    if (proof_root != root_hash) {
      Fail("proof with count root mismatch");
    }

    std::vector<uint8_t> absence_proof;
    std::vector<uint8_t> absence_root;
    if (!count_tree.GenerateAbsenceProofWithCount(KeyFromString("bb"),
                                                  value_hash_fn,
                                                  &absence_proof,
                                                  &absence_root,
                                                  &error)) {
      Fail("absence proof with count failed: " + error);
    }
    if (absence_root != root_hash) {
      Fail("absence proof with count root mismatch");
    }

    std::vector<uint8_t> range_proof;
    std::vector<uint8_t> range_root;
    if (!count_tree.GenerateRangeProofWithCount(KeyFromString("a"),
                                                KeyFromString("c"),
                                                true,
                                                true,
                                                value_hash_fn,
                                                &range_proof,
                                                &range_root,
                                                &error)) {
      Fail("range proof with count failed: " + error);
    }
    if (range_root != root_hash) {
      Fail("range proof with count root mismatch");
    }
  }

  {
    grovedb::MerkTree proof_tree;
    auto value_hash_fn = DefaultValueHashFn();
    if (!proof_tree.Insert(KeyFromString("k1"), KeyFromString("v1"), &error) ||
        !proof_tree.Insert(KeyFromString("k2"), KeyFromString("v2"), &error) ||
        !proof_tree.Insert(KeyFromString("k3"), KeyFromString("v3"), &error)) {
      Fail("insert failed for proof encoding tree: " + error);
    }
    const std::vector<grovedb::TargetEncoding> encodings = {
        grovedb::TargetEncoding::kKv,
        grovedb::TargetEncoding::kKvValueHash,
    };
    for (auto encoding : encodings) {
      std::vector<uint8_t> proof;
      std::vector<uint8_t> root;
      std::vector<uint8_t> value;
      if (!proof_tree.GenerateProof(KeyFromString("k2"),
                                    encoding,
                                    value_hash_fn,
                                    &proof,
                                    &root,
                                    &value,
                                    &error)) {
        Fail("generate proof encoding failed: " + error);
      }
      std::vector<std::vector<uint8_t>> keys;
      if (!grovedb::CollectProofKeys(proof, &keys, &error)) {
        Fail("collect proof keys failed: " + error);
      }
      if (keys.empty()) {
        Fail("expected proof keys for encoding");
      }
    }
    {
      std::vector<uint8_t> proof;
      std::vector<uint8_t> root;
      if (!proof_tree.GenerateRangeProof(KeyFromString("k1"),
                                         KeyFromString("k3"),
                                         true,
                                         true,
                                         value_hash_fn,
                                         &proof,
                                         &root,
                                         &error)) {
        Fail("generate range proof encoding failed: " + error);
      }
      std::vector<std::vector<uint8_t>> keys;
      if (!grovedb::CollectProofKeys(proof, &keys, &error)) {
        Fail("collect range proof keys failed: " + error);
      }
      if (keys.empty()) {
        Fail("expected range proof keys");
      }
    }
  }

  {
    grovedb::MerkTree fuzz_tree;
    std::map<std::vector<uint8_t>, std::vector<uint8_t>> expected;
    uint32_t seed = 0xC001CAFEu;
    auto next_u32 = [&seed]() -> uint32_t {
      seed ^= seed << 13;
      seed ^= seed >> 17;
      seed ^= seed << 5;
      return seed;
    };
    for (size_t i = 0; i < 300; ++i) {
      uint8_t key = static_cast<uint8_t>(next_u32() % 64);
      std::vector<uint8_t> key_bytes = KeyFromByte(key);
      if ((next_u32() & 1u) == 0) {
        std::vector<uint8_t> value = {static_cast<uint8_t>(key + 1)};
        if (!fuzz_tree.Insert(key_bytes, value, &error)) {
          Fail("fuzz insert failed: " + error);
        }
        expected[key_bytes] = value;
      } else {
        bool deleted = false;
        if (!fuzz_tree.Delete(key_bytes, &deleted, &error)) {
          Fail("fuzz delete failed: " + error);
        }
        if (deleted) {
          expected.erase(key_bytes);
        }
      }
      if (!fuzz_tree.Validate(&error)) {
        Fail("fuzz tree validation failed: " + error);
      }
    }
    uint64_t count = 0;
    if (!fuzz_tree.ComputeCount(&count, &error)) {
      Fail("fuzz tree compute count failed: " + error);
    }
    if (count != expected.size()) {
      Fail("fuzz tree count mismatch");
    }
    for (const auto& entry : expected) {
      std::vector<uint8_t> actual;
      if (!fuzz_tree.Get(entry.first, &actual) || actual != entry.second) {
        Fail("fuzz tree value mismatch");
      }
    }
  }

  return 0;
}
