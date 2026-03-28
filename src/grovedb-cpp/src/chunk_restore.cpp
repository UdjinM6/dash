#include "chunk_restore.h"

#include <memory>
#include <optional>

#include "binary.h"
#include "chunk.h"
#include "hash.h"
#include "merk_node.h"
#include "merk_storage.h"
#include "proof.h"

namespace grovedb {
namespace {

struct ProofTreeNode {
  ProofNode node;
  std::unique_ptr<ProofTreeNode> left;
  std::unique_ptr<ProofTreeNode> right;
};

enum class OpType {
  kPush,
  kPushInverted,
  kParent,
  kChild,
  kParentInverted,
  kChildInverted,
};

struct ProofOp {
  OpType type = OpType::kPush;
  ProofNode node;
};

bool DecodeKvValue(const std::vector<uint8_t>& proof,
                   size_t* cursor,
                   std::vector<uint8_t>* key,
                   std::vector<uint8_t>* value,
                   std::string* error,
                   bool large_value) {
  if (*cursor >= proof.size()) {
    if (error) {
      *error = "proof truncated";
    }
    return false;
  }
  uint8_t key_len = proof[*cursor];
  *cursor += 1;
  if (!ReadBytes(proof, cursor, key_len, key, error)) {
    return false;
  }
  if (large_value) {
    uint32_t value_len = 0;
    if (!ReadU32BE(proof, cursor, &value_len, error)) {
      return false;
    }
    return ReadBytes(proof, cursor, value_len, value, error);
  }
  uint16_t value_len = 0;
  if (!ReadU16BE(proof, cursor, &value_len, error)) {
    return false;
  }
  return ReadBytes(proof, cursor, value_len, value, error);
}

bool DecodeTreeFeatureType(const std::vector<uint8_t>& data,
                           size_t* cursor,
                           ProofNode* node,
                           std::string* error) {
  if (cursor == nullptr || node == nullptr) {
    if (error) {
      *error = "feature type output is null";
    }
    return false;
  }
  if (*cursor >= data.size()) {
    if (error) {
      *error = "feature type truncated";
    }
    return false;
  }
  uint8_t tag = data[*cursor];
  *cursor += 1;
  node->tree_feature_type = tag;
  node->has_tree_feature_type = true;
  switch (tag) {
    case 0:
      return true;
    case 1:
      return ReadVarintI64(data, cursor, &node->tree_feature_sum, error);
    case 2: {
      std::vector<uint8_t> ignored;
      if (!ReadBytes(data, cursor, 16, &ignored, error)) {
        return false;
      }
      node->tree_feature_big_sum = 0;
      for (uint8_t b : ignored) {
        node->tree_feature_big_sum = (node->tree_feature_big_sum << 8) | b;
      }
      return true;
    }
    case 3:
      return ReadVarintU64(data, cursor, &node->tree_feature_count, error);
    case 4:
      if (!ReadVarintU64(data, cursor, &node->tree_feature_count, error)) {
        return false;
      }
      return ReadVarintI64(data, cursor, &node->tree_feature_sum, error);
    case 5:
      if (!ReadVarintU64(data, cursor, &node->tree_feature_count, error)) {
        return false;
      }
      node->provable_count = true;
      node->count = node->tree_feature_count;
      return true;
    case 6:
      if (!ReadVarintU64(data, cursor, &node->tree_feature_count, error)) {
        return false;
      }
      if (!ReadVarintI64(data, cursor, &node->tree_feature_sum, error)) {
        return false;
      }
      node->provable_count = true;
      node->count = node->tree_feature_count;
      return true;
    default:
      if (error) {
        *error = "unsupported tree feature type";
      }
      return false;
  }
}

bool DecodeNextOp(const std::vector<uint8_t>& proof,
                  size_t* cursor,
                  ProofOp* out,
                  std::string* error) {
  if (cursor == nullptr || out == nullptr) {
    if (error) {
      *error = "cursor or output is null";
    }
    return false;
  }
  if (*cursor >= proof.size()) {
    if (error) {
      *error = "proof truncated";
    }
    return false;
  }
  uint8_t opcode = proof[*cursor];
  *cursor += 1;

  ProofOp op;
  switch (opcode) {
    case 0x10:
      op.type = OpType::kParent;
      break;
    case 0x11:
      op.type = OpType::kChild;
      break;
    case 0x01: {
      op.type = OpType::kPush;
      op.node.type = NodeType::kHash;
      if (!ReadBytes(proof, cursor, 32, &op.node.node_hash, error)) {
        return false;
      }
      break;
    }
    case 0x07:
    case 0x23: {
      op.type = OpType::kPush;
      op.node.type = NodeType::kKvValueHash;
      bool large_value = (opcode == 0x23);
      if (!DecodeKvValue(proof, cursor, &op.node.key, &op.node.value, error, large_value)) {
        return false;
      }
      if (!ReadBytes(proof, cursor, 32, &op.node.value_hash, error)) {
        return false;
      }
      if (!DecodeTreeFeatureType(proof, cursor, &op.node, error)) {
        return false;
      }
      break;
    }
    default:
      if (error) {
        *error = "unsupported proof opcode in chunk";
      }
      return false;
  }
  *out = std::move(op);
  return true;
}

bool BuildProofTree(const std::vector<uint8_t>& proof,
                    std::unique_ptr<ProofTreeNode>* out,
                    std::string* error) {
  std::vector<std::unique_ptr<ProofTreeNode>> stack;
  size_t cursor = 0;
  while (cursor < proof.size()) {
    ProofOp op;
    if (!DecodeNextOp(proof, &cursor, &op, error)) {
      return false;
    }
    switch (op.type) {
      case OpType::kPush:
      case OpType::kPushInverted: {
        auto node = std::make_unique<ProofTreeNode>();
        node->node = std::move(op.node);
        stack.push_back(std::move(node));
        break;
      }
      case OpType::kParent:
      case OpType::kParentInverted: {
        if (stack.size() < 2) {
          if (error) {
            *error = "proof stack underflow";
          }
          return false;
        }
        auto parent = std::move(stack.back());
        stack.pop_back();
        auto child = std::move(stack.back());
        stack.pop_back();
        if (op.type == OpType::kParent) {
          parent->left = std::move(child);
        } else {
          parent->right = std::move(child);
        }
        stack.push_back(std::move(parent));
        break;
      }
      case OpType::kChild:
      case OpType::kChildInverted: {
        if (stack.size() < 2) {
          if (error) {
            *error = "proof stack underflow";
          }
          return false;
        }
        auto child = std::move(stack.back());
        stack.pop_back();
        auto parent = std::move(stack.back());
        stack.pop_back();
        if (op.type == OpType::kChild) {
          parent->right = std::move(child);
        } else {
          parent->left = std::move(child);
        }
        stack.push_back(std::move(parent));
        break;
      }
    }
  }
  if (stack.size() != 1) {
    if (error) {
      *error = "proof did not reduce to single root";
    }
    return false;
  }
  *out = std::move(stack.back());
  return true;
}

struct HashNodeWithParent {
  std::vector<bool> path;
  std::vector<uint8_t> hash;
  std::vector<uint8_t> parent_key;
};

struct ProofNodeInfo {
  std::vector<uint8_t> hash;
  size_t height = 1;
  size_t left_height = 0;
  size_t right_height = 0;
};

void CollectHashNodesWithParent(const ProofTreeNode* node,
                                const ProofTreeNode* parent,
                                std::vector<bool>* path,
                                std::vector<HashNodeWithParent>* out) {
  if (node == nullptr) {
    return;
  }
  if (node->node.type == NodeType::kHash) {
    HashNodeWithParent entry;
    entry.path = *path;
    entry.hash = node->node.node_hash;
    if (parent && parent->node.type == NodeType::kKvValueHash) {
      entry.parent_key = parent->node.key;
    }
    out->push_back(std::move(entry));
  }
  if (node->left) {
    path->push_back(kChunkLeft);
    CollectHashNodesWithParent(node->left.get(), node, path, out);
    path->pop_back();
  }
  if (node->right) {
    path->push_back(kChunkRight);
    CollectHashNodesWithParent(node->right.get(), node, path, out);
    path->pop_back();
  }
}

AggregateData AggregateFromProofNode(const ProofNode& node) {
  AggregateData aggregate;
  if (!node.has_tree_feature_type) {
    aggregate.tag = AggregateDataTag::kNone;
    return aggregate;
  }
  switch (node.tree_feature_type) {
    case 0:
      aggregate.tag = AggregateDataTag::kNone;
      break;
    case 1:
      aggregate.tag = AggregateDataTag::kSum;
      aggregate.sum = node.tree_feature_sum;
      break;
    case 2:
      aggregate.tag = AggregateDataTag::kBigSum;
      aggregate.big_sum = node.tree_feature_big_sum;
      break;
    case 3:
      aggregate.tag = AggregateDataTag::kCount;
      aggregate.count = node.tree_feature_count;
      break;
    case 4:
      aggregate.tag = AggregateDataTag::kCountSum;
      aggregate.count = node.tree_feature_count;
      aggregate.sum = node.tree_feature_sum;
      break;
    case 5:
      aggregate.tag = AggregateDataTag::kProvableCount;
      aggregate.count = node.tree_feature_count;
      break;
    case 6:
      aggregate.tag = AggregateDataTag::kProvableCountSum;
      aggregate.count = node.tree_feature_count;
      aggregate.sum = node.tree_feature_sum;
      break;
    default:
      aggregate.tag = AggregateDataTag::kNone;
      break;
  }
  return aggregate;
}

TreeFeatureType FeatureTypeFromProofNode(const ProofNode& node) {
  TreeFeatureType feature;
  feature.tag = static_cast<TreeFeatureTypeTag>(node.tree_feature_type);
  feature.sum = node.tree_feature_sum;
  feature.big_sum = node.tree_feature_big_sum;
  feature.count = node.tree_feature_count;
  feature.sum2 = node.tree_feature_sum2;
  return feature;
}

bool EncodeAndStoreNodes(const ProofTreeNode* node,
                         RocksDbWrapper* storage,
                         const std::vector<std::vector<uint8_t>>& path,
                         ProofNodeInfo* out,
                         std::string* error) {
  if (node == nullptr) {
    if (out) {
      out->hash.clear();
      out->height = 0;
      out->left_height = 0;
      out->right_height = 0;
    }
    return true;
  }
  if (node->node.type == NodeType::kHash) {
    if (out) {
      out->hash = node->node.node_hash;
      out->height = 1;
      out->left_height = 0;
      out->right_height = 0;
    }
    return true;
  }
  ProofNodeInfo left_info;
  ProofNodeInfo right_info;
  bool has_left = node->left != nullptr;
  bool has_right = node->right != nullptr;
  if (has_left) {
    if (!EncodeAndStoreNodes(node->left.get(), storage, path, &left_info, error)) {
      return false;
    }
  }
  if (has_right) {
    if (!EncodeAndStoreNodes(node->right.get(), storage, path, &right_info, error)) {
      return false;
    }
  }
  TreeNodeInner tree_node;
  tree_node.has_left = has_left;
  tree_node.has_right = has_right;
  if (has_left) {
    tree_node.left.hash = left_info.hash;
    tree_node.left.left_height = static_cast<uint8_t>(left_info.left_height);
    tree_node.left.right_height = static_cast<uint8_t>(left_info.right_height);
    if (node->left->node.type == NodeType::kKvValueHash) {
      tree_node.left.key = node->left->node.key;
      tree_node.left.aggregate = AggregateFromProofNode(node->left->node);
    } else {
      tree_node.left.aggregate = AggregateData{};
    }
  }
  if (has_right) {
    tree_node.right.hash = right_info.hash;
    tree_node.right.left_height = static_cast<uint8_t>(right_info.left_height);
    tree_node.right.right_height = static_cast<uint8_t>(right_info.right_height);
    if (node->right->node.type == NodeType::kKvValueHash) {
      tree_node.right.key = node->right->node.key;
      tree_node.right.aggregate = AggregateFromProofNode(node->right->node);
    } else {
      tree_node.right.aggregate = AggregateData{};
    }
  }
  tree_node.kv.key = node->node.key;
  tree_node.kv.value = node->node.value;
  tree_node.kv.value_hash = node->node.value_hash;
  if (!KvDigestToKvHash(tree_node.kv.key, tree_node.kv.value_hash, &tree_node.kv.kv_hash, error)) {
    return false;
  }
  tree_node.kv.feature_type = FeatureTypeFromProofNode(node->node);

  std::vector<uint8_t> encoded;
  if (!EncodeTreeNodeInner(tree_node, &encoded, error)) {
    return false;
  }
  if (!storage->Put(ColumnFamilyKind::kDefault, path, tree_node.kv.key, encoded, error)) {
    return false;
  }
  if (out) {
    std::vector<uint8_t> zero(32, 0);
    const std::vector<uint8_t>& left_hash = has_left ? left_info.hash : zero;
    const std::vector<uint8_t>& right_hash = has_right ? right_info.hash : zero;
    if (node->node.provable_count) {
      if (!NodeHashWithCount(tree_node.kv.kv_hash,
                             left_hash,
                             right_hash,
                             node->node.count,
                             &out->hash,
                             error)) {
        return false;
      }
    } else {
      if (!NodeHash(tree_node.kv.kv_hash, left_hash, right_hash, &out->hash, error)) {
        return false;
      }
    }
    out->left_height = has_left ? left_info.height : 0;
    out->right_height = has_right ? right_info.height : 0;
    out->height = std::max(out->left_height, out->right_height) + 1;
  }
  return true;
}

std::string BytesKey(const std::vector<uint8_t>& bytes) {
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

}  // namespace

bool ChunkRestorer::Init(const std::vector<uint8_t>& expected_root_hash,
                         const std::vector<uint8_t>* parent_value_hash,
                         std::string* error) {
  if (expected_root_hash.size() != 32) {
    if (error) {
      *error = "expected root hash length mismatch";
    }
    return false;
  }
  expected_hashes_.clear();
  parent_keys_.clear();
  expected_hashes_[BytesKey({})] = expected_root_hash;
  expected_root_hash_ = expected_root_hash;
  root_key_.clear();
  has_parent_hash_ = false;
  parent_value_hash_.clear();
  if (parent_value_hash) {
    if (parent_value_hash->size() != 32) {
      if (error) {
        *error = "parent value hash length mismatch";
      }
      return false;
    }
    parent_value_hash_ = *parent_value_hash;
    has_parent_hash_ = true;
  }
  return true;
}

bool ChunkRestorer::ProcessChunkProof(const std::vector<uint8_t>& proof,
                                      std::vector<std::vector<uint8_t>>* next_chunk_ids,
                                      std::string* error) {
  return ProcessChunkProofInternal(proof, next_chunk_ids, nullptr, nullptr, error);
}

bool ChunkRestorer::ProcessChunkProofAndStore(
    const std::vector<uint8_t>& proof,
    RocksDbWrapper* storage,
    const std::vector<std::vector<uint8_t>>& path,
    std::vector<std::vector<uint8_t>>* next_chunk_ids,
    std::string* error) {
  if (storage == nullptr) {
    if (error) {
      *error = "storage is null";
    }
    return false;
  }
  return ProcessChunkProofInternal(proof, next_chunk_ids, storage, &path, error);
}

bool ChunkRestorer::ProcessChunkProofInternal(
    const std::vector<uint8_t>& proof,
    std::vector<std::vector<uint8_t>>* next_chunk_ids,
    RocksDbWrapper* storage,
    const std::vector<std::vector<uint8_t>>* path,
    std::string* error) {
  if (next_chunk_ids == nullptr) {
    if (error) {
      *error = "next chunk output is null";
    }
    return false;
  }
  next_chunk_ids->clear();
  size_t cursor = 0;
  std::vector<uint8_t> current_chunk_id;
  bool expecting_chunk_id = true;

  while (cursor < proof.size()) {
    uint8_t marker = proof[cursor++];
    uint64_t len = 0;
    if (!ReadVarintU64(proof, &cursor, &len, error)) {
      return false;
    }
    if (marker == 0) {
      if (!expecting_chunk_id) {
        if (error) {
          *error = "invalid multi chunk ordering";
        }
        return false;
      }
      if (cursor + len > proof.size()) {
        if (error) {
          *error = "chunk id truncated";
        }
        return false;
      }
      current_chunk_id.assign(proof.begin() + static_cast<long>(cursor),
                               proof.begin() + static_cast<long>(cursor + len));
      cursor += static_cast<size_t>(len);
      expecting_chunk_id = false;
      continue;
    }
    if (marker != 1) {
      if (error) {
        *error = "unsupported chunk proof marker";
      }
      return false;
    }
    if (expecting_chunk_id) {
      if (error) {
        *error = "missing chunk id for chunk";
      }
      return false;
    }
    std::vector<uint8_t> ops_bytes;
    size_t ops_start = cursor;
    size_t push_count = 0;
    size_t branch_count = 0;
    for (uint64_t i = 0; i < len; ++i) {
      ProofOp op;
      if (!DecodeNextOp(proof, &cursor, &op, error)) {
        return false;
      }
      if (op.type == OpType::kPush) {
        if (op.node.type != NodeType::kHash && op.node.type != NodeType::kKvValueHash) {
          if (error) {
            *error = "invalid chunk proof node type";
          }
          return false;
        }
        push_count += 1;
      } else if (op.type == OpType::kParent || op.type == OpType::kChild) {
        branch_count += 1;
      } else {
        if (error) {
          *error = "invalid chunk proof op";
        }
        return false;
      }
    }
    if (push_count == 0 || branch_count + 1 != push_count) {
      if (error) {
        *error = "invalid chunk proof structure";
      }
      return false;
    }
    ops_bytes.assign(proof.begin() + static_cast<long>(ops_start),
                     proof.begin() + static_cast<long>(cursor));

    auto expected_it = expected_hashes_.find(BytesKey(current_chunk_id));
    if (expected_it == expected_hashes_.end()) {
      if (error) {
        *error = "unexpected chunk";
      }
      return false;
    }
    std::string chunk_key = expected_it->first;
    std::vector<uint8_t> computed_root;
    if (!ExecuteMerkProof(ops_bytes, &computed_root, error)) {
      return false;
    }
    std::vector<uint8_t> expected_hash = expected_it->second;
    if (current_chunk_id.empty() && has_parent_hash_) {
      std::vector<uint8_t> combined;
      if (!CombineHash(parent_value_hash_, computed_root, &combined, error)) {
        return false;
      }
      if (combined != expected_hash) {
        if (error) {
          *error = "chunk doesn't match expected root hash";
        }
        return false;
      }
    } else if (computed_root != expected_hash) {
      if (error) {
        *error = "chunk doesn't match expected root hash";
      }
      return false;
    }
    expected_hashes_.erase(expected_it);

    std::unique_ptr<ProofTreeNode> proof_tree;
    if (!BuildProofTree(ops_bytes, &proof_tree, error)) {
      return false;
    }
    if (current_chunk_id.empty() && proof_tree->node.type == NodeType::kKvValueHash) {
      root_key_ = proof_tree->node.key;
    }

    auto parent_it = parent_keys_.find(chunk_key);

    if (storage != nullptr) {
      if (path == nullptr) {
        if (error) {
          *error = "storage path is null";
        }
        return false;
      }
      ProofNodeInfo info;
      if (!EncodeAndStoreNodes(proof_tree.get(), storage, *path, &info, error)) {
        return false;
      }
      if (!current_chunk_id.empty()) {
        if (parent_it != parent_keys_.end()) {
          std::vector<uint8_t> parent_bytes;
          bool parent_found = false;
          if (!storage->Get(ColumnFamilyKind::kDefault,
                            *path,
                            parent_it->second,
                            &parent_bytes,
                            &parent_found,
                            error)) {
            return false;
          }
          if (!parent_found) {
            if (error) {
              *error = "parent node not found in encoded storage";
            }
            return false;
          }
          TreeNodeInner parent_node;
          if (!DecodeTreeNodeInner(parent_bytes, &parent_node, error)) {
            return false;
          }
          if (proof_tree->node.type == NodeType::kKvValueHash) {
            std::vector<bool> chunk_prefix;
            if (!BytesToTraversalInstruction(current_chunk_id, &chunk_prefix, error)) {
              return false;
            }
            if (!chunk_prefix.empty()) {
              uint8_t side = chunk_prefix.back() ? 1 : 0;
              Link* link = side == 1 ? &parent_node.left : &parent_node.right;
              if (side == 1) {
                parent_node.has_left = true;
              } else {
                parent_node.has_right = true;
              }
              link->key = proof_tree->node.key;
              link->hash = info.hash;
              link->left_height = static_cast<uint8_t>(info.left_height);
              link->right_height = static_cast<uint8_t>(info.right_height);
              link->aggregate = AggregateFromProofNode(proof_tree->node);
              std::vector<uint8_t> updated_parent;
              if (!EncodeTreeNodeInner(parent_node, &updated_parent, error)) {
                return false;
              }
              if (!storage->Put(ColumnFamilyKind::kDefault,
                                *path,
                                parent_it->second,
                                updated_parent,
                                error)) {
                return false;
              }
              std::vector<uint8_t> meta_key = {'l', 'i', 'n', 'k', ':'};
              meta_key.insert(meta_key.end(), parent_it->second.begin(), parent_it->second.end());
              meta_key.push_back(':');
              meta_key.push_back(side);
              std::vector<uint8_t> meta_value = proof_tree->node.key;
              if (!storage->Put(ColumnFamilyKind::kMeta, *path, meta_key, meta_value, error)) {
                return false;
              }
            }
          }
        }
      }
    }
    if (parent_it != parent_keys_.end()) {
      parent_keys_.erase(parent_it);
    }
    std::vector<HashNodeWithParent> hash_nodes;
    std::vector<bool> path_bits;
    CollectHashNodesWithParent(proof_tree.get(), nullptr, &path_bits, &hash_nodes);

    std::vector<bool> chunk_prefix;
    if (!BytesToTraversalInstruction(current_chunk_id, &chunk_prefix, error)) {
      return false;
    }
    for (const auto& entry : hash_nodes) {
      std::vector<bool> child_path = chunk_prefix;
      child_path.insert(child_path.end(), entry.path.begin(), entry.path.end());
      std::vector<uint8_t> child_id = TraversalInstructionToBytes(child_path);
      std::string key = BytesKey(child_id);
      if (expected_hashes_.find(key) == expected_hashes_.end()) {
        expected_hashes_[key] = entry.hash;
        if (!entry.parent_key.empty()) {
          parent_keys_[key] = entry.parent_key;
        }
        next_chunk_ids->push_back(std::move(child_id));
      }
    }

    expecting_chunk_id = true;
  }

  if (!expecting_chunk_id) {
    if (error) {
      *error = "dangling chunk id without chunk";
    }
    return false;
  }
  return true;
}

bool ChunkRestorer::HasPendingChunks() const {
  return !expected_hashes_.empty() || !parent_keys_.empty();
}

bool ChunkRestorer::FinalizeToMerkTree(RocksDbWrapper* storage,
                                       const std::vector<std::vector<uint8_t>>& path,
                                       MerkTree* out,
                                       std::string* error) {
  if (storage == nullptr || out == nullptr) {
    if (error) {
      *error = "storage or output is null";
    }
    return false;
  }
  if (HasPendingChunks()) {
    if (error) {
      *error = "restoration not complete";
    }
    return false;
  }
  std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> meta_entries;
  if (!storage->ScanPrefix(ColumnFamilyKind::kMeta, path, &meta_entries, error)) {
    return false;
  }
  const std::vector<uint8_t> prefix = {'l', 'i', 'n', 'k', ':'};
  for (const auto& entry : meta_entries) {
    const std::vector<uint8_t>& meta_key = entry.first;
    if (meta_key.size() < prefix.size() + 2) {
      if (error) {
        *error = "invalid parent link metadata";
      }
      return false;
    }
    if (!std::equal(prefix.begin(), prefix.end(), meta_key.begin())) {
      continue;
    }
    if (meta_key[meta_key.size() - 2] != ':') {
      if (error) {
        *error = "invalid parent link metadata";
      }
      return false;
    }
    std::vector<uint8_t> parent_key(meta_key.begin() + static_cast<long>(prefix.size()),
                                    meta_key.begin() + static_cast<long>(meta_key.size() - 2));
    std::vector<uint8_t> child_key = entry.second;
    bool parent_found = false;
    bool child_found = false;
    std::string lookup_error;
    std::vector<uint8_t> ignored_value;
    if (!storage->Get(ColumnFamilyKind::kDefault, path, parent_key, &ignored_value,
                      &parent_found, &lookup_error)) {
      if (error) {
        *error = "parent link lookup failed: " + lookup_error;
      }
      return false;
    }
    ignored_value.clear();
    if (!storage->Get(ColumnFamilyKind::kDefault, path, child_key, &ignored_value, &child_found,
                      &lookup_error)) {
      if (error) {
        *error = "child link lookup failed: " + lookup_error;
      }
      return false;
    }
    if (!parent_found || !child_found) {
      if (error) {
        *error = "parent link unresolved";
      }
      return false;
    }
    bool deleted = false;
    if (!storage->Delete(ColumnFamilyKind::kMeta, path, meta_key, &deleted, error)) {
      return false;
    }
  }

  if (!root_key_.empty()) {
    if (!out->LoadEncodedTree(storage, path, root_key_, ColumnFamilyKind::kDefault, error)) {
      return false;
    }
  } else {
    if (!MerkStorage::LoadTree(storage, path, out, error)) {
      return false;
    }
    if (!out->Validate(error)) {
      std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> entries;
      if (!out->Export(&entries)) {
        if (error) {
          *error = "failed to export restored tree";
        }
        return false;
      }
      MerkTree rebuilt;
      for (const auto& kv : entries) {
        if (!rebuilt.Insert(kv.first, kv.second, error)) {
          return false;
        }
      }
      *out = std::move(rebuilt);
      if (!out->Validate(error)) {
        return false;
      }
    }
  }
  std::vector<uint8_t> root_hash;
  if (!out->ComputeRootHash(nullptr, &root_hash, error)) {
    return false;
  }
  if (root_hash != expected_root_hash_) {
    if (error) {
      *error = "restored tree hash mismatch";
    }
    return false;
  }
  return true;
}

}  // namespace grovedb
