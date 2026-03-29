#ifndef GROVEDB_CPP_MERK_H
#define GROVEDB_CPP_MERK_H

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <unordered_set>

#include "merk_node.h"
#include "operation_cost.h"
#include "rocksdb_wrapper.h"
#include "value_defined_cost.h"

namespace grovedb {

enum class TargetEncoding {
  kKv,
  kKvValueHash,
};

class MerkTree {
 public:
  using ValueHashFn = std::function<bool(const std::vector<uint8_t>& key,
                                         const std::vector<uint8_t>& value,
                                         std::vector<uint8_t>* out,
                                         std::string* error)>;
  using ValueDefinedCostFn = std::function<bool(
      const std::vector<uint8_t>& value,
      std::optional<ValueDefinedCostType>* out,
      std::string* error)>;
  using SumValueFn = std::function<bool(const std::vector<uint8_t>& value,
                                        int64_t* out_sum,
                                        bool* has_sum,
                                        std::string* error)>;

  bool Insert(const std::vector<uint8_t>& key,
              const std::vector<uint8_t>& value,
              std::string* error);
  bool Insert(const std::vector<uint8_t>& key,
              const std::vector<uint8_t>& value,
              uint64_t* hash_calls,
              std::string* error);
  bool Insert(const std::vector<uint8_t>& key,
              const std::vector<uint8_t>& value,
              OperationCost* cost,
              std::string* error);
  bool Delete(const std::vector<uint8_t>& key, bool* deleted, std::string* error);
  bool Delete(const std::vector<uint8_t>& key,
              bool* deleted,
              uint64_t* hash_calls,
              std::string* error);
  bool Delete(const std::vector<uint8_t>& key,
              bool* deleted,
              OperationCost* cost,
              std::string* error);
  void SetValueHashFn(ValueHashFn value_hash_fn);
  void SetValueDefinedCostFn(ValueDefinedCostFn value_defined_cost_fn);
  const ValueHashFn& GetValueHashFn() const { return value_hash_fn_; }
  // Rebuild all cached value_hash / node_hash in the loaded tree using the
  // current value_hash_fn_.  After success, hash_caches_canonical_ is true and
  // the child-hash shortcut in PushHashOpForChild is enabled.
  bool RebuildHashCaches(std::string* error);

  bool Get(const std::vector<uint8_t>& key, std::vector<uint8_t>* value) const;
  bool GetValueAndValueHash(const std::vector<uint8_t>& key,
                            std::vector<uint8_t>* value,
                            std::vector<uint8_t>* value_hash) const;
  bool RecomputeHashesForKey(const std::vector<uint8_t>& key, std::string* error);
  bool RootKey(std::vector<uint8_t>* out) const;
  bool InitialRootKey(std::vector<uint8_t>* out) const;
  bool InitialRootKeyEqualsCurrent() const;
  void MarkPersistedRootKey(const std::vector<uint8_t>& root_key);
  void AttachStorage(RocksDbWrapper* storage,
                     const std::vector<std::vector<uint8_t>>& path,
                     ColumnFamilyKind cf);
  void AttachStorage(RocksDbWrapper::Transaction* transaction,
                     const std::vector<std::vector<uint8_t>>& path,
                     ColumnFamilyKind cf);
  void AttachStorage(RocksDbWrapper* storage,
                     RocksDbWrapper::Transaction* transaction,
                     const std::vector<std::vector<uint8_t>>& path,
                     ColumnFamilyKind cf);
  bool IsLazyLoading() const { return lazy_loading_; }
  bool MinKey(std::vector<uint8_t>* out) const;
  bool MaxKey(std::vector<uint8_t>* out) const;

  // Testing-only: enumerate all key-value pairs via callback
  bool EnumerateKvPairsForTesting(
      const std::function<bool(const std::vector<uint8_t>& key, const std::vector<uint8_t>& value)>& callback,
      std::string* error) const;

