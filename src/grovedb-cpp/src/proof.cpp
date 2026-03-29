#include "proof.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "binary.h"
#include "element.h"
#include "hash.h"
#include "query.h"

namespace grovedb {

namespace {

bool CompareKeys(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
  const size_t min_len = std::min(a.size(), b.size());
  if (min_len > 0) {
    const int cmp = std::memcmp(a.data(), b.data(), min_len);
    if (cmp != 0) {
      return cmp < 0;
    }
  }
  return a.size() < b.size();
}

int CompareKeyOrder(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
  const size_t min_len = std::min(a.size(), b.size());
  if (min_len > 0) {
    const int cmp = std::memcmp(a.data(), b.data(), min_len);
    if (cmp < 0) {
      return -1;
    }
    if (cmp > 0) {
      return 1;
    }
  }
  if (a.size() < b.size()) {
    return -1;
  }
  if (a.size() > b.size()) {
    return 1;
  }
  return 0;
}

struct ByteVectorHash {
  size_t operator()(const std::vector<uint8_t>& key) const noexcept {
    // 64-bit FNV-1a over raw bytes.
    uint64_t hash = 1469598103934665603ULL;
    for (uint8_t b : key) {
      hash ^= static_cast<uint64_t>(b);
      hash *= 1099511628211ULL;
    }
    return static_cast<size_t>(hash);
  }
};

std::string HexPrefix(const std::vector<uint8_t>& bytes, size_t max_len) {
  static const char kHex[] = "0123456789abcdef";
  size_t len = bytes.size() < max_len ? bytes.size() : max_len;
  std::string out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    uint8_t b = bytes[i];
    out.push_back(kHex[(b >> 4) & 0x0f]);
    out.push_back(kHex[b & 0x0f]);
  }
  return out;
}

std::string HexEncode(const std::vector<uint8_t>& bytes) {
  return HexPrefix(bytes, bytes.size());
}

std::string HexSlice(const std::vector<uint8_t>& bytes, size_t offset, size_t max_len) {
  if (offset >= bytes.size()) {
    return "";
  }
  std::vector<uint8_t> slice(bytes.begin() + static_cast<long>(offset), bytes.end());
  return HexPrefix(slice, max_len);
}

std::string EncodePathCacheKey(const std::vector<std::vector<uint8_t>>& path) {
  std::string out;
  for (const auto& segment : path) {
    const uint32_t len = static_cast<uint32_t>(segment.size());
    out.push_back(static_cast<char>((len >> 24) & 0xFF));
    out.push_back(static_cast<char>((len >> 16) & 0xFF));
    out.push_back(static_cast<char>((len >> 8) & 0xFF));
    out.push_back(static_cast<char>(len & 0xFF));
    out.append(reinterpret_cast<const char*>(segment.data()), segment.size());
  }
  return out;
}

bool QueryItemContainsAny(const std::vector<QueryItem>& items,
                          const std::vector<uint8_t>& key) {
  if (items.size() == 1) {
    return items.front().Contains(key);
  }
  for (const auto& item : items) {
    if (item.Contains(key)) {
      return true;
    }
  }
  return false;
}

bool QueryHasExactKeyItems(const std::vector<QueryItem>& items) {
  for (const auto& item : items) {
    if (item.IsKey()) {
      return true;
    }
  }
  return false;
}

struct VerifyPathQueryProfileTotals {
  uint64_t calls = 0;
  uint64_t decode_ns = 0;
  uint64_t root_total_ns = 0;
  uint64_t leaf_root_ns = 0;
  uint64_t link_ns = 0;
  uint64_t query_ns = 0;
  uint64_t total_ns = 0;
};

bool VerifyPathQueryProfileEnabled() {
  static const bool enabled = []() {
    const char* env = std::getenv("GROVEDB_PROOF_PROFILE");
    return env != nullptr && env[0] == '1' && env[1] == '\0';
  }();
  return enabled;
}

std::string CurrentVerifyPathQueryProfileLabel() {
  const char* label = std::getenv("GROVEDB_PROOF_PROFILE_LABEL");
  if (label == nullptr || label[0] == '\0') {
    return "default";
  }
  return std::string(label);
}

std::unordered_map<std::string, VerifyPathQueryProfileTotals>& VerifyPathQueryProfileMap() {
  static std::unordered_map<std::string, VerifyPathQueryProfileTotals> totals;
  return totals;
}

void PrintVerifyPathQueryProfileReport() {
  auto& totals = VerifyPathQueryProfileMap();
  if (totals.empty()) {
    return;
  }
  std::vector<std::pair<std::string, VerifyPathQueryProfileTotals>> rows;
  rows.reserve(totals.size());
  for (const auto& it : totals) {
    rows.emplace_back(it.first, it.second);
  }
  std::sort(rows.begin(),
            rows.end(),
            [](const auto& a, const auto& b) { return a.second.total_ns > b.second.total_ns; });
  std::cerr << "[proof-profile] label calls total_ns decode_ns root_total_ns leaf_root_ns "
               "link_ns query_ns decode_pct root_total_pct leaf_root_pct link_pct query_pct\n";
  for (const auto& row : rows) {
    const auto& label = row.first;
    const auto& t = row.second;
    if (t.calls == 0 || t.total_ns == 0) {
      continue;
    }
    const double decode_pct = static_cast<double>(t.decode_ns) * 100.0 /
                              static_cast<double>(t.total_ns);
    const double root_total_pct = static_cast<double>(t.root_total_ns) * 100.0 /
                            static_cast<double>(t.total_ns);
    const double leaf_root_pct = static_cast<double>(t.leaf_root_ns) * 100.0 /
                                 static_cast<double>(t.total_ns);
    const double link_pct = static_cast<double>(t.link_ns) * 100.0 /
                            static_cast<double>(t.total_ns);
    const double query_pct = static_cast<double>(t.query_ns) * 100.0 /
                             static_cast<double>(t.total_ns);
    std::cerr << "[proof-profile] " << label << " " << t.calls << " " << t.total_ns
              << " " << t.decode_ns << " " << t.root_total_ns << " " << t.leaf_root_ns
              << " " << t.link_ns << " " << t.query_ns << " " << decode_pct
              << " " << root_total_pct << " " << leaf_root_pct << " " << link_pct
              << " " << query_pct << "\n";
  }
}

const SubqueryBranch* ResolveSubqueryBranchForKey(const Query& query,
                                                  const std::vector<uint8_t>& key) {
  if (query.conditional_subquery_branches) {
    for (const auto& entry : *query.conditional_subquery_branches) {
      if (entry.first.Contains(key)) {
        return &entry.second;
      }
    }
  }
  if (query.default_subquery_branch.subquery ||
      query.default_subquery_branch.subquery_path) {
    return &query.default_subquery_branch;
  }
  return nullptr;
}

bool ResolveReferencePathForProof(const std::vector<std::vector<uint8_t>>& current_path,
                                  const std::vector<uint8_t>& current_key,
                                  const ReferencePathType& reference_path,
                                  std::vector<std::vector<uint8_t>>* out,
                                  std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "reference output is null";
    }
    return false;
  }
  out->clear();
  switch (reference_path.kind) {
    case ReferencePathKind::kAbsolute:
      *out = reference_path.path;
      return true;
    case ReferencePathKind::kUpstreamRootHeight: {
      size_t height = reference_path.height;
      if (height > current_path.size()) {
        if (error) {
          *error = "reference height exceeds current path";
        }
        return false;
      }
      auto offset = static_cast<std::vector<std::vector<uint8_t>>::difference_type>(height);
      out->assign(current_path.begin(), current_path.begin() + offset);
      out->insert(out->end(), reference_path.path.begin(), reference_path.path.end());
      return true;
    }
    case ReferencePathKind::kUpstreamRootHeightWithParentPathAddition: {
      size_t height = reference_path.height;
      if (current_path.empty()) {
        if (error) {
          *error = "current path is empty";
        }
        return false;
      }
      if (height > current_path.size()) {
        if (error) {
          *error = "reference height exceeds current path";
        }
        return false;
      }
      auto offset = static_cast<std::vector<std::vector<uint8_t>>::difference_type>(height);
      out->assign(current_path.begin(), current_path.begin() + offset);
      out->insert(out->end(), reference_path.path.begin(), reference_path.path.end());
      out->push_back(current_path.back());
      return true;
    }
    case ReferencePathKind::kUpstreamFromElementHeight: {
      size_t height = reference_path.height;
      if (height > current_path.size()) {
        if (error) {
          *error = "reference height exceeds current path";
        }
        return false;
      }
      auto offset =
          static_cast<std::vector<std::vector<uint8_t>>::difference_type>(current_path.size() -
                                                                          height);
      out->assign(current_path.begin(), current_path.begin() + offset);
      out->insert(out->end(), reference_path.path.begin(), reference_path.path.end());
      return true;
    }
    case ReferencePathKind::kCousin: {
      if (current_path.empty()) {
        if (error) {
          *error = "current path is empty";
        }
        return false;
      }
      out->assign(current_path.begin(), current_path.end() - 1);
      out->push_back(reference_path.key);
      out->push_back(current_key);
      return true;
    }
    case ReferencePathKind::kRemovedCousin: {
      if (current_path.empty()) {
        if (error) {
          *error = "current path is empty";
        }
        return false;
      }
      out->assign(current_path.begin(), current_path.end() - 1);
      out->insert(out->end(), reference_path.path.begin(), reference_path.path.end());
      out->push_back(current_key);
      return true;
    }
    case ReferencePathKind::kSibling: {
      out->assign(current_path.begin(), current_path.end());
      out->push_back(reference_path.key);
      return true;
    }
  }
  if (error) {
    *error = "unsupported reference path kind";
  }
  return false;
}

bool IsTreeElementVariant(const std::vector<uint8_t>& value,
                          bool* out,
                          std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "tree variant output is null";
    }
    return false;
  }
  uint64_t variant = 0;
  if (!DecodeElementVariant(value, &variant, error)) {
    return false;
  }
  switch (variant) {
    case 2:
    case 4:
    case 5:
    case 6:
    case 7:
    case 8:
    case 10:
      *out = true;
      return true;
    case 0:
    case 1:
    case 3:
    case 9:
      *out = false;
      return true;
    default:
      if (error) {
        *error = "unsupported element variant";
      }
      return false;
  }
}

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

bool DecodeNextOp(const std::vector<uint8_t>& proof,
                  size_t* cursor,
                  ProofOp* out,
                  std::string* error);
bool ComputeNodeHash(ProofNode* node,
                     std::vector<uint8_t>* out,
                     std::string* error);
bool ExecuteMerkProof(const std::vector<uint8_t>& proof,
                      const std::vector<uint8_t>* match_key,
                      ProofNode* matched,
                      std::vector<uint8_t>* root_hash,
                      std::string* error);
bool DecodeMerkProofOps(const std::vector<uint8_t>& proof,
                        std::vector<ProofOp>* out,
                        std::string* error);
bool ExecuteProofOps(const std::vector<ProofOp>& ops,
                     const std::vector<uint8_t>* match_key,
                     ProofNode* matched,
                     std::vector<uint8_t>* root_hash,
                     std::string* error);

bool DecodeLayerProof(const std::vector<uint8_t>& data,
                      size_t* cursor,
                      GroveLayerProof* out,
                      std::string* error,
                      int depth = 0) {
  if (depth > 8) {
    if (error) {
      *error = "layer proof nesting too deep";
    }
    return false;
  }
  if (!DecodeBincodeVecU8(data, cursor, &out->merk_proof, error)) {
    return false;
  }
  uint64_t map_len = 0;
  if (!ReadBincodeVarintU64(data, cursor, &map_len, error)) {
    return false;
  }
  if (map_len > 10000) {
    if (error) {
      *error = "layer proof map too large";
    }
    return false;
  }
  out->lower_layers.clear();
  out->lower_layers.reserve(static_cast<size_t>(map_len));
  for (uint64_t i = 0; i < map_len; ++i) {
    std::vector<uint8_t> key;
    if (!DecodeBincodeVecU8(data, cursor, &key, error)) {
      return false;
    }
    GroveLayerProof child;
    if (!DecodeLayerProof(data, cursor, &child, error, depth + 1)) {
      return false;
    }
    out->lower_layers.emplace_back(std::move(key), std::make_unique<GroveLayerProof>(std::move(child)));
  }
  return true;
}

bool EncodeLayerProof(const GroveLayerProof& layer,
                      std::vector<uint8_t>* out,
                      std::string* error,
                      int depth = 0) {
  if (out == nullptr) {
    if (error) {
      *error = "proof output is null";
    }
    return false;
  }
  if (depth > 8) {
    if (error) {
      *error = "layer proof nesting too deep";
    }
    return false;
  }
  EncodeBincodeVecU8(layer.merk_proof, out);
  EncodeBincodeVarintU64(static_cast<uint64_t>(layer.lower_layers.size()), out);
  for (const auto& entry : layer.lower_layers) {
    EncodeBincodeVecU8(entry.first, out);
    if (!EncodeLayerProof(*entry.second, out, error, depth + 1)) {
      return false;
    }
  }
  return true;
}

bool DecodeGroveDbProofV0(const std::vector<uint8_t>& data,
                          GroveLayerProof* out,
                          std::string* error) {
  size_t cursor = 0;
  uint64_t variant = 0;
  if (!ReadBincodeVarintU64(data, &cursor, &variant, error)) {
    return false;
  }
  if (variant != 0) {
    if (error) {
      *error = "unsupported GroveDBProof variant";
    }
    return false;
  }
  if (!DecodeLayerProof(data, &cursor, out, error)) {
    return false;
  }
  std::vector<uint8_t> prove_option;
  if (!ReadBytes(data, &cursor, 1, &prove_option, error)) {
    return false;
  }
  if (prove_option[0] > 1) {
    if (error) {
      *error = "invalid prove options encoding";
    }
    return false;
  }
  out->prove_options_tag = prove_option[0];
  if (cursor != data.size()) {
    if (error) {
      *error = "extra bytes after GroveDBProof decode";
    }
    return false;
  }
  return true;
}

bool CombineTreeValueHashLocal(const std::vector<uint8_t>& value,
                               const std::vector<uint8_t>& subtree_root,
                               std::vector<uint8_t>* out,
                               std::string* error);

bool EncodeGroveDbProofV0(const GroveLayerProof& layer,
                          std::vector<uint8_t>* out,
                          std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "proof output is null";
    }
    return false;
  }
  out->clear();
  EncodeBincodeVarintU64(0, out);  // GroveDBProof::V0
  if (!EncodeLayerProof(layer, out, error)) {
    return false;
  }
  out->push_back(layer.prove_options_tag);
  return true;
}

bool VerifyLayerLink(const std::vector<uint8_t>& layer_proof,
                     const std::vector<uint8_t>& key,
                     const std::vector<uint8_t>& child_hash,
                     std::vector<uint8_t>* out_root_hash,
                     std::string* error) {
  std::vector<ProofOp> ops;
  std::string layer_error;
  if (!DecodeMerkProofOps(layer_proof, &ops, &layer_error)) {
    if (error) {
      *error = "layer proof decode failed: " + layer_error +
               " prefix=" + HexPrefix(layer_proof, 8);
    }
    return false;
  }

  bool found_match = false;
  ProofNode layer_match;
  size_t match_index = 0;
  for (size_t i = 0; i < ops.size(); ++i) {
    const auto& op = ops[i];
    if (op.type != OpType::kPush && op.type != OpType::kPushInverted) {
      continue;
    }
    if (op.node.key == key) {
      layer_match = op.node;
      found_match = true;
      match_index = i;
    }
  }
  if (!found_match) {
    if (error) {
      *error = "path key not found in layer proof";
    }
    return false;
  }
  if (layer_match.value_hash.size() != 32) {
    if (error) {
      *error = "layer proof missing value hash";
    }
    return false;
  }
  bool is_tree = false;
  if (!IsTreeElementVariant(layer_match.value, &is_tree, error)) {
    return false;
  }
  if (!is_tree) {
    if (error) {
      *error = "layer path key does not point to tree element";
    }
    return false;
  }

  std::vector<uint8_t> combined_hash;
  if (!CombineTreeValueHashLocal(layer_match.value, child_hash, &combined_hash, error)) {
    return false;
  }
  if (match_index >= ops.size()) {
    if (error) {
      *error = "path key not found in layer proof ops";
    }
    return false;
  }
  ops[match_index].node.type = NodeType::kKvValueHash;
  ops[match_index].node.value_hash = combined_hash;

  if (out_root_hash) {
    if (!ExecuteProofOps(ops, nullptr, nullptr, out_root_hash, error)) {
      return false;
    }
  } else {
    std::vector<uint8_t> ignored_root;
    if (!ExecuteProofOps(ops, nullptr, nullptr, &ignored_root, error)) {
      return false;
    }
  }
  return true;
}

bool CombineTreeValueHashLocal(const std::vector<uint8_t>& value,
                               const std::vector<uint8_t>& subtree_root,
                               std::vector<uint8_t>* out,
                               std::string* error) {
  std::vector<uint8_t> value_hash;
  if (!ValueHash(value, &value_hash, error)) {
    return false;
  }
  return CombineHash(value_hash, subtree_root, out, error);
}

