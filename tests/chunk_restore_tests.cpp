#include "chunk_restore.h"
#include "chunk.h"
#include "merk.h"
#include "merk_node.h"
#include "rocksdb_wrapper.h"
#include "binary.h"
#include "test_utils.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

using test_utils::Fail;
using test_utils::MakeTempDir;

namespace {
bool FindFirstChunkOpsOffset(const std::vector<uint8_t>& proof, size_t* out) {
  if (out == nullptr) {
    return false;
  }
  size_t cursor = 0;
  if (cursor >= proof.size()) {
    return false;
  }
  if (proof[cursor++] != 0x00) {
    return false;
  }
  uint64_t id_len = 0;
  if (!grovedb::ReadVarintU64(proof, &cursor, &id_len, nullptr)) {
    return false;
  }
  if (cursor + id_len > proof.size()) {
    return false;
  }
  cursor += static_cast<size_t>(id_len);
  if (cursor >= proof.size() || proof[cursor++] != 0x01) {
    return false;
  }
  uint64_t ops_len = 0;
  if (!grovedb::ReadVarintU64(proof, &cursor, &ops_len, nullptr)) {
    return false;
  }
  (void)ops_len;
  *out = cursor;
  return cursor < proof.size();
}
}  // namespace

void TestChunkRestoreHappyPath() {
  std::string error;
  grovedb::MerkTree tree;
  for (char c = 'a'; c <= 'z'; ++c) {
    if (!tree.Insert({static_cast<uint8_t>(c)}, {static_cast<uint8_t>(c)}, &error)) {
      Fail("insert failed: " + error);
    }
  }

  std::vector<uint8_t> root_hash;
  if (!tree.ComputeRootHash(nullptr, &root_hash, &error)) {
    Fail("compute root hash failed: " + error);
  }

  grovedb::ChunkRestorer restorer;
  if (!restorer.Init(root_hash, nullptr, &error)) {
    Fail("restorer init failed: " + error);
  }

  std::vector<uint8_t> root_chunk;
  if (!tree.GenerateChunkProof(2, nullptr, false, &root_chunk, &error)) {
    Fail("generate root chunk failed: " + error);
  }

  std::vector<std::vector<uint8_t>> next_chunks;
  if (!restorer.ProcessChunkProof(root_chunk, &next_chunks, &error)) {
    Fail("process root chunk failed: " + error);
  }

  while (!next_chunks.empty()) {
    std::vector<std::vector<uint8_t>> pending;
    for (const auto& chunk_id : next_chunks) {
      std::vector<bool> instructions;
      if (!grovedb::BytesToTraversalInstruction(chunk_id, &instructions, &error)) {
        Fail("decode chunk id failed: " + error);
      }
      std::vector<uint8_t> chunk;
      if (!tree.GenerateChunkProofAt(instructions, 2, nullptr, false, &chunk, &error)) {
        Fail("generate chunk failed: " + error);
      }
      std::vector<std::vector<uint8_t>> more;
      if (!restorer.ProcessChunkProof(chunk, &more, &error)) {
        Fail("process chunk failed: " + error);
      }
      pending.insert(pending.end(), more.begin(), more.end());
    }
    next_chunks.swap(pending);
  }

  if (restorer.HasPendingChunks()) {
    Fail("expected no pending chunks after restore");
  }
}

void TestChunkRestoreBadHash() {
  std::string error;
  grovedb::MerkTree tree;
  if (!tree.Insert({'a'}, {'1'}, &error)) {
    Fail("insert failed: " + error);
  }
  std::vector<uint8_t> root_hash;
  if (!tree.ComputeRootHash(nullptr, &root_hash, &error)) {
    Fail("compute root hash failed: " + error);
  }
  root_hash[0] ^= 0xFF;
  grovedb::ChunkRestorer restorer;
  if (!restorer.Init(root_hash, nullptr, &error)) {
    Fail("restorer init failed: " + error);
  }
  std::vector<uint8_t> chunk;
  if (!tree.GenerateChunkProof(2, nullptr, false, &chunk, &error)) {
    Fail("generate chunk failed: " + error);
  }
  std::vector<std::vector<uint8_t>> next_chunks;
  if (restorer.ProcessChunkProof(chunk, &next_chunks, &error)) {
    Fail("expected hash mismatch to fail");
  }
  if (error.find("chunk doesn't match expected root hash") == std::string::npos) {
    Fail("unexpected error for hash mismatch: " + error);
  }
}

