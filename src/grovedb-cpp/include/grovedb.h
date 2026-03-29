#ifndef GROVEDB_CPP_GROVEDB_H
#define GROVEDB_CPP_GROVEDB_H

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "element.h"
#include "grove_version.h"
#include "merk_cache.h"
#include "merk_storage.h"
#include "operation_cost.h"
#include "query.h"

namespace grovedb {

struct VerificationIssue {
  std::vector<std::vector<uint8_t>> path;
  std::vector<uint8_t> root_hash;
  std::vector<uint8_t> expected_value_hash;
  std::vector<uint8_t> actual_value_hash;
};

class GroveDb {
 public:
  struct BatchOp {
    enum class Kind {
      kInsert = 0,
      kDelete = 1,
      kInsertOnly = 2,
      kReplace = 3,
      kDeleteTree = 4,
      kPatch = 5,
      kRefreshReference = 6,
      kInsertOrReplace = 7,
      kInsertTree = 8,
    };
    Kind kind = Kind::kInsert;
    std::vector<std::vector<uint8_t>> path;
    std::vector<uint8_t> key;
    std::vector<uint8_t> element_bytes;
    // Rust patch ops carry a signed byte delta used for cost accounting.
    // C++ keeps the field for parity/API alignment; current execution semantics
    // do not yet consume it.
    int32_t change_in_bytes = 0;
  };

  struct BatchApplyOptions {
    bool validate_insertion_does_not_override = false;
    bool validate_insertion_does_not_override_tree = false;
    bool allow_deleting_non_empty_trees = false;
    bool deleting_non_empty_trees_returns_error = true;
    bool disable_operation_consistency_check = false;
    bool trust_refresh_reference = false;
    bool base_root_storage_is_free = true;
    int32_t batch_pause_height = -1;
  };

  // Leftover operations from a paused partial batch, keyed by level (path.size()).
  using OpsByLevelPath = std::map<uint32_t, std::vector<BatchOp>>;

  struct PathKeyOptionalElement {
    std::vector<std::vector<uint8_t>> path;
    std::vector<uint8_t> key;
    bool element_found = false;
    std::vector<uint8_t> element_bytes;
  };

  struct KeyElementPair {
    std::vector<uint8_t> key;
    std::vector<uint8_t> element_bytes;
  };

  struct QueryItemOrSumValue {
    enum class Kind {
      kItemData = 0,
      kSumValue = 1,
      kBigSumValue = 2,
      kCountValue = 3,
      kCountSumValue = 4,
      kItemDataWithSumValue = 5,
    };
    Kind kind = Kind::kItemData;
    std::vector<uint8_t> item_data;
    int64_t sum_value = 0;
    std::vector<uint8_t> big_sum_value;
    int64_t count_value = 0;
  };

  struct Transaction {
    enum class State {
      kUninitialized = 0,
      kActive = 1,
      kRolledBack = 2,
      kCommitted = 3,
    };
    RocksDbWrapper::Transaction inner;
    std::unordered_set<std::string> validated_tree_paths;
    std::unique_ptr<MerkCache> insert_cache;
    std::string insert_cache_path_key;
    State state = State::kUninitialized;
  };

  bool Open(const std::string& path, std::string* error);
  bool OpenCheckpoint(const std::string& path, std::string* error);
  bool Wipe(std::string* error);
  bool Flush(std::string* error);
  bool StartVisualizer(const std::string& addr, std::string* error);
  bool CreateCheckpoint(const std::string& path, std::string* error);
  static bool DeleteCheckpoint(const std::string& path, std::string* error);

