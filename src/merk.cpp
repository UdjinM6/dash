#include "merk.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <iostream>
#include <unordered_set>

#include "binary.h"
#include "chunk.h"
#include "element.h"
#include "hash.h"
#include "insert_profile.h"
#include "merk_node.h"
#include "merk_costs.h"
#include "operation_cost.h"
#include "rocksdb_wrapper.h"

namespace grovedb {

namespace {

bool DebugFacadeCostEnabled() {
  const char* env = std::getenv("GROVEDB_DEBUG_FACADE_COST");
  return env != nullptr && env[0] != '\0' && env[0] != '0';
}

bool CompareKeys(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
  return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end());
}

int CompareKeyOrder(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
  if (a == b) {
    return 0;
  }
  return CompareKeys(a, b) ? -1 : 1;
}

bool ComputeValueHash(const std::vector<uint8_t>& key,
                      const std::vector<uint8_t>& value,
                      const MerkTree::ValueHashFn& value_hash_fn,
                      std::vector<uint8_t>* out,
                      std::string* error) {
  insert_profile::AddCounter(insert_profile::Counter::kComputeValueHashCalls);
  if (value_hash_fn) {
    return value_hash_fn(key, value, out, error);
  }
  return ValueHash(value, out, error);
}

}  // namespace

OperationCost::TreeCostType MerkTree::TreeCostTypeForFeature(TreeFeatureTypeTag tag) {
  switch (tag) {
    case TreeFeatureTypeTag::kSum:
    case TreeFeatureTypeTag::kCount:
    case TreeFeatureTypeTag::kProvableCount:
      return OperationCost::TreeCostType::kVarIntAs8Bytes;
    case TreeFeatureTypeTag::kCountSum:
    case TreeFeatureTypeTag::kProvableCountSum:
      return OperationCost::TreeCostType::kTwoVarIntsAs16Bytes;
    case TreeFeatureTypeTag::kBigSum:
      return OperationCost::TreeCostType::kFixed16Bytes;
    case TreeFeatureTypeTag::kBasic:
      return OperationCost::TreeCostType::kVarIntAs8Bytes;
  }
  return OperationCost::TreeCostType::kVarIntAs8Bytes;
}

bool MerkTree::BuildChildCostInfo(const Node* child,
                                  const Node::ChildMeta& meta,
                                  ChildCostInfo* out) {
  if (out == nullptr) {
    return false;
  }
  if (child == nullptr && !meta.present) {
    out->present = false;
    out->key_len = 0;
    out->sum_len = 0;
    return true;
  }
  Link link;
  if (child != nullptr) {
    link.key = child->key;
  } else if (meta.present) {
    link.key = meta.key;
  }
  if (meta.present) {
    link.aggregate = meta.aggregate;
  } else {
    link.aggregate.tag = AggregateDataTag::kNone;
  }
  return ComputeChildCostInfo(link, out);
}

bool MerkTree::BuildChildrenSizes(const Node* node,
                                  OperationCost::ChildrenSizesWithIsSumTree* out,
                                  std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "children sizes output is null";
    }
    return false;
  }
  if (node == nullptr) {
    *out = std::nullopt;
    return true;
  }
  std::optional<std::pair<OperationCost::TreeCostType, uint32_t>> sum_tree_info;
  TreeFeatureType feature;
  feature.tag = TreeFeatureTypeTag::kBasic;
  TreeFeatureCostInfo feature_cost;
  if (!ComputeTreeFeatureCostInfo(feature, &feature_cost, error)) {
    return false;
  }
  if (feature_cost.present) {
    sum_tree_info = std::make_pair(TreeCostTypeForFeature(feature_cost.tag),
                                   feature_cost.sum_len);
  }

  std::optional<OperationCost::ChildSize> left_child;
  std::optional<OperationCost::ChildSize> right_child;
  ChildCostInfo left_info;
  ChildCostInfo right_info;
  if (!BuildChildCostInfo(node->left.get(), node->left_meta, &left_info)) {
    if (error) {
      *error = "failed to compute left child cost info";
    }
    return false;
  }
  if (!BuildChildCostInfo(node->right.get(), node->right_meta, &right_info)) {
    if (error) {
      *error = "failed to compute right child cost info";
    }
    return false;
  }
  if (left_info.present) {
    left_child = std::make_pair(left_info.key_len, left_info.sum_len);
  }
  if (right_info.present) {
    right_child = std::make_pair(right_info.key_len, right_info.sum_len);
  }
  *out = std::make_tuple(sum_tree_info, left_child, right_child);
  return true;
}

bool MerkTree::ComputePaidValueLen(
    uint32_t key_len,
    uint32_t value_len,
    const OperationCost::ChildrenSizesWithIsSumTree& children_sizes,
    uint32_t* out) {
  if (out == nullptr) {
    return false;
  }
  uint32_t paid_value_len = value_len;
  if (children_sizes.has_value()) {
    auto [sum_tree_info, left_child, right_child] = *children_sizes;
    paid_value_len -= 2;
    if (left_child.has_value()) {
      paid_value_len -= left_child->first;
      paid_value_len -= left_child->second;
    }
    if (right_child.has_value()) {
      paid_value_len -= right_child->first;
      paid_value_len -= right_child->second;
    }
    uint32_t sum_tree_node_size = 0;
    if (sum_tree_info.has_value()) {
      sum_tree_node_size = OperationCost::TreeCostSize(sum_tree_info->first);
      paid_value_len -= sum_tree_info->second;
      paid_value_len += sum_tree_node_size;
    }
    paid_value_len += RequiredSpaceU32(paid_value_len);
    paid_value_len += key_len + 4 + sum_tree_node_size;
  } else {
    paid_value_len += RequiredSpaceU32(paid_value_len);
  }
  *out = paid_value_len;
  return true;
}

namespace {

bool EncodeKv(const std::vector<uint8_t>& key,
              const std::vector<uint8_t>& value,
              std::vector<uint8_t>* out,
              std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "proof output is null";
    }
    return false;
  }
  if (key.size() > 255) {
    if (error) {
      *error = "key length exceeds u8";
    }
    return false;
  }
  if (value.size() > 0xFFFFFFFFu) {
    if (error) {
      *error = "value length exceeds u32";
    }
    return false;
  }
  bool large_value = value.size() > 0xFFFF;
  out->push_back(large_value ? 0x20 : 0x03);
  out->push_back(static_cast<uint8_t>(key.size()));
  out->insert(out->end(), key.begin(), key.end());
  if (large_value) {
    EncodeU32BE(static_cast<uint32_t>(value.size()), out);
  } else {
    EncodeU16BE(static_cast<uint16_t>(value.size()), out);
  }
  out->insert(out->end(), value.begin(), value.end());
  return true;
}

void AppendProvableCountFeatureType(uint64_t count, std::vector<uint8_t>* out) {
  out->push_back(5);
  EncodeVarintU64(count, out);
}

void AppendFeatureTypeTagZero(std::vector<uint8_t>* out) {
  out->push_back(0);
}

}  // namespace

int MerkTree::ChildHeightFromMeta(const Node::ChildMeta& meta) {
  if (!meta.present) {
    return 0;
  }
  int left = meta.left_height;
  int right = meta.right_height;
  return 1 + (left > right ? left : right);
}

int MerkTree::ChildHeight(const Node* node, bool left) {
  if (!node) {
    return 0;
  }
  const auto& meta = left ? node->left_meta : node->right_meta;
  const auto* child = left ? node->left.get() : node->right.get();
  if (child) {
    return child->height;
  }
  if (meta.present) {
    return ChildHeightFromMeta(meta);
  }
  return 0;
}

bool MerkTree::AggregateCount(const AggregateData& aggregate, uint64_t* out, bool* has_count) {
  if (out == nullptr || has_count == nullptr) {
    return false;
  }
  switch (aggregate.tag) {
    case AggregateDataTag::kCount:
    case AggregateDataTag::kCountSum:
    case AggregateDataTag::kProvableCount:
    case AggregateDataTag::kProvableCountSum:
      *out = aggregate.count;
      *has_count = true;
      return true;
    case AggregateDataTag::kNone:
    case AggregateDataTag::kSum:
    case AggregateDataTag::kBigSum:
      *out = 0;
      *has_count = false;
      return true;
  }
  *out = 0;
  *has_count = false;
  return true;
}

bool MerkTree::AggregateSumI64(const AggregateData& aggregate,
                               int64_t* out,
                               bool* has_sum,
                               std::string* error) {
  if (out == nullptr || has_sum == nullptr) {
    if (error) {
      *error = "sum output is null";
    }
    return false;
  }
  switch (aggregate.tag) {
    case AggregateDataTag::kSum:
      *out = aggregate.sum;
      *has_sum = true;
      return true;
    case AggregateDataTag::kCountSum:
    case AggregateDataTag::kProvableCountSum:
      *out = aggregate.sum2;
      *has_sum = true;
      return true;
    case AggregateDataTag::kBigSum:
      if (error) {
        *error = "big sum aggregate under normal sum computation";
      }
      return false;
    case AggregateDataTag::kNone:
    case AggregateDataTag::kCount:
    case AggregateDataTag::kProvableCount:
      *out = 0;
      *has_sum = false;
      return true;
  }
  *out = 0;
  *has_sum = false;
  return true;
}

bool MerkTree::AggregateSumI128(const AggregateData& aggregate, __int128* out, bool* has_sum) {
  if (out == nullptr || has_sum == nullptr) {
    return false;
  }
  switch (aggregate.tag) {
    case AggregateDataTag::kBigSum:
      *out = aggregate.big_sum;
      *has_sum = true;
      return true;
    case AggregateDataTag::kSum:
      *out = static_cast<__int128>(aggregate.sum);
      *has_sum = true;
      return true;
    case AggregateDataTag::kCountSum:
    case AggregateDataTag::kProvableCountSum:
      *out = static_cast<__int128>(aggregate.sum2);
      *has_sum = true;
      return true;
    case AggregateDataTag::kNone:
    case AggregateDataTag::kCount:
    case AggregateDataTag::kProvableCount:
      *out = 0;
      *has_sum = false;
      return true;
  }
  *out = 0;
  *has_sum = false;
  return true;
}

bool MerkTree::Insert(const std::vector<uint8_t>& key,
                      const std::vector<uint8_t>& value,
                      std::string* error) {
  const bool ok = InsertNode(&root_, key, value, value_hash_fn_, &dirty_keys_, error);
  if (ok) {
    hash_caches_canonical_ = true;
  }
  return ok;
}

bool MerkTree::Insert(const std::vector<uint8_t>& key,
                      const std::vector<uint8_t>& value,
                      uint64_t* hash_calls,
                      std::string* error) {
  if (hash_calls == nullptr) {
    if (error) {
      *error = "hash call output is null";
    }
    return false;
  }
  OperationCost cost;
  if (!InsertNodeWithCost(&root_,
                          key,
                          value,
                          value_hash_fn_,
                          &value_defined_cost_fn_,
                          &dirty_keys_,
                          tree_feature_tag_,
                          &cost,
                          error)) {
    return false;
  }
  *hash_calls = cost.hash_node_calls;
  hash_caches_canonical_ = true;
  return true;
}

bool MerkTree::Insert(const std::vector<uint8_t>& key,
                      const std::vector<uint8_t>& value,
                      OperationCost* cost,
                      std::string* error) {
  if (cost == nullptr) {
    if (error) {
      *error = "cost output is null";
    }
    return false;
  }
  std::vector<uint8_t> old_value;
  const bool had_existing_value = Get(key, &old_value);
  if (!EnsureFullyLoaded(error)) {
    return false;
  }
  const bool ok = InsertNodeWithCost(&root_,
                                     key,
                                     value,
                                     value_hash_fn_,
                                     &value_defined_cost_fn_,
                                     &dirty_keys_,
                                     tree_feature_tag_,
                                     cost,
                                     error);
  if (ok && had_existing_value && cost->storage_cost.removed_bytes.TotalRemovedBytes() == 0) {
    std::optional<ValueDefinedCostType> old_defined_cost;
    std::optional<ValueDefinedCostType> new_defined_cost;
    if (value_defined_cost_fn_) {
      if (!value_defined_cost_fn_(old_value, &old_defined_cost, error)) {
        return false;
      }
      if (!value_defined_cost_fn_(value, &new_defined_cost, error)) {
        return false;
      }
    }
    const uint32_t key_len = static_cast<uint32_t>(key.size());
    auto paid_value_len = [&](const std::vector<uint8_t>& bytes,
                              const std::optional<ValueDefinedCostType>& defined_cost) -> uint32_t {
      if (defined_cost.has_value()) {
        if (defined_cost->kind == ValueDefinedCostType::Kind::kLayered) {
          return LayeredValueByteCostSizeForKeyAndValueLengths(
              key_len, defined_cost->cost, TreeFeatureTypeTag::kBasic);
        }
        return NodeValueByteCostSize(key_len, defined_cost->cost, TreeFeatureTypeTag::kBasic);
      }
      return NodeValueByteCostSize(key_len, static_cast<uint32_t>(bytes.size()),
                                   TreeFeatureTypeTag::kBasic);
    };
    const uint32_t old_paid_value_len = paid_value_len(old_value, old_defined_cost);
    const uint32_t new_paid_value_len = paid_value_len(value, new_defined_cost);
    if (new_paid_value_len < old_paid_value_len) {
      cost->storage_cost.removed_bytes.Add(
          StorageRemovedBytes::Basic(old_paid_value_len - new_paid_value_len));
    }
  }
  if (ok) {
    hash_caches_canonical_ = true;
  }
  return ok;
}

bool MerkTree::Delete(const std::vector<uint8_t>& key, bool* deleted, std::string* error) {
  if (deleted == nullptr) {
    if (error) {
      *error = "deleted output is null";
    }
    return false;
  }
  *deleted = false;
  const bool ok = DeleteNode(&root_,
                              key,
                              value_hash_fn_,
                              &dirty_keys_,
                              &deleted_keys_,
                              deleted,
                              error);
  if (ok) {
    hash_caches_canonical_ = true;
  }
  return ok;
}

bool MerkTree::Delete(const std::vector<uint8_t>& key,
                      bool* deleted,
                      uint64_t* hash_calls,
                      std::string* error) {
  if (hash_calls == nullptr) {
    if (error) {
      *error = "hash call output is null";
    }
    return false;
  }
  OperationCost cost;
  if (!DeleteNodeWithCost(&root_,
                          key,
                          value_hash_fn_,
                          &value_defined_cost_fn_,
                          &dirty_keys_,
                          &deleted_keys_,
                          deleted,
                          tree_feature_tag_,
                          &cost,
                          error)) {
    return false;
  }
  *hash_calls = cost.hash_node_calls;
  hash_caches_canonical_ = true;
  return true;
}

bool MerkTree::Delete(const std::vector<uint8_t>& key,
                      bool* deleted,
                      OperationCost* cost,
                      std::string* error) {
  if (cost == nullptr) {
    if (error) {
      *error = "cost output is null";
    }
    return false;
  }
  if (!EnsureFullyLoaded(error)) {
    return false;
  }
  const bool ok = DeleteNodeWithCost(&root_,
                                     key,
                                     value_hash_fn_,
                                     &value_defined_cost_fn_,
                                     &dirty_keys_,
                                     &deleted_keys_,
                                     deleted,
                                     tree_feature_tag_,
                                     cost,
                                     error);
  if (ok) {
    hash_caches_canonical_ = true;
  }
  return ok;
}

void MerkTree::SetValueHashFn(ValueHashFn value_hash_fn) {
  value_hash_fn_ = std::move(value_hash_fn);
  hash_caches_canonical_ = false;
  if (hash_policy_generation_ == std::numeric_limits<uint64_t>::max()) {
    hash_policy_generation_ = 1;
    ClearCachedHashes(root_.get());
  } else {
    ++hash_policy_generation_;
  }
}

void MerkTree::SetValueDefinedCostFn(ValueDefinedCostFn value_defined_cost_fn) {
  value_defined_cost_fn_ = std::move(value_defined_cost_fn);
}

void MerkTree::SetTreeFeatureTag(TreeFeatureTypeTag tag) {
  tree_feature_tag_ = tag;
}

bool MerkTree::UsesProvableCountHashing(TreeFeatureTypeTag tag) {
  return tag == TreeFeatureTypeTag::kProvableCount ||
         tag == TreeFeatureTypeTag::kProvableCountSum;
}

bool MerkTree::ComputeTreeTypedNodeHash(const Node* node,
                                        const ValueHashFn& value_hash_fn,
                                        std::vector<uint8_t>* out,
                                        std::string* error) const {
  if (node != nullptr && UsesProvableCountHashing(tree_feature_tag_)) {
    uint64_t count = 0;
    return ComputeNodeHashWithCount(node, value_hash_fn, out, &count, error);
  }
  return ComputeNodeHash(node, value_hash_fn, out, error);
}

std::string MerkTree::KeyToString(const std::vector<uint8_t>& key) {
  return std::string(reinterpret_cast<const char*>(key.data()), key.size());
}

std::vector<uint8_t> MerkTree::StringToKey(const std::string& key) {
  return std::vector<uint8_t>(key.begin(), key.end());
}

void MerkTree::MarkDirtyKey(std::unordered_set<std::string>* dirty,
                            const std::vector<uint8_t>& key) {
  if (dirty == nullptr) {
    return;
  }
  dirty->insert(KeyToString(key));
}

void MerkTree::MarkDeletedKey(std::unordered_set<std::string>* deleted,
                              const std::vector<uint8_t>& key) {
  if (deleted == nullptr) {
    return;
  }
  deleted->insert(KeyToString(key));
}

void RemoveTrackedDirtyKey(std::unordered_set<std::string>* dirty,
                           const std::vector<uint8_t>& key) {
  if (dirty == nullptr) {
    return;
  }
  dirty->erase(std::string(reinterpret_cast<const char*>(key.data()), key.size()));
}

bool MerkTree::AggregateValuesFromMeta(const AggregateData& aggregate,
                                       AggregateValues* out,
                                       std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "aggregate values output is null";
    }
    return false;
  }
  out->count = 0;
  out->sum = 0;
  out->big_sum = 0;
  switch (aggregate.tag) {
    case AggregateDataTag::kNone:
      return true;
    case AggregateDataTag::kSum:
      out->sum = aggregate.sum;
      out->big_sum = static_cast<__int128>(aggregate.sum);
      return true;
    case AggregateDataTag::kBigSum:
      out->big_sum = aggregate.big_sum;
      return true;
    case AggregateDataTag::kCount:
      out->count = aggregate.count;
      return true;
    case AggregateDataTag::kCountSum:
      out->count = aggregate.count;
      out->sum = aggregate.sum;
      out->big_sum = static_cast<__int128>(aggregate.sum);
      return true;
    case AggregateDataTag::kProvableCount:
      out->count = aggregate.count;
      return true;
    case AggregateDataTag::kProvableCountSum:
      out->count = aggregate.count;
      out->sum = aggregate.sum;
      out->big_sum = static_cast<__int128>(aggregate.sum);
      return true;
  }
  if (error) {
    *error = "unsupported aggregate data";
  }
  return false;
}

AggregateData MerkTree::AggregateDataFromValues(TreeFeatureTypeTag tag,
                                                const AggregateValues& values) {
  AggregateData aggregate;
  switch (tag) {
    case TreeFeatureTypeTag::kBasic:
      aggregate.tag = AggregateDataTag::kNone;
      break;
    case TreeFeatureTypeTag::kSum:
      aggregate.tag = AggregateDataTag::kSum;
      aggregate.sum = values.sum;
      break;
    case TreeFeatureTypeTag::kBigSum:
      aggregate.tag = AggregateDataTag::kBigSum;
      aggregate.big_sum = values.big_sum;
      break;
    case TreeFeatureTypeTag::kCount:
      aggregate.tag = AggregateDataTag::kCount;
      aggregate.count = values.count;
      break;
    case TreeFeatureTypeTag::kCountSum:
      aggregate.tag = AggregateDataTag::kCountSum;
      aggregate.count = values.count;
      aggregate.sum = values.sum;
      aggregate.sum2 = values.sum;
      break;
    case TreeFeatureTypeTag::kProvableCount:
      aggregate.tag = AggregateDataTag::kProvableCount;
      aggregate.count = values.count;
      break;
    case TreeFeatureTypeTag::kProvableCountSum:
      aggregate.tag = AggregateDataTag::kProvableCountSum;
      aggregate.count = values.count;
      aggregate.sum = values.sum;
      aggregate.sum2 = values.sum;
      break;
  }
  return aggregate;
}

TreeFeatureType MerkTree::FeatureTypeFromValues(TreeFeatureTypeTag tag,
                                                const AggregateValues& values) {
  TreeFeatureType feature;
  feature.tag = tag;
  switch (tag) {
    case TreeFeatureTypeTag::kBasic:
      break;
    case TreeFeatureTypeTag::kSum:
      feature.sum = values.sum;
      break;
    case TreeFeatureTypeTag::kBigSum:
      feature.big_sum = values.big_sum;
      break;
    case TreeFeatureTypeTag::kCount:
      feature.count = values.count;
      break;
    case TreeFeatureTypeTag::kCountSum:
      feature.count = values.count;
      feature.sum = values.sum;
      feature.sum2 = values.sum;
      break;
    case TreeFeatureTypeTag::kProvableCount:
      feature.count = values.count;
      break;
    case TreeFeatureTypeTag::kProvableCountSum:
      feature.count = values.count;
      feature.sum = values.sum;
      feature.sum2 = values.sum;
      break;
  }
  return feature;
}

bool MerkTree::ComputeAggregateValuesFromChildren(const Node* node,
                                                  TreeFeatureTypeTag tree_feature_tag,
                                                  const AggregateValues& left,
                                                  const AggregateValues& right,
                                                  AggregateValues* out,
                                                  std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "aggregate output is null";
    }
    return false;
  }
  out->count = 0;
  out->sum = 0;
  out->big_sum = 0;
  if (node == nullptr || tree_feature_tag == TreeFeatureTypeTag::kBasic) {
    return true;
  }
  int64_t node_sum = 0;
  bool has_sum = false;
  __int128 node_big_sum = 0;
  bool has_big_sum = false;
  if (tree_feature_tag == TreeFeatureTypeTag::kBigSum) {
    if (!ExtractBigSumValueFromElementBytes(node->value, &node_big_sum, &has_big_sum, error)) {
      return false;
    }
    if (!has_big_sum) {
      node_big_sum = 0;
    }
  } else if (tree_feature_tag == TreeFeatureTypeTag::kSum ||
             tree_feature_tag == TreeFeatureTypeTag::kCountSum ||
             tree_feature_tag == TreeFeatureTypeTag::kProvableCountSum) {
    if (!ExtractSumValueFromElementBytes(node->value, &node_sum, &has_sum, error)) {
      return false;
    }
    if (!has_sum) {
      node_sum = 0;
    }
    node_big_sum = static_cast<__int128>(node_sum);
  }
  out->count = 1 + left.count + right.count;
  out->sum = node_sum + left.sum + right.sum;
  out->big_sum = node_big_sum + left.big_sum + right.big_sum;
  return true;
}

bool MerkTree::ComputeNodeFeatureValues(const Node* node,
                                        TreeFeatureTypeTag tree_feature_tag,
                                        AggregateValues* out,
                                        std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "aggregate output is null";
    }
    return false;
  }
  out->count = 0;
  out->sum = 0;
  out->big_sum = 0;
  if (node == nullptr || tree_feature_tag == TreeFeatureTypeTag::kBasic) {
    return true;
  }

  if (tree_feature_tag == TreeFeatureTypeTag::kCount ||
      tree_feature_tag == TreeFeatureTypeTag::kCountSum ||
      tree_feature_tag == TreeFeatureTypeTag::kProvableCount ||
      tree_feature_tag == TreeFeatureTypeTag::kProvableCountSum) {
    out->count = 1;
  }

  if (tree_feature_tag == TreeFeatureTypeTag::kBigSum) {
    __int128 node_big_sum = 0;
    bool has_big_sum = false;
    if (!ExtractBigSumValueFromElementBytes(node->value, &node_big_sum, &has_big_sum, error)) {
      return false;
    }
    if (has_big_sum) {
      out->big_sum = node_big_sum;
      out->sum = static_cast<int64_t>(node_big_sum);
    }
  } else if (tree_feature_tag == TreeFeatureTypeTag::kSum ||
             tree_feature_tag == TreeFeatureTypeTag::kCountSum ||
             tree_feature_tag == TreeFeatureTypeTag::kProvableCountSum) {
    int64_t node_sum = 0;
    bool has_sum = false;
    if (!ExtractSumValueFromElementBytes(node->value, &node_sum, &has_sum, error)) {
      return false;
    }
    if (has_sum) {
      out->sum = node_sum;
      out->big_sum = static_cast<__int128>(node_sum);
    }
  }
  return true;
}

bool MerkTree::ComputeAggregateValues(const Node* node,
                                      TreeFeatureTypeTag tree_feature_tag,
                                      AggregateValues* out,
                                      std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "aggregate output is null";
    }
    return false;
  }
  out->count = 0;
  out->sum = 0;
  out->big_sum = 0;
  if (node == nullptr || tree_feature_tag == TreeFeatureTypeTag::kBasic) {
    return true;
  }
  AggregateValues left;
  AggregateValues right;
  if (node->left) {
    if (!ComputeAggregateValues(node->left.get(), tree_feature_tag, &left, error)) {
      return false;
    }
  } else if (node->left_meta.present) {
    if (!AggregateValuesFromMeta(node->left_meta.aggregate, &left, error)) {
      return false;
    }
  }
  if (node->right) {
    if (!ComputeAggregateValues(node->right.get(), tree_feature_tag, &right, error)) {
      return false;
    }
  } else if (node->right_meta.present) {
    if (!AggregateValuesFromMeta(node->right_meta.aggregate, &right, error)) {
      return false;
    }
  }
  return ComputeAggregateValuesFromChildren(node,
                                            tree_feature_tag,
                                            left,
                                            right,
                                            out,
                                            error);
}

void MerkTree::ConsumeDirtyKeys(std::vector<std::vector<uint8_t>>* out) {
  SnapshotDirtyKeys(out);
  dirty_keys_.clear();
}

void MerkTree::SnapshotDirtyKeys(std::vector<std::vector<uint8_t>>* out) const {
  if (out == nullptr) {
    return;
  }
  out->clear();
  out->reserve(dirty_keys_.size());
  for (const auto& entry : dirty_keys_) {
    out->push_back(StringToKey(entry));
  }
}

void MerkTree::ConsumeDeletedKeys(std::vector<std::vector<uint8_t>>* out) {
  SnapshotDeletedKeys(out);
  deleted_keys_.clear();
}

void MerkTree::SnapshotDeletedKeys(std::vector<std::vector<uint8_t>>* out) const {
  if (out == nullptr) {
    return;
  }
  out->clear();
  out->reserve(deleted_keys_.size());
  for (const auto& entry : deleted_keys_) {
    out->push_back(StringToKey(entry));
  }
}

