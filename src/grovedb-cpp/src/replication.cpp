#include "replication.h"

#include <algorithm>

#include "binary.h"

namespace grovedb {

namespace {

bool DecodeReplicationTreeType(uint8_t value, ReplicationTreeType* out, std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "tree type output is null";
    }
    return false;
  }
  switch (value) {
    case 0:
      *out = ReplicationTreeType::kNormalTree;
      return true;
    case 1:
      *out = ReplicationTreeType::kSumTree;
      return true;
    case 2:
      *out = ReplicationTreeType::kBigSumTree;
      return true;
    case 3:
      *out = ReplicationTreeType::kCountTree;
      return true;
    case 4:
      *out = ReplicationTreeType::kCountSumTree;
      return true;
    case 5:
      *out = ReplicationTreeType::kProvableCountTree;
      return true;
    case 6:
      *out = ReplicationTreeType::kProvableCountSumTree;
      return true;
    default:
      if (error) {
        *error = "got " + std::to_string(value) + ", max is 6";
      }
      return false;
  }
}

bool CheckStateSyncVersion(uint16_t version, std::string* error) {
  if (version != kCurrentStateSyncVersion) {
    if (error) {
      *error = "unsupported state sync protocol version";
    }
    return false;
  }
  return true;
}

bool ReadChunkKvValue(const std::vector<uint8_t>& chunk_ops,
                      size_t* cursor,
                      bool large_value,
                      std::string* error) {
  if (*cursor >= chunk_ops.size()) {
    if (error) {
      *error = "proof truncated";
    }
    return false;
  }
  size_t key_len = static_cast<size_t>(chunk_ops[*cursor]);
  *cursor += 1;
  if (*cursor + key_len > chunk_ops.size()) {
    if (error) {
      *error = "proof truncated";
    }
    return false;
  }
  *cursor += key_len;
  if (large_value) {
    uint32_t value_len = 0;
    if (!ReadU32BE(chunk_ops, cursor, &value_len, error)) {
      return false;
    }
    if (*cursor + value_len > chunk_ops.size()) {
      if (error) {
        *error = "proof truncated";
      }
      return false;
    }
    *cursor += static_cast<size_t>(value_len);
    return true;
  }
  uint16_t value_len = 0;
  if (!ReadU16BE(chunk_ops, cursor, &value_len, error)) {
    return false;
  }
  if (*cursor + value_len > chunk_ops.size()) {
    if (error) {
      *error = "proof truncated";
    }
    return false;
  }
  *cursor += static_cast<size_t>(value_len);
  return true;
}

bool ReadChunkFeatureType(const std::vector<uint8_t>& chunk_ops,
                          size_t* cursor,
                          std::string* error) {
  if (*cursor >= chunk_ops.size()) {
    if (error) {
      *error = "feature type truncated";
    }
    return false;
  }
  uint8_t tag = chunk_ops[*cursor];
  *cursor += 1;
  switch (tag) {
    case 0:
      return true;
    case 1: {
      int64_t ignored = 0;
      return ReadVarintI64(chunk_ops, cursor, &ignored, error);
    }
    case 2:
      if (*cursor + 16 > chunk_ops.size()) {
        if (error) {
          *error = "proof truncated";
        }
        return false;
      }
      *cursor += 16;
      return true;
    case 3:
    case 5: {
      uint64_t ignored = 0;
      return ReadVarintU64(chunk_ops, cursor, &ignored, error);
    }
    case 4:
    case 6: {
      uint64_t ignored_count = 0;
      int64_t ignored_sum = 0;
      if (!ReadVarintU64(chunk_ops, cursor, &ignored_count, error)) {
        return false;
      }
      return ReadVarintI64(chunk_ops, cursor, &ignored_sum, error);
    }
    default:
      if (error) {
        *error = "unsupported tree feature type";
      }
      return false;
  }
}

