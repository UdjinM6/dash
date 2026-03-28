#include "replication.h"

#include "binary.h"
#include "chunk.h"
#include "merk.h"
#include "test_utils.h"

#include <string>
#include <vector>

using test_utils::Fail;
using test_utils::Expect;

namespace {

std::array<uint8_t, 32> Filled(uint8_t value) {
  std::array<uint8_t, 32> out{};
  out.fill(value);
  return out;
}

std::array<uint8_t, 32> ToArray32(const std::vector<uint8_t>& bytes) {
  if (bytes.size() != 32) {
    Fail("expected 32-byte value");
  }
  std::array<uint8_t, 32> out{};
  for (size_t i = 0; i < out.size(); ++i) {
    out[i] = bytes[i];
  }
  return out;
}

std::vector<uint8_t> BuildPackedGlobalChunksForRequest(
    const std::vector<uint8_t>& packed_global_chunk_ids,
    const std::array<uint8_t, 32>& app_hash,
    const grovedb::MerkTree& tree) {
  std::vector<std::vector<uint8_t>> global_chunk_ids;
  std::string error;
  if (packed_global_chunk_ids.size() == app_hash.size() &&
      std::equal(packed_global_chunk_ids.begin(),
                 packed_global_chunk_ids.end(),
                 app_hash.begin())) {
    global_chunk_ids.push_back(packed_global_chunk_ids);
  } else if (!grovedb::UnpackNestedBytes(packed_global_chunk_ids, &global_chunk_ids, &error)) {
    Fail("failed to unpack requested global chunk ids: " + error);
  }

  std::vector<std::vector<uint8_t>> per_global_chunks;
  for (const auto& global_chunk_id : global_chunk_ids) {
    grovedb::ChunkIdentifier decoded;
    if (!grovedb::DecodeGlobalChunkId(global_chunk_id, app_hash, &decoded, &error)) {
      Fail("failed to decode global chunk id: " + error);
    }
    std::vector<std::vector<uint8_t>> local_chunk_ids = decoded.nested_chunk_ids;
    if (local_chunk_ids.empty()) {
      local_chunk_ids.push_back({});
    }
    std::vector<std::vector<uint8_t>> local_chunk_ops;
    const size_t tree_height = static_cast<size_t>(tree.Height());
    for (const auto& local_chunk_id : local_chunk_ids) {
      std::vector<bool> instructions;
      if (!grovedb::BytesToTraversalInstruction(local_chunk_id, &instructions, &error)) {
        Fail("failed to decode local chunk id: " + error);
      }
      size_t chunk_index = 0;
      if (!grovedb::ChunkIndexFromTraversalInstructionWithRecovery(
              instructions, tree_height, &chunk_index, &error)) {
        Fail("failed to derive chunk index: " + error);
      }
      size_t chunk_height = 0;
      if (!grovedb::ChunkHeight(tree_height, chunk_index, &chunk_height, &error)) {
        Fail("failed to derive chunk height: " + error);
      }
      std::vector<uint8_t> chunk_ops;
      if (!tree.GenerateChunkOpsAt(
              instructions, chunk_height, nullptr, false, &chunk_ops, &error)) {
        Fail("failed to generate local chunk ops: " + error);
      }
      local_chunk_ops.push_back(std::move(chunk_ops));
    }
    std::vector<uint8_t> packed_local_chunk_ops;
    if (!grovedb::PackNestedBytes(local_chunk_ops, &packed_local_chunk_ops, &error)) {
      Fail("failed to pack local chunk ops: " + error);
    }
    per_global_chunks.push_back(std::move(packed_local_chunk_ops));
  }

  std::vector<uint8_t> packed_global_chunks;
  if (!grovedb::PackNestedBytes(per_global_chunks, &packed_global_chunks, &error)) {
    Fail("failed to pack global chunks: " + error);
  }
  return packed_global_chunks;
}

void AssertPackUnpackRoundTrip() {
  const std::vector<std::vector<uint8_t>> nested = {
      {0x01, 0x02, 0x03},
      {},
      {0xAA},
      {0x10, 0x11, 0x12, 0x13},
  };
  std::vector<uint8_t> packed;
  std::string error;
  if (!grovedb::PackNestedBytes(nested, &packed, &error)) {
    Fail("pack nested bytes failed: " + error);
  }
  std::vector<std::vector<uint8_t>> unpacked;
  if (!grovedb::UnpackNestedBytes(packed, &unpacked, &error)) {
    Fail("unpack nested bytes failed: " + error);
  }
  Expect(unpacked == nested, "pack/unpack nested bytes mismatch");
}

void AssertUnpackRejectsMalformedData() {
  std::vector<std::vector<uint8_t>> unpacked;
  std::string error;
  if (grovedb::UnpackNestedBytes({}, &unpacked, &error)) {
    Fail("unpack should reject empty input");
  }
  if (error != "Input data is empty") {
    Fail("unexpected empty-input error: " + error);
  }

  if (grovedb::UnpackNestedBytes({0x00}, &unpacked, &error)) {
    Fail("unpack should reject missing count bytes");
  }

  // 1 element but missing its 4-byte length.
  if (grovedb::UnpackNestedBytes({0x00, 0x01}, &unpacked, &error)) {
    Fail("unpack should reject missing nested length");
  }

  // 1 element, length says 3 bytes but only 2 present.
  if (grovedb::UnpackNestedBytes({0x00, 0x01, 0x00, 0x00, 0x00, 0x03, 0xA1, 0xA2},
                                 &unpacked,
                                 &error)) {
    Fail("unpack should reject truncated nested payload");
  }

  // 1 empty element plus one extra byte.
  if (grovedb::UnpackNestedBytes({0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0xFF},
                                 &unpacked,
                                 &error)) {
    Fail("unpack should reject trailing bytes");
  }
}

void AssertGlobalChunkIdRoundTrip() {
  const std::array<uint8_t, 32> prefix = Filled(0x2A);
  const std::optional<std::vector<uint8_t>> root_key =
      std::vector<uint8_t>{'r', 'o', 'o', 't'};
  const std::vector<std::vector<uint8_t>> nested_ids = {
      {0x01, 0x02},
      {0x03},
  };
  std::vector<uint8_t> encoded;
  std::string error;
  if (!grovedb::EncodeGlobalChunkId(prefix,
                                    root_key,
                                    grovedb::ReplicationTreeType::kProvableCountSumTree,
                                    nested_ids,
                                    &encoded,
                                    &error)) {
    Fail("encode global chunk id failed: " + error);
  }
  grovedb::ChunkIdentifier decoded;
  if (!grovedb::DecodeGlobalChunkId(encoded, Filled(0x77), &decoded, &error)) {
    Fail("decode global chunk id failed: " + error);
  }
  Expect(decoded.subtree_prefix == prefix, "decoded prefix mismatch");
  Expect(decoded.root_key.has_value(), "decoded root key should be present");
  Expect(*decoded.root_key == *root_key, "decoded root key mismatch");
  Expect(decoded.tree_type == grovedb::ReplicationTreeType::kProvableCountSumTree,
         "decoded tree type mismatch");
  Expect(decoded.nested_chunk_ids == nested_ids, "decoded nested chunk ids mismatch");
}

void AssertVersionedGlobalChunkIdBehavior() {
  const std::array<uint8_t, 32> prefix = Filled(0x18);
  const std::optional<std::vector<uint8_t>> root_key =
      std::vector<uint8_t>{'v', 'k'};
  const std::vector<std::vector<uint8_t>> nested_ids = {
      {0x01},
      {0x02, 0x03},
  };
  std::vector<uint8_t> encoded;
  std::string error;
  if (!grovedb::EncodeGlobalChunkIdForVersion(prefix,
                                              root_key,
                                              grovedb::ReplicationTreeType::kCountTree,
                                              nested_ids,
                                              grovedb::kCurrentStateSyncVersion,
                                              &encoded,
                                              &error)) {
    Fail("versioned encode failed for supported version: " + error);
  }
  grovedb::ChunkIdentifier decoded;
  if (!grovedb::DecodeGlobalChunkIdForVersion(encoded,
                                              Filled(0x88),
                                              grovedb::kCurrentStateSyncVersion,
                                              &decoded,
                                              &error)) {
    Fail("versioned decode failed for supported version: " + error);
  }
  Expect(decoded.subtree_prefix == prefix, "versioned decode prefix mismatch");
  Expect(decoded.root_key.has_value(), "versioned decode root key should be present");
  Expect(*decoded.root_key == *root_key, "versioned decode root key mismatch");
  Expect(decoded.tree_type == grovedb::ReplicationTreeType::kCountTree,
         "versioned decode tree type mismatch");
  Expect(decoded.nested_chunk_ids == nested_ids, "versioned decode nested ids mismatch");

  error.clear();
  if (grovedb::EncodeGlobalChunkIdForVersion(prefix,
                                             root_key,
                                             grovedb::ReplicationTreeType::kCountTree,
                                             nested_ids,
                                             grovedb::kCurrentStateSyncVersion + 1,
                                             &encoded,
                                             &error)) {
    Fail("versioned encode should reject unsupported version");
  }
  if (error != "unsupported state sync protocol version") {
    Fail("unexpected unsupported-version encode error: " + error);
  }

  error.clear();
  if (grovedb::DecodeGlobalChunkIdForVersion(encoded,
                                             Filled(0x88),
                                             grovedb::kCurrentStateSyncVersion + 1,
                                             &decoded,
                                             &error)) {
    Fail("versioned decode should reject unsupported version");
  }
  if (error != "unsupported state sync protocol version") {
    Fail("unexpected unsupported-version decode error: " + error);
  }

  error.clear();
  if (!grovedb::IsSupportedStateSyncVersion(grovedb::kCurrentStateSyncVersion, &error)) {
    Fail("supported state sync version was rejected: " + error);
  }
  if (grovedb::IsSupportedStateSyncVersion(grovedb::kCurrentStateSyncVersion + 1, &error)) {
    Fail("unsupported state sync version was accepted");
  }
  if (error != "unsupported state sync protocol version") {
    Fail("unexpected unsupported-version state-sync error: " + error);
  }
}

void AssertGlobalChunkIdRejectsOversizedRootKey() {
  const std::array<uint8_t, 32> prefix = Filled(0x33);
  const std::optional<std::vector<uint8_t>> root_key = std::vector<uint8_t>(256, 0xAB);
  std::vector<uint8_t> encoded;
  std::string error;
  if (grovedb::EncodeGlobalChunkId(prefix,
                                   root_key,
                                   grovedb::ReplicationTreeType::kNormalTree,
                                   {},
                                   &encoded,
                                   &error)) {
    Fail("EncodeGlobalChunkId should reject oversized root keys");
  }
  if (error != "root key exceeds 255 bytes") {
    Fail("unexpected oversized root key error: " + error);
  }
}

void AssertPackNestedBytesRejectsTooManyArrays() {
  std::vector<std::vector<uint8_t>> nested(65536);
  std::vector<uint8_t> packed;
  std::string error;
  if (grovedb::PackNestedBytes(nested, &packed, &error)) {
    Fail("PackNestedBytes should reject more than 65535 arrays");
  }
  if (error != "too many nested byte arrays") {
    Fail("unexpected nested count overflow error: " + error);
  }
}

void AssertGlobalChunkIdRootHashSpecialCase() {
  const std::array<uint8_t, 32> app_hash = Filled(0x55);
  const std::vector<uint8_t> global_chunk_id(app_hash.begin(), app_hash.end());
  grovedb::ChunkIdentifier decoded;
  std::string error;
  if (!grovedb::DecodeGlobalChunkId(global_chunk_id, app_hash, &decoded, &error)) {
    Fail("decode root-app-hash chunk id failed: " + error);
  }
  Expect(decoded.subtree_prefix == Filled(0), "root special-case prefix mismatch");
  Expect(!decoded.root_key.has_value(), "root special-case should have no root key");
  Expect(decoded.tree_type == grovedb::ReplicationTreeType::kNormalTree,
         "root special-case tree type mismatch");
  Expect(decoded.nested_chunk_ids.empty(), "root special-case should have no nested ids");
}

void AssertGlobalChunkIdRejectsMalformedData() {
  grovedb::ChunkIdentifier decoded;
  std::string error;
  if (grovedb::DecodeGlobalChunkId({0x01, 0x02, 0x03}, Filled(0x11), &decoded, &error)) {
    Fail("decode should reject too-short global chunk id");
  }
  if (error != "expected global chunk id of at least 32 length") {
    Fail("unexpected short-id error: " + error);
  }

  std::vector<uint8_t> missing_root_size(32, 0xAB);
  if (grovedb::DecodeGlobalChunkId(missing_root_size, Filled(0x11), &decoded, &error)) {
    Fail("decode should reject missing root key size");
  }
  if (error != "unable to decode root key size") {
    Fail("unexpected missing-root-size error: " + error);
  }

  std::vector<uint8_t> truncated_root(32, 0xAB);
  truncated_root.push_back(3);  // root key len 3
  truncated_root.push_back(0x01);
  truncated_root.push_back(0x02);  // one byte missing
  if (grovedb::DecodeGlobalChunkId(truncated_root, Filled(0x11), &decoded, &error)) {
    Fail("decode should reject truncated root key");
  }
  if (error != "unable to decode root key") {
    Fail("unexpected truncated-root error: " + error);
  }

  std::vector<uint8_t> missing_tree_type(32, 0xAB);
  missing_tree_type.push_back(0);  // empty root key
  if (grovedb::DecodeGlobalChunkId(missing_tree_type, Filled(0x11), &decoded, &error)) {
    Fail("decode should reject missing tree type");
  }
  if (error != "unable to decode root key") {
    Fail("unexpected missing-tree-type error: " + error);
  }

  std::vector<uint8_t> invalid_tree_type(32, 0xAB);
  invalid_tree_type.push_back(0);
  invalid_tree_type.push_back(7);
  if (grovedb::DecodeGlobalChunkId(invalid_tree_type, Filled(0x11), &decoded, &error)) {
    Fail("decode should reject unknown tree type");
  }
  if (error != "got 7, max is 6") {
    Fail("unexpected tree-type error: " + error);
  }
}

void AssertStateSyncSessionLifecycle() {
  std::string error;
  grovedb::MerkTree tree;
  for (char c = 'a'; c <= 'm'; ++c) {
    if (!tree.Insert({static_cast<uint8_t>(c)}, {static_cast<uint8_t>(c)}, &error)) {
      Fail("insert failed: " + error);
    }
  }
  std::vector<uint8_t> root_hash;
  if (!tree.ComputeRootHash(nullptr, &root_hash, &error)) {
    Fail("failed to compute root hash: " + error);
  }
  const std::array<uint8_t, 32> app_hash = ToArray32(root_hash);

  grovedb::StateSyncSession session;
  if (!grovedb::StartStateSyncSession(app_hash,
                                      1,
                                      grovedb::kCurrentStateSyncVersion,
                                      &session,
                                      &error)) {
    Fail("failed to start state sync session: " + error);
  }

  bool sync_completed = false;
  if (!grovedb::IsStateSyncSessionCompleted(&session, &sync_completed, &error)) {
    Fail("failed to query initial sync completion: " + error);
  }
  if (sync_completed) {
    Fail("newly started state sync session should not be completed");
  }

  for (size_t iter = 0; iter < 256; ++iter) {
    std::vector<uint8_t> packed_request_ids;
    if (!grovedb::FetchStateSyncChunk(&session, 32, &packed_request_ids, &error)) {
      Fail("failed to fetch state sync chunk ids: " + error);
    }
    std::vector<std::vector<uint8_t>> requested_global_ids;
    if (!grovedb::UnpackNestedBytes(packed_request_ids, &requested_global_ids, &error)) {
      Fail("failed to unpack fetched request ids: " + error);
    }
    if (requested_global_ids.empty()) {
      if (sync_completed) {
        break;
      }
      Fail("fetched no ids before sync completed");
    }

    const std::vector<uint8_t> packed_global_chunks =
        BuildPackedGlobalChunksForRequest(packed_request_ids, app_hash, tree);
    std::vector<uint8_t> packed_next_global_ids;
    if (!grovedb::ApplyStateSyncChunk(&session,
                                      packed_request_ids,
                                      packed_global_chunks,
                                      grovedb::kCurrentStateSyncVersion,
                                      &packed_next_global_ids,
                                      &sync_completed,
                                      &error)) {
      Fail("failed to apply state sync chunk: " + error);
    }
    bool queried_sync_completed = false;
    if (!grovedb::IsStateSyncSessionCompleted(&session, &queried_sync_completed, &error)) {
      Fail("failed to query sync completion after apply: " + error);
    }
    if (queried_sync_completed != sync_completed) {
      Fail("queried sync-completed state mismatch");
    }
    if (sync_completed) {
      std::vector<std::vector<uint8_t>> next_ids;
      if (!grovedb::UnpackNestedBytes(packed_next_global_ids, &next_ids, &error)) {
        Fail("failed to unpack next ids: " + error);
      }
      if (!next_ids.empty()) {
        Fail("sync completed but returned additional next ids");
      }
      break;
    }
  }

  if (!sync_completed) {
    Fail("state sync session did not complete");
  }

  bool queried_sync_completed = false;
  if (!grovedb::IsStateSyncSessionCompleted(&session, &queried_sync_completed, &error)) {
    Fail("failed to query completion before commit: " + error);
  }
  if (!queried_sync_completed) {
    Fail("completed state sync session should report completed");
  }

  if (!grovedb::CommitStateSyncSession(&session, &error)) {
    Fail("commit state sync session failed: " + error);
  }

  std::vector<uint8_t> packed_after_commit;
  error.clear();
  if (grovedb::FetchStateSyncChunk(&session, 1, &packed_after_commit, &error)) {
    Fail("fetch should fail after session commit");
  }
  if (error != "state sync session is not started") {
    Fail("unexpected fetch-after-commit error: " + error);
  }

  error.clear();
  if (grovedb::IsStateSyncSessionCompleted(&session, &queried_sync_completed, &error)) {
    Fail("completion query should fail after session commit");
  }
  if (error != "state sync session is not started") {
    Fail("unexpected completion-query-after-commit error: " + error);
  }
}

void AssertStateSyncSessionRejectsInvalidUsage() {
  std::string error;
  grovedb::StateSyncSession session;
  std::vector<uint8_t> packed;
  if (grovedb::FetchStateSyncChunk(&session, 1, &packed, &error)) {
    Fail("fetch should fail for non-started session");
  }
  if (error != "state sync session is not started") {
    Fail("unexpected fetch error for non-started session: " + error);
  }

  bool sync_completed = false;
  std::vector<uint8_t> packed_next_ids;
  error.clear();
  if (grovedb::ApplyStateSyncChunk(&session,
                                   {},
                                   {},
                                   grovedb::kCurrentStateSyncVersion,
                                   &packed_next_ids,
                                   &sync_completed,
                                   &error)) {
    Fail("apply should fail for non-started session");
  }
  if (error != "state sync session is not started") {
    Fail("unexpected apply error for non-started session: " + error);
  }

  error.clear();
  if (grovedb::IsStateSyncSessionCompleted(&session, &sync_completed, &error)) {
    Fail("completion query should fail for non-started session");
  }
  if (error != "state sync session is not started") {
    Fail("unexpected completion query error for non-started session: " + error);
  }

  error.clear();
  if (grovedb::CommitStateSyncSession(&session, &error)) {
    Fail("commit should fail for non-started session");
  }
  if (error != "state sync session is not started") {
    Fail("unexpected commit error for non-started session: " + error);
  }

  error.clear();
  if (grovedb::StartStateSyncSession(Filled(0x01),
                                     0,
                                     grovedb::kCurrentStateSyncVersion,
                                     &session,
                                     &error)) {
    Fail("start session should reject zero subtrees_batch_size");
  }
  if (error != "subtrees_batch_size cannot be zero") {
    Fail("unexpected zero subtrees_batch_size error: " + error);
  }

  error.clear();
  if (!grovedb::StartStateSyncSession(Filled(0x01),
                                      1,
                                      grovedb::kCurrentStateSyncVersion,
                                      &session,
                                      &error)) {
    Fail("start session should succeed: " + error);
  }
  std::vector<uint8_t> packed_request_ids;
  if (!grovedb::FetchStateSyncChunk(&session, 1, &packed_request_ids, &error)) {
    Fail("fetch request ids should succeed: " + error);
  }
  const std::vector<std::vector<uint8_t>> bad_global_chunks = {
      {0x00, 0x00},
      {0x00, 0x00},
  };
  std::vector<uint8_t> packed_bad_global_chunks;
  if (!grovedb::PackNestedBytes(bad_global_chunks, &packed_bad_global_chunks, &error)) {
    Fail("failed to pack malformed global chunks: " + error);
  }

  error.clear();
  if (grovedb::ApplyStateSyncChunk(&session,
                                   packed_request_ids,
                                   packed_bad_global_chunks,
                                   grovedb::kCurrentStateSyncVersion,
                                   &packed_next_ids,
                                   &sync_completed,
                                   &error)) {
    Fail("apply should reject mismatched global chunk counts");
  }
  if (error != "Packed num of global chunkIDs and chunks are not matching") {
    Fail("unexpected apply mismatch error: " + error);
  }

  error.clear();
  if (!grovedb::CommitStateSyncSession(&session, &error)) {
    Fail("commit should succeed for started session: " + error);
  }

  error.clear();
  if (grovedb::CommitStateSyncSession(&session, &error)) {
    Fail("second commit should fail after session is consumed");
  }
  if (error != "state sync session is not started") {
    Fail("unexpected second-commit error: " + error);
  }
}

}  // namespace

int main() {
  AssertPackUnpackRoundTrip();
  AssertUnpackRejectsMalformedData();
  AssertGlobalChunkIdRoundTrip();
  AssertVersionedGlobalChunkIdBehavior();
  AssertGlobalChunkIdRejectsOversizedRootKey();
  AssertPackNestedBytesRejectsTooManyArrays();
  AssertGlobalChunkIdRootHashSpecialCase();
  AssertGlobalChunkIdRejectsMalformedData();
  AssertStateSyncSessionLifecycle();
  AssertStateSyncSessionRejectsInvalidUsage();
  return 0;
}