  bool Insert(const std::vector<std::vector<uint8_t>>& path,
              const std::vector<uint8_t>& key,
              const std::vector<uint8_t>& element_bytes,
              std::string* error);
  bool Insert(const std::vector<std::vector<uint8_t>>& path,
              const std::vector<uint8_t>& key,
              const std::vector<uint8_t>& element_bytes,
              OperationCost* cost,
              std::string* error);
  bool Insert(const std::vector<std::vector<uint8_t>>& path,
              const std::vector<uint8_t>& key,
              const std::vector<uint8_t>& element_bytes,
              Transaction* tx,
              std::string* error);
  bool Insert(const std::vector<std::vector<uint8_t>>& path,
              const std::vector<uint8_t>& key,
              const std::vector<uint8_t>& element_bytes,
              OperationCost* cost,
              Transaction* tx,
              std::string* error);
  bool InsertItem(const std::vector<std::vector<uint8_t>>& path,
                  const std::vector<uint8_t>& key,
                  const std::vector<uint8_t>& value,
                  std::string* error);
  bool InsertItem(const std::vector<std::vector<uint8_t>>& path,
                  const std::vector<uint8_t>& key,
                  const std::vector<uint8_t>& value,
                  OperationCost* cost,
                  std::string* error);
  bool InsertItem(const std::vector<std::vector<uint8_t>>& path,
                  const std::vector<uint8_t>& key,
                  const std::vector<uint8_t>& value,
                  Transaction* tx,
                  std::string* error);
  bool InsertItem(const std::vector<std::vector<uint8_t>>& path,
                  const std::vector<uint8_t>& key,
                  const std::vector<uint8_t>& value,
                  OperationCost* cost,
                  Transaction* tx,
                  std::string* error);
  bool InsertEmptyTree(const std::vector<std::vector<uint8_t>>& path,
                       const std::vector<uint8_t>& key,
                       std::string* error);
  bool InsertEmptyTree(const std::vector<std::vector<uint8_t>>& path,
                       const std::vector<uint8_t>& key,
                       OperationCost* cost,
                       std::string* error);
  bool InsertEmptyTree(const std::vector<std::vector<uint8_t>>& path,
                       const std::vector<uint8_t>& key,
                       Transaction* tx,
                       std::string* error);
  bool InsertEmptyTree(const std::vector<std::vector<uint8_t>>& path,
                       const std::vector<uint8_t>& key,
                       OperationCost* cost,
                       Transaction* tx,
                       std::string* error);
  bool InsertBigSumTree(const std::vector<std::vector<uint8_t>>& path,
                        const std::vector<uint8_t>& key,
                        std::string* error);
  bool InsertBigSumTree(const std::vector<std::vector<uint8_t>>& path,
                        const std::vector<uint8_t>& key,
                        OperationCost* cost,
                        std::string* error);
  bool InsertBigSumTree(const std::vector<std::vector<uint8_t>>& path,
                        const std::vector<uint8_t>& key,
                        Transaction* tx,
                        std::string* error);
  bool InsertBigSumTree(const std::vector<std::vector<uint8_t>>& path,
                        const std::vector<uint8_t>& key,
                        OperationCost* cost,
                        Transaction* tx,
                        std::string* error);
  bool InsertCountTree(const std::vector<std::vector<uint8_t>>& path,
                       const std::vector<uint8_t>& key,
                       std::string* error);
  bool InsertCountTree(const std::vector<std::vector<uint8_t>>& path,
                       const std::vector<uint8_t>& key,
                       OperationCost* cost,
                       std::string* error);
  bool InsertCountTree(const std::vector<std::vector<uint8_t>>& path,
                       const std::vector<uint8_t>& key,
                       Transaction* tx,
                       std::string* error);
  bool InsertCountTree(const std::vector<std::vector<uint8_t>>& path,
                       const std::vector<uint8_t>& key,
                       OperationCost* cost,
                       Transaction* tx,
                       std::string* error);
  bool InsertProvableCountTree(const std::vector<std::vector<uint8_t>>& path,
                               const std::vector<uint8_t>& key,
                               std::string* error);
  bool InsertProvableCountTree(const std::vector<std::vector<uint8_t>>& path,
                               const std::vector<uint8_t>& key,
                               OperationCost* cost,
                               std::string* error);
  bool InsertProvableCountTree(const std::vector<std::vector<uint8_t>>& path,
                               const std::vector<uint8_t>& key,
                               Transaction* tx,
                               std::string* error);
  bool InsertProvableCountTree(const std::vector<std::vector<uint8_t>>& path,
                               const std::vector<uint8_t>& key,
                               OperationCost* cost,
                               Transaction* tx,
                               std::string* error);
  bool InsertProvableCountSumTree(const std::vector<std::vector<uint8_t>>& path,
                                  const std::vector<uint8_t>& key,
                                  std::string* error);
  bool InsertProvableCountSumTree(const std::vector<std::vector<uint8_t>>& path,
                                  const std::vector<uint8_t>& key,
                                  OperationCost* cost,
                                  std::string* error);
  bool InsertProvableCountSumTree(const std::vector<std::vector<uint8_t>>& path,
                                  const std::vector<uint8_t>& key,
                                  Transaction* tx,
                                  std::string* error);
  bool InsertProvableCountSumTree(const std::vector<std::vector<uint8_t>>& path,
                                  const std::vector<uint8_t>& key,
                                  OperationCost* cost,
                                  Transaction* tx,
                                  std::string* error);
  bool InsertSumItem(const std::vector<std::vector<uint8_t>>& path,
                     const std::vector<uint8_t>& key,
                     int64_t sum_value,
                     std::string* error);
  bool InsertSumItem(const std::vector<std::vector<uint8_t>>& path,
                     const std::vector<uint8_t>& key,
                     int64_t sum_value,
                     OperationCost* cost,
                     std::string* error);
  bool InsertSumItem(const std::vector<std::vector<uint8_t>>& path,
                     const std::vector<uint8_t>& key,
                     int64_t sum_value,
                     Transaction* tx,
                     std::string* error);
  bool InsertSumItem(const std::vector<std::vector<uint8_t>>& path,
                     const std::vector<uint8_t>& key,
                     int64_t sum_value,
                     OperationCost* cost,
                     Transaction* tx,
                     std::string* error);
  bool InsertItemWithSum(const std::vector<std::vector<uint8_t>>& path,
                          const std::vector<uint8_t>& key,
                          const std::vector<uint8_t>& value,
                          int64_t sum_value,
                          std::string* error);
  bool InsertItemWithSum(const std::vector<std::vector<uint8_t>>& path,
                          const std::vector<uint8_t>& key,
                          const std::vector<uint8_t>& value,
                          int64_t sum_value,
                          OperationCost* cost,
                          std::string* error);
  bool InsertItemWithSum(const std::vector<std::vector<uint8_t>>& path,
                          const std::vector<uint8_t>& key,
                          const std::vector<uint8_t>& value,
                          int64_t sum_value,
                          Transaction* tx,
                          std::string* error);
  bool InsertItemWithSum(const std::vector<std::vector<uint8_t>>& path,
                           const std::vector<uint8_t>& key,
                           const std::vector<uint8_t>& value,
                           int64_t sum_value,
                           OperationCost* cost,
                           Transaction* tx,
                           std::string* error);
  bool InsertReference(const std::vector<std::vector<uint8_t>>& path,
                       const std::vector<uint8_t>& key,
                       const ReferencePathType& reference_path,
                       std::string* error);
  bool InsertReference(const std::vector<std::vector<uint8_t>>& path,
                       const std::vector<uint8_t>& key,
                       const ReferencePathType& reference_path,
                       OperationCost* cost,
                       std::string* error);
  bool InsertReference(const std::vector<std::vector<uint8_t>>& path,
                       const std::vector<uint8_t>& key,
                       const ReferencePathType& reference_path,
                       Transaction* tx,
                       std::string* error);
  bool InsertReference(const std::vector<std::vector<uint8_t>>& path,
                       const std::vector<uint8_t>& key,
                       const ReferencePathType& reference_path,
                       OperationCost* cost,
                       Transaction* tx,
                       std::string* error);
  bool InsertIfNotExists(const std::vector<std::vector<uint8_t>>& path,
                         const std::vector<uint8_t>& key,
                         const std::vector<uint8_t>& element_bytes,
                         bool* inserted,
                         std::string* error);
  bool InsertIfNotExists(const std::vector<std::vector<uint8_t>>& path,
                         const std::vector<uint8_t>& key,
                         const std::vector<uint8_t>& element_bytes,
                         bool* inserted,
                         OperationCost* cost,
                         std::string* error);
  bool InsertIfNotExists(const std::vector<std::vector<uint8_t>>& path,
                         const std::vector<uint8_t>& key,
                         const std::vector<uint8_t>& element_bytes,
                         bool* inserted,
                         Transaction* tx,
                         std::string* error);
  bool InsertIfNotExists(const std::vector<std::vector<uint8_t>>& path,
                         const std::vector<uint8_t>& key,
                         const std::vector<uint8_t>& element_bytes,
                         bool* inserted,
                         OperationCost* cost,
                         Transaction* tx,
                         std::string* error);
  bool InsertIfChangedValue(const std::vector<std::vector<uint8_t>>& path,
                            const std::vector<uint8_t>& key,
                            const std::vector<uint8_t>& element_bytes,
                            bool* inserted,
                            std::vector<uint8_t>* previous_element_bytes,
                            bool* previous_element_found,
                            std::string* error);
  bool InsertIfChangedValue(const std::vector<std::vector<uint8_t>>& path,
                            const std::vector<uint8_t>& key,
                            const std::vector<uint8_t>& element_bytes,
                            bool* inserted,
                            std::vector<uint8_t>* previous_element_bytes,
                            bool* previous_element_found,
                            OperationCost* cost,
                            std::string* error);
  bool InsertIfChangedValue(const std::vector<std::vector<uint8_t>>& path,
                            const std::vector<uint8_t>& key,
                            const std::vector<uint8_t>& element_bytes,
                            bool* inserted,
                            std::vector<uint8_t>* previous_element_bytes,
                            bool* previous_element_found,
                            Transaction* tx,
                            std::string* error);
  bool InsertIfChangedValue(const std::vector<std::vector<uint8_t>>& path,
                            const std::vector<uint8_t>& key,
                            const std::vector<uint8_t>& element_bytes,
                            bool* inserted,
                            std::vector<uint8_t>* previous_element_bytes,
                            bool* previous_element_found,
                            OperationCost* cost,
                            Transaction* tx,
                            std::string* error);
  bool InsertIfNotExistsReturnExisting(
      const std::vector<std::vector<uint8_t>>& path,
      const std::vector<uint8_t>& key,
      const std::vector<uint8_t>& element_bytes,
      std::vector<uint8_t>* existing_element_bytes,
      bool* had_existing,
      std::string* error);
  bool InsertIfNotExistsReturnExisting(
      const std::vector<std::vector<uint8_t>>& path,
      const std::vector<uint8_t>& key,
      const std::vector<uint8_t>& element_bytes,
      std::vector<uint8_t>* existing_element_bytes,
      bool* had_existing,
      OperationCost* cost,
      std::string* error);
  bool InsertIfNotExistsReturnExisting(
      const std::vector<std::vector<uint8_t>>& path,
      const std::vector<uint8_t>& key,
      const std::vector<uint8_t>& element_bytes,
      std::vector<uint8_t>* existing_element_bytes,
      bool* had_existing,
      Transaction* tx,
      std::string* error);
  bool InsertIfNotExistsReturnExisting(
      const std::vector<std::vector<uint8_t>>& path,
      const std::vector<uint8_t>& key,
      const std::vector<uint8_t>& element_bytes,
      std::vector<uint8_t>* existing_element_bytes,
      bool* had_existing,
      OperationCost* cost,
      Transaction* tx,
      std::string* error);

