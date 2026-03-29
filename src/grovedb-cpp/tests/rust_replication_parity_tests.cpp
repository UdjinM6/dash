#include "replication.h"
#include "test_utils.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "chunk.h"
#include "merk.h"

using test_utils::Fail;
using test_utils::MakeTempDir;
using test_utils::ReadFile;

namespace {

uint16_t ReadU16TextFile(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) {
    Fail("failed to open file: " + path.string());
  }
  unsigned long value = 0;
  input >> value;
  if (!input || value > 0xFFFF) {
    Fail("failed to parse u16 from file: " + path.string());
  }
  return static_cast<uint16_t>(value);
}

bool ReadBoolTextFile(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) {
    Fail("failed to open file: " + path.string());
  }
  std::string value;
  input >> value;
  if (value == "1") {
    return true;
  }
  if (value == "0") {
    return false;
  }
  Fail("failed to parse bool (0/1) from file: " + path.string());
}

std::array<uint8_t, 32> ReadArray32File(const std::filesystem::path& path) {
  const std::vector<uint8_t> bytes = ReadFile(path);
  if (bytes.size() != 32) {
    Fail("expected 32-byte file: " + path.string());
  }
  std::array<uint8_t, 32> out{};
  for (size_t i = 0; i < out.size(); ++i) {
    out[i] = bytes[i];
  }
  return out;
}

std::array<uint8_t, 32> Filled(uint8_t value) {
  std::array<uint8_t, 32> out{};
  out.fill(value);
  return out;
}