void TestChunkRestoreInvalidOrdering() {
  std::string error;
  grovedb::ChunkRestorer restorer;
  if (!restorer.Init(std::vector<uint8_t>(32, 0), nullptr, &error)) {
    Fail("restorer init failed: " + error);
  }
  std::vector<uint8_t> invalid = {0x00, 0x00, 0x00, 0x00};
  std::vector<std::vector<uint8_t>> next_chunks;
  if (restorer.ProcessChunkProof(invalid, &next_chunks, &error)) {
    Fail("expected invalid ordering to fail");
  }
  if (error.find("invalid multi chunk ordering") == std::string::npos) {
    Fail("unexpected error for ordering: " + error);
  }
}

void TestChunkRestoreInvalidOp() {
  std::string error;
  grovedb::MerkTree tree;
  if (!tree.Insert({'a'}, {'1'}, &error)) {
    Fail("insert failed: " + error);
  }
  std::vector<uint8_t> root_hash;
  if (!tree.ComputeRootHash(nullptr, &root_hash, &error)) {
    Fail("compute root hash failed: " + error);
  }
  std::vector<uint8_t> chunk;
  if (!tree.GenerateChunkProof(2, nullptr, false, &chunk, &error)) {
    Fail("generate chunk failed: " + error);
  }
  size_t ops_offset = 0;
  if (!FindFirstChunkOpsOffset(chunk, &ops_offset)) {
    Fail("failed to find ops offset");
  }
  chunk[ops_offset] = 0x03;
  grovedb::ChunkRestorer restorer;
  if (!restorer.Init(root_hash, nullptr, &error)) {
    Fail("restorer init failed: " + error);
  }
  std::vector<std::vector<uint8_t>> next_chunks;
  if (restorer.ProcessChunkProof(chunk, &next_chunks, &error)) {
    Fail("expected invalid op to fail");
  }
  if (error.find("unsupported proof opcode in chunk") == std::string::npos) {
    Fail("unexpected error for invalid op: " + error);
  }
}