bool CountChunkProofOps(const std::vector<uint8_t>& chunk_ops,
                        uint64_t* out_op_count,
                        std::string* error) {
  if (out_op_count == nullptr) {
    if (error) {
      *error = "op count output is null";
    }
    return false;
  }
  size_t cursor = 0;
  uint64_t count = 0;
  while (cursor < chunk_ops.size()) {
    uint8_t opcode = chunk_ops[cursor++];
    switch (opcode) {
      case 0x10:
      case 0x11:
        break;
      case 0x01:
        if (cursor + 32 > chunk_ops.size()) {
          if (error) {
            *error = "proof truncated";
          }
          return false;
        }
        cursor += 32;
        break;
      case 0x07:
      case 0x23:
        if (!ReadChunkKvValue(chunk_ops, &cursor, opcode == 0x23, error)) {
          return false;
        }
        if (cursor + 32 > chunk_ops.size()) {
          if (error) {
            *error = "proof truncated";
          }
          return false;
        }
        cursor += 32;
        if (!ReadChunkFeatureType(chunk_ops, &cursor, error)) {
          return false;
        }
        break;
      default:
        if (error) {
          *error = "unsupported proof opcode in chunk";
        }
        return false;
    }
    count += 1;
  }
  *out_op_count = count;
  return true;
}

bool EncodeChunkProofFromIdAndOps(const std::vector<uint8_t>& chunk_id,
                                  const std::vector<uint8_t>& chunk_ops,
                                  std::vector<uint8_t>* out_proof,
                                  std::string* error) {
  uint64_t op_count = 0;
  if (!CountChunkProofOps(chunk_ops, &op_count, error)) {
    return false;
  }
  out_proof->clear();
  out_proof->push_back(0x00);
  EncodeVarintU64(static_cast<uint64_t>(chunk_id.size()), out_proof);
  out_proof->insert(out_proof->end(), chunk_id.begin(), chunk_id.end());
  out_proof->push_back(0x01);
  EncodeVarintU64(op_count, out_proof);
  out_proof->insert(out_proof->end(), chunk_ops.begin(), chunk_ops.end());
  return true;
}

bool IsAppHashChunkId(const std::vector<uint8_t>& global_chunk_id,
                      const std::array<uint8_t, 32>& app_hash) {
  return global_chunk_id.size() == app_hash.size() &&
         std::equal(global_chunk_id.begin(), global_chunk_id.end(), app_hash.begin());
}

bool IsSyncCompleted(const StateSyncSession& session) {
  if (!session.pending_global_chunk_ids.empty() || !session.in_flight_global_chunk_ids.empty()) {
    return false;
  }
  return session.received_empty_tree_chunk || !session.root_restorer.HasPendingChunks();
}

}  // namespace

bool PackNestedBytes(const std::vector<std::vector<uint8_t>>& nested_bytes,
                     std::vector<uint8_t>* out,
                     std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "pack output is null";
    }
    return false;
  }
  out->clear();
  if (nested_bytes.size() > 0xFFFFu) {
    if (error) {
      *error = "too many nested byte arrays";
    }
    return false;
  }
  const uint16_t count = static_cast<uint16_t>(nested_bytes.size());
  out->push_back(static_cast<uint8_t>((count >> 8) & 0xFF));
  out->push_back(static_cast<uint8_t>(count & 0xFF));
  for (const auto& bytes : nested_bytes) {
    if (static_cast<uint64_t>(bytes.size()) > 0xFFFFFFFFULL) {
      if (error) {
        *error = "nested byte array too large";
      }
      return false;
    }
    const uint32_t len = static_cast<uint32_t>(bytes.size());
    out->push_back(static_cast<uint8_t>((len >> 24) & 0xFF));
    out->push_back(static_cast<uint8_t>((len >> 16) & 0xFF));
    out->push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
    out->push_back(static_cast<uint8_t>(len & 0xFF));
    out->insert(out->end(), bytes.begin(), bytes.end());
  }
  return true;
}