void MerkTree::AcknowledgeDirtyKeys(const std::vector<std::vector<uint8_t>>& keys) {
  for (const auto& key : keys) {
    dirty_keys_.erase(KeyToString(key));
  }
}

void MerkTree::AcknowledgeDeletedKeys(const std::vector<std::vector<uint8_t>>& keys) {
  for (const auto& key : keys) {
    deleted_keys_.erase(KeyToString(key));
  }
}

bool MerkTree::Get(const std::vector<uint8_t>& key, std::vector<uint8_t>* value) const {
  std::vector<uint8_t> dummy_hash;
  return GetValueAndValueHash(key, value, &dummy_hash);
}

bool MerkTree::GetValueAndValueHash(const std::vector<uint8_t>& key,
                                    std::vector<uint8_t>* value,
                                    std::vector<uint8_t>* value_hash) const {
  if (!lazy_loading_) {
    Node* node = FindNode(root_.get(), key);
    if (!node) {
      return false;
    }
    if (value) {
      *value = node->value;
    }
    if (value_hash) {
      *value_hash = node->value_hash;
    }
    return true;
  }
  Node* current = root_.get();
  while (current) {
    if (key == current->key) {
      if (value) {
        *value = current->value;
      }
      if (value_hash) {
        *value_hash = current->value_hash;
      }
      return true;
    }
    bool go_left = CompareKeys(key, current->key);
    std::string error;
    if (!EnsureChildLoaded(current, go_left, &error)) {
      return false;
    }
    current = go_left ? current->left.get() : current->right.get();
  }
  return false;
}

bool MerkTree::RecomputeHashesForKey(const std::vector<uint8_t>& key, std::string* error) {
  return RecomputeHashesForKey(root_.get(), key, value_hash_fn_, &dirty_keys_, error);
}

bool MerkTree::RootKey(std::vector<uint8_t>* out) const {
  if (out == nullptr) {
    return false;
  }
  if (!root_) {
    out->clear();
    return false;
  }
  *out = root_->key;
  return true;
}

bool MerkTree::InitialRootKey(std::vector<uint8_t>* out) const {
  if (out == nullptr) {
    return false;
  }
  if (!has_initial_root_key_) {
    out->clear();
    return false;
  }
  *out = initial_root_key_;
  return true;
}

bool MerkTree::InitialRootKeyEqualsCurrent() const {
  if (!has_initial_root_key_ || !root_) {
    return false;
  }
  return root_->key == initial_root_key_;
}

void MerkTree::MarkPersistedRootKey(const std::vector<uint8_t>& root_key) {
  initial_root_key_ = root_key;
  has_initial_root_key_ = true;
}

void MerkTree::AttachStorage(RocksDbWrapper* storage,
                             const std::vector<std::vector<uint8_t>>& path,
                             ColumnFamilyKind cf) {
  storage_ = storage;
  storage_tx_ = nullptr;
  storage_path_ = path;
  storage_cf_ = cf;
  lazy_loading_ = (storage != nullptr);
}

void MerkTree::AttachStorage(RocksDbWrapper::Transaction* transaction,
                             const std::vector<std::vector<uint8_t>>& path,
                             ColumnFamilyKind cf) {
  AttachStorage(nullptr, transaction, path, cf);
}

void MerkTree::AttachStorage(RocksDbWrapper* storage,
                             RocksDbWrapper::Transaction* transaction,
                             const std::vector<std::vector<uint8_t>>& path,
                             ColumnFamilyKind cf) {
  storage_ = storage;
  storage_tx_ = transaction;
  storage_path_ = path;
  storage_cf_ = cf;
  lazy_loading_ = (storage != nullptr || transaction != nullptr);
}

namespace {

bool IsTransactionUnavailableError(const std::string& error) {
  return error == "transaction not initialized";
}

}  // namespace

bool MerkTree::MinKey(std::vector<uint8_t>* out) const {
  if (out == nullptr) {
    return false;
  }
  if (!root_) {
    out->clear();
    return false;
  }
  Node* node = root_.get();
  while (true) {
    std::string error;
    if (!EnsureChildLoaded(node, true, &error)) {
      return false;
    }
    if (!node->left) {
      break;
    }
    node = node->left.get();
  }
  *out = node->key;
  return true;
}

bool MerkTree::MaxKey(std::vector<uint8_t>* out) const {
  if (out == nullptr) {
    return false;
  }
  if (!root_) {
    out->clear();
    return false;
  }
  Node* node = root_.get();
  while (true) {
    std::string error;
    if (!EnsureChildLoaded(node, false, &error)) {
      return false;
    }
    if (!node->right) {
      break;
    }
    node = node->right.get();
  }
  *out = node->key;
  return true;
}

bool MerkTree::EnumerateKvPairsForTesting(
    const std::function<bool(const std::vector<uint8_t>&, const std::vector<uint8_t>&)>& callback,
    std::string* error) const {
  if (!callback) {
    if (error) {
      *error = "callback is null";
    }
    return false;
  }
  if (!root_) {
    return true;  // empty tree is valid
  }
  std::function<void(Node*)> traverse = [&](Node* node) {
    if (node == nullptr) return;
    traverse(node->left.get());
    callback(node->key, node->value);
    traverse(node->right.get());
  };
  traverse(root_.get());
  return true;
}

bool MerkTree::ComputeRootHash(const ValueHashFn& value_hash_fn,
                               std::vector<uint8_t>* out,
                               std::string* error) const {
  if (out == nullptr) {
    if (error) {
      *error = "root hash output is null";
    }
    return false;
  }
  return ComputeTreeTypedNodeHash(root_.get(), value_hash_fn, out, error);
}

bool MerkTree::ComputeRootHashWithCount(const ValueHashFn& value_hash_fn,
                                        std::vector<uint8_t>* out,
                                        uint64_t* out_count,
                                        std::string* error) const {
  if (out == nullptr || out_count == nullptr) {
    if (error) {
      *error = "root hash/count output is null";
    }
    return false;
  }
  return ComputeNodeHashWithCount(root_.get(), value_hash_fn, out, out_count, error);
}

bool MerkTree::GetCachedRootHash(std::vector<uint8_t>* out, std::string* error) const {
  if (out == nullptr) {
    if (error) {
      *error = "root hash output is null";
    }
    return false;
  }
  out->clear();
  if (!root_) {
    out->assign(32, 0);
    return true;
  }
  if (root_->node_hash.empty()) {
    if (error) {
      *error = "root hash cache missing";
    }
    return false;
  }
  *out = root_->node_hash;
  return true;
}

bool MerkTree::RootAggregateData(RootAggregate* out, std::string* error) const {
  if (out == nullptr) {
    if (error) {
      *error = "root aggregate output is null";
    }
    return false;
  }
  *out = RootAggregate{};
  if (!root_) {
    return true;
  }

  // Fast path is O(1) when child aggregate metadata is present on the traversed
  // nodes. It degrades to a recursive walk over already-loaded children when
  // metadata is missing, but it still avoids forcing a full disk-backed load.
  auto compute_values_from_meta_or_loaded_children = [&](auto&& self,
                                                         const Node* node,
                                                         AggregateValues* values_out,
                                                         std::string* err) -> bool {
    if (values_out == nullptr) {
      if (err) {
        *err = "aggregate output is null";
      }
      return false;
    }
    values_out->count = 0;
    values_out->sum = 0;
    values_out->big_sum = 0;
    if (node == nullptr || tree_feature_tag_ == TreeFeatureTypeTag::kBasic) {
      return true;
    }

    AggregateValues left_values;
    if (node->left_meta.present) {
      if (!AggregateValuesFromMeta(node->left_meta.aggregate, &left_values, err)) {
        return false;
      }
    } else if (node->left) {
      if (!self(self, node->left.get(), &left_values, err)) {
        return false;
      }
    }

    AggregateValues right_values;
    if (node->right_meta.present) {
      if (!AggregateValuesFromMeta(node->right_meta.aggregate, &right_values, err)) {
        return false;
      }
    } else if (node->right) {
      if (!self(self, node->right.get(), &right_values, err)) {
        return false;
      }
    }

    return ComputeAggregateValuesFromChildren(node,
                                             tree_feature_tag_,
                                             left_values,
                                             right_values,
                                             values_out,
                                             err);
  };

  AggregateValues values;
  if (!compute_values_from_meta_or_loaded_children(
          compute_values_from_meta_or_loaded_children, root_.get(), &values, error)) {
    return false;
  }

  switch (tree_feature_tag_) {
    case TreeFeatureTypeTag::kBasic:
      return true;
    case TreeFeatureTypeTag::kSum:
    case TreeFeatureTypeTag::kBigSum:
      out->sum = values.big_sum;
      out->has_sum = true;
      return true;
    case TreeFeatureTypeTag::kCount:
    case TreeFeatureTypeTag::kProvableCount:
      out->count = values.count;
      out->has_count = true;
      return true;
    case TreeFeatureTypeTag::kCountSum:
    case TreeFeatureTypeTag::kProvableCountSum:
      out->count = values.count;
      out->sum = values.big_sum;
      out->has_count = true;
      out->has_sum = true;
      return true;
  }

  if (error) {
    *error = "unknown tree feature tag";
  }
  return false;
}

void MerkTree::ClearCachedHashes() {
  ClearCachedHashes(root_.get());
}

bool MerkTree::EstimateHashCallsForKey(const std::vector<uint8_t>& key,
                                       uint64_t* out_count,
                                       std::string* error) const {
  if (out_count == nullptr) {
    if (error) {
      *error = "hash count output is null";
    }
    return false;
  }
  if (!EnsureFullyLoaded(error)) {
    return false;
  }
  uint64_t count = 0;
  const Node* current = root_.get();
  while (current) {
    count += 1;
    if (key == current->key) {
      break;
    }
    if (CompareKeys(key, current->key)) {
      current = current->left.get();
    } else {
      current = current->right.get();
    }
  }
  *out_count = count;
  return true;
}

bool MerkTree::ComputeCount(uint64_t* out_count, std::string* error) const {
  if (out_count == nullptr) {
    if (error) {
      *error = "count output is null";
    }
    return false;
  }
  if (!EnsureFullyLoaded(error)) {
    return false;
  }
  return ComputeNodeCount(root_.get(), out_count, error);
}

bool MerkTree::ComputeSum(const SumValueFn& sum_fn,
                          int64_t* out_sum,
                          std::string* error) const {
  if (out_sum == nullptr) {
    if (error) {
      *error = "sum output is null";
    }
    return false;
  }
  if (!EnsureFullyLoaded(error)) {
    return false;
  }
  return ComputeNodeSum(root_.get(), sum_fn, out_sum, error);
}

bool MerkTree::ComputeSumBig(const SumValueFn& sum_fn,
                             __int128* out_sum,
                             std::string* error) const {
  if (out_sum == nullptr) {
    if (error) {
      *error = "sum output is null";
    }
    return false;
  }
  if (!EnsureFullyLoaded(error)) {
    return false;
  }
  return ComputeNodeSumBig(root_.get(), sum_fn, out_sum, error);
}

bool MerkTree::ComputeCountAndSum(const SumValueFn& sum_fn,
                                  uint64_t* out_count,
                                  int64_t* out_sum,
                                  std::string* error) const {
  if (out_count == nullptr || out_sum == nullptr) {
    if (error) {
      *error = "count/sum output is null";
    }
    return false;
  }
  if (!EnsureFullyLoaded(error)) {
    return false;
  }
  return ComputeNodeCountAndSum(root_.get(), sum_fn, out_count, out_sum, error);
}

bool MerkTree::ComputeCountAndSumBig(const SumValueFn& sum_fn,
                                     uint64_t* out_count,
                                     __int128* out_sum,
                                     std::string* error) const {
  if (out_count == nullptr || out_sum == nullptr) {
    if (error) {
      *error = "count/sum output is null";
    }
    return false;
  }
  if (!EnsureFullyLoaded(error)) {
    return false;
  }
  return ComputeNodeCountAndSumBig(root_.get(), sum_fn, out_count, out_sum, error);
}

bool MerkTree::GenerateProof(const std::vector<uint8_t>& key,
                             TargetEncoding target_encoding,
                             const ValueHashFn& value_hash_fn,
                             std::vector<uint8_t>* out_proof,
                             std::vector<uint8_t>* out_root_hash,
                             std::vector<uint8_t>* out_value,
                             std::string* error) const {
  if (out_proof == nullptr || out_root_hash == nullptr || out_value == nullptr) {
    if (error) {
      *error = "proof outputs are null";
    }
    return false;
  }
  if (!Get(key, out_value)) {
    if (error) {
      *error = "key not found";
    }
    return false;
  }
  std::vector<ProofOp> ops;
  if (!EmitProofOps(root_.get(),
                    key,
                    target_encoding,
                    ProofMode::kTargetValue,
                    value_hash_fn,
                    &ops,
                    error)) {
    return false;
  }
  out_proof->clear();
  if (!EncodeProofOps(ops, out_proof, error)) {
    return false;
  }
  if (hash_caches_canonical_ && !value_hash_fn) {
    bool ok = GetCachedRootHash(out_root_hash, error);
    if (ok) {
      return true;
    }
  }
  return ComputeNodeHash(root_.get(), value_hash_fn, out_root_hash, error);
}

bool MerkTree::GenerateProofWithCount(const std::vector<uint8_t>& key,
                                      TargetEncoding target_encoding,
                                      const ValueHashFn& value_hash_fn,
                                      std::vector<uint8_t>* out_proof,
                                      std::vector<uint8_t>* out_root_hash,
                                      std::vector<uint8_t>* out_value,
                                      std::string* error) const {
  if (target_encoding != TargetEncoding::kKv) {
    if (error) {
      *error = "provable count proof requires kv target encoding";
    }
    return false;
  }
  if (out_proof == nullptr || out_root_hash == nullptr || out_value == nullptr) {
    if (error) {
      *error = "proof outputs are null";
    }
    return false;
  }
  if (!Get(key, out_value)) {
    if (error) {
      *error = "key not found";
    }
    return false;
  }
  std::vector<ProofOp> ops;
  if (!EmitProofOpsForPresentWithCount(root_.get(),
                                       key,
                                       target_encoding,
                                       value_hash_fn,
                                       &ops,
                                       error)) {
    return false;
  }
  out_proof->clear();
  if (!EncodeProofOps(ops, out_proof, error)) {
    return false;
  }
  uint64_t count = 0;
  return ComputeNodeHashWithCount(root_.get(), value_hash_fn, out_root_hash, &count, error);
}

bool MerkTree::GenerateAbsenceProof(const std::vector<uint8_t>& key,
                                    const ValueHashFn& value_hash_fn,
                                    std::vector<uint8_t>* out_proof,
                                    std::vector<uint8_t>* out_root_hash,
                                    std::string* error) const {
  if (out_proof == nullptr || out_root_hash == nullptr) {
    if (error) {
      *error = "proof outputs are null";
    }
    return false;
  }
  if (Get(key, nullptr)) {
    if (error) {
      *error = "key exists";
    }
    return false;
  }
  if (!root_) {
    out_proof->clear();
    out_proof->push_back(0x01);
    out_proof->insert(out_proof->end(), 32, 0);
    out_root_hash->assign(32, 0);
    return true;
  }
  const Node* predecessor = nullptr;
  const Node* successor = nullptr;
  Node* current = root_.get();
  while (current) {
    if (CompareKeys(key, current->key)) {
      successor = current;
      if (!EnsureChildLoaded(current, true, error)) {
        return false;
      }
      current = current->left.get();
    } else if (CompareKeys(current->key, key)) {
      predecessor = current;
      if (!EnsureChildLoaded(current, false, error)) {
        return false;
      }
      current = current->right.get();
    } else {
      if (error) {
        *error = "key exists";
      }
      return false;
    }
  }

  std::vector<ProofOp> ops;
  if (!EmitProofOpsForAbsent(root_.get(),
                             key,
                             predecessor,
                             successor,
                             value_hash_fn,
                             &ops,
                             error)) {
    return false;
  }
  out_proof->clear();
  if (!EncodeProofOps(ops, out_proof, error)) {
    return false;
  }
  if (hash_caches_canonical_ && !value_hash_fn) {
    bool ok = GetCachedRootHash(out_root_hash, error);
    if (ok) {
      return true;
    }
  }
  return ComputeNodeHash(root_.get(), value_hash_fn, out_root_hash, error);
}

bool MerkTree::GenerateAbsenceProofWithCount(const std::vector<uint8_t>& key,
                                             const ValueHashFn& value_hash_fn,
                                             std::vector<uint8_t>* out_proof,
                                             std::vector<uint8_t>* out_root_hash,
                                             std::string* error) const {
  if (out_proof == nullptr || out_root_hash == nullptr) {
    if (error) {
      *error = "proof outputs are null";
    }
    return false;
  }
  if (Get(key, nullptr)) {
    if (error) {
      *error = "key exists";
    }
    return false;
  }
  if (!root_) {
    out_proof->clear();
    out_proof->push_back(0x01);
    out_proof->insert(out_proof->end(), 32, 0);
    out_root_hash->assign(32, 0);
    return true;
  }
  const Node* predecessor = nullptr;
  const Node* successor = nullptr;
  Node* current = root_.get();
  while (current) {
    if (CompareKeys(key, current->key)) {
      successor = current;
      if (!EnsureChildLoaded(current, true, error)) {
        return false;
      }
      current = current->left.get();
    } else if (CompareKeys(current->key, key)) {
      predecessor = current;
      if (!EnsureChildLoaded(current, false, error)) {
        return false;
      }
      current = current->right.get();
    } else {
      if (error) {
        *error = "key exists";
      }
      return false;
    }
  }

  std::vector<ProofOp> ops;
  if (!EmitProofOpsForAbsentWithCount(root_.get(),
                                      key,
                                      predecessor,
                                      successor,
                                      value_hash_fn,
                                      &ops,
                                      error)) {
    return false;
  }
  out_proof->clear();
  if (!EncodeProofOps(ops, out_proof, error)) {
    return false;
  }
  uint64_t count = 0;
  return ComputeNodeHashWithCount(root_.get(), value_hash_fn, out_root_hash, &count, error);
}

bool MerkTree::GenerateRangeProof(const std::vector<uint8_t>& start_key,
                                  const std::vector<uint8_t>& end_key,
                                  bool start_inclusive,
                                  bool end_inclusive,
                                  const ValueHashFn& value_hash_fn,
                                  std::vector<uint8_t>* out_proof,
                                  std::vector<uint8_t>* out_root_hash,
                                  std::string* error) const {
  return GenerateRangeProofWithTargetEncoding(start_key,
                                              end_key,
                                              start_inclusive,
                                              end_inclusive,
                                              TargetEncoding::kKv,
                                              value_hash_fn,
                                              out_proof,
                                              out_root_hash,
                                              error);
}

bool MerkTree::GenerateRangeProofWithTargetEncoding(const std::vector<uint8_t>& start_key,
                                                    const std::vector<uint8_t>& end_key,
                                                    bool start_inclusive,
                                                    bool end_inclusive,
                                                    TargetEncoding target_encoding,
                                                    const ValueHashFn& value_hash_fn,
                                                    std::vector<uint8_t>* out_proof,
                                                    std::vector<uint8_t>* out_root_hash,
                                                    std::string* error) const {
  if (out_proof == nullptr || out_root_hash == nullptr) {
    if (error) {
      *error = "proof outputs are null";
    }
    return false;
  }
  if (CompareKeyOrder(end_key, start_key) < 0) {
    if (error) {
      *error = "range end precedes start";
    }
    return false;
  }
  if (root_ == nullptr) {
    return GenerateAbsenceProof(start_key, value_hash_fn, out_proof, out_root_hash, error);
  }
  std::vector<ProofOp> ops;
  if (!EmitProofOpsForRange(root_.get(),
                            start_key,
                            end_key,
                            start_inclusive,
                            end_inclusive,
                            target_encoding,
                            value_hash_fn,
                            &ops,
                            error)) {
    return false;
  }
  out_proof->clear();
  if (!EncodeProofOps(ops, out_proof, error)) {
    return false;
  }
  if (hash_caches_canonical_ && !value_hash_fn) {
    bool ok = GetCachedRootHash(out_root_hash, error);
    if (ok) {
      return true;
    }
  }
  return ComputeNodeHash(root_.get(), value_hash_fn, out_root_hash, error);
}

bool MerkTree::GenerateRangeProofWithLimit(const std::vector<uint8_t>& start_key,
                                           const std::vector<uint8_t>& end_key,
                                           bool start_inclusive,
                                           bool end_inclusive,
                                           size_t limit,
                                           const ValueHashFn& value_hash_fn,
                                           std::vector<uint8_t>* out_proof,
                                           std::vector<uint8_t>* out_root_hash,
                                           std::string* error) const {
  if (out_proof == nullptr || out_root_hash == nullptr) {
    if (error) {
      *error = "proof outputs are null";
    }
    return false;
  }
  if (limit == 0) {
    if (error) {
      *error = "range proof limit must be greater than zero";
    }
    return false;
  }
  if (CompareKeyOrder(end_key, start_key) < 0) {
    if (error) {
      *error = "range end precedes start";
    }
    return false;
  }
  if (root_ == nullptr) {
    return GenerateAbsenceProof(start_key, value_hash_fn, out_proof, out_root_hash, error);
  }
  std::vector<ProofOp> ops;
  size_t remaining_limit = limit;
  if (!EmitProofOpsForRangeWithLimit(root_.get(),
                                     start_key,
                                     end_key,
                                     start_inclusive,
                                     end_inclusive,
                                     &remaining_limit,
                                     value_hash_fn,
                                     &ops,
                                     error)) {
    return false;
  }
  out_proof->clear();
  if (!EncodeProofOps(ops, out_proof, error)) {
    return false;
  }
  if (hash_caches_canonical_ && !value_hash_fn) {
    bool ok = GetCachedRootHash(out_root_hash, error);
    if (ok) {
      return true;
    }
  }
  return ComputeNodeHash(root_.get(), value_hash_fn, out_root_hash, error);
}

bool MerkTree::GenerateRangeProofWithCount(const std::vector<uint8_t>& start_key,
                                           const std::vector<uint8_t>& end_key,
                                           bool start_inclusive,
                                           bool end_inclusive,
                                           const ValueHashFn& value_hash_fn,
                                           std::vector<uint8_t>* out_proof,
                                           std::vector<uint8_t>* out_root_hash,
                                           std::string* error) const {
  if (out_proof == nullptr || out_root_hash == nullptr) {
    if (error) {
      *error = "proof outputs are null";
    }
    return false;
  }
  if (CompareKeyOrder(end_key, start_key) < 0) {
    if (error) {
      *error = "range end precedes start";
    }
    return false;
  }
  if (!HasKeyInRange(root_.get(), start_key, end_key, start_inclusive, end_inclusive, error)) {
    if (error && !error->empty()) {
      return false;
    }
    return GenerateAbsenceProofWithCount(start_key,
                                         value_hash_fn,
                                         out_proof,
                                         out_root_hash,
                                         error);
  }
  std::vector<ProofOp> ops;
  if (!EmitProofOpsForRangeWithCount(root_.get(),
                                     start_key,
                                     end_key,
                                     start_inclusive,
                                     end_inclusive,
                                     value_hash_fn,
                                     &ops,
                                     error)) {
    return false;
  }
  out_proof->clear();
  if (!EncodeProofOps(ops, out_proof, error)) {
    return false;
  }
  uint64_t count = 0;
  return ComputeNodeHashWithCount(root_.get(), value_hash_fn, out_root_hash, &count, error);
}

bool MerkTree::Export(
    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>>* out) const {
  if (out == nullptr) {
    return false;
  }
  if (lazy_loading_) {
    std::string error;
    if (!EnsureFullyLoaded(&error)) {
      return false;
    }
  }
  out->clear();
  std::function<void(const Node*)> walk = [&](const Node* node) {
    if (node == nullptr) {
      return;
    }
    walk(node->left.get());
    out->emplace_back(node->key, node->value);
    walk(node->right.get());
  };
  walk(root_.get());
  return true;
}

bool MerkTree::ExportEncodedNodes(
    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>>* out,
    std::vector<uint8_t>* root_key,
    const ValueHashFn& value_hash_fn,
    std::string* error) const {
  if (out == nullptr || root_key == nullptr) {
    if (error) {
      *error = "encoded export output is null";
    }
    return false;
  }
  if (!EnsureFullyLoaded(error)) {
    return false;
  }
  out->clear();
  root_key->clear();
  if (root_ == nullptr) {
    return true;
  }
  struct EncodedMeta {
    std::vector<uint8_t> hash;
    int height = 0;
    int left_height = 0;
    int right_height = 0;
    AggregateValues aggregate_values;
  };
  std::function<bool(const Node*, EncodedMeta*)> walk =
      [&](const Node* node, EncodedMeta* meta) {
        if (node == nullptr || meta == nullptr) {
          if (error) {
            *error = "encoded export encountered null node";
          }
          return false;
        }
        EncodedMeta left_meta;
        EncodedMeta right_meta;
        bool has_left = node->left != nullptr;
        bool has_right = node->right != nullptr;
        if (has_left) {
          if (!walk(node->left.get(), &left_meta)) {
            return false;
          }
        }
        if (has_right) {
          if (!walk(node->right.get(), &right_meta)) {
            return false;
          }
        }

        std::vector<uint8_t> value_hash;
        if (!ComputeValueHash(node->key, node->value, value_hash_fn, &value_hash, error)) {
          return false;
        }
        std::vector<uint8_t> kv_hash;
        if (!KvDigestToKvHash(node->key, value_hash, &kv_hash, error)) {
          return false;
        }

        AggregateValues node_values;
        if (!ComputeAggregateValuesFromChildren(node,
                                                tree_feature_tag_,
                                                left_meta.aggregate_values,
                                                right_meta.aggregate_values,
                                                &node_values,
                                                error)) {
          return false;
        }
        AggregateValues node_feature_values;
        if (!ComputeNodeFeatureValues(node, tree_feature_tag_, &node_feature_values, error)) {
          return false;
        }

        TreeNodeInner inner;
        inner.has_left = has_left;
        inner.has_right = has_right;
        if (has_left) {
          inner.left.key = node->left->key;
          inner.left.hash = left_meta.hash;
          inner.left.left_height = static_cast<uint8_t>(left_meta.left_height);
          inner.left.right_height = static_cast<uint8_t>(left_meta.right_height);
          inner.left.aggregate = AggregateDataFromValues(tree_feature_tag_,
                                                         left_meta.aggregate_values);
        }
        if (has_right) {
          inner.right.key = node->right->key;
          inner.right.hash = right_meta.hash;
          inner.right.left_height = static_cast<uint8_t>(right_meta.left_height);
          inner.right.right_height = static_cast<uint8_t>(right_meta.right_height);
          inner.right.aggregate = AggregateDataFromValues(tree_feature_tag_,
                                                          right_meta.aggregate_values);
        }
        inner.kv.key = node->key;
        inner.kv.value = node->value;
        inner.kv.kv_hash = kv_hash;
        inner.kv.value_hash = value_hash;
        inner.kv.feature_type = FeatureTypeFromValues(tree_feature_tag_, node_feature_values);

        std::vector<uint8_t> encoded;
        if (!EncodeTreeNodeInner(inner, &encoded, error)) {
          return false;
        }
        out->emplace_back(node->key, std::move(encoded));

        const std::vector<uint8_t> zero(32, 0);
        const std::vector<uint8_t>& left_hash = has_left ? left_meta.hash : zero;
        const std::vector<uint8_t>& right_hash = has_right ? right_meta.hash : zero;
        if (UsesProvableCountHashing(tree_feature_tag_)) {
          if (!NodeHashWithCount(kv_hash, left_hash, right_hash, node_values.count, &meta->hash, error)) {
            return false;
          }
        } else {
          if (!NodeHash(kv_hash, left_hash, right_hash, &meta->hash, error)) {
            return false;
          }
        }
        meta->left_height = has_left ? left_meta.height : 0;
        meta->right_height = has_right ? right_meta.height : 0;
        meta->height = std::max(meta->left_height, meta->right_height) + 1;
        meta->aggregate_values = node_values;
        return true;
      };
  EncodedMeta root_meta;
  if (!walk(root_.get(), &root_meta)) {
    return false;
  }
  *root_key = root_->key;
  return true;
}

