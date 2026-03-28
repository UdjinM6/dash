#include "chunk.h"
#include "chunk_producer.h"
#include "merk_storage.h"
#include "proof.h"
#include "test_utils.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

using test_utils::Fail;
using test_utils::MakeTempDir;
using test_utils::ReadFile;

namespace {
size_t ReadChunkCount(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) {
    Fail("failed to open file: " + path.string());
  }
  size_t count = 0;
  input >> count;
  if (count == 0) {
    Fail("invalid chunk count");
  }
  return count;
}

size_t ReadSizeT(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) {
    Fail("failed to open file: " + path.string());
  }
  size_t value = 0;
  input >> value;
  return value;
}

bool HasRejectedMutation(const std::vector<uint8_t>& proof,
                         const std::vector<uint8_t>& expected_root) {
  for (size_t i = 0; i < proof.size(); ++i) {
    std::vector<uint8_t> mutated = proof;
    mutated[i] ^= 0x01;
    std::vector<uint8_t> root_hash;
    std::string error;
    if (!grovedb::ExecuteChunkProof(mutated, &root_hash, &error)) {
      return true;
    }
    if (root_hash != expected_root) {
      return true;
    }
  }
  return false;
}

void AssertTruncatedRejected(const std::vector<uint8_t>& proof, const std::string& label) {
  if (proof.size() < 2) {
    Fail(label + " chunk proof unexpectedly too short");
  }
  std::vector<uint8_t> truncated = proof;
  truncated.pop_back();
  std::vector<uint8_t> root_hash;
  std::string error;
  if (grovedb::ExecuteChunkProof(truncated, &root_hash, &error)) {
    Fail("truncated " + label + " chunk proof unexpectedly verified");
  }
}

std::vector<uint8_t> ExecuteChunkOrFail(const std::vector<uint8_t>& proof,
                                        const std::string& label) {
  std::vector<uint8_t> root_hash;
  std::string error;
  if (!grovedb::ExecuteChunkProof(proof, &root_hash, &error)) {
    Fail("execute " + label + " chunk proof failed: " + error);
  }
  if (root_hash.empty()) {
    Fail(label + " chunk proof produced empty root hash");
  }
  return root_hash;
}
}  // namespace