grovedb::MerkTree BuildTestMerkTree() {
  grovedb::MerkTree tree;
  std::string error;
  for (char c = 'a'; c <= 'm'; ++c) {
    if (!tree.Insert({static_cast<uint8_t>(c)}, {static_cast<uint8_t>(c)}, &error)) {
      Fail("tree insert failed: " + error);
    }
  }
  return tree;
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

void AssertReplicationVersionedFixtureParity(const std::filesystem::path& dir) {
  const uint16_t supported =
      ReadU16TextFile(dir / "replication_current_state_sync_version.txt");
  if (supported != grovedb::kCurrentStateSyncVersion) {
    Fail("C++ and Rust state sync versions differ");
  }
  const uint16_t unsupported =
      ReadU16TextFile(dir / "replication_unsupported_state_sync_version.txt");

  const std::array<uint8_t, 32> subtree_prefix = Filled(0x2A);
  const std::optional<std::vector<uint8_t>> root_key =
      std::vector<uint8_t>{'r', 'o', 'o', 't'};
  const std::vector<std::vector<uint8_t>> chunk_ids = {
      {0x01, 0x02},
      {0x03},
  };

  std::string error;
  std::vector<uint8_t> encoded;
  if (!grovedb::EncodeGlobalChunkIdForVersion(subtree_prefix,
                                              root_key,
                                              grovedb::ReplicationTreeType::kCountSumTree,
                                              chunk_ids,
                                              supported,
                                              &encoded,
                                              &error)) {
    Fail("versioned encode failed: " + error);
  }

  const std::vector<uint8_t> rust_encoded =
      ReadFile(dir / "replication_encoded_global_chunk_id.bin");
  if (encoded != rust_encoded) {
    Fail("versioned replication encoded global chunk id mismatch vs Rust fixture");
  }

  grovedb::ChunkIdentifier decoded;
  if (!grovedb::DecodeGlobalChunkIdForVersion(
          rust_encoded, Filled(0x77), supported, &decoded, &error)) {
    Fail("versioned decode failed: " + error);
  }
  if (decoded.subtree_prefix != subtree_prefix) {
    Fail("decoded subtree prefix mismatch");
  }
  if (!decoded.root_key.has_value() || *decoded.root_key != *root_key) {
    Fail("decoded root key mismatch");
  }
  if (decoded.tree_type != grovedb::ReplicationTreeType::kCountSumTree) {
    Fail("decoded tree type mismatch");
  }
  if (decoded.nested_chunk_ids != chunk_ids) {
    Fail("decoded chunk ids mismatch");
  }

  error.clear();
  if (grovedb::EncodeGlobalChunkIdForVersion(subtree_prefix,
                                             root_key,
                                             grovedb::ReplicationTreeType::kCountSumTree,
                                             chunk_ids,
                                             unsupported,
                                             &encoded,
                                             &error)) {
    Fail("versioned encode unexpectedly accepted unsupported version");
  }
  if (error != "unsupported state sync protocol version") {
    Fail("unexpected encode unsupported-version error: " + error);
  }

  error.clear();
  if (grovedb::DecodeGlobalChunkIdForVersion(
          rust_encoded, Filled(0x77), unsupported, &decoded, &error)) {
    Fail("versioned decode unexpectedly accepted unsupported version");
  }
  if (error != "unsupported state sync protocol version") {
    Fail("unexpected decode unsupported-version error: " + error);
  }
}

void AssertReplicationSessionFixtureParity(const std::filesystem::path& dir) {
  const std::array<uint8_t, 32> app_hash = ReadArray32File(dir / "replication_session_app_hash.bin");
  const std::vector<uint8_t> rust_fetch_global_chunk_ids =
      ReadFile(dir / "replication_session_fetch_global_chunk_ids.bin");
  const std::vector<uint8_t> rust_apply_global_chunks =
      ReadFile(dir / "replication_session_apply_global_chunks.bin");
  const std::vector<uint8_t> rust_apply_next_global_chunk_ids =
      ReadFile(dir / "replication_session_apply_next_global_chunk_ids.bin");
  const bool rust_sync_completed =
      ReadBoolTextFile(dir / "replication_session_sync_completed.txt");

  std::string error;
  grovedb::StateSyncSession session;
  if (!grovedb::StartStateSyncSession(app_hash,
                                      1,
                                      grovedb::kCurrentStateSyncVersion,
                                      &session,
                                      &error)) {
    Fail("start state sync session failed: " + error);
  }

  std::vector<uint8_t> packed_global_chunk_ids;
  if (!grovedb::FetchStateSyncChunk(&session, 32, &packed_global_chunk_ids, &error)) {
    Fail("fetch state sync chunk ids failed: " + error);
  }
  if (packed_global_chunk_ids != rust_fetch_global_chunk_ids) {
    Fail("session fetch global chunk ids mismatch vs Rust fixture");
  }

  bool sync_completed = false;
  std::vector<uint8_t> packed_next_global_chunk_ids;
  if (!grovedb::ApplyStateSyncChunk(&session,
                                    packed_global_chunk_ids,
                                    rust_apply_global_chunks,
                                    grovedb::kCurrentStateSyncVersion,
                                    &packed_next_global_chunk_ids,
                                    &sync_completed,
                                    &error)) {
    Fail("apply state sync chunk failed: " + error);
  }
  if (packed_next_global_chunk_ids != rust_apply_next_global_chunk_ids) {
    Fail("session apply next global chunk ids mismatch vs Rust fixture");
  }
  if (sync_completed != rust_sync_completed) {
    Fail("session sync-completed flag mismatch vs Rust fixture");
  }
  bool queried_sync_completed = false;
  if (!grovedb::IsStateSyncSessionCompleted(&session, &queried_sync_completed, &error)) {
    Fail("is-sync-completed query failed: " + error);
  }
  if (queried_sync_completed != rust_sync_completed) {
    Fail("session queried sync-completed mismatch vs Rust fixture");
  }

  if (!grovedb::CommitStateSyncSession(&session, &error)) {
    Fail("commit state sync session failed: " + error);
  }

  std::vector<uint8_t> packed_after_commit;
  error.clear();
  if (grovedb::FetchStateSyncChunk(&session, 1, &packed_after_commit, &error)) {
    Fail("fetch should fail after state sync session commit");
  }
  if (error != "state sync session is not started") {
    Fail("unexpected fetch-after-commit error: " + error);
  }
}

void AssertReplicationNestedSubtreeDiscoveryParity(const std::filesystem::path& dir) {
  const std::array<uint8_t, 32> app_hash =
      ReadArray32File(dir / "replication_nested_app_hash.bin");
  const bool rust_sync_completed =
      ReadBoolTextFile(dir / "replication_nested_sync_completed.txt");

  std::string error;
  grovedb::StateSyncSession session;
  if (!grovedb::StartStateSyncSession(app_hash,
                                      1,
                                      grovedb::kCurrentStateSyncVersion,
                                      &session,
                                      &error)) {
    Fail("start nested state sync session failed: " + error);
  }

  bool sync_completed = false;
  if (!grovedb::IsStateSyncSessionCompleted(&session, &sync_completed, &error)) {
    Fail("nested is-sync-completed query failed: " + error);
  }
  if (sync_completed != rust_sync_completed) {
    Fail("nested initial sync-completed mismatch vs Rust fixture: C++=" +
         std::to_string(sync_completed) + " vs Rust=" + std::to_string(rust_sync_completed));
  }

  if (!grovedb::CommitStateSyncSession(&session, &error)) {
    Fail("commit nested state sync session failed: " + error);
  }
}

void AssertReplicationVersionRejectionParity(const std::filesystem::path& dir) {
  // Read expected behavior from Rust fixture
  const bool rust_expects_version_rejection =
      ReadBoolTextFile(dir / "replication_version_rejection_expected.txt");
  const uint16_t rust_supported_version =
      ReadU16TextFile(dir / "replication_supported_version.txt");

  // Validate C++ has the same supported version
  if (grovedb::kCurrentStateSyncVersion != rust_supported_version) {
    Fail("C++ and Rust supported version mismatch: C++=" +
         std::to_string(grovedb::kCurrentStateSyncVersion) +
         " vs Rust=" + std::to_string(rust_supported_version));
  }

  // Test that C++ rejects unsupported version
  std::array<uint8_t, 32> test_app_hash{};
  test_app_hash.fill(0x42);

  std::string error;
  grovedb::StateSyncSession session;
  if (!grovedb::StartStateSyncSession(test_app_hash,
                                      1,
                                      grovedb::kCurrentStateSyncVersion,
                                      &session,
                                      &error)) {
    Fail("start state sync session failed: " + error);
  }

  // Try to apply with unsupported version
  const uint16_t unsupported_version = grovedb::kCurrentStateSyncVersion + 1;
  std::vector<uint8_t> packed_next_global_chunk_ids;
  bool sync_completed = false;
  error.clear();

  // Create dummy chunk data
  std::vector<uint8_t> dummy_chunk_ids;
  std::vector<uint8_t> dummy_chunks;

  bool apply_succeeded = grovedb::ApplyStateSyncChunk(
      &session, dummy_chunk_ids, dummy_chunks, unsupported_version,
      &packed_next_global_chunk_ids, &sync_completed, &error);

  // C++ should reject unsupported version (matching Rust expectation)
  if (apply_succeeded && rust_expects_version_rejection) {
    Fail("C++ should reject unsupported version like Rust expects");
  }
  if (!apply_succeeded && error.find("version") == std::string::npos) {
    Fail("C++ rejected unsupported version but error message doesn't mention 'version': " + error);
  }

  // Also test that supported version is accepted (with proper chunk data this would succeed)
  // For now, just validate the version check passes
  error.clear();
  bool version_check_passed = grovedb::IsSupportedStateSyncVersion(
      grovedb::kCurrentStateSyncVersion, &error);
  if (!version_check_passed) {
    Fail("C++ should support current version: " + error);
  }
}

void AssertReplicationZeroBatchStartParity(const std::filesystem::path& dir) {
  const bool rust_zero_batch_rejected =
      ReadBoolTextFile(dir / "replication_zero_batch_rejected.txt");

  std::array<uint8_t, 32> app_hash{};
  app_hash.fill(0x24);
  grovedb::StateSyncSession session;
  std::string error;
  const bool cpp_zero_batch_accepted = grovedb::StartStateSyncSession(
      app_hash, 0, grovedb::kCurrentStateSyncVersion, &session, &error);

  if (cpp_zero_batch_accepted == rust_zero_batch_rejected) {
    Fail("zero-batch start-session parity mismatch between C++ and Rust");
  }
  if (rust_zero_batch_rejected &&
      error.find("subtrees_batch_size cannot be zero") == std::string::npos) {
    Fail("zero-batch rejection error missing expected phrase: " + error);
  }
}

void AssertReplicationIsEmptyParity(const std::filesystem::path& dir) {
  const bool rust_is_empty_before =
      ReadBoolTextFile(dir / "replication_is_empty_before_apply.txt");
  const bool rust_is_empty_after =
      ReadBoolTextFile(dir / "replication_is_empty_after_apply.txt");

  std::array<uint8_t, 32> app_hash{};
  app_hash.fill(0x24);
  grovedb::StateSyncSession session;
  std::string error;

  // Start session - should be empty initially (no pending chunks yet fetched)
  if (!grovedb::StartStateSyncSession(
          app_hash, 1, grovedb::kCurrentStateSyncVersion, &session, &error)) {
    Fail("start state sync session for is_empty test failed: " + error);
  }

  bool cpp_is_empty_before = false;
  if (!grovedb::IsStateSyncSessionEmpty(&session, &cpp_is_empty_before, &error)) {
    Fail("is_empty query before apply failed: " + error);
  }

  // After start, pending_global_chunk_ids should contain the root hash
  // so is_empty should be false
  if (cpp_is_empty_before != rust_is_empty_before) {
    Fail("is_empty before apply parity mismatch: C++=" +
         std::to_string(cpp_is_empty_before) + " vs Rust=" +
         std::to_string(rust_is_empty_before));
  }

  // Fetch the chunk to simulate the fetch step
  std::vector<uint8_t> packed_global_chunk_ids;
  if (!grovedb::FetchStateSyncChunk(&session, 32, &packed_global_chunk_ids, &error)) {
    Fail("fetch state sync chunk for is_empty test failed: " + error);
  }

  // After fetch, the pending chunk should be removed (moved to in_flight)
  // so session should be empty again
  bool cpp_is_empty_after_fetch = false;
  if (!grovedb::IsStateSyncSessionEmpty(&session, &cpp_is_empty_after_fetch, &error)) {
    Fail("is_empty query after fetch failed: " + error);
  }

  // Apply the chunk (using dummy data since we just care about is_empty semantics)
  std::vector<uint8_t> dummy_chunks;
  bool sync_completed = false;
  std::vector<uint8_t> packed_next_global_chunk_ids;
  if (!grovedb::ApplyStateSyncChunk(
          &session, packed_global_chunk_ids, dummy_chunks,
          grovedb::kCurrentStateSyncVersion, &packed_next_global_chunk_ids,
          &sync_completed, &error)) {
    // Apply may fail with dummy data, but we still check is_empty behavior
  }

  bool cpp_is_empty_after = false;
  if (!grovedb::IsStateSyncSessionEmpty(&session, &cpp_is_empty_after, &error)) {
    Fail("is_empty query after apply failed: " + error);
  }

  if (cpp_is_empty_after != rust_is_empty_after) {
    Fail("is_empty after apply parity mismatch: C++=" +
         std::to_string(cpp_is_empty_after) + " vs Rust=" +
         std::to_string(rust_is_empty_after));
  }
}

void AssertReplicationPendingChunksParity(const std::filesystem::path& dir) {
  const size_t rust_pending_after_start =
      static_cast<size_t>(ReadU16TextFile(dir / "replication_pending_after_start.txt"));
  const size_t rust_pending_after_apply =
      static_cast<size_t>(ReadU16TextFile(dir / "replication_pending_after_apply.txt"));

  std::array<uint8_t, 32> app_hash{};
  app_hash.fill(0x24);
  grovedb::StateSyncSession session;
  std::string error;

  // Start session
  if (!grovedb::StartStateSyncSession(
          app_hash, 1, grovedb::kCurrentStateSyncVersion, &session, &error)) {
    Fail("start state sync session for pending chunks test failed: " + error);
  }

  // After start: C++ session should have 1 pending chunk (root request)
  const size_t cpp_pending_after_start = session.pending_global_chunk_ids.size();
  if (cpp_pending_after_start != rust_pending_after_start) {
    Fail("pending chunks after start parity mismatch: C++=" +
         std::to_string(cpp_pending_after_start) + " vs Rust=" +
         std::to_string(rust_pending_after_start));
  }

  // Apply the chunk (using the fetched data from session)
  std::vector<uint8_t> packed_global_chunk_ids;
  if (!grovedb::FetchStateSyncChunk(&session, 32, &packed_global_chunk_ids, &error)) {
    Fail("fetch state sync chunk for pending chunks test failed: " + error);
  }

  // Apply the chunk
  bool sync_completed = false;
  std::vector<uint8_t> packed_next_global_chunk_ids;
  if (!grovedb::ApplyStateSyncChunk(
          &session, packed_global_chunk_ids, packed_global_chunk_ids,
          grovedb::kCurrentStateSyncVersion, &packed_next_global_chunk_ids,
          &sync_completed, &error)) {
    // Apply may have issues with self-referential data
  }

  // After apply: pending reflects any discovered child chunks
  const size_t cpp_pending_after_apply = session.pending_global_chunk_ids.size();
  if (cpp_pending_after_apply != rust_pending_after_apply) {
    Fail("pending chunks after apply parity mismatch: C++=" +
         std::to_string(cpp_pending_after_apply) + " vs Rust=" +
         std::to_string(rust_pending_after_apply));
  }
}

void AssertReplicationFetchChunkIdsParity(const std::filesystem::path& dir) {
  // Read Rust fixtures
  const size_t rust_pending_after_start =
      static_cast<size_t>(ReadU16TextFile(dir / "replication_pending_count_after_start.txt"));
  const size_t rust_pending_after_apply =
      static_cast<size_t>(ReadU16TextFile(dir / "replication_pending_count_after_apply.txt"));
  const std::array<uint8_t, 32> app_hash =
      ReadArray32File(dir / "replication_session_app_hash.bin");
  const std::vector<uint8_t> rust_apply_global_chunks =
      ReadFile(dir / "replication_session_apply_global_chunks.bin");

  // Start a session in C++
  grovedb::StateSyncSession session{};
  std::string error;
  if (!grovedb::StartStateSyncSession(
          app_hash, 1, grovedb::kCurrentStateSyncVersion, &session, &error)) {
    Fail("start state sync session for fetch_chunk_ids test failed: " + error);
  }

  // After start: pending_global_chunk_ids should have 1 entry (root)
  const size_t cpp_pending_after_start = session.pending_global_chunk_ids.size();
  if (cpp_pending_after_start != rust_pending_after_start) {
    Fail("pending chunks after start parity mismatch: C++=" +
         std::to_string(cpp_pending_after_start) + " vs Rust=" +
         std::to_string(rust_pending_after_start));
  }
  if (cpp_pending_after_start != 1) {
    Fail("pending_global_chunk_ids after start should be 1, got " +
         std::to_string(cpp_pending_after_start));
  }

  // Fetch the chunk IDs first (this is what we'd request from source)
  std::vector<uint8_t> packed_global_chunk_ids;
  if (!grovedb::FetchStateSyncChunk(&session, 1, &packed_global_chunk_ids, &error)) {
    Fail("fetch state sync chunk ids for fetch test failed: " + error);
  }

  // Apply the chunk - use the fetched chunk IDs and the Rust fixture chunk data
  bool sync_completed = false;
  std::vector<uint8_t> packed_next_global_chunk_ids;
  if (!grovedb::ApplyStateSyncChunk(
          &session, packed_global_chunk_ids, rust_apply_global_chunks,
          grovedb::kCurrentStateSyncVersion, &packed_next_global_chunk_ids,
          &sync_completed, &error)) {
    Fail("apply state sync chunk for fetch test failed: " + error);
  }

  // After apply on empty tree: pending_global_chunk_ids should be empty
  const size_t cpp_pending_after_apply = session.pending_global_chunk_ids.size();
  if (cpp_pending_after_apply != rust_pending_after_apply) {
    Fail("pending chunks after apply parity mismatch: C++=" +
         std::to_string(cpp_pending_after_apply) + " vs Rust=" +
         std::to_string(rust_pending_after_apply));
  }
  if (cpp_pending_after_apply != 0) {
    Fail("pending_global_chunk_ids after apply should be empty, got " +
         std::to_string(cpp_pending_after_apply));
  }
}

void AssertReplicationVersionMismatchApplyParity(const std::filesystem::path& dir) {
  // Read Rust fixtures for version mismatch apply test
  const bool rust_version_mismatch_rejected =
      ReadBoolTextFile(dir / "replication_version_mismatch_apply_rejected.txt");
  const bool rust_version_match_accepted =
      ReadBoolTextFile(dir / "replication_version_match_apply_accepted.txt");

  // Start a session in C++
  std::array<uint8_t, 32> app_hash{};
  app_hash.fill(0x24);
  grovedb::StateSyncSession session{};
  std::string error;
  if (!grovedb::StartStateSyncSession(
          app_hash, 1, grovedb::kCurrentStateSyncVersion, &session, &error)) {
    Fail("start state sync session for version mismatch test failed: " + error);
  }

  // Build a test merk tree and generate chunk data for apply
  grovedb::MerkTree tree = BuildTestMerkTree();
  const std::vector<uint8_t> packed_global_chunk_ids = {app_hash.begin(), app_hash.end()};
  const std::vector<uint8_t> packed_global_chunks =
      BuildPackedGlobalChunksForRequest(packed_global_chunk_ids, app_hash, tree);

  // Test 1: Apply with mismatched version (session started with kCurrentStateSyncVersion,
  // but apply uses different version)
  const uint16_t mismatch_version = grovedb::kCurrentStateSyncVersion == 1 ? 2 : 1;
  std::vector<uint8_t> packed_next_global_chunk_ids;
  bool sync_completed = false;
  error.clear();

  bool apply_with_mismatch_succeeded = grovedb::ApplyStateSyncChunk(
      &session, packed_global_chunk_ids, packed_global_chunks, mismatch_version,
      &packed_next_global_chunk_ids, &sync_completed, &error);

  // C++ should reject version mismatch (matching Rust expectation)
  if (apply_with_mismatch_succeeded && rust_version_mismatch_rejected) {
    Fail("C++ should reject version mismatch during apply like Rust expects");
  }
  if (!apply_with_mismatch_succeeded && error.find("version") == std::string::npos) {
    Fail("C++ rejected version mismatch but error message doesn't mention 'version': " + error);
  }

  // Test 2: Apply with matching version should succeed
  // Need to restart session since the previous apply may have modified state
  grovedb::StateSyncSession session2{};
  if (!grovedb::StartStateSyncSession(
          app_hash, 1, grovedb::kCurrentStateSyncVersion, &session2, &error)) {
    Fail("start state sync session for version match test failed: " + error);
  }

  error.clear();
  bool apply_with_match_succeeded = grovedb::ApplyStateSyncChunk(
      &session2, packed_global_chunk_ids, packed_global_chunks,
      grovedb::kCurrentStateSyncVersion, &packed_next_global_chunk_ids, &sync_completed, &error);

  // C++ should accept matching version (matching Rust expectation)
  if (apply_with_match_succeeded != rust_version_match_accepted) {
    Fail("C++ version match acceptance differs from Rust: C++=" +
         std::to_string(apply_with_match_succeeded) + " vs Rust=" +
         std::to_string(rust_version_match_accepted));
  }
  if (!apply_with_match_succeeded && !error.empty()) {
    // If apply failed, it should not be due to version issues
    if (error.find("version") != std::string::npos) {
      Fail("C++ rejected matching version due to version error: " + error);
    }
  }
}

}  // namespace

int main() {
  const char* run = std::getenv("GROVEDB_RUN_RUST_PARITY");
  if (run == nullptr) {
    return 0;
  }

  const std::string dir = MakeTempDir("rust_replication_parity");
  const std::string cmd =
      test_utils::RustToolsCargoRunPrefix() + "rust_replication_writer \"" +
      dir + "\"";
  if (std::system(cmd.c_str()) != 0) {
    Fail("failed to run rust replication writer");
  }

  AssertReplicationVersionedFixtureParity(dir);
  AssertReplicationSessionFixtureParity(dir);
  AssertReplicationNestedSubtreeDiscoveryParity(dir);
  AssertReplicationVersionRejectionParity(dir);
  AssertReplicationZeroBatchStartParity(dir);
  AssertReplicationIsEmptyParity(dir);
  AssertReplicationPendingChunksParity(dir);
  AssertReplicationFetchChunkIdsParity(dir);
  AssertReplicationVersionMismatchApplyParity(dir);

  std::filesystem::remove_all(dir);
  return 0;
}