  bool Delete(const std::vector<std::vector<uint8_t>>& path,
              const std::vector<uint8_t>& key,
              bool* deleted,
              std::string* error);
  bool Delete(const std::vector<std::vector<uint8_t>>& path,
              const std::vector<uint8_t>& key,
              bool* deleted,
              OperationCost* cost,
              std::string* error);
  bool Delete(const std::vector<std::vector<uint8_t>>& path,
              const std::vector<uint8_t>& key,
              bool* deleted,
              Transaction* tx,
              std::string* error);
  bool Delete(const std::vector<std::vector<uint8_t>>& path,
              const std::vector<uint8_t>& key,
              bool* deleted,
              OperationCost* cost,
              Transaction* tx,
              std::string* error);
  bool ClearSubtree(const std::vector<std::vector<uint8_t>>& path,
                    std::string* error);
  bool ClearSubtree(const std::vector<std::vector<uint8_t>>& path,
                    OperationCost* cost,
                    std::string* error);
  bool ClearSubtree(const std::vector<std::vector<uint8_t>>& path,
                    Transaction* tx,
                    std::string* error);
  bool ClearSubtree(const std::vector<std::vector<uint8_t>>& path,
                    OperationCost* cost,
                    Transaction* tx,
                    std::string* error);
  bool DeleteSubtree(const std::vector<std::vector<uint8_t>>& path,
                     const std::vector<uint8_t>& key,
                     std::string* error);
  bool DeleteSubtree(const std::vector<std::vector<uint8_t>>& path,
                     const std::vector<uint8_t>& key,
                     OperationCost* cost,
                     std::string* error);
  bool DeleteSubtree(const std::vector<std::vector<uint8_t>>& path,
                     const std::vector<uint8_t>& key,
                     Transaction* tx,
                     std::string* error);
  bool DeleteSubtree(const std::vector<std::vector<uint8_t>>& path,
                     const std::vector<uint8_t>& key,
                     OperationCost* cost,
                     Transaction* tx,
                     std::string* error);
  /// Version-gated wrapper for DeleteSubtree with unsupported-version rejection.
  bool DeleteSubtreeForVersion(const std::vector<std::vector<uint8_t>>& path,
                               const std::vector<uint8_t>& key,
                               const GroveVersion& version,
                               std::string* error);
  bool DeleteSubtreeForVersion(const std::vector<std::vector<uint8_t>>& path,
                               const std::vector<uint8_t>& key,
                               const GroveVersion& version,
                               OperationCost* cost,
                               std::string* error);
  bool DeleteSubtreeForVersion(const std::vector<std::vector<uint8_t>>& path,
                               const std::vector<uint8_t>& key,
                               const GroveVersion& version,
                               Transaction* tx,
                               std::string* error);
  bool DeleteSubtreeForVersion(const std::vector<std::vector<uint8_t>>& path,
                               const std::vector<uint8_t>& key,
                               const GroveVersion& version,
                               OperationCost* cost,
                               Transaction* tx,
                               std::string* error);
  bool DeleteIfEmptyTree(const std::vector<std::vector<uint8_t>>& path,
                         const std::vector<uint8_t>& key,
                         bool* deleted,
                         std::string* error);
  bool DeleteIfEmptyTree(const std::vector<std::vector<uint8_t>>& path,
                         const std::vector<uint8_t>& key,
                         bool* deleted,
                         OperationCost* cost,
                         std::string* error);
  bool DeleteIfEmptyTree(const std::vector<std::vector<uint8_t>>& path,
                         const std::vector<uint8_t>& key,
                         bool* deleted,
                         Transaction* tx,
                         std::string* error);
  bool DeleteIfEmptyTree(const std::vector<std::vector<uint8_t>>& path,
                         const std::vector<uint8_t>& key,
                         bool* deleted,
                         OperationCost* cost,
                         Transaction* tx,
                         std::string* error);
  bool DeleteUpTreeWhileEmpty(const std::vector<std::vector<uint8_t>>& path,
                              const std::vector<uint8_t>& key,
                              uint16_t* deleted_count,
                              std::string* error);
  bool DeleteUpTreeWhileEmpty(const std::vector<std::vector<uint8_t>>& path,
                              const std::vector<uint8_t>& key,
                              uint16_t* deleted_count,
                              OperationCost* cost,
                              std::string* error);
  bool DeleteUpTreeWhileEmpty(const std::vector<std::vector<uint8_t>>& path,
                              const std::vector<uint8_t>& key,
                              uint16_t* deleted_count,
                              Transaction* tx,
                              std::string* error);
  bool DeleteUpTreeWhileEmpty(const std::vector<std::vector<uint8_t>>& path,
                              const std::vector<uint8_t>& key,
                              uint16_t* deleted_count,
                              OperationCost* cost,
                              Transaction* tx,
                              std::string* error);
  bool DeleteUpTreeWhileEmpty(const std::vector<std::vector<uint8_t>>& path,
                              const std::vector<uint8_t>& key,
                              uint16_t* deleted_count,
                              uint16_t stop_path_height,
                              std::string* error);
  bool DeleteUpTreeWhileEmpty(const std::vector<std::vector<uint8_t>>& path,
                              const std::vector<uint8_t>& key,
                              uint16_t* deleted_count,
                              uint16_t stop_path_height,
                              OperationCost* cost,
                              std::string* error);
  bool DeleteUpTreeWhileEmpty(const std::vector<std::vector<uint8_t>>& path,
                              const std::vector<uint8_t>& key,
                              uint16_t* deleted_count,
                              uint16_t stop_path_height,
                              Transaction* tx,
                              std::string* error);
  bool DeleteUpTreeWhileEmpty(const std::vector<std::vector<uint8_t>>& path,
                              const std::vector<uint8_t>& key,
                              uint16_t* deleted_count,
                              uint16_t stop_path_height,
                              OperationCost* cost,
                              Transaction* tx,
                              std::string* error);