void TestChunkRestoreStorage() {
  std::string error;
  grovedb::MerkTree tree;
  for (char c = 'a'; c <= 'm'; ++c) {
    if (!tree.Insert({static_cast<uint8_t>(c)}, {static_cast<uint8_t>(c)}, &error)) {
      Fail("insert failed: " + error);
    }
  }
  std::vector<uint8_t> root_hash;
  if (!tree.ComputeRootHash(nullptr, &root_hash, &error)) {
    Fail("compute root hash failed: " + error);
  }

  auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
  std::string dir = MakeTempDir("chunk_restore_test_storage_" + std::to_string(now));
  grovedb::RocksDbWrapper storage;
  if (!storage.Open(dir, &error)) {
    Fail("open rocksdb failed: " + error);
  }

  grovedb::ChunkRestorer restorer;
  if (!restorer.Init(root_hash, nullptr, &error)) {
    Fail("restorer init failed: " + error);
  }

  std::vector<uint8_t> root_chunk;
  if (!tree.GenerateChunkProof(2, nullptr, false, &root_chunk, &error)) {
    Fail("generate root chunk failed: " + error);
  }

  std::vector<std::vector<uint8_t>> next_chunks;
  if (!restorer.ProcessChunkProofAndStore(root_chunk, &storage, {}, &next_chunks, &error)) {
    Fail("process root chunk failed: " + error);
  }

  while (!next_chunks.empty()) {
    std::vector<std::vector<uint8_t>> pending;
    for (const auto& chunk_id : next_chunks) {
      std::vector<bool> instructions;
      if (!grovedb::BytesToTraversalInstruction(chunk_id, &instructions, &error)) {
        Fail("decode chunk id failed: " + error);
      }
      std::vector<uint8_t> chunk;
      if (!tree.GenerateChunkProofAt(instructions, 2, nullptr, false, &chunk, &error)) {
        Fail("generate chunk failed: " + error);
      }
      std::vector<std::vector<uint8_t>> more;
      if (!restorer.ProcessChunkProofAndStore(chunk, &storage, {}, &more, &error)) {
        Fail("process chunk failed: " + error);
      }
      pending.insert(pending.end(), more.begin(), more.end());
    }
    next_chunks.swap(pending);
  }

  std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> meta_before;
  if (!storage.ScanPrefix(grovedb::ColumnFamilyKind::kMeta, {}, &meta_before, &error)) {
    Fail("scan meta before finalize failed: " + error);
  }
  if (meta_before.empty()) {
    Fail("expected parent link metadata before finalize");
  }

  grovedb::MerkTree restored_tree;
  if (!restorer.FinalizeToMerkTree(&storage, {}, &restored_tree, &error)) {
    Fail("finalize restore failed: " + error);
  }
  std::vector<uint8_t> restored_root;
  if (!restored_tree.ComputeRootHash(nullptr, &restored_root, &error)) {
    Fail("compute restored root failed: " + error);
  }
  if (restored_root != root_hash) {
    Fail("restored root hash mismatch");
  }
  if (restored_tree.Height() != tree.Height()) {
    Fail("restored tree height mismatch");
  }

  std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> encoded_entries;
  if (!storage.ScanPrefix(grovedb::ColumnFamilyKind::kDefault, {}, &encoded_entries, &error)) {
    Fail("scan storage failed: " + error);
  }
  if (encoded_entries.empty()) {
    Fail("expected encoded nodes in storage");
  }
  bool checked_meta = false;
  for (const auto& entry : encoded_entries) {
    grovedb::TreeNodeInner decoded;
    if (!grovedb::DecodeTreeNodeInner(entry.second, &decoded, &error)) {
      Fail("decode storage node failed: " + error);
    }
    std::vector<uint8_t> original_value;
    if (!tree.Get(entry.first, &original_value)) {
      Fail("missing original value for storage node");
    }
    if (decoded.kv.value != original_value) {
      Fail("storage node value mismatch");
    }
    if (decoded.has_left || decoded.has_right) {
      grovedb::MerkTree::NodeMeta meta;
      if (!restored_tree.GetNodeMeta(entry.first, &meta, &error)) {
        Fail("get node meta failed: " + error);
      }
      if (decoded.has_left != meta.has_left || decoded.has_right != meta.has_right) {
        Fail("node meta presence mismatch");
      }
      if (decoded.has_left && decoded.left.hash != meta.left_hash) {
        Fail("node meta left hash mismatch");
      }
      if (decoded.has_right && decoded.right.hash != meta.right_hash) {
        Fail("node meta right hash mismatch");
      }
      checked_meta = true;
      break;
    }
  }
  if (!checked_meta) {
    Fail("no storage node with children to verify metadata");
  }

  std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> meta_after;
  if (!storage.ScanPrefix(grovedb::ColumnFamilyKind::kMeta, {}, &meta_after, &error)) {
    Fail("scan meta after finalize failed: " + error);
  }
  if (!meta_after.empty()) {
    Fail("expected no parent link metadata after finalize");
  }

  std::filesystem::remove_all(dir);
}