bool ComputeLayerRootHash(const GroveLayerProof& layer,
                          std::vector<uint8_t>* out_root_hash,
                          std::string* error) {
  if (out_root_hash == nullptr) {
    if (error) {
      *error = "root hash output is null";
    }
    return false;
  }
  std::unordered_map<std::vector<uint8_t>, std::vector<uint8_t>, ByteVectorHash> child_hashes;
  child_hashes.reserve(layer.lower_layers.size());
  for (const auto& entry : layer.lower_layers) {
    std::vector<uint8_t> child_root;
    if (!ComputeLayerRootHash(*entry.second, &child_root, error)) {
      return false;
    }
    child_hashes[entry.first] = std::move(child_root);
  }

  std::vector<ProofOp> ops;
  if (!DecodeMerkProofOps(layer.merk_proof, &ops, error)) {
    return false;
  }
  for (auto& op : ops) {
    if (op.type != OpType::kPush && op.type != OpType::kPushInverted) {
      continue;
    }
    auto it = child_hashes.find(op.node.key);
    if (it == child_hashes.end()) {
      continue;
    }
    std::vector<uint8_t> combined_hash;
    if (!CombineTreeValueHashLocal(op.node.value, it->second, &combined_hash, error)) {
      return false;
    }
    op.node.type = NodeType::kKvValueHash;
    op.node.value_hash = std::move(combined_hash);
  }

  return ExecuteProofOps(ops, nullptr, nullptr, out_root_hash, error);
}

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
                           bool* provable_count,
                           uint64_t* count_out,
                           std::string* error) {
  if (provable_count) {
    *provable_count = false;
  }
  if (count_out) {
    *count_out = 0;
  }
  if (cursor == nullptr || provable_count == nullptr || count_out == nullptr) {
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
  switch (tag) {
    case 0:
      return true;
    case 1: {
      int64_t ignored = 0;
      return ReadVarintI64(data, cursor, &ignored, error);
    }
    case 2: {
      std::vector<uint8_t> ignored;
      return ReadBytes(data, cursor, 16, &ignored, error);
    }
    case 3: {
      uint64_t ignored = 0;
      return ReadVarintU64(data, cursor, &ignored, error);
    }
    case 4: {
      uint64_t ignored_count = 0;
      int64_t ignored_sum = 0;
      if (!ReadVarintU64(data, cursor, &ignored_count, error)) {
        return false;
      }
      return ReadVarintI64(data, cursor, &ignored_sum, error);
    }
    case 5: {
      uint64_t count = 0;
      if (!ReadVarintU64(data, cursor, &count, error)) {
        return false;
      }
      *provable_count = true;
      *count_out = count;
      return true;
    }
    case 6: {
      uint64_t count = 0;
      int64_t ignored_sum = 0;
      if (!ReadVarintU64(data, cursor, &count, error)) {
        return false;
      }
      if (!ReadVarintI64(data, cursor, &ignored_sum, error)) {
        return false;
      }
      *provable_count = true;
      *count_out = count;
      return true;
    }
    default:
      if (error) {
        *error = "unsupported tree feature type";
      }
      return false;
  }
}

bool EncodeKvRefValueHashOp(const std::vector<uint8_t>& key,
                            const std::vector<uint8_t>& value,
                            const std::vector<uint8_t>& value_hash,
                            bool inverted,
                            bool with_count,
                            uint64_t count,
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
  if (value_hash.size() != 32) {
    if (error) {
      *error = "value hash length mismatch";
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
  uint8_t opcode = 0x06;
  if (with_count) {
    opcode = inverted ? (large_value ? 0x2d : 0x19) : (large_value ? 0x25 : 0x18);
  } else {
    opcode = inverted ? (large_value ? 0x2a : 0x0d) : (large_value ? 0x22 : 0x06);
  }
  out->push_back(opcode);
  out->push_back(static_cast<uint8_t>(key.size()));
  out->insert(out->end(), key.begin(), key.end());
  if (large_value) {
    EncodeU32BE(static_cast<uint32_t>(value.size()), out);
  } else {
    EncodeU16BE(static_cast<uint16_t>(value.size()), out);
  }
  out->insert(out->end(), value.begin(), value.end());
  out->insert(out->end(), value_hash.begin(), value_hash.end());
  if (with_count) {
    EncodeU64BE(count, out);
  }
  return true;
}

bool EncodeKvDigestRewriteOp(const std::vector<uint8_t>& key,
                             const std::vector<uint8_t>& value_hash,
                             bool inverted,
                             bool with_count,
                             uint64_t count,
                             std::vector<uint8_t>* out,
                             std::string* error) {
  if (out == nullptr) {
    if (error) *error = "proof output is null";
    return false;
  }
  if (key.size() > 255) {
    if (error) *error = "key length exceeds u8";
    return false;
  }
  if (value_hash.size() != 32) {
    if (error) *error = "value hash length mismatch";
    return false;
  }
  uint8_t opcode = 0x05;
  if (with_count) {
    opcode = 0x1a;
  } else if (inverted) {
    opcode = 0x0c;
  }
  out->push_back(opcode);
  out->push_back(static_cast<uint8_t>(key.size()));
  out->insert(out->end(), key.begin(), key.end());
  out->insert(out->end(), value_hash.begin(), value_hash.end());
  if (with_count) {
    EncodeU64BE(count, out);
  }
  return true;
}

bool EncodeKvValueHashRewriteOp(const std::vector<uint8_t>& key,
                                const std::vector<uint8_t>& value,
                                const std::vector<uint8_t>& value_hash,
                                bool inverted,
                                bool with_count,
                                uint64_t count,
                                std::vector<uint8_t>* out,
                                std::string* error) {
  if (out == nullptr) {
    if (error) *error = "proof output is null";
    return false;
  }
  if (key.size() > 255) {
    if (error) *error = "key length exceeds u8";
    return false;
  }
  if (value_hash.size() != 32) {
    if (error) *error = "value hash length mismatch";
    return false;
  }
  if (value.size() > 0xFFFFFFFFu) {
    if (error) *error = "value length exceeds u32";
    return false;
  }
  bool large_value = value.size() > 0xFFFF;
  uint8_t opcode = 0x04;
  if (with_count) {
    opcode = inverted ? (large_value ? 0x2d : 0x19) : (large_value ? 0x25 : 0x18);
  } else if (inverted) {
    opcode = large_value ? 0x29 : 0x0b;
  } else if (large_value) {
    opcode = 0x21;
  }
  out->push_back(opcode);
  out->push_back(static_cast<uint8_t>(key.size()));
  out->insert(out->end(), key.begin(), key.end());
  if (large_value) {
    EncodeU32BE(static_cast<uint32_t>(value.size()), out);
  } else {
    EncodeU16BE(static_cast<uint16_t>(value.size()), out);
  }
  out->insert(out->end(), value.begin(), value.end());
  out->insert(out->end(), value_hash.begin(), value_hash.end());
  if (with_count) {
    EncodeU64BE(count, out);
  }
  return true;
}

bool RewriteProofOpForReference(const std::vector<uint8_t>& proof,
                                size_t* cursor,
                                const std::vector<uint8_t>& target_key,
                                const std::vector<uint8_t>& reference_value,
                                const std::vector<uint8_t>& resolved_value,
                                const std::vector<uint8_t>& reference_value_hash,
                                std::vector<uint8_t>* out,
                                bool* rewritten,
                                std::string* error) {
  if (cursor == nullptr || out == nullptr || rewritten == nullptr) {
    if (error) {
      *error = "rewrite cursor or output is null";
    }
    return false;
  }
  size_t op_start = *cursor;
  if (*cursor >= proof.size()) {
    if (error) {
      *error = "proof truncated";
    }
    return false;
  }
  uint8_t opcode = proof[(*cursor)++];
  bool inverted = false;
  bool has_key_value = false;
  bool has_count = false;
  std::vector<uint8_t> key;
  std::vector<uint8_t> value;
  uint64_t count = 0;

  switch (opcode) {
    case 0x10:
    case 0x11:
    case 0x12:
    case 0x13:
      break;
    case 0x01:
    case 0x02:
    case 0x08:
    case 0x09: {
      if (!ReadBytes(proof, cursor, 32, nullptr, error)) {
        return false;
      }
      break;
    }
    case 0x03:
    case 0x20:
    case 0x04:
    case 0x21:
    case 0x06:
    case 0x22:
    case 0x07:
    case 0x23:
    case 0x14:
    case 0x24:
    case 0x18:
    case 0x25: {
      has_key_value = true;
      bool large_value = (opcode == 0x20 || opcode == 0x21 || opcode == 0x22 ||
                          opcode == 0x23 || opcode == 0x24 || opcode == 0x25);
      if (!DecodeKvValue(proof, cursor, &key, &value, error, large_value)) {
        return false;
      }
      if (opcode == 0x04 || opcode == 0x21 || opcode == 0x06 || opcode == 0x22 ||
          opcode == 0x07 || opcode == 0x23 || opcode == 0x18 || opcode == 0x25) {
        if (!ReadBytes(proof, cursor, 32, nullptr, error)) {
          return false;
        }
      }
      if (opcode == 0x07 || opcode == 0x23) {
        bool ignored = false;
        uint64_t ignored_count = 0;
        if (!DecodeTreeFeatureType(proof, cursor, &ignored, &ignored_count, error)) {
          return false;
        }
      }
      if (opcode == 0x14 || opcode == 0x24 || opcode == 0x18 || opcode == 0x25) {
        has_count = true;
        if (!ReadU64BE(proof, cursor, &count, error)) {
          return false;
        }
      }
      break;
    }
    case 0x05:
    case 0x0c:
    case 0x1a: {
      inverted = (opcode == 0x0c);
      if (*cursor >= proof.size()) {
        if (error) {
          *error = "proof truncated";
        }
        return false;
      }
      uint8_t key_len = proof[(*cursor)++];
      if (!ReadBytes(proof, cursor, key_len, &key, error)) {
        return false;
      }
      if (!ReadBytes(proof, cursor, 32, nullptr, error)) {
        return false;
      }
      if (opcode == 0x1a) {
        if (!ReadU64BE(proof, cursor, &count, error)) {
          return false;
        }
      }
      break;
    }
    case 0x0a:
    case 0x28:
    case 0x0b:
    case 0x29:
    case 0x0d:
    case 0x2a:
    case 0x0e:
    case 0x2b:
    case 0x16:
    case 0x2c:
    case 0x19:
    case 0x2d: {
      inverted = true;
      has_key_value = true;
      bool large_value = (opcode == 0x28 || opcode == 0x29 || opcode == 0x2a ||
                          opcode == 0x2b || opcode == 0x2c || opcode == 0x2d);
      if (!DecodeKvValue(proof, cursor, &key, &value, error, large_value)) {
        return false;
      }
      if (opcode == 0x0b || opcode == 0x29 || opcode == 0x0d || opcode == 0x2a ||
          opcode == 0x0e || opcode == 0x2b || opcode == 0x19 || opcode == 0x2d) {
        if (!ReadBytes(proof, cursor, 32, nullptr, error)) {
          return false;
        }
      }
      if (opcode == 0x0e || opcode == 0x2b) {
        bool ignored = false;
        uint64_t ignored_count = 0;
        if (!DecodeTreeFeatureType(proof, cursor, &ignored, &ignored_count, error)) {
          return false;
        }
      }
      if (opcode == 0x16 || opcode == 0x2c || opcode == 0x19 || opcode == 0x2d) {
        has_count = true;
        if (!ReadU64BE(proof, cursor, &count, error)) {
          return false;
        }
      }
      break;
    }
    case 0x15:
    case 0x17: {
      if (!ReadBytes(proof, cursor, 32, nullptr, error)) {
        return false;
      }
      if (!ReadU64BE(proof, cursor, &count, error)) {
        return false;
      }
      break;
    }
    default:
      if (error) {
        size_t offset = *cursor > 0 ? *cursor - 1 : 0;
        *error = "unsupported proof opcode: " + std::to_string(opcode) +
                 " at offset " + std::to_string(offset) +
                 " bytes=" + HexSlice(proof, offset, 8);
      }
      return false;
  }

  if (has_key_value && key == target_key && value == reference_value) {
    uint64_t ref_count = has_count ? count : 0;
    if (!EncodeKvRefValueHashOp(key,
                                resolved_value,
                                reference_value_hash,
                                inverted,
                                has_count,
                                ref_count,
                                out,
                                error)) {
      return false;
    }
    *rewritten = true;
    return true;
  }
  out->insert(out->end(),
              proof.begin() + static_cast<long>(op_start),
              proof.begin() + static_cast<long>(*cursor));
  return true;
}

bool RewriteProofOpForDigestKey(const std::vector<uint8_t>& proof,
                                size_t* cursor,
                                const std::vector<uint8_t>& target_key,
                                const std::vector<uint8_t>& target_value,
                                const std::vector<uint8_t>& target_value_hash,
                                std::vector<uint8_t>* out,
                                bool* rewritten,
                                std::string* error) {
  if (cursor == nullptr || out == nullptr || rewritten == nullptr) {
    if (error) {
      *error = "rewrite cursor or output is null";
    }
    return false;
  }
  size_t op_start = *cursor;
  if (*cursor >= proof.size()) {
    if (error) *error = "proof truncated";
    return false;
  }
  uint8_t opcode = proof[(*cursor)++];
  bool inverted = false;
  bool has_key_value = false;
  bool has_count = false;
  std::vector<uint8_t> key;
  std::vector<uint8_t> value;
  uint64_t count = 0;

  switch (opcode) {
    case 0x10:
    case 0x11:
    case 0x12:
    case 0x13:
      break;
    case 0x01:
    case 0x02:
    case 0x08:
    case 0x09:
    case 0x15:
    case 0x17: {
      if (!ReadBytes(proof, cursor, 32, nullptr, error)) return false;
      if (opcode == 0x15 || opcode == 0x17) {
        if (!ReadU64BE(proof, cursor, &count, error)) return false;
      }
      break;
    }
    case 0x03:
    case 0x20:
    case 0x04:
    case 0x21:
    case 0x06:
    case 0x22:
    case 0x07:
    case 0x23:
    case 0x14:
    case 0x24:
    case 0x18:
    case 0x25: {
      has_key_value = true;
      bool large_value = (opcode == 0x20 || opcode == 0x21 || opcode == 0x22 ||
                          opcode == 0x23 || opcode == 0x24 || opcode == 0x25);
      if (!DecodeKvValue(proof, cursor, &key, &value, error, large_value)) return false;
      if (opcode == 0x04 || opcode == 0x21 || opcode == 0x06 || opcode == 0x22 ||
          opcode == 0x07 || opcode == 0x23 || opcode == 0x18 || opcode == 0x25) {
        if (!ReadBytes(proof, cursor, 32, nullptr, error)) return false;
      }
      if (opcode == 0x07 || opcode == 0x23) {
        bool ignored = false;
        uint64_t ignored_count = 0;
        if (!DecodeTreeFeatureType(proof, cursor, &ignored, &ignored_count, error)) return false;
      }
      if (opcode == 0x14 || opcode == 0x24 || opcode == 0x18 || opcode == 0x25) {
        has_count = true;
        if (!ReadU64BE(proof, cursor, &count, error)) return false;
      }
      break;
    }
    case 0x05:
    case 0x0c:
    case 0x1a: {
      inverted = (opcode == 0x0c);
      if (*cursor >= proof.size()) {
        if (error) *error = "proof truncated";
        return false;
      }
      uint8_t key_len = proof[(*cursor)++];
      if (!ReadBytes(proof, cursor, key_len, &key, error)) return false;
      if (!ReadBytes(proof, cursor, 32, nullptr, error)) return false;
      if (opcode == 0x1a) {
        has_count = true;
        if (!ReadU64BE(proof, cursor, &count, error)) return false;
      }
      break;
    }
    case 0x0a:
    case 0x28:
    case 0x0b:
    case 0x29:
    case 0x0d:
    case 0x2a:
    case 0x0e:
    case 0x2b:
    case 0x16:
    case 0x2c:
    case 0x19:
    case 0x2d: {
      inverted = true;
      has_key_value = true;
      bool large_value = (opcode == 0x28 || opcode == 0x29 || opcode == 0x2a ||
                          opcode == 0x2b || opcode == 0x2c || opcode == 0x2d);
      if (!DecodeKvValue(proof, cursor, &key, &value, error, large_value)) return false;
      if (opcode == 0x0b || opcode == 0x29 || opcode == 0x0d || opcode == 0x2a ||
          opcode == 0x0e || opcode == 0x2b || opcode == 0x19 || opcode == 0x2d) {
        if (!ReadBytes(proof, cursor, 32, nullptr, error)) return false;
      }
      if (opcode == 0x0e || opcode == 0x2b) {
        bool ignored = false;
        uint64_t ignored_count = 0;
        if (!DecodeTreeFeatureType(proof, cursor, &ignored, &ignored_count, error)) return false;
      }
      if (opcode == 0x16 || opcode == 0x2c || opcode == 0x19 || opcode == 0x2d) {
        has_count = true;
        if (!ReadU64BE(proof, cursor, &count, error)) return false;
      }
      break;
    }
    default:
      if (error) {
        size_t offset = *cursor > 0 ? *cursor - 1 : 0;
        *error = "unsupported proof opcode: " + std::to_string(opcode) +
                 " at offset " + std::to_string(offset);
      }
      return false;
  }

  if (has_key_value && key == target_key && value == target_value) {
    if (!EncodeKvDigestRewriteOp(key, target_value_hash, inverted, has_count, count, out, error)) {
      return false;
    }
    *rewritten = true;
    return true;
  }
  out->insert(out->end(),
              proof.begin() + static_cast<long>(op_start),
              proof.begin() + static_cast<long>(*cursor));
  return true;
}

bool DecodeChunkProofOps(const std::vector<uint8_t>& proof,
                         std::vector<ProofOp>* out,
                         std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "chunk proof output is null";
    }
    return false;
  }
  size_t cursor = 0;
  out->clear();
  while (cursor < proof.size()) {
    uint8_t marker = proof[cursor++];
    uint64_t len = 0;
    if (!ReadVarintU64(proof, &cursor, &len, error)) {
      return false;
    }
    if (marker == 0) {
      if (cursor + len > proof.size()) {
        if (error) {
          *error = "chunk proof truncated";
        }
        return false;
      }
      cursor += static_cast<size_t>(len);
      continue;
    }
    if (marker != 1) {
      if (error) {
        *error = "unsupported chunk proof marker";
      }
      return false;
    }
    out->reserve(out->size() + static_cast<size_t>(len));
    for (uint64_t i = 0; i < len; ++i) {
      ProofOp op;
      if (!DecodeNextOp(proof, &cursor, &op, error)) {
        return false;
      }
      out->push_back(std::move(op));
    }
  }
  return true;
}

bool DecodeMerkProofOps(const std::vector<uint8_t>& proof,
                        std::vector<ProofOp>* out,
                        std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "proof ops output is null";
    }
    return false;
  }
  out->clear();
  if (proof.empty()) {
    if (error) {
      *error = "empty proof";
    }
    return false;
  }
  if (proof[0] == 0) {
    return DecodeChunkProofOps(proof, out, error);
  }
  out->reserve(proof.size() / 8);
  size_t cursor = 0;
  while (cursor < proof.size()) {
    ProofOp op;
    if (!DecodeNextOp(proof, &cursor, &op, error)) {
      return false;
    }
    out->push_back(std::move(op));
  }
  return true;
}