bool UnpackNestedBytes(const std::vector<uint8_t>& packed_data,
                       std::vector<std::vector<uint8_t>>* out,
                       std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "unpack output is null";
    }
    return false;
  }
  out->clear();
  if (packed_data.empty()) {
    if (error) {
      *error = "Input data is empty";
    }
    return false;
  }
  if (packed_data.size() < 2) {
    if (error) {
      *error = "Unexpected end of data while reading number of nested arrays";
    }
    return false;
  }
  const size_t num_elements =
      (static_cast<size_t>(packed_data[0]) << 8) | static_cast<size_t>(packed_data[1]);
  out->reserve(num_elements);
  size_t index = 2;
  for (size_t i = 0; i < num_elements; ++i) {
    if (index + 3 >= packed_data.size()) {
      if (error) {
        *error = "Unexpected end of data while reading length of nested array " +
                 std::to_string(i);
      }
      return false;
    }
    const size_t byte_length = (static_cast<size_t>(packed_data[index]) << 24) |
                               (static_cast<size_t>(packed_data[index + 1]) << 16) |
                               (static_cast<size_t>(packed_data[index + 2]) << 8) |
                               static_cast<size_t>(packed_data[index + 3]);
    index += 4;
    if (index + byte_length > packed_data.size()) {
      if (error) {
        *error = "Unexpected end of data while reading nested array " + std::to_string(i) +
                 " (expected length: " + std::to_string(byte_length) + ")";
      }
      return false;
    }
    out->emplace_back(packed_data.begin() + static_cast<long>(index),
                      packed_data.begin() + static_cast<long>(index + byte_length));
    index += byte_length;
  }
  if (index != packed_data.size()) {
    if (error) {
      *error =
          "Extra unexpected bytes found at the end of input (expected length: " +
          std::to_string(index) + ", actual: " + std::to_string(packed_data.size()) + ")";
    }
    return false;
  }
  return true;
}

bool EncodeGlobalChunkId(const std::array<uint8_t, 32>& subtree_prefix,
                         const std::optional<std::vector<uint8_t>>& root_key_opt,
                         ReplicationTreeType tree_type,
                         const std::vector<std::vector<uint8_t>>& chunk_ids,
                         std::vector<uint8_t>* out,
                         std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "global chunk id output is null";
    }
    return false;
  }
  std::vector<uint8_t> packed_chunk_ids;
  if (!PackNestedBytes(chunk_ids, &packed_chunk_ids, error)) {
    return false;
  }
  out->clear();
  out->insert(out->end(), subtree_prefix.begin(), subtree_prefix.end());
  if (root_key_opt.has_value()) {
    const auto& root_key = *root_key_opt;
    if (root_key.size() > 255) {
      if (error) {
        *error = "root key exceeds 255 bytes";
      }
      return false;
    }
    out->push_back(static_cast<uint8_t>(root_key.size()));
    out->insert(out->end(), root_key.begin(), root_key.end());
  } else {
    out->push_back(0);
  }
  out->push_back(static_cast<uint8_t>(tree_type));
  out->insert(out->end(), packed_chunk_ids.begin(), packed_chunk_ids.end());
  return true;
}

bool EncodeGlobalChunkIdForVersion(const std::array<uint8_t, 32>& subtree_prefix,
                                   const std::optional<std::vector<uint8_t>>& root_key_opt,
                                   ReplicationTreeType tree_type,
                                   const std::vector<std::vector<uint8_t>>& chunk_ids,
                                   uint16_t version,
                                   std::vector<uint8_t>* out,
                                   std::string* error) {
  if (!CheckStateSyncVersion(version, error)) {
    return false;
  }
  return EncodeGlobalChunkId(subtree_prefix, root_key_opt, tree_type, chunk_ids, out, error);
}

