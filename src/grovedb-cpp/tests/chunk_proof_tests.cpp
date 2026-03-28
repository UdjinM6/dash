#include "merk.h"
#include "proof.h"
#include "test_utils.h"

#include <string>
#include <vector>

using test_utils::Fail;

int main() {
  std::string error;
  grovedb::MerkTree tree;
  if (!tree.Insert({'a'}, {'1'}, &error) ||
      !tree.Insert({'b'}, {'2'}, &error) ||
      !tree.Insert({'c'}, {'3'}, &error) ||
      !tree.Insert({'d'}, {'4'}, &error) ||
      !tree.Insert({'e'}, {'5'}, &error)) {
    Fail("insert failed: " + error);
  }

  std::vector<uint8_t> expected_root;
  if (!tree.ComputeRootHash(nullptr, &expected_root, &error)) {
    Fail("compute root hash failed: " + error);
  }

  std::vector<uint8_t> chunk_proof;
  if (!tree.GenerateChunkProof(100, nullptr, false, &chunk_proof, &error)) {
    Fail("generate chunk proof failed: " + error);
  }
  if (chunk_proof.empty() || chunk_proof.front() != 0x00) {
    Fail("chunk proof missing chunk id marker");
  }

  std::vector<uint8_t> proof_root;
  if (!grovedb::ExecuteChunkProof(chunk_proof, &proof_root, &error)) {
    Fail("execute chunk proof failed: " + error);
  }
  if (proof_root != expected_root) {
    Fail("chunk proof root hash mismatch");
  }

  {
    grovedb::MerkTree provable_tree;
    if (!provable_tree.Insert({'k'}, {0xFF}, &error)) {
      Fail("insert provable count failed: " + error);
    }
    std::vector<uint8_t> provable_proof;
    if (!provable_tree.GenerateChunkProof(4, nullptr, true, &provable_proof, &error)) {
      Fail("generate provable chunk proof failed: " + error);
    }
    bool has_feature_type = false;
    for (uint8_t byte : provable_proof) {
      if (byte == 0x07 || byte == 0x23) {
        has_feature_type = true;
        break;
      }
    }
    if (!has_feature_type) {
      Fail("provable chunk proof missing feature type opcode");
    }
  }

  {
    grovedb::MerkTree rewrite_tree;
    std::vector<uint8_t> target_key = {'r'};
    std::vector<uint8_t> ref_value = {0x01, 0x02};
    std::vector<uint8_t> resolved_value = {0x09};
    if (!rewrite_tree.Insert(target_key, ref_value, &error)) {
      Fail("insert rewrite value failed: " + error);
    }
    std::vector<uint8_t> proof;
    if (!rewrite_tree.GenerateChunkProof(4, nullptr, false, &proof, &error)) {
      Fail("generate rewrite chunk proof failed: " + error);
    }
    std::vector<uint8_t> rewritten;
    if (!grovedb::RewriteMerkProofForReference(proof,
                                               target_key,
                                               ref_value,
                                               resolved_value,
                                               false,
                                               &rewritten,
                                               &error)) {
      Fail("rewrite chunk proof failed: " + error);
    }
    std::vector<uint8_t> rewritten_root;
    if (!grovedb::ExecuteChunkProof(rewritten, &rewritten_root, &error)) {
      Fail("execute rewritten chunk proof failed: " + error);
    }
  }

  return 0;
}