bool MerkTree::ExportEncodedNodesForKeys(
    const std::vector<std::vector<uint8_t>>& keys,
    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>>* out,
    std::vector<uint8_t>* root_key,
    const ValueHashFn& value_hash_fn,
    std::string* error) const {
  if (out == nullptr || root_key == nullptr) {
    if (error) {
      *error = "encoded export output is null";
    }
    return false;
  }
  out->clear();
  out->reserve(keys.size());
  root_key->clear();
  if (root_ == nullptr) {
    return true;
  }
  const bool basic_tree = tree_feature_tag_ == TreeFeatureTypeTag::kBasic;
  auto encode_link = [&](const std::vector<uint8_t>& key_bytes,
                         const std::vector<uint8_t>& hash_bytes,
                         uint8_t left_height,
                         uint8_t right_height,
                         const AggregateData& aggregate,
                         std::vector<uint8_t>* encoded_out) -> bool {
    if (encoded_out == nullptr) {
      if (error) {
        *error = "encoded export output is null";
      }
      return false;
    }
    if (key_bytes.size() > std::numeric_limits<uint8_t>::max()) {
      if (error) {
        *error = "link key too long";
      }
      return false;
    }
    if (hash_bytes.size() != 32) {
      if (error) {
        *error = "link hash length mismatch";
      }
      return false;
    }
    encoded_out->push_back(static_cast<uint8_t>(key_bytes.size()));
    encoded_out->insert(encoded_out->end(), key_bytes.begin(), key_bytes.end());
    encoded_out->insert(encoded_out->end(), hash_bytes.begin(), hash_bytes.end());
    encoded_out->push_back(left_height);
    encoded_out->push_back(right_height);
    return EncodeAggregateData(aggregate, encoded_out, error);
  };
  for (const auto& key : keys) {
    Node* node = FindNode(root_.get(), key);
    if (node == nullptr) {
      if (error) {
        *error = "dirty key missing from in-memory tree";
      }
      return false;
    }
    std::vector<uint8_t> value_hash = node->value_hash;
    std::vector<uint8_t> kv_hash = node->kv_hash;
    if (value_hash.empty() || kv_hash.empty()) {
      std::string local_error;
      if (!ComputeValueHash(node->key, node->value, value_hash_fn, &value_hash, &local_error)) {
        if (local_error.find("reference hop limit exceeded") != std::string::npos) {
          if (!ValueHash(node->value, &value_hash, error)) {
            return false;
          }
        } else {
          if (error) {
            *error = local_error;
          }
          return false;
        }
      }
      if (!KvDigestToKvHash(node->key, value_hash, &kv_hash, error)) {
        return false;
      }
    }
    auto recover_meta_child_key = [&](bool left_child, std::vector<uint8_t>* out_key) -> bool {
      if (out_key == nullptr) {
        if (error) {
          *error = "meta child key output is null";
        }
        return false;
      }
      Node::ChildMeta* meta = left_child ? &node->left_meta : &node->right_meta;
      if (!meta->present) {
        out_key->clear();
        return true;
      }
      if (meta->key.empty()) {
        if (error) {
          *error = "child meta is present but has empty key";
        }
        return false;
      }
      if (meta->key != node->key) {
        *out_key = meta->key;
        return true;
      }
      insert_profile::AddCounter(insert_profile::Counter::kRecoverMetaChildKeyLookups);
      if (storage_ == nullptr && storage_tx_ == nullptr) {
        if (error) {
          *error = left_child ? "present left child has invalid persisted key during export"
                              : "present right child has invalid persisted key during export";
        }
        return false;
      }
      std::vector<uint8_t> encoded_current;
      bool found_current = false;
      std::string load_error;
      bool got = false;
      if (storage_tx_ != nullptr) {
        got = storage_tx_->Get(storage_cf_,
                               storage_path_,
                               node->key,
                               &encoded_current,
                               &found_current,
                               &load_error);
        if (!got && IsTransactionUnavailableError(load_error) && storage_ != nullptr) {
          load_error.clear();
          got = storage_->Get(storage_cf_,
                              storage_path_,
                              node->key,
                              &encoded_current,
                              &found_current,
                              &load_error);
        }
      } else {
        got = storage_->Get(storage_cf_,
                            storage_path_,
                            node->key,
                            &encoded_current,
                            &found_current,
                            &load_error);
      }
      if (!got) {
        if (error) {
          *error = load_error.empty() ? "failed to recover child key from stored parent"
                                      : load_error;
        }
        return false;
      }
      if (!found_current) {
        if (error) {
          *error = left_child ? "present left child has invalid persisted key during export"
                              : "present right child has invalid persisted key during export";
        }
        return false;
      }
      TreeNodeInner stored_inner;
      if (!DecodeTreeNodeInner(encoded_current, &stored_inner, error)) {
        return false;
      }
      const auto& stored_child = left_child ? stored_inner.left : stored_inner.right;
      const bool has_child = left_child ? stored_inner.has_left : stored_inner.has_right;
      if (!has_child || stored_child.key.empty() || stored_child.key == node->key) {
        if (error) {
          *error = left_child ? "present left child has invalid persisted key during export"
                              : "present right child has invalid persisted key during export";
        }
        return false;
      }
      *out_key = stored_child.key;
      meta->key = *out_key;
      return true;
    };
    const bool has_left = node->left != nullptr || node->left_meta.present;
    const bool has_right = node->right != nullptr || node->right_meta.present;
    AggregateValues left_values;
    AggregateValues right_values;
    if (!basic_tree) {
      if (node->left) {
        if (!ComputeAggregateValues(node->left.get(), tree_feature_tag_, &left_values, error)) {
          return false;
        }
      } else if (node->left_meta.present) {
        if (!AggregateValuesFromMeta(node->left_meta.aggregate, &left_values, error)) {
          return false;
        }
      }
      if (node->right) {
        if (!ComputeAggregateValues(node->right.get(), tree_feature_tag_, &right_values, error)) {
          return false;
        }
      } else if (node->right_meta.present) {
        if (!AggregateValuesFromMeta(node->right_meta.aggregate, &right_values, error)) {
          return false;
        }
      }
    }
    std::vector<uint8_t> left_key;
    std::vector<uint8_t> left_hash;
    uint8_t left_left_height = 0;
    uint8_t left_right_height = 0;
    AggregateData left_aggregate =
        basic_tree ? AggregateData{} : AggregateDataFromValues(tree_feature_tag_, left_values);
    if (node->left) {
      left_key = node->left->key;
      if (!UsesProvableCountHashing(tree_feature_tag_) && node->left->node_hash.size() == 32) {
        left_hash = node->left->node_hash;
      } else {
        if (!ComputeTreeTypedNodeHash(node->left.get(), value_hash_fn, &left_hash, error)) {
          return false;
        }
      }
      left_left_height = static_cast<uint8_t>(ChildHeight(node->left.get(), true));
      left_right_height = static_cast<uint8_t>(ChildHeight(node->left.get(), false));
    } else if (node->left_meta.present) {
      if (node->left_meta.key.empty()) {
        if (error) {
          *error = "child meta is present but has empty key";
        }
        return false;
      }
      if (node->left_meta.key == node->key) {
        if (!recover_meta_child_key(/*left_child=*/true, &left_key)) {
          return false;
        }
      } else {
        left_key = node->left_meta.key;
      }
      left_hash = node->left_meta.hash;
      left_left_height = static_cast<uint8_t>(node->left_meta.left_height);
      left_right_height = static_cast<uint8_t>(node->left_meta.right_height);
    }
    std::vector<uint8_t> right_key;
    std::vector<uint8_t> right_hash;
    uint8_t right_left_height = 0;
    uint8_t right_right_height = 0;
    AggregateData right_aggregate =
        basic_tree ? AggregateData{} : AggregateDataFromValues(tree_feature_tag_, right_values);
    if (node->right) {
      right_key = node->right->key;
      if (!UsesProvableCountHashing(tree_feature_tag_) && node->right->node_hash.size() == 32) {
        right_hash = node->right->node_hash;
      } else {
        if (!ComputeTreeTypedNodeHash(node->right.get(), value_hash_fn, &right_hash, error)) {
          return false;
        }
      }
      right_left_height = static_cast<uint8_t>(ChildHeight(node->right.get(), true));
      right_right_height = static_cast<uint8_t>(ChildHeight(node->right.get(), false));
    } else if (node->right_meta.present) {
      if (node->right_meta.key.empty()) {
        if (error) {
          *error = "child meta is present but has empty key";
        }
        return false;
      }
      if (node->right_meta.key == node->key) {
        if (!recover_meta_child_key(/*left_child=*/false, &right_key)) {
          return false;
        }
      } else {
        right_key = node->right_meta.key;
      }
      right_hash = node->right_meta.hash;
      right_left_height = static_cast<uint8_t>(node->right_meta.left_height);
      right_right_height = static_cast<uint8_t>(node->right_meta.right_height);
    }
    AggregateValues node_feature_values;
    if (!basic_tree) {
      if (!ComputeNodeFeatureValues(node, tree_feature_tag_, &node_feature_values, error)) {
        return false;
      }
    }
    std::vector<uint8_t> encoded;
    encoded.reserve(2 + 64 + node->value.size() + left_key.size() + right_key.size());
    encoded.push_back(has_left ? 1 : 0);
    if (has_left &&
        !encode_link(left_key,
                     left_hash,
                     left_left_height,
                     left_right_height,
                     left_aggregate,
                     &encoded)) {
      return false;
    }
    encoded.push_back(has_right ? 1 : 0);
    if (has_right &&
        !encode_link(right_key,
                     right_hash,
                     right_left_height,
                     right_right_height,
                     right_aggregate,
                     &encoded)) {
      return false;
    }
    if (basic_tree) {
      encoded.push_back(static_cast<uint8_t>(TreeFeatureTypeTag::kBasic));
    } else {
      if (!EncodeTreeFeatureType(FeatureTypeFromValues(tree_feature_tag_, node_feature_values),
                                 &encoded,
                                 error)) {
        return false;
      }
    }
    if (kv_hash.size() != 32 || value_hash.size() != 32) {
      if (error) {
        *error = "kv hash length mismatch";
      }
      return false;
    }
    encoded.insert(encoded.end(), kv_hash.begin(), kv_hash.end());
    encoded.insert(encoded.end(), value_hash.begin(), value_hash.end());
    encoded.insert(encoded.end(), node->value.begin(), node->value.end());
    out->emplace_back(node->key, std::move(encoded));
  }
  *root_key = root_->key;
  return true;
}

bool MerkTree::FeatureEncodingLengthForKey(const std::vector<uint8_t>& key,
                                           TreeFeatureTypeTag tag,
                                           uint32_t* out_len,
                                           std::string* error) const {
  if (out_len == nullptr) {
    if (error) {
      *error = "feature length output is null";
    }
    return false;
  }
  *out_len = 1;
  if (tag == TreeFeatureTypeTag::kBasic) {
    return true;
  }
  auto zigzag = [](int64_t value) -> uint64_t {
    return (static_cast<uint64_t>(value) << 1) ^ static_cast<uint64_t>(value >> 63);
  };
  struct AggregateResult {
    uint64_t count = 0;
    int64_t sum = 0;
    __int128 big_sum = 0;
    bool found = false;
    uint64_t found_count = 0;
    int64_t found_sum = 0;
    __int128 found_big_sum = 0;
  };
  bool ok = true;
  auto aggregate = [&](auto&& self, const Node* node) -> AggregateResult {
    AggregateResult result;
    if (node == nullptr) {
      return result;
    }
    AggregateResult left = self(self, node->left.get());
    AggregateResult right = self(self, node->right.get());
    bool has_sum = false;
    int64_t node_sum = 0;
    __int128 node_big_sum = 0;
    bool has_big_sum = false;
    if (tag == TreeFeatureTypeTag::kBigSum) {
      if (!ExtractBigSumValueFromElementBytes(node->value, &node_big_sum, &has_big_sum, error)) {
        ok = false;
        return result;
      }
      if (!has_big_sum) {
        node_big_sum = 0;
      }
    } else if (tag == TreeFeatureTypeTag::kSum ||
               tag == TreeFeatureTypeTag::kCountSum ||
               tag == TreeFeatureTypeTag::kProvableCountSum) {
      if (!ExtractSumValueFromElementBytes(node->value, &node_sum, &has_sum, error)) {
        ok = false;
        return result;
      }
      if (!has_sum) {
        node_sum = 0;
      }
      node_big_sum = static_cast<__int128>(node_sum);
    }
    result.count = 1 + left.count + right.count;
    result.sum = node_sum + left.sum + right.sum;
    result.big_sum = node_big_sum + left.big_sum + right.big_sum;
    if (node->key == key) {
      result.found = true;
      result.found_count = result.count;
      result.found_sum = result.sum;
      result.found_big_sum = result.big_sum;
    } else if (left.found) {
      result.found = true;
      result.found_count = left.found_count;
      result.found_sum = left.found_sum;
      result.found_big_sum = left.found_big_sum;
    } else if (right.found) {
      result.found = true;
      result.found_count = right.found_count;
      result.found_sum = right.found_sum;
      result.found_big_sum = right.found_big_sum;
    }
    return result;
  };
  AggregateResult agg = aggregate(aggregate, root_.get());
  if (!ok) {
    return false;
  }
  if (!agg.found) {
    if (error) {
      *error = "feature length key not found";
    }
    return false;
  }
  switch (tag) {
    case TreeFeatureTypeTag::kSum: {
      *out_len = 1 + VarintLenU64(zigzag(agg.found_sum));
      return true;
    }
    case TreeFeatureTypeTag::kBigSum:
      *out_len = 17;
      return true;
    case TreeFeatureTypeTag::kCount:
    case TreeFeatureTypeTag::kProvableCount:
      *out_len = 1 + VarintLenU64(agg.found_count);
      return true;
    case TreeFeatureTypeTag::kCountSum:
    case TreeFeatureTypeTag::kProvableCountSum:
      *out_len = 1 + VarintLenU64(agg.found_count) +
                 VarintLenU64(zigzag(agg.found_sum));
      return true;
    case TreeFeatureTypeTag::kBasic:
      break;
  }
  *out_len = 1;
  return true;
}

bool MerkTree::ExportEncodedNodes(
    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>>* out,
    std::vector<uint8_t>* root_key,
    std::string* error) const {
  return ExportEncodedNodes(out, root_key, ValueHashFn(), error);
}

int MerkTree::Height() const {
  return Height(root_);
}

bool MerkTree::Validate(std::string* error) const {
  if (!EnsureFullyLoaded(error)) {
    return false;
  }
  int height = 0;
  return ValidateNode(root_.get(), nullptr, nullptr, &height, error);
}

MerkTree MerkTree::Clone() const {
  MerkTree out;
  out.root_ = CloneNode(root_.get());
  out.value_hash_fn_ = value_hash_fn_;
  out.value_defined_cost_fn_ = value_defined_cost_fn_;
  out.storage_ = storage_;
  out.storage_tx_ = storage_tx_;
  out.storage_cf_ = storage_cf_;
  out.storage_path_ = storage_path_;
  out.lazy_loading_ = lazy_loading_;
  out.tree_feature_tag_ = tree_feature_tag_;
  out.hash_caches_canonical_ = hash_caches_canonical_;
  out.hash_policy_generation_ = hash_policy_generation_;
  return out;
}

bool MerkTree::FindKeyPath(const std::vector<uint8_t>& key,
                           std::vector<bool>* out,
                           std::string* error) const {
  if (out == nullptr) {
    if (error) {
      *error = "path output is null";
    }
    return false;
  }
  out->clear();
  Node* node = root_.get();
  while (node) {
    int cmp = CompareKeyOrder(key, node->key);
    if (cmp == 0) {
      return true;
    }
    bool go_left = cmp < 0;
    out->push_back(go_left ? kChunkLeft : kChunkRight);
    if (!EnsureChildLoaded(node, go_left, error)) {
      return false;
    }
    node = go_left ? node->left.get() : node->right.get();
  }
  if (error) {
    *error = "key not found";
  }
  return false;
}

bool MerkTree::ComputeNodeHashAtPath(const std::vector<bool>& path,
                                     const ValueHashFn& value_hash_fn,
                                     bool provable_count,
                                     std::vector<uint8_t>* out,
                                     std::string* error) const {
  if (out == nullptr) {
    if (error) {
      *error = "hash output is null";
    }
    return false;
  }
  Node* node = root_.get();
  for (bool step : path) {
    if (node == nullptr) {
      if (error) {
        *error = "invalid traversal path";
      }
      return false;
    }
    if (!EnsureChildLoaded(node, step, error)) {
      return false;
    }
    node = step ? node->left.get() : node->right.get();
  }
  if (node == nullptr) {
    if (error) {
      *error = "invalid traversal path";
    }
    return false;
  }
  if (provable_count) {
    uint64_t count = 0;
    return ComputeNodeHashWithCount(node, value_hash_fn, out, &count, error);
  }
  return ComputeNodeHash(node, value_hash_fn, out, error);
}

bool MerkTree::InsertNode(std::unique_ptr<Node>* node,
                          const std::vector<uint8_t>& key,
                          const std::vector<uint8_t>& value,
                          const ValueHashFn& value_hash_fn,
                          std::unordered_set<std::string>* dirty,
                          std::string* error) {
  if (node == nullptr) {
    if (error) {
      *error = "node pointer is null";
    }
    return false;
  }
  if (!*node) {
    *node = std::make_unique<Node>();
    (*node)->key = key;
    (*node)->value = value;
    (*node)->height = 1;
    if (!UpdateNodeHash(node->get(), value_hash_fn, error)) {
      return false;
    }
    MarkDirtyKey(dirty, key);
    return true;
  }
  if (key == (*node)->key) {
    (*node)->value = value;
    (*node)->hash_generation = 0;
    if (!UpdateNodeHash(node->get(), value_hash_fn, error)) {
      return false;
    }
    MarkDirtyKey(dirty, (*node)->key);
    return true;
  }
  std::vector<uint8_t> changed_child_key;
  bool changed_child_key_valid = false;
  if (CompareKeys(key, (*node)->key)) {
    if (!EnsureChildLoaded(node->get(), true, error)) {
      return false;
    }
    if (!InsertNode(&(*node)->left, key, value, value_hash_fn, dirty, error)) {
      return false;
    }
    if ((*node)->left != nullptr) {
      changed_child_key = (*node)->left->key;
      changed_child_key_valid = true;
    }
    (*node)->left_meta.present = false;
    (*node)->left_meta.key.clear();
    (*node)->left_meta.hash.clear();
    (*node)->left_meta.left_height = 0;
    (*node)->left_meta.right_height = 0;
    (*node)->left_meta.aggregate = AggregateData{};
    MarkDirtyKey(dirty, (*node)->key);
  } else {
    if (!EnsureChildLoaded(node->get(), false, error)) {
      return false;
    }
    if (!InsertNode(&(*node)->right, key, value, value_hash_fn, dirty, error)) {
      return false;
    }
    if ((*node)->right != nullptr) {
      changed_child_key = (*node)->right->key;
      changed_child_key_valid = true;
    }
    (*node)->right_meta.present = false;
    (*node)->right_meta.key.clear();
    (*node)->right_meta.hash.clear();
    (*node)->right_meta.left_height = 0;
    (*node)->right_meta.right_height = 0;
    (*node)->right_meta.aggregate = AggregateData{};
    MarkDirtyKey(dirty, (*node)->key);
  }
  UpdateHeight(node->get());
  int balance = BalanceFactor(*node);
  bool rotated = false;

  if (balance > 1) {
    if (!EnsureChildLoaded(node->get(), true, error) || (*node)->left == nullptr) {
      if (error && error->empty()) {
        *error = "left child missing while rebalancing";
      }
      return false;
    }
    if (CompareKeys(key, (*node)->left->key)) {
      RotateRight(node);
      rotated = true;
    } else {
      if (!EnsureChildLoaded((*node)->left.get(), false, error)) {
        return false;
      }
      if ((*node)->left->right == nullptr) {
        RotateRight(node);
        rotated = true;
      } else {
        RotateLeft(&(*node)->left);
        rotated = true;
      }
      RotateRight(node);
      rotated = true;
    }
  } else if (balance < -1) {
    if (!EnsureChildLoaded(node->get(), false, error) || (*node)->right == nullptr) {
      if (error && error->empty()) {
        *error = "right child missing while rebalancing";
      }
      return false;
    }
    if (!CompareKeys(key, (*node)->right->key)) {
      RotateLeft(node);
      rotated = true;
    } else {
      if (!EnsureChildLoaded((*node)->right.get(), true, error)) {
        return false;
      }
      if ((*node)->right->left == nullptr) {
        RotateLeft(node);
        rotated = true;
      } else {
        RotateRight(&(*node)->right);
        rotated = true;
      }
      RotateLeft(node);
      rotated = true;
    }
  }
  if (*node) {
    if (rotated && (*node)->left) {
      if (!UpdateNodeHash((*node)->left.get(), value_hash_fn, error)) {
        return false;
      }
    }
    if (rotated && (*node)->right) {
      if (!UpdateNodeHash((*node)->right.get(), value_hash_fn, error)) {
        return false;
      }
    }
    if (!UpdateNodeHash(node->get(), value_hash_fn, error)) {
      return false;
    }
    MarkDirtyKey(dirty, (*node)->key);
    if (rotated && (*node)->left) {
      MarkDirtyKey(dirty, (*node)->left->key);
    }
    if (rotated && (*node)->right) {
      MarkDirtyKey(dirty, (*node)->right->key);
    } else if (!rotated && changed_child_key_valid) {
      MarkDirtyKey(dirty, changed_child_key);
    }
  }
  return true;
}

bool MerkTree::InsertNodeWithCount(std::unique_ptr<Node>* node,
                                   const std::vector<uint8_t>& key,
                                   const std::vector<uint8_t>& value,
                                   const ValueHashFn& value_hash_fn,
                                   std::unordered_set<std::string>* dirty,
                                   uint64_t* hash_calls,
                                   std::string* error) {
  if (node == nullptr || hash_calls == nullptr) {
    if (error) {
      *error = "node or hash output is null";
    }
    return false;
  }
  if (!*node) {
    *node = std::make_unique<Node>();
    (*node)->key = key;
    (*node)->value = value;
    (*node)->height = 1;
    *hash_calls += 1;
    if (!UpdateNodeHash(node->get(), value_hash_fn, error)) {
      return false;
    }
    MarkDirtyKey(dirty, key);
    return true;
  }
  if (key == (*node)->key) {
    (*node)->value = value;
    (*node)->hash_generation = 0;
    *hash_calls += 1;
    if (!UpdateNodeHash(node->get(), value_hash_fn, error)) {
      return false;
    }
    MarkDirtyKey(dirty, (*node)->key);
    return true;
  }
  std::vector<uint8_t> changed_child_key;
  bool changed_child_key_valid = false;
  if (CompareKeys(key, (*node)->key)) {
    if (!EnsureChildLoaded(node->get(), true, error)) {
      return false;
    }
    if (!InsertNodeWithCount(&(*node)->left,
                             key,
                             value,
                             value_hash_fn,
                             dirty,
                             hash_calls,
                             error)) {
      return false;
    }
    if ((*node)->left != nullptr) {
      changed_child_key = (*node)->left->key;
      changed_child_key_valid = true;
    }
    (*node)->left_meta.present = false;
    (*node)->left_meta.key.clear();
  } else {
    if (!EnsureChildLoaded(node->get(), false, error)) {
      return false;
    }
    if (!InsertNodeWithCount(&(*node)->right,
                             key,
                             value,
                             value_hash_fn,
                             dirty,
                             hash_calls,
                             error)) {
      return false;
    }
    if ((*node)->right != nullptr) {
      changed_child_key = (*node)->right->key;
      changed_child_key_valid = true;
    }
    (*node)->right_meta.present = false;
    (*node)->right_meta.key.clear();
  }
  UpdateHeight(node->get());
  int balance = BalanceFactor(*node);
  bool rotated = false;

  if (balance > 1) {
    if (!EnsureChildLoaded(node->get(), true, error) || (*node)->left == nullptr) {
      if (error && error->empty()) {
        *error = "left child missing while rebalancing";
      }
      return false;
    }
    if (CompareKeys(key, (*node)->left->key)) {
      RotateRight(node);
      rotated = true;
    } else {
      if (!EnsureChildLoaded((*node)->left.get(), false, error)) {
        return false;
      }
      if ((*node)->left->right == nullptr) {
        RotateRight(node);
        rotated = true;
      } else {
        RotateLeft(&(*node)->left);
        rotated = true;
      }
      RotateRight(node);
      rotated = true;
    }
  } else if (balance < -1) {
    if (!EnsureChildLoaded(node->get(), false, error) || (*node)->right == nullptr) {
      if (error && error->empty()) {
        *error = "right child missing while rebalancing";
      }
      return false;
    }
    if (!CompareKeys(key, (*node)->right->key)) {
      RotateLeft(node);
      rotated = true;
    } else {
      if (!EnsureChildLoaded((*node)->right.get(), true, error)) {
        return false;
      }
      if ((*node)->right->left == nullptr) {
        RotateLeft(node);
        rotated = true;
      } else {
        RotateRight(&(*node)->right);
        rotated = true;
      }
      RotateLeft(node);
      rotated = true;
    }
  }
  *hash_calls += 1;
  if (*node) {
    if (rotated && (*node)->left) {
      if (!UpdateNodeHash((*node)->left.get(), value_hash_fn, error)) {
        return false;
      }
    }
    if (rotated && (*node)->right) {
      if (!UpdateNodeHash((*node)->right.get(), value_hash_fn, error)) {
        return false;
      }
    }
    if (!UpdateNodeHash(node->get(), value_hash_fn, error)) {
      return false;
    }
    MarkDirtyKey(dirty, (*node)->key);
    if (rotated && (*node)->left) {
      MarkDirtyKey(dirty, (*node)->left->key);
    }
    if (rotated && (*node)->right) {
      MarkDirtyKey(dirty, (*node)->right->key);
    } else if (!rotated && changed_child_key_valid) {
      MarkDirtyKey(dirty, changed_child_key);
    }
  }
  return true;
}