int main() {
  const char* run = std::getenv("GROVEDB_RUN_RUST_PARITY");
  if (run == nullptr) {
    return 0;
  }
  std::string dir = MakeTempDir("rust_merk_chunk_parity");

  std::string cmd =
      test_utils::RustToolsCargoRunPrefix() + "rust_merk_chunk_proof_writer \"" +
      dir + "\"";
  if (std::system(cmd.c_str()) != 0) {
    Fail("failed to run rust merk chunk proof writer");
  }

  grovedb::RocksDbWrapper storage;
  std::string error;
  if (!storage.Open(dir, &error)) {
    Fail("open storage failed: " + error);
  }

  grovedb::MerkTree tree;
  if (!grovedb::MerkStorage::LoadTree(&storage, {{'r', 'o', 'o', 't'}}, &tree, &error)) {
    Fail("load tree failed: " + error);
  }

  grovedb::ChunkProducer producer;
  if (!grovedb::ChunkProducer::Create(tree, false, &producer, &error)) {
    Fail("create chunk producer failed: " + error);
  }

  grovedb::SubtreeChunk root_chunk;
  if (!producer.ChunkWithIndex(1, &root_chunk, &error)) {
    Fail("generate root chunk proof failed: " + error);
  }
  const size_t chunk_count = ReadChunkCount(std::filesystem::path(dir) / "chunk_count.txt");
  if (chunk_count < 2) {
    Fail("unexpectedly small chunk count");
  }
  if (chunk_count != producer.Len()) {
    Fail("chunk count mismatch between rust and c++ producers");
  }

  std::vector<uint8_t> expected_root_hash;
  if (!tree.ComputeRootHash(grovedb::MerkTree::ValueHashFn(), &expected_root_hash, &error)) {
    Fail("compute tree root hash failed: " + error);
  }
  {
    std::vector<uint8_t> out_root;
    if (grovedb::ExecuteChunkProof({}, &out_root, &error)) {
      Fail("empty chunk proof should not execute");
    }
    if (grovedb::ExecuteChunkProof({0xff}, &out_root, &error)) {
      Fail("malformed chunk proof should not execute");
    }
  }

  std::vector<uint8_t> root_chunk_hash = ExecuteChunkOrFail(root_chunk.chunk, "root");
  if (root_chunk_hash != expected_root_hash) {
    Fail("root chunk proof root mismatch");
  }
  if (grovedb::ExecuteChunkProof(root_chunk.chunk, nullptr, &error)) {
    Fail("execute chunk proof should fail with null root output");
  }

  for (size_t i = 1; i <= chunk_count; ++i) {
    grovedb::SubtreeChunk chunk;
    if (!producer.ChunkWithIndex(i, &chunk, &error)) {
      Fail("generate chunk proof failed: " + error);
    }
    const std::string file_name = "chunk_" + std::to_string(i) + ".bin";
    const std::vector<uint8_t> rust_chunk = ReadFile(std::filesystem::path(dir) / file_name);
    if (chunk.chunk != rust_chunk) {
      Fail("chunk proof mismatch at index " + std::to_string(i));
    }
    std::cout << "MERK_CHUNK_FIXTURE " << file_name << " PASS bytes=" << chunk.chunk.size() << "\n";

    const std::string label = "chunk_" + std::to_string(i);
    const std::vector<uint8_t> chunk_hash = ExecuteChunkOrFail(chunk.chunk, label);
    if (i == 1 && chunk_hash != expected_root_hash) {
      Fail("root chunk hash mismatch at index 1");
    }
    AssertTruncatedRejected(chunk.chunk, label);
    if (!HasRejectedMutation(chunk.chunk, chunk_hash)) {
      Fail("chunk mutation check failed at index " + std::to_string(i));
    }
  }

  {
    grovedb::SubtreeChunk invalid_chunk;
    if (producer.ChunkWithIndex(0, &invalid_chunk, &error)) {
      Fail("chunk index 0 should fail");
    }
    if (producer.ChunkWithIndex(chunk_count + 1, &invalid_chunk, &error)) {
      Fail("chunk index len+1 should fail");
    }
  }

  const std::vector<uint8_t> expected_multi_unlimited =
      ReadFile(std::filesystem::path(dir) / "multichunk_2_unlimited.bin");
  const std::vector<uint8_t> expected_multi_unlimited_next =
      ReadFile(std::filesystem::path(dir) / "multichunk_2_unlimited_next.bin");
  {
    grovedb::MultiChunk multi;
    if (!producer.MultiChunkWithLimitAndIndex(2, std::nullopt, &multi, &error)) {
      Fail("multi chunk unlimited failed: " + error);
    }
    if (multi.chunk != expected_multi_unlimited) {
      Fail("multi chunk unlimited bytes mismatch");
    }
    std::cout << "MERK_CHUNK_FIXTURE multichunk_2_unlimited.bin PASS bytes=" << multi.chunk.size()
              << "\n";
    if (expected_multi_unlimited_next.empty()) {
      if (multi.next_index.has_value()) {
        Fail("multi chunk unlimited next index expected none");
      }
    } else {
      if (!multi.next_index.has_value() || *multi.next_index != expected_multi_unlimited_next) {
        Fail("multi chunk unlimited next index mismatch");
      }
    }
  }

  const std::vector<uint8_t> expected_multi_limited =
      ReadFile(std::filesystem::path(dir) / "multichunk_2_limited.bin");
  const std::vector<uint8_t> expected_multi_limited_next =
      ReadFile(std::filesystem::path(dir) / "multichunk_2_limited_next.bin");
  const size_t expected_multi_limited_remaining =
      ReadSizeT(std::filesystem::path(dir) / "multichunk_2_limited_remaining.txt");
  {
    grovedb::SubtreeChunk chunk2;
    grovedb::SubtreeChunk chunk3;
    if (!producer.ChunkWithIndex(2, &chunk2, &error) || !producer.ChunkWithIndex(3, &chunk3, &error)) {
      Fail("failed to prepare chunk lengths for limited multichunk");
    }
    const size_t limit = chunk2.chunk.size() + chunk3.chunk.size() + 5;
    grovedb::MultiChunk multi;
    if (!producer.MultiChunkWithLimitAndIndex(2, limit, &multi, &error)) {
      Fail("multi chunk limited failed: " + error);
    }
    if (multi.chunk != expected_multi_limited) {
      Fail("multi chunk limited bytes mismatch");
    }
    std::cout << "MERK_CHUNK_FIXTURE multichunk_2_limited.bin PASS bytes=" << multi.chunk.size()
              << "\n";
    if (multi.remaining_limit != expected_multi_limited_remaining) {
      const std::string actual = multi.remaining_limit.has_value()
                                     ? std::to_string(*multi.remaining_limit)
                                     : std::string("none");
      Fail("multi chunk limited remaining limit mismatch expected=" +
           std::to_string(expected_multi_limited_remaining) +
           " actual=" + actual);
    }
    if (expected_multi_limited_next.empty()) {
      if (multi.next_index.has_value()) {
        Fail("multi chunk limited next index expected none");
      }
    } else {
      if (!multi.next_index.has_value() || *multi.next_index != expected_multi_limited_next) {
        Fail("multi chunk limited next index mismatch");
      }
    }
  }

  {
    grovedb::MultiChunk multi;
    if (producer.MultiChunkWithLimitAndIndex(2, 1, &multi, &error)) {
      Fail("limit-too-small should fail");
    }
  }

  {
    std::vector<bool> instructions;
    if (!grovedb::GenerateTraversalInstruction(tree.Height(), 2, &instructions, &error)) {
      Fail("generate traversal instruction failed: " + error);
    }
    const std::vector<uint8_t> chunk_id = grovedb::TraversalInstructionToBytes(instructions);
    grovedb::MultiChunk by_index;
    grovedb::MultiChunk by_id;
    if (!producer.MultiChunkWithLimitAndIndex(2, std::nullopt, &by_index, &error)) {
      Fail("multi chunk by index failed: " + error);
    }
    if (!producer.MultiChunkWithLimit(chunk_id, std::nullopt, &by_id, &error)) {
      Fail("multi chunk by id failed: " + error);
    }
    if (by_index.chunk != by_id.chunk || by_index.next_index != by_id.next_index ||
        by_index.remaining_limit != by_id.remaining_limit) {
      Fail("multi chunk by-id and by-index mismatch");
    }
  }

  std::cout << "MERK_CHUNK_FIXTURE_SUMMARY checked=" << (chunk_count + 2) << " status=PASS\n";

  std::filesystem::remove_all(dir);
  return 0;
}