  bool Get(const std::vector<std::vector<uint8_t>>& path,
           const std::vector<uint8_t>& key,
           std::vector<uint8_t>* element_bytes,
           bool* found,
           std::string* error);
  bool GetRaw(const std::vector<std::vector<uint8_t>>& path,
              const std::vector<uint8_t>& key,
              std::vector<uint8_t>* element_bytes,
              bool* found,
              std::string* error);
  bool GetRaw(const std::vector<std::vector<uint8_t>>& path,
              const std::vector<uint8_t>& key,
              std::vector<uint8_t>* element_bytes,
              bool* found,
              OperationCost* cost,
              std::string* error);
  bool FollowReference(const std::vector<std::vector<uint8_t>>& path,
                       const std::vector<uint8_t>& key,
                       std::vector<uint8_t>* element_bytes,
                       bool* found,
                       std::string* error);
  bool FollowReference(const std::vector<std::vector<uint8_t>>& path,
                       const std::vector<uint8_t>& key,
                       std::vector<uint8_t>* element_bytes,
                       bool* found,
                       OperationCost* cost,
                       std::string* error);
  bool Get(const std::vector<std::vector<uint8_t>>& path,
           const std::vector<uint8_t>& key,
           std::vector<uint8_t>* element_bytes,
           bool* found,
           Transaction* tx,
           std::string* error);
  bool GetRaw(const std::vector<std::vector<uint8_t>>& path,
              const std::vector<uint8_t>& key,
              std::vector<uint8_t>* element_bytes,
              bool* found,
              Transaction* tx,
              std::string* error);
  bool GetRaw(const std::vector<std::vector<uint8_t>>& path,
              const std::vector<uint8_t>& key,
              std::vector<uint8_t>* element_bytes,
              bool* found,
              OperationCost* cost,
              Transaction* tx,
              std::string* error);
  bool GetRawOptional(const std::vector<std::vector<uint8_t>>& path,
                      const std::vector<uint8_t>& key,
                      std::vector<uint8_t>* element_bytes,
                      bool* found,
                      std::string* error);
  bool GetRawOptional(const std::vector<std::vector<uint8_t>>& path,
                      const std::vector<uint8_t>& key,
                      std::vector<uint8_t>* element_bytes,
                      bool* found,
                      OperationCost* cost,
                      std::string* error);
  bool GetRawOptional(const std::vector<std::vector<uint8_t>>& path,
                      const std::vector<uint8_t>& key,
                      std::vector<uint8_t>* element_bytes,
                      bool* found,
                      Transaction* tx,
                      std::string* error);
  bool GetRawOptional(const std::vector<std::vector<uint8_t>>& path,
                      const std::vector<uint8_t>& key,
                      std::vector<uint8_t>* element_bytes,
                      bool* found,
                      OperationCost* cost,
                      Transaction* tx,
                      std::string* error);
  bool GetCachingOptional(const std::vector<std::vector<uint8_t>>& path,
                          const std::vector<uint8_t>& key,
                          std::vector<uint8_t>* element_bytes,
                          bool* found,
                          bool allow_cache,
                          std::string* error);
  bool GetCachingOptional(const std::vector<std::vector<uint8_t>>& path,
                          const std::vector<uint8_t>& key,
                          std::vector<uint8_t>* element_bytes,
                          bool* found,
                          bool allow_cache,
                          OperationCost* cost,
                          std::string* error);
  bool GetCachingOptional(const std::vector<std::vector<uint8_t>>& path,
                          const std::vector<uint8_t>& key,
                          std::vector<uint8_t>* element_bytes,
                          bool* found,
                          bool allow_cache,
                          Transaction* tx,
                          std::string* error);
  bool GetCachingOptional(const std::vector<std::vector<uint8_t>>& path,
                          const std::vector<uint8_t>& key,
                          std::vector<uint8_t>* element_bytes,
                          bool* found,
                          bool allow_cache,
                          OperationCost* cost,
                          Transaction* tx,
                          std::string* error);
  bool GetRawCachingOptional(const std::vector<std::vector<uint8_t>>& path,
                             const std::vector<uint8_t>& key,
                             std::vector<uint8_t>* element_bytes,
                             bool* found,
                             bool allow_cache,
                             std::string* error);
  bool GetRawCachingOptional(const std::vector<std::vector<uint8_t>>& path,
                             const std::vector<uint8_t>& key,
                             std::vector<uint8_t>* element_bytes,
                             bool* found,
                             bool allow_cache,
                             OperationCost* cost,
                             std::string* error);
  bool GetRawCachingOptional(const std::vector<std::vector<uint8_t>>& path,
                             const std::vector<uint8_t>& key,
                             std::vector<uint8_t>* element_bytes,
                             bool* found,
                             bool allow_cache,
                             Transaction* tx,
                             std::string* error);
  bool GetRawCachingOptional(const std::vector<std::vector<uint8_t>>& path,
                             const std::vector<uint8_t>& key,
                             std::vector<uint8_t>* element_bytes,
                             bool* found,
                             bool allow_cache,
                             OperationCost* cost,
                             Transaction* tx,
                             std::string* error);
  bool FollowReference(const std::vector<std::vector<uint8_t>>& path,
                       const std::vector<uint8_t>& key,
                       std::vector<uint8_t>* element_bytes,
                       bool* found,
                       Transaction* tx,
                       std::string* error);
  bool FollowReference(const std::vector<std::vector<uint8_t>>& path,
                       const std::vector<uint8_t>& key,
                       std::vector<uint8_t>* element_bytes,
                       bool* found,
                       OperationCost* cost,
                       Transaction* tx,
                       std::string* error);
  bool Has(const std::vector<std::vector<uint8_t>>& path,
           const std::vector<uint8_t>& key,
           bool* found,
           std::string* error);
  bool Has(const std::vector<std::vector<uint8_t>>& path,
           const std::vector<uint8_t>& key,
           bool* found,
           Transaction* tx,
           std::string* error);
  bool HasRaw(const std::vector<std::vector<uint8_t>>& path,
              const std::vector<uint8_t>& key,
              bool* found,
              std::string* error);
  bool HasRaw(const std::vector<std::vector<uint8_t>>& path,
              const std::vector<uint8_t>& key,
              bool* found,
              OperationCost* cost,
              std::string* error);
  bool HasRaw(const std::vector<std::vector<uint8_t>>& path,
              const std::vector<uint8_t>& key,
              bool* found,
              Transaction* tx,
              std::string* error);
  bool HasRaw(const std::vector<std::vector<uint8_t>>& path,
              const std::vector<uint8_t>& key,
              bool* found,
              OperationCost* cost,
              Transaction* tx,
              std::string* error);
  bool HasCachingOptional(const std::vector<std::vector<uint8_t>>& path,
                          const std::vector<uint8_t>& key,
                          bool* found,
                          bool allow_cache,
                          std::string* error);
  bool HasCachingOptional(const std::vector<std::vector<uint8_t>>& path,
                          const std::vector<uint8_t>& key,
                          bool* found,
                          bool allow_cache,
                          OperationCost* cost,
                          std::string* error);
  bool HasCachingOptional(const std::vector<std::vector<uint8_t>>& path,
                          const std::vector<uint8_t>& key,
                          bool* found,
                          bool allow_cache,
                          Transaction* tx,
                          std::string* error);
  bool HasCachingOptional(const std::vector<std::vector<uint8_t>>& path,
                          const std::vector<uint8_t>& key,
                          bool* found,
                          bool allow_cache,
                          OperationCost* cost,
                          Transaction* tx,
                          std::string* error);
  bool RootKey(std::vector<uint8_t>* out_key,
               bool* found,
               std::string* error);
  bool RootKey(std::vector<uint8_t>* out_key,
               bool* found,
               OperationCost* cost,
               std::string* error);
  bool RootKey(std::vector<uint8_t>* out_key,
               bool* found,
               Transaction* tx,
               std::string* error);
  bool RootKey(std::vector<uint8_t>* out_key,
               bool* found,
               OperationCost* cost,
               Transaction* tx,
               std::string* error);
  bool RootHash(std::vector<uint8_t>* out_hash,
                std::string* error);
  bool RootHash(std::vector<uint8_t>* out_hash,
                Transaction* tx,
                std::string* error);
  bool RootHashForVersion(const GroveVersion& version,
                          std::vector<uint8_t>* out_hash,
                          std::string* error);
  bool RootHashForVersion(const GroveVersion& version,
                          std::vector<uint8_t>* out_hash,
                          Transaction* tx,
                          std::string* error);
  bool VerifyGroveDb(bool verify_references,
                     bool allow_cache,
                     std::vector<VerificationIssue>* issues,
                     std::string* error);
  bool VerifyGroveDb(bool verify_references,
                     bool allow_cache,
                     std::vector<VerificationIssue>* issues,
                     Transaction* tx,
                     std::string* error);
  bool VerifyGroveDbForVersion(const GroveVersion& version,
                               bool verify_references,
                               bool allow_cache,
                               std::vector<VerificationIssue>* issues,
                               std::string* error);
  bool VerifyGroveDbForVersion(const GroveVersion& version,
                               bool verify_references,
                               bool allow_cache,
                               std::vector<VerificationIssue>* issues,
                               Transaction* tx,
                               std::string* error);
  bool IsEmptyTree(const std::vector<std::vector<uint8_t>>& path,
                   bool* is_empty,
                   std::string* error);
  bool IsEmptyTree(const std::vector<std::vector<uint8_t>>& path,
                   bool* is_empty,
                   Transaction* tx,
                   std::string* error);
  bool CheckSubtreeExistsInvalidPath(const std::vector<std::vector<uint8_t>>& path,
                                     std::string* error);
  bool CheckSubtreeExistsInvalidPath(const std::vector<std::vector<uint8_t>>& path,
                                     Transaction* tx,
                                     std::string* error);
  bool CheckSubtreeExistsInvalidPathForVersion(
      const std::vector<std::vector<uint8_t>>& path,
      const GroveVersion& version,
      std::string* error);
  bool CheckSubtreeExistsInvalidPathForVersion(
      const std::vector<std::vector<uint8_t>>& path,
      const GroveVersion& version,
      Transaction* tx,
      std::string* error);
  bool FindSubtrees(const std::vector<std::vector<uint8_t>>& path,
                    std::vector<std::vector<std::vector<uint8_t>>>* out_subtrees,
                    std::string* error);
  bool FindSubtrees(const std::vector<std::vector<uint8_t>>& path,
                    std::vector<std::vector<std::vector<uint8_t>>>* out_subtrees,
                    OperationCost* cost,
                    std::string* error);
  bool FindSubtrees(const std::vector<std::vector<uint8_t>>& path,
                    std::vector<std::vector<std::vector<uint8_t>>>* out_subtrees,
                    Transaction* tx,
                    std::string* error);
  bool FindSubtrees(const std::vector<std::vector<uint8_t>>& path,
                    std::vector<std::vector<std::vector<uint8_t>>>* out_subtrees,
                    OperationCost* cost,
                    Transaction* tx,
                    std::string* error);
  /// Find subtrees with optional filtering by tree kinds.
  /// If tree_kinds_filter is null or empty, returns all subtrees (same as FindSubtrees).
  /// Tree kinds: 2=Tree, 4=SumTree, 5=BigSumTree, 6=CountTree, 7=CountSumTree,
  /// 8=ProvableCountTree, 10=ProvableCountSumTree
  bool FindSubtreesByKinds(const std::vector<std::vector<uint8_t>>& path,
                           std::vector<std::vector<std::vector<uint8_t>>>* out_subtrees,
                           const std::vector<uint64_t>* tree_kinds_filter,
                           std::string* error);
  bool FindSubtreesByKinds(const std::vector<std::vector<uint8_t>>& path,
                           std::vector<std::vector<std::vector<uint8_t>>>* out_subtrees,
                           const std::vector<uint64_t>* tree_kinds_filter,
                           OperationCost* cost,
                           std::string* error);
  bool FindSubtreesByKinds(const std::vector<std::vector<uint8_t>>& path,
                           std::vector<std::vector<std::vector<uint8_t>>>* out_subtrees,
                           const std::vector<uint64_t>* tree_kinds_filter,
                           Transaction* tx,
                           std::string* error);
  bool FindSubtreesByKinds(const std::vector<std::vector<uint8_t>>& path,
                           std::vector<std::vector<std::vector<uint8_t>>>* out_subtrees,
                           const std::vector<uint64_t>* tree_kinds_filter,
                           OperationCost* cost,
                           Transaction* tx,
                           std::string* error);
  /// Count subtrees under a path without enumerating them.
  /// Returns the total count of tree-type elements in the subtree rooted at path.
  /// Tree kinds: 2=Tree, 4=SumTree, 5=BigSumTree, 6=CountTree, 7=CountSumTree,
  /// 8=ProvableCountTree, 10=ProvableCountSumTree
  bool CountSubtrees(const std::vector<std::vector<uint8_t>>& path,
                     uint64_t* out_count,
                     std::string* error);
  bool CountSubtrees(const std::vector<std::vector<uint8_t>>& path,
                     uint64_t* out_count,
                     OperationCost* cost,
                     std::string* error);
  bool CountSubtrees(const std::vector<std::vector<uint8_t>>& path,
                     uint64_t* out_count,
                     Transaction* tx,
                     std::string* error);
  bool CountSubtrees(const std::vector<std::vector<uint8_t>>& path,
                     uint64_t* out_count,
                     OperationCost* cost,
                     Transaction* tx,
                     std::string* error);
  bool CountSubtreesForVersion(const std::vector<std::vector<uint8_t>>& path,
                               const GroveVersion& version,
                               uint64_t* out_count,
                               std::string* error);
  bool CountSubtreesForVersion(const std::vector<std::vector<uint8_t>>& path,
                               const GroveVersion& version,
                               uint64_t* out_count,
                               Transaction* tx,
                               std::string* error);
  /// Get the root element (key + element bytes) of a specific subtree.
  /// Returns the root key and encoded element for the subtree at the given path.
  /// This is useful for replication and verification scenarios.
  /// Returns false if the path does not point to a valid tree element.
  bool GetSubtreeRoot(const std::vector<std::vector<uint8_t>>& path,
                      std::vector<uint8_t>* out_root_key,
                      std::vector<uint8_t>* out_element_bytes,
                      std::string* error);
  bool GetSubtreeRoot(const std::vector<std::vector<uint8_t>>& path,
                      std::vector<uint8_t>* out_root_key,
                      std::vector<uint8_t>* out_element_bytes,
                      OperationCost* cost,
                      std::string* error);
  bool GetSubtreeRoot(const std::vector<std::vector<uint8_t>>& path,
                      std::vector<uint8_t>* out_root_key,
                      std::vector<uint8_t>* out_element_bytes,
                      Transaction* tx,
                      std::string* error);
  bool GetSubtreeRoot(const std::vector<std::vector<uint8_t>>& path,
                      std::vector<uint8_t>* out_root_key,
                      std::vector<uint8_t>* out_element_bytes,
                      OperationCost* cost,
                      Transaction* tx,
                      std::string* error);
  bool IsEmptyTreeForVersion(const std::vector<std::vector<uint8_t>>& path,
                             const GroveVersion& version,
                             bool* is_empty,
                             std::string* error);
  bool IsEmptyTreeForVersion(const std::vector<std::vector<uint8_t>>& path,
                             const GroveVersion& version,
                             bool* is_empty,
                             Transaction* tx,
                             std::string* error);
  bool DeleteIfEmptyTreeForVersion(const std::vector<std::vector<uint8_t>>& path,
                                   const std::vector<uint8_t>& key,
                                   const GroveVersion& version,
                                   bool* deleted,
                                   std::string* error);
  bool DeleteIfEmptyTreeForVersion(const std::vector<std::vector<uint8_t>>& path,
                                   const std::vector<uint8_t>& key,
                                   const GroveVersion& version,
                                   bool* deleted,
                                   Transaction* tx,
                                   std::string* error);