bool DecodeGlobalChunkId(const std::vector<uint8_t>& global_chunk_id,
                         const std::array<uint8_t, 32>& app_hash,
                         ChunkIdentifier* out,
                         std::string* error) {
  if (out == nullptr) {
    if (error) {
      *error = "chunk identifier output is null";
    }
    return false;
  }
  constexpr size_t kChunkPrefixLength = 32;
  if (global_chunk_id.size() < kChunkPrefixLength) {
    if (error) {
      *error = "expected global chunk id of at least 32 length";
    }
    return false;
  }
  if (global_chunk_id.size() == app_hash.size()) {
    bool equal = true;
    for (size_t i = 0; i < app_hash.size(); ++i) {
      if (global_chunk_id[i] != app_hash[i]) {
        equal = false;
        break;
      }
    }
    if (equal) {
      out->subtree_prefix.fill(0);
      out->root_key = std::nullopt;
      out->tree_type = ReplicationTreeType::kNormalTree;
      out->nested_chunk_ids.clear();
      return true;
    }
  }

  for (size_t i = 0; i < kChunkPrefixLength; ++i) {
    out->subtree_prefix[i] = global_chunk_id[i];
  }
  if (global_chunk_id.size() <= kChunkPrefixLength) {
    if (error) {
      *error = "unable to decode root key size";
    }
    return false;
  }
  size_t cursor = kChunkPrefixLength;
  const size_t root_key_size = static_cast<size_t>(global_chunk_id[cursor]);
  cursor += 1;
  if (global_chunk_id.size() < cursor + root_key_size) {
    if (error) {
      *error = "unable to decode root key";
    }
    return false;
  }
  if (root_key_size == 0) {
    out->root_key = std::nullopt;
  } else {
    out->root_key = std::vector<uint8_t>(global_chunk_id.begin() + static_cast<long>(cursor),
                                         global_chunk_id.begin() +
                                             static_cast<long>(cursor + root_key_size));
  }
  cursor += root_key_size;
  if (global_chunk_id.size() <= cursor) {
    if (error) {
      *error = "unable to decode root key";
    }
    return false;
  }
  if (!DecodeReplicationTreeType(global_chunk_id[cursor], &out->tree_type, error)) {
    return false;
  }
  cursor += 1;
  const std::vector<uint8_t> packed_nested_chunk_ids(global_chunk_id.begin() + static_cast<long>(cursor),
                                                     global_chunk_id.end());
  return UnpackNestedBytes(packed_nested_chunk_ids, &out->nested_chunk_ids, error);
}

bool DecodeGlobalChunkIdForVersion(const std::vector<uint8_t>& global_chunk_id,
                                   const std::array<uint8_t, 32>& app_hash,
                                   uint16_t version,
                                   ChunkIdentifier* out,
                                   std::string* error) {
  if (!CheckStateSyncVersion(version, error)) {
    return false;
  }
  return DecodeGlobalChunkId(global_chunk_id, app_hash, out, error);
}

bool IsSupportedStateSyncVersion(uint16_t version, std::string* error) {
  return CheckStateSyncVersion(version, error);
}

bool StartStateSyncSession(const std::array<uint8_t, 32>& app_hash,
                           size_t subtrees_batch_size,
                           uint16_t version,
                           StateSyncSession* session,
                           std::string* error) {
  if (session == nullptr) {
    if (error) {
      *error = "state sync session output is null";
    }
    return false;
  }
  if (!CheckStateSyncVersion(version, error)) {
    return false;
  }
  if (subtrees_batch_size == 0) {
    if (error) {
      *error = "subtrees_batch_size cannot be zero";
    }
    return false;
  }
  session->app_hash = app_hash;
  session->version = version;
  session->subtrees_batch_size = subtrees_batch_size;
  session->started = true;
  session->received_empty_tree_chunk = false;
  session->pending_global_chunk_ids.clear();
  session->in_flight_global_chunk_ids.clear();
  std::vector<uint8_t> root_hash(app_hash.begin(), app_hash.end());
  if (!session->root_restorer.Init(root_hash, nullptr, error)) {
    return false;
  }
  session->pending_global_chunk_ids.insert(root_hash);
  return true;
}