bool ExecuteProofOps(const std::vector<ProofOp>& ops,
                     const std::vector<uint8_t>* match_key,
                     ProofNode* matched,
                     std::vector<uint8_t>* root_hash,
                     std::string* error) {
  if (root_hash == nullptr) {
    if (error) {
      *error = "root hash output is null";
    }
    return false;
  }
  std::vector<ProofNode> stack;
  stack.reserve(ops.size());
  bool found = false;
  ProofNode found_node;
  const std::vector<uint8_t>* last_key = nullptr;
  std::vector<uint8_t> child_hash;
  child_hash.reserve(32);

  for (const auto& op : ops) {
    switch (op.type) {
      case OpType::kPush:
      case OpType::kPushInverted: {
        if (op.node.type == NodeType::kKv ||
            op.node.type == NodeType::kKvValueHash ||
            op.node.type == NodeType::kKvRefValueHash ||
            op.node.type == NodeType::kKvRefValueHashCount ||
            op.node.type == NodeType::kKvDigest) {
          if (last_key != nullptr) {
            if (op.type == OpType::kPush) {
              if (op.node.key <= *last_key) {
                if (error) {
                  *error = "incorrect key ordering";
                }
                return false;
              }
            } else {
              if (op.node.key >= *last_key) {
                if (error) {
                  *error = "incorrect key ordering inverted";
                }
                return false;
              }
            }
          }
          last_key = &op.node.key;
        }
        if (match_key && op.node.key == *match_key) {
          found = true;
          found_node = op.node;
        }
        stack.push_back(op.node);
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
        ProofNode parent = std::move(stack.back());
        stack.pop_back();
        ProofNode child = std::move(stack.back());
        stack.pop_back();
        child_hash.clear();
        if (!ComputeNodeHash(&child, &child_hash, error)) {
          return false;
        }
        if (op.type == OpType::kParent) {
          parent.left_hash = child_hash;
          parent.has_left = true;
        } else {
          parent.right_hash = child_hash;
          parent.has_right = true;
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
        ProofNode child = std::move(stack.back());
        stack.pop_back();
        ProofNode parent = std::move(stack.back());
        stack.pop_back();
        child_hash.clear();
        if (!ComputeNodeHash(&child, &child_hash, error)) {
          return false;
        }
        if (op.type == OpType::kChild) {
          parent.right_hash = child_hash;
          parent.has_right = true;
        } else {
          parent.left_hash = child_hash;
          parent.has_left = true;
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
  if (!ComputeNodeHash(&stack.back(), root_hash, error)) {
    return false;
  }
  if (matched && found) {
    *matched = found_node;
  }
  return true;
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
    case 0x12:
      op.type = OpType::kParentInverted;
      break;
    case 0x13:
      op.type = OpType::kChildInverted;
      break;
    case 0x01: {
      op.type = OpType::kPush;
      op.node.type = NodeType::kHash;
      if (!ReadBytes(proof, cursor, 32, &op.node.node_hash, error)) {
        return false;
      }
      break;
    }
    case 0x02: {
      op.type = OpType::kPush;
      op.node.type = NodeType::kKvHash;
      if (!ReadBytes(proof, cursor, 32, &op.node.kv_hash, error)) {
        return false;
      }
      break;
    }
    case 0x03:
    case 0x20: {
      op.type = OpType::kPush;
      op.node.type = NodeType::kKv;
      bool large_value = (opcode == 0x20);
      if (!DecodeKvValue(proof, cursor, &op.node.key, &op.node.value, error,
                         large_value)) {
        return false;
      }
      break;
    }
    case 0x04:
    case 0x21: {
      op.type = OpType::kPush;
      op.node.type = NodeType::kKvValueHash;
      bool large_value = (opcode == 0x21);
      if (!DecodeKvValue(proof, cursor, &op.node.key, &op.node.value, error,
                         large_value)) {
        return false;
      }
      if (!ReadBytes(proof, cursor, 32, &op.node.value_hash, error)) {
        return false;
      }
      break;
    }
    case 0x07:
    case 0x23: {
      op.type = OpType::kPush;
      op.node.type = NodeType::kKvValueHash;
      bool large_value = (opcode == 0x23);
      if (!DecodeKvValue(proof, cursor, &op.node.key, &op.node.value, error,
                         large_value)) {
        return false;
      }
      if (!ReadBytes(proof, cursor, 32, &op.node.value_hash, error)) {
        return false;
      }
      bool provable = false;
      uint64_t count = 0;
      if (!DecodeTreeFeatureType(proof, cursor, &provable, &count, error)) {
        return false;
      }
      if (provable) {
        op.node.provable_count = true;
        op.node.count = count;
      }
      break;
    }
    case 0x06:
    case 0x22: {
      op.type = OpType::kPush;
      op.node.type = NodeType::kKvRefValueHash;
      bool large_value = (opcode == 0x22);
      if (!DecodeKvValue(proof, cursor, &op.node.key, &op.node.value, error,
                         large_value)) {
        return false;
      }
      if (!ReadBytes(proof, cursor, 32, &op.node.value_hash, error)) {
        return false;
      }
      break;
    }
    case 0x05: {
      op.type = OpType::kPush;
      op.node.type = NodeType::kKvDigest;
      if (*cursor >= proof.size()) {
        if (error) {
          *error = "proof truncated";
        }
        return false;
      }
      uint8_t key_len = proof[*cursor];
      *cursor += 1;
      if (!ReadBytes(proof, cursor, key_len, &op.node.key, error)) {
        return false;
      }
      if (!ReadBytes(proof, cursor, 32, &op.node.value_hash, error)) {
        return false;
      }
      break;
    }
    case 0x14:
    case 0x24: {
      op.type = OpType::kPush;
      op.node.type = NodeType::kKv;
      bool large_value = (opcode == 0x24);
      if (!DecodeKvValue(proof, cursor, &op.node.key, &op.node.value, error,
                         large_value)) {
        return false;
      }
      if (!ReadU64BE(proof, cursor, &op.node.count, error)) {
        return false;
      }
      op.node.provable_count = true;
      break;
    }
    case 0x15: {
      op.type = OpType::kPush;
      op.node.type = NodeType::kKvHash;
      if (!ReadBytes(proof, cursor, 32, &op.node.kv_hash, error)) {
        return false;
      }
      if (!ReadU64BE(proof, cursor, &op.node.count, error)) {
        return false;
      }
      op.node.provable_count = true;
      break;
    }
    case 0x18:
    case 0x25: {
      op.type = OpType::kPush;
      op.node.type = NodeType::kKvRefValueHashCount;
      bool large_value = (opcode == 0x25);
      if (!DecodeKvValue(proof, cursor, &op.node.key, &op.node.value, error,
                         large_value)) {
        return false;
      }
      if (!ReadBytes(proof, cursor, 32, &op.node.value_hash, error)) {
        return false;
      }
      if (!ReadU64BE(proof, cursor, &op.node.count, error)) {
        return false;
      }
      op.node.provable_count = true;
      break;
    }
    case 0x1a: {
      op.type = OpType::kPush;
      op.node.type = NodeType::kKvDigest;
      if (*cursor >= proof.size()) {
        if (error) {
          *error = "proof truncated";
        }
        return false;
      }
      uint8_t key_len = proof[*cursor];
      *cursor += 1;
      if (!ReadBytes(proof, cursor, key_len, &op.node.key, error)) {
        return false;
      }
      if (!ReadBytes(proof, cursor, 32, &op.node.value_hash, error)) {
        return false;
      }
      if (!ReadU64BE(proof, cursor, &op.node.count, error)) {
        return false;
      }
      op.node.provable_count = true;
      break;
    }
    case 0x08:
    case 0x09:
    case 0x0a:
    case 0x28:
    case 0x0b:
    case 0x29:
    case 0x0c:
    case 0x0d:
    case 0x2a:
    case 0x0e:
    case 0x2b:
    case 0x16:
    case 0x17:
    case 0x19:
    case 0x1b:
    case 0x2c:
    case 0x2d: {
      op.type = OpType::kPushInverted;
      uint8_t adjusted = opcode;
      if (opcode == 0x28) {
        adjusted = 0x0a;
      } else if (opcode == 0x29) {
        adjusted = 0x0b;
      } else if (opcode == 0x2a) {
        adjusted = 0x0d;
      } else if (opcode == 0x2b) {
        adjusted = 0x0e;
      } else if (opcode == 0x2c) {
        adjusted = 0x16;
      } else if (opcode == 0x2d) {
        adjusted = 0x19;
      }
      if (adjusted == 0x08) {
        op.node.type = NodeType::kHash;
        if (!ReadBytes(proof, cursor, 32, &op.node.node_hash, error)) {
          return false;
        }
      } else if (adjusted == 0x09) {
        op.node.type = NodeType::kKvHash;
        if (!ReadBytes(proof, cursor, 32, &op.node.kv_hash, error)) {
          return false;
        }
      } else if (adjusted == 0x0a) {
        op.node.type = NodeType::kKv;
        bool large_value = (opcode == 0x28);
        if (!DecodeKvValue(proof, cursor, &op.node.key, &op.node.value, error,
                           large_value)) {
          return false;
        }
      } else if (adjusted == 0x0b || adjusted == 0x0d || adjusted == 0x0e ||
                 adjusted == 0x19) {
        op.node.type = (adjusted == 0x0d || adjusted == 0x19)
                           ? NodeType::kKvRefValueHash
                           : NodeType::kKvValueHash;
        bool large_value =
            (opcode == 0x29 || opcode == 0x2a || opcode == 0x2b || opcode == 0x2d);
        if (!DecodeKvValue(proof, cursor, &op.node.key, &op.node.value, error,
                           large_value)) {
          return false;
        }
        if (!ReadBytes(proof, cursor, 32, &op.node.value_hash, error)) {
          return false;
        }
        if (adjusted == 0x0e) {
          bool provable = false;
          uint64_t count = 0;
          if (!DecodeTreeFeatureType(proof, cursor, &provable, &count, error)) {
            return false;
          }
          if (provable) {
            op.node.provable_count = true;
            op.node.count = count;
          }
        } else if (adjusted == 0x19) {
          if (!ReadU64BE(proof, cursor, &op.node.count, error)) {
            return false;
          }
          op.node.type = NodeType::kKvRefValueHashCount;
          op.node.provable_count = true;
        }
      } else {
        if (adjusted == 0x16) {
          op.node.type = NodeType::kKv;
          bool large_value = (opcode == 0x2c);
          if (!DecodeKvValue(proof, cursor, &op.node.key, &op.node.value, error,
                             large_value)) {
            return false;
          }
          if (!ReadU64BE(proof, cursor, &op.node.count, error)) {
            return false;
          }
          op.node.provable_count = true;
        } else if (adjusted == 0x17) {
          op.node.type = NodeType::kKvHash;
          if (!ReadBytes(proof, cursor, 32, &op.node.kv_hash, error)) {
            return false;
          }
          if (!ReadU64BE(proof, cursor, &op.node.count, error)) {
            return false;
          }
          op.node.provable_count = true;
        } else {
          op.node.type = NodeType::kKvDigest;
          if (*cursor >= proof.size()) {
            if (error) {
              *error = "proof truncated";
            }
            return false;
          }
          uint8_t key_len = proof[*cursor];
          *cursor += 1;
          if (!ReadBytes(proof, cursor, key_len, &op.node.key, error)) {
            return false;
          }
          if (!ReadBytes(proof, cursor, 32, &op.node.value_hash, error)) {
            return false;
          }
          if (adjusted == 0x1b) {
            if (!ReadU64BE(proof, cursor, &op.node.count, error)) {
              return false;
            }
            op.node.provable_count = true;
          }
        }
      }
      break;
    }
    default:
      if (error) {
        size_t offset = *cursor > 0 ? *cursor - 1 : 0;
        *error = "unsupported proof opcode: " + std::to_string(opcode) +
                 " at offset " + std::to_string(offset) +
                 " bytes=" + HexSlice(proof, offset, 8);
      }
      return false;
  }
  *out = std::move(op);
  return true;
}

bool ComputeNodeHash(ProofNode* node,
                     std::vector<uint8_t>* out,
                     std::string* error) {
  if (node == nullptr || out == nullptr) {
    if (error) {
      *error = "node output is null";
    }
    return false;
  }
  if (node->type == NodeType::kHash) {
    if (node->node_hash.size() != 32) {
      if (error) {
        *error = "node hash length mismatch";
      }
      return false;
    }
    *out = node->node_hash;
    return true;
  }

  std::vector<uint8_t> kv_hash;
  if (node->type == NodeType::kKvHash) {
    kv_hash = node->kv_hash;
  } else if (node->type == NodeType::kKv) {
    if (!KvHash(node->key, node->value, &kv_hash, error)) {
      return false;
    }
  } else if (node->type == NodeType::kKvRefValueHash ||
             node->type == NodeType::kKvRefValueHashCount) {
    if (node->value_hash.size() != 32) {
      if (error) {
        *error = "value hash length mismatch";
      }
      return false;
    }
    std::vector<uint8_t> referenced_value_hash;
    if (!ValueHash(node->value, &referenced_value_hash, error)) {
      return false;
    }
    std::vector<uint8_t> combined_value_hash;
    if (!CombineHash(node->value_hash, referenced_value_hash, &combined_value_hash, error)) {
      return false;
    }
    if (!KvDigestToKvHash(node->key, combined_value_hash, &kv_hash, error)) {
      return false;
    }
  } else if (node->type == NodeType::kKvValueHash ||
             node->type == NodeType::kKvDigest) {
    if (node->value_hash.size() != 32) {
      if (error) {
        *error = "value hash length mismatch";
      }
      return false;
    }
    if (!KvDigestToKvHash(node->key, node->value_hash, &kv_hash, error)) {
      return false;
    }
  } else {
    if (error) {
      *error = "unsupported node type";
    }
    return false;
  }

  if (kv_hash.size() != 32) {
    if (error) {
      *error = "kv hash length mismatch";
    }
    return false;
  }

  static const std::vector<uint8_t> kZeroHash(32, 0);
  const std::vector<uint8_t>& left = node->has_left ? node->left_hash : kZeroHash;
  const std::vector<uint8_t>& right = node->has_right ? node->right_hash : kZeroHash;
  if (node->provable_count) {
    return NodeHashWithCount(kv_hash, left, right, node->count, out, error);
  }
  return NodeHash(kv_hash, left, right, out, error);
}

bool ExecuteMerkProof(const std::vector<uint8_t>& proof,
                      const std::vector<uint8_t>* match_key,
                      ProofNode* matched,
                      std::vector<uint8_t>* root_hash,
                      std::string* error) {
  if (proof.empty()) {
    if (error) {
      *error = "empty proof";
    }
    return false;
  }
  std::vector<ProofOp> ops;
  if (proof[0] == 0) {
    if (!DecodeChunkProofOps(proof, &ops, error)) {
      return false;
    }
    return ExecuteProofOps(ops, match_key, matched, root_hash, error);
  }

  size_t cursor = 0;
  while (cursor < proof.size()) {
    ProofOp op;
    if (!DecodeNextOp(proof, &cursor, &op, error)) {
      return false;
    }
    ops.push_back(std::move(op));
  }
  return ExecuteProofOps(ops, match_key, matched, root_hash, error);
}

}  // namespace

bool CollectKvNodes(const std::vector<uint8_t>& proof,
                    std::vector<ProofNode>* out,
                    std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "kv output is null";
    }
    return false;
  }
  std::vector<ProofOp> ops;
  if (!DecodeMerkProofOps(proof, &ops, error)) {
    return false;
  }
  out->clear();
  for (const auto& op : ops) {
    if (op.type == OpType::kPush && op.node.type == NodeType::kKv) {
      out->push_back(op.node);
    }
  }
  return true;
}

bool DecodeGroveDbProof(const std::vector<uint8_t>& data,
                        GroveLayerProof* out,
                        std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "grove proof output is null";
    }
    return false;
  }
  return DecodeGroveDbProofV0(data, out, error);
}