  bool ComputeRootHash(const ValueHashFn& value_hash_fn,
                       std::vector<uint8_t>* out,
                       std::string* error) const;
  bool ComputeRootHashWithCount(const ValueHashFn& value_hash_fn,
                                std::vector<uint8_t>* out,
                                uint64_t* out_count,
                                std::string* error) const;
  bool GetCachedRootHash(std::vector<uint8_t>* out, std::string* error) const;
  void ClearCachedHashes();
  bool EstimateHashCallsForKey(const std::vector<uint8_t>& key,
                               uint64_t* out_count,
                               std::string* error) const;
  struct RootAggregate {
    uint64_t count = 0;
    __int128 sum = 0;
    bool has_count = false;
    bool has_sum = false;
  };
  bool RootAggregateData(RootAggregate* out, std::string* error) const;
  bool ComputeCount(uint64_t* out_count, std::string* error) const;
  bool ComputeSum(const SumValueFn& sum_fn, int64_t* out_sum, std::string* error) const;
  bool ComputeSumBig(const SumValueFn& sum_fn, __int128* out_sum, std::string* error) const;
  bool ComputeCountAndSum(const SumValueFn& sum_fn,
                          uint64_t* out_count,
                          int64_t* out_sum,
                          std::string* error) const;
  bool ComputeCountAndSumBig(const SumValueFn& sum_fn,
                             uint64_t* out_count,
                             __int128* out_sum,
                             std::string* error) const;