bool FetchStateSyncChunk(StateSyncSession* session,
                         size_t max_global_chunk_ids,
                         std::vector<uint8_t>* packed_global_chunk_ids,
                         std::string* error) {
  if (session == nullptr || packed_global_chunk_ids == nullptr) {
    if (error) {
      *error = "state sync fetch input is null";
    }
    return false;
  }
  if (!session->started) {
    if (error) {
      *error = "state sync session is not started";
    }
    return false;
  }
  if (max_global_chunk_ids == 0) {
    if (error) {
      *error = "max_global_chunk_ids must be greater than zero";
    }
    return false;
  }
  std::vector<std::vector<uint8_t>> fetched_ids;
  fetched_ids.reserve(max_global_chunk_ids);
  auto it = session->pending_global_chunk_ids.begin();
  while (it != session->pending_global_chunk_ids.end() &&
         fetched_ids.size() < max_global_chunk_ids) {
    fetched_ids.push_back(*it);
    session->in_flight_global_chunk_ids.insert(*it);
    it = session->pending_global_chunk_ids.erase(it);
  }
  return PackNestedBytes(fetched_ids, packed_global_chunk_ids, error);
}

bool ApplyStateSyncChunk(StateSyncSession* session,
                         const std::vector<uint8_t>& packed_global_chunk_ids,
                         const std::vector<uint8_t>& packed_global_chunks,
                         uint16_t version,
                         std::vector<uint8_t>* packed_next_global_chunk_ids,
                         bool* sync_completed,
                         std::string* error) {
  if (session == nullptr || packed_next_global_chunk_ids == nullptr ||
      sync_completed == nullptr) {
    if (error) {
      *error = "state sync apply input is null";
    }
    return false;
  }
  if (!session->started) {
    if (error) {
      *error = "state sync session is not started";
    }
    return false;
  }
  if (!CheckStateSyncVersion(version, error) || version != session->version) {
    if (error && error->empty()) {
      *error = "unsupported state sync protocol version";
    }
    return false;
  }

  std::vector<std::vector<uint8_t>> nested_global_chunk_ids;
  if (IsAppHashChunkId(packed_global_chunk_ids, session->app_hash)) {
    nested_global_chunk_ids.push_back(packed_global_chunk_ids);
  } else if (!UnpackNestedBytes(packed_global_chunk_ids, &nested_global_chunk_ids, error)) {
    return false;
  }

  std::vector<std::vector<uint8_t>> nested_global_chunks;
  if (!UnpackNestedBytes(packed_global_chunks, &nested_global_chunks, error)) {
    return false;
  }
  if (nested_global_chunk_ids.size() != nested_global_chunks.size()) {
    if (error) {
      *error = "Packed num of global chunkIDs and chunks are not matching";
    }
    return false;
  }

  std::vector<std::vector<uint8_t>> next_global_chunk_ids;
  for (size_t i = 0; i < nested_global_chunk_ids.size(); ++i) {
    const auto& global_chunk_id = nested_global_chunk_ids[i];
    const auto& packed_local_chunks = nested_global_chunks[i];

    if (session->in_flight_global_chunk_ids.count(global_chunk_id) == 0) {
      if (error) {
        *error = "Incoming global_chunk_id not expected";
      }
      return false;
    }

    ChunkIdentifier decoded;
    if (!DecodeGlobalChunkId(global_chunk_id, session->app_hash, &decoded, error)) {
      return false;
    }

    std::vector<std::vector<uint8_t>> local_chunk_ids = decoded.nested_chunk_ids;
    if (local_chunk_ids.empty()) {
      local_chunk_ids.push_back({});
    }

    std::vector<std::vector<uint8_t>> local_chunk_ops;
    if (!UnpackNestedBytes(packed_local_chunks, &local_chunk_ops, error)) {
      return false;
    }
    if (local_chunk_ids.size() != local_chunk_ops.size()) {
      if (error) {
        *error = "Packed num of chunkIDs and chunks are not matching #2";
      }
      return false;
    }

    std::vector<std::vector<uint8_t>> next_local_chunk_ids;
    for (size_t j = 0; j < local_chunk_ids.size(); ++j) {
      const auto& chunk_id = local_chunk_ids[j];
      const auto& chunk_ops = local_chunk_ops[j];
      if (chunk_ops.empty()) {
        if (IsAppHashChunkId(global_chunk_id, session->app_hash)) {
          session->received_empty_tree_chunk = true;
        }
        continue;
      }
      std::vector<uint8_t> chunk_proof;
      if (!EncodeChunkProofFromIdAndOps(chunk_id, chunk_ops, &chunk_proof, error)) {
        return false;
      }
      std::vector<std::vector<uint8_t>> produced_next_ids;
      if (!session->root_restorer.ProcessChunkProof(chunk_proof, &produced_next_ids, error)) {
        return false;
      }
      next_local_chunk_ids.insert(next_local_chunk_ids.end(),
                                  produced_next_ids.begin(),
                                  produced_next_ids.end());
    }

    for (size_t offset = 0; offset < next_local_chunk_ids.size(); offset += 32) {
      size_t end = std::min(next_local_chunk_ids.size(), offset + 32);
      std::vector<std::vector<uint8_t>> grouped_ids(next_local_chunk_ids.begin() + offset,
                                                    next_local_chunk_ids.begin() +
                                                        static_cast<long>(end));
      std::vector<uint8_t> encoded_next_global_id;
      if (!EncodeGlobalChunkId(decoded.subtree_prefix,
                               decoded.root_key,
                               decoded.tree_type,
                               grouped_ids,
                               &encoded_next_global_id,
                               error)) {
        return false;
      }
      if (session->pending_global_chunk_ids.insert(encoded_next_global_id).second) {
        next_global_chunk_ids.push_back(std::move(encoded_next_global_id));
      }
    }

    session->in_flight_global_chunk_ids.erase(global_chunk_id);
  }

  if (!PackNestedBytes(next_global_chunk_ids, packed_next_global_chunk_ids, error)) {
    return false;
  }
  *sync_completed = IsSyncCompleted(*session);
  return true;
}