bool MerkTree::InsertNodeWithCost(std::unique_ptr<Node>* node,
                                  const std::vector<uint8_t>& key,
                                  const std::vector<uint8_t>& value,
                                  const ValueHashFn& value_hash_fn,
                                  const ValueDefinedCostFn* value_defined_cost_fn,
                                  std::unordered_set<std::string>* dirty,
                                  TreeFeatureTypeTag tree_feature_tag,
                                  OperationCost* cost,
                                  std::string* error) {
  (void)tree_feature_tag;
  if (cost == nullptr) {
    if (error) {
      *error = "cost output is null";
    }
    return false;
  }
  bool had_existing = false;
  Node* old_node = nullptr;
  uint32_t old_value_len = 0;
  std::optional<ValueDefinedCostType> old_defined_cost;
  uint32_t old_paid_value_len = 0;
  bool old_paid_value_len_set = false;
  std::unordered_map<std::string, uint32_t> old_paid_values;
  std::function<bool(const Node*)> collect_old_costs = [&](const Node* current) -> bool {
    if (current == nullptr) {
      return true;
    }
    std::optional<ValueDefinedCostType> current_defined_cost;
    if (value_defined_cost_fn != nullptr && *value_defined_cost_fn) {
      if (!(*value_defined_cost_fn)(current->value, &current_defined_cost, error)) {
        return false;
      }
    }
    const uint32_t key_len = static_cast<uint32_t>(current->key.size());
    uint32_t paid_value_len = 0;
    if (current_defined_cost.has_value()) {
      if (current_defined_cost->kind == ValueDefinedCostType::Kind::kLayered) {
        paid_value_len = LayeredValueByteCostSizeForKeyAndValueLengths(
            key_len, current_defined_cost->cost, TreeFeatureTypeTag::kBasic);
      } else {
        paid_value_len = NodeValueByteCostSize(
            key_len, current_defined_cost->cost, TreeFeatureTypeTag::kBasic);
      }
    } else {
      paid_value_len = NodeValueByteCostSize(
          key_len, static_cast<uint32_t>(current->value.size()), TreeFeatureTypeTag::kBasic);
    }
    old_paid_values.emplace(KeyToString(current->key), paid_value_len);
    return collect_old_costs(current->left.get()) && collect_old_costs(current->right.get());
  };
  if (node != nullptr && *node) {
    if (!collect_old_costs(node->get())) {
      return false;
    }
    old_node = FindNode(node->get(), key);
    had_existing = old_node != nullptr;
    if (old_node != nullptr) {
      if (value_defined_cost_fn != nullptr && *value_defined_cost_fn) {
        if (!(*value_defined_cost_fn)(old_node->value, &old_defined_cost, error)) {
          return false;
        }
        if (DebugFacadeCostEnabled()) {
          std::cerr << "MERK_COST\tinsert\told_defined\tkey_len=" << key.size()
                    << "\thas=" << (old_defined_cost.has_value() ? 1 : 0);
          if (old_defined_cost.has_value()) {
            std::cerr << "\tkind="
                      << (old_defined_cost->kind == ValueDefinedCostType::Kind::kLayered
                              ? "layered"
                              : "specialized")
                      << "\tcost=" << old_defined_cost->cost;
          }
          std::cerr << "\n";
        }
      }
      if (old_defined_cost.has_value()) {
        uint32_t key_len = static_cast<uint32_t>(key.size());
        if (old_defined_cost->kind == ValueDefinedCostType::Kind::kLayered) {
          old_paid_value_len = LayeredValueByteCostSizeForKeyAndValueLengths(
              key_len, old_defined_cost->cost, TreeFeatureTypeTag::kBasic);
        } else {
          old_paid_value_len = NodeValueByteCostSize(
              key_len, old_defined_cost->cost, TreeFeatureTypeTag::kBasic);
        }
        old_paid_value_len_set = true;
      } else {
        old_value_len = static_cast<uint32_t>(old_node->value.size());
        old_paid_value_len = NodeValueByteCostSize(
            static_cast<uint32_t>(key.size()), old_value_len, TreeFeatureTypeTag::kBasic);
        old_paid_value_len_set = true;
      }
    }
  }
  std::unordered_set<std::string> local_dirty;
  std::unordered_set<std::string>* dirty_target = &local_dirty;
  uint64_t hash_calls = 0;
  if (!InsertNodeWithCount(node, key, value, value_hash_fn, dirty_target, &hash_calls, error)) {
    return false;
  }
  if (dirty != nullptr) {
    dirty->insert(local_dirty.begin(), local_dirty.end());
  }
  cost->hash_node_calls = static_cast<uint32_t>(hash_calls);
  const std::string key_str = KeyToString(key);
  for (const auto& entry : local_dirty) {
    std::vector<uint8_t> entry_key = StringToKey(entry);
    Node* dirty_node = FindNode(node ? node->get() : nullptr, entry_key);
    if (dirty_node == nullptr) {
      continue;
    }
    uint32_t value_len = 0;
    value_len = static_cast<uint32_t>(dirty_node->value.size());
    OperationCost::ChildrenSizesWithIsSumTree children_sizes;
    if (!BuildChildrenSizes(dirty_node, &children_sizes, error)) {
      return false;
    }
    std::optional<ValueDefinedCostType> defined_cost;
    if (value_defined_cost_fn != nullptr && *value_defined_cost_fn) {
      if (!(*value_defined_cost_fn)(dirty_node->value, &defined_cost, error)) {
        return false;
      }
      if (DebugFacadeCostEnabled()) {
        std::cerr << "MERK_COST\tinsert\tdirty_defined\tentry_key_len=" << entry_key.size()
                  << "\thas=" << (defined_cost.has_value() ? 1 : 0);
        if (defined_cost.has_value()) {
          std::cerr << "\tkind="
                    << (defined_cost->kind == ValueDefinedCostType::Kind::kLayered
                            ? "layered"
                            : "specialized")
                    << "\tcost=" << defined_cost->cost;
        }
        std::cerr << "\n";
      }
    }
    uint32_t key_len = static_cast<uint32_t>(entry_key.size());
    uint32_t prefixed_key_len = key_len + 32;
    uint32_t paid_key_len = prefixed_key_len + RequiredSpaceU32(prefixed_key_len);
    uint32_t paid_value_len = 0;
    if (defined_cost.has_value()) {
      if (defined_cost->kind == ValueDefinedCostType::Kind::kLayered) {
        paid_value_len = LayeredValueByteCostSizeForKeyAndValueLengths(
            key_len, defined_cost->cost, TreeFeatureTypeTag::kBasic);
      } else {
        paid_value_len = NodeValueByteCostSize(
            key_len, defined_cost->cost, TreeFeatureTypeTag::kBasic);
      }
    } else {
      paid_value_len = NodeValueByteCostSize(key_len, value_len, TreeFeatureTypeTag::kBasic);
    }
    KeyValueStorageCost kv_cost;
    kv_cost.needs_value_verification = false;
    if (entry == key_str) {
      if (had_existing) {
        kv_cost.new_node = false;
        if (!old_paid_value_len_set) {
          if (error) {
            *error = "missing old value length for cost update";
          }
          return false;
        }
        if (paid_value_len == old_paid_value_len) {
          kv_cost.value_storage_cost.replaced_bytes = old_paid_value_len;
        } else if (paid_value_len < old_paid_value_len) {
          kv_cost.value_storage_cost.replaced_bytes = paid_value_len;
          kv_cost.value_storage_cost.removed_bytes =
              StorageRemovedBytes::Basic(old_paid_value_len - paid_value_len);
        } else {
          kv_cost.value_storage_cost.replaced_bytes = old_paid_value_len;
          kv_cost.value_storage_cost.added_bytes = paid_value_len - old_paid_value_len;
        }
      } else {
        kv_cost.new_node = true;
        kv_cost.key_storage_cost.added_bytes = paid_key_len;
        kv_cost.value_storage_cost.added_bytes = paid_value_len;
      }
    } else {
      kv_cost.new_node = false;
      auto old_it = old_paid_values.find(entry);
      if (old_it == old_paid_values.end()) {
        kv_cost.value_storage_cost.replaced_bytes = paid_value_len;
      } else {
        const uint32_t old_cost = old_it->second;
        if (paid_value_len == old_cost) {
          kv_cost.value_storage_cost.replaced_bytes = old_cost;
        } else if (paid_value_len < old_cost) {
          kv_cost.value_storage_cost.replaced_bytes = paid_value_len;
          kv_cost.value_storage_cost.removed_bytes =
              StorageRemovedBytes::Basic(old_cost - paid_value_len);
        } else {
          kv_cost.value_storage_cost.replaced_bytes = old_cost;
          kv_cost.value_storage_cost.added_bytes = paid_value_len - old_cost;
        }
      }
    }
    if (!cost->AddKeyValueStorageCosts(prefixed_key_len,
                                       value_len,
                                       children_sizes,
                                       std::optional<KeyValueStorageCost>(kv_cost),
                                       error)) {
      return false;
    }
  }
  return true;
}

int MerkTree::Height(const std::unique_ptr<Node>& node) {
  return node ? node->height : 0;
}

void MerkTree::UpdateHeight(Node* node) {
  if (!node) {
    return;
  }
  int left_height = ChildHeight(node, true);
  int right_height = ChildHeight(node, false);
  node->height = 1 + (left_height > right_height ? left_height : right_height);
}

bool MerkTree::UpdateNodeHash(Node* node,
                              const ValueHashFn& value_hash_fn,
                              std::string* error) {
  insert_profile::AddCounter(insert_profile::Counter::kUpdateNodeHashCalls);
  if (node == nullptr) {
    if (error) {
      *error = "node is null";
    }
    return false;
  }
  std::vector<uint8_t> value_hash = node->value_hash;
  std::vector<uint8_t> kv_hash = node->kv_hash;
  if (node->hash_generation != hash_policy_generation_ ||
      value_hash.size() != 32 ||
      kv_hash.size() != 32) {
    std::string local_error;
    if (!ComputeValueHash(node->key, node->value, value_hash_fn, &value_hash, &local_error)) {
      if (local_error.find("reference hop limit exceeded") != std::string::npos) {
        if (!ValueHash(node->value, &value_hash, error)) {
          return false;
        }
      } else {
        if (error) {
          *error = local_error;
        }
        return false;
      }
    }
    if (!KvDigestToKvHash(node->key, value_hash, &kv_hash, error)) {
      return false;
    }
  }
  std::vector<uint8_t> left_hash(32, 0);
  std::vector<uint8_t> right_hash(32, 0);
  if (node->left) {
    if (node->left->hash_generation != hash_policy_generation_ ||
        node->left->node_hash.size() != 32) {
      if (!UpdateNodeHash(node->left.get(), value_hash_fn, error)) {
        return false;
      }
    }
    if (node->left->hash_generation != hash_policy_generation_ ||
        node->left->node_hash.size() != 32) {
      if (error) {
        *error = "left child hash length mismatch";
      }
      return false;
    }
    left_hash = node->left->node_hash;
  } else if (node->left_meta.present) {
    if (node->left_meta.hash.size() != 32) {
      if (error) {
        *error = "left child metadata hash length mismatch";
      }
      return false;
    }
    left_hash = node->left_meta.hash;
  }
  if (node->right) {
    if (node->right->hash_generation != hash_policy_generation_ ||
        node->right->node_hash.size() != 32) {
      if (!UpdateNodeHash(node->right.get(), value_hash_fn, error)) {
        return false;
      }
    }
    if (node->right->hash_generation != hash_policy_generation_ ||
        node->right->node_hash.size() != 32) {
      if (error) {
        *error = "right child hash length mismatch";
      }
      return false;
    }
    right_hash = node->right->node_hash;
  } else if (node->right_meta.present) {
    if (node->right_meta.hash.size() != 32) {
      if (error) {
        *error = "right child metadata hash length mismatch";
      }
      return false;
    }
    right_hash = node->right_meta.hash;
  }
  std::vector<uint8_t> node_hash;
  if (!NodeHash(kv_hash, left_hash, right_hash, &node_hash, error)) {
    return false;
  }
  node->value_hash = std::move(value_hash);
  node->kv_hash = std::move(kv_hash);
  node->node_hash = std::move(node_hash);
  node->hash_generation = hash_policy_generation_;
  return true;
}

void MerkTree::UpdateNodeHashOrDie(Node* node, const ValueHashFn& value_hash_fn) {
  std::string error;
  if (!UpdateNodeHash(node, value_hash_fn, &error)) {
    // Best-effort: keep hash empty to avoid crashing on benign paths.
  }
}

void MerkTree::ClearCachedHashes(Node* node) {
  if (node == nullptr) {
    return;
  }
  node->value_hash.clear();
  node->kv_hash.clear();
  node->node_hash.clear();
  node->hash_generation = 0;
  ClearCachedHashes(node->left.get());
  ClearCachedHashes(node->right.get());
}

bool MerkTree::RebuildHashCaches(std::string* error) {
  if (!root_) {
    hash_caches_canonical_ = true;
    return true;
  }
  if (!EnsureFullyLoaded(error)) {
    return false;
  }
  ClearCachedHashes(root_.get());
  if (!UpdateNodeHash(root_.get(), value_hash_fn_, error)) {
    return false;
  }
  hash_caches_canonical_ = true;
  return true;
}

int MerkTree::BalanceFactor(const std::unique_ptr<Node>& node) {
  if (!node) {
    return 0;
  }
  int left_height = ChildHeight(node.get(), true);
  int right_height = ChildHeight(node.get(), false);
  return left_height - right_height;
}

MerkTree::Node* MerkTree::FindMinNode(Node* node) {
  Node* current = node;
  while (current && current->left) {
    current = current->left.get();
  }
  return current;
}

MerkTree::Node* MerkTree::FindNode(Node* node, const std::vector<uint8_t>& key) {
  Node* current = node;
  while (current) {
    if (key == current->key) {
      return current;
    }
    if (CompareKeys(key, current->key)) {
      current = current->left.get();
    } else {
      current = current->right.get();
    }
  }
  return nullptr;
}

bool MerkTree::RecomputeHashesForKey(Node* node,
                                     const std::vector<uint8_t>& key,
                                     const ValueHashFn& value_hash_fn,
                                     std::unordered_set<std::string>* dirty,
                                     std::string* error) {
  if (node == nullptr) {
    if (error) {
      *error = "key not found";
    }
    return false;
  }
  if (key < node->key) {
    if (!RecomputeHashesForKey(node->left.get(), key, value_hash_fn, dirty, error)) {
      return false;
    }
  } else if (key > node->key) {
    if (!RecomputeHashesForKey(node->right.get(), key, value_hash_fn, dirty, error)) {
      return false;
    }
  }
  if (!UpdateNodeHash(node, value_hash_fn, error)) {
    return false;
  }
  if (dirty != nullptr) {
    dirty->insert(KeyToString(node->key));
  }
  return true;
}

void MerkTree::RotateLeft(std::unique_ptr<Node>* node) {
  if (node == nullptr || *node == nullptr || (*node)->right == nullptr) {
    return;
  }
  std::unique_ptr<Node> pivot = std::move((*node)->right);
  Node::ChildMeta transferred_meta = pivot->left_meta;
  (*node)->right = std::move(pivot->left);
  if ((*node)->right != nullptr) {
    // Clear all right metadata fields to force recomputation from actual loaded child.
    (*node)->right_meta.present = false;
    (*node)->right_meta.key.clear();
    (*node)->right_meta.hash.clear();
    (*node)->right_meta.left_height = 0;
    (*node)->right_meta.right_height = 0;
    (*node)->right_meta.aggregate = AggregateData{};
  } else {
    (*node)->right_meta = std::move(transferred_meta);
  }
  pivot->left = std::move(*node);
  // Clear all left metadata fields to force recomputation from actual children
  pivot->left_meta.present = false;
  pivot->left_meta.key.clear();
  pivot->left_meta.hash.clear();
  pivot->left_meta.left_height = 0;
  pivot->left_meta.right_height = 0;
  pivot->left_meta.aggregate = AggregateData{};
  UpdateHeight(pivot->left.get());
  UpdateHeight(pivot.get());
  *node = std::move(pivot);
}

void MerkTree::RotateRight(std::unique_ptr<Node>* node) {
  if (node == nullptr || *node == nullptr || (*node)->left == nullptr) {
    return;
  }
  std::unique_ptr<Node> pivot = std::move((*node)->left);
  Node::ChildMeta transferred_meta = pivot->right_meta;
  (*node)->left = std::move(pivot->right);
  if ((*node)->left != nullptr) {
    // Clear all left metadata fields to force recomputation from actual loaded child.
    (*node)->left_meta.present = false;
    (*node)->left_meta.key.clear();
    (*node)->left_meta.hash.clear();
    (*node)->left_meta.left_height = 0;
    (*node)->left_meta.right_height = 0;
    (*node)->left_meta.aggregate = AggregateData{};
  } else {
    (*node)->left_meta = std::move(transferred_meta);
  }
  pivot->right = std::move(*node);
  // Clear all right metadata fields to force recomputation from actual children
  pivot->right_meta.present = false;
  pivot->right_meta.key.clear();
  pivot->right_meta.hash.clear();
  pivot->right_meta.left_height = 0;
  pivot->right_meta.right_height = 0;
  pivot->right_meta.aggregate = AggregateData{};
  UpdateHeight(pivot->right.get());
  UpdateHeight(pivot.get());
  *node = std::move(pivot);
}

bool MerkTree::DeleteNode(std::unique_ptr<Node>* node,
                          const std::vector<uint8_t>& key,
                          const ValueHashFn& value_hash_fn,
                          std::unordered_set<std::string>* dirty,
                          std::unordered_set<std::string>* deleted,
                          bool* deleted_flag,
                          std::string* error) {
  if (node == nullptr || deleted_flag == nullptr) {
    if (error) {
      *error = "node or deleted output is null";
    }
    return false;
  }
  if (!*node) {
    *deleted_flag = false;
    return true;
  }
  if (CompareKeys(key, (*node)->key)) {
    if (!EnsureChildLoaded(node->get(), true, error)) {
      return false;
    }
    if (!DeleteNode(&(*node)->left,
                    key,
                    value_hash_fn,
                    dirty,
                    deleted,
                    deleted_flag,
                    error)) {
      return false;
    }
    (*node)->left_meta.present = false;
    (*node)->left_meta.key.clear();
    (*node)->left_meta.hash.clear();
    (*node)->left_meta.left_height = 0;
    (*node)->left_meta.right_height = 0;
    (*node)->left_meta.aggregate = AggregateData{};
    MarkDirtyKey(dirty, (*node)->key);
  } else if (CompareKeys((*node)->key, key)) {
    if (!EnsureChildLoaded(node->get(), false, error)) {
      return false;
    }
    if (!DeleteNode(&(*node)->right,
                    key,
                    value_hash_fn,
                    dirty,
                    deleted,
                    deleted_flag,
                    error)) {
      return false;
    }
    (*node)->right_meta.present = false;
    (*node)->right_meta.key.clear();
    (*node)->right_meta.hash.clear();
    (*node)->right_meta.left_height = 0;
    (*node)->right_meta.right_height = 0;
    (*node)->right_meta.aggregate = AggregateData{};
    MarkDirtyKey(dirty, (*node)->key);
  } else {
    *deleted_flag = true;
    MarkDeletedKey(deleted, key);
    RemoveTrackedDirtyKey(dirty, key);
    if (!EnsureChildLoaded(node->get(), true, error)) {
      return false;
    }
    if (!EnsureChildLoaded(node->get(), false, error)) {
      return false;
    }
    if (!(*node)->left || !(*node)->right) {
      std::unique_ptr<Node> replacement =
          (*node)->left ? std::move((*node)->left) : std::move((*node)->right);
      *node = std::move(replacement);
      return true;
    }
    if (!EnsureChildLoaded(node->get(), false, error)) {
      return false;
    }
    Node* successor = (*node)->right.get();
    while (successor != nullptr) {
      if (!EnsureChildLoaded(successor, true, error)) {
        return false;
      }
      if (successor->left == nullptr) {
        break;
      }
      successor = successor->left.get();
    }
    if (successor == nullptr) {
      if (error) {
        *error = "failed to find successor";
      }
      return false;
    }
    (*node)->key = successor->key;
    (*node)->value = successor->value;
    (*node)->hash_generation = 0;
    bool removed = false;
    if (!DeleteNode(&(*node)->right,
                    successor->key,
                    value_hash_fn,
                    dirty,
                    deleted,
                    &removed,
                    error)) {
      return false;
    }
    (*node)->right_meta.present = false;
    (*node)->right_meta.key.clear();
    (*node)->right_meta.hash.clear();
    (*node)->right_meta.left_height = 0;
    (*node)->right_meta.right_height = 0;
    (*node)->right_meta.aggregate = AggregateData{};
    *deleted_flag = true;
  }

  if (!*node) {
    return true;
  }
  UpdateHeight(node->get());
  int balance = BalanceFactor(*node);
  if (balance > 1) {
    if (!EnsureChildLoaded(node->get(), true, error) || (*node)->left == nullptr) {
      if (error && error->empty()) {
        *error = "left child missing while rebalancing";
      }
      return false;
    }
    if (BalanceFactor((*node)->left) >= 0) {
      RotateRight(node);
    } else {
      if (!EnsureChildLoaded((*node)->left.get(), false, error)) {
        return false;
      }
      if ((*node)->left->right == nullptr) {
        RotateRight(node);
      } else {
        RotateLeft(&(*node)->left);
      }
      RotateRight(node);
    }
  } else if (balance < -1) {
    if (!EnsureChildLoaded(node->get(), false, error) || (*node)->right == nullptr) {
      if (error && error->empty()) {
        *error = "right child missing while rebalancing";
      }
      return false;
    }
    if (BalanceFactor((*node)->right) <= 0) {
      RotateLeft(node);
    } else {
      if (!EnsureChildLoaded((*node)->right.get(), true, error)) {
        return false;
      }
      if ((*node)->right->left == nullptr) {
        RotateLeft(node);
      } else {
        RotateRight(&(*node)->right);
      }
      RotateLeft(node);
    }
  }
  if (*node) {
    if ((*node)->left) {
      if (!UpdateNodeHash((*node)->left.get(), value_hash_fn, error)) {
        return false;
      }
    }
    if ((*node)->right) {
      if (!UpdateNodeHash((*node)->right.get(), value_hash_fn, error)) {
        return false;
      }
    }
    if (!UpdateNodeHash(node->get(), value_hash_fn, error)) {
      return false;
    }
    MarkDirtyKey(dirty, (*node)->key);
    if ((*node)->left) {
      MarkDirtyKey(dirty, (*node)->left->key);
    }
    if ((*node)->right) {
      MarkDirtyKey(dirty, (*node)->right->key);
    }
  }
  return true;
}

bool MerkTree::DeleteNodeWithCount(std::unique_ptr<Node>* node,
                                   const std::vector<uint8_t>& key,
                                   const ValueHashFn& value_hash_fn,
                                   std::unordered_set<std::string>* dirty,
                                   std::unordered_set<std::string>* deleted,
                                   bool* deleted_flag,
                                   uint64_t* hash_calls,
                                   std::string* error) {
  if (node == nullptr || deleted_flag == nullptr || hash_calls == nullptr) {
    if (error) {
      *error = "node, deleted, or hash output is null";
    }
    return false;
  }
  if (!*node) {
    *deleted_flag = false;
    return true;
  }
  if (CompareKeys(key, (*node)->key)) {
    if (!EnsureChildLoaded(node->get(), true, error)) {
      return false;
    }
    if (!DeleteNodeWithCount(&(*node)->left,
                             key,
                             value_hash_fn,
                             dirty,
                             deleted,
                             deleted_flag,
                             hash_calls,
                             error)) {
      return false;
    }
    (*node)->left_meta.present = false;
    (*node)->left_meta.key.clear();
  } else if (CompareKeys((*node)->key, key)) {
    if (!EnsureChildLoaded(node->get(), false, error)) {
      return false;
    }
    if (!DeleteNodeWithCount(&(*node)->right,
                             key,
                             value_hash_fn,
                             dirty,
                             deleted,
                             deleted_flag,
                             hash_calls,
                             error)) {
      return false;
    }
    (*node)->right_meta.present = false;
    (*node)->right_meta.key.clear();
  } else {
    *deleted_flag = true;
    MarkDeletedKey(deleted, key);
    RemoveTrackedDirtyKey(dirty, key);
    if (!EnsureChildLoaded(node->get(), true, error)) {
      return false;
    }
    if (!EnsureChildLoaded(node->get(), false, error)) {
      return false;
    }
    if (!(*node)->left || !(*node)->right) {
      std::unique_ptr<Node> replacement =
          (*node)->left ? std::move((*node)->left) : std::move((*node)->right);
      *node = std::move(replacement);
      *hash_calls += 1;
      return true;
    }
    if (!EnsureChildLoaded(node->get(), false, error)) {
      return false;
    }
    Node* successor = (*node)->right.get();
    while (successor != nullptr) {
      if (!EnsureChildLoaded(successor, true, error)) {
        return false;
      }
      if (successor->left == nullptr) {
        break;
      }
      successor = successor->left.get();
    }
    if (successor == nullptr) {
      if (error) {
        *error = "failed to find successor";
      }
      return false;
    }
    (*node)->key = successor->key;
    (*node)->value = successor->value;
    (*node)->hash_generation = 0;
    bool removed = false;
    if (!DeleteNodeWithCount(&(*node)->right,
                             successor->key,
                             value_hash_fn,
                             dirty,
                             deleted,
                             &removed,
                             hash_calls,
                             error)) {
      return false;
    }
    (*node)->right_meta.present = false;
    (*node)->right_meta.key.clear();
    (*node)->right_meta.hash.clear();
    (*node)->right_meta.left_height = 0;
    (*node)->right_meta.right_height = 0;
    (*node)->right_meta.aggregate = AggregateData{};
    *deleted_flag = true;
  }

  if (!*node) {
    return true;
  }
  UpdateHeight(node->get());
  int balance = BalanceFactor(*node);
  if (balance > 1) {
    if (!EnsureChildLoaded(node->get(), true, error) || (*node)->left == nullptr) {
      if (error && error->empty()) {
        *error = "left child missing while rebalancing";
      }
      return false;
    }
    if (BalanceFactor((*node)->left) >= 0) {
      RotateRight(node);
    } else {
      if (!EnsureChildLoaded((*node)->left.get(), false, error)) {
        return false;
      }
      if ((*node)->left->right == nullptr) {
        RotateRight(node);
      } else {
        RotateLeft(&(*node)->left);
      }
      RotateRight(node);
    }
  } else if (balance < -1) {
    if (!EnsureChildLoaded(node->get(), false, error) || (*node)->right == nullptr) {
      if (error && error->empty()) {
        *error = "right child missing while rebalancing";
      }
      return false;
    }
    if (BalanceFactor((*node)->right) <= 0) {
      RotateLeft(node);
    } else {
      if (!EnsureChildLoaded((*node)->right.get(), true, error)) {
        return false;
      }
      if ((*node)->right->left == nullptr) {
        RotateLeft(node);
      } else {
        RotateRight(&(*node)->right);
      }
      RotateLeft(node);
    }
  }
  *hash_calls += 1;
  if (*node) {
    if ((*node)->left) {
      if (!UpdateNodeHash((*node)->left.get(), value_hash_fn, error)) {
        return false;
      }
    }
    if ((*node)->right) {
      if (!UpdateNodeHash((*node)->right.get(), value_hash_fn, error)) {
        return false;
      }
    }
    if (!UpdateNodeHash(node->get(), value_hash_fn, error)) {
      return false;
    }
    MarkDirtyKey(dirty, (*node)->key);
    if ((*node)->left) {
      MarkDirtyKey(dirty, (*node)->left->key);
    }
    if ((*node)->right) {
      MarkDirtyKey(dirty, (*node)->right->key);
    }
  }
  return true;
}