void TestEncodedTreeLoadDetectsCorruption() {
  std::string error;
  grovedb::MerkTree tree;
  for (char c = 'a'; c <= 'm'; ++c) {
    if (!tree.Insert({static_cast<uint8_t>(c)}, {static_cast<uint8_t>(c)}, &error)) {
      Fail("insert failed: " + error);
    }
  }
  std::vector<uint8_t> root_hash;
  if (!tree.ComputeRootHash(nullptr, &root_hash, &error)) {
    Fail("compute root hash failed: " + error);
  }

  auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
  std::string dir = MakeTempDir("chunk_restore_test_corrupt_" + std::to_string(now));
  grovedb::RocksDbWrapper storage;
  if (!storage.Open(dir, &error)) {
    Fail("open rocksdb failed: " + error);
  }

  grovedb::ChunkRestorer restorer;
  if (!restorer.Init(root_hash, nullptr, &error)) {
    Fail("restorer init failed: " + error);
  }

  std::vector<uint8_t> root_chunk;
  if (!tree.GenerateChunkProof(2, nullptr, false, &root_chunk, &error)) {
    Fail("generate root chunk failed: " + error);
  }

  std::vector<std::vector<uint8_t>> next_chunks;
  if (!restorer.ProcessChunkProofAndStore(root_chunk, &storage, {}, &next_chunks, &error)) {
    Fail("process root chunk failed: " + error);
  }

  while (!next_chunks.empty()) {
    std::vector<std::vector<uint8_t>> pending;
    for (const auto& chunk_id : next_chunks) {
      std::vector<bool> instructions;
      if (!grovedb::BytesToTraversalInstruction(chunk_id, &instructions, &error)) {
        Fail("decode chunk id failed: " + error);
      }
      std::vector<uint8_t> chunk;
      if (!tree.GenerateChunkProofAt(instructions, 2, nullptr, false, &chunk, &error)) {
        Fail("generate chunk failed: " + error);
      }
      std::vector<std::vector<uint8_t>> more;
      if (!restorer.ProcessChunkProofAndStore(chunk, &storage, {}, &more, &error)) {
        Fail("process chunk failed: " + error);
      }
      pending.insert(pending.end(), more.begin(), more.end());
    }
    next_chunks.swap(pending);
  }

  std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> encoded_entries;
  if (!storage.ScanPrefix(grovedb::ColumnFamilyKind::kDefault, {}, &encoded_entries, &error)) {
    Fail("scan storage failed: " + error);
  }
  if (encoded_entries.empty()) {
    Fail("expected encoded entries");
  }
  grovedb::TreeNodeInner decoded;
  if (!grovedb::DecodeTreeNodeInner(encoded_entries[0].second, &decoded, &error)) {
    Fail("decode storage node failed: " + error);
  }
  if (!decoded.kv.value_hash.empty()) {
    decoded.kv.value_hash[0] ^= 0xFF;
  }
  std::vector<uint8_t> corrupted;
  if (!grovedb::EncodeTreeNodeInner(decoded, &corrupted, &error)) {
    Fail("encode corrupted node failed: " + error);
  }
  if (!storage.Put(grovedb::ColumnFamilyKind::kDefault,
                   {},
                  encoded_entries[0].first,
                   corrupted,
                   &error)) {
    Fail("write corrupted node failed: " + error);
  }

  grovedb::MerkTree restored;
  if (restorer.FinalizeToMerkTree(&storage, {}, &restored, &error)) {
    std::vector<uint8_t> restored_root;
    if (!restored.ComputeRootHash(nullptr, &restored_root, &error)) {
      Fail("compute restored root failed: " + error);
    }
    if (restored_root == root_hash) {
      Fail("expected finalize to detect corrupted encoded node");
    }
  }

  std::filesystem::remove_all(dir);
}