  bool GenerateProof(const std::vector<uint8_t>& key,
                     TargetEncoding target_encoding,
                     const ValueHashFn& value_hash_fn,
                     std::vector<uint8_t>* out_proof,
                     std::vector<uint8_t>* out_root_hash,
                     std::vector<uint8_t>* out_value,
                     std::string* error) const;
  bool GenerateProofWithCount(const std::vector<uint8_t>& key,
                              TargetEncoding target_encoding,
                              const ValueHashFn& value_hash_fn,
                              std::vector<uint8_t>* out_proof,
                              std::vector<uint8_t>* out_root_hash,
                              std::vector<uint8_t>* out_value,
                              std::string* error) const;
  bool GenerateRangeProof(const std::vector<uint8_t>& start_key,
                          const std::vector<uint8_t>& end_key,
                          bool start_inclusive,
                          bool end_inclusive,
                          const ValueHashFn& value_hash_fn,
                          std::vector<uint8_t>* out_proof,
                          std::vector<uint8_t>* out_root_hash,
                          std::string* error) const;
  bool GenerateRangeProofWithTargetEncoding(const std::vector<uint8_t>& start_key,
                                            const std::vector<uint8_t>& end_key,
                                            bool start_inclusive,
                                            bool end_inclusive,
                                            TargetEncoding target_encoding,
                                            const ValueHashFn& value_hash_fn,
                                            std::vector<uint8_t>* out_proof,
                                            std::vector<uint8_t>* out_root_hash,
                                            std::string* error) const;
  bool GenerateRangeProofWithLimit(const std::vector<uint8_t>& start_key,
                                   const std::vector<uint8_t>& end_key,
                                   bool start_inclusive,
                                   bool end_inclusive,
                                   size_t limit,
                                   const ValueHashFn& value_hash_fn,
                                   std::vector<uint8_t>* out_proof,
                                   std::vector<uint8_t>* out_root_hash,
                                   std::string* error) const;
  bool GenerateRangeProofWithCount(const std::vector<uint8_t>& start_key,
                                   const std::vector<uint8_t>& end_key,
                                   bool start_inclusive,
                                   bool end_inclusive,
                                   const ValueHashFn& value_hash_fn,
                                   std::vector<uint8_t>* out_proof,
                                   std::vector<uint8_t>* out_root_hash,
                                   std::string* error) const;
  bool GenerateAbsenceProof(const std::vector<uint8_t>& key,
                            const ValueHashFn& value_hash_fn,
                            std::vector<uint8_t>* out_proof,
                            std::vector<uint8_t>* out_root_hash,
                            std::string* error) const;
  bool GenerateAbsenceProofWithCount(const std::vector<uint8_t>& key,
                                     const ValueHashFn& value_hash_fn,
                                     std::vector<uint8_t>* out_proof,
                                     std::vector<uint8_t>* out_root_hash,
                                     std::string* error) const;
  bool GenerateChunkProof(size_t depth,
                          const ValueHashFn& value_hash_fn,
                          bool provable_count,
                          std::vector<uint8_t>* out_proof,
                          std::string* error) const;
  bool GenerateChunkProofAt(const std::vector<bool>& instructions,
                            size_t depth,
                            const ValueHashFn& value_hash_fn,
                            bool provable_count,
                            std::vector<uint8_t>* out_proof,
                            std::string* error) const;
  bool GenerateChunkOps(size_t depth,
                        const ValueHashFn& value_hash_fn,
                        bool provable_count,
                        std::vector<uint8_t>* out_proof,
                        std::string* error) const;
  bool GenerateChunkOpsAt(const std::vector<bool>& instructions,
                          size_t depth,
                          const ValueHashFn& value_hash_fn,
                          bool provable_count,
                          std::vector<uint8_t>* out_proof,
                          std::string* error) const;
  int Height() const;
  bool FindKeyPath(const std::vector<uint8_t>& key,
                   std::vector<bool>* out,
                   std::string* error) const;
  bool ComputeNodeHashAtPath(const std::vector<bool>& path,
                             const ValueHashFn& value_hash_fn,
                             bool provable_count,
                             std::vector<uint8_t>* out,
                             std::string* error) const;
  struct NodeMeta {
    bool has_left = false;
    bool has_right = false;
    std::vector<uint8_t> left_hash;
    int left_left_height = 0;
    int left_right_height = 0;
    std::vector<uint8_t> right_hash;
    int right_left_height = 0;
    int right_right_height = 0;
  };
  bool GetNodeMeta(const std::vector<uint8_t>& key, NodeMeta* out, std::string* error) const;
  bool LoadEncodedTree(RocksDbWrapper* storage,
                       const std::vector<std::vector<uint8_t>>& path,
                       const std::vector<uint8_t>& root_key,
                       std::string* error);
  bool LoadEncodedTree(RocksDbWrapper* storage,
                       const std::vector<std::vector<uint8_t>>& path,
                       const std::vector<uint8_t>& root_key,
                       ColumnFamilyKind cf,
                       std::string* error);
  bool LoadEncodedTree(RocksDbWrapper* storage,
                       const std::vector<std::vector<uint8_t>>& path,
                       const std::vector<uint8_t>& root_key,
                       ColumnFamilyKind cf,
                       bool lazy,
                       std::string* error);
  bool LoadEncodedTree(RocksDbWrapper::Transaction* transaction,
                       const std::vector<std::vector<uint8_t>>& path,
                       const std::vector<uint8_t>& root_key,
                       ColumnFamilyKind cf,
                       bool lazy,
                       std::string* error);
  bool ExportEncodedNodes(
      std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>>* out,
      std::vector<uint8_t>* root_key,
      const ValueHashFn& value_hash_fn,
      std::string* error) const;
  bool ExportEncodedNodesForKeys(
      const std::vector<std::vector<uint8_t>>& keys,
      std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>>* out,
      std::vector<uint8_t>* root_key,
      const ValueHashFn& value_hash_fn,
      std::string* error) const;
  bool FeatureEncodingLengthForKey(const std::vector<uint8_t>& key,
                                   TreeFeatureTypeTag tag,
                                   uint32_t* out_len,
                                   std::string* error) const;
  void SetTreeFeatureTag(TreeFeatureTypeTag tag);
  TreeFeatureTypeTag GetTreeFeatureTag() const { return tree_feature_tag_; }
  void SnapshotDirtyKeys(std::vector<std::vector<uint8_t>>* out) const;
  void SnapshotDeletedKeys(std::vector<std::vector<uint8_t>>* out) const;
  void AcknowledgeDirtyKeys(const std::vector<std::vector<uint8_t>>& keys);
  void AcknowledgeDeletedKeys(const std::vector<std::vector<uint8_t>>& keys);
  void ConsumeDirtyKeys(std::vector<std::vector<uint8_t>>* out);
  void ConsumeDeletedKeys(std::vector<std::vector<uint8_t>>* out);
  bool ExportEncodedNodes(
      std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>>* out,
      std::vector<uint8_t>* root_key,
      std::string* error) const;
  bool Export(std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>>* out) const;
  bool Validate(std::string* error) const;
  MerkTree Clone() const;
  bool EnsureFullyLoaded(std::string* error) const;

 private:
  struct AggregateValues {
    uint64_t count = 0;
    int64_t sum = 0;
    __int128 big_sum = 0;
  };

  struct Node {
    struct ChildMeta {
      std::vector<uint8_t> key;
      std::vector<uint8_t> hash;
      int left_height = 0;
      int right_height = 0;
      AggregateData aggregate;
      bool present = false;
      bool loaded = false;
    };