bool EncodeGroveDbProof(const GroveLayerProof& layer,
                        std::vector<uint8_t>* out,
                        std::string* error) {
  return EncodeGroveDbProofV0(layer, out, error);
}

bool CollectProofKeys(const std::vector<uint8_t>& proof,
                      std::vector<std::vector<uint8_t>>* keys,
                      std::string* error) {
  if (keys == nullptr) {
    if (error) {
      *error = "keys output is null";
    }
    return false;
  }
  std::vector<ProofOp> ops;
  if (!DecodeMerkProofOps(proof, &ops, error)) {
    return false;
  }
  keys->clear();
  for (const auto& op : ops) {
    if (op.type != OpType::kPush && op.type != OpType::kPushInverted) {
      continue;
    }
    switch (op.node.type) {
      case NodeType::kKv:
      case NodeType::kKvValueHash:
      case NodeType::kKvRefValueHash:
      case NodeType::kKvRefValueHashCount:
      case NodeType::kKvDigest:
        keys->push_back(op.node.key);
        break;
      default:
        break;
    }
  }
  return true;
}

bool VerifySingleKeyProof(const SingleKeyProofInput& input, std::string* error) {
  if (input.proof.empty()) {
    if (error) {
      *error = "missing grove proof bytes";
    }
    return false;
  }
  if (input.key.empty()) {
    if (error) {
      *error = "missing key";
    }
    return false;
  }

  GroveLayerProof root_layer;
  if (!DecodeGroveDbProofV0(input.proof, &root_layer, error)) {
    return false;
  }
  std::vector<std::vector<uint8_t>> path = input.path;
  if (path.empty() && !input.subtree_key.empty()) {
    path.push_back(input.subtree_key);
  }
  if (root_layer.lower_layers.empty()) {
    ProofNode subtree_match;
    std::vector<uint8_t> subtree_root_hash;
    std::string subtree_error;
    if (!ExecuteMerkProof(root_layer.merk_proof,
                          &input.key,
                          &subtree_match,
                          &subtree_root_hash,
                          &subtree_error)) {
      if (error) {
        *error = "subtree proof decode failed: " + subtree_error +
                 " prefix=" + HexPrefix(root_layer.merk_proof, 8);
      }
      return false;
    }
    if (!subtree_match.key.empty()) {
      if (error) {
        *error = "key unexpectedly present in subtree proof";
      }
      return false;
    }
    if (subtree_root_hash != input.root_hash) {
      if (error) {
        *error = "computed root hash does not match expected root hash";
      }
      return false;
    }
    return true;
  }

  const GroveLayerProof* current = &root_layer;
  std::vector<const GroveLayerProof*> layers;
  layers.push_back(current);
  for (const auto& key : path) {
    const GroveLayerProof* next = nullptr;
    for (const auto& entry : current->lower_layers) {
      if (entry.first == key) {
        next = entry.second.get();
        break;
      }
    }
    if (next == nullptr) {
      if (error) {
        *error = "path key not found in lower layers";
      }
      return false;
    }
    layers.push_back(next);
    current = next;
  }
  if (layers.size() < 2) {
    if (error) {
      *error = "missing subtree layer";
    }
    return false;
  }
  const std::vector<uint8_t>& decoded_subtree_proof =
      layers.back()->merk_proof;

  ProofNode subtree_match;
  std::vector<uint8_t> subtree_root_hash;
  std::string subtree_error;
  if (!ExecuteMerkProof(decoded_subtree_proof, &input.key, &subtree_match,
                        &subtree_root_hash, &subtree_error)) {
    if (error) {
      *error = "subtree proof decode failed: " + subtree_error +
               " prefix=" + HexPrefix(decoded_subtree_proof, 8);
    }
    return false;
  }
  if (subtree_match.key.empty()) {
    if (error) {
      *error = "key not found in subtree proof";
    }
    return false;
  }
  if (subtree_match.value != input.element_bytes) {
    if (error) {
      *error = "element bytes do not match proof value";
    }
    return false;
  }

  std::vector<uint8_t> child_hash = subtree_root_hash;
  for (size_t i = layers.size() - 1; i-- > 0;) {
    const std::vector<uint8_t>& layer_proof = layers[i]->merk_proof;
    const std::vector<uint8_t>& layer_key = path[i];
    std::vector<uint8_t> computed_root_hash;
    if (!VerifyLayerLink(layer_proof,
                         layer_key,
                         child_hash,
                         &computed_root_hash,
                         error)) {
      return false;
    }
    if (i == 0) {
      if (computed_root_hash != input.root_hash) {
        if (error) {
          *error = "computed root hash does not match expected root hash";
        }
        return false;
      }
    }
    child_hash = std::move(computed_root_hash);
  }

  return true;
}

bool VerifySingleKeyAbsenceProof(const SingleKeyProofInput& input, std::string* error) {
  if (input.proof.empty()) {
    if (error) {
      *error = "missing grove proof bytes";
    }
    return false;
  }
  if (input.key.empty()) {
    if (error) {
      *error = "missing key";
    }
    return false;
  }

  GroveLayerProof root_layer;
  if (!DecodeGroveDbProofV0(input.proof, &root_layer, error)) {
    return false;
  }
  std::vector<std::vector<uint8_t>> path = input.path;
  if (path.empty() && !input.subtree_key.empty()) {
    path.push_back(input.subtree_key);
  }
  if (path.empty() && root_layer.lower_layers.empty()) {
    ProofNode subtree_match;
    std::vector<uint8_t> subtree_root_hash;
    std::string subtree_error;
    if (!ExecuteMerkProof(root_layer.merk_proof,
                          &input.key,
                          &subtree_match,
                          &subtree_root_hash,
                          &subtree_error)) {
      if (error) {
        *error = "subtree proof decode failed: " + subtree_error +
                 " prefix=" + HexPrefix(root_layer.merk_proof, 8);
      }
      return false;
    }
    if (!subtree_match.key.empty()) {
      if (error) {
        *error = "key unexpectedly present in subtree proof";
      }
      return false;
    }
    if (subtree_root_hash != input.root_hash) {
      if (error) {
        *error = "computed root hash does not match expected root hash";
      }
      return false;
    }
    return true;
  }

  const GroveLayerProof* current = &root_layer;
  std::vector<const GroveLayerProof*> layers;
  layers.push_back(current);
  for (const auto& key : path) {
    const GroveLayerProof* next = nullptr;
    for (const auto& entry : current->lower_layers) {
      if (entry.first == key) {
        next = entry.second.get();
        break;
      }
    }
    if (next == nullptr) {
      if (error) {
        *error = "path key not found in lower layers";
      }
      return false;
    }
    layers.push_back(next);
    current = next;
  }
  if (layers.size() < 2) {
    if (error) {
      *error = "missing subtree layer";
    }
    return false;
  }
  const std::vector<uint8_t>& decoded_subtree_proof =
      layers.back()->merk_proof;

  ProofNode subtree_match;
  std::vector<uint8_t> subtree_root_hash;
  std::string subtree_error;
  if (!ExecuteMerkProof(decoded_subtree_proof, &input.key, &subtree_match,
                        &subtree_root_hash, &subtree_error)) {
    if (error) {
      *error = "subtree proof decode failed: " + subtree_error +
               " prefix=" + HexPrefix(decoded_subtree_proof, 8);
    }
    return false;
  }
  if (!subtree_match.key.empty()) {
    if (error) {
      *error = "key unexpectedly present in subtree proof";
    }
    return false;
  }

  std::vector<uint8_t> child_hash = subtree_root_hash;
  for (size_t i = layers.size() - 1; i-- > 0;) {
    const std::vector<uint8_t>& layer_proof = layers[i]->merk_proof;
    const std::vector<uint8_t>& layer_key = path[i];
    std::vector<uint8_t> computed_root_hash;
    if (!VerifyLayerLink(layer_proof,
                         layer_key,
                         child_hash,
                         &computed_root_hash,
                         error)) {
      return false;
    }
    if (i == 0) {
      if (computed_root_hash != input.root_hash) {
        if (error) {
          *error = "computed root hash does not match expected root hash";
        }
        return false;
      }
    }
    child_hash = std::move(computed_root_hash);
  }

  return true;
}

bool VerifyRangeProof(const RangeProofInput& input, std::string* error) {
  if (input.proof.empty()) {
    if (error) {
      *error = "missing grove proof bytes";
    }
    return false;
  }
  if (input.path.empty() && input.subtree_key.empty()) {
    if (error) {
      *error = "missing path";
    }
    return false;
  }
  if (input.expected_keys.size() != input.expected_element_bytes.size()) {
    if (error) {
      *error = "expected key/value list size mismatch";
    }
    return false;
  }

  GroveLayerProof root_layer;
  if (!DecodeGroveDbProofV0(input.proof, &root_layer, error)) {
    return false;
  }
  std::vector<std::vector<uint8_t>> path = input.path;
  if (path.empty()) {
    path.push_back(input.subtree_key);
  }

  const GroveLayerProof* current = &root_layer;
  std::vector<const GroveLayerProof*> layers;
  layers.push_back(current);
  for (const auto& key : path) {
    const GroveLayerProof* next = nullptr;
    for (const auto& entry : current->lower_layers) {
      if (entry.first == key) {
        next = entry.second.get();
        break;
      }
    }
    if (next == nullptr) {
      if (error) {
        *error = "path key not found in lower layers";
      }
      return false;
    }
    layers.push_back(next);
    current = next;
  }
  if (layers.size() < 2) {
    if (error) {
      *error = "missing subtree layer";
    }
    return false;
  }

  const std::vector<uint8_t>& subtree_proof = layers.back()->merk_proof;
  std::vector<ProofNode> kv_nodes;
  if (!CollectKvNodes(subtree_proof, &kv_nodes, error)) {
    return false;
  }

  std::vector<std::vector<uint8_t>> actual_keys;
  std::vector<std::vector<uint8_t>> actual_values;
  for (const auto& node : kv_nodes) {
    int cmp_start = CompareKeyOrder(node.key, input.start_key);
    int cmp_end = CompareKeyOrder(node.key, input.end_key);
    bool after_start = input.start_inclusive ? (cmp_start >= 0) : (cmp_start > 0);
    bool before_end = input.end_inclusive ? (cmp_end <= 0) : (cmp_end < 0);
    if (after_start && before_end) {
      actual_keys.push_back(node.key);
      actual_values.push_back(node.value);
    }
  }
  if (actual_keys.size() != input.expected_keys.size()) {
    if (error) {
      *error = "range proof result count mismatch";
    }
    return false;
  }
  for (size_t i = 0; i < actual_keys.size(); ++i) {
    if (actual_keys[i] != input.expected_keys[i] ||
        actual_values[i] != input.expected_element_bytes[i]) {
      if (error) {
        *error = "range proof result mismatch";
      }
      return false;
    }
  }

  ProofNode subtree_match;
  std::vector<uint8_t> subtree_root_hash;
  std::string subtree_error;
  if (!ExecuteMerkProof(subtree_proof, nullptr, &subtree_match, &subtree_root_hash,
                        &subtree_error)) {
    if (error) {
      *error = "subtree proof decode failed: " + subtree_error +
               " prefix=" + HexPrefix(subtree_proof, 8);
    }
    return false;
  }

  std::vector<uint8_t> child_hash = subtree_root_hash;
  for (size_t i = layers.size() - 1; i-- > 0;) {
    const std::vector<uint8_t>& layer_proof = layers[i]->merk_proof;
    const std::vector<uint8_t>& layer_key = path[i];
    std::vector<uint8_t> computed_root_hash;
    if (!VerifyLayerLink(layer_proof,
                         layer_key,
                         child_hash,
                         &computed_root_hash,
                         error)) {
      return false;
    }
    if (i == 0) {
      if (computed_root_hash != input.root_hash) {
        if (error) {
          *error = "computed root hash does not match expected root hash";
        }
        return false;
      }
    }
    child_hash = std::move(computed_root_hash);
  }

  return true;
}

bool VerifyPathQueryProofLayer(const GroveLayerProof& layer,
                               const std::vector<std::vector<uint8_t>>& path,
                               const Query& query,
                               std::optional<uint16_t> limit,
                               std::optional<uint16_t> offset,
                               std::vector<VerifiedPathKeyElement>* out_elements,
                               std::string* error);

using PathKeyPair = std::pair<std::vector<std::vector<uint8_t>>, std::vector<uint8_t>>;

struct DecodedLayerIndex {
  std::unordered_map<std::vector<uint8_t>, std::vector<uint8_t>, ByteVectorHash> value_by_key;
  std::unordered_map<std::vector<uint8_t>, std::vector<uint8_t>, ByteVectorHash>
      exact_key_fallback_values;
  std::unordered_map<std::vector<uint8_t>, NodeType, ByteVectorHash> exact_key_fallback_types;
  std::vector<std::vector<uint8_t>> proof_keys;
};

bool IsProofKeyNodeType(NodeType type) {
  switch (type) {
    case NodeType::kKv:
    case NodeType::kKvValueHash:
    case NodeType::kKvRefValueHash:
    case NodeType::kKvRefValueHashCount:
    case NodeType::kKvDigest:
      return true;
    default:
      return false;
  }
}

template <typename OpConsumer>
bool ScanMerkProofOps(const std::vector<uint8_t>& proof,
                      OpConsumer&& consume_op,
                      std::string* error) {
  if (proof.empty()) {
    if (error) {
      *error = "empty proof";
    }
    return false;
  }
  size_t cursor = 0;
  if (proof[0] == 0) {
    while (cursor < proof.size()) {
      if (cursor >= proof.size()) {
        if (error) {
          *error = "chunk proof truncated";
        }
        return false;
      }
      uint8_t marker = proof[cursor++];
      uint64_t len = 0;
      if (!ReadVarintU64(proof, &cursor, &len, error)) {
        return false;
      }
      if (marker == 0) {
        if (cursor + len > proof.size()) {
          if (error) {
            *error = "chunk proof truncated";
          }
          return false;
        }
        cursor += static_cast<size_t>(len);
        continue;
      }
      if (marker != 1) {
        if (error) {
          *error = "unsupported chunk proof marker";
        }
        return false;
      }
      for (uint64_t i = 0; i < len; ++i) {
        ProofOp op;
        if (!DecodeNextOp(proof, &cursor, &op, error)) {
          return false;
        }
        consume_op(op);
      }
    }
    return true;
  }

  while (cursor < proof.size()) {
    ProofOp op;
    if (!DecodeNextOp(proof, &cursor, &op, error)) {
      return false;
    }
    consume_op(op);
  }
  return true;
}

bool FindSingleKeyValueInMerkProof(const std::vector<uint8_t>& proof,
                                   const std::vector<uint8_t>& query_key,
                                   std::vector<uint8_t>* out_value,
                                   bool* out_has_value,
                                   std::string* error) {
  if (out_value == nullptr || out_has_value == nullptr) {
    if (error) {
      *error = "single-key output is null";
    }
    return false;
  }
  out_value->clear();
  *out_has_value = false;
  bool saw_matching_key = false;
  bool has_materialized = false;
  bool has_fallback = false;
  std::vector<uint8_t> materialized_value;
  std::vector<uint8_t> fallback_value;

  auto scan_op = [&](const ProofOp& op) {
    if (op.type != OpType::kPush && op.type != OpType::kPushInverted) {
      return;
    }
    if (!IsProofKeyNodeType(op.node.type)) {
      return;
    }
    if (op.node.key != query_key) {
      return;
    }
    saw_matching_key = true;
    if (op.node.type == NodeType::kKv && !has_materialized) {
      materialized_value = op.node.value;
      has_materialized = true;
      return;
    }
    if ((op.node.type == NodeType::kKvValueHash ||
         op.node.type == NodeType::kKvRefValueHash ||
         op.node.type == NodeType::kKvRefValueHashCount) &&
        !has_fallback) {
      fallback_value = op.node.value;
      has_fallback = true;
    }
  };

  if (!ScanMerkProofOps(proof, scan_op, error)) {
    return false;
  }

  if (!saw_matching_key) {
    return true;
  }
  if (has_materialized) {
    *out_value = std::move(materialized_value);
    *out_has_value = true;
    return true;
  }
  if (has_fallback) {
    *out_value = std::move(fallback_value);
    *out_has_value = true;
  }
  return true;
}

