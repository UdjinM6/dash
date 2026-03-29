#include "binary.h"
#include "merk.h"
#include "proof.h"
#include "query.h"
#include "test_utils.h"

#include <string>
#include <vector>

using test_utils::Fail;

namespace {

std::vector<uint8_t> BuildMerkProofOrFail() {
  grovedb::MerkTree tree;
  std::string error;
  if (!tree.Insert({'a'}, {'1'}, &error) || !tree.Insert({'b'}, {'2'}, &error)) {
    Fail("failed to seed merk tree: " + error);
  }
  std::vector<uint8_t> proof;
  std::vector<uint8_t> proof_root;
  std::vector<uint8_t> proof_value;
  if (!tree.GenerateProof({'a'},
                          grovedb::TargetEncoding::kKv,
                          nullptr,
                          &proof,
                          &proof_root,
                          &proof_value,
                          &error)) {
    Fail("failed to generate proof: " + error);
  }
  return proof;
}

std::vector<uint8_t> BuildChunkProofOrFail() {
  grovedb::MerkTree tree;
  std::string error;
  if (!tree.Insert({'a'}, {'1'}, &error) || !tree.Insert({'b'}, {'2'}, &error)) {
    Fail("failed to seed merk tree: " + error);
  }
  std::vector<uint8_t> proof;
  if (!tree.GenerateChunkProof(4, nullptr, false, &proof, &error)) {
    Fail("failed to generate chunk proof: " + error);
  }
  return proof;
}

}  // namespace