    std::vector<uint8_t> key;
    std::vector<uint8_t> value;
    std::vector<uint8_t> value_hash;
    std::vector<uint8_t> kv_hash;
    std::vector<uint8_t> node_hash;
    uint64_t hash_generation = 0;
    TreeFeatureType feature_type;
    std::unique_ptr<Node> left;
    std::unique_ptr<Node> right;
    int height = 1;
    ChildMeta left_meta;
    ChildMeta right_meta;
  };

  bool InsertNode(std::unique_ptr<Node>* node,
                  const std::vector<uint8_t>& key,
                  const std::vector<uint8_t>& value,
                  const ValueHashFn& value_hash_fn,
                  std::unordered_set<std::string>* dirty,
                  std::string* error);
  bool InsertNodeWithCount(std::unique_ptr<Node>* node,
                           const std::vector<uint8_t>& key,
                           const std::vector<uint8_t>& value,
                           const ValueHashFn& value_hash_fn,
                           std::unordered_set<std::string>* dirty,
                           uint64_t* hash_calls,
                           std::string* error);
  bool InsertNodeWithCost(std::unique_ptr<Node>* node,
                          const std::vector<uint8_t>& key,
                          const std::vector<uint8_t>& value,
                          const ValueHashFn& value_hash_fn,
                          const ValueDefinedCostFn* value_defined_cost_fn,
                          std::unordered_set<std::string>* dirty,
                          TreeFeatureTypeTag tree_feature_tag,
                          OperationCost* cost,
                          std::string* error);
  bool DeleteNode(std::unique_ptr<Node>* node,
                  const std::vector<uint8_t>& key,
                  const ValueHashFn& value_hash_fn,
                  std::unordered_set<std::string>* dirty,
                  std::unordered_set<std::string>* deleted,
                  bool* deleted_flag,
                  std::string* error);
  bool DeleteNodeWithCount(std::unique_ptr<Node>* node,
                           const std::vector<uint8_t>& key,
                           const ValueHashFn& value_hash_fn,
                           std::unordered_set<std::string>* dirty,
                           std::unordered_set<std::string>* deleted,
                           bool* deleted_flag,
                           uint64_t* hash_calls,
                           std::string* error);
  bool DeleteNodeWithCost(std::unique_ptr<Node>* node,
                          const std::vector<uint8_t>& key,
                          const ValueHashFn& value_hash_fn,
                          const ValueDefinedCostFn* value_defined_cost_fn,
                          std::unordered_set<std::string>* dirty,
                          std::unordered_set<std::string>* deleted,
                          bool* deleted_flag,
                          TreeFeatureTypeTag tree_feature_tag,
                          OperationCost* cost,
                          std::string* error);
  static OperationCost::TreeCostType TreeCostTypeForFeature(TreeFeatureTypeTag tag);
  static bool BuildChildCostInfo(const Node* child,
                                 const Node::ChildMeta& meta,
                                 ChildCostInfo* out);
  static bool BuildChildrenSizes(const Node* node,
                                 OperationCost::ChildrenSizesWithIsSumTree* out,
                                 std::string* error);
  static bool ComputePaidValueLen(
      uint32_t key_len,
      uint32_t value_len,
      const OperationCost::ChildrenSizesWithIsSumTree& children_sizes,
      uint32_t* out);
  static Node* FindMinNode(Node* node);
  static Node* FindNode(Node* node, const std::vector<uint8_t>& key);
  bool RecomputeHashesForKey(Node* node,
                             const std::vector<uint8_t>& key,
                             const ValueHashFn& value_hash_fn,
                             std::unordered_set<std::string>* dirty,
                             std::string* error);
  static int Height(const std::unique_ptr<Node>& node);
  static void UpdateHeight(Node* node);
  bool UpdateNodeHash(Node* node,
                      const ValueHashFn& value_hash_fn,
                      std::string* error);
  void UpdateNodeHashOrDie(Node* node, const ValueHashFn& value_hash_fn);
  static void ClearCachedHashes(Node* node);
  static int BalanceFactor(const std::unique_ptr<Node>& node);
  static void RotateLeft(std::unique_ptr<Node>* node);
  static void RotateRight(std::unique_ptr<Node>* node);
  static bool GetNode(const Node* node,
                      const std::vector<uint8_t>& key,
                      std::vector<uint8_t>* value);
  static bool ComputeNodeHash(const Node* node,
                              const ValueHashFn& value_hash_fn,
                              std::vector<uint8_t>* out,
                              std::string* error);
  static bool ComputeNodeHashWithCount(const Node* node,
                                       const ValueHashFn& value_hash_fn,
                                       std::vector<uint8_t>* out,
                                       uint64_t* out_count,
                                       std::string* error);
  static bool UsesProvableCountHashing(TreeFeatureTypeTag tag);
  bool ComputeTreeTypedNodeHash(const Node* node,
                                const ValueHashFn& value_hash_fn,
                                std::vector<uint8_t>* out,
                                std::string* error) const;
  static bool ComputeNodeCount(const Node* node, uint64_t* out_count, std::string* error);
  static bool ComputeNodeSum(const Node* node,
                             const SumValueFn& sum_fn,
                             int64_t* out_sum,
                             std::string* error);
  static bool ComputeNodeSumBig(const Node* node,
                                const SumValueFn& sum_fn,
                                __int128* out_sum,
                                std::string* error);
  static bool ComputeNodeCountAndSum(const Node* node,
                                     const SumValueFn& sum_fn,
                                     uint64_t* out_count,
                                     int64_t* out_sum,
                                     std::string* error);
  static bool ComputeNodeCountAndSumBig(const Node* node,
                                        const SumValueFn& sum_fn,
                                        uint64_t* out_count,
                                        __int128* out_sum,
                                        std::string* error);
  bool EnsureChildLoaded(Node* node, bool left, std::string* error) const;
  static bool BuildEncodedNodeInternal(RocksDbWrapper* storage,
                                       const std::vector<std::vector<uint8_t>>& path,
                                       const std::vector<uint8_t>& key,
                                       ColumnFamilyKind cf,
                                       std::unordered_set<std::string>* visiting,
                                       std::unique_ptr<Node>* out,
                                       std::vector<uint8_t>* out_hash,
                                       int* out_height,
                                       int* out_left_height,
                                       int* out_right_height,
                                       bool load_children,
                                       std::string* error);
  static bool BuildEncodedNodeInternalTx(RocksDbWrapper::Transaction* transaction,
                                         const std::vector<std::vector<uint8_t>>& path,
                                         const std::vector<uint8_t>& key,
                                         ColumnFamilyKind cf,
                                         std::unordered_set<std::string>* visiting,
                                         std::unique_ptr<Node>* out,
                                         std::vector<uint8_t>* out_hash,
                                         int* out_height,
                                         int* out_left_height,
                                         int* out_right_height,
                                         bool load_children,
                                         std::string* error);
  static bool ValidateNode(const Node* node,
                           const std::vector<uint8_t>* min_key,
                           const std::vector<uint8_t>* max_key,
                           int* out_height,
                           std::string* error);
  static bool BuildEncodedNode(RocksDbWrapper* storage,
                               const std::vector<std::vector<uint8_t>>& path,
                               const std::vector<uint8_t>& key,
                               ColumnFamilyKind cf,
                               std::unordered_set<std::string>* visiting,
                               std::unique_ptr<Node>* out,
                               std::vector<uint8_t>* out_hash,
                               int* out_height,
                               int* out_left_height,
                               int* out_right_height,
                               std::string* error);
  static int ChildHeightFromMeta(const Node::ChildMeta& meta);
  static int ChildHeight(const Node* node, bool left);
  static bool AggregateCount(const AggregateData& aggregate, uint64_t* out, bool* has_count);
  static bool AggregateSumI64(const AggregateData& aggregate,
                              int64_t* out,
                              bool* has_sum,
                              std::string* error);
  static bool AggregateSumI128(const AggregateData& aggregate, __int128* out, bool* has_sum);
  static std::unique_ptr<Node> CloneNode(const Node* node);