bool IsSimpleSingleKeyNoSubquery(const Query& query) {
  if (query.items.size() != 1) {
    return false;
  }
  if (!query.items.front().IsKey()) {
    return false;
  }
  if (query.conditional_subquery_branches.has_value()) {
    return false;
  }
  if (query.default_subquery_branch.subquery != nullptr) {
    return false;
  }
  if (query.default_subquery_branch.subquery_path.has_value()) {
    return false;
  }
  return true;
}

bool IsSimpleSingleRangeNoSubquery(const Query& query) {
  if (query.items.size() != 1) {
    return false;
  }
  if (!query.items.front().IsRange()) {
    return false;
  }
  if (query.conditional_subquery_branches.has_value()) {
    return false;
  }
  if (query.default_subquery_branch.subquery != nullptr) {
    return false;
  }
  if (query.default_subquery_branch.subquery_path.has_value()) {
    return false;
  }
  return true;
}

bool KeyMatchesSimpleSingleRangeItem(const QueryItem& item,
                                     const std::vector<uint8_t>& key) {
  switch (item.type) {
    case QueryItemType::kRange:
      return CompareKeyOrder(key, item.start) >= 0 &&
             CompareKeyOrder(key, item.end) < 0;
    case QueryItemType::kRangeInclusive:
      return CompareKeyOrder(key, item.start) >= 0 &&
             CompareKeyOrder(key, item.end) <= 0;
    case QueryItemType::kRangeFull:
      return true;
    case QueryItemType::kRangeFrom:
      return CompareKeyOrder(key, item.start) >= 0;
    case QueryItemType::kRangeTo:
      return CompareKeyOrder(key, item.end) < 0;
    case QueryItemType::kRangeToInclusive:
      return CompareKeyOrder(key, item.end) <= 0;
    case QueryItemType::kRangeAfter:
      return CompareKeyOrder(key, item.start) > 0;
    case QueryItemType::kRangeAfterTo:
      return CompareKeyOrder(key, item.start) > 0 &&
             CompareKeyOrder(key, item.end) < 0;
    case QueryItemType::kRangeAfterToInclusive:
      return CompareKeyOrder(key, item.start) > 0 &&
             CompareKeyOrder(key, item.end) <= 0;
    case QueryItemType::kKey:
      return false;
  }
  return false;
}

bool VerifySimpleSingleKeyLayer(const GroveLayerProof& layer,
                                const std::vector<std::vector<uint8_t>>& path,
                                const Query& query,
                                std::optional<uint16_t> limit,
                                std::optional<uint16_t> offset,
                                std::vector<VerifiedPathKeyElement>* out_elements,
                                std::string* error) {
  if (out_elements == nullptr) {
    if (error) {
      *error = "query output is null";
    }
    return false;
  }
  if (offset.has_value() && *offset > 0) {
    return true;
  }
  if (limit.has_value() && *limit == 0) {
    return true;
  }

  const std::vector<uint8_t>& query_key = query.items.front().start;
  std::vector<uint8_t> materialized_value;
  bool has_value = false;
  if (!FindSingleKeyValueInMerkProof(layer.merk_proof,
                                     query_key,
                                     &materialized_value,
                                     &has_value,
                                     error)) {
    return false;
  }
  if (!has_value) {
    return true;
  }

  VerifiedPathKeyElement element;
  element.path = path;
  element.key = query_key;
  element.has_element = true;
  element.element_bytes = std::move(materialized_value);
  out_elements->push_back(std::move(element));
  return true;
}

bool VerifySimpleSingleRangeLayer(const GroveLayerProof& layer,
                                  const std::vector<std::vector<uint8_t>>& path,
                                  const Query& query,
                                  std::optional<uint16_t> limit,
                                  std::optional<uint16_t> offset,
                                  std::vector<VerifiedPathKeyElement>* out_elements,
                                  std::string* error) {
  if (out_elements == nullptr) {
    if (error) {
      *error = "query output is null";
    }
    return false;
  }
  if (limit.has_value() && *limit == 0) {
    return true;
  }

  const QueryItem& range_item = query.items.front();
  if (!range_item.IsRange()) {
    if (error) {
      *error = "range fast path requires range query item";
    }
    return false;
  }
  std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> matched_kv;
  matched_kv.reserve(64);
  std::unordered_set<std::vector<uint8_t>, ByteVectorHash> seen_matched_keys;
  if (!ScanMerkProofOps(layer.merk_proof,
                        [&](const ProofOp& op) {
                          if (op.type != OpType::kPush && op.type != OpType::kPushInverted) {
                            return;
                          }
                          if (op.node.type != NodeType::kKv) {
                            return;
                          }
                          if (!KeyMatchesSimpleSingleRangeItem(range_item, op.node.key)) {
                            return;
                          }
                          if (seen_matched_keys.insert(op.node.key).second) {
                            matched_kv.emplace_back(op.node.key, op.node.value);
                          }
                        },
                        error)) {
    return false;
  }

  std::sort(matched_kv.begin(),
            matched_kv.end(),
            [](const auto& a, const auto& b) { return CompareKeys(a.first, b.first); });
  if (!query.left_to_right) {
    std::reverse(matched_kv.begin(), matched_kv.end());
  }

  size_t start_index = 0;
  if (offset && *offset > 0) {
    start_index = std::min(static_cast<size_t>(*offset), matched_kv.size());
  }
  size_t remaining = matched_kv.size() - start_index;
  if (limit) {
    remaining = std::min(remaining, static_cast<size_t>(*limit));
  }
  size_t end_index = start_index + remaining;
  for (size_t i = start_index; i < end_index; ++i) {
    VerifiedPathKeyElement element;
    element.path = path;
    element.key = matched_kv[i].first;
    element.has_element = true;
    element.element_bytes = matched_kv[i].second;
    out_elements->push_back(std::move(element));
  }
  return true;
}

bool BuildSimpleLeafFastPathResult(const GroveLayerProof& leaf_layer,
                                   const std::vector<std::vector<uint8_t>>& path,
                                   const Query& query,
                                   std::optional<uint16_t> limit,
                                   std::vector<uint8_t>* out_leaf_root_hash,
                                   std::vector<VerifiedPathKeyElement>* out_elements,
                                   std::string* error) {
  if (out_leaf_root_hash == nullptr || out_elements == nullptr) {
    if (error) {
      *error = "leaf fast path output is null";
    }
    return false;
  }
  std::vector<ProofOp> ops;
  if (!DecodeMerkProofOps(leaf_layer.merk_proof, &ops, error)) {
    return false;
  }
  if (!ExecuteProofOps(ops, nullptr, nullptr, out_leaf_root_hash, error)) {
    return false;
  }
  out_elements->clear();
  if (limit.has_value() && *limit == 0) {
    return true;
  }

  if (IsSimpleSingleKeyNoSubquery(query)) {
    const std::vector<uint8_t>& query_key = query.items.front().start;
    const std::vector<uint8_t>* materialized_value = nullptr;
    const std::vector<uint8_t>* fallback_value = nullptr;
    bool saw_matching_key = false;
    for (const auto& op : ops) {
      if (op.type != OpType::kPush && op.type != OpType::kPushInverted) {
        continue;
      }
      if (!IsProofKeyNodeType(op.node.type)) {
        continue;
      }
      if (op.node.key != query_key) {
        continue;
      }
      saw_matching_key = true;
      if (op.node.type == NodeType::kKv && materialized_value == nullptr) {
        materialized_value = &op.node.value;
      } else if ((op.node.type == NodeType::kKvValueHash ||
                  op.node.type == NodeType::kKvRefValueHash ||
                  op.node.type == NodeType::kKvRefValueHashCount) &&
                 fallback_value == nullptr) {
        fallback_value = &op.node.value;
      }
    }
    if (!saw_matching_key) {
      return true;
    }
    if (materialized_value == nullptr) {
      materialized_value = fallback_value;
    }
    if (materialized_value == nullptr) {
      return true;
    }
    VerifiedPathKeyElement element;
    element.path = path;
    element.key = query_key;
    element.has_element = true;
    element.element_bytes = *materialized_value;
    out_elements->push_back(std::move(element));
    return true;
  }

  if (IsSimpleSingleRangeNoSubquery(query)) {
    const QueryItem& range_item = query.items.front();
    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> matched_kv;
    matched_kv.reserve(64);
    std::unordered_set<std::vector<uint8_t>, ByteVectorHash> seen_matched_keys;
    for (const auto& op : ops) {
      if (op.type != OpType::kPush && op.type != OpType::kPushInverted) {
        continue;
      }
      if (op.node.type != NodeType::kKv) {
        continue;
      }
      if (!KeyMatchesSimpleSingleRangeItem(range_item, op.node.key)) {
        continue;
      }
      if (seen_matched_keys.insert(op.node.key).second) {
        matched_kv.emplace_back(op.node.key, op.node.value);
      }
    }
    std::sort(matched_kv.begin(),
              matched_kv.end(),
              [](const auto& a, const auto& b) { return CompareKeys(a.first, b.first); });
    if (!query.left_to_right) {
      std::reverse(matched_kv.begin(), matched_kv.end());
    }

    size_t remaining = matched_kv.size();
    if (limit) {
      remaining = std::min(remaining, static_cast<size_t>(*limit));
    }
    for (size_t i = 0; i < remaining; ++i) {
      VerifiedPathKeyElement element;
      element.path = path;
      element.key = matched_kv[i].first;
      element.has_element = true;
      element.element_bytes = matched_kv[i].second;
      out_elements->push_back(std::move(element));
    }
    return true;
  }

  if (error) {
    *error = "query is not supported by simple leaf fast path";
  }
  return false;
}

bool BuildDecodedLayerIndex(const std::vector<uint8_t>& merk_proof,
                            bool include_exact_fallbacks,
                            DecodedLayerIndex* out,
                            std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "decoded layer output is null";
    }
    return false;
  }
  std::vector<ProofOp> ops;
  if (!DecodeMerkProofOps(merk_proof, &ops, error)) {
    return false;
  }

  out->value_by_key.clear();
  out->exact_key_fallback_values.clear();
  out->exact_key_fallback_types.clear();
  out->proof_keys.clear();
  out->value_by_key.reserve(ops.size());
  out->proof_keys.reserve(ops.size());
  if (include_exact_fallbacks) {
    out->exact_key_fallback_values.reserve(ops.size() / 2);
    out->exact_key_fallback_types.reserve(ops.size() / 2);
  }

  for (const auto& op : ops) {
    if (op.type != OpType::kPush && op.type != OpType::kPushInverted) {
      continue;
    }

    switch (op.node.type) {
      case NodeType::kKv:
      case NodeType::kKvValueHash:
      case NodeType::kKvRefValueHash:
      case NodeType::kKvRefValueHashCount:
      case NodeType::kKvDigest:
        out->proof_keys.push_back(op.node.key);
        break;
      default:
        break;
    }

    if (op.node.type == NodeType::kKv) {
      if (out->value_by_key.find(op.node.key) == out->value_by_key.end()) {
        out->value_by_key.emplace(op.node.key, op.node.value);
      }
      continue;
    }

    if (!include_exact_fallbacks) {
      continue;
    }
    if (op.node.type != NodeType::kKvValueHash &&
        op.node.type != NodeType::kKvRefValueHash &&
        op.node.type != NodeType::kKvRefValueHashCount) {
      continue;
    }
    if (out->exact_key_fallback_values.find(op.node.key) ==
        out->exact_key_fallback_values.end()) {
      out->exact_key_fallback_values.emplace(op.node.key, op.node.value);
      out->exact_key_fallback_types.emplace(op.node.key, op.node.type);
    }
  }
  return true;
}

bool AppendDistinctTerminalKeysForQueryItem(const QueryItem& item,
                                            std::vector<std::vector<uint8_t>>* out_keys,
                                            std::set<std::vector<uint8_t>>* seen,
                                            std::string* error) {
  if (out_keys == nullptr || seen == nullptr) {
    if (error) {
      *error = "terminal keys output is null";
    }
    return false;
  }
  auto push_key = [&](std::vector<uint8_t> key) {
    if (seen->insert(key).second) {
      out_keys->push_back(std::move(key));
    }
  };
  if (item.IsKey()) {
    push_key(item.start);
    return true;
  }
  if (!(item.type == QueryItemType::kRange || item.type == QueryItemType::kRangeInclusive)) {
    if (error) {
      *error = "terminal keys are not supported with unbounded ranges";
    }
    return false;
  }
  if (item.start.size() > 1 || item.end.size() != 1) {
    if (error) {
      *error = "distinct keys are not available for ranges using more or less than 1 byte";
    }
    return false;
  }
  std::vector<uint8_t> start_key = item.start;
  uint8_t start = 0;
  if (!item.start.empty()) {
    start = item.start.front();
  } else {
    push_key({});
  }
  const uint8_t end = item.end.front();
  if (item.type == QueryItemType::kRange) {
    for (uint16_t i = start; i < end; ++i) {
      push_key({static_cast<uint8_t>(i)});
    }
  } else {
    for (uint16_t i = start; i <= end; ++i) {
      push_key({static_cast<uint8_t>(i)});
      if (i == 255) {
        break;
      }
    }
  }
  return true;
}

bool CollectTerminalPathKeysForQuery(const Query& query,
                                     const std::vector<std::vector<uint8_t>>& current_path,
                                     size_t max_results,
                                     std::vector<PathKeyPair>* out,
                                     std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "terminal keys output is null";
    }
    return false;
  }
  auto maybe_append_terminal = [&](const std::vector<std::vector<uint8_t>>& path,
                                   const std::vector<uint8_t>& key) -> bool {
    if (out->size() >= max_results) {
      if (error) {
        *error = "terminal keys limit exceeded";
      }
      return false;
    }
    out->push_back({path, key});
    return true;
  };

  auto collect_for_branch = [&](const SubqueryBranch& branch,
                                const std::vector<std::vector<uint8_t>>& base_path,
                                const std::vector<uint8_t>& key) -> bool {
    std::vector<std::vector<uint8_t>> path = base_path;
    if (branch.subquery_path.has_value()) {
      path.push_back(key);
      const auto& subquery_path = *branch.subquery_path;
      if (branch.subquery) {
        path.insert(path.end(), subquery_path.begin(), subquery_path.end());
        return CollectTerminalPathKeysForQuery(*branch.subquery, path, max_results, out, error);
      }
      if (subquery_path.empty()) {
        if (error) {
          *error = "subquery_path set but doesn't contain any values";
        }
        return false;
      }
      path.insert(path.end(), subquery_path.begin(), subquery_path.end() - 1);
      return maybe_append_terminal(path, subquery_path.back());
    }
    if (branch.subquery) {
      path.push_back(key);
      return CollectTerminalPathKeysForQuery(*branch.subquery, path, max_results, out, error);
    }
    return maybe_append_terminal(path, key);
  };

  std::set<std::vector<uint8_t>> conditional_seen_keys;
  if (query.conditional_subquery_branches.has_value()) {
    for (const auto& entry : *query.conditional_subquery_branches) {
      const QueryItem& item = entry.first;
      const SubqueryBranch& branch = entry.second;
      std::vector<std::vector<uint8_t>> keys;
      if (!AppendDistinctTerminalKeysForQueryItem(item, &keys, &conditional_seen_keys, error)) {
        return false;
      }
      for (const auto& key : keys) {
        if (!collect_for_branch(branch, current_path, key)) {
          return false;
        }
      }
    }
  }

  std::set<std::vector<uint8_t>> item_keys_seen;
  for (const auto& item : query.items) {
    std::vector<std::vector<uint8_t>> keys;
    if (!AppendDistinctTerminalKeysForQueryItem(item, &keys, &item_keys_seen, error)) {
      return false;
    }
    for (const auto& key : keys) {
      if (conditional_seen_keys.find(key) != conditional_seen_keys.end()) {
        continue;
      }
      if (!collect_for_branch(query.default_subquery_branch, current_path, key)) {
        return false;
      }
    }
  }
  return true;
}

bool ResolveWrappedLayer(const GroveLayerProof& layer,
                         const std::vector<std::vector<uint8_t>>& wrapper_path,
                         std::vector<const GroveLayerProof*>* layers,
                         std::vector<std::vector<uint8_t>>* keys,
                         const GroveLayerProof** out_leaf,
                         std::string* error) {
  if (layers == nullptr || keys == nullptr || out_leaf == nullptr) {
    if (error) {
      *error = "wrapper output is null";
    }
    return false;
  }
  layers->clear();
  keys->clear();
  const GroveLayerProof* current = &layer;
  for (const auto& key : wrapper_path) {
    const GroveLayerProof* next = nullptr;
    for (const auto& entry : current->lower_layers) {
      if (entry.first == key) {
        next = entry.second.get();
        break;
      }
    }
    if (next == nullptr) {
      if (error) {
        *error = "wrapper path key not found in lower layers";
      }
      return false;
    }
    layers->push_back(current);
    keys->push_back(key);
    current = next;
  }
  *out_leaf = current;
  return true;
}

