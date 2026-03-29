#include "merk_storage.h"
#include "proof.h"
#include "test_utils.h"

#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <algorithm>
#include <vector>

using test_utils::Fail;
using test_utils::MakeTempDir;
using test_utils::ReadFile;

namespace {
std::string HexPrefix(const std::vector<uint8_t>& bytes, size_t max_len = 16) {
  static const char kHex[] = "0123456789abcdef";
  size_t limit = std::min(bytes.size(), max_len);
  std::string out;
  out.reserve(limit * 2);
  for (size_t i = 0; i < limit; ++i) {
    uint8_t b = bytes[i];
    out.push_back(kHex[(b >> 4) & 0x0f]);
    out.push_back(kHex[b & 0x0f]);
  }
  return out;
}

bool HasRejectedMutation(const std::vector<uint8_t>& proof,
                         const std::vector<uint8_t>& expected_root,
                         std::string* error) {
  for (size_t i = 0; i < proof.size(); ++i) {
    std::vector<uint8_t> mutated = proof;
    mutated[i] ^= 0x01;
    std::vector<uint8_t> mutated_root;
    std::string local_error;
    if (!grovedb::ExecuteMerkProof(mutated, &mutated_root, &local_error)) {
      return true;
    }
    if (mutated_root != expected_root) {
      return true;
    }
  }
  if (error) {
    *error = "no single-byte mutation was rejected";
  }
  return false;
}

void AssertTruncatedRejected(const std::vector<uint8_t>& proof,
                             std::vector<uint8_t>* executed_root,
                             std::string* error,
                             const std::string& label) {
  if (proof.size() < 2) {
    Fail(label + " proof unexpectedly too short");
  }
  std::vector<uint8_t> truncated = proof;
  truncated.pop_back();
  std::string local_error;
  if (grovedb::ExecuteMerkProof(truncated, executed_root, &local_error)) {
    Fail("truncated " + label + " proof unexpectedly verified");
  }
  if (error != nullptr && error->empty()) {
    *error = local_error;
  }
}

void AssertProofStats(const std::vector<uint8_t>& proof, const std::string& label) {
  std::string error;
  uint64_t hash_calls = 0;
  if (!grovedb::CountMerkProofHashCalls(proof, &hash_calls, &error)) {
    Fail("count hash calls failed for " + label + ": " + error);
  }
  if (hash_calls == 0) {
    Fail("expected non-zero hash calls for " + label);
  }

  uint64_t hash_nodes = 0;
  if (!grovedb::CountMerkProofHashNodes(proof, &hash_nodes, &error)) {
    Fail("count hash nodes failed for " + label + ": " + error);
  }

  bool has_ref = false;
  if (!grovedb::MerkProofHasReferenceNodes(proof, &has_ref, &error)) {
    Fail("reference-node check failed for " + label + ": " + error);
  }
  (void)has_ref;
}

bool ContainsKey(const std::vector<std::vector<uint8_t>>& keys,
                 const std::vector<uint8_t>& key) {
  return std::find(keys.begin(), keys.end(), key) != keys.end();
}

void AppendU16BE(std::vector<uint8_t>* out, uint16_t value) {
  out->push_back(static_cast<uint8_t>((value >> 8) & 0xff));
  out->push_back(static_cast<uint8_t>(value & 0xff));
}

void AppendU64BE(std::vector<uint8_t>* out, uint64_t value) {
  out->push_back(static_cast<uint8_t>((value >> 56) & 0xff));
  out->push_back(static_cast<uint8_t>((value >> 48) & 0xff));
  out->push_back(static_cast<uint8_t>((value >> 40) & 0xff));
  out->push_back(static_cast<uint8_t>((value >> 32) & 0xff));
  out->push_back(static_cast<uint8_t>((value >> 24) & 0xff));
  out->push_back(static_cast<uint8_t>((value >> 16) & 0xff));
  out->push_back(static_cast<uint8_t>((value >> 8) & 0xff));
  out->push_back(static_cast<uint8_t>(value & 0xff));
}

std::vector<uint8_t> BuildRustMerkOpFixture(uint8_t opcode) {
  const std::vector<uint8_t> key = {'k'};
  const std::vector<uint8_t> value = {'v', '1'};
  const std::vector<uint8_t> hash(32, 0x42);
  const uint64_t count = 7;

  std::vector<uint8_t> proof;
  proof.push_back(opcode);
  switch (opcode) {
    case 0x01:
    case 0x02:
    case 0x08:
    case 0x09:
      proof.insert(proof.end(), hash.begin(), hash.end());
      return proof;
    case 0x03:
    case 0x04:
    case 0x06:
    case 0x07:
    case 0x0a:
    case 0x0b:
    case 0x0d:
    case 0x0e:
    case 0x14:
    case 0x16:
    case 0x18:
    case 0x19:
      proof.push_back(static_cast<uint8_t>(key.size()));
      proof.insert(proof.end(), key.begin(), key.end());
      AppendU16BE(&proof, static_cast<uint16_t>(value.size()));
      proof.insert(proof.end(), value.begin(), value.end());
      if (opcode == 0x04 || opcode == 0x06 || opcode == 0x07 ||
          opcode == 0x0b || opcode == 0x0d || opcode == 0x0e ||
          opcode == 0x18 || opcode == 0x19) {
        proof.insert(proof.end(), hash.begin(), hash.end());
      }
      if (opcode == 0x07 || opcode == 0x0e) {
        proof.push_back(0x00);  // BasicMerkNode tree feature tag.
      }
      if (opcode == 0x14 || opcode == 0x16 ||
          opcode == 0x18 || opcode == 0x19) {
        AppendU64BE(&proof, count);
      }
      return proof;
    case 0x05:
    case 0x0c:
    case 0x1a:
    case 0x1b:
      proof.push_back(static_cast<uint8_t>(key.size()));
      proof.insert(proof.end(), key.begin(), key.end());
      proof.insert(proof.end(), hash.begin(), hash.end());
      if (opcode == 0x1a || opcode == 0x1b) {
        AppendU64BE(&proof, count);
      }
      return proof;
    case 0x15:
    case 0x17:
      proof.insert(proof.end(), hash.begin(), hash.end());
      AppendU64BE(&proof, count);
      return proof;
    case 0x10:
    case 0x11:
    case 0x12:
    case 0x13:
      return proof;
    default:
      Fail("unhandled opcode fixture: " + std::to_string(opcode));
  }
}

void AssertRustMerkOpcodeCoverage() {
  struct VariantCase {
    uint8_t opcode;
    bool expect_key;
  };
  const std::vector<VariantCase> variants = {
      {0x01, false}, {0x02, false}, {0x03, true},  {0x04, true},  {0x05, true},
      {0x06, true},  {0x07, true},  {0x08, false}, {0x09, false}, {0x0a, true},
      {0x0b, true},  {0x0c, true},  {0x0d, true},  {0x0e, true},  {0x10, false},
      {0x11, false}, {0x12, false}, {0x13, false}, {0x14, true},  {0x15, false},
      {0x16, true},  {0x17, false}, {0x18, true},  {0x19, true},  {0x1a, true},
      {0x1b, true},
  };
  for (const auto& variant : variants) {
    std::vector<uint8_t> proof = BuildRustMerkOpFixture(variant.opcode);
    std::vector<std::vector<uint8_t>> keys;
    std::string error;
    if (!grovedb::CollectProofKeys(proof, &keys, &error)) {
      Fail("failed decoding Rust proof opcode " + std::to_string(variant.opcode) + ": " + error);
    }
    if (variant.expect_key && !ContainsKey(keys, {'k'})) {
      Fail("opcode expected key but none decoded: " + std::to_string(variant.opcode));
    }
    if (!variant.expect_key && !keys.empty()) {
      Fail("opcode unexpectedly decoded a key: " + std::to_string(variant.opcode));
    }
  }
}

size_t FindKeyIndex(const std::vector<std::vector<uint8_t>>& keys,
                    const std::vector<uint8_t>& key) {
  for (size_t i = 0; i < keys.size(); ++i) {
    if (keys[i] == key) {
      return i;
    }
  }
  return static_cast<size_t>(-1);
}

void AssertCollectedKeys(const std::vector<uint8_t>& proof,
                         const std::string& label,
                         const std::vector<std::vector<uint8_t>>& expected_keys) {
  std::string error;
  std::vector<std::vector<uint8_t>> keys;
  if (!grovedb::CollectProofKeys(proof, &keys, &error)) {
    Fail("collect proof keys failed for " + label + ": " + error);
  }
  if (keys.empty()) {
    Fail("collect proof keys returned empty for " + label);
  }
  for (const auto& expected : expected_keys) {
    if (!ContainsKey(keys, expected)) {
      Fail("collect proof keys missing expected key for " + label);
    }
  }
  if (label == "range" && expected_keys.size() >= 3) {
    size_t i1 = FindKeyIndex(keys, expected_keys[0]);
    size_t i2 = FindKeyIndex(keys, expected_keys[1]);
    size_t i3 = FindKeyIndex(keys, expected_keys[2]);
    if (i1 == static_cast<size_t>(-1) || i2 == static_cast<size_t>(-1) ||
        i3 == static_cast<size_t>(-1) || !(i1 < i2 && i2 < i3)) {
      Fail("collect proof keys order mismatch for range");
    }
  }
}

void AssertCollectedKvNodes(const std::vector<uint8_t>& proof,
                            const std::string& label,
                            bool expect_non_empty) {
  std::string error;
  std::vector<grovedb::ProofNode> nodes;
  if (!grovedb::CollectKvNodes(proof, &nodes, &error)) {
    Fail("collect kv nodes failed for " + label + ": " + error);
  }
  if (expect_non_empty && nodes.empty()) {
    Fail("collect kv nodes returned empty for " + label);
  }
  if ((label == "present" || label == "range") && !nodes.empty()) {
    bool saw_kv_like = false;
    bool saw_tree_feature = false;
    for (const auto& node : nodes) {
      if (node.type == grovedb::NodeType::kKv ||
          node.type == grovedb::NodeType::kKvHash ||
          node.type == grovedb::NodeType::kKvValueHash ||
          node.type == grovedb::NodeType::kKvRefValueHash ||
          node.type == grovedb::NodeType::kKvRefValueHashCount ||
          node.type == grovedb::NodeType::kKvDigest) {
        saw_kv_like = true;
      }
      if (node.has_tree_feature_type) {
        saw_tree_feature = true;
      }
    }
    if (!saw_kv_like) {
      Fail("collect kv nodes missing kv-like node type for " + label);
    }
    // Normal-tree fixtures should not emit tree-feature tags.
    if (saw_tree_feature) {
      Fail("collect kv nodes unexpectedly has tree feature type for " + label);
    }
  }
}

void AssertInvalidInputHandling(const std::vector<uint8_t>& proof) {
  std::string error;
  std::vector<uint8_t> root_hash;
  std::vector<uint8_t> out;
  if (grovedb::ExecuteMerkProof({}, &root_hash, &error)) {
    Fail("empty proof should not execute");
  }
  uint64_t hash_calls = 0;
  if (grovedb::CountMerkProofHashCalls({}, &hash_calls, &error)) {
    Fail("empty proof should not count hash calls");
  }
  uint64_t hash_nodes = 0;
  if (grovedb::CountMerkProofHashNodes({}, &hash_nodes, &error)) {
    Fail("empty proof should not count hash nodes");
  }
  bool has_ref = false;
  if (grovedb::MerkProofHasReferenceNodes({}, &has_ref, &error)) {
    Fail("empty proof should not decode reference nodes");
  }
  std::vector<uint8_t> malformed = {0xff};
  if (grovedb::ExecuteMerkProof(malformed, &root_hash, &error)) {
    Fail("malformed proof should not execute");
  }
  uint64_t malformed_calls = 0;
  if (grovedb::CountMerkProofHashCalls(malformed, &malformed_calls, &error)) {
    Fail("malformed proof should not count hash calls");
  }
  uint64_t malformed_nodes = 0;
  if (grovedb::CountMerkProofHashNodes(malformed, &malformed_nodes, &error)) {
    Fail("malformed proof should not count hash nodes");
  }
  bool malformed_has_ref = false;
  if (grovedb::MerkProofHasReferenceNodes(malformed, &malformed_has_ref, &error)) {
    Fail("malformed proof should not decode reference nodes");
  }
  if (grovedb::RewriteMerkProofForReference({},
                                            {'k', '2'},
                                            {'v', '2'},
                                            {'r', 'e', 's'},
                                            false,
                                            &out,
                                            &error)) {
    Fail("rewrite should fail on empty proof");
  }
  if (grovedb::RewriteMerkProofForReference(proof,
                                            {'k', '2'},
                                            {'v', '2'},
                                            {'r', 'e', 's'},
                                            false,
                                            nullptr,
                                            &error)) {
    Fail("rewrite should fail with null output");
  }
  if (grovedb::ExecuteMerkProof(proof, nullptr, &error)) {
    Fail("execute proof should fail with null root output");
  }
  if (grovedb::CountMerkProofHashCalls(proof, nullptr, &error)) {
    Fail("count hash calls should fail with null output");
  }
  if (grovedb::CountMerkProofHashNodes(proof, nullptr, &error)) {
    Fail("count hash nodes should fail with null output");
  }
  if (grovedb::MerkProofHasReferenceNodes(proof, nullptr, &error)) {
    Fail("reference-node query should fail with null output");
  }
  std::vector<grovedb::ProofNode> kv_nodes;
  if (grovedb::CollectKvNodes(proof, nullptr, &error)) {
    Fail("collect kv nodes should fail with null output");
  }
  if (grovedb::CollectKvNodes(malformed, &kv_nodes, &error)) {
    Fail("collect kv nodes should fail on malformed proof");
  }
  std::vector<std::vector<uint8_t>> keys;
  if (grovedb::CollectProofKeys(proof, nullptr, &error)) {
    Fail("collect proof keys should fail with null output");
  }
  if (grovedb::CollectProofKeys(malformed, &keys, &error)) {
    Fail("collect proof keys should fail on malformed proof");
  }

  grovedb::GroveLayerProof layer;
  if (grovedb::DecodeGroveDbProof(proof, nullptr, &error)) {
    Fail("decode grovedb proof should fail with null output");
  }
  if (grovedb::DecodeGroveDbProof({}, &layer, &error)) {
    Fail("decode grovedb proof should fail on empty input");
  }
  if (grovedb::DecodeGroveDbProof(malformed, &layer, &error)) {
    Fail("decode grovedb proof should fail on malformed input");
  }

  const grovedb::PathQuery path_query =
      grovedb::PathQuery::NewSingleKey({}, {'k'});
  std::vector<grovedb::VerifiedPathKeyElement> elements;
  if (grovedb::VerifyPathQueryProof({}, path_query, &root_hash, &elements, &error)) {
    Fail("verify path query proof should fail on empty proof");
  }
  if (grovedb::VerifyPathQueryProof(malformed, path_query, &root_hash, &elements, &error)) {
    Fail("verify path query proof should fail on malformed proof");
  }
  if (grovedb::VerifyPathQueryProof(malformed, path_query, nullptr, &elements, &error)) {
    Fail("verify path query proof should fail with null root output");
  }
  if (grovedb::VerifyPathQueryProof(malformed, path_query, &root_hash, nullptr, &error)) {
    Fail("verify path query proof should fail with null elements output");
  }
  grovedb::PathQuery empty_items_query = path_query;
  empty_items_query.query.query.items.clear();
  if (grovedb::VerifyPathQueryProof(malformed,
                                    empty_items_query,
                                    &root_hash,
                                    &elements,
                                    &error)) {
    Fail("verify path query proof should fail with empty query items");
  }
  grovedb::PathQuery offset_query = path_query;
  offset_query.query.offset = 1;
  if (grovedb::VerifyPathQueryProof(malformed, offset_query, &root_hash, &elements, &error)) {
    Fail("verify path query proof should fail with query offset");
  }
  grovedb::PathQuery zero_offset_query = path_query;
  zero_offset_query.query.offset = 0;
  if (grovedb::VerifyPathQueryProof(malformed,
                                    zero_offset_query,
                                    &root_hash,
                                    &elements,
                                    &error)) {
    Fail("verify path query proof should still fail on malformed proof");
  }
  if (error == "offsets in path queries are not supported for proofs") {
    Fail("verify path query proof should allow zero offset");
  }

  // Chunk-proof envelope with marker+declared-len but truncated payload.
  std::vector<uint8_t> malformed_chunk = {0x00, 0x01, 0x00};
  if (grovedb::ExecuteChunkProof(malformed_chunk, &root_hash, &error)) {
    Fail("execute chunk proof should fail on truncated envelope");
  }
  // Unsupported chunk marker after a valid envelope header.
  std::vector<uint8_t> unsupported_chunk = {0x00, 0x01, 0x02, 0x00};
  if (grovedb::ExecuteChunkProof(unsupported_chunk, &root_hash, &error)) {
    Fail("execute chunk proof should fail on unsupported marker");
  }
}
}  // namespace