  struct ProofOp {
    enum class Type {
      kPushHash,
      kPushKv,
      kPushKvHash,
      kPushKvValueHash,
      kPushKvDigest,
      kPushKvCount,
      kPushKvHashCount,
      kPushKvDigestCount,
      kParent,
      kChild,
    } type;
    std::vector<uint8_t> key;
    std::vector<uint8_t> value;
    std::vector<uint8_t> hash;
    uint64_t count = 0;
    bool provable_count = false;
    bool has_feature_type = false;
  };

  enum class ProofMode {
    kTargetValue,
    kAllHashes,
  };
  bool EmitProofOps(Node* node,
                    const std::vector<uint8_t>& target_key,
                    TargetEncoding target_encoding,
                    ProofMode mode,
                    const ValueHashFn& value_hash_fn,
                    std::vector<ProofOp>* ops,
                    std::string* error) const;
  bool EmitProofOpsForPresent(Node* node,
                              const std::vector<uint8_t>& target_key,
                              TargetEncoding target_encoding,
                              const ValueHashFn& value_hash_fn,
                              std::vector<ProofOp>* ops,
                              std::string* error) const;
  bool EmitProofOpsForPresentWithCount(Node* node,
                                       const std::vector<uint8_t>& target_key,
                                       TargetEncoding target_encoding,
                                       const ValueHashFn& value_hash_fn,
                                       std::vector<ProofOp>* ops,
                                       std::string* error) const;
  bool EmitProofOpsForRange(Node* node,
                            const std::vector<uint8_t>& start_key,
                            const std::vector<uint8_t>& end_key,
                            bool start_inclusive,
                            bool end_inclusive,
                            TargetEncoding target_encoding,
                            const ValueHashFn& value_hash_fn,
                            std::vector<ProofOp>* ops,
                            std::string* error) const;
  bool EmitProofOpsForRange(Node* node,
                            const std::vector<uint8_t>& start_key,
                            const std::vector<uint8_t>& end_key,
                            bool start_inclusive,
                            bool end_inclusive,
                            TargetEncoding target_encoding,
                            const ValueHashFn& value_hash_fn,
                            std::vector<ProofOp>* ops,
                            bool* out_has_match,
                            std::string* error) const;
  bool EmitProofOpsForRange(Node* node,
                            const std::vector<uint8_t>& start_key,
                            const std::vector<uint8_t>& end_key,
                            bool start_inclusive,
                            bool end_inclusive,
                            TargetEncoding target_encoding,
                            const ValueHashFn& value_hash_fn,
                            std::vector<ProofOp>* ops,
                            bool* out_has_match,
                            bool* out_left_absence,
                            bool* out_right_absence,
                            std::string* error) const;
  bool EmitProofOpsForRangeWithLimit(Node* node,
                                     const std::vector<uint8_t>& start_key,
                                     const std::vector<uint8_t>& end_key,
                                     bool start_inclusive,
                                     bool end_inclusive,
                                     size_t* remaining_limit,
                                     const ValueHashFn& value_hash_fn,
                                     std::vector<ProofOp>* ops,
                                     std::string* error) const;
  bool EmitProofOpsForRangeWithCount(Node* node,
                                     const std::vector<uint8_t>& start_key,
                                     const std::vector<uint8_t>& end_key,
                                     bool start_inclusive,
                                     bool end_inclusive,
                                     const ValueHashFn& value_hash_fn,
                                     std::vector<ProofOp>* ops,
                                     std::string* error) const;
  bool HasKeyInRange(Node* node,
                     const std::vector<uint8_t>& start_key,
                     const std::vector<uint8_t>& end_key,
                     bool start_inclusive,
                     bool end_inclusive,
                     std::string* error) const;
  bool EmitProofOpsForAbsent(Node* node,
                             const std::vector<uint8_t>& target_key,
                             const Node* predecessor,
                             const Node* successor,
                             const ValueHashFn& value_hash_fn,
                             std::vector<ProofOp>* ops,
                             std::string* error) const;
  bool EmitProofOpsForAbsentWithCount(Node* node,
                                      const std::vector<uint8_t>& target_key,
                                      const Node* predecessor,
                                      const Node* successor,
                                      const ValueHashFn& value_hash_fn,
                                      std::vector<ProofOp>* ops,
                                      std::string* error) const;
  bool EmitProofOpsForExtreme(Node* node,
                              bool max_key,
                              const ValueHashFn& value_hash_fn,
                              std::vector<ProofOp>* ops,
                              std::string* error) const;
  bool EmitChunkProofOps(Node* node,
                         size_t remaining_depth,
                         const ValueHashFn& value_hash_fn,
                         bool provable_count,
                         std::vector<ProofOp>* ops,
                         std::string* error) const;
  static bool EmitChunkProofOpsForNode(const Node* node,
                                       const ValueHashFn& value_hash_fn,
                                       bool provable_count,
                                       std::vector<ProofOp>* ops,
                                       std::string* error);
  static bool EncodeChunkProofOps(const std::vector<ProofOp>& ops,
                                  std::vector<uint8_t>* out,
                                  std::string* error);
  static bool PushHashOp(const Node* node,
                         const ValueHashFn& value_hash_fn,
                         std::vector<ProofOp>* ops,
                         std::string* error);
  static bool PushHashOpWithCount(const Node* node,
                                  const ValueHashFn& value_hash_fn,
                                  std::vector<ProofOp>* ops,
                                  std::string* error);
  static bool PushKvHashOp(const Node* node,
                           const ValueHashFn& value_hash_fn,
                           std::vector<ProofOp>* ops,
                           std::string* error);
  static bool PushKvHashCountOp(const Node* node,
                                const ValueHashFn& value_hash_fn,
                                std::vector<ProofOp>* ops,
                                std::string* error);
  static bool PushKvDigestOp(const Node* node,
                             const ValueHashFn& value_hash_fn,
                             std::vector<ProofOp>* ops,
                             std::string* error);
  static bool PushKvDigestCountOp(const Node* node,
                                  const ValueHashFn& value_hash_fn,
                                  std::vector<ProofOp>* ops,
                                  std::string* error);
  static bool EncodeProofOps(const std::vector<ProofOp>& ops,
                             std::vector<uint8_t>* out,
                             std::string* error);
  bool HasChild(const Node* node, bool left) const;
  bool PushHashOpForChild(const Node* node,
                          bool left,
                          const ValueHashFn& value_hash_fn,
                          std::vector<ProofOp>* ops,
                          std::string* error) const;
  bool PushHashOpWithCountForChild(const Node* node,
                                   bool left,
                                   const ValueHashFn& value_hash_fn,
                                   std::vector<ProofOp>* ops,
                                   std::string* error) const;