bool MerkTree::DeleteNodeWithCost(std::unique_ptr<Node>* node,
                                  const std::vector<uint8_t>& key,
                                  const ValueHashFn& value_hash_fn,
                                  const ValueDefinedCostFn* value_defined_cost_fn,
                                  std::unordered_set<std::string>* dirty,
                                  std::unordered_set<std::string>* deleted,
                                  bool* deleted_flag,
                                  TreeFeatureTypeTag tree_feature_tag,
                                  OperationCost* cost,
                                  std::string* error) {
  if (cost == nullptr) {
    if (error) {
      *error = "cost output is null";
    }
    return false;
  }
  uint32_t removed_key_len = 0;
  uint32_t removed_value_len = 0;
  std::optional<ValueDefinedCostType> removed_defined_cost;
  bool had_existing = false;
  std::unordered_map<std::string, uint32_t> old_paid_values;
  std::function<bool(const Node*)> collect_old_costs = [&](const Node* current) -> bool {
    if (current == nullptr) {
      return true;
    }
    std::optional<ValueDefinedCostType> current_defined_cost;
    if (value_defined_cost_fn != nullptr && *value_defined_cost_fn) {
      if (!(*value_defined_cost_fn)(current->value, &current_defined_cost, error)) {
        return false;
      }
    }
    const uint32_t key_len = static_cast<uint32_t>(current->key.size());
    uint32_t paid_value_len = 0;
    if (current_defined_cost.has_value()) {
      if (current_defined_cost->kind == ValueDefinedCostType::Kind::kLayered) {
        paid_value_len = LayeredValueByteCostSizeForKeyAndValueLengths(
            key_len, current_defined_cost->cost, TreeFeatureTypeTag::kBasic);
      } else {
        paid_value_len = NodeValueByteCostSize(
            key_len, current_defined_cost->cost, TreeFeatureTypeTag::kBasic);
      }
    } else {
      paid_value_len = NodeValueByteCostSize(
          key_len, static_cast<uint32_t>(current->value.size()), TreeFeatureTypeTag::kBasic);
    }
    old_paid_values.emplace(KeyToString(current->key), paid_value_len);
    return collect_old_costs(current->left.get()) && collect_old_costs(current->right.get());
  };
  if (node != nullptr && *node) {
    if (!collect_old_costs(node->get())) {
      return false;
    }
    Node* target = FindNode(node->get(), key);
    if (target != nullptr) {
      had_existing = true;
      removed_key_len = static_cast<uint32_t>(key.size());
      removed_value_len = static_cast<uint32_t>(target->value.size());
      if (value_defined_cost_fn != nullptr && *value_defined_cost_fn) {
        if (!(*value_defined_cost_fn)(target->value, &removed_defined_cost, error)) {
          return false;
        }
      }
    }
  }
  std::unordered_set<std::string> local_dirty;
  std::unordered_set<std::string> local_deleted;
  std::unordered_set<std::string>* dirty_target = &local_dirty;
  std::unordered_set<std::string>* deleted_target = &local_deleted;
  uint64_t hash_calls = 0;
  if (!DeleteNodeWithCount(node,
                           key,
                           value_hash_fn,
                           dirty_target,
                           deleted_target,
                           deleted_flag,
                           &hash_calls,
                           error)) {
    return false;
  }
  if (dirty != nullptr) {
    dirty->insert(local_dirty.begin(), local_dirty.end());
  }
  if (deleted != nullptr) {
    deleted->insert(local_deleted.begin(), local_deleted.end());
  }
  cost->hash_node_calls = static_cast<uint32_t>(hash_calls);
  if (deleted_flag && *deleted_flag && had_existing) {
    uint32_t prefixed_removed_key_len = removed_key_len + 32;
    uint32_t paid_key_len =
        prefixed_removed_key_len + RequiredSpaceU32(prefixed_removed_key_len);
    uint32_t paid_value_len = 0;
    if (removed_defined_cost.has_value()) {
      if (removed_defined_cost->kind == ValueDefinedCostType::Kind::kLayered) {
        paid_value_len = LayeredValueByteCostSizeForKeyAndValueLengths(
            removed_key_len, removed_defined_cost->cost, TreeFeatureTypeTag::kBasic);
      } else {
        paid_value_len = NodeValueByteCostSize(
            removed_key_len, removed_defined_cost->cost, TreeFeatureTypeTag::kBasic);
      }
    } else {
      paid_value_len =
          NodeValueByteCostSize(removed_key_len, removed_value_len, TreeFeatureTypeTag::kBasic);
    }
    cost->storage_cost.removed_bytes.Add(
        StorageRemovedBytes::Basic(paid_key_len + paid_value_len));
  }
  for (const auto& entry : local_dirty) {
    std::vector<uint8_t> entry_key = StringToKey(entry);
    Node* dirty_node = FindNode(node ? node->get() : nullptr, entry_key);
    if (dirty_node == nullptr) {
      continue;
    }
    uint32_t value_len = 0;
    value_len = static_cast<uint32_t>(dirty_node->value.size());
    OperationCost::ChildrenSizesWithIsSumTree children_sizes;
    if (!BuildChildrenSizes(dirty_node, &children_sizes, error)) {
      return false;
    }
    std::optional<ValueDefinedCostType> defined_cost;
    if (value_defined_cost_fn != nullptr && *value_defined_cost_fn) {
      if (!(*value_defined_cost_fn)(dirty_node->value, &defined_cost, error)) {
        return false;
      }
    }
    uint32_t key_len = static_cast<uint32_t>(entry_key.size());
    uint32_t prefixed_key_len = key_len + 32;
    uint32_t paid_value_len = 0;
    if (defined_cost.has_value()) {
      if (defined_cost->kind == ValueDefinedCostType::Kind::kLayered) {
        paid_value_len = LayeredValueByteCostSizeForKeyAndValueLengths(
            key_len, defined_cost->cost, TreeFeatureTypeTag::kBasic);
      } else {
        paid_value_len = NodeValueByteCostSize(
            key_len, defined_cost->cost, TreeFeatureTypeTag::kBasic);
      }
    } else {
      paid_value_len = NodeValueByteCostSize(key_len, value_len, TreeFeatureTypeTag::kBasic);
    }
    KeyValueStorageCost kv_cost;
    kv_cost.new_node = false;
    kv_cost.needs_value_verification = false;
    auto old_it = old_paid_values.find(entry);
    if (old_it == old_paid_values.end()) {
      kv_cost.value_storage_cost.replaced_bytes = paid_value_len;
    } else {
      const uint32_t old_cost = old_it->second;
      if (paid_value_len == old_cost) {
        kv_cost.value_storage_cost.replaced_bytes = old_cost;
      } else if (paid_value_len < old_cost) {
        kv_cost.value_storage_cost.replaced_bytes = paid_value_len;
        kv_cost.value_storage_cost.removed_bytes =
            StorageRemovedBytes::Basic(old_cost - paid_value_len);
      } else {
        kv_cost.value_storage_cost.replaced_bytes = old_cost;
        kv_cost.value_storage_cost.added_bytes = paid_value_len - old_cost;
      }
    }
    if (!cost->AddKeyValueStorageCosts(prefixed_key_len,
                                       value_len,
                                       children_sizes,
                                       std::optional<KeyValueStorageCost>(kv_cost),
                                       error)) {
      return false;
    }
  }
  
  // Compute aggregates for dirty nodes (bottom-up order)
  // This is needed for tree kinds with aggregate metadata (SumTree, BigSumTree, CountTree, etc.)
  if (tree_feature_tag != TreeFeatureTypeTag::kBasic && !local_dirty.empty()) {
    // Collect dirty nodes and sort by height (process leaves first, then parents)
    std::vector<std::pair<int, Node*>> dirty_nodes_by_height;
    for (const auto& entry : local_dirty) {
      std::vector<uint8_t> entry_key = StringToKey(entry);
      Node* dirty_node = FindNode(node ? node->get() : nullptr, entry_key);
      if (dirty_node != nullptr) {
        dirty_nodes_by_height.emplace_back(dirty_node->height, dirty_node);
      }
    }
    // Sort by height ascending (process lower nodes first)
    std::sort(dirty_nodes_by_height.begin(), dirty_nodes_by_height.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    
    // Compute aggregates for each dirty node
    for (auto& [height, dirty_node] : dirty_nodes_by_height) {
      (void)height;
      AggregateValues left_values;
      AggregateValues right_values;
      
      // Get aggregate values from left child
      if (dirty_node->left) {
        if (!ComputeAggregateValues(dirty_node->left.get(), tree_feature_tag, &left_values, error)) {
          return false;
        }
      } else if (dirty_node->left_meta.present) {
        if (!AggregateValuesFromMeta(dirty_node->left_meta.aggregate, &left_values, error)) {
          return false;
        }
      } else {
        left_values.count = 0;
        left_values.sum = 0;
        left_values.big_sum = 0;
      }
      
      // Get aggregate values from right child
      if (dirty_node->right) {
        if (!ComputeAggregateValues(dirty_node->right.get(), tree_feature_tag, &right_values, error)) {
          return false;
        }
      } else if (dirty_node->right_meta.present) {
        if (!AggregateValuesFromMeta(dirty_node->right_meta.aggregate, &right_values, error)) {
          return false;
        }
      } else {
        right_values.count = 0;
        right_values.sum = 0;
        right_values.big_sum = 0;
      }
      
      // Compute aggregate values from children
      AggregateValues node_values;
      if (!ComputeAggregateValuesFromChildren(dirty_node,
                                              tree_feature_tag,
                                              left_values,
                                              right_values,
                                              &node_values,
                                              error)) {
        return false;
      }
      
      // Update node's aggregate metadata
      dirty_node->left_meta.aggregate = AggregateDataFromValues(tree_feature_tag, left_values);
      dirty_node->right_meta.aggregate = AggregateDataFromValues(tree_feature_tag, right_values);
      
      // Recompute node hash after updating aggregates
      if (!UpdateNodeHash(dirty_node, value_hash_fn, error)) {
        return false;
      }
    }
  }

  return true;
}

bool MerkTree::GetNode(const Node* node,
                       const std::vector<uint8_t>& key,
                       std::vector<uint8_t>* value) {
  const Node* current = node;
  while (current) {
    if (key == current->key) {
      if (value) {
        *value = current->value;
      }
      return true;
    }
    if (CompareKeys(key, current->key)) {
      current = current->left.get();
    } else {
      current = current->right.get();
    }
  }
  return false;
}

bool MerkTree::ComputeNodeHash(const Node* node,
                               const ValueHashFn& value_hash_fn,
                               std::vector<uint8_t>* out,
                               std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "hash output is null";
    }
    return false;
  }
  if (node == nullptr) {
    out->assign(32, 0);
    return true;
  }
  std::vector<uint8_t> value_hash = node->value_hash;
  if (value_hash.size() != 32) {
    if (!ComputeValueHash(node->key, node->value, value_hash_fn, &value_hash, error)) {
      return false;
    }
  }
  std::vector<uint8_t> kv_hash;
  if (!KvDigestToKvHash(node->key, value_hash, &kv_hash, error)) {
    return false;
  }
  std::vector<uint8_t> left_hash(32, 0);
  std::vector<uint8_t> right_hash(32, 0);
  if (node->left) {
    if (!ComputeNodeHash(node->left.get(), value_hash_fn, &left_hash, error)) {
      return false;
    }
  } else if (node->left_meta.present) {
    if (node->left_meta.hash.size() != 32) {
      if (error) {
        *error = "left child metadata hash length mismatch";
      }
      return false;
    }
    left_hash = node->left_meta.hash;
  }
  if (node->right) {
    if (!ComputeNodeHash(node->right.get(), value_hash_fn, &right_hash, error)) {
      return false;
    }
  } else if (node->right_meta.present) {
    if (node->right_meta.hash.size() != 32) {
      if (error) {
        *error = "right child metadata hash length mismatch";
      }
      return false;
    }
    right_hash = node->right_meta.hash;
  }
  return NodeHash(kv_hash, left_hash, right_hash, out, error);
}

bool MerkTree::ComputeNodeHashWithCount(const Node* node,
                                        const ValueHashFn& value_hash_fn,
                                        std::vector<uint8_t>* out,
                                        uint64_t* out_count,
                                        std::string* error) {
  if (out == nullptr || out_count == nullptr) {
    if (error) {
      *error = "hash/count output is null";
    }
    return false;
  }
  if (node == nullptr) {
    out->assign(32, 0);
    *out_count = 0;
    return true;
  }
  std::vector<uint8_t> value_hash = node->value_hash;
  if (value_hash.size() != 32) {
    if (!ComputeValueHash(node->key, node->value, value_hash_fn, &value_hash, error)) {
      return false;
    }
  }
  std::vector<uint8_t> kv_hash;
  if (!KvDigestToKvHash(node->key, value_hash, &kv_hash, error)) {
    return false;
  }
  std::vector<uint8_t> left_hash(32, 0);
  std::vector<uint8_t> right_hash(32, 0);
  uint64_t left_count = 0;
  uint64_t right_count = 0;
  if (node->left) {
    if (!ComputeNodeHashWithCount(node->left.get(),
                                  value_hash_fn,
                                  &left_hash,
                                  &left_count,
                                  error)) {
      return false;
    }
  } else if (node->left_meta.present) {
    if (node->left_meta.hash.size() != 32) {
      if (error) {
        *error = "left child metadata hash length mismatch";
      }
      return false;
    }
    left_hash = node->left_meta.hash;
    bool has_count = false;
    if (!AggregateCount(node->left_meta.aggregate, &left_count, &has_count)) {
      if (error) {
        *error = "left child metadata count parse failed";
      }
      return false;
    }
    if (!has_count) {
      if (error) {
        *error = "left child metadata missing count";
      }
      return false;
    }
  }
  if (node->right) {
    if (!ComputeNodeHashWithCount(node->right.get(),
                                  value_hash_fn,
                                  &right_hash,
                                  &right_count,
                                  error)) {
      return false;
    }
  } else if (node->right_meta.present) {
    if (node->right_meta.hash.size() != 32) {
      if (error) {
        *error = "right child metadata hash length mismatch";
      }
      return false;
    }
    right_hash = node->right_meta.hash;
    bool has_count = false;
    if (!AggregateCount(node->right_meta.aggregate, &right_count, &has_count)) {
      if (error) {
        *error = "right child metadata count parse failed";
      }
      return false;
    }
    if (!has_count) {
      if (error) {
        *error = "right child metadata missing count";
      }
      return false;
    }
  }
  uint64_t count = 1 + left_count + right_count;
  if (!NodeHashWithCount(kv_hash, left_hash, right_hash, count, out, error)) {
    return false;
  }
  *out_count = count;
  return true;
}

bool MerkTree::ComputeNodeCount(const Node* node, uint64_t* out_count, std::string* error) {
  if (out_count == nullptr) {
    if (error) {
      *error = "count output is null";
    }
    return false;
  }
  if (node == nullptr) {
    *out_count = 0;
    return true;
  }
  uint64_t left_count = 0;
  uint64_t right_count = 0;
  if (node->left) {
    if (!ComputeNodeCount(node->left.get(), &left_count, error)) {
      return false;
    }
    if (node->left_meta.present) {
      bool has_count = false;
      uint64_t meta_count = 0;
      if (!AggregateCount(node->left_meta.aggregate, &meta_count, &has_count)) {
        if (error) {
          *error = "left child metadata count parse failed";
        }
        return false;
      }
      if (has_count && meta_count != left_count) {
        if (error) {
          *error = "left child metadata count mismatch";
        }
        return false;
      }
    }
  } else if (node->left_meta.present) {
    bool has_count = false;
    if (!AggregateCount(node->left_meta.aggregate, &left_count, &has_count)) {
      if (error) {
        *error = "left child metadata count parse failed";
      }
      return false;
    }
    if (!has_count) {
      if (error) {
        *error = "left child metadata missing count";
      }
      return false;
    }
  }
  if (node->right) {
    if (!ComputeNodeCount(node->right.get(), &right_count, error)) {
      return false;
    }
    if (node->right_meta.present) {
      bool has_count = false;
      uint64_t meta_count = 0;
      if (!AggregateCount(node->right_meta.aggregate, &meta_count, &has_count)) {
        if (error) {
          *error = "right child metadata count parse failed";
        }
        return false;
      }
      if (has_count && meta_count != right_count) {
        if (error) {
          *error = "right child metadata count mismatch";
        }
        return false;
      }
    }
  } else if (node->right_meta.present) {
    bool has_count = false;
    if (!AggregateCount(node->right_meta.aggregate, &right_count, &has_count)) {
      if (error) {
        *error = "right child metadata count parse failed";
      }
      return false;
    }
    if (!has_count) {
      if (error) {
        *error = "right child metadata missing count";
      }
      return false;
    }
  }
  *out_count = 1 + left_count + right_count;
  return true;
}

bool MerkTree::ComputeNodeSum(const Node* node,
                              const SumValueFn& sum_fn,
                              int64_t* out_sum,
                              std::string* error) {
  if (out_sum == nullptr) {
    if (error) {
      *error = "sum output is null";
    }
    return false;
  }
  if (node == nullptr) {
    *out_sum = 0;
    return true;
  }
  int64_t left_sum = 0;
  int64_t right_sum = 0;
  if (node->left) {
    if (!ComputeNodeSum(node->left.get(), sum_fn, &left_sum, error)) {
      return false;
    }
    if (node->left_meta.present) {
      bool has_sum = false;
      int64_t meta_sum = 0;
      if (!AggregateSumI64(node->left_meta.aggregate, &meta_sum, &has_sum, error)) {
        return false;
      }
      if (has_sum && meta_sum != left_sum) {
        if (error) {
          *error = "left child metadata sum mismatch";
        }
        return false;
      }
    }
  } else if (node->left_meta.present) {
    bool has_sum = false;
    if (!AggregateSumI64(node->left_meta.aggregate, &left_sum, &has_sum, error)) {
      return false;
    }
    if (!has_sum) {
      if (error) {
        *error = "left child metadata missing sum";
      }
      return false;
    }
  }
  if (node->right) {
    if (!ComputeNodeSum(node->right.get(), sum_fn, &right_sum, error)) {
      return false;
    }
    if (node->right_meta.present) {
      bool has_sum = false;
      int64_t meta_sum = 0;
      if (!AggregateSumI64(node->right_meta.aggregate, &meta_sum, &has_sum, error)) {
        return false;
      }
      if (has_sum && meta_sum != right_sum) {
        if (error) {
          *error = "right child metadata sum mismatch";
        }
        return false;
      }
    }
  } else if (node->right_meta.present) {
    bool has_sum = false;
    if (!AggregateSumI64(node->right_meta.aggregate, &right_sum, &has_sum, error)) {
      return false;
    }
    if (!has_sum) {
      if (error) {
        *error = "right child metadata missing sum";
      }
      return false;
    }
  }
  int64_t node_sum = 0;
  bool has_sum = false;
  if (!sum_fn(node->value, &node_sum, &has_sum, error)) {
    return false;
  }
  *out_sum = left_sum + right_sum + (has_sum ? node_sum : 0);
  return true;
}

bool MerkTree::ComputeNodeSumBig(const Node* node,
                                 const SumValueFn& sum_fn,
                                 __int128* out_sum,
                                 std::string* error) {
  if (out_sum == nullptr) {
    if (error) {
      *error = "sum output is null";
    }
    return false;
  }
  if (node == nullptr) {
    *out_sum = 0;
    return true;
  }
  __int128 left_sum = 0;
  __int128 right_sum = 0;
  if (node->left) {
    if (!ComputeNodeSumBig(node->left.get(), sum_fn, &left_sum, error)) {
      return false;
    }
    if (node->left_meta.present) {
      bool has_sum = false;
      __int128 meta_sum = 0;
      if (!AggregateSumI128(node->left_meta.aggregate, &meta_sum, &has_sum)) {
        if (error) {
          *error = "left child metadata sum parse failed";
        }
        return false;
      }
      if (has_sum && meta_sum != left_sum) {
        if (error) {
          *error = "left child metadata sum mismatch";
        }
        return false;
      }
    }
  } else if (node->left_meta.present) {
    bool has_sum = false;
    if (!AggregateSumI128(node->left_meta.aggregate, &left_sum, &has_sum)) {
      if (error) {
        *error = "left child metadata sum parse failed";
      }
      return false;
    }
    if (!has_sum) {
      if (error) {
        *error = "left child metadata missing sum";
      }
      return false;
    }
  }
  if (node->right) {
    if (!ComputeNodeSumBig(node->right.get(), sum_fn, &right_sum, error)) {
      return false;
    }
    if (node->right_meta.present) {
      bool has_sum = false;
      __int128 meta_sum = 0;
      if (!AggregateSumI128(node->right_meta.aggregate, &meta_sum, &has_sum)) {
        if (error) {
          *error = "right child metadata sum parse failed";
        }
        return false;
      }
      if (has_sum && meta_sum != right_sum) {
        if (error) {
          *error = "right child metadata sum mismatch";
        }
        return false;
      }
    }
  } else if (node->right_meta.present) {
    bool has_sum = false;
    if (!AggregateSumI128(node->right_meta.aggregate, &right_sum, &has_sum)) {
      if (error) {
        *error = "right child metadata sum parse failed";
      }
      return false;
    }
    if (!has_sum) {
      if (error) {
        *error = "right child metadata missing sum";
      }
      return false;
    }
  }
  int64_t node_sum = 0;
  bool has_sum = false;
  if (!sum_fn(node->value, &node_sum, &has_sum, error)) {
    return false;
  }
  *out_sum = left_sum + right_sum + (has_sum ? static_cast<__int128>(node_sum) : 0);
  return true;
}

bool MerkTree::ComputeNodeCountAndSum(const Node* node,
                                      const SumValueFn& sum_fn,
                                      uint64_t* out_count,
                                      int64_t* out_sum,
                                      std::string* error) {
  if (out_count == nullptr || out_sum == nullptr) {
    if (error) {
      *error = "count/sum output is null";
    }
    return false;
  }
  if (node == nullptr) {
    *out_count = 0;
    *out_sum = 0;
    return true;
  }
  uint64_t left_count = 0;
  uint64_t right_count = 0;
  int64_t left_sum = 0;
  int64_t right_sum = 0;
  if (node->left) {
    if (!ComputeNodeCountAndSum(node->left.get(),
                                sum_fn,
                                &left_count,
                                &left_sum,
                                error)) {
      return false;
    }
    if (node->left_meta.present) {
      bool has_count = false;
      uint64_t meta_count = 0;
      if (!AggregateCount(node->left_meta.aggregate, &meta_count, &has_count)) {
        if (error) {
          *error = "left child metadata count parse failed";
        }
        return false;
      }
      if (has_count && meta_count != left_count) {
        if (error) {
          *error = "left child metadata count mismatch";
        }
        return false;
      }
      bool has_sum = false;
      int64_t meta_sum = 0;
      if (!AggregateSumI64(node->left_meta.aggregate, &meta_sum, &has_sum, error)) {
        return false;
      }
      if (has_sum && meta_sum != left_sum) {
        if (error) {
          *error = "left child metadata sum mismatch";
        }
        return false;
      }
    }
  } else if (node->left_meta.present) {
    bool has_count = false;
    if (!AggregateCount(node->left_meta.aggregate, &left_count, &has_count)) {
      if (error) {
        *error = "left child metadata count parse failed";
      }
      return false;
    }
    if (!has_count) {
      if (error) {
        *error = "left child metadata missing count";
      }
      return false;
    }
    bool has_sum = false;
    if (!AggregateSumI64(node->left_meta.aggregate, &left_sum, &has_sum, error)) {
      return false;
    }
    if (!has_sum) {
      if (error) {
        *error = "left child metadata missing sum";
      }
      return false;
    }
  }
  if (node->right) {
    if (!ComputeNodeCountAndSum(node->right.get(),
                                sum_fn,
                                &right_count,
                                &right_sum,
                                error)) {
      return false;
    }
    if (node->right_meta.present) {
      bool has_count = false;
      uint64_t meta_count = 0;
      if (!AggregateCount(node->right_meta.aggregate, &meta_count, &has_count)) {
        if (error) {
          *error = "right child metadata count parse failed";
        }
        return false;
      }
      if (has_count && meta_count != right_count) {
        if (error) {
          *error = "right child metadata count mismatch";
        }
        return false;
      }
      bool has_sum = false;
      int64_t meta_sum = 0;
      if (!AggregateSumI64(node->right_meta.aggregate, &meta_sum, &has_sum, error)) {
        return false;
      }
      if (has_sum && meta_sum != right_sum) {
        if (error) {
          *error = "right child metadata sum mismatch";
        }
        return false;
      }
    }
  } else if (node->right_meta.present) {
    bool has_count = false;
    if (!AggregateCount(node->right_meta.aggregate, &right_count, &has_count)) {
      if (error) {
        *error = "right child metadata count parse failed";
      }
      return false;
    }
    if (!has_count) {
      if (error) {
        *error = "right child metadata missing count";
      }
      return false;
    }
    bool has_sum = false;
    if (!AggregateSumI64(node->right_meta.aggregate, &right_sum, &has_sum, error)) {
      return false;
    }
    if (!has_sum) {
      if (error) {
        *error = "right child metadata missing sum";
      }
      return false;
    }
  }
  int64_t node_sum = 0;
  bool has_sum = false;
  if (!sum_fn(node->value, &node_sum, &has_sum, error)) {
    return false;
  }
  *out_count = 1 + left_count + right_count;
  *out_sum = left_sum + right_sum + (has_sum ? node_sum : 0);
  return true;
}