int main() {
  const char* run = std::getenv("GROVEDB_RUN_RUST_PARITY");
  if (run == nullptr) {
    return 0;
  }
  AssertRustMerkOpcodeCoverage();
  std::string dir = MakeTempDir("rust_merk_proof_parity");

  std::string cmd =
      test_utils::RustToolsCargoRunPrefix() + "rust_merk_proof_writer \"" +
      dir + "\"";
  if (std::system(cmd.c_str()) != 0) {
    Fail("failed to run rust merk proof writer");
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
  grovedb::MerkTree count_tree;
  if (!grovedb::MerkStorage::LoadTree(&storage,
                                      {{'r', 'o', 'o', 't', '_', 'c', 'o', 'u', 'n', 't'}},
                                      &count_tree,
                                      &error)) {
    Fail("load count tree failed: " + error);
  }

  std::vector<uint8_t> proof_present;
  std::vector<uint8_t> root_hash_present;
  std::vector<uint8_t> value_present;
  if (!tree.GenerateProof({'k', '2'},
                          grovedb::TargetEncoding::kKv,
                          nullptr,
                          &proof_present,
                          &root_hash_present,
                          &value_present,
                          &error)) {
    Fail("generate present proof failed: " + error);
  }
  std::vector<uint8_t> proof_present_leftmost;
  std::vector<uint8_t> root_hash_present_leftmost;
  std::vector<uint8_t> value_present_leftmost;
  if (!tree.GenerateProof({'k', '1'},
                          grovedb::TargetEncoding::kKv,
                          nullptr,
                          &proof_present_leftmost,
                          &root_hash_present_leftmost,
                          &value_present_leftmost,
                          &error)) {
    Fail("generate present leftmost proof failed: " + error);
  }
  std::vector<uint8_t> proof_present_rightmost;
  std::vector<uint8_t> root_hash_present_rightmost;
  std::vector<uint8_t> value_present_rightmost;
  if (!tree.GenerateProof({'k', '3'},
                          grovedb::TargetEncoding::kKv,
                          nullptr,
                          &proof_present_rightmost,
                          &root_hash_present_rightmost,
                          &value_present_rightmost,
                          &error)) {
    Fail("generate present rightmost proof failed: " + error);
  }
  std::vector<uint8_t> proof_absent;
  std::vector<uint8_t> root_hash_absent;
  if (!tree.GenerateAbsenceProof({'k', '0'}, nullptr, &proof_absent, &root_hash_absent, &error)) {
    Fail("generate absent proof failed: " + error);
  }
  std::vector<uint8_t> proof_range;
  std::vector<uint8_t> root_hash_range;
  if (!tree.GenerateRangeProof({'k', '1'},
                               {'k', '3'},
                               true,
                               true,
                               nullptr,
                               &proof_range,
                               &root_hash_range,
                               &error)) {
    Fail("generate range proof failed: " + error);
  }
  std::vector<uint8_t> proof_range_left_edge_boundary;
  std::vector<uint8_t> root_hash_range_left_edge_boundary;
  if (!tree.GenerateRangeProof({'k', '0'},
                               {'k', '1'},
                               true,
                               true,
                               nullptr,
                               &proof_range_left_edge_boundary,
                               &root_hash_range_left_edge_boundary,
                               &error)) {
    Fail("generate left-edge boundary range proof failed: " + error);
  }
  std::vector<uint8_t> proof_range_right_edge_absence;
  std::vector<uint8_t> root_hash_range_right_edge_absence;
  if (!tree.GenerateRangeProof({'k', '4'},
                               {'k', '5'},
                               true,
                               true,
                               nullptr,
                               &proof_range_right_edge_absence,
                               &root_hash_range_right_edge_absence,
                               &error)) {
    Fail("generate right-edge absence range proof failed: " + error);
  }

  std::vector<uint8_t> proof_range_exclusive_end;
  std::vector<uint8_t> root_hash_range_exclusive_end;
  if (!tree.GenerateRangeProof({'k', '1'},
                                {'k', '3'},
                                true,
                                false,
                                nullptr,
                                &proof_range_exclusive_end,
                                &root_hash_range_exclusive_end,
                                &error)) {
    Fail("generate exclusive-end range proof failed: " + error);
  }

  std::vector<uint8_t> expected_present =
      ReadFile(std::filesystem::path(dir) / "proof_present.bin");
  std::vector<uint8_t> expected_absent =
      ReadFile(std::filesystem::path(dir) / "proof_absent.bin");
  std::vector<uint8_t> expected_range =
      ReadFile(std::filesystem::path(dir) / "proof_range.bin");
  std::vector<uint8_t> expected_left_edge_boundary_range =
      ReadFile(std::filesystem::path(dir) / "proof_range_left_edge_boundary.bin");
  std::vector<uint8_t> expected_right_edge_absence_range = ReadFile(
      std::filesystem::path(dir) / "proof_range_right_edge_absence.bin");
  std::vector<uint8_t> expected_root =
      ReadFile(std::filesystem::path(dir) / "root_hash.bin");

  if (proof_present != expected_present) {
    Fail("present proof mismatch");
  }
  std::cout << "MERK_PROOF_FIXTURE proof_present.bin PASS bytes=" << proof_present.size() << "\n";
  std::vector<uint8_t> expected_present_leftmost =
      ReadFile(std::filesystem::path(dir) / "proof_present_leftmost.bin");
  if (proof_present_leftmost != expected_present_leftmost) {
    std::cerr << "present leftmost proof mismatch: cpp_len=" << proof_present_leftmost.size()
              << " rust_len=" << expected_present_leftmost.size() << "\n";
    std::cerr << "cpp_prefix=" << HexPrefix(proof_present_leftmost)
              << " rust_prefix=" << HexPrefix(expected_present_leftmost) << "\n";
    Fail("present leftmost proof mismatch");
  }
  std::cout << "MERK_PROOF_FIXTURE proof_present_leftmost.bin PASS bytes="
            << proof_present_leftmost.size() << "\n";
  if (root_hash_present_leftmost != expected_root) {
    Fail("present leftmost root hash mismatch");
  }
  std::vector<uint8_t> expected_present_rightmost =
      ReadFile(std::filesystem::path(dir) / "proof_present_rightmost.bin");
  if (proof_present_rightmost != expected_present_rightmost) {
    std::cerr << "present rightmost proof mismatch: cpp_len=" << proof_present_rightmost.size()
              << " rust_len=" << expected_present_rightmost.size() << "\n";
    std::cerr << "cpp_prefix=" << HexPrefix(proof_present_rightmost)
              << " rust_prefix=" << HexPrefix(expected_present_rightmost) << "\n";
    Fail("present rightmost proof mismatch");
  }
  std::cout << "MERK_PROOF_FIXTURE proof_present_rightmost.bin PASS bytes="
            << proof_present_rightmost.size() << "\n";
  if (root_hash_present_rightmost != expected_root) {
    Fail("present rightmost root hash mismatch");
  }
  if (proof_absent != expected_absent) {
    Fail("absent proof mismatch");
  }
  std::cout << "MERK_PROOF_FIXTURE proof_absent.bin PASS bytes=" << proof_absent.size() << "\n";
  if (root_hash_present != expected_root || root_hash_absent != expected_root) {
    Fail("root hash mismatch");
  }
  if (proof_range != expected_range) {
    std::cerr << "range proof mismatch: cpp_len=" << proof_range.size()
              << " rust_len=" << expected_range.size() << "\n";
    std::cerr << "cpp_prefix=" << HexPrefix(proof_range)
              << " rust_prefix=" << HexPrefix(expected_range) << "\n";
    Fail("range proof mismatch");
  }
  std::cout << "MERK_PROOF_FIXTURE proof_range.bin PASS bytes=" << proof_range.size() << "\n";
  if (root_hash_range != expected_root) {
    Fail("range root hash mismatch");
  }
  if (proof_range_left_edge_boundary != expected_left_edge_boundary_range) {
    std::cerr << "left-edge boundary range proof mismatch: cpp_len="
              << proof_range_left_edge_boundary.size()
              << " rust_len=" << expected_left_edge_boundary_range.size() << "\n";
    std::cerr << "cpp_prefix=" << HexPrefix(proof_range_left_edge_boundary)
              << " rust_prefix=" << HexPrefix(expected_left_edge_boundary_range) << "\n";
    Fail("left-edge boundary range proof mismatch");
  }
  std::cout << "MERK_PROOF_FIXTURE proof_range_left_edge_boundary.bin PASS bytes="
            << proof_range_left_edge_boundary.size() << "\n";
  if (root_hash_range_left_edge_boundary != expected_root) {
    Fail("left-edge boundary range root hash mismatch");
  }
  if (proof_range_right_edge_absence != expected_right_edge_absence_range) {
    std::cerr << "right-edge absence range proof mismatch: cpp_len="
              << proof_range_right_edge_absence.size()
              << " rust_len=" << expected_right_edge_absence_range.size() << "\n";
    std::cerr << "cpp_prefix=" << HexPrefix(proof_range_right_edge_absence)
              << " rust_prefix=" << HexPrefix(expected_right_edge_absence_range) << "\n";
    Fail("right-edge absence range proof mismatch");
  }
  std::cout << "MERK_PROOF_FIXTURE proof_range_right_edge_absence.bin PASS bytes="
            << proof_range_right_edge_absence.size() << "\n";
  if (root_hash_range_right_edge_absence != expected_root) {
    Fail("right-edge absence range root hash mismatch");
  }
  std::vector<uint8_t> expected_range_exclusive_end =
      ReadFile(std::filesystem::path(dir) / "proof_range_exclusive_end.bin");
  if (proof_range_exclusive_end != expected_range_exclusive_end) {
    std::cerr << "exclusive-end range proof mismatch: cpp_len="
              << proof_range_exclusive_end.size()
              << " rust_len=" << expected_range_exclusive_end.size() << "\n";
    std::cerr << "cpp_prefix=" << HexPrefix(proof_range_exclusive_end)
              << " rust_prefix=" << HexPrefix(expected_range_exclusive_end) << "\n";
    Fail("exclusive-end range proof mismatch");
  }
  std::cout << "MERK_PROOF_FIXTURE proof_range_exclusive_end.bin PASS bytes="
            << proof_range_exclusive_end.size() << "\n";
  if (root_hash_range_exclusive_end != expected_root) {
    Fail("exclusive-end range root hash mismatch");
  }

  std::vector<uint8_t> proof_count_present;
  std::vector<uint8_t> root_hash_count_present;
  std::vector<uint8_t> value_count_present;
  std::vector<uint8_t> proof_count_absent;
  std::vector<uint8_t> root_hash_count_absent;
  std::vector<uint8_t> proof_count_range;
  std::vector<uint8_t> root_hash_count_range;
  std::vector<uint8_t> expected_count_root =
      ReadFile(std::filesystem::path(dir) / "root_count_hash.bin");
  if (!count_tree.GenerateProof({'k', '2'},
                                grovedb::TargetEncoding::kKv,
                                nullptr,
                                &proof_count_present,
                                &root_hash_count_present,
                                &value_count_present,
                                &error)) {
    Fail("generate present provable-count proof failed: " + error);
  }
  if (!count_tree.GenerateAbsenceProofWithCount(
          {'k', '0'}, nullptr, &proof_count_absent, &root_hash_count_absent, &error)) {
    Fail("generate absent provable-count proof with count failed: " + error);
  }
  if (!count_tree.GenerateRangeProof({'k', '1'},
                                     {'k', '3'},
                                     true,
                                     true,
                                     nullptr,
                                     &proof_count_range,
                                     &root_hash_count_range,
                                     &error)) {
    Fail("generate range provable-count proof failed: " + error);
  }
  if (proof_count_present != ReadFile(std::filesystem::path(dir) / "proof_count_present.bin")) {
    Fail("present count proof mismatch");
  }
  std::cout << "MERK_PROOF_FIXTURE proof_count_present.bin PASS bytes="
            << proof_count_present.size() << "\n";
  {
    std::vector<uint8_t> expected = ReadFile(std::filesystem::path(dir) / "proof_count_absent.bin");
    if (proof_count_absent != expected) {
      std::cerr << "absent count proof mismatch: cpp_len=" << proof_count_absent.size()
                << " rust_len=" << expected.size() << "\n";
      std::cerr << "cpp_prefix=" << HexPrefix(proof_count_absent)
                << " rust_prefix=" << HexPrefix(expected) << "\n";
      Fail("absent count proof mismatch");
    }
  }
  std::cout << "MERK_PROOF_FIXTURE proof_count_absent.bin PASS bytes="
            << proof_count_absent.size() << "\n";
  {
    std::vector<uint8_t> expected = ReadFile(std::filesystem::path(dir) / "proof_count_range.bin");
    if (proof_count_range != expected) {
      std::cerr << "range count proof mismatch: cpp_len=" << proof_count_range.size()
                << " rust_len=" << expected.size() << "\n";
      std::cerr << "cpp_prefix=" << HexPrefix(proof_count_range)
                << " rust_prefix=" << HexPrefix(expected) << "\n";
      Fail("range count proof mismatch");
    }
  }
  std::cout << "MERK_PROOF_FIXTURE proof_count_range.bin PASS bytes=" << proof_count_range.size()
            << "\n";
  std::vector<uint8_t> executed_count_root;

  {
    std::vector<uint8_t> with_count_present;
    std::vector<uint8_t> with_count_absent;
    std::vector<uint8_t> with_count_range;
    std::vector<uint8_t> with_count_root;
    std::vector<uint8_t> with_count_value;
    if (!count_tree.GenerateProofWithCount({'k', '2'},
                                           grovedb::TargetEncoding::kKv,
                                           nullptr,
                                           &with_count_present,
                                           &with_count_root,
                                           &with_count_value,
                                           &error)) {
      Fail("generate proof with count failed: " + error);
    }
    if (!count_tree.GenerateAbsenceProofWithCount(
            {'k', '0'}, nullptr, &with_count_absent, &with_count_root, &error)) {
      Fail("generate absence proof with count failed: " + error);
    }
    if (!count_tree.GenerateRangeProofWithCount({'k', '1'},
                                                {'k', '3'},
                                                true,
                                                true,
                                                nullptr,
                                                &with_count_range,
                                                &with_count_root,
                                                &error)) {
      Fail("generate range proof with count failed: " + error);
    }
    if (!grovedb::ExecuteMerkProof(with_count_present, &executed_count_root, &error) ||
        executed_count_root != expected_count_root) {
      Fail("execute present proof with count root mismatch");
    }
    if (!grovedb::ExecuteMerkProof(with_count_absent, &executed_count_root, &error) ||
        executed_count_root != expected_count_root) {
      Fail("execute absent proof with count root mismatch");
    }
    if (!grovedb::ExecuteMerkProof(with_count_range, &executed_count_root, &error) ||
        executed_count_root != expected_count_root) {
      Fail("execute range proof with count root mismatch");
    }
  }

  AssertProofStats(proof_present, "present");
  AssertProofStats(proof_present_leftmost, "present_leftmost");
  AssertProofStats(proof_present_rightmost, "present_rightmost");
  AssertProofStats(proof_absent, "absent");
  AssertProofStats(proof_range, "range");
  AssertProofStats(proof_range_left_edge_boundary, "range_left_edge_boundary");
  AssertProofStats(proof_range_right_edge_absence, "range_right_edge_absence");
  AssertProofStats(proof_range_exclusive_end, "range_exclusive_end");
  AssertCollectedKvNodes(proof_present, "present", true);
  AssertCollectedKvNodes(proof_present_leftmost, "present_leftmost", true);
  AssertCollectedKvNodes(proof_present_rightmost, "present_rightmost", true);
  AssertCollectedKvNodes(proof_absent, "absent", false);
  AssertCollectedKvNodes(proof_range, "range", true);
  AssertCollectedKvNodes(proof_range_left_edge_boundary, "range_left_edge_boundary", true);
  AssertCollectedKvNodes(proof_range_right_edge_absence, "range_right_edge_absence", false);
  AssertCollectedKvNodes(proof_range_exclusive_end, "range_exclusive_end", true);
  AssertCollectedKeys(proof_present, "present", {{'k', '2'}});
  AssertCollectedKeys(proof_present_leftmost, "present_leftmost", {{'k', '1'}});
  AssertCollectedKeys(proof_present_rightmost, "present_rightmost", {{'k', '3'}});
  AssertCollectedKeys(proof_absent, "absent", {});
  AssertCollectedKeys(proof_range, "range", {{'k', '1'}, {'k', '2'}, {'k', '3'}});
  AssertCollectedKeys(proof_range_left_edge_boundary, "range_left_edge_boundary", {{'k', '1'}});
  AssertCollectedKeys(proof_range_exclusive_end, "range_exclusive_end", {{'k', '1'}, {'k', '2'}});

  std::vector<uint8_t> executed_root;
  if (!grovedb::ExecuteMerkProof(proof_present, &executed_root, &error)) {
    Fail("execute present proof failed: " + error);
  }
  if (executed_root != expected_root) {
    Fail("execute present proof root mismatch");
  }
  if (!grovedb::ExecuteMerkProof(proof_present_leftmost, &executed_root, &error)) {
    Fail("execute present leftmost proof failed: " + error);
  }
  if (executed_root != expected_root) {
    Fail("execute present leftmost proof root mismatch");
  }
  if (!grovedb::ExecuteMerkProof(proof_present_rightmost, &executed_root, &error)) {
    Fail("execute present rightmost proof failed: " + error);
  }
  if (executed_root != expected_root) {
    Fail("execute present rightmost proof root mismatch");
  }
  if (!grovedb::ExecuteMerkProof(proof_absent, &executed_root, &error)) {
    Fail("execute absent proof failed: " + error);
  }
  if (executed_root != expected_root) {
    Fail("execute absent proof root mismatch");
  }
  if (!grovedb::ExecuteMerkProof(proof_range, &executed_root, &error)) {
    Fail("execute range proof failed: " + error);
  }
  if (executed_root != expected_root) {
    Fail("execute range proof root mismatch");
  }
  if (!grovedb::ExecuteMerkProof(proof_range_left_edge_boundary, &executed_root, &error)) {
    Fail("execute left-edge boundary range proof failed: " + error);
  }
  if (executed_root != expected_root) {
    Fail("execute left-edge boundary range proof root mismatch");
  }
  if (!grovedb::ExecuteMerkProof(proof_range_right_edge_absence, &executed_root, &error)) {
    Fail("execute right-edge absence range proof failed: " + error);
  }
  if (executed_root != expected_root) {
    Fail("execute right-edge absence range proof root mismatch");
  }
  if (!grovedb::ExecuteMerkProof(proof_range_exclusive_end, &executed_root, &error)) {
    Fail("execute exclusive-end range proof failed: " + error);
  }
  if (executed_root != expected_root) {
    Fail("execute exclusive-end range proof root mismatch");
  }

  if (proof_present.empty()) {
    Fail("present proof unexpectedly empty");
  }
  if (!HasRejectedMutation(proof_present, expected_root, &error)) {
    Fail("present proof mutation check failed: " + error);
  }

  if (proof_present_leftmost.empty()) {
    Fail("present leftmost proof unexpectedly empty");
  }
  if (!HasRejectedMutation(proof_present_leftmost, expected_root, &error)) {
    Fail("present leftmost proof mutation check failed: " + error);
  }
  if (proof_present_rightmost.empty()) {
    Fail("present rightmost proof unexpectedly empty");
  }
  if (!HasRejectedMutation(proof_present_rightmost, expected_root, &error)) {
    Fail("present rightmost proof mutation check failed: " + error);
  }
  if (proof_absent.empty()) {
    Fail("absent proof unexpectedly empty");
  }
  if (!HasRejectedMutation(proof_absent, expected_root, &error)) {
    Fail("absent proof mutation check failed: " + error);
  }

  if (proof_range.empty()) {
    Fail("range proof unexpectedly empty");
  }
  if (!HasRejectedMutation(proof_range, expected_root, &error)) {
    Fail("range proof mutation check failed: " + error);
  }
  if (proof_range_left_edge_boundary.empty()) {
    Fail("left-edge boundary range proof unexpectedly empty");
  }
  if (!HasRejectedMutation(proof_range_left_edge_boundary, expected_root, &error)) {
    Fail("left-edge boundary range proof mutation check failed: " + error);
  }
  if (proof_range_right_edge_absence.empty()) {
    Fail("right-edge absence range proof unexpectedly empty");
  }
  if (!HasRejectedMutation(proof_range_right_edge_absence, expected_root, &error)) {
    Fail("right-edge absence range proof mutation check failed: " + error);
  }
  if (proof_range_exclusive_end.empty()) {
    Fail("exclusive-end range proof unexpectedly empty");
  }
  if (!HasRejectedMutation(proof_range_exclusive_end, expected_root, &error)) {
    Fail("exclusive-end range proof mutation check failed: " + error);
  }

  AssertTruncatedRejected(proof_present, &executed_root, &error, "present");
  AssertTruncatedRejected(proof_present_leftmost, &executed_root, &error, "present leftmost");
  AssertTruncatedRejected(proof_present_rightmost, &executed_root, &error, "present rightmost");
  AssertTruncatedRejected(proof_absent, &executed_root, &error, "absent");
  AssertTruncatedRejected(proof_range, &executed_root, &error, "range");
  AssertTruncatedRejected(
      proof_range_left_edge_boundary, &executed_root, &error, "left-edge boundary range");
  AssertTruncatedRejected(
      proof_range_right_edge_absence, &executed_root, &error, "right-edge absence range");
  AssertTruncatedRejected(
      proof_range_exclusive_end, &executed_root, &error, "exclusive-end range");
  AssertInvalidInputHandling(proof_present);

  {
    std::vector<uint8_t> rewritten;
    const std::vector<uint8_t> target_key = {'k', '2'};
    const std::vector<uint8_t> reference_value = {'v', '2'};
    const std::vector<uint8_t> resolved_value = {'r', 'e', 's', 'o', 'l', 'v', 'e', 'd'};
    if (!grovedb::RewriteMerkProofForReference(proof_present,
                                               target_key,
                                               reference_value,
                                               resolved_value,
                                               false,
                                               &rewritten,
                                               &error)) {
      Fail("rewrite proof for reference failed: " + error);
    }
    bool has_ref = false;
    if (!grovedb::MerkProofHasReferenceNodes(rewritten, &has_ref, &error)) {
      Fail("reference-node check on rewritten proof failed: " + error);
    }
    if (!has_ref) {
      Fail("rewritten proof should contain reference nodes");
    }
    if (!grovedb::ExecuteMerkProof(rewritten, &executed_root, &error)) {
      Fail("execute rewritten reference proof failed: " + error);
    }
  }

  {
    std::vector<uint8_t> rewritten;
    const std::vector<uint8_t> target_key = {'k', '2'};
    const std::vector<uint8_t> reference_value = {'v', '2'};
    const std::vector<uint8_t> resolved_value = {'r', 'e', 's', 'o', 'l', 'v', 'e', 'd'};
    if (!grovedb::RewriteMerkProofForReference(proof_present,
                                               target_key,
                                               reference_value,
                                               resolved_value,
                                               true,
                                               &rewritten,
                                               &error)) {
      Fail("rewrite proof with provable_count=true failed: " + error);
    }
    if (!grovedb::ExecuteMerkProof(rewritten, &executed_root, &error)) {
      Fail("execute rewritten proof with provable_count=true failed: " + error);
    }
  }

  {
    std::vector<uint8_t> rewritten;
    std::string rewrite_error;
    if (grovedb::RewriteMerkProofForReference(proof_present,
                                              {'k', '2'},
                                              {'n', 'o', 't', '_', 'v', '2'},
                                              {'r', 'e', 's'},
                                              false,
                                              &rewritten,
                                              &rewrite_error)) {
      Fail("rewrite should fail when reference value does not match");
    }
  }

  {
    std::vector<uint8_t> rewritten;
    std::string rewrite_error;
    if (grovedb::RewriteMerkProofForReference(proof_present,
                                              {'z', 'z'},
                                              {'v', '2'},
                                              {'r', 'e', 's'},
                                              false,
                                              &rewritten,
                                              &rewrite_error)) {
      Fail("rewrite should fail when target key is absent");
    }
  }

  std::cout << "MERK_PROOF_FIXTURE_SUMMARY checked=11 status=PASS\n";

  std::filesystem::remove_all(dir);
  return 0;
}