bool VerifyWrappedLayerChain(const GroveLayerProof& layer,
                             const std::vector<std::vector<uint8_t>>& wrapper_path,
                             std::vector<uint8_t>* out_root_hash,
                             std::string* error) {
  if (wrapper_path.empty()) {
    if (out_root_hash == nullptr) {
      return true;
    }
    return ComputeLayerRootHash(layer, out_root_hash, error);
  }
  std::vector<const GroveLayerProof*> layers;
  std::vector<std::vector<uint8_t>> keys;
  const GroveLayerProof* leaf = nullptr;
  if (!ResolveWrappedLayer(layer, wrapper_path, &layers, &keys, &leaf, error)) {
    return false;
  }
  std::vector<uint8_t> child_hash;
  if (!ComputeLayerRootHash(*leaf, &child_hash, error)) {
    return false;
  }
  for (size_t i = layers.size(); i-- > 0;) {
    std::vector<uint8_t> parent_root;
    if (!VerifyLayerLink(layers[i]->merk_proof,
                         keys[i],
                         child_hash,
                         &parent_root,
                         error)) {
      return false;
    }
    child_hash = std::move(parent_root);
  }
  if (out_root_hash) {
    *out_root_hash = std::move(child_hash);
  }
  return true;
}

bool VerifyPathQueryProofLayer(const GroveLayerProof& layer,
                               const std::vector<std::vector<uint8_t>>& path,
                               const Query& query,
                               std::optional<uint16_t> limit,
                               std::optional<uint16_t> offset,
                               std::vector<VerifiedPathKeyElement>* out_elements,
                               std::string* error) {
  if (out_elements == nullptr) {
    if (error) {
      *error = "query output is null";
    }
    return false;
  }
  if (IsSimpleSingleKeyNoSubquery(query)) {
    return VerifySimpleSingleKeyLayer(layer, path, query, limit, offset, out_elements, error);
  }
  if (IsSimpleSingleRangeNoSubquery(query)) {
    return VerifySimpleSingleRangeLayer(layer, path, query, limit, offset, out_elements, error);
  }
  const bool has_exact_key_items = QueryHasExactKeyItems(query.items);
  std::unordered_set<std::vector<uint8_t>, ByteVectorHash> exact_query_keys;
  if (has_exact_key_items) {
    exact_query_keys.reserve(query.items.size());
    for (const auto& item : query.items) {
      if (item.IsKey()) {
        exact_query_keys.insert(item.start);
      }
    }
  }
  DecodedLayerIndex decoded;
  if (!BuildDecodedLayerIndex(layer.merk_proof, has_exact_key_items, &decoded, error)) {
    return false;
  }
  // Pointer-keyed cache is safe here because layer/lower_layers are immutable
  // for the lifetime of this verification call.
  std::unordered_map<const GroveLayerProof*, std::vector<uint8_t>> lower_layer_root_hash_cache;
  std::unordered_set<const GroveLayerProof*> lower_layer_root_hash_failures;
  std::unordered_map<const GroveLayerProof*, bool> wrapper_path_valid_cache_empty_path;
  std::unordered_map<std::string, std::unordered_map<const GroveLayerProof*, bool>>
      wrapper_path_valid_cache_by_path;
  std::unordered_map<std::vector<uint8_t>,
                     const std::pair<std::vector<uint8_t>, std::unique_ptr<GroveLayerProof>>*,
                     ByteVectorHash>
      lower_layer_by_key;
  lower_layer_by_key.reserve(layer.lower_layers.size());
  for (const auto& entry : layer.lower_layers) {
    lower_layer_by_key.emplace(entry.first, &entry);
  }
  auto get_cached_lower_layer_root_hash = [&](const GroveLayerProof* candidate,
                                              std::vector<uint8_t>* out_hash) -> bool {
    if (out_hash == nullptr || candidate == nullptr) {
      return false;
    }
    auto it = lower_layer_root_hash_cache.find(candidate);
    if (it != lower_layer_root_hash_cache.end()) {
      *out_hash = it->second;
      return true;
    }
    if (lower_layer_root_hash_failures.find(candidate) != lower_layer_root_hash_failures.end()) {
      return false;
    }
    std::vector<uint8_t> computed;
    std::string candidate_error;
    if (!ComputeLayerRootHash(*candidate, &computed, &candidate_error)) {
      lower_layer_root_hash_failures.insert(candidate);
      return false;
    }
    lower_layer_root_hash_cache.emplace(candidate, computed);
    *out_hash = std::move(computed);
    return true;
  };
  std::vector<std::vector<uint8_t>> matched_keys;
  matched_keys.reserve(decoded.proof_keys.size());
  for (const auto& key : decoded.proof_keys) {
    if (QueryItemContainsAny(query.items, key)) {
      matched_keys.push_back(key);
    }
  }
  std::sort(matched_keys.begin(), matched_keys.end(), CompareKeys);
  matched_keys.erase(std::unique(matched_keys.begin(), matched_keys.end()),
                     matched_keys.end());
  if (!query.left_to_right) {
    std::reverse(matched_keys.begin(), matched_keys.end());
  }
  size_t start_index = 0;
  if (offset && *offset > 0) {
    start_index = std::min(static_cast<size_t>(*offset), matched_keys.size());
  }
  size_t remaining = matched_keys.size() - start_index;
  if (limit) {
    remaining = std::min(remaining, static_cast<size_t>(*limit));
  }
  size_t end_index = start_index + remaining;

  for (size_t i = start_index; i < end_index; ++i) {
    const auto& key = matched_keys[i];
    auto value_it = decoded.value_by_key.find(key);
    const std::vector<uint8_t>* materialized_value = (value_it == decoded.value_by_key.end())
                                                         ? nullptr
                                                         : &value_it->second;
    bool materialized_from_ref_fallback = false;
    if (has_exact_key_items &&
        materialized_value == nullptr &&
        exact_query_keys.find(key) != exact_query_keys.end()) {
      auto fallback_it = decoded.exact_key_fallback_values.find(key);
      if (fallback_it != decoded.exact_key_fallback_values.end()) {
        materialized_value = &fallback_it->second;
        auto fallback_type_it = decoded.exact_key_fallback_types.find(key);
        if (fallback_type_it != decoded.exact_key_fallback_types.end()) {
          materialized_from_ref_fallback =
              (fallback_type_it->second == NodeType::kKvRefValueHash ||
               fallback_type_it->second == NodeType::kKvRefValueHashCount);
        }
      }
    }

    const SubqueryBranch* branch = ResolveSubqueryBranchForKey(query, key);
    bool branch_has_subquery =
        (branch != nullptr && (branch->subquery != nullptr || branch->subquery_path.has_value()));
    bool parent_emitted = false;
    if (branch == nullptr || !branch_has_subquery) {
      if (materialized_value != nullptr) {
        VerifiedPathKeyElement element;
        element.path = path;
        element.key = key;
        element.has_element = true;
        element.element_bytes = *materialized_value;
        out_elements->push_back(std::move(element));
        parent_emitted = true;
      }
    }
    if (!branch || !branch_has_subquery) {
      continue;
    }
    bool allow_ref_fallback_parent_emit =
        query.conditional_subquery_branches.has_value();
    if (allow_ref_fallback_parent_emit && materialized_from_ref_fallback &&
        materialized_value != nullptr) {
      VerifiedPathKeyElement element;
      element.path = path;
      element.key = key;
      element.has_element = true;
      element.element_bytes = *materialized_value;
      out_elements->push_back(std::move(element));
      parent_emitted = true;
    }
    std::vector<std::vector<uint8_t>> resolved_reference_path;
    bool has_resolved_reference_path = false;
    if (materialized_value != nullptr) {
      std::vector<std::vector<uint8_t>> current_parent_path = path;
      std::vector<uint8_t> current_key = key;
      const std::vector<uint8_t>* current_value = materialized_value;
      std::unordered_set<std::vector<uint8_t>, ByteVectorHash> visited_reference_keys;
      uint32_t hop_count = 0;
      while (current_value != nullptr) {
        ElementReference reference;
        std::string ref_error;
        if (!DecodeReferenceFromElementBytes(*current_value, &reference, &ref_error)) {
          break;
        }
        if (reference.has_max_hop && hop_count >= reference.max_hop) {
          // Rust proof verification treats exhausted hop limits as "cannot
          // resolve further" for nested subquery traversal, not as a hard
          // proof failure.
          break;
        }
        std::vector<std::vector<uint8_t>> resolved_path;
        if (!ResolveReferencePathForProof(current_parent_path,
                                          current_key,
                                          reference.reference_path,
                                          &resolved_path,
                                          error)) {
          return false;
        }
        if (resolved_path.empty()) {
          break;
        }
        has_resolved_reference_path = true;
        resolved_reference_path = resolved_path;
        hop_count += 1;
        if (resolved_path.size() != path.size() + 1) {
          break;
        }
        bool same_layer_parent = true;
        for (size_t j = 0; j < path.size(); ++j) {
          if (resolved_path[j] != path[j]) {
            same_layer_parent = false;
            break;
          }
        }
        if (!same_layer_parent) {
          break;
        }
        const std::vector<uint8_t>& next_key = resolved_path.back();
        if (!visited_reference_keys.insert(next_key).second) {
          if (error) {
            *error = "reference cycle detected";
          }
          return false;
        }
        auto next_it = decoded.value_by_key.find(next_key);
        if (next_it == decoded.value_by_key.end()) {
          break;
        }
        current_key = next_key;
        current_value = &next_it->second;
      }
    }

    std::vector<std::vector<uint8_t>> extra_path;
    if (branch->subquery_path) {
      extra_path = *branch->subquery_path;
    }
    std::unordered_map<const GroveLayerProof*, bool>* wrapper_path_valid_cache =
        &wrapper_path_valid_cache_empty_path;
    if (!extra_path.empty()) {
      std::string path_cache_key = EncodePathCacheKey(extra_path);
      wrapper_path_valid_cache = &wrapper_path_valid_cache_by_path[path_cache_key];
    }
    auto is_wrapper_path_valid_for = [&](const GroveLayerProof* candidate) -> bool {
      if (extra_path.empty()) {
        return true;
      }
      auto it = wrapper_path_valid_cache->find(candidate);
      if (it != wrapper_path_valid_cache->end()) {
        return it->second;
      }
      std::string candidate_error;
      bool ok = VerifyWrappedLayerChain(*candidate, extra_path, nullptr, &candidate_error);
      wrapper_path_valid_cache->emplace(candidate, ok);
      return ok;
    };

    std::vector<uint8_t> child_layer_key = key;
    if (has_resolved_reference_path &&
        resolved_reference_path.size() == path.size() + 1) {
      bool same_parent = true;
      for (size_t i = 0; i < path.size(); ++i) {
        if (resolved_reference_path[i] != path[i]) {
          same_parent = false;
          break;
        }
      }
      if (same_parent) {
        child_layer_key = resolved_reference_path.back();
      }
    }

    const GroveLayerProof* child_layer = nullptr;
    std::vector<uint8_t> selected_child_layer_key;
    auto direct_it = lower_layer_by_key.find(child_layer_key);
    if (direct_it != lower_layer_by_key.end()) {
      child_layer = direct_it->second->second.get();
      selected_child_layer_key = direct_it->second->first;
    }
    if (child_layer == nullptr) {
      // Reference-based subquery proofs may omit parent value materialization while
      // still carrying a single resolved lower-layer branch. Accept that shape.
      if (!has_resolved_reference_path && query.items.size() == 1 &&
          layer.lower_layers.size() == 1) {
        child_layer = layer.lower_layers.front().second.get();
        selected_child_layer_key = layer.lower_layers.front().first;
      }
    }
    if (child_layer == nullptr) {
      // Rust can key lower layers by the resolved tree key while the queried key is a
      // reference key rewritten into KVRef* in the merk proof. In that case, pick the
      // unique lower layer that is cryptographically linked from this layer and matches
      // any requested wrapper path.
      std::vector<const std::pair<std::vector<uint8_t>, std::unique_ptr<GroveLayerProof>>*> linked_candidates;
      for (const auto& entry : layer.lower_layers) {
        std::vector<uint8_t> candidate_hash;
        if (!get_cached_lower_layer_root_hash(entry.second.get(), &candidate_hash)) {
          continue;
        }
        std::string candidate_error;
        if (!VerifyLayerLink(layer.merk_proof,
                             entry.first,
                             candidate_hash,
                             nullptr,
                             &candidate_error)) {
          candidate_error.clear();
          if (!VerifyLayerLink(layer.merk_proof,
                               key,
                               candidate_hash,
                               nullptr,
                               &candidate_error)) {
            continue;
          }
        }
        if (!is_wrapper_path_valid_for(entry.second.get())) {
          continue;
        }
        linked_candidates.push_back(&entry);
      }
      if (linked_candidates.size() == 1) {
        child_layer = linked_candidates.front()->second.get();
        selected_child_layer_key = linked_candidates.front()->first;
      }
    }
    if (child_layer == nullptr && allow_ref_fallback_parent_emit &&
        materialized_from_ref_fallback) {
      std::vector<const std::pair<std::vector<uint8_t>, std::unique_ptr<GroveLayerProof>>*> wrapper_candidates;
      for (const auto& entry : layer.lower_layers) {
        if (!is_wrapper_path_valid_for(entry.second.get())) {
          continue;
        }
        wrapper_candidates.push_back(&entry);
      }
      if (wrapper_candidates.size() == 1) {
        child_layer = wrapper_candidates.front()->second.get();
        selected_child_layer_key = wrapper_candidates.front()->first;
      }
    }
    if (child_layer == nullptr) {
      if (layer.lower_layers.empty()) {
        // Rust verifier accepts reference-subquery proof shapes where no lower
        // layers are provided for the selected key and simply yields no nested
        // results for that branch.
        continue;
      }
      if (error) {
        *error = "missing lower layer for subquery key=" + HexPrefix(key, 32) +
                 " lookup=" + HexPrefix(child_layer_key, 32);
      }
      return false;
    }

    const GroveLayerProof* final_layer = child_layer;
    if (!extra_path.empty()) {
      std::vector<const GroveLayerProof*> wrapper_layers;
      std::vector<std::vector<uint8_t>> wrapper_keys;
      if (!ResolveWrappedLayer(*child_layer,
                               extra_path,
                               &wrapper_layers,
                               &wrapper_keys,
                               &final_layer,
                               error)) {
        return false;
      }
      if (!VerifyWrappedLayerChain(*child_layer, extra_path, nullptr, error)) {
        return false;
      }
    }

    std::vector<std::vector<uint8_t>> child_path = path;
    child_path.push_back(key);
    std::vector<std::vector<uint8_t>> target_path = child_path;
    if (has_resolved_reference_path) {
      target_path = resolved_reference_path;
    } else if (materialized_from_ref_fallback && !selected_child_layer_key.empty() &&
               selected_child_layer_key != key) {
      target_path = path;
      target_path.push_back(selected_child_layer_key);
    }
    if (!extra_path.empty()) {
      child_path.insert(child_path.end(), extra_path.begin(), extra_path.end());
      target_path.insert(target_path.end(), extra_path.begin(), extra_path.end());
    }
    if (!parent_emitted &&
        (query.add_parent_tree_on_subquery || has_resolved_reference_path ||
         (allow_ref_fallback_parent_emit && materialized_from_ref_fallback))) {
      VerifiedPathKeyElement element;
      element.path = path;
      element.key = key;
      element.has_element = (materialized_value != nullptr);
      if (materialized_value != nullptr) {
        element.element_bytes = *materialized_value;
      }
      out_elements->push_back(std::move(element));
    }

    if (branch->subquery) {
      Query subquery = *branch->subquery;
      subquery.left_to_right = query.left_to_right;
      if (!VerifyPathQueryProofLayer(*final_layer,
                                     target_path,
                                     subquery,
                                     std::nullopt,
                                     std::nullopt,
                                     out_elements,
                                     error)) {
        return false;
      }
      continue;
    }

    if (!extra_path.empty()) {
      std::vector<std::vector<uint8_t>> leaf_parent(target_path.begin(),
                                                    target_path.end() - 1);
      std::vector<uint8_t> leaf_key = target_path.back();
      Query leaf_query = Query::NewSingleKey(leaf_key);
      if (!VerifyPathQueryProofLayer(*final_layer,
                                     leaf_parent,
                                     leaf_query,
                                     std::nullopt,
                                     std::nullopt,
                                     out_elements,
                                     error)) {
        return false;
      }
    }
  }

  return true;
}

bool VerifySingleKeyProofForVersion(const SingleKeyProofInput& input,
                                    const GroveVersion& version,
                                    std::string* error) {
  if (!version.IsSupported()) {
    if (error) {
      *error = "unsupported grove version: " + version.ToString();
    }
    return false;
  }
  return VerifySingleKeyProof(input, error);
}

bool VerifySingleKeyAbsenceProofForVersion(const SingleKeyProofInput& input,
                                           const GroveVersion& version,
                                           std::string* error) {
  if (!version.IsSupported()) {
    if (error) {
      *error = "unsupported grove version: " + version.ToString();
    }
    return false;
  }
  return VerifySingleKeyAbsenceProof(input, error);
}

bool VerifyRangeProofForVersion(const RangeProofInput& input,
                                const GroveVersion& version,
                                std::string* error) {
  if (!version.IsSupported()) {
    if (error) {
      *error = "unsupported grove version: " + version.ToString();
    }
    return false;
  }
  return VerifyRangeProof(input, error);
}