bool MerkTree::ComputeNodeCountAndSumBig(const Node* node,
                                         const SumValueFn& sum_fn,
                                         uint64_t* out_count,
                                         __int128* out_sum,
                                         std::string* error) {
  if (out_count == nullptr || out_sum == nullptr) {
    if (error) {
      *error = "count/sum output is null";
    }
    return false;
  }
  if (node == nullptr) {
    *out_count = 0;
    *out_sum = 0;
    return true;
  }
  uint64_t left_count = 0;
  uint64_t right_count = 0;
  __int128 left_sum = 0;
  __int128 right_sum = 0;
  if (node->left) {
    if (!ComputeNodeCountAndSumBig(node->left.get(),
                                   sum_fn,
                                   &left_count,
                                   &left_sum,
                                   error)) {
      return false;
    }
    if (node->left_meta.present) {
      bool has_count = false;
      uint64_t meta_count = 0;
      if (!AggregateCount(node->left_meta.aggregate, &meta_count, &has_count)) {
        if (error) {
          *error = "left child metadata count parse failed";
        }
        return false;
      }
      if (has_count && meta_count != left_count) {
        if (error) {
          *error = "left child metadata count mismatch";
        }
        return false;
      }
      bool has_sum = false;
      __int128 meta_sum = 0;
      if (!AggregateSumI128(node->left_meta.aggregate, &meta_sum, &has_sum)) {
        if (error) {
          *error = "left child metadata sum parse failed";
        }
        return false;
      }
      if (has_sum && meta_sum != left_sum) {
        if (error) {
          *error = "left child metadata sum mismatch";
        }
        return false;
      }
    }
  } else if (node->left_meta.present) {
    bool has_count = false;
    if (!AggregateCount(node->left_meta.aggregate, &left_count, &has_count)) {
      if (error) {
        *error = "left child metadata count parse failed";
      }
      return false;
    }
    if (!has_count) {
      if (error) {
        *error = "left child metadata missing count";
      }
      return false;
    }
    bool has_sum = false;
    if (!AggregateSumI128(node->left_meta.aggregate, &left_sum, &has_sum)) {
      if (error) {
        *error = "left child metadata sum parse failed";
      }
      return false;
    }
    if (!has_sum) {
      if (error) {
        *error = "left child metadata missing sum";
      }
      return false;
    }
  }
  if (node->right) {
    if (!ComputeNodeCountAndSumBig(node->right.get(),
                                   sum_fn,
                                   &right_count,
                                   &right_sum,
                                   error)) {
      return false;
    }
    if (node->right_meta.present) {
      bool has_count = false;
      uint64_t meta_count = 0;
      if (!AggregateCount(node->right_meta.aggregate, &meta_count, &has_count)) {
        if (error) {
          *error = "right child metadata count parse failed";
        }
        return false;
      }
      if (has_count && meta_count != right_count) {
        if (error) {
          *error = "right child metadata count mismatch";
        }
        return false;
      }
      bool has_sum = false;
      __int128 meta_sum = 0;
      if (!AggregateSumI128(node->right_meta.aggregate, &meta_sum, &has_sum)) {
        if (error) {
          *error = "right child metadata sum parse failed";
        }
        return false;
      }
      if (has_sum && meta_sum != right_sum) {
        if (error) {
          *error = "right child metadata sum mismatch";
        }
        return false;
      }
    }
  } else if (node->right_meta.present) {
    bool has_count = false;
    if (!AggregateCount(node->right_meta.aggregate, &right_count, &has_count)) {
      if (error) {
        *error = "right child metadata count parse failed";
      }
      return false;
    }
    if (!has_count) {
      if (error) {
        *error = "right child metadata missing count";
      }
      return false;
    }
    bool has_sum = false;
    if (!AggregateSumI128(node->right_meta.aggregate, &right_sum, &has_sum)) {
      if (error) {
        *error = "right child metadata sum parse failed";
      }
      return false;
    }
    if (!has_sum) {
      if (error) {
        *error = "right child metadata missing sum";
      }
      return false;
    }
  }
  int64_t node_sum = 0;
  bool has_sum = false;
  if (!sum_fn(node->value, &node_sum, &has_sum, error)) {
    return false;
  }
  *out_count = 1 + left_count + right_count;
  *out_sum = left_sum + right_sum + (has_sum ? static_cast<__int128>(node_sum) : 0);
  return true;
}

bool MerkTree::ValidateNode(const Node* node,
                            const std::vector<uint8_t>* min_key,
                            const std::vector<uint8_t>* max_key,
                            int* out_height,
                            std::string* error) {
  if (out_height == nullptr) {
    if (error) {
      *error = "height output is null";
    }
    return false;
  }
  if (node == nullptr) {
    *out_height = 0;
    return true;
  }
  if (min_key && !CompareKeys(*min_key, node->key)) {
    if (error) {
      *error = "node key not greater than min bound";
    }
    return false;
  }
  if (max_key && !CompareKeys(node->key, *max_key)) {
    if (error) {
      *error = "node key not less than max bound";
    }
    return false;
  }
  int left_height = 0;
  int right_height = 0;
  if (node->left) {
    if (!ValidateNode(node->left.get(), min_key, &node->key, &left_height, error)) {
      return false;
    }
    if (node->left_meta.present) {
      int meta_height = ChildHeightFromMeta(node->left_meta);
      if (meta_height != left_height) {
        if (error) {
          *error = "left child metadata height mismatch";
        }
        return false;
      }
    }
  } else if (node->left_meta.present) {
    left_height = ChildHeightFromMeta(node->left_meta);
  }
  if (node->right) {
    if (!ValidateNode(node->right.get(), &node->key, max_key, &right_height, error)) {
      return false;
    }
    if (node->right_meta.present) {
      int meta_height = ChildHeightFromMeta(node->right_meta);
      if (meta_height != right_height) {
        if (error) {
          *error = "right child metadata height mismatch";
        }
        return false;
      }
    }
  } else if (node->right_meta.present) {
    right_height = ChildHeightFromMeta(node->right_meta);
  }
  int expected_height = 1 + (left_height > right_height ? left_height : right_height);
  if (node->height != expected_height) {
    if (error) {
      *error = "node height mismatch";
    }
    return false;
  }
  int balance = left_height - right_height;
  if (balance > 1 || balance < -1) {
    if (error) {
      *error = "node balance out of range";
    }
    return false;
  }
  *out_height = expected_height;
  return true;
}

bool MerkTree::EmitProofOps(Node* node,
                            const std::vector<uint8_t>& target_key,
                            TargetEncoding target_encoding,
                            ProofMode mode,
                            const ValueHashFn& value_hash_fn,
                            std::vector<ProofOp>* ops,
                            std::string* error) const {
  if (mode != ProofMode::kTargetValue) {
    if (error) {
      *error = "unexpected proof mode for present proof";
    }
    return false;
  }
  return EmitProofOpsForPresent(node, target_key, target_encoding, value_hash_fn, ops, error);
}

bool MerkTree::EmitProofOpsForPresent(Node* node,
                                      const std::vector<uint8_t>& target_key,
                                      TargetEncoding target_encoding,
                                      const ValueHashFn& value_hash_fn,
                                      std::vector<ProofOp>* ops,
                                      std::string* error) const {
  if (ops == nullptr) {
    if (error) {
      *error = "proof ops output is null";
    }
    return false;
  }
  if (node == nullptr) {
    return true;
  }
  if (CompareKeys(target_key, node->key)) {
    if (!EnsureChildLoaded(node, true, error)) {
      return false;
    }
    if (!EmitProofOpsForPresent(node->left.get(),
                                target_key,
                                target_encoding,
                                value_hash_fn,
                                ops,
                                error)) {
      return false;
    }
    if (!PushKvHashOp(node, value_hash_fn, ops, error)) {
      return false;
    }
    if (HasChild(node, true)) {
      ops->push_back(ProofOp{ProofOp::Type::kParent, {}, {}, {}, 0, false});
    }
    if (HasChild(node, false)) {
      if (!PushHashOpForChild(node, false, value_hash_fn, ops, error)) {
        return false;
      }
      ops->push_back(ProofOp{ProofOp::Type::kChild, {}, {}, {}, 0, false});
    }
    return true;
  }
  if (CompareKeys(node->key, target_key)) {
    if (HasChild(node, true)) {
      if (!PushHashOpForChild(node, true, value_hash_fn, ops, error)) {
        return false;
      }
    }
    if (!PushKvHashOp(node, value_hash_fn, ops, error)) {
      return false;
    }
    if (HasChild(node, true)) {
      ops->push_back(ProofOp{ProofOp::Type::kParent, {}, {}, {}, 0, false});
    }
    if (!EnsureChildLoaded(node, false, error)) {
      return false;
    }
    if (!EmitProofOpsForPresent(node->right.get(),
                                target_key,
                                target_encoding,
                                value_hash_fn,
                                ops,
                                error)) {
      return false;
    }
    if (HasChild(node, false)) {
      ops->push_back(ProofOp{ProofOp::Type::kChild, {}, {}, {}, 0, false});
    }
    return true;
  }

  if (HasChild(node, true)) {
    if (!PushHashOpForChild(node, true, value_hash_fn, ops, error)) {
      return false;
    }
  }
  uint64_t element_variant = 0;
  const bool decoded_variant = DecodeElementVariant(node->value, &element_variant, nullptr);
  const bool is_tree_element =
      decoded_variant && (element_variant == 2 || element_variant == 4 ||
                          element_variant == 5 || element_variant == 6 ||
                          element_variant == 7 || element_variant == 8 ||
                          element_variant == 10);
  if (is_tree_element) {
    if (!PushKvDigestOp(node, value_hash_fn, ops, error)) {
      return false;
    }
  } else {
    ProofOp op;
    if (target_encoding == TargetEncoding::kKvValueHash) {
      (void)value_hash_fn;
      op.type = ProofOp::Type::kPushKvValueHash;
      op.key = node->key;
      op.value = node->value;
      op.hash = node->value_hash;
    } else {
      op.type = ProofOp::Type::kPushKv;
      op.key = node->key;
      op.value = node->value;
    }
    ops->push_back(std::move(op));
  }
  if (HasChild(node, true)) {
    ops->push_back(ProofOp{ProofOp::Type::kParent, {}, {}, {}, 0, false});
  }
  if (HasChild(node, false)) {
    if (!PushHashOpForChild(node, false, value_hash_fn, ops, error)) {
      return false;
    }
    ops->push_back(ProofOp{ProofOp::Type::kChild, {}, {}, {}, 0, false});
  }
  return true;
}

bool MerkTree::EmitProofOpsForPresentWithCount(Node* node,
                                               const std::vector<uint8_t>& target_key,
                                               TargetEncoding target_encoding,
                                               const ValueHashFn& value_hash_fn,
                                               std::vector<ProofOp>* ops,
                                               std::string* error) const {
  if (ops == nullptr) {
    if (error) {
      *error = "proof ops output is null";
    }
    return false;
  }
  if (node == nullptr) {
    return true;
  }
  if (CompareKeys(target_key, node->key)) {
    if (!EnsureChildLoaded(node, true, error)) {
      return false;
    }
    if (!EmitProofOpsForPresentWithCount(node->left.get(),
                                         target_key,
                                         target_encoding,
                                         value_hash_fn,
                                         ops,
                                         error)) {
      return false;
    }
    if (!PushKvHashCountOp(node, value_hash_fn, ops, error)) {
      return false;
    }
    if (HasChild(node, true)) {
      ops->push_back(ProofOp{ProofOp::Type::kParent, {}, {}, {}, 0, false});
    }
    if (HasChild(node, false)) {
      if (!PushHashOpWithCountForChild(node, false, value_hash_fn, ops, error)) {
        return false;
      }
      ops->push_back(ProofOp{ProofOp::Type::kChild, {}, {}, {}, 0, false});
    }
    return true;
  }
  if (CompareKeys(node->key, target_key)) {
    if (HasChild(node, true)) {
      if (!PushHashOpWithCountForChild(node, true, value_hash_fn, ops, error)) {
        return false;
      }
    }
    if (!PushKvHashCountOp(node, value_hash_fn, ops, error)) {
      return false;
    }
    if (HasChild(node, true)) {
      ops->push_back(ProofOp{ProofOp::Type::kParent, {}, {}, {}, 0, false});
    }
    if (!EnsureChildLoaded(node, false, error)) {
      return false;
    }
    if (!EmitProofOpsForPresentWithCount(node->right.get(),
                                         target_key,
                                         target_encoding,
                                         value_hash_fn,
                                         ops,
                                         error)) {
      return false;
    }
    if (HasChild(node, false)) {
      ops->push_back(ProofOp{ProofOp::Type::kChild, {}, {}, {}, 0, false});
    }
    return true;
  }

  if (HasChild(node, true)) {
    if (!PushHashOpWithCountForChild(node, true, value_hash_fn, ops, error)) {
      return false;
    }
  }
  ProofOp op;
  if (target_encoding != TargetEncoding::kKv) {
    if (error) {
      *error = "provable count proofs require kv target encoding";
    }
    return false;
  }
  uint64_t count = 0;
  if (!ComputeNodeCount(node, &count, error)) {
    return false;
  }
  op.type = ProofOp::Type::kPushKvCount;
  op.key = node->key;
  op.value = node->value;
  op.count = count;
  ops->push_back(std::move(op));
  if (HasChild(node, true)) {
    ops->push_back(ProofOp{ProofOp::Type::kParent, {}, {}, {}, 0, false});
  }
  if (HasChild(node, false)) {
    if (!PushHashOpWithCountForChild(node, false, value_hash_fn, ops, error)) {
      return false;
    }
    ops->push_back(ProofOp{ProofOp::Type::kChild, {}, {}, {}, 0, false});
  }
  return true;
}

bool MerkTree::EmitProofOpsForRange(Node* node,
                                    const std::vector<uint8_t>& start_key,
                                    const std::vector<uint8_t>& end_key,
                                    bool start_inclusive,
                                    bool end_inclusive,
                                    TargetEncoding target_encoding,
                                    const ValueHashFn& value_hash_fn,
                                    std::vector<ProofOp>* ops,
                                    std::string* error) const {
  bool has_match = false;
  bool left_absence = false;
  bool right_absence = false;
  return EmitProofOpsForRange(node,
                              start_key,
                              end_key,
                              start_inclusive,
                              end_inclusive,
                              target_encoding,
                              value_hash_fn,
                              ops,
                              &has_match,
                              &left_absence,
                              &right_absence,
                              error);
}

bool MerkTree::EmitProofOpsForRange(Node* node,
                                    const std::vector<uint8_t>& start_key,
                                    const std::vector<uint8_t>& end_key,
                                    bool start_inclusive,
                                    bool end_inclusive,
                                    TargetEncoding target_encoding,
                                    const ValueHashFn& value_hash_fn,
                                    std::vector<ProofOp>* ops,
                                    bool* out_has_match,
                                    bool* out_left_absence,
                                    bool* out_right_absence,
                                    std::string* error) const {
  if (ops == nullptr) {
    if (error) {
      *error = "proof ops output is null";
    }
    return false;
  }
  if (out_has_match == nullptr || out_left_absence == nullptr || out_right_absence == nullptr) {
    if (error) {
      *error = "range proof state output is null";
    }
    return false;
  }
  if (node == nullptr) {
    *out_has_match = false;
    *out_left_absence = true;
    *out_right_absence = true;
    return true;
  }

  int cmp_start = CompareKeyOrder(node->key, start_key);
  int cmp_end = CompareKeyOrder(node->key, end_key);

  if (cmp_start < 0 || (cmp_start == 0 && !start_inclusive)) {
    std::vector<ProofOp> right_ops;
    bool right_has_match = false;
    bool right_left_absence = false;
    bool right_right_absence = false;
    if (!EnsureChildLoaded(node, false, error)) {
      return false;
    }
    if (!EmitProofOpsForRange(node->right.get(),
                              start_key,
                              end_key,
                              start_inclusive,
                              end_inclusive,
                              target_encoding,
                              value_hash_fn,
                              &right_ops,
                              &right_has_match,
                              &right_left_absence,
                              &right_right_absence,
                              error)) {
      return false;
    }
    if (HasChild(node, true)) {
      if (!PushHashOpForChild(node, true, value_hash_fn, ops, error)) {
        return false;
      }
    }
    if ((cmp_start == 0 && !start_inclusive) || right_left_absence) {
      if (!PushKvDigestOp(node, value_hash_fn, ops, error)) {
        return false;
      }
    } else {
      if (!PushKvHashOp(node, value_hash_fn, ops, error)) {
        return false;
      }
    }
    if (HasChild(node, true)) {
      ops->push_back(ProofOp{ProofOp::Type::kParent, {}, {}, {}, 0, false});
    }
    ops->insert(ops->end(), right_ops.begin(), right_ops.end());
    if (HasChild(node, false)) {
      ops->push_back(ProofOp{ProofOp::Type::kChild, {}, {}, {}, 0, false});
    }
    *out_has_match = right_has_match;
    *out_left_absence = false;
    *out_right_absence = right_right_absence;
    return true;
  }

  if (cmp_end > 0 || (cmp_end == 0 && !end_inclusive)) {
    std::vector<ProofOp> left_ops;
    bool left_has_match = false;
    bool left_left_absence = false;
    bool left_right_absence = false;
    if (!EnsureChildLoaded(node, true, error)) {
      return false;
    }
    if (!EmitProofOpsForRange(node->left.get(),
                              start_key,
                              end_key,
                              start_inclusive,
                              end_inclusive,
                              target_encoding,
                              value_hash_fn,
                              &left_ops,
                              &left_has_match,
                              &left_left_absence,
                              &left_right_absence,
                              error)) {
      return false;
    }
    ops->insert(ops->end(), left_ops.begin(), left_ops.end());
    if ((cmp_end == 0 && !end_inclusive) || left_right_absence) {
      if (!PushKvDigestOp(node, value_hash_fn, ops, error)) {
        return false;
      }
    } else {
      if (!PushKvHashOp(node, value_hash_fn, ops, error)) {
        return false;
      }
    }
    if (HasChild(node, true)) {
      ops->push_back(ProofOp{ProofOp::Type::kParent, {}, {}, {}, 0, false});
    }
    if (HasChild(node, false)) {
      if (!PushHashOpForChild(node, false, value_hash_fn, ops, error)) {
        return false;
      }
      ops->push_back(ProofOp{ProofOp::Type::kChild, {}, {}, {}, 0, false});
    }
    *out_has_match = left_has_match;
    *out_left_absence = left_left_absence;
    *out_right_absence = false;
    return true;
  }

  bool left_needed = CompareKeyOrder(start_key, node->key) < 0;
  bool right_needed = CompareKeyOrder(node->key, end_key) < 0;
  bool left_has_match = false;
  bool right_has_match = false;
  bool left_left_absence = false;
  bool left_right_absence = false;
  bool right_left_absence = false;
  bool right_right_absence = false;

  if (left_needed) {
    if (!EnsureChildLoaded(node, true, error)) {
      return false;
    }
    if (!EmitProofOpsForRange(node->left.get(),
                              start_key,
                              end_key,
                              start_inclusive,
                              end_inclusive,
                              target_encoding,
                              value_hash_fn,
                              ops,
                              &left_has_match,
                              &left_left_absence,
                              &left_right_absence,
                              error)) {
      return false;
    }
  } else if (HasChild(node, true)) {
    if (!PushHashOpForChild(node, true, value_hash_fn, ops, error)) {
      return false;
    }
  }

  ProofOp op;
  if (target_encoding == TargetEncoding::kKvValueHash) {
    (void)value_hash_fn;
    op.type = ProofOp::Type::kPushKvValueHash;
    op.key = node->key;
    op.value = node->value;
    op.hash = node->value_hash;
  } else {
    op.type = ProofOp::Type::kPushKv;
    op.key = node->key;
    op.value = node->value;
  }
  ops->push_back(std::move(op));
  if (HasChild(node, true)) {
    ops->push_back(ProofOp{ProofOp::Type::kParent, {}, {}, {}, 0, false});
  }

  if (right_needed) {
    if (!EnsureChildLoaded(node, false, error)) {
      return false;
    }
    if (!EmitProofOpsForRange(node->right.get(),
                              start_key,
                              end_key,
                              start_inclusive,
                              end_inclusive,
                              target_encoding,
                              value_hash_fn,
                              ops,
                              &right_has_match,
                              &right_left_absence,
                              &right_right_absence,
                              error)) {
      return false;
    }
  } else if (HasChild(node, false)) {
    if (!PushHashOpForChild(node, false, value_hash_fn, ops, error)) {
      return false;
    }
  }
  if (HasChild(node, false)) {
    ops->push_back(ProofOp{ProofOp::Type::kChild, {}, {}, {}, 0, false});
  }
  *out_has_match = true;
  *out_left_absence = left_left_absence;
  *out_right_absence = right_right_absence;
  return true;
}

bool MerkTree::EmitProofOpsForExtreme(Node* node,
                                      bool max_key,
                                      const ValueHashFn& value_hash_fn,
                                      std::vector<ProofOp>* ops,
                                      std::string* error) const {
  if (ops == nullptr) {
    if (error) {
      *error = "proof ops output is null";
    }
    return false;
  }
  if (node == nullptr) {
    return true;
  }
  const bool recurse_side = max_key ? false : true;    // right for max, left for min
  const bool opposite_side = !recurse_side;
  const bool has_recurse = HasChild(node, recurse_side);
  const bool has_opposite = HasChild(node, opposite_side);
  auto push_node = [&]() -> bool {
    if (has_recurse) {
      return PushKvHashOp(node, value_hash_fn, ops, error);
    }
    return PushKvDigestOp(node, value_hash_fn, ops, error);
  };
  auto recurse = [&]() -> bool {
    if (!has_recurse) {
      return true;
    }
    if (!EnsureChildLoaded(node, recurse_side, error)) {
      return false;
    }
    return EmitProofOpsForExtreme(recurse_side ? node->left.get() : node->right.get(),
                                  max_key,
                                  value_hash_fn,
                                  ops,
                                  error);
  };

  if (recurse_side) {  // min path, recurse left first
    if (!recurse()) {
      return false;
    }
    if (!push_node()) {
      return false;
    }
    if (has_recurse) {
      ops->push_back(ProofOp{ProofOp::Type::kParent, {}, {}, {}, 0, false});
    }
    if (has_opposite) {
      if (!PushHashOpForChild(node, opposite_side, value_hash_fn, ops, error)) {
        return false;
      }
      ops->push_back(ProofOp{ProofOp::Type::kChild, {}, {}, {}, 0, false});
    }
  } else {  // max path, hash left first then recurse right
    if (has_opposite) {
      if (!PushHashOpForChild(node, opposite_side, value_hash_fn, ops, error)) {
        return false;
      }
    }
    if (!push_node()) {
      return false;
    }
    if (has_opposite) {
      ops->push_back(ProofOp{ProofOp::Type::kParent, {}, {}, {}, 0, false});
    }
    if (!recurse()) {
      return false;
    }
    if (has_recurse) {
      ops->push_back(ProofOp{ProofOp::Type::kChild, {}, {}, {}, 0, false});
    }
  }
  return true;
}

bool MerkTree::EmitProofOpsForRangeWithLimit(Node* node,
                                             const std::vector<uint8_t>& start_key,
                                             const std::vector<uint8_t>& end_key,
                                             bool start_inclusive,
                                             bool end_inclusive,
                                             size_t* remaining_limit,
                                             const ValueHashFn& value_hash_fn,
                                             std::vector<ProofOp>* ops,
                                             std::string* error) const {
  if (ops == nullptr || remaining_limit == nullptr) {
    if (error) {
      *error = "proof ops output or limit is null";
    }
    return false;
  }
  if (node == nullptr) {
    return true;
  }

  // Once the query limit is exhausted, Rust query proof generation clears
  // the remaining proof items and emits only hashes for the untouched suffix.
  if (*remaining_limit == 0) {
    return PushHashOp(node, value_hash_fn, ops, error);
  }

  int cmp_start = CompareKeyOrder(node->key, start_key);
  int cmp_end = CompareKeyOrder(node->key, end_key);

  if (cmp_start < 0 || (cmp_start == 0 && !start_inclusive)) {
    if (HasChild(node, true)) {
      if (!PushHashOpForChild(node, true, value_hash_fn, ops, error)) {
        return false;
      }
    }
    if (cmp_start == 0 && !start_inclusive) {
      if (!PushKvDigestOp(node, value_hash_fn, ops, error)) {
        return false;
      }
    } else {
      if (!PushKvHashOp(node, value_hash_fn, ops, error)) {
        return false;
      }
    }
    if (HasChild(node, true)) {
      ops->push_back(ProofOp{ProofOp::Type::kParent, {}, {}, {}, 0, false});
    }
    if (!EnsureChildLoaded(node, false, error)) {
      return false;
    }
    if (!EmitProofOpsForRangeWithLimit(node->right.get(),
                                       start_key,
                                       end_key,
                                       start_inclusive,
                                       end_inclusive,
                                       remaining_limit,
                                       value_hash_fn,
                                       ops,
                                       error)) {
      return false;
    }
    if (HasChild(node, false)) {
      ops->push_back(ProofOp{ProofOp::Type::kChild, {}, {}, {}, 0, false});
    }
    return true;
  }

  if (cmp_end > 0 || (cmp_end == 0 && !end_inclusive)) {
    if (!EnsureChildLoaded(node, true, error)) {
      return false;
    }
    if (!EmitProofOpsForRangeWithLimit(node->left.get(),
                                       start_key,
                                       end_key,
                                       start_inclusive,
                                       end_inclusive,
                                       remaining_limit,
                                       value_hash_fn,
                                       ops,
                                       error)) {
      return false;
    }
    if (cmp_end == 0 && !end_inclusive) {
      if (!PushKvDigestOp(node, value_hash_fn, ops, error)) {
        return false;
      }
    } else {
      if (!PushKvHashOp(node, value_hash_fn, ops, error)) {
        return false;
      }
    }
    if (HasChild(node, true)) {
      ops->push_back(ProofOp{ProofOp::Type::kParent, {}, {}, {}, 0, false});
    }
    if (HasChild(node, false)) {
      if (!PushHashOpForChild(node, false, value_hash_fn, ops, error)) {
        return false;
      }
      ops->push_back(ProofOp{ProofOp::Type::kChild, {}, {}, {}, 0, false});
    }
    return true;
  }

  bool left_needed = CompareKeyOrder(start_key, node->key) < 0;
  bool right_needed = CompareKeyOrder(node->key, end_key) < 0;

  if (left_needed) {
    if (!EnsureChildLoaded(node, true, error)) {
      return false;
    }
    if (!EmitProofOpsForRangeWithLimit(node->left.get(),
                                       start_key,
                                       end_key,
                                       start_inclusive,
                                       end_inclusive,
                                       remaining_limit,
                                       value_hash_fn,
                                       ops,
                                       error)) {
      return false;
    }
  } else if (HasChild(node, true)) {
    if (!PushHashOpForChild(node, true, value_hash_fn, ops, error)) {
      return false;
    }
  }

  if (*remaining_limit > 0) {
    ProofOp op;
    op.type = ProofOp::Type::kPushKv;
    op.key = node->key;
    op.value = node->value;
    ops->push_back(std::move(op));
    (*remaining_limit)--;
  } else {
    if (!PushKvHashOp(node, value_hash_fn, ops, error)) {
      return false;
    }
  }
  if (HasChild(node, true)) {
    ops->push_back(ProofOp{ProofOp::Type::kParent, {}, {}, {}, 0, false});
  }

  if (right_needed) {
    if (!EnsureChildLoaded(node, false, error)) {
      return false;
    }
    if (!EmitProofOpsForRangeWithLimit(node->right.get(),
                                       start_key,
                                       end_key,
                                       start_inclusive,
                                       end_inclusive,
                                       remaining_limit,
                                       value_hash_fn,
                                       ops,
                                       error)) {
      return false;
    }
  } else if (HasChild(node, false)) {
    if (!PushHashOpForChild(node, false, value_hash_fn, ops, error)) {
      return false;
    }
  }
  if (HasChild(node, false)) {
    ops->push_back(ProofOp{ProofOp::Type::kChild, {}, {}, {}, 0, false});
  }
  return true;
}