int main() {
  const std::vector<uint8_t> proof = BuildMerkProofOrFail();
  const std::vector<uint8_t> chunk_proof = BuildChunkProofOrFail();
  const std::vector<uint8_t> malformed = {0xFF, 0x00};
  std::string error;

  {
    std::vector<uint8_t> root;
    if (grovedb::ExecuteMerkProof({}, &root, &error)) {
      Fail("ExecuteMerkProof should fail on empty proof");
    }
    if (grovedb::ExecuteMerkProof(proof, nullptr, &error)) {
      Fail("ExecuteMerkProof should fail with null output");
    }
  }

  {
    uint64_t hash_calls = 0;
    if (grovedb::CountMerkProofHashCalls({}, &hash_calls, &error)) {
      Fail("CountMerkProofHashCalls should fail on empty proof");
    }
    if (grovedb::CountMerkProofHashCalls(proof, nullptr, &error)) {
      Fail("CountMerkProofHashCalls should fail with null output");
    }
  }

  {
    uint64_t hash_nodes = 0;
    if (grovedb::CountMerkProofHashNodes({}, &hash_nodes, &error)) {
      Fail("CountMerkProofHashNodes should fail on empty proof");
    }
    if (grovedb::CountMerkProofHashNodes(proof, nullptr, &error)) {
      Fail("CountMerkProofHashNodes should fail with null output");
    }
  }

  {
    bool has_ref = false;
    if (grovedb::MerkProofHasReferenceNodes({}, &has_ref, &error)) {
      Fail("MerkProofHasReferenceNodes should fail on empty proof");
    }
    if (grovedb::MerkProofHasReferenceNodes(proof, nullptr, &error)) {
      Fail("MerkProofHasReferenceNodes should fail with null output");
    }
  }

  {
    std::vector<grovedb::ProofNode> nodes;
    if (grovedb::CollectKvNodes({}, &nodes, &error)) {
      Fail("CollectKvNodes should fail on empty proof");
    }
    if (grovedb::CollectKvNodes(proof, nullptr, &error)) {
      Fail("CollectKvNodes should fail with null output");
    }
    if (grovedb::CollectKvNodes(malformed, &nodes, &error)) {
      Fail("CollectKvNodes should fail on malformed proof");
    }
  }

  {
    std::vector<std::vector<uint8_t>> keys;
    if (grovedb::CollectProofKeys({}, &keys, &error)) {
      Fail("CollectProofKeys should fail on empty proof");
    }
    if (grovedb::CollectProofKeys(proof, nullptr, &error)) {
      Fail("CollectProofKeys should fail with null output");
    }
    if (grovedb::CollectProofKeys(malformed, &keys, &error)) {
      Fail("CollectProofKeys should fail on malformed proof");
    }
    // Ensure PushInverted key-bearing ops are included.
    const std::vector<uint8_t> push_inverted_kv_proof = {
        0x0a,  // PushInverted KV
        0x01,  // key len
        'k',
        0x00, 0x01,  // value len
        'v',
    };
    if (!grovedb::CollectProofKeys(push_inverted_kv_proof, &keys, &error)) {
      Fail("CollectProofKeys should parse PushInverted KV proof: " + error);
    }
    if (keys.size() != 1 || keys.front() != std::vector<uint8_t>({'k'})) {
      Fail("CollectProofKeys should collect key from PushInverted KV proof");
    }
  }

  {
    std::vector<uint8_t> rewritten;
    if (grovedb::RewriteMerkProofForReference({},
                                              {'a'},
                                              {'1'},
                                              {'x'},
                                              false,
                                              &rewritten,
                                              &error)) {
      Fail("RewriteMerkProofForReference should fail on empty proof");
    }
    if (grovedb::RewriteMerkProofForReference(proof,
                                              {'a'},
                                              {'1'},
                                              {'x'},
                                              false,
                                              nullptr,
                                              &error)) {
      Fail("RewriteMerkProofForReference should fail with null output");
    }
  }

  {
    grovedb::GroveLayerProof layer;
    if (grovedb::DecodeGroveDbProof({}, &layer, &error)) {
      Fail("DecodeGroveDbProof should fail on empty payload");
    }
    if (grovedb::DecodeGroveDbProof(malformed, &layer, &error)) {
      Fail("DecodeGroveDbProof should fail on malformed payload");
    }
    if (grovedb::DecodeGroveDbProof(malformed, nullptr, &error)) {
      Fail("DecodeGroveDbProof should fail with null output");
    }
    if (grovedb::EncodeGroveDbProof(layer, nullptr, &error)) {
      Fail("EncodeGroveDbProof should fail with null output");
    }
  }

  {
    std::vector<uint8_t> oversized_map;
    grovedb::EncodeBincodeVarintU64(0, &oversized_map);
    grovedb::EncodeBincodeVecU8({}, &oversized_map);
    grovedb::EncodeBincodeVarintU64(10001, &oversized_map);
    oversized_map.push_back(0);
    grovedb::GroveLayerProof layer;
    error.clear();
    if (grovedb::DecodeGroveDbProof(oversized_map, &layer, &error)) {
      Fail("DecodeGroveDbProof should reject oversized map lengths");
    }
    if (error != "layer proof map too large") {
      Fail("unexpected oversized map error: " + error);
    }
  }

  {
    std::vector<uint8_t> root;
    if (grovedb::ExecuteChunkProof({}, &root, &error)) {
      Fail("ExecuteChunkProof should fail on empty proof");
    }
    if (grovedb::ExecuteChunkProof(chunk_proof, nullptr, &error)) {
      Fail("ExecuteChunkProof should fail with null output");
    }
  }

  {
    const grovedb::PathQuery query = grovedb::PathQuery::NewSingleKey({}, {'k'});
    std::vector<uint8_t> root_hash;
    std::vector<grovedb::VerifiedPathKeyElement> elements;
    if (grovedb::VerifyPathQueryProof({}, query, &root_hash, &elements, &error)) {
      Fail("VerifyPathQueryProof should fail on empty proof");
    }
    if (grovedb::VerifyPathQueryProof(malformed, query, nullptr, &elements, &error)) {
      Fail("VerifyPathQueryProof should fail with null root output");
    }
    if (grovedb::VerifyPathQueryProof(malformed, query, &root_hash, nullptr, &error)) {
      Fail("VerifyPathQueryProof should fail with null elements output");
    }
  }

  return 0;
}