bool VerifyPathQueryProof(const std::vector<uint8_t>& proof,
                          const PathQuery& query,
                          std::vector<uint8_t>* out_root_hash,
                          std::vector<VerifiedPathKeyElement>* out_elements,
                          std::string* error) {
  const bool profile_enabled = VerifyPathQueryProfileEnabled();
  std::string profile_label;
  using ProfileClock = std::chrono::steady_clock;
  ProfileClock::time_point total_start;
  uint64_t decode_ns = 0;
  uint64_t root_total_ns = 0;
  uint64_t leaf_root_ns = 0;
  uint64_t link_ns = 0;
  uint64_t query_ns = 0;
  if (profile_enabled) {
    profile_label = CurrentVerifyPathQueryProfileLabel();
    total_start = ProfileClock::now();
  }
  if (out_root_hash == nullptr || out_elements == nullptr) {
    if (error) {
      *error = "proof output is null";
    }
    return false;
  }
  if (proof.empty()) {
    if (error) {
      *error = "missing grove proof bytes";
    }
    return false;
  }
  if (query.query.query.items.empty()) {
    if (error) {
      *error = "query has no items";
    }
    return false;
  }
  if (query.query.offset.has_value() && query.query.offset.value() != 0) {
    if (error) {
      *error = "offsets in path queries are not supported for proofs";
    }
    return false;
  }

  GroveLayerProof root_layer;
  auto decode_start = profile_enabled ? ProfileClock::now() : ProfileClock::time_point{};
  if (!DecodeGroveDbProofV0(proof, &root_layer, error)) {
    return false;
  }
  if (profile_enabled) {
    decode_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(ProfileClock::now() - decode_start)
            .count());
  }

  const GroveLayerProof* current = &root_layer;
  std::vector<const GroveLayerProof*> layers;
  layers.push_back(current);
  for (const auto& key : query.path) {
    const GroveLayerProof* next = nullptr;
    for (const auto& entry : current->lower_layers) {
      if (entry.first == key) {
        next = entry.second.get();
        break;
      }
    }
    if (next == nullptr) {
      if (error) {
        *error = "path key not found in lower layers";
      }
      return false;
    }
    layers.push_back(next);
    current = next;
  }

  const bool simple_single_key_query = IsSimpleSingleKeyNoSubquery(query.query.query);
  const bool simple_single_range_query = IsSimpleSingleRangeNoSubquery(query.query.query);
  const bool can_fastpath_simple_leaf =
      (simple_single_key_query || simple_single_range_query) &&
      layers.back()->lower_layers.empty();
  std::vector<VerifiedPathKeyElement> fastpath_elements;
  std::vector<uint8_t> child_hash;
  auto root_start = profile_enabled ? ProfileClock::now() : ProfileClock::time_point{};
  auto leaf_root_start = profile_enabled ? ProfileClock::now() : ProfileClock::time_point{};
  if (can_fastpath_simple_leaf) {
    if (!BuildSimpleLeafFastPathResult(*layers.back(),
                                       query.path,
                                       query.query.query,
                                       query.query.limit,
                                       &child_hash,
                                       &fastpath_elements,
                                       error)) {
      return false;
    }
  } else if (!ComputeLayerRootHash(*layers.back(), &child_hash, error)) {
    return false;
  }
  if (profile_enabled) {
    leaf_root_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(ProfileClock::now() -
                                                              leaf_root_start)
            .count());
  }
  auto link_start = profile_enabled ? ProfileClock::now() : ProfileClock::time_point{};
  if (layers.size() > 1) {
    for (size_t i = layers.size() - 1; i-- > 0;) {
      std::vector<uint8_t> parent_root;
      if (!VerifyLayerLink(layers[i]->merk_proof,
                           query.path[i],
                           child_hash,
                           &parent_root,
                           error)) {
        return false;
      }
      child_hash = std::move(parent_root);
    }
  }
  if (profile_enabled) {
    link_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(ProfileClock::now() - link_start)
            .count());
  }
  if (profile_enabled) {
    root_total_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(ProfileClock::now() - root_start)
            .count());
  }
  *out_root_hash = child_hash;

  auto query_start = profile_enabled ? ProfileClock::now() : ProfileClock::time_point{};
  if (can_fastpath_simple_leaf) {
    *out_elements = std::move(fastpath_elements);
    if (profile_enabled) {
      query_ns = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(ProfileClock::now() - query_start)
              .count());
      const uint64_t total_ns = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(ProfileClock::now() - total_start)
              .count());
      auto& totals = VerifyPathQueryProfileMap()[profile_label];
      totals.calls += 1;
      totals.decode_ns += decode_ns;
      totals.root_total_ns += root_total_ns;
      totals.leaf_root_ns += leaf_root_ns;
      totals.link_ns += link_ns;
      totals.query_ns += query_ns;
      totals.total_ns += total_ns;
    }
    return true;
  }
  out_elements->clear();
  if (!VerifyPathQueryProofLayer(*layers.back(),
                                 query.path,
                                 query.query.query,
                                 query.query.limit,
                                 query.query.offset,
                                 out_elements,
                                 error)) {
    return false;
  }
  if (profile_enabled) {
    query_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(ProfileClock::now() - query_start)
            .count());
    const uint64_t total_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(ProfileClock::now() - total_start)
            .count());
    auto& totals = VerifyPathQueryProfileMap()[profile_label];
    totals.calls += 1;
    totals.decode_ns += decode_ns;
    totals.root_total_ns += root_total_ns;
    totals.leaf_root_ns += leaf_root_ns;
    totals.link_ns += link_ns;
    totals.query_ns += query_ns;
    totals.total_ns += total_ns;
  }
  return true;
}

bool VerifySubsetQuery(const std::vector<uint8_t>& proof,
                       const PathQuery& query,
                       std::vector<uint8_t>* out_root_hash,
                       std::vector<VerifiedPathKeyElement>* out_elements,
                       std::string* error) {
  // VerifySubsetQuery mirrors Rust's verify_subset_query which uses
  // verify_proof_succinctness: false. The proof verification logic is the same
  // as VerifyPathQueryProof, but it allows non-minimal proofs (proofs that may
  // contain extra data beyond what is strictly required for the query).
  // Currently C++ implementation doesn't enforce succinctness, so this is
  // functionally equivalent to VerifyPathQueryProof.
  return VerifyPathQueryProof(proof, query, out_root_hash, out_elements, error);
}

bool VerifyQueryWithAbsenceProof(const std::vector<uint8_t>& proof,
                                 const PathQuery& query,
                                 std::vector<uint8_t>* out_root_hash,
                                 std::vector<VerifiedPathKeyElement>* out_elements,
                                 std::string* error) {
  // VerifyQueryWithAbsenceProof mirrors Rust's verify_query_with_absence_proof
  // which uses absence_proofs_for_non_existing_searched_keys: true.
  // This requires the query to have a limit set and will include absence entries
  // for terminal keys that were searched but not found in the proof.
  if (out_root_hash == nullptr || out_elements == nullptr) {
    if (error) {
      *error = "proof output is null";
    }
    return false;
  }
  if (proof.empty()) {
    if (error) {
      *error = "missing grove proof bytes";
    }
    return false;
  }
  if (query.query.query.items.empty()) {
    if (error) {
      *error = "query has no items";
    }
    return false;
  }
  if (query.query.offset.has_value() && query.query.offset.value() != 0) {
    if (error) {
      *error = "offsets in path queries are not supported for proofs";
    }
    return false;
  }
  // Absence proof requires a limit to be set
  if (!query.query.limit.has_value()) {
    if (error) {
      *error = "limits must be set in verify_query_with_absence_proof";
    }
    return false;
  }

  GroveLayerProof root_layer;
  if (!DecodeGroveDbProofV0(proof, &root_layer, error)) {
    return false;
  }

  const GroveLayerProof* current = &root_layer;
  std::vector<const GroveLayerProof*> layers;
  layers.push_back(current);
  for (const auto& key : query.path) {
    const GroveLayerProof* next = nullptr;
    for (const auto& entry : current->lower_layers) {
      if (entry.first == key) {
        next = entry.second.get();
        break;
      }
    }
    if (next == nullptr) {
      if (error) {
        *error = "path key not found in lower layers";
      }
      return false;
    }
    layers.push_back(next);
    current = next;
  }

  std::vector<uint8_t> child_hash;
  if (!ComputeLayerRootHash(*layers.back(), &child_hash, error)) {
    return false;
  }
  if (layers.size() > 1) {
    for (size_t i = layers.size() - 1; i-- > 0;) {
      std::vector<uint8_t> parent_root;
      if (!VerifyLayerLink(layers[i]->merk_proof,
                           query.path[i],
                           child_hash,
                           &parent_root,
                           error)) {
        return false;
      }
      child_hash = std::move(parent_root);
    }
  }
  *out_root_hash = child_hash;

  out_elements->clear();
  if (!VerifyPathQueryProofLayer(*layers.back(),
                                 query.path,
                                 query.query.query,
                                 query.query.limit,
                                 query.query.offset,
                                 out_elements,
                                 error)) {
    return false;
  }

  // Rust verify_query_with_absence_proof expands terminal searched keys into
  // explicit absent rows (None) when they are not present in the proof result.
  const size_t max_results = static_cast<size_t>(query.query.limit.value());
  std::vector<PathKeyPair> terminal_path_keys;
  terminal_path_keys.reserve(max_results);
  if (!CollectTerminalPathKeysForQuery(query.query.query,
                                       query.path,
                                       max_results,
                                       &terminal_path_keys,
                                       error)) {
    return false;
  }
  std::set<PathKeyPair> present;
  for (const auto& element : *out_elements) {
    present.insert({element.path, element.key});
  }
  for (const auto& terminal : terminal_path_keys) {
    if (present.find(terminal) != present.end()) {
      continue;
    }
    VerifiedPathKeyElement absent;
    absent.path = terminal.first;
    absent.key = terminal.second;
    absent.has_element = false;
    out_elements->push_back(std::move(absent));
  }

  return true;
}

bool VerifySubsetQueryWithAbsenceProof(const std::vector<uint8_t>& proof,
                                       const PathQuery& query,
                                       std::vector<uint8_t>* out_root_hash,
                                       std::vector<VerifiedPathKeyElement>* out_elements,
                                       std::string* error) {
  // Mirrors Rust verify_subset_query_with_absence_proof:
  // absence_proofs_for_non_existing_searched_keys=true with
  // verify_proof_succinctness=false.
  //
  // C++ currently does not enforce proof succinctness in query verification, so
  // this is behaviorally equivalent to VerifyQueryWithAbsenceProof.
  return VerifyQueryWithAbsenceProof(proof, query, out_root_hash, out_elements, error);
}

bool VerifyPathQueryProofWithChainedQueries(
    const std::vector<uint8_t>& proof,
    const PathQuery& first_query,
    const std::vector<PathQuery>& chained_queries,
    std::vector<uint8_t>* out_root_hash,
    std::vector<std::vector<VerifiedPathKeyElement>>* out_results,
    std::string* error) {
  if (out_root_hash == nullptr || out_results == nullptr) {
    if (error) {
      *error = "proof output is null";
    }
    return false;
  }

  std::vector<uint8_t> first_root_hash;
  std::vector<VerifiedPathKeyElement> first_elements;
  if (!VerifyPathQueryProof(proof, first_query, &first_root_hash, &first_elements, error)) {
    return false;
  }

  out_results->clear();
  out_results->push_back(std::move(first_elements));

  for (size_t i = 0; i < chained_queries.size(); ++i) {
    std::vector<uint8_t> query_root_hash;
    std::vector<VerifiedPathKeyElement> query_elements;
    if (!VerifyPathQueryProof(
            proof, chained_queries[i], &query_root_hash, &query_elements, error)) {
      return false;
    }
    if (query_root_hash != first_root_hash) {
      if (error) {
        *error = "root hash for different path queries do not match, first is " +
                 HexEncode(first_root_hash) + ", this one is " + HexEncode(query_root_hash);
      }
      return false;
    }
    out_results->push_back(std::move(query_elements));
  }

  *out_root_hash = std::move(first_root_hash);
  return true;
}

bool VerifyPathQueryProofForVersion(const std::vector<uint8_t>& proof,
                                    const PathQuery& query,
                                    const GroveVersion& version,
                                    std::vector<uint8_t>* out_root_hash,
                                    std::vector<VerifiedPathKeyElement>* out_elements,
                                    std::string* error) {
  if (!version.IsSupported()) {
    if (error) {
      *error = "unsupported grove version: " + version.ToString();
    }
    return false;
  }
  return VerifyPathQueryProof(proof, query, out_root_hash, out_elements, error);
}

bool VerifySubsetQueryForVersion(const std::vector<uint8_t>& proof,
                                 const PathQuery& query,
                                 const GroveVersion& version,
                                 std::vector<uint8_t>* out_root_hash,
                                 std::vector<VerifiedPathKeyElement>* out_elements,
                                 std::string* error) {
  if (!version.IsSupported()) {
    if (error) {
      *error = "unsupported grove version: " + version.ToString();
    }
    return false;
  }
  return VerifySubsetQuery(proof, query, out_root_hash, out_elements, error);
}

bool VerifyQueryWithAbsenceProofForVersion(const std::vector<uint8_t>& proof,
                                           const PathQuery& query,
                                           const GroveVersion& version,
                                           std::vector<uint8_t>* out_root_hash,
                                           std::vector<VerifiedPathKeyElement>* out_elements,
                                           std::string* error) {
  if (!version.IsSupported()) {
    if (error) {
      *error = "unsupported grove version: " + version.ToString();
    }
    return false;
  }
  return VerifyQueryWithAbsenceProof(proof, query, out_root_hash, out_elements, error);
}

bool VerifySubsetQueryWithAbsenceProofForVersion(
    const std::vector<uint8_t>& proof,
    const PathQuery& query,
    const GroveVersion& version,
    std::vector<uint8_t>* out_root_hash,
    std::vector<VerifiedPathKeyElement>* out_elements,
    std::string* error) {
  if (!version.IsSupported()) {
    if (error) {
      *error = "unsupported grove version: " + version.ToString();
    }
    return false;
  }
  return VerifySubsetQueryWithAbsenceProof(proof, query, out_root_hash, out_elements, error);
}

bool VerifyPathQueryProofWithChainedQueriesForVersion(
    const std::vector<uint8_t>& proof,
    const PathQuery& first_query,
    const std::vector<PathQuery>& chained_queries,
    const GroveVersion& version,
    std::vector<uint8_t>* out_root_hash,
    std::vector<std::vector<VerifiedPathKeyElement>>* out_results,
    std::string* error) {
  if (!version.IsSupported()) {
    if (error) {
      *error = "unsupported grove version: " + version.ToString();
    }
    return false;
  }
  return VerifyPathQueryProofWithChainedQueries(
      proof, first_query, chained_queries, out_root_hash, out_results, error);
}

bool VerifyQueryGetParentTreeInfo(const std::vector<uint8_t>& proof,
                                  const PathQuery& query,
                                  VerifiedQueryResult* out_result,
                                  std::string* error) {
  // VerifyQueryGetParentTreeInfo mirrors Rust's verify_query_get_parent_tree_info
  // which returns the parent tree feature type along with the verified elements.
  // This is useful for verifying subtree queries where the tree type matters.
  if (out_result == nullptr) {
    if (error) {
      *error = "result output is null";
    }
    return false;
  }
  if (proof.empty()) {
    if (error) {
      *error = "missing grove proof bytes";
    }
    return false;
  }
  // verify_query_get_parent_tree_info doesn't support subqueries
  if (query.query.query.default_subquery_branch.subquery ||
      query.query.query.conditional_subquery_branches.has_value()) {
    if (error) {
      *error = "getting the parent tree info is not available when using subqueries";
    }
    return false;
  }
  // must have no offset
  if (query.query.offset.has_value() && query.query.offset.value() != 0) {
    if (error) {
      *error = "offsets in path queries are not supported for proofs";
    }
    return false;
  }

  GroveLayerProof root_layer;
  if (!DecodeGroveDbProofV0(proof, &root_layer, error)) {
    return false;
  }

  const GroveLayerProof* current = &root_layer;
  std::vector<const GroveLayerProof*> layers;
  layers.push_back(current);
  for (const auto& key : query.path) {
    const GroveLayerProof* next = nullptr;
    for (const auto& entry : current->lower_layers) {
      if (entry.first == key) {
        next = entry.second.get();
        break;
      }
    }
    if (next == nullptr) {
      if (error) {
        *error = "path key not found in lower layers";
      }
      return false;
    }
    layers.push_back(next);
    current = next;
  }

  std::vector<uint8_t> child_hash;
  if (!ComputeLayerRootHash(*layers.back(), &child_hash, error)) {
    return false;
  }
  if (layers.size() > 1) {
    for (size_t i = layers.size() - 1; i-- > 0;) {
      std::vector<uint8_t> parent_root;
      if (!VerifyLayerLink(layers[i]->merk_proof,
                           query.path[i],
                           child_hash,
                           &parent_root,
                           error)) {
        return false;
      }
      child_hash = std::move(parent_root);
    }
  }
  out_result->root_hash = child_hash;

  // Extract parent tree type from the parent layer's element bytes for query.path.back().
  out_result->has_tree_type = false;
  out_result->tree_type = 0;
  if (!query.path.empty() && layers.size() >= 2) {
    const GroveLayerProof* holder_layer = layers[layers.size() - 2];
    std::vector<uint8_t> key = query.path.back();
    ProofNode parent_entry;
    std::vector<uint8_t> ignored_holder_root;
    std::string parent_entry_error;
    if (!ExecuteMerkProof(holder_layer->merk_proof,
                          &key,
                          &parent_entry,
                          &ignored_holder_root,
                          &parent_entry_error)) {
      if (error) {
        *error = "failed to extract parent tree info from proof: " + parent_entry_error;
      }
      return false;
    }
    if (!parent_entry.key.empty()) {
      uint64_t variant = 0;
      bool is_tree = false;
      if (!DecodeElementVariant(parent_entry.value, &variant, error)) {
        return false;
      }
      if (!IsTreeElementVariant(parent_entry.value, &is_tree, error)) {
        return false;
      }
      if (is_tree) {
        out_result->tree_type = static_cast<uint8_t>(variant);
        out_result->has_tree_type = true;
      }
    }
  }

  out_result->elements.clear();
  if (!VerifyPathQueryProofLayer(*layers.back(),
                                 query.path,
                                 query.query.query,
                                 query.query.limit,
                                 query.query.offset,
                                 &out_result->elements,
                                 error)) {
    return false;
  }

  return true;
}

