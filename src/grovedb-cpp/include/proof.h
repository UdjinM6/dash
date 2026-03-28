#ifndef GROVEDB_CPP_PROOF_H
#define GROVEDB_CPP_PROOF_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "grove_version.h"
#include "query.h"

namespace grovedb {

enum class NodeType {
  kHash,
  kKvHash,
  kKv,
  kKvValueHash,
  kKvRefValueHash,
  kKvRefValueHashCount,
  kKvDigest,
};

struct ProofNode {
  NodeType type = NodeType::kKv;
  std::vector<uint8_t> key;
  std::vector<uint8_t> value;
  std::vector<uint8_t> value_hash;
  std::vector<uint8_t> kv_hash;
  std::vector<uint8_t> node_hash;
  std::vector<uint8_t> left_hash;
  std::vector<uint8_t> right_hash;
  uint64_t count = 0;
  bool provable_count = false;
  uint8_t tree_feature_type = 0;
  int64_t tree_feature_sum = 0;
  __int128 tree_feature_big_sum = 0;
  uint64_t tree_feature_count = 0;
  int64_t tree_feature_sum2 = 0;
  bool has_tree_feature_type = false;
  bool has_left = false;
  bool has_right = false;
};

struct GroveLayerProof {
  std::vector<uint8_t> merk_proof;
  std::vector<std::pair<std::vector<uint8_t>, std::unique_ptr<GroveLayerProof>>> lower_layers;
  uint8_t prove_options_tag = 0;
};

struct SingleKeyProofInput {
  std::vector<uint8_t> proof;
  std::vector<uint8_t> root_hash;
  std::vector<uint8_t> element_bytes;
  std::vector<uint8_t> key;
  std::vector<uint8_t> subtree_key;
  std::vector<std::vector<uint8_t>> path;
};

struct RangeProofInput {
  std::vector<uint8_t> proof;
  std::vector<uint8_t> root_hash;
  std::vector<uint8_t> subtree_key;
  std::vector<std::vector<uint8_t>> path;
  std::vector<uint8_t> start_key;
  std::vector<uint8_t> end_key;
  bool start_inclusive = true;
  bool end_inclusive = false;
  std::vector<std::vector<uint8_t>> expected_keys;
  std::vector<std::vector<uint8_t>> expected_element_bytes;
};

struct VerifiedPathKeyElement {
  std::vector<std::vector<uint8_t>> path;
  std::vector<uint8_t> key;
  bool has_element = false;
  std::vector<uint8_t> element_bytes;
};

// Verify a single-key proof. Placeholder for the proof VM implementation.
bool VerifySingleKeyProof(const SingleKeyProofInput& input, std::string* error);
bool VerifySingleKeyAbsenceProof(const SingleKeyProofInput& input, std::string* error);
bool VerifyRangeProof(const RangeProofInput& input, std::string* error);
bool VerifySingleKeyProofForVersion(const SingleKeyProofInput& input,
                                    const GroveVersion& version,
                                    std::string* error);
bool VerifySingleKeyAbsenceProofForVersion(const SingleKeyProofInput& input,
                                           const GroveVersion& version,
                                           std::string* error);
bool VerifyRangeProofForVersion(const RangeProofInput& input,
                                const GroveVersion& version,
                                std::string* error);
bool VerifyPathQueryProof(const std::vector<uint8_t>& proof,
                          const PathQuery& query,
                          std::vector<uint8_t>* out_root_hash,
                          std::vector<VerifiedPathKeyElement>* out_elements,
                          std::string* error);
bool VerifySubsetQuery(const std::vector<uint8_t>& proof,
                       const PathQuery& query,
                       std::vector<uint8_t>* out_root_hash,
                       std::vector<VerifiedPathKeyElement>* out_elements,
                       std::string* error);
bool VerifyQueryWithAbsenceProof(const std::vector<uint8_t>& proof,
                                 const PathQuery& query,
                                 std::vector<uint8_t>* out_root_hash,
                                 std::vector<VerifiedPathKeyElement>* out_elements,
                                 std::string* error);
bool VerifySubsetQueryWithAbsenceProof(const std::vector<uint8_t>& proof,
                                       const PathQuery& query,
                                       std::vector<uint8_t>* out_root_hash,
                                       std::vector<VerifiedPathKeyElement>* out_elements,
                                       std::string* error);
bool VerifyPathQueryProofWithChainedQueries(
    const std::vector<uint8_t>& proof,
    const PathQuery& first_query,
    const std::vector<PathQuery>& chained_queries,
    std::vector<uint8_t>* out_root_hash,
    std::vector<std::vector<VerifiedPathKeyElement>>* out_results,
    std::string* error);
bool VerifyPathQueryProofForVersion(const std::vector<uint8_t>& proof,
                                    const PathQuery& query,
                                    const GroveVersion& version,
                                    std::vector<uint8_t>* out_root_hash,
                                    std::vector<VerifiedPathKeyElement>* out_elements,
                                    std::string* error);
bool VerifySubsetQueryForVersion(const std::vector<uint8_t>& proof,
                                 const PathQuery& query,
                                 const GroveVersion& version,
                                 std::vector<uint8_t>* out_root_hash,
                                 std::vector<VerifiedPathKeyElement>* out_elements,
                                 std::string* error);
bool VerifyQueryWithAbsenceProofForVersion(const std::vector<uint8_t>& proof,
                                           const PathQuery& query,
                                           const GroveVersion& version,
                                           std::vector<uint8_t>* out_root_hash,
                                           std::vector<VerifiedPathKeyElement>* out_elements,
                                           std::string* error);
bool VerifySubsetQueryWithAbsenceProofForVersion(
    const std::vector<uint8_t>& proof,
    const PathQuery& query,
    const GroveVersion& version,
    std::vector<uint8_t>* out_root_hash,
    std::vector<VerifiedPathKeyElement>* out_elements,
    std::string* error);
bool VerifyPathQueryProofWithChainedQueriesForVersion(
    const std::vector<uint8_t>& proof,
    const PathQuery& first_query,
    const std::vector<PathQuery>& chained_queries,
    const GroveVersion& version,
    std::vector<uint8_t>* out_root_hash,
    std::vector<std::vector<VerifiedPathKeyElement>>* out_results,
    std::string* error);
void DumpVerifyPathQueryProfile();

// VerifiedQueryResult holds the result of verify_query_get_parent_tree_info.
struct VerifiedQueryResult {
  std::vector<uint8_t> root_hash;
  uint8_t tree_type = 0;  // Tree feature type (0=unknown, 2=Tree, 4=SumTree, etc.)
  bool has_tree_type = false;
  std::vector<VerifiedPathKeyElement> elements;
};

bool VerifyQueryGetParentTreeInfo(const std::vector<uint8_t>& proof,
                                  const PathQuery& query,
                                  VerifiedQueryResult* out_result,
                                  std::string* error);
bool VerifyQueryGetParentTreeInfoForVersion(const std::vector<uint8_t>& proof,
                                            const PathQuery& query,
                                            const GroveVersion& version,
                                            VerifiedQueryResult* out_result,
                                            std::string* error);

bool CollectKvNodes(const std::vector<uint8_t>& proof,
                    std::vector<ProofNode>* out,
                    std::string* error);
bool CollectProofKeys(const std::vector<uint8_t>& proof,
                      std::vector<std::vector<uint8_t>>* keys,
                      std::string* error);
bool DecodeGroveDbProof(const std::vector<uint8_t>& data,
                        GroveLayerProof* out,
                        std::string* error);
bool EncodeGroveDbProof(const GroveLayerProof& layer,
                        std::vector<uint8_t>* out,
                        std::string* error);
bool RewriteMerkProofForReference(const std::vector<uint8_t>& proof,
                                  const std::vector<uint8_t>& target_key,
                                  const std::vector<uint8_t>& reference_value,
                                  const std::vector<uint8_t>& resolved_value,
                                  bool provable_count,
                                  std::vector<uint8_t>* out,
                                  std::string* error);
bool RewriteMerkProofForDigestKey(const std::vector<uint8_t>& proof,
                                  const std::vector<uint8_t>& target_key,
                                  const std::vector<uint8_t>& target_value,
                                  bool provable_count,
                                  std::vector<uint8_t>* out,
                                  std::string* error);
bool RewriteMerkProofForValueHashKey(const std::vector<uint8_t>& proof,
                                     const std::vector<uint8_t>& target_key,
                                     const std::vector<uint8_t>& target_value,
                                     const std::vector<uint8_t>& target_value_hash,
                                     bool provable_count,
                                     std::vector<uint8_t>* out,
                                     std::string* error);
bool ExecuteChunkProof(const std::vector<uint8_t>& proof,
                       std::vector<uint8_t>* root_hash,
                       std::string* error);
bool ExecuteMerkProof(const std::vector<uint8_t>& proof,
                      std::vector<uint8_t>* root_hash,
                      std::string* error);
bool CountMerkProofHashCalls(const std::vector<uint8_t>& proof,
                             uint64_t* out,
                             std::string* error);
bool CountMerkProofHashNodes(const std::vector<uint8_t>& proof,
                             uint64_t* out,
                             std::string* error);
bool MerkProofHasReferenceNodes(const std::vector<uint8_t>& proof,
                                bool* out,
                                std::string* error);

}  // namespace grovedb

#endif  // GROVEDB_CPP_PROOF_H