void TestEncodedTreeLoadDetectsChildHashCorruption() {
  std::string error;
  grovedb::MerkTree tree;
  for (char c = 'a'; c <= 'm'; ++c) {
    if (!tree.Insert({static_cast<uint8_t>(c)}, {static_cast<uint8_t>(c)}, &error)) {
      Fail("insert failed: " + error);
    }
  }
  std::vector<uint8_t> root_hash;
  if (!tree.ComputeRootHash(nullptr, &root_hash, &error)) {
    Fail("compute root hash failed: " + error);
  }

  auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
  std::string dir = MakeTempDir("chunk_restore_test_corrupt_child_" + std::to_string(now));
  grovedb::RocksDbWrapper storage;
  if (!storage.Open(dir, &error)) {
    Fail("open rocksdb failed: " + error);
  }

  grovedb::ChunkRestorer restorer;
  if (!restorer.Init(root_hash, nullptr, &error)) {
    Fail("restorer init failed: " + error);
  }

  std::vector<uint8_t> root_chunk;
  if (!tree.GenerateChunkProof(2, nullptr, false, &root_chunk, &error)) {
    Fail("generate root chunk failed: " + error);
  }

  std::vector<std::vector<uint8_t>> next_chunks;
  if (!restorer.ProcessChunkProofAndStore(root_chunk, &storage, {}, &next_chunks, &error)) {
    Fail("process root chunk failed: " + error);
  }

  while (!next_chunks.empty()) {
    std::vector<std::vector<uint8_t>> pending;
    for (const auto& chunk_id : next_chunks) {
      std::vector<bool> instructions;
      if (!grovedb::BytesToTraversalInstruction(chunk_id, &instructions, &error)) {
        Fail("decode chunk id failed: " + error);
      }
      std::vector<uint8_t> chunk;
      if (!tree.GenerateChunkProofAt(instructions, 2, nullptr, false, &chunk, &error)) {
        Fail("generate chunk failed: " + error);
      }
      std::vector<std::vector<uint8_t>> more;
      if (!restorer.ProcessChunkProofAndStore(chunk, &storage, {}, &more, &error)) {
        Fail("process chunk failed: " + error);
      }
      pending.insert(pending.end(), more.begin(), more.end());
    }
    next_chunks.swap(pending);
  }

  std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> encoded_entries;
  if (!storage.ScanPrefix(grovedb::ColumnFamilyKind::kDefault, {}, &encoded_entries, &error)) {
    Fail("scan storage failed: " + error);
  }
  if (encoded_entries.empty()) {
    Fail("expected encoded entries");
  }

  bool corrupted = false;
  for (const auto& entry : encoded_entries) {
    grovedb::TreeNodeInner decoded;
    if (!grovedb::DecodeTreeNodeInner(entry.second, &decoded, &error)) {
      Fail("decode storage node failed: " + error);
    }
    if (decoded.has_left && !decoded.left.hash.empty()) {
      decoded.left.hash[0] ^= 0xFF;
      std::vector<uint8_t> out;
      if (!grovedb::EncodeTreeNodeInner(decoded, &out, &error)) {
        Fail("encode corrupted node failed: " + error);
      }
      if (!storage.Put(grovedb::ColumnFamilyKind::kDefault, {}, entry.first, out, &error)) {
        Fail("write corrupted node failed: " + error);
      }
      corrupted = true;
      break;
    }
    if (decoded.has_right && !decoded.right.hash.empty()) {
      decoded.right.hash[0] ^= 0xFF;
      std::vector<uint8_t> out;
      if (!grovedb::EncodeTreeNodeInner(decoded, &out, &error)) {
        Fail("encode corrupted node failed: " + error);
      }
      if (!storage.Put(grovedb::ColumnFamilyKind::kDefault, {}, entry.first, out, &error)) {
        Fail("write corrupted node failed: " + error);
      }
      corrupted = true;
      break;
    }
  }

  if (!corrupted) {
    Fail("no storage node with child link to corrupt");
  }

  grovedb::MerkTree restored;
  if (restorer.FinalizeToMerkTree(&storage, {}, &restored, &error)) {
    std::vector<uint8_t> restored_root;
    if (!restored.ComputeRootHash(nullptr, &restored_root, &error)) {
      Fail("compute restored root failed: " + error);
    }
    if (restored_root == root_hash) {
      Fail("expected finalize to detect corrupted child hash");
    }
  }

  std::filesystem::remove_all(dir);
}