bool MerkTree::EmitProofOpsForRangeWithCount(Node* node,
                                             const std::vector<uint8_t>& start_key,
                                             const std::vector<uint8_t>& end_key,
                                             bool start_inclusive,
                                             bool end_inclusive,
                                             const ValueHashFn& value_hash_fn,
                                             std::vector<ProofOp>* ops,
                                             std::string* error) const {
  if (ops == nullptr) {
    if (error) {
      *error = "proof ops output is null";
    }
    return false;
  }
  if (node == nullptr) {
    return true;
  }

  int cmp_start = CompareKeyOrder(node->key, start_key);
  int cmp_end = CompareKeyOrder(node->key, end_key);

  if (cmp_start < 0 || (cmp_start == 0 && !start_inclusive)) {
    if (HasChild(node, true)) {
      if (!PushHashOpWithCountForChild(node, true, value_hash_fn, ops, error)) {
        return false;
      }
    }
    if (cmp_start == 0 && !start_inclusive) {
      if (!PushKvDigestCountOp(node, value_hash_fn, ops, error)) {
        return false;
      }
    } else {
      if (!PushKvHashCountOp(node, value_hash_fn, ops, error)) {
        return false;
      }
    }
    if (HasChild(node, true)) {
      ops->push_back(ProofOp{ProofOp::Type::kParent, {}, {}, {}, 0, false});
    }
    if (!EnsureChildLoaded(node, false, error)) {
      return false;
    }
    if (!EmitProofOpsForRangeWithCount(node->right.get(),
                                       start_key,
                                       end_key,
                                       start_inclusive,
                                       end_inclusive,
                                       value_hash_fn,
                                       ops,
                                       error)) {
      return false;
    }
    if (HasChild(node, false)) {
      ops->push_back(ProofOp{ProofOp::Type::kChild, {}, {}, {}, 0, false});
    }
    return true;
  }

  if (cmp_end > 0 || (cmp_end == 0 && !end_inclusive)) {
    if (!EnsureChildLoaded(node, true, error)) {
      return false;
    }
    if (!EmitProofOpsForRangeWithCount(node->left.get(),
                                       start_key,
                                       end_key,
                                       start_inclusive,
                                       end_inclusive,
                                       value_hash_fn,
                                       ops,
                                       error)) {
      return false;
    }
    if (cmp_end == 0 && !end_inclusive) {
      if (!PushKvDigestCountOp(node, value_hash_fn, ops, error)) {
        return false;
      }
    } else {
      if (!PushKvHashCountOp(node, value_hash_fn, ops, error)) {
        return false;
      }
    }
    if (HasChild(node, true)) {
      ops->push_back(ProofOp{ProofOp::Type::kParent, {}, {}, {}, 0, false});
    }
    if (HasChild(node, false)) {
      if (!PushHashOpWithCountForChild(node, false, value_hash_fn, ops, error)) {
        return false;
      }
      ops->push_back(ProofOp{ProofOp::Type::kChild, {}, {}, {}, 0, false});
    }
    return true;
  }

  bool left_needed = CompareKeyOrder(start_key, node->key) < 0;
  bool right_needed = CompareKeyOrder(node->key, end_key) < 0;

  if (left_needed) {
    if (!EnsureChildLoaded(node, true, error)) {
      return false;
    }
    if (!EmitProofOpsForRangeWithCount(node->left.get(),
                                       start_key,
                                       end_key,
                                       start_inclusive,
                                       end_inclusive,
                                       value_hash_fn,
                                       ops,
                                       error)) {
      return false;
    }
  } else if (HasChild(node, true)) {
    if (!PushHashOpWithCountForChild(node, true, value_hash_fn, ops, error)) {
      return false;
    }
  }

  ProofOp op;
  uint64_t count = 0;
  if (!ComputeNodeCount(node, &count, error)) {
    return false;
  }
  op.type = ProofOp::Type::kPushKvCount;
  op.key = node->key;
  op.value = node->value;
  op.count = count;
  ops->push_back(std::move(op));
  if (HasChild(node, true)) {
    ops->push_back(ProofOp{ProofOp::Type::kParent, {}, {}, {}, 0, false});
  }

  if (right_needed) {
    if (!EnsureChildLoaded(node, false, error)) {
      return false;
    }
    if (!EmitProofOpsForRangeWithCount(node->right.get(),
                                       start_key,
                                       end_key,
                                       start_inclusive,
                                       end_inclusive,
                                       value_hash_fn,
                                       ops,
                                       error)) {
      return false;
    }
  } else if (HasChild(node, false)) {
    if (!PushHashOpWithCountForChild(node, false, value_hash_fn, ops, error)) {
      return false;
    }
  }
  if (HasChild(node, false)) {
    ops->push_back(ProofOp{ProofOp::Type::kChild, {}, {}, {}, 0, false});
  }
  return true;
}

bool MerkTree::HasKeyInRange(Node* node,
                             const std::vector<uint8_t>& start_key,
                             const std::vector<uint8_t>& end_key,
                             bool start_inclusive,
                             bool end_inclusive,
                             std::string* error) const {
  if (node == nullptr) {
    return false;
  }
  int cmp_start = CompareKeyOrder(node->key, start_key);
  int cmp_end = CompareKeyOrder(node->key, end_key);
  if (cmp_start < 0 || (cmp_start == 0 && !start_inclusive)) {
    if (!EnsureChildLoaded(node, false, error)) {
      return false;
    }
    return HasKeyInRange(node->right.get(),
                         start_key,
                         end_key,
                         start_inclusive,
                         end_inclusive,
                         error);
  }
  if (cmp_end > 0 || (cmp_end == 0 && !end_inclusive)) {
    if (!EnsureChildLoaded(node, true, error)) {
      return false;
    }
    return HasKeyInRange(node->left.get(),
                         start_key,
                         end_key,
                         start_inclusive,
                         end_inclusive,
                         error);
  }
  return true;
}

bool MerkTree::EmitProofOpsForAbsent(Node* node,
                                     const std::vector<uint8_t>& target_key,
                                     const Node* predecessor,
                                     const Node* successor,
                                     const ValueHashFn& value_hash_fn,
                                     std::vector<ProofOp>* ops,
                                     std::string* error) const {
  if (ops == nullptr) {
    if (error) {
      *error = "proof ops output is null";
    }
    return false;
  }
  if (node == nullptr) {
    return true;
  }
  bool is_boundary =
      (predecessor && node->key == predecessor->key) ||
      (successor && node->key == successor->key);

  if (CompareKeys(target_key, node->key)) {
    if (!EnsureChildLoaded(node, true, error)) {
      return false;
    }
    if (!EmitProofOpsForAbsent(node->left.get(),
                               target_key,
                               predecessor,
                               successor,
                               value_hash_fn,
                               ops,
                               error)) {
      return false;
    }
    if (is_boundary) {
      if (!PushKvDigestOp(node, value_hash_fn, ops, error)) {
        return false;
      }
    } else {
      if (!PushKvHashOp(node, value_hash_fn, ops, error)) {
        return false;
      }
    }
    if (HasChild(node, true)) {
      ops->push_back(ProofOp{ProofOp::Type::kParent, {}, {}, {}, 0, false});
    }
    if (HasChild(node, false)) {
      if (!PushHashOpForChild(node, false, value_hash_fn, ops, error)) {
        return false;
      }
      ops->push_back(ProofOp{ProofOp::Type::kChild, {}, {}, {}, 0, false});
    }
    return true;
  }
  if (CompareKeys(node->key, target_key)) {
    if (HasChild(node, true)) {
      if (!PushHashOpForChild(node, true, value_hash_fn, ops, error)) {
        return false;
      }
    }
    if (is_boundary) {
      if (!PushKvDigestOp(node, value_hash_fn, ops, error)) {
        return false;
      }
    } else {
      if (!PushKvHashOp(node, value_hash_fn, ops, error)) {
        return false;
      }
    }
    if (HasChild(node, true)) {
      ops->push_back(ProofOp{ProofOp::Type::kParent, {}, {}, {}, 0, false});
    }
    if (!EnsureChildLoaded(node, false, error)) {
      return false;
    }
    if (!EmitProofOpsForAbsent(node->right.get(),
                               target_key,
                               predecessor,
                               successor,
                               value_hash_fn,
                               ops,
                               error)) {
      return false;
    }
    if (HasChild(node, false)) {
      ops->push_back(ProofOp{ProofOp::Type::kChild, {}, {}, {}, 0, false});
    }
    return true;
  }
  if (error) {
    *error = "absence proof reached existing key";
  }
  return false;
}

bool MerkTree::EmitProofOpsForAbsentWithCount(Node* node,
                                              const std::vector<uint8_t>& target_key,
                                              const Node* predecessor,
                                              const Node* successor,
                                              const ValueHashFn& value_hash_fn,
                                              std::vector<ProofOp>* ops,
                                              std::string* error) const {
  if (ops == nullptr) {
    if (error) {
      *error = "proof ops output is null";
    }
    return false;
  }
  if (node == nullptr) {
    return true;
  }
  bool is_boundary =
      (predecessor && node->key == predecessor->key) ||
      (successor && node->key == successor->key);

  if (CompareKeys(target_key, node->key)) {
    if (!EnsureChildLoaded(node, true, error)) {
      return false;
    }
    if (!EmitProofOpsForAbsentWithCount(node->left.get(),
                                        target_key,
                                        predecessor,
                                        successor,
                                        value_hash_fn,
                                        ops,
                                        error)) {
      return false;
    }
    if (is_boundary) {
      if (!PushKvDigestCountOp(node, value_hash_fn, ops, error)) {
        return false;
      }
    } else {
      if (!PushKvHashCountOp(node, value_hash_fn, ops, error)) {
        return false;
      }
    }
    if (HasChild(node, true)) {
      ops->push_back(ProofOp{ProofOp::Type::kParent, {}, {}, {}, 0, false});
    }
    if (HasChild(node, false)) {
      if (!PushHashOpWithCountForChild(node, false, value_hash_fn, ops, error)) {
        return false;
      }
      ops->push_back(ProofOp{ProofOp::Type::kChild, {}, {}, {}, 0, false});
    }
    return true;
  }
  if (CompareKeys(node->key, target_key)) {
    if (HasChild(node, true)) {
      if (!PushHashOpWithCountForChild(node, true, value_hash_fn, ops, error)) {
        return false;
      }
    }
    if (is_boundary) {
      if (!PushKvDigestCountOp(node, value_hash_fn, ops, error)) {
        return false;
      }
    } else {
      if (!PushKvHashCountOp(node, value_hash_fn, ops, error)) {
        return false;
      }
    }
    if (HasChild(node, true)) {
      ops->push_back(ProofOp{ProofOp::Type::kParent, {}, {}, {}, 0, false});
    }
    if (!EnsureChildLoaded(node, false, error)) {
      return false;
    }
    if (!EmitProofOpsForAbsentWithCount(node->right.get(),
                                        target_key,
                                        predecessor,
                                        successor,
                                        value_hash_fn,
                                        ops,
                                        error)) {
      return false;
    }
    if (HasChild(node, false)) {
      ops->push_back(ProofOp{ProofOp::Type::kChild, {}, {}, {}, 0, false});
    }
    return true;
  }
  if (error) {
    *error = "absence proof reached existing key";
  }
  return false;
}

bool MerkTree::PushHashOp(const Node* node,
                          const ValueHashFn& value_hash_fn,
                          std::vector<ProofOp>* ops,
                          std::string* error) {
  if (node == nullptr || ops == nullptr) {
    if (error) {
      *error = "hash op inputs are null";
    }
    return false;
  }
  std::vector<uint8_t> hash;
  if (!ComputeNodeHash(node, value_hash_fn, &hash, error)) {
    return false;
  }
  ProofOp op;
  op.type = ProofOp::Type::kPushHash;
  op.hash = std::move(hash);
  ops->push_back(std::move(op));
  return true;
}

bool MerkTree::PushHashOpWithCount(const Node* node,
                                   const ValueHashFn& value_hash_fn,
                                   std::vector<ProofOp>* ops,
                                   std::string* error) {
  if (node == nullptr || ops == nullptr) {
    if (error) {
      *error = "hash op inputs are null";
    }
    return false;
  }
  std::vector<uint8_t> hash;
  uint64_t count = 0;
  if (!ComputeNodeHashWithCount(node, value_hash_fn, &hash, &count, error)) {
    return false;
  }
  ProofOp op;
  op.type = ProofOp::Type::kPushHash;
  op.hash = std::move(hash);
  ops->push_back(std::move(op));
  return true;
}

bool MerkTree::HasChild(const Node* node, bool left) const {
  if (node == nullptr) {
    return false;
  }
  return left ? (node->left != nullptr || node->left_meta.present)
              : (node->right != nullptr || node->right_meta.present);
}

bool MerkTree::PushHashOpForChild(const Node* node,
                                  bool left,
                                  const ValueHashFn& value_hash_fn,
                                  std::vector<ProofOp>* ops,
                                  std::string* error) const {
  if (node == nullptr || ops == nullptr) {
    if (error) {
      *error = "hash op inputs are null";
    }
    return false;
  }
  const Node* child = left ? node->left.get() : node->right.get();
  if (child) {
    if (hash_caches_canonical_ &&
        !UsesProvableCountHashing(tree_feature_tag_) &&
        !value_hash_fn &&
        child->hash_generation == hash_policy_generation_ &&
        child->node_hash.size() == 32) {
      ProofOp op;
      op.type = ProofOp::Type::kPushHash;
      op.hash = child->node_hash;
      ops->push_back(std::move(op));
      return true;
    }
    return PushHashOp(child, value_hash_fn, ops, error);
  }
  const Node::ChildMeta& meta = left ? node->left_meta : node->right_meta;
  if (!meta.present) {
    if (error) {
      *error = "missing child metadata";
    }
    return false;
  }
  if (meta.hash.size() != 32) {
    if (error) {
      *error = "child metadata hash length mismatch";
    }
    return false;
  }
  ProofOp op;
  op.type = ProofOp::Type::kPushHash;
  op.hash = meta.hash;
  ops->push_back(std::move(op));
  return true;
}

bool MerkTree::PushHashOpWithCountForChild(const Node* node,
                                           bool left,
                                           const ValueHashFn& value_hash_fn,
                                           std::vector<ProofOp>* ops,
                                           std::string* error) const {
  if (node == nullptr || ops == nullptr) {
    if (error) {
      *error = "hash op inputs are null";
    }
    return false;
  }
  const Node* child = left ? node->left.get() : node->right.get();
  if (child) {
    return PushHashOpWithCount(child, value_hash_fn, ops, error);
  }
  const Node::ChildMeta& meta = left ? node->left_meta : node->right_meta;
  if (!meta.present) {
    if (error) {
      *error = "missing child metadata";
    }
    return false;
  }
  if (meta.hash.size() != 32) {
    if (error) {
      *error = "child metadata hash length mismatch";
    }
    return false;
  }
  ProofOp op;
  op.type = ProofOp::Type::kPushHash;
  op.hash = meta.hash;
  ops->push_back(std::move(op));
  return true;
}

bool MerkTree::PushKvHashOp(const Node* node,
                            const ValueHashFn& value_hash_fn,
                            std::vector<ProofOp>* ops,
                            std::string* error) {
  if (node == nullptr || ops == nullptr) {
    if (error) {
      *error = "kv hash op inputs are null";
    }
    return false;
  }
  (void)value_hash_fn;
  ProofOp op;
  op.type = ProofOp::Type::kPushKvHash;
  op.hash = node->kv_hash;
  ops->push_back(std::move(op));
  return true;
}

bool MerkTree::PushKvHashCountOp(const Node* node,
                                 const ValueHashFn& value_hash_fn,
                                 std::vector<ProofOp>* ops,
                                 std::string* error) {
  if (node == nullptr || ops == nullptr) {
    if (error) {
      *error = "kv hash op inputs are null";
    }
    return false;
  }
  (void)value_hash_fn;
  uint64_t count = 0;
  if (!ComputeNodeCount(node, &count, error)) {
    return false;
  }
  ProofOp op;
  op.type = ProofOp::Type::kPushKvHashCount;
  op.hash = node->kv_hash;
  op.count = count;
  ops->push_back(std::move(op));
  return true;
}

bool MerkTree::PushKvDigestOp(const Node* node,
                              const ValueHashFn& value_hash_fn,
                              std::vector<ProofOp>* ops,
                              std::string* error) {
  if (node == nullptr || ops == nullptr) {
    if (error) {
      *error = "kv digest op inputs are null";
    }
    return false;
  }
  (void)value_hash_fn;
  ProofOp op;
  op.type = ProofOp::Type::kPushKvDigest;
  op.key = node->key;
  op.hash = node->value_hash;
  ops->push_back(std::move(op));
  return true;
}

bool MerkTree::PushKvDigestCountOp(const Node* node,
                                   const ValueHashFn& value_hash_fn,
                                   std::vector<ProofOp>* ops,
                                   std::string* error) {
  if (node == nullptr || ops == nullptr) {
    if (error) {
      *error = "kv digest op inputs are null";
    }
    return false;
  }
  (void)value_hash_fn;
  uint64_t count = 0;
  if (!ComputeNodeCount(node, &count, error)) {
    return false;
  }
  ProofOp op;
  op.type = ProofOp::Type::kPushKvDigestCount;
  op.key = node->key;
  op.hash = node->value_hash;
  op.count = count;
  ops->push_back(std::move(op));
  return true;
}

bool MerkTree::EncodeProofOps(const std::vector<ProofOp>& ops,
                              std::vector<uint8_t>* out,
                              std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "proof output is null";
    }
    return false;
  }
  out->clear();
  for (const auto& op : ops) {
    switch (op.type) {
      case ProofOp::Type::kPushHash:
        if (op.hash.size() != 32) {
          if (error) {
            *error = "hash node length mismatch";
          }
          return false;
        }
        out->push_back(0x01);
        out->insert(out->end(), op.hash.begin(), op.hash.end());
        break;
      case ProofOp::Type::kPushKvHash:
        if (op.hash.size() != 32) {
          if (error) {
            *error = "kv hash length mismatch";
          }
          return false;
        }
        out->push_back(0x02);
        out->insert(out->end(), op.hash.begin(), op.hash.end());
        break;
      case ProofOp::Type::kPushKv:
        if (!EncodeKv(op.key, op.value, out, error)) {
          return false;
        }
        break;
      case ProofOp::Type::kPushKvValueHash:
        if (op.hash.size() != 32) {
          if (error) {
            *error = "value hash length mismatch";
          }
          return false;
        }
        if (op.key.size() > 255) {
          if (error) {
            *error = "key length exceeds u8";
          }
          return false;
        }
        if (op.value.size() > 0xFFFFFFFFu) {
          if (error) {
            *error = "value length exceeds u32";
          }
          return false;
        }
        {
          bool large_value = op.value.size() > 0xFFFF;
          if (op.has_feature_type || op.provable_count) {
            out->push_back(large_value ? 0x23 : 0x07);
          } else {
            out->push_back(large_value ? 0x21 : 0x04);
          }
          out->push_back(static_cast<uint8_t>(op.key.size()));
          out->insert(out->end(), op.key.begin(), op.key.end());
          if (large_value) {
            EncodeU32BE(static_cast<uint32_t>(op.value.size()), out);
          } else {
            EncodeU16BE(static_cast<uint16_t>(op.value.size()), out);
          }
          out->insert(out->end(), op.value.begin(), op.value.end());
          out->insert(out->end(), op.hash.begin(), op.hash.end());
          if (op.has_feature_type || op.provable_count) {
            if (op.provable_count) {
              AppendProvableCountFeatureType(op.count, out);
            } else {
              AppendFeatureTypeTagZero(out);
            }
          }
        }
        break;
      case ProofOp::Type::kPushKvDigest:
        if (op.key.size() > 255) {
          if (error) {
            *error = "key length exceeds u8";
          }
          return false;
        }
        if (op.hash.size() != 32) {
          if (error) {
            *error = "value hash length mismatch";
          }
          return false;
        }
        out->push_back(0x05);
        out->push_back(static_cast<uint8_t>(op.key.size()));
        out->insert(out->end(), op.key.begin(), op.key.end());
        out->insert(out->end(), op.hash.begin(), op.hash.end());
        break;
      case ProofOp::Type::kPushKvCount: {
        if (op.key.size() > 255) {
          if (error) {
            *error = "key length exceeds u8";
          }
          return false;
        }
        if (op.value.size() > 0xFFFFFFFFu) {
          if (error) {
            *error = "value length exceeds u32";
          }
          return false;
        }
        bool large_value = op.value.size() > 0xFFFF;
        out->push_back(large_value ? 0x24 : 0x14);
        out->push_back(static_cast<uint8_t>(op.key.size()));
        out->insert(out->end(), op.key.begin(), op.key.end());
        if (large_value) {
          EncodeU32BE(static_cast<uint32_t>(op.value.size()), out);
        } else {
          EncodeU16BE(static_cast<uint16_t>(op.value.size()), out);
        }
        out->insert(out->end(), op.value.begin(), op.value.end());
        EncodeU64BE(op.count, out);
        break;
      }
      case ProofOp::Type::kPushKvHashCount:
        if (op.hash.size() != 32) {
          if (error) {
            *error = "kv hash length mismatch";
          }
          return false;
        }
        out->push_back(0x15);
        out->insert(out->end(), op.hash.begin(), op.hash.end());
        EncodeU64BE(op.count, out);
        break;
      case ProofOp::Type::kPushKvDigestCount:
        if (op.key.size() > 255) {
          if (error) {
            *error = "key length exceeds u8";
          }
          return false;
        }
        if (op.hash.size() != 32) {
          if (error) {
            *error = "value hash length mismatch";
          }
          return false;
        }
        out->push_back(0x1a);
        out->push_back(static_cast<uint8_t>(op.key.size()));
        out->insert(out->end(), op.key.begin(), op.key.end());
        out->insert(out->end(), op.hash.begin(), op.hash.end());
        EncodeU64BE(op.count, out);
        break;
      case ProofOp::Type::kParent:
        out->push_back(0x10);
        break;
      case ProofOp::Type::kChild:
        out->push_back(0x11);
        break;
    }
  }
  return true;
}

bool MerkTree::EncodeChunkProofOps(const std::vector<ProofOp>& ops,
                                   std::vector<uint8_t>* out,
                                   std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "chunk proof output is null";
    }
    return false;
  }
  std::vector<uint8_t> encoded_ops;
  if (!EncodeProofOps(ops, &encoded_ops, error)) {
    return false;
  }
  out->clear();
  out->push_back(0x01);
  EncodeVarintU64(static_cast<uint64_t>(ops.size()), out);
  out->insert(out->end(), encoded_ops.begin(), encoded_ops.end());
  return true;
}

bool MerkTree::EmitChunkProofOpsForNode(const Node* node,
                                        const ValueHashFn& value_hash_fn,
                                        bool provable_count,
                                        std::vector<ProofOp>* ops,
                                        std::string* error) {
  if (node == nullptr || ops == nullptr) {
    if (error) {
      *error = "chunk node inputs are null";
    }
    return false;
  }
  uint64_t variant = 0;
  bool has_variant = DecodeElementVariant(node->value, &variant, nullptr);
  bool is_tree_variant = has_variant && (variant == 2 || variant == 4 || variant == 5 ||
                                         variant == 6 || variant == 7 || variant == 8 ||
                                         variant == 10);
  bool is_reference_variant = has_variant && (variant == 1);
  bool use_value_hash = is_tree_variant || is_reference_variant || !has_variant;

  if (provable_count && !use_value_hash) {
    uint64_t count = 0;
    if (!ComputeNodeCount(node, &count, error)) {
      return false;
    }
    ProofOp op;
    op.type = ProofOp::Type::kPushKvCount;
    op.key = node->key;
    op.value = node->value;
    op.count = count;
    op.provable_count = true;
    ops->push_back(std::move(op));
    return true;
  }

  if (use_value_hash) {
    std::vector<uint8_t> value_hash;
    if (!ComputeValueHash(node->key, node->value, value_hash_fn, &value_hash, error)) {
      return false;
    }
    ProofOp op;
    op.type = ProofOp::Type::kPushKvValueHash;
    op.key = node->key;
    op.value = node->value;
    op.hash = std::move(value_hash);
    op.has_feature_type = true;
    if (provable_count) {
      uint64_t count = 0;
      if (!ComputeNodeCount(node, &count, error)) {
        return false;
      }
      op.count = count;
      op.provable_count = true;
    }
    ops->push_back(std::move(op));
    return true;
  }

  ProofOp op;
  op.type = ProofOp::Type::kPushKv;
  op.key = node->key;
  op.value = node->value;
  if (provable_count) {
    uint64_t count = 0;
    if (!ComputeNodeCount(node, &count, error)) {
      return false;
    }
    op.count = count;
  }
  ops->push_back(std::move(op));
  return true;
}

bool MerkTree::EmitChunkProofOps(Node* node,
                                 size_t remaining_depth,
                                 const ValueHashFn& value_hash_fn,
                                 bool provable_count,
                                 std::vector<ProofOp>* ops,
                                 std::string* error) const {
  if (node == nullptr || ops == nullptr) {
    if (error) {
      *error = "chunk proof inputs are null";
    }
    return false;
  }
  if (remaining_depth == 0) {
    if (provable_count) {
      return PushHashOpWithCount(node, value_hash_fn, ops, error);
    }
    return PushHashOp(node, value_hash_fn, ops, error);
  }
  bool has_left = HasChild(node, true);
  if (has_left) {
    if (!EnsureChildLoaded(node, true, error)) {
      return false;
    }
    if (node->left == nullptr) {
      if (error) {
        *error = "missing left child while generating chunk proof";
      }
      return false;
    }
    if (!EmitChunkProofOps(node->left.get(),
                           remaining_depth - 1,
                           value_hash_fn,
                           provable_count,
                           ops,
                           error)) {
      return false;
    }
  }
  if (!EmitChunkProofOpsForNode(node, value_hash_fn, provable_count, ops, error)) {
    return false;
  }
  if (has_left) {
    ops->push_back(ProofOp{ProofOp::Type::kParent, {}, {}, {}, 0, false});
  }
  if (HasChild(node, false)) {
    if (!EnsureChildLoaded(node, false, error)) {
      return false;
    }
    if (node->right == nullptr) {
      if (error) {
        *error = "missing right child while generating chunk proof";
      }
      return false;
    }
    if (!EmitChunkProofOps(node->right.get(),
                           remaining_depth - 1,
                           value_hash_fn,
                           provable_count,
                           ops,
                           error)) {
      return false;
    }
    ops->push_back(ProofOp{ProofOp::Type::kChild, {}, {}, {}, 0, false});
  }
  return true;
}