  bool PutAux(const std::vector<uint8_t>& key,
              const std::vector<uint8_t>& value,
              std::string* error);
  bool PutAux(const std::vector<uint8_t>& key,
              const std::vector<uint8_t>& value,
              Transaction* tx,
              std::string* error);
  bool GetAux(const std::vector<uint8_t>& key,
              std::vector<uint8_t>* value,
              bool* found,
              std::string* error);
  bool GetAux(const std::vector<uint8_t>& key,
              std::vector<uint8_t>* value,
              bool* found,
              Transaction* tx,
              std::string* error);
  bool DeleteAux(const std::vector<uint8_t>& key,
                 bool* deleted,
                 std::string* error);
  bool DeleteAux(const std::vector<uint8_t>& key,
                 bool* deleted,
                 Transaction* tx,
                 std::string* error);
  bool HasAux(const std::vector<uint8_t>& key,
              bool* exists,
              std::string* error);
  bool HasAux(const std::vector<uint8_t>& key,
              bool* exists,
              Transaction* tx,
              std::string* error);

  bool QueryRange(const std::vector<std::vector<uint8_t>>& path,
                  const std::vector<uint8_t>& start_key,
                  const std::vector<uint8_t>& end_key,
                  bool start_inclusive,
                  bool end_inclusive,
                  std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>>* out,
                  std::string* error);
  bool QueryRange(const std::vector<std::vector<uint8_t>>& path,
                  const std::vector<uint8_t>& start_key,
                  const std::vector<uint8_t>& end_key,
                  bool start_inclusive,
                  bool end_inclusive,
                  std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>>* out,
                  Transaction* tx,
                  std::string* error);
  bool QueryRaw(const PathQuery& path_query,
                std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>>* out,
                std::string* error);
  bool QueryRaw(const PathQuery& path_query,
                std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>>* out,
                Transaction* tx,
                std::string* error);
  bool QueryKeyElementPairs(const PathQuery& path_query,
                            std::vector<KeyElementPair>* out,
                            std::string* error);
  bool QueryKeyElementPairs(const PathQuery& path_query,
                            std::vector<KeyElementPair>* out,
                            Transaction* tx,
                            std::string* error);
  bool QueryRawKeysOptional(const PathQuery& path_query,
                            std::vector<PathKeyOptionalElement>* out,
                            std::string* error);
  bool QueryKeysOptional(const PathQuery& path_query,
                         std::vector<PathKeyOptionalElement>* out,
                         std::string* error);
  bool QueryKeysOptional(const PathQuery& path_query,
                         std::vector<PathKeyOptionalElement>* out,
                         Transaction* tx,
                         std::string* error);
  bool QueryRawKeysOptional(const PathQuery& path_query,
                            std::vector<PathKeyOptionalElement>* out,
                            Transaction* tx,
                            std::string* error);
  bool QuerySums(const PathQuery& path_query,
                 std::vector<int64_t>* out_sums,
                 std::string* error);
  bool QuerySums(const PathQuery& path_query,
                 std::vector<int64_t>* out_sums,
                 Transaction* tx,
                 std::string* error);
  bool QueryItemValue(const PathQuery& path_query,
                      std::vector<std::vector<uint8_t>>* out_values,
                      std::string* error);
  bool QueryItemValue(const PathQuery& path_query,
                      std::vector<std::vector<uint8_t>>* out_values,
                      Transaction* tx,
                      std::string* error);
  bool QueryItemValueForVersion(const PathQuery& path_query,
                                const GroveVersion& version,
                                std::vector<std::vector<uint8_t>>* out_values,
                                std::string* error);
  bool QueryItemValueForVersion(const PathQuery& path_query,
                                const GroveVersion& version,
                                std::vector<std::vector<uint8_t>>* out_values,
                                Transaction* tx,
                                std::string* error);
  bool QueryItemValueOrSum(const PathQuery& path_query,
                           std::vector<QueryItemOrSumValue>* out,
                           std::string* error);
  bool QueryItemValueOrSum(const PathQuery& path_query,
                           std::vector<QueryItemOrSumValue>* out,
                           Transaction* tx,
                           std::string* error);
  bool QueryKeysOptionalForVersion(const PathQuery& path_query,
                                   const GroveVersion& version,
                                   std::vector<PathKeyOptionalElement>* out,
                                   std::string* error);
  bool QueryKeysOptionalForVersion(const PathQuery& path_query,
                                   const GroveVersion& version,
                                   std::vector<PathKeyOptionalElement>* out,
                                   Transaction* tx,
                                   std::string* error);
  bool QuerySumsForVersion(const PathQuery& path_query,
                           const GroveVersion& version,
                           std::vector<int64_t>* out_sums,
                           std::string* error);
  bool QuerySumsForVersion(const PathQuery& path_query,
                           const GroveVersion& version,
                           std::vector<int64_t>* out_sums,
                           Transaction* tx,
                           std::string* error);
  bool QueryRawForVersion(
      const PathQuery& path_query,
      const GroveVersion& version,
      std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>>* out,
      std::string* error);
  bool QueryRawForVersion(
      const PathQuery& path_query,
      const GroveVersion& version,
      std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>>* out,
      Transaction* tx,
      std::string* error);
  bool ProveQuery(const PathQuery& query,
                  std::vector<uint8_t>* out_proof,
                  std::string* error);
  bool ProveQueryForVersion(const PathQuery& query,
                            const GroveVersion& version,
                            std::vector<uint8_t>* out_proof,
                            std::string* error);
  bool ProveQueryForVersion(const PathQuery& query,
                            const GroveVersion& version,
                            std::vector<uint8_t>* out_proof,
                            Transaction* tx,
                            std::string* error);