  mutable std::unique_ptr<Node> root_;
  ValueHashFn value_hash_fn_;
  ValueDefinedCostFn value_defined_cost_fn_;
  static std::string KeyToString(const std::vector<uint8_t>& key);
  static std::vector<uint8_t> StringToKey(const std::string& key);
  static void MarkDirtyKey(std::unordered_set<std::string>* dirty,
                           const std::vector<uint8_t>& key);
  static void MarkDeletedKey(std::unordered_set<std::string>* deleted,
                             const std::vector<uint8_t>& key);
  static bool AggregateValuesFromMeta(const AggregateData& aggregate,
                                      AggregateValues* out,
                                      std::string* error);
  static AggregateData AggregateDataFromValues(TreeFeatureTypeTag tag,
                                               const AggregateValues& values);
  static TreeFeatureType FeatureTypeFromValues(TreeFeatureTypeTag tag,
                                               const AggregateValues& values);
  static bool ComputeAggregateValues(const Node* node,
                                     TreeFeatureTypeTag tree_feature_tag,
                                     AggregateValues* out,
                                     std::string* error);
  static bool ComputeAggregateValuesFromChildren(const Node* node,
                                                 TreeFeatureTypeTag tree_feature_tag,
                                                 const AggregateValues& left,
                                                 const AggregateValues& right,
                                                 AggregateValues* out,
                                                 std::string* error);
  static bool ComputeNodeFeatureValues(const Node* node,
                                       TreeFeatureTypeTag tree_feature_tag,
                                       AggregateValues* out,
                                       std::string* error);
  std::unordered_set<std::string> dirty_keys_;
  std::unordered_set<std::string> deleted_keys_;
  RocksDbWrapper* storage_ = nullptr;
  RocksDbWrapper::Transaction* storage_tx_ = nullptr;
  ColumnFamilyKind storage_cf_ = ColumnFamilyKind::kDefault;
  std::vector<std::vector<uint8_t>> storage_path_;
  std::vector<uint8_t> initial_root_key_;
  bool has_initial_root_key_ = false;
  mutable bool lazy_loading_ = false;
  TreeFeatureTypeTag tree_feature_tag_ = TreeFeatureTypeTag::kBasic;
  // True only when all cached node_hash / value_hash values in the tree are
  // known to match the current effective hash policy (default ValueHash, no
  // provable-count).  Initialized false because persisted hashes may have been
  // produced under a different policy.  Set true only by an explicit
  // RebuildHashCaches() call that rehashes the full loaded tree with the
  // current value_hash_fn_.
  mutable bool hash_caches_canonical_ = false;
  uint64_t hash_policy_generation_ = 1;
};

}  // namespace grovedb

#endif  // GROVEDB_CPP_MERK_H