void TestEncodedTreeLoadUsesLinkMetadata() {
  std::string error;
  grovedb::MerkTree tree;
  for (char c = 'a'; c <= 'm'; ++c) {
    if (!tree.Insert({static_cast<uint8_t>(c)}, {static_cast<uint8_t>(c)}, &error)) {
      Fail("insert failed: " + error);
    }
  }
  std::vector<uint8_t> root_hash;
  if (!tree.ComputeRootHash(nullptr, &root_hash, &error)) {
    Fail("compute root hash failed: " + error);
  }

  auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
  std::string dir = MakeTempDir("chunk_restore_test_link_meta_" + std::to_string(now));
  grovedb::RocksDbWrapper storage;
  if (!storage.Open(dir, &error)) {
    Fail("open rocksdb failed: " + error);
  }

  grovedb::ChunkRestorer restorer;
  if (!restorer.Init(root_hash, nullptr, &error)) {
    Fail("restorer init failed: " + error);
  }

  std::vector<uint8_t> root_chunk;
  if (!tree.GenerateChunkProof(2, nullptr, false, &root_chunk, &error)) {
    Fail("generate root chunk failed: " + error);
  }

  std::vector<std::vector<uint8_t>> next_chunks;
  if (!restorer.ProcessChunkProofAndStore(root_chunk, &storage, {}, &next_chunks, &error)) {
    Fail("process root chunk failed: " + error);
  }

  while (!next_chunks.empty()) {
    std::vector<std::vector<uint8_t>> pending;
    for (const auto& chunk_id : next_chunks) {
      std::vector<bool> instructions;
      if (!grovedb::BytesToTraversalInstruction(chunk_id, &instructions, &error)) {
        Fail("decode chunk id failed: " + error);
      }
      std::vector<uint8_t> chunk;
      if (!tree.GenerateChunkProofAt(instructions, 2, nullptr, false, &chunk, &error)) {
        Fail("generate chunk failed: " + error);
      }
      std::vector<std::vector<uint8_t>> more;
      if (!restorer.ProcessChunkProofAndStore(chunk, &storage, {}, &more, &error)) {
        Fail("process chunk failed: " + error);
      }
      pending.insert(pending.end(), more.begin(), more.end());
    }
    next_chunks.swap(pending);
  }

  std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> encoded_entries;
  if (!storage.ScanPrefix(grovedb::ColumnFamilyKind::kDefault, {}, &encoded_entries, &error)) {
    Fail("scan storage failed: " + error);
  }
  if (encoded_entries.empty()) {
    Fail("expected encoded entries");
  }

  bool modified = false;
  for (const auto& entry : encoded_entries) {
    grovedb::TreeNodeInner decoded;
    if (!grovedb::DecodeTreeNodeInner(entry.second, &decoded, &error)) {
      Fail("decode storage node failed: " + error);
    }
    if (decoded.has_left && !decoded.left.key.empty()) {
      decoded.left.key.clear();
    } else if (decoded.has_right && !decoded.right.key.empty()) {
      decoded.right.key.clear();
    } else {
      continue;
    }
    std::vector<uint8_t> out;
    if (!grovedb::EncodeTreeNodeInner(decoded, &out, &error)) {
      Fail("encode updated node failed: " + error);
    }
    if (!storage.Put(grovedb::ColumnFamilyKind::kDefault, {}, entry.first, out, &error)) {
      Fail("write updated node failed: " + error);
    }
    modified = true;
    break;
  }

  if (!modified) {
    Fail("no storage node with child key to clear");
  }

  grovedb::MerkTree restored;
  if (!restorer.FinalizeToMerkTree(&storage, {}, &restored, &error)) {
    Fail("finalize restore failed: " + error);
  }
  std::vector<uint8_t> restored_root;
  if (!restored.ComputeRootHash(nullptr, &restored_root, &error)) {
    Fail("compute restored root failed: " + error);
  }
  if (restored_root != root_hash) {
    Fail("restored root hash mismatch after clearing child link key");
  }

  std::filesystem::remove_all(dir);
}

int main() {
  TestChunkRestoreHappyPath();
  TestChunkRestoreBadHash();
  TestChunkRestoreInvalidOrdering();
  TestChunkRestoreInvalidOp();
  TestChunkRestoreStorage();
  TestEncodedTreeLoadDetectsCorruption();
  TestEncodedTreeLoadDetectsChildHashCorruption();
  TestEncodedTreeLoadUsesLinkMetadata();

  return 0;
}
