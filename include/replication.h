#ifndef GROVEDB_CPP_REPLICATION_H
#define GROVEDB_CPP_REPLICATION_H

#include <array>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "chunk_restore.h"

namespace grovedb {

constexpr uint16_t kCurrentStateSyncVersion = 1;

enum class ReplicationTreeType : uint8_t {
  kNormalTree = 0,
  kSumTree = 1,
  kBigSumTree = 2,
  kCountTree = 3,
  kCountSumTree = 4,
  kProvableCountTree = 5,
  kProvableCountSumTree = 6,
};

struct ChunkIdentifier {
  std::array<uint8_t, 32> subtree_prefix{};
  std::optional<std::vector<uint8_t>> root_key;
  ReplicationTreeType tree_type = ReplicationTreeType::kNormalTree;
  std::vector<std::vector<uint8_t>> nested_chunk_ids;
};

struct StateSyncSession {
  std::array<uint8_t, 32> app_hash{};
  uint16_t version = kCurrentStateSyncVersion;
  size_t subtrees_batch_size = 0;
  bool started = false;
  bool received_empty_tree_chunk = false;
  ChunkRestorer root_restorer;
  std::set<std::vector<uint8_t>> pending_global_chunk_ids;
  std::set<std::vector<uint8_t>> in_flight_global_chunk_ids;
};

bool PackNestedBytes(const std::vector<std::vector<uint8_t>>& nested_bytes,
                     std::vector<uint8_t>* out,
                     std::string* error);
bool UnpackNestedBytes(const std::vector<uint8_t>& packed_data,
                       std::vector<std::vector<uint8_t>>* out,
                       std::string* error);

bool EncodeGlobalChunkId(const std::array<uint8_t, 32>& subtree_prefix,
                         const std::optional<std::vector<uint8_t>>& root_key_opt,
                         ReplicationTreeType tree_type,
                         const std::vector<std::vector<uint8_t>>& chunk_ids,
                         std::vector<uint8_t>* out,
                         std::string* error);
bool EncodeGlobalChunkIdForVersion(const std::array<uint8_t, 32>& subtree_prefix,
                                   const std::optional<std::vector<uint8_t>>& root_key_opt,
                                   ReplicationTreeType tree_type,
                                   const std::vector<std::vector<uint8_t>>& chunk_ids,
                                   uint16_t version,
                                   std::vector<uint8_t>* out,
                                   std::string* error);
bool DecodeGlobalChunkId(const std::vector<uint8_t>& global_chunk_id,
                         const std::array<uint8_t, 32>& app_hash,
                         ChunkIdentifier* out,
                         std::string* error);
bool DecodeGlobalChunkIdForVersion(const std::vector<uint8_t>& global_chunk_id,
                                   const std::array<uint8_t, 32>& app_hash,
                                   uint16_t version,
                                   ChunkIdentifier* out,
                                   std::string* error);
bool IsSupportedStateSyncVersion(uint16_t version, std::string* error);
bool StartStateSyncSession(const std::array<uint8_t, 32>& app_hash,
                           size_t subtrees_batch_size,
                           uint16_t version,
                           StateSyncSession* session,
                           std::string* error);
bool FetchStateSyncChunk(StateSyncSession* session,
                         size_t max_global_chunk_ids,
                         std::vector<uint8_t>* packed_global_chunk_ids,
                         std::string* error);
bool ApplyStateSyncChunk(StateSyncSession* session,
                         const std::vector<uint8_t>& packed_global_chunk_ids,
                         const std::vector<uint8_t>& packed_global_chunks,
                         uint16_t version,
                         std::vector<uint8_t>* packed_next_global_chunk_ids,
                         bool* sync_completed,
                         std::string* error);
bool IsStateSyncSessionCompleted(const StateSyncSession* session,
                                 bool* sync_completed,
                                 std::string* error);
bool IsStateSyncSessionEmpty(const StateSyncSession* session,
                             bool* is_empty,
                             std::string* error);
bool CommitStateSyncSession(StateSyncSession* session, std::string* error);

}  // namespace grovedb

#endif  // GROVEDB_CPP_REPLICATION_H