bool IsStateSyncSessionCompleted(const StateSyncSession* session,
                                 bool* sync_completed,
                                 std::string* error) {
  if (session == nullptr || sync_completed == nullptr) {
    if (error) {
      *error = "state sync status input is null";
    }
    return false;
  }
  if (!session->started) {
    if (error) {
      *error = "state sync session is not started";
    }
    return false;
  }
  *sync_completed = IsSyncCompleted(*session);
  return true;
}

bool IsStateSyncSessionEmpty(const StateSyncSession* session,
                             bool* is_empty,
                             std::string* error) {
  if (session == nullptr || is_empty == nullptr) {
    if (error) {
      *error = "state sync empty check input is null";
    }
    return false;
  }
  if (!session->started) {
    if (error) {
      *error = "state sync session is not started";
    }
    return false;
  }
  // Mirror Rust MultiStateSyncSession::is_empty(): check if current_prefixes is empty
  // In C++, this corresponds to pending_global_chunk_ids being empty
  *is_empty = session->pending_global_chunk_ids.empty();
  return true;
}

bool CommitStateSyncSession(StateSyncSession* session, std::string* error) {
  if (session == nullptr) {
    if (error) {
      *error = "state sync commit input is null";
    }
    return false;
  }
  if (!session->started) {
    if (error) {
      *error = "state sync session is not started";
    }
    return false;
  }

  session->started = false;
  session->received_empty_tree_chunk = false;
  session->pending_global_chunk_ids.clear();
  session->in_flight_global_chunk_ids.clear();
  session->root_restorer = ChunkRestorer();
  return true;
}

}  // namespace grovedb