bool VerifyQueryGetParentTreeInfoForVersion(const std::vector<uint8_t>& proof,
                                            const PathQuery& query,
                                            const GroveVersion& version,
                                            VerifiedQueryResult* out_result,
                                            std::string* error) {
  if (!version.IsSupported()) {
    if (error) {
      *error = "unsupported grove version: " + version.ToString();
    }
    return false;
  }
  return VerifyQueryGetParentTreeInfo(proof, query, out_result, error);
}

bool RewriteMerkProofForReference(const std::vector<uint8_t>& proof,
                                  const std::vector<uint8_t>& target_key,
                                  const std::vector<uint8_t>& reference_value,
                                  const std::vector<uint8_t>& resolved_value,
                                  bool provable_count,
                                  std::vector<uint8_t>* out,
                                  std::string* error) {
  (void)provable_count;
  if (out == nullptr) {
    if (error) {
      *error = "proof output is null";
    }
    return false;
  }
  if (proof.empty()) {
    if (error) {
      *error = "empty proof";
    }
    return false;
  }
  std::vector<uint8_t> value_hash;
  if (!ValueHash(reference_value, &value_hash, error)) {
    return false;
  }
  out->clear();
  out->reserve(proof.size());

  size_t cursor = 0;
  bool rewritten = false;
  if (proof[0] == 0) {
    while (cursor < proof.size()) {
      size_t op_start = cursor;
      uint8_t marker = proof[cursor++];
      uint64_t len = 0;
      if (!ReadVarintU64(proof, &cursor, &len, error)) {
        return false;
      }
      if (marker == 0) {
        if (cursor + len > proof.size()) {
          if (error) {
            *error = "chunk proof truncated";
          }
          return false;
        }
        out->insert(out->end(),
                    proof.begin() + static_cast<long>(op_start),
                    proof.begin() + static_cast<long>(cursor + len));
        cursor += static_cast<size_t>(len);
        continue;
      }
      if (marker != 1) {
        if (error) {
          *error = "unsupported chunk proof marker";
        }
        return false;
      }
      out->insert(out->end(),
                  proof.begin() + static_cast<long>(op_start),
                  proof.begin() + static_cast<long>(cursor));
      for (uint64_t i = 0; i < len; ++i) {
        if (!RewriteProofOpForReference(proof,
                                        &cursor,
                                        target_key,
                                        reference_value,
                                        resolved_value,
                                        value_hash,
                                        out,
                                        &rewritten,
                                        error)) {
          return false;
        }
      }
    }
  } else {
    while (cursor < proof.size()) {
      if (!RewriteProofOpForReference(proof,
                                      &cursor,
                                      target_key,
                                      reference_value,
                                      resolved_value,
                                      value_hash,
                                      out,
                                      &rewritten,
                                      error)) {
        return false;
      }
    }
  }

  if (!rewritten) {
    if (error) {
      *error = "reference key not found in proof";
    }
    return false;
  }
  return true;
}

bool RewriteMerkProofForDigestKey(const std::vector<uint8_t>& proof,
                                  const std::vector<uint8_t>& target_key,
                                  const std::vector<uint8_t>& target_value,
                                  bool provable_count,
                                  std::vector<uint8_t>* out,
                                  std::string* error) {
  (void)provable_count;
  if (out == nullptr) {
    if (error) *error = "proof output is null";
    return false;
  }
  if (proof.empty()) {
    if (error) *error = "empty proof";
    return false;
  }
  std::vector<uint8_t> value_hash;
  if (!ValueHash(target_value, &value_hash, error)) {
    return false;
  }
  out->clear();
  out->reserve(proof.size());
  size_t cursor = 0;
  bool rewritten = false;
  if (proof[0] == 0) {
    while (cursor < proof.size()) {
      size_t op_start = cursor;
      uint8_t marker = proof[cursor++];
      uint64_t len = 0;
      if (!ReadVarintU64(proof, &cursor, &len, error)) return false;
      if (marker == 0) {
        if (cursor + len > proof.size()) {
          if (error) *error = "chunk proof truncated";
          return false;
        }
        out->insert(out->end(), proof.begin() + static_cast<long>(op_start),
                    proof.begin() + static_cast<long>(cursor + len));
        cursor += static_cast<size_t>(len);
        continue;
      }
      if (marker != 1) {
        if (error) *error = "unsupported chunk proof marker";
        return false;
      }
      out->insert(out->end(), proof.begin() + static_cast<long>(op_start),
                  proof.begin() + static_cast<long>(cursor));
      for (uint64_t i = 0; i < len; ++i) {
        if (!RewriteProofOpForDigestKey(proof,
                                        &cursor,
                                        target_key,
                                        target_value,
                                        value_hash,
                                        out,
                                        &rewritten,
                                        error)) {
          return false;
        }
      }
    }
  } else {
    while (cursor < proof.size()) {
      if (!RewriteProofOpForDigestKey(proof,
                                      &cursor,
                                      target_key,
                                      target_value,
                                      value_hash,
                                      out,
                                      &rewritten,
                                      error)) {
        return false;
      }
    }
  }
  if (!rewritten) {
    if (error) *error = "digest key not found in proof";
    return false;
  }
  return true;
}

bool RewriteMerkProofForValueHashKey(const std::vector<uint8_t>& proof,
                                     const std::vector<uint8_t>& target_key,
                                     const std::vector<uint8_t>& target_value,
                                     const std::vector<uint8_t>& target_value_hash,
                                     bool provable_count,
                                     std::vector<uint8_t>* out,
                                     std::string* error) {
  (void)provable_count;
  if (out == nullptr) {
    if (error) *error = "proof output is null";
    return false;
  }
  if (proof.empty()) {
    if (error) *error = "empty proof";
    return false;
  }
  if (target_value_hash.size() != 32) {
    if (error) *error = "target value hash length mismatch";
    return false;
  }
  out->clear();
  out->reserve(proof.size());
  size_t cursor = 0;
  bool rewritten = false;
  auto rewrite_one = [&](const std::vector<uint8_t>& src,
                         size_t* c,
                         std::vector<uint8_t>* dst,
                         bool* did_rewrite,
                         std::string* err) -> bool {
    size_t op_start = *c;
    if (*c >= src.size()) {
      if (err) *err = "proof truncated";
      return false;
    }
    uint8_t opcode = src[(*c)++];
    bool inverted = false;
    bool has_key_value = false;
    bool has_count = false;
    std::vector<uint8_t> key;
    std::vector<uint8_t> value;
    uint64_t count = 0;
    switch (opcode) {
      case 0x10:
      case 0x11:
      case 0x12:
      case 0x13:
        break;
      case 0x01:
      case 0x02:
      case 0x08:
      case 0x09:
      case 0x15:
      case 0x17:
        if (!ReadBytes(src, c, 32, nullptr, err)) return false;
        if (opcode == 0x15 || opcode == 0x17) {
          if (!ReadU64BE(src, c, &count, err)) return false;
        }
        break;
      case 0x03:
      case 0x20:
      case 0x04:
      case 0x21:
      case 0x06:
      case 0x22:
      case 0x07:
      case 0x23:
      case 0x14:
      case 0x24:
      case 0x18:
      case 0x25: {
        has_key_value = true;
        bool large_value = (opcode == 0x20 || opcode == 0x21 || opcode == 0x22 ||
                            opcode == 0x23 || opcode == 0x24 || opcode == 0x25);
        if (!DecodeKvValue(src, c, &key, &value, err, large_value)) return false;
        if (opcode == 0x04 || opcode == 0x21 || opcode == 0x06 || opcode == 0x22 ||
            opcode == 0x07 || opcode == 0x23 || opcode == 0x18 || opcode == 0x25) {
          if (!ReadBytes(src, c, 32, nullptr, err)) return false;
        }
        if (opcode == 0x07 || opcode == 0x23) {
          bool ignored = false;
          uint64_t ignored_count = 0;
          if (!DecodeTreeFeatureType(src, c, &ignored, &ignored_count, err)) return false;
        }
        if (opcode == 0x14 || opcode == 0x24 || opcode == 0x18 || opcode == 0x25) {
          has_count = true;
          if (!ReadU64BE(src, c, &count, err)) return false;
        }
        break;
      }
      case 0x05:
      case 0x0c:
      case 0x1a: {
        inverted = (opcode == 0x0c);
        if (*c >= src.size()) {
          if (err) *err = "proof truncated";
          return false;
        }
        uint8_t key_len = src[(*c)++];
        if (!ReadBytes(src, c, key_len, &key, err)) return false;
        if (!ReadBytes(src, c, 32, nullptr, err)) return false;
        if (opcode == 0x1a) {
          has_count = true;
          if (!ReadU64BE(src, c, &count, err)) return false;
        }
        break;
      }
      case 0x0a:
      case 0x28:
      case 0x0b:
      case 0x29:
      case 0x0d:
      case 0x2a:
      case 0x0e:
      case 0x2b:
      case 0x16:
      case 0x2c:
      case 0x19:
      case 0x2d: {
        inverted = true;
        has_key_value = true;
        bool large_value = (opcode == 0x28 || opcode == 0x29 || opcode == 0x2a ||
                            opcode == 0x2b || opcode == 0x2c || opcode == 0x2d);
        if (!DecodeKvValue(src, c, &key, &value, err, large_value)) return false;
        if (opcode == 0x0b || opcode == 0x29 || opcode == 0x0d || opcode == 0x2a ||
            opcode == 0x0e || opcode == 0x2b || opcode == 0x19 || opcode == 0x2d) {
          if (!ReadBytes(src, c, 32, nullptr, err)) return false;
        }
        if (opcode == 0x0e || opcode == 0x2b) {
          bool ignored = false;
          uint64_t ignored_count = 0;
          if (!DecodeTreeFeatureType(src, c, &ignored, &ignored_count, err)) return false;
        }
        if (opcode == 0x16 || opcode == 0x2c || opcode == 0x19 || opcode == 0x2d) {
          has_count = true;
          if (!ReadU64BE(src, c, &count, err)) return false;
        }
        break;
      }
      default:
        if (err) *err = "unsupported proof opcode: " + std::to_string(opcode);
        return false;
    }
    if (has_key_value && key == target_key && value == target_value) {
      if (!EncodeKvValueHashRewriteOp(
              key, value, target_value_hash, inverted, has_count, count, dst, err)) {
        return false;
      }
      *did_rewrite = true;
      return true;
    }
    dst->insert(dst->end(),
                src.begin() + static_cast<long>(op_start),
                src.begin() + static_cast<long>(*c));
    return true;
  };

  if (proof[0] == 0) {
    while (cursor < proof.size()) {
      size_t op_start = cursor;
      uint8_t marker = proof[cursor++];
      uint64_t len = 0;
      if (!ReadVarintU64(proof, &cursor, &len, error)) return false;
      if (marker == 0) {
        if (cursor + len > proof.size()) {
          if (error) *error = "chunk proof truncated";
          return false;
        }
        out->insert(out->end(), proof.begin() + static_cast<long>(op_start),
                    proof.begin() + static_cast<long>(cursor + len));
        cursor += static_cast<size_t>(len);
        continue;
      }
      if (marker != 1) {
        if (error) *error = "unsupported chunk proof marker";
        return false;
      }
      out->insert(out->end(), proof.begin() + static_cast<long>(op_start),
                  proof.begin() + static_cast<long>(cursor));
      for (uint64_t i = 0; i < len; ++i) {
        if (!rewrite_one(proof, &cursor, out, &rewritten, error)) return false;
      }
    }
  } else {
    while (cursor < proof.size()) {
      if (!rewrite_one(proof, &cursor, out, &rewritten, error)) return false;
    }
  }
  if (!rewritten) {
    if (error) *error = "value-hash key not found in proof";
    return false;
  }
  return true;
}

bool ExecuteChunkProof(const std::vector<uint8_t>& proof,
                       std::vector<uint8_t>* root_hash,
                       std::string* error) {
  std::vector<ProofOp> ops;
  if (!DecodeChunkProofOps(proof, &ops, error)) {
    return false;
  }
  return ExecuteProofOps(ops, nullptr, nullptr, root_hash, error);
}

bool ExecuteMerkProof(const std::vector<uint8_t>& proof,
                      std::vector<uint8_t>* root_hash,
                      std::string* error) {
  std::vector<ProofOp> ops;
  if (!DecodeMerkProofOps(proof, &ops, error)) {
    return false;
  }
  return ExecuteProofOps(ops, nullptr, nullptr, root_hash, error);
}

bool CountMerkProofHashCalls(const std::vector<uint8_t>& proof,
                             uint64_t* out,
                             std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "hash count output is null";
    }
    return false;
  }
  std::vector<ProofOp> ops;
  if (!DecodeMerkProofOps(proof, &ops, error)) {
    return false;
  }
  uint64_t hash_calls = 0;
  std::vector<ProofNode> stack;
  bool has_last_key = false;
  std::vector<uint8_t> last_key;

  for (const auto& op : ops) {
    switch (op.type) {
      case OpType::kPush:
      case OpType::kPushInverted: {
        if (op.node.type == NodeType::kKv ||
            op.node.type == NodeType::kKvValueHash ||
            op.node.type == NodeType::kKvRefValueHash ||
            op.node.type == NodeType::kKvRefValueHashCount ||
            op.node.type == NodeType::kKvDigest) {
          if (has_last_key) {
            if (op.type == OpType::kPush) {
              if (op.node.key <= last_key) {
                if (error) {
                  *error = "incorrect key ordering";
                }
                return false;
              }
            } else {
              if (op.node.key >= last_key) {
                if (error) {
                  *error = "incorrect key ordering inverted";
                }
                return false;
              }
            }
          }
          last_key = op.node.key;
          has_last_key = true;
        }
        stack.push_back(op.node);
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
        ProofNode parent = stack.back();
        stack.pop_back();
        ProofNode child = stack.back();
        stack.pop_back();
        std::vector<uint8_t> child_hash;
        if (!ComputeNodeHash(&child, &child_hash, error)) {
          return false;
        }
        hash_calls += 1;
        if (op.type == OpType::kParent) {
          parent.left_hash = child_hash;
          parent.has_left = true;
        } else {
          parent.right_hash = child_hash;
          parent.has_right = true;
        }
        stack.push_back(parent);
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
        ProofNode child = stack.back();
        stack.pop_back();
        ProofNode parent = stack.back();
        stack.pop_back();
        std::vector<uint8_t> child_hash;
        if (!ComputeNodeHash(&child, &child_hash, error)) {
          return false;
        }
        hash_calls += 1;
        if (op.type == OpType::kChild) {
          parent.right_hash = child_hash;
          parent.has_right = true;
        } else {
          parent.left_hash = child_hash;
          parent.has_left = true;
        }
        stack.push_back(parent);
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
  ProofNode root = stack.back();
  std::vector<uint8_t> root_hash;
  if (!ComputeNodeHash(&root, &root_hash, error)) {
    return false;
  }
  hash_calls += 1;
  *out = hash_calls;
  return true;
}

bool CountMerkProofHashNodes(const std::vector<uint8_t>& proof,
                             uint64_t* out,
                             std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "hash node output is null";
    }
    return false;
  }
  std::vector<ProofOp> ops;
  if (!DecodeMerkProofOps(proof, &ops, error)) {
    return false;
  }
  uint64_t count = 0;
  for (const auto& op : ops) {
    if (op.type != OpType::kPush && op.type != OpType::kPushInverted) {
      continue;
    }
    if (op.node.type == NodeType::kHash) {
      count += 1;
    }
  }
  *out = count;
  return true;
}

bool MerkProofHasReferenceNodes(const std::vector<uint8_t>& proof,
                                bool* out,
                                std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "reference output is null";
    }
    return false;
  }
  std::vector<ProofOp> ops;
  if (!DecodeMerkProofOps(proof, &ops, error)) {
    return false;
  }
  for (const auto& op : ops) {
    if (op.type != OpType::kPush && op.type != OpType::kPushInverted) {
      continue;
    }
    if (op.node.type == NodeType::kKvRefValueHash ||
        op.node.type == NodeType::kKvRefValueHashCount) {
      *out = true;
      return true;
    }
  }
  *out = false;
  return true;
}

void DumpVerifyPathQueryProfile() {
  PrintVerifyPathQueryProfileReport();
}

}  // namespace grovedb