  bool StartTransaction(Transaction* out, std::string* error);
  bool CommitTransaction(Transaction* tx, std::string* error);
  bool RollbackTransaction(Transaction* tx, std::string* error);
  bool ApplyBatch(const std::vector<BatchOp>& ops, std::string* error);
  bool ApplyBatch(const std::vector<BatchOp>& ops,
                  const BatchApplyOptions& options,
                  std::string* error);
  bool ApplyBatch(const std::vector<BatchOp>& ops, Transaction* tx, std::string* error);
  bool ApplyBatch(const std::vector<BatchOp>& ops,
                  const BatchApplyOptions& options,
                  Transaction* tx,
                  std::string* error);
  // Partial batches are non-atomic across pause/resume boundaries: executed
  // operations are committed, while leftovers may be resumed later.
  bool ApplyPartialBatch(const std::vector<BatchOp>& ops,
                         const BatchApplyOptions& options,
                         Transaction* tx,
                         OpsByLevelPath* leftover_ops,
                         std::string* error);
  bool ContinuePartialApplyBatch(const OpsByLevelPath& previous_leftover,
                                 const std::vector<BatchOp>& additional_ops,
                                 const BatchApplyOptions& options,
                                 Transaction* tx,
                                 OpsByLevelPath* new_leftover_ops,
                                 std::string* error);
  bool ValidateBatch(const std::vector<BatchOp>& ops, std::string* error);
  bool ValidateBatch(const std::vector<BatchOp>& ops,
                     const BatchApplyOptions& options,
                     std::string* error);
  bool EstimatedCaseOperationsForBatch(const std::vector<BatchOp>& ops,
                                       OperationCost* cost,
                                       std::string* error);
  bool EstimatedCaseOperationsForBatch(const std::vector<BatchOp>& ops,
                                       const BatchApplyOptions& options,
                                       OperationCost* cost,
                                       std::string* error);

