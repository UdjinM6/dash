#include "merk.h"
#include "test_utils.h"

#include <optional>
#include <string>
#include <vector>

using test_utils::Fail;

namespace {

void ExpectError(const std::string& label,
                 bool ok,
                 const std::string& actual,
                 const std::string& expected) {
  if (ok) {
    Fail(label + " unexpectedly succeeded");
  }
  if (actual != expected) {
    Fail(label + " expected '" + expected + "', got '" + actual + "'");
  }
}

std::vector<uint8_t> Key(const char* s) {
  return std::vector<uint8_t>(s, s + std::char_traits<char>::length(s));
}

}  // namespace

int main() {
  std::string error;
  grovedb::MerkTree tree;
  const std::vector<std::vector<uint8_t>> subtree_path = {{'r', 'o', 'o', 't'}};

  if (!tree.Insert(Key("a"), Key("va"), &error) || !tree.Insert(Key("b"), Key("vb"), &error) ||
      !tree.Insert(Key("c"), Key("vc"), &error)) {
    Fail("seed insert failed: " + error);
  }

  const grovedb::MerkTree::ValueHashFn value_hash_fn = nullptr;
  const grovedb::MerkTree::SumValueFn sum_fn =
      [](const std::vector<uint8_t>&, int64_t* out_sum, bool* has_sum, std::string*) -> bool {
    *out_sum = 0;
    *has_sum = false;
    return true;
  };

  {
    std::vector<uint8_t> root;
    uint64_t count = 0;
    int64_t sum = 0;
    __int128 big_sum = 0;
    error.clear();
    ExpectError("ComputeRootHash null out",
                tree.ComputeRootHash(value_hash_fn, nullptr, &error),
                error,
                "root hash output is null");
    error.clear();
    ExpectError("ComputeRootHashWithCount null out",
                tree.ComputeRootHashWithCount(value_hash_fn, nullptr, &count, &error),
                error,
                "root hash/count output is null");
    error.clear();
    ExpectError("ComputeRootHashWithCount null count",
                tree.ComputeRootHashWithCount(value_hash_fn, &root, nullptr, &error),
                error,
                "root hash/count output is null");
    error.clear();
    ExpectError("EstimateHashCallsForKey null out",
                tree.EstimateHashCallsForKey(Key("a"), nullptr, &error),
                error,
                "hash count output is null");
    error.clear();
    ExpectError("ComputeCount null out", tree.ComputeCount(nullptr, &error), error, "count output is null");
    error.clear();
    ExpectError("ComputeSum null out", tree.ComputeSum(sum_fn, nullptr, &error), error, "sum output is null");
    error.clear();
    ExpectError("ComputeSumBig null out",
                tree.ComputeSumBig(sum_fn, nullptr, &error),
                error,
                "sum output is null");
    error.clear();
    ExpectError("ComputeCountAndSum null count",
                tree.ComputeCountAndSum(sum_fn, nullptr, &sum, &error),
                error,
                "count/sum output is null");
    error.clear();
    ExpectError("ComputeCountAndSum null sum",
                tree.ComputeCountAndSum(sum_fn, &count, nullptr, &error),
                error,
                "count/sum output is null");
    error.clear();
    ExpectError("ComputeCountAndSumBig null count",
                tree.ComputeCountAndSumBig(sum_fn, nullptr, &big_sum, &error),
                error,
                "count/sum output is null");
    error.clear();
    ExpectError("ComputeCountAndSumBig null sum",
                tree.ComputeCountAndSumBig(sum_fn, &count, nullptr, &error),
                error,
                "count/sum output is null");
  }

  {
    std::vector<uint8_t> proof;
    std::vector<uint8_t> root;
    std::vector<uint8_t> value;
    error.clear();
    ExpectError("GenerateProof null outputs",
                tree.GenerateProof(Key("a"),
                                   grovedb::TargetEncoding::kKv,
                                   value_hash_fn,
                                   nullptr,
                                   &root,
                                   &value,
                                   &error),
                error,
                "proof outputs are null");
    error.clear();
    ExpectError("GenerateProof missing key",
                tree.GenerateProof(Key("z"),
                                   grovedb::TargetEncoding::kKv,
                                   value_hash_fn,
                                   &proof,
                                   &root,
                                   &value,
                                   &error),
                error,
                "key not found");
    error.clear();
    ExpectError("GenerateProofWithCount invalid encoding",
                tree.GenerateProofWithCount(Key("a"),
                                            grovedb::TargetEncoding::kKvValueHash,
                                            value_hash_fn,
                                            &proof,
                                            &root,
                                            &value,
                                            &error),
                error,
                "provable count proof requires kv target encoding");
    error.clear();
    ExpectError("GenerateAbsenceProof existing key",
                tree.GenerateAbsenceProof(Key("a"), value_hash_fn, &proof, &root, &error),
                error,
                "key exists");
    error.clear();
    ExpectError("GenerateAbsenceProofWithCount existing key",
                tree.GenerateAbsenceProofWithCount(Key("a"), value_hash_fn, &proof, &root, &error),
                error,
                "key exists");
    error.clear();
    ExpectError("GenerateRangeProof reversed bounds",
                tree.GenerateRangeProof(Key("c"),
                                        Key("a"),
                                        true,
                                        true,
                                        value_hash_fn,
                                        &proof,
                                        &root,
                                        &error),
                error,
                "range end precedes start");
    error.clear();
    ExpectError("GenerateRangeProofWithCount reversed bounds",
                tree.GenerateRangeProofWithCount(Key("c"),
                                                 Key("a"),
                                                 true,
                                                 true,
                                                 value_hash_fn,
                                                 &proof,
                                                 &root,
                                                 &error),
                error,
                "range end precedes start");
  }

  {
    std::vector<bool> path_bits;
    std::vector<uint8_t> hash;
    grovedb::MerkTree::NodeMeta meta;
    uint32_t feature_len = 0;
    error.clear();
    ExpectError("FindKeyPath null out",
                tree.FindKeyPath(Key("a"), nullptr, &error),
                error,
                "path output is null");
    error.clear();
    ExpectError("FindKeyPath missing key",
                tree.FindKeyPath(Key("z"), &path_bits, &error),
                error,
                "key not found");
    error.clear();
    ExpectError("ComputeNodeHashAtPath null out",
                tree.ComputeNodeHashAtPath({}, value_hash_fn, false, nullptr, &error),
                error,
                "hash output is null");
    error.clear();
    ExpectError("ComputeNodeHashAtPath invalid path",
                tree.ComputeNodeHashAtPath({true, true, true, true}, value_hash_fn, false, &hash, &error),
                error,
                "invalid traversal path");
    error.clear();
    ExpectError("GetNodeMeta null out",
                tree.GetNodeMeta(Key("a"), nullptr, &error),
                error,
                "node meta output is null");
    error.clear();
    ExpectError("GetNodeMeta missing key",
                tree.GetNodeMeta(Key("z"), &meta, &error),
                error,
                "node key not found");
    error.clear();
    ExpectError("FeatureEncodingLengthForKey null out",
                tree.FeatureEncodingLengthForKey(Key("a"),
                                                 grovedb::TreeFeatureTypeTag::kBasic,
                                                 nullptr,
                                                 &error),
                error,
                "feature length output is null");
    error.clear();
    ExpectError("FeatureEncodingLengthForKey missing key",
                tree.FeatureEncodingLengthForKey(Key("z"),
                                                 grovedb::TreeFeatureTypeTag::kSum,
                                                 &feature_len,
                                                 &error),
                error,
                "feature length key not found");
  }

  {
    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> nodes;
    std::vector<uint8_t> root_key;
    error.clear();
    ExpectError("ExportEncodedNodes null out",
                tree.ExportEncodedNodes(nullptr, &root_key, value_hash_fn, &error),
                error,
                "encoded export output is null");
    error.clear();
    ExpectError("ExportEncodedNodesForKeys null out",
                tree.ExportEncodedNodesForKeys({Key("a")}, nullptr, &root_key, value_hash_fn, &error),
                error,
                "encoded export output is null");
  }

  {
    grovedb::MerkTree loaded;
    grovedb::RocksDbWrapper::Transaction tx;
    error.clear();
    ExpectError("LoadEncodedTree null storage",
                loaded.LoadEncodedTree(static_cast<grovedb::RocksDbWrapper*>(nullptr),
                                       subtree_path,
                                       Key("root"),
                                       &error),
                error,
                "storage is null");
    error.clear();
    ExpectError("LoadEncodedTree empty root key",
                loaded.LoadEncodedTree(static_cast<grovedb::RocksDbWrapper*>(nullptr),
                                       subtree_path,
                                       {},
                                       &error),
                error,
                "storage is null");
    error.clear();
    ExpectError("LoadEncodedTree tx null tx",
                loaded.LoadEncodedTree(static_cast<grovedb::RocksDbWrapper::Transaction*>(nullptr),
                                       subtree_path,
                                       Key("root"),
                                       grovedb::ColumnFamilyKind::kDefault,
                                       false,
                                       &error),
                error,
                "transaction is null");
    error.clear();
    ExpectError("LoadEncodedTree tx empty root",
                loaded.LoadEncodedTree(&tx,
                                       subtree_path,
                                       {},
                                       grovedb::ColumnFamilyKind::kDefault,
                                       false,
                                       &error),
                error,
                "root key is empty");
  }

  {
    grovedb::MerkTree empty;
    std::vector<uint8_t> chunk;
    error.clear();
    ExpectError("GenerateChunkProof empty tree",
                empty.GenerateChunkProof(2, value_hash_fn, false, &chunk, &error),
                error,
                "empty tree");
    error.clear();
    ExpectError("GenerateChunkProof null output",
                tree.GenerateChunkProof(2, value_hash_fn, false, nullptr, &error),
                error,
                "chunk proof output is null");
    error.clear();
    ExpectError("GenerateChunkProofAt null output",
                tree.GenerateChunkProofAt({}, 2, value_hash_fn, false, nullptr, &error),
                error,
                "chunk proof output is null");
    error.clear();
    ExpectError("GenerateChunkProofAt invalid traversal",
                tree.GenerateChunkProofAt({true, true, true, true}, 2, value_hash_fn, false, &chunk, &error),
                error,
                "invalid chunk traversal instruction");
  }

  return 0;
}