bool MerkTree::GenerateChunkProof(size_t depth,
                                  const ValueHashFn& value_hash_fn,
                                  bool provable_count,
                                  std::vector<uint8_t>* out_proof,
                                  std::string* error) const {
  if (root_ == nullptr) {
    if (error) {
      *error = "empty tree";
    }
    return false;
  }
  size_t max_depth = static_cast<size_t>(Height());
  if (depth > max_depth) {
    depth = max_depth;
  }
  std::vector<ProofOp> ops;
  if (!EmitChunkProofOps(root_.get(), depth, value_hash_fn, provable_count, &ops, error)) {
    return false;
  }
  std::vector<uint8_t> chunk_bytes;
  if (!EncodeChunkProofOps(ops, &chunk_bytes, error)) {
    return false;
  }
  if (out_proof == nullptr) {
    if (error) {
      *error = "chunk proof output is null";
    }
    return false;
  }
  out_proof->clear();
  out_proof->push_back(0x00);
  EncodeVarintU64(0, out_proof);
  out_proof->insert(out_proof->end(), chunk_bytes.begin(), chunk_bytes.end());
  return true;
}

bool MerkTree::GenerateChunkProofAt(const std::vector<bool>& instructions,
                                    size_t depth,
                                    const ValueHashFn& value_hash_fn,
                                    bool provable_count,
                                    std::vector<uint8_t>* out_proof,
                                    std::string* error) const {
  if (root_ == nullptr) {
    if (error) {
      *error = "empty tree";
    }
    return false;
  }
  Node* node = root_.get();
  for (bool step : instructions) {
    if (!EnsureChildLoaded(node, step, error)) {
      return false;
    }
    node = step ? node->left.get() : node->right.get();
    if (node == nullptr) {
      if (error) {
        *error = "invalid chunk traversal instruction";
      }
      return false;
    }
  }
  size_t max_depth = static_cast<size_t>(Height());
  if (depth > max_depth) {
    depth = max_depth;
  }
  std::vector<ProofOp> ops;
  if (!EmitChunkProofOps(node, depth, value_hash_fn, provable_count, &ops, error)) {
    return false;
  }
  std::vector<uint8_t> chunk_bytes;
  if (!EncodeChunkProofOps(ops, &chunk_bytes, error)) {
    return false;
  }
  if (out_proof == nullptr) {
    if (error) {
      *error = "chunk proof output is null";
    }
    return false;
  }
  std::vector<uint8_t> chunk_id = TraversalInstructionToBytes(instructions);
  out_proof->clear();
  out_proof->push_back(0x00);
  EncodeVarintU64(static_cast<uint64_t>(chunk_id.size()), out_proof);
  out_proof->insert(out_proof->end(), chunk_id.begin(), chunk_id.end());
  out_proof->insert(out_proof->end(), chunk_bytes.begin(), chunk_bytes.end());
  return true;
}

bool MerkTree::GenerateChunkOps(size_t depth,
                                const ValueHashFn& value_hash_fn,
                                bool provable_count,
                                std::vector<uint8_t>* out_proof,
                                std::string* error) const {
  if (root_ == nullptr) {
    if (error) {
      *error = "empty tree";
    }
    return false;
  }
  size_t max_depth = static_cast<size_t>(Height());
  if (depth > max_depth) {
    depth = max_depth;
  }
  std::vector<ProofOp> ops;
  if (!EmitChunkProofOps(root_.get(), depth, value_hash_fn, provable_count, &ops, error)) {
    return false;
  }
  return EncodeProofOps(ops, out_proof, error);
}

bool MerkTree::GenerateChunkOpsAt(const std::vector<bool>& instructions,
                                  size_t depth,
                                  const ValueHashFn& value_hash_fn,
                                  bool provable_count,
                                  std::vector<uint8_t>* out_proof,
                                  std::string* error) const {
  if (root_ == nullptr) {
    if (error) {
      *error = "empty tree";
    }
    return false;
  }
  Node* node = root_.get();
  for (bool step : instructions) {
    if (!EnsureChildLoaded(node, step, error)) {
      return false;
    }
    node = step ? node->left.get() : node->right.get();
    if (node == nullptr) {
      if (error) {
        *error = "invalid chunk traversal instruction";
      }
      return false;
    }
  }
  size_t max_depth = static_cast<size_t>(Height());
  if (depth > max_depth) {
    depth = max_depth;
  }
  std::vector<ProofOp> ops;
  if (!EmitChunkProofOps(node, depth, value_hash_fn, provable_count, &ops, error)) {
    return false;
  }
  return EncodeProofOps(ops, out_proof, error);
}

bool MerkTree::BuildEncodedNodeInternal(RocksDbWrapper* storage,
                                        const std::vector<std::vector<uint8_t>>& path,
                                        const std::vector<uint8_t>& key,
                                        ColumnFamilyKind cf,
                                        std::unordered_set<std::string>* visiting,
                                        std::unique_ptr<Node>* out,
                                        std::vector<uint8_t>* out_hash,
                                        int* out_height,
                                        int* out_left_height,
                                        int* out_right_height,
                                        bool load_children,
                                        std::string* error) {
  if (storage == nullptr || out == nullptr || visiting == nullptr) {
    if (error) {
      *error = "encoded node inputs are null";
    }
    return false;
  }
  std::string key_str(reinterpret_cast<const char*>(key.data()), key.size());
  if (visiting->find(key_str) != visiting->end()) {
    if (error) {
      *error = "encoded node cycle detected at key '" +
               std::string(key.begin(), key.end()) + "' path_depth=" +
               std::to_string(path.size());
    }
    return false;
  }
  visiting->insert(key_str);
  std::vector<uint8_t> encoded;
  bool found = false;
  if (!storage->Get(cf, path, key, &encoded, &found, error)) {
    return false;
  }
  if (!found) {
    if (error) {
      *error = "encoded node not found";
    }
    return false;
  }
  TreeNodeInner inner;
  if (!DecodeTreeNodeInner(encoded, &inner, error)) {
    return false;
  }
  if (inner.kv.kv_hash.size() != 32 || inner.kv.value_hash.size() != 32) {
    if (error) {
      *error = "encoded node hash length mismatch";
    }
    return false;
  }
  std::vector<uint8_t> computed_kv_hash;
  if (!KvDigestToKvHash(key, inner.kv.value_hash, &computed_kv_hash, error)) {
    return false;
  }
  if (computed_kv_hash != inner.kv.kv_hash) {
    if (error) {
      *error = "encoded kv hash mismatch";
    }
    return false;
  }
  auto node = std::make_unique<Node>();
  node->key = key;
  node->value = inner.kv.value;
  node->value_hash = inner.kv.value_hash;
  node->kv_hash = inner.kv.kv_hash;
  node->feature_type = inner.kv.feature_type;
  int left_height = 0;
  int right_height = 0;
  int left_left_height = 0;
  int left_right_height = 0;
  int right_left_height = 0;
  int right_right_height = 0;
  std::vector<uint8_t> left_hash;
  std::vector<uint8_t> right_hash;
  if (inner.has_left) {
    if (inner.left.hash.size() != 32) {
      if (error) {
        *error = "encoded left hash length mismatch";
      }
      return false;
    }
    node->left_meta.present = true;
    node->left_meta.loaded = load_children && !inner.left.key.empty();
    node->left_meta.key = inner.left.key;
    node->left_meta.hash = inner.left.hash;
    node->left_meta.left_height = inner.left.left_height;
    node->left_meta.right_height = inner.left.right_height;
    node->left_meta.aggregate = inner.left.aggregate;
    if (load_children && !inner.left.key.empty()) {
      if (!BuildEncodedNodeInternal(storage,
                                    path,
                                    inner.left.key,
                                    cf,
                                    visiting,
                                    &node->left,
                                    &left_hash,
                                    &left_height,
                                    &left_left_height,
                                    &left_right_height,
                                    true,
                                    error)) {
        return false;
      }
      if (inner.left.hash != left_hash) {
        if (error) {
          *error = "encoded left hash mismatch";
        }
        return false;
      }
    } else {
      left_hash = inner.left.hash;
      left_left_height = inner.left.left_height;
      left_right_height = inner.left.right_height;
      left_height = std::max(left_left_height, left_right_height) + 1;
    }
  }
  if (inner.has_right) {
    if (inner.right.hash.size() != 32) {
      if (error) {
        *error = "encoded right hash length mismatch";
      }
      return false;
    }
    node->right_meta.present = true;
    node->right_meta.loaded = load_children && !inner.right.key.empty();
    node->right_meta.key = inner.right.key;
    node->right_meta.hash = inner.right.hash;
    node->right_meta.left_height = inner.right.left_height;
    node->right_meta.right_height = inner.right.right_height;
    node->right_meta.aggregate = inner.right.aggregate;
    if (load_children && !inner.right.key.empty()) {
      if (!BuildEncodedNodeInternal(storage,
                                    path,
                                    inner.right.key,
                                    cf,
                                    visiting,
                                    &node->right,
                                    &right_hash,
                                    &right_height,
                                    &right_left_height,
                                    &right_right_height,
                                    true,
                                    error)) {
        return false;
      }
      if (inner.right.hash != right_hash) {
        if (error) {
          *error = "encoded right hash mismatch";
        }
        return false;
      }
    } else {
      right_hash = inner.right.hash;
      right_left_height = inner.right.left_height;
      right_right_height = inner.right.right_height;
      right_height = std::max(right_left_height, right_right_height) + 1;
    }
  }
  node->height = std::max(left_height, right_height) + 1;
  std::vector<uint8_t> zero(32, 0);
  const std::vector<uint8_t>& left_node_hash = inner.has_left ? left_hash : zero;
  const std::vector<uint8_t>& right_node_hash = inner.has_right ? right_hash : zero;
  std::vector<uint8_t> node_hash;
  bool provable_count = inner.kv.feature_type.tag == TreeFeatureTypeTag::kProvableCount ||
                        inner.kv.feature_type.tag == TreeFeatureTypeTag::kProvableCountSum;
  std::vector<uint8_t> node_hash_basic;
  if (!NodeHash(inner.kv.kv_hash, left_node_hash, right_node_hash, &node_hash_basic, error)) {
    return false;
  }
  if (provable_count) {
    if (!NodeHashWithCount(inner.kv.kv_hash,
                           left_node_hash,
                           right_node_hash,
                           inner.kv.feature_type.count,
                           &node_hash,
                           error)) {
      return false;
    }
  } else {
    node_hash = node_hash_basic;
  }
  node->node_hash = node_hash_basic;
  if (out_hash) {
    *out_hash = node_hash;
  }
  if (out_height) {
    *out_height = node->height;
  }
  if (out_left_height) {
    *out_left_height = left_height;
  }
  if (out_right_height) {
    *out_right_height = right_height;
  }
  visiting->erase(key_str);
  *out = std::move(node);
  return true;
}

bool MerkTree::BuildEncodedNodeInternalTx(RocksDbWrapper::Transaction* transaction,
                                          const std::vector<std::vector<uint8_t>>& path,
                                          const std::vector<uint8_t>& key,
                                          ColumnFamilyKind cf,
                                          std::unordered_set<std::string>* visiting,
                                          std::unique_ptr<Node>* out,
                                          std::vector<uint8_t>* out_hash,
                                          int* out_height,
                                          int* out_left_height,
                                          int* out_right_height,
                                          bool load_children,
                                          std::string* error) {
  if (transaction == nullptr || out == nullptr || visiting == nullptr) {
    if (error) {
      *error = "encoded node tx inputs are null";
    }
    return false;
  }
  std::string key_str(reinterpret_cast<const char*>(key.data()), key.size());
  if (visiting->find(key_str) != visiting->end()) {
    if (error) {
      *error = "encoded node cycle detected at key '" +
               std::string(key.begin(), key.end()) + "' path_depth=" +
               std::to_string(path.size());
    }
    return false;
  }
  visiting->insert(key_str);
  std::vector<uint8_t> encoded;
  bool found = false;
  if (!transaction->Get(cf, path, key, &encoded, &found, error)) {
    return false;
  }
  if (!found) {
    if (error) {
      *error = "encoded node not found";
    }
    return false;
  }
  TreeNodeInner inner;
  if (!DecodeTreeNodeInner(encoded, &inner, error)) {
    return false;
  }
  if (inner.kv.kv_hash.size() != 32 || inner.kv.value_hash.size() != 32) {
    if (error) {
      *error = "encoded node hash length mismatch";
    }
    return false;
  }
  std::vector<uint8_t> computed_kv_hash;
  if (!KvDigestToKvHash(key, inner.kv.value_hash, &computed_kv_hash, error)) {
    return false;
  }
  if (computed_kv_hash != inner.kv.kv_hash) {
    if (error) {
      *error = "encoded kv hash mismatch";
    }
    return false;
  }
  auto node = std::make_unique<Node>();
  node->key = key;
  node->value = inner.kv.value;
  node->value_hash = inner.kv.value_hash;
  node->kv_hash = inner.kv.kv_hash;
  node->feature_type = inner.kv.feature_type;
  int left_height = 0;
  int right_height = 0;
  int left_left_height = 0;
  int left_right_height = 0;
  int right_left_height = 0;
  int right_right_height = 0;
  std::vector<uint8_t> left_hash;
  std::vector<uint8_t> right_hash;
  if (inner.has_left) {
    if (inner.left.hash.size() != 32) {
      if (error) {
        *error = "encoded left hash length mismatch";
      }
      return false;
    }
    node->left_meta.present = true;
    node->left_meta.loaded = load_children && !inner.left.key.empty();
    node->left_meta.key = inner.left.key;
    node->left_meta.hash = inner.left.hash;
    node->left_meta.left_height = inner.left.left_height;
    node->left_meta.right_height = inner.left.right_height;
    node->left_meta.aggregate = inner.left.aggregate;
    if (load_children && !inner.left.key.empty()) {
      if (!BuildEncodedNodeInternalTx(transaction,
                                      path,
                                      inner.left.key,
                                      cf,
                                      visiting,
                                      &node->left,
                                      &left_hash,
                                      &left_height,
                                      &left_left_height,
                                      &left_right_height,
                                      true,
                                      error)) {
        return false;
      }
      if (inner.left.hash != left_hash) {
        if (error) {
          *error = "encoded left hash mismatch";
        }
        return false;
      }
    } else {
      left_hash = inner.left.hash;
      left_left_height = inner.left.left_height;
      left_right_height = inner.left.right_height;
      left_height = std::max(left_left_height, left_right_height) + 1;
    }
  }
  if (inner.has_right) {
    if (inner.right.hash.size() != 32) {
      if (error) {
        *error = "encoded right hash length mismatch";
      }
      return false;
    }
    node->right_meta.present = true;
    node->right_meta.loaded = load_children && !inner.right.key.empty();
    node->right_meta.key = inner.right.key;
    node->right_meta.hash = inner.right.hash;
    node->right_meta.left_height = inner.right.left_height;
    node->right_meta.right_height = inner.right.right_height;
    node->right_meta.aggregate = inner.right.aggregate;
    if (load_children && !inner.right.key.empty()) {
      if (!BuildEncodedNodeInternalTx(transaction,
                                      path,
                                      inner.right.key,
                                      cf,
                                      visiting,
                                      &node->right,
                                      &right_hash,
                                      &right_height,
                                      &right_left_height,
                                      &right_right_height,
                                      true,
                                      error)) {
        return false;
      }
      if (inner.right.hash != right_hash) {
        if (error) {
          *error = "encoded right hash mismatch";
        }
        return false;
      }
    } else {
      right_hash = inner.right.hash;
      right_left_height = inner.right.left_height;
      right_right_height = inner.right.right_height;
      right_height = std::max(right_left_height, right_right_height) + 1;
    }
  }
  node->height = std::max(left_height, right_height) + 1;
  std::vector<uint8_t> zero(32, 0);
  const std::vector<uint8_t>& left_node_hash = inner.has_left ? left_hash : zero;
  const std::vector<uint8_t>& right_node_hash = inner.has_right ? right_hash : zero;
  std::vector<uint8_t> node_hash;
  bool provable_count = inner.kv.feature_type.tag == TreeFeatureTypeTag::kProvableCount ||
                        inner.kv.feature_type.tag == TreeFeatureTypeTag::kProvableCountSum;
  std::vector<uint8_t> node_hash_basic;
  if (!NodeHash(inner.kv.kv_hash, left_node_hash, right_node_hash, &node_hash_basic, error)) {
    return false;
  }
  if (provable_count) {
    if (!NodeHashWithCount(inner.kv.kv_hash,
                           left_node_hash,
                           right_node_hash,
                           inner.kv.feature_type.count,
                           &node_hash,
                           error)) {
      return false;
    }
  } else {
    node_hash = node_hash_basic;
  }
  node->node_hash = node_hash_basic;
  if (out_hash) {
    *out_hash = node_hash;
  }
  if (out_height) {
    *out_height = node->height;
  }
  if (out_left_height) {
    *out_left_height = left_height;
  }
  if (out_right_height) {
    *out_right_height = right_height;
  }
  visiting->erase(key_str);
  *out = std::move(node);
  return true;
}

bool MerkTree::BuildEncodedNode(RocksDbWrapper* storage,
                                const std::vector<std::vector<uint8_t>>& path,
                                const std::vector<uint8_t>& key,
                                ColumnFamilyKind cf,
                                std::unordered_set<std::string>* visiting,
                                std::unique_ptr<Node>* out,
                                std::vector<uint8_t>* out_hash,
                                int* out_height,
                                int* out_left_height,
                                int* out_right_height,
                                std::string* error) {
  return BuildEncodedNodeInternal(storage,
                                  path,
                                  key,
                                  cf,
                                  visiting,
                                  out,
                                  out_hash,
                                  out_height,
                                  out_left_height,
                                  out_right_height,
                                  true,
                                  error);
}

bool MerkTree::GetNodeMeta(const std::vector<uint8_t>& key,
                           NodeMeta* out,
                           std::string* error) const {
  if (out == nullptr) {
    if (error) {
      *error = "node meta output is null";
    }
    return false;
  }
  // Rust parity: use lazy on-demand child loading instead of forcing full tree
  // load. This matches Rust's walker-based traversal that loads nodes only
  // along the search path.
  Node* node = root_.get();
  while (node) {
    int cmp = CompareKeyOrder(key, node->key);
    if (cmp == 0) {
      out->has_left = node->left_meta.present;
      out->has_right = node->right_meta.present;
      out->left_hash = node->left_meta.hash;
      out->left_left_height = node->left_meta.left_height;
      out->left_right_height = node->left_meta.right_height;
      out->right_hash = node->right_meta.hash;
      out->right_left_height = node->right_meta.left_height;
      out->right_right_height = node->right_meta.right_height;
      return true;
    }
    const bool go_left = cmp < 0;
    if (!EnsureChildLoaded(node, go_left, error)) {
      return false;
    }
    node = go_left ? node->left.get() : node->right.get();
  }
  if (error) {
    *error = "node key not found";
  }
  return false;
}

bool MerkTree::LoadEncodedTree(RocksDbWrapper* storage,
                               const std::vector<std::vector<uint8_t>>& path,
                               const std::vector<uint8_t>& root_key,
                               std::string* error) {
  return LoadEncodedTree(storage, path, root_key, ColumnFamilyKind::kDefault, false, error);
}

bool MerkTree::LoadEncodedTree(RocksDbWrapper* storage,
                               const std::vector<std::vector<uint8_t>>& path,
                               const std::vector<uint8_t>& root_key,
                               ColumnFamilyKind cf,
                               std::string* error) {
  return LoadEncodedTree(storage, path, root_key, cf, false, error);
}

bool MerkTree::LoadEncodedTree(RocksDbWrapper* storage,
                               const std::vector<std::vector<uint8_t>>& path,
                               const std::vector<uint8_t>& root_key,
                               ColumnFamilyKind cf,
                               bool lazy,
                               std::string* error) {
  if (storage == nullptr) {
    if (error) {
      *error = "storage is null";
    }
    return false;
  }
  if (root_key.empty()) {
    if (error) {
      *error = "root key is empty";
    }
    return false;
  }
  std::unordered_set<std::string> visiting;
  std::unique_ptr<Node> root;
  if (!BuildEncodedNodeInternal(storage,
                                path,
                                root_key,
                                cf,
                                &visiting,
                                &root,
                                nullptr,
                                nullptr,
                                nullptr,
                                nullptr,
                                !lazy,
                                error)) {
    return false;
  }
  root_ = std::move(root);
  hash_caches_canonical_ = false;
  dirty_keys_.clear();
  deleted_keys_.clear();
  storage_ = storage;
  storage_tx_ = nullptr;
  storage_cf_ = cf;
  storage_path_ = path;
  initial_root_key_ = root_key;
  has_initial_root_key_ = true;
  lazy_loading_ = lazy;
  if (root_ && root_->node_hash.empty()) {
    UpdateNodeHashOrDie(root_.get(), value_hash_fn_);
  }
  return true;
}

bool MerkTree::LoadEncodedTree(RocksDbWrapper::Transaction* transaction,
                               const std::vector<std::vector<uint8_t>>& path,
                               const std::vector<uint8_t>& root_key,
                               ColumnFamilyKind cf,
                               bool lazy,
                               std::string* error) {
  if (transaction == nullptr) {
    if (error) {
      *error = "transaction is null";
    }
    return false;
  }
  if (root_key.empty()) {
    if (error) {
      *error = "root key is empty";
    }
    return false;
  }
  std::unordered_set<std::string> visiting;
  std::unique_ptr<Node> root;
  if (!BuildEncodedNodeInternalTx(transaction,
                                  path,
                                  root_key,
                                  cf,
                                  &visiting,
                                  &root,
                                  nullptr,
                                  nullptr,
                                  nullptr,
                                  nullptr,
                                  !lazy,
                                  error)) {
    return false;
  }
  root_ = std::move(root);
  hash_caches_canonical_ = false;
  dirty_keys_.clear();
  deleted_keys_.clear();
  storage_ = nullptr;
  storage_tx_ = transaction;
  storage_cf_ = cf;
  storage_path_ = path;
  initial_root_key_ = root_key;
  has_initial_root_key_ = true;
  lazy_loading_ = lazy;
  if (root_ && root_->node_hash.empty()) {
    UpdateNodeHashOrDie(root_.get(), value_hash_fn_);
  }
  return true;
}

bool MerkTree::EnsureChildLoaded(Node* node, bool left, std::string* error) const {
  insert_profile::AddCounter(insert_profile::Counter::kEnsureChildLoadedCalls);
  if (!lazy_loading_ || node == nullptr) {
    return true;
  }
  Node::ChildMeta& meta = left ? node->left_meta : node->right_meta;
  std::unique_ptr<Node>& child = left ? node->left : node->right;
  if (child || !meta.present) {
    return true;
  }
  if (meta.key.empty()) {
    if (error) {
      *error = "child meta is present but has empty key";
    }
    return false;
  }
  if (storage_ == nullptr && storage_tx_ == nullptr) {
    if (error) {
      *error = "lazy loading requires storage";
    }
    return false;
  }
  std::unordered_set<std::string> visiting;
  std::unique_ptr<Node> loaded;
  std::string load_error;
  bool success = false;
  insert_profile::AddCounter(insert_profile::Counter::kEnsureChildLoadedMisses);
  if (storage_tx_ != nullptr) {
    success = BuildEncodedNodeInternalTx(storage_tx_,
                                         storage_path_,
                                         meta.key,
                                         storage_cf_,
                                         &visiting,
                                         &loaded,
                                         nullptr,
                                         nullptr,
                                         nullptr,
                                         nullptr,
                                         false,
                                         &load_error);
    if (!success && IsTransactionUnavailableError(load_error) && storage_ != nullptr) {
      load_error.clear();
      visiting.clear();
      success = BuildEncodedNodeInternal(storage_,
                                         storage_path_,
                                         meta.key,
                                         storage_cf_,
                                         &visiting,
                                         &loaded,
                                         nullptr,
                                         nullptr,
                                         nullptr,
                                         nullptr,
                                         false,
                                         &load_error);
    }
  } else {
    success = BuildEncodedNodeInternal(storage_,
                                  storage_path_,
                                  meta.key,
                                  storage_cf_,
                                  &visiting,
                                  &loaded,
                                  nullptr,
                                  nullptr,
                                  nullptr,
                                  nullptr,
                                  false,
                                  &load_error);
  }
  if (!success) {
    if (error) {
      *error = load_error;
    }
    return false;
  }
  child = std::move(loaded);
  meta.loaded = true;
  return true;
}

bool MerkTree::EnsureFullyLoaded(std::string* error) const {
  if (!lazy_loading_) {
    return true;
  }
  if (storage_ == nullptr && storage_tx_ == nullptr) {
    if (error) {
      *error = "lazy loading requires storage";
    }
    return false;
  }
  if (root_ == nullptr) {
    lazy_loading_ = false;
    return true;
  }
  std::unordered_set<std::string> visiting;
  std::unique_ptr<Node> root;
  if (storage_tx_ != nullptr) {
    std::string tx_error;
    if (!BuildEncodedNodeInternalTx(storage_tx_,
                                    storage_path_,
                                    root_->key,
                                    storage_cf_,
                                    &visiting,
                                    &root,
                                    nullptr,
                                    nullptr,
                                    nullptr,
                                    nullptr,
                                    true,
                                    &tx_error)) {
      if (!(IsTransactionUnavailableError(tx_error) && storage_ != nullptr)) {
        if (error) {
          *error = tx_error;
        }
        return false;
      }
      tx_error.clear();
      visiting.clear();
      if (!BuildEncodedNodeInternal(storage_,
                                    storage_path_,
                                    root_->key,
                                    storage_cf_,
                                    &visiting,
                                    &root,
                                    nullptr,
                                    nullptr,
                                    nullptr,
                                    nullptr,
                                    true,
                                    &tx_error)) {
        if (error) {
          *error = tx_error;
        }
        return false;
      }
    }
  } else {
    if (!BuildEncodedNodeInternal(storage_,
                                  storage_path_,
                                  root_->key,
                                  storage_cf_,
                                  &visiting,
                                  &root,
                                  nullptr,
                                  nullptr,
                                  nullptr,
                                  nullptr,
                                  true,
                                  error)) {
      return false;
    }
  }
  root_ = std::move(root);
  hash_caches_canonical_ = false;
  lazy_loading_ = false;
  return true;
}

std::unique_ptr<MerkTree::Node> MerkTree::CloneNode(const Node* node) {
  if (node == nullptr) {
    return nullptr;
  }
  auto copy = std::make_unique<Node>();
  copy->key = node->key;
  copy->value = node->value;
  copy->value_hash = node->value_hash;
  copy->kv_hash = node->kv_hash;
  copy->node_hash = node->node_hash;
  copy->hash_generation = node->hash_generation;
  copy->height = node->height;
  copy->left_meta = node->left_meta;
  copy->right_meta = node->right_meta;
  copy->left = CloneNode(node->left.get());
  copy->right = CloneNode(node->right.get());
  return copy;
}

}  // namespace grovedb