 private:
  bool ReadSubtreeRootKey(const std::vector<std::vector<uint8_t>>& path,
                          Transaction* tx,
                          std::vector<uint8_t>* root_key,
                          bool* found,
                          std::string* error);
  bool EncodeTreeElementWithRootKey(const std::vector<uint8_t>& previous_element,
                                    const std::vector<uint8_t>* root_key,
                                    uint64_t propagated_count,
                                    __int128 propagated_sum,
                                    std::vector<uint8_t>* updated_element,
                                    std::string* error);
  static MerkTree::SumValueFn MakeSumValueFn();
  bool PropagateSubtreeRootKeyUp(const std::vector<std::vector<uint8_t>>& path,
                                 Transaction* tx,
                                 OperationCost* cost,
                                 std::string* error);
  bool PropagateSubtreeRootKeyUp(const std::vector<std::vector<uint8_t>>& path,
                                 Transaction* tx,
                                 OperationCost* cost,
                                 std::string* error,
                                 MerkCache* cache);
  bool EnsurePathTreeElements(const std::vector<std::vector<uint8_t>>& path,
                              Transaction* tx,
                              OperationCost* cost,
                              std::string* error);
  bool EnsurePathTreeElements(const std::vector<std::vector<uint8_t>>& path,
                              Transaction* tx,
                              OperationCost* cost,
                              std::string* error,
                              MerkCache* cache);
  bool EnsurePathTreeElementsOptional(const std::vector<std::vector<uint8_t>>& path,
                                      Transaction* tx,
                                      OperationCost* cost,
                                      std::string* error);
  bool VerifyMerkAndSubmerks(RocksDbWrapper* storage,
                               RocksDbWrapper::Transaction* tx,
                               const std::vector<std::vector<uint8_t>>& path,
                               MerkTree* merk,
                               bool verify_references,
                               bool allow_cache,
                               std::vector<VerificationIssue>* issues,
                               std::string* error);
  bool GetMerkCached(const std::vector<std::vector<uint8_t>>& path,
                     RocksDbWrapper::Transaction* tx,
                     MerkTree** out,
                     std::string* error);
  bool ApplyBatchInternal(const std::vector<BatchOp>& ops,
                          const BatchApplyOptions& options,
                          Transaction* tx,
                          int32_t stop_level,
                          OpsByLevelPath* leftover_ops,
                          std::string* error);
  bool PropagateCachedRootHashes(RocksDbWrapper::Transaction* tx,
                                 int32_t stop_depth,
                                 std::string* error);
  static bool IsTreeElementVariant(uint64_t variant);

  RocksDbWrapper storage_;
  std::unique_ptr<MerkCache> merk_cache_;
  std::unique_ptr<MerkCache> non_tx_insert_cache_;
  std::string non_tx_insert_cache_path_key_;
  std::unordered_set<std::string> non_tx_validated_tree_paths_;
  bool opened_ = false;
};

}  // namespace grovedb

#endif  // GROVEDB_CPP_GROVEDB_H
